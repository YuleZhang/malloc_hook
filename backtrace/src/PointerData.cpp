#include <cxxabi.h>
#include <inttypes.h>
#include <pthread.h>
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
#include <new>
#include <string>
#include <vector>

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
    uint64_t capture_interval = 0;
    uint64_t capture_accumulated = 0;
};

struct ModuleRange {
    uintptr_t start = 0;
    uintptr_t end = 0;
    uintptr_t load_bias = 0;
    std::string name;
};

// Report-time module attribution for raw Fast-capture PCs.
//
// Built once per dump from /proc/self/maps. /proc is used deliberately instead
// of dladdr()/dl_iterate_phdr(): the exit dump runs with all other allocator
// operations blocked, so taking the dynamic loader lock there can deadlock
// against a thread that already holds it and is waiting on the hook lock.
// Reading /proc touches no loader state and costs one pass per dump instead of
// one dladdr() per frame per allocation.
class ModuleTable {
public:
    void Build() {
        ranges_.clear();
        const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return;
        }
        std::string content;
        char buffer[8192];
        ssize_t bytes;
        while ((bytes = read(fd, buffer, sizeof(buffer))) > 0) {
            content.append(buffer, static_cast<size_t>(bytes));
        }
        close(fd);

        std::vector<RawMapping> mappings;
        size_t pos = 0;
        while (pos < content.size()) {
            size_t eol = content.find('\n', pos);
            if (eol == std::string::npos) {
                eol = content.size();
            }
            ParseLine(content.substr(pos, eol - pos), &mappings);
            pos = eol + 1;
        }

        // The load bias of a mapped ELF is the start of its offset-0 mapping:
        // the first PT_LOAD of a shared object or PIE has p_vaddr == 0 and
        // p_offset == 0, so that mapping begins exactly at the bias.
        //
        // Deriving the bias per-mapping as (start - offset) is wrong: with
        // -z separate-code (the default in modern binutils) a segment's
        // page-aligned p_vaddr and p_offset differ, which silently shifts every
        // relative PC in that segment by a page and makes offline symbolization
        // resolve to the wrong function.
        std::unordered_map<std::string, uintptr_t> module_base;
        for (const RawMapping& mapping : mappings) {
            if (mapping.offset != 0) {
                continue;
            }
            auto known = module_base.find(mapping.name);
            if (known == module_base.end() || mapping.start < known->second) {
                module_base[mapping.name] = mapping.start;
            }
        }

        for (RawMapping& mapping : mappings) {
            if (!mapping.executable) {
                continue;
            }
            auto base = module_base.find(mapping.name);
            ModuleRange range;
            range.start = mapping.start;
            range.end = mapping.end;
            range.load_bias = base != module_base.end()
                    ? base->second
                    : mapping.start - mapping.offset;
            range.name = std::move(mapping.name);
            ranges_.push_back(std::move(range));
        }
        std::sort(
                ranges_.begin(), ranges_.end(),
                [](const ModuleRange& a, const ModuleRange& b) {
                    return a.start < b.start;
                });
    }

    // Maps a runtime PC to the ELF virtual address llvm-symbolizer expects,
    // plus the owning module path.
    bool Resolve(uintptr_t pc, uintptr_t* rel_pc, const char** name) const {
        auto entry = std::upper_bound(
                ranges_.begin(), ranges_.end(), pc,
                [](uintptr_t value, const ModuleRange& range) {
                    return value < range.start;
                });
        if (entry == ranges_.begin()) {
            return false;
        }
        --entry;
        if (pc >= entry->end) {
            return false;
        }
        *rel_pc = pc - entry->load_bias;
        *name = entry->name.c_str();
        return true;
    }

