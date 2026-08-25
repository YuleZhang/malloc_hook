#include <cassert>
#include <cstdint>

#include "HookSourcePolicy.h"

int main() {
    void* success = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000));
    assert(!hook_source::AllocationSucceeded(nullptr));
    assert(hook_source::AllocationSucceeded(success));
    assert(!hook_source::MappingSucceeded(nullptr));
    assert(!hook_source::MappingSucceeded(MAP_FAILED));
    assert(hook_source::MappingSucceeded(success));
    assert(hook_source::SyscallSucceeded(0));
    assert(!hook_source::SyscallSucceeded(-1));

    using Kind = hook_source::MmapCaptureKind;
    assert(hook_source::ClassifyMmap(MAP_FAILED, MAP_ANONYMOUS, -1, true) == Kind::None);
    assert(hook_source::ClassifyMmap(success, MAP_ANONYMOUS, -1, false) == Kind::Anonymous);
    assert(hook_source::ClassifyMmap(success, MAP_ANONYMOUS, -1, true) ==
           Kind::PendingIoctl);
    assert(hook_source::ClassifyMmap(success, MAP_SHARED, 3, true) == Kind::PendingIoctl);
    assert(hook_source::ClassifyMmap(success, MAP_SHARED, 3, false) == Kind::None);
    assert(hook_source::ClassifyMmap(success, MAP_PRIVATE, -1, false) == Kind::None);
    assert(hook_source::ClassifyMmap(success, MAP_ANONYMOUS, 3, false) == Kind::None);

    hook_source::PendingIoctlAllocation pending;
    pending.Mark(7);
    assert(pending.Take() == 7);
    assert(pending.Take() == 0);
    pending.Mark(9);
    pending.Mark(11);
    assert(pending.Take() == 11);
}
