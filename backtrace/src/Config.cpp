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
static constexpr char kSamplingIntervalBytesEnv[] =
        "ALLOC_HOOK_SAMPLING_INTERVAL_BYTES";
static constexpr char kFastCaptureIntervalEnv[] =
        "ALLOC_HOOK_FAST_CAPTURE_INTERVAL_BYTES";
static constexpr char kDumpPrefixEnv[] = "ALLOC_HOOK_DUMP_PREFIX";
static constexpr char kPeakSampleIntervalEnv[] = "ALLOC_HOOK_PEAK_SAMPLE_MS";
static constexpr char kDumpPeakValueEnv[] = "DUMP_PEAK_VALUE_MB";
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

static bool ParseValue(const char* value, size_t* parsed_value) {
    *parsed_value = 0;
    if (value == nullptr) {
        return false;
    }
    // Parse the value into a size_t value.
    errno = 0;
    char* end;
    long long_value = strtol(value, &end, 10);
    if (errno != 0) {
        printf("Error %s:%s\n", value, strerror(errno));
        return false;
    }
    if (end == value) {
        printf("Error %s\n", value);
        return false;
    }
    // 指针值相减
    if (static_cast<size_t>(end - value) != strlen(value)) {
        printf("Error %s\n", value);
        return false;
    }
    if (long_value < 0) {
        printf("Error %s\n", value);
        return false;
    }
    *parsed_value = static_cast<size_t>(long_value);
    return true;
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
        if (ParseValue(equals + 1, interval_ms) && *interval_ms > 0) {
            return true;
        }
    }
    return false;
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
    sampling_interval_bytes_ = 1;
    fast_capture_interval_bytes_ = 1;
    size_t fast_capture_interval = 0;
    if (ParseValue(getenv(kFastCaptureIntervalEnv), &fast_capture_interval) &&
        fast_capture_interval > 1) {
        fast_capture_interval_bytes_ = fast_capture_interval;
    }
    const char* sampling_interval_env = getenv(kSamplingIntervalBytesEnv);
    size_t sampling_interval = 0;
    if (ParseValue(sampling_interval_env, &sampling_interval) &&
        sampling_interval > 1) {
        sampling_interval_bytes_ = sampling_interval;
    }

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

    // 开启 unwind
    options_ |= BACKTRACE;
    // 记录 trace
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

    // A floor of 0 is no floor at all -- every run passes it on its first sample
    // -- so it selects peak-chasing instead of first-crossing. It still enables
    // peak recording: that is what this variable has always done, and a
    // deployment that passed 0 to mean "enable with no floor" must not silently
    // lose its report.
    size_t peak_floor_mb = 0;
    const bool peak_value_set =
            ParseValue(getenv(kDumpPeakValueEnv), &peak_floor_mb);
    const bool has_peak_floor = peak_value_set && peak_floor_mb > 0;

    // Either mode's own variable enables peak recording: a floor selects
    // first-crossing, an explicit interval selects peak-chasing. A framework's
    // interval only ever supplies the cadence -- letting it enable recording
    // would hand a sampler thread and an exit report to a process that set none
    // of these variables and asked for neither.
    const bool record_peak =
            peak_value_set || (explicit_sample_interval && peak_sample_ms > 0);

    backtrace_dump_peak_val_ = has_peak_floor ? peak_floor_mb * 1024 * 1024 : 0;
    peak_retention_ = has_peak_floor ? PeakRetention::FirstCrossing
                                     : PeakRetention::ChaseMax;
    if (record_peak) {
        options_ |= RECORD_MEMORY_PEAK;
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

    peak_record_step_bytes_ = DefaultPeakStepBytes();
    size_t peak_step_mb = 0;
    if (ParseValue(getenv("DUMP_PEAK_STEP_MB"), &peak_step_mb)) {
        peak_record_step_bytes_ = peak_step_mb * 1024 * 1024;
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
    backtrace_dump_signal_ = DefaultBacktraceSignal();
    size_t dump_signal = 0;
    if (ParseValue(getenv("BACKTRACE_DUMP_SIGNAL"), &dump_signal)) {
        backtrace_dump_signal_ = static_cast<int>(dump_signal);
    }

    return true;
}
