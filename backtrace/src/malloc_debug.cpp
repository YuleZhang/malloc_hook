#include <signal.h>
#include <sys/mman.h>
#include <sys/param.h>  // powerof2 ---> ((((x) - 1) & (x)) == 0)
#include <sys/syscall.h>
#include <linux/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <limits.h>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>
#include <android-base/stringprintf.h>

#include "Config.h"
#include "DebugData.h"
#include "PointerData.h"
#include "debug_disable.h"
#include "linux_dma_heap_compat.h"
#include "malloc_debug.h"

#include "midgard/mali_kbase_ioctl.h"
#include "msm_ksgl/msm_ksgl.h"
#include "mtk_camera/camera_mem.h"

#include "memory_hook.h"

class ScopedConcurrentLock {
public:
    ScopedConcurrentLock() {
        if (!initialized_) {
            return;
        }
        locked_ = pthread_rwlock_rdlock(&lock_) == 0;
    }
    ~ScopedConcurrentLock() {
        if (locked_) {
            pthread_rwlock_unlock(&lock_);
        }
    }

    static bool Init() {
        if (initialized_) {
            return true;
        }

        pthread_rwlockattr_t attr;
        int error = pthread_rwlockattr_init(&attr);
        if (error != 0) {
            return false;
        }
        // Set the attribute so that when a write lock is pending, read locks are no
        // longer granted.
#if __ANDROID_API__ >= 23
        error = pthread_rwlockattr_setkind_np(
                &attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
        if (error != 0) {
            pthread_rwlockattr_destroy(&attr);
            return false;
        }
#endif
        error = pthread_rwlock_init(&lock_, &attr);
        pthread_rwlockattr_destroy(&attr);
        if (error != 0) {
            return false;
        }
        initialized_ = true;
        return true;
    }

    static bool BlockAllOperations() {
        if (!initialized_) {
            return false;
        }
        return pthread_rwlock_wrlock(&lock_) == 0;
    }

private:
    bool locked_ = false;
    static pthread_rwlock_t lock_;
    static bool initialized_;
};
pthread_rwlock_t ScopedConcurrentLock::lock_;
bool ScopedConcurrentLock::initialized_ = false;

DebugData* g_debug;
static std::atomic<uint32_t> g_dump_sequence{0};

static void LogHookError(const char* format, ...) {
    char buffer[512];
    va_list ap;
    va_start(ap, format);
    int written = vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    if (written <= 0) {
        return;
    }

    size_t len = static_cast<size_t>(written);
    if (len >= sizeof(buffer)) {
        len = sizeof(buffer) - 1;
    }
    ssize_t ignored = write(STDERR_FILENO, buffer, len);
    (void)ignored;
}

static bool EnsureParentDirectoryExists(const char* file_name) {
    if (file_name == nullptr || file_name[0] == '\0') {
        errno = EINVAL;
        return false;
    }

    size_t len = strnlen(file_name, PATH_MAX);
    if (len == 0 || len >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return false;
    }

    char dir_path[PATH_MAX];
    memcpy(dir_path, file_name, len + 1);

    char* slash = strrchr(dir_path, '/');
    if (slash == nullptr) {
        return true;
    }
    *slash = '\0';
    if (dir_path[0] == '\0') {
        return true;
    }

    for (char* current = dir_path + 1; *current != '\0'; ++current) {
        if (*current != '/') {
            continue;
        }

        *current = '\0';
        if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
            return false;
        }
        *current = '/';
    }

    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
        return false;
    }

    return true;
}

static bool DumpHeapToFile(const char* file_name) {
    ScopedDisableDebugCalls disable;

    if (g_debug == nullptr || g_debug->pointer == nullptr) {
        LogHookError("alloc_hook: dump requested before initialization\n");
        return false;
    }

    if (!EnsureParentDirectoryExists(file_name)) {
        LogHookError(
                "alloc_hook: failed to create parent directory for %s: %s\n",
                file_name != nullptr ? file_name : "<null>", strerror(errno));
        return false;
    }

    int fd = open(file_name, O_RDWR | O_CREAT | O_NOFOLLOW | O_TRUNC | O_CLOEXEC, 0644);
    if (fd == -1) {
        LogHookError(
                "alloc_hook: failed to open dump file %s: %s\n",
                file_name != nullptr ? file_name : "<null>", strerror(errno));
        return false;
    }

    LogHookError("alloc_hook: writing dump to %s\n",
                 file_name != nullptr ? file_name : "<null>");
    g_debug->pointer->DumpLiveToFile(fd);
    close(fd);
    LogHookError("alloc_hook: finished dump to %s\n",
                 file_name != nullptr ? file_name : "<null>");
    return true;
}

