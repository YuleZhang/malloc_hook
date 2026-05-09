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
#include <stdint.h>
#include <cstring>
#include <unistd.h>

#if defined(__linux__) && !defined(__BIONIC__)
#include <dlfcn.h>
#include <execinfo.h>
#endif

#include <mutex>
#include <string>
#include <vector>
#include "unwindstack/Error.h"

#include <android-base/stringprintf.h>
#include <unwindstack/Arch.h>
#include <unwindstack/AndroidUnwinder.h>
#include <unwindstack/Memory.h>
#include <unwindstack/Maps.h>
#include <unwindstack/Unwinder.h>

#include "UnwindBacktrace.h"

#if defined(__linux__) && !defined(__BIONIC__)
namespace {

constexpr size_t kLinuxBacktraceExtraFrames = 16;

unwindstack::ArchEnum CurrentArch() {
#if defined(__aarch64__)
    return unwindstack::ARCH_ARM64;
#elif defined(__arm__)
    return unwindstack::ARCH_ARM;
#elif defined(__x86_64__)
    return unwindstack::ARCH_X86_64;
#elif defined(__i386__)
    return unwindstack::ARCH_X86;
#elif defined(__mips64)
    return unwindstack::ARCH_MIPS64;
#elif defined(__mips__)
    return unwindstack::ARCH_MIPS;
#else
    return unwindstack::ARCH_UNKNOWN;
#endif
}

bool ContainsSubstring(const char* haystack, const char* needle) {
    return haystack != nullptr && needle != nullptr &&
           std::strstr(haystack, needle) != nullptr;
}

bool ShouldSkipLinuxBacktraceFrame(uintptr_t pc) {
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(pc), &info) == 0) {
        return false;
    }

    if (ContainsSubstring(info.dli_fname, "liballoc_hook.so")) {
        return true;
    }

    return ContainsSubstring(info.dli_sname, "backtrace") ||
           ContainsSubstring(info.dli_sname, "_Unwind_Backtrace") ||
           ContainsSubstring(info.dli_sname, "__gnu_Unwind_Find_exidx");
}

void PopulateFrameFromDladdr(uintptr_t pc, unwindstack::FrameData* frame) {
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(pc), &info) == 0) {
        return;
    }

    uint64_t base = reinterpret_cast<uint64_t>(info.dli_fbase);
    if (info.dli_fname != nullptr && info.dli_fname[0] != '\0') {
        frame->map_info =
                unwindstack::MapInfo::Create(base, base + 1, 0, 0, info.dli_fname);
    }
    if (base != 0 && pc >= base) {
        frame->rel_pc = pc - base;
    }
    if (info.dli_sname != nullptr && info.dli_sname[0] != '\0') {
        frame->function_name = info.dli_sname;
    }
    if (info.dli_saddr != nullptr) {
        uint64_t symbol_pc = reinterpret_cast<uint64_t>(info.dli_saddr);
        if (pc >= symbol_pc) {
            frame->function_offset = pc - symbol_pc;
        }
    }
}

unwindstack::ErrorCode UnwindWithExecInfo(
        std::vector<uintptr_t>* frames, std::vector<unwindstack::FrameData>* frame_info,
        size_t max_frames) {
    if (max_frames == 0) {
        frames->clear();
        frame_info->clear();
        return unwindstack::ERROR_INVALID_PARAMETER;
    }

    const size_t capture_limit = max_frames + kLinuxBacktraceExtraFrames;
    std::vector<void*> raw_frames(capture_limit);
    int frame_count = backtrace(raw_frames.data(), static_cast<int>(raw_frames.size()));
    if (frame_count <= 0) {
        frames->clear();
        frame_info->clear();
        return unwindstack::ERROR_UNWIND_INFO;
    }

    [[clang::no_destroy]] static std::mutex maps_mutex;
    [[clang::no_destroy]] static unwindstack::LocalUpdatableMaps maps;
    [[clang::no_destroy]] static std::shared_ptr<unwindstack::Memory> process_memory =
            unwindstack::Memory::CreateProcessMemoryCached(getpid());
    [[clang::no_destroy]] static bool maps_initialized = maps.Parse();

    std::lock_guard<std::mutex> lock(maps_mutex);
    if (!maps_initialized) {
        maps_initialized = maps.Parse();
    } else {
        maps.Reparse();
    }

    frames->clear();
    frame_info->clear();
    frames->reserve(max_frames);
    frame_info->reserve(max_frames);

    auto append_frame = [&](uintptr_t pc) {
        uint64_t adjusted_pc = pc > 0 ? pc - 1 : 0;
        unwindstack::FrameData frame = unwindstack::Unwinder::BuildFrameFromPcOnly(
                adjusted_pc, CurrentArch(), &maps, nullptr, process_memory, true);
        frame.num = frame_info->size();
        frame.pc = adjusted_pc;
        if (frame.rel_pc == 0) {
            frame.rel_pc = adjusted_pc;
        }
        if (frame.map_info == nullptr || frame.function_name.empty()) {
            PopulateFrameFromDladdr(adjusted_pc, &frame);
        }
        frames->push_back(frame.pc);
        frame_info->push_back(std::move(frame));
    };

    for (int i = 0; i < frame_count && frame_info->size() < max_frames; ++i) {
        uintptr_t pc = reinterpret_cast<uintptr_t>(raw_frames[static_cast<size_t>(i)]);
        if (ShouldSkipLinuxBacktraceFrame(pc)) {
            continue;
        }
        append_frame(pc);
    }

    if (frame_info->empty()) {
        for (int i = 0; i < frame_count && frame_info->size() < max_frames; ++i) {
            uintptr_t pc = reinterpret_cast<uintptr_t>(raw_frames[static_cast<size_t>(i)]);
            append_frame(pc);
        }
    }

    if (frame_info->empty()) {
        return unwindstack::ERROR_UNWIND_INFO;
    }
    if (frame_info->size() == max_frames &&
        static_cast<size_t>(frame_count) > max_frames) {
        return unwindstack::ERROR_MAX_FRAMES_EXCEEDED;
    }
    return unwindstack::ERROR_NONE;
}

}  // namespace
#endif

unwindstack::ErrorCode Unwind(
        std::vector<uintptr_t>* frames, std::vector<unwindstack::FrameData>* frame_info,
        size_t max_frames) {
#if defined(__linux__) && !defined(__BIONIC__)
    return UnwindWithExecInfo(frames, frame_info, max_frames);
#else
    [[clang::no_destroy]] static unwindstack::AndroidLocalUnwinder unwinder(
            std::vector<std::string>{"liballoc_hook.so"}, {},
            std::vector<std::string>{
                    "_Z24__init_additional_stacksP18pthread_internal_t",
                    "_Z25__allocate_thread_mappingmm"});
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
    return data.error.code;
#endif
}
