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
// How many consistent growth steps decide whether VmRSS counts these mappings.
// More than one, so a single step where a mapping appears in the same tick as
// unrelated host allocation cannot settle it the wrong way.
constexpr unsigned kGpuInclusionEvidence = 2;

// Device nodes whose mappings escape both other signals. Only kgsl qualifies;
// see GpuMmapBytesFromSmapsText for why adding Mali here would inflate rather
// than complete the total.
//
// Matched as the region's whole pathname, not as a substring of the line. A
// substring test accepts two things that are not the device:
//   "/dev/kgsl-3d0-shim.bin"      an ordinary file whose name starts the same way
//   "[anon:/dev/kgsl-3d0 shadow]" an anonymous region named through
//                                 PR_SET_VMA_ANON_NAME, whose name is chosen by
//                                 whoever mapped it and lands verbatim in the
//                                 header line
// The second is the dangerous one: such a region can be a large sparse
// reservation with Rss 0, which is counted in full -- exactly the over-report
// this scan excludes ARM Mali to avoid, reintroduced through the name.
constexpr char kGpuNodePaths[][16] = {"/dev/kgsl-3d0", "/dev/kgsl"};

// Whether `path` names one of the device nodes above, allowing the " (deleted)"
// suffix the kernel appends once the node has been unlinked.
bool IsGpuNodePath(const char* path) {
    for (const char* node : kGpuNodePaths) {
        const size_t length = strlen(node);
        if (strncmp(path, node, length) != 0) {
            continue;
        }
        const char* rest = path + length;
        if (*rest == '\0' || strcmp(rest, " (deleted)") == 0) {
            return true;
        }
    }
    return false;
}

// The pathname of a /proc maps-style header line: the sixth field, which begins
// after the inode and runs to end of line. Returns nullptr when the region is
// anonymous, i.e. when there is no sixth field at all.
//
// Taken positionally rather than by searching the line, so no text appearing in
// an earlier field -- a device number, an offset -- can be mistaken for a path.
const char* MapsLinePath(const char* line) {
    // "start-end perms offset dev inode path": skip five whitespace-delimited
    // fields, then the whitespace padding before the sixth.
    for (int field = 0; field < 5; ++field) {
        while (*line != '\0' && *line != ' ' && *line != '\t') {
            ++line;
        }
        while (*line == ' ' || *line == '\t') {
            ++line;
        }
        if (*line == '\0') {
            return nullptr;
        }
    }
    return line;
}

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

// Convenience wrapper naming the only path the sampler ever reads.
bool ReadGpuMmapBytesFromSmaps(size_t* bytes) {
    return ReadGpuMmapBytesFrom("/proc/self/smaps", bytes);
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
    if (set->count >= DmaInodeSet::kMaxDedupedBuffers) {
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

void SleepMicros(uint64_t micros) {
    struct timespec request;
    request.tv_sec = static_cast<time_t>(micros / 1000000ULL);
    request.tv_nsec = static_cast<long>(micros % 1000000ULL) * 1000L;
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
        case GpuMmapSource::Maps:
            return "maps";
        case GpuMmapSource::Unprobed:
            break;
    }
    return "unprobed";
}

size_t GpuMmapBytesFromSmapsLine(const char* line, GpuRegionScan* scan) {
    if (line == nullptr || scan == nullptr) {
        return 0;
    }
    uintptr_t start = 0;
    uintptr_t end = 0;
    // A region header is the only line that parses as two hex addresses joined by
    // '-'. Field lines ("Size:", "Rss:", "VmFlags:") begin with a letter and
    // cannot.
    if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &start, &end) == 2) {
        const char* path = MapsLinePath(line);
        scan->in_gpu_region = path != nullptr && IsGpuNodePath(path);
        scan->region_size = end > start ? static_cast<size_t>(end - start) : 0;
        return 0;
    }
    if (!scan->in_gpu_region || strncmp(line, "Rss:", 4) != 0) {
        return 0;
    }
    long rss_kb = 0;
    // One Rss line per region; clearing here keeps a later field line of the same
    // region from being read as a second one. Done before the parse check so a
    // line that fails to parse still closes the region rather than leaving it
    // open for the next field line to be read as its residency.
    scan->in_gpu_region = false;
    if (sscanf(line + 4, "%ld", &rss_kb) != 1) {
        // No number where the kernel always puts one. Counting the region in
        // full here would treat a parse failure as "nothing is resident", which
        // is the most inflationary reading available; drop the region instead.
        return 0;
    }
    const size_t resident = static_cast<size_t>(rss_kb < 0 ? 0 : rss_kb) * 1024;
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

