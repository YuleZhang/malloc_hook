#include "DebugData.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

alignas(4096) unsigned char g_old_mapping[4096];
alignas(4096) unsigned char g_new_mapping[8192];
int g_mremap_calls = 0;
void* g_mremap_result = g_new_mapping;
int g_munmap_result = 0;
bool g_realloc_should_fail = false;

void* FakeMmap(void*, size_t, int, int, int, off_t) {
    return g_old_mapping;
}

int FakeMunmap(void*, size_t) {
    return g_munmap_result;
}

void* FakeRealloc(void* pointer, size_t size) {
    if (g_realloc_should_fail) {
        // A failed realloc leaves the caller's original block untouched.
        return nullptr;
    }
    return std::realloc(pointer, size);
}

void* FakeMremap(void* old_addr, size_t old_size, size_t new_size, int flags, ...) {
    assert((old_addr == g_old_mapping && g_mremap_calls == 0) ||
           (old_addr == g_new_mapping && g_mremap_calls >= 1));
    assert(old_size == (g_mremap_calls == 0 ? sizeof(g_old_mapping)
                                             : sizeof(g_new_mapping)));
    assert(new_size == sizeof(g_new_mapping));
    assert((flags & MREMAP_MAYMOVE) != 0);
    ++g_mremap_calls;
    return g_mremap_result;
}

std::string DumpLive() {
    FILE* file = tmpfile();
    assert(file != nullptr);
    g_debug->pointer->DumpLiveToFile(fileno(file), false);
    fflush(file);
    fseek(file, 0, SEEK_SET);
    std::string output;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), file) != nullptr) {
        output += buffer;
    }
    fclose(file);
    return output;
}

}  // namespace

int main() {
    setenv("ALLOC_HOOK_CAPTURE_MODE", "fast", 1);
    unsetenv("ALLOC_HOOK_FAST_CAPTURE_INTERVAL_BYTES");
    m_sys_malloc = std::malloc;
    m_sys_free = std::free;
    m_sys_calloc = std::calloc;
    m_sys_realloc = FakeRealloc;
    m_sys_memalign = nullptr;
    m_sys_aligned_alloc = nullptr;
    m_sys_posix_memalign = nullptr;
    m_sys_mmap = FakeMmap;
    m_sys_munmap = FakeMunmap;
    m_sys_mremap = FakeMremap;

    alignas(DebugData) static unsigned char debug_storage[sizeof(DebugData)];
    alignas(PointerData) static unsigned char pointer_storage[sizeof(PointerData)];
    void* init_space[] = {debug_storage, pointer_storage};
    // debug_initialize() sets up g_debug, so it must run outside assert():
    // under NDEBUG the call would be dropped and every debug_* entry point
    // below would dereference a null g_debug.
    const bool initialized = debug_initialize(init_space);
    assert(initialized);
    (void)initialized;

    void* mapping = debug_mmap(
            nullptr, sizeof(g_old_mapping), PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping == g_old_mapping);

    void* moved = debug_mremap(
            g_old_mapping, sizeof(g_old_mapping), sizeof(g_new_mapping),
            MREMAP_MAYMOVE, nullptr);
    assert(moved == g_new_mapping);
    assert(g_mremap_calls == 1);
    assert(g_debug->pointer->MightContain(g_new_mapping));
    std::string output = DumpLive();
    assert(output.find("alloc_size:8.000000KB") != std::string::npos);

    g_mremap_result = MAP_FAILED;
    void* failed_remap = debug_mremap(
            g_new_mapping, sizeof(g_new_mapping), sizeof(g_new_mapping),
            MREMAP_MAYMOVE, nullptr);
    assert(failed_remap == MAP_FAILED);
    (void)failed_remap;
    assert(g_mremap_calls == 2);
    assert(g_debug->pointer->MightContain(g_new_mapping));
    assert(!g_debug->pointer->MightContain(MAP_FAILED));
    output = DumpLive();
    assert(output.find("alloc_size:8.000000KB") != std::string::npos);

    g_mremap_result = g_new_mapping;
    void* same_remap = debug_mremap(
            g_new_mapping, sizeof(g_new_mapping), sizeof(g_new_mapping),
            MREMAP_MAYMOVE, nullptr);
    assert(same_remap == g_new_mapping);
    (void)same_remap;
    assert(g_mremap_calls == 3);
    assert(g_debug->pointer->MightContain(g_new_mapping));
    output = DumpLive();
    assert(output.find("alloc_size:8.000000KB") != std::string::npos);

    // A failed munmap leaves the mapping live, so it must stay accounted for.
    // The record is detached before the syscall runs (so a concurrent mmap
    // cannot have its own record erased) and restored when the syscall fails.
    g_munmap_result = -1;
    assert(debug_munmap(g_new_mapping, sizeof(g_new_mapping)) == -1);
    output = DumpLive();
    assert(output.find("alloc_size:8.000000KB") != std::string::npos);

    // A successful munmap drops the record.
    g_munmap_result = 0;
    assert(debug_munmap(g_new_mapping, sizeof(g_new_mapping)) == 0);
    output = DumpLive();
    assert(output.find("alloc_size:8.000000KB") == std::string::npos);

    // Same contract on the realloc path: a failed realloc keeps the original
    // block tracked at its original size.
    void* host = debug_malloc(4096);
    assert(host != nullptr);
    assert(g_debug->pointer->MightContain(host));
    output = DumpLive();
    assert(output.find("alloc_size:4.000000KB") != std::string::npos);

    g_realloc_should_fail = true;
    void* failed_realloc = debug_realloc(host, 16384);
    g_realloc_should_fail = false;
    assert(failed_realloc == nullptr);
    (void)failed_realloc;
    output = DumpLive();
    assert(output.find("alloc_size:4.000000KB") != std::string::npos);
    assert(output.find("alloc_size:16.000000KB") == std::string::npos);

    // A successful realloc moves the accounting over to the new size exactly
    // once, with no residual entry for the old block.
    void* grown = debug_realloc(host, 16384);
    assert(grown != nullptr);
    output = DumpLive();
    assert(output.find("alloc_size:16.000000KB") != std::string::npos);
    assert(output.find("alloc_size:4.000000KB") == std::string::npos);

    debug_free(grown);
    output = DumpLive();
    assert(output.find("alloc_size:16.000000KB") == std::string::npos);

    debug_finalize();
    return 0;
}
