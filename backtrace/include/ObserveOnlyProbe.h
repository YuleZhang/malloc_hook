#pragma once

#include <atomic>

// ---------------------------------------------------------------------------
// Observe-only probe: measure the footprint, take over nothing.
//
// LD_PRELOAD'ing this library interposes the allocation family unconditionally.
// The symbols are in the dynamic table and no runtime switch can remove them.
// What *is* conditional is whether an interposed call does any work: tracking
// the pointer, unwinding a stack, taking the tracker's two mutexes, holding the
// stack table for the life of the process. That work only pays for itself if
// something consumes it, and the only consumers are the exit report and the
// on-demand checkpoint.
//
// So a run that asks only for a sampling cadence -- ALLOC_HOOK_PEAK_SAMPLE_MS on
// its own, or a host framework's published interval -- gets the footprint and
// nothing else: the tracker is never built and every interposed call forwards
// straight to libc. What it pays is one sampler thread reading /proc plus one
// relaxed load per interposed call; what it gets is the observed rss/dma/gpu
// peaks on stderr at exit.
//
// Asking for a report is what buys tracking, and there are two ways to ask:
// DUMP_PEAK_VALUE_MB for the first crossing of a floor, or the interval together
// with DUMP_PEAK_STEP_MB for peak-chasing. Either way Config::Init() enables peak
// recording and this probe stays out of the process. Config::ObserveOnlyRequested
// is the exact inverse of that gate, so the two modes cannot both apply.
//
// What this mode deliberately does not do, because none of it is free: capture
// stacks, so it cannot attribute the peak to a call site; build a live
// allocation table, so `checkpoint()` answers with the observed figures instead
// of a heap report; disable Bionic heap tagging or install the tracker's fork
// handlers, since there is no tracker state to protect.
// ---------------------------------------------------------------------------

namespace observe_only {

enum Mode : int {
    // The environment has not been consulted yet, or it was too early to
    // (see ResolveMode). Not latched.
    kUndecided = 0,
    // Observe-only: interposed calls forward to libc untouched.
    kBypass = 1,
    // Normal tracking.
    kTrack = 2,
};

// Latched once per process. Public so the gate below can inline its fast path
// into the interposers.
extern std::atomic<int> g_mode;

// Reads the environment and latches the answer, returning the resolved mode.
// Returns kUndecided without latching when the environment is not readable yet,
// so an allocation that runs before the loader publishes `environ` cannot decide
// this process's mode from an empty one.
int ResolveMode();

// The interposition gate: one relaxed load once the mode is latched.
inline bool Bypassed() {
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (__builtin_expect(mode == kUndecided, 0)) {
        return ResolveMode() == kBypass;
    }
    return mode == kBypass;
}

// Starts the footprint sampler with no tracker attached, and returns whether
// this process is now running as an observe-only probe. A no-op returning false
// in every other mode. Call once, from a library constructor.
//
// Also registers the exit report with atexit(), because Bionic runs a preloaded
// library's destructors on dlclose rather than at process exit.
bool StartProbe();

// Stops the sampler and writes the observed figures to stderr. A no-op unless
// StartProbe() succeeded in this same process, and idempotent: on a libc that
// runs both atexit handlers and this library's destructors, only the first of
// the two reports.
void ReportAtExit();

// Writes the same figures to `file_name`, or to stderr when it is null or
// cannot be opened. This is what the exported `checkpoint()` answers with in
// observe-only mode, where there is no live allocation table to report.
// Does not stop the sampler.
bool WriteReport(const char* file_name);

}  // namespace observe_only
