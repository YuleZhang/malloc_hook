#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cstdlib>

#include "ObservedMemory.h"
#include "debug_disable.h"

namespace {

constexpr size_t kDefaultPeakStepBytes = 12 * 1024 * 1024;
// Floor for the scaled snapshot interval, so a very small peak cannot make the
// snapshot refresh on essentially every sample.
constexpr size_t kMinPeakStepBytes = 64 * 1024;
// How many samples the carried mapping-pass figure may be reused for before it
// is refreshed regardless of the peak gate.
constexpr unsigned kMapRefreshSamples = 8;
// The same bound for the GPU pass. Larger because that pass costs an order of
// magnitude more than the mapping pass and the quantity it reads moves in a few
// large steps rather than continuously: a driver maps its working set once and
// holds it. The bound is what makes the staleness visible instead of unbounded --
// GpuMmapCache::max_bytes reports how much was ever at stake.
constexpr unsigned kGpuRefreshSamples = 32;

// Device nodes whose mappings escape both other signals. Only kgsl qualifies;
// see GpuMmapBytesFromSmapsText for why adding Mali here would inflate rather
// than complete the total.
//
// Matched with the "/dev/" prefix, not on "/kgsl" alone. A bare "/kgsl" also
// matches any *directory* of that name -- "/vendor/lib64/kgsl/libfoo.so" -- and
// such a mapping is file-backed, so the part of it that is not resident would be
// added here as if it were unaccounted device memory. The device node itself is
// always under /dev, including in the "(deleted)" spelling the kernel appends
// when the node has been unlinked, so the longer needle loses nothing.
constexpr char kGpuRegionNeedle[] = "/dev/kgsl";

// Whether a device node this scan counts exists at all.
//
// Probed once and remembered. This check has to happen before /proc/self/smaps
// is opened, not while parsing it: the per-region name filter can only reject
// regions the kernel has already walked page tables to produce, so on a platform
// with no such node the entire cost is paid to return zero. Measured on an arm64
// target, a ~460 MB process pays ~25 ms per read; at a 5 ms sampling interval
// that is a sampler which no longer samples and which serialises every mmap in
// the process behind mmap_lock.
bool GpuDeviceNodePresent() {
    static std::atomic<int> state{0};  // 0 unknown, 1 present, 2 absent
    int known = state.load(std::memory_order_relaxed);
    if (known == 0) {
        // Both spellings seen in the wild: the renderer node the driver mmaps,
        // and a parent node on some trees.
        known = (access("/dev/kgsl-3d0", F_OK) == 0 || access("/dev/kgsl", F_OK) == 0)
                ? 1
                : 2;
        state.store(known, std::memory_order_relaxed);
    }
    return known == 1;
}

// Reads /proc/self/smaps and sums the GPU regions in it. Returns false only when
// the file could not be opened, so "opened and found nothing" stays
// distinguishable from "no accounting available".
//
// Streamed through a stack buffer rather than slurped whole, matching
// ReadDmaBytesFromMaps: smaps for a process with thousands of mappings runs to
// megabytes, and a shared static buffer large enough for it would both sit in
// .bss for the life of every process and be corrupted by any second caller.
// GpuRegionScan carries the only state a refill can split.
bool ReadGpuMmapBytesFromSmaps(size_t* bytes) {
    const int fd = open("/proc/self/smaps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    size_t total = 0;
    GpuRegionScan scan;
    char buffer[16384];
    size_t held = 0;
    for (;;) {
        const ssize_t bytes_read = read(fd, buffer + held, sizeof(buffer) - held - 1);
        if (bytes_read <= 0) {
            break;
        }
        const size_t available = held + static_cast<size_t>(bytes_read);
        buffer[available] = '\0';
        size_t start = 0;
        for (;;) {
            char* newline = static_cast<char*>(
                    memchr(buffer + start, '\n', available - start));
            if (newline == nullptr) {
                break;
            }
            *newline = '\0';
            total += GpuMmapBytesFromSmapsLine(buffer + start, &scan);
            start = static_cast<size_t>(newline - buffer) + 1;
        }
        held = available - start;
        if (held >= sizeof(buffer) - 1) {
            // A single line longer than the buffer. Dropping it is preferable to
            // parsing a fragment as if it were a whole region header.
            held = 0;
            scan = GpuRegionScan{};
        } else if (held > 0) {
            memmove(buffer, buffer + start, held);
        }
    }
    close(fd);
    *bytes = total;
    return true;
}

// The kernel's getdents64 record layout. Declared here rather than taken from
// libc so the directory walk needs no opendir()/readdir(), which would allocate
// once per sample on a path that runs every millisecond.
struct LinuxDirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

// A descriptor referring to a dmabuf resolves to one of these. Both spellings
// are accepted because the same buffer is named differently depending on
// whether the kernel gives the dmabuf filesystem its own mount.
bool LinkNamesDmaBuf(const char* target) {
    return strncmp(target, "/dmabuf", 7) == 0 ||
           strncmp(target, "anon_inode:dmabuf", 17) == 0;
}

// Inserts `inode`; returns true if it was not already present. A full table
// reports every inode as new, which over-counts rather than under-counts, and
// sets the overflow flag so the report can say so.
bool InsertInode(DmaInodeSet* set, uint64_t inode) {
    if (set->count * 2 >= DmaInodeSet::kSlots) {
        set->overflowed = true;
        return true;
    }
    size_t slot = static_cast<size_t>(inode * 0x9e3779b97f4a7c15ULL >> 40) &
                  (DmaInodeSet::kSlots - 1);
    for (size_t probe = 0; probe < DmaInodeSet::kSlots; ++probe) {
        const size_t index = (slot + probe) & (DmaInodeSet::kSlots - 1);
        if (set->stamp[index] != set->generation) {
            set->stamp[index] = set->generation;
            set->inode[index] = inode;
            ++set->count;
            return true;
        }
        if (set->inode[index] == inode) {
            return false;
        }
    }
    set->overflowed = true;
    return true;
}

// Buffers this process holds a descriptor for. Size and inode come from
// fstat() on the descriptor itself rather than from parsing
// /proc/self/fdinfo/<fd>: both give the same two numbers, and one stat costs a
// syscall where the fdinfo route costs an open, one or more reads, a close and
// a text parse -- per descriptor, per sample.
size_t ReadDmaBytesFromFds(DmaInodeSet* set, bool* saw_any) {
    const int dir_fd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        return 0;
    }
    size_t total = 0;
    char records[8192];
    for (;;) {
        const long bytes =
                syscall(SYS_getdents64, dir_fd, records, sizeof(records));
        if (bytes <= 0) {
            break;
        }
        for (long offset = 0; offset < bytes;) {
            const auto* entry =
                    reinterpret_cast<const LinuxDirent64*>(records + offset);
            offset += entry->d_reclen;
            char* end = nullptr;
            const long fd = strtol(entry->d_name, &end, 10);
            if (end == entry->d_name || *end != '\0' || fd < 0 || fd == dir_fd) {
                continue;
            }
            char target[64];
            const ssize_t length =
                    readlinkat(dir_fd, entry->d_name, target, sizeof(target) - 1);
            if (length <= 0) {
                continue;
            }
            target[length] = '\0';
            if (!LinkNamesDmaBuf(target)) {
                continue;
            }
            struct stat info;
            if (fstat(static_cast<int>(fd), &info) != 0) {
                // The descriptor was closed between the directory read and
                // here. Skipping is correct: it is no longer held.
                continue;
            }
            *saw_any = true;
            if (!InsertInode(set, static_cast<uint64_t>(info.st_ino))) {
                continue;
            }
            // st_size is the buffer size on every kernel checked; st_blocks is
            // the documented fallback for those that leave it zero.
            total += info.st_size > 0
                    ? static_cast<size_t>(info.st_size)
                    : static_cast<size_t>(info.st_blocks) * 512;
        }
    }
    close(dir_fd);
    return total;
}

// One /proc/self/maps line, counted only if it maps a dmabuf that the
// descriptor pass did not already account for. A buffer whose descriptor was
// closed while a mapping survives is still resident and still counted by an
// external evaluator, so it cannot be dropped.
size_t ReadDmaBytesFromMaps(DmaInodeSet* set, bool* saw_any) {
    const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    size_t total = 0;
    char buffer[16384];
    size_t held = 0;
    for (;;) {
        const ssize_t bytes = read(fd, buffer + held, sizeof(buffer) - held - 1);
        if (bytes <= 0) {
            break;
        }
        const size_t available = held + static_cast<size_t>(bytes);
        buffer[available] = '\0';
        size_t start = 0;
        for (;;) {
            char* newline = static_cast<char*>(
                    memchr(buffer + start, '\n', available - start));
            if (newline == nullptr) {
                break;
            }
            *newline = '\0';
            total += DmaBytesFromMapsLine(buffer + start, set, saw_any);
            start = static_cast<size_t>(newline - buffer) + 1;
        }
        held = available - start;
        if (held >= sizeof(buffer) - 1) {
            // A single line longer than the buffer. Dropping it is preferable
            // to parsing a fragment as if it were a whole mapping.
            held = 0;
        } else if (held > 0) {
            memmove(buffer, buffer + start, held);
        }
    }
    close(fd);
    return total;
}

constexpr char kIonProcInfoPath[] = "/proc/ion_process_info";

// Probed once and remembered. On the kernels that do have this file, reading it
// walks every process on the device and costs milliseconds, so it must not be
// attempted speculatively on the kernels that do not.
bool IonProcInfoAvailable() {
    static std::atomic<int> state{0};  // 0 unknown, 1 present, 2 absent
    int known = state.load(std::memory_order_relaxed);
    if (known == 0) {
        known = access(kIonProcInfoPath, R_OK) == 0 ? 1 : 2;
        state.store(known, std::memory_order_relaxed);
    }
    return known == 1;
}

// Kernels that publish per-process ION accounting in one process-wide file
// rather than under /proc/<pid>. Lines are "<name> <pid> <fd> <size> ...".
bool ReadIonProcInfoBytes(size_t* bytes) {
    if (!IonProcInfoAvailable()) {
        return false;
    }
    return ReadIonProcInfoBytesFrom(kIonProcInfoPath, getpid(), bytes);
}

uint64_t MonotonicMicros() {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(now.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(now.tv_nsec) / 1000ULL;
}

void SleepMillis(unsigned millis) {
    struct timespec request;
    request.tv_sec = static_cast<time_t>(millis / 1000);
    request.tv_nsec = static_cast<long>(millis % 1000) * 1000000L;
    while (nanosleep(&request, &request) != 0 && errno == EINTR) {
    }
}

ObservedPeakSampler g_observed_peak_sampler;

}  // namespace

size_t ReadStatusField(const char* text, const char* key) {
    const char* found = strstr(text, key);
    if (found == nullptr) {
        return 0;
    }
    found += strlen(key);
    while (*found == ' ' || *found == '\t') {
        ++found;
    }
    size_t value = 0;
    while (*found >= '0' && *found <= '9') {
        value = value * 10 + static_cast<size_t>(*found - '0');
        ++found;
    }
    return value;
}

RssBreakdown ReadSelfRss() {
    RssBreakdown rss;
    const int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return rss;
    }
    char buffer[8192];
    size_t used = 0;
    ssize_t bytes;
    while (used + 1 < sizeof(buffer) &&
           (bytes = read(fd, buffer + used, sizeof(buffer) - used - 1)) > 0) {
        used += static_cast<size_t>(bytes);
    }
    close(fd);
    buffer[used] = '\0';
    rss.vm_rss_kb = ReadStatusField(buffer, "VmRSS:");
    rss.anon_kb = ReadStatusField(buffer, "RssAnon:");
    rss.file_kb = ReadStatusField(buffer, "RssFile:");
    rss.shmem_kb = ReadStatusField(buffer, "RssShmem:");
    rss.valid = rss.vm_rss_kb != 0;
    return rss;
}

