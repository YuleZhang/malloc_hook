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
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#if defined(__aarch64__)
#include <pthread.h>
#endif
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

constexpr bool FrameRecordFitsStack(
        uintptr_t fp, uintptr_t stack_low, uintptr_t stack_high) {
    constexpr uintptr_t kFrameRecordSize = 2 * sizeof(uintptr_t);
    return (fp & 7) == 0 && fp >= stack_low && fp <= stack_high &&
            stack_high - fp >= kFrameRecordSize;
}

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
    static bool enabled = getenv("ALLOC_HOOK_DEBUG") != nullptr;
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

void CaptureWithCompiler(
        RawStackRecord* record, StackCaptureMode mode, StackCaptureBackend backend,
        size_t max_frames, size_t skipped_frames) {
    record->mode = mode;
    record->backend = backend;
    record->skipped_frames = static_cast<uint16_t>(
            std::min(skipped_frames, static_cast<size_t>(UINT16_MAX)));
    if (max_frames == 0) {
        return;
    }
    max_frames = std::min(max_frames, kMaxAsyncRawFrames);
    FastCaptureContext context{record, max_frames, skipped_frames};
    const _Unwind_Reason_Code reason = _Unwind_Backtrace(CaptureFrame, &context);
    if (record->frame_count != 0) {
        if (record->capture_state != StackCaptureState::Partial) {
            record->capture_state = reason == _URC_END_OF_STACK
                    ? StackCaptureState::Complete
                    : StackCaptureState::Partial;
        }
    } else if (reason != _URC_END_OF_STACK) {
        record->capture_state = StackCaptureState::Error;
        record->terminal_error = static_cast<uint8_t>(reason);
    }
}

}  // namespace
#endif

#if defined(MALLOC_HOOK_TARGET_OS_LINUX)
namespace {
void CaptureLinuxNative(
        RawStackRecord* record, StackCaptureMode mode, size_t max_frames,
        size_t skipped_frames) {
    record->mode = mode;
    record->backend = StackCaptureBackend::LinuxNative;
    record->skipped_frames = static_cast<uint16_t>(
            std::min(skipped_frames, static_cast<size_t>(UINT16_MAX)));

    const size_t output_limit = std::min(max_frames, kMaxAsyncRawFrames);
    if (output_limit == 0) {
        return;
    }
    const size_t skip_limit = std::min(skipped_frames, kMaxAsyncRawFrames);
    const size_t capture_limit = std::min(kMaxAsyncRawFrames, output_limit + skip_limit);
    // Left uninitialized on purpose; backtrace() reports how many it wrote.
    std::array<void*, kMaxAsyncRawFrames> captured_pcs;
    const int captured = backtrace(captured_pcs.data(), static_cast<int>(capture_limit));
    if (captured <= 0) {
        record->capture_state = StackCaptureState::Error;
        record->terminal_error = 5;
        return;
    }

    const size_t captured_count = static_cast<size_t>(captured);
    const size_t first = std::min(skip_limit, captured_count);
    const size_t frame_count = std::min(output_limit, captured_count - first);
    for (size_t i = 0; i < frame_count; ++i) {
        record->pcs[i] = reinterpret_cast<uintptr_t>(captured_pcs[first + i]);
    }
    record->frame_count = static_cast<uint16_t>(frame_count);
    if (frame_count == 0) {
        return;
    }
    if (captured_count == capture_limit) {
        record->capture_state = StackCaptureState::Partial;
        record->terminal_error = 1;
    } else {
        record->capture_state = StackCaptureState::Complete;
    }
}
}  // namespace
#endif

#if defined(__aarch64__)
namespace {

// Strip a pointer-authentication code from a return address.
//
// Encoded in the HINT space (xpaclri), so it is a NOP on cores without PAC and
// needs no runtime capability check. Using the instruction rather than a
// hand-rolled mask keeps this correct regardless of the kernel's virtual
// address size, which determines where the PAC field actually sits.
inline uintptr_t StripPointerAuth(uintptr_t pc) {
    register uintptr_t x30 __asm__("x30") = pc;
    __asm__ __volatile__("hint #7" : "+r"(x30));
    return x30;
}

struct StackBounds {
    uintptr_t low = 0;
    uintptr_t high = 0;
};

// Cached per thread: pthread_getattr_np() allocates and reads /proc for the
// main thread, which must not happen on every allocation.
thread_local StackBounds g_stack_bounds;
thread_local bool g_stack_bounds_valid = false;

bool CurrentStackBounds(StackBounds* bounds) {
    if (!g_stack_bounds_valid) {
        g_stack_bounds_valid = true;
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void* stack_addr = nullptr;
            size_t stack_size = 0;
            if (pthread_attr_getstack(&attr, &stack_addr, &stack_size) == 0 &&
                stack_addr != nullptr && stack_size != 0) {
                g_stack_bounds.low = reinterpret_cast<uintptr_t>(stack_addr);
                g_stack_bounds.high = g_stack_bounds.low + stack_size;
            }
            pthread_attr_destroy(&attr);
        }
    }
    *bounds = g_stack_bounds;
    return g_stack_bounds.low != 0;
}

