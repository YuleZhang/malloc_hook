#pragma once

#include <fcntl.h>
#include <stdint.h>
#include <sys/time.h>

#include <cstdint>
#include <cstdio>
#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Config.h"
#include "AsyncStackPipeline.h"
#include "ObservedMemory.h"
#include "Sampling.h"
#include "UnwindBacktrace.h"

// Live allocations left out of a report because they carry no stack (filtered
// by size, or capture was suppressed/failed). Reported as one aggregate line:
// emitting a block per allocation made a signal-triggered dump grow to one
// entry per live pointer, written with unbuffered dprintf while every
// allocation in the process is blocked.
struct OmittedStats {
    size_t count = 0;
    size_t bytes = 0;
};

// One mapped region's virtual size and resident size, as reported by
// /proc/self/smaps. Both are needed: tracked allocation bytes are *requested*
// bytes, so they line up with virtual size, while an evaluator measuring RSS
// sees only the resident part. Pages that were allocated but never written
// appear in size_kb and not in rss_kb.
struct MappingRss {
    std::string name;
    size_t rss_kb = 0;
    size_t size_kb = 0;
};

struct MappingTotals {
    size_t file_rss_kb = 0;
    size_t anon_rss_kb = 0;
    size_t file_size_kb = 0;
    size_t anon_size_kb = 0;
};

// The /proc-derived context recorded alongside a peak snapshot.
//
// Collected outside the allocation locks wherever the caller can, because
// reading /proc/self/smaps walks this process's page tables: on a 200 MB heap
// it took up to 20 ms, and every allocation in the process would be blocked for
// all of it. The sampler thread can afford that time; the allocators cannot.
struct PeakProcContext {
    RssBreakdown rss;
    std::vector<MappingRss> mappings;
    MappingTotals totals;
    bool filled = false;
};

enum MemType { HOST, MMAP, DMA };

// What made the retained peak snapshot the peak.
//
// `Tracked` is the sum of requested bytes the hook is accounting for, which is
// what the allocation path can see. `Observed` is host RSS plus dmabuf bytes
// read from /proc, which is what an external evaluator reports. They peak at
// different instants, so a report has to say which one it snapshotted.
enum class PeakSnapshotSource : uint8_t {
    None = 0,
    Tracked,
    Observed,
};

// Dedup key for a captured raw stack. It deliberately *borrows* the PC array
// rather than owning a fixed-size copy: an inline std::array<uintptr_t, 256>
// made this a 2KB object that was zero-initialized on every allocation and
// stored at 2KB per unique-stack map node. Lookups point `pcs` at the
// freshly captured record on the stack; stored keys point at the PC vector
// owned by the matching frames_ entry, which outlives the key.
struct FrameKeyType {
    uint16_t frame_count = 0;
    uint64_t module_generation = 0;
    const uintptr_t* pcs = nullptr;

    bool operator==(const FrameKeyType& comp) const {
        if (frame_count != comp.frame_count ||
            module_generation != comp.module_generation)
            return false;
        for (size_t i = 0; i < frame_count; i++) {
            if (pcs[i] != comp.pcs[i]) {
                return false;
            }
        }
        return true;
    }
};

// 新增 hash 算法
namespace std {
template <>
struct hash<FrameKeyType> {
    std::size_t operator()(const FrameKeyType& key) const {
        if (key.frame_count == 0) {
            return static_cast<std::size_t>(key.module_generation);
        }
        std::size_t cur_hash =
                key.pcs[0] ^ static_cast<std::size_t>(key.module_generation);
        // Limit the number of frames to speed up hashing.
        size_t max_frames = (key.frame_count > 5) ? 5 : key.frame_count;
        for (size_t i = 1; i < max_frames; i++) {
            cur_hash ^= key.pcs[i];
        }
        return cur_hash;
    }
};
};  // namespace std

struct FrameInfoType {
    size_t references = 0;
    // Shared so a peak snapshot can retain the PCs with a refcount bump
    // instead of a deep copy, and so a snapshot stays valid after the live
    // stack is released.
    std::shared_ptr<const std::vector<uintptr_t>> frames;
    uint64_t module_generation = 0;
    StackCaptureState capture_state = StackCaptureState::Empty;
    uint8_t terminal_error = 0;
    AsyncStackId async_stack_id = 0;
    StackResolutionState resolution_state = StackResolutionState::Pending;
};

