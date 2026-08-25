#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "UnwindBacktrace.h"

using AsyncStackId = uint64_t;

// True only while the async resolver/symbolizer worker is executing. Hook
// boundaries use this backend-neutral predicate to keep worker internals out
// of allocation tracking.
bool AsyncStackWorkerThread();

#if defined(MALLOC_HOOK_ASYNC_STACK_TESTING)
void AsyncStackPipelineForceWorkerMarkerFailureForTesting(bool fail);
#endif

enum class StackResolutionState : uint8_t {
    Pending,
    Resolved,
    Dropped,
    Failed,
};

struct ModuleInfo {
    uint64_t generation = 0;
    uintptr_t start = 0;
    uintptr_t end = 0;
    // ELF load bias used for canonical module-relative PCs and dladdr
    // identity checks. This differs from the first PT_LOAD runtime address
    // for images whose lowest virtual segment does not start at zero.
    uintptr_t load_bias = 0;
    std::string name;
    std::string build_id;
};

class ModuleResolver {
public:
    virtual ~ModuleResolver() = default;
    virtual uint64_t CurrentGeneration() const = 0;
    virtual bool Resolve(uintptr_t pc, uint64_t generation, ModuleInfo* module) = 0;
    // Native resolvers may refresh their immutable process-module snapshot
    // from a worker/checkpoint boundary. The default keeps test resolvers
    // deterministic and avoids requiring a refresh implementation.
    virtual bool RefreshIfSupported() { return false; }
};

// A small immutable-snapshot registry useful for platform adapters and tests.
// A requested generation is never silently resolved against a newer snapshot.
class ModuleRegistry final : public ModuleResolver {
public:
    explicit ModuleRegistry(size_t max_retained_generations = 64);
    uint64_t CurrentGeneration() const override;
    bool Resolve(uintptr_t pc, uint64_t generation, ModuleInfo* module) override;
    uint64_t Publish(std::vector<ModuleInfo> modules);
    size_t SnapshotCount() const;

private:
    static bool Equivalent(
            const std::vector<ModuleInfo>& lhs, const std::vector<ModuleInfo>& rhs);

    mutable std::mutex mutex_;
    const size_t max_retained_generations_;
    uint64_t generation_ = 1;
    std::unordered_map<uint64_t, std::shared_ptr<const std::vector<ModuleInfo>>> snapshots_;
};

// Immutable module snapshots are refreshed explicitly outside hook execution.
// Old generations either resolve against their original snapshot or fail after
// bounded eviction; they are never resolved against a newer address occupant.
class NativeModuleResolver final : public ModuleResolver {
public:
    NativeModuleResolver();
    uint64_t CurrentGeneration() const override;
    bool Resolve(uintptr_t pc, uint64_t generation, ModuleInfo* module) override;
    uint64_t Refresh();
    bool RefreshIfSupported() override {
        Refresh();
        return true;
    }

private:
    ModuleRegistry registry_;
};

class Symbolizer {
public:
    virtual ~Symbolizer() = default;
    virtual bool Symbolize(
            const RawStackRecord& raw, const std::vector<ModuleInfo>& modules,
            std::vector<SymbolizedFrame>* frames) = 0;
};

class NativeSymbolizer final : public Symbolizer {
public:
    bool Symbolize(
            const RawStackRecord& raw, const std::vector<ModuleInfo>& modules,
            std::vector<SymbolizedFrame>* frames) override;
};

struct StackResult {
    AsyncStackId id = 0;
    StackResolutionState state = StackResolutionState::Pending;
    RawStackRecord raw;
    std::vector<SymbolizedFrame> frames;
};

struct AsyncStackStats {
    uint64_t accepted = 0;
    uint64_t duplicates = 0;
    uint64_t dropped = 0;
    uint64_t processed = 0;
    uint64_t failed = 0;
    uint64_t worker_start_failures = 0;
    uint64_t recursive_submissions = 0;
    size_t queue_high_water = 0;
};

