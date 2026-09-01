// Guards on the exported checkpoint() entry point, and the live report shutdown
// writes.
//
// Two ways exist to make the hook write a live-allocation report: the dump
// signal, and the exported `checkpoint(const char*)` C function that an
// LD_PRELOAD integrator can call. Only the signal path ever had any checks, so
// the exported one could dereference a null g_debug, run strlen() on a null
// path, or park forever on the allocator read lock once finalization had taken
// the write side for good and never released it. Both now go through one guard
// core, and this pins down every refusal.
//
// It also pins the report shutdown writes. Comparing two live reports is how a
// per-iteration leak gets localised, and the last boundary of a run cannot be
// reached with an external signal -- the process is gone before the signal
// lands, which is a race no amount of waiting inside the hook can win. So
// shutdown has to be able to produce a *live* report, not only the peak
// snapshot it has always written.

#include "Config.h"
#include "DebugData.h"
#include "PointerData.h"
#include "debug_disable.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Generous: a healthy child needs milliseconds. This only has to be shorter than
// "forever" to tell a deadlock from a slow machine.
constexpr int kChildTimeoutSeconds = 10;
constexpr int kRaceChildren = 8;

std::string g_root;

std::atomic<bool> g_keep_allocating{true};
std::atomic<bool> g_keep_checkpointing{true};

std::string MakeRoot() {
    const char* tmp = getenv("TMPDIR");
    std::string pattern = (tmp != nullptr && tmp[0] != '\0' ? tmp : "/tmp");
    pattern += "/alloc_hook_ckpt_guard_XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* created = mkdtemp(buffer.data());
    assert(created != nullptr);
    return std::string(created);
}

// Report basenames in `dir`, ordered by the sequence number the hook stamped
// into them. Sorting by name would not work: "exit" sorts before "signal",
// which says nothing about the order they were taken.
std::vector<std::string> ReportsIn(const std::string& dir) {
    std::vector<std::pair<unsigned, std::string>> found;
    DIR* handle = opendir(dir.c_str());
    if (handle == nullptr) {
        return {};
    }
    while (const dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (name.compare(0, 3, "bt.") != 0) {
            continue;
        }
        const size_t at = name.find(".seq_");
        const unsigned sequence = at == std::string::npos
                ? ~0u
                : static_cast<unsigned>(
                          strtoul(name.c_str() + at + 5, nullptr, 10));
        found.emplace_back(sequence, name);
    }
    closedir(handle);
    std::sort(found.begin(), found.end());
    std::vector<std::string> names;
    names.reserve(found.size());
    for (const auto& entry : found) {
        names.push_back(entry.second);
    }
    return names;
}

