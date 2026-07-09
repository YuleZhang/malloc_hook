#include "DebugData.h"
#include "TraceWriter.h"

bool DebugData::Initialize(void* storage) {
    if (!config_.Init()) {
        return false;
    }

    if (config_.options() & TRACE_PERFETTO) {
        TraceWriter::Get().Initialize(
                config_.trace_min_size_bytes(), config_.trace_max_size_bytes());
    }

    pointer.reset(new (storage) PointerData());
    if (!pointer->Initialize(config_)) {
        return false;
    }

    return true;
}