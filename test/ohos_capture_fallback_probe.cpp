#include "UnwindBacktrace.h"

#include <cassert>

int main() {
    const RawStackRecord record = CaptureStack(StackCaptureMode::Accurate, 4, 0);
    assert(record.mode == StackCaptureMode::Accurate);
    assert(record.backend == StackCaptureBackend::CompilerUnwind);
    assert(record.backend != StackCaptureBackend::OhosNative);
    assert(record.capture_state == StackCaptureState::Complete ||
           record.capture_state == StackCaptureState::Partial ||
           record.capture_state == StackCaptureState::Empty);
    return 0;
}
