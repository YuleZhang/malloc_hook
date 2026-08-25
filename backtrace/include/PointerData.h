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
#include "Sampling.h"
#include "UnwindBacktrace.h"

enum MemType { HOST, MMAP, DMA };

struct FrameKeyType {
    uint16_t frame_count = 0;
    uint64_t module_generation = 0;
    std::array<uintptr_t, kMaxAsyncRawFrames> pcs{};

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
    std::vector<uintptr_t> frames;
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
    size_t AddBacktrace(size_t num_frames, size_t size_bytes);
    void Remove(const void* ptr);
    void RemoveBacktrace(size_t hash_index);

    void DumpLiveToFile(int fd, bool dump_peak = true);
    void DumpPeakInfo();
    void FlushAsync();
    void BeginFinalization();
    void CompleteAsyncStack(const StackResult& result);
    AsyncStackStats AsyncStats() const;

private:
    inline uintptr_t ManglePointer(uintptr_t pointer) { return pointer ^ UINTPTR_MAX; }
    inline uintptr_t DemanglePointer(uintptr_t pointer) {
        return pointer ^ UINTPTR_MAX;
    }

    void GetList(std::vector<ListInfoType>* list, bool only_with_backtrace, Pred pred);
    void GetUniqueList(std::vector<ListInfoType>* list, bool only_with_backtrace);

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
    static constexpr size_t kPointerFilterWords = 1 << 13;
    std::array<std::atomic<uint64_t>, kPointerFilterWords> pointer_filter_{};

    PointerData(const PointerData&) = delete;
    PointerData& operator=(const PointerData&) = delete;
};