// Streamed through a stack buffer rather than slurped whole, matching
// ReadDmaBytesFromMaps: smaps for a process with thousands of mappings runs to
// megabytes, and a shared static buffer large enough for it would both sit in
// .bss for the life of every process and be corrupted by any second caller.
// GpuRegionScan carries the only state a refill can split.
bool ReadGpuMmapBytesFrom(const char* path, size_t* bytes) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
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

size_t GpuMappedBytesFromMapsLine(const char* line) {
    if (line == nullptr) {
        return 0;
    }
    uintptr_t start = 0;
    uintptr_t end = 0;
    if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &start, &end) != 2 || end <= start) {
        return 0;
    }
    const char* path = MapsLinePath(line);
    if (path == nullptr || !IsGpuNodePath(path)) {
        return 0;
    }
    return static_cast<size_t>(end - start);
}

bool ReadGpuMappedBytesFrom(const char* path, size_t* bytes) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    size_t total = 0;
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
            total += GpuMappedBytesFromMapsLine(buffer + start);
            start = static_cast<size_t>(newline - buffer) + 1;
        }
        held = available - start;
        if (held >= sizeof(buffer) - 1) {
            // A single line longer than the buffer. Dropping it is preferable to
            // parsing a fragment as if it were a whole region header.
            held = 0;
        } else if (held > 0) {
            memmove(buffer, buffer + start, held);
        }
    }
    close(fd);
    *bytes = total;
    return true;
}

bool ReadGpuSmapsReading(const char* path, GpuSmapsReading* into) {
    if (into == nullptr) {
        return false;
    }
    *into = GpuSmapsReading{};
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char buffer[16384];
    size_t held = 0;
    bool in_device = false;
    for (;;) {
        const ssize_t got = read(fd, buffer + held, sizeof(buffer) - held - 1);
        if (got <= 0) {
            break;
        }
        const size_t available = held + static_cast<size_t>(got);
        buffer[available] = '\0';
        size_t start = 0;
        for (;;) {
            char* newline =
                    static_cast<char*>(memchr(buffer + start, '\n', available - start));
            if (newline == nullptr) {
                break;
            }
            *newline = '\0';
            char* line = buffer + start;
            start = static_cast<size_t>(newline - buffer) + 1;
            uintptr_t lo = 0, hi = 0;
            if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &lo, &hi) == 2) {
                const char* p = MapsLinePath(line);
                in_device = p != nullptr && IsGpuNodePath(p);
                if (in_device && hi > lo) {
                    into->device_mapped_bytes += static_cast<size_t>(hi - lo);
                }
                continue;
            }
            if (strncmp(line, "Rss:", 4) != 0) {
                continue;
            }
            long rss_kb = 0;
            if (sscanf(line + 4, "%ld", &rss_kb) != 1 || rss_kb < 0) {
                continue;
            }
            const size_t rss = static_cast<size_t>(rss_kb) * 1024;
            into->all_rss_bytes += rss;
            if (in_device) {
                into->device_rss_bytes += rss;
            }
        }
        held = available - start;
        if (held >= sizeof(buffer) - 1) {
            held = 0;
            in_device = false;
        } else if (held > 0) {
            memmove(buffer, buffer + start, held);
        }
    }
    close(fd);
    into->valid = true;
    return true;
}