// 新增 timeval 比较函数
inline bool operator<(const timeval& lhs, const timeval& rhs) {
    // Convert both times to microseconds and compare directly
    long long l_time = static_cast<long long>(lhs.tv_sec) * 1000000 + lhs.tv_usec;
    long long r_time = static_cast<long long>(rhs.tv_sec) * 1000000 + rhs.tv_usec;
    return l_time < r_time;
}

struct PointerInfoType {
    size_t size;
    size_t hash_index;
    MemType mem_type;
    timeval alloc_time;
    size_t RealSize() const { return size & ~(1U << 31); }
    static size_t MaxSize() { return (1U << 31) - 1; }
};

struct ListInfoType {
    uintptr_t pointer;
    size_t num_allocations;
    size_t size;
    MemType mem_type;
    FrameInfoType* frame_info;
    // Refcount bump, not a deep copy: a peak snapshot must not duplicate the
    // PC array (or per-frame module strings) for every live allocation.
    std::shared_ptr<const std::vector<uintptr_t>> raw_frames;
    std::shared_ptr<std::vector<SymbolizedFrame>> backtrace_info;
    StackCaptureState capture_state = StackCaptureState::Empty;
    uint8_t terminal_error = 0;
    StackResolutionState resolution_state = StackResolutionState::Pending;
    timeval alloc_time;
};
using Pred = std::function<bool(const ListInfoType&, const ListInfoType&)>;

class PointerData {
public:
    PointerData() = default;
    virtual ~PointerData();

    bool Initialize(const Config& config);

    bool ShouldTrackAllocation(size_t size, MemType type, size_t* tracked_size);
    bool MightContain(const void* ptr) const;
    void Add(
            const void* ptr, size_t requested_size, size_t tracked_size,
            MemType type = HOST);
    // Update a tracked mapping after a successful mremap without capturing a
    // second allocation stack. Untracked mappings remain untracked.
    void Remap(const void* old_ptr, const void* new_ptr, size_t new_size);
    // Detach a tracked pointer and hand its record back to the caller. Size
    // accounting is reversed immediately, but the allocation stack reference is
    // retained so the caller can either release it (RemoveBacktrace) once the
    // underlying free/unmap has succeeded, or hand the record back through
    // RestoreEntry() if the operation failed. Callers must take the entry
    // *before* the memory can be released, otherwise a concurrent allocation
    // may be handed the same address and have its record erased instead.
    bool TakeEntry(const void* ptr, PointerInfoType* info);
    // Re-attach a record produced by TakeEntry(). No allocation stack is
    // captured and no peak is recorded: the record is restored to the exact
    // state it had before the failed operation.
    void RestoreEntry(const void* ptr, const PointerInfoType& info);
    size_t AddBacktrace(size_t num_frames, size_t size_bytes);
    bool ShouldCaptureBacktrace(size_t size_bytes);
    void Remove(const void* ptr);
    void RemoveBacktrace(size_t hash_index);

    void DumpLiveToFile(int fd, bool dump_peak = true);
    void DumpPeakInfo();
    // Snapshots the live allocation stacks because the evaluator-visible
    // footprint (host RSS + dmabuf bytes) has reached a new maximum. Called
    // from the sampler thread, never from the allocation path.
    //
    // Taking this over as the peak criterion is the point of the call: once an
    // observed peak has been snapshotted, the tracked-bytes trigger stops
    // overwriting it, because that trigger fires at an instant nothing outside
    // the process reports.
    void RecordObservedPeak(const ObservedMemSample& sample);
    void FlushAsync();
    void BeginFinalization();
    void CompleteAsyncStack(const StackResult& result);
    AsyncStackStats AsyncStats() const;

private:
    inline uintptr_t ManglePointer(uintptr_t pointer) { return pointer ^ UINTPTR_MAX; }
    inline uintptr_t DemanglePointer(uintptr_t pointer) {
        return pointer ^ UINTPTR_MAX;
    }

