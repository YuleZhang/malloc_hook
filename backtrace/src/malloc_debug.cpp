#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/param.h>  // powerof2 ---> ((((x) - 1) & (x)) == 0)
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#ifndef MALLOC_HOOK_ENABLE_DMA_CAPTURE
#define MALLOC_HOOK_ENABLE_DMA_CAPTURE 0
#endif

#if MALLOC_HOOK_ENABLE_DMA_CAPTURE
#include "LinuxResourceBackend.h"
#endif

#include "AsyncStackPipeline.h"
#include "Config.h"
#include "DebugData.h"
#include "HookSourcePolicy.h"
#include "ObservedMemory.h"
#include "PointerData.h"
#include "debug_disable.h"
#include "malloc_debug.h"

#if MALLOC_HOOK_ENABLE_DMA_CAPTURE
#ifndef __user
#define __user
#define MALLOC_HOOK_DEFINED_KERNEL_USER_ANNOTATION
#endif
#include "midgard/mali_kbase_ioctl.h"
#include "msm_ksgl/msm_ksgl.h"
#include "mtk_camera/camera_mem.h"
#include "ion/ion_uapi.h"
#ifdef MALLOC_HOOK_DEFINED_KERNEL_USER_ANNOTATION
#undef MALLOC_HOOK_DEFINED_KERNEL_USER_ANNOTATION
#undef __user
#endif
#endif

#include "memory_hook.h"

class ScopedConcurrentLock {
public:
    ScopedConcurrentLock() { pthread_rwlock_rdlock(&lock_); }
    ~ScopedConcurrentLock() { pthread_rwlock_unlock(&lock_); }

    static void Init() {
        pthread_rwlockattr_t attr;
        pthread_rwlockattr_init(&attr);
        // Set the attribute so that when a write lock is pending, read locks are no
        // longer granted.
#if __ANDROID_API__ >= 23
        pthread_rwlockattr_setkind_np(
                &attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
        pthread_rwlock_init(&lock_, &attr);
        pthread_rwlockattr_destroy(&attr);
    }

    static void BlockAllOperations() { pthread_rwlock_wrlock(&lock_); }
    // Child-side fork recovery only. The child inherits the lock with the read
    // counts of threads fork() did not clone, so a later wrlock (finalization's
    // BlockAllOperations) would wait on readers that no longer exist. Only the
    // forking thread runs at this point, so re-initialising is safe and is the
    // only way to discard those phantom readers.
    static void ReinitAfterForkInChild() {
        pthread_rwlock_destroy(&lock_);
        Init();
    }

private:
    static pthread_rwlock_t lock_;
};
pthread_rwlock_t ScopedConcurrentLock::lock_;

DebugData* g_debug;

static int g_signal_pipe[2] = {-1, -1};
static pthread_t g_signal_thread;
static bool g_signal_thread_started = false;
static bool g_signal_debug_enabled = false;
// Checkpoint accounting. The signal handler may only do async-signal-safe work,
// so it takes responsibility for a request by bumping `requests` and waking the
// worker; whoever finishes with that request -- the worker after writing the
// report, or either side after deciding it cannot be honoured -- bumps
// `settled`. Finalization compares the two so a checkpoint asked for just before
// the process exits still produces its report.
static std::atomic<unsigned> g_checkpoint_requests{0};
static std::atomic<unsigned> g_checkpoint_settled{0};
static std::atomic<unsigned> g_checkpoint_dropped{0};
// Set once finalization has decided no further checkpoint can be served, and
// always before it blocks allocator operations. See DrainPendingCheckpoints().
static std::atomic<bool> g_checkpoints_closed{false};
// How many requests were already outstanding when finalization closed the door.
// Closing alone is not a usable admission rule: the worker often only reaches a
// request after finalization has started, and refusing it there would throw away
// the very checkpoint the run asked for at the end of its last iteration.
static std::atomic<unsigned> g_checkpoint_admit_upto{0};
static bool SignalDebugEnabled() {
    return g_signal_debug_enabled;
}

static void SignalDebugLog(const char* message) {
    if (SignalDebugEnabled()) {
        write(STDERR_FILENO, message, strlen(message));
    }
}

static void SignalDebugLogInt(const char* prefix, int value) {
    if (!SignalDebugEnabled()) {
        return;
    }
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "%s%d\n", prefix, value);
    write(STDERR_FILENO, buffer, strlen(buffer));
}

// Create the directories leading to `path`, like `mkdir -p`.
//
// The default dump prefix points into a directory that does not exist on a
// freshly flashed device. Without this, report generation fails at open() and
// the run produces memory counters but no report at all, with nothing in the
// log to say why.
static void EnsureParentDirectory(const char* path) {
    char buffer[PATH_MAX];
    const size_t length = strlen(path);
    if (length == 0 || length >= sizeof(buffer)) {
        return;
    }
    memcpy(buffer, path, length + 1);
    char* last_slash = strrchr(buffer, '/');
    if (last_slash == nullptr || last_slash == buffer) {
        // Relative name, or a file directly under "/": nothing to create.
        return;
    }
    *last_slash = '\0';
    // Walk the prefix creating each component; an already-existing component
    // reports EEXIST, which is the expected outcome and not an error.
    for (char* cursor = buffer + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        mkdir(buffer, 0755);
        *cursor = '/';
    }
    mkdir(buffer, 0755);
}

// Returns false when the report could not be written, so a caller can tell the
// difference between "no report because nothing to say" and "no report because
// the path was unusable".
static bool DumpHeapToFileUnlocked(const char* file_name, bool dump_peak) {
    ScopedDisableDebugCalls disable;

    EnsureParentDirectory(file_name);
    int fd = open(file_name, O_RDWR | O_CREAT | O_NOFOLLOW | O_TRUNC | O_CLOEXEC, 0644);
    if (fd == -1) {
        // Never fail silently here: a missing report is otherwise
        // indistinguishable from a hook that captured nothing.
        char message[PATH_MAX + 64];
        const int written = snprintf(
                message, sizeof(message),
                "alloc_hook: cannot write report to %s: %s\n", file_name,
                strerror(errno));
        if (written > 0) {
            write(STDERR_FILENO, message, static_cast<size_t>(written));
        }
        return false;
    }

    g_debug->pointer->DumpLiveToFile(fd, dump_peak);
    close(fd);
    return true;
}

static bool ShouldTrackAllocation(
        size_t requested_size, MemType type, size_t* tracked_size) {
    if (!g_debug->TrackPointers()) {
        *tracked_size = requested_size;
        return false;
    }
    return g_debug->pointer->ShouldTrackAllocation(requested_size, type, tracked_size);
}

