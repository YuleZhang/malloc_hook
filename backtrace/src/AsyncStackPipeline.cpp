#include "AsyncStackPipeline.h"

#include <dlfcn.h>
#if defined(__linux__) || defined(__ANDROID__) || defined(__OHOS__)
#include <elf.h>
#include <link.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <pthread.h>
#include <sstream>
#include <string>
#include <utility>

namespace {
pthread_key_t g_async_worker_key;
pthread_once_t g_async_worker_key_once = PTHREAD_ONCE_INIT;
std::atomic<bool> g_async_worker_key_ready{false};
#if defined(MALLOC_HOOK_ASYNC_STACK_TESTING)
std::atomic<bool> g_force_worker_marker_failure{false};
#endif

void InitializeAsyncWorkerKey() {
    const int error = pthread_key_create(&g_async_worker_key, nullptr);
    g_async_worker_key_ready.store(error == 0, std::memory_order_release);
}

bool EnsureAsyncWorkerKey() {
    const int once_error = pthread_once(&g_async_worker_key_once, InitializeAsyncWorkerKey);
    return once_error == 0 && g_async_worker_key_ready.load(std::memory_order_acquire);
}

size_t HashPc(size_t seed, uintptr_t pc) {
    seed ^= static_cast<size_t>(pc) + static_cast<size_t>(0x9e3779b97f4a7c15ULL) +
            (seed << 6) + (seed >> 2);
    return seed;
}

#if defined(__linux__) || defined(__ANDROID__) || defined(__OHOS__)
template <typename Offset>
bool CheckedAddOffset(uintptr_t base, Offset offset, uintptr_t* result) {
    const uintmax_t offset_value = static_cast<uintmax_t>(offset);
    const uintmax_t remaining =
            static_cast<uintmax_t>(std::numeric_limits<uintptr_t>::max() - base);
    if (offset_value > remaining) {
        return false;
    }
    *result = base + static_cast<uintptr_t>(offset_value);
    return true;
}

int CollectLoadedModule(dl_phdr_info* info, size_t, void* opaque) {
    auto* modules = static_cast<std::vector<ModuleInfo>*>(opaque);
    uintptr_t start = std::numeric_limits<uintptr_t>::max();
    uintptr_t end = 0;
    uintptr_t load_bias = static_cast<uintptr_t>(info->dlpi_addr);
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD) {
            continue;
        }
        uintptr_t segment_start = 0;
        uintptr_t segment_end = 0;
        if (!CheckedAddOffset(
                    static_cast<uintptr_t>(info->dlpi_addr), header.p_vaddr,
                    &segment_start) ||
            !CheckedAddOffset(segment_start, header.p_memsz, &segment_end)) {
            continue;
        }
        start = std::min(start, segment_start);
        end = std::max(end, segment_end);
    }
    if (start < end) {
        ModuleInfo module;
        module.start = start;
        module.end = end;
        module.load_bias = load_bias;
        module.name = info->dlpi_name == nullptr ? "" : info->dlpi_name;
        for (size_t i = 0; i < info->dlpi_phnum && module.build_id.empty(); ++i) {
            const ElfW(Phdr)& header = info->dlpi_phdr[i];
            if (header.p_type != PT_NOTE || header.p_memsz < sizeof(ElfW(Nhdr))) {
                continue;
            }
            const auto* note_begin = reinterpret_cast<const unsigned char*>(
                    load_bias + static_cast<uintptr_t>(header.p_vaddr));
            const auto* note_end = note_begin + header.p_memsz;
            const auto* cursor = note_begin;
            while (cursor + sizeof(ElfW(Nhdr)) <= note_end) {
                ElfW(Nhdr) note;
                std::memcpy(&note, cursor, sizeof(note));
                cursor += sizeof(note);
                const size_t name_size =
                        (static_cast<size_t>(note.n_namesz) + 3U) & ~size_t(3U);
                const size_t desc_size =
                        (static_cast<size_t>(note.n_descsz) + 3U) & ~size_t(3U);
                if (name_size > static_cast<size_t>(note_end - cursor) ||
                    desc_size > static_cast<size_t>(note_end - cursor - name_size)) {
                    break;
                }
                const char* name = reinterpret_cast<const char*>(cursor);
                const auto* desc = cursor + name_size;
                if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz >= 3 &&
                    std::memcmp(name, "GNU", 3) == 0) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    module.build_id.reserve(static_cast<size_t>(note.n_descsz) * 2);
                    for (size_t byte = 0; byte < note.n_descsz; ++byte) {
                        module.build_id.push_back(kHex[desc[byte] >> 4]);
                        module.build_id.push_back(kHex[desc[byte] & 0xf]);
                    }
                    break;
                }
                cursor += name_size + desc_size;
            }
        }
        modules->push_back(std::move(module));
    }
    return 0;
}
#endif
}  // namespace

