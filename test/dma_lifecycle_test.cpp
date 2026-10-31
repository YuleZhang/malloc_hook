#include "DmaLifecycle.h"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_map>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::fprintf(                                                    \
                    stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                             \
            std::exit(1);                                                    \
        }                                                                    \
    } while (false)

namespace {

constexpr size_t kPage = 4096;

void* Address(uintptr_t value) {
    return reinterpret_cast<void*>(value);
}

class RecordingSink final : public dma_lifecycle::Sink {
public:
    void Track(const void* token, size_t size) override {
        CHECK(live.emplace(token, size).second);
        ++tracks;
    }

    void Resize(const void* token, size_t size) override {
        auto found = live.find(token);
        CHECK(found != live.end());
        found->second = size;
        ++resizes;
    }

    void Release(const void* token) override {
        CHECK(live.erase(token) == 1);
        ++releases;
    }

    size_t LiveBytes() const {
        size_t total = 0;
        for (const auto& entry : live) {
            total += entry.second;
        }
        return total;
    }

    std::unordered_map<const void*, size_t> live;
    size_t tracks = 0;
    size_t resizes = 0;
    size_t releases = 0;
};

void TestFdsanBadCaseClosesLastOwner() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    const dma_lifecycle::Identity identity{1, 100};

    tracker.RegisterFd(10, 16 * kPage, identity);
    tracker.RegisterMapping(10, Address(0x100000), 16 * kPage, 16 * kPage, identity);
    CHECK(tracker.LiveObjects() == 1);
    CHECK(sink.tracks == 1);

