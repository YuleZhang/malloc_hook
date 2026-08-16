#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/param.h>  // powerof2 ---> ((((x) - 1) & (x)) == 0)
#include <unistd.h>
#include <sys/stat.h>
#include "linux_dma_heap_compat.h"

#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <android-base/stringprintf.h>

#include "Config.h"
#include "DebugData.h"
#include "PointerData.h"
#include "debug_disable.h"
#include "malloc_debug.h"

#include "midgard/mali_kbase_ioctl.h"
#include "msm_ksgl/msm_ksgl.h"
#include "mtk_camera/camera_mem.h"
#include "ion/ion_uapi.h"

#include "memory_hook.h"

class ScopedConcurrentLock {
public:
    ScopedConcurrentLock() { pthread_rwlock_rdlock(&lock_); }
    ~ScopedConcurrentLock() { pthread_rwlock_unlock(&lock_); }

    static void Init() {
        pthread_rwlockattr_t attr;
        pthread_rwlockattr_init(&attr);
        // Set the attribute so that when a write lock is pending, read locks are no
        // longer granted.
#if __ANDROID_API__ >= 23
        pthread_rwlockattr_setkind_np(
                &attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
        pthread_rwlock_init(&lock_, &attr);
        pthread_rwlockattr_destroy(&attr);
    }

    static void BlockAllOperations() { pthread_rwlock_wrlock(&lock_); }

private:
    static pthread_rwlock_t lock_;
};
pthread_rwlock_t ScopedConcurrentLock::lock_;

DebugData* g_debug;

static int g_signal_pipe[2] = {-1, -1};
static pthread_t g_signal_thread;
static bool g_signal_thread_started = false;
static bool g_signal_debug_enabled = false;
static bool SignalDebugEnabled() {
    return g_signal_debug_enabled;
}

static void SignalDebugLog(const char* message) {
    if (SignalDebugEnabled()) {
        write(STDERR_FILENO, message, strlen(message));
    }
}

static void SignalDebugLogInt(const char* prefix, int value) {
    if (!SignalDebugEnabled()) {
        return;
    }
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "%s%d\n", prefix, value);
    write(STDERR_FILENO, buffer, strlen(buffer));
}

static void DumpHeapToFileUnlocked(const char* file_name, bool dump_peak) {
    ScopedDisableDebugCalls disable;

    int fd = open(file_name, O_RDWR | O_CREAT | O_NOFOLLOW | O_TRUNC | O_CLOEXEC, 0644);
    if (fd == -1) {
        return;
    }

    g_debug->pointer->DumpLiveToFile(fd, dump_peak);
    close(fd);
}

static void singal_dump_heap(int) {
    if (g_signal_pipe[1] != -1) {
        const char command = 'd';
        ssize_t bytes = write(g_signal_pipe[1], &command, sizeof(command));
        if (g_signal_debug_enabled) {
            if (bytes == sizeof(command)) {
                static const char message[] = "alloc_hook: signal handler queued dump\n";
                write(STDERR_FILENO, message, sizeof(message) - 1);
            } else {
                static const char message[] = "alloc_hook: signal handler write failed\n";
                write(STDERR_FILENO, message, sizeof(message) - 1);
            }
        }
    }
}