private:
    struct RawMapping {
        uintptr_t start = 0;
        uintptr_t end = 0;
        uintptr_t offset = 0;
        bool executable = false;
        std::string name;
    };

    static void ParseLine(
            const std::string& entry, std::vector<RawMapping>* mappings) {
        // <start>-<end> <perms> <offset> <dev> <inode> <path>
        uintptr_t start = 0;
        uintptr_t end = 0;
        uintptr_t offset = 0;
        char perms[8] = {};
        int path_pos = 0;
        if (sscanf(entry.c_str(),
                   "%" SCNxPTR "-%" SCNxPTR " %7s %" SCNxPTR " %*s %*s %n",
                   &start, &end, perms, &offset, &path_pos) < 4) {
            return;
        }
        const char* path = entry.c_str() + path_pos;
        while (*path == ' ') {
            ++path;
        }
        // Skip anonymous mappings and pseudo-regions such as [stack].
        if (*path == '\0' || *path == '[') {
            return;
        }
        RawMapping mapping;
        mapping.start = start;
        mapping.end = end;
        mapping.offset = offset;
        mapping.executable = strchr(perms, 'x') != nullptr;
        mapping.name = path;
        mappings->push_back(std::move(mapping));
    }

    std::vector<ModuleRange> ranges_;
};

// Resolving a PC known to live in this library identifies the hook's own
// module without a dladdr() call, so the report can drop the capture plumbing
// frames that survive inlining.
void SelfModuleProbe() {}

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
    // No in-process symbol/module resolver is started on any platform. Module
    // identity and relative PCs are recovered at report time from
    // /proc/self/maps, which is uniform across targets, costs one pass per
    // dump instead of a worker thread plus a submission per unique stack, and
    // cannot leave the report full of unresolved frames when the resolver
    // fails.
    async_pipeline_.reset();

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

bool PointerData::ShouldCaptureBacktrace(size_t size_bytes) {
    if (g_debug == nullptr ||
        g_debug->config().capture_mode() != StackCaptureMode::Fast) {
        return true;
    }
    const uint64_t interval = g_debug->config().fast_capture_interval_bytes();
    // Check the interval before touching thread-local state: with sampling off
    // (the default) this must not cost a pthread_getspecific per allocation.
    if (interval <= 1) {
        return true;
    }
    SamplerTlsState* state = GetSamplerState();
    if (state == nullptr) {
        return true;
    }
    if (state->capture_interval != interval) {
        state->capture_interval = interval;
        state->capture_accumulated = 0;
    }
    state->capture_accumulated += size_bytes;
    if (state->capture_accumulated < interval) {
        return false;
    }
    state->capture_accumulated %= interval;
    return true;
}

