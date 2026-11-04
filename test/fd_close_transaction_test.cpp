// A fake Bionic tagged-close callback tests forwarding without Android devices.
// A real tagged FD check still requires Android device validation.
#include "DebugData.h"
#include "PointerData.h"
#include "malloc_debug.h"
#include "memory_hook.h"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
struct Context {
    unsigned calls = 0;
    uint64_t expected_tag = 0;
    uint64_t tag = 0;
    int injected_errno = 0;
    bool close_before_error = false;
};
int TaggedOperation(int fd, void* opaque) {
    auto& c = *static_cast<Context*>(opaque);
    ++c.calls;
    assert(c.tag == c.expected_tag);
    if (c.injected_errno != 0) {
        if (c.close_before_error) {
            assert(syscall(SYS_close, fd) == 0);
        }
        errno = c.injected_errno;
        return -1;
    }
    return static_cast<int>(syscall(SYS_close, fd));
}
double LiveDmaMiB() {
    FILE* report = tmpfile();
    assert(report != nullptr);
    g_debug->pointer->DumpLiveToFile(fileno(report), false);
    rewind(report);
    char first_line[512] = {};
    assert(fgets(first_line, sizeof(first_line), report) != nullptr);
    double host_mb = 0.0, dma_mb = 0.0, total_mb = 0.0;
    assert(std::sscanf(first_line,
            "current host used: %lfMB, current dma used %lfMB, current total peak used: %lfMB",
            &host_mb, &dma_mb, &total_mb) == 3);
    assert(fclose(report) == 0);
    return dma_mb;
}
void ExpectDmaMiB(double expected) {
    const double observed = LiveDmaMiB();
    assert(std::fabs(observed - expected) < 0.01);
}
constexpr size_t kFourMiB = 4u * 1024u * 1024u;
void TrackFd(int fd) {
    debug_seed_dma_fd_for_test(fd, kFourMiB);
    PointerInfoType info{};
    const void* key = reinterpret_cast<void*>(fd);
    // Prove it is a live DMA entry, then put the exact entry back so close can
    // exercise its ordinary bookkeeping removal path.
    assert(g_debug->pointer->TakeEntry(key, &info));
    assert(info.mem_type == DMA);
    assert(info.size == kFourMiB);
    g_debug->pointer->RestoreEntry(key, info);
    ExpectDmaMiB(4.0);
}
int PipeReadFd() {
    int fds[2];
    assert(pipe(fds) == 0);
    assert(syscall(SYS_close, fds[1]) == 0);
    return fds[0];
}
}

int main() {
    m_sys_malloc = std::malloc;
    m_sys_free = std::free;
    m_sys_calloc = std::calloc;
    m_sys_realloc = std::realloc;
    alignas(DebugData) static unsigned char debug_mem[sizeof(DebugData)];
    alignas(PointerData) static unsigned char pointer_mem[sizeof(PointerData)];
    void* spaces[2] = {debug_mem, pointer_mem};
    const bool ready = debug_initialize(spaces);
    assert(ready);

    int fd = PipeReadFd();
    ExpectDmaMiB(0.0);
    TrackFd(fd);
    assert(debug_close(fd) == 0);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    ExpectDmaMiB(0.0);

    fd = PipeReadFd();
    TrackFd(fd);
    Context good;
    good.tag = good.expected_tag = 0xaabbccddeeffULL;
    assert(debug_close_with(fd, TaggedOperation, &good) == 0);
    assert(good.calls == 1);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    ExpectDmaMiB(0.0);

    fd = PipeReadFd();
    TrackFd(fd);
    Context bad;
    bad.tag = bad.expected_tag = 0x9911ULL;
    bad.injected_errno = EBADF;
    errno = 0;
    assert(debug_close_with(fd, TaggedOperation, &bad) == -1);
    assert(errno == EBADF && bad.calls == 1);
    assert(fcntl(fd, F_GETFD) >= 0);
    ExpectDmaMiB(4.0);  // failed tagged-close must preserve the DMA entry
    assert(debug_close(fd) == 0);
    ExpectDmaMiB(0.0);

    fd = PipeReadFd();
    TrackFd(fd);
    Context late;
    late.tag = late.expected_tag = 0x2233ULL;
    late.injected_errno = EINTR;
    late.close_before_error = true;
    errno = 0;
    assert(debug_close_with(fd, TaggedOperation, &late) == -1);
    assert(errno == EINTR && late.calls == 1);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    ExpectDmaMiB(0.0);

    debug_finalize();
    return 0;
}
