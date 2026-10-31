#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#if defined(MALLOC_HOOK_TARGET_OS_OHOS)
#include <asm-generic/ioctl.h>
#endif
#include <pthread.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>

#include "DebugData.h"
#include "ObserveOnlyProbe.h"
#include "PointerData.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#define RESOLVE(name)                                                              \
    do {                                                                           \
        if (m_sys_##name == nullptr) {                                             \
            auto addr = ResolveLibcSymbol(#name);                                  \
            if (addr) {                                                            \
                m_sys_##name = reinterpret_cast<decltype(m_sys_##name)>(addr);     \
            }                                                                      \
        }                                                                          \
    } while (0)

struct InitState {
    InitState() { allocHook_setup = true; }
    ~InitState() { allocHook_setup = false; }
    static volatile bool allocHook_setup;
};
volatile bool InitState::allocHook_setup = false;

namespace {

volatile bool g_resolving_symbols = false;

#if defined(MALLOC_HOOK_TARGET_OS_ANDROID) || defined(MALLOC_HOOK_TARGET_OS_OHOS)
using HookIoctlRequest = int;
#else
using HookIoctlRequest = unsigned long;
#endif

struct alignas(std::max_align_t) BootstrapHeader {
    void* map_base;
    size_t map_size;
    size_t requested_size;
    uint64_t magic;
    BootstrapHeader* next;
};

constexpr uint64_t kBootstrapMagic = 0x6d616c6c6f635f68ULL;
BootstrapHeader* g_bootstrap_list = nullptr;

bool IsPowerOfTwo(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool CheckedAdd(size_t lhs, size_t rhs, size_t* result) {
    if (rhs > SIZE_MAX - lhs) {
        return false;
    }
    *result = lhs + rhs;
    return true;
}

size_t PageAlign(size_t size) {
    long page_size = sysconf(_SC_PAGESIZE);
    size_t alignment = page_size > 0 ? static_cast<size_t>(page_size) : 4096;
    if (!IsPowerOfTwo(alignment) || size > SIZE_MAX - (alignment - 1)) {
        return 0;
    }
    return (size + alignment - 1) & ~(alignment - 1);
}

bool IsBootstrapPointer(const void* ptr) {
    if (ptr == nullptr) {
        return false;
    }
    for (BootstrapHeader* header = g_bootstrap_list; header != nullptr; header = header->next) {
        if (header->magic == kBootstrapMagic && header + 1 == ptr) {
            return true;
        }
    }
    return false;
}

void* BootstrapAlignedMalloc(size_t alignment, size_t size) {
    if (!IsPowerOfTwo(alignment) || alignment < alignof(void*)) {
        errno = EINVAL;
        return nullptr;
    }
    const size_t effective_alignment =
            std::max(alignment, alignof(BootstrapHeader));
    size_t payload = 0;
    size_t total = 0;
    if (!CheckedAdd(sizeof(BootstrapHeader), size, &payload) ||
        !CheckedAdd(payload, effective_alignment - 1, &total)) {
        errno = ENOMEM;
        return nullptr;
    }
    size_t map_size = PageAlign(total);
    if (map_size == 0) {
        errno = ENOMEM;
        return nullptr;
    }
    void* map = reinterpret_cast<void*>(syscall(
            SYS_mmap, nullptr, map_size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (map == MAP_FAILED) {
        return nullptr;
    }
    const uintptr_t first_payload =
            reinterpret_cast<uintptr_t>(map) + sizeof(BootstrapHeader);
    const uintptr_t aligned_payload =
            (first_payload + effective_alignment - 1) &
            ~(effective_alignment - 1);
    auto* header = reinterpret_cast<BootstrapHeader*>(aligned_payload) - 1;
    header->map_base = map;
    header->map_size = map_size;
    header->requested_size = size;
    header->magic = kBootstrapMagic;
    header->next = g_bootstrap_list;
    g_bootstrap_list = header;
    return header + 1;
}

void* BootstrapMalloc(size_t size) {
    return BootstrapAlignedMalloc(alignof(std::max_align_t), size);
}

void BootstrapFree(void* ptr) {
    if (!IsBootstrapPointer(ptr)) {
        return;
    }
    auto* header = reinterpret_cast<BootstrapHeader*>(ptr) - 1;
    BootstrapHeader** current = &g_bootstrap_list;
    while (*current != nullptr && *current != header) {
        current = &(*current)->next;
    }
    if (*current == header) {
        *current = header->next;
    }
    size_t map_size = header->map_size;
    header->magic = 0;
    syscall(SYS_munmap, header->map_base, map_size);
}

void* BootstrapCalloc(size_t nmemb, size_t size) {
    size_t total = 0;
    if (__builtin_mul_overflow(nmemb, size, &total)) {
        errno = ENOMEM;
        return nullptr;
    }
    void* ptr = BootstrapMalloc(total);
    if (ptr != nullptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void* BootstrapRealloc(void* ptr, size_t size) {
    if (ptr == nullptr) {
        return BootstrapMalloc(size);
    }
    if (size == 0) {
        BootstrapFree(ptr);
        return nullptr;
    }
    auto* header = IsBootstrapPointer(ptr)
            ? reinterpret_cast<BootstrapHeader*>(ptr) - 1
            : nullptr;
    size_t old_size = header == nullptr ? 0 : header->requested_size;
    void* new_ptr = BootstrapMalloc(size);
    if (new_ptr != nullptr && old_size != 0) {
        memcpy(new_ptr, ptr, std::min(old_size, size));
        BootstrapFree(ptr);
    }
    return new_ptr;
}

void* ResolveLibcSymbol(const char* name) {
    if (g_resolving_symbols) {
        return nullptr;
    }

    g_resolving_symbols = true;
    void* addr = dlsym(RTLD_NEXT, name);
#if !defined(MALLOC_HOOK_TARGET_OS_OHOS)
    if (addr == nullptr) {
        void* handle = dlopen("libc.so", RTLD_LAZY);
        if (handle) {
            addr = dlsym(handle, name);
            dlclose(handle);
        }
    }
#endif
    g_resolving_symbols = false;
    return addr;
}

using ForkFn = pid_t (*)();
std::atomic<ForkFn> g_real_fork{nullptr};

ForkFn RealForkFunction() {
    ForkFn fn = g_real_fork.load(std::memory_order_acquire);
    if (fn == nullptr) {
        fn = reinterpret_cast<ForkFn>(ResolveLibcSymbol("fork"));
        if (fn != nullptr) {
            g_real_fork.store(fn, std::memory_order_release);
        }
    }
    return fn;
}

void ResolveAllLibcAllocationSymbols() {
    RESOLVE(malloc);
    RESOLVE(free);
    RESOLVE(calloc);
    RESOLVE(realloc);
    RESOLVE(memalign);
    RESOLVE(aligned_alloc);
    RESOLVE(posix_memalign);
#if MALLOC_HOOK_EXPORT_RESOURCE_HOOKS
    RESOLVE(dup);
    RESOLVE(dup2);
    RESOLVE(dup3);
    RESOLVE(fcntl);
#if defined(MALLOC_HOOK_TARGET_OS_LINUX)
    RESOLVE(fcntl64);
#endif
#endif
#if MALLOC_HOOK_EXPORT_MMAP_HOOK
    RESOLVE(mmap);
    RESOLVE(munmap);
    RESOLVE(mremap);
#if !defined(mmap64)
    RESOLVE(mmap64);
#endif
#endif
}

#if MALLOC_HOOK_EXPORT_RESOURCE_HOOKS
int CallRealIoctl(int fd, unsigned long request, void* arg) {
    return static_cast<int>(syscall(SYS_ioctl, fd, request, arg));
}

int CallRealClose(int fd) {
    return static_cast<int>(syscall(SYS_close, fd));
}

int CallRealDup(int old_fd) {
    RESOLVE(dup);
    return m_sys_dup != nullptr ? m_sys_dup(old_fd)
                                : static_cast<int>(syscall(SYS_dup, old_fd));
}

int CallRealDup2(int old_fd, int new_fd) {
    RESOLVE(dup2);
    if (m_sys_dup2 != nullptr) {
        return m_sys_dup2(old_fd, new_fd);
    }
#if defined(SYS_dup2)
    return static_cast<int>(syscall(SYS_dup2, old_fd, new_fd));
#else
    return static_cast<int>(syscall(SYS_dup3, old_fd, new_fd, 0));
#endif
}

int CallRealDup3(int old_fd, int new_fd, int flags) {
    RESOLVE(dup3);
    if (m_sys_dup3 != nullptr) {
        return m_sys_dup3(old_fd, new_fd, flags);
    }
#if defined(SYS_dup3)
    return static_cast<int>(syscall(SYS_dup3, old_fd, new_fd, flags));
#else
    errno = ENOSYS;
    return -1;
#endif
}

int CallRealFcntl(int fd, int command, uintptr_t argument) {
    RESOLVE(fcntl);
    return m_sys_fcntl != nullptr
                 ? m_sys_fcntl(fd, command, argument)
                 : static_cast<int>(syscall(SYS_fcntl, fd, command, argument));
}
#if defined(MALLOC_HOOK_TARGET_OS_LINUX)
int CallRealFcntl64(int fd, int command, uintptr_t argument) {
    RESOLVE(fcntl64);
    return m_sys_fcntl64 != nullptr ? m_sys_fcntl64(fd, command, argument)
                                    : CallRealFcntl(fd, command, argument);
}

int InvokeRealFcntl64(int fd, int command, uintptr_t argument, void*) {
    return CallRealFcntl64(fd, command, argument);
}
#endif

#if defined(MALLOC_HOOK_TARGET_OS_ANDROID)
using FdsanCloseFn = int (*)(int, uint64_t);
std::atomic<FdsanCloseFn> g_real_fdsan_close{nullptr};
pthread_mutex_t g_fdsan_resolve_mutex = PTHREAD_MUTEX_INITIALIZER;
std::atomic<bool> g_fdsan_resolution_attempted{false};

FdsanCloseFn RealFdsanCloseFunction() {
    FdsanCloseFn function = g_real_fdsan_close.load(std::memory_order_acquire);
    if (function != nullptr ||
        g_fdsan_resolution_attempted.load(std::memory_order_acquire)) {
        return function;
    }
    pthread_mutex_lock(&g_fdsan_resolve_mutex);
    if (!g_fdsan_resolution_attempted.load(std::memory_order_acquire)) {
        function = reinterpret_cast<FdsanCloseFn>(
                ResolveLibcSymbol("android_fdsan_close_with_tag"));
        g_real_fdsan_close.store(function, std::memory_order_release);
        g_fdsan_resolution_attempted.store(true, std::memory_order_release);
    } else {
        function = g_real_fdsan_close.load(std::memory_order_acquire);
    }
    pthread_mutex_unlock(&g_fdsan_resolve_mutex);
    return function;
}

struct FdsanCloseContext {
    FdsanCloseFn function = nullptr;
    uint64_t tag = 0;
};

int InvokeFdsanClose(int fd, void* opaque) {
    auto* context = static_cast<FdsanCloseContext*>(opaque);
    return context->function(fd, context->tag);
}
#endif

enum class FcntlArgumentKind { None, Integer, Pointer, Word };

FcntlArgumentKind FcntlArgumentKindForCommand(int command) {
    switch (command) {
        case F_GETFD:
        case F_GETFL:
#ifdef F_GETOWN
        case F_GETOWN:
#endif
#ifdef F_GETSIG
        case F_GETSIG:
#endif
#ifdef F_GETLEASE
        case F_GETLEASE:
#endif
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
#ifdef F_GET_SEALS
        case F_GET_SEALS:
#endif
            return FcntlArgumentKind::None;

        case F_DUPFD:
        case F_SETFD:
        case F_SETFL:
#ifdef F_DUPFD_CLOEXEC
        case F_DUPFD_CLOEXEC:
#endif
#ifdef F_SETOWN
        case F_SETOWN:
#endif
#ifdef F_SETSIG
        case F_SETSIG:
#endif
#ifdef F_SETLEASE
        case F_SETLEASE:
#endif
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
#ifdef F_ADD_SEALS
        case F_ADD_SEALS:
#endif
            return FcntlArgumentKind::Integer;

        case F_GETLK:
        case F_SETLK:
        case F_SETLKW:
#ifdef F_GETOWN_EX
        case F_GETOWN_EX:
#endif
#ifdef F_SETOWN_EX
        case F_SETOWN_EX:
#endif
#ifdef F_OFD_GETLK
        case F_OFD_GETLK:
#endif
#ifdef F_OFD_SETLK
        case F_OFD_SETLK:
#endif
#ifdef F_OFD_SETLKW
        case F_OFD_SETLKW:
#endif
#ifdef F_GET_RW_HINT
        case F_GET_RW_HINT:
#endif
#ifdef F_SET_RW_HINT
        case F_SET_RW_HINT:
#endif
#ifdef F_GET_FILE_RW_HINT
        case F_GET_FILE_RW_HINT:
#endif
#ifdef F_SET_FILE_RW_HINT
        case F_SET_FILE_RW_HINT:
#endif
            return FcntlArgumentKind::Pointer;
        default:
            return FcntlArgumentKind::Word;
    }
}
#endif

#if MALLOC_HOOK_EXPORT_MMAP_HOOK
void* CallRealMmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    RESOLVE(mmap);
    if (m_sys_mmap != nullptr) {
        return m_sys_mmap(addr, size, prot, flags, fd, offset);
    }
    return reinterpret_cast<void*>(syscall(SYS_mmap, addr, size, prot, flags, fd, offset));
}

int CallRealMunmap(void* addr, size_t size) {
    RESOLVE(munmap);
    if (m_sys_munmap != nullptr) {
        return m_sys_munmap(addr, size);
    }
    return static_cast<int>(syscall(SYS_munmap, addr, size));
}

void* CallRealMremap(
        void* old_addr, size_t old_size, size_t new_size, int flags, void* new_addr) {
    RESOLVE(mremap);
    if (m_sys_mremap != nullptr) {
        if ((flags & MREMAP_FIXED) != 0) {
            return m_sys_mremap(old_addr, old_size, new_size, flags, new_addr);
        }
        return m_sys_mremap(old_addr, old_size, new_size, flags);
    }
#if defined(SYS_mremap)
    if ((flags & MREMAP_FIXED) != 0) {
        return reinterpret_cast<void*>(
                syscall(SYS_mremap, old_addr, old_size, new_size, flags, new_addr));
    }
    return reinterpret_cast<void*>(
            syscall(SYS_mremap, old_addr, old_size, new_size, flags));
#else
    errno = ENOSYS;
    return MAP_FAILED;
#endif
}

#if !defined(mmap64)
void* CallRealMmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    RESOLVE(mmap64);
    if (m_sys_mmap64 != nullptr) {
        return m_sys_mmap64(addr, size, prot, flags, fd, offset);
    }
    return CallRealMmap(addr, size, prot, flags, fd, offset);
}
#endif
#endif

struct AddressRange {
    uintptr_t begin = 0;
    uintptr_t end = 0;

    bool Contains(const void* address) const {
        uintptr_t value = reinterpret_cast<uintptr_t>(address);
        return begin <= value && value < end;
    }

    bool Valid() const { return begin < end; }
};

AddressRange g_libc_range;

int CollectLibcRange(dl_phdr_info* info, size_t, void*) {
    if (info == nullptr || info->dlpi_name == nullptr) {
        return 0;
    }

    const char* so_name = strrchr(info->dlpi_name, '/');
    so_name = (so_name == nullptr) ? info->dlpi_name : so_name + 1;
    if (strcmp(so_name, "libc.so") != 0) {
        return 0;
    }

    uintptr_t begin = UINTPTR_MAX;
    uintptr_t end = 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) {
            continue;
        }

        uintptr_t seg_begin = info->dlpi_addr + phdr.p_vaddr;
        uintptr_t seg_end = seg_begin + phdr.p_memsz;
        if (seg_begin < begin) {
            begin = seg_begin;
        }
        if (seg_end > end) {
            end = seg_end;
        }
    }

    if (begin < end) {
        g_libc_range.begin = begin;
        g_libc_range.end = end;
        return 1;
    }
    return 0;
}

void CacheLibcRange() {
    if (!g_libc_range.Valid()) {
        dl_iterate_phdr(CollectLibcRange, nullptr);
    }
}

bool ShouldBypassLibcInternalMmap(const void* caller) {
    CacheLibcRange();
    return g_libc_range.Valid() && g_libc_range.Contains(caller);
}

void DisableTrackingHeapTagging() {
#if defined(MALLOC_HOOK_TARGET_OS_ANDROID)
    // Raw-pointer tracking requires Bionic heap tagging off. Resolve dynamically
    // because old releases lack mallopt and the NDK declaration is API-gated.
    using MalloptFn = int (*)(int, int);
    constexpr int kBionicSetHeapTaggingLevel = -204;
    constexpr int kHeapTaggingLevelNone = 0;
    auto mallopt = reinterpret_cast<MalloptFn>(ResolveLibcSymbol("mallopt"));
    if (mallopt != nullptr &&
        mallopt(kBionicSetHeapTaggingLevel, kHeapTaggingLevelNone) == 0) {
        static constexpr char kMessage[] =
                "alloc_hook: Bionic heap-tagging disable failed\n";
        syscall(SYS_write, STDERR_FILENO, kMessage, sizeof(kMessage) - 1);
    }
#endif
}

void InitializeTargetScope() {
    InitState state;
    // Early tracking can precede Bionic's final tagging setup. Environment
    // reallocation must use the same policy as those existing allocations.
    if (g_debug != nullptr && observe_only::CurrentMode() == observe_only::kTrack) {
        DisableTrackingHeapTagging();
    }
    // A preinit fork child must not enter dlsym while inheriting loader locks.
    // Resolve every forwarding entry in the parent before libc runs callbacks.
    ResolveAllLibcAllocationSymbols();
    (void)RealForkFunction();
#if defined(MALLOC_HOOK_TARGET_OS_ANDROID) && MALLOC_HOOK_EXPORT_RESOURCE_HOOKS
    (void)RealFdsanCloseFunction();
#endif
    if (observe_only::InitializeTargetProcess() &&
        observe_only::TargetProcessInitialized()) {
        (void)observe_only::IsPrimary();
    }
}

}  // namespace

// Cleared by the constructor below, once the loader has finished with this
// library. Allocations that arrive before that point are still tracked, but any
// work that needs a working libc runtime has to wait for it.
static volatile bool in_preinit_phase = true;

class AllocHook {
public:
    AllocHook() {
        InitState state;
        DisableTrackingHeapTagging();
        ResolveAllLibcAllocationSymbols();
        void* ptr[2] = {&Db_storage, &Pd_storage};
        debug_initialize(ptr);
        CacheLibcRange();
        if (!in_preinit_phase) {
            debug_start_helper_threads();
        }
    }
    ~AllocHook() { debug_finalize(); }

    void* malloc(size_t size) { return debug_malloc(size); }
    void free(void* ptr) { debug_free(ptr); }
    void* calloc(size_t a, size_t b) { return debug_calloc(a, b); }
    void* realloc(void* ptr, size_t size) { return debug_realloc(ptr, size); }
    void* aligned_alloc(size_t alignment, size_t size) {
        return debug_aligned_alloc(alignment, size);
    }
    int posix_memalign(void** ptr, size_t alignment, size_t size) {
        return debug_posix_memalign(ptr, alignment, size);
    }
    void* memalign(size_t alignment, size_t bytes) { return debug_memalign(alignment, bytes); }
    void* mmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
        return debug_mmap(addr, size, prot, flags, fd, offset);
    }
    int munmap(void* addr, size_t size) { return debug_munmap(addr, size); }
    void* mremap(void* old_addr, size_t old_size, size_t new_size,
                 int flags, void* new_addr) {
        return debug_mremap(old_addr, old_size, new_size, flags, new_addr);
    }
    int ioctl(int fd, unsigned long request, void* arg) {
        return debug_ioctl(fd, request, arg);
    }
    int close(int fd) { return debug_close(fd); }
    int close_with(int fd, DebugCloseOperation operation, void* context) {
        return debug_close_with(fd, operation, context);
    }
    int dup(int old_fd) { return debug_dup(old_fd); }
    int dup2(int old_fd, int new_fd) { return debug_dup2(old_fd, new_fd); }
    int dup3(int old_fd, int new_fd, int flags) {
        return debug_dup3(old_fd, new_fd, flags);
    }
    int fcntl_with(
            int fd, int command, uintptr_t argument, DebugFcntlOperation operation,
            void* context) {
        return debug_fcntl_with(fd, command, argument, operation, context);
    }
    int fcntl(int fd, int command, uintptr_t argument) {
        return debug_fcntl(fd, command, argument);
    }
#if !defined(mmap64)
    void* mmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
        return debug_mmap64(addr, size, prot, flags, fd, offset);
    }
#endif

    void checkpoint(const char* file_name) { return debug_dump_heap(file_name); }

    static AllocHook& inst();

private:
    static std::aligned_storage<sizeof(DebugData), alignof(DebugData)>::type Db_storage;
    static std::aligned_storage<sizeof(PointerData), alignof(PointerData)>::type
            Pd_storage;
};
std::aligned_storage<sizeof(DebugData), alignof(DebugData)>::type AllocHook::Db_storage;
std::aligned_storage<sizeof(PointerData), alignof(PointerData)>::type
        AllocHook::Pd_storage;

