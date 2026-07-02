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

#include <cxxabi.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "unwindstack/Error.h"

#if defined(__MUSL__)
#include <execinfo.h>
#endif

#include <android-base/stringprintf.h>
#include <unwindstack/AndroidUnwinder.h>
#include <unwindstack/Unwinder.h>

#include "UnwindBacktrace.h"

namespace {

unwindstack::AndroidLocalUnwinder& LocalUnwinder() {
    [[clang::no_destroy]] static unwindstack::AndroidLocalUnwinder unwinder(
            std::vector<std::string>{"liballoc_hook.so"}, {},
            std::vector<std::string>{
                    "_Z24__init_additional_stacksP18pthread_internal_t",
                    "_Z25__allocate_thread_mappingmm"});
    return unwinder;
}

bool IsAllocHookFrame(const unwindstack::FrameData& frame) {
    auto map_info = frame.map_info;
    return map_info != nullptr && strstr(map_info->name().c_str(), "liballoc_hook.so") != nullptr;
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

#if defined(__MUSL__)
bool UnwindWithExecinfo(
        unwindstack::AndroidLocalUnwinder& unwinder, std::vector<uintptr_t>* frames,
        std::vector<unwindstack::FrameData>* frame_info, size_t max_frames) {
    if (max_frames == 0) {
        return false;
    }

    unwindstack::ErrorData error;
    if (!unwinder.Initialize(error)) {
        return false;
    }

    constexpr size_t kExtraFrames = 16;
    size_t capture_frames = std::min(max_frames + kExtraFrames, static_cast<size_t>(256));
    std::vector<void*> pcs(capture_frames);
    int num_pcs = backtrace(pcs.data(), static_cast<int>(pcs.size()));
    if (num_pcs <= 0) {
        DebugLog("alloc_hook: execinfo backtrace returned no frames\n");
        return false;
    }

    frames->clear();
    frame_info->clear();
    frames->reserve(std::min(static_cast<size_t>(num_pcs), max_frames));
    frame_info->reserve(std::min(static_cast<size_t>(num_pcs), max_frames));

    bool skipped_initial_hook_frames = false;
    for (int i = 0; i < num_pcs && frames->size() < max_frames; ++i) {
        uintptr_t pc = reinterpret_cast<uintptr_t>(pcs[static_cast<size_t>(i)]);
        if (pc == 0) {
            continue;
        }

        unwindstack::FrameData frame = unwinder.BuildFrameFromPcOnly(pc);
        frame.num = frame_info->size();

        if (!skipped_initial_hook_frames && IsAllocHookFrame(frame)) {
            continue;
        }
        skipped_initial_hook_frames = true;

        frames->push_back(static_cast<uintptr_t>(frame.pc));
        frame_info->push_back(std::move(frame));
    }

    if (frames->empty()) {
        DebugLog("alloc_hook: execinfo fallback only saw alloc_hook frames\n");
    } else {
        DebugLog("alloc_hook: execinfo fallback produced frames\n");
    }
    return !frames->empty();
}
#endif

}  // namespace

unwindstack::ErrorCode Unwind(
        std::vector<uintptr_t>* frames, std::vector<unwindstack::FrameData>* frame_info,
        size_t max_frames) {
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

#if defined(__MUSL__)
    bool usable_error = data.error.code == unwindstack::ERROR_NONE ||
                        data.error.code == unwindstack::ERROR_MAX_FRAMES_EXCEEDED ||
                        data.error.code == unwindstack::ERROR_EXIT_FUNC;
    if ((!usable_error || frames->empty()) &&
        UnwindWithExecinfo(unwinder, frames, frame_info, max_frames)) {
        return unwindstack::ERROR_NONE;
    }
    if (!usable_error && !frames->empty()) {
        return unwindstack::ERROR_NONE;
    }
#endif

    if (frames->empty()) {
        DebugLog("alloc_hook: unwind produced empty frames\n");
    }

    return data.error.code;
}
