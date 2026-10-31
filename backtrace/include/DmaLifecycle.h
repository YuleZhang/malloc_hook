#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace dma_lifecycle {

struct Identity {
    uint64_t device = 0;
    uint64_t inode = 0;

    bool valid() const { return inode != 0; }
    bool operator==(const Identity& other) const {
        return device == other.device && inode == other.inode;
    }
};

class Sink {
public:
    virtual ~Sink() = default;
    virtual void Track(const void* token, size_t size) = 0;
    virtual void Resize(const void* token, size_t size) = 0;
    virtual void Release(const void* token) = 0;
};

class Tracker {
public:
    using IntOperation = std::function<int()>;
    using MapOperation = std::function<void*()>;
    using DescribeOperation = std::function<bool(size_t*, Identity*)>;

    explicit Tracker(Sink& sink);
    ~Tracker();

    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;

    // Register ownership returned by an allocation/import ioctl. Re-registering
    // the same object through another fd attaches an alias rather than counting
    // the physical buffer twice.
    void RegisterFd(int fd, size_t size, Identity identity);

    // Register one mapped range. `mapping_only` is for driver allocations that
    // have no DMA-BUF fd identity (for example KBASE/KGSL allocation+mmap).
    void RegisterMapping(
            int fd, void* address, size_t mapped_size, size_t object_size,
            Identity identity, bool mapping_only = false);

    // Atomically snapshot fd identity, perform mmap, and register the returned
    // range while close/fd reuse and competing mapping operations are blocked.
    void* Map(
            int fd, size_t mapped_size, bool mapping_only,
            const DescribeOperation& describe, const MapOperation& operation);

    // fd/address cannot be rebound in the tracker between kernel reuse and the
    // corresponding state transition.
    int CloseFd(int fd, const IntOperation& operation);
    int DuplicateFd(
            int old_fd, int replaced_fd, const DescribeOperation& describe,
            const IntOperation& operation);
    int DuplicateFd(int old_fd, int replaced_fd, const IntOperation& operation) {
        return DuplicateFd(old_fd, replaced_fd, {}, operation);
    }
    int Unmap(void* address, size_t size, const IntOperation& operation);
    void* Remap(
            void* old_address, size_t old_size, size_t new_size, int flags,
            const MapOperation& operation, bool* handled);

    bool LookupFd(int fd, size_t* size, Identity* identity = nullptr) const;
    bool HasMapping(const void* address) const;
    size_t LiveObjects() const;
    size_t LiveBytes() const;

    void Reset();
    // fork() does not carry mutex ownership from vanished threads. The child is
    // suppressed from tracking, so abandon the inherited implementation without
    // locking, destroying, or allocating. All later child operations become no-ops.
    void ResetAfterForkInChild();

private:
    struct Impl;
    Sink* sink_ = nullptr;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dma_lifecycle