AllocHook& AllocHook::inst() {
    static AllocHook hook;
    return hook;
}

__attribute__((constructor(201))) void mark_init_done() {
    InitializeTargetScope();
    in_preinit_phase = false;
    if (!observe_only::IsTargetProcess()) {
        return;
    }
    // The loader is done with us, so pthread_create works from here on. If a
    // preinit allocation already built the tracker, this is where its threads
    // start; if not, AllocHook's constructor starts them itself.
    debug_start_helper_threads();
    // The probe's own thread, for the same reason and at the same instant. A
    // no-op unless this run asked for a cadence and no report; the two are
    // mutually exclusive, so at most one of them starts a sampler.
    observe_only::StartProbe();
}

// The probe's counterpart to AllocHook's destructor, which is what emits the exit
// report in tracking mode. Exactly one of the two runs: the tracker is never
// constructed in observe-only mode, and the probe never starts in any other one.
//
// This is the dlclose path only. Process exit is covered by the atexit()
// registration in StartProbe(), because Bionic does not run a preloaded library's
// .fini_array at exit; the report itself reports once either way.
__attribute__((destructor(201))) void report_observe_only_probe() {
    observe_only::ReportAtExit();
}

// Hook-source capability contract:
// - Current creation sources are the malloc family, C++ new/new[], successful
//   anonymous mmap/mmap64, selected successful resource-allocating ioctl calls,
//   and DMA-BUF mappings discovered by fd identity. DMA ownership follows the
//   object across fd aliases (dup family/fcntl) and mapped ranges.
//   Successful mremap preserves the original stack identity while updating the
//   tracked mapping ownership and size.
// - Release paths reuse stored object identity. close, Android fdsan close,
//   munmap, and replacement dup operations all terminate only the ownership they
//   actually release; the object closes when its final fd/mapping owner is gone.
// - mmap/ioctl/close/dup/fcntl interposition covers exported libc calls only;
//   direct syscalls are an explicit limitation. Platform export policy decides
//   which sources are enabled without changing the shared lifecycle contract.
// - ARCH-01 source ownership is local to this adapter: malloc family and C++
//   new creation enter the shared tracker, anonymous mmap is success-only when
//   exported, and resource ioctl records are success/filter gated. Direct
//   mremap syscalls remain outside the interposition boundary, as do
//   managed-runtime/other-thread captures.
//
// Every interposer below also takes the observe-only gate. Interposition itself
// cannot be switched off -- these symbols are exported and the loader has already
// bound them -- but a run with no report to produce must not pay for the tracker,
// so the gate sends the call straight to libc and the tracker is never
// constructed at all. See ObserveOnlyProbe.h.
extern "C" {
pid_t fork() {
    const int saved_errno = errno;
    // Executable preinit callbacks run before this library's constructors.
    // Publish scope before libc invokes any application child callbacks.
    if (!observe_only::TargetProcessInitialized()) {
        InitializeTargetScope();
    }
    ForkFn fn = RealForkFunction();
    if (fn == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    errno = saved_errno;
    return fn();
}

// 程序初始化会间接调用 malloc 和 free
void* malloc(size_t size) {
    RESOLVE(malloc);
    if (InitState::allocHook_setup || g_resolving_symbols || m_sys_malloc == nullptr ||
        observe_only::Bypassed()) {
        if (m_sys_malloc == nullptr) {
            return BootstrapMalloc(size);
        }
        return m_sys_malloc(size);
    }
    return AllocHook::inst().malloc(size);
}

void free(void* ptr) {
    if (IsBootstrapPointer(ptr)) {
        BootstrapFree(ptr);
        return;
    }
    RESOLVE(free);
    if (InitState::allocHook_setup || g_resolving_symbols || m_sys_free == nullptr ||
        observe_only::Bypassed()) {
        if (m_sys_free == nullptr) {
            return;
        }
        return m_sys_free(ptr);
    }
    return AllocHook::inst().free(ptr);
}

// calloc 和 realloc 属于用户级函数
void* calloc(size_t a, size_t b) {
    RESOLVE(calloc);
    if (InitState::allocHook_setup || g_resolving_symbols || m_sys_calloc == nullptr ||
        observe_only::Bypassed()) {
        if (m_sys_calloc == nullptr) {
            return BootstrapCalloc(a, b);
        }
        return m_sys_calloc(a, b);
    }
    return AllocHook::inst().calloc(a, b);
}

void* realloc(void* ptr, size_t size) {
    bool bootstrap_ptr = IsBootstrapPointer(ptr);
    RESOLVE(realloc);
    if (InitState::allocHook_setup || g_resolving_symbols || m_sys_realloc == nullptr ||
        bootstrap_ptr || observe_only::Bypassed()) {
        if (m_sys_realloc == nullptr || bootstrap_ptr) {
            return BootstrapRealloc(ptr, size);
        }
        return m_sys_realloc(ptr, size);
    }
    return AllocHook::inst().realloc(ptr, size);
}

void* aligned_alloc(size_t alignment, size_t size) {
    RESOLVE(aligned_alloc);
    if (InitState::allocHook_setup || g_resolving_symbols ||
        m_sys_aligned_alloc == nullptr || observe_only::Bypassed()) {
        if (m_sys_aligned_alloc == nullptr) {
            if (!IsPowerOfTwo(alignment) || alignment < alignof(void*) ||
                size % alignment != 0) {
                errno = EINVAL;
                return nullptr;
            }
            return BootstrapAlignedMalloc(alignment, size);
        }
        return m_sys_aligned_alloc(alignment, size);
    }
    return AllocHook::inst().aligned_alloc(alignment, size);
}

void* memalign(size_t alignment, size_t bytes)  {
    RESOLVE(memalign);
    if (InitState::allocHook_setup || g_resolving_symbols || m_sys_memalign == nullptr ||
        observe_only::Bypassed()) {
        if (m_sys_memalign == nullptr) {
            return BootstrapAlignedMalloc(alignment, bytes);
        }
        return m_sys_memalign(alignment, bytes);
    }
    return AllocHook::inst().memalign(alignment, bytes);
}

// 进程初始化 和 debug init 的过程不应该调用 posix_memalign
int posix_memalign(void** ptr, size_t alignment, size_t size) {
    RESOLVE(memalign);
    RESOLVE(posix_memalign);
    if (InitState::allocHook_setup || g_resolving_symbols ||
        m_sys_posix_memalign == nullptr || observe_only::Bypassed()) {
        if (m_sys_posix_memalign == nullptr) {
            if (ptr == nullptr || !IsPowerOfTwo(alignment) ||
                alignment < sizeof(void*) || alignment % sizeof(void*) != 0) {
                return EINVAL;
            }
            *ptr = BootstrapAlignedMalloc(alignment, size);
            return (*ptr != nullptr) ? 0 : ENOMEM;
        }
        return m_sys_posix_memalign(ptr, alignment, size);
    }
    return AllocHook::inst().posix_memalign(ptr, alignment, size);
}

#if MALLOC_HOOK_EXPORT_MMAP_HOOK
void* mmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (in_preinit_phase || InitState::allocHook_setup ||
        observe_only::Bypassed()) {
        return CallRealMmap(addr, size, prot, flags, fd, offset);
    }
    if (fd < 0 && ShouldBypassLibcInternalMmap(__builtin_return_address(0))) {
        return CallRealMmap(addr, size, prot, flags, fd, offset);
    }
    void* result = AllocHook::inst().mmap(addr, size, prot, flags, fd, offset);
    return result;
}

int munmap(void* addr, size_t size) {
    if (in_preinit_phase || InitState::allocHook_setup ||
        observe_only::Bypassed()) {
        return CallRealMunmap(addr, size);
    }
    return AllocHook::inst().munmap(addr, size);
}

void* mremap(
        void* old_addr, size_t old_size, size_t new_size, int flags, ...) {
    void* new_addr = nullptr;
    if ((flags & MREMAP_FIXED) != 0) {
        va_list ap;
        va_start(ap, flags);
        new_addr = va_arg(ap, void*);
        va_end(ap);
    }
    if (in_preinit_phase || InitState::allocHook_setup ||
        observe_only::Bypassed()) {
        return CallRealMremap(old_addr, old_size, new_size, flags, new_addr);
    }
    return AllocHook::inst().mremap(old_addr, old_size, new_size, flags, new_addr);
}
#endif

#if MALLOC_HOOK_EXPORT_RESOURCE_HOOKS
int ioctl(int fd, HookIoctlRequest request, ...) {
    // The public Android/OHOS libc prototype uses a signed int for the
    // request, while _IOWR requests commonly have bit 31 set.  Normalize
    // through unsigned int before widening so the backend sees the original
    // 32-bit ioctl number instead of a sign-extended value.
    const unsigned long request_value =
            static_cast<unsigned long>(static_cast<unsigned int>(request));

    // ioctl is a variadic ABI.  A number of legacy and value-passing commands
    // have no _IOC_SIZE encoding but still require the third argument.  The
    // previous size-based heuristic silently forwarded nullptr for those
    // commands.  Preserve the established interposer contract and forward the
    // caller-supplied argument unchanged.
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);

    if (in_preinit_phase || InitState::allocHook_setup ||
        observe_only::Bypassed()) {
        return CallRealIoctl(fd, request_value, arg);
    }
    return AllocHook::inst().ioctl(fd, request_value, arg);
}

