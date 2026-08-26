#include "UnwindBacktrace.h"

static_assert(kMaxAsyncRawFrames > 0);
static_assert(static_cast<unsigned>(StackCaptureMode::Fast) !=
              static_cast<unsigned>(StackCaptureMode::Accurate));

int main() {
  const RawStackRecord record = CaptureStack(StackCaptureMode::Fast, 1, 0);
  return record.frame_count <= kMaxAsyncRawFrames ? 0 : 1;
}
