#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <strings.h>

#if defined(__BIONIC__)
#include <bionic/reserved_signals.h>
#endif

#include "Config.h"
#include "ObservedMemory.h"

extern "C" char** environ;

static constexpr size_t DEFAULT_BACKTRACE_FRAMES = 128;
static constexpr const char DEFAULT_BACKTRACE_DUMP_PREFIX[] =
        "/data/local/tmp/trace/backtrace_heap";
static constexpr size_t DEFAULT_OHOS_BACKTRACE_MIN_SIZE_BYTES = 40960;
// Size filter applied when peak recording is enabled on a platform that sets no
// default of its own. Peak recording walks the live stack table, so an unfiltered
// run pays for stacks that no report line can attribute.
static constexpr size_t kPeakRecordingMinSizeBytes = 1024;
static constexpr char kDumpPrefixEnv[] = "ALLOC_HOOK_DUMP_PREFIX";
static constexpr char kPeakSampleIntervalEnv[] = "ALLOC_HOOK_PEAK_SAMPLE_MS";
static constexpr char kDumpPeakValueEnv[] = "DUMP_PEAK_VALUE_MB";
static constexpr char kPeakStepEnv[] = "DUMP_PEAK_STEP_MB";
// Cadence used when peak recording is on but nothing published an interval. The
// criterion is a watermark of the observed total, which only the sampler can
// evaluate, so there is no "no sampler" fallback to take.
//
// Deliberately coarse. Under first-crossing retention exactly one snapshot is
// taken for the whole run, so the cadence only bounds how far past the floor the
// crossing is noticed; a fine cadence would read /proc hundreds of times a second
// on a pipeline that is being measured precisely because its timing matters.
static constexpr size_t kDefaultPeakSampleMs = 50;
// A host framework that samples this process's memory publishes the interval it
// uses under a variable whose name ends in this suffix. Adopting that interval
// makes the hook snapshot the stacks at the same instant such a framework calls
// the peak, which is the only way the two numbers describe the same moment.
//
// Matched by suffix rather than by full name so that no downstream framework or
// product name is embedded in this repository, and so that every prefix variant
// of the same variable is picked up.
static constexpr char kExternalSampleIntervalSuffix[] =
        "AUTO_SHOW_MEM_USE_DURATION_MS";

// The tunable environment surface, emitted verbatim into a dedicated ELF
// section so it can be read straight off the .so file -- no process, no run,
// host and on-device .so alike:
//
//     readelf -p .alloc_hook_help liballoc_hook.so
//     strings   liballoc_hook.so | grep -A40 'alloc_hook env'
//
// This is the single documentation source for the knobs; the reads themselves
// live in Config::Init(), ObserveOnlyProbe, UnwindBacktrace and malloc_debug.
// Keep this block in sync when a knob is added, renamed or removed -- it is the
// interface contract, and a reader who trusts it must not have to grep getenv.
// `used` keeps the compiler from eliding it; `retain` (SHF_GNU_RETAIN) keeps the
// linker from dropping it under --gc-sections, which the Android/NDK build turns
// on -- without it the section survives the host gcc build but not that one.
#if defined(__has_attribute)
#if __has_attribute(retain)
#define ALLOC_HOOK_RETAIN __attribute__((retain))
#endif
#endif
#ifndef ALLOC_HOOK_RETAIN
#define ALLOC_HOOK_RETAIN
#endif
__attribute__((used, section(".alloc_hook_help"))) ALLOC_HOOK_RETAIN
static const char kAllocHookEnvManifest[] =
        "alloc_hook env manifest (read with: readelf -p .alloc_hook_help <so>)\n"
        "  ALLOC_HOOK_PEAK_SAMPLE_MS   /proc footprint sampling period, ms. >0 "
        "starts the observe-only probe (rss+dma+gpu peak); 0/unset = tracked "
        "allocation peak only. default: 0\n"
        "  DUMP_PEAK_VALUE_MB          first-crossing peak: snapshot stacks once "
        "when the observed total first passes this many MB. selects tracking + "
        "first-crossing. default: 0 (off)\n"
        "  DUMP_PEAK_STEP_MB           chase-max peak: re-snapshot each time the "
        "peak grows this many MB. with PEAK_SAMPLE_MS>0 = chasing. default: 0 "
        "(off)\n"
        "  ALLOC_HOOK_DUMP_PREFIX      output path prefix for heap/backtrace "
        "dumps. default: built-in\n"
        "  ALLOC_HOOK_CAPTURE_MODE     stack capture mode: fast | accurate. "
        "default: fast\n"
        "  BACKTRACE_MIN_SIZE          minimum allocation size, bytes, to record. "
        "default: 0 (OHOS: 40960)\n"
        "  ALLOC_HOOK_FAST_UNWINDER    force the fast frame-pointer unwinder when "
        "set. default: unset\n"
        "  ENABLE_HOOK_DEBUG           verbose hook debug logging when set. "
        "default: unset\n"
        "  *AUTO_SHOW_MEM_USE_DURATION_MS  adopted as the sampling period (suffix "
        "match) when ALLOC_HOOK_PEAK_SAMPLE_MS is unset; a host framework's own "
        "knob.\n"
        "  (internal) ALLOC_HOOK_PRIMARY_PID  cross-copy report election, set by "
        "the library itself -- not a user knob.\n";

