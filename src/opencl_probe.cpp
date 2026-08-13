#include "opencl_probe.h"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CL/cl.h"

using DlsymFn = void* (*)(void*, const char*);
static DlsymFn g_real_dlsym = nullptr;
static volatile bool g_resolving_real_dlsym = false;

static int FindLibcDlsym(dl_phdr_info* info, size_t, void*) {
    if (!info || !info->dlpi_name) return 0;
    const char* module = info->dlpi_name;
    if (!strstr(module, "libc.so") && !strstr(module, "libdl.so") &&
        !strstr(module, "ld-musl-")) return 0;
    const ElfW(Dyn)* dynamic = nullptr;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dynamic = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dynamic) return 0;
    const char* strtab = nullptr;
    const ElfW(Sym)* symtab = nullptr;
    size_t nchain = 0;
    const uint32_t* gnu_hash = nullptr;
    for (auto d = dynamic; d->d_tag != DT_NULL; ++d) {
        auto dyn_ptr = [&](ElfW(Addr) value) -> uintptr_t {
            return value < info->dlpi_addr ? info->dlpi_addr + value : value;
        };
        if (d->d_tag == DT_STRTAB) strtab = reinterpret_cast<const char*>(dyn_ptr(d->d_un.d_ptr));
        if (d->d_tag == DT_SYMTAB) symtab = reinterpret_cast<const ElfW(Sym)*>(dyn_ptr(d->d_un.d_ptr));
        if (d->d_tag == DT_HASH) nchain = reinterpret_cast<const uint32_t*>(dyn_ptr(d->d_un.d_ptr))[1];
        if (d->d_tag == DT_GNU_HASH) gnu_hash = reinterpret_cast<const uint32_t*>(dyn_ptr(d->d_un.d_ptr));
    }
    if (!nchain && gnu_hash) {
        // GNU hash does not carry a symbol count.  The last chain entry is
        // marked by its low bit; scan from the largest bucket.
        uint32_t nbuckets = gnu_hash[0];
        uint32_t symoffset = gnu_hash[1];
        uint32_t bloom_size = gnu_hash[2];
        const uint32_t* buckets = gnu_hash + 4 + (sizeof(uintptr_t) / 4) * bloom_size;
        const uint32_t* chains = buckets + nbuckets;
        uint32_t max_index = symoffset;
        for (uint32_t i = 0; i < nbuckets; ++i) {
            uint32_t index = static_cast<uint32_t>(buckets[i]);
            if (index < symoffset) continue;
            uint32_t chain_index = index - symoffset;
            while ((chains[chain_index] & 1U) == 0U) ++chain_index;
            const uint32_t last_symbol = symoffset + chain_index;
            if (last_symbol + 1 > max_index) max_index = last_symbol + 1;
        }
        nchain = max_index;
    }
    if (!strtab || !symtab || !nchain) return 0;
    for (size_t i = 0; i < nchain; ++i) {
        const char* name = strtab + symtab[i].st_name;
        if (!strcmp(name, "dlsym") && symtab[i].st_value) {
            g_real_dlsym = reinterpret_cast<DlsymFn>(info->dlpi_addr + symtab[i].st_value);
            return 1;
        }
    }
    return 0;
}

static void ResolveRealDlsym() {
    if (g_real_dlsym || g_resolving_real_dlsym) return;
    g_resolving_real_dlsym = true;
    dl_iterate_phdr(FindLibcDlsym, nullptr);
    g_resolving_real_dlsym = false;
}

