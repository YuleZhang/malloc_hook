#include "DebugData.h"
#include "debug_disable.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <new>

#include <dirent.h>
#include <unistd.h>

namespace {

alignas(64) unsigned char g_malloc_storage[128];
alignas(64) unsigned char g_calloc_storage[128];
alignas(64) unsigned char g_aligned_storage[128];
alignas(64) unsigned char g_memalign_storage[128];
alignas(64) unsigned char g_posix_storage[128];
size_t g_malloc_calls = 0;
size_t g_calloc_calls = 0;
size_t g_calloc_nmemb = 0;
size_t g_calloc_size = 0;
size_t g_aligned_calls = 0;
size_t g_memalign_calls = 0;
size_t g_free_calls = 0;
bool g_free_was_disabled = false;

void* FakeMalloc(size_t size) {
    ++g_malloc_calls;
    return size <= sizeof(g_malloc_storage) ? g_malloc_storage : nullptr;
}

void FakeFree(void* pointer) {
    if (pointer == g_malloc_storage || pointer == g_aligned_storage ||
        pointer == g_calloc_storage || pointer == g_memalign_storage ||
        pointer == g_posix_storage) {
        ++g_free_calls;
        g_free_was_disabled = DebugCallsDisabled();
    }
}

void* FakeCalloc(size_t nmemb, size_t size) {
    ++g_calloc_calls;
    g_calloc_nmemb = nmemb;
    g_calloc_size = size;
    return nmemb != 0 && size <= sizeof(g_calloc_storage) / nmemb
                   ? g_calloc_storage
                   : nullptr;
}

void* FakeRealloc(void* pointer, size_t size) {
    if (pointer == nullptr) {
        return FakeMalloc(size);
    }
    return size <= sizeof(g_malloc_storage) ? g_malloc_storage : nullptr;
}

void* FakeMemalign(size_t alignment, size_t size) {
    (void)alignment;
    ++g_memalign_calls;
    return size <= sizeof(g_memalign_storage) ? g_memalign_storage : nullptr;
}

void* FakeAlignedAlloc(size_t alignment, size_t size) {
    (void)alignment;
    ++g_aligned_calls;
    return size <= sizeof(g_aligned_storage) ? g_aligned_storage : nullptr;
}

int FakePosixMemalign(void** pointer, size_t alignment, size_t size) {
    (void)alignment;
    if (size > sizeof(g_posix_storage)) {
        *pointer = nullptr;
        return ENOMEM;
    }
    *pointer = g_posix_storage;
    return 0;
}

// Live entries under /proc/self/task, which is one per thread.
long CountThreads() {
    DIR* dir = opendir("/proc/self/task");
    if (dir == nullptr) {
        return -1;
    }
    long count = 0;
    while (const dirent* entry = readdir(dir)) {
        if (entry->d_name[0] != '.') {
            ++count;
        }
    }
    closedir(dir);
    return count;
}

// pthread_create needs a moment before the task directory reflects it.
long ThreadsAtLeast(long target) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        const long count = CountThreads();
        if (count < 0 || count >= target) {
            return count;
        }
        usleep(1000);
    }
    return CountThreads();
}

}  // namespace

int main() {
    // Force Fast-mode sampling to leave this small allocation unsampled.
    setenv("ALLOC_HOOK_CAPTURE_MODE", "fast", 1);
    setenv("ALLOC_HOOK_SAMPLING_INTERVAL_BYTES", "1000000000", 1);

    m_sys_malloc = FakeMalloc;
    m_sys_free = FakeFree;
    m_sys_calloc = FakeCalloc;
    m_sys_realloc = FakeRealloc;
    m_sys_memalign = FakeMemalign;
    m_sys_aligned_alloc = FakeAlignedAlloc;
    m_sys_posix_memalign = FakePosixMemalign;

    alignas(DebugData) static unsigned char debug_storage[sizeof(DebugData)];
    alignas(PointerData) static unsigned char pointer_storage[sizeof(PointerData)];
    void* init_space[] = {debug_storage, pointer_storage};
    const long threads_before_init = CountThreads();
    if (threads_before_init < 1) {
        return 10;
    }
    if (!debug_initialize(init_space)) {
        return 1;
    }

    // Initialization must not create a thread. It can run from an allocation the
    // loader itself made, before the process runtime is complete, and
    // pthread_create is not usable there -- on Android libc instruments it with
    // a trace scope that reads a system property whose area is not mapped yet,
    // so the call faults instead of failing. Thread creation therefore belongs
    // to debug_start_helper_threads(), which the adapter calls later.
    if (CountThreads() != threads_before_init) {
        return 11;
    }

    // That call is what starts them, and it is idempotent because the adapter
    // reaches it from two directions.
    debug_start_helper_threads();
    const long threads_after_start = ThreadsAtLeast(threads_before_init + 1);
    if (threads_after_start != threads_before_init + 1) {
        return 12;
    }
    debug_start_helper_threads();
    if (CountThreads() != threads_after_start) {
        return 13;
    }

    void* aligned = debug_aligned_alloc(64, 64);
    if (aligned != g_aligned_storage || g_aligned_calls != 1 ||
        g_memalign_calls != 0) {
        return 2;
    }

    void* memaligned = debug_memalign(64, 64);
    if (memaligned != g_memalign_storage || g_memalign_calls != 1 ||
        g_aligned_calls != 1) {
        return 8;
    }

    void* zeroed = debug_calloc(3, 32);
    if (zeroed != g_calloc_storage || g_calloc_calls != 1 ||
        g_calloc_nmemb != 3 || g_calloc_size != 32) {
        return 3;
    }
    debug_free(zeroed);

    void* posix = nullptr;
    if (debug_posix_memalign(&posix, 64, 64) != 0 ||
        posix != g_posix_storage) {
        return 4;
    }
    debug_free(posix);

    void* pointer = debug_malloc(64);
    if (pointer != g_malloc_storage) {
        return 5;
    }
    debug_realloc(pointer, 0);
    if (g_free_calls != 3 || !g_free_was_disabled) {
        return 6;
    }

    debug_free(aligned);
    if (g_free_calls != 4 || !g_free_was_disabled) {
        return 7;
    }
    debug_free(memaligned);
    if (g_free_calls != 5 || !g_free_was_disabled) {
        return 9;
    }
    return 0;
}