const char* DmaSourceName(DmaSource source) {
    switch (source) {
        case DmaSource::None:
            return "none";
        case DmaSource::FdAndMaps:
            return "fd+maps";
        case DmaSource::IonProcInfo:
            return "ion_process_info";
        case DmaSource::Unprobed:
            break;
    }
    return "unprobed";
}

const char* GpuMmapSourceName(GpuMmapSource source) {
    switch (source) {
        case GpuMmapSource::NotApplicable:
            return "not_applicable";
        case GpuMmapSource::Smaps:
            return "smaps";
        case GpuMmapSource::Unprobed:
            break;
    }
    return "unprobed";
}

size_t GpuMmapBytesFromSmapsLine(char* line, GpuRegionScan* scan) {
    if (line == nullptr || scan == nullptr) {
        return 0;
    }
    uintptr_t start = 0;
    uintptr_t end = 0;
    // A region header is the only line that parses as two hex addresses joined by
    // '-'. Field lines ("Size:", "Rss:", "VmFlags:") begin with a letter and
    // cannot.
    if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &start, &end) == 2) {
        scan->in_gpu_region = strstr(line, kGpuRegionNeedle) != nullptr;
        scan->region_size = end > start ? static_cast<size_t>(end - start) : 0;
        return 0;
    }
    if (!scan->in_gpu_region || strncmp(line, "Rss:", 4) != 0) {
        return 0;
    }
    long rss_kb = 0;
    sscanf(line + 4, "%ld", &rss_kb);
    const size_t resident = static_cast<size_t>(rss_kb < 0 ? 0 : rss_kb) * 1024;
    // One Rss line per region; clearing here keeps a later field line of the same
    // region from being read as a second one.
    scan->in_gpu_region = false;
    // Only the part VmRSS does not already hold, so rss_bytes + gpu_bytes never
    // counts the same page twice, and a fully resident region cannot underflow.
    return scan->region_size > resident ? scan->region_size - resident : 0;
}