size_t GpuBytesFromReading(const GpuSmapsReading& reading, size_t vmrss_bytes) {
    if (!reading.valid) {
        return 0;
    }
    // How much of what the per-VMA walk counted is absent from VmRSS. Floored at
    // zero: on one vendor's parts the divergence runs the other way, VmRSS
    // counting pages the walk does not, and that is not this dimension's memory.
    const size_t divergence = reading.all_rss_bytes > vmrss_bytes
            ? reading.all_rss_bytes - vmrss_bytes
            : 0;
    // Bounded by the device regions' own Rss, so any other source of divergence
    // cannot be attributed here.
    return divergence < reading.device_rss_bytes ? divergence
                                                 : reading.device_rss_bytes;
}

bool GpuSampleNeedsRead(const GpuMmapCache& cache, size_t mapped_bytes) {
    // Nothing mapped and nothing read: there is no device memory to attribute, so
    // the expensive read buys nothing. This is what keeps a process that merely
    // has the device node from paying for a dimension it does not use.
    if (mapped_bytes == 0 && !cache.have_read) {
        return false;
    }
    if (!cache.have_read) {
        return true;
    }
    // The mapped total changing is the event that can change the answer.
    return mapped_bytes != cache.mapped_at_read;
}

size_t ReadSelfGpuMmapBytes(ObservedMemSample* into) {
    if (into == nullptr) {
        return 0;
    }
    return ReadSelfGpuMmapBytesGated(into, nullptr, 0);
}

size_t ReadSelfGpuMmapBytesGated(
        ObservedMemSample* into, GpuMmapCache* cache, size_t peak_total_bytes) {
    // peak_total_bytes is no longer consulted, and there is no longer a pass
    // expensive enough to want gating. What used to gate the smaps read on
    // "could this sample move the peak" was true on every sample of a growing
    // run, so the costliest read in the sampler ran at full rate through the
    // whole ramp to the peak. smaps is not read at all now: see GpuMmapCache.
    (void)peak_total_bytes;

    // Checked before the clock is read: on a platform without the device node
    // this pass must cost nothing at all, and a clock_gettime per sample to time
    // a no-op is exactly the kind of cost that accumulates unseen.
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

    const uint64_t begin_us = MonotonicMicros();
    size_t mapped = 0;
    const bool ok = ReadGpuMappedBytesFrom("/proc/self/maps", &mapped);
    into->gpu_us = static_cast<uint32_t>(MonotonicMicros() - begin_us);
    if (!ok) {
        if (cache != nullptr) {
            ++cache->read_failures;
            if (cache->have_read) {
                // A figure carried from an earlier successful read is still the
                // measurement it was; labelling it Unprobed would report a real
                // number as "nothing was measured", and because the sampler
                // stores the source last-sample-wins, one transient failure
                // would relabel the whole run.
                into->gpu_bytes = cache->bytes;
                into->gpu_source = GpuMmapSource::Maps;
                return into->gpu_bytes;
            }
        }
        // Nothing was ever read, so there is no figure -- and a zero here must
        // not be mistaken for a measured zero.
        into->gpu_bytes = 0;
        into->gpu_source = GpuMmapSource::Unprobed;
        return 0;
    }

    into->gpu_source = GpuMmapSource::Maps;
    if (cache == nullptr) {
        // One-shot read with nowhere to carry state: pay smaps and answer exactly.
        GpuSmapsReading r;
        if (!ReadGpuSmapsReading("/proc/self/smaps", &r)) {
            into->gpu_bytes = 0;
            into->gpu_source = GpuMmapSource::Unprobed;
            return 0;
        }
        into->gpu_bytes = GpuBytesFromReading(r, into->rss_bytes);
        return into->gpu_bytes;
    }
    if (GpuSampleNeedsRead(*cache, mapped)) {
        GpuSmapsReading r;
        if (ReadGpuSmapsReading("/proc/self/smaps", &r)) {
            cache->bytes = GpuBytesFromReading(r, into->rss_bytes);
            cache->mapped_at_read = mapped;
            cache->have_read = true;
            ++cache->reads;
            if (cache->bytes > cache->max_bytes) {
                cache->max_bytes = cache->bytes;
            }
        } else {
            ++cache->read_failures;
        }
    }
    into->gpu_bytes = cache->bytes;
    into->gpu_us = static_cast<uint32_t>(MonotonicMicros() - begin_us);
    return into->gpu_bytes;
}

