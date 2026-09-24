#include <cxxabi.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "Config.h"
#include "DebugData.h"
#include "PointerData.h"
#include "UnwindBacktrace.h"

#include "android-base/stringprintf.h"
#include "unwindstack/Error.h"

constexpr size_t kBacktraceExitIndex = 0;
constexpr size_t kBacktraceEmptyIndex = 1;
constexpr size_t kDefaultPeakRecordStepBytes = 12 * 1024 * 1024;
const char* mtype[3] = {"host", "mmap", "dma"};

// Platforms whose kernel exposes an ftrace trace_marker we can write atrace-style
// events into (bionic/Android and musl/OHOS). On anything else the marker helpers
// compile to no-ops.
#if defined(__MUSL__) || defined(__ANDROID__)
#define MALLOC_HOOK_HAS_TRACE_MARKER 1
#else
#define MALLOC_HOOK_HAS_TRACE_MARKER 0
#endif

#if MALLOC_HOOK_HAS_TRACE_MARKER
// One cached, write-only fd to the kernel trace_marker, opened lazily and reused for
// every event so the alloc/free hot path does not pay an open()/close() per marker.
// A negative fd (tracefs absent or not writable by this process) makes every write a
// graceful no-op.
static int TraceMarkerFd() {
    static int fd = static_cast<int>(
            syscall(SYS_openat, AT_FDCWD, "/sys/kernel/tracing/trace_marker",
                    O_WRONLY | O_CLOEXEC, 0));
    return fd;
}

static inline void WriteTraceMarker(const char* buf, size_t len) {
    int fd = TraceMarkerFd();
    if (fd >= 0) {
        syscall(SYS_write, fd, buf, len);
    }
}

static bool AllocTraceMarkerEnabled() {
    static bool enabled = [] {
        const char* value = getenv("MALLOC_HOOK_TRACE_ALLOC");
        return value != nullptr && value[0] != '\0' && strcmp(value, "0") != 0;
    }();
    return enabled;
}

// Emit an atrace async begin ('S') / end ('F') for a single tracked allocation, so a
// Perfetto trace can render each mat's alloc->free lifetime. The event name embeds the
// pointer (unique among live allocations, so S and F pair) and the backtrace hash as
// ".h<idx>" — build_perfetto_alloc_track.py keys off that hash to attach the symbolized
// variable / call site. Only allocations that carry a backtrace (i.e. large enough to be
// tracked) are marked; the cookie is the pointer so overlapping lifetimes stay distinct.
static void WriteAllocTraceMarker(
        char phase, const void* ptr, size_t hash_index, MemType type) {
    if (!AllocTraceMarkerEnabled()) {
        return;
    }
    const int pid = static_cast<int>(syscall(SYS_getpid));
    const unsigned long long cookie =
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ptr));
    char marker[160];
    int length = snprintf(
            marker, sizeof(marker), "%c|%d|memory_%s@%p.h%zu|%llu", phase, pid,
            mtype[type], ptr, hash_index, cookie);
    if (length > 0 && static_cast<size_t>(length) < sizeof(marker)) {
        WriteTraceMarker(marker, static_cast<size_t>(length));
    }
}
#else
static inline bool AllocTraceMarkerEnabled() { return false; }
static inline void WriteAllocTraceMarker(char, const void*, size_t, MemType) {}
#endif

static bool PeakTraceMarkerEnabled() {
#if MALLOC_HOOK_HAS_TRACE_MARKER
    // Peak markers share the single MALLOC_HOOK_TRACE_ALLOC switch with the
    // per-allocation lifetime markers: one env turns the whole Perfetto marker
    // stream on or off.
    static bool enabled = [] {
        const char* value = getenv("MALLOC_HOOK_TRACE_ALLOC");
        return value != nullptr && value[0] != '\0' && strcmp(value, "0") != 0;
    }();
    return enabled;
#else
    return false;
#endif
}

