#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/mman.h>
#include <signal.h>
#include <cstddef>
#include <cstdarg>
#include <cstdio>

#include <dlfcn.h>
#include <fcntl.h>

#include "DebugData.h"
#include "PointerData.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#define RESOLVE(name)                                                              \
    do {                                                                           \
        if (m_sys_##name == nullptr) {                                             \
            auto addr = dlsym(RTLD_NEXT, #name);                                   \
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

static int SysDup2(int oldfd, int newfd) {
    if (oldfd == newfd) {
        long ret = syscall(SYS_fcntl, oldfd, F_GETFD);
        return ret == -1 ? -1 : newfd;
    }
    return (int)syscall(SYS_dup3, oldfd, newfd, 0);
}

class AllocHook {
public:
    AllocHook() {
        InitState state;
        void* ptr[2] = {&Db_storage, &Pd_storage};
        initialized_ = debug_initialize(ptr);
        if (!initialized_) {
            LogHookError("alloc_hook: debug initialization failed, fallback to passthrough mode\n");
        }
    }
    ~AllocHook() {
        if (initialized_) {
            debug_finalize();
        }
    }

    void* malloc(size_t size) { return initialized_ ? debug_malloc(size) : m_sys_malloc(size); }
    void free(void* ptr) {
        if (initialized_) {
            debug_free(ptr);
            return;
        }
        m_sys_free(ptr);
    }
    void* calloc(size_t a, size_t b) {
        return initialized_ ? debug_calloc(a, b) : m_sys_calloc(a, b);
    }
    void* realloc(void* ptr, size_t size) {
        return initialized_ ? debug_realloc(ptr, size) : m_sys_realloc(ptr, size);
    }
    int posix_memalign(void** ptr, size_t alignment, size_t size) {
        return initialized_ ? debug_posix_memalign(ptr, alignment, size)
                            : m_sys_posix_memalign(ptr, alignment, size);
    }
    void* memalign(size_t alignment, size_t bytes) {
        return initialized_ ? debug_memalign(alignment, bytes)
                            : m_sys_memalign(alignment, bytes);
    }
    void* mmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
        return initialized_ ? debug_mmap(addr, size, prot, flags, fd, offset)
                            : (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    }
    void* mremap(
            void* old_address, size_t old_size, size_t new_size, int flags,
            void* new_address) {
        return initialized_
                       ? debug_mremap(old_address, old_size, new_size, flags, new_address)
                       : (void*)syscall(
                                 SYS_mremap, old_address, old_size, new_size, flags,
                                 new_address);
    }
    int madvise(void* addr, size_t size, int advice) {
        return initialized_ ? debug_madvise(addr, size, advice)
                            : (int)syscall(SYS_madvise, addr, size, advice);
    }
    int munmap(void* addr, size_t size) {
        return initialized_ ? debug_munmap(addr, size)
                            : (int)syscall(SYS_munmap, addr, size);
    }
    int ioctl(int fd, int request, void* arg) {
        return initialized_ ? debug_ioctl(fd, request, arg)
                            : (int)syscall(SYS_ioctl, fd, request, arg);
    }
    int close(int fd) {
        return initialized_ ? debug_close(fd) : (int)syscall(SYS_close, fd);
    }
    int dup(int oldfd) {
        return initialized_ ? debug_dup(oldfd) : (int)syscall(SYS_dup, oldfd);
    }
    int dup2(int oldfd, int newfd) {
        return initialized_ ? debug_dup2(oldfd, newfd)
                            : SysDup2(oldfd, newfd);
    }
    int dup3(int oldfd, int newfd, int flags) {
        return initialized_ ? debug_dup3(oldfd, newfd, flags)
                            : (int)syscall(SYS_dup3, oldfd, newfd, flags);
    }
    void* mmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
        return initialized_ ? debug_mmap64(addr, size, prot, flags, fd, offset)
                            : (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    }

    void checkpoint(const char* file_name) {
        if (initialized_) {
            LogHookError("alloc_hook: checkpoint() called with path=%s\n",
                         file_name != nullptr ? file_name : "<null>");
            debug_dump_heap(file_name);
        }
    }
    int kill(pid_t pid, int sig) {
        if (initialized_ && pid == getpid() && sig == 33) {
            LogHookError("alloc_hook: intercepted kill(pid=%d, sig=%d) for in-process dump\n",
                         pid, sig);
            debug_dump_heap_on_signal();
            return 0;
        }
        return (int)syscall(SYS_kill, pid, sig);
    }

    static AllocHook& inst();

private:
    bool initialized_ = false;
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

static volatile bool in_preinit_phase = true;
__attribute__((constructor(201))) void mark_init_done() {
    in_preinit_phase = false;
}

extern "C" {
// 程序初始化会间接调用 malloc 和 free
void* malloc(size_t size) {
    RESOLVE(malloc);
    if (InitState::allocHook_setup) {
        return m_sys_malloc(size);
    }
    return AllocHook::inst().malloc(size);
}

void free(void* ptr) {
    RESOLVE(free);
    if (InitState::allocHook_setup) {
        return m_sys_free(ptr);
    }
    return AllocHook::inst().free(ptr);
}

// calloc 和 realloc 属于用户级函数
void* calloc(size_t a, size_t b) {
    RESOLVE(calloc);
    if (InitState::allocHook_setup) {
        return m_sys_calloc(a, b);
    }
    return AllocHook::inst().calloc(a, b);
}

void* realloc(void* ptr, size_t size) {
    RESOLVE(realloc);
    if (InitState::allocHook_setup) {
        return m_sys_realloc(ptr, size);
    }
    return AllocHook::inst().realloc(ptr, size);
}

void* memalign(size_t alignment, size_t bytes)  {
    RESOLVE(memalign);
    return AllocHook::inst().memalign(alignment, bytes);
}

// 进程初始化 和 debug init 的过程不应该调用 posix_memalign
int posix_memalign(void** ptr, size_t alignment, size_t size) {
    RESOLVE(memalign);
    RESOLVE(posix_memalign);
    return AllocHook::inst().posix_memalign(ptr, alignment, size);
}

void* mmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    }
    void* result = AllocHook::inst().mmap(addr, size, prot, flags, fd, offset);
    return result;
}

void* mremap(void* old_address, size_t old_size, size_t new_size, int flags, ...) {
    void* new_address = nullptr;
    if (flags & MREMAP_FIXED) {
        va_list ap;
        va_start(ap, flags);
        new_address = va_arg(ap, void*);
        va_end(ap);
    }

    if (in_preinit_phase || InitState::allocHook_setup) {
        return (void*)syscall(
                SYS_mremap, old_address, old_size, new_size, flags, new_address);
    }
    return AllocHook::inst().mremap(
            old_address, old_size, new_size, flags, new_address);
}

int munmap(void* addr, size_t size) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return (int)syscall(SYS_munmap, addr, size);
    }
    return AllocHook::inst().munmap(addr, size);
}

int madvise(void* addr, size_t size, int advice) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return (int)syscall(SYS_madvise, addr, size, advice);
    }
    return AllocHook::inst().madvise(addr, size, advice);
}

int ioctl(int fd, int request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);

    return AllocHook::inst().ioctl(fd, request, arg);
}

int close(int fd) {
    return AllocHook::inst().close(fd);
}

int dup(int oldfd) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return (int)syscall(SYS_dup, oldfd);
    }
    return AllocHook::inst().dup(oldfd);
}

int dup2(int oldfd, int newfd) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return SysDup2(oldfd, newfd);
    }
    return AllocHook::inst().dup2(oldfd, newfd);
}

int dup3(int oldfd, int newfd, int flags) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return (int)syscall(SYS_dup3, oldfd, newfd, flags);
    }
    return AllocHook::inst().dup3(oldfd, newfd, flags);
}

int kill(pid_t pid, int sig) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return (int)syscall(SYS_kill, pid, sig);
    }
    return AllocHook::inst().kill(pid, sig);
}

void* mmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    }
    void* result = AllocHook::inst().mmap64(addr, size, prot, flags, fd, offset);
    return result;
}

void checkpoint(const char* file_name) {
    AllocHook::inst().checkpoint(file_name);
}
}