static int SysDup2(int oldfd, int newfd) {
    if (oldfd == newfd) {
        long ret = syscall(SYS_fcntl, oldfd, F_GETFD);
        return ret == -1 ? -1 : newfd;
    }
    return (int)syscall(SYS_dup3, oldfd, newfd, 0);
}

static std::string BuildAutoDumpPath(const char* tag) {
    return android::base::StringPrintf(
            "%s.%s.pid_%d.seq_%u.time_%ld.txt", g_debug->config().backtrace_dump_prefix(),
            tag, getpid(), g_dump_sequence.fetch_add(1, std::memory_order_relaxed), time(NULL));
}

static void singal_dump_heap(int) {
    if ((g_debug->config().options() & BACKTRACE)) {
        debug_dump_heap_on_signal();
    }
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

    if (!ScopedConcurrentLock::Init()) {
        LogHookError("alloc_hook: failed to initialize rwlock\n");
        g_debug = nullptr;
        DebugDisableFinalize();
        return false;
    }

    if (g_debug->config().options() & DUMP_ON_SINGAL) {
        struct sigaction enable_act = {};
        enable_act.sa_handler = singal_dump_heap;
        enable_act.sa_flags = SA_RESTART | SA_ONSTACK;
        if (sigaction(
                    g_debug->config().backtrace_dump_signal(), &enable_act, nullptr) !=
            0) {
            LogHookError(
                    "alloc_hook: failed to install dump signal %d: %s\n",
                    g_debug->config().backtrace_dump_signal(), strerror(errno));
        }
    }

    LogHookError(
            "alloc_hook: initialized pid=%d dump_prefix=%s dump_on_exit=%d "
            "frames=%zu min_size=%zu max_size=%zu record_peak=%d peak_dump_mb=%zu "
            "signal=%d\n",
            getpid(), g_debug->config().backtrace_dump_prefix(),
            g_debug->config().backtrace_dump_on_exit() ? 1 : 0,
            g_debug->config().backtrace_frames(),
            g_debug->config().backtrace_min_size_bytes(),
            g_debug->config().backtrace_max_size_bytes(),
            (g_debug->config().options() & RECORD_MEMORY_PEAK) ? 1 : 0,
            g_debug->config().backtrace_dump_peak_val() / 1024 / 1024,
            g_debug->config().backtrace_dump_signal());

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
        DumpHeapToFile(BuildAutoDumpPath("exit").c_str());
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
    LogHookError("alloc_hook: checkpoint dump requested path=%s\n",
                 file_name != nullptr ? file_name : "<null>");
    DumpHeapToFile(file_name);
}

void debug_dump_heap_on_signal() {
    ScopedConcurrentLock lock;
    LogHookError("alloc_hook: signal dump requested\n");
    DumpHeapToFile(BuildAutoDumpPath("signal").c_str());
}

