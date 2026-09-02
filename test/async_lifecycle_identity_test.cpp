#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "AsyncStackPipeline.h"

namespace {

#define CHECK_TRUE(condition)                                                   \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ":" << __LINE__                           \
                      << ": check failed: " #condition << std::endl;            \
            return false;                                                       \
        }                                                                       \
    } while (false)

class PassthroughSymbolizer final : public Symbolizer {
public:
    bool Symbolize(
            const RawStackRecord& raw, const std::vector<ModuleInfo>& modules,
            std::vector<SymbolizedFrame>* frames) override {
        frames->clear();
        for (size_t i = 0; i < raw.frame_count; ++i) {
            SymbolizedFrame frame;
            frame.pc = raw.pcs[i];
            if (i < modules.size()) {
                frame.module_start = modules[i].start;
                frame.module_name = modules[i].name;
                frame.rel_pc = frame.pc - frame.module_start;
            }
            frames->push_back(std::move(frame));
        }
        return !frames->empty();
    }
};

RawStackRecord MakeRaw(uintptr_t pc, uint64_t generation) {
    RawStackRecord raw;
    raw.capture_state = StackCaptureState::Complete;
    raw.mode = StackCaptureMode::Fast;
    raw.backend = StackCaptureBackend::CompilerUnwind;
    raw.module_generation = generation;
    raw.frame_count = 1;
    raw.pcs[0] = pc;
    return raw;
}

template <typename T, typename = void>
struct HasRelease : std::false_type {};

template <typename T>
struct HasRelease<
        T, std::void_t<decltype(std::declval<T&>().Release(AsyncStackId{}))>>
    : std::true_type {};

template <typename Pipeline>
bool ReleaseLastReference(Pipeline* pipeline, AsyncStackId id) {
    if constexpr (!HasRelease<Pipeline>::value) {
        return false;
    } else if constexpr (std::is_same_v<
                                 decltype(pipeline->Release(id)), bool>) {
        return pipeline->Release(id);
    } else {
        pipeline->Release(id);
        return true;
    }
}

template <typename T, typename = void>
struct HasBuildId : std::false_type {};

template <typename T>
struct HasBuildId<T, std::void_t<decltype(std::declval<T>().build_id)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasModuleGeneration : std::false_type {};

template <typename T>
struct HasModuleGeneration<
        T, std::void_t<decltype(std::declval<T>().module_generation)>>
    : std::true_type {};

template <typename Frame>
bool HasDurableIdentity(
        const Frame& frame, const std::string& build_id, uint64_t generation) {
    if constexpr (HasBuildId<Frame>::value &&
                  HasModuleGeneration<Frame>::value) {
        return frame.build_id == build_id &&
                frame.module_generation == generation;
    }
    return false;
}

bool ResolveReleaseAndRecaptureGetsFreshIdentity() {
    auto resolver = std::make_unique<ModuleRegistry>();
    const uint64_t generation = resolver->Publish({ModuleInfo{
            .start = 0x1000,
            .end = 0x2000,
            .load_bias = 0x1000,
            .name = "lifecycle.so",
            .build_id = "lifecycle-build-id"}});
    AsyncStackPipeline pipeline(
            std::move(resolver), std::make_unique<PassthroughSymbolizer>(), 4);
    const RawStackRecord raw = MakeRaw(0x1010, generation);

    const auto first = pipeline.Submit(raw);
    CHECK_TRUE(first.accepted);
    pipeline.Flush();
    StackResult result;
    CHECK_TRUE(pipeline.GetResult(first.id, &result));
    CHECK_TRUE(result.state == StackResolutionState::Resolved);

    CHECK_TRUE(ReleaseLastReference(&pipeline, first.id));
    CHECK_TRUE(!pipeline.GetResult(first.id, &result));

    const auto recaptured = pipeline.Submit(raw);
    CHECK_TRUE(recaptured.accepted);
    CHECK_TRUE(!recaptured.duplicate);
    CHECK_TRUE(recaptured.id != first.id);
    pipeline.Flush();
    CHECK_TRUE(pipeline.GetResult(recaptured.id, &result));
    CHECK_TRUE(result.state == StackResolutionState::Resolved);
    return true;
}

