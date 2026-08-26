#pragma once

#include <cstddef>
#include <cstdint>
#include <random>

// Deterministic per-thread byte sampler used by Fast-mode host allocations.
// A sample represents sampling_interval bytes of live memory.
class PoissonSampler {
public:
    void SetSamplingInterval(uint64_t sampling_interval) {
        sampling_interval_ = sampling_interval > 1 ? sampling_interval : 1;
        distribution_ = std::exponential_distribution<double>(
                1.0 / static_cast<double>(sampling_interval_));
        interval_to_next_sample_ = NextSampleInterval();
    }

    size_t SampleSize(size_t allocation_size) {
        if (sampling_interval_ <= 1) {
            return allocation_size;
        }
        interval_to_next_sample_ -= static_cast<int64_t>(allocation_size);
        size_t samples = 0;
        while (interval_to_next_sample_ <= 0) {
            interval_to_next_sample_ += NextSampleInterval();
            ++samples;
        }
        if (samples == 0 || samples > SIZE_MAX / sampling_interval_) {
            return samples == 0 ? 0 : SIZE_MAX;
        }
        return static_cast<size_t>(sampling_interval_) * samples;
    }

private:
    int64_t NextSampleInterval() {
        return static_cast<int64_t>(distribution_(engine_)) + 1;
    }

    uint64_t sampling_interval_ = 1;
    int64_t interval_to_next_sample_ = 1;
    std::mt19937_64 engine_{1};
    std::exponential_distribution<double> distribution_{1.0};
};