void CaptureWithFramePointer(
        RawStackRecord* record, StackCaptureMode mode, size_t max_frames,
        size_t skipped_frames) {
    record->mode = mode;
    record->backend = StackCaptureBackend::FramePointer;
    record->skipped_frames = static_cast<uint16_t>(
            std::min(skipped_frames, static_cast<size_t>(UINT16_MAX)));

    const size_t output_limit = std::min(max_frames, kMaxAsyncRawFrames);
    if (output_limit == 0) {
        return;
    }
    StackBounds bounds;
    if (!CurrentStackBounds(&bounds)) {
        record->capture_state = StackCaptureState::Error;
        record->terminal_error = 6;
        return;
    }

    uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    size_t skip = skipped_frames;
    bool truncated = false;
    while (true) {
        // Every frame record must be 8-byte aligned and lie inside this
        // thread's stack, with room for both saved words.
        if (!FrameRecordFitsStack(fp, bounds.low, bounds.high)) {
            break;
        }
        const uintptr_t next_fp = reinterpret_cast<const uintptr_t*>(fp)[0];
        const uintptr_t lr = reinterpret_cast<const uintptr_t*>(fp)[1];
        if (lr != 0) {
            if (skip != 0) {
                --skip;
            } else if (record->frame_count >= output_limit) {
                truncated = true;
                break;
            } else {
                record->pcs[record->frame_count++] = StripPointerAuth(lr);
            }
        }
        // The chain must move monotonically toward the stack base; anything
        // else is a corrupt or absent frame record, not a shorter stack.
        if (next_fp <= fp) {
            break;
        }
        fp = next_fp;
    }

    if (record->frame_count == 0) {
        record->capture_state = StackCaptureState::Empty;
    } else if (truncated) {
        record->capture_state = StackCaptureState::Partial;
        record->terminal_error = 1;
    } else {
        record->capture_state = StackCaptureState::Complete;
    }
}

}  // namespace
#endif  // __aarch64__

#if defined(__aarch64__)
namespace {
bool ForceCompilerUnwindForFast() {
    static const bool forced = [] {
        const char* value = getenv("ALLOC_HOOK_FAST_UNWINDER");
        return value != nullptr && strcmp(value, "compiler") == 0;
    }();
    return forced;
}
}  // namespace
#endif

#if defined(MALLOC_HOOK_STACK_CAPTURE_TESTING)
void SetStackCaptureReentryHookForTest(StackCaptureReentryHook hook) {
    g_capture_reentry_hook = hook;
}

bool FrameRecordFitsStackForTest(
        uintptr_t fp, uintptr_t stack_low, uintptr_t stack_high) {
    return FrameRecordFitsStack(fp, stack_low, stack_high);
}

RawStackRecord BuildCapturedRecordForTest(
        StackCaptureMode mode, StackCaptureBackend backend, uint8_t terminal_error,
        const std::vector<uintptr_t>& frames, size_t max_frames, size_t skipped_frames) {
    return BuildCapturedRecord(
            mode, backend, terminal_error, frames, max_frames, skipped_frames);
}
#endif

void CaptureStackInto(
        RawStackRecord* record, StackCaptureMode mode, size_t max_frames,
        size_t skipped_frames) {
    // Reset the scalar header only. Assigning a value-initialized
    // RawStackRecord{} here would memset the 2KB pcs array on every
    // allocation, which is exactly the cost this entry point exists to avoid.
    record->capture_state = StackCaptureState::Empty;
    record->backend = StackCaptureBackend::Unknown;
    record->terminal_error = 0;
    record->module_generation = 0;
    record->frame_count = 0;
    record->mode = mode;
    record->skipped_frames = static_cast<uint16_t>(
            std::min(skipped_frames, static_cast<size_t>(UINT16_MAX)));
    if (g_capture_active) {
        record->capture_state = StackCaptureState::Error;
        record->backend = StackCaptureBackend::Fallback;
        record->terminal_error = 2;
        return;
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
#if defined(__aarch64__)
        // Prefer the frame-pointer walk on aarch64. Besides being cheaper than
        // the compiler unwinder, it avoids libgcc's CFI-driven
        // pointer-authentication path: a traced library whose unwind tables
        // disagree with libgcc drives autia1716 into an authentication failure,
        // which is a fatal SIGILL on FEAT_FPAC cores. Set
        // ALLOC_HOOK_FAST_UNWINDER=compiler to force the old backend.
        if (!ForceCompilerUnwindForFast()) {
            CaptureWithFramePointer(record, mode, max_frames, skipped_frames);
            return;
        }
#endif
#if defined(MALLOC_HOOK_HAVE_COMPILER_UNWIND) && MALLOC_HOOK_HAVE_COMPILER_UNWIND
        CaptureWithCompiler(
                record, mode, StackCaptureBackend::CompilerUnwind, max_frames,
                skipped_frames);
#else
        record->backend = StackCaptureBackend::Fallback;
        record->capture_state = StackCaptureState::Error;
        record->terminal_error = 3;
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
        *record = BuildCapturedRecord(
                mode, StackCaptureBackend::AndroidUnwindstack,
                static_cast<uint8_t>(error), frames, max_frames, skipped_frames);
#elif defined(MALLOC_HOOK_TARGET_OS_LINUX)
        CaptureLinuxNative(record, mode, max_frames, skipped_frames);
#elif defined(MALLOC_HOOK_TARGET_OS_OHOS)
#if defined(MALLOC_HOOK_HAVE_COMPILER_UNWIND) && MALLOC_HOOK_HAVE_COMPILER_UNWIND
        // No OHOS-specific Accurate backend is shipped yet. Preserve the
        // requested mode while identifying the compiler fallback truthfully.
        CaptureWithCompiler(
                record, mode, StackCaptureBackend::CompilerUnwind, max_frames,
                skipped_frames);
#else
        record->backend = StackCaptureBackend::Fallback;
        record->capture_state = StackCaptureState::Error;
        record->terminal_error = 3;
#endif
#else
        record->backend = StackCaptureBackend::Fallback;
        record->capture_state = StackCaptureState::Error;
        record->terminal_error = 4;
#endif
    }
}

RawStackRecord CaptureStack(
        StackCaptureMode mode, size_t max_frames, size_t skipped_frames) {
    RawStackRecord record;
    CaptureStackInto(&record, mode, max_frames, skipped_frames);
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
