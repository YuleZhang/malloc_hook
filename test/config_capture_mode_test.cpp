#include <cstdlib>
#include <cassert>

#include "Config.h"

int main() {
    if (Config::ParseCaptureMode(nullptr) != StackCaptureMode::Fast) {
        return 1;
    }
    unsetenv("ALLOC_HOOK_CAPTURE_MODE");
    // Config::Init() carries the behaviour under test, so it must be invoked
    // outside assert(): an NDEBUG build would otherwise drop the call and turn
    // every check below into a vacuous pass.
    Config config;
    bool initialized = config.Init();
    assert(initialized);
    assert(config.capture_mode() == StackCaptureMode::Fast);

    setenv("ALLOC_HOOK_CAPTURE_MODE", "Accurate", 1);
    initialized = config.Init();
    assert(initialized);
    assert(config.capture_mode() == StackCaptureMode::Accurate);

    setenv("ALLOC_HOOK_CAPTURE_MODE", "fAsT", 1);
    initialized = config.Init();
    assert(initialized);
    assert(config.capture_mode() == StackCaptureMode::Fast);
    (void)initialized;

    setenv("ALLOC_HOOK_CAPTURE_MODE", "invalid-mode", 1);
    if (!config.Init() || config.capture_mode() != StackCaptureMode::Fast) {
        return 6;
    }

    setenv("ALLOC_HOOK_CAPTURE_MODE", "ACCURATE", 1);
    if (!config.Init() || config.capture_mode() != StackCaptureMode::Accurate) {
        return 7;
    }

    // Mode gating. A bare run -- no report env -- is the lightweight tracked
    // probe: it tracks allocations (for counters and the on-demand checkpoint)
    // but does not capture stacks. Configuring a report turns stack capture on.
    unsetenv("ALLOC_HOOK_CAPTURE_MODE");
    unsetenv("DUMP_PEAK_VALUE_MB");
    unsetenv("DUMP_PEAK_STEP_MB");
    unsetenv("ALLOC_HOOK_PEAK_SAMPLE_MS");
    initialized = config.Init();
    assert(initialized);
    assert((config.options() & TRACK_ALLOCS) != 0);
    assert((config.options() & BACKTRACE) == 0);

    setenv("DUMP_PEAK_VALUE_MB", "128", 1);
    initialized = config.Init();
    assert(initialized);
    assert((config.options() & BACKTRACE) != 0);
    assert((config.options() & RECORD_MEMORY_PEAK) != 0);
    unsetenv("DUMP_PEAK_VALUE_MB");
    (void)initialized;

    return 0;
}