bool AsyncStackWorkerThread() {
    return g_async_worker_key_ready.load(std::memory_order_acquire) &&
           pthread_getspecific(g_async_worker_key) != nullptr;
}

#if defined(MALLOC_HOOK_ASYNC_STACK_TESTING)
void AsyncStackPipelineForceWorkerMarkerFailureForTesting(bool fail) {
    g_force_worker_marker_failure.store(fail, std::memory_order_release);
}
#endif

bool RawStackRecord::operator==(const RawStackRecord& other) const {
    if (frame_count != other.frame_count || module_generation != other.module_generation) {
        return false;
    }
    for (size_t i = 0; i < frame_count; ++i) {
        if (pcs[i] != other.pcs[i]) {
            return false;
        }
    }
    return true;
}

ModuleRegistry::ModuleRegistry(size_t max_retained_generations)
    : max_retained_generations_(std::max<size_t>(max_retained_generations, 1)) {
    snapshots_.emplace(generation_, std::make_shared<const std::vector<ModuleInfo>>());
}

bool ModuleRegistry::Equivalent(
        const std::vector<ModuleInfo>& lhs, const std::vector<ModuleInfo>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        const ModuleInfo& a = lhs[i];
        const ModuleInfo& b = rhs[i];
        if (a.start != b.start || a.end != b.end || a.load_bias != b.load_bias ||
            a.name != b.name || a.build_id != b.build_id) {
            return false;
        }
    }
    return true;
}

uint64_t ModuleRegistry::CurrentGeneration() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return generation_;
}

uint64_t ModuleRegistry::Publish(std::vector<ModuleInfo> modules) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto current = snapshots_.find(generation_);
    if (current != snapshots_.end() && Equivalent(*current->second, modules)) {
        return generation_;
    }
    ++generation_;
    for (auto& module : modules) {
        module.generation = generation_;
    }
    snapshots_[generation_] =
            std::make_shared<const std::vector<ModuleInfo>>(std::move(modules));
    while (snapshots_.size() > max_retained_generations_) {
        auto oldest = snapshots_.begin();
        for (auto entry = snapshots_.begin(); entry != snapshots_.end(); ++entry) {
            if (entry->first < oldest->first) {
                oldest = entry;
            }
        }
        snapshots_.erase(oldest);
    }
    return generation_;
}

size_t ModuleRegistry::SnapshotCount() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return snapshots_.size();
}

bool ModuleRegistry::Resolve(uintptr_t pc, uint64_t generation, ModuleInfo* module) {
    if (module == nullptr) {
        return false;
    }
    std::shared_ptr<const std::vector<ModuleInfo>> snapshot;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto entry = snapshots_.find(generation);
        if (entry == snapshots_.end()) {
            return false;
        }
        snapshot = entry->second;
    }
    for (const auto& candidate : *snapshot) {
        if (candidate.start <= pc && pc < candidate.end) {
            *module = candidate;
            return true;
        }
    }
    return false;
}

NativeModuleResolver::NativeModuleResolver() {
    Refresh();
}

uint64_t NativeModuleResolver::CurrentGeneration() const {
    return registry_.CurrentGeneration();
}

uint64_t NativeModuleResolver::Refresh() {
    std::vector<ModuleInfo> modules;
#if defined(__linux__) || defined(__ANDROID__) || defined(__OHOS__)
    dl_iterate_phdr(CollectLoadedModule, &modules);
#endif
    return registry_.Publish(std::move(modules));
}

bool NativeModuleResolver::Resolve(
        uintptr_t pc, uint64_t generation, ModuleInfo* module) {
    return registry_.Resolve(pc, generation, module);
}