static int DefaultBacktraceSignal() {
#if defined(__BIONIC__)
    return BIONIC_SIGNAL_BACKTRACE;
#elif defined(MALLOC_HOOK_TARGET_OS_OHOS)
    return 46;
#else
    return SIGRTMIN + 6;
#endif
}

StackCaptureMode Config::ParseCaptureMode(const char* value) {
    if (value != nullptr && strcasecmp(value, "accurate") == 0) {
        return StackCaptureMode::Accurate;
    }
    return StackCaptureMode::Fast;
}

// Parses a non-negative decimal, reporting *why* it failed instead of printing.
//
// The silent form is the primitive because the observe-only decision is answered
// from the allocation path (see Config::ObserveOnlyRequested), where a printf
// would re-enter malloc -- and would then print once per allocation for as long
// as a malformed value stayed in the environment.
static bool ParseValueQuiet(
        const char* value, size_t* parsed_value, const char** error) {
    *parsed_value = 0;
    *error = nullptr;
    if (value == nullptr) {
        return false;
    }
    // Parse the value into a size_t value.
    errno = 0;
    char* end;
    long long_value = strtol(value, &end, 10);
    if (errno != 0) {
        *error = strerror(errno);
        return false;
    }
    // 指针值相减
    if (end == value || static_cast<size_t>(end - value) != strlen(value) ||
        long_value < 0) {
        *error = "expected a non-negative decimal integer";
        return false;
    }
    *parsed_value = static_cast<size_t>(long_value);
    return true;
}

static bool ParseValue(const char* value, size_t* parsed_value) {
    const char* error = nullptr;
    if (ParseValueQuiet(value, parsed_value, &error)) {
        return true;
    }
    if (error != nullptr) {
        printf("Error %s:%s\n", value, error);
    }
    return false;
}

// Finds the sampling interval published by a host framework, if any. Only a
// strictly positive value counts: such frameworks use 0 to mean "not sampling",
// in which case there is no external instant to align with.
static bool FindExternalSampleIntervalMs(size_t* interval_ms) {
    if (environ == nullptr) {
        return false;
    }
    const size_t suffix_length = strlen(kExternalSampleIntervalSuffix);
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const char* equals = strchr(*entry, '=');
        if (equals == nullptr) {
            continue;
        }
        const size_t name_length = static_cast<size_t>(equals - *entry);
        if (name_length < suffix_length) {
            continue;
        }
        if (strncmp(*entry + name_length - suffix_length,
                    kExternalSampleIntervalSuffix, suffix_length) != 0) {
            continue;
        }
        // Quiet on purpose: this variable belongs to a host framework, so a
        // value this library cannot parse is not this library's user's mistake
        // to be told about on the allocation path.
        const char* error = nullptr;
        if (ParseValueQuiet(equals + 1, interval_ms, &error) && *interval_ms > 0) {
            return true;
        }
    }
    return false;
}