size_t GpuMmapBytesFromSmapsText(char* text) {
    if (text == nullptr) {
        return 0;
    }
    size_t total = 0;
    GpuRegionScan scan;
    char* cursor = text;
    while (*cursor != '\0') {
        char* newline = strchr(cursor, '\n');
        if (newline != nullptr) {
            *newline = '\0';
        }
        total += GpuMmapBytesFromSmapsLine(cursor, &scan);
        if (newline == nullptr) {
            break;
        }
        cursor = newline + 1;
    }
    return total;
}

size_t ReadSelfGpuMmapBytes(ObservedMemSample* into) {
    return ReadSelfGpuMmapBytesGated(into, nullptr, 0);
}

size_t ReadSelfGpuMmapBytesGated(
        ObservedMemSample* into, GpuMmapCache* cache, size_t peak_total_bytes) {
    // Every gate below is checked before the clock is read: on a platform without
    // the device node this pass must cost nothing at all, and a clock_gettime per
    // sample to time a no-op is exactly the kind of cost that accumulates unseen.
    if (cache != nullptr && cache->probed_absent) {
        into->gpu_source = GpuMmapSource::NotApplicable;
        into->gpu_bytes = 0;
        return 0;
    }
    if (!GpuDeviceNodePresent()) {
        if (cache != nullptr) {
            cache->probed_absent = true;
        }
        into->gpu_source = GpuMmapSource::NotApplicable;
        into->gpu_bytes = 0;
        return 0;
    }
    if (cache != nullptr) {
        // Compared against the peak *including* the carried figure, so the gate
        // can only skip samples that stay below the peak even when credited with
        // the last known GPU contribution.
        const size_t provisional =
                into->rss_bytes + into->dma_bytes + cache->bytes;
        const bool run = !cache->ever_ran || provisional >= peak_total_bytes ||
                         cache->samples_since_refresh >= kGpuRefreshSamples;
        if (!run) {
            into->gpu_bytes = cache->bytes;
            into->gpu_source = GpuMmapSource::Smaps;
            ++cache->samples_since_refresh;
            return into->gpu_bytes;
        }
    }
    const uint64_t begin_us = MonotonicMicros();
    size_t bytes = 0;
    if (!ReadGpuMmapBytesFromSmaps(&bytes)) {
        // The node exists but the file does not, which is not a real zero.
        into->gpu_source = GpuMmapSource::Unprobed;
        into->gpu_bytes = cache != nullptr ? cache->bytes : 0;
        into->gpu_us = static_cast<uint32_t>(MonotonicMicros() - begin_us);
        return into->gpu_bytes;
    }
    into->gpu_bytes = bytes;
    into->gpu_source = GpuMmapSource::Smaps;
    into->gpu_us = static_cast<uint32_t>(MonotonicMicros() - begin_us);
    if (cache != nullptr) {
        cache->bytes = bytes;
        cache->ever_ran = true;
        cache->samples_since_refresh = 0;
        ++cache->refreshes;
        if (bytes > cache->max_bytes) {
            cache->max_bytes = bytes;
        }
    }
    return bytes;
}