    void GetList(
            std::vector<ListInfoType>* list, bool only_with_backtrace, Pred pred,
            OmittedStats* omitted = nullptr);
    void GetUniqueList(std::vector<ListInfoType>* list, bool only_with_backtrace);
    // Records a peak snapshot if the new peak has passed the next threshold.
    // Caller must hold pointer_mutex_; this takes frame_mutex_.
    void MaybeRecordPeakSnapshotLocked();
    // Copies the live allocation stacks and the surrounding /proc state into
    // the retained snapshot. Caller must hold pointer_mutex_; this takes
    // frame_mutex_. Shared by both peak criteria so the snapshot contents can
    // never differ depending on what triggered it. `proc` is the already
    // collected /proc context, or nullptr to collect it under the locks.
    void TakePeakSnapshotLocked(
            PeakSnapshotSource source, const ObservedMemSample* observed,
            PeakProcContext* proc);
    // Reads the /proc state a snapshot records. Takes no hook lock, so it can
    // be hoisted out of the locked region by callers that are able to.
    void CollectPeakProcContext(PeakProcContext* out);
    // Records `ptr` in the probabilistic membership filter. Caller holds
    // pointer_mutex_; the words themselves are atomic so lookups stay lock-free.
    void MarkPointerFilter(const void* ptr);

    std::mutex pointer_mutex_;
    std::unordered_map<uintptr_t, PointerInfoType> pointers_;

    std::mutex frame_mutex_;
    std::unordered_map<FrameKeyType, size_t> key_to_index_;
    std::unordered_map<size_t, FrameInfoType> frames_;
    std::unordered_map<size_t, std::shared_ptr<std::vector<SymbolizedFrame>>> backtraces_info_;
    std::unordered_map<AsyncStackId, size_t> async_stack_to_index_;
    std::unique_ptr<AsyncStackPipeline> async_pipeline_;
    size_t cur_hash_index_;

    size_t current_used, current_host, current_dma;
    size_t peak_tot, peak_host, peak_dma;
    size_t next_peak_record_threshold_;
    size_t peak_record_step_bytes_;
    std::vector<ListInfoType> peak_list;
    // Exact live totals at the moment peak_list was taken. The snapshot only
    // holds allocations that carry a stack, so report totals must come from
    // these counters rather than from summing the list.
    size_t peak_list_host = 0;
    size_t peak_list_dma = 0;
    size_t peak_list_tot = 0;
    // Which criterion produced the retained snapshot, and -- when it was the
    // observed footprint -- what that footprint read at that instant. Reported
    // so the snapshot can be lined up against an external sampler's series
    // instead of being assumed to describe the same moment.
    PeakSnapshotSource peak_snapshot_source_ = PeakSnapshotSource::None;
    size_t peak_observed_rss_ = 0;
    size_t peak_observed_dma_ = 0;
    size_t peak_observed_gpu_ = 0;
    // Set once an observed peak has been snapshotted. From then on the
    // allocation path must not overwrite it with a tracked-bytes peak.
    std::atomic<bool> observed_peak_active_{false};
    // This process's own RSS breakdown at that same moment, so a reader can see
    // how much of RSS tracked allocations could possibly account for.
    size_t peak_rss_kb = 0;
    size_t peak_rss_anon_kb = 0;
    size_t peak_rss_file_kb = 0;
    size_t peak_rss_shmem_kb = 0;
    // Per-mapping residency at that same moment. Sampled with the totals rather
    // than at exit: by exit the heap is gone and the anonymous side reads an
    // order of magnitude low.
    std::vector<MappingRss> peak_mappings;
    MappingTotals peak_map_totals;
    // The hook's own bookkeeping at that moment. This is what the tool adds to
    // the traced process's RSS, and it is dominated by one pointers_ entry per
    // live tracked allocation -- a cost paid for every allocation, independent
    // of BACKTRACE_MIN_SIZE, which only gates stack capture.
    size_t peak_live_pointers = 0;
    size_t peak_pointer_buckets = 0;
    size_t peak_unique_stacks = 0;
    size_t peak_stack_pc_bytes = 0;
    static constexpr size_t kPointerFilterWords = 1 << 13;
    std::array<std::atomic<uint64_t>, kPointerFilterWords> pointer_filter_{};

    PointerData(const PointerData&) = delete;
    PointerData& operator=(const PointerData&) = delete;
};