SampleSchedule NextSampleSchedule(
        uint64_t previous_deadline_us, uint64_t now_us, uint64_t work_us,
        uint64_t interval_us) {
    SampleSchedule schedule;
    if (interval_us == 0) {
        // Start() rejects a zero interval; treat it as the smallest grid rather
        // than dividing by it.
        interval_us = 1;
    }

    uint64_t next = previous_deadline_us + interval_us;

    // Slots that elapsed while this sample was being taken are skipped, never
    // fired back to back. Catching up would concentrate the sampler's own cost
    // on a process that has just demonstrated it cannot absorb it -- and each
    // catch-up read holds mmap_lock, so the burst lands on the allocation path.
    if (next < now_us) {
        const uint64_t advance = (now_us - next) / interval_us + 1;
        next += advance * interval_us;
        schedule.skipped_slots += advance;
    }

    // Duty cap: the next sample never starts sooner than this one took, so the
    // sampler cannot exceed half a core however expensive /proc becomes. Kept
    // separate from the cadence above so a throttled run is reported as
    // throttled instead of silently redefining the requested interval.
    const uint64_t earliest_us = now_us + work_us;
    if (next < earliest_us) {
        schedule.throttled = true;
        // Advance in whole intervals so a slow sample shifts which grid slot is
        // served without shifting the grid itself.
        const uint64_t advance =
                (earliest_us - next + interval_us - 1) / interval_us;
        next += advance * interval_us;
        schedule.skipped_slots += advance;
    }

    schedule.next_deadline_us = next;
    return schedule;
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
        unsigned interval_ms, size_t floor_bytes, size_t step_bytes, bool one_shot,
        PeakCallback on_new_peak) {
    if (interval_ms == 0 || started_.load(std::memory_order_acquire)) {
        return false;
    }
    interval_ms_ = interval_ms;
    floor_bytes_ = floor_bytes;
    step_bytes_ = step_bytes;
    one_shot_ = one_shot;
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

void ObservedPeakSampler::ResetAfterForkInChild() {
    // No join: thread_ names a thread that fork() did not clone. Clearing the
    // flags makes a later Start() in the child legal and keeps Stop() from
    // blocking on a thread id that was never valid here.
    running_.store(false, std::memory_order_release);
    started_.store(false, std::memory_order_release);
    thread_ = pthread_t{};
}

bool ObservedPeakSampler::Stalled() const {
    // Never started, or Stop() shut it down: not a stall. See the header for why
    // a deliberate stop must not release the observed-peak latch.
    if (!started_.load(std::memory_order_acquire) ||
        !running_.load(std::memory_order_acquire)) {
        return false;
    }
    const uint64_t last = last_sample_us_.load(std::memory_order_relaxed);
    if (last == 0) {
        // Running, but the first read has not completed yet.
        return false;
    }
    // Deliberately generous. Run() treats the requested interval as a floor and
    // sleeps at least as long as the read took, so on a large process the real
    // period can be many times interval_ms; a tight bound here would call a
    // merely slow sampler dead and hand the peak back to the tracked criterion
    // mid-run.
    uint64_t stale_us = static_cast<uint64_t>(interval_ms_) * 1000ULL * 20ULL;
    static constexpr uint64_t kMinStaleUs = 2ULL * 1000ULL * 1000ULL;
    if (stale_us < kMinStaleUs) {
        stale_us = kMinStaleUs;
    }
    return MonotonicMicros() > last + stale_us;
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
    out.max_gpu_bytes_seen = gpu_cache_.max_bytes;
    out.gpu_reads = gpu_cache_.reads;
    out.gpu_read_failures = gpu_cache_.read_failures;
    out.gpu_source =
            static_cast<GpuMmapSource>(gpu_source_.load(std::memory_order_relaxed));
    const uint64_t first = first_sample_us_.load(std::memory_order_relaxed);
    const uint64_t last = last_sample_us_.load(std::memory_order_relaxed);
    out.span_us = last > first ? last - first : 0;
    out.dedup_overflowed = dedup_overflowed_.load(std::memory_order_relaxed);
    out.skipped_slots = skipped_slots_.load(std::memory_order_relaxed);
    out.throttled_samples = throttled_samples_.load(std::memory_order_relaxed);
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
    //
    // Skipped with no callback: nothing tracks in that mode, so there is nothing
    // to exclude, and the tracker's thread-local key was never created -- writing
    // to it would be a write to whatever key id 0 belongs to in this process.
    if (on_new_peak_ != nullptr) {
        DebugDisableSet(true);
    }

    size_t next_threshold = floor_bytes_;
    // The grid the cadence is measured against. Anchored once here so every
    // later deadline derives from this instant rather than from whenever the
    // previous sample happened to finish.
    const uint64_t interval_us = static_cast<uint64_t>(interval_ms_) * 1000ULL;
    uint64_t next_deadline_us = MonotonicMicros();
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
                if (on_new_peak_ != nullptr && total > next_threshold) {
                    snapshots_.fetch_add(1, std::memory_order_relaxed);
                    const bool retained = on_new_peak_(sample);
                    // A one-shot run pins the threshold out of reach instead of
                    // stopping the thread: the reads are the cheap half and keep
                    // the observed-peak statistics complete, so the report can
                    // still say what the run's real peak was and how far past the
                    // floor it went. Only the stack walk -- the half that takes
                    // the allocation locks -- is given up.
                    //
                    // A crossing that retained nothing leaves the threshold at
                    // the floor so the next new peak tries again; otherwise a
                    // process that crossed before its first stack-carrying
                    // allocation would report nothing at all.
                    if (one_shot_) {
                        if (retained) {
                            next_threshold = SIZE_MAX;
                        }
                    } else {
                        next_threshold = NextPeakThreshold(total, step_bytes_);
                    }
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
        // Schedule the next sample against the interval grid rather than
        // against "now". work_us covers the snapshot as well as the read: both
        // are cost this sampler imposes on the process it is measuring.
        const uint64_t work_done_us = MonotonicMicros();
        const SampleSchedule schedule = NextSampleSchedule(
                next_deadline_us, work_done_us, work_done_us - begin_us,
                interval_us);
        next_deadline_us = schedule.next_deadline_us;
        if (schedule.skipped_slots != 0) {
            skipped_slots_.fetch_add(
                    schedule.skipped_slots, std::memory_order_relaxed);
        }
        if (schedule.throttled) {
            throttled_samples_.fetch_add(1, std::memory_order_relaxed);
        }

        // Sliced so Stop() stays responsive on a slow cadence. The remaining
        // time is recomputed from the deadline on every slice instead of
        // decrementing a precomputed budget: each nanosleep overshoots a little,
        // and a decremented budget would accumulate one overshoot per slice
        // (ten of them for a 100ms interval) into the very drift this loop
        // exists to avoid.
        while (running_.load(std::memory_order_acquire)) {
            const uint64_t now_us = MonotonicMicros();
            if (now_us >= next_deadline_us) {
                break;
            }
            const uint64_t remaining_us = next_deadline_us - now_us;
            static constexpr uint64_t kMaxSliceUs = 10000;
            SleepMicros(remaining_us > kMaxSliceUs ? kMaxSliceUs : remaining_us);
        }
    }
}

ObservedPeakSampler& ObservedPeakSamplerInstance() {
    return g_observed_peak_sampler;
}