void ResetDmaInodeSet(DmaInodeSet* set) {
    set->count = 0;
    set->overflowed = false;
    if (++set->generation == 0) {
        // Wrapped after 2^32 passes; stamps must be cleared or stale slots
        // would read as occupied.
        memset(set->stamp, 0, sizeof(set->stamp));
        set->generation = 1;
    }
}

size_t DmaBytesFromMapsLine(char* line, DmaInodeSet* set, bool* saw_any) {
    // Cheap reject first: almost every line of a real process's maps is not a
    // dmabuf, and this avoids running sscanf over all of them.
    if (strstr(line, "/dmabuf") == nullptr) {
        return 0;
    }
    uintptr_t start = 0;
    uintptr_t end = 0;
    unsigned long long inode = 0;
    if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %*s %*s %*s %llu", &start, &end,
               &inode) < 3 ||
        end <= start) {
        return 0;
    }
    *saw_any = true;
    if (!InsertInode(set, static_cast<uint64_t>(inode))) {
        return 0;
    }
    return static_cast<size_t>(end - start);
}

bool ReadIonProcInfoBytesFrom(const char* path, int pid, size_t* bytes) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    size_t total = 0;
    char buffer[8192];
    size_t held = 0;
    for (;;) {
        const ssize_t bytes_read = read(fd, buffer + held, sizeof(buffer) - held - 1);
        if (bytes_read <= 0) {
            break;
        }
        const size_t available = held + static_cast<size_t>(bytes_read);
        buffer[available] = '\0';
        size_t start = 0;
        for (;;) {
            char* newline = static_cast<char*>(
                    memchr(buffer + start, '\n', available - start));
            if (newline == nullptr) {
                break;
            }
            *newline = '\0';
            char name[256];
            int entry_pid = -1;
            int entry_fd = -1;
            unsigned long long size = 0;
            if (sscanf(buffer + start, "%255s %d %d %llu", name, &entry_pid,
                       &entry_fd, &size) >= 4 &&
                entry_pid == pid) {
                total += static_cast<size_t>(size);
            }
            start = static_cast<size_t>(newline - buffer) + 1;
        }
        held = available - start;
        if (held >= sizeof(buffer) - 1) {
            held = 0;
        } else if (held > 0) {
            memmove(buffer, buffer + start, held);
        }
    }
    close(fd);
    *bytes = total;
    return true;
}

