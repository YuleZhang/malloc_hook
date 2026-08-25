#include <gtest/gtest.h>

#include "UnwindBacktrace.h"

#if defined(MALLOC_HOOK_TARGET_OS_LINUX)
TEST(LinuxUnwindAdapter, FastAndAccurateProduceBoundedUsableRecords) {
    const RawStackRecord fast = CaptureStack(StackCaptureMode::Fast, 8, 1);
    const RawStackRecord accurate = CaptureStack(StackCaptureMode::Accurate, 8, 1);

    EXPECT_LE(fast.frame_count, 8u);
    EXPECT_LE(accurate.frame_count, 8u);
    EXPECT_EQ(fast.skipped_frames, 1u);
    EXPECT_EQ(accurate.skipped_frames, 1u);
#if defined(MALLOC_HOOK_HAVE_COMPILER_UNWIND) && MALLOC_HOOK_HAVE_COMPILER_UNWIND
    EXPECT_EQ(fast.backend, StackCaptureBackend::CompilerUnwind);
    EXPECT_EQ(accurate.backend, StackCaptureBackend::LinuxNative);
    EXPECT_TRUE(fast.capture_state == StackCaptureState::Complete ||
                fast.capture_state == StackCaptureState::Partial);
    EXPECT_TRUE(accurate.capture_state == StackCaptureState::Complete ||
                accurate.capture_state == StackCaptureState::Partial);
#else
    EXPECT_EQ(fast.backend, StackCaptureBackend::Fallback);
    EXPECT_EQ(accurate.backend, StackCaptureBackend::Fallback);
    EXPECT_EQ(fast.capture_state, StackCaptureState::Error);
    EXPECT_EQ(accurate.capture_state, StackCaptureState::Error);
#endif
}
#endif
