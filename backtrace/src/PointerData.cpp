#include <cxxabi.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/time.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>

#include "Config.h"
#include "DebugData.h"
#include "PointerData.h"
#include "UnwindBacktrace.h"
#include "memory_hook.h"

constexpr size_t kBacktraceExitIndex = 0;
constexpr size_t kBacktraceEmptyIndex = 1;
constexpr size_t kDefaultPeakRecordStepBytes = 12 * 1024 * 1024;
// The first frames belong to the capture plumbing itself.  Keep this policy
// at the hook boundary so reports start at the allocation caller while both
// Fast and Accurate backends retain the same neutral contract.
constexpr size_t kHookCaptureSkipFrames = 3;
const char* mtype[3] = {"host", "mmap", "dma"};

namespace {
void AsyncStackComplete(void* opaque, const StackResult& result) {
    if (opaque != nullptr) {
        static_cast<PointerData*>(opaque)->CompleteAsyncStack(result);
    }
}
}  // namespace

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

namespace {

uint64_t HashPointer(uintptr_t pointer) {
    uint64_t hash = static_cast<uint64_t>(pointer);
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    return hash ^ (hash >> 33);
}

struct SamplerTlsState {
    uint64_t configured_interval = 0;
    PoissonSampler sampler;
};

pthread_key_t& SamplerKey() {
    static pthread_key_t key;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, [] { pthread_key_create(&key, nullptr); });
    return key;
}

SamplerTlsState* GetSamplerState() {
    SamplerTlsState* state =
            static_cast<SamplerTlsState*>(pthread_getspecific(SamplerKey()));
    if (state != nullptr) {
        return state;
    }
    void* storage = m_sys_malloc(sizeof(SamplerTlsState));
    if (storage == nullptr) {
        return nullptr;
    }
    state = new (storage) SamplerTlsState();
    pthread_setspecific(SamplerKey(), state);
    return state;
}

}  // namespace

PointerData::~PointerData() {
    if (async_pipeline_ != nullptr) {
        async_pipeline_->Shutdown();
    }
}

bool PointerData::Initialize(const Config& config) {
    if (async_pipeline_ != nullptr) {
        async_pipeline_->Shutdown();
    }
    pointers_.clear();
    key_to_index_.clear();
    frames_.clear();
    backtraces_info_.clear();
    async_stack_to_index_.clear();
    peak_list.clear();
    // A hash index of kBacktraceEmptyIndex indicates that we tried to get
    // a backtrace, but there was nothing recorded.
    cur_hash_index_ = kBacktraceEmptyIndex + 1;
    current_used = current_host = current_dma = 0;
    peak_tot = peak_host = peak_dma = 0;
    for (auto& word : pointer_filter_) {
        word.store(0, std::memory_order_relaxed);
    }
    next_peak_record_threshold_ = config.backtrace_dump_peak_val();
    peak_record_step_bytes_ = ParsePeakStepBytes();
    async_pipeline_ = std::make_unique<AsyncStackPipeline>(
            nullptr, nullptr, 256, AsyncStackComplete, this);

    return true;
}

bool PointerData::ShouldTrackAllocation(
        size_t pointer_size, MemType type, size_t* tracked_size) {
    *tracked_size = pointer_size;
    if (type != HOST || !g_debug->config().sampling_enabled()) {
        return true;
    }
    SamplerTlsState* state = GetSamplerState();
    if (state == nullptr) {
        return true;
    }
    const size_t interval = g_debug->config().sampling_interval_bytes();
    if (state->configured_interval != interval) {
        state->sampler.SetSamplingInterval(interval);
        state->configured_interval = interval;
    }
    *tracked_size = state->sampler.SampleSize(pointer_size);
    return *tracked_size != 0;
}