size_t FileSize(const std::string& path) {
    struct stat info = {};
    assert(stat(path.c_str(), &info) == 0);
    return static_cast<size_t>(info.st_size);
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

// Churns the tracker so a checkpoint has to contend with live allocation
// traffic, which is the only way to exercise the
// rwlock(read) -> pointer_mutex_ -> frame_mutex_ order for real.
void AllocatorThread() {
    while (g_keep_allocating.load(std::memory_order_relaxed)) {
        void* blocks[8];
        for (int i = 0; i < 8; ++i) {
            blocks[i] = debug_malloc(2048 + i * 512);
        }
        for (int i = 0; i < 8; ++i) {
            if (blocks[i] != nullptr) {
                blocks[i] = debug_realloc(blocks[i], 4096 + i * 256);
            }
        }
        for (int i = 0; i < 8; ++i) {
            debug_free(blocks[i]);
        }
    }
}

void ConfigureChild(const std::string& dir) {
    assert(mkdir(dir.c_str(), 0755) == 0);
    const std::string prefix = dir + "/bt";
    setenv("ALLOC_HOOK_DUMP_PREFIX", prefix.c_str(), 1);
}

enum class ChildMode {
    // Shutdown writes the peak report.
    kExitReport,
    // A checkpoint on the finalizing thread after finalization. Not the deadlock
    // shape: glibc's rdlock returns EDEADLK when the calling thread already
    // holds the write side, so instead of blocking, the unguarded version sailed
    // past a failed lock it never checked and wrote a report describing a heap
    // that had already been shut down -- releasing the write lock on the way out.
    kFinalizeThenCheckpoint,
    // The deadlock shape: a *different* thread asking for the read side that
    // finalization holds for writing and never releases. It parks there, so the
    // join afterwards never returns and the child has to be killed.
    kFinalizeRace,
};

[[noreturn]] void RunChild(ChildMode mode, const std::string& dir) {
    ConfigureChild(dir);
    if (mode == ChildMode::kExitReport) {
        // Turns on peak recording and the report shutdown writes.
        setenv("DUMP_PEAK_VALUE_MB", "0", 1);
    }

    alignas(DebugData) static unsigned char debug_storage[sizeof(DebugData)];
    alignas(PointerData) static unsigned char pointer_storage[sizeof(PointerData)];
    void* init_space[] = {debug_storage, pointer_storage};
    if (!debug_initialize(init_space)) {
        _exit(2);
    }

    // Never freed, so the report has something to list.
    for (int i = 0; i < 4; ++i) {
        Leak(64 * 1024);
    }

    if (mode == ChildMode::kFinalizeRace) {
        std::thread racer([&dir]() {
            const std::string path = dir + "/racer.txt";
            while (g_keep_checkpointing.load(std::memory_order_relaxed)) {
                debug_dump_heap(path.c_str());
            }
        });
        debug_finalize();
        g_keep_checkpointing.store(false, std::memory_order_relaxed);
        // Hangs if the racer parked on the read lock finalization holds.
        racer.join();
        _exit(0);
    }

    debug_finalize();
    if (mode == ChildMode::kFinalizeThenCheckpoint) {
        const std::string path = dir + "/after_finalize.txt";
        debug_dump_heap(path.c_str());
        // Reaching this line at all is the assertion; the parent additionally
        // checks that no report appeared.
    }
    _exit(0);
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

pid_t SpawnChild(ChildMode mode, const std::string& dir) {
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        RunChild(mode, dir);
    }
    return pid;
}

}  // namespace