bool DedupAndResultCachesStayBounded() {
    auto resolver = std::make_unique<ModuleRegistry>();
    const uint64_t generation = resolver->Publish({ModuleInfo{
            .start = 0x2000,
            .end = 0x3000,
            .load_bias = 0x2000,
            .name = "bounded.so",
            .build_id = "bounded-build-id"}});
    AsyncStackPipeline pipeline(
            std::move(resolver), std::make_unique<PassthroughSymbolizer>(), 2);

    const auto first = pipeline.Submit(MakeRaw(0x2010, generation));
    CHECK_TRUE(first.accepted);
    pipeline.Flush();
    CHECK_TRUE(pipeline.Submit(MakeRaw(0x2020, generation)).accepted);
    pipeline.Flush();
    CHECK_TRUE(pipeline.Submit(MakeRaw(0x2030, generation)).accepted);
    pipeline.Flush();

    StackResult evicted;
    CHECK_TRUE(!pipeline.GetResult(first.id, &evicted));
    const auto recaptured = pipeline.Submit(MakeRaw(0x2010, generation));
    CHECK_TRUE(recaptured.accepted);
    CHECK_TRUE(!recaptured.duplicate);
    CHECK_TRUE(recaptured.id != first.id);
    pipeline.Flush();
    return true;
}

bool SymbolizedFramesCarryDurableModuleIdentity() {
    constexpr uint64_t kGeneration = 19;
    const std::string build_id = "0123456789abcdef";
    RawStackRecord raw = MakeRaw(0x4034, kGeneration);
    ModuleInfo module{
            .generation = kGeneration,
            .start = 0x4000,
            .end = 0x5000,
            .load_bias = 0x4000,
            .name = "identity.so",
            .build_id = build_id};
    NativeSymbolizer symbolizer;
    std::vector<SymbolizedFrame> frames;
    CHECK_TRUE(symbolizer.Symbolize(raw, {module}, &frames));
    CHECK_TRUE(frames.size() == 1);
    CHECK_TRUE(frames[0].pc == raw.pcs[0]);
    // rel_pc is the module-relative address of the *call site*, not of the
    // captured return address: symbolizing the return address resolves the
    // instruction after the call, which is a different source line and
    // sometimes a different function.
    CHECK_TRUE(frames[0].rel_pc ==
               CallSitePcFromReturnAddress(raw.pcs[0]) - module.load_bias);
    CHECK_TRUE(frames[0].rel_pc < 0x34);
    CHECK_TRUE(HasDurableIdentity(frames[0], build_id, kGeneration));
    return true;
}

bool BuildIdChangeCreatesAConservativeNewGeneration() {
    ModuleRegistry registry(2);
    const uint64_t first_generation = registry.Publish({ModuleInfo{
            .start = 0x6000,
            .end = 0x7000,
            .load_bias = 0x6000,
            .name = "reloaded.so",
            .build_id = "build-a"}});
    const uint64_t second_generation = registry.Publish({ModuleInfo{
            .start = 0x6000,
            .end = 0x7000,
            .load_bias = 0x6000,
            .name = "reloaded.so",
            .build_id = "build-b"}});
    CHECK_TRUE(second_generation != first_generation);

    ModuleInfo old_module;
    ModuleInfo new_module;
    CHECK_TRUE(registry.Resolve(0x6010, first_generation, &old_module));
    CHECK_TRUE(registry.Resolve(0x6010, second_generation, &new_module));
    CHECK_TRUE(old_module.build_id == "build-a");
    CHECK_TRUE(new_module.build_id == "build-b");

    registry.Publish({ModuleInfo{
            .start = 0x6000,
            .end = 0x7000,
            .load_bias = 0x6000,
            .name = "reloaded.so",
            .build_id = "build-c"}});
    ModuleInfo evicted;
    CHECK_TRUE(!registry.Resolve(0x6010, first_generation, &evicted));
    return true;
}

}  // namespace

int main() {
    bool passed = true;
    passed = ResolveReleaseAndRecaptureGetsFreshIdentity() && passed;
    passed = DedupAndResultCachesStayBounded() && passed;
    passed = SymbolizedFramesCarryDurableModuleIdentity() && passed;
    passed = BuildIdChangeCreatesAConservativeNewGeneration() && passed;
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