bool NativeSymbolizer::Symbolize(
        const RawStackRecord& raw, const std::vector<ModuleInfo>& modules,
        std::vector<SymbolizedFrame>* frames) {
    if (frames == nullptr) {
        return false;
    }
    frames->clear();
    frames->reserve(raw.frame_count);
    for (size_t i = 0; i < raw.frame_count; ++i) {
        const uintptr_t pc = raw.pcs[i];
        const ModuleInfo* module = i < modules.size() ? &modules[i] : nullptr;
        SymbolizedFrame frame;
        frame.pc = pc;
        if (module != nullptr) {
            frame.module_generation = module->generation;
            frame.module_start = module->start;
            frame.module_name = module->name;
            frame.build_id = module->build_id;
            // ET_EXEC images legitimately have a zero load bias.  Their
            // canonical relative PC is the runtime/ELF virtual address,
            // while PIE/DSO images use the dynamic-loader load bias.
            frame.rel_pc = module->load_bias == 0
                    ? pc
                    : (pc >= module->load_bias ? pc - module->load_bias : pc);
        } else {
            frame.rel_pc = pc;
        }

        Dl_info info = {};
        if (dladdr(reinterpret_cast<void*>(pc), &info) != 0 &&
            info.dli_sname != nullptr && module != nullptr &&
            reinterpret_cast<uintptr_t>(info.dli_fbase) ==
                    module->load_bias &&
            (module->name.empty() || info.dli_fname == nullptr ||
             module->name == info.dli_fname)) {
            frame.function_name = info.dli_sname;
            const uintptr_t symbol = reinterpret_cast<uintptr_t>(info.dli_saddr);
            frame.function_offset = pc >= symbol ? pc - symbol : 0;
        }
        frames->push_back(std::move(frame));
    }
    return !frames->empty();
}

size_t AsyncStackPipeline::RawStackKeyHash::operator()(const RawStackKey& key) const {
    size_t value = HashPc(key.frame_count, key.module_generation);
    const size_t count = std::min<size_t>(key.frame_count, 8);
    for (size_t i = 0; i < count; ++i) {
        value = HashPc(value, key.pcs[i]);
    }
    return value;
}

AsyncStackPipeline::RawStackKey AsyncStackPipeline::MakeKey(const RawStackRecord& raw) {
    RawStackKey key;
    key.frame_count = std::min<uint16_t>(raw.frame_count, kMaxAsyncRawFrames);
    key.module_generation = raw.module_generation;
    std::copy_n(raw.pcs.begin(), key.frame_count, key.pcs.begin());
    return key;
}

AsyncStackPipeline::AsyncStackPipeline(
        std::unique_ptr<ModuleResolver> resolver, std::unique_ptr<Symbolizer> symbolizer,
        size_t capacity, CompletionCallback callback, void* callback_opaque)
    : resolver_(resolver != nullptr ? std::move(resolver)
                                    : std::make_unique<NativeModuleResolver>()),
      symbolizer_(symbolizer != nullptr ? std::move(symbolizer)
                                        : std::make_unique<NativeSymbolizer>()),
      capacity_(std::max<size_t>(capacity, 1)),
      result_capacity_(std::max<size_t>(capacity, 1)),
      callback_(callback),
      callback_opaque_(callback_opaque),
      queue_(capacity_) {
    if (!EnsureAsyncWorkerKey()) {
        stopping_ = true;
        ++stats_.worker_start_failures;
        return;
    }
    worker_ = std::thread(&AsyncStackPipeline::WorkerLoop, this);
}

AsyncStackPipeline::~AsyncStackPipeline() {
    Shutdown();
}

AsyncStackPipeline::SubmitResult AsyncStackPipeline::Submit(const RawStackRecord& input) {
    RawStackRecord raw = input;
    raw.frame_count = std::min<uint16_t>(raw.frame_count, kMaxAsyncRawFrames);
    if (AsyncStackWorkerThread()) {
        std::lock_guard<std::mutex> guard(mutex_);
        ++stats_.recursive_submissions;
        return SubmitResult{.rejected_recursion = true};
    }
    if (raw.module_generation == 0) {
        raw.module_generation = resolver_->CurrentGeneration();
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_) {
        return SubmitResult{};
    }
    const RawStackKey key = MakeKey(raw);
    auto existing = unique_stacks_.find(key);
    if (existing != unique_stacks_.end()) {
        ++stats_.duplicates;
        return SubmitResult{.id = existing->second, .duplicate = true};
    }

    const AsyncStackId id = next_id_++;
    unique_stacks_.emplace(key, id);
    id_to_key_.emplace(id, key);
    StackResult pending;
    pending.id = id;
    pending.state = StackResolutionState::Pending;
    pending.raw = raw;
    results_.emplace(id, std::move(pending));
    if (queue_size_ == capacity_) {
        results_[id].state = StackResolutionState::Dropped;
        unique_stacks_.erase(key);
        ++stats_.dropped;
        return SubmitResult{.id = id, .dropped = true};
    }

    const size_t index = (queue_head_ + queue_size_) % capacity_;
    queue_[index] = QueueItem{.id = id, .raw = raw};
    ++queue_size_;
    ++stats_.accepted;
    stats_.queue_high_water = std::max(stats_.queue_high_water, queue_size_);
    lock.unlock();
    work_cv_.notify_one();
    return SubmitResult{.id = id, .accepted = true};
}

