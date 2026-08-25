#include <cassert>
#include <cstddef>

#include "Sampling.h"

int main() {
    PoissonSampler disabled;
    disabled.SetSamplingInterval(1);
    assert(disabled.SampleSize(0) == 0);
    assert(disabled.SampleSize(17) == 17);

    PoissonSampler sampler;
    sampler.SetSamplingInterval(1024);
    size_t sampled = 0;
    size_t skipped = 0;
    for (size_t i = 0; i < 1000; ++i) {
        const size_t estimate = sampler.SampleSize(128);
        sampled += estimate;
        skipped += estimate == 0;
    }
    // The fixed seed makes this deterministic while allowing the sampler to
    // preserve a non-zero estimate and skip most small allocations.
    assert(sampled > 0);
    assert(skipped > 0);
}
