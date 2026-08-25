#include "AsyncStackPipeline.h"
#include "DebugData.h"
#include "debug_disable.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

namespace {

std::mutex g_mutex;
std::condition_variable g_condition;
bool g_callback_entered = false;
bool g_callback_on_worker = false;
size_t g_malloc_calls = 0;
size_t g_free_calls = 0;
alignas(std::max_align_t) unsigned char g_storage[64];

void* CountingMalloc(size_t size) {
    if (size > sizeof(g_storage)) {
        return nullptr;
    }
    ++g_malloc_calls;
    return g_storage;
}

void CountingFree(void* pointer) {
    if (pointer == g_storage) {
        ++g_free_calls;
    }
}

class Resolver final : public ModuleResolver {
public:
    uint64_t CurrentGeneration() const override { return 1; }

    bool Resolve(uintptr_t pc, uint64_t generation, ModuleInfo* module) override {
        if (module == nullptr || generation != 1) {
            return false;
        }
        module->generation = generation;
        module->start = pc;
        module->end = pc + 1;
        module->name = "task10-test";
        return true;
    }
};

class RecordingSymbolizer final : public ::Symbolizer {
public:
    bool Symbolize(
            const RawStackRecord& raw, const std::vector<ModuleInfo>& modules,
            std::vector<SymbolizedFrame>* frames) override {
        if (frames == nullptr) {
            return false;
        }
        frames->clear();
        for (size_t i = 0; i < raw.frame_count; ++i) {
            SymbolizedFrame frame;
            frame.pc = raw.pcs[i];
            if (i < modules.size()) {
                frame.module_name = modules[i].name;
                frame.module_start = modules[i].start;
                frame.rel_pc = frame.pc - frame.module_start;
            }
            frames->push_back(std::move(frame));
        }
        return !frames->empty();
    }
};

void Completion(void*, const StackResult&) {
    if (!AsyncStackWorkerThread()) {
        return;
    }
    // If the async-worker bypass is removed, g_debug is null and this path
    // must fail before reaching the fake allocator bookkeeping below.
    void* pointer = debug_malloc(sizeof(g_storage));
    debug_free(pointer);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_callback_on_worker = true;
        g_callback_entered = true;
    }
    g_condition.notify_all();
}

RawStackRecord MakeRaw() {
    RawStackRecord raw;
    raw.capture_state = StackCaptureState::Complete;
    raw.mode = StackCaptureMode::Fast;
    raw.backend = StackCaptureBackend::CompilerUnwind;
    raw.module_generation = 1;
    raw.frame_count = 1;
    raw.pcs[0] = 0x1000;
    return raw;
}

}  // namespace

int main() {
    if (!DebugDisableInitialize()) {
        std::fputs("DebugDisableInitialize failed\n", stderr);
        return 1;
    }
    m_sys_malloc = CountingMalloc;
    m_sys_free = CountingFree;
    g_debug = nullptr;

    bool passed = false;
    {
        AsyncStackPipeline pipeline(
                std::make_unique<Resolver>(), std::make_unique<RecordingSymbolizer>(), 2,
                Completion, nullptr);
        const auto submitted = pipeline.Submit(MakeRaw());
        pipeline.Flush();

        std::unique_lock<std::mutex> lock(g_mutex);
        const bool completed = submitted.accepted && g_condition.wait_for(
                lock, std::chrono::seconds(1), []() { return g_callback_entered; });
        passed = completed && g_callback_on_worker && g_malloc_calls == 1 &&
                 g_free_calls == 1;
        pipeline.Shutdown();
    }
    DebugDisableFinalize();
    if (!passed) {
        std::fputs("task10 hook boundary gate failed\n", stderr);
        return 1;
    }
    std::puts("task10 hook boundary gate passed");
    return 0;
}