static void WritePeakTraceMarker(size_t host_bytes, size_t dma_bytes, size_t total_bytes) {
#if MALLOC_HOOK_HAS_TRACE_MARKER
    if (!PeakTraceMarkerEnabled()) {
        return;
    }

    int fd = static_cast<int>(
            syscall(SYS_openat, AT_FDCWD, "/sys/kernel/tracing/trace_marker",
                    O_WRONLY | O_CLOEXEC, 0));
    if (fd < 0) {
        return;
    }

    const int pid = static_cast<int>(syscall(SYS_getpid));
    // Report the peak in MiB — the natural unit for a memory budget and what the
    // Perfetto post-processor surfaces on the "Memory Top Allocations" track.
    const double total_mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    const double host_mb = static_cast<double>(host_bytes) / (1024.0 * 1024.0);
    const double dma_mb = static_cast<double>(dma_bytes) / (1024.0 * 1024.0);
    char marker[256];
    int length = snprintf(
            marker, sizeof(marker),
            "B|%d|malloc_hook_peak_snapshot total_mb=%.1f host_mb=%.1f "
            "dma_mb=%.1f",
            pid, total_mb, host_mb, dma_mb);
    if (length > 0 && static_cast<size_t>(length) < sizeof(marker)) {
        syscall(SYS_write, fd, marker, static_cast<size_t>(length));
        static constexpr char kTraceEnd[] = "E";
        syscall(SYS_write, fd, kTraceEnd, sizeof(kTraceEnd) - 1);
    }

    length = snprintf(
            marker, sizeof(marker), "C|%d|malloc_hook_peak_total_mb|%zu", pid,
            total_bytes >> 20);
    if (length > 0 && static_cast<size_t>(length) < sizeof(marker)) {
        syscall(SYS_write, fd, marker, static_cast<size_t>(length));
    }
    length = snprintf(
            marker, sizeof(marker), "C|%d|malloc_hook_peak_host_mb|%zu", pid,
            host_bytes >> 20);
    if (length > 0 && static_cast<size_t>(length) < sizeof(marker)) {
        syscall(SYS_write, fd, marker, static_cast<size_t>(length));
    }
    length = snprintf(
            marker, sizeof(marker), "C|%d|malloc_hook_peak_dma_mb|%zu", pid,
            dma_bytes >> 20);
    if (length > 0 && static_cast<size_t>(length) < sizeof(marker)) {
        syscall(SYS_write, fd, marker, static_cast<size_t>(length));
    }

    syscall(SYS_close, fd);
#endif
}

static size_t ParsePeakStepBytes() {
    const char* value = getenv("DUMP_PEAK_STEP_MB");
    if (value == nullptr) {
        return kDefaultPeakRecordStepBytes;
    }
    char* end = nullptr;
    long step_mb = strtol(value, &end, 10);
    if (end == value || *end != '\0' || step_mb < 0) {
        return kDefaultPeakRecordStepBytes;
    }
    return static_cast<size_t>(step_mb) * 1024 * 1024;
}

static inline bool ShouldBacktraceAllocSize(size_t size_bytes) {
    static bool only_backtrace_specific_sizes =
            g_debug->config().options() & BACKTRACE_SPECIFIC_SIZES;
    if (!only_backtrace_specific_sizes) {
        return true;
    }

    static size_t min_size_bytes = g_debug->config().backtrace_min_size_bytes();
    static size_t max_size_bytes = g_debug->config().backtrace_max_size_bytes();
    return size_bytes >= min_size_bytes && size_bytes <= max_size_bytes;
}

bool PointerData::Initialize(const Config& config) {
    pointers_.clear();
    key_to_index_.clear();
    frames_.clear();
    backtraces_info_.clear();
    peak_list.clear();
    // A hash index of kBacktraceEmptyIndex indicates that we tried to get
    // a backtrace, but there was nothing recorded.
    cur_hash_index_ = kBacktraceEmptyIndex + 1;
    current_used = current_host = current_dma = 0;
    peak_tot = peak_host = peak_dma = 0;
    next_peak_record_threshold_ = config.backtrace_dump_peak_val();
    peak_record_step_bytes_ = ParsePeakStepBytes();

    return true;
}