// Stable, human-readable diagnostics shared by heap reports and tests.
std::string FormatAsyncStackStats(const AsyncStackStats& stats);

class AsyncStackPipeline {
public:
    using CompletionCallback = void (*)(void* opaque, const StackResult& result);

    struct SubmitResult {
        AsyncStackId id = 0;
        bool accepted = false;
        bool duplicate = false;
        bool dropped = false;
        bool rejected_recursion = false;
    };

    AsyncStackPipeline(
            std::unique_ptr<ModuleResolver> resolver = nullptr,
            std::unique_ptr<Symbolizer> symbolizer = nullptr,
            size_t capacity = 256, CompletionCallback callback = nullptr,
            void* callback_opaque = nullptr);
    ~AsyncStackPipeline();

    AsyncStackPipeline(const AsyncStackPipeline&) = delete;
    AsyncStackPipeline& operator=(const AsyncStackPipeline&) = delete;

    SubmitResult Submit(const RawStackRecord& raw);
    // Releases the caller's last reference to a completed stack result. The
    // raw-stack dedup key and cached result are removed together so a later
    // identical stack can be submitted again without retaining stale identity.
    bool Release(AsyncStackId id);
    // Returns the resolver generation used to distinguish loaded-module
    // snapshots. This is a read-only lookup suitable for capture-side
    // identity construction; it does not refresh or symbolize modules.
    uint64_t CurrentModuleGeneration() const;
    bool GetResult(AsyncStackId id, StackResult* result) const;
    AsyncStackStats stats() const;

    // Waits for all accepted work, including the completion callback, to finish.
    // A completion callback invoking Flush() returns immediately because it is
    // itself part of the external flush barrier and cannot wait for itself.
    void Flush();
    // Marks the pipeline as entering allocator/process finalization. This
    // waits for an in-flight native module refresh and prevents future
    // refreshes from allocating during teardown.
    void BeginFinalization();
    // Rejects new work, drains accepted work, and joins the worker.
    void Shutdown();

private:
    struct RawStackKey {
        uint16_t frame_count = 0;
        uint64_t module_generation = 0;
        std::array<uintptr_t, kMaxAsyncRawFrames> pcs{};
        bool operator==(const RawStackKey& other) const {
            if (frame_count != other.frame_count ||
                module_generation != other.module_generation) {
                return false;
            }
            for (size_t i = 0; i < frame_count; ++i) {
                if (pcs[i] != other.pcs[i]) {
                    return false;
                }
            }
            return true;
        }
    };

    struct RawStackKeyHash {
        size_t operator()(const RawStackKey& key) const;
    };

    struct QueueItem {
        AsyncStackId id = 0;
        RawStackRecord raw;
    };

    static RawStackKey MakeKey(const RawStackRecord& raw);
    void EvictResultCacheLocked();
    void WorkerLoop();

    std::unique_ptr<ModuleResolver> resolver_;
    std::unique_ptr<Symbolizer> symbolizer_;
    const size_t capacity_;
    const size_t result_capacity_;
    CompletionCallback callback_;
    void* callback_opaque_;

    // Serializes normal-runtime module refresh with Shutdown().  A refresh
    // performs dynamic-loader and container work; it must never race the
    // transition into allocator finalization.
    mutable std::mutex lifecycle_mutex_;
    std::atomic<bool> shutdown_started_{false};
    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable idle_cv_;
    std::vector<QueueItem> queue_;
    size_t queue_head_ = 0;
    size_t queue_size_ = 0;
    size_t active_ = 0;
    bool stopping_ = false;
    bool joined_ = false;
    std::thread::id worker_thread_id_{};
    AsyncStackId next_id_ = 1;
    AsyncStackStats stats_;
    std::unordered_map<RawStackKey, AsyncStackId, RawStackKeyHash> unique_stacks_;
    std::unordered_map<AsyncStackId, RawStackKey> id_to_key_;
    std::unordered_map<AsyncStackId, StackResult> results_;
    std::deque<AsyncStackId> result_order_;
    std::thread worker_;
};
