// Android API >=29 on-device fdsan/DMABUF regression gate.
// Build standalone, run with LD_PRELOAD=./liballoc_hook.so.
// Does not require privileged access for fdsan valid/invalid modes.
// A real /dev/dma_heap allocation requires an accessible kernel heap.
// This binary deliberately does not simulate MDW/APU driver or kernel lifetime.
#include <android/fdsan.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dma_heap_uapi.h"

namespace {
constexpr uint64_t kOwnedTag = 0x112233445566ULL;
constexpr uint64_t kWrongTag = 0x665544332211ULL;
constexpr uint64_t kAllocSize = 4ULL * 1024 * 1024;
constexpr int kSkip = 77;
using CheckpointFn = void (*)(const char*);

int Fail(const char* step) {
    std::fprintf(stderr, "FAIL %s: errno=%d (%s)\n", step, errno, std::strerror(errno));
    return 1;
}

bool CheckInterposedFdsan() {
    auto* symbol = dlsym(RTLD_DEFAULT, "android_fdsan_close_with_tag");
    Dl_info owner = {};
    if (!symbol || !dladdr(symbol, &owner) || !owner.dli_fname ||
        std::strstr(owner.dli_fname, "liballoc_hook.so") == nullptr) {
        std::fprintf(stderr,
                "FAIL fdsan symbol is not resolved to preloaded liballoc_hook.so"
                " (owner=%s)\n", owner.dli_fname ? owner.dli_fname : "(null)");
        return false;
    }
    return true;
}

bool CheckBionicApi() {
    if (!android_fdsan_exchange_owner_tag || !android_fdsan_close_with_tag ||
        !android_fdsan_set_error_level) {
        std::fprintf(stderr, "SKIP: Bionic fdsan API 29 functions unavailable\n");
        return false;
    }
    return true;
}

int Valid() {
    if (!CheckBionicApi()) return kSkip;
    if (!CheckInterposedFdsan()) return 1;

    const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Fail("open /dev/null");
    android_fdsan_exchange_owner_tag(fd, 0, kOwnedTag);
    if (android_fdsan_close_with_tag(fd, kOwnedTag) != 0) {
        return Fail("valid android_fdsan_close_with_tag");
    }
    errno = 0;
    if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
        return Fail("valid tagged close must release fd");
    }
    std::puts("PASS: valid tagged close preserved Bionic ownership enforcement");
    return 0;
}

int InvalidChild() {
    if (!CheckBionicApi()) return kSkip;
    if (!CheckInterposedFdsan()) return 1;
    // Set fatal inside the exec'd test process; never disable checks in the
    // parent. Bionic disables fd checks after fork until exec, hence exec is
    // necessary to genuinely verify ownership mismatch behavior.
    android_fdsan_set_error_level(ANDROID_FDSAN_ERROR_LEVEL_FATAL);
    const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Fail("open /dev/null");
    android_fdsan_exchange_owner_tag(fd, 0, kOwnedTag);
    (void)android_fdsan_close_with_tag(fd, kWrongTag);
    std::fprintf(stderr, "FAIL mismatched tag returned without SIGABRT\n");
    return 1;
}

int Invalid(const char* executable) {
    if (!CheckBionicApi()) return kSkip;
    if (!CheckInterposedFdsan()) return 1;
    const pid_t pid = fork();
    if (pid < 0) return Fail("fork");
    if (pid == 0) {
        char* const args[] = {const_cast<char*>(executable),
                             const_cast<char*>("invalid-child"), nullptr};
        execv(executable, args);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return Fail("waitpid");
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        std::fprintf(stderr,
                "FAIL mismatched owner tag did not cause SIGABRT (status=0x%x)\n",
                status);
        return 1;
    }
    std::puts("PASS: invalid owner tag caused Bionic SIGABRT");
    return 0;
}

bool DumpDmaMiB(CheckpointFn checkpoint, const char* label, double* dma) {
    char report[128];
    std::snprintf(report, sizeof(report),
                  "/data/local/tmp/fdsan_gate_%d_%s.txt", getpid(), label);
    checkpoint(report);
    FILE* stream = std::fopen(report, "r");
    if (!stream) return false;
    char line[512] = {};
    const bool read_ok = std::fgets(line, sizeof(line), stream) != nullptr;
    std::fclose(stream);
    unlink(report);
    if (!read_ok) return false;
    double host = 0.0, total_peak = 0.0;
    return std::sscanf(line,
            "current host used: %lfMB, current dma used %lfMB, current total peak used: %lfMB",
            &host, dma, &total_peak) == 3;
}

int DmaLedger(const char* heap_path) {
    if (!CheckBionicApi()) return kSkip;
    if (!CheckInterposedFdsan()) return 1;
    auto checkpoint = reinterpret_cast<CheckpointFn>(dlsym(RTLD_DEFAULT, "checkpoint"));
    if (!checkpoint) return Fail("checkpoint symbol unavailable");

    const int heap_fd = open(heap_path, O_RDONLY | O_CLOEXEC);
    if (heap_fd < 0) {
        std::fprintf(stderr, "SKIP: cannot open %s: %s\n", heap_path, std::strerror(errno));
        return kSkip;
    }
    double baseline = 0.0, allocated = 0.0, released = 0.0;
    if (!DumpDmaMiB(checkpoint, "baseline", &baseline)) return Fail("baseline dump");

    struct dma_heap_allocation_data allocation = {};
    allocation.len = kAllocSize;
    allocation.fd_flags = O_RDWR | O_CLOEXEC;
    const int io = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation);
    const int saved_errno = errno;
    close(heap_fd);
    if (io != 0 || static_cast<int>(allocation.fd) < 0) {
        std::fprintf(stderr, "SKIP: %s did not allocate: %s\n",
                     heap_path, std::strerror(saved_errno));
        return kSkip;
    }

    const int fd = static_cast<int>(allocation.fd);
    android_fdsan_exchange_owner_tag(fd, 0, kOwnedTag);
    if (!DumpDmaMiB(checkpoint, "allocated", &allocated)) return Fail("allocation dump");
    if (android_fdsan_close_with_tag(fd, kOwnedTag) != 0) return Fail("DMA tagged close");
    if (!DumpDmaMiB(checkpoint, "released", &released)) return Fail("release dump");

    std::printf("DMA ledger MiB: baseline=%.3f allocated=%.3f released=%.3f\n",
                baseline, allocated, released);
    // Compare the logical bookkeeping delta, not kernel residency. Kernel may
    // retain DMA backing asynchronously after the FD has been closed.
    if (std::fabs((allocated - baseline) - 4.0) > 0.01 ||
        std::fabs(released - baseline) > 0.01) {
        std::fprintf(stderr,
                "FAIL: expected +4MiB on ALLOC and return to baseline after tagged close\n");
        return 1;
    }
    std::puts("PASS: real DMA-HEAP 4MiB allocation accounted and released by tagged close");
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                "Usage: %s valid | invalid | dma [heap-device-path]\n", argv[0]);
        return 2;
    }
    if (std::strcmp(argv[1], "valid") == 0) return Valid();
    if (std::strcmp(argv[1], "invalid") == 0) return Invalid(argv[0]);
    if (std::strcmp(argv[1], "invalid-child") == 0) return InvalidChild();
    if (std::strcmp(argv[1], "dma") == 0) {
        return DmaLedger(argc >= 3 ? argv[2] : "/dev/dma_heap/system");
    }
    std::fprintf(stderr, "Unknown mode: %s\n", argv[1]);
    return 2;
}