namespace {
static std::atomic<int> g_marker_fd{-2};
static std::atomic<size_t> g_live_requested_bytes{0};
static std::atomic<unsigned long long> g_dispatch_count{0};
static std::atomic<const char*> g_last_api{"<none>"};
enum BalanceKind : size_t {
    kContext = 0,
    kQueue,
    kProgram,
    kKernel,
    kMem,
    kEvent,
    kSampler,
    kBalanceCount,
};
static std::atomic<long long> g_balance[kBalanceCount] = {};
static std::atomic<unsigned long long> g_balance_underflow[kBalanceCount] = {};
static thread_local bool g_in_probe = false;
enum class BalanceAction : uint8_t { kOther, kCreate, kRetain, kRelease };

struct MemEntry {
    uintptr_t object = 0;
    size_t size = 0;
    unsigned refs = 0;
    const char* kind = nullptr;
};
constexpr size_t kMaxMemEntries = 8192;
static MemEntry g_mem_entries[kMaxMemEntries];
static std::atomic_flag g_mem_lock = ATOMIC_FLAG_INIT;

class MemLock {
public:
    MemLock() { while (g_mem_lock.test_and_set(std::memory_order_acquire)) {} }
    ~MemLock() { g_mem_lock.clear(std::memory_order_release); }
};

static bool Enabled() {
    // dlsym can be called while the preload library and libc++ are still being
    // initialized.  A function-local static here uses __cxa_guard_acquire;
    // re-entering dlsym during that initialization aborts with a recursive
    // initialization error.  Reading the process environment directly keeps
    // this early-loader path guard-free.
    const char* value = getenv("MALLOC_HOOK_OPENCL_MARKERS");
    return value != nullptr && value[0] != '\0' && strcmp(value, "0") != 0;
}

static bool ApiEnabled(const char* api) {
    const char* filter = getenv("MALLOC_HOOK_OPENCL_FUNCS");
    if (filter == nullptr || filter[0] == '\0') {
        filter = getenv("HAIO_OCL_HOOK_FUNCS");
    }
    if (filter == nullptr || filter[0] == '\0') {
        return true;
    }
    const size_t api_len = strlen(api);
    const char* cursor = filter;
    while (*cursor != '\0') {
        while (*cursor == ',' || *cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        const char* end = cursor;
        while (*end != '\0' && *end != ',') {
            ++end;
        }
        const size_t token_len = static_cast<size_t>(end - cursor);
        if (token_len == api_len && strncmp(cursor, api, api_len) == 0) {
            return true;
        }
        cursor = end;
    }
    return false;
}

static int ClassifyBalance(const char* api, BalanceAction* action) {
    *action = BalanceAction::kOther;
    struct Entry {
        const char* name;
        int kind;
        BalanceAction action;
    };
    static constexpr Entry kEntries[] = {
            {"clCreateContext", kContext, BalanceAction::kCreate},
            {"clCreateContextFromType", kContext, BalanceAction::kCreate},
            {"clRetainContext", kContext, BalanceAction::kRetain},
            {"clReleaseContext", kContext, BalanceAction::kRelease},
            {"clCreateCommandQueue", kQueue, BalanceAction::kCreate},
            {"clCreateCommandQueueWithProperties", kQueue, BalanceAction::kCreate},
            {"clRetainCommandQueue", kQueue, BalanceAction::kRetain},
            {"clReleaseCommandQueue", kQueue, BalanceAction::kRelease},
            {"clCreateProgramWithSource", kProgram, BalanceAction::kCreate},
            {"clCreateProgramWithBinary", kProgram, BalanceAction::kCreate},
            {"clCreateProgramWithBuiltInKernels", kProgram, BalanceAction::kCreate},
            {"clCreateProgramWithIL", kProgram, BalanceAction::kCreate},
            {"clRetainProgram", kProgram, BalanceAction::kRetain},
            {"clReleaseProgram", kProgram, BalanceAction::kRelease},
            {"clCreateKernel", kKernel, BalanceAction::kCreate},
            {"clCloneKernel", kKernel, BalanceAction::kCreate},
            {"clRetainKernel", kKernel, BalanceAction::kRetain},
            {"clReleaseKernel", kKernel, BalanceAction::kRelease},
            {"clCreateBuffer", kMem, BalanceAction::kCreate},
            {"clCreateSubBuffer", kMem, BalanceAction::kCreate},
            {"clCreateImage", kMem, BalanceAction::kCreate},
            {"clCreateImage2D", kMem, BalanceAction::kCreate},
            {"clCreateImage3D", kMem, BalanceAction::kCreate},
            {"clCreatePipe", kMem, BalanceAction::kCreate},
            {"clCreateBufferWithProperties", kMem, BalanceAction::kCreate},
            {"clCreateImageWithProperties", kMem, BalanceAction::kCreate},
            {"clRetainMemObject", kMem, BalanceAction::kRetain},
            {"clReleaseMemObject", kMem, BalanceAction::kRelease},
            {"clCreateUserEvent", kEvent, BalanceAction::kCreate},
            {"clRetainEvent", kEvent, BalanceAction::kRetain},
            {"clReleaseEvent", kEvent, BalanceAction::kRelease},
            {"clCreateSampler", kSampler, BalanceAction::kCreate},
            {"clCreateSamplerWithProperties", kSampler, BalanceAction::kCreate},
            {"clRetainSampler", kSampler, BalanceAction::kRetain},
            {"clReleaseSampler", kSampler, BalanceAction::kRelease},
    };
    for (const auto& entry : kEntries) {
        if (strcmp(api, entry.name) == 0) {
            *action = entry.action;
            return entry.kind;
        }
    }
    return -1;
}

static void UpdateBalance(
        const char* api, uintptr_t object, size_t, cl_int status) {
    if (!ApiEnabled(api) || object == 0 || status != CL_SUCCESS) {
        return;
    }
    BalanceAction action = BalanceAction::kOther;
    const int kind = ClassifyBalance(api, &action);
    if (kind < 0) {
        return;
    }
    if (action == BalanceAction::kRelease) {
        long long current = g_balance[kind].load(std::memory_order_relaxed);
        while (current > 0 &&
               !g_balance[kind].compare_exchange_weak(
                       current, current - 1, std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
        }
        if (current <= 0) {
            g_balance_underflow[kind].fetch_add(1, std::memory_order_relaxed);
        }
    } else if (action == BalanceAction::kCreate ||
               action == BalanceAction::kRetain) {
        g_balance[kind].fetch_add(1, std::memory_order_relaxed);
    }
}

static unsigned DispatchEvery() {
    const char* value = getenv("MALLOC_HOOK_OPENCL_DISPATCH_EVERY");
    if (!value || !value[0]) return 1U;
    char* end = nullptr;
    unsigned long parsed = strtoul(value, &end, 10);
    return end != value && *end == '\0' ? static_cast<unsigned>(parsed) : 1U;
}

static bool ProcessRssEnabled() {
    const char* value = getenv("MALLOC_HOOK_OPENCL_PROCESS_RSS");
    return value != nullptr && value[0] != '\0' && strcmp(value, "0") != 0;
}

static size_t ProcessRssBytes() {
    if (!ProcessRssEnabled()) return 0;
    int fd = static_cast<int>(
            syscall(SYS_openat, AT_FDCWD, "/proc/self/status",
                    O_RDONLY | O_CLOEXEC, 0));
    if (fd < 0) return 0;
    char buffer[4096];
    ssize_t count = syscall(SYS_read, fd, buffer, sizeof(buffer) - 1);
    syscall(SYS_close, fd);
    if (count <= 0) return 0;
    buffer[count] = '\0';
    const char* cursor = strstr(buffer, "\nVmRSS:");
    if (cursor) {
        ++cursor;
    } else if (!strncmp(buffer, "VmRSS:", 6)) {
        cursor = buffer;
    }
    if (!cursor) return 0;
    cursor += 6;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    char* end = nullptr;
    unsigned long long kib = strtoull(cursor, &end, 10);
    return end != cursor ? static_cast<size_t>(kib) * 1024U : 0;
}

static int MarkerFd() {
    if (!Enabled()) return -1;
    int fd = g_marker_fd.load(std::memory_order_acquire);
    if (fd != -2) return fd;
    const char* path = getenv("MALLOC_HOOK_OPENCL_MARKER_PATH");
    if (!path || !path[0]) path = "/sys/kernel/tracing/trace_marker";
    int flags = O_WRONLY | O_CLOEXEC;
    if (getenv("MALLOC_HOOK_OPENCL_MARKER_PATH") != nullptr) {
        flags |= O_CREAT | O_APPEND;
    }
    fd = static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, 0644));
    int expected = -2;
    if (!g_marker_fd.compare_exchange_strong(expected, fd, std::memory_order_release,
                                               std::memory_order_relaxed)) {
        if (fd >= 0) syscall(SYS_close, fd);
        fd = expected;
    }
    return fd;
}

static void WriteMarker(const char* data, size_t size) {
    int fd = MarkerFd();
    if (fd >= 0 && size) {
        syscall(SYS_write, fd, data, size);
        if (getenv("MALLOC_HOOK_OPENCL_MARKER_PATH") != nullptr) {
            syscall(SYS_write, fd, "\n", 1);
        }
    }
}

static void TraceBegin(const char* api, uintptr_t object = 0, size_t size = 0,
                       const char* extra = nullptr) {
    if (!Enabled() || g_in_probe || !ApiEnabled(api)) return;
    g_last_api.store(api, std::memory_order_relaxed);
    g_in_probe = true;
    char marker[1536];
    int length = snprintf(marker, sizeof(marker),
                          "B|%ld|malloc_hook_ocl api=%s tid=%ld obj=0x%llx size=%zu live=%zu process_rss=%zu%s%s",
                          static_cast<long>(syscall(SYS_getpid)), api,
                          static_cast<long>(syscall(SYS_gettid)),
                          static_cast<unsigned long long>(object), size,
                          g_live_requested_bytes.load(std::memory_order_relaxed),
                          ProcessRssBytes(),
                          extra ? " " : "", extra ? extra : "");
    if (length > 0 && static_cast<size_t>(length) < sizeof(marker)) {
        WriteMarker(marker, static_cast<size_t>(length));
    }
    g_in_probe = false;
}

static void TraceEnd(const char* api, uintptr_t object, size_t size, cl_int status,
                     const char* extra = nullptr) {
    if (!Enabled() || g_in_probe || !ApiEnabled(api)) return;
    UpdateBalance(api, object, size, status);
    g_in_probe = true;
    static constexpr char kEnd[] = "E";
    WriteMarker(kEnd, sizeof(kEnd) - 1);
    char marker[1536];
    int length = snprintf(marker, sizeof(marker),
                          "B|%ld|malloc_hook_ocl_result api=%s tid=%ld obj=0x%llx size=%zu status=%d live=%zu process_rss=%zu%s%s",
                          static_cast<long>(syscall(SYS_getpid)), api,
                          static_cast<long>(syscall(SYS_gettid)),
                          static_cast<unsigned long long>(object), size, status,
                          g_live_requested_bytes.load(std::memory_order_relaxed),
                          ProcessRssBytes(),
                          extra ? " " : "", extra ? extra : "");
    if (length > 0 && static_cast<size_t>(length) < sizeof(marker)) {
        WriteMarker(marker, static_cast<size_t>(length));
        WriteMarker(kEnd, sizeof(kEnd) - 1);
    }
    length = snprintf(marker, sizeof(marker),
                      "C|%ld|malloc_hook_ocl_live_requested_bytes|%zu",
                      static_cast<long>(syscall(SYS_getpid)),
                      g_live_requested_bytes.load(std::memory_order_relaxed));
    if (length > 0 && static_cast<size_t>(length) < sizeof(marker)) {
        WriteMarker(marker, static_cast<size_t>(length));
    }
    g_in_probe = false;
}

static void DumpBalanceAtExit() {
    if (!Enabled()) {
        return;
    }
    char marker[512];
    const int length = snprintf(
            marker, sizeof(marker),
            "I|%ld|malloc_hook_ocl_balance context=%lld queue=%lld "
            "program=%lld kernel=%lld mem=%lld event=%lld sampler=%lld "
            "underflow_context=%llu underflow_queue=%llu "
            "underflow_program=%llu underflow_kernel=%llu "
            "underflow_mem=%llu underflow_event=%llu underflow_sampler=%llu",
            static_cast<long>(syscall(SYS_getpid)),
            g_balance[kContext].load(std::memory_order_relaxed),
            g_balance[kQueue].load(std::memory_order_relaxed),
            g_balance[kProgram].load(std::memory_order_relaxed),
            g_balance[kKernel].load(std::memory_order_relaxed),
            g_balance[kMem].load(std::memory_order_relaxed),
            g_balance[kEvent].load(std::memory_order_relaxed),
            g_balance[kSampler].load(std::memory_order_relaxed),
            g_balance_underflow[kContext].load(std::memory_order_relaxed),
            g_balance_underflow[kQueue].load(std::memory_order_relaxed),
            g_balance_underflow[kProgram].load(std::memory_order_relaxed),
            g_balance_underflow[kKernel].load(std::memory_order_relaxed),
            g_balance_underflow[kMem].load(std::memory_order_relaxed),
            g_balance_underflow[kEvent].load(std::memory_order_relaxed),
            g_balance_underflow[kSampler].load(std::memory_order_relaxed));
    if (length > 0 && static_cast<size_t>(length) < sizeof(marker)) {
        WriteMarker(marker, static_cast<size_t>(length));
    }
}

static void RegisterBalanceAtExit() {
    // Register before any OpenCL call so the summary is emitted even when the
    // runtime unloads the probe during process teardown instead of running the
    // normal shared-object destructor path.
    atexit(DumpBalanceAtExit);
}

__attribute__((constructor(101))) static void OpenClProbeInit() {
    RegisterBalanceAtExit();
}

__attribute__((destructor(101))) static void OpenClProbeAtExit() {
    DumpBalanceAtExit();
}

static void RegisterMem(uintptr_t object, size_t size, const char* kind) {
    if (!object) return;
    MemLock lock;
    for (auto& entry : g_mem_entries) {
        if (!entry.object) {
            entry = {object, size, 1, kind};
            g_live_requested_bytes.fetch_add(size, std::memory_order_relaxed);
            return;
        }
    }
}

static void RetainMem(uintptr_t object) {
    MemLock lock;
    for (auto& entry : g_mem_entries) {
        if (entry.object == object) {
            ++entry.refs;
            return;
        }
    }
}

static size_t ReleaseMem(uintptr_t object, bool release_succeeded) {
    if (!release_succeeded || !object) return 0;
    MemLock lock;
    for (auto& entry : g_mem_entries) {
        if (entry.object == object) {
            size_t size = entry.size;
            if (entry.refs > 1) {
                --entry.refs;
            } else {
                entry = {};
                g_live_requested_bytes.fetch_sub(size, std::memory_order_relaxed);
            }
            return size;
        }
    }
    return 0;
}

static size_t ImagePixelBytes(const cl_image_format* format) {
    if (!format) return 0;
    size_t channels = 0;
    switch (format->image_channel_order) {
        case CL_R: case CL_A: case CL_INTENSITY: case CL_LUMINANCE: channels = 1; break;
        case CL_RG: case CL_RA: channels = 2; break;
        case CL_RGB: channels = 3; break;
        case CL_RGBA: case CL_ARGB: case CL_BGRA: channels = 4; break;
        default: return 0;
    }
    size_t bytes = 0;
    switch (format->image_channel_data_type) {
        case CL_SNORM_INT8: case CL_UNORM_INT8: case CL_SIGNED_INT8: case CL_UNSIGNED_INT8: bytes = 1; break;
        case CL_SNORM_INT16: case CL_UNORM_INT16: case CL_SIGNED_INT16: case CL_UNSIGNED_INT16: case CL_HALF_FLOAT: bytes = 2; break;
        case CL_SIGNED_INT32: case CL_UNSIGNED_INT32: case CL_FLOAT: bytes = 4; break;
        case CL_UNORM_SHORT_565: case CL_UNORM_SHORT_555: bytes = 2; channels = 1; break;
        case CL_UNORM_INT_101010: bytes = 4; channels = 1; break;
        default: return 0;
    }
    return channels * bytes;
}

static size_t ImageBytes(const cl_image_format* format, const cl_image_desc* desc) {
    if (!desc) return 0;
    size_t pixel = ImagePixelBytes(format);
    size_t height = desc->image_height ? desc->image_height : 1;
    size_t depth = desc->image_depth ? desc->image_depth : 1;
    size_t array = desc->image_array_size ? desc->image_array_size : 1;
    size_t row = desc->image_row_pitch ? desc->image_row_pitch : desc->image_width * pixel;
    size_t slice = desc->image_slice_pitch ? desc->image_slice_pitch : row * height;
    return slice * depth * array;
}

#define DECLARE_REAL(name, type) static type g_##name = nullptr

using CreateContextFn = cl_context (*)(const cl_context_properties*, cl_uint, const cl_device_id*, void(CL_CALLBACK*)(const char*, const void*, size_t, void*), void*, cl_int*);
DECLARE_REAL(clCreateContext, CreateContextFn);
static cl_context ProbeCreateContext(const cl_context_properties* p, cl_uint n, const cl_device_id* d, void(CL_CALLBACK* cb)(const char*,const void*,size_t,void*), void* u, cl_int* e) { TraceBegin("clCreateContext",0,n); cl_context r=g_clCreateContext(p,n,d,cb,u,e); TraceEnd("clCreateContext",reinterpret_cast<uintptr_t>(r),0,e?*e:CL_SUCCESS); return r; }
using ReleaseContextFn = cl_int (*)(cl_context); DECLARE_REAL(clReleaseContext, ReleaseContextFn);
static cl_int ProbeReleaseContext(cl_context c) {
    TraceBegin("clReleaseContext", reinterpret_cast<uintptr_t>(c));
    cl_int r = g_clReleaseContext(c);
    TraceEnd("clReleaseContext", reinterpret_cast<uintptr_t>(c), 0, r);
    if (r == CL_SUCCESS) {
        // Context release is the final lifecycle boundary for this probe in
        // FaceSR. Emit a summary while the marker sink is still usable.
        DumpBalanceAtExit();
    }
    return r;
}
using CreateQueueFn = cl_command_queue (*)(cl_context,cl_device_id,cl_command_queue_properties,cl_int*); DECLARE_REAL(clCreateCommandQueue, CreateQueueFn);
static cl_command_queue ProbeCreateQueue(cl_context c,cl_device_id d,cl_command_queue_properties p,cl_int* e) { TraceBegin("clCreateCommandQueue",reinterpret_cast<uintptr_t>(c)); cl_command_queue r=g_clCreateCommandQueue(c,d,p,e); TraceEnd("clCreateCommandQueue",reinterpret_cast<uintptr_t>(r),0,e?*e:CL_SUCCESS); return r; }
using CreateQueuePropsFn = cl_command_queue (*)(cl_context,cl_device_id,const cl_queue_properties*,cl_int*); DECLARE_REAL(clCreateCommandQueueWithProperties, CreateQueuePropsFn);
static cl_command_queue ProbeCreateQueueProps(cl_context c,cl_device_id d,const cl_queue_properties* p,cl_int* e) { TraceBegin("clCreateCommandQueueWithProperties",reinterpret_cast<uintptr_t>(c)); cl_command_queue r=g_clCreateCommandQueueWithProperties(c,d,p,e); TraceEnd("clCreateCommandQueueWithProperties",reinterpret_cast<uintptr_t>(r),0,e?*e:CL_SUCCESS); return r; }
using ReleaseQueueFn = cl_int (*)(cl_command_queue); DECLARE_REAL(clReleaseCommandQueue, ReleaseQueueFn);
static cl_int ProbeReleaseQueue(cl_command_queue q) { TraceBegin("clReleaseCommandQueue",reinterpret_cast<uintptr_t>(q)); cl_int r=g_clReleaseCommandQueue(q); TraceEnd("clReleaseCommandQueue",reinterpret_cast<uintptr_t>(q),0,r); return r; }
using CreateBufferFn = cl_mem (*)(cl_context,cl_mem_flags,size_t,void*,cl_int*); DECLARE_REAL(clCreateBuffer, CreateBufferFn);
static cl_mem ProbeCreateBuffer(cl_context c,cl_mem_flags f,size_t s,void* h,cl_int* e) { TraceBegin("clCreateBuffer",reinterpret_cast<uintptr_t>(c),s); cl_mem r=g_clCreateBuffer(c,f,s,h,e); cl_int st=e?*e:CL_SUCCESS; if(r&&st==CL_SUCCESS) RegisterMem(reinterpret_cast<uintptr_t>(r),s,"buffer"); TraceEnd("clCreateBuffer",reinterpret_cast<uintptr_t>(r),s,st); return r; }
using CreateImageFn = cl_mem (*)(cl_context,cl_mem_flags,const cl_image_format*,const cl_image_desc*,void*,cl_int*); DECLARE_REAL(clCreateImage, CreateImageFn);
static cl_mem ProbeCreateImage(cl_context c,cl_mem_flags f,const cl_image_format* fmt,const cl_image_desc* d,void* h,cl_int* e) { size_t s=ImageBytes(fmt,d); TraceBegin("clCreateImage",reinterpret_cast<uintptr_t>(c),s); cl_mem r=g_clCreateImage(c,f,fmt,d,h,e); cl_int st=e?*e:CL_SUCCESS; if(r&&st==CL_SUCCESS) RegisterMem(reinterpret_cast<uintptr_t>(r),s,"image"); TraceEnd("clCreateImage",reinterpret_cast<uintptr_t>(r),s,st); return r; }
using RetainMemFn = cl_int (*)(cl_mem); DECLARE_REAL(clRetainMemObject, RetainMemFn);
static cl_int ProbeRetainMem(cl_mem m) { TraceBegin("clRetainMemObject",reinterpret_cast<uintptr_t>(m)); cl_int r=g_clRetainMemObject(m); if(r==CL_SUCCESS) RetainMem(reinterpret_cast<uintptr_t>(m)); TraceEnd("clRetainMemObject",reinterpret_cast<uintptr_t>(m),0,r); return r; }
using ReleaseMemFn = cl_int (*)(cl_mem); DECLARE_REAL(clReleaseMemObject, ReleaseMemFn);
static cl_int ProbeReleaseMem(cl_mem m) { TraceBegin("clReleaseMemObject",reinterpret_cast<uintptr_t>(m)); cl_int r=g_clReleaseMemObject(m); size_t s=ReleaseMem(reinterpret_cast<uintptr_t>(m),r==CL_SUCCESS); TraceEnd("clReleaseMemObject",reinterpret_cast<uintptr_t>(m),s,r); return r; }
using SvmAllocFn = void* (*)(cl_context,cl_svm_mem_flags,size_t,cl_uint); DECLARE_REAL(clSVMAlloc, SvmAllocFn);
static void* ProbeSvmAlloc(cl_context c,cl_svm_mem_flags f,size_t s,cl_uint a) { TraceBegin("clSVMAlloc",reinterpret_cast<uintptr_t>(c),s); void* r=g_clSVMAlloc(c,f,s,a); if(r) RegisterMem(reinterpret_cast<uintptr_t>(r),s,"svm"); TraceEnd("clSVMAlloc",reinterpret_cast<uintptr_t>(r),s,r?CL_SUCCESS:CL_OUT_OF_HOST_MEMORY); return r; }
using SvmFreeFn = void (*)(cl_context,void*); DECLARE_REAL(clSVMFree, SvmFreeFn);
static void ProbeSvmFree(cl_context c,void* p) { TraceBegin("clSVMFree",reinterpret_cast<uintptr_t>(p)); g_clSVMFree(c,p); size_t s=ReleaseMem(reinterpret_cast<uintptr_t>(p),true); TraceEnd("clSVMFree",reinterpret_cast<uintptr_t>(p),s,CL_SUCCESS); }
using CreateSourceFn = cl_program (*)(cl_context,cl_uint,const char**,const size_t*,cl_int*); DECLARE_REAL(clCreateProgramWithSource, CreateSourceFn);
static cl_program ProbeCreateSource(cl_context c,cl_uint n,const char** src,const size_t* len,cl_int* e) { size_t s=(len&&n)?len[0]:0; TraceBegin("clCreateProgramWithSource",reinterpret_cast<uintptr_t>(c),s); cl_program r=g_clCreateProgramWithSource(c,n,src,len,e); TraceEnd("clCreateProgramWithSource",reinterpret_cast<uintptr_t>(r),s,e?*e:CL_SUCCESS); return r; }
using CreateBinaryFn = cl_program (*)(cl_context,cl_uint,const cl_device_id*,const size_t*,const unsigned char**,cl_int*,cl_int*); DECLARE_REAL(clCreateProgramWithBinary, CreateBinaryFn);
using GetProgramInfoFn = cl_int (*)(cl_program,cl_program_info,size_t,void*,size_t*); DECLARE_REAL(clGetProgramInfo, GetProgramInfoFn);
using GetKernelInfoFn = cl_int (*)(cl_kernel,cl_kernel_info,size_t,void*,size_t*); DECLARE_REAL(clGetKernelInfo, GetKernelInfoFn);
using GetMemInfoFn = cl_int (*)(cl_mem,cl_mem_info,size_t,void*,size_t*); DECLARE_REAL(clGetMemObjectInfo, GetMemInfoFn);

static void QueryProgramMetadata(cl_program p, char* extra, size_t extra_size) {
    if (!p || !g_clGetProgramInfo || !extra || !extra_size) return;
    cl_uint num_devices = 0;
    cl_uint ref_count = 0;
    size_t binary_bytes = 0;
    size_t num_kernels = 0;
    if (g_clGetProgramInfo(p, CL_PROGRAM_NUM_DEVICES, sizeof(num_devices),
                           &num_devices, nullptr) == CL_SUCCESS &&
        num_devices > 0 && num_devices <= 16) {
        size_t sizes[16] = {};
        if (g_clGetProgramInfo(p, CL_PROGRAM_BINARY_SIZES,
                               num_devices * sizeof(size_t), sizes, nullptr) == CL_SUCCESS) {
            for (cl_uint i = 0; i < num_devices; ++i) binary_bytes += sizes[i];
        }
    }
    if (g_clGetProgramInfo(p, CL_PROGRAM_NUM_KERNELS, sizeof(num_kernels),
                           &num_kernels, nullptr) != CL_SUCCESS) {
        num_kernels = 0;
    }
    if (g_clGetProgramInfo(p, CL_PROGRAM_REFERENCE_COUNT, sizeof(ref_count),
                           &ref_count, nullptr) != CL_SUCCESS) {
        ref_count = 0;
    }
    int used = snprintf(extra, extra_size,
                        "binary_bytes=%zu num_devices=%u num_kernels=%zu ref_count=%u",
                        binary_bytes, num_devices, num_kernels, ref_count);
    if (used < 0 || static_cast<size_t>(used) >= extra_size) return;
    size_t names_size = 0;
    if (num_kernels &&
        g_clGetProgramInfo(p, CL_PROGRAM_KERNEL_NAMES, 0, nullptr,
                           &names_size) == CL_SUCCESS &&
        names_size > 1 && names_size < 1024) {
        char names[1024] = {};
        if (g_clGetProgramInfo(p, CL_PROGRAM_KERNEL_NAMES, names_size, names,
                               nullptr) == CL_SUCCESS) {
            snprintf(extra + used, extra_size - static_cast<size_t>(used),
                     " kernel_names=%s", names);
        }
    }
}

static cl_program ProbeCreateBinary(cl_context c,cl_uint n,const cl_device_id* d,const size_t* len,const unsigned char** b,cl_int* bs,cl_int* e) {
    size_t s=(len&&n)?len[0]:0;
    TraceBegin("clCreateProgramWithBinary",reinterpret_cast<uintptr_t>(c),s);
    cl_program r=g_clCreateProgramWithBinary(c,n,d,len,b,bs,e);
    char extra[1024] = {};
    if (r && (!e || *e == CL_SUCCESS)) QueryProgramMetadata(r, extra, sizeof(extra));
    TraceEnd("clCreateProgramWithBinary",reinterpret_cast<uintptr_t>(r),s,e?*e:CL_SUCCESS,extra[0]?extra:nullptr);
    return r;
}
using BuildProgramFn = cl_int (*)(cl_program,cl_uint,const cl_device_id*,const char*,void(CL_CALLBACK*)(cl_program,void*),void*); DECLARE_REAL(clBuildProgram, BuildProgramFn);
static cl_int ProbeBuildProgram(cl_program p,cl_uint n,const cl_device_id* d,const char* o,void(CL_CALLBACK* cb)(cl_program,void*),void* u) {
    TraceBegin("clBuildProgram",reinterpret_cast<uintptr_t>(p),0,o);
    cl_int r=g_clBuildProgram(p,n,d,o,cb,u);
    char extra[1024] = {};
    if (r == CL_SUCCESS) QueryProgramMetadata(p, extra, sizeof(extra));
    TraceEnd("clBuildProgram",reinterpret_cast<uintptr_t>(p),0,r,extra[0]?extra:o);
    return r;
}
using CompileProgramFn = cl_int (*)(cl_program,cl_uint,const cl_device_id*,const char*,cl_uint,const cl_program*,cl_uint,const char**,void(CL_CALLBACK*)(cl_program,void*),void*);
DECLARE_REAL(clCompileProgram, CompileProgramFn);
static cl_int ProbeCompileProgram(cl_program p,cl_uint n,const cl_device_id* d,const char* o,cl_uint h,const cl_program* hp,cl_uint ih,const char** hn,void(CL_CALLBACK* cb)(cl_program,void*),void* u) { TraceBegin("clCompileProgram",reinterpret_cast<uintptr_t>(p),0,o); cl_int r=g_clCompileProgram(p,n,d,o,h,hp,ih,hn,cb,u); TraceEnd("clCompileProgram",reinterpret_cast<uintptr_t>(p),0,r,o); return r; }
using LinkProgramFn = cl_program (*)(cl_context,cl_uint,const cl_device_id*,const char*,cl_uint,const cl_program*,void(CL_CALLBACK*)(cl_program,void*),void*,cl_int*);
DECLARE_REAL(clLinkProgram, LinkProgramFn);
static cl_program ProbeLinkProgram(cl_context c,cl_uint n,const cl_device_id* d,const char* o,cl_uint np,const cl_program* p,void(CL_CALLBACK* cb)(cl_program,void*),void* u,cl_int* e) { TraceBegin("clLinkProgram",reinterpret_cast<uintptr_t>(c),0,o); cl_program r=g_clLinkProgram(c,n,d,o,np,p,cb,u,e); TraceEnd("clLinkProgram",reinterpret_cast<uintptr_t>(r),0,e?*e:CL_SUCCESS,o); return r; }
using ReleaseProgramFn = cl_int (*)(cl_program); DECLARE_REAL(clReleaseProgram, ReleaseProgramFn);
using RetainProgramFn = cl_int (*)(cl_program); DECLARE_REAL(clRetainProgram, RetainProgramFn);
static cl_int ProbeRetainProgram(cl_program p) { TraceBegin("clRetainProgram",reinterpret_cast<uintptr_t>(p)); cl_int r=g_clRetainProgram(p); char extra[1024] = {}; if(r==CL_SUCCESS) QueryProgramMetadata(p,extra,sizeof(extra)); TraceEnd("clRetainProgram",reinterpret_cast<uintptr_t>(p),0,r,extra[0]?extra:nullptr); return r; }
static cl_int ProbeReleaseProgram(cl_program p) { char extra[1024] = {}; QueryProgramMetadata(p,extra,sizeof(extra)); TraceBegin("clReleaseProgram",reinterpret_cast<uintptr_t>(p),0,extra[0]?extra:nullptr); cl_int r=g_clReleaseProgram(p); TraceEnd("clReleaseProgram",reinterpret_cast<uintptr_t>(p),0,r,extra[0]?extra:nullptr); return r; }
using CreateKernelFn = cl_kernel (*)(cl_program,const char*,cl_int*); DECLARE_REAL(clCreateKernel, CreateKernelFn);
static cl_kernel ProbeCreateKernel(cl_program p,const char* n,cl_int* e) {
    TraceBegin("clCreateKernel",reinterpret_cast<uintptr_t>(p),0,n);
    cl_kernel r=g_clCreateKernel(p,n,e);
    char extra[1024] = {};
    snprintf(extra, sizeof(extra), "program=0x%llx name=%s",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)),
             n ? n : "<unnamed>");
    TraceEnd("clCreateKernel",reinterpret_cast<uintptr_t>(r),0,e?*e:CL_SUCCESS,extra);
    return r;
}
using ReleaseKernelFn = cl_int (*)(cl_kernel); DECLARE_REAL(clReleaseKernel, ReleaseKernelFn);
using RetainKernelFn = cl_int (*)(cl_kernel); DECLARE_REAL(clRetainKernel, RetainKernelFn);
static void QueryKernelMetadata(cl_kernel k, char* extra, size_t extra_size) {
    if (!k || !g_clGetKernelInfo || !extra || !extra_size) return;
    cl_uint ref_count = 0;
    cl_program program = nullptr;
    char name[256] = {};
    g_clGetKernelInfo(k, CL_KERNEL_REFERENCE_COUNT, sizeof(ref_count),
                      &ref_count, nullptr);
    g_clGetKernelInfo(k, CL_KERNEL_PROGRAM, sizeof(program), &program, nullptr);
    g_clGetKernelInfo(k, CL_KERNEL_FUNCTION_NAME, sizeof(name), name, nullptr);
    snprintf(extra, extra_size, "program=0x%llx ref_count=%u name=%s",
             static_cast<unsigned long long>(
                     reinterpret_cast<uintptr_t>(program)),
             ref_count, name[0] ? name : "<unnamed>");
}
static cl_int ProbeRetainKernel(cl_kernel k) { TraceBegin("clRetainKernel",reinterpret_cast<uintptr_t>(k)); cl_int r=g_clRetainKernel(k); char extra[512] = {}; if(r==CL_SUCCESS) QueryKernelMetadata(k,extra,sizeof(extra)); TraceEnd("clRetainKernel",reinterpret_cast<uintptr_t>(k),0,r,extra[0]?extra:nullptr); return r; }
static cl_int ProbeReleaseKernel(cl_kernel k) { char extra[512] = {}; QueryKernelMetadata(k,extra,sizeof(extra)); TraceBegin("clReleaseKernel",reinterpret_cast<uintptr_t>(k),0,extra[0]?extra:nullptr); cl_int r=g_clReleaseKernel(k); TraceEnd("clReleaseKernel",reinterpret_cast<uintptr_t>(k),0,r,extra[0]?extra:nullptr); return r; }
using DispatchFn = cl_int (*)(cl_command_queue,cl_kernel,cl_uint,const size_t*,const size_t*,const size_t*,cl_uint,const cl_event*,cl_event*); DECLARE_REAL(clEnqueueNDRangeKernel, DispatchFn);
static cl_int ProbeDispatch(cl_command_queue q, cl_kernel k, cl_uint dim,
                            const size_t* off, const size_t* global,
                            const size_t* local, cl_uint n,
                            const cl_event* wl, cl_event* ev) {
    unsigned every = DispatchEvery();
    auto count = g_dispatch_count.fetch_add(1, std::memory_order_relaxed) + 1;
    bool mark = every && count % every == 0;
    char extra[192] = {};
    if (mark) {
        snprintf(extra, sizeof(extra),
                 "dispatch=%llu dim=%u global=%zu,%zu,%zu local=%zu,%zu,%zu",
                 count, dim, global ? global[0] : 0,
                 dim > 1 && global ? global[1] : 0,
                 dim > 2 && global ? global[2] : 0,
                 local ? local[0] : 0, dim > 1 && local ? local[1] : 0,
                 dim > 2 && local ? local[2] : 0);
        TraceBegin("clEnqueueNDRangeKernel", reinterpret_cast<uintptr_t>(k), 0,
                   extra);
    }
    cl_int result = g_clEnqueueNDRangeKernel
                            ? g_clEnqueueNDRangeKernel(
                                      q, k, dim, off, global, local, n, wl, ev)
                            : CL_INVALID_OPERATION;
    if (mark) {
        TraceEnd("clEnqueueNDRangeKernel", reinterpret_cast<uintptr_t>(k), 0,
                 result, extra);
    }
    return result;
}
using FinishFn = cl_int (*)(cl_command_queue); DECLARE_REAL(clFinish, FinishFn);
static cl_int ProbeFinish(cl_command_queue q) { TraceBegin("clFinish",reinterpret_cast<uintptr_t>(q)); cl_int r=g_clFinish(q); TraceEnd("clFinish",reinterpret_cast<uintptr_t>(q),0,r); return r; }
using UnloadCompilerFn = cl_int (*)(); DECLARE_REAL(clUnloadCompiler, UnloadCompilerFn);
static cl_int ProbeUnloadCompiler() { TraceBegin("clUnloadCompiler"); cl_int r=g_clUnloadCompiler ? g_clUnloadCompiler() : CL_INVALID_OPERATION; TraceEnd("clUnloadCompiler",0,0,r); return r; }
using UnloadPlatformCompilerFn = cl_int (*)(cl_platform_id); DECLARE_REAL(clUnloadPlatformCompiler, UnloadPlatformCompilerFn);
static cl_int ProbeUnloadPlatformCompiler(cl_platform_id p) { TraceBegin("clUnloadPlatformCompiler",reinterpret_cast<uintptr_t>(p)); cl_int r=g_clUnloadPlatformCompiler ? g_clUnloadPlatformCompiler(p) : CL_INVALID_OPERATION; TraceEnd("clUnloadPlatformCompiler",reinterpret_cast<uintptr_t>(p),0,r); return r; }
static cl_int ProbeGetProgramInfo(cl_program p,cl_program_info param,size_t size,void* value,size_t* size_ret) {
    TraceBegin("clGetProgramInfo",reinterpret_cast<uintptr_t>(p),size);
    cl_int r=g_clGetProgramInfo(p,param,size,value,size_ret);
    char extra[128];
    snprintf(extra,sizeof(extra),"param=0x%x result_bytes=%zu",param,size_ret?*size_ret:0);
    TraceEnd("clGetProgramInfo",reinterpret_cast<uintptr_t>(p),size,r,extra);
    return r;
}
static cl_int ProbeGetKernelInfo(cl_kernel k,cl_kernel_info param,size_t size,void* value,size_t* size_ret) {
    TraceBegin("clGetKernelInfo",reinterpret_cast<uintptr_t>(k),size);
    cl_int r=g_clGetKernelInfo(k,param,size,value,size_ret);
    uintptr_t program=0;
    if(r==CL_SUCCESS&&param==CL_KERNEL_PROGRAM&&value&&size>=sizeof(cl_program)) program=reinterpret_cast<uintptr_t>(*static_cast<cl_program*>(value));
    char extra[160];
    snprintf(extra,sizeof(extra),"param=0x%x result_bytes=%zu program=0x%llx",param,size_ret?*size_ret:0,static_cast<unsigned long long>(program));
    TraceEnd("clGetKernelInfo",reinterpret_cast<uintptr_t>(k),size,r,extra);
    return r;
}
static cl_int ProbeGetMemInfo(cl_mem m,cl_mem_info param,size_t size,void* value,size_t* size_ret) {
    TraceBegin("clGetMemObjectInfo",reinterpret_cast<uintptr_t>(m),size);
    cl_int r=g_clGetMemObjectInfo(m,param,size,value,size_ret);
    size_t reported=0;
    if(r==CL_SUCCESS&&param==CL_MEM_SIZE&&value&&size>=sizeof(size_t)) reported=*static_cast<size_t*>(value);
    char extra[160];
    snprintf(extra,sizeof(extra),"param=0x%x result_bytes=%zu reported_size=%zu",param,size_ret?*size_ret:0,reported);
    TraceEnd("clGetMemObjectInfo",reinterpret_cast<uintptr_t>(m),size,r,extra);
    return r;
}

