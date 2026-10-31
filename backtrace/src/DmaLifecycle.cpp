#include "DmaLifecycle.h"

#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dma_lifecycle {
namespace {

#if defined(MREMAP_DONTUNMAP)
constexpr int kMremapDontunmap = MREMAP_DONTUNMAP;
#else
constexpr int kMremapDontunmap = 4;
#endif

uintptr_t RangeEnd(uintptr_t start, size_t size) {
    if (size > std::numeric_limits<uintptr_t>::max() - start) {
        return std::numeric_limits<uintptr_t>::max();
    }
    return start + size;
}

struct IdentityHash {
    size_t operator()(const Identity& identity) const {
        size_t seed = std::hash<uint64_t>{}(identity.device);
        seed ^= std::hash<uint64_t>{}(identity.inode) + 0x9e3779b97f4a7c15ULL +
                (seed << 6) + (seed >> 2);
        return seed;
    }
};

}  // namespace

struct Tracker::Impl {
    struct Object {
        Identity identity;
        size_t size = 0;
        std::unordered_set<int> fds;
        size_t mapping_segments = 0;
        size_t holds = 0;
        bool mapping_only = false;
    };

    struct Mapping {
        uintptr_t end = 0;
        Object* object = nullptr;
    };

    explicit Impl(Sink& sink_in) : sink(sink_in) {}

    Object* CreateObject(size_t size, Identity identity, bool mapping_only = false) {
        auto object = std::make_unique<Object>();
        object->identity = identity;
        object->size = size;
        object->mapping_only = mapping_only;
        Object* token = object.get();
        objects.emplace(token, std::move(object));
        if (identity.valid()) {
            identities[identity] = token;
        }
        sink.Track(token, size);
        return token;
    }

    bool IsLive(Object* object) const {
        return object != nullptr && objects.find(object) != objects.end();
    }

    void UpdateSize(Object* object, size_t size) {
        if (size <= object->size) {
            return;
        }
        object->size = size;
        sink.Resize(object, size);
    }

    Object* FindIdentity(Identity identity) const {
        if (!identity.valid()) {
            return nullptr;
        }
        auto found = identities.find(identity);
        return found == identities.end() ? nullptr : found->second;
    }

    void MaybeRelease(Object* object) {
        if (!IsLive(object) || !object->fds.empty() || object->mapping_segments != 0 ||
            object->holds != 0) {
            return;
        }
        if (object->identity.valid()) {
            auto identity = identities.find(object->identity);
            if (identity != identities.end() && identity->second == object) {
                identities.erase(identity);
            }
        }
        sink.Release(object);
        objects.erase(object);
    }

    void DetachFd(int fd) {
        auto found = fd_objects.find(fd);
        if (found == fd_objects.end()) {
            return;
        }
        Object* object = found->second;
        fd_objects.erase(found);
        object->fds.erase(fd);
        MaybeRelease(object);
    }

    void AttachFd(Object* object, int fd) {
        if (fd < 0) {
            return;
        }
        auto existing = fd_objects.find(fd);
        if (existing != fd_objects.end()) {
            if (existing->second == object) {
                object->fds.insert(fd);
                return;
            }
            Object* displaced = existing->second;
            fd_objects.erase(existing);
            displaced->fds.erase(fd);
            MaybeRelease(displaced);
        }
        fd_objects[fd] = object;
        object->fds.insert(fd);
    }

    Object* MergeObjects(Object* source, Object* target) {
        if (source == target) {
            return target;
        }
        UpdateSize(target, source->size);
        target->mapping_only = target->mapping_only && source->mapping_only;
        for (int fd : source->fds) {
            fd_objects[fd] = target;
            target->fds.insert(fd);
        }
        for (auto& entry : mappings) {
            if (entry.second.object == source) {
                entry.second.object = target;
                ++target->mapping_segments;
            }
        }
        if (source->identity.valid()) {
            auto identity = identities.find(source->identity);
            if (identity != identities.end() && identity->second == source) {
                identities.erase(identity);
            }
        }
        sink.Release(source);
        objects.erase(source);
        return target;
    }

    Object* FindOrCreateFd(int fd, size_t size, Identity identity) {
        auto by_fd = fd_objects.find(fd);
        if (by_fd != fd_objects.end()) {
            Object* object = by_fd->second;
            if (!identity.valid() || !object->identity.valid() ||
                object->identity == identity) {
                if (identity.valid() && !object->identity.valid()) {
                    Object* canonical = FindIdentity(identity);
                    if (canonical != nullptr && canonical != object) {
                        object = MergeObjects(object, canonical);
                    } else {
                        object->identity = identity;
                        identities[identity] = object;
                    }
                }
                UpdateSize(object, size);
                return object;
            }
            DetachFd(fd);
        }

        Object* object = FindIdentity(identity);
        if (object == nullptr) {
            object = CreateObject(size, identity);
        } else {
            UpdateSize(object, size);
        }
        AttachFd(object, fd);
        return object;
    }

