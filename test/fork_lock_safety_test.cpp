// fork() clones only the calling thread. Any hook lock another thread held at
// fork time is inherited *locked* by the child, where the owner does not exist
// to release it, so the child's first tracked allocation blocks forever. This
// gate keeps several threads allocating (and therefore repeatedly holding the
// tracker locks) while the main thread forks, and requires every child to reach
// _exit() within a timeout.
//
// The polarity matters: with the pthread_atfork barrier in place the test passes
// deterministically, because the pre-fork handler cannot return until no other
// thread is inside the tracker. Without it, a child hangs and the timeout fires.

#include "DebugData.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int kForkRounds = 40;
constexpr int kAllocatorThreads = 3;
// Generous: a healthy child needs microseconds. This only has to be shorter
// than "forever" to tell a deadlock from a slow machine.
constexpr int kChildTimeoutSeconds = 10;

std::atomic<bool> g_keep_allocating{true};

// Churns the tracker so the fork lands while the locks are in active use.
void AllocatorThread() {
    while (g_keep_allocating.load(std::memory_order_relaxed)) {
        void* blocks[8];
        for (int i = 0; i < 8; ++i) {
            blocks[i] = debug_malloc(64 + i * 32);
        }
        for (int i = 0; i < 8; ++i) {
            if (blocks[i] != nullptr) {
                blocks[i] = debug_realloc(blocks[i], 256 + i * 16);
            }
        }
        for (int i = 0; i < 8; ++i) {
            debug_free(blocks[i]);
        }
    }
}

// waitpid() with a deadline. Returns false if the child had to be killed, which
// is the deadlock signature this gate exists to catch.
bool WaitForChild(pid_t pid) {
    for (int elapsed_ms = 0; elapsed_ms < kChildTimeoutSeconds * 1000;
         elapsed_ms += 5) {
        int status = 0;
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (done < 0 && errno != EINTR) {
            return false;
        }
        usleep(5000);
    }
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    return false;
}

}  // namespace

int main() {
    setenv("ALLOC_HOOK_CAPTURE_MODE", "fast", 1);
    m_sys_malloc = std::malloc;
    m_sys_free = std::free;
    m_sys_calloc = std::calloc;
    m_sys_realloc = std::realloc;
    m_sys_memalign = nullptr;
    m_sys_aligned_alloc = nullptr;
    m_sys_posix_memalign = nullptr;

    alignas(DebugData) static unsigned char debug_storage[sizeof(DebugData)];
    alignas(PointerData) static unsigned char pointer_storage[sizeof(PointerData)];
    void* init_space[] = {debug_storage, pointer_storage};
    // Outside assert(): under NDEBUG the call would vanish and every debug_*
    // entry point below would dereference a null g_debug.
    const bool initialized = debug_initialize(init_space);
    assert(initialized);
    (void)initialized;

    std::vector<std::thread> allocators;
    allocators.reserve(kAllocatorThreads);
    for (int i = 0; i < kAllocatorThreads; ++i) {
        allocators.emplace_back(AllocatorThread);
    }

    int hung_children = 0;
    for (int round = 0; round < kForkRounds; ++round) {
        const pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            // Child: single-threaded, and the tracker must be usable. Any
            // inherited-locked mutex deadlocks here instead of returning.
            void* block = debug_malloc(4096);
            if (block == nullptr) {
                _exit(2);
            }
            block = debug_realloc(block, 8192);
            if (block == nullptr) {
                _exit(3);
            }
            debug_free(block);
            // _exit, not exit: skip atexit/finalize so this measures the
            // allocation path alone.
            _exit(0);
        }
        if (!WaitForChild(pid)) {
            // Stop at the first deadlock: continuing would burn the per-child
            // deadline once per remaining round and turn a precise assertion
            // failure into an opaque ctest timeout.
            ++hung_children;
            break;
        }
    }

    g_keep_allocating.store(false, std::memory_order_relaxed);
    for (std::thread& allocator : allocators) {
        allocator.join();
    }

    assert(hung_children == 0);
    (void)hung_children;

    debug_finalize();
    return 0;
}
