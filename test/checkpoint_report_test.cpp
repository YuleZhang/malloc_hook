// Contract for the checkpoint (dump-signal) reports that a repeated-run leak
// comparison depends on.
//
// The workflow this pins down: a test loops one full create -> work -> release
// cycle N times and sends the hook's dump signal at the end of an early
// iteration and again at the end of the last one, then diffs the two reports to
// find what each cycle leaks. That only works if
//
//   * every checkpoint gets its own report -- two checkpoints inside the same
//     wall-clock second used to produce the same file name, and the second
//     silently truncated the first;
//   * a checkpoint asked for at the end of the *last* iteration still produces
//     its report, even though the process starts exiting immediately after --
//     finalization used to take the allocator write lock and never release it,
//     parking the dump worker until the process died with nothing on disk and
//     nothing in the log;
//   * a checkpoint that arrives once teardown has passed the point of no return
//     is refused rather than left to block forever.

#include "Config.h"
#include "DebugData.h"
#include "PointerData.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string g_report_dir;

std::string MakeReportDir() {
    const char* tmp = getenv("TMPDIR");
    std::string pattern = (tmp != nullptr && tmp[0] != '\0' ? tmp : "/tmp");
    pattern += "/alloc_hook_checkpoint_XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* created = mkdtemp(buffer.data());
    assert(created != nullptr);
    return std::string(created);
}

// Report basenames in the directory, ordered by the sequence number the hook
// stamped into them, so a positional check reads as the order the reports were
// produced in. Sorting by name would not: "exit" sorts before "signal".
std::vector<std::string> Reports() {
    std::vector<std::pair<unsigned, std::string>> found;
    DIR* dir = opendir(g_report_dir.c_str());
    assert(dir != nullptr);
    while (const dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.compare(0, 3, "bt.") != 0) {
            continue;
        }
        const size_t at = name.find(".seq_");
        // A name with no sequence field is a regression, not a reason to abort
        // here: ordering it last lets the assertions below name the real
        // problem instead of failing inside this helper.
        const unsigned sequence = at == std::string::npos
                ? ~0u
                : static_cast<unsigned>(
                          strtoul(name.c_str() + at + 5, nullptr, 10));
        found.emplace_back(sequence, name);
    }
    closedir(dir);
    std::sort(found.begin(), found.end());
    std::vector<std::string> names;
    names.reserve(found.size());
    for (const auto& entry : found) {
        names.push_back(entry.second);
    }
    return names;
}

// Reports are written by a worker thread, so the count only settles
// asynchronously. Bounded so a regression fails the assertion below instead of
// hanging the suite.
std::vector<std::string> WaitForReports(size_t expected) {
    for (int waited_ms = 0; waited_ms < 5000; ++waited_ms) {
        std::vector<std::string> names = Reports();
        if (names.size() >= expected) {
            return names;
        }
        usleep(1000);
    }
    return Reports();
}

size_t FileSize(const std::string& name) {
    struct stat info = {};
    assert(stat((g_report_dir + "/" + name).c_str(), &info) == 0);
    return static_cast<size_t>(info.st_size);
}

// "current host used: 6.758754MB, ..." -> 6.758754
double HostUsedMb(const std::string& name) {
    FILE* file = fopen((g_report_dir + "/" + name).c_str(), "r");
    assert(file != nullptr);
    char line[512] = {};
    const char* read = fgets(line, sizeof(line), file);
    fclose(file);
    assert(read != nullptr);
    double value = -1.0;
    assert(sscanf(line, "current host used: %lfMB", &value) == 1);
    return value;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void* Leak(size_t size) {
    void* pointer = debug_malloc(size);
    assert(pointer != nullptr);
    memset(pointer, 0x5a, size);
    return pointer;
}

}  // namespace