struct Wrapper { const char* name; void** real_slot; void* probe; };
static Wrapper kWrappers[] = {
    {"clCreateContext",reinterpret_cast<void**>(&g_clCreateContext),(void*)ProbeCreateContext}, {"clReleaseContext",reinterpret_cast<void**>(&g_clReleaseContext),(void*)ProbeReleaseContext},
    {"clCreateCommandQueue",reinterpret_cast<void**>(&g_clCreateCommandQueue),(void*)ProbeCreateQueue}, {"clCreateCommandQueueWithProperties",reinterpret_cast<void**>(&g_clCreateCommandQueueWithProperties),(void*)ProbeCreateQueueProps}, {"clReleaseCommandQueue",reinterpret_cast<void**>(&g_clReleaseCommandQueue),(void*)ProbeReleaseQueue},
    {"clCreateBuffer",reinterpret_cast<void**>(&g_clCreateBuffer),(void*)ProbeCreateBuffer}, {"clCreateImage",reinterpret_cast<void**>(&g_clCreateImage),(void*)ProbeCreateImage}, {"clRetainMemObject",reinterpret_cast<void**>(&g_clRetainMemObject),(void*)ProbeRetainMem}, {"clReleaseMemObject",reinterpret_cast<void**>(&g_clReleaseMemObject),(void*)ProbeReleaseMem},
    {"clSVMAlloc",reinterpret_cast<void**>(&g_clSVMAlloc),(void*)ProbeSvmAlloc}, {"clSVMFree",reinterpret_cast<void**>(&g_clSVMFree),(void*)ProbeSvmFree},
    {"clCreateProgramWithSource",reinterpret_cast<void**>(&g_clCreateProgramWithSource),(void*)ProbeCreateSource}, {"clCreateProgramWithBinary",reinterpret_cast<void**>(&g_clCreateProgramWithBinary),(void*)ProbeCreateBinary}, {"clBuildProgram",reinterpret_cast<void**>(&g_clBuildProgram),(void*)ProbeBuildProgram}, {"clCompileProgram",reinterpret_cast<void**>(&g_clCompileProgram),(void*)ProbeCompileProgram}, {"clLinkProgram",reinterpret_cast<void**>(&g_clLinkProgram),(void*)ProbeLinkProgram}, {"clRetainProgram",reinterpret_cast<void**>(&g_clRetainProgram),(void*)ProbeRetainProgram}, {"clReleaseProgram",reinterpret_cast<void**>(&g_clReleaseProgram),(void*)ProbeReleaseProgram},
    {"clCreateKernel",reinterpret_cast<void**>(&g_clCreateKernel),(void*)ProbeCreateKernel}, {"clRetainKernel",reinterpret_cast<void**>(&g_clRetainKernel),(void*)ProbeRetainKernel}, {"clReleaseKernel",reinterpret_cast<void**>(&g_clReleaseKernel),(void*)ProbeReleaseKernel}, {"clEnqueueNDRangeKernel",reinterpret_cast<void**>(&g_clEnqueueNDRangeKernel),(void*)ProbeDispatch}, {"clFinish",reinterpret_cast<void**>(&g_clFinish),(void*)ProbeFinish}, {"clUnloadCompiler",reinterpret_cast<void**>(&g_clUnloadCompiler),(void*)ProbeUnloadCompiler}, {"clUnloadPlatformCompiler",reinterpret_cast<void**>(&g_clUnloadPlatformCompiler),(void*)ProbeUnloadPlatformCompiler},
    {"clGetProgramInfo",reinterpret_cast<void**>(&g_clGetProgramInfo),(void*)ProbeGetProgramInfo}, {"clGetKernelInfo",reinterpret_cast<void**>(&g_clGetKernelInfo),(void*)ProbeGetKernelInfo}, {"clGetMemObjectInfo",reinterpret_cast<void**>(&g_clGetMemObjectInfo),(void*)ProbeGetMemInfo},
};
}  // namespace

extern "C" void* dlsym(void* handle, const char* name) {
    if (g_resolving_real_dlsym) return nullptr;
    ResolveRealDlsym();
    if (!g_real_dlsym) return nullptr;
    void* real = g_real_dlsym(handle, name);
    return malloc_hook_opencl_maybe_wrap(name, real);
}

extern "C" void* malloc_hook_opencl_maybe_wrap(const char* name, void* real_symbol) {
    if (!name || !real_symbol) return real_symbol;
    for (auto& wrapper : kWrappers) {
        if (strcmp(name, wrapper.name) == 0) {
            if (!Enabled()) return real_symbol;
            *wrapper.real_slot = real_symbol;
            return wrapper.probe;
        }
    }
    return real_symbol;
}

extern "C" void malloc_hook_opencl_get_snapshot(
        size_t* live_requested_bytes, const char** last_api) {
    if (live_requested_bytes != nullptr) {
        *live_requested_bytes =
                g_live_requested_bytes.load(std::memory_order_relaxed);
    }
    if (last_api != nullptr) {
        *last_api = g_last_api.load(std::memory_order_relaxed);
    }
}