int close(int fd) {
    if (in_preinit_phase || InitState::allocHook_setup ||
        observe_only::Bypassed()) {
        return CallRealClose(fd);
    }
    return AllocHook::inst().close(fd);
}

int dup(int old_fd) {
    if (in_preinit_phase || InitState::allocHook_setup || observe_only::Bypassed()) {
        return CallRealDup(old_fd);
    }
    return AllocHook::inst().dup(old_fd);
}

int dup2(int old_fd, int new_fd) {
    if (in_preinit_phase || InitState::allocHook_setup || observe_only::Bypassed()) {
        return CallRealDup2(old_fd, new_fd);
    }
    return AllocHook::inst().dup2(old_fd, new_fd);
}

int dup3(int old_fd, int new_fd, int flags) {
    if (in_preinit_phase || InitState::allocHook_setup || observe_only::Bypassed()) {
        return CallRealDup3(old_fd, new_fd, flags);
    }
    return AllocHook::inst().dup3(old_fd, new_fd, flags);
}

int fcntl(int fd, int command, ...) {
    uintptr_t argument = 0;
    const FcntlArgumentKind kind = FcntlArgumentKindForCommand(command);
    if (kind != FcntlArgumentKind::None) {
        va_list ap;
        va_start(ap, command);
        switch (kind) {
            case FcntlArgumentKind::Integer:
                argument = static_cast<uintptr_t>(va_arg(ap, int));
                break;
            case FcntlArgumentKind::Pointer:
                argument = reinterpret_cast<uintptr_t>(va_arg(ap, void*));
                break;
            case FcntlArgumentKind::Word:
                argument = va_arg(ap, uintptr_t);
                break;
            case FcntlArgumentKind::None:
                break;
        }
        va_end(ap);
    }
    if (in_preinit_phase || InitState::allocHook_setup || observe_only::Bypassed()) {
        return CallRealFcntl(fd, command, argument);
    }
    return AllocHook::inst().fcntl(fd, command, argument);
}