static void singal_dump_heap(int) {
    if (g_signal_pipe[1] == -1) {
        g_checkpoint_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Counted before the write so that a worker which consumes the byte
    // immediately can never settle a request that has not been counted yet --
    // the drain below would otherwise conclude there is nothing to wait for.
    g_checkpoint_requests.fetch_add(1, std::memory_order_release);
    const char command = 'd';
    ssize_t bytes = write(g_signal_pipe[1], &command, sizeof(command));
    if (bytes == sizeof(command)) {
        SignalDebugLog("alloc_hook: signal handler queued dump\n");
        return;
    }
    // Nothing will consume this request, so settle it here or the drain would
    // wait out its whole timeout at exit.
    g_checkpoint_dropped.fetch_add(1, std::memory_order_relaxed);
    g_checkpoint_settled.fetch_add(1, std::memory_order_release);
    // Reported unconditionally: a checkpoint whose report never appears is
    // otherwise indistinguishable from one that was never asked for.
    static const char message[] =
            "alloc_hook: checkpoint dropped, dump worker queue is full\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
}

// Reports are named with a per-process sequence number as well as the wall
// clock. time() has one-second granularity, so two checkpoints inside the same
// second produced identical names and the second silently truncated the first --
// precisely the case a repeated-checkpoint leak comparison depends on. The
// sequence is shared with the exit report so one run's reports carry a single
// total order.
static unsigned NextReportSequence() {
    static std::atomic<unsigned> next{0};
    return next.fetch_add(1, std::memory_order_relaxed);
}

// Builds "<prefix>.<kind>.pid_<pid>.seq_<n>.time_<t>.txt". Returns false when the
// configured prefix is long enough to truncate the name, because a truncated
// name can drop the very field that keeps two reports apart.
//
// The sequence and wall clock are parameters so a deferred report can be named
// for the instant it was captured rather than the instant it was written.
static bool FormatReportNameAt(
        char* buffer, size_t size, const char* kind, unsigned sequence,
        long wall_time) {
    const int written = snprintf(
            buffer, size, "%s.%s.pid_%d.seq_%u.time_%ld.txt",
            g_debug->config().backtrace_dump_prefix(), kind, getpid(), sequence,
            wall_time);
    if (written > 0 && static_cast<size_t>(written) < size) {
        return true;
    }
    static const char message[] =
            "alloc_hook: cannot name report, ALLOC_HOOK_DUMP_PREFIX is too long\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    return false;
}

static bool FormatReportName(char* buffer, size_t size, const char* kind) {
    return FormatReportNameAt(
            buffer, size, kind, NextReportSequence(),
            static_cast<long>(time(NULL)));
}

// Snapshots the signal path captured, written out at shutdown.
//
// Deliberately leaked, like the rest of the hook's state: finalization runs
// while the C++ runtime is being torn down, and a static destructor racing that
// is how a diagnostic tool turns into a crash.
static std::vector<CheckpointSnapshot>& RetainedCheckpoints() {
    static std::vector<CheckpointSnapshot>* snapshots =
            new std::vector<CheckpointSnapshot>();
    return *snapshots;
}
static std::mutex g_checkpoint_snapshot_mutex;
// A bound, so a run that signals in a loop cannot grow this without limit. Each
// snapshot is a few hundred (stack, size) buckets, tens of KB.
static constexpr size_t kMaxRetainedCheckpoints = 64;

// Writes a live-allocation report, refusing with a diagnostic in every state
// where the write would crash, block forever, or produce nothing.
//
// This is the single implementation behind both ways in: the dump signal and the
// exported checkpoint() entry point. They used to share nothing, so only the
// signal path had any of these checks -- a direct call could dereference a null
// g_debug, run strlen() on a null path, or park forever on the allocator read
// lock after finalization took the write side for good.
//
// Exactly one of `kind` (report named by the hook) and `explicit_path` (name
// chosen by the caller) is used. `preclaimed_slot` is non-zero only for the
// signal worker: the signal handler already claimed that request's slot so the
// drain at exit would know about a byte still sitting in the pipe. Every other
// caller claims one here, which is what makes finalization wait for a direct
// call's report too.
static bool Checkpoint(
        const char* kind, const char* explicit_path, unsigned preclaimed_slot) {
    // Nothing below may run before the hook has state to report on. Checked
    // first because it is the only check that does not need g_debug itself.
    if (g_debug == nullptr) {
        static const char message[] =
                "alloc_hook: checkpoint ignored, the hook is not initialized\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
        return false;
    }
    if (!(g_debug->config().options() & BACKTRACE)) {
        static const char message[] =
                "alloc_hook: checkpoint ignored, stack capture is disabled\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
        return false;
    }

    const unsigned slot = preclaimed_slot != 0
            ? preclaimed_slot
            : g_checkpoint_requests.fetch_add(1, std::memory_order_acq_rel) + 1;
    bool written = false;

    // Checked before taking any lock. Finalization holds the allocator write
    // lock and never releases it, so a read lock taken after that point parks
    // the caller until the process dies -- the report would never appear and the
    // run would not say why. A request that was already claimed before the door
    // shut is still served; see DrainPendingCheckpoints().
    if (g_checkpoints_closed.load(std::memory_order_acquire) &&
        slot > g_checkpoint_admit_upto.load(std::memory_order_acquire)) {
        static const char message[] =
                "alloc_hook: checkpoint ignored, the process is already exiting\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
        g_checkpoint_dropped.fetch_add(1, std::memory_order_relaxed);
    } else if (DebugCallsDisabled()) {
        // Already inside the hook on this thread. DumpLiveToFile takes the two
        // tracker mutexes, both non-recursive, so re-entering would self-lock.
        static const char message[] =
                "alloc_hook: checkpoint ignored, this thread is already inside "
                "the hook\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
        g_checkpoint_dropped.fetch_add(1, std::memory_order_relaxed);
    } else {
        char generated[PATH_MAX];
        const char* path = explicit_path;
        bool have_path = false;
        if (kind != nullptr) {
            // The signal path. It must not generate the report here: doing so
            // holds both tracker mutexes for the whole of GetUniqueList's sort,
            // two /proc walks and one unbuffered dprintf per line, which
            // measured 1.2s with 60k live allocations -- with every allocation
            // in the process blocked behind it, perturbing the very thing the
            // checkpoint is measuring. Capture a snapshot, resolve it off the
            // locks, and let shutdown write it.
            ScopedDisableDebugCalls disable;
            CheckpointSnapshot snapshot;
            snapshot.sequence = NextReportSequence();
            snapshot.wall_time = static_cast<long>(time(NULL));
            g_debug->pointer->TakeCheckpointSnapshot(&snapshot);
            g_debug->pointer->ResolveCheckpointSnapshot(&snapshot);
            if (FormatReportNameAt(
                        generated, sizeof(generated), kind, snapshot.sequence,
                        snapshot.wall_time)) {
                snapshot.report_path = generated;
                std::lock_guard<std::mutex> guard(g_checkpoint_snapshot_mutex);
                std::vector<CheckpointSnapshot>& retained = RetainedCheckpoints();
                if (retained.size() < kMaxRetainedCheckpoints) {
                    retained.push_back(std::move(snapshot));
                    written = true;
                } else {
                    static const char message[] =
                            "alloc_hook: checkpoint dropped, too many retained "
                            "snapshots\n";
                    write(STDERR_FILENO, message, sizeof(message) - 1);
                }
            }
        } else if (explicit_path != nullptr && explicit_path[0] != '\0') {
            // The exported checkpoint() entry point. Stays synchronous: the
            // caller named a file and expects it to exist when the call
            // returns, and having asked for it explicitly is having opted into
            // the cost.
            path = explicit_path;
            have_path = true;
        } else {
            // A null or empty path would reach strlen() inside
            // EnsureParentDirectory.
            static const char message[] =
                    "alloc_hook: checkpoint ignored, no report path given\n";
            write(STDERR_FILENO, message, sizeof(message) - 1);
        }
        if (have_path) {
            ScopedConcurrentLock lock;
            written = DumpHeapToFileUnlocked(path, false);
        }
        if (!written) {
            g_checkpoint_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // On every path, or the drain at exit would wait out its whole timeout.
    g_checkpoint_settled.fetch_add(1, std::memory_order_release);
    return written;
}

static void* signal_dump_thread(void*) {
    sigset_t blocked_signals;
    sigemptyset(&blocked_signals);
    sigaddset(&blocked_signals, g_debug->config().backtrace_dump_signal());
    pthread_sigmask(SIG_BLOCK, &blocked_signals, nullptr);

    unsigned consumed = 0;
    while (true) {
        char command;
        ssize_t bytes = read(g_signal_pipe[0], &command, sizeof(command));
        if (bytes <= 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        // 1-based position of this request in the queue. Local because this is
        // the only consumer, and passed to Checkpoint() so that "already asked
        // for" is decided by when the signal arrived, not by when this thread
        // happened to get scheduled.
        ++consumed;
        if (command != 'd') {
            // Still settles: every consumed byte settles exactly one request,
            // whatever the outcome, so the drain at exit always terminates.
            g_checkpoint_settled.fetch_add(1, std::memory_order_release);
            continue;
        }
        SignalDebugLog("alloc_hook: signal thread taking checkpoint snapshot\n");
        Checkpoint("signal", nullptr, consumed);
        SignalDebugLog("alloc_hook: signal thread finished snapshot\n");
    }
    return nullptr;
}

// Writes every retained checkpoint snapshot. Called at shutdown, once
// allocations are already blocked, so the formatting costs the run nothing.
static void WriteRetainedCheckpoints() {
    std::lock_guard<std::mutex> guard(g_checkpoint_snapshot_mutex);
    for (CheckpointSnapshot& snapshot : RetainedCheckpoints()) {
        EnsureParentDirectory(snapshot.report_path.c_str());
        const int fd = open(
                snapshot.report_path.c_str(),
                O_RDWR | O_CREAT | O_NOFOLLOW | O_TRUNC | O_CLOEXEC, 0644);
        if (fd == -1) {
            char message[PATH_MAX + 64];
            const int written = snprintf(
                    message, sizeof(message),
                    "alloc_hook: cannot write checkpoint report to %s: %s\n",
                    snapshot.report_path.c_str(), strerror(errno));
            if (written > 0) {
                write(STDERR_FILENO, message, static_cast<size_t>(written));
            }
            continue;
        }
        g_debug->pointer->WriteCheckpointSnapshot(fd, snapshot);
        close(fd);
    }
}

// Lets every checkpoint that was already asked for finish before finalization
// shuts the allocator down.
//
// The admission bound is published before the closed flag, so the worker can
// only ever read a bound that is at least as large as the set of requests that
// existed before the door shut: a request the run had already made is served,
// and one that arrives afterwards settles without reaching for a lock nobody
// will release. Bounded, because a wedged worker must not be able to stop the
// process from exiting.
static void DrainPendingCheckpoints() {
    g_checkpoint_admit_upto.store(
            g_checkpoint_requests.load(std::memory_order_acquire),
            std::memory_order_release);
    g_checkpoints_closed.store(true, std::memory_order_release);
    // No early return on a missing signal worker: a direct checkpoint() call
    // takes a slot in the same accounting, so its report can be in flight here
    // whether or not the dump signal was ever configured.
    constexpr unsigned kTimeoutMs = 5000;
    constexpr unsigned kPollUs = 1000;
    for (unsigned waited_ms = 0; waited_ms < kTimeoutMs; ++waited_ms) {
        if (g_checkpoint_settled.load(std::memory_order_acquire) >=
            g_checkpoint_requests.load(std::memory_order_acquire)) {
            return;
        }
        usleep(kPollUs);
    }
    char message[128];
    const int written = snprintf(
            message, sizeof(message),
            "alloc_hook: gave up waiting for %u checkpoint report(s) after %ums\n",
            g_checkpoint_requests.load(std::memory_order_acquire) -
                    g_checkpoint_settled.load(std::memory_order_acquire),
            kTimeoutMs);
    if (written > 0) {
        write(STDERR_FILENO, message, static_cast<size_t>(written));
    }
}

static bool StartSignalDumpThread() {
    g_signal_debug_enabled = getenv("ALLOC_HOOK_DEBUG") != nullptr;
    if (g_signal_thread_started) {
        return true;
    }
    if (pipe(g_signal_pipe) != 0) {
        g_signal_pipe[0] = -1;
        g_signal_pipe[1] = -1;
        return false;
    }
    fcntl(g_signal_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_signal_pipe[1], F_SETFD, FD_CLOEXEC);
    int flags = fcntl(g_signal_pipe[1], F_GETFL, 0);
    if (flags != -1) {
        fcntl(g_signal_pipe[1], F_SETFL, flags | O_NONBLOCK);
    }
    if (pthread_create(&g_signal_thread, nullptr, signal_dump_thread, nullptr) != 0) {
        close(g_signal_pipe[0]);
        close(g_signal_pipe[1]);
        g_signal_pipe[0] = -1;
        g_signal_pipe[1] = -1;
        return false;
    }
    pthread_detach(g_signal_thread);
    g_signal_thread_started = true;
    SignalDebugLog("alloc_hook: signal dump thread started\n");
    return true;
}

// Relays a new evaluator-visible peak to the allocation bookkeeping, which
// snapshots the live stacks at that instant.
static void OnObservedMemoryPeak(const ObservedMemSample& sample) {
    if (g_debug != nullptr && g_debug->TrackPointers()) {
        g_debug->pointer->RecordObservedPeak(sample);
    }
}

// Starts sampling this process's real footprint, so the peak snapshot lands at
// the instant an external evaluator calls the peak rather than at the instant
// tracked bytes happen to top out. Off unless something is sampling the process
// (see Config::observed_peak_sample_ms) -- with nothing to align to, the extra
// thread would cost the run without making any report more accurate.
static void StartObservedPeakSampler() {
    if (!(g_debug->config().options() & RECORD_MEMORY_PEAK)) {
        return;
    }
    const unsigned interval_ms = g_debug->config().observed_peak_sample_ms();
    if (interval_ms == 0) {
        return;
    }
    const bool started = ObservedPeakSamplerInstance().Start(
            interval_ms, g_debug->config().backtrace_dump_peak_val(),
            g_debug->config().peak_record_step_bytes(), &OnObservedMemoryPeak);
    if (!started) {
        // Not fatal: the allocation-path criterion still produces a snapshot,
        // and the report says which criterion produced the one it retained.
        static const char message[] =
                "alloc_hook: could not start the memory peak sampler; peak "
                "snapshots fall back to tracked allocation bytes\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
    } else {
        SignalDebugLogInt(
                "alloc_hook: peak sampler started, interval ms ",
                static_cast<int>(interval_ms));
    }
}

// fork() clones only the calling thread. Any hook lock another thread held at
// fork time is inherited *locked* by the child, where its owner does not exist
// to release it, so the child's first tracked allocation would block forever.
//
// The barrier deliberately takes only the two tracker mutexes, in the same
// pointer -> frame order the tracker itself uses. It must not take the rwlock's
// write side: PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP is only applied on
// Android, so on a reader-preferring glibc a writer starves indefinitely
// against threads that allocate in a loop -- the fork itself would hang. The
// rwlock is instead discarded and rebuilt in the child, which removes the
// phantom read counts of the threads fork() did not clone.
static void HookAtForkPrepare() {
    if (g_debug != nullptr && g_debug->pointer != nullptr) {
        g_debug->pointer->LockForFork();
    }
}

static void HookAtForkParent() {
    if (g_debug != nullptr && g_debug->pointer != nullptr) {
        g_debug->pointer->UnlockAfterFork();
    }
}

static void HookAtForkChild() {
    if (g_debug != nullptr && g_debug->pointer != nullptr) {
        g_debug->pointer->UnlockAfterFork();
    }
    ScopedConcurrentLock::ReinitAfterForkInChild();
    // Both helper threads are gone. Clearing their bookkeeping keeps the child
    // from joining thread ids that were never valid in this process; the child
    // keeps accounting allocations, it just has no signal dump or sampler until
    // something restarts them.
    g_signal_thread_started = false;
    // A request the parent had in flight at fork time has no worker here to
    // settle it, so the child would otherwise wait out the whole drain timeout
    // at exit for a report that can never be written.
    g_checkpoint_requests.store(0, std::memory_order_relaxed);
    g_checkpoint_settled.store(0, std::memory_order_relaxed);
    g_checkpoint_dropped.store(0, std::memory_order_relaxed);
    g_checkpoint_admit_upto.store(0, std::memory_order_relaxed);
    ObservedPeakSamplerInstance().ResetAfterForkInChild();
}

static void RegisterForkHandlers() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    pthread_atfork(HookAtForkPrepare, HookAtForkParent, HookAtForkChild);
}

bool debug_initialize(void* init_space[]) {
    if (!DebugDisableInitialize()) {
        return false;
    }

    DebugData* debug = new (init_space[0]) DebugData();
    if (!debug->Initialize(init_space[1])) {
        DebugDisableFinalize();
        return false;
    }
    g_debug = debug;

    ScopedConcurrentLock::Init();

    // Registered after the lock exists and before any helper thread starts, so
    // there is no window in which a fork could find a half-built barrier.
    RegisterForkHandlers();

    if (g_debug->config().options() & DUMP_ON_SINGAL) {
        if (!StartSignalDumpThread()) {
            return false;
        }
        struct sigaction enable_act = {};
        enable_act.sa_handler = singal_dump_heap;
        sigemptyset(&enable_act.sa_mask);
        enable_act.sa_flags = SA_RESTART | SA_ONSTACK;
        SignalDebugLogInt(
                "alloc_hook: installing dump signal ",
                g_debug->config().backtrace_dump_signal());
        if (sigaction(
                    g_debug->config().backtrace_dump_signal(), &enable_act, nullptr) !=
            0) {
            SignalDebugLog("alloc_hook: failed to install dump signal\n");
            return false;
        }
    }

    StartObservedPeakSampler();

    return true;
}

void debug_finalize() {
    if (g_debug == nullptr) {
        return;
    }

    // Stopped before anything else: the sampler thread takes the allocation
    // locks to snapshot stacks, and the next step blocks every allocator
    // operation in the process. Stop() joins, so no snapshot can be in flight
    // once it returns.
    ObservedPeakSamplerInstance().Stop();

    // Before anything narrows what the allocator may do: a checkpoint asked for
    // at the end of the last iteration of a run has to still produce its report,
    // and this is the last point at which the worker can take the locks it needs.
    DrainPendingCheckpoints();

    // Make sure that there are no other threads doing debug allocations
    // before we kill everything.
    ScopedConcurrentLock::BlockAllOperations();

    // Turn off capturing allocations calls.
    DebugDisableSet(true);

    // The checkpoints this run asked for. They were captured when the signal
    // arrived and are only written now: formatting is the expensive half, and
    // doing it here costs the run nothing because the process is already on its
    // way out. Each file is still named for the instant it was captured.
    WriteRetainedCheckpoints();

    if ((g_debug->config().options() & BACKTRACE) &&
        g_debug->config().backtrace_dump_on_exit()) {
        // The PID is part of the name so a report can be tied back to the
        // process that produced it; a hook injected process-wide may write
        // several. The sequence number continues the one the checkpoint reports
        // use, so the exit report sorts after them.
        char file_name[PATH_MAX];
        if (FormatReportName(file_name, sizeof(file_name), "exit")) {
            DumpHeapToFileUnlocked(file_name, true);
        }
    }

    if (g_debug->TrackPointers()) {
        g_debug->pointer->DumpPeakInfo();
    }

    // 对于调试工具或在调试模式下运行的代码, 资源管理可能不是首要关注点.
    // 为了避免在清理过程中出现多线程访问冲突, 决定故意不释放这些资源. 包括
    // g_debug、pthread 键等.
}

void debug_dump_heap(const char* file_name) {
    // Every guard lives in Checkpoint(), so the exported checkpoint() entry
    // point is exactly as safe as the signal path rather than being the one way
    // in with no checks at all.
    Checkpoint(nullptr, file_name, 0);
}

static void* InternalMalloc(size_t requested_size, size_t tracked_size) {
    void* result = m_sys_malloc(requested_size);
    if (hook_source::AllocationSucceeded(result) && g_debug->TrackPointers()) {
        g_debug->pointer->Add(result, requested_size, tracked_size);
    }

    return result;
}

static void* SystemMallocNoHook(size_t size) {
    ScopedDisableDebugCalls disable;
    return m_sys_malloc(size);
}

static void SystemFreeNoHook(void* pointer) {
    ScopedDisableDebugCalls disable;
    m_sys_free(pointer);
}

static void* SystemCallocNoHook(size_t nmemb, size_t size) {
    ScopedDisableDebugCalls disable;
    return m_sys_calloc(nmemb, size);
}

static void* SystemMemalignNoHook(size_t alignment, size_t size) {
    ScopedDisableDebugCalls disable;
    return m_sys_memalign(alignment, size);
}

static void* SystemAlignedAllocNoHook(size_t alignment, size_t size) {
    ScopedDisableDebugCalls disable;
    return m_sys_aligned_alloc(alignment, size);
}

static int SystemPosixMemalignNoHook(void** pointer, size_t alignment, size_t size) {
    ScopedDisableDebugCalls disable;
    return m_sys_posix_memalign(pointer, alignment, size);
}

static void* SystemReallocNoHook(void* pointer, size_t size) {
    ScopedDisableDebugCalls disable;
    return m_sys_realloc(pointer, size);
}

static bool DebugCallsDisabledOrAsyncWorker() {
    return DebugCallsDisabled() || AsyncStackWorkerThread();
}

static void InternalFree(void* pointer) {
    if (g_debug->TrackPointers()) {
        g_debug->pointer->Remove(pointer);
    }
    m_sys_free(pointer);
}

void* debug_malloc(size_t size) {
    if (DebugCallsDisabledOrAsyncWorker()) {
        return m_sys_malloc(size);
    }

    if (size > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    size_t tracked_size = size;
    if (!ShouldTrackAllocation(size, HOST, &tracked_size)) {
        return SystemMallocNoHook(size);
    }
    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;
    return InternalMalloc(size, tracked_size);
}

void debug_free(void* pointer) {
    if (DebugCallsDisabledOrAsyncWorker() || pointer == nullptr) {
        return m_sys_free(pointer);
    }

    if (g_debug->config().sampling_enabled() &&
        !g_debug->pointer->MightContain(pointer)) {
        return SystemFreeNoHook(pointer);
    }
    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    InternalFree(pointer);
}

void* debug_realloc(void* pointer, size_t bytes) {
    if (DebugCallsDisabledOrAsyncWorker()) {
        return m_sys_realloc(pointer, bytes);
    }

    if (pointer == nullptr) {
        if (bytes > PointerInfoType::MaxSize()) {
            errno = ENOMEM;
            return nullptr;
        }
        size_t tracked_size = bytes;
        if (!ShouldTrackAllocation(bytes, HOST, &tracked_size)) {
            return SystemReallocNoHook(nullptr, bytes);
        }
        ScopedConcurrentLock lock;
        ScopedDisableDebugCalls disable;
        return InternalMalloc(bytes, tracked_size);
    }

    if (bytes == 0) {
        ScopedConcurrentLock lock;
        ScopedDisableDebugCalls disable;
        InternalFree(pointer);
        return nullptr;
    }

    if (bytes > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    size_t tracked_size = bytes;
    bool track_new_allocation = ShouldTrackAllocation(bytes, HOST, &tracked_size);

    // Detach the old record *before* the allocator can release the block. If we
    // waited until after m_sys_realloc(), a concurrent malloc on another thread
    // could be handed the freed address and register it, and our removal would
    // erase that thread's live record instead of ours.
    PointerInfoType previous{};
    const bool had_entry =
            g_debug->TrackPointers() && g_debug->pointer->TakeEntry(pointer, &previous);

    void* new_pointer = m_sys_realloc(pointer, bytes);
    if (!hook_source::AllocationSucceeded(new_pointer)) {
        // realloc() failed, so the original block is still live and must stay
        // tracked exactly as it was.
        if (had_entry) {
            g_debug->pointer->RestoreEntry(pointer, previous);
        }
        return new_pointer;
    }

    if (track_new_allocation && g_debug->TrackPointers()) {
        g_debug->pointer->Add(new_pointer, bytes, tracked_size);
    }
    if (had_entry) {
        // Released after the new Add() so an unchanged call site keeps its
        // frame entry alive instead of being dropped and re-symbolized.
        g_debug->pointer->RemoveBacktrace(previous.hash_index);
    }

    return new_pointer;
}

void* debug_calloc(size_t nmemb, size_t bytes) {
    if (DebugCallsDisabledOrAsyncWorker()) {
        return m_sys_calloc(nmemb, bytes);
    }

    size_t size;
    if (__builtin_mul_overflow(nmemb, bytes, &size)) {
        // Overflow
        errno = ENOMEM;
        return nullptr;
    }

    size_t tracked_size = size;
    if (!ShouldTrackAllocation(size, HOST, &tracked_size)) {
        return SystemCallocNoHook(nmemb, bytes);
    }
    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;
    void* pointer = m_sys_calloc(nmemb, bytes);
    if (pointer != nullptr && g_debug->TrackPointers()) {
        g_debug->pointer->Add(pointer, size, tracked_size);
    }

    return pointer;
}

void* debug_memalign(size_t alignment, size_t bytes) {
    if (DebugCallsDisabledOrAsyncWorker()) {
        return m_sys_memalign(alignment, bytes);
    }

    if (bytes > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    size_t tracked_size = bytes;
    if (!ShouldTrackAllocation(bytes, HOST, &tracked_size)) {
        return SystemMemalignNoHook(alignment, bytes);
    }
    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;
    void* pointer = m_sys_memalign(alignment, bytes);

    if (pointer != nullptr && g_debug->TrackPointers()) {
        g_debug->pointer->Add(pointer, bytes, tracked_size);
    }

    return pointer;
}

void* debug_aligned_alloc(size_t alignment, size_t bytes) {
    if (DebugCallsDisabledOrAsyncWorker()) {
        return m_sys_aligned_alloc(alignment, bytes);
    }

    if (bytes > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    size_t tracked_size = bytes;
    if (!ShouldTrackAllocation(bytes, HOST, &tracked_size)) {
        return SystemAlignedAllocNoHook(alignment, bytes);
    }
    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;
    void* pointer = m_sys_aligned_alloc(alignment, bytes);
    if (pointer != nullptr && g_debug->TrackPointers()) {
        g_debug->pointer->Add(pointer, bytes, tracked_size);
    }

    return pointer;
}

int debug_posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (DebugCallsDisabledOrAsyncWorker()) {
        return m_sys_posix_memalign(memptr, alignment, size);
    }

    if (alignment < sizeof(void*) || !powerof2(alignment)) {
        return EINVAL;
    }
    if (size > PointerInfoType::MaxSize()) {
        return ENOMEM;
    }

    size_t tracked_size = size;
    if (!ShouldTrackAllocation(size, HOST, &tracked_size)) {
        return SystemPosixMemalignNoHook(memptr, alignment, size);
    }
    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;
    const int result = m_sys_posix_memalign(memptr, alignment, size);
    if (result == 0 && *memptr != nullptr && g_debug->TrackPointers()) {
        g_debug->pointer->Add(*memptr, size, tracked_size);
    }
    return result;
}

#if MALLOC_HOOK_ENABLE_DMA_CAPTURE
namespace DMA_BUF {

static thread_local hook_source::PendingIoctlAllocation pending_gpu_allocation;
struct PendingIonAllocation {
    size_t size = 0;
};

struct DmaFdInfo {
    size_t size = 0;
    bool tracked = false;
};

static std::mutex state_mutex;
static std::unordered_map<uint64_t, PendingIonAllocation> pending_ion_allocations;
static std::unordered_map<int, DmaFdInfo> dma_fds;

static uint64_t IonHandleKey(int ion_fd, ion_user_handle_t handle) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(ion_fd)) << 32) |
           static_cast<uint32_t>(handle);
}

static bool IsIonRequest(unsigned long request, unsigned int nr, unsigned int size) {
    return _IOC_TYPE(request) == ALLOC_HOOK_ION_IOC_MAGIC &&
           _IOC_NR(request) == nr && _IOC_SIZE(request) == size;
}

static bool IsIonLegacyAlloc(unsigned long request) {
    return IsIonRequest(request, 0, sizeof(struct ion_allocation_data));
}

static bool IsIonNewAlloc(unsigned long request) {
    return IsIonRequest(request, 0, sizeof(struct ion_new_allocation_data));
}

static bool IsIonFdRequest(unsigned long request, unsigned int nr) {
    return IsIonRequest(request, nr, sizeof(struct ion_fd_data));
}

static bool IsIonFree(unsigned long request) {
    return IsIonRequest(request, 1, sizeof(struct ion_handle_data));
}

static void ForgetDmaFd(int fd) {
    std::lock_guard<std::mutex> guard(state_mutex);
    dma_fds.erase(fd);
}

static bool RegisterDmaFd(int fd, size_t size, bool* should_track) {
    if (fd < 0 || size == 0) {
        return false;
    }
    std::lock_guard<std::mutex> guard(state_mutex);
    auto [entry, inserted] = dma_fds.emplace(fd, DmaFdInfo{size, false});
    *should_track = inserted || !entry->second.tracked;
    entry->second.size = size;
    return true;
}

static bool LookupDmaFd(int fd, size_t* size, bool* tracked = nullptr) {
    std::lock_guard<std::mutex> guard(state_mutex);
    auto entry = dma_fds.find(fd);
    if (entry == dma_fds.end()) {
        return false;
    }
    *size = entry->second.size;
    if (tracked != nullptr) {
        *tracked = entry->second.tracked;
    }
    return true;
}

static void MarkDmaFdTracked(int fd) {
    std::lock_guard<std::mutex> guard(state_mutex);
    auto entry = dma_fds.find(fd);
    if (entry != dma_fds.end()) {
        entry->second.tracked = true;
    }
}

static bool TakeDmaFd(int fd, size_t* size, bool* tracked) {
    std::lock_guard<std::mutex> guard(state_mutex);
    auto entry = dma_fds.find(fd);
    if (entry == dma_fds.end()) {
        return false;
    }
    *size = entry->second.size;
    *tracked = entry->second.tracked;
    dma_fds.erase(entry);
    return true;
}

static void RecordIonAllocation(int ion_fd, ion_user_handle_t handle, size_t size) {
    if (size == 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(state_mutex);
    pending_ion_allocations[IonHandleKey(ion_fd, handle)] = PendingIonAllocation{size};
}

static size_t LookupIonAllocation(int ion_fd, ion_user_handle_t handle) {
    std::lock_guard<std::mutex> guard(state_mutex);
    auto key = IonHandleKey(ion_fd, handle);
    auto entry = pending_ion_allocations.find(key);
    if (entry == pending_ion_allocations.end()) {
        return 0;
    }
    return entry->second.size;
}

static void DropIonAllocation(int ion_fd, ion_user_handle_t handle) {
    std::lock_guard<std::mutex> guard(state_mutex);
    pending_ion_allocations.erase(IonHandleKey(ion_fd, handle));
}

static void DropIonAllocationsForFd(int ion_fd) {
    std::lock_guard<std::mutex> guard(state_mutex);
    const uint32_t fd_key = static_cast<uint32_t>(ion_fd);
    for (auto entry = pending_ion_allocations.begin();
         entry != pending_ion_allocations.end();) {
        if (static_cast<uint32_t>(entry->first >> 32) == fd_key) {
            entry = pending_ion_allocations.erase(entry);
        } else {
            ++entry;
        }
    }
}

static bool is_dma_buf(int fd, size_t* size) {
    if (LookupDmaFd(fd, size)) {
        return true;
    }
    std::string fdinfo = "/proc/self/fdinfo/" + std::to_string(fd);
    auto fp = std::unique_ptr<FILE, decltype(&fclose)>{fopen(fdinfo.c_str(), "re"), fclose};
    if (fp == nullptr) {
        return false;
    }

    bool is_dmabuf_file = false;
    uint64_t inode = -1;
    char* line = nullptr;
    size_t len = 0;
    while (getline(&line, &len, fp.get()) > 0) {
        switch (line[0]) {
            case 'i':
                if (strncmp(line, "ino:", 4) == 0) {
                    char* c = line + 4;
                    inode = strtoull(c, nullptr, 10);
                }
                break;
            case 's':
                if (strncmp(line, "size:", 5) == 0) {
                    char* c = line + 5;
                    *size = strtoull(c, nullptr, 10);
                }
                break;
            case 'e':
                if (strncmp(line, "exp_name:", 9) == 0) {
                    is_dmabuf_file = true;
                }
                break;
            default:
                break;
        }
    }
    m_sys_free(line);

    if (!is_dmabuf_file) {
        return false;
    }

    if (inode == static_cast<uint64_t>(-1)) {
        // Fallback to stat() on the fd path to get inode number
        std::string fd_path = "/proc/self/fd/" + std::to_string(fd);

        struct stat sb;
        if (stat(fd_path.c_str(), &sb) < 0) {
            return false;
        }
        inode = sb.st_ino;
    }

    return *size > 0;
}

static void IonPathLog(const char* tag, unsigned long request, size_t sz) {
    static bool enabled = getenv("ALLOC_HOOK_DEBUG") != nullptr;
    if (!enabled) {
        return;
    }
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "alloc_hook_ion: %s nr=%u size=%zu\n",
                     tag, static_cast<unsigned>(_IOC_NR(request)), sz);
    if (n > 0) {
        write(STDERR_FILENO, buf, static_cast<size_t>(n));
    }
}

static bool handle_dma_node(unsigned long request, void* arg, int* fd, size_t* size) {
    // delay parsing the backtrace until mmap64.
    auto set_pending_gpu_allocation = [request]() -> bool {
        pending_gpu_allocation.Mark(request);
        return false;
    };

    switch (request) {
        case KBASE_IOCTL_MEM_ALLOC:
        case KBASE_IOCTL_MEM_ALLOC_EX:
        case IOCTL_KGSL_GPUOBJ_ALLOC:
            return set_pending_gpu_allocation();
        // parse the backtrace immediately
        case DMA_HEAP_IOCTL_ALLOC: {
                // A driver that accepts this command with a null argument would
                // otherwise fault inside the hook. HandleIonIoctl guards the
                // same way; the GPU cases above never dereference `arg`.
                if (arg == nullptr) {
                    return false;
                }
                struct dma_heap_allocation_data* heap = (struct dma_heap_allocation_data*)arg;
                *fd = heap->fd;
                IonPathLog("DMA_HEAP", request, static_cast<size_t>(heap->len));
                // The successful allocation ioctl already returns the exact
                // requested buffer length.  Use it on every platform instead
                // of depending on optional /proc fdinfo fields (which vary
                // across Android kernels and are absent on OHOS).
                *size = static_cast<size_t>(heap->len);
                return *fd >= 0 && *size > 0;
            }
        case CAM_MEM_ION_MAP_PA: {
                if (arg == nullptr) {
                    return false;
                }
                struct CAM_MEM_DEV_ION_NODE_STRUCT* heap = (struct CAM_MEM_DEV_ION_NODE_STRUCT*)arg;
                *fd = heap->memID;
            }
            return is_dma_buf(*fd, size);
        default:
            return false;
    }
}

static bool HandleIonIoctl(
        int ion_fd, unsigned long request, void* arg, int* dma_fd, size_t* size) {
    if (arg == nullptr || _IOC_TYPE(request) != ALLOC_HOOK_ION_IOC_MAGIC) {
        return false;
    }
    if (IsIonLegacyAlloc(request)) {
        auto* allocation = static_cast<struct ion_allocation_data*>(arg);
        IonPathLog("ION_LEGACY_ALLOC", request, allocation->len);
        RecordIonAllocation(ion_fd, allocation->handle, allocation->len);
        return false;
    }
    if (IsIonNewAlloc(request)) {
        auto* allocation = static_cast<struct ion_new_allocation_data*>(arg);
        IonPathLog("ION_MODERN_ALLOC", request, static_cast<size_t>(allocation->len));
        *dma_fd = static_cast<int>(allocation->fd);
        *size = static_cast<size_t>(allocation->len);
        return *dma_fd >= 0 && *size > 0;
    }
    if (IsIonFdRequest(request, 2) || IsIonFdRequest(request, 4)) {
        auto* fd_data = static_cast<struct ion_fd_data*>(arg);
        *dma_fd = fd_data->fd;
        *size = LookupIonAllocation(ion_fd, fd_data->handle);
        IonPathLog("ION_MAP_SHARE", request, *size);
        return *dma_fd >= 0 && *size > 0;
    }
    if (IsIonFdRequest(request, 5)) {
        return false;
    }
    if (IsIonFree(request)) {
        auto* handle_data = static_cast<struct ion_handle_data*>(arg);
        DropIonAllocation(ion_fd, handle_data->handle);
    }
    return false;
}

static bool ShouldTrackDmaMapping(int fd, int flags, size_t* size) {
    if (fd < 0 || !(flags & MAP_SHARED)) {
        return false;
    }
    bool tracked = false;
    if (LookupDmaFd(fd, size, &tracked)) {
        return !tracked;
    }
    if (!is_dma_buf(fd, size)) {
        return false;
    }
    bool should_track = false;
    return RegisterDmaFd(fd, *size, &should_track) && should_track;
}

}  // namespace DMA_BUF
#endif

static void* CallMmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (m_sys_mmap != nullptr) {
        return m_sys_mmap(addr, size, prot, flags, fd, offset);
    }
    return reinterpret_cast<void*>(syscall(SYS_mmap, addr, size, prot, flags, fd, offset));
}

static int CallMunmap(void* addr, size_t size) {
    if (m_sys_munmap != nullptr) {
        return m_sys_munmap(addr, size);
    }
    return static_cast<int>(syscall(SYS_munmap, addr, size));
}

// Older glibc/NDK/OHOS sysroots predate this flag but the running kernel may
// still honour it, so match on the kernel's ABI value rather than on whether
// the build headers happen to declare it.
#if defined(MREMAP_DONTUNMAP)
#define ALLOC_HOOK_MREMAP_DONTUNMAP MREMAP_DONTUNMAP
#else
#define ALLOC_HOOK_MREMAP_DONTUNMAP 4
#endif

static void* CallMremap(
        void* old_addr, size_t old_size, size_t new_size, int flags, void* new_addr) {
    if (m_sys_mremap != nullptr) {
        if ((flags & MREMAP_FIXED) != 0) {
            return m_sys_mremap(old_addr, old_size, new_size, flags, new_addr);
        }
        return m_sys_mremap(old_addr, old_size, new_size, flags);
    }
#if defined(SYS_mremap)
    if ((flags & MREMAP_FIXED) != 0) {
        return reinterpret_cast<void*>(
                syscall(SYS_mremap, old_addr, old_size, new_size, flags, new_addr));
    }
    return reinterpret_cast<void*>(
            syscall(SYS_mremap, old_addr, old_size, new_size, flags));
#else
    errno = ENOSYS;
    return MAP_FAILED;
#endif
}

#if !defined(mmap64)
static void* CallMmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (m_sys_mmap64 != nullptr) {
        return m_sys_mmap64(addr, size, prot, flags, fd, offset);
    }
    return CallMmap(addr, size, prot, flags, fd, offset);
}
#endif

int debug_ioctl(int fd, unsigned long request, void* arg) {
#if !MALLOC_HOOK_ENABLE_DMA_CAPTURE
    return (int)syscall(SYS_ioctl, fd, request, arg);
#else
    if (DebugCallsDisabledOrAsyncWorker()) {
        return (int)syscall(SYS_ioctl, fd, request, arg);
    }

    ScopedDisableDebugCalls disable;

    // Avoid holding the global hook lock while the ioctl blocks in vendor code.
    int ret = (int)syscall(SYS_ioctl, fd, request, arg);

    int node_fd = -1;
    size_t node_sz = 0;
    if (hook_source::SyscallSucceeded(ret) && g_debug->TrackPointers()) {
        // ioctl request numbers are 32-bit values even when the backend API
        // stores them in unsigned long.  Keep the low 32 bits so Android's
        // signed-int libc prototype cannot sign-extend _IOWR requests and
        // defeat the command match.
        const unsigned int normalized_request =
                static_cast<unsigned int>(request);
        const bool recognized =
                DMA_BUF::HandleIonIoctl(
                        fd, normalized_request, arg, &node_fd, &node_sz) ||
                DMA_BUF::handle_dma_node(
                        normalized_request, arg, &node_fd, &node_sz);
        if (recognized) {
            bool should_track = false;
            if (DMA_BUF::RegisterDmaFd(node_fd, node_sz, &should_track) &&
                should_track) {
                ScopedConcurrentLock lock;
                void* ptr = reinterpret_cast<void*>(node_fd);
                g_debug->pointer->Add(ptr, node_sz, node_sz, DMA);
                DMA_BUF::MarkDmaFdTracked(node_fd);
            }
        }
    }

    return ret;
#endif
}

int debug_close(int fd) {
#if !MALLOC_HOOK_ENABLE_DMA_CAPTURE
    return (int)syscall(SYS_close, fd);
#else
    if (DebugCallsDisabledOrAsyncWorker()) {
        return (int)syscall(SYS_close, fd);
    }

    ScopedDisableDebugCalls disable;

    int ret = (int)syscall(SYS_close, fd);
    if (hook_source::SyscallSucceeded(ret) && g_debug->TrackPointers()) {
        size_t size = 0;
        bool tracked = false;
        if (DMA_BUF::TakeDmaFd(fd, &size, &tracked) && tracked) {
            ScopedConcurrentLock lock;
            void* ptr = reinterpret_cast<void*>(fd);
            g_debug->pointer->Remove(ptr);
        }
        DMA_BUF::DropIonAllocationsForFd(fd);
    }
    return ret;
#endif
}

static void TrackMmapResult(
        void* result, size_t size, int flags, int fd, bool pending_ioctl) {
    if (!g_debug->TrackPointers()) {
        return;
    }
    switch (hook_source::ClassifyMmap(result, flags, fd, pending_ioctl)) {
        case hook_source::MmapCaptureKind::Anonymous:
            g_debug->pointer->Add(result, size, size, MMAP);
            return;
        case hook_source::MmapCaptureKind::PendingIoctl:
            g_debug->pointer->Add(result, size, size, DMA);
            return;
        case hook_source::MmapCaptureKind::None:
            break;
    }
#if MALLOC_HOOK_ENABLE_DMA_CAPTURE
    if (result != MAP_FAILED) {
        size_t dma_size = 0;
        if (DMA_BUF::ShouldTrackDmaMapping(fd, flags, &dma_size)) {
            g_debug->pointer->Add(reinterpret_cast<void*>(fd), dma_size, dma_size, DMA);
            DMA_BUF::MarkDmaFdTracked(fd);
        }
    }
#endif
}

void* debug_mmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (DebugCallsDisabledOrAsyncWorker()) {
#if !defined(mmap64)
        return CallMmap64(addr, size, prot, flags, fd, offset);
#else
        return CallMmap(addr, size, prot, flags, fd, offset);
#endif
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

#if MALLOC_HOOK_ENABLE_DMA_CAPTURE
    const bool pending_ioctl = DMA_BUF::pending_gpu_allocation.Take() != 0;
#else
    const bool pending_ioctl = false;
#endif

    if (size > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return MAP_FAILED;
    }

#if !defined(mmap64)
    void* result = CallMmap64(addr, size, prot, flags, fd, offset);
#else
    void* result = CallMmap(addr, size, prot, flags, fd, offset);
#endif

    TrackMmapResult(result, size, flags, fd, pending_ioctl);

    return result;
}

void* debug_mmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (DebugCallsDisabledOrAsyncWorker()) {
        return CallMmap(addr, size, prot, flags, fd, offset);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

#if MALLOC_HOOK_ENABLE_DMA_CAPTURE
    const bool pending_ioctl = DMA_BUF::pending_gpu_allocation.Take() != 0;
#else
    const bool pending_ioctl = false;
#endif

    if (size > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return MAP_FAILED;
    }

    void* result = CallMmap(addr, size, prot, flags, fd, offset);
    TrackMmapResult(result, size, flags, fd, pending_ioctl);

    return result;
}

int debug_munmap(void* addr, size_t size) {
    if (DebugCallsDisabledOrAsyncWorker()) {
        return CallMunmap(addr, size);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    // Same ordering constraint as realloc: the record must be gone before the
    // kernel can hand the address to a concurrent mmap on another thread.
    PointerInfoType previous{};
    const bool had_entry =
            g_debug->TrackPointers() && g_debug->pointer->TakeEntry(addr, &previous);

    int ret = CallMunmap(addr, size);
    if (hook_source::SyscallSucceeded(ret)) {
        if (had_entry) {
            g_debug->pointer->RemoveBacktrace(previous.hash_index);
        }
    } else if (had_entry) {
        // The mapping is still live; restore its accounting untouched.
        g_debug->pointer->RestoreEntry(addr, previous);
    }
    return ret;
}

void* debug_mremap(
        void* old_addr, size_t old_size, size_t new_size, int flags, void* new_addr) {
    if (DebugCallsDisabledOrAsyncWorker()) {
        return CallMremap(old_addr, old_size, new_size, flags, new_addr);
    }
    if (new_size > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return MAP_FAILED;
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;
    void* result = CallMremap(old_addr, old_size, new_size, flags, new_addr);
    if (hook_source::MappingSucceeded(result) && g_debug->TrackPointers()) {
        if ((flags & ALLOC_HOOK_MREMAP_DONTUNMAP) != 0) {
            // MREMAP_DONTUNMAP leaves the source range mapped and live, so the
            // process now holds two mappings rather than one that moved.
            // Re-keying the record would drop the still-valid old range from
            // current_used and leave a later munmap(old_addr) with no entry to
            // remove, so record the destination as its own mapping instead.
            g_debug->pointer->Add(result, new_size, new_size, MMAP);
        } else {
            g_debug->pointer->Remap(old_addr, result, new_size);
        }
    }
    return result;
}
