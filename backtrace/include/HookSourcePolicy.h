#pragma once

#include <sys/mman.h>

#include <cstdint>

namespace hook_source {

inline bool AllocationSucceeded(const void* result) noexcept {
    return result != nullptr;
}

inline bool MappingSucceeded(const void* result) noexcept {
    return result != nullptr && result != MAP_FAILED;
}

inline bool SyscallSucceeded(int result) noexcept {
    return result == 0;
}

enum class MmapCaptureKind : uint8_t { None, Anonymous, PendingIoctl };

inline MmapCaptureKind ClassifyMmap(
        const void* result, int flags, int fd, bool pending_ioctl) noexcept {
    if (result == MAP_FAILED) {
        return MmapCaptureKind::None;
    }
    if (pending_ioctl) {
        return MmapCaptureKind::PendingIoctl;
    }
    if (fd < 0 && (flags & MAP_ANONYMOUS)) {
        return MmapCaptureKind::Anonymous;
    }
    return MmapCaptureKind::None;
}

class PendingIoctlAllocation {
public:
    void Mark(unsigned int request) noexcept { request_ = request; }

    unsigned int Take() noexcept {
        const unsigned int request = request_;
        request_ = 0;
        return request;
    }

private:
    unsigned int request_ = 0;
};

}  // namespace hook_source
