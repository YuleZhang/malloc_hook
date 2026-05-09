#include <cxxabi.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <algorithm>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>

#include "Config.h"
#include "DebugData.h"
#include "PointerData.h"
#include "UnwindBacktrace.h"

#include "android-base/stringprintf.h"
#include "unwindstack/Error.h"

constexpr size_t kBacktraceExitIndex = 0;
constexpr size_t kBacktraceEmptyIndex = 1;
constexpr size_t kMaxRecentAdviceEvents = 64;
const char* mtype[3] = {"host", "mmap", "dma"};

namespace {

void LogPointerEvent(const char* format, ...) {
    char buffer[1024];
    va_list ap;
    va_start(ap, format);
    int written = vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    if (written <= 0) {
        return;
    }

    size_t len = static_cast<size_t>(written);
    if (len >= sizeof(buffer)) {
        len = sizeof(buffer) - 1;
    }
    ssize_t ignored = write(STDERR_FILENO, buffer, len);
    (void)ignored;
}

std::atomic<uint64_t> g_size_filter_log_count{0};
std::atomic<uint64_t> g_unwind_failure_log_count{0};

struct SmapsRegion {
    uintptr_t start = 0;
    uintptr_t end = 0;
    std::string name;
    size_t size_kb = 0;
    size_t rss_kb = 0;
    size_t anonymous_kb = 0;
    size_t anon_huge_kb = 0;
    size_t private_dirty_kb = 0;
    const ListInfoType* matched = nullptr;
    size_t matched_overlap = 0;
};

static bool ParseSmapsHeader(const char* line, SmapsRegion* region) {
    uintptr_t start = 0;
    uintptr_t end = 0;
    char perms[5] = {};
    unsigned long long offset = 0;
    char dev[32] = {};
    unsigned long inode = 0;
    char name[512] = {};
    int matched = sscanf(line,
                         "%" SCNxPTR "-%" SCNxPTR " %4s %llx %31s %lu %511[^\n]",
                         &start, &end, perms, &offset, dev, &inode, name);
    if (matched < 6) {
        return false;
    }

    region->start = start;
    region->end = end;
    region->name = matched == 7 ? name : "";
    region->size_kb = 0;
    region->rss_kb = 0;
    region->anonymous_kb = 0;
    region->anon_huge_kb = 0;
    region->private_dirty_kb = 0;
    region->matched = nullptr;
    region->matched_overlap = 0;
    return true;
}

static bool ParseKbField(const char* line, const char* prefix, size_t* value_kb) {
    if (strncmp(line, prefix, strlen(prefix)) != 0) {
        return false;
    }

    unsigned long long value = 0;
    if (sscanf(line + strlen(prefix), "%llu kB", &value) != 1) {
        return false;
    }
    *value_kb = static_cast<size_t>(value);
    return true;
}

static size_t RangeOverlapBytes(
        uintptr_t first_start, uintptr_t first_end, uintptr_t second_start,
        uintptr_t second_end) {
    const uintptr_t overlap_start = std::max(first_start, second_start);
    const uintptr_t overlap_end = std::min(first_end, second_end);
    if (overlap_end <= overlap_start) {
        return 0;
    }
    return static_cast<size_t>(overlap_end - overlap_start);
}

static bool IsInterestingSmapsRegion(const SmapsRegion& region) {
    return region.rss_kb >= 1024 &&
           (region.anonymous_kb != 0 || region.anon_huge_kb != 0 ||
            region.name == "[heap]" || region.matched != nullptr);
}

static const char* AdviceName(int advice) {
    switch (advice) {
        case MADV_DONTNEED:
            return "MADV_DONTNEED";
#ifdef MADV_FREE
        case MADV_FREE:
            return "MADV_FREE";
#endif
#ifdef MADV_POPULATE_WRITE
        case MADV_POPULATE_WRITE:
            return "MADV_POPULATE_WRITE";
#endif
        default:
            return "OTHER";
    }
}

static void DumpBacktraceLines(
        int fd, const std::shared_ptr<std::vector<unwindstack::FrameData>>& backtrace_info,
        size_t max_frames = SIZE_MAX) {
    if (!backtrace_info) {
        return;
    }

    const size_t frame_count = std::min(backtrace_info->size(), max_frames);
    for (size_t i = 0; i < frame_count; ++i) {
        const unwindstack::FrameData* frame = &backtrace_info->at(i);
        auto map_info = frame->map_info;

        std::string line =
                android::base::StringPrintf("#%0zd %" PRIx64 " ", i, frame->rel_pc);
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

    if (frame_count < backtrace_info->size()) {
        dprintf(fd, "... %zu more frames omitted ...\n", backtrace_info->size() - frame_count);
    }
}

}  // namespace

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
    recent_advice_events_.clear();
    // A hash index of kBacktraceEmptyIndex indicates that we tried to get
    // a backtrace, but there was nothing recorded.
    cur_hash_index_ = kBacktraceEmptyIndex + 1;
    current_used = current_host = current_dma = 0;
    peak_tot = peak_host = peak_dma = 0;
    advice_dontneed_calls_ = 0;
    advice_dontneed_bytes_ = 0;
    advice_free_calls_ = 0;
    advice_free_bytes_ = 0;
    advice_populate_write_calls_ = 0;
    advice_populate_write_bytes_ = 0;
    advice_other_calls_ = 0;
    advice_other_bytes_ = 0;
    advice_matched_calls_ = 0;
    advice_matched_bytes_ = 0;
    advice_unmatched_calls_ = 0;
    advice_unmatched_bytes_ = 0;
    total_add_calls_.store(0, std::memory_order_relaxed);
    total_remove_calls_.store(0, std::memory_order_relaxed);
    total_size_filtered_.store(0, std::memory_order_relaxed);
    total_unwind_success_.store(0, std::memory_order_relaxed);
    total_unwind_success_max_frames_.store(0, std::memory_order_relaxed);
    total_unwind_success_empty_frames_.store(0, std::memory_order_relaxed);
    total_unwind_exit_.store(0, std::memory_order_relaxed);
    total_unwind_fail_.store(0, std::memory_order_relaxed);
    total_unwind_fail_memory_invalid_.store(0, std::memory_order_relaxed);
    total_unwind_fail_unwind_info_.store(0, std::memory_order_relaxed);
    total_unwind_fail_unsupported_.store(0, std::memory_order_relaxed);
    total_unwind_fail_invalid_map_.store(0, std::memory_order_relaxed);
    total_unwind_fail_repeated_frame_.store(0, std::memory_order_relaxed);
    total_unwind_fail_invalid_elf_.store(0, std::memory_order_relaxed);
    total_unwind_fail_thread_missing_.store(0, std::memory_order_relaxed);
    total_unwind_fail_thread_timeout_.store(0, std::memory_order_relaxed);
    total_unwind_fail_system_call_.store(0, std::memory_order_relaxed);
    total_unwind_fail_bad_arch_.store(0, std::memory_order_relaxed);
    total_unwind_fail_maps_parse_.store(0, std::memory_order_relaxed);
    total_unwind_fail_invalid_parameter_.store(0, std::memory_order_relaxed);
    total_unwind_fail_ptrace_call_.store(0, std::memory_order_relaxed);
    total_unwind_fail_other_.store(0, std::memory_order_relaxed);
    last_unwind_error_.store(unwindstack::ERROR_NONE, std::memory_order_relaxed);

