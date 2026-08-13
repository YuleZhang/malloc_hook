#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/param.h>  // powerof2 ---> ((((x) - 1) & (x)) == 0)
#include <unistd.h>
#include <sys/stat.h>
#include <linux/dma-heap.h>
#include <dlfcn.h>

#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <android-base/stringprintf.h>

#include "Config.h"
#include "DebugData.h"
#include "PointerData.h"
#include "debug_disable.h"
#include "malloc_debug.h"

#include "midgard/mali_kbase_ioctl.h"
#include "msm_ksgl/msm_ksgl.h"
#include "mtk_camera/camera_mem.h"

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
static std::mutex g_sample_peak_mutex;
static uint64_t g_sample_peak_last_total_bytes = 0;

namespace {

bool SamplePeakEnabled() {
    const char* value = getenv("MALLOC_HOOK_SAMPLE_PEAK");
    return value != nullptr && value[0] != '\0' && strcmp(value, "0") != 0;
}

uint64_t ParseEnvMiB(const char* name, uint64_t default_mib) {
    const char* value = getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return default_mib * 1024ULL * 1024ULL;
    }
    char* end = nullptr;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (end == value || *end != '\0') {
        return default_mib * 1024ULL * 1024ULL;
    }
    return parsed * 1024ULL * 1024ULL;
}

uint64_t EpochMilliseconds() {
    struct timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000ULL +
           static_cast<uint64_t>(now.tv_nsec) / 1000000ULL;
}

void CopyProcFile(const char* source, const std::string& destination) {
    int input = open(source, O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        return;
    }
    int output = open(
            destination.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (output < 0) {
        close(input);
        return;
    }
    char buffer[16384];
    while (true) {
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        const char* cursor = buffer;
        while (count > 0) {
            ssize_t written = write(output, cursor, static_cast<size_t>(count));
            if (written <= 0) {
                count = 0;
                break;
            }
            cursor += written;
            count -= written;
        }
    }
    close(output);
    close(input);
}

void QueryOpenClSnapshot(size_t* live_bytes, const char** last_api) {
    *live_bytes = 0;
    *last_api = "<unavailable>";
    using SnapshotFn = void (*)(size_t*, const char**);
    auto snapshot = reinterpret_cast<SnapshotFn>(
            dlsym(RTLD_DEFAULT, "malloc_hook_opencl_get_snapshot"));
    if (snapshot != nullptr) {
        snapshot(live_bytes, last_api);
    }
}

}  // namespace

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

void debug_record_sample_peak(
        uint64_t epoch_ms, uint64_t dma_bytes, uint64_t rss_bytes) {
    if (!SamplePeakEnabled() || g_debug == nullptr) {
        return;
    }
    const uint64_t total_bytes = dma_bytes + rss_bytes;
    const uint64_t threshold =
            ParseEnvMiB("MALLOC_HOOK_SAMPLE_PEAK_THRESHOLD_MB", 0);
    const uint64_t step = ParseEnvMiB("MALLOC_HOOK_SAMPLE_PEAK_STEP_MB", 0);
    if (total_bytes < threshold) {
        return;
    }

    std::lock_guard<std::mutex> capture_guard(g_sample_peak_mutex);
    if (total_bytes <= g_sample_peak_last_total_bytes ||
        (g_sample_peak_last_total_bytes != 0 &&
         total_bytes - g_sample_peak_last_total_bytes < step)) {
        return;
    }
    g_sample_peak_last_total_bytes = total_bytes;

    ScopedConcurrentLock operation_lock;
    ScopedDisableDebugCalls disable;
    const char* root = getenv("MALLOC_HOOK_SAMPLE_PEAK_DIR");
    if (root == nullptr || root[0] == '\0') {
        root = "/data/local/tmp/peak_snapshots";
    }
    mkdir(root, 0755);

    char snapshot_dir[512];
    snprintf(
            snapshot_dir, sizeof(snapshot_dir), "%s/%llu_%llu", root,
            static_cast<unsigned long long>(epoch_ms),
            static_cast<unsigned long long>(total_bytes));
    if (mkdir(snapshot_dir, 0755) != 0 && errno != EEXIST) {
        return;
    }

    const uint64_t capture_start_ms = EpochMilliseconds();
    size_t hook_host_bytes = 0;
    size_t hook_dma_bytes = 0;
    g_debug->pointer->GetCurrentUsage(&hook_host_bytes, &hook_dma_bytes);
    size_t opencl_live_bytes = 0;
    const char* opencl_last_api = nullptr;
    QueryOpenClSnapshot(&opencl_live_bytes, &opencl_last_api);

    // Capture process residency first.  Formatting the hook ledger can be
    // comparatively expensive, so doing it before smaps would move the PSS
    // observation farther away from the HAIO sample that triggered us.
    CopyProcFile("/proc/self/smaps", std::string(snapshot_dir) + "/smaps");
    CopyProcFile(
            "/proc/self/smaps_rollup",
            std::string(snapshot_dir) + "/smaps_rollup");
    CopyProcFile("/proc/self/status", std::string(snapshot_dir) + "/status");
    CopyProcFile(
            "/proc/ion_process_info",
            std::string(snapshot_dir) + "/ion_process_info");
    std::string ledger_path = std::string(snapshot_dir) + "/hook_live_ledger.txt";
    int ledger_fd = open(
            ledger_path.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (ledger_fd >= 0) {
        g_debug->pointer->DumpLiveToFile(ledger_fd, false);
        close(ledger_fd);
    }

    const uint64_t capture_end_ms = EpochMilliseconds();
    std::string metadata_path = std::string(snapshot_dir) + "/metadata.txt";
    int metadata_fd = open(
            metadata_path.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (metadata_fd >= 0) {
        dprintf(
                metadata_fd,
                "pid=%d\ncsv_epoch_ms=%llu\ndma_bytes=%llu\nrss_bytes=%llu\n"
                "dma_plus_rss_bytes=%llu\ncapture_start_ms=%llu\n"
                "capture_end_ms=%llu\nhook_host_live_bytes=%zu\n"
                "hook_dma_live_bytes=%zu\nopencl_live_requested_bytes=%zu\n"
                "opencl_last_api=%s\n",
                getpid(), static_cast<unsigned long long>(epoch_ms),
                static_cast<unsigned long long>(dma_bytes),
                static_cast<unsigned long long>(rss_bytes),
                static_cast<unsigned long long>(total_bytes),
                static_cast<unsigned long long>(capture_start_ms),
                static_cast<unsigned long long>(capture_end_ms), hook_host_bytes,
                hook_dma_bytes, opencl_live_bytes,
                opencl_last_api != nullptr ? opencl_last_api : "<unknown>");
        close(metadata_fd);
    }
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
static std::mutex inode_set_mutex;

static bool is_dma_buf(int fd, size_t* size) {
    static std::unordered_set<uint64_t> inode_set;
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

    std::lock_guard<std::mutex> guard(inode_set_mutex);
    return inode_set.insert(inode).second;
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
    if (g_debug->TrackPointers() && DMA_BUF::handle_dma_node(request, arg, &node_fd, &node_sz)) {
        ScopedConcurrentLock lock;
        void* ptr = reinterpret_cast<void*>(node_fd);
        g_debug->pointer->Add(ptr, node_sz, DMA);
    }

    return ret;
}

int debug_close(int fd) {
    if (DebugCallsDisabled()) {
        return (int)syscall(SYS_close, fd);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (g_debug->TrackPointers()) {
        void* ptr = reinterpret_cast<void*>(fd);
        g_debug->pointer->Remove(ptr);
    }

    return (int)syscall(SYS_close, fd);
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
