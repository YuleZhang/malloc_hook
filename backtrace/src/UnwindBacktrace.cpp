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

#include <stdint.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#if defined(MALLOC_HOOK_TARGET_OS_LINUX)
#include <execinfo.h>
#endif
#if defined(MALLOC_HOOK_HAVE_COMPILER_UNWIND) && MALLOC_HOOK_HAVE_COMPILER_UNWIND
#include <unwind.h>
#endif
#if defined(MALLOC_HOOK_ENABLE_LEGACY_UNWINDSTACK_ADAPTER)
#include "unwindstack/Error.h"
#include <unwindstack/Unwinder.h>
#endif

#if defined(MALLOC_HOOK_TARGET_OS_ANDROID) && \
        defined(MALLOC_HOOK_ENABLE_LEGACY_UNWINDSTACK_ADAPTER)
#include <android-base/stringprintf.h>
#include <unwindstack/AndroidUnwinder.h>
#include <unistd.h>
#endif

#include "UnwindBacktrace.h"

namespace {
thread_local bool g_capture_active = false;
#if defined(MALLOC_HOOK_STACK_CAPTURE_TESTING)
thread_local StackCaptureReentryHook g_capture_reentry_hook = nullptr;
#endif

struct CaptureActiveGuard {
    ~CaptureActiveGuard() { g_capture_active = false; }
};

#if defined(MALLOC_HOOK_TARGET_OS_ANDROID) || defined(MALLOC_HOOK_STACK_CAPTURE_TESTING)
RawStackRecord BuildCapturedRecord(
        StackCaptureMode mode, StackCaptureBackend backend, uint8_t terminal_error,
        const std::vector<uintptr_t>& frames, size_t max_frames, size_t skipped_frames) {
    RawStackRecord record;
    record.mode = mode;
    record.backend = backend;
    record.terminal_error = terminal_error;
    record.skipped_frames = static_cast<uint16_t>(
            std::min(skipped_frames, static_cast<size_t>(UINT16_MAX)));
    const size_t output_limit = std::min(max_frames, kMaxAsyncRawFrames);
    const size_t first = std::min(skipped_frames, frames.size());
    const size_t frame_count = std::min(output_limit, frames.size() - first);
    std::copy_n(frames.begin() + first, frame_count, record.pcs.begin());
    record.frame_count = static_cast<uint16_t>(frame_count);
    if (frame_count == 0) {
        record.capture_state = terminal_error == 0
                ? StackCaptureState::Empty
                : StackCaptureState::Error;
    } else {
        record.capture_state = terminal_error == 0
                ? StackCaptureState::Complete
                : StackCaptureState::Partial;
    }
    return record;
}
#endif

#if defined(MALLOC_HOOK_TARGET_OS_ANDROID) && \
        defined(MALLOC_HOOK_ENABLE_LEGACY_UNWINDSTACK_ADAPTER)
unwindstack::AndroidLocalUnwinder& LocalUnwinder() {
    [[clang::no_destroy]] static unwindstack::AndroidLocalUnwinder unwinder(
            std::vector<std::string>{"liballoc_hook.so"}, {},
            std::vector<std::string>{
                    "_Z24__init_additional_stacksP18pthread_internal_t",
                    "_Z25__allocate_thread_mappingmm"});
    return unwinder;
}

bool UnwindDebugEnabled() {
    static bool enabled = getenv("ALLOC_HOOK_DEBUG_UNWIND") != nullptr;
    return enabled;
}

void DebugLog(const char* message) {
    if (UnwindDebugEnabled()) {
        write(STDERR_FILENO, message, strlen(message));
    }
}
#endif

}  // namespace