bool AsyncStackPipeline::Release(AsyncStackId id) {
    if (id == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = results_.find(id);
    if (result == results_.end() ||
        result->second.state == StackResolutionState::Pending) {
        return false;
    }
    auto key = id_to_key_.find(id);
    if (key != id_to_key_.end()) {
        auto unique = unique_stacks_.find(key->second);
        if (unique != unique_stacks_.end() && unique->second == id) {
            unique_stacks_.erase(unique);
        }
        id_to_key_.erase(key);
    }
    results_.erase(result);
    result_order_.erase(
            std::remove(result_order_.begin(), result_order_.end(), id),
            result_order_.end());
    return true;
}

void AsyncStackPipeline::EvictResultCacheLocked() {
    while (result_order_.size() > result_capacity_) {
        const AsyncStackId evicted = result_order_.front();
        result_order_.pop_front();
        auto result = results_.find(evicted);
        if (result == results_.end() ||
            result->second.state == StackResolutionState::Pending) {
            continue;
        }
        auto key = id_to_key_.find(evicted);
        if (key != id_to_key_.end()) {
            auto unique = unique_stacks_.find(key->second);
            if (unique != unique_stacks_.end() && unique->second == evicted) {
                unique_stacks_.erase(unique);
            }
            id_to_key_.erase(key);
        }
        results_.erase(result);
    }
}

uint64_t AsyncStackPipeline::CurrentModuleGeneration() const {
    return resolver_->CurrentGeneration();
}

bool AsyncStackPipeline::GetResult(AsyncStackId id, StackResult* result) const {
    if (result == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    auto entry = results_.find(id);
    if (entry == results_.end()) {
        return false;
    }
    *result = entry->second;
    return true;
}

AsyncStackStats AsyncStackPipeline::stats() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return stats_;
}

std::string FormatAsyncStackStats(const AsyncStackStats& stats) {
    std::ostringstream output;
    output << "accepted=" << stats.accepted
           << " duplicates=" << stats.duplicates
           << " dropped=" << stats.dropped
           << " processed=" << stats.processed
           << " failed=" << stats.failed
           << " worker_start_failures=" << stats.worker_start_failures
           << " recursive_submissions=" << stats.recursive_submissions
           << " queue_high_water=" << stats.queue_high_water;
    return output.str();
}

void AsyncStackPipeline::Flush() {
    // A callback executes on the worker.  Waiting for active_ from that same
    // thread would wait for the callback itself to return, so treat this as a
    // re-entrant no-op while preserving the external callback-inclusive
    // barrier for all other callers.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::this_thread::get_id() == worker_thread_id_) {
            return;
        }
    }
    // Refresh only at an explicit normal-runtime checkpoint. Finalization
    // calls BeginFinalization() first, which makes this path wait-only and
    // prevents dynamic-loader/container allocation during teardown.
    {
        std::lock_guard<std::mutex> lifecycle_guard(lifecycle_mutex_);
        if (!shutdown_started_.load(std::memory_order_acquire)) {
            resolver_->RefreshIfSupported();
        }
    }
    std::unique_lock<std::mutex> lock(mutex_);
    idle_cv_.wait(lock, [this]() { return queue_size_ == 0 && active_ == 0; });
}

void AsyncStackPipeline::BeginFinalization() {
    shutdown_started_.store(true, std::memory_order_release);
    // A worker refresh holds this mutex for its entire dynamic-loader and
    // registry-publication sequence. Waiting here makes the transition a
    // proper lifecycle barrier instead of a best-effort flag check.
    std::lock_guard<std::mutex> lifecycle_guard(lifecycle_mutex_);
}

