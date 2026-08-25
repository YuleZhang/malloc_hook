#include "DebugData.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

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
    assert(debug_initialize(init_space));

    // Anonymous mappings are tracked independently of malloc interception.
    void* mapping = debug_mmap(
            nullptr, 8192, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    assert(g_debug->pointer->MightContain(mapping));
    assert(debug_munmap(mapping, 8192) == 0);

    // Legacy requests have no _IOC_SIZE field.  The interposer must still
    // forward their vararg instead of replacing it with nullptr.
    int pipe_fds[2] = {-1, -1};
    assert(pipe(pipe_fds) == 0);
    const char payload[] = "hook";
    assert(write(pipe_fds[1], payload, sizeof(payload)) ==
           static_cast<ssize_t>(sizeof(payload)));
    int available = 0;
    assert(debug_ioctl(pipe_fds[0], FIONREAD, &available) == 0);
    assert(available == static_cast<int>(sizeof(payload)));
    assert(close(pipe_fds[0]) == 0);
    assert(close(pipe_fds[1]) == 0);

    debug_finalize();
    return 0;
}