size_t ReadSelfDmaBytes(DmaInodeSet* set, ObservedMemSample* into) {
    return ReadSelfDmaBytesGated(set, into, nullptr, 0);
}

// `map_cache == nullptr` forces the mapping pass, which is what a one-shot read
// and every test wants.
size_t ReadSelfDmaBytesGated(
        DmaInodeSet* set, ObservedMemSample* into, DmaMapCache* map_cache,
        size_t peak_total_bytes) {
    const uint64_t begin_us = MonotonicMicros();
    size_t ion_bytes = 0;
    if (ReadIonProcInfoBytes(&ion_bytes)) {
        into->dma_source = DmaSource::IonProcInfo;
        into->dma_fd_bytes = ion_bytes;
        into->dma_map_bytes = 0;
        into->fd_us = static_cast<uint32_t>(MonotonicMicros() - begin_us);
        return ion_bytes;
    }
    bool saw_any = false;
    into->dma_fd_bytes = ReadDmaBytesFromFds(set, &saw_any);
    const uint64_t fds_done_us = MonotonicMicros();
    into->fd_us = static_cast<uint32_t>(fds_done_us - begin_us);

    bool run_map_pass = true;
    if (map_cache != nullptr) {
        // Compared against the peak *including* the carried mapping figure, so
        // the gate can only skip samples that are already below the peak even
        // when credited with the last known mapping contribution.
        const size_t provisional =
                into->rss_bytes + into->dma_fd_bytes + map_cache->bytes;
        run_map_pass = !map_cache->ever_ran || provisional >= peak_total_bytes ||
                       map_cache->samples_since_refresh >= kMapRefreshSamples;
    }
    if (run_map_pass) {
        into->dma_map_bytes = ReadDmaBytesFromMaps(set, &saw_any);
        into->map_us = static_cast<uint32_t>(MonotonicMicros() - fds_done_us);
        if (map_cache != nullptr) {
            map_cache->bytes = into->dma_map_bytes;
            map_cache->ever_ran = true;
            map_cache->samples_since_refresh = 0;
            ++map_cache->refreshes;
            if (into->dma_map_bytes > map_cache->max_bytes) {
                map_cache->max_bytes = into->dma_map_bytes;
            }
        }
    } else {
        into->dma_map_bytes = map_cache->bytes;
        ++map_cache->samples_since_refresh;
        // The descriptor pass alone decided this sample, so the source label
        // must not fall back to "no accounting" just because the mapping pass
        // was skipped.
        saw_any = saw_any || map_cache->max_bytes != 0;
    }
    // "No buffer found" and "this kernel exposes no dmabuf accounting" are
    // different facts, and only the second one invalidates a zero.
    into->dma_source = saw_any ? DmaSource::FdAndMaps : DmaSource::None;
    return into->dma_fd_bytes + into->dma_map_bytes;
}