int main() {
    g_report_dir = MakeReportDir();
    const std::string prefix = g_report_dir + "/bt";

    // SIGRTMIN+7 rather than the platform default: nothing else in the test
    // binary uses it, so a stray delivery cannot be mistaken for a checkpoint.
    const int dump_signal = SIGRTMIN + 7;
    char signal_text[16];
    snprintf(signal_text, sizeof(signal_text), "%d", dump_signal);

    setenv("ALLOC_HOOK_CAPTURE_MODE", "fast", 1);
    setenv("ALLOC_HOOK_DUMP_PREFIX", prefix.c_str(), 1);
    setenv("BACKTRACE_DUMP_SIGNAL", signal_text, 1);
    setenv("BACKTRACE_MIN_SIZE", "1024", 1);
    // Also turns on the exit report, so the test covers that the exit report
    // continues the same sequence instead of colliding with a checkpoint.
    setenv("DUMP_PEAK_VALUE_MB", "0", 1);
    unsetenv("ALLOC_HOOK_PEAK_SAMPLE_MS");
    unsetenv("ALLOC_HOOK_FAST_CAPTURE_INTERVAL_BYTES");
    unsetenv("ALLOC_HOOK_SAMPLING_INTERVAL_BYTES");

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
    assert((g_debug->config().options() & DUMP_ON_SINGAL) != 0);
    assert(g_debug->config().backtrace_dump_signal() == dump_signal);

    // Iteration 1 of the modelled run leaks 64KB.
    Leak(64 * 1024);
    const time_t first_checkpoint_second = time(NULL);
    raise(dump_signal);

    // Nothing on disk yet, and that is the contract: the signal path only
    // captures a snapshot. Generating the report there held both tracker mutexes
    // for the whole of it, which blocked every allocation in the process --
    // measured at 446ms with 20k live allocations. Give the worker time to have
    // written a file if it were still going to.
    for (int waited_ms = 0; waited_ms < 200; ++waited_ms) {
        usleep(1000);
    }
    assert(Reports().empty());

    // Iterations 2..N leak the same amount again, then checkpoint a second time.
    // Deliberately no delay: back-to-back checkpoints land in the same second,
    // which is exactly the case that used to lose a report.
    for (int i = 0; i < 4; ++i) {
        Leak(64 * 1024);
    }
    raise(dump_signal);
    const time_t second_checkpoint_second = time(NULL);

    // The checkpoint at the end of the last iteration: requested, then the
    // process immediately starts exiting. Its snapshot must still be taken, and
    // every retained snapshot must reach disk.
    Leak(64 * 1024);
    raise(dump_signal);
    debug_finalize();

    // All three checkpoints plus the exit report, written by shutdown.
    std::vector<std::string> reports = WaitForReports(4);
    assert(reports.size() == 4);

    // Three checkpoints, three reports, kept apart by the sequence number rather
    // than by a one-second clock. The sequence is assigned when the checkpoint
    // was taken, not when it was written, so it still describes the order the
    // run asked for them in.
    assert(Contains(reports[0], ".signal.") && Contains(reports[0], ".seq_0."));
    assert(Contains(reports[1], ".signal.") && Contains(reports[1], ".seq_1."));
    assert(Contains(reports[2], ".signal.") && Contains(reports[2], ".seq_2."));
    assert(FileSize(reports[0]) != 0 && FileSize(reports[1]) != 0);
    assert(FileSize(reports[2]) != 0);
    assert(Contains(reports[3], ".exit.") && Contains(reports[3], ".seq_3."));
    assert(FileSize(reports[3]) != 0);
    // Only meaningful when the pair really did share a second; if the run
    // straddled a boundary the sequence check above is what carried the case.
    if (first_checkpoint_second == second_checkpoint_second) {
        assert(reports[0] != reports[1]);
    }

    // The point of several reports: the growth between them is the
    // per-iteration leak, which is only visible because the earlier snapshot
    // survived.
    const double first_mb = HostUsedMb(reports[0]);
    const double second_mb = HostUsedMb(reports[1]);
    assert(second_mb > first_mb);
    assert(second_mb - first_mb > 0.2);  // 4 x 64KB = 0.25MB

    // A checkpoint that arrives after teardown has closed the door must be
    // refused, not served and not left blocking: finalization holds the
    // allocator write lock for good, so a dump worker that reached for the read
    // side here would never come back. Reaching the end of this test at all is
    // the no-hang half of the assertion.
    raise(dump_signal);
    for (int waited_ms = 0; waited_ms < 200; ++waited_ms) {
        usleep(1000);
    }
    assert(Reports().size() == 4);

    for (const std::string& name : Reports()) {
        unlink((g_report_dir + "/" + name).c_str());
    }
    rmdir(g_report_dir.c_str());
    return 0;
}