void PointerData::Add(const void* ptr, size_t pointer_size, MemType type) {
    size_t hash_index =
            AddBacktrace(g_debug->config().backtrace_frames(), pointer_size);
    size_t replaced_hash_index = kBacktraceEmptyIndex;
    MemType replaced_type = HOST;

    {
        std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uintptr_t mangled_ptr = ManglePointer(reinterpret_cast<uintptr_t>(ptr));
        auto existing = pointers_.find(mangled_ptr);
        if (existing != pointers_.end()) {
            current_used -= existing->second.size;
            size_t* replaced_current =
                    (existing->second.mem_type == DMA) ? &current_dma : &current_host;
            *replaced_current -= existing->second.size;
            replaced_hash_index = existing->second.hash_index;
            replaced_type = existing->second.mem_type;
        }

        pointers_[mangled_ptr] = PointerInfoType{pointer_size, hash_index, type, tv};
        current_used += pointer_size;
        size_t* current = (type == DMA) ? &current_dma : &current_host;
        size_t* peak = (type == DMA) ? &peak_dma : &peak_host;
        *current += pointer_size;
        if (*current > *peak) {
            *peak = *current;
        }
        if (peak_tot < current_used) {
            peak_tot = current_used;

            if ((g_debug->config().options() & RECORD_MEMORY_PEAK) &&
                peak_tot > next_peak_record_threshold_) {
                std::lock_guard<std::mutex> frame_guard(frame_mutex_);
                std::vector<ListInfoType> next_peak_list;
                GetUniqueList(&next_peak_list, false);
                if (!next_peak_list.empty()) {
                    peak_list = std::move(next_peak_list);
                    WritePeakTraceMarker(current_host, current_dma, current_used);
                    // Climb mode: keep this rung's snapshot so teardown can write a
                    // report per step. Capped so a pathological run can't grow forever.
                    if (peak_record_step_bytes_ != 0 && peak_snapshots_.size() < 256) {
                        peak_snapshots_.emplace_back(current_used, peak_list);
                    }
                    if (peak_record_step_bytes_ == 0) {
                        next_peak_record_threshold_ = peak_tot;
                    } else {
                        next_peak_record_threshold_ = peak_tot + peak_record_step_bytes_;
                    }
                }
            }
        }
    }

    // A replaced pointer means the old allocation is gone: close its lifetime slice
    // first, then open one for the new allocation. Both are gated on carrying a
    // backtrace (tracked large allocations only).
    if (replaced_hash_index > kBacktraceEmptyIndex) {
        WriteAllocTraceMarker('F', ptr, replaced_hash_index, replaced_type);
    }
    if (hash_index > kBacktraceEmptyIndex) {
        WriteAllocTraceMarker('S', ptr, hash_index, type);
    }

    RemoveBacktrace(replaced_hash_index);
}

size_t PointerData::AddBacktrace(size_t num_frames, size_t size_bytes) {
    if (!ShouldBacktraceAllocSize(size_bytes)) {
        return kBacktraceEmptyIndex;
    }

    std::vector<uintptr_t> frames;
    std::vector<unwindstack::FrameData> frames_info;
    if (g_debug->config().options() & BACKTRACE) {
        switch (Unwind(&frames, &frames_info, num_frames)) {
            case unwindstack::ERROR_NONE:
            case unwindstack::ERROR_MAX_FRAMES_EXCEEDED:
                break;
            case unwindstack::ERROR_EXIT_FUNC:
                return kBacktraceExitIndex;
            default:
                return kBacktraceEmptyIndex;
        }
    } else {
        return kBacktraceEmptyIndex;
    }

    if (frames.empty()) {
        return kBacktraceEmptyIndex;
    }

    FrameKeyType key{.num_frames = frames.size(), .frames = frames.data()};
    size_t hash_index;
    std::lock_guard<std::mutex> frame_guard(frame_mutex_);
    auto entry = key_to_index_.find(key);
    if (entry == key_to_index_.end()) {
        hash_index = cur_hash_index_++;
        key.frames = frames.data();
        key_to_index_.emplace(key, hash_index);

        frames_.emplace(
                hash_index,
                FrameInfoType{.references = 1, .frames = std::move(frames)});
        if (g_debug->config().options() & BACKTRACE) {
            backtraces_info_.emplace(
                    hash_index,
                    std::make_shared<std::vector<unwindstack::FrameData>>(frames_info));
        }
    } else {
        hash_index = entry->second;
        FrameInfoType* frame_info = &frames_[hash_index];
        frame_info->references++;
    }
    return hash_index;
}

void PointerData::Remove(const void* ptr) {
    size_t hash_index;
    MemType removed_type = HOST;
    {
        std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
        uintptr_t mangled_ptr = ManglePointer(reinterpret_cast<uintptr_t>(ptr));
        auto entry = pointers_.find(mangled_ptr);
        if (entry == pointers_.end()) {
            // No tracked pointer.
            return;
        }
        current_used -= entry->second.size;
        size_t* target = (entry->second.mem_type == DMA) ? &current_dma : &current_host;
        *target -= entry->second.size;
        hash_index = entry->second.hash_index;
        removed_type = entry->second.mem_type;
        pointers_.erase(mangled_ptr);
    }

    // Close this allocation's lifetime slice (tracked large allocations only). This is
    // where a mat's release time lands on the Perfetto timeline.
    if (hash_index > kBacktraceEmptyIndex) {
        WriteAllocTraceMarker('F', ptr, hash_index, removed_type);
    }

    RemoveBacktrace(hash_index);
}