ObservedMemSample ReadObservedMemory(DmaInodeSet* set) {
    return ReadObservedMemoryGated(set, nullptr, nullptr, 0);
}

ObservedMemSample ReadObservedMemoryGated(
        DmaInodeSet* set, DmaMapCache* map_cache, GpuMmapCache* gpu_cache,
        size_t peak_total_bytes) {
    ObservedMemSample sample;
    const uint64_t begin_us = MonotonicMicros();
    const RssBreakdown rss = ReadSelfRss();
    sample.status_us = static_cast<uint32_t>(MonotonicMicros() - begin_us);
    if (!rss.valid) {
        return sample;
    }
    sample.rss_bytes = rss.vm_rss_kb * 1024;
    ResetDmaInodeSet(set);
    sample.dma_bytes =
            ReadSelfDmaBytesGated(set, &sample, map_cache, peak_total_bytes);
    // Last, so its own gate can weigh rss and dma as they were actually measured
    // this sample rather than against a carried guess.
    ReadSelfGpuMmapBytesGated(&sample, gpu_cache, peak_total_bytes);
    sample.valid = true;
    return sample;
}

size_t DefaultPeakStepBytes() {
    return kDefaultPeakStepBytes;
}

size_t MinPeakStepBytes() {
    return kMinPeakStepBytes;
}

size_t NextPeakThreshold(size_t peak_bytes, size_t step_bytes) {
    if (step_bytes == 0) {
        // Refresh on every new peak.
        return peak_bytes;
    }
    size_t increment = peak_bytes / 4;
    if (increment > step_bytes) {
        increment = step_bytes;
    }
    if (increment < kMinPeakStepBytes) {
        increment = kMinPeakStepBytes;
    }
    return peak_bytes + increment;
}

bool ObservedPeakSampler::Start(
        unsigned interval_ms, size_t floor_bytes, size_t step_bytes,
        PeakCallback on_new_peak) {
    if (interval_ms == 0 || on_new_peak == nullptr ||
        started_.load(std::memory_order_acquire)) {
        return false;
    }
    interval_ms_ = interval_ms;
    floor_bytes_ = floor_bytes;
    step_bytes_ = step_bytes;
    on_new_peak_ = on_new_peak;
    running_.store(true, std::memory_order_release);
    if (pthread_create(&thread_, nullptr, &ObservedPeakSampler::Trampoline, this) !=
        0) {
        running_.store(false, std::memory_order_release);
        return false;
    }
    started_.store(true, std::memory_order_release);
    return true;
}