void AsyncStackPipeline::Shutdown() {
    BeginFinalization();
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (joined_) {
            return;
        }
        stopping_ = true;
    }
    work_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> guard(mutex_);
    joined_ = true;
}

void AsyncStackPipeline::WorkerLoop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_thread_id_ = std::this_thread::get_id();
    }
    bool worker_marker_set = false;
    for (;;) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_cv_.wait(lock, [this]() { return stopping_ || queue_size_ != 0; });
            if (queue_size_ == 0 && stopping_) {
                break;
            }
            item = queue_[queue_head_];
            queue_head_ = (queue_head_ + 1) % capacity_;
            --queue_size_;
            ++active_;
        }

        if (!worker_marker_set) {
            int marker_error = 0;
#if defined(MALLOC_HOOK_ASYNC_STACK_TESTING)
            if (g_force_worker_marker_failure.load(std::memory_order_acquire)) {
                marker_error = EAGAIN;
            } else
#endif
            {
                marker_error =
                        pthread_setspecific(g_async_worker_key, reinterpret_cast<void*>(1));
            }
            if (marker_error != 0) {
                std::vector<StackResult> failed_results;
                CompletionCallback callback = nullptr;
                void* callback_opaque = nullptr;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto fail_result = [this, &failed_results](
                                               AsyncStackId id, const RawStackRecord& raw) {
                        StackResult failed;
                        failed.id = id;
                        failed.state = StackResolutionState::Failed;
                        failed.raw = raw;
                        failed_results.push_back(failed);
                        auto result = results_.find(id);
                        if (result != results_.end()) {
                            result->second = failed;
                        }
                        ++stats_.processed;
                        ++stats_.failed;
                    };
                    fail_result(item.id, item.raw);
                    while (queue_size_ != 0) {
                        const QueueItem queued = queue_[queue_head_];
                        fail_result(queued.id, queued.raw);
                    queue_head_ = (queue_head_ + 1) % capacity_;
                    --queue_size_;
                    }
                    --active_;
                    stopping_ = true;
                    ++stats_.worker_start_failures;
                    callback = callback_;
                    callback_opaque = callback_opaque_;
                }
                if (callback != nullptr) {
                    for (const StackResult& failed : failed_results) {
                        callback(callback_opaque, failed);
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (queue_size_ == 0 && active_ == 0) {
                        idle_cv_.notify_all();
                    }
                }
                return;
            }
            worker_marker_set = true;
        }

        StackResult completed;
        completed.id = item.id;
        completed.raw = item.raw;
        std::vector<ModuleInfo> modules;
        bool unresolved_module = false;
        for (size_t i = 0; i < item.raw.frame_count; ++i) {
            ModuleInfo module;
            if (!resolver_->Resolve(item.raw.pcs[i], item.raw.module_generation, &module)) {
                unresolved_module = true;
            }
            modules.push_back(std::move(module));
        }
        // Do not refresh the native module registry from the worker's
        // unresolved-frame path.  Native refresh walks the dynamic loader and
        // publishes allocating containers; doing that opportunistically while
        // an Android process is unwinding can trip allocator/tagging rules and
        // turn an otherwise recoverable unresolved frame into a process abort.
        // Refreshes are therefore restricted to explicit normal-runtime
        // checkpoints (Flush) before finalization.  The raw PC and original
        // generation remain available for a later/offline resolver.
        const bool symbolized =
                symbolizer_->Symbolize(completed.raw, modules, &completed.frames);
        if (symbolized && !unresolved_module) {
            completed.state = StackResolutionState::Resolved;
        } else {
            completed.state = StackResolutionState::Failed;
        }

        CompletionCallback callback = nullptr;
        void* callback_opaque = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto result = results_.find(item.id);
            if (result != results_.end()) {
                result->second = completed;
            }
            result_order_.push_back(item.id);
            EvictResultCacheLocked();
            ++stats_.processed;
            if (completed.state == StackResolutionState::Failed) {
                ++stats_.failed;
            }
            callback = callback_;
            callback_opaque = callback_opaque_;
        }
        if (callback != nullptr) {
            callback(callback_opaque, completed);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            --active_;
            if (queue_size_ == 0 && active_ == 0) {
                idle_cv_.notify_all();
            }
        }
    }
    if (worker_marker_set) {
        pthread_setspecific(g_async_worker_key, nullptr);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_size_ == 0 && active_ == 0) {
        idle_cv_.notify_all();
    }
}
