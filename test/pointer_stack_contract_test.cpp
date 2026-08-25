#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

#include "PointerData.h"

int main() {
    FrameKeyType first;
    first.frame_count = 2;
    first.pcs[0] = 0x1000;
    first.pcs[1] = 0x2000;
    first.module_generation = 7;
    FrameKeyType duplicate = first;
    FrameKeyType other_generation = first;
    other_generation.module_generation = 8;
    FrameKeyType other_frames = first;
    other_frames.pcs[1] = 0x3000;

    assert(first == duplicate);
    assert(!(first == other_generation));
    assert(!(first == other_frames));
    assert(std::hash<FrameKeyType>{}(first) ==
           std::hash<FrameKeyType>{}(duplicate));

    RawStackRecord partial;
    partial.capture_state = StackCaptureState::Partial;
    partial.terminal_error = 5;
    partial.frame_count = 1;
    partial.pcs[0] = 0x1234;
    assert(partial.frame_count != 0);
    assert(partial.capture_state == StackCaptureState::Complete ||
           partial.capture_state == StackCaptureState::Partial);

    RawStackRecord error;
    error.capture_state = StackCaptureState::Error;
    error.terminal_error = 2;
    assert(error.frame_count == 0);
    assert(error.capture_state != StackCaptureState::Complete &&
           error.capture_state != StackCaptureState::Partial);

    auto symbols = std::make_shared<std::vector<SymbolizedFrame>>();
    symbols->push_back(SymbolizedFrame{
            .pc = 0x1234,
            .rel_pc = 0x234,
            .module_start = 0x1000,
            .function_offset = 4,
            .module_name = "libsample.so",
            .function_name = "sample"});

    FrameInfoType frame_info{
            .references = 1,
            .frames = {first.pcs[0], first.pcs[1]},
            .module_generation = 7,
            .capture_state = partial.capture_state,
            .terminal_error = partial.terminal_error};
    ListInfoType report{
            .pointer = 0x4000,
            .num_allocations = 1,
            .size = 64,
            .mem_type = HOST,
            .frame_info = &frame_info,
            .backtrace_info = symbols,
            .capture_state = partial.capture_state,
            .terminal_error = partial.terminal_error};

    assert(report.backtrace_info->front().module_name == "libsample.so");
    assert(report.capture_state == StackCaptureState::Partial);
    assert(report.terminal_error == 5);
    return 0;
}
