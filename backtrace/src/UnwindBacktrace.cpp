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
#include <android-base/threads.h>
#include <elf.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#endif

#include <mutex>
#include <string>
#include <vector>
#include "unwindstack/Error.h"

#include <android-base/stringprintf.h>
#include <unwindstack/Arch.h>
#include <unwindstack/AndroidUnwinder.h>
#include <unwindstack/DexFiles.h>
#include <unwindstack/JitDebug.h>
#include <unwindstack/Memory.h>
#include <unwindstack/Maps.h>
#include <unwindstack/MachineArm64.h>
#include <unwindstack/RegsGetLocal.h>
#include <unwindstack/RegsArm64.h>
#include <unwindstack/Unwinder.h>
#include "bionic/macros.h"

#include "UnwindBacktrace.h"

#if defined(__linux__) && !defined(__BIONIC__)
namespace {

constexpr size_t kLinuxBacktraceExtraFrames = 16;
constexpr size_t kLinuxSearchLibCount = 2;
constexpr const char* kLinuxSearchLibs[kLinuxSearchLibCount] = {"libart.so", "libartd.so"};

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

void NormalizeLinuxLocalRegs(unwindstack::Regs* regs) {
#if defined(__aarch64__)
    if (regs == nullptr || regs->Arch() != unwindstack::ARCH_ARM64) {
        return;
    }

    auto* arm64 = static_cast<unwindstack::RegsArm64*>(regs);
    arm64->set_pc(untag_address(arm64->pc()));
    arm64->set_sp(untag_address(arm64->sp()));
    (*arm64)[unwindstack::ARM64_REG_LR] =
            untag_address((*arm64)[unwindstack::ARM64_REG_LR]);
#else
    (void)regs;
#endif
}

bool FallbackToMappedReturnAddress(unwindstack::Regs* regs, unwindstack::Maps* maps) {
#if defined(__aarch64__)
    if (regs == nullptr || maps == nullptr || regs->Arch() != unwindstack::ARCH_ARM64) {
        return false;
    }

    auto* arm64 = static_cast<unwindstack::RegsArm64*>(regs);
    const uint64_t lr = untag_address((*arm64)[unwindstack::ARM64_REG_LR]);
    if (lr == 0 || maps->Find(lr) == nullptr) {
        return false;
    }
    arm64->set_pc(lr);
    (*arm64)[unwindstack::ARM64_REG_LR] = lr;
    return true;
#else
    (void)regs;
    (void)maps;
    return false;
#endif
}

unwindstack::ErrorCode UnwindWithLocalRegs(
        std::vector<uintptr_t>* frames, std::vector<unwindstack::FrameData>* frame_info,
        size_t max_frames) {
    if (max_frames == 0) {
        frames->clear();
        frame_info->clear();
        return unwindstack::ERROR_INVALID_PARAMETER;
    }

    [[clang::no_destroy]] static std::mutex maps_mutex;
    [[clang::no_destroy]] static unwindstack::LocalUpdatableMaps maps;
    [[clang::no_destroy]] static std::shared_ptr<unwindstack::Memory> process_memory =
            unwindstack::Memory::CreateProcessMemoryThreadCached(getpid());
    [[clang::no_destroy]] static std::unique_ptr<unwindstack::JitDebug> jit_debug =
            unwindstack::CreateJitDebug(
                    CurrentArch(), process_memory,
                    std::vector<std::string>{kLinuxSearchLibs, kLinuxSearchLibs + kLinuxSearchLibCount});
    [[clang::no_destroy]] static std::unique_ptr<unwindstack::DexFiles> dex_files =
            unwindstack::CreateDexFiles(
                    CurrentArch(), process_memory,
                    std::vector<std::string>{kLinuxSearchLibs, kLinuxSearchLibs + kLinuxSearchLibCount});
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

    std::unique_ptr<unwindstack::Regs> regs(unwindstack::Regs::CreateFromLocal());
    if (regs == nullptr) {
        return unwindstack::ERROR_BAD_ARCH;
    }
    unwindstack::RegsGetLocal(regs.get());
    NormalizeLinuxLocalRegs(regs.get());
    if (maps.Find(regs->pc()) == nullptr && !FallbackToMappedReturnAddress(regs.get(), &maps)) {
        regs->fallback_pc();
        NormalizeLinuxLocalRegs(regs.get());
        FallbackToMappedReturnAddress(regs.get(), &maps);
    }

    unwindstack::Unwinder unwinder(
            max_frames + kLinuxBacktraceExtraFrames, &maps, regs.get(), process_memory);
    unwinder.SetJitDebug(jit_debug.get());
    unwinder.SetDexFiles(dex_files.get());
    unwinder.Unwind();
    std::vector<unwindstack::FrameData> raw_frames = unwinder.ConsumeFrames();

    auto append_frame = [&](unwindstack::FrameData frame) {
        if (frame.map_info == nullptr || frame.function_name.empty()) {
            PopulateFrameFromDladdr(frame.pc, &frame);
        }
        frame.num = frame_info->size();
        if (frame.rel_pc == 0) {
            frame.rel_pc = frame.pc;
        }
        frames->push_back(frame.pc);
        frame_info->push_back(std::move(frame));
    };

    for (auto& frame : raw_frames) {
        if (frame_info->size() >= max_frames) {
            break;
        }
        if (ShouldSkipLinuxBacktraceFrame(frame.pc)) {
            continue;
        }
        append_frame(std::move(frame));
    }

    if (frame_info->empty()) {
        for (auto& frame : raw_frames) {
            if (frame_info->size() >= max_frames) {
                break;
            }
            append_frame(std::move(frame));
        }
    }

    if (frame_info->empty()) {
        return unwinder.LastErrorCode();
    }

    const unwindstack::ErrorCode last_error = unwinder.LastErrorCode();
    if (last_error == unwindstack::ERROR_NONE ||
        last_error == unwindstack::ERROR_MAX_FRAMES_EXCEEDED) {
        return last_error;
    }

    // On Linux/glibc, local unwinding frequently reaches a terminal unmapped frame
    // after already collecting useful frames. Preserve the captured stack instead of
    // discarding it as a hard failure.
    return unwindstack::ERROR_NONE;
}

}  // namespace
#endif

unwindstack::ErrorCode Unwind(
        std::vector<uintptr_t>* frames, std::vector<unwindstack::FrameData>* frame_info,
        size_t max_frames) {
#if defined(__linux__) && !defined(__BIONIC__)
    return UnwindWithLocalRegs(frames, frame_info, max_frames);
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
