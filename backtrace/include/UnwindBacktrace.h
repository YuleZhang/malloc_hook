/*
 * Copyright (C) 2018 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class StackCaptureState : uint8_t { Empty, Complete, Partial, Error };
enum class StackCaptureMode : uint8_t { Fast, Accurate };
enum class StackCaptureBackend : uint8_t {
    Unknown,
    CompilerUnwind,
    AndroidUnwindstack,
    LinuxNative,
    OhosNative,
    Fallback,
    // aarch64 frame-pointer (x29) chain walk. Parses no CFI and authenticates
    // no return address, so it is both cheaper than the compiler unwinder and
    // immune to third-party libraries whose unwind tables drive libgcc's
    // pointer-authentication path into a fault.
    FramePointer,
};

constexpr size_t kMaxAsyncRawFrames = 256;

struct RawStackRecord {
    StackCaptureState capture_state = StackCaptureState::Empty;
    StackCaptureMode mode = StackCaptureMode::Fast;
    StackCaptureBackend backend = StackCaptureBackend::Unknown;
    uint8_t terminal_error = 0;
    uint16_t skipped_frames = 0;
    uint64_t module_generation = 0;
    uint16_t frame_count = 0;
    // Deliberately left uninitialized: zero-initializing 2KB on every
    // allocation dominated the Fast hot path and evicted the traced
    // application's own working set from L1. Only the first `frame_count`
    // entries are ever defined, and every reader is bounded by it.
    std::array<uintptr_t, kMaxAsyncRawFrames> pcs;

    bool operator==(const RawStackRecord& other) const;
    bool HasFrames() const { return frame_count != 0; }
    bool IsUsable() const {
        return capture_state == StackCaptureState::Complete ||
               capture_state == StackCaptureState::Partial;
    }
};

struct SymbolizedFrame {
    uintptr_t pc = 0;
    uintptr_t rel_pc = 0;
    uintptr_t sp = 0;
    uintptr_t module_start = 0;
    uint64_t module_generation = 0;
    uint64_t function_offset = 0;
    std::string module_name;
    std::string build_id;
    std::string function_name;
};

// Capture a bounded raw stack without module or symbol lookup. Fast uses the
// compiler unwinder when the configure-time capability probe succeeds;
// Accurate selects an explicit OS backend and preserves useful partial frames.
//
// CaptureStackInto() is the allocation-hot-path entry point: it fills a record
// the caller already owns so no 2KB record is copied or returned by value.
void CaptureStackInto(
        RawStackRecord* record, StackCaptureMode mode = StackCaptureMode::Fast,
        size_t max_frames = 128, size_t skipped_frames = 0);

RawStackRecord CaptureStack(
        StackCaptureMode mode = StackCaptureMode::Fast, size_t max_frames = 128,
        size_t skipped_frames = 0);

#if defined(MALLOC_HOOK_STACK_CAPTURE_TESTING)
using StackCaptureReentryHook = void (*)();
void SetStackCaptureReentryHookForTest(StackCaptureReentryHook hook);
bool FrameRecordFitsStackForTest(
        uintptr_t fp, uintptr_t stack_low, uintptr_t stack_high);
RawStackRecord BuildCapturedRecordForTest(
        StackCaptureMode mode, StackCaptureBackend backend, uint8_t terminal_error,
        const std::vector<uintptr_t>& frames, size_t max_frames, size_t skipped_frames);
#endif

struct StackRecord {
    StackCaptureState state = StackCaptureState::Empty;
    StackCaptureMode mode = StackCaptureMode::Accurate;
    StackCaptureBackend backend = StackCaptureBackend::Unknown;
    uint8_t terminal_error = 0;
    size_t skipped_frames = 0;
    std::vector<uintptr_t> raw_pcs;
    std::vector<SymbolizedFrame> frames;

    bool HasFrames() const { return !raw_pcs.empty(); }
    bool IsUsable() const {
        return state == StackCaptureState::Complete || state == StackCaptureState::Partial;
    }
};

// Capture capability contract:
// - Fast is bounded current-thread native capture. It writes raw PCs, performs no
//   synchronous symbol/module lookup, and uses _Unwind_Backtrace only when the
//   MALLOC_HOOK_HAVE_COMPILER_UNWIND capability probe succeeds.
// - Accurate selects an OS-specific native backend, but returns the same
//   project-owned raw record. Useful partial frames and the terminal error are
//   both retained. A fallback must be explicit rather than silently changing
//   platform or mode.
// - Native C/C++ stacks are required. Managed-runtime, other-thread/context, and
//   offline-unwind support are optional capabilities.
// - The configure-time ARCH-01 capability contract exposes these decisions as
//   MALLOC_HOOK_CAP_* definitions. A zero managed-runtime or direct-syscall
//   capability is an explicit limitation, not a failed capture.
// - Downstream ownership is split deliberately: this header owns the neutral
//   capture/result shape; UnwindBacktrace.cpp owns platform routing; the async
//   resolver owns module/symbol work; PointerData owns aggregation/report
//   identity; alloc_hook.cpp owns interposition and success filtering.
//
#if defined(MALLOC_HOOK_ENABLE_LEGACY_UNWINDSTACK_ADAPTER)
namespace unwindstack {
enum ErrorCode : uint8_t;
struct FrameData;
}  // namespace unwindstack

// Legacy backend entry point retained temporarily for the capture-backend wave.
// It is opt-in so Fast consumers do not inherit unwindstack domain types.
unwindstack::ErrorCode Unwind(
        std::vector<uintptr_t>* frames, std::vector<unwindstack::FrameData>* info,
        size_t max_frames);
#endif
