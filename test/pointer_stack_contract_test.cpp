#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

#include "PointerData.h"

int main() {
    // FrameKeyType borrows the PC array rather than owning a fixed-size copy,
    // so callers supply the storage the key points at.
    const uintptr_t first_pcs[2] = {0x1000, 0x2000};
    const uintptr_t other_pcs[2] = {0x1000, 0x3000};

    FrameKeyType first;
    first.frame_count = 2;
    first.pcs = first_pcs;
    first.module_generation = 7;
    FrameKeyType duplicate = first;
    FrameKeyType other_generation = first;
    other_generation.module_generation = 8;
    FrameKeyType other_frames = first;
    other_frames.pcs = other_pcs;

    assert(first == duplicate);
    assert(!(first == other_generation));
    assert(!(first == other_frames));
    assert(std::hash<FrameKeyType>{}(first) ==
           std::hash<FrameKeyType>{}(duplicate));
    // A key comparing equal must do so through the borrowed contents, not the
    // pointer identity.
    FrameKeyType aliased = first;
    const uintptr_t copied_pcs[2] = {first_pcs[0], first_pcs[1]};
    aliased.pcs = copied_pcs;
    assert(aliased.pcs != first.pcs);
    assert(first == aliased);

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

    auto raw_frames = std::make_shared<const std::vector<uintptr_t>>(
            std::vector<uintptr_t>{first_pcs[0], first_pcs[1]});
    FrameInfoType frame_info{
            .references = 1,
            .frames = raw_frames,
            .module_generation = 7,
            .capture_state = partial.capture_state,
            .terminal_error = partial.terminal_error};
    ListInfoType report{
            .pointer = 0x4000,
            .num_allocations = 1,
            .size = 64,
            .mem_type = HOST,
            .frame_info = &frame_info,
            .raw_frames = raw_frames,
            .backtrace_info = symbols,
            .capture_state = partial.capture_state,
            .terminal_error = partial.terminal_error};

    assert(report.backtrace_info->front().module_name == "libsample.so");
    assert(report.capture_state == StackCaptureState::Partial);
    assert(report.terminal_error == 5);
    // A peak snapshot must share the PC array with the live entry instead of
    // deep-copying it.
    assert(report.raw_frames.get() == frame_info.frames.get());
    assert(report.raw_frames->size() == 2);

    // Captured PCs are return addresses. A symbolizer resolves the address it
    // is handed as the instruction being executed, so the report converts each
    // one to its call site before module lookup; otherwise a call in a
    // function's last instruction resolves to the next function, and a call in
    // a module's last instruction resolves to no module at all.
    assert(CallSitePcFromReturnAddress(0x1000) == 0x1000 - kReturnAddressPcAdjust);
    assert(CallSitePcFromReturnAddress(0x1000) < 0x1000);
#if defined(__aarch64__)
    // Fixed-width instructions: exactly the bl/blr.
    assert(kReturnAddressPcAdjust == 4);
#else
    assert(kReturnAddressPcAdjust == 1);
#endif
    // Too small to sit after a call; must not underflow into the top of the
    // address space.
    assert(CallSitePcFromReturnAddress(0) == 0);
    assert(CallSitePcFromReturnAddress(kReturnAddressPcAdjust) ==
           kReturnAddressPcAdjust);
    return 0;
}
