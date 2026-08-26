#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AsyncStackPipeline.h"

namespace {

class RecordingSymbolizer final : public Symbolizer {
public:
    bool Symbolize(
            const RawStackRecord& raw, const std::vector<ModuleInfo>& modules,
            std::vector<SymbolizedFrame>* frames) override {
        {
            std::lock_guard<std::mutex> guard(mutex);
            ++calls;
            worker_id = std::this_thread::get_id();
            started.notify_all();
        }
        frames->clear();
        for (size_t i = 0; i < raw.frame_count; ++i) {
            SymbolizedFrame frame;
            frame.pc = raw.pcs[i];
            if (i < modules.size() && !modules[i].name.empty()) {
                frame.module_name = modules[i].name;
                frame.module_start = modules[i].start;
                frame.rel_pc = frame.pc - frame.module_start;
            } else {
                frame.rel_pc = frame.pc;
            }
            frames->push_back(std::move(frame));
        }
        return !frames->empty();
    }

    void WaitUntilStarted() {
        std::unique_lock<std::mutex> lock(mutex);
        started.wait(lock, [this]() { return calls != 0; });
    }

    std::mutex mutex;
    std::condition_variable started;
    size_t calls = 0;
    std::thread::id worker_id;
};

class BlockingSymbolizer final : public Symbolizer {
public:
    bool Symbolize(
            const RawStackRecord& raw, const std::vector<ModuleInfo>&,
            std::vector<SymbolizedFrame>* frames) override {
        std::unique_lock<std::mutex> lock(mutex);
        started = true;
        started_cv.notify_all();
        release_cv.wait(lock, [this]() { return released; });
        frames->resize(raw.frame_count);
        return !frames->empty();
    }

    void WaitUntilStarted() {
        std::unique_lock<std::mutex> lock(mutex);
        started_cv.wait(lock, [this]() { return started; });
    }

    void Release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        release_cv.notify_all();
    }

    std::mutex mutex;
    std::condition_variable started_cv;
    std::condition_variable release_cv;
    bool started = false;
    bool released = false;
};

class PartialResolver final : public ModuleResolver {
public:
    uint64_t CurrentGeneration() const override { return 1; }

    bool Resolve(uintptr_t pc, uint64_t generation, ModuleInfo* module) override {
        if (module == nullptr || generation != 1 || pc == 0x2002) {
            return false;
        }
        module->generation = generation;
        module->start = 0x2000;
        module->end = 0x3000;
        module->name = "partial.so";
        return true;
    }
};

struct CallbackContext {
    AsyncStackPipeline* pipeline = nullptr;
    AsyncStackPipeline::SubmitResult nested;
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    bool exited = false;
    bool flush_started = false;
    bool flush_returned = false;
};

void BlockingCompletion(void* opaque, const StackResult&) {
    auto* context = static_cast<CallbackContext*>(opaque);
    std::unique_lock<std::mutex> lock(context->mutex);
    context->entered = true;
    context->condition.notify_all();
    context->condition.wait(lock, [context]() { return context->release; });
    context->exited = true;
    context->condition.notify_all();
}

void RecursiveCompletion(void* opaque, const StackResult& result) {
    auto* context = static_cast<CallbackContext*>(opaque);
    context->entered = true;
    context->nested = context->pipeline->Submit(result.raw);
}

struct AllocatingCompletionContext {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool worker = false;
    size_t allocated = 0;
};

void AllocatingCompletion(void* opaque, const StackResult&) {
    auto* context = static_cast<AllocatingCompletionContext*>(opaque);
    std::vector<std::string> frames;
    frames.emplace_back("completion allocation");
    std::lock_guard<std::mutex> lock(context->mutex);
    context->entered = true;
    context->worker = AsyncStackWorkerThread();
    context->allocated = frames.front().size();
    context->condition.notify_all();
}

void ReentrantFlushCompletion(void* opaque, const StackResult&) {
    auto* context = static_cast<CallbackContext*>(opaque);
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->entered = true;
        context->condition.notify_all();
    }
    context->pipeline->Flush();
}

RawStackRecord MakeRaw(uintptr_t pc, uint64_t generation = 0) {
    RawStackRecord raw;
    raw.capture_state = StackCaptureState::Complete;
    raw.frame_count = 1;
    raw.pcs[0] = pc;
    raw.module_generation = generation;
    return raw;
}

}  // namespace

