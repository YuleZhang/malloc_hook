#include <cstdint>
#include <vector>

#include "UnwindBacktrace.h"

namespace {
RawStackRecord g_recursive_result;

void ReenterCapture() {
    g_recursive_result = CaptureStack(StackCaptureMode::Fast, 4, 0);
}

bool HasUsableState(const RawStackRecord& record) {
    return record.capture_state == StackCaptureState::Complete ||
           record.capture_state == StackCaptureState::Partial;
}
}  // namespace

int main() {
    const RawStackRecord fast = CaptureStack(StackCaptureMode::Fast, 4, 1);
    if (fast.mode != StackCaptureMode::Fast || fast.skipped_frames != 1 ||
        fast.frame_count > 4) {
        return 1;
    }
#if defined(MALLOC_HOOK_HAVE_COMPILER_UNWIND) && MALLOC_HOOK_HAVE_COMPILER_UNWIND
    if (fast.backend != StackCaptureBackend::CompilerUnwind ||
        (fast.frame_count != 0 && !HasUsableState(fast))) {
        return 2;
    }
#else
    if (fast.backend != StackCaptureBackend::Fallback ||
        fast.capture_state != StackCaptureState::Error ||
        fast.terminal_error == 0) {
        return 3;
    }
#endif

    const RawStackRecord accurate = CaptureStack(StackCaptureMode::Accurate, 4, 1);
    if (accurate.mode != StackCaptureMode::Accurate ||
        accurate.skipped_frames != 1 || accurate.frame_count > 4) {
        return 4;
    }
#if defined(MALLOC_HOOK_TARGET_OS_ANDROID)
    if (accurate.backend != StackCaptureBackend::AndroidUnwindstack) {
        return 5;
    }
#elif defined(MALLOC_HOOK_TARGET_OS_LINUX)
    if (accurate.backend != StackCaptureBackend::LinuxNative ||
        accurate.frame_count == 0 || !HasUsableState(accurate)) {
        return 6;
    }
#elif defined(MALLOC_HOOK_TARGET_OS_OHOS)
#if defined(MALLOC_HOOK_HAVE_COMPILER_UNWIND) && MALLOC_HOOK_HAVE_COMPILER_UNWIND
    // OHOS currently has no dedicated native Accurate backend. The truthful
    // outcome is the compiler unwinder fallback, while mode remains Accurate.
    if (accurate.backend != StackCaptureBackend::CompilerUnwind ||
        accurate.mode != StackCaptureMode::Accurate) {
        return 7;
    }
#else
    if (accurate.backend != StackCaptureBackend::Fallback ||
        accurate.capture_state != StackCaptureState::Error) {
        return 8;
    }
#endif
#endif

    const std::vector<uintptr_t> frames{0x10, 0x20, 0x30, 0x40};
    const RawStackRecord partial = BuildCapturedRecordForTest(
            StackCaptureMode::Accurate, StackCaptureBackend::AndroidUnwindstack,
            9, frames, 2, 1);
    if (partial.capture_state != StackCaptureState::Partial ||
        partial.terminal_error != 9 || partial.frame_count != 2 ||
        partial.pcs[0] != 0x20 || partial.pcs[1] != 0x30) {
        return 9;
    }

    g_recursive_result = RawStackRecord{};
    SetStackCaptureReentryHookForTest(ReenterCapture);
    (void)CaptureStack(StackCaptureMode::Fast, 4, 0);
    if (g_recursive_result.backend != StackCaptureBackend::Fallback ||
        g_recursive_result.capture_state != StackCaptureState::Error ||
        g_recursive_result.terminal_error != 2) {
        return 10;
    }

    return 0;
}