void PointerData::RemoveBacktrace(size_t hash_index) {
    if (hash_index <= kBacktraceEmptyIndex) {
        return;
    }

    std::lock_guard<std::mutex> frame_guard(frame_mutex_);
    auto frame_entry = frames_.find(hash_index);
    if (frame_entry == frames_.end()) {
        // does not have matching frame data.
        return;
    }
    FrameInfoType* frame_info = &frame_entry->second;
    if (--frame_info->references == 0 &&
        !(g_debug->config().options() & RECORD_MEMORY_PEAK)) {
        FrameKeyType key{
                .num_frames = frame_info->frames.size(),
                .frames = frame_info->frames.data()};
        key_to_index_.erase(key);
        frames_.erase(hash_index);
        if (g_debug->config().options() & BACKTRACE) {
            backtraces_info_.erase(hash_index);
        }
    }
}

void PointerData::GetList(
        std::vector<ListInfoType>* list, bool only_with_backtrace, Pred pred) {
    for (auto& entry : pointers_) {
        // 舍弃没有堆栈的 pointer
        size_t hash_index = entry.second.hash_index;
        if (hash_index <= kBacktraceEmptyIndex && only_with_backtrace) {
            continue;
        }

        uintptr_t pointer = DemanglePointer(entry.first);
        FrameInfoType* frame_info = nullptr;
        std::shared_ptr<std::vector<unwindstack::FrameData>> backtrace_info;
        if (hash_index > kBacktraceEmptyIndex) {
            auto frame_entry = frames_.find(hash_index);
            if (frame_entry != frames_.end()) {
                frame_info = &frame_entry->second;
            }
            auto backtrace_entry = backtraces_info_.find(hash_index);
            if (backtrace_entry != backtraces_info_.end()) {
                backtrace_info = backtrace_entry->second;
            }
        }

        list->emplace_back(ListInfoType{
                pointer, 1, entry.second.RealSize(), entry.second.mem_type, frame_info,
                std::move(backtrace_info), entry.second.alloc_time,
                entry.second.hash_index});
    }

    std::sort(list->begin(), list->end(), pred);
}

void PointerData::GetUniqueList(
        std::vector<ListInfoType>* list, bool only_with_backtrace) {
    // Sort by the size of the allocation.
    GetList(list, only_with_backtrace,
            [](const ListInfoType& a, const ListInfoType& b) {
                if (a.size != b.size)
                    return a.size > b.size;

                // Put pointers with no backtrace last.
                FrameInfoType* a_frame = a.frame_info;
                FrameInfoType* b_frame = b.frame_info;
                if (a_frame == nullptr && b_frame != nullptr) {
                    return false;
                } else if (a_frame != nullptr && b_frame == nullptr) {
                    return true;
                } else if (a_frame == nullptr && b_frame == nullptr) {
                    return a.pointer < b.pointer;
                }

                // Put the pointers with longest backtrace first.
                if (a_frame->frames.size() != b_frame->frames.size()) {
                    return a_frame->frames.size() > b_frame->frames.size();
                }

                // Last sort by pointer.
                return a.pointer < b.pointer;
            });

    // Remove duplicates of size/backtraces.
    for (auto iter = list->begin(); iter != list->end();) {
        auto dup_iter = iter + 1;
        size_t size = iter->size;
        FrameInfoType* frame_info = iter->frame_info;
        for (; dup_iter != list->end(); ++dup_iter) {
            if (size != dup_iter->size || frame_info != dup_iter->frame_info ||
                iter->mem_type != dup_iter->mem_type) {
                break;
            }
            iter->num_allocations++;
        }
        iter = list->erase(iter + 1, dup_iter);
    }
}

