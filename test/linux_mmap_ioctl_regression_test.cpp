#include "DebugData.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// Deliberately not <cassert>. Every check below wraps a call whose side effect
// is the thing under test -- debug_initialize(), debug_ioctl(), close() -- and
// assert() compiles its argument away under NDEBUG, which would turn this gate
// into an unconditional pass. CHECK evaluates its condition exactly once and
// does so in every build type.
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(                                                      \
                    stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #condition);                                               \
            std::exit(1);                                                      \
        }                                                                      \
    } while (false)

int main() {
    m_sys_malloc = std::malloc;
    m_sys_free = std::free;
    m_sys_calloc = std::calloc;
    m_sys_realloc = std::realloc;
    m_sys_memalign = nullptr;
    m_sys_aligned_alloc = nullptr;
    m_sys_posix_memalign = nullptr;
    m_sys_mmap = ::mmap;
    m_sys_munmap = ::munmap;
    m_sys_mremap = nullptr;

    alignas(DebugData) static unsigned char debug_storage[sizeof(DebugData)];
    alignas(PointerData) static unsigned char pointer_storage[sizeof(PointerData)];
    void* init_space[] = {debug_storage, pointer_storage};
    CHECK(debug_initialize(init_space));

    // Anonymous mappings are tracked independently of malloc interception.
    void* mapping = debug_mmap(
            nullptr, 8192, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED);
    CHECK(g_debug->pointer->MightContain(mapping));
    CHECK(debug_munmap(mapping, 8192) == 0);

    // Legacy requests have no _IOC_SIZE field.  The interposer must still
    // forward their vararg instead of replacing it with nullptr.
    int pipe_fds[2] = {-1, -1};
    CHECK(pipe(pipe_fds) == 0);
    const char payload[] = "hook";
    CHECK(write(pipe_fds[1], payload, sizeof(payload)) ==
          static_cast<ssize_t>(sizeof(payload)));
    int available = 0;
    CHECK(debug_ioctl(pipe_fds[0], FIONREAD, &available) == 0);
    CHECK(available == static_cast<int>(sizeof(payload)));
    CHECK(close(pipe_fds[0]) == 0);
    CHECK(close(pipe_fds[1]) == 0);

    debug_finalize();
    return 0;
}