void ObservedPeakSampler::Stop() {
    if (!started_.load(std::memory_order_acquire)) {
        return;
    }
    running_.store(false, std::memory_order_release);
    // Joined rather than detached: the sampler can be inside a snapshot holding
    // the hook's locks, and finalization is about to block every allocator
    // operation in the process.
    pthread_join(thread_, nullptr);
    started_.store(false, std::memory_order_release);
}

ObservedSamplerStats ObservedPeakSampler::stats() const {
    ObservedSamplerStats out;
    out.interval_ms = interval_ms_;
    out.dma_source =
            static_cast<DmaSource>(dma_source_.load(std::memory_order_relaxed));
    out.samples = samples_.load(std::memory_order_relaxed);
    out.valid_samples = valid_samples_.load(std::memory_order_relaxed);
    out.snapshots = snapshots_.load(std::memory_order_relaxed);
    out.total_sample_us = total_sample_us_.load(std::memory_order_relaxed);
    out.max_sample_us = max_sample_us_.load(std::memory_order_relaxed);
    out.total_snapshot_us = total_snapshot_us_.load(std::memory_order_relaxed);
    out.max_snapshot_us = max_snapshot_us_.load(std::memory_order_relaxed);
    out.total_status_us = total_status_us_.load(std::memory_order_relaxed);
    out.total_fd_us = total_fd_us_.load(std::memory_order_relaxed);
    out.total_map_us = total_map_us_.load(std::memory_order_relaxed);
    out.total_gpu_us = total_gpu_us_.load(std::memory_order_relaxed);
    out.map_passes = map_cache_.refreshes;
    out.max_map_only_bytes = map_cache_.max_bytes;
    out.gpu_passes = gpu_cache_.refreshes;
    out.max_gpu_bytes_seen = gpu_cache_.max_bytes;
    out.gpu_source =
            static_cast<GpuMmapSource>(gpu_source_.load(std::memory_order_relaxed));
    const uint64_t first = first_sample_us_.load(std::memory_order_relaxed);
    const uint64_t last = last_sample_us_.load(std::memory_order_relaxed);
    out.span_us = last > first ? last - first : 0;
    out.dedup_overflowed = dedup_overflowed_.load(std::memory_order_relaxed);
    out.peak_total_bytes = peak_total_bytes_.load(std::memory_order_relaxed);
    out.peak_total_rss_bytes = peak_total_rss_bytes_.load(std::memory_order_relaxed);
    out.peak_total_dma_bytes = peak_total_dma_bytes_.load(std::memory_order_relaxed);
    out.peak_total_dma_fd_bytes =
            peak_total_dma_fd_bytes_.load(std::memory_order_relaxed);
    out.peak_total_dma_map_bytes =
            peak_total_dma_map_bytes_.load(std::memory_order_relaxed);
    out.peak_total_gpu_bytes = peak_total_gpu_bytes_.load(std::memory_order_relaxed);
    out.max_rss_bytes = max_rss_bytes_.load(std::memory_order_relaxed);
    out.max_dma_bytes = max_dma_bytes_.load(std::memory_order_relaxed);
    out.max_gpu_bytes = max_gpu_bytes_.load(std::memory_order_relaxed);
    return out;
}

void* ObservedPeakSampler::Trampoline(void* self) {
    static_cast<ObservedPeakSampler*>(self)->Run();
    return nullptr;
}