bool PointerData::MightContain(const void* ptr) const {
    const uint64_t hash = HashPointer(reinterpret_cast<uintptr_t>(ptr));
    const size_t index0 = hash & (kPointerFilterWords - 1);
    const uint64_t bit0 = 1ULL << ((hash >> 16) & 63);
    if ((pointer_filter_[index0].load(std::memory_order_relaxed) & bit0) == 0) {
        return false;
    }
    const size_t index1 = (hash >> 6) & (kPointerFilterWords - 1);
    const uint64_t bit1 = 1ULL << ((hash >> 22) & 63);
    return (pointer_filter_[index1].load(std::memory_order_relaxed) & bit1) != 0;
}

void PointerData::Add(
        const void* ptr, size_t pointer_size, size_t tracked_size, MemType type) {
    size_t hash_index = 0;
    hash_index = AddBacktrace(g_debug->config().backtrace_frames(), pointer_size);

    // unwind 跳过的函数，不记录其堆栈和 pointer 信息
    if (hash_index == kBacktraceExitIndex)
        return;

    std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uintptr_t mangled_ptr = ManglePointer(reinterpret_cast<uintptr_t>(ptr));
    pointers_[mangled_ptr] = PointerInfoType{tracked_size, hash_index, type, tv};
    const uint64_t hash = HashPointer(reinterpret_cast<uintptr_t>(ptr));
    pointer_filter_[hash & (kPointerFilterWords - 1)].fetch_or(
            1ULL << ((hash >> 16) & 63), std::memory_order_relaxed);
    pointer_filter_[(hash >> 6) & (kPointerFilterWords - 1)].fetch_or(
            1ULL << ((hash >> 22) & 63), std::memory_order_relaxed);
    current_used += tracked_size;
    size_t* current = (type == DMA) ? &current_dma : &current_host;
    size_t* peak = (type == DMA) ? &peak_dma : &peak_host;
    *current += tracked_size;
    if (*current > *peak) {
        *peak = *current;
    }
    if (peak_tot < current_used) {
        peak_tot = current_used;

        if ((g_debug->config().options() & RECORD_MEMORY_PEAK) &&
            hash_index > kBacktraceEmptyIndex && peak_tot > next_peak_record_threshold_) {
            std::lock_guard<std::mutex> frame_guard(frame_mutex_);
            std::vector<ListInfoType> next_peak_list;
            GetUniqueList(&next_peak_list, true);
            if (!next_peak_list.empty()) {
                peak_list = std::move(next_peak_list);
                if (peak_record_step_bytes_ == 0) {
                    next_peak_record_threshold_ = peak_tot;
                } else {
                    next_peak_record_threshold_ = peak_tot + peak_record_step_bytes_;
                }
            }
        }
    }
}

void PointerData::Remap(const void* old_ptr, const void* new_ptr, size_t new_size) {
    if (old_ptr == nullptr || new_ptr == nullptr || new_size > PointerInfoType::MaxSize()) {
        return;
    }
    std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
    const uintptr_t old_key = ManglePointer(reinterpret_cast<uintptr_t>(old_ptr));
    auto entry = pointers_.find(old_key);
    if (entry == pointers_.end()) {
        return;
    }
    PointerInfoType info = entry->second;
    const size_t old_size = info.size;
    info.size = new_size;
    pointers_.erase(entry);
    pointers_[ManglePointer(reinterpret_cast<uintptr_t>(new_ptr))] = info;
    if (new_size >= old_size) {
        current_used += new_size - old_size;
        if (info.mem_type == DMA) {
            current_dma += new_size - old_size;
        } else {
            current_host += new_size - old_size;
        }
    } else {
        current_used -= old_size - new_size;
        if (info.mem_type == DMA) {
            current_dma -= old_size - new_size;
        } else {
            current_host -= old_size - new_size;
        }
    }
    size_t* peak = info.mem_type == DMA ? &peak_dma : &peak_host;
    size_t current = info.mem_type == DMA ? current_dma : current_host;
    if (current > *peak) {
        *peak = current;
    }
    if (current_used > peak_tot) {
        peak_tot = current_used;
    }
    const uint64_t hash = HashPointer(reinterpret_cast<uintptr_t>(new_ptr));
    pointer_filter_[hash & (kPointerFilterWords - 1)].fetch_or(
            1ULL << ((hash >> 16) & 63), std::memory_order_relaxed);
    pointer_filter_[(hash >> 6) & (kPointerFilterWords - 1)].fetch_or(
            1ULL << ((hash >> 22) & 63), std::memory_order_relaxed);
}

