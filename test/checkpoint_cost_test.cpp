// What a checkpoint is allowed to cost.
//
// A checkpoint is taken by sending the hook a signal, and the workload keeps
// running around it. Whatever the checkpoint does while it holds the two tracker
// mutexes blocks every allocation in the process for that long, so it perturbs
// the very thing it is measuring. Generating the report there measured 130ms at
// 5k live allocations, 446ms at 20k and 1186ms at 60k -- roughly 20us per live
// allocation, because it sorted the live set, walked /proc/self/maps and
// /proc/self/smaps, and wrote one unbuffered dprintf per line, all under the
// locks.
//
// So the signal path only captures a snapshot; resolving module-relative PCs
// and writing the report happen afterwards. This gate exists to keep it that
// way: the temptation to "just format it here" is exactly how the cost comes
// back, and nothing else in the suite measures cost at all.
//
// The primary bound is a ratio against the full report on the same live set, not
// a wall-clock number, so it means the same thing on a fast developer machine
// and a slow CI box. A loose absolute ceiling backs it up, because a ratio alone
// would still pass if both sides regressed together.

#include "Config.h"
#include "DebugData.h"
#include "PointerData.h"
#include "debug_disable.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Enough live allocations to be representative: measured runs of a real camera
// pipeline carried 21k-74k live tracked allocations behind a few hundred
// distinct stacks.
constexpr int kLiveAllocations = 20000;
// The capture must be a small fraction of the report it replaced. Twenty is far
// enough below the ~100x actually measured to absorb a noisy machine, and far
// enough above 1 to fail immediately if the report work moves back under the
// locks.
constexpr double kMinSpeedup = 20.0;
// Backstop for the case where both sides regress together, which the ratio
// alone would not catch. Generous: the capture is expected in single-digit
// milliseconds at this size.
constexpr double kMaxCaptureMs = 200.0;

double NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

std::string MakeDir() {
    const char* tmp = getenv("TMPDIR");
    std::string pattern = (tmp != nullptr && tmp[0] != '\0' ? tmp : "/tmp");
    pattern += "/alloc_hook_cost_XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* created = mkdtemp(buffer.data());
    assert(created != nullptr);
    return std::string(created);
}

// Varied sizes and call depths so the live set is not one uniform bucket.
//
// Note the recursion's call sites all share a return address, so on this host
// the depths collapse into a single captured stack and the diversity comes from
// the sizes alone. That does not skew the measurement: the capture's cost is the
// per-live-allocation walk (20k lookups), not the handful of bucket inserts. The
// assertion below is therefore "did not collapse to one bucket", not "looks like
// a real workload's stack count".
volatile void* g_sink = nullptr;

__attribute__((noinline)) void* AllocateAtDepth(size_t size, int depth) {
    if (depth > 0) {
        void* deeper = AllocateAtDepth(size, depth - 1);
        g_sink = deeper;
        return deeper;
    }
    void* pointer = debug_malloc(size);
    if (pointer != nullptr) {
        memset(pointer, 0x5a, 64);
    }
    return pointer;
}

}  // namespace

int main() {
    const std::string dir = MakeDir();
    const std::string prefix = dir + "/bt";

    setenv("ALLOC_HOOK_CAPTURE_MODE", "fast", 1);
    setenv("ALLOC_HOOK_DUMP_PREFIX", prefix.c_str(), 1);
    setenv("BACKTRACE_MIN_SIZE", "1024", 1);
    unsetenv("DUMP_PEAK_VALUE_MB");
    unsetenv("ALLOC_HOOK_PEAK_SAMPLE_MS");

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
    // Outside assert(): under NDEBUG the call itself would be dropped and every
    // debug_* entry point below would dereference a null g_debug.
    const bool initialized = debug_initialize(init_space);
    assert(initialized);
    (void)initialized;

    std::vector<void*> kept;
    kept.reserve(kLiveAllocations);
    for (int i = 0; i < kLiveAllocations; ++i) {
        void* pointer = AllocateAtDepth(2048 + (i % 16) * 64, i % 12);
        if (pointer != nullptr) {
            kept.push_back(pointer);
        }
    }
    assert(kept.size() > static_cast<size_t>(kLiveAllocations) / 2);
    // The locked half of a checkpoint, measured the way the signal path runs it.
    CheckpointSnapshot snapshot;
    double capture_ms = 0.0;
    {
        ScopedDisableDebugCalls disable;
        const double start = NowMs();
        g_debug->pointer->TakeCheckpointSnapshot(&snapshot);
        capture_ms = NowMs() - start;
    }

    fprintf(stderr, "capture=%.1fms live=%zu buckets=%zu omitted=%zu\n",
            capture_ms, snapshot.live_pointers, snapshot.stacks.size(),
            snapshot.omitted.count);

    // A fast capture that captured nothing would pass a speed gate trivially.
    assert(!snapshot.stacks.empty());
    // Nor should it collapse the whole live set onto a single bucket.
    assert(snapshot.stacks.size() >= 8);
    assert(snapshot.live_pointers >= kept.size());
    size_t counted = 0;
    for (const CheckpointStack& stack : snapshot.stacks) {
        counted += stack.num_allocations;
    }
    assert(counted + snapshot.omitted.count == snapshot.live_pointers);

    // The work that used to sit inside that critical section.
    const std::string report = dir + "/full_report.txt";
    const double report_start = NowMs();
    debug_dump_heap(report.c_str());
    const double report_ms = NowMs() - report_start;

    struct stat info = {};
    assert(stat(report.c_str(), &info) == 0 && info.st_size > 0);

    printf("capture=%.1fms report=%.1fms speedup=%.1fx live=%zu buckets=%zu\n",
           capture_ms, report_ms, report_ms / capture_ms, snapshot.live_pointers,
           snapshot.stacks.size());

    assert(capture_ms > 0.0);
    assert(capture_ms < kMaxCaptureMs);
    assert(report_ms > capture_ms * kMinSpeedup);

    unlink(report.c_str());
    rmdir(dir.c_str());
    return 0;
}