    return true;
}

void PointerData::Add(const void* ptr, size_t pointer_size, MemType type) {
    total_add_calls_.fetch_add(1, std::memory_order_relaxed);
    size_t hash_index = 0;
    hash_index = AddBacktrace(g_debug->config().backtrace_frames(), pointer_size);

    // unwind 跳过的函数，不记录其堆栈和 pointer 信息
    if (hash_index == kBacktraceExitIndex)
        return;

    std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uintptr_t mangled_ptr = ManglePointer(reinterpret_cast<uintptr_t>(ptr));
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
            peak_tot > g_debug->config().backtrace_dump_peak_val()) {
            std::lock_guard<std::mutex> frame_guard(frame_mutex_);
            peak_list.clear();
            GetUniqueList(&peak_list, true);
        }
    }
}

void PointerData::RecordAdvice(const void* addr, size_t size, int advice) {
    if (addr == nullptr || size == 0) {
        return;
    }

    std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
    switch (advice) {
        case MADV_DONTNEED:
            advice_dontneed_calls_++;
            advice_dontneed_bytes_ += size;
            break;
#ifdef MADV_FREE
        case MADV_FREE:
            advice_free_calls_++;
            advice_free_bytes_ += size;
            break;
#endif
#ifdef MADV_POPULATE_WRITE
        case MADV_POPULATE_WRITE:
            advice_populate_write_calls_++;
            advice_populate_write_bytes_ += size;
            break;
#endif
        default:
            advice_other_calls_++;
            advice_other_bytes_ += size;
            break;
    }

    AdviceEvent event;
    gettimeofday(&event.event_time, nullptr);
    event.addr = reinterpret_cast<uintptr_t>(addr);
    event.size = size;
    event.advice = advice;

    const uintptr_t advice_start = event.addr;
    const uintptr_t advice_end = advice_start + size;
    size_t best_overlap = 0;
    for (const auto& entry : pointers_) {
        if (entry.second.mem_type == HOST) {
            continue;
        }

        const uintptr_t tracked_start = DemanglePointer(entry.first);
        const uintptr_t tracked_end = tracked_start + entry.second.RealSize();
        const size_t overlap =
                RangeOverlapBytes(advice_start, advice_end, tracked_start, tracked_end);
        if (overlap == 0) {
            continue;
        }

        event.matched_regions++;
        event.matched_bytes += overlap;
        if (overlap > best_overlap) {
            best_overlap = overlap;
            event.has_best_match = true;
            event.best_match_type = entry.second.mem_type;
            event.best_match_pointer = tracked_start;
            event.best_match_size = entry.second.RealSize();
        }
    }

    if (event.matched_regions != 0) {
        advice_matched_calls_++;
        advice_matched_bytes_ += event.matched_bytes;
    } else {
        advice_unmatched_calls_++;
        advice_unmatched_bytes_ += size;
    }

    recent_advice_events_.push_back(event);
    if (recent_advice_events_.size() > kMaxRecentAdviceEvents) {
        recent_advice_events_.erase(
                recent_advice_events_.begin(),
                recent_advice_events_.begin() +
                        (recent_advice_events_.size() - kMaxRecentAdviceEvents));
    }
}