static void* signal_dump_thread(void*) {
    sigset_t blocked_signals;
    sigemptyset(&blocked_signals);
    sigaddset(&blocked_signals, g_debug->config().backtrace_dump_signal());
    pthread_sigmask(SIG_BLOCK, &blocked_signals, nullptr);

    while (true) {
        char command;
        ssize_t bytes = read(g_signal_pipe[0], &command, sizeof(command));
        if (bytes <= 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (command != 'd' || g_debug == nullptr ||
            !(g_debug->config().options() & BACKTRACE)) {
            continue;
        }

        char file_name[256];
        snprintf(
                file_name, sizeof(file_name), "%s.time.%ld.txt",
                g_debug->config().backtrace_dump_prefix(), time(NULL));
        SignalDebugLog("alloc_hook: signal thread dumping heap\n");
        debug_dump_heap(file_name);
        SignalDebugLog("alloc_hook: signal thread finished heap dump\n");
    }
    return nullptr;
}

static bool StartSignalDumpThread() {
    g_signal_debug_enabled = getenv("ALLOC_HOOK_DEBUG_SIGNAL") != nullptr;
    if (g_signal_thread_started) {
        return true;
    }
    if (pipe(g_signal_pipe) != 0) {
        g_signal_pipe[0] = -1;
        g_signal_pipe[1] = -1;
        return false;
    }
    fcntl(g_signal_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_signal_pipe[1], F_SETFD, FD_CLOEXEC);
    int flags = fcntl(g_signal_pipe[1], F_GETFL, 0);
    if (flags != -1) {
        fcntl(g_signal_pipe[1], F_SETFL, flags | O_NONBLOCK);
    }
    if (pthread_create(&g_signal_thread, nullptr, signal_dump_thread, nullptr) != 0) {
        close(g_signal_pipe[0]);
        close(g_signal_pipe[1]);
        g_signal_pipe[0] = -1;
        g_signal_pipe[1] = -1;
        return false;
    }
    pthread_detach(g_signal_thread);
    g_signal_thread_started = true;
    SignalDebugLog("alloc_hook: signal dump thread started\n");
    return true;
}

bool debug_initialize(void* init_space[]) {
    if (!DebugDisableInitialize()) {
        return false;
    }

    DebugData* debug = new (init_space[0]) DebugData();
    if (!debug->Initialize(init_space[1])) {
        DebugDisableFinalize();
        return false;
    }
    g_debug = debug;

    ScopedConcurrentLock::Init();

    if (g_debug->config().options() & DUMP_ON_SINGAL) {
        if (!StartSignalDumpThread()) {
            return false;
        }
        struct sigaction enable_act = {};
        enable_act.sa_handler = singal_dump_heap;
        sigemptyset(&enable_act.sa_mask);
        enable_act.sa_flags = SA_RESTART | SA_ONSTACK;
        SignalDebugLogInt(
                "alloc_hook: installing dump signal ",
                g_debug->config().backtrace_dump_signal());
        if (sigaction(
                    g_debug->config().backtrace_dump_signal(), &enable_act, nullptr) !=
            0) {
            SignalDebugLog("alloc_hook: failed to install dump signal\n");
            return false;
        }
    }

    return true;
}

void debug_finalize() {
    if (g_debug == nullptr) {
        return;
    }

    // Make sure that there are no other threads doing debug allocations
    // before we kill everything.
    ScopedConcurrentLock::BlockAllOperations();

    // Turn off capturing allocations calls.
    DebugDisableSet(true);

    if ((g_debug->config().options() & BACKTRACE) &&
        g_debug->config().backtrace_dump_on_exit()) {
        DumpHeapToFileUnlocked(android::base::StringPrintf(
                                       "%s.exit.%ld.txt",
                                       g_debug->config().backtrace_dump_prefix(), time(NULL))
                                       .c_str(), true);
    }

    if (g_debug->TrackPointers()) {
        g_debug->pointer->DumpPeakInfo();
    }

    // 对于调试工具或在调试模式下运行的代码, 资源管理可能不是首要关注点.
    // 为了避免在清理过程中出现多线程访问冲突, 决定故意不释放这些资源. 包括
    // g_debug、pthread 键等.
}

void debug_dump_heap(const char* file_name) {
    ScopedConcurrentLock lock;
    DumpHeapToFileUnlocked(file_name, false);
}

static void* InternalMalloc(size_t size) {
    void* result = m_sys_malloc(size);
    if (g_debug->TrackPointers()) {
        g_debug->pointer->Add(result, size);
    }

    return result;
}

static void InternalFree(void* pointer) {
    if (g_debug->TrackPointers()) {
        g_debug->pointer->Remove(pointer);
    }
    m_sys_free(pointer);
}

void* debug_malloc(size_t size) {
    if (DebugCallsDisabled()) {
        return m_sys_malloc(size);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (size > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    return InternalMalloc(size);
}

void debug_free(void* pointer) {
    if (DebugCallsDisabled() || pointer == nullptr) {
        return m_sys_free(pointer);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    InternalFree(pointer);
}

void* debug_realloc(void* pointer, size_t bytes) {
    if (DebugCallsDisabled()) {
        return m_sys_realloc(pointer, bytes);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (pointer == nullptr) {
        return InternalMalloc(bytes);
    }

    if (bytes == 0) {
        InternalFree(pointer);
        return nullptr;
    }

    if (bytes > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    if (g_debug->TrackPointers()) {
        g_debug->pointer->Remove(pointer);
    }

    void* new_pointer = m_sys_realloc(pointer, bytes);

    if (g_debug->TrackPointers()) {
        g_debug->pointer->Add(new_pointer, bytes);
    }

    return new_pointer;
}

void* debug_calloc(size_t nmemb, size_t bytes) {
    if (DebugCallsDisabled()) {
        return m_sys_calloc(nmemb, bytes);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    size_t size;
    if (__builtin_mul_overflow(nmemb, bytes, &size)) {
        // Overflow
        errno = ENOMEM;
        return nullptr;
    }

    void* pointer = m_sys_calloc(1, size);
    if (pointer != nullptr && g_debug->TrackPointers()) {
        g_debug->pointer->Add(pointer, size);
    }

    return pointer;
}

void* debug_memalign(size_t alignment, size_t bytes) {
    if (DebugCallsDisabled()) {
        return m_sys_memalign(alignment, bytes);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (bytes > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    void* pointer = m_sys_memalign(alignment, bytes);

    if (pointer != nullptr && g_debug->TrackPointers()) {
        g_debug->pointer->Add(pointer, bytes);
    }

    return pointer;
}

void* debug_aligned_alloc(size_t alignment, size_t bytes) {
    if (DebugCallsDisabled()) {
        return m_sys_aligned_alloc(alignment, bytes);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (bytes > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    void* pointer = m_sys_aligned_alloc(alignment, bytes);
    if (pointer != nullptr && g_debug->TrackPointers()) {
        g_debug->pointer->Add(pointer, bytes);
    }

    return pointer;
}

int debug_posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (DebugCallsDisabled()) {
        return m_sys_posix_memalign(memptr, alignment, size);
    }

    if (alignment < sizeof(void*) || !powerof2(alignment)) {
        return EINVAL;
    }
    int saved_errno = errno;
    *memptr = debug_memalign(alignment, size);
    errno = saved_errno;
    return (*memptr != nullptr) ? 0 : ENOMEM;
}

namespace DMA_BUF {

static thread_local bool gpu_ioctl_alloc = false;  // TLS to store a unique flag per thread
struct PendingIonAllocation {
    size_t size = 0;
};

struct DmaFdInfo {
    size_t size = 0;
    bool tracked = false;
};

static std::mutex state_mutex;
static std::unordered_map<uint64_t, PendingIonAllocation> pending_ion_allocations;
static std::unordered_map<int, DmaFdInfo> dma_fds;

static uint64_t IonHandleKey(int ion_fd, ion_user_handle_t handle) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(ion_fd)) << 32) |
           static_cast<uint32_t>(handle);
}

static bool IsIonRequest(unsigned int request, unsigned int nr, unsigned int size) {
    return _IOC_TYPE(request) == ALLOC_HOOK_ION_IOC_MAGIC &&
           _IOC_NR(request) == nr && _IOC_SIZE(request) == size;
}

static bool IsIonLegacyAlloc(unsigned int request) {
    return IsIonRequest(request, 0, sizeof(struct ion_allocation_data));
}

static bool IsIonNewAlloc(unsigned int request) {
    return IsIonRequest(request, 0, sizeof(struct ion_new_allocation_data));
}

static bool IsIonFdRequest(unsigned int request, unsigned int nr) {
    return IsIonRequest(request, nr, sizeof(struct ion_fd_data));
}

static bool IsIonFree(unsigned int request) {
    return IsIonRequest(request, 1, sizeof(struct ion_handle_data));
}

static void ForgetDmaFd(int fd) {
    std::lock_guard<std::mutex> guard(state_mutex);
    dma_fds.erase(fd);
}

static bool RegisterDmaFd(int fd, size_t size, bool* should_track) {
    if (fd < 0 || size == 0) {
        return false;
    }
    std::lock_guard<std::mutex> guard(state_mutex);
    auto [entry, inserted] = dma_fds.emplace(fd, DmaFdInfo{size, false});
    *should_track = inserted || !entry->second.tracked;
    entry->second.size = size;
    return true;
}

static bool LookupDmaFd(int fd, size_t* size, bool* tracked = nullptr) {
    std::lock_guard<std::mutex> guard(state_mutex);
    auto entry = dma_fds.find(fd);
    if (entry == dma_fds.end()) {
        return false;
    }
    *size = entry->second.size;
    if (tracked != nullptr) {
        *tracked = entry->second.tracked;
    }
    return true;
}

static void MarkDmaFdTracked(int fd) {
    std::lock_guard<std::mutex> guard(state_mutex);
    auto entry = dma_fds.find(fd);
    if (entry != dma_fds.end()) {
        entry->second.tracked = true;
    }
}

static bool TakeDmaFd(int fd, size_t* size, bool* tracked) {
    std::lock_guard<std::mutex> guard(state_mutex);
    auto entry = dma_fds.find(fd);
    if (entry == dma_fds.end()) {
        return false;
    }
    *size = entry->second.size;
    *tracked = entry->second.tracked;
    dma_fds.erase(entry);
    return true;
}

static void RecordIonAllocation(int ion_fd, ion_user_handle_t handle, size_t size) {
    if (size == 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(state_mutex);
    pending_ion_allocations[IonHandleKey(ion_fd, handle)] = PendingIonAllocation{size};
}

static size_t LookupIonAllocation(int ion_fd, ion_user_handle_t handle) {
    std::lock_guard<std::mutex> guard(state_mutex);
    auto key = IonHandleKey(ion_fd, handle);
    auto entry = pending_ion_allocations.find(key);
    if (entry == pending_ion_allocations.end()) {
        return 0;
    }
    return entry->second.size;
}

static void DropIonAllocation(int ion_fd, ion_user_handle_t handle) {
    std::lock_guard<std::mutex> guard(state_mutex);
    pending_ion_allocations.erase(IonHandleKey(ion_fd, handle));
}

static void DropIonAllocationsForFd(int ion_fd) {
    std::lock_guard<std::mutex> guard(state_mutex);
    const uint32_t fd_key = static_cast<uint32_t>(ion_fd);
    for (auto entry = pending_ion_allocations.begin();
         entry != pending_ion_allocations.end();) {
        if (static_cast<uint32_t>(entry->first >> 32) == fd_key) {
            entry = pending_ion_allocations.erase(entry);
        } else {
            ++entry;
        }
    }
}

static bool is_dma_buf(int fd, size_t* size) {
    if (LookupDmaFd(fd, size)) {
        return true;
    }
    std::string fdinfo = android::base::StringPrintf("/proc/self/fdinfo/%d", fd);
    auto fp = std::unique_ptr<FILE, decltype(&fclose)>{fopen(fdinfo.c_str(), "re"), fclose};
    if (fp == nullptr) {
        return false;
    }

    bool is_dmabuf_file = false;
    uint64_t inode = -1;
    char* line = nullptr;
    size_t len = 0;
    while (getline(&line, &len, fp.get()) > 0) {
        switch (line[0]) {
            case 'i':
                if (strncmp(line, "ino:", 4) == 0) {
                    char* c = line + 4;
                    inode = strtoull(c, nullptr, 10);
                }
                break;
            case 's':
                if (strncmp(line, "size:", 5) == 0) {
                    char* c = line + 5;
                    *size = strtoull(c, nullptr, 10);
                }
                break;
            case 'e':
                if (strncmp(line, "exp_name:", 9) == 0) {
                    is_dmabuf_file = true;
                }
                break;
            default:
                break;
        }
    }
    m_sys_free(line);

    if (!is_dmabuf_file) {
        return false;
    }

    if (inode == static_cast<uint64_t>(-1)) {
        // Fallback to stat() on the fd path to get inode number
        std::string fd_path = android::base::StringPrintf("/proc/self/fd/%d", fd);

        struct stat sb;
        if (stat(fd_path.c_str(), &sb) < 0) {
            return false;
        }
        inode = sb.st_ino;
    }

    return *size > 0;
}

static void IonPathLog(const char* tag, unsigned int request, size_t sz) {
    static bool enabled = getenv("ALLOC_HOOK_DEBUG_ION") != nullptr;
    if (!enabled) {
        return;
    }
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "alloc_hook_ion: %s nr=%u size=%zu\n",
                     tag, _IOC_NR(request), sz);
    if (n > 0) {
        write(STDERR_FILENO, buf, static_cast<size_t>(n));
    }
}

static bool handle_dma_node(unsigned int request, void* arg, int* fd, size_t* size) {
    // delay parsing the backtrace until mmap64.
    auto set_gpu_ioctl_alloc_and_return_false = []() -> bool {
        gpu_ioctl_alloc = true;
        return false;
    };

    switch (request) {
        case KBASE_IOCTL_MEM_ALLOC:
        case KBASE_IOCTL_MEM_ALLOC_EX:
        case IOCTL_KGSL_GPUOBJ_ALLOC:
            return set_gpu_ioctl_alloc_and_return_false();
        // parse the backtrace immediately
        case DMA_HEAP_IOCTL_ALLOC: {
                struct dma_heap_allocation_data* heap = (struct dma_heap_allocation_data*)arg;
                *fd = heap->fd;
                IonPathLog("DMA_HEAP", request, static_cast<size_t>(heap->len));
#if defined(__MUSL__)
                // OHOS fdinfo omits Android's size/exp_name fields and may report
                // ino: 0. DMA_HEAP_IOCTL_ALLOC itself is authoritative here.
                *size = static_cast<size_t>(heap->len);
                return *fd >= 0 && *size > 0;
#endif
            }
            return is_dma_buf(*fd, size);
        case CAM_MEM_ION_MAP_PA: {
                struct CAM_MEM_DEV_ION_NODE_STRUCT* heap = (struct CAM_MEM_DEV_ION_NODE_STRUCT*)arg;
                *fd = heap->memID;
            }
            return is_dma_buf(*fd, size);
        default:
            return false;
    }
}

static bool HandleIonIoctl(
        int ion_fd, unsigned int request, void* arg, int* dma_fd, size_t* size) {
    if (arg == nullptr || _IOC_TYPE(request) != ALLOC_HOOK_ION_IOC_MAGIC) {
        return false;
    }
    if (IsIonLegacyAlloc(request)) {
        auto* allocation = static_cast<struct ion_allocation_data*>(arg);
        IonPathLog("ION_LEGACY_ALLOC", request, allocation->len);
        RecordIonAllocation(ion_fd, allocation->handle, allocation->len);
        return false;
    }
    if (IsIonNewAlloc(request)) {
        auto* allocation = static_cast<struct ion_new_allocation_data*>(arg);
        IonPathLog("ION_MODERN_ALLOC", request, static_cast<size_t>(allocation->len));
        *dma_fd = static_cast<int>(allocation->fd);
        *size = static_cast<size_t>(allocation->len);
        return *dma_fd >= 0 && *size > 0;
    }
    if (IsIonFdRequest(request, 2) || IsIonFdRequest(request, 4)) {
        auto* fd_data = static_cast<struct ion_fd_data*>(arg);
        *dma_fd = fd_data->fd;
        *size = LookupIonAllocation(ion_fd, fd_data->handle);
        IonPathLog("ION_MAP_SHARE", request, *size);
        return *dma_fd >= 0 && *size > 0;
    }
    if (IsIonFdRequest(request, 5)) {
        return false;
    }
    if (IsIonFree(request)) {
        auto* handle_data = static_cast<struct ion_handle_data*>(arg);
        DropIonAllocation(ion_fd, handle_data->handle);
    }
    return false;
}

}  // namespace DMA_BUF

#if defined(__MUSL__)
static bool ShouldTrackMmapAllocation(void* result, int prot, int flags, int fd) {
    if (result == MAP_FAILED) {
        return false;
    }
    (void)prot;
    return fd < 0 && (flags & MAP_ANONYMOUS);
}
#endif

static void* CallMmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (m_sys_mmap != nullptr) {
        return m_sys_mmap(addr, size, prot, flags, fd, offset);
    }
    return reinterpret_cast<void*>(syscall(SYS_mmap, addr, size, prot, flags, fd, offset));
}

static int CallMunmap(void* addr, size_t size) {
    if (m_sys_munmap != nullptr) {
        return m_sys_munmap(addr, size);
    }
    return static_cast<int>(syscall(SYS_munmap, addr, size));
}

#if !defined(mmap64)
static void* CallMmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (m_sys_mmap64 != nullptr) {
        return m_sys_mmap64(addr, size, prot, flags, fd, offset);
    }
    return CallMmap(addr, size, prot, flags, fd, offset);
}
#endif

int debug_ioctl(int fd, unsigned int request, void* arg) {
    if (DebugCallsDisabled()) {
        return (int)syscall(SYS_ioctl, fd, request, arg);
    }

    ScopedDisableDebugCalls disable;

    // Avoid holding the global hook lock while the ioctl blocks in vendor code.
    int ret = (int)syscall(SYS_ioctl, fd, request, arg);

    int node_fd = -1;
    size_t node_sz = 0;
    if (ret == 0 && g_debug->TrackPointers()) {
        bool recognized =
                DMA_BUF::HandleIonIoctl(fd, request, arg, &node_fd, &node_sz) ||
                DMA_BUF::handle_dma_node(request, arg, &node_fd, &node_sz);
        if (recognized) {
            bool should_track = false;
            if (DMA_BUF::RegisterDmaFd(node_fd, node_sz, &should_track) &&
                should_track) {
                ScopedConcurrentLock lock;
                void* ptr = reinterpret_cast<void*>(node_fd);
                g_debug->pointer->Add(ptr, node_sz, DMA);
                DMA_BUF::MarkDmaFdTracked(node_fd);
            }
        }
    }

    return ret;
}

int debug_close(int fd) {
    if (DebugCallsDisabled()) {
        return (int)syscall(SYS_close, fd);
    }

    ScopedDisableDebugCalls disable;

    int ret = (int)syscall(SYS_close, fd);
    if (ret == 0 && g_debug->TrackPointers()) {
        size_t size = 0;
        bool tracked = false;
        if (DMA_BUF::TakeDmaFd(fd, &size, &tracked) && tracked) {
            ScopedConcurrentLock lock;
            void* ptr = reinterpret_cast<void*>(fd);
            g_debug->pointer->Remove(ptr);
        }
        DMA_BUF::DropIonAllocationsForFd(fd);
    }
    return ret;
}

void* debug_mmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (DebugCallsDisabled()) {
#if !defined(mmap64)
        return CallMmap64(addr, size, prot, flags, fd, offset);
#else
        return CallMmap(addr, size, prot, flags, fd, offset);
#endif
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (size > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

#if !defined(mmap64)
    void* result = CallMmap64(addr, size, prot, flags, fd, offset);
#else
    void* result = CallMmap(addr, size, prot, flags, fd, offset);
#endif

#if defined(__MUSL__)
    if (g_debug->TrackPointers() && ShouldTrackMmapAllocation(result, prot, flags, fd)) {
        g_debug->pointer->Add(result, size, MMAP);
    } else if (g_debug->TrackPointers() && result != MAP_FAILED && fd >= 0 &&
               (flags & MAP_SHARED)) {
        size_t dma_size = 0;
        bool tracked = false;
        if (DMA_BUF::LookupDmaFd(fd, &dma_size, &tracked) && !tracked) {
            void* ptr = reinterpret_cast<void*>(fd);
            g_debug->pointer->Add(ptr, dma_size, DMA);
            DMA_BUF::MarkDmaFdTracked(fd);
        }
    }
#else
    if (g_debug->TrackPointers() && DMA_BUF::gpu_ioctl_alloc) {
        DMA_BUF::gpu_ioctl_alloc = false;  // Reset the flag immediately after processing
        g_debug->pointer->Add(result, size, DMA);
    }
#endif

    return result;
}

void* debug_mmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (DebugCallsDisabled()) {
        return CallMmap(addr, size, prot, flags, fd, offset);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (size > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    void* result = CallMmap(addr, size, prot, flags, fd, offset);
#if defined(__MUSL__)
    if (g_debug->TrackPointers() && ShouldTrackMmapAllocation(result, prot, flags, fd)) {
        g_debug->pointer->Add(result, size, MMAP);
    } else if (g_debug->TrackPointers() && result != MAP_FAILED && fd >= 0 &&
               (flags & MAP_SHARED)) {
        size_t dma_size = 0;
        bool tracked = false;
        if (DMA_BUF::LookupDmaFd(fd, &dma_size, &tracked) && !tracked) {
            void* ptr = reinterpret_cast<void*>(fd);
            g_debug->pointer->Add(ptr, dma_size, DMA);
            DMA_BUF::MarkDmaFdTracked(fd);
        }
    }
#else
    if (g_debug->TrackPointers()) {
        size_t node_sz = 0;
        if (fd < 0) {
            g_debug->pointer->Add(result, size, MMAP);
        } else if (DMA_BUF::is_dma_buf(fd, &node_sz)) {
            void* ptr = reinterpret_cast<void*>(fd);
            g_debug->pointer->Add(ptr, node_sz, DMA);
        }
    }
#endif

    return result;
}

int debug_munmap(void* addr, size_t size) {
    if (DebugCallsDisabled()) {
        return CallMunmap(addr, size);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (g_debug->TrackPointers()) {
        g_debug->pointer->Remove(addr);
    }

    return CallMunmap(addr, size);
}