    void AddMapping(Object* object, uintptr_t start, uintptr_t end) {
        if (start >= end) {
            return;
        }
        mappings[start] = Mapping{end, object};
        ++object->mapping_segments;
    }

    void RemoveRange(uintptr_t start, uintptr_t end, Object* keep = nullptr) {
        if (start >= end || mappings.empty()) {
            return;
        }

        std::vector<std::pair<uintptr_t, Mapping>> additions;
        std::unordered_set<Object*> touched;
        auto current = mappings.upper_bound(start);
        if (current != mappings.begin()) {
            --current;
        }
        while (current != mappings.end()) {
            const uintptr_t mapping_start = current->first;
            const Mapping mapping = current->second;
            if (mapping.end <= start) {
                ++current;
                continue;
            }
            if (mapping_start >= end) {
                break;
            }

            current = mappings.erase(current);
            --mapping.object->mapping_segments;
            touched.insert(mapping.object);
            if (mapping_start < start) {
                additions.push_back({mapping_start, Mapping{start, mapping.object}});
            }
            if (end < mapping.end) {
                additions.push_back({end, Mapping{mapping.end, mapping.object}});
            }
        }

        for (const auto& addition : additions) {
            mappings.emplace(addition);
            ++addition.second.object->mapping_segments;
        }
        for (Object* object : touched) {
            if (object != keep) {
                MaybeRelease(object);
            }
        }
    }

    Object* ObjectForAddress(uintptr_t address) const {
        auto found = mappings.upper_bound(address);
        if (found == mappings.begin()) {
            return nullptr;
        }
        --found;
        return address < found->second.end ? found->second.object : nullptr;
    }

    void RegisterFd(int fd, size_t size, Identity identity) {
        if (fd < 0 || size == 0) {
            return;
        }
        FindOrCreateFd(fd, size, identity);
    }

    void RegisterMapping(
            int fd, void* address, size_t mapped_size, size_t object_size,
            Identity identity, bool mapping_only) {
        if (address == MAP_FAILED || mapped_size == 0) {
            return;
        }
        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        const uintptr_t end = RangeEnd(start, mapped_size);
        if (start >= end) {
            return;
        }

        RemoveRange(start, end);
        Object* object = nullptr;
        if (!mapping_only && fd >= 0) {
            const size_t physical_size = object_size != 0 ? object_size : mapped_size;
            object = FindOrCreateFd(fd, physical_size, identity);
        }
        if (object == nullptr) {
            const size_t initial_size =
                    mapping_only ? mapped_size
                                 : (object_size != 0 ? object_size : mapped_size);
            object = CreateObject(initial_size, identity, mapping_only);
        }
        AddMapping(object, start, end);
    }

    Sink& sink;
    mutable std::mutex mutex;
    std::unordered_map<Object*, std::unique_ptr<Object>> objects;
    std::unordered_map<Identity, Object*, IdentityHash> identities;
    std::unordered_map<int, Object*> fd_objects;
    std::map<uintptr_t, Mapping> mappings;
};

Tracker::Tracker(Sink& sink) : sink_(&sink), impl_(std::make_unique<Impl>(sink)) {}

Tracker::~Tracker() {
    Reset();
}