void PointerData::MarkPointerFilter(const void* ptr) {
    const uint64_t hash = HashPointer(reinterpret_cast<uintptr_t>(ptr));
    pointer_filter_[hash & (kPointerFilterWords - 1)].fetch_or(
            1ULL << ((hash >> 16) & 63), std::memory_order_relaxed);
    pointer_filter_[(hash >> 6) & (kPointerFilterWords - 1)].fetch_or(
            1ULL << ((hash >> 22) & 63), std::memory_order_relaxed);
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
    MarkPointerFilter(ptr);
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
            hash_index > kBacktraceEmptyIndex &&
            peak_tot > next_peak_record_threshold_) {
            std::lock_guard<std::mutex> frame_guard(frame_mutex_);
            std::vector<ListInfoType> next_peak_list;
            // Snapshot only allocations that actually carry a stack. Including
            // stack-less records here walked every live pointer -- with the
            // default size filter that is >99% of them -- on every 12MB step of
            // a ~1.7GB peak, while holding both mutexes. Exact live/peak totals
            // come from the counters below, not from this list, so accounting
            // stays exact without paying for the walk.
            GetUniqueList(&next_peak_list, true);
            if (!next_peak_list.empty()) {
                peak_list = std::move(next_peak_list);
                peak_list_host = current_host;
                peak_list_dma = current_dma;
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
    MarkPointerFilter(new_ptr);
}

size_t PointerData::AddBacktrace(size_t num_frames, size_t size_bytes) {
    if (!ShouldBacktraceAllocSize(size_bytes) ||
        !ShouldCaptureBacktrace(size_bytes)) {
        return kBacktraceEmptyIndex;
    }

    if (!(g_debug->config().options() & BACKTRACE)) {
        return kBacktraceEmptyIndex;
    }
    RawStackRecord raw;
    CaptureStackInto(
            &raw, g_debug->config().capture_mode(), num_frames,
            kHookCaptureSkipFrames);
    if (raw.frame_count == 0 ||
        (raw.capture_state != StackCaptureState::Complete &&
         raw.capture_state != StackCaptureState::Partial)) {
        return kBacktraceEmptyIndex;
    }
#if !defined(MALLOC_HOOK_TARGET_OS_LINUX)
    if (raw.module_generation == 0 && async_pipeline_ != nullptr) {
        raw.module_generation = async_pipeline_->CurrentModuleGeneration();
    }
#endif
    // Look the stack up through a key that borrows the just-captured PCs. The
    // common case is a repeat stack, which must not allocate anything at all.
    FrameKeyType probe;
    probe.frame_count = raw.frame_count;
    probe.module_generation = raw.module_generation;
    probe.pcs = raw.pcs.data();

    size_t hash_index;
    std::lock_guard<std::mutex> frame_guard(frame_mutex_);
    auto entry = key_to_index_.find(probe);
    if (entry == key_to_index_.end()) {
        hash_index = cur_hash_index_++;
        // Module attribution and relative PCs are resolved once at report
        // time, not here: dladdr() takes the loader lock and would run under
        // frame_mutex_ on the allocation path. Fast capture stores raw PCs
        // only.
        auto frames = std::make_shared<const std::vector<uintptr_t>>(
                raw.pcs.begin(), raw.pcs.begin() + raw.frame_count);
        frames_.emplace(
                hash_index,
                FrameInfoType{.references = 1,
                              .frames = frames,
                              .module_generation = raw.module_generation,
                              .capture_state = raw.capture_state,
                              .terminal_error = raw.terminal_error,
                              .resolution_state = StackResolutionState::Pending});
        // The stored key must borrow the PCs owned by the frames_ entry, which
        // lives exactly as long as the key does.
        FrameKeyType stored = probe;
        stored.pcs = frames->data();
        key_to_index_.emplace(stored, hash_index);

#if !defined(MALLOC_HOOK_TARGET_OS_LINUX)
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
            } else if (submit.duplicate) {
                // The pipeline still holds this raw stack from an earlier
                // capture whose frame entry has since been released, so no
                // further completion callback will fire for it. Adopt the
                // cached result now instead of reporting Pending with no stack
                // forever. A still-pending duplicate needs nothing here: the
                // callback routes through async_stack_to_index_, which was
                // just repointed at this hash_index.
                StackResult cached;
                if (async_pipeline_->GetResult(submit.id, &cached) &&
                    cached.state != StackResolutionState::Pending) {
                    frame_info.resolution_state = cached.state;
                    if (!cached.frames.empty()) {
                        backtraces_info_[hash_index] =
                                std::make_shared<std::vector<SymbolizedFrame>>(
                                        std::move(cached.frames));
                    }
                }
            } else if (!submit.accepted) {
                // A pipeline that could not start, or that has already entered
                // shutdown, must not leave the report permanently Pending.
                frame_info.resolution_state = StackResolutionState::Failed;
            }
        }
#endif
    } else {
        hash_index = entry->second;
        FrameInfoType* frame_info = &frames_[hash_index];
        frame_info->references++;
    }
    return hash_index;
}

bool PointerData::TakeEntry(const void* ptr, PointerInfoType* info) {
    std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
    const uintptr_t mangled_ptr = ManglePointer(reinterpret_cast<uintptr_t>(ptr));
    auto entry = pointers_.find(mangled_ptr);
    if (entry == pointers_.end()) {
        // No tracked pointer.
        return false;
    }
    if (info != nullptr) {
        *info = entry->second;
    }
    current_used -= entry->second.size;
    size_t* target = (entry->second.mem_type == DMA) ? &current_dma : &current_host;
    *target -= entry->second.size;
    pointers_.erase(entry);
    return true;
}