// Lock-free: format an already-built allocation list to a fd. The caller owns any
// locking (DumpLiveToFile holds the mutexes; DumpStepReports runs at teardown). Safe
// against re-entrancy because debug calls are disabled on every path that reaches it.
static void WriteListToFd(int fd, const std::vector<ListInfoType>& list) {
    size_t host_use = 0, dma_use = 0;
    for (const auto& it : list) {
        size_t bt_size = it.size * it.num_allocations;
        it.mem_type == DMA ? dma_use += bt_size : host_use += bt_size;
    }

    dprintf(fd,
            "current host used: %fMB, current dma used %fMB, current total peak "
            "used: %fMB\n",
            host_use / 1024.0 / 1024.0, dma_use / 1024.0 / 1024.0,
            (host_use + dma_use) / 1024.0 / 1024.0);
    dprintf(fd,
            "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
            "+++++++++++++++\n\n");
    for (const auto& info : list) {
        // 解析时间
        struct tm* local_time = localtime(&info.alloc_time.tv_sec);
        char formatted_time[20];
        strftime(
                formatted_time, sizeof(formatted_time), "%Y-%m-%d %H:%M:%S",
                local_time);

        dprintf(fd,
                "alloc_size:%fKB \t alloc_type:%s \t alloc_num:%zu \t "
                "hash_index:%zu \t alloc_time:%s.%zu\n",
                info.size / 1024.0, mtype[info.mem_type], info.num_allocations,
                info.hash_index, formatted_time, info.alloc_time.tv_usec / 1000);
        if (info.backtrace_info == nullptr || info.backtrace_info->empty()) {
            dprintf(fd, "#00 <backtrace unavailable>\n\n");
            continue;
        }
        for (size_t i = 0; i < info.backtrace_info->size(); ++i) {
            const unwindstack::FrameData* frame = &info.backtrace_info->at(i);
            auto map_info = frame->map_info;

            std::string line =
                    android::base::StringPrintf("#%0zd %" PRIx64 " ", i, frame->rel_pc);
            // so path
            if (map_info == nullptr) {
                line += "<unknown>";
            } else if (map_info->name().empty()) {
                line += android::base::StringPrintf(
                        "<anonymous:%" PRIx64 ">", map_info->start());
            } else {
                line += map_info->name();
            }

            if (!frame->function_name.empty()) {
                line += " (";
                char* demangled_name = abi::__cxa_demangle(
                        frame->function_name.c_str(), nullptr, nullptr, nullptr);
                if (demangled_name != nullptr) {
                    line += demangled_name;
                    free(demangled_name);
                } else {
                    line += frame->function_name;
                }
                if (frame->function_offset != 0) {
                    line += "+" + std::to_string(frame->function_offset);
                }
                line += ")";
            }
            dprintf(fd, "%s\n", line.c_str());
        }
        dprintf(fd, "\n");
    }
}


void PointerData::DumpLiveToFile(int fd, bool dump_peak) {
    std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
    std::lock_guard<std::mutex> frame_guard(frame_mutex_);

    std::vector<ListInfoType> list;
    if ((g_debug->config().options() & RECORD_MEMORY_PEAK) && dump_peak) {
        list = peak_list;
    } else {
        // Sort by the time of the allocation.
        GetList(&list, false, [](const ListInfoType& a, const ListInfoType& b) {
            return a.alloc_time < b.alloc_time;
        });
    }

    WriteListToFd(fd, list);
}


// Write one report per accumulated climb-mode snapshot. Runs at teardown where the
// tracker is already quiesced, so no locking and no hot-path cost. Each file is named
// by that rung's peak size so the set reads as a climb (…_412MB, …_436MB, …).
void PointerData::DumpStepReports(const char* prefix) {
    if (peak_snapshots_.empty() || prefix == nullptr) {
        return;
    }
    for (const auto& snap : peak_snapshots_) {
        size_t total_mb = snap.first >> 20;
        char path[512];
        int n = snprintf(path, sizeof(path), "%s.step.%zuMB.txt", prefix, total_mb);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) {
            continue;
        }
        int fd = static_cast<int>(syscall(
                SYS_openat, AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
        if (fd < 0) {
            continue;
        }
        WriteListToFd(fd, snap.second);
        syscall(SYS_close, fd);
    }
}

void PointerData::DumpPeakInfo() {
    std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
    printf("\n+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
           "++++++++++++++++\n");
    printf("host peak used: %fMB, dma peak used %fMB, total peak used: %fMB\n\n",
           peak_host / 1024.0 / 1024.0, peak_dma / 1024.0 / 1024.0,
           peak_tot / 1024.0 / 1024.0);
}

void PointerData::GetCurrentUsage(size_t* host_bytes, size_t* dma_bytes) {
    std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
    if (host_bytes != nullptr) {
        *host_bytes = current_host;
    }
    if (dma_bytes != nullptr) {
        *dma_bytes = current_dma;
    }
}