static void* InternalMalloc(size_t size) {
    void* result = m_sys_malloc(size);
    if (result != nullptr && g_debug->TrackPointers()) {
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

    void* new_pointer = m_sys_realloc(pointer, bytes);
    if (new_pointer == nullptr) {
        return nullptr;
    }

    if (g_debug->TrackPointers()) {
        g_debug->pointer->Remove(pointer);
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

struct DmaBufInfo {
    uint64_t inode = 0;
    size_t size = 0;
};

static std::mutex tracked_dma_fd_mutex;
static std::unordered_map<int, DmaBufInfo> tracked_dma_fds;
static std::unordered_map<uint64_t, size_t> tracked_dma_inode_refcounts;

static bool ReadDmaBufInfo(int fd, DmaBufInfo* info) {
    std::string fdinfo = android::base::StringPrintf("/proc/self/fdinfo/%d", fd);
    auto fp = std::unique_ptr<FILE, decltype(&fclose)>{fopen(fdinfo.c_str(), "re"), fclose};
    if (fp == nullptr) {
        return false;
    }

    bool is_dmabuf_file = false;
    uint64_t inode = static_cast<uint64_t>(-1);
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
                    info->size = strtoull(c, nullptr, 10);
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
        std::string fd_path = android::base::StringPrintf("/proc/self/fd/%d", fd);
        struct stat sb;
        if (stat(fd_path.c_str(), &sb) != 0) {
            return false;
        }
        inode = sb.st_ino;
    }

    info->inode = inode;
    return true;
}

static bool TrackDmaBufFd(int fd, DmaBufInfo* info) {
    DmaBufInfo new_info;
    if (!ReadDmaBufInfo(fd, &new_info)) {
        return false;
    }

    std::lock_guard<std::mutex> guard(tracked_dma_fd_mutex);
    auto [fd_entry, inserted] = tracked_dma_fds.emplace(fd, new_info);
    if (!inserted) {
        if (info != nullptr) {
            *info = fd_entry->second;
        }
        return false;
    }

    const size_t refs = ++tracked_dma_inode_refcounts[new_info.inode];
    if (info != nullptr) {
        *info = new_info;
    }
    return refs == 1;
}

static bool UntrackDmaBufFd(int fd, DmaBufInfo* info) {
    std::lock_guard<std::mutex> guard(tracked_dma_fd_mutex);
    auto fd_entry = tracked_dma_fds.find(fd);
    if (fd_entry == tracked_dma_fds.end()) {
        return false;
    }

    const DmaBufInfo current = fd_entry->second;
    tracked_dma_fds.erase(fd_entry);

    auto ref_entry = tracked_dma_inode_refcounts.find(current.inode);
    if (ref_entry == tracked_dma_inode_refcounts.end()) {
        return false;
    }

    if (ref_entry->second > 1) {
        ref_entry->second--;
        return false;
    }

    tracked_dma_inode_refcounts.erase(ref_entry);
    if (info != nullptr) {
        *info = current;
    }
    return true;
}

static void TrackDupFd(int oldfd, int newfd) {
    if (oldfd == newfd) {
        return;
    }

    std::lock_guard<std::mutex> guard(tracked_dma_fd_mutex);
    auto old_entry = tracked_dma_fds.find(oldfd);
    if (old_entry == tracked_dma_fds.end()) {
        return;
    }

    auto new_entry = tracked_dma_fds.find(newfd);
    if (new_entry != tracked_dma_fds.end()) {
        auto old_ref = tracked_dma_inode_refcounts.find(new_entry->second.inode);
        if (old_ref != tracked_dma_inode_refcounts.end()) {
            if (old_ref->second > 1) {
                old_ref->second--;
            } else {
                tracked_dma_inode_refcounts.erase(old_ref);
            }
        }
    }

    tracked_dma_fds[newfd] = old_entry->second;
    ++tracked_dma_inode_refcounts[old_entry->second.inode];
}

static bool ConsumePendingGpuAlloc() {
    if (!gpu_ioctl_alloc) {
        return false;
    }
    gpu_ioctl_alloc = false;
    return true;
}

static bool handle_dma_node(
        unsigned int request, void* arg, int* fd, DmaBufInfo* info) {
    // Delay parsing the backtrace until the next mmap/mmap64 on this thread.
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
            return TrackDmaBufFd(*fd, info);
        case CAM_MEM_ION_MAP_PA: {
                struct CAM_MEM_DEV_ION_NODE_STRUCT* heap = (struct CAM_MEM_DEV_ION_NODE_STRUCT*)arg;
                *fd = heap->memID;
            }
            return TrackDmaBufFd(*fd, info);
        default:
            return false;
    }
}

}  // namespace DMA_BUF

int debug_ioctl(int fd, unsigned int request, void* arg) {
    if (DebugCallsDisabled()) {
        return (int)syscall(SYS_ioctl, fd, request, arg);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    int ret = (int)syscall(SYS_ioctl, fd, request, arg);

    int node_fd = -1;
    DMA_BUF::DmaBufInfo node_info;
    if (ret == 0 && g_debug->TrackPointers() &&
        DMA_BUF::handle_dma_node(request, arg, &node_fd, &node_info)) {
        void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(node_info.inode));
        g_debug->pointer->Add(ptr, node_info.size, DMA);
    }

    return ret;
}