TEST(AsyncStackPipeline, DeduplicatesAndResolvesOffSubmitThread) {
    auto symbolizer = std::make_unique<RecordingSymbolizer>();
    RecordingSymbolizer* symbolizer_ptr = symbolizer.get();
    auto resolver = std::make_unique<ModuleRegistry>();
    resolver->Publish({ModuleInfo{.start = 0x1000, .end = 0x2000, .name = "libone.so", .build_id = ""}});
    AsyncStackPipeline pipeline(
            std::move(resolver), std::move(symbolizer), 8);

    const auto raw = MakeRaw(0x1010, 2);
    const auto first = pipeline.Submit(raw);
    const auto duplicate = pipeline.Submit(raw);
    ASSERT_TRUE(first.accepted);
    ASSERT_TRUE(duplicate.duplicate);
    EXPECT_EQ(first.id, duplicate.id);

    symbolizer_ptr->WaitUntilStarted();
    EXPECT_NE(symbolizer_ptr->worker_id, std::this_thread::get_id());
    pipeline.Flush();

    StackResult result;
    ASSERT_TRUE(pipeline.GetResult(first.id, &result));
    EXPECT_EQ(result.state, StackResolutionState::Resolved);
    ASSERT_EQ(result.frames.size(), 1u);
    EXPECT_EQ(result.frames[0].module_name, "libone.so");
    const auto stats = pipeline.stats();
    EXPECT_EQ(stats.accepted, 1u);
    EXPECT_EQ(stats.duplicates, 1u);
}

TEST(AsyncStackPipeline, DropsOnBoundedQueueAndFlushesAcceptedWork) {
    auto symbolizer = std::make_unique<BlockingSymbolizer>();
    BlockingSymbolizer* symbolizer_ptr = symbolizer.get();
    AsyncStackPipeline pipeline(
            std::make_unique<ModuleRegistry>(), std::move(symbolizer), 2);

    const auto first = pipeline.Submit(MakeRaw(0x1001, 1));
    symbolizer_ptr->WaitUntilStarted();
    const auto second = pipeline.Submit(MakeRaw(0x1002, 1));
    const auto third = pipeline.Submit(MakeRaw(0x1003, 1));
    const auto dropped = pipeline.Submit(MakeRaw(0x1004, 1));
    ASSERT_TRUE(first.accepted);
    ASSERT_TRUE(second.accepted);
    ASSERT_TRUE(third.accepted);
    EXPECT_TRUE(dropped.dropped);

    symbolizer_ptr->Release();
    pipeline.Flush();
    const auto stats = pipeline.stats();
    EXPECT_EQ(stats.accepted, 3u);
    EXPECT_EQ(stats.dropped, 1u);
    EXPECT_GE(stats.queue_high_water, 2u);
    StackResult dropped_result;
    ASSERT_TRUE(pipeline.GetResult(dropped.id, &dropped_result));
    EXPECT_EQ(dropped_result.state, StackResolutionState::Dropped);

    const auto retry = pipeline.Submit(MakeRaw(0x1004, 1));
    EXPECT_TRUE(retry.accepted);
    EXPECT_NE(retry.id, dropped.id);
    pipeline.Flush();
}

TEST(AsyncStackPipeline, DroppedRecordsAreReclaimedInsteadOfLeaking) {
    auto symbolizer = std::make_unique<BlockingSymbolizer>();
    BlockingSymbolizer* symbolizer_ptr = symbolizer.get();
    // Capacity 1 also bounds the retained-record count at 1, which makes the
    // reclamation observable without submitting hundreds of stacks.
    AsyncStackPipeline pipeline(
            std::make_unique<ModuleRegistry>(), std::move(symbolizer), 1);

    ASSERT_TRUE(pipeline.Submit(MakeRaw(0x2001, 1)).accepted);
    // The worker has taken the first item, so exactly one queue slot is free.
    symbolizer_ptr->WaitUntilStarted();
    ASSERT_TRUE(pipeline.Submit(MakeRaw(0x2002, 1)).accepted);

    const auto first_drop = pipeline.Submit(MakeRaw(0x2003, 1));
    const auto second_drop = pipeline.Submit(MakeRaw(0x2004, 1));
    ASSERT_TRUE(first_drop.dropped);
    ASSERT_TRUE(second_drop.dropped);

    // Dropped submissions are retained in a bounded FIFO: the newest stays
    // queryable for diagnostics while the older one is reclaimed. Retaining
    // every drop forever would leak a full RawStackRecord per dropped stack.
    StackResult result;
    EXPECT_FALSE(pipeline.GetResult(first_drop.id, &result));
    ASSERT_TRUE(pipeline.GetResult(second_drop.id, &result));
    EXPECT_EQ(result.state, StackResolutionState::Dropped);

    symbolizer_ptr->Release();
    pipeline.Flush();
    EXPECT_EQ(pipeline.stats().dropped, 2u);
}