void Tracker::RegisterFd(int fd, size_t size, Identity identity) {
    if (impl_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RegisterFd(fd, size, identity);
}

void Tracker::RegisterMapping(
        int fd, void* address, size_t mapped_size, size_t object_size,
        Identity identity, bool mapping_only) {
    if (impl_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->RegisterMapping(
            fd, address, mapped_size, object_size, identity, mapping_only);
}

void* Tracker::Map(
        int fd, size_t mapped_size, bool mapping_only,
        const DescribeOperation& describe, const MapOperation& operation) {
    if (impl_ == nullptr) {
        return operation();
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    size_t object_size = mapped_size;
    Identity identity;
    bool should_track = mapping_only;
    if (!mapping_only && describe) {
        should_track = describe(&object_size, &identity);
        if (!should_track) {
            auto known = impl_->fd_objects.find(fd);
            if (known != impl_->fd_objects.end() &&
                (!identity.valid() || !known->second->identity.valid() ||
                 known->second->identity == identity)) {
                object_size = known->second->size;
                identity = known->second->identity;
                should_track = true;
            } else if (identity.valid()) {
                Impl::Object* identity_object = impl_->FindIdentity(identity);
                if (identity_object != nullptr) {
                    object_size = identity_object->size;
                    should_track = true;
                }
            }
        }
    }
    void* result = operation();
    if (result != MAP_FAILED) {
        if (should_track) {
            impl_->RegisterMapping(
                    fd, result, mapped_size, object_size, identity, mapping_only);
        } else {
            const uintptr_t start = reinterpret_cast<uintptr_t>(result);
            impl_->RemoveRange(start, RangeEnd(start, mapped_size));
        }
    }
    return result;
}

int Tracker::CloseFd(int fd, const IntOperation& operation) {
    if (impl_ == nullptr) {
        return operation();
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const int result = operation();
    const int close_errno = errno;
    if (result == 0 || close_errno != EBADF) {
        impl_->DetachFd(fd);
    }
    errno = close_errno;
    return result;
}

int Tracker::DuplicateFd(
        int old_fd, int replaced_fd, const DescribeOperation& describe,
        const IntOperation& operation) {
    if (impl_ == nullptr) {
        return operation();
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::Object* source = nullptr;
    auto source_entry = impl_->fd_objects.find(old_fd);
    if (source_entry == impl_->fd_objects.end() && describe) {
        size_t size = 0;
        Identity identity;
        const bool described = describe(&size, &identity);
        if (described && size != 0) {
            source = impl_->FindOrCreateFd(old_fd, size, identity);
        } else if (identity.valid()) {
            source = impl_->FindIdentity(identity);
            if (source != nullptr) {
                impl_->AttachFd(source, old_fd);
            }
        }
    } else if (source_entry != impl_->fd_objects.end()) {
        source = source_entry->second;
    }

    const int result = operation();
    if (result < 0) {
        return result;
    }
    const int new_fd = replaced_fd >= 0 ? replaced_fd : result;
    if (new_fd == old_fd) {
        return result;
    }

    impl_->DetachFd(new_fd);
    if (source != nullptr && impl_->IsLive(source)) {
        impl_->AttachFd(source, new_fd);
    }
    return result;
}

int Tracker::Unmap(void* address, size_t size, const IntOperation& operation) {
    if (impl_ == nullptr) {
        return operation();
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const int result = operation();
    if (result == 0 && size != 0) {
        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        impl_->RemoveRange(start, RangeEnd(start, size));
    }
    return result;
}

void* Tracker::Remap(
        void* old_address, size_t old_size, size_t new_size, int flags,
        const MapOperation& operation, bool* handled) {
    if (impl_ == nullptr) {
        *handled = false;
        return operation();
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const uintptr_t old_start = reinterpret_cast<uintptr_t>(old_address);
    Impl::Object* object = impl_->ObjectForAddress(old_start);
    void* result = operation();
    *handled = object != nullptr;
    if (result == MAP_FAILED) {
        return result;
    }
    const uintptr_t new_start = reinterpret_cast<uintptr_t>(result);
    const uintptr_t new_end = RangeEnd(new_start, new_size);
    if (object == nullptr) {
        impl_->RemoveRange(new_start, new_end);
        return result;
    }

    ++object->holds;
    if ((flags & kMremapDontunmap) == 0) {
        impl_->RemoveRange(old_start, RangeEnd(old_start, old_size), object);
    }
    impl_->RemoveRange(new_start, new_end, object);
    impl_->AddMapping(object, new_start, new_end);
    if (object->mapping_only) {
        impl_->UpdateSize(object, new_size);
    }
    --object->holds;
    impl_->MaybeRelease(object);
    return result;
}

bool Tracker::LookupFd(int fd, size_t* size, Identity* identity) const {
    if (impl_ == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto found = impl_->fd_objects.find(fd);
    if (found == impl_->fd_objects.end()) {
        return false;
    }
    *size = found->second->size;
    if (identity != nullptr) {
        *identity = found->second->identity;
    }
    return true;
}

bool Tracker::HasMapping(const void* address) const {
    if (impl_ == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->ObjectForAddress(reinterpret_cast<uintptr_t>(address)) != nullptr;
}

size_t Tracker::LiveObjects() const {
    if (impl_ == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->objects.size();
}

size_t Tracker::LiveBytes() const {
    if (impl_ == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    size_t total = 0;
    for (const auto& entry : impl_->objects) {
        total += entry.second->size;
    }
    return total;
}

void Tracker::Reset() {
    if (impl_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& entry : impl_->objects) {
        impl_->sink.Release(entry.first);
    }
    impl_->mappings.clear();
    impl_->fd_objects.clear();
    impl_->identities.clear();
    impl_->objects.clear();
}

void Tracker::ResetAfterForkInChild() {
    // Do not lock, destroy, or allocate: the inherited mutex/allocator may be
    // owned by a thread that fork() did not clone. The child is permanently
    // suppressed from tracking, so intentionally leak and abandon the snapshot.
    (void)impl_.release();
    sink_ = nullptr;
}

}  // namespace dma_lifecycle
