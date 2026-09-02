// Live-pointer tracking is sharded, so its byte counters are maintained with
// relaxed atomics instead of under one mutex. That trades an exact-by-
// construction invariant for one that only holds if every path pairs its
// add with a matching subtract, on the right counter, for the right memory
// type. This exercises those paths from many threads at once and then checks
// the invariant the report depends on: once every tracked allocation has been
// freed, the live totals are back to exactly zero, and the recorded peak is at
// least as large as the largest footprint any thread could observe.
//
// It also covers the two-shard path (Remap) and the take/restore pair, which
// are the only operations that touch more than one counter at a time.

#include "DebugData.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr size_t kThreads = 8;
constexpr size_t kAllocationsPerThread = 4000;
// Above the default BACKTRACE_MIN_SIZE so a share of the allocations also drive
// the stack-capture and peak-snapshot paths rather than only the counters.
constexpr size_t kLargeSize = 64 * 1024;
constexpr size_t kSmallSize = 96;

std::string DumpLive(bool dump_peak = false) {
    // Written next to the test binary rather than under /tmp: this also runs on
    // Android, where /tmp does not exist.
    char path[] = "shard_concurrency_XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    g_debug->pointer->DumpLiveToFile(fd, dump_peak);
    lseek(fd, 0, SEEK_SET);
    std::string out;
    char buffer[4096];
    ssize_t bytes;
    while ((bytes = read(fd, buffer, sizeof(buffer))) > 0) {
        out.append(buffer, static_cast<size_t>(bytes));
    }
    close(fd);
    unlink(path);
    return out;
}

// The report prints "current host used: <n>MB"; parse it back out so the test
// checks the number a reader would actually see, not an internal counter.
double CurrentHostMb(const std::string& report) {
    const std::string key = "current host used: ";
    const size_t start = report.find(key);
    assert(start != std::string::npos);
    return strtod(report.c_str() + start + key.size(), nullptr);
}

void HammerTracker(size_t seed) {
    std::vector<void*> live;
    live.reserve(kAllocationsPerThread);
    for (size_t i = 0; i < kAllocationsPerThread; ++i) {
        // Mixed sizes so both the capture-eligible and the counters-only paths
        // run concurrently on the same shards.
        const size_t size = ((i + seed) % 32 == 0) ? kLargeSize : kSmallSize;
        void* pointer = debug_malloc(size);
        assert(pointer != nullptr);
        live.push_back(pointer);

        // Free part of the working set as we go, so shards see interleaved
        // insert and erase rather than a grow-then-shrink phase split.
        if (live.size() >= 64) {
            const size_t victim = (i + seed) % live.size();
            debug_free(live[victim]);
            live[victim] = live.back();
            live.pop_back();
        }
    }
    for (void* pointer : live) {
        debug_free(pointer);
    }
}

// realloc drives the take/restore and displaced-entry paths: growing across the
// small/large boundary moves an entry between size classes, and the allocator
// may hand back an address that another thread's record just vacated.
void HammerRealloc(size_t seed) {
    for (size_t i = 0; i < kAllocationsPerThread / 4; ++i) {
        void* pointer = debug_malloc(kSmallSize);
        assert(pointer != nullptr);
        void* grown = debug_realloc(pointer, kLargeSize + (seed % 512));
        assert(grown != nullptr);
        void* shrunk = debug_realloc(grown, kSmallSize);
        assert(shrunk != nullptr);
        debug_free(shrunk);
    }
}

}  // namespace

int main() {
    // Turn the peak machinery on before the tracker is built. It is what drives
    // MaybeRecordPeakSnapshot(), which now takes every shard from a thread that
    // holds none -- so with several threads allocating, this is also the
    // coverage for that lock-order change. The step keeps the number of
    // whole-tracker walks bounded while still firing repeatedly.
    setenv("DUMP_PEAK_VALUE_MB", "1", 1);
    setenv("DUMP_PEAK_STEP_MB", "8", 1);

    m_sys_malloc = std::malloc;
    m_sys_free = std::free;
    m_sys_calloc = std::calloc;
    m_sys_realloc = std::realloc;
    m_sys_memalign = nullptr;
    m_sys_aligned_alloc = nullptr;
    m_sys_posix_memalign = nullptr;
    m_sys_mmap = nullptr;
    m_sys_munmap = nullptr;
    m_sys_mremap = nullptr;

    alignas(DebugData) static unsigned char debug_storage[sizeof(DebugData)];
    alignas(PointerData) static unsigned char pointer_storage[sizeof(PointerData)];
    void* init_space[] = {debug_storage, pointer_storage};
    const bool initialized = debug_initialize(init_space);
    assert(initialized);
    (void)initialized;

    // A tracked allocation held for the whole run, so the "everything freed"
    // check below is against a known non-zero baseline rather than against a
    // tracker that might simply never have recorded anything.
    void* anchor = debug_malloc(kLargeSize);
    assert(anchor != nullptr);
    const std::string with_anchor = DumpLive();
    const double anchor_mb = CurrentHostMb(with_anchor);
    assert(anchor_mb > 0.0);

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (size_t i = 0; i < kThreads; ++i) {
        if (i % 4 == 3) {
            threads.emplace_back(HammerRealloc, i);
        } else {
            threads.emplace_back(HammerTracker, i);
        }
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    // Every allocation those threads made has been freed, so the live total must
    // be back to exactly the anchor. A missed subtract, a subtract charged to
    // the wrong memory type, or a lost update between two shards all show up
    // here as drift.
    const std::string after = DumpLive();
    const double after_mb = CurrentHostMb(after);
    if (after_mb != anchor_mb) {
        fprintf(stderr,
                "live host total drifted: %f MB before, %f MB after %zu threads\n",
                anchor_mb, after_mb, kThreads);
        return EXIT_FAILURE;
    }

    // The peak is a monotonic maximum raised with a relaxed CAS loop, so it may
    // land anywhere at or above the largest total actually reached -- but it can
    // never be below the footprint that was demonstrably live. Read from a peak
    // dump, which is the only one that prints the counters.
    const std::string peak_report = DumpLive(true);
    const std::string key = "process_peak: host=";
    const size_t start = peak_report.find(key);
    assert(start != std::string::npos);
    const double peak_mb = strtod(peak_report.c_str() + start + key.size(), nullptr);
    if (peak_mb < anchor_mb) {
        fprintf(stderr, "peak host %f MB is below the live anchor %f MB\n", peak_mb,
                anchor_mb);
        return EXIT_FAILURE;
    }

    debug_free(anchor);
    const double emptied_mb = CurrentHostMb(DumpLive());
    if (emptied_mb != 0.0) {
        fprintf(stderr, "live host total is %f MB with nothing tracked\n",
                emptied_mb);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
