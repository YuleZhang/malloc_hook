#include "AsyncStackPipeline.h"
#include "UnwindBacktrace.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

namespace {

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::cerr << "runtime smoke check failed: " #condition << '\n';      \
            return 1;                                                             \
        }                                                                         \
    } while (false)

class AnyModuleResolver final : public ModuleResolver {
public:
    uint64_t CurrentGeneration() const override { return 1; }

    bool Resolve(uintptr_t pc, uint64_t generation, ModuleInfo* module) override {
        if (module == nullptr || generation != 1) {
            return false;
        }
        module->generation = generation;
        module->start = pc > 0x100 ? pc - 0x100 : 0;
        module->end = pc + 0x100;
        module->name = "smoke-module";
        return true;
    }
};

class RefreshTrackingResolver final : public ModuleResolver {
public:
    uint64_t CurrentGeneration() const override { return 1; }

    bool Resolve(uintptr_t pc, uint64_t generation, ModuleInfo* module) override {
        if (module == nullptr || generation != 1) {
            return false;
        }
        module->generation = generation;
        module->start = pc > 0x100 ? pc - 0x100 : 0;
        module->end = pc + 0x100;
        module->name = "refresh-tracking-module";
        return true;
    }

    bool RefreshIfSupported() override {
        ++refresh_calls;
        return true;
    }

    size_t refresh_calls = 0;
};

class EchoSymbolizer final : public Symbolizer {
public:
    bool Symbolize(
            const RawStackRecord& raw, const std::vector<ModuleInfo>& modules,
            std::vector<SymbolizedFrame>* frames) override {
        if (frames == nullptr) {
            return false;
        }
        calls++;
        ran_on_worker = AsyncStackWorkerThread();
        frames->clear();
        for (size_t i = 0; i < raw.frame_count; ++i) {
            SymbolizedFrame frame;
            frame.pc = raw.pcs[i];
            frame.module_name = i < modules.size() ? modules[i].name : "";
            frame.module_start = i < modules.size() ? modules[i].start : 0;
            frame.rel_pc = frame.module_start == 0 ? frame.pc : frame.pc - frame.module_start;
            frames->push_back(std::move(frame));
        }
        return !frames->empty();
    }

    size_t calls = 0;
    bool ran_on_worker = false;
};

class BlockingSymbolizer final : public Symbolizer {
public:
    bool Symbolize(
            const RawStackRecord& raw, const std::vector<ModuleInfo>&,
            std::vector<SymbolizedFrame>* frames) override {
        std::unique_lock<std::mutex> lock(mutex_);
        started_ = true;
        started_cv_.notify_all();
        release_cv_.wait(lock, [this]() { return released_; });
        frames->resize(raw.frame_count);
        return !frames->empty();
    }

    void WaitUntilStarted() {
        std::unique_lock<std::mutex> lock(mutex_);
        started_cv_.wait(lock, [this]() { return started_; });
    }

    void Release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        release_cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable started_cv_;
    std::condition_variable release_cv_;
    bool started_ = false;
    bool released_ = false;
};

struct FailureCallbackContext {
    size_t calls = 0;
    StackResolutionState state = StackResolutionState::Pending;
};

void RecordFailureCallback(void* opaque, const StackResult& result) {
    auto* context = static_cast<FailureCallbackContext*>(opaque);
    ++context->calls;
    context->state = result.state;
}

RawStackRecord MakeRaw(uintptr_t pc) {
    RawStackRecord raw;
    raw.capture_state = StackCaptureState::Complete;
    raw.mode = StackCaptureMode::Fast;
    raw.backend = StackCaptureBackend::CompilerUnwind;
    raw.module_generation = 1;
    raw.frame_count = 2;
    raw.pcs[0] = pc;
    raw.pcs[1] = pc + 4;
    return raw;
}

}  // namespace

