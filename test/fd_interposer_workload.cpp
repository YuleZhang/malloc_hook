#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::fprintf(                                                     \
                    stderr, "%s:%d: check failed: %s (errno=%d)\n", __FILE__, \
                    __LINE__, #condition, errno);                             \
            std::exit(1);                                                     \
        }                                                                     \
    } while (false)

using CheckpointFn = void (*)(const char*);

void CheckDmabufNamedRegularFileIsNotTracked() {
    char file_path[] = "/tmp/dmabuf_cache.XXXXXX";
    const int fd = mkstemp(file_path);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, 4096) == 0);
    void* mapping = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(mapping != MAP_FAILED);
    static_cast<char*>(mapping)[0] = 1;

    auto checkpoint = reinterpret_cast<CheckpointFn>(dlsym(RTLD_DEFAULT, "checkpoint"));
    CHECK(checkpoint != nullptr);
    char report_path[128];
    std::snprintf(
            report_path, sizeof(report_path), "/tmp/fd_interposer.%d.txt", getpid());
    checkpoint(report_path);

    FILE* report = std::fopen(report_path, "r");
    CHECK(report != nullptr);
    char line[512] = {};
    CHECK(std::fgets(line, sizeof(line), report) != nullptr);
    CHECK(std::fclose(report) == 0);
    double host_mb = 0.0;
    double dma_mb = 0.0;
    double total_mb = 0.0;
    CHECK(std::sscanf(
                  line,
                  "current host used: %lfMB, current dma used %lfMB, current total "
                  "peak used: %lfMB",
                  &host_mb, &dma_mb, &total_mb) == 3);
    CHECK(dma_mb == 0.0);

    CHECK(munmap(mapping, 4096) == 0);
    CHECK(close(fd) == 0);
    CHECK(unlink(file_path) == 0);
    CHECK(unlink(report_path) == 0);
}

int main() {
    CheckDmabufNamedRegularFileIsNotTracked();
    int pipe_fds[2] = {-1, -1};
    CHECK(pipe(pipe_fds) == 0);

    const int flags = fcntl(pipe_fds[0], F_GETFD);
    CHECK(flags >= 0);
    CHECK(fcntl(pipe_fds[0], F_SETFD, flags | FD_CLOEXEC) == 0);

    const int duplicated = dup(pipe_fds[0]);
    CHECK(duplicated >= 0);
    const int fcntl_duplicate = fcntl(pipe_fds[0], F_DUPFD_CLOEXEC, 100);
    CHECK(fcntl_duplicate >= 100);
#if defined(__linux__)
    const int fcntl64_duplicate = fcntl64(pipe_fds[0], F_DUPFD_CLOEXEC, 200);
    CHECK(fcntl64_duplicate >= 200);
#endif

    const int target = duplicated + 100;
    CHECK(dup2(pipe_fds[1], target) == target);
#if defined(__linux__) && defined(O_CLOEXEC)
    const int target3 = target + 1;
    CHECK(dup3(pipe_fds[1], target3, O_CLOEXEC) == target3);
    CHECK(close(target3) == 0);
#endif

    FILE* file = tmpfile();
    CHECK(file != nullptr);
    struct flock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    CHECK(fcntl(fileno(file), F_GETLK, &lock) == 0);
    CHECK(fclose(file) == 0);

    CHECK(close(target) == 0);
    CHECK(close(fcntl_duplicate) == 0);
#if defined(__linux__)
    CHECK(close(fcntl64_duplicate) == 0);
#endif
    CHECK(close(duplicated) == 0);
    CHECK(close(pipe_fds[0]) == 0);
    CHECK(close(pipe_fds[1]) == 0);
    return 0;
}
