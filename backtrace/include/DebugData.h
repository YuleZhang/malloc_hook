#pragma once

#include "Config.h"
#include "PointerData.h"

class DebugData {
public:
    DebugData() = default;
    ~DebugData() = default;

    bool Initialize(void* storage);

    const Config& config() { return config_; }

    bool TrackPointers() { return config_.options() & TRACK_ALLOCS; }

    std::unique_ptr<PointerData> pointer;

private:
    Config config_;

    DebugData(const DebugData&) = delete;
    DebugData& operator=(const DebugData&) = delete;
};

extern DebugData* g_debug;
