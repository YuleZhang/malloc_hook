#include <cstdlib>
#include <cassert>

#include "Config.h"

int main() {
    if (Config::ParseCaptureMode(nullptr) != StackCaptureMode::Fast) {
        return 1;
    }
    unsetenv("ALLOC_HOOK_CAPTURE_MODE");
    unsetenv("ALLOC_HOOK_SAMPLING_INTERVAL_BYTES");
    unsetenv("ALLOC_HOOK_SAMPLING_INTERVAL");
    unsetenv("ALLOC_HOOK_FAST_CAPTURE_INTERVAL_BYTES");
    // Config::Init() carries the behaviour under test, so it must be invoked
    // outside assert(): an NDEBUG build would otherwise drop the call and turn
    // every check below into a vacuous pass.
    Config config;
    bool initialized = config.Init();
    assert(initialized);
    assert(config.capture_mode() == StackCaptureMode::Fast);
    assert(!config.sampling_enabled());
    assert(config.fast_capture_interval_bytes() == 1);

    setenv("ALLOC_HOOK_FAST_CAPTURE_INTERVAL_BYTES", "1048576", 1);
    initialized = config.Init();
    assert(initialized);
    assert(config.fast_capture_interval_bytes() == 1048576);

    setenv("ALLOC_HOOK_SAMPLING_INTERVAL_BYTES", "4096", 1);
    initialized = config.Init();
    assert(initialized);
    assert(config.sampling_interval_bytes() == 4096);
    assert(config.sampling_enabled());

    setenv("ALLOC_HOOK_CAPTURE_MODE", "Accurate", 1);
    initialized = config.Init();
    assert(initialized);
    assert(config.capture_mode() == StackCaptureMode::Accurate);
    assert(!config.sampling_enabled());

    setenv("ALLOC_HOOK_CAPTURE_MODE", "fAsT", 1);
    initialized = config.Init();
    assert(initialized);
    assert(config.capture_mode() == StackCaptureMode::Fast);
    assert(config.sampling_enabled());
    (void)initialized;

    setenv("ALLOC_HOOK_CAPTURE_MODE", "invalid-mode", 1);
    if (!config.Init() || config.capture_mode() != StackCaptureMode::Fast) {
        return 6;
    }

    setenv("ALLOC_HOOK_CAPTURE_MODE", "ACCURATE", 1);
    if (!config.Init() || config.capture_mode() != StackCaptureMode::Accurate) {
        return 7;
    }
    return 0;
}