void ObservedPeakSampler::Run() {
    // Everything this thread allocates -- including the snapshot vector -- must
    // stay out of the tracked totals, or the sampler would show up in the
    // numbers it exists to measure.
    DebugDisableSet(true);

    size_t next_threshold = floor_bytes_;
    while (running_.load(std::memory_order_acquire)) {
        const uint64_t begin_us = MonotonicMicros();
        const ObservedMemSample sample = ReadObservedMemoryGated(
                &inodes_, &map_cache_, &gpu_cache_,
                peak_total_bytes_.load(std::memory_order_relaxed));
        const uint64_t read_done_us = MonotonicMicros();
        const uint64_t read_us = read_done_us - begin_us;
        samples_.fetch_add(1, std::memory_order_relaxed);
        if (first_sample_us_.load(std::memory_order_relaxed) == 0) {
            first_sample_us_.store(begin_us, std::memory_order_relaxed);
        }
        last_sample_us_.store(read_done_us, std::memory_order_relaxed);
        total_sample_us_.fetch_add(read_us, std::memory_order_relaxed);
        if (read_us > max_sample_us_.load(std::memory_order_relaxed)) {
            max_sample_us_.store(read_us, std::memory_order_relaxed);
        }
        total_status_us_.fetch_add(sample.status_us, std::memory_order_relaxed);
        total_fd_us_.fetch_add(sample.fd_us, std::memory_order_relaxed);
        total_map_us_.fetch_add(sample.map_us, std::memory_order_relaxed);
        total_gpu_us_.fetch_add(sample.gpu_us, std::memory_order_relaxed);
        if (sample.valid) {
            valid_samples_.fetch_add(1, std::memory_order_relaxed);
            dma_source_.store(
                    static_cast<uint8_t>(sample.dma_source),
                    std::memory_order_relaxed);
            gpu_source_.store(
                    static_cast<uint8_t>(sample.gpu_source),
                    std::memory_order_relaxed);
            if (inodes_.overflowed) {
                dedup_overflowed_.store(true, std::memory_order_relaxed);
            }
            if (sample.rss_bytes > max_rss_bytes_.load(std::memory_order_relaxed)) {
                max_rss_bytes_.store(sample.rss_bytes, std::memory_order_relaxed);
            }
            if (sample.dma_bytes > max_dma_bytes_.load(std::memory_order_relaxed)) {
                max_dma_bytes_.store(sample.dma_bytes, std::memory_order_relaxed);
            }
            if (sample.gpu_bytes > max_gpu_bytes_.load(std::memory_order_relaxed)) {
                max_gpu_bytes_.store(sample.gpu_bytes, std::memory_order_relaxed);
            }
            const size_t total = sample.total();
            if (total > peak_total_bytes_.load(std::memory_order_relaxed)) {
                peak_total_bytes_.store(total, std::memory_order_relaxed);
                peak_total_rss_bytes_.store(
                        sample.rss_bytes, std::memory_order_relaxed);
                peak_total_dma_bytes_.store(
                        sample.dma_bytes, std::memory_order_relaxed);
                peak_total_dma_fd_bytes_.store(
                        sample.dma_fd_bytes, std::memory_order_relaxed);
                peak_total_dma_map_bytes_.store(
                        sample.dma_map_bytes, std::memory_order_relaxed);
                peak_total_gpu_bytes_.store(
                        sample.gpu_bytes, std::memory_order_relaxed);
                if (total > next_threshold) {
                    next_threshold = NextPeakThreshold(total, step_bytes_);
                    snapshots_.fetch_add(1, std::memory_order_relaxed);
                    on_new_peak_(sample);
                    const uint64_t snapshot_us = MonotonicMicros() - read_done_us;
                    total_snapshot_us_.fetch_add(
                            snapshot_us, std::memory_order_relaxed);
                    if (snapshot_us >
                        max_snapshot_us_.load(std::memory_order_relaxed)) {
                        max_snapshot_us_.store(
                                snapshot_us, std::memory_order_relaxed);
                    }
                }
            }
        }
        // Sleep at least as long as reading /proc took, so the sampler can never
        // exceed half a core no matter how many descriptors or mappings the
        // process holds. A run whose reads are cheap keeps the requested cadence
        // exactly; one whose reads are not would otherwise perturb the very peak
        // it is measuring. The achieved cadence is reported, so a throttled run
        // is visible rather than silently assumed to be on cadence.
        unsigned sleep_ms = interval_ms_;
        const unsigned read_ms = static_cast<unsigned>(read_us / 1000);
        if (read_ms > sleep_ms) {
            sleep_ms = read_ms;
        }
        // Bounded so Stop() does not wait a whole interval on a slow cadence.
        while (sleep_ms > 0 && running_.load(std::memory_order_acquire)) {
            const unsigned slice = sleep_ms > 10 ? 10 : sleep_ms;
            SleepMillis(slice);
            sleep_ms -= slice;
        }
    }
}

ObservedPeakSampler& ObservedPeakSamplerInstance() {
    return g_observed_peak_sampler;
}