TEST(AsyncStackPipeline, DuplicateSubmitPublishesTheCachedResult) {
    auto resolver = std::make_unique<ModuleRegistry>();
    ModuleRegistry* registry = resolver.get();
    const uint64_t generation = registry->Publish({ModuleInfo{
            .start = 0x3000, .end = 0x4000, .name = "cached.so", .build_id = ""}});
    AsyncStackPipeline pipeline(
            std::move(resolver), std::make_unique<RecordingSymbolizer>(), 4);

    const RawStackRecord raw = MakeRaw(0x3010, generation);
    const auto first = pipeline.Submit(raw);
    ASSERT_TRUE(first.accepted);
    pipeline.Flush();

    // A new owner of an already-resolved stack (its previous owner released its
    // frame entry) is told the submission is a duplicate and gets no further
    // completion callback. The cached result must therefore be readable, or the
    // report would show a permanently pending stack with no frames.
    const auto again = pipeline.Submit(raw);
    EXPECT_TRUE(again.duplicate);
    EXPECT_FALSE(again.accepted);
    EXPECT_EQ(again.id, first.id);

    StackResult cached;
    ASSERT_TRUE(pipeline.GetResult(again.id, &cached));
    EXPECT_EQ(cached.state, StackResolutionState::Resolved);
    ASSERT_EQ(cached.frames.size(), 1u);
    EXPECT_EQ(cached.frames[0].module_name, "cached.so");
}

TEST(AsyncStackPipeline, ModuleGenerationPreventsAddressReuseMisSymbolization) {
    auto resolver = std::make_unique<ModuleRegistry>();
    ModuleRegistry* registry = resolver.get();
    const uint64_t first_generation =
            registry->Publish({ModuleInfo{.start = 0x4000, .end = 0x5000, .name = "module-a", .build_id = ""}});
    auto symbolizer = std::make_unique<RecordingSymbolizer>();
    AsyncStackPipeline pipeline(
            std::move(resolver), std::move(symbolizer), 8);

    const auto first = pipeline.Submit(MakeRaw(0x4010, first_generation));
    pipeline.Flush();
    const uint64_t second_generation =
            registry->Publish({ModuleInfo{.start = 0x4000, .end = 0x5000, .name = "module-b", .build_id = ""}});
    const auto second = pipeline.Submit(MakeRaw(0x4010, second_generation));
    pipeline.Flush();

    StackResult first_result;
    StackResult second_result;
    ASSERT_TRUE(pipeline.GetResult(first.id, &first_result));
    ASSERT_TRUE(pipeline.GetResult(second.id, &second_result));
    ASSERT_EQ(first_result.frames.size(), 1u);
    ASSERT_EQ(second_result.frames.size(), 1u);
    EXPECT_EQ(first_result.frames[0].module_name, "module-a");
    EXPECT_EQ(second_result.frames[0].module_name, "module-b");
}

TEST(AsyncStackPipeline, BoundsGenerationRetentionWithoutUsingNewerSnapshot) {
    ModuleRegistry registry(2);
    const uint64_t evicted =
            registry.Publish({ModuleInfo{.start = 0x5000, .end = 0x6000, .name = "old", .build_id = ""}});
    registry.Publish({ModuleInfo{.start = 0x5000, .end = 0x6000, .name = "middle", .build_id = ""}});
    const uint64_t current =
            registry.Publish({ModuleInfo{.start = 0x5000, .end = 0x6000, .name = "new", .build_id = ""}});

    EXPECT_EQ(registry.SnapshotCount(), 2u);
    ModuleInfo module;
    EXPECT_FALSE(registry.Resolve(0x5010, evicted, &module));
    ASSERT_TRUE(registry.Resolve(0x5010, current, &module));
    EXPECT_EQ(module.name, "new");
}

TEST(AsyncStackPipeline, ShutdownDrainsAndRejectsNewSubmissions) {
    auto resolver = std::make_unique<ModuleRegistry>();
    resolver->Publish({ModuleInfo{
            .start = 0x7000, .end = 0x8000, .name = "shutdown.so", .build_id = ""}});
    AsyncStackPipeline pipeline(
            std::move(resolver), std::make_unique<RecordingSymbolizer>(), 4);
    const auto accepted = pipeline.Submit(MakeRaw(0x7000));
    pipeline.Shutdown();
    const auto rejected = pipeline.Submit(MakeRaw(0x7001));
    EXPECT_TRUE(accepted.accepted);
    EXPECT_FALSE(rejected.accepted);
    EXPECT_FALSE(rejected.duplicate);
    StackResult result;
    ASSERT_TRUE(pipeline.GetResult(accepted.id, &result));
    EXPECT_EQ(result.state, StackResolutionState::Resolved);
    pipeline.Shutdown();
}