#if defined(MALLOC_HOOK_HAVE_COMPILER_UNWIND) && MALLOC_HOOK_HAVE_COMPILER_UNWIND
namespace {
struct FastCaptureContext {
    RawStackRecord* record = nullptr;
    size_t max_frames = 0;
    size_t skip = 0;
};

_Unwind_Reason_Code CaptureFrame(
        _Unwind_Context* context, void* opaque) {
    auto* capture = static_cast<FastCaptureContext*>(opaque);
    const uintptr_t pc = static_cast<uintptr_t>(_Unwind_GetIP(context));
    if (pc == 0) {
        return _URC_NO_REASON;
    }
    if (capture->skip != 0) {
        --capture->skip;
        return _URC_NO_REASON;
    }
    if (capture->record->frame_count >= capture->max_frames) {
        capture->record->capture_state = StackCaptureState::Partial;
        capture->record->terminal_error = 1;
        return _URC_END_OF_STACK;
    }
    capture->record->pcs[capture->record->frame_count++] = pc;
    return _URC_NO_REASON;
}

RawStackRecord CaptureWithCompiler(
        StackCaptureMode mode, StackCaptureBackend backend, size_t max_frames,
        size_t skipped_frames) {
    RawStackRecord record;
    record.mode = mode;
    record.backend = backend;
    record.skipped_frames = static_cast<uint16_t>(
            std::min(skipped_frames, static_cast<size_t>(UINT16_MAX)));
    if (max_frames == 0) {
        return record;
    }
    max_frames = std::min(max_frames, kMaxAsyncRawFrames);
    FastCaptureContext context{&record, max_frames, skipped_frames};
    const _Unwind_Reason_Code reason = _Unwind_Backtrace(CaptureFrame, &context);
    if (record.frame_count != 0) {
        if (record.capture_state != StackCaptureState::Partial) {
            record.capture_state = reason == _URC_END_OF_STACK
                    ? StackCaptureState::Complete
                    : StackCaptureState::Partial;
        }
    } else if (reason != _URC_END_OF_STACK) {
        record.capture_state = StackCaptureState::Error;
        record.terminal_error = static_cast<uint8_t>(reason);
    }
    return record;
}

}  // namespace
#endif

#if defined(MALLOC_HOOK_TARGET_OS_LINUX)
namespace {
RawStackRecord CaptureLinuxNative(
        StackCaptureMode mode, size_t max_frames, size_t skipped_frames) {
    RawStackRecord record;
    record.mode = mode;
    record.backend = StackCaptureBackend::LinuxNative;
    record.skipped_frames = static_cast<uint16_t>(
            std::min(skipped_frames, static_cast<size_t>(UINT16_MAX)));

    const size_t output_limit = std::min(max_frames, kMaxAsyncRawFrames);
    if (output_limit == 0) {
        return record;
    }
    const size_t skip_limit = std::min(skipped_frames, kMaxAsyncRawFrames);
    const size_t capture_limit = std::min(kMaxAsyncRawFrames, output_limit + skip_limit);
    std::array<void*, kMaxAsyncRawFrames> captured_pcs{};
    const int captured = backtrace(captured_pcs.data(), static_cast<int>(capture_limit));
    if (captured <= 0) {
        record.capture_state = StackCaptureState::Error;
        record.terminal_error = 5;
        return record;
    }

    const size_t captured_count = static_cast<size_t>(captured);
    const size_t first = std::min(skip_limit, captured_count);
    const size_t frame_count = std::min(output_limit, captured_count - first);
    for (size_t i = 0; i < frame_count; ++i) {
        record.pcs[i] = reinterpret_cast<uintptr_t>(captured_pcs[first + i]);
    }
    record.frame_count = static_cast<uint16_t>(frame_count);
    if (frame_count == 0) {
        return record;
    }
    if (captured_count == capture_limit) {
        record.capture_state = StackCaptureState::Partial;
        record.terminal_error = 1;
    } else {
        record.capture_state = StackCaptureState::Complete;
    }
    return record;
}
}  // namespace
#endif

#if defined(MALLOC_HOOK_STACK_CAPTURE_TESTING)
void SetStackCaptureReentryHookForTest(StackCaptureReentryHook hook) {
    g_capture_reentry_hook = hook;
}

RawStackRecord BuildCapturedRecordForTest(
        StackCaptureMode mode, StackCaptureBackend backend, uint8_t terminal_error,
        const std::vector<uintptr_t>& frames, size_t max_frames, size_t skipped_frames) {
    return BuildCapturedRecord(
            mode, backend, terminal_error, frames, max_frames, skipped_frames);
}
#endif

