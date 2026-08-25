#include <gtest/gtest.h>

#include "UnwindBacktrace.h"

TEST(StackContract, KeepsCaptureStatesDistinct) {
    RawStackRecord empty;
    RawStackRecord partial;
    partial.capture_state = StackCaptureState::Partial;
    partial.terminal_error = 5;
    partial.frame_count = 1;
    partial.pcs[0] = 0x1000;

    RawStackRecord complete;
    complete.capture_state = StackCaptureState::Complete;
    complete.frame_count = 2;
    complete.pcs[0] = 0x1000;
    complete.pcs[1] = 0x2000;

    RawStackRecord error;
    error.capture_state = StackCaptureState::Error;
    error.terminal_error = 2;

    EXPECT_EQ(empty.capture_state, StackCaptureState::Empty);
    EXPECT_EQ(partial.capture_state, StackCaptureState::Partial);
    EXPECT_EQ(complete.capture_state, StackCaptureState::Complete);
    EXPECT_EQ(error.capture_state, StackCaptureState::Error);
    EXPECT_EQ(partial.frame_count, 1u);
    EXPECT_EQ(complete.frame_count, 2u);
    EXPECT_NE(partial.terminal_error, complete.terminal_error);
}

TEST(StackContract, CarriesNeutralSymbolizedFields) {
    SymbolizedFrame frame{
            .pc = 0x1234,
            .rel_pc = 0x234,
            .sp = 0x8000,
            .module_start = 0x1000,
            .function_offset = 7,
            .module_name = "libsample.so",
            .function_name = "sample"};

    EXPECT_EQ(frame.pc - frame.module_start, frame.rel_pc);
    EXPECT_EQ(frame.function_name, "sample");
    EXPECT_EQ(frame.module_name, "libsample.so");
}
