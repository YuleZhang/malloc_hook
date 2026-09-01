#pragma once

#include <stdint.h>
#include <cstddef>

#include "UnwindBacktrace.h"

constexpr uint64_t BACKTRACE = 0x1;                 // 记录堆栈
constexpr uint64_t TRACK_ALLOCS = 0x2;              // 记录内存申请动作
constexpr uint64_t BACKTRACE_SPECIFIC_SIZES = 0x4;  // 记录特定大小的内存申请
constexpr uint64_t RECORD_MEMORY_PEAK = 0x8;        // 记录内存峰值
constexpr uint64_t DUMP_ON_SINGAL = 0x80;           // 记录内存峰值

// Which instant of the run the retained peak snapshot describes.
//
// Both policies measure the same criterion -- the observed total, host
// RSS + dmabuf + GPU mappings -- and differ only in which crossing of it they
// keep the allocation stacks for.
enum class PeakRetention {
    // Re-snapshot every time the criterion grows past the configured step, so
    // the report describes the highest watermark the run reached. Needs no prior
    // knowledge of the peak, and pays a stack walk per step.
    ChaseMax,
    // Snapshot once, the first time the criterion passes the configured floor,
    // and never again. One stack walk for the whole run, so no allocating thread
    // is stalled by a snapshot after that point -- at the cost of describing the
    // floor crossing rather than the maximum, which is only the same question
    // when the floor is set near the known peak.
    FirstCrossing,
};

class Config {
public:
    bool Init();

    uint64_t options() const { return options_; }

    int backtrace_dump_signal() const { return backtrace_dump_signal_; }

    size_t backtrace_frames() const { return backtrace_frames_; }
    bool backtrace_dump_on_exit() const { return backtrace_dump_on_exit_; }
    const char* backtrace_dump_prefix() const { return backtrace_dump_prefix_; }

    size_t backtrace_min_size_bytes() const { return backtrace_min_size_bytes_; }
    size_t backtrace_max_size_bytes() const { return backtrace_max_size_bytes_; }

    // Floor on the peak criterion, in bytes, below which no snapshot is taken.
    // 0 means no floor. A floor selects PeakRetention::FirstCrossing, so this is
    // also the watermark the retained snapshot describes in that mode.
    size_t backtrace_dump_peak_val() const { return backtrace_dump_peak_val_; }
    size_t peak_record_step_bytes() const { return peak_record_step_bytes_; }
    PeakRetention peak_retention() const { return peak_retention_; }
    // Interval, in milliseconds, at which the evaluator-visible footprint
    // (host RSS + dmabuf + GPU mappings, read from /proc) is sampled. 0 leaves
    // the peak criterion on tracked allocation bytes, which is either an
    // explicit opt-out or the fallback for a sampler that could not start.
    unsigned observed_peak_sample_ms() const { return observed_peak_sample_ms_; }
    size_t sampling_interval_bytes() const { return sampling_interval_bytes_; }
    size_t fast_capture_interval_bytes() const { return fast_capture_interval_bytes_; }
    bool sampling_enabled() const {
        return capture_mode_ == StackCaptureMode::Fast && sampling_interval_bytes_ > 1;
    }
    StackCaptureMode capture_mode() const { return capture_mode_; }

    static StackCaptureMode ParseCaptureMode(const char* value);

private:
    int backtrace_dump_signal_ = 0;

    size_t backtrace_frames_ = 0;
    bool backtrace_dump_on_exit_ = false;
    const char* backtrace_dump_prefix_;

    size_t backtrace_min_size_bytes_ = 0;
    size_t backtrace_max_size_bytes_ = 0;

    size_t backtrace_dump_peak_val_ = 0;
    size_t peak_record_step_bytes_ = 0;
    PeakRetention peak_retention_ = PeakRetention::ChaseMax;
    unsigned observed_peak_sample_ms_ = 0;
    size_t sampling_interval_bytes_ = 1;
    size_t fast_capture_interval_bytes_ = 1;
    StackCaptureMode capture_mode_ = StackCaptureMode::Fast;

    uint64_t options_ = 0;
};