#if defined(MALLOC_HOOK_TARGET_OS_LINUX)
int fcntl64(int fd, int command, ...) {
    uintptr_t argument = 0;
    const FcntlArgumentKind kind = FcntlArgumentKindForCommand(command);
    if (kind != FcntlArgumentKind::None) {
        va_list ap;
        va_start(ap, command);
        switch (kind) {
            case FcntlArgumentKind::Integer:
                argument = static_cast<uintptr_t>(va_arg(ap, int));
                break;
            case FcntlArgumentKind::Pointer:
                argument = reinterpret_cast<uintptr_t>(va_arg(ap, void*));
                break;
            case FcntlArgumentKind::Word:
                argument = va_arg(ap, uintptr_t);
                break;
            case FcntlArgumentKind::None:
                break;
        }
        va_end(ap);
    }
    if (in_preinit_phase || InitState::allocHook_setup || observe_only::Bypassed()) {
        return CallRealFcntl64(fd, command, argument);
    }
    return AllocHook::inst().fcntl_with(
            fd, command, argument, InvokeRealFcntl64, nullptr);
}
#endif

#if defined(MALLOC_HOOK_TARGET_OS_ANDROID)
int android_fdsan_close_with_tag(int fd, uint64_t tag) {
    FdsanCloseFn function = RealFdsanCloseFunction();
    if (function == nullptr) {
        // On API levels before fdsan, exporting this weak symbol changes an
        // application's null-check result. Preserve Bionic's documented fallback
        // by performing the same tracked ordinary close instead of returning ENOSYS.
        if (in_preinit_phase || InitState::allocHook_setup ||
            observe_only::Bypassed()) {
            return CallRealClose(fd);
        }
        return AllocHook::inst().close(fd);
    }
    if (in_preinit_phase || InitState::allocHook_setup || observe_only::Bypassed()) {
        return function(fd, tag);
    }
    FdsanCloseContext context{function, tag};
    return AllocHook::inst().close_with(fd, InvokeFdsanClose, &context);
}
#endif
#endif