bool Config::ObserveOnlyRequested(unsigned* interval_ms) {
    *interval_ms = 0;
    const char* error = nullptr;
    size_t value = 0;
    // A floor asks for a first-crossing report, so tracking has something to
    // produce. Mirrors Init()'s gate exactly, 0 and malformed values included:
    // whatever Init() would refuse to record a peak for must also leave this off,
    // or the two would disagree about which mode the process is in.
    if (ParseValueQuiet(getenv(kDumpPeakValueEnv), &value, &error) && value > 0) {
        return false;
    }
    size_t peak_sample_ms = 0;
    const bool explicit_interval =
            ParseValueQuiet(getenv(kPeakSampleIntervalEnv), &peak_sample_ms, &error);
    if (explicit_interval && peak_sample_ms > 0 &&
        ParseValueQuiet(getenv(kPeakStepEnv), &value, &error) && value > 0) {
        // An interval and a positive step together are chasing, which is a report.
        return false;
    }
    if (!explicit_interval && !FindExternalSampleIntervalMs(&peak_sample_ms)) {
        return false;
    }
    // An explicit 0 is the opt-out: no sampler thread, and no probe either. A
    // process with neither a cadence nor a report keeps the tracking it has
    // always had, because the on-demand checkpoint still needs a live table.
    if (peak_sample_ms == 0) {
        return false;
    }
    *interval_ms = static_cast<unsigned>(peak_sample_ms);
    return true;
}

int Config::DumpSignal() {
    // The report-trigger signal is not tunable: each platform uses its
    // conventional backtrace signal (Bionic's reserved backtrace signal on
    // Android, 46 on OHOS, SIGRTMIN+6 elsewhere) so a deployment script never
    // has to keep an env var in sync with the platform.
    return DefaultBacktraceSignal();
}