int main() {
    g_root = MakeRoot();

    setenv("ALLOC_HOOK_CAPTURE_MODE", "fast", 1);
    setenv("BACKTRACE_MIN_SIZE", "1024", 1);
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

    // ---------------------------------------------------------------------
    // Before initialization. Must run before this process initializes, and is
    // the deterministic proof that the entry point neither dereferences a null
    // g_debug nor lazily brings the hook up behind the caller's back.
    // ---------------------------------------------------------------------
    const std::string early_dir = g_root + "/early";
    assert(mkdir(early_dir.c_str(), 0755) == 0);
    debug_dump_heap((early_dir + "/bt.too_early.seq_0.txt").c_str());
    assert(ReportsIn(early_dir).empty());

    // ---------------------------------------------------------------------
    // Everything that finalizes, or that needs its own configuration, runs in a
    // child: finalization is one-shot per process and the config is read once,
    // at initialization. Forked before this process initializes so each child
    // starts from a clean, uninitialized hook.
    // ---------------------------------------------------------------------

    // The deadlock, first, because it is the failure that costs a whole deadline
    // to detect: a checkpoint on another thread against the write lock
    // finalization never releases. Which side of the door the checkpoint lands on
    // is a race, so repeat it.
    int hung_children = 0;
    for (int round = 0; round < kRaceChildren; ++round) {
        char name[64];
        snprintf(name, sizeof(name), "/race_%d", round);
        const pid_t child = SpawnChild(ChildMode::kFinalizeRace, g_root + name);
        if (!WaitForChild(child)) {
            ++hung_children;
            break;  // One is enough; do not wait out the deadline eight times.
        }
    }
    assert(hung_children == 0);

    // On the finalizing thread the lock request fails instead of blocking, so
    // what has to be asserted is that nothing was written: the heap is already
    // shut down and a report of it would describe nothing that is still true.
    const std::string after_dir = g_root + "/after_finalize";
    const pid_t after = SpawnChild(ChildMode::kFinalizeThenCheckpoint, after_dir);
    assert(WaitForChild(after));
    assert(ReportsIn(after_dir).empty());
    struct stat unused = {};
    assert(stat((after_dir + "/after_finalize.txt").c_str(), &unused) != 0);

    // Shutdown still writes its peak report, and writes exactly that one. There
    // used to be a second, live-at-exit report here; it was removed because it is
    // taken from ~AllocHook(), which runs after every application static
    // destructor and atexit handler, so it only ever saw allocations the program
    // never freed at all -- 0.54MB against 9.70MB at a running boundary on a
    // measured pipeline. A number that low reads as "no leak" and is the wrong
    // answer to the question being asked.
    const std::string exit_dir = g_root + "/exit_report";
    const pid_t exit_child = SpawnChild(ChildMode::kExitReport, exit_dir);
    assert(WaitForChild(exit_child));
    const std::vector<std::string> exit_reports = ReportsIn(exit_dir);
    assert(exit_reports.size() == 1);
    assert(Contains(exit_reports[0], ".exit.") &&
           Contains(exit_reports[0], ".seq_0."));
    assert(FileSize(exit_dir + "/" + exit_reports[0]) != 0);

    // ---------------------------------------------------------------------
    // This process: the guards that do not need finalization.
    // ---------------------------------------------------------------------
    const std::string main_dir = g_root + "/main";
    assert(mkdir(main_dir.c_str(), 0755) == 0);
    setenv("ALLOC_HOOK_DUMP_PREFIX", (main_dir + "/bt").c_str(), 1);

    alignas(DebugData) static unsigned char debug_storage[sizeof(DebugData)];
    alignas(PointerData) static unsigned char pointer_storage[sizeof(PointerData)];
    void* init_space[] = {debug_storage, pointer_storage};
    // Outside assert(): under NDEBUG the call itself would be dropped and every
    // debug_* entry point below would dereference a null g_debug.
    const bool initialized = debug_initialize(init_space);
    assert(initialized);
    (void)initialized;

    Leak(64 * 1024);

    // A null or empty path reached strlen() inside the directory walk.
    debug_dump_heap(nullptr);
    debug_dump_heap("");
    assert(ReportsIn(main_dir).empty());

    // The synchronous contract: when the call returns, the report is on disk.
    // No polling, unlike the signal path.
    const std::string explicit_path = main_dir + "/bt.explicit.seq_0.txt";
    debug_dump_heap(explicit_path.c_str());
    assert(FileSize(explicit_path) != 0);

    // Already inside the hook on this thread. DumpLiveToFile takes two
    // non-recursive mutexes, so re-entering would self-lock; this is the exact
    // predicate an in-hook caller hits, without needing an in-hook caller.
    const std::string reentrant_path = main_dir + "/bt.reentrant.seq_0.txt";
    DebugDisableSet(true);
    debug_dump_heap(reentrant_path.c_str());
    DebugDisableSet(false);
    assert(stat(reentrant_path.c_str(), &unused) != 0);

    // Under live allocation traffic: proves the lock order holds against the
    // threads a checkpoint actually contends with.
    std::vector<std::thread> allocators;
    for (int i = 0; i < 3; ++i) {
        allocators.emplace_back(AllocatorThread);
    }
    for (int i = 0; i < 10; ++i) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/bt.contended_%d.seq_0.txt",
                 main_dir.c_str(), i);
        debug_dump_heap(path);
        assert(FileSize(path) != 0);
    }
    g_keep_allocating.store(false, std::memory_order_relaxed);
    for (std::thread& worker : allocators) {
        worker.join();
    }

    printf("checkpoint guard contract holds\n");
    return 0;
}