RawStackRecord CaptureStack(
        StackCaptureMode mode, size_t max_frames, size_t skipped_frames) {
    RawStackRecord record;
    record.mode = mode;
    record.skipped_frames = static_cast<uint16_t>(
            std::min(skipped_frames, static_cast<size_t>(UINT16_MAX)));
    if (g_capture_active) {
        record.capture_state = StackCaptureState::Error;
        record.backend = StackCaptureBackend::Fallback;
        record.terminal_error = 2;
        return record;
    }
    g_capture_active = true;
    CaptureActiveGuard active_guard;
#if defined(MALLOC_HOOK_STACK_CAPTURE_TESTING)
    if (g_capture_reentry_hook != nullptr) {
        const StackCaptureReentryHook hook = g_capture_reentry_hook;
        g_capture_reentry_hook = nullptr;
        hook();
    }
#endif

    if (mode == StackCaptureMode::Fast) {
#if defined(MALLOC_HOOK_HAVE_COMPILER_UNWIND) && MALLOC_HOOK_HAVE_COMPILER_UNWIND
        record = CaptureWithCompiler(
                mode, StackCaptureBackend::CompilerUnwind, max_frames, skipped_frames);
#else
        record.backend = StackCaptureBackend::Fallback;
        record.capture_state = StackCaptureState::Error;
        record.terminal_error = 3;
#endif
    } else {
#if defined(MALLOC_HOOK_TARGET_OS_ANDROID) && \
        defined(MALLOC_HOOK_ENABLE_LEGACY_UNWINDSTACK_ADAPTER)
        std::vector<uintptr_t> frames;
        std::vector<unwindstack::FrameData> info;
        const size_t skip_limit = std::min(skipped_frames, kMaxAsyncRawFrames);
        const size_t output_limit = std::min(max_frames, kMaxAsyncRawFrames);
        const size_t capture_limit =
                std::min(kMaxAsyncRawFrames, output_limit + skip_limit);
        const auto error = Unwind(&frames, &info, capture_limit);
        record = BuildCapturedRecord(
                mode, StackCaptureBackend::AndroidUnwindstack,
                static_cast<uint8_t>(error), frames, max_frames, skipped_frames);
#elif defined(MALLOC_HOOK_TARGET_OS_LINUX)
        record = CaptureLinuxNative(mode, max_frames, skipped_frames);
#elif defined(MALLOC_HOOK_TARGET_OS_OHOS)
#if defined(MALLOC_HOOK_HAVE_COMPILER_UNWIND) && MALLOC_HOOK_HAVE_COMPILER_UNWIND
        // No OHOS-specific Accurate backend is shipped yet. Preserve the
        // requested mode while identifying the compiler fallback truthfully.
        record = CaptureWithCompiler(
                mode, StackCaptureBackend::CompilerUnwind, max_frames, skipped_frames);
#else
        record.backend = StackCaptureBackend::Fallback;
        record.capture_state = StackCaptureState::Error;
        record.terminal_error = 3;
#endif
#else
        record.backend = StackCaptureBackend::Fallback;
        record.capture_state = StackCaptureState::Error;
        record.terminal_error = 4;
#endif
    }
    return record;
}

#if defined(MALLOC_HOOK_ENABLE_LEGACY_UNWINDSTACK_ADAPTER)
unwindstack::ErrorCode Unwind(
        std::vector<uintptr_t>* frames, std::vector<unwindstack::FrameData>* frame_info,
        size_t max_frames) {
#if !defined(MALLOC_HOOK_TARGET_OS_ANDROID)
    frames->clear();
    frame_info->clear();
    (void)max_frames;
    return unwindstack::ERROR_UNSUPPORTED;
#else
    auto& unwinder = LocalUnwinder();
    unwindstack::AndroidUnwinderData data(max_frames);
    if (!unwinder.Unwind(data)) {
        frames->clear();
        frame_info->clear();
    } else {
        frames->resize(data.frames.size());
        for (const auto& frame : data.frames) {
            frames->at(frame.num) = frame.pc;
        }
        *frame_info = std::move(data.frames);
    }

    if (frames->empty()) {
        DebugLog("alloc_hook: unwind produced empty frames\n");
    }

    return data.error.code;
#endif
}
#endif