int main() {
    constexpr size_t kIterations = 32;
    for (size_t i = 0; i < 4; ++i) {
        (void)CaptureStack(StackCaptureMode::Fast, 16, 0);
        (void)CaptureStack(StackCaptureMode::Accurate, 16, 0);
    }
    const auto fast_start = std::chrono::steady_clock::now();
    RawStackRecord fast;
    for (size_t i = 0; i < kIterations; ++i) {
        fast = CaptureStack(StackCaptureMode::Fast, 16, 0);
    }
    const auto fast_elapsed = std::chrono::steady_clock::now() - fast_start;
    const auto accurate_start = std::chrono::steady_clock::now();
    RawStackRecord accurate;
    for (size_t i = 0; i < kIterations; ++i) {
        accurate = CaptureStack(StackCaptureMode::Accurate, 16, 0);
    }
    const auto accurate_elapsed = std::chrono::steady_clock::now() - accurate_start;

    CHECK(fast.frame_count <= 16);
    CHECK(accurate.frame_count <= 16);
    CHECK(fast_elapsed.count() > 0);
    CHECK(accurate_elapsed.count() > 0);

    auto symbolizer = std::make_unique<EchoSymbolizer>();
    EchoSymbolizer* symbolizer_ptr = symbolizer.get();
    AsyncStackPipeline pipeline(
        std::make_unique<AnyModuleResolver>(),
        std::move(symbolizer), 8);
    const auto first = pipeline.Submit(MakeRaw(0x1000));
    const auto duplicate = pipeline.Submit(MakeRaw(0x1000));
    CHECK(first.accepted);
    CHECK(duplicate.duplicate);
    pipeline.Flush();

    StackResult result;
    CHECK(pipeline.GetResult(first.id, &result));
    CHECK(result.state == StackResolutionState::Resolved);
    CHECK(result.raw == MakeRaw(0x1000));
    CHECK(result.frames.size() == 2);
    CHECK(result.frames[0].rel_pc == result.frames[0].pc - result.frames[0].module_start);
    CHECK(symbolizer_ptr->calls == 1);
    CHECK(symbolizer_ptr->ran_on_worker);

    auto refresh_resolver = std::make_unique<RefreshTrackingResolver>();
    RefreshTrackingResolver* refresh_resolver_ptr = refresh_resolver.get();
    AsyncStackPipeline flush_pipeline(
            std::move(refresh_resolver),
            std::make_unique<EchoSymbolizer>(), 4);
    CHECK(flush_pipeline.Submit(MakeRaw(0x1800)).accepted);
    flush_pipeline.Flush();
    // A normal checkpoint may refresh the module snapshot, but the explicit
    // finalization barrier must make subsequent Flush calls wait-only.
    CHECK(refresh_resolver_ptr->refresh_calls > 0);
    const size_t refreshes_before_finalization = refresh_resolver_ptr->refresh_calls;
    flush_pipeline.BeginFinalization();
    CHECK(flush_pipeline.Submit(MakeRaw(0x1804)).accepted);
    flush_pipeline.Flush();
    CHECK(refresh_resolver_ptr->refresh_calls == refreshes_before_finalization);
    flush_pipeline.Shutdown();

    for (uintptr_t pc = 0x2000; pc < 0x2040; pc += 4) {
        pipeline.Submit(MakeRaw(pc));
    }
    pipeline.Flush();
    const AsyncStackStats stats = pipeline.stats();
    CHECK(stats.accepted >= 2);
    CHECK(stats.processed == stats.accepted);
    CHECK(stats.queue_high_water > 0);
    pipeline.Shutdown();
    CHECK(!pipeline.Submit(MakeRaw(0x3000)).accepted);

    ModuleRegistry generations(1);
    const uint64_t old_generation =
        generations.Publish({ModuleInfo{.start = 0x4000, .end = 0x5000, .name = "old"}});
    const uint64_t new_generation =
        generations.Publish({ModuleInfo{.start = 0x4000, .end = 0x5000, .name = "new"}});
    ModuleInfo module;
    CHECK(!generations.Resolve(0x4010, old_generation, &module));
    CHECK(generations.Resolve(0x4010, new_generation, &module));
    CHECK(module.name == "new");

    AsyncStackPipelineForceWorkerMarkerFailureForTesting(true);
    AsyncStackPipeline failed_worker(
            std::make_unique<AnyModuleResolver>(),
            std::make_unique<EchoSymbolizer>(), 2);
    const auto failed_submission = failed_worker.Submit(MakeRaw(0x4800));
    CHECK(failed_submission.accepted);
    failed_worker.Flush();
    StackResult failed_result;
    CHECK(failed_worker.GetResult(failed_submission.id, &failed_result));
    CHECK(failed_result.state == StackResolutionState::Failed);
    FailureCallbackContext failure_callback;
    AsyncStackPipelineForceWorkerMarkerFailureForTesting(true);
    AsyncStackPipeline callback_failed_worker(
            std::make_unique<AnyModuleResolver>(),
            std::make_unique<EchoSymbolizer>(), 2,
            RecordFailureCallback, &failure_callback);
    const auto callback_submission = callback_failed_worker.Submit(MakeRaw(0x4808));
    CHECK(callback_submission.accepted);
    callback_failed_worker.Flush();
    CHECK(failure_callback.calls == 1);
    CHECK(failure_callback.state == StackResolutionState::Failed);
    callback_failed_worker.Shutdown();
    const AsyncStackStats failed_stats = failed_worker.stats();
    CHECK(failed_stats.accepted == 1);
    CHECK(failed_stats.processed == 1);
    CHECK(failed_stats.failed == 1);
    CHECK(failed_stats.worker_start_failures == 1);
    CHECK(!failed_worker.Submit(MakeRaw(0x4804)).accepted);
    failed_worker.Shutdown();
    AsyncStackPipelineForceWorkerMarkerFailureForTesting(false);

    auto blocking_symbolizer = std::make_unique<BlockingSymbolizer>();
    BlockingSymbolizer* blocking = blocking_symbolizer.get();
    AsyncStackPipeline bounded(
        std::make_unique<AnyModuleResolver>(), std::move(blocking_symbolizer), 2);
    CHECK(bounded.Submit(MakeRaw(0x5000)).accepted);
    blocking->WaitUntilStarted();
    CHECK(bounded.Submit(MakeRaw(0x5004)).accepted);
    CHECK(bounded.Submit(MakeRaw(0x5008)).accepted);
    const auto overflow = bounded.Submit(MakeRaw(0x500c));
    CHECK(overflow.dropped);
    blocking->Release();
    bounded.Flush();
    CHECK(bounded.stats().dropped == 1);
    bounded.Shutdown();

    std::cout << "runtime smoke gate passed: fast_ns="
              << std::chrono::duration_cast<std::chrono::nanoseconds>(fast_elapsed).count()
              << " accurate_ns="
              << std::chrono::duration_cast<std::chrono::nanoseconds>(accurate_elapsed).count()
              << " fast_per_call_ns="
              << std::chrono::duration_cast<std::chrono::nanoseconds>(fast_elapsed).count() /
                     kIterations
              << " accurate_per_call_ns="
              << std::chrono::duration_cast<std::chrono::nanoseconds>(accurate_elapsed).count() /
                     kIterations
              << " accepted=" << stats.accepted
              << " processed=" << stats.processed
              << " dropped=" << stats.dropped << '\n';
    return 0;
}