bool Config::Init() {
    options_ = 0;
    // 退出时输出 trace
    backtrace_dump_on_exit_ = false;
    backtrace_frames_ = DEFAULT_BACKTRACE_FRAMES;
    backtrace_dump_prefix_ = DEFAULT_BACKTRACE_DUMP_PREFIX;
    const char* dump_prefix = getenv(kDumpPrefixEnv);
    if (dump_prefix != nullptr && dump_prefix[0] != '\0') {
        backtrace_dump_prefix_ = dump_prefix;
    }
    capture_mode_ = ParseCaptureMode(getenv("ALLOC_HOOK_CAPTURE_MODE"));

    // 如果开启 BACKTRACE_SPECIFIC_SIZES, 请指定内存申请的最大和最小 size
    options_ |= BACKTRACE_SPECIFIC_SIZES;
#if defined(MALLOC_HOOK_TARGET_OS_OHOS)
    backtrace_min_size_bytes_ = DEFAULT_OHOS_BACKTRACE_MIN_SIZE_BYTES;
#else
    backtrace_min_size_bytes_ = 0;
#endif
    // Only a value that parsed replaces the platform default. ParseValue zeroes
    // its out-parameter before reporting failure, so passing the member directly
    // erased the default on every run that did not set the variable -- which is
    // every run on the one platform that has a default.
    //
    // Assigned unconditionally above rather than left to that side effect,
    // because Init() runs more than once in a process and must not carry a
    // previous run's explicit value into one that has none.
    size_t min_size_bytes = 0;
    const bool explicit_min_size =
            ParseValue(getenv("BACKTRACE_MIN_SIZE"), &min_size_bytes);
    if (explicit_min_size) {
        backtrace_min_size_bytes_ = min_size_bytes;
    }
    backtrace_max_size_bytes_ = SIZE_MAX;

    // 记录 trace（指针表和 host/dma 峰值计数器，独立于抓栈）
    options_ |= TRACK_ALLOCS;

    // Peak criterion cadence, resolved before the peak options below because the
    // criterion decides what the floor means: the floor is a watermark of the
    // observed total (host RSS + dmabuf + GPU mappings), and only the sampler can
    // evaluate that.
    size_t peak_sample_ms = 0;
    const bool explicit_sample_interval =
            ParseValue(getenv(kPeakSampleIntervalEnv), &peak_sample_ms);
    const bool adopted_sample_interval =
            !explicit_sample_interval && FindExternalSampleIntervalMs(&peak_sample_ms);

    // 0 means off, for every variable in this group and not just for the cadence
    // above: a value of 0 is how each of these is turned off individually, so a
    // floor of 0 asks for no first-crossing snapshot and a step of 0 asks for no
    // chasing. Neither is read as "on, with no limit".
    size_t peak_floor_mb = 0;
    ParseValue(getenv(kDumpPeakValueEnv), &peak_floor_mb);
    const bool has_peak_floor = peak_floor_mb > 0;

    // How much the peak must grow before the snapshot is rebuilt, and -- being
    // positive at all -- whether chasing was asked for. Unset and 0 are the same
    // answer here. There is deliberately no spelling for "rebuild on every new
    // peak": a small positive step is within 25% of it (NextPeakThreshold scales
    // for small peaks) without offering a value that reads like "off".
    size_t peak_step_mb = 0;
    ParseValue(getenv(kPeakStepEnv), &peak_step_mb);
    peak_record_step_bytes_ = peak_step_mb * 1024 * 1024;
    const bool has_peak_step = peak_step_mb > 0;

    // What asks for a report, and nothing else does:
    //   DUMP_PEAK_VALUE_MB=N                          -> first-crossing at N MB
    //   ALLOC_HOOK_PEAK_SAMPLE_MS=k + DUMP_PEAK_STEP_MB=s -> chasing, rebuilt per s
    //
    // An interval on its own asks to *watch* the footprint, not to attribute it,
    // and is answered by the observe-only probe: a log block at exit and no
    // tracking (see ObserveOnlyProbe.h). A framework's published interval never
    // reaches here either -- it supplies cadence to a run that asked for a report
    // by other means, and on its own it selects the probe as well.
    const bool record_peak =
            has_peak_floor ||
            (explicit_sample_interval && peak_sample_ms > 0 && has_peak_step);

    backtrace_dump_peak_val_ = peak_floor_mb * 1024 * 1024;
    peak_retention_ = has_peak_floor ? PeakRetention::FirstCrossing
                                     : PeakRetention::ChaseMax;
    if (record_peak) {
        options_ |= RECORD_MEMORY_PEAK;
        // Stack capture only earns its keep when a report consumes it. A run that
        // configures no report is a lightweight tracked probe: it keeps the
        // pointer table and the host/dma/total peak counters (TRACK_ALLOCS) but
        // never unwinds, and prints the tracked peaks at exit. So BACKTRACE is
        // enabled here, with the report, rather than unconditionally -- a bare
        // LD_PRELOAD must not pay ~2762ns/alloc to capture stacks nothing reads.
        options_ |= BACKTRACE;
        // Fills in a filter where the platform sets none. It must not relax one
        // that a platform did set: that default exists for that platform's own
        // cost reasons, and enabling peak recording is not a reason to capture
        // stacks it had decided to skip.
        if (!explicit_min_size && backtrace_min_size_bytes_ == 0) {
            backtrace_min_size_bytes_ = kPeakRecordingMinSizeBytes;
        }
        // Both modes report on exit. Without this a run configured only for
        // peak-chasing would sample the footprint for its whole lifetime and then
        // discard the snapshot it took.
        backtrace_dump_on_exit_ = true;
    }

    // An explicit interval always wins, including an explicit 0: that is the
    // opt-out for a caller that wants no extra thread and accepts a floor
    // compared against tracked allocation bytes instead.
    observed_peak_sample_ms_ = 0;
    if (record_peak) {
        if (explicit_sample_interval || adopted_sample_interval) {
            observed_peak_sample_ms_ = static_cast<unsigned>(peak_sample_ms);
        } else {
            observed_peak_sample_ms_ = static_cast<unsigned>(kDefaultPeakSampleMs);
        }
    }

    // 通过信号插入 check point
    options_ |= DUMP_ON_SINGAL;
    backtrace_dump_signal_ = DumpSignal();

    return true;
}
