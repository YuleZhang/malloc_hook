#include <gtest/gtest.h>

#include "UnwindBacktrace.h"

namespace {
RawStackRecord g_recursive_result;
void ReenterCapture() {
    g_recursive_result = CaptureStack(StackCaptureMode::Fast, 4, 0);
}

void ThrowCapture() {
    throw 7;
}
}  // namespace

TEST(CaptureRouting, FastIsBoundedAndHasExplicitBackend) {
    const RawStackRecord result = CaptureStack(StackCaptureMode::Fast, 4, 1);
    EXPECT_LE(result.frame_count, 4u);
    EXPECT_EQ(result.skipped_frames, 1u);
#if defined(MALLOC_HOOK_HAVE_COMPILER_UNWIND) && MALLOC_HOOK_HAVE_COMPILER_UNWIND
    EXPECT_EQ(result.backend, StackCaptureBackend::CompilerUnwind);
    EXPECT_TRUE(result.capture_state == StackCaptureState::Complete ||
                result.capture_state == StackCaptureState::Partial ||
                result.capture_state == StackCaptureState::Empty);
#else
    EXPECT_EQ(result.backend, StackCaptureBackend::Fallback);
    EXPECT_EQ(result.capture_state, StackCaptureState::Error);
    EXPECT_NE(result.terminal_error, 0u);
#endif
}

TEST(CaptureRouting, AccurateUsesTargetSpecificBackendOrFallback) {
    const RawStackRecord result = CaptureStack(StackCaptureMode::Accurate, 4, 0);
#if defined(MALLOC_HOOK_TARGET_OS_ANDROID)
    EXPECT_EQ(result.backend, StackCaptureBackend::AndroidUnwindstack);
#elif defined(MALLOC_HOOK_TARGET_OS_LINUX)
    EXPECT_EQ(result.backend, StackCaptureBackend::LinuxNative);
    EXPECT_GT(result.frame_count, 0u);
    EXPECT_TRUE(result.capture_state == StackCaptureState::Complete ||
                result.capture_state == StackCaptureState::Partial);
#elif defined(MALLOC_HOOK_TARGET_OS_OHOS)
#if defined(MALLOC_HOOK_HAVE_COMPILER_UNWIND) && MALLOC_HOOK_HAVE_COMPILER_UNWIND
    EXPECT_EQ(result.backend, StackCaptureBackend::CompilerUnwind);
#else
    EXPECT_EQ(result.backend, StackCaptureBackend::Fallback);
#endif
#else
    EXPECT_EQ(result.backend, StackCaptureBackend::Fallback);
#endif
}

TEST(CaptureRouting, FastCapacityProducesPartialStack) {
#if defined(MALLOC_HOOK_HAVE_COMPILER_UNWIND) && MALLOC_HOOK_HAVE_COMPILER_UNWIND
    const RawStackRecord result = CaptureStack(StackCaptureMode::Fast, 1, 0);
    EXPECT_EQ(result.frame_count, 1u);
    EXPECT_EQ(result.capture_state, StackCaptureState::Partial);
    EXPECT_NE(result.terminal_error, 0u);
#else
    GTEST_SKIP() << "compiler unwind unavailable";
#endif
}

TEST(CaptureRouting, FastFrameRecordRejectsWrappingAddress) {
    constexpr uintptr_t kStackLow = 0x1000;
    constexpr uintptr_t kStackHigh = 0x2000;
    EXPECT_TRUE(FrameRecordFitsStackForTest(0x1ff0, kStackLow, kStackHigh));
    EXPECT_FALSE(FrameRecordFitsStackForTest(0x1ff8, kStackLow, kStackHigh));
    EXPECT_FALSE(FrameRecordFitsStackForTest(UINTPTR_MAX - 7, kStackLow, kStackHigh));
}

TEST(CaptureRouting, RecursiveCaptureIsRejectedExplicitly) {
    g_recursive_result = RawStackRecord{};
    SetStackCaptureReentryHookForTest(ReenterCapture);
    (void)CaptureStack(StackCaptureMode::Fast, 4, 0);
    EXPECT_EQ(g_recursive_result.capture_state, StackCaptureState::Error);
    EXPECT_EQ(g_recursive_result.backend, StackCaptureBackend::Fallback);
    EXPECT_EQ(g_recursive_result.terminal_error, 2u);
}

TEST(CaptureRouting, RecursionGuardResetsAfterCaptureThrows) {
    SetStackCaptureReentryHookForTest(ThrowCapture);
    EXPECT_THROW(CaptureStack(StackCaptureMode::Fast, 4, 0), int);
    const RawStackRecord result = CaptureStack(StackCaptureMode::Fast, 4, 0);
#if defined(MALLOC_HOOK_HAVE_COMPILER_UNWIND) && MALLOC_HOOK_HAVE_COMPILER_UNWIND
    EXPECT_NE(result.capture_state, StackCaptureState::Error);
#else
    EXPECT_EQ(result.backend, StackCaptureBackend::Fallback);
    EXPECT_EQ(result.capture_state, StackCaptureState::Error);
    EXPECT_EQ(result.terminal_error, 3u);
#endif
}


#if defined(MALLOC_HOOK_TARGET_OS_LINUX)
TEST(CaptureRouting, LinuxAccurateIsBoundedAndHonorsSkip) {
    const RawStackRecord result = CaptureStack(StackCaptureMode::Accurate, 4, 1);
    EXPECT_EQ(result.mode, StackCaptureMode::Accurate);
    EXPECT_EQ(result.backend, StackCaptureBackend::LinuxNative);
    EXPECT_EQ(result.skipped_frames, 1u);
    EXPECT_GT(result.frame_count, 0u);
    EXPECT_LE(result.frame_count, 4u);
    EXPECT_TRUE(result.capture_state == StackCaptureState::Complete ||
                result.capture_state == StackCaptureState::Partial);
}
#endif


TEST(CaptureRouting, AndroidAccurateSlicesSkippedFramesBeforeBounding) {
    const std::vector<uintptr_t> frames{0x10, 0x20, 0x30, 0x40};
    const RawStackRecord result = BuildCapturedRecordForTest(
            StackCaptureMode::Accurate, StackCaptureBackend::AndroidUnwindstack,
            0, frames, 2, 1);
    EXPECT_EQ(result.backend, StackCaptureBackend::AndroidUnwindstack);
    EXPECT_EQ(result.skipped_frames, 1u);
    ASSERT_EQ(result.frame_count, 2u);
    EXPECT_EQ(result.pcs[0], 0x20u);
    EXPECT_EQ(result.pcs[1], 0x30u);
    EXPECT_EQ(result.capture_state, StackCaptureState::Complete);
}