size_t PointerData::AddBacktrace(size_t num_frames, size_t size_bytes) {
    if (!ShouldBacktraceAllocSize(size_bytes)) {
        total_size_filtered_.fetch_add(1, std::memory_order_relaxed);
        if (g_size_filter_log_count.fetch_add(1, std::memory_order_relaxed) < 5) {
            LogPointerEvent(
                    "alloc_hook: skipped backtrace for alloc_size=%zu bytes because it is outside [%zu, %zu]\n",
                    size_bytes, g_debug->config().backtrace_min_size_bytes(),
                    g_debug->config().backtrace_max_size_bytes());
        }
        return kBacktraceEmptyIndex;
    }

    std::vector<uintptr_t> frames;
    std::vector<unwindstack::FrameData> frames_info;
    if (g_debug->config().options() & BACKTRACE) {
        unwindstack::ErrorCode error = Unwind(&frames, &frames_info, num_frames);
        last_unwind_error_.store(error, std::memory_order_relaxed);
        switch (error) {
            case unwindstack::ERROR_NONE:
                total_unwind_success_.fetch_add(1, std::memory_order_relaxed);
                if (frames.empty()) {
                    total_unwind_success_empty_frames_.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            case unwindstack::ERROR_MAX_FRAMES_EXCEEDED:
                total_unwind_success_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_success_max_frames_.fetch_add(1, std::memory_order_relaxed);
                if (frames.empty()) {
                    total_unwind_success_empty_frames_.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            case unwindstack::ERROR_EXIT_FUNC:
                total_unwind_exit_.fetch_add(1, std::memory_order_relaxed);
                return kBacktraceExitIndex;
            case unwindstack::ERROR_MEMORY_INVALID:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_memory_invalid_.fetch_add(1, std::memory_order_relaxed);
                break;
            case unwindstack::ERROR_UNWIND_INFO:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_unwind_info_.fetch_add(1, std::memory_order_relaxed);
                break;
            case unwindstack::ERROR_UNSUPPORTED:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_unsupported_.fetch_add(1, std::memory_order_relaxed);
                break;
            case unwindstack::ERROR_INVALID_MAP:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_invalid_map_.fetch_add(1, std::memory_order_relaxed);
                break;
            case unwindstack::ERROR_REPEATED_FRAME:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_repeated_frame_.fetch_add(1, std::memory_order_relaxed);
                break;
            case unwindstack::ERROR_INVALID_ELF:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_invalid_elf_.fetch_add(1, std::memory_order_relaxed);
                break;
            case unwindstack::ERROR_THREAD_DOES_NOT_EXIST:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_thread_missing_.fetch_add(1, std::memory_order_relaxed);
                break;
            case unwindstack::ERROR_THREAD_TIMEOUT:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_thread_timeout_.fetch_add(1, std::memory_order_relaxed);
                break;
            case unwindstack::ERROR_SYSTEM_CALL:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_system_call_.fetch_add(1, std::memory_order_relaxed);
                break;
            case unwindstack::ERROR_BAD_ARCH:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_bad_arch_.fetch_add(1, std::memory_order_relaxed);
                break;
            case unwindstack::ERROR_MAPS_PARSE:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_maps_parse_.fetch_add(1, std::memory_order_relaxed);
                break;
            case unwindstack::ERROR_INVALID_PARAMETER:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_invalid_parameter_.fetch_add(1, std::memory_order_relaxed);
                break;
            case unwindstack::ERROR_PTRACE_CALL:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_ptrace_call_.fetch_add(1, std::memory_order_relaxed);
                break;
            default:
                total_unwind_fail_.fetch_add(1, std::memory_order_relaxed);
                total_unwind_fail_other_.fetch_add(1, std::memory_order_relaxed);
                break;
        }
        if (error != unwindstack::ERROR_NONE &&
            error != unwindstack::ERROR_MAX_FRAMES_EXCEEDED &&
            error != unwindstack::ERROR_EXIT_FUNC) {
            if (g_unwind_failure_log_count.fetch_add(1, std::memory_order_relaxed) < 10) {
                LogPointerEvent(
                        "alloc_hook: unwind failed for alloc_size=%zu frames=%zu error=%u(%s)\n",
                        size_bytes, num_frames, static_cast<unsigned>(error),
                        unwindstack::GetErrorCodeString(error));
            }
                return kBacktraceEmptyIndex;
        }
    } else {
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

bool PointerData::GetInfo(const void* ptr, PointerInfoType* info) {
    if (ptr == nullptr || info == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
    uintptr_t mangled_ptr = ManglePointer(reinterpret_cast<uintptr_t>(ptr));
    auto entry = pointers_.find(mangled_ptr);
    if (entry == pointers_.end()) {
        return false;
    }

    *info = entry->second;
    return true;
}

void PointerData::Remove(const void* ptr) {
    size_t hash_index;
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
        pointers_.erase(mangled_ptr);
        total_remove_calls_.fetch_add(1, std::memory_order_relaxed);
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
    if (--frame_info->references == 0) {
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
        auto frame_entry = frames_.find(hash_index);
        auto backtrace_entry = backtraces_info_.find(hash_index);
        if (frame_entry == frames_.end() || backtrace_entry == backtraces_info_.end() ||
            !backtrace_entry->second) {
            LogPointerEvent(
                    "alloc_hook: inconsistent backtrace state for live allocation ptr=%" PRIxPTR " hash_index=%zu\n",
                    pointer, hash_index);
            continue;
        }
        FrameInfoType* frame_info = &frame_entry->second;
        std::shared_ptr<std::vector<unwindstack::FrameData>> backtrace_info =
                backtrace_entry->second;

        list->emplace_back(ListInfoType{
                pointer, 1, entry.second.RealSize(), entry.second.mem_type, frame_info,
                std::move(backtrace_info), entry.second.alloc_time});
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
            if (size != dup_iter->size || frame_info != dup_iter->frame_info) {
                break;
            }
            iter->num_allocations++;
        }
        iter = list->erase(iter + 1, dup_iter);
    }
}

void PointerData::DumpLiveToFile(int fd) {
    std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
    std::lock_guard<std::mutex> frame_guard(frame_mutex_);

    size_t tracked_live_allocs = pointers_.size();
    size_t tracked_with_backtrace_allocs = 0;
    size_t tracked_without_backtrace_allocs = 0;
    size_t tracked_with_backtrace_host = 0;
    size_t tracked_with_backtrace_dma = 0;
    size_t inconsistent_backtrace_entries = 0;
    for (const auto& entry : pointers_) {
        size_t hash_index = entry.second.hash_index;
        if (hash_index <= kBacktraceEmptyIndex) {
            tracked_without_backtrace_allocs++;
            continue;
        }
        auto frame_entry = frames_.find(hash_index);
        auto backtrace_entry = backtraces_info_.find(hash_index);
        if (frame_entry == frames_.end() || backtrace_entry == backtraces_info_.end() ||
            !backtrace_entry->second) {
            inconsistent_backtrace_entries++;
            continue;
        }

        tracked_with_backtrace_allocs++;
        size_t size = entry.second.RealSize();
        if (entry.second.mem_type == DMA) {
            tracked_with_backtrace_dma += size;
        } else {
            tracked_with_backtrace_host += size;
        }
    }

    std::vector<ListInfoType> list = std::move(peak_list);
    if (!(g_debug->config().options() & RECORD_MEMORY_PEAK)) {
        list.clear();
        // Sort by the time of the allocation.
        GetList(&list, true, [](const ListInfoType& a, const ListInfoType& b) {
            return a.alloc_time < b.alloc_time;
        });
    }
    std::vector<ListInfoType> mapped_regions;
    GetList(&mapped_regions, true, [](const ListInfoType& a, const ListInfoType& b) {
        return a.pointer < b.pointer;
    });
    mapped_regions.erase(
            std::remove_if(mapped_regions.begin(), mapped_regions.end(),
                           [](const ListInfoType& info) { return info.mem_type == HOST; }),
            mapped_regions.end());

    size_t host_use = 0, dma_use = 0;
    for (const auto& it : list) {
        size_t bt_size = it.size * it.num_allocations;
        it.mem_type == DMA ? dma_use += bt_size : host_use += bt_size;
    }
    const bool record_peak = (g_debug->config().options() & RECORD_MEMORY_PEAK) != 0;

    dprintf(fd,
            "shown host used: %fMB, shown dma used %fMB, shown total used: %fMB\n",
            host_use / 1024.0 / 1024.0, dma_use / 1024.0 / 1024.0,
            (host_use + dma_use) / 1024.0 / 1024.0);
    dprintf(
            fd,
            "peak_stats: peak_host_used=%fMB peak_dma_used=%fMB peak_total_used=%fMB "
            "dump_mode=%s record_peak=%d backtrace_min_size=%zu backtrace_max_size=%zu\n",
            peak_host / 1024.0 / 1024.0, peak_dma / 1024.0 / 1024.0,
            peak_tot / 1024.0 / 1024.0, record_peak ? "peak" : "live",
            record_peak ? 1 : 0, g_debug->config().backtrace_min_size_bytes(),
            g_debug->config().backtrace_max_size_bytes());
    if (!record_peak && peak_tot > current_used) {
        dprintf(
                fd,
                "note: peak tracking is disabled, so this dump only shows allocations "
                "still live at dump time; set DUMP_PEAK_VALUE_MB to capture peak backtraces.\n");
    }
    if (tracked_without_backtrace_allocs != 0) {
        dprintf(
                fd,
                "note: %zu live allocations are tracked in totals but omitted from the "
                "stack list because they were filtered by size or have no saved backtrace.\n",
                tracked_without_backtrace_allocs);
    }
    dprintf(
            fd,
            "debug_stats: tracked_live_allocs=%zu tracked_with_backtrace_allocs=%zu "
            "tracked_without_backtrace_allocs=%zu tracked_host_used=%fMB "
            "tracked_dma_used=%fMB tracked_total_used=%fMB tracked_with_backtrace_host=%fMB "
            "tracked_with_backtrace_dma=%fMB shown_allocs=%zu inconsistent_backtrace_entries=%zu\n",
            tracked_live_allocs, tracked_with_backtrace_allocs,
            tracked_without_backtrace_allocs, current_host / 1024.0 / 1024.0,
            current_dma / 1024.0 / 1024.0, current_used / 1024.0 / 1024.0,
            tracked_with_backtrace_host / 1024.0 / 1024.0,
            tracked_with_backtrace_dma / 1024.0 / 1024.0, list.size(),
            inconsistent_backtrace_entries);
    unwindstack::ErrorCode last_error = static_cast<unwindstack::ErrorCode>(
            last_unwind_error_.load(std::memory_order_relaxed));
    dprintf(
            fd,
            "debug_unwind_stats: add_calls=%" PRIu64 " remove_calls=%" PRIu64
            " size_filtered=%" PRIu64 " unwind_success=%" PRIu64
            " unwind_success_max_frames=%" PRIu64
            " unwind_success_empty_frames=%" PRIu64 " unwind_exit=%" PRIu64
            " unwind_fail=%" PRIu64 " last_unwind_error=%u(%s)\n",
            total_add_calls_.load(std::memory_order_relaxed),
            total_remove_calls_.load(std::memory_order_relaxed),
            total_size_filtered_.load(std::memory_order_relaxed),
            total_unwind_success_.load(std::memory_order_relaxed),
            total_unwind_success_max_frames_.load(std::memory_order_relaxed),
            total_unwind_success_empty_frames_.load(std::memory_order_relaxed),
            total_unwind_exit_.load(std::memory_order_relaxed),
            total_unwind_fail_.load(std::memory_order_relaxed),
            static_cast<unsigned>(last_error), unwindstack::GetErrorCodeString(last_error));
    dprintf(
            fd,
            "debug_unwind_failures: memory_invalid=%" PRIu64
            " unwind_info=%" PRIu64 " unsupported=%" PRIu64
            " invalid_map=%" PRIu64 " repeated_frame=%" PRIu64
            " invalid_elf=%" PRIu64 " thread_missing=%" PRIu64
            " thread_timeout=%" PRIu64 " system_call=%" PRIu64
            " bad_arch=%" PRIu64 " maps_parse=%" PRIu64
            " invalid_parameter=%" PRIu64 " ptrace_call=%" PRIu64
            " other=%" PRIu64 "\n",
            total_unwind_fail_memory_invalid_.load(std::memory_order_relaxed),
            total_unwind_fail_unwind_info_.load(std::memory_order_relaxed),
            total_unwind_fail_unsupported_.load(std::memory_order_relaxed),
            total_unwind_fail_invalid_map_.load(std::memory_order_relaxed),
            total_unwind_fail_repeated_frame_.load(std::memory_order_relaxed),
            total_unwind_fail_invalid_elf_.load(std::memory_order_relaxed),
            total_unwind_fail_thread_missing_.load(std::memory_order_relaxed),
            total_unwind_fail_thread_timeout_.load(std::memory_order_relaxed),
            total_unwind_fail_system_call_.load(std::memory_order_relaxed),
            total_unwind_fail_bad_arch_.load(std::memory_order_relaxed),
            total_unwind_fail_maps_parse_.load(std::memory_order_relaxed),
            total_unwind_fail_invalid_parameter_.load(std::memory_order_relaxed),
            total_unwind_fail_ptrace_call_.load(std::memory_order_relaxed),
            total_unwind_fail_other_.load(std::memory_order_relaxed));
    size_t smaps_total_rss_kb = 0;
    size_t smaps_total_anonymous_kb = 0;
    size_t smaps_total_anon_huge_kb = 0;
    std::vector<SmapsRegion> suspicious_regions;
    auto smaps = std::unique_ptr<FILE, decltype(&fclose)>{fopen("/proc/self/smaps", "re"),
                                                          fclose};
    if (smaps != nullptr) {
        SmapsRegion current_region;
        bool have_region = false;
        char* line = nullptr;
        size_t len = 0;
        auto flush_region = [&]() {
            if (!have_region) {
                return;
            }

            smaps_total_rss_kb += current_region.rss_kb;
            smaps_total_anonymous_kb += current_region.anonymous_kb;
            smaps_total_anon_huge_kb += current_region.anon_huge_kb;

            for (const auto& tracked : mapped_regions) {
                const uintptr_t tracked_start = tracked.pointer;
                const uintptr_t tracked_end = tracked.pointer + tracked.size;
                const size_t overlap = RangeOverlapBytes(
                        current_region.start, current_region.end, tracked_start, tracked_end);
                if (overlap > current_region.matched_overlap) {
                    current_region.matched_overlap = overlap;
                    current_region.matched = &tracked;
                }
            }

            if (IsInterestingSmapsRegion(current_region)) {
                suspicious_regions.push_back(current_region);
            }
        };

        while (getline(&line, &len, smaps.get()) > 0) {
            SmapsRegion parsed_region;
            if (ParseSmapsHeader(line, &parsed_region)) {
                flush_region();
                current_region = parsed_region;
                have_region = true;
                continue;
            }
            if (!have_region) {
                continue;
            }

            ParseKbField(line, "Size:", &current_region.size_kb) ||
                    ParseKbField(line, "Rss:", &current_region.rss_kb) ||
                    ParseKbField(line, "Anonymous:", &current_region.anonymous_kb) ||
                    ParseKbField(line, "AnonHugePages:", &current_region.anon_huge_kb) ||
                    ParseKbField(line, "Private_Dirty:", &current_region.private_dirty_kb);
        }
        flush_region();
        free(line);
    }
    std::sort(suspicious_regions.begin(), suspicious_regions.end(),
              [](const SmapsRegion& a, const SmapsRegion& b) {
                  const size_t a_score = a.anonymous_kb + a.anon_huge_kb + a.rss_kb;
                  const size_t b_score = b.anonymous_kb + b.anon_huge_kb + b.rss_kb;
                  if (a_score != b_score) {
                      return a_score > b_score;
                  }
                  return a.start < b.start;
              });
    if (suspicious_regions.size() > 12) {
        suspicious_regions.resize(12);
    }
    dprintf(
            fd,
            "smaps_summary: rss=%fMB anonymous=%fMB anon_huge=%fMB suspicious_regions=%zu\n",
            smaps_total_rss_kb / 1024.0, smaps_total_anonymous_kb / 1024.0,
            smaps_total_anon_huge_kb / 1024.0, suspicious_regions.size());
    dprintf(
            fd,
            "madvise_stats: dontneed_calls=%zu dontneed_bytes=%fMB free_calls=%zu "
            "free_bytes=%fMB populate_write_calls=%zu populate_write_bytes=%fMB "
            "other_calls=%zu other_bytes=%fMB matched_calls=%zu matched_bytes=%fMB "
            "unmatched_calls=%zu unmatched_bytes=%fMB recent_events=%zu\n",
            advice_dontneed_calls_, advice_dontneed_bytes_ / 1024.0 / 1024.0,
            advice_free_calls_, advice_free_bytes_ / 1024.0 / 1024.0,
            advice_populate_write_calls_,
            advice_populate_write_bytes_ / 1024.0 / 1024.0, advice_other_calls_,
            advice_other_bytes_ / 1024.0 / 1024.0, advice_matched_calls_,
            advice_matched_bytes_ / 1024.0 / 1024.0, advice_unmatched_calls_,
            advice_unmatched_bytes_ / 1024.0 / 1024.0, recent_advice_events_.size());
    for (const auto& event : recent_advice_events_) {
        struct tm* local_time = localtime(&event.event_time.tv_sec);
        char formatted_time[20];
        strftime(
                formatted_time, sizeof(formatted_time), "%Y-%m-%d %H:%M:%S",
                local_time);
        dprintf(
                fd,
                "madvise_event: time=%s.%03ld advice=%s range=%" PRIxPTR "-%" PRIxPTR
                " size=%fMB matched_regions=%zu matched_bytes=%fMB best_match_type=%s "
                "best_match_range=%" PRIxPTR "-%" PRIxPTR " best_match_size=%fMB\n",
                formatted_time, event.event_time.tv_usec / 1000,
                AdviceName(event.advice), event.addr, event.addr + event.size,
                event.size / 1024.0 / 1024.0, event.matched_regions,
                event.matched_bytes / 1024.0 / 1024.0,
                event.has_best_match ? mtype[event.best_match_type] : "none",
                event.best_match_pointer,
                event.best_match_pointer + event.best_match_size,
                event.best_match_size / 1024.0 / 1024.0);
    }
    for (const auto& region : suspicious_regions) {
        const char* region_name =
                region.name.empty() ? "<anonymous>" : region.name.c_str();
        dprintf(
                fd,
                "smaps_region: %" PRIxPTR "-%" PRIxPTR " name=%s rss=%fMB anonymous=%fMB "
                "anon_huge=%fMB private_dirty=%fMB matched_type=%s matched_tracked_size=%fMB "
                "matched_overlap=%fMB\n",
                region.start, region.end, region_name, region.rss_kb / 1024.0,
                region.anonymous_kb / 1024.0, region.anon_huge_kb / 1024.0,
                region.private_dirty_kb / 1024.0,
                region.matched ? mtype[region.matched->mem_type] : "none",
                region.matched ? region.matched->size / 1024.0 / 1024.0 : 0.0,
                region.matched_overlap / 1024.0 / 1024.0);
        if (region.matched != nullptr) {
            DumpBacktraceLines(fd, region.matched->backtrace_info, 16);
        }
        dprintf(fd, "\n");
    }
    dprintf(fd,
            "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
            "+++++++++++++++\n\n");
    LogPointerEvent(
            "alloc_hook: dump stats tracked_live_allocs=%zu tracked_total=%fMB "
            "tracked_with_backtrace_allocs=%zu tracked_with_backtrace_total=%fMB "
            "shown_allocs=%zu shown_total=%fMB size_filtered=%" PRIu64
            " unwind_fail=%" PRIu64 " last_unwind_error=%u(%s)\n",
            tracked_live_allocs, current_used / 1024.0 / 1024.0,
            tracked_with_backtrace_allocs,
            (tracked_with_backtrace_host + tracked_with_backtrace_dma) / 1024.0 / 1024.0,
            list.size(), (host_use + dma_use) / 1024.0 / 1024.0,
            total_size_filtered_.load(std::memory_order_relaxed),
            total_unwind_fail_.load(std::memory_order_relaxed),
            static_cast<unsigned>(last_error), unwindstack::GetErrorCodeString(last_error));
    for (const auto& info : list) {
        // 解析时间
        struct tm* local_time = localtime(&info.alloc_time.tv_sec);
        char formatted_time[20];
        strftime(
                formatted_time, sizeof(formatted_time), "%Y-%m-%d %H:%M:%S",
                local_time);

        dprintf(fd,
                "alloc_size:%fKB \t alloc_type:%s \t alloc_num:%zu \t "
                "alloc_time:%s.%zu\n",
                info.size / 1024.0, mtype[info.mem_type], info.num_allocations,
                formatted_time, info.alloc_time.tv_usec / 1000);
        DumpBacktraceLines(fd, info.backtrace_info);
        dprintf(fd, "\n");
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
