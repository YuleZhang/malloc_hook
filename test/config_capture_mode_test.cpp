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
    Config config;
    assert(config.Init());
    assert(config.capture_mode() == StackCaptureMode::Fast);
    assert(!config.sampling_enabled());

    setenv("ALLOC_HOOK_SAMPLING_INTERVAL_BYTES", "4096", 1);
    assert(config.Init());
    assert(config.sampling_interval_bytes() == 4096);
    assert(config.sampling_enabled());

    setenv("ALLOC_HOOK_CAPTURE_MODE", "Accurate", 1);
    assert(config.Init());
    assert(config.capture_mode() == StackCaptureMode::Accurate);
    assert(!config.sampling_enabled());

    setenv("ALLOC_HOOK_CAPTURE_MODE", "fAsT", 1);
    assert(config.Init());
    assert(config.capture_mode() == StackCaptureMode::Fast);
    assert(config.sampling_enabled());

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