size_t PointerData::AddBacktrace(size_t num_frames, size_t size_bytes) {
    if (!ShouldBacktraceAllocSize(size_bytes)) {
        return kBacktraceEmptyIndex;
    }

    if (!(g_debug->config().options() & BACKTRACE)) {
        return kBacktraceEmptyIndex;
    }
    RawStackRecord raw = CaptureStack(
            g_debug->config().capture_mode(), num_frames, kHookCaptureSkipFrames);
    if (raw.frame_count == 0 ||
        (raw.capture_state != StackCaptureState::Complete &&
         raw.capture_state != StackCaptureState::Partial)) {
        return kBacktraceEmptyIndex;
    }
    if (raw.module_generation == 0 && async_pipeline_ != nullptr) {
        raw.module_generation = async_pipeline_->CurrentModuleGeneration();
    }
    std::vector<uintptr_t> frames(raw.pcs.begin(), raw.pcs.begin() + raw.frame_count);

    FrameKeyType key;
    key.frame_count = raw.frame_count;
    key.module_generation = raw.module_generation;
    std::copy_n(raw.pcs.begin(), raw.frame_count, key.pcs.begin());
    size_t hash_index;
    std::lock_guard<std::mutex> frame_guard(frame_mutex_);
    auto entry = key_to_index_.find(key);
    if (entry == key_to_index_.end()) {
        hash_index = cur_hash_index_++;
        key_to_index_.emplace(key, hash_index);

        frames_.emplace(
                hash_index,
                FrameInfoType{.references = 1,
                              .frames = std::move(frames),
                              .module_generation = raw.module_generation,
                              .capture_state = raw.capture_state,
                              .terminal_error = raw.terminal_error,
                              .resolution_state = StackResolutionState::Pending});

        if (async_pipeline_ != nullptr) {
            const auto submit = async_pipeline_->Submit(raw);
            FrameInfoType& frame_info = frames_.at(hash_index);
            if (submit.id != 0) {
                frame_info.async_stack_id = submit.id;
                async_stack_to_index_[submit.id] = hash_index;
            }
            if (submit.dropped || submit.rejected_recursion) {
                frame_info.resolution_state = submit.rejected_recursion
                                                      ? StackResolutionState::Failed
                                                      : StackResolutionState::Dropped;
            } else if (!submit.accepted && !submit.duplicate) {
                // A pipeline that could not start, or that has already entered
                // shutdown, must not leave the report permanently Pending.
                frame_info.resolution_state = StackResolutionState::Failed;
            }
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
        FrameKeyType key;
        key.frame_count = static_cast<uint16_t>(
                std::min(frame_info->frames.size(), kMaxAsyncRawFrames));
        key.module_generation = frame_info->module_generation;
        std::copy_n(frame_info->frames.begin(), key.frame_count, key.pcs.begin());
        key_to_index_.erase(key);
        frames_.erase(hash_index);
        for (auto entry = async_stack_to_index_.begin();
             entry != async_stack_to_index_.end();) {
            if (entry->second == hash_index) {
                entry = async_stack_to_index_.erase(entry);
            } else {
                ++entry;
            }
        }
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
        FrameInfoType* frame_info =
                frame_entry == frames_.end() ? nullptr : &frame_entry->second;
        auto backtrace_entry = backtraces_info_.find(hash_index);
        std::shared_ptr<std::vector<SymbolizedFrame>> backtrace_info =
                backtrace_entry == backtraces_info_.end() ? nullptr : backtrace_entry->second;

        list->emplace_back(ListInfoType{
                pointer,
                1,
                entry.second.RealSize(),
                entry.second.mem_type,
                frame_info,
                std::move(backtrace_info),
                frame_info == nullptr ? StackCaptureState::Empty : frame_info->capture_state,
                static_cast<uint8_t>(
                        frame_info == nullptr ? 0 : frame_info->terminal_error),
                frame_info == nullptr ? StackResolutionState::Dropped
                                       : frame_info->resolution_state,
                entry.second.alloc_time});
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

void PointerData::DumpLiveToFile(int fd, bool dump_peak) {
    FlushAsync();
    const AsyncStackStats async_stats = AsyncStats();
    std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
    std::lock_guard<std::mutex> frame_guard(frame_mutex_);

    std::vector<ListInfoType> list;
    if ((g_debug->config().options() & RECORD_MEMORY_PEAK) && dump_peak) {
        list = peak_list;
    } else {
        // Sort by the time of the allocation.
        GetList(&list, true, [](const ListInfoType& a, const ListInfoType& b) {
            return a.alloc_time < b.alloc_time;
        });
    }

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
    const std::string async_stats_line = FormatAsyncStackStats(async_stats);
    dprintf(fd, "async_stack_stats: %s\n", async_stats_line.c_str());
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
                "alloc_time:%s.%zu\n",
                info.size / 1024.0, mtype[info.mem_type], info.num_allocations,
                formatted_time, info.alloc_time.tv_usec / 1000);
        if (info.backtrace_info == nullptr) {
            dprintf(fd, "<no stack: capture_state=%u capture_error=%u "
                        "resolution_state=%u>\n\n",
                    static_cast<unsigned>(info.capture_state),
                    static_cast<unsigned>(info.terminal_error),
                    static_cast<unsigned>(info.resolution_state));
            continue;
        }
        for (size_t i = 0; i < info.backtrace_info->size(); ++i) {
            const SymbolizedFrame* frame = &info.backtrace_info->at(i);

            char frame_prefix[64];
            snprintf(frame_prefix, sizeof(frame_prefix), "#%0zd %" PRIx64 " ", i,
                     frame->rel_pc);
            std::string line(frame_prefix);
            // so path
            if (frame->module_name.empty()) {
                line += "<unknown>";
            } else {
                line += frame->module_name;
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

void PointerData::FlushAsync() {
    if (async_pipeline_ != nullptr) {
        async_pipeline_->Flush();
    }
}

void PointerData::BeginFinalization() {
    if (async_pipeline_ != nullptr) {
        async_pipeline_->BeginFinalization();
    }
}

void PointerData::CompleteAsyncStack(const StackResult& result) {
    std::lock_guard<std::mutex> frame_guard(frame_mutex_);
    auto stack_entry = async_stack_to_index_.find(result.id);
    if (stack_entry == async_stack_to_index_.end()) {
        return;
    }
    auto frame_entry = frames_.find(stack_entry->second);
    if (frame_entry == frames_.end()) {
        return;
    }
    FrameInfoType& frame_info = frame_entry->second;
    frame_info.resolution_state = result.state;
    if (!result.frames.empty()) {
        backtraces_info_[stack_entry->second] =
                std::make_shared<std::vector<SymbolizedFrame>>(result.frames);
    }
}

AsyncStackStats PointerData::AsyncStats() const {
    if (async_pipeline_ == nullptr) {
        return {};
    }
    return async_pipeline_->stats();
}

void PointerData::DumpPeakInfo() {
    FlushAsync();
    std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
    printf("\n+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
           "++++++++++++++++\n");
    printf("host peak used: %fMB, dma peak used %fMB, total peak used: %fMB\n\n",
           peak_host / 1024.0 / 1024.0, peak_dma / 1024.0 / 1024.0,
           peak_tot / 1024.0 / 1024.0);
    printf("async_stack_stats: %s\n",
           FormatAsyncStackStats(AsyncStats()).c_str());
}