void PointerData::RestoreEntry(const void* ptr, const PointerInfoType& info) {
    std::lock_guard<std::mutex> pointer_guard(pointer_mutex_);
    pointers_[ManglePointer(reinterpret_cast<uintptr_t>(ptr))] = info;
    MarkPointerFilter(ptr);
    current_used += info.size;
    size_t* current = (info.mem_type == DMA) ? &current_dma : &current_host;
    *current += info.size;
    // Peaks are deliberately not re-evaluated: this restores a state that was
    // already accounted for, so it can never establish a new peak.
}

void PointerData::Remove(const void* ptr) {
    PointerInfoType info{};
    if (!TakeEntry(ptr, &info)) {
        return;
    }

    RemoveBacktrace(info.hash_index);
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
                std::min(frame_info->frames->size(), kMaxAsyncRawFrames));
        key.module_generation = frame_info->module_generation;
        key.pcs = frame_info->frames->data();
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
                frame_info == nullptr
                        ? std::shared_ptr<const std::vector<uintptr_t>>{}
                        : frame_info->frames,
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
                if (a_frame->frames->size() != b_frame->frames->size()) {
                    return a_frame->frames->size() > b_frame->frames->size();
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
    size_t host_use = 0, dma_use = 0;
    const bool dumping_peak =
            (g_debug->config().options() & RECORD_MEMORY_PEAK) && dump_peak;
    if (dumping_peak) {
        list = peak_list;
        // The snapshot only retains allocations that carry a stack, so its
        // totals come from the exact counters captured alongside it.
        host_use = peak_list_host;
        dma_use = peak_list_dma;
    } else {
        // Sort by the time of the allocation.
        GetList(&list, false, [](const ListInfoType& a, const ListInfoType& b) {
            return a.alloc_time < b.alloc_time;
        });
        host_use = current_host;
        dma_use = current_dma;
    }

    // Resolve module identity for raw PCs once for the whole report rather than
    // once per allocation on the hook path.
    ModuleTable modules;
    modules.Build();
    // Capture-time frame skipping is a fixed count, so inlining decides how many
    // of this library's own frames survive it. Identify them here and drop the
    // leading run: leaving them in makes every allocation look like it
    // originated in liballoc_hook.so, which breaks per-library attribution in
    // the offline symbolizer.
    const char* self_module = nullptr;
    {
        uintptr_t unused_rel_pc = 0;
        if (!modules.Resolve(
                    reinterpret_cast<uintptr_t>(&SelfModuleProbe), &unused_rel_pc,
                    &self_module)) {
            self_module = nullptr;
        }
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
        if (info.raw_frames != nullptr && !info.raw_frames->empty()) {
            // Raw PCs plus report-time module identity are what the offline
            // symbolizer consumes, so this is the path on every platform. It is
            // preferred over any in-process symbolization result: a resolver
            // that failed still leaves frame entries behind, and emitting those
            // would produce absolute PCs against "<unknown>" modules.
            const std::vector<uintptr_t>& raw = *info.raw_frames;
            size_t first = 0;
            if (self_module != nullptr) {
                while (first < raw.size()) {
                    uintptr_t rel_pc = 0;
                    const char* module_name = nullptr;
                    if (!modules.Resolve(raw[first], &rel_pc, &module_name) ||
                        strcmp(module_name, self_module) != 0) {
                        break;
                    }
                    ++first;
                }
                // A stack that is entirely inside the hook carries no caller
                // information; keep it verbatim rather than emitting nothing.
                if (first == raw.size()) {
                    first = 0;
                }
            }
            for (size_t i = first; i < raw.size(); ++i) {
                const uintptr_t pc = raw[i];
                uintptr_t rel_pc = pc;
                const char* module_name = "<unknown>";
                modules.Resolve(pc, &rel_pc, &module_name);
                dprintf(fd, "#%0zu %" PRIxPTR " %s\n", i - first, rel_pc,
                        module_name);
            }
            dprintf(fd, "\n");
            continue;
        }
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