TEST(AsyncStackPipeline, PartialResolutionPreservesFramesAndFailureState) {
    AsyncStackPipeline pipeline(
            std::make_unique<PartialResolver>(),
            std::make_unique<RecordingSymbolizer>(), 4);
    RawStackRecord raw;
    raw.capture_state = StackCaptureState::Partial;
    raw.terminal_error = 7;
    raw.module_generation = 1;
    raw.frame_count = 2;
    raw.pcs[0] = 0x2001;
    raw.pcs[1] = 0x2002;

    const auto submitted = pipeline.Submit(raw);
    ASSERT_TRUE(submitted.accepted);
    pipeline.Flush();

    StackResult result;
    ASSERT_TRUE(pipeline.GetResult(submitted.id, &result));
    EXPECT_EQ(result.state, StackResolutionState::Failed);
    EXPECT_EQ(result.raw.capture_state, StackCaptureState::Partial);
    EXPECT_EQ(result.raw.terminal_error, 7);
    EXPECT_EQ(result.frames.size(), 2u);
    EXPECT_EQ(result.frames[0].module_name, "partial.so");
    EXPECT_TRUE(result.frames[1].module_name.empty());
}

TEST(AsyncStackPipeline, FlushWaitsForCompletionCallback) {
    CallbackContext context;
    AsyncStackPipeline pipeline(
            std::make_unique<ModuleRegistry>(),
            std::make_unique<RecordingSymbolizer>(), 4, BlockingCompletion, &context);
    context.pipeline = &pipeline;

    ASSERT_TRUE(pipeline.Submit(MakeRaw(0x3001)).accepted);
    {
        std::unique_lock<std::mutex> lock(context.mutex);
        context.condition.wait(lock, [&context]() { return context.entered; });
    }
    std::thread waiter([&pipeline, &context]() {
        {
            std::lock_guard<std::mutex> lock(context.mutex);
            context.flush_started = true;
            context.condition.notify_all();
        }
        pipeline.Flush();
        std::lock_guard<std::mutex> lock(context.mutex);
        context.flush_returned = true;
        context.condition.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(context.mutex);
        context.condition.wait(lock, [&context]() { return context.flush_started; });
        EXPECT_FALSE(context.condition.wait_for(
                lock, std::chrono::milliseconds(20),
                [&context]() { return context.flush_returned; }));
        context.release = true;
        context.condition.notify_all();
    }
    waiter.join();
    EXPECT_TRUE(context.exited);
    EXPECT_TRUE(context.flush_returned);
}

TEST(AsyncStackPipeline, CallbackRecursionIsSuppressed) {
    CallbackContext context;
    AsyncStackPipeline pipeline(
            std::make_unique<ModuleRegistry>(),
            std::make_unique<RecordingSymbolizer>(), 4, RecursiveCompletion, &context);
    context.pipeline = &pipeline;

    const auto submitted = pipeline.Submit(MakeRaw(0x3002));
    ASSERT_TRUE(submitted.accepted);
    pipeline.Flush();
    EXPECT_TRUE(context.entered);
    EXPECT_TRUE(context.nested.rejected_recursion);
    EXPECT_EQ(pipeline.stats().recursive_submissions, 1u);
}

TEST(AsyncStackPipeline, CompletionCallbackCanCallFlushWithoutDeadlock) {
    CallbackContext context;
    AsyncStackPipeline pipeline(
            std::make_unique<ModuleRegistry>(),
            std::make_unique<RecordingSymbolizer>(), 4, ReentrantFlushCompletion, &context);
    context.pipeline = &pipeline;

    ASSERT_TRUE(pipeline.Submit(MakeRaw(0x3003)).accepted);
    {
        std::unique_lock<std::mutex> lock(context.mutex);
        ASSERT_TRUE(context.condition.wait_for(
                lock, std::chrono::seconds(1), [&context]() { return context.entered; }));
    }
    pipeline.Flush();
    EXPECT_TRUE(context.entered);
}

TEST(AsyncStackPipeline, CompletionAllocationsStayOnWorkerWithoutRecursiveTracking) {
    AllocatingCompletionContext context;
    AsyncStackPipeline pipeline(
            std::make_unique<ModuleRegistry>(),
            std::make_unique<RecordingSymbolizer>(), 4, AllocatingCompletion, &context);

    ASSERT_TRUE(pipeline.Submit(MakeRaw(0x3004)).accepted);
    pipeline.Flush();
    std::unique_lock<std::mutex> lock(context.mutex);
    ASSERT_TRUE(context.condition.wait_for(
            lock, std::chrono::seconds(1), [&context]() { return context.entered; }));
    EXPECT_TRUE(context.worker);
    EXPECT_GT(context.allocated, 0u);
}