int debug_close(int fd) {
    if (DebugCallsDisabled()) {
        return (int)syscall(SYS_close, fd);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    DMA_BUF::DmaBufInfo removed_info;
    if (g_debug->TrackPointers() && DMA_BUF::UntrackDmaBufFd(fd, &removed_info)) {
        void* ptr =
                reinterpret_cast<void*>(static_cast<uintptr_t>(removed_info.inode));
        g_debug->pointer->Remove(ptr);
    }

    return (int)syscall(SYS_close, fd);
}

int debug_dup(int oldfd) {
    if (DebugCallsDisabled()) {
        return (int)syscall(SYS_dup, oldfd);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    int newfd = (int)syscall(SYS_dup, oldfd);
    if (newfd >= 0 && g_debug->TrackPointers()) {
        DMA_BUF::TrackDupFd(oldfd, newfd);
    }
    return newfd;
}

int debug_dup2(int oldfd, int newfd) {
    if (DebugCallsDisabled()) {
        return SysDup2(oldfd, newfd);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    int result = SysDup2(oldfd, newfd);
    if (result >= 0 && g_debug->TrackPointers()) {
        DMA_BUF::TrackDupFd(oldfd, result);
    }
    return result;
}

int debug_dup3(int oldfd, int newfd, int flags) {
    if (DebugCallsDisabled()) {
        return (int)syscall(SYS_dup3, oldfd, newfd, flags);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    int result = (int)syscall(SYS_dup3, oldfd, newfd, flags);
    if (result >= 0 && g_debug->TrackPointers()) {
        DMA_BUF::TrackDupFd(oldfd, result);
    }
    return result;
}

void* debug_mmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (DebugCallsDisabled()) {
        return (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (size > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    void* result = (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    const bool pending_gpu_alloc =
            g_debug->TrackPointers() && DMA_BUF::ConsumePendingGpuAlloc();
    if (pending_gpu_alloc && result != MAP_FAILED) {
        g_debug->pointer->Add(result, size, DMA);
    }
    return result;
}

void* debug_mmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (DebugCallsDisabled()) {
        return (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (size > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    void* result = (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    if (g_debug->TrackPointers()) {
        if (DMA_BUF::ConsumePendingGpuAlloc()) {
            if (result != MAP_FAILED) {
                g_debug->pointer->Add(result, size, DMA);
            }
            return result;
        }

        DMA_BUF::DmaBufInfo node_info;
        if (fd < 0) {
            if (result != MAP_FAILED) {
                g_debug->pointer->Add(result, size, MMAP);
            }
        } else if (DMA_BUF::TrackDmaBufFd(fd, &node_info)) {
            void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(node_info.inode));
            g_debug->pointer->Add(ptr, node_info.size, DMA);
        }
    }

    return result;
}

void* debug_mremap(
        void* old_address, size_t old_size, size_t new_size, int flags,
        void* new_address) {
    if (DebugCallsDisabled()) {
        return (void*)syscall(
                SYS_mremap, old_address, old_size, new_size, flags, new_address);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    PointerInfoType old_info = {};
    const bool was_tracked =
            g_debug->TrackPointers() && g_debug->pointer->GetInfo(old_address, &old_info);

    void* result = (void*)syscall(
            SYS_mremap, old_address, old_size, new_size, flags, new_address);
    if (result == MAP_FAILED) {
        return result;
    }

    if (was_tracked) {
        if ((flags & MREMAP_DONTUNMAP) == 0) {
            g_debug->pointer->Remove(old_address);
        }
        g_debug->pointer->Add(result, new_size, old_info.mem_type);
    }
    return result;
}

int debug_madvise(void* addr, size_t size, int advice) {
    if (DebugCallsDisabled()) {
        return (int)syscall(SYS_madvise, addr, size, advice);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    int ret = (int)syscall(SYS_madvise, addr, size, advice);
    if (ret == 0 && g_debug->TrackPointers()) {
        g_debug->pointer->RecordAdvice(addr, size, advice);
    }
    return ret;
}

int debug_munmap(void* addr, size_t size) {
    if (DebugCallsDisabled()) {
        return (int)syscall(SYS_munmap, addr, size);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (g_debug->TrackPointers()) {
        g_debug->pointer->Remove(addr);
    }

    return (int)syscall(SYS_munmap, addr, size);
}