#if MALLOC_HOOK_EXPORT_MMAP_HOOK && !defined(mmap64)
void* mmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (in_preinit_phase || InitState::allocHook_setup ||
        observe_only::Bypassed()) {
        return CallRealMmap64(addr, size, prot, flags, fd, offset);
    }
    if (fd < 0 && ShouldBypassLibcInternalMmap(__builtin_return_address(0))) {
        return CallRealMmap64(addr, size, prot, flags, fd, offset);
    }
    void* result = AllocHook::inst().mmap64(addr, size, prot, flags, fd, offset);
    return result;
}
#endif

void checkpoint(const char* file_name) {
    if (observe_only::Suppressed()) {
        return;
    }
    // Observe-only runs have no live allocation table to report. Constructing the
    // tracker now to answer this would report the allocations that happen from
    // here on as if they were the process's, so the observed figures are written
    // to the requested path instead.
    if (observe_only::Bypassed()) {
        observe_only::WriteReport(file_name);
        return;
    }
    AllocHook::inst().checkpoint(file_name);
}
}

#if MALLOC_HOOK_EXPORT_CPP_NEW_HOOK
namespace {

void* HookOperatorNew(size_t size) {
    if (size == 0) {
        size = 1;
    }
    void* ptr = malloc(size);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void* HookOperatorNewNoThrow(size_t size) noexcept {
    try {
        return HookOperatorNew(size);
    } catch (...) {
        return nullptr;
    }
}

void* HookOperatorNewAligned(size_t size, std::align_val_t alignment) {
    if (size == 0) {
        size = 1;
    }
    void* ptr = nullptr;
    const size_t align = static_cast<size_t>(alignment);
    int ret = posix_memalign(&ptr, align, size);
    if (ret != 0 || ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void* HookOperatorNewAlignedNoThrow(size_t size, std::align_val_t alignment) noexcept {
    try {
        return HookOperatorNewAligned(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

}  // namespace

void* operator new(size_t size) { return HookOperatorNew(size); }
void* operator new[](size_t size) { return HookOperatorNew(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return HookOperatorNewNoThrow(size); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return HookOperatorNewNoThrow(size); }

void operator delete(void* ptr) noexcept { free(ptr); }
void operator delete[](void* ptr) noexcept { free(ptr); }
void operator delete(void* ptr, size_t) noexcept { free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { free(ptr); }

void* operator new(size_t size, std::align_val_t alignment) { return HookOperatorNewAligned(size, alignment); }
void* operator new[](size_t size, std::align_val_t alignment) { return HookOperatorNewAligned(size, alignment); }
void* operator new(size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return HookOperatorNewAlignedNoThrow(size, alignment);
}
void* operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return HookOperatorNewAlignedNoThrow(size, alignment);
}

void operator delete(void* ptr, std::align_val_t) noexcept { free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { free(ptr); }
void operator delete(void* ptr, size_t, std::align_val_t) noexcept { free(ptr); }
void operator delete[](void* ptr, size_t, std::align_val_t) noexcept { free(ptr); }
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { free(ptr); }
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { free(ptr); }
#endif