    CHECK(tracker.Unmap(Address(0x100000), 16 * kPage, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 1);  // fd still owns the DMA-BUF.

    // Models android_fdsan_close_with_tag after its ownership check. The
    // lifecycle result must be identical to ordinary close.
    CHECK(tracker.CloseFd(10, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
    CHECK(sink.releases == 1);
}

void TestFdAndMappingOwnIndependentReferences() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    const dma_lifecycle::Identity identity{2, 200};

    tracker.RegisterFd(20, 8 * kPage, identity);
    tracker.RegisterMapping(20, Address(0x200000), 8 * kPage, 8 * kPage, identity);
    CHECK(tracker.CloseFd(20, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 1);  // mapping survives close.
    CHECK(tracker.Unmap(Address(0x200000), 8 * kPage, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);

    tracker.RegisterFd(21, 8 * kPage, identity);
    tracker.RegisterMapping(21, Address(0x210000), 8 * kPage, 8 * kPage, identity);
    CHECK(tracker.Unmap(Address(0x210000), 8 * kPage, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 1);  // fd survives unmap.
    CHECK(tracker.CloseFd(21, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestFailedReleaseRollsBackByConstruction() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    const dma_lifecycle::Identity identity{3, 300};

    tracker.RegisterFd(30, kPage, identity);
    tracker.RegisterMapping(30, Address(0x300000), kPage, kPage, identity);
    CHECK(tracker.CloseFd(30, [] {
        errno = EBADF;
        return -1;
    }) == -1);
    CHECK(tracker.Unmap(Address(0x300000), kPage, [] { return -1; }) == -1);
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.HasMapping(Address(0x300000)));
    CHECK(sink.releases == 0);

    CHECK(tracker.Unmap(Address(0x300000), kPage, [] { return 0; }) == 0);
    CHECK(tracker.CloseFd(30, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestLateCloseErrorStillConsumesFd() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    tracker.RegisterFd(31, kPage, {3, 301});
    errno = 0;
    CHECK(tracker.CloseFd(31, [] {
        errno = EINTR;
        return -1;
    }) == -1);
    CHECK(errno == EINTR);
    CHECK(tracker.LiveObjects() == 0);
    CHECK(sink.releases == 1);
}

void TestAliasesReleaseOnlyAtLastOwner() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    const dma_lifecycle::Identity identity{4, 400};

    tracker.RegisterFd(40, 4 * kPage, identity);
    CHECK(tracker.DuplicateFd(40, -1, [] { return 41; }) == 41);
    CHECK(tracker.DuplicateFd(40, -1, [] { return 42; }) == 42);
    CHECK(tracker.LiveObjects() == 1);
    CHECK(sink.tracks == 1);

    CHECK(tracker.CloseFd(40, [] { return 0; }) == 0);
    CHECK(tracker.CloseFd(41, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.CloseFd(42, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
    CHECK(sink.releases == 1);
}

void TestDupDiscoversPreviouslyUnknownSource() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    size_t describe_calls = 0;
    const auto describe = [&](size_t* size, dma_lifecycle::Identity* identity) {
        ++describe_calls;
        *size = 6 * kPage;
        *identity = {13, 1300};
        return true;
    };
    CHECK(tracker.DuplicateFd(130, -1, describe, [] { return 131; }) == 131);
    CHECK(describe_calls == 1);
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.LiveBytes() == 6 * kPage);
    CHECK(tracker.CloseFd(131, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 1);  // original fd was discovered too.
    CHECK(tracker.CloseFd(130, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestDup2ReplacesTargetAtomically() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    const dma_lifecycle::Identity source_identity{5, 500};
    const dma_lifecycle::Identity target_identity{5, 501};

    tracker.RegisterFd(50, 2 * kPage, source_identity);
    tracker.RegisterFd(51, 3 * kPage, target_identity);
    CHECK(tracker.LiveObjects() == 2);
    CHECK(tracker.DuplicateFd(50, 51, [] { return 51; }) == 51);
    CHECK(tracker.LiveObjects() == 1);
    CHECK(sink.releases == 1);  // replaced target closed by dup2.

    CHECK(tracker.CloseFd(50, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.CloseFd(51, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestFailedDupKeepsBothObjects() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    tracker.RegisterFd(52, kPage, {5, 502});
    tracker.RegisterFd(53, 2 * kPage, {5, 503});
    CHECK(tracker.DuplicateFd(52, 53, [] { return -1; }) == -1);
    CHECK(tracker.LiveObjects() == 2);
    CHECK(sink.releases == 0);
    CHECK(tracker.CloseFd(52, [] { return 0; }) == 0);
    CHECK(tracker.CloseFd(53, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestIdentityDeduplicatesIndependentAliases() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    const dma_lifecycle::Identity identity{6, 600};

    tracker.RegisterFd(60, 5 * kPage, identity);
    tracker.RegisterFd(61, 5 * kPage, identity);
    tracker.RegisterMapping(60, Address(0x600000), 2 * kPage, 5 * kPage, identity);
    tracker.RegisterMapping(61, Address(0x610000), 2 * kPage, 5 * kPage, identity);
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.LiveBytes() == 5 * kPage);
    CHECK(sink.tracks == 1);

    CHECK(tracker.CloseFd(60, [] { return 0; }) == 0);
    CHECK(tracker.CloseFd(61, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.Unmap(Address(0x600000), 2 * kPage, [] { return 0; }) == 0);
    CHECK(tracker.Unmap(Address(0x610000), 2 * kPage, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestMissingInodeDoesNotMergeDevicePeers() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    tracker.RegisterFd(62, kPage, {99, 0});
    tracker.RegisterFd(63, 2 * kPage, {99, 0});
    CHECK(tracker.LiveObjects() == 2);
    CHECK(tracker.LiveBytes() == 3 * kPage);
    CHECK(tracker.CloseFd(62, [] { return 0; }) == 0);
    CHECK(tracker.CloseFd(63, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestIdentityPromotionMergesWithCanonicalObject() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    tracker.RegisterFd(64, kPage, {99, 0});
    tracker.RegisterMapping(64, Address(0x640000), kPage, kPage, {99, 0});
    tracker.RegisterFd(65, 2 * kPage, {99, 650});
    CHECK(tracker.LiveObjects() == 2);
    tracker.RegisterFd(64, kPage, {99, 650});
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.LiveBytes() == 2 * kPage);
    CHECK(sink.tracks == 2);
    CHECK(sink.releases == 1);
    CHECK(tracker.CloseFd(64, [] { return 0; }) == 0);
    CHECK(tracker.CloseFd(65, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.HasMapping(Address(0x640000)));
    CHECK(tracker.Unmap(Address(0x640000), kPage, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestFdReuseCannotInheritOldObject() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    const dma_lifecycle::Identity old_identity{7, 700};
    const dma_lifecycle::Identity new_identity{7, 701};

    tracker.RegisterFd(70, 4 * kPage, old_identity);
    tracker.RegisterFd(70, 9 * kPage, new_identity);
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.LiveBytes() == 9 * kPage);
    CHECK(sink.tracks == 2);
    CHECK(sink.releases == 1);
    CHECK(tracker.CloseFd(70, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestPartialUnmapSplitsRange() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);

    tracker.RegisterMapping(-1, Address(0x800000), 3 * kPage, 3 * kPage, {}, true);
    CHECK(tracker.Unmap(Address(0x801000), kPage, [] { return 0; }) == 0);
    CHECK(tracker.HasMapping(Address(0x800000)));
    CHECK(!tracker.HasMapping(Address(0x801000)));
    CHECK(tracker.HasMapping(Address(0x802000)));
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.LiveBytes() == 3 * kPage);  // physical allocation stays live.

    CHECK(tracker.Unmap(Address(0x800000), kPage, [] { return 0; }) == 0);
    CHECK(tracker.Unmap(Address(0x802000), kPage, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestRemapPreservesObjectIdentity() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    tracker.RegisterMapping(-1, Address(0x900000), kPage, kPage, {}, true);

    bool handled = false;
    void* result = tracker.Remap(
            Address(0x900000), kPage, 2 * kPage, MREMAP_MAYMOVE,
            [] { return Address(0x910000); }, &handled);
    CHECK(result == Address(0x910000));
    CHECK(handled);
    CHECK(!tracker.HasMapping(Address(0x900000)));
    CHECK(tracker.HasMapping(Address(0x910000)));
    CHECK(sink.tracks == 1);
    CHECK(sink.releases == 0);

#if defined(MREMAP_DONTUNMAP)
    constexpr int dontunmap = MREMAP_DONTUNMAP;
#else
    constexpr int dontunmap = 4;
#endif
    result = tracker.Remap(
            Address(0x910000), 2 * kPage, 2 * kPage, MREMAP_MAYMOVE | dontunmap,
            [] { return Address(0x930000); }, &handled);
    CHECK(result == Address(0x930000));
    CHECK(handled);
    CHECK(tracker.HasMapping(Address(0x910000)));
    CHECK(tracker.HasMapping(Address(0x930000)));

    CHECK(tracker.Unmap(Address(0x910000), 2 * kPage, [] { return 0; }) == 0);
    CHECK(tracker.Unmap(Address(0x930000), 2 * kPage, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestMappingLengthsDoNotInflatePhysicalObject() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    const dma_lifecycle::Identity identity{14, 1400};
    tracker.RegisterFd(140, 4 * kPage, identity);
    tracker.RegisterMapping(140, Address(0x1400000), 8 * kPage, 4 * kPage, identity);
    CHECK(tracker.LiveBytes() == 4 * kPage);
    bool handled = false;
    CHECK(tracker.Remap(
                  Address(0x1400000), 8 * kPage, 16 * kPage, MREMAP_MAYMOVE,
                  [] { return Address(0x1410000); }, &handled) == Address(0x1410000));
    CHECK(handled);
    CHECK(tracker.LiveBytes() == 4 * kPage);  // VMA growth is not physical growth.
    CHECK(tracker.Unmap(Address(0x1410000), 16 * kPage, [] { return 0; }) == 0);
    CHECK(tracker.CloseFd(140, [] { return 0; }) == 0);

    tracker.RegisterMapping(-1, Address(0x1500000), 2 * kPage, 2 * kPage, {}, true);
#if defined(MREMAP_DONTUNMAP)
    constexpr int dontunmap = MREMAP_DONTUNMAP;
#else
    constexpr int dontunmap = 4;
#endif
    CHECK(tracker.Remap(
                  Address(0x1500000), 2 * kPage, kPage, MREMAP_MAYMOVE | dontunmap,
                  [] { return Address(0x1600000); }, &handled) == Address(0x1600000));
    CHECK(handled);
    CHECK(tracker.LiveBytes() == 2 * kPage);  // physical allocation is counted once.
    CHECK(tracker.Unmap(Address(0x1500000), 2 * kPage, [] { return 0; }) == 0);
    CHECK(tracker.LiveBytes() == 2 * kPage);  // physical object remains allocated.
    CHECK(tracker.Unmap(Address(0x1600000), kPage, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestMapRegistrationIsAtomicWithUnmap() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    std::atomic<bool> kernel_map_entered{false};
    std::atomic<bool> allow_map_to_return{false};
    std::atomic<bool> unmap_finished{false};

    std::thread mapper([&] {
        const auto describe = [](size_t* size, dma_lifecycle::Identity* identity) {
            *size = 4 * kPage;
            *identity = {15, 1500};
            return true;
        };
        CHECK(tracker.Map(150, 4 * kPage, false, describe, [&] {
            kernel_map_entered.store(true, std::memory_order_release);
            while (!allow_map_to_return.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return Address(0x1700000);
        }) == Address(0x1700000));
    });
    while (!kernel_map_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::thread unmapper([&] {
        CHECK(tracker.Unmap(Address(0x1700000), 4 * kPage, [] { return 0; }) == 0);
        unmap_finished.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!unmap_finished.load(std::memory_order_acquire));

    allow_map_to_return.store(true, std::memory_order_release);
    mapper.join();
    unmapper.join();
    CHECK(unmap_finished.load(std::memory_order_acquire));
    CHECK(tracker.LiveObjects() == 1);  // fd still owns the object.
    CHECK(!tracker.HasMapping(Address(0x1700000)));
    CHECK(tracker.CloseFd(150, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestNonDmaSharedMappingIsNotTracked() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    const auto describe = [](size_t*, dma_lifecycle::Identity*) { return false; };
    CHECK(tracker.Map(151, kPage, false, describe, [] { return Address(0x1800000); }) ==
          Address(0x1800000));
    CHECK(tracker.LiveObjects() == 0);
    CHECK(!tracker.HasMapping(Address(0x1800000)));

    tracker.RegisterMapping(-1, Address(0x1800000), kPage, kPage, {}, true);
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.Map(152, kPage, false, describe, [] { return Address(0x1800000); }) ==
          Address(0x1800000));
    CHECK(tracker.LiveObjects() == 0);  // non-DMA MAP_FIXED-style replacement.

    tracker.RegisterMapping(-1, Address(0x1900000), kPage, kPage, {}, true);
    bool handled = true;
    CHECK(tracker.Remap(
                  Address(0x1a00000), kPage, kPage, MREMAP_MAYMOVE | MREMAP_FIXED,
                  [] { return Address(0x1900000); }, &handled) == Address(0x1900000));
    CHECK(!handled);
    CHECK(tracker.LiveObjects() == 0);  // non-DMA mremap replaced destination.

    const dma_lifecycle::Identity known_identity{16, 1600};
    tracker.RegisterFd(153, 3 * kPage, known_identity);
    const auto known_but_unqueryable =
            [known_identity](size_t*, dma_lifecycle::Identity* identity) {
                *identity = known_identity;
                return false;
            };
    CHECK(tracker.Map(153, kPage, false, known_but_unqueryable, [] {
        return Address(0x1b00000);
    }) == Address(0x1b00000));
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.HasMapping(Address(0x1b00000)));
    CHECK(tracker.Unmap(Address(0x1b00000), kPage, [] { return 0; }) == 0);
    CHECK(tracker.CloseFd(153, [] { return 0; }) == 0);
}

void TestCloseSerializesFdReuse() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    const dma_lifecycle::Identity old_identity{10, 1000};
    const dma_lifecycle::Identity new_identity{10, 1001};
    tracker.RegisterFd(100, 4 * kPage, old_identity);

    std::atomic<bool> kernel_close_entered{false};
    std::atomic<bool> allow_close_to_return{false};
    std::atomic<bool> replacement_registered{false};

    std::thread closer([&] {
        CHECK(tracker.CloseFd(100, [&] {
            kernel_close_entered.store(true, std::memory_order_release);
            while (!allow_close_to_return.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return 0;
        }) == 0);
    });
    while (!kernel_close_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::thread reuser([&] {
        tracker.RegisterFd(100, 9 * kPage, new_identity);
        replacement_registered.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!replacement_registered.load(std::memory_order_acquire));

    allow_close_to_return.store(true, std::memory_order_release);
    closer.join();
    reuser.join();
    CHECK(replacement_registered.load(std::memory_order_acquire));
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.LiveBytes() == 9 * kPage);
    CHECK(sink.tracks == 2);
    CHECK(sink.releases == 1);
    CHECK(tracker.CloseFd(100, [] { return 0; }) == 0);
    CHECK(tracker.LiveObjects() == 0);
}

void TestForkChildCanAbandonLockedParentState() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    tracker.RegisterFd(120, kPage, {12, 1200});

    std::atomic<bool> operation_entered{false};
    std::atomic<bool> allow_operation_to_return{false};
    std::thread holder([&] {
        CHECK(tracker.CloseFd(120, [&] {
            operation_entered.store(true, std::memory_order_release);
            while (!allow_operation_to_return.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return 0;
        }) == 0);
    });
    while (!operation_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        tracker.ResetAfterForkInChild();
        if (tracker.LiveObjects() != 0) {
            _exit(2);
        }
        _exit(0);
    }

    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    allow_operation_to_return.store(true, std::memory_order_release);
    holder.join();
}

void TestSizeGrowthPreservesSingleTrack() {
    RecordingSink sink;
    dma_lifecycle::Tracker tracker(sink);
    const dma_lifecycle::Identity identity{11, 1100};
    tracker.RegisterFd(110, kPage, identity);
    tracker.RegisterFd(111, 4 * kPage, identity);
    CHECK(tracker.LiveObjects() == 1);
    CHECK(tracker.LiveBytes() == 4 * kPage);
    CHECK(sink.tracks == 1);
    CHECK(sink.resizes == 1);
    CHECK(tracker.CloseFd(110, [] { return 0; }) == 0);
    CHECK(tracker.CloseFd(111, [] { return 0; }) == 0);
    CHECK(sink.releases == 1);
}

}  // namespace

int main() {
    TestFdsanBadCaseClosesLastOwner();
    TestFdAndMappingOwnIndependentReferences();
    TestFailedReleaseRollsBackByConstruction();
    TestLateCloseErrorStillConsumesFd();
    TestAliasesReleaseOnlyAtLastOwner();
    TestDupDiscoversPreviouslyUnknownSource();
    TestDup2ReplacesTargetAtomically();
    TestFailedDupKeepsBothObjects();
    TestIdentityDeduplicatesIndependentAliases();
    TestMissingInodeDoesNotMergeDevicePeers();
    TestIdentityPromotionMergesWithCanonicalObject();
    TestFdReuseCannotInheritOldObject();
    TestPartialUnmapSplitsRange();
    TestRemapPreservesObjectIdentity();
    TestMappingLengthsDoNotInflatePhysicalObject();
    TestMapRegistrationIsAtomicWithUnmap();
    TestNonDmaSharedMappingIsNotTracked();
    TestCloseSerializesFdReuse();
    TestForkChildCanAbandonLockedParentState();
    TestSizeGrowthPreservesSingleTrack();
    return 0;
}
