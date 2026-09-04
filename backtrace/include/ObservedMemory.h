#pragma once

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include <atomic>

// ---------------------------------------------------------------------------
// The externally observable memory footprint of this process.
//
// The hook's own counters answer "which call site asked for how many bytes".
// They do not answer "what does an evaluator watching this process see", and
// the two do not peak at the same instant: requested bytes peak when the most
// memory has been asked for, resident bytes peak when the most pages have been
// touched, and device buffers peak whenever the driver holds the most. On one
// measured run the resident peak led the device-buffer peak by 167 ms, so a
// snapshot keyed on tracked bytes described a moment the evaluator never
// reported, and its host figure was ~19 MB below the one being optimised
// against.
//
// Everything declared here therefore reads the kernel's own accounting out of
// the same /proc files an external sampler reads, so the stack snapshot can be
// placed at the instant the evaluator calls the peak.
// ---------------------------------------------------------------------------

// Parses "<key><whitespace><decimal>" out of an already-read /proc text buffer.
// Returns 0 when the key is absent, which every caller treats as "not
// reported by this kernel" rather than as a real zero.
size_t ReadStatusField(const char* text, const char* key);

// This process's resident-set breakdown, in KB, from /proc/self/status.
//
// Tracked allocation bytes can only ever explain the anonymous part. Recording
// the split next to the tracked totals makes the difference attributable
// instead of leaving the reader to guess whether the hook missed allocations or
// is being compared against file-backed pages it can never see.
struct RssBreakdown {
    size_t vm_rss_kb = 0;
    size_t anon_kb = 0;
    size_t file_kb = 0;
    size_t shmem_kb = 0;
    bool valid = false;
};

RssBreakdown ReadSelfRss();

// Which interface supplied the dmabuf figure. Reported so an empty measurement
// is never presented as "this process holds no device memory".
enum class DmaSource : uint8_t {
    Unprobed = 0,
    // No dmabuf accounting is reachable on this kernel.
    None,
    // /proc/self/fd + fstat, plus /proc/self/maps for buffers whose descriptor
    // was closed while a mapping survives.
    FdAndMaps,
    // Kernels that expose per-process ION accounting in one process-wide file
    // instead of under /proc/<pid>.
    IonProcInfo,
};

const char* DmaSourceName(DmaSource source);

// Which interface supplied the GPU-mapping figure, and why it may be zero.
//
// A zero here has three different meanings and they are not interchangeable: no
// measurement was attempted, this platform structurally cannot hold such memory,
// or it was measured and the process holds none. Collapsing them would let a
// missing dimension read as a real zero.
enum class GpuMmapSource : uint8_t {
    Unprobed = 0,
    // No GPU device node whose mappings escape both other signals exists here,
    // so the figure is structurally zero and nothing is read.
    NotApplicable,
    // Region sizes from /proc/self/maps, every sample. Nothing else is read.
    Maps,
};

const char* GpuMmapSourceName(GpuMmapSource source);

// GPU device memory mapped into this process that neither of the other two
// signals can see.
//
// Some GPU drivers hand out device memory by mmap'ing a character device rather
// than a dmabuf. Such a region is invisible twice over: it is not a dmabuf, so
// the descriptor and mapping passes above never see it, and on the platforms
// that need this dimension the kernel does not count it in VmRSS either. It is
// therefore absent from rss_bytes and from dma_bytes at the same time, while an
// evaluator reading the process's real footprint reports it.
//
// Two corrections to what an earlier version of this claimed, both measured:
//
//   * It is not true that these regions carry Rss 0 in smaps. On an Adreno
//     target an OpenCL buffer's kgsl region reports Rss equal to its full Size
//     while VmRSS does not move at all -- per-VMA Rss counts pages that
//     mm->rss_stat does not, so the two genuinely disagree for these mappings.
//     A `Size - Rss` figure therefore evaluates to zero exactly where the real
//     contribution is the full size.
//   * It is not a property of the vendor. A second Adreno target, on a newer
//     kernel and allocating through the driver's ioctl rather than OpenCL,
//     counts the same kind of mapping in VmRSS as it faults.
//
// So the sampler reads the mapped size from /proc/self/maps and decides what to
// do with it from observed VmRSS behaviour; see GpuMmapCache. smaps is not read.
//
// Scoped to the kgsl node. This is a deliberate asymmetry and not an oversight:
// dma-buf backed paths -- including the vendor OpenCL host-pointer imports and
// EGL dma-buf image imports -- are already counted by dma_bytes, and importing
// one was measured to add no mapping of its own. Other drivers reserve large
// sparse ranges whose mapped size has no relation to the pages backed. Both
// cases are set out in docs/GPU_MEMORY_ACCOUNTING.md.

// Carried parse state for the smaps scan below.
//
// A region's residency arrives on a line *after* its header, so a reader that
// refills its buffer part-way through a region has to keep this across refills.
struct GpuRegionScan {
    bool in_gpu_region = false;
    size_t region_size = 0;
};

// Bytes one /proc/self/smaps line contributes, as per-region `Size - Rss`.
//
// NOT on the sampling path: see above for why that quantity is not the one this
// dimension needs. Kept because it is the only way to read per-region residency,
// which is what a cross-check against an external sampler's own figures needs,
// and because the parse is worth having covered by a test. `scan` carries region
// state between calls; `line` is only read, never written.
size_t GpuMmapBytesFromSmapsLine(const char* line, GpuRegionScan* scan);

// The whole-buffer form of the above. Takes a mutable buffer because it splits
// `text` on newlines in place.
size_t GpuMmapBytesFromSmapsText(char* text);

// Sums the GPU regions in a smaps-format file. Returns false only when the file
// could not be opened, so "opened and found nothing" stays distinguishable from
// "no accounting available". Exposed with an explicit path for the same reason as
// ReadIonProcInfoBytesFrom: the kernels that emit these regions are not the
// kernels a host test runs on, and an unreadable path is how a test reaches the
// read-failure path at all.
bool ReadGpuMmapBytesFrom(const char* path, size_t* bytes);

// The size one /proc/self/maps line contributes: the region's full mapped size
// if it is one of the device nodes, else 0.
//
// maps carries the same header lines as smaps, with the same paths and the same
// address ranges, and costs ~240x less to read because the kernel does not walk
// page tables to produce it. What it does not carry is per-region residency,
// which is the only thing smaps is needed for -- and residency is exactly the
// part that barely moves. See GpuMmapCache.
size_t GpuMappedBytesFromMapsLine(const char* line);

// Sums the device regions' mapped sizes in a maps-format file.
bool ReadGpuMappedBytesFrom(const char* path, size_t* bytes);

// One smaps reading of the three quantities the figure is computed from.
//
// `VmRSS` is supposed to equal the sum of per-VMA `Rss`. Where a device mapping's
// pages are counted by the page-table walk that produces per-VMA `Rss` but not by
// the `mm->rss_stat` counters behind `VmRSS`, the two diverge by exactly the
// amount that is invisible to `rss_bytes` -- which is the quantity this dimension
// exists to report. Measured on two targets of the same vendor that disagree:
//
//   * one reports the region's `Rss` as its full size while `VmRSS` never moves,
//     and the divergence grows by exactly that size;
//   * the other faults the region in later, and its `Rss` and `VmRSS` grow
//     together, so the divergence stays at zero throughout.
//
// Later parts of the same vendor create no device mapping at all, and the other
// vendor's parts count theirs in `VmRSS` at creation. All four fall out of the
// same arithmetic without being special-cased. See
// docs/GPU_MEMORY_ACCOUNTING.md.
struct GpuSmapsReading {
    // Sum of per-VMA Rss over every region.
    size_t all_rss_bytes = 0;
    // Sum of per-VMA Rss over device-node regions only.
    size_t device_rss_bytes = 0;
    // Sum of mapped size over device-node regions.
    size_t device_mapped_bytes = 0;
    bool valid = false;
};

// Reads the three sums from a smaps-format file. Exposed with an explicit path so
// device region text can be covered by a test.
bool ReadGpuSmapsReading(const char* path, GpuSmapsReading* into);

// The figure to report, given a reading and the process's VmRSS.
//
// Bounded by the device regions' own Rss, so an unrelated source of divergence
// cannot inflate it, and floored at zero, so the reverse divergence measured on
// one vendor's parts -- `VmRSS` counting pages the per-VMA walk does not -- reports
// nothing rather than underflowing.
size_t GpuBytesFromReading(const GpuSmapsReading& reading, size_t vmrss_bytes);

// Carried state for the GPU pass.
//
// The mapped total comes from /proc/self/maps every sample, which is cheap. The
// reading above needs smaps, which is not: measured 2.4-10 ms against 27 us for
// maps, because the kernel walks every PTE of every VMA to produce per-region
// Rss. So smaps is read only when the device mapped total changes -- the event
// that can change the answer -- and the figure is carried between those events.
//
// On a platform with no device node the pass stops at `probed_absent` and reads
// neither file. Where device mappings exist but never change size, the smaps read
// happens once for the run.
struct GpuMmapCache {
    // Last figure reported, and the largest ever reported.
    size_t bytes = 0;
    size_t max_bytes = 0;
    // Device mapped total at the last smaps read, and whether one has happened.
    size_t mapped_at_read = 0;
    bool have_read = false;
    // How many smaps reads the run paid, and how many failed.
    size_t reads = 0;
    size_t read_failures = 0;
    bool probed_absent = false;
};

// Whether this sample must pay for a smaps read, given the mapped total it just
// read from maps. Pure, so the policy is testable without a device.
bool GpuSampleNeedsRead(const GpuMmapCache& cache, size_t mapped_bytes);

// Scratch state for one dmabuf measurement pass.
//
// A buffer can be referenced by several descriptors and several mappings at
// once, and an evaluator counts each *buffer* once, so a pass has to dedup by
// inode. The set is owned by the caller and stamped rather than cleared, so a
// sampler running every millisecond neither allocates nor re-zeroes it per
// sample.
struct DmaInodeSet {
    static constexpr size_t kSlots = 4096;
    // Buffers one pass can actually dedup, which is *half* the slot count, not
    // all of it. Open addressing degrades badly as it fills, so insertion stops
    // at a 0.5 load factor and reports overflow from there on. Named explicitly
    // because kSlots alone reads as the capacity and overstates it 2x: an
    // overflow at 2048 buffers would otherwise look like a bug against a
    // "4096-slot" table rather than the documented limit.
    static constexpr size_t kMaxDedupedBuffers = kSlots / 2;
    uint64_t inode[kSlots];
    uint32_t stamp[kSlots];
    uint32_t generation;
    size_t count;
    // Set when a pass held more distinct buffers than the table can dedup (see
    // kMaxDedupedBuffers). The resulting total may double-count, so it is
    // surfaced rather than hidden: a silently wrong DMA figure would move the
    // apparent peak instant.
    bool overflowed;
};

// Begins a pass. Cheap: it bumps the stamp generation instead of clearing.
void ResetDmaInodeSet(DmaInodeSet* set);

// Bytes one /proc/<pid>/maps line contributes, counted only if it maps a dmabuf
// that this pass has not already accounted for. `line` is modified in place.
// Exposed so the exact mapping text a device emits can be covered by a test
// rather than by inspection.
size_t DmaBytesFromMapsLine(char* line, DmaInodeSet* set, bool* saw_any);

// Sums this process's buffers out of a process-wide ION accounting file.
// Exposed with an explicit path and pid for the same reason: the kernels that
// publish this file are not the kernels a host test runs on.
bool ReadIonProcInfoBytesFrom(const char* path, int pid, size_t* bytes);

// What the sampler loop should do after finishing a sample.
//
// Split out of ObservedPeakSampler::Run() as a pure function so the scheduling
// rules can be tested exhaustively without a thread or a clock: the loop itself
// is untestable in any deterministic way.
struct SampleSchedule {
    // Instant the next sample should start at. Always on the interval grid
    // established by the first sample, so phase is preserved across a slow one.
    uint64_t next_deadline_us = 0;
    // Grid slots that went unserved, either because the last sample overran
    // them or because the duty cap pushed past them.
    uint64_t skipped_slots = 0;
    // The duty cap, rather than the requested cadence, decided the wait.
    bool throttled = false;
};

// Fixed-rate scheduling for the sampler.
//
// The next deadline is derived from the previous *deadline*, not from "now", so
// the time a sample took is absorbed into the period instead of being added to
// it. A fixed-delay loop (sleep interval after the work) instead runs at
// interval + work_us forever, and because the reads get more expensive as the
// process grows, that drift is worst exactly when peak resolution matters most.
//
// `work_us` is the whole iteration's cost, not just the /proc read: the duty cap
// exists to bound what the sampler takes from the process, and a peak snapshot
// is as much the sampler's cost as the read that triggered it.
SampleSchedule NextSampleSchedule(
        uint64_t previous_deadline_us, uint64_t now_us, uint64_t work_us,
        uint64_t interval_us);

// One reading of the footprint an evaluator sees: host resident bytes, device
// buffer bytes, and GPU device mappings that neither of those two covers. The sum
// is the quantity to maximise, because that is the quantity such an evaluator
// reports as the process peak.
struct ObservedMemSample {
    size_t rss_bytes = 0;
    size_t dma_bytes = 0;
    // The two halves of dma_bytes. Reported separately because they answer
    // whether the second, more expensive pass is buying anything on a given
    // platform: buffers still held by a descriptor, versus buffers whose
    // descriptor was closed while a mapping survives.
    size_t dma_fd_bytes = 0;
    size_t dma_map_bytes = 0;
    // GPU device mappings counted by neither of the above. Third term of the sum
    // rather than folded into dma_bytes: it comes from a different interface, is
    // zero on platforms where such memory is a dmabuf, and an evaluator that
    // reports it reports it as its own line. See GpuMmapBytesFromSmapsText.
    size_t gpu_bytes = 0;
    DmaSource dma_source = DmaSource::Unprobed;
    GpuMmapSource gpu_source = GpuMmapSource::Unprobed;
    bool valid = false;
    // Cost of each /proc pass, so the read budget can be attributed instead of
    // guessed. Which pass dominates is platform- and workload-dependent: a
    // process with thousands of mappings pays far more for the mapping pass
    // than for the descriptor pass, even when the mapping pass finds almost
    // nothing.
    uint32_t status_us = 0;
    uint32_t fd_us = 0;
    uint32_t map_us = 0;
    uint32_t gpu_us = 0;

    size_t total() const { return rss_bytes + dma_bytes + gpu_bytes; }
};

ObservedMemSample ReadObservedMemory(DmaInodeSet* set);

// Whether this sample must pay for a residency calibration, given the mapped
// total it just read. Pure, so the policy that decides how often the expensive
// read happens can be tested exhaustively -- the surrounding pass cannot be,
// because on any machine without the device node it returns before reaching it,
// which is every host CI runner.
bool GpuSampleNeedsCalibration(const GpuMmapCache& cache, size_t mapped);

// Folds a completed calibration into the cache. `unaccounted` is what smaps
// reported for the same regions whose mapped total is `mapped`, so the residency
// the kernel already counts in VmRSS is their difference.
void ApplyGpuCalibration(GpuMmapCache* cache, size_t mapped, size_t unaccounted);

// The figure a sample reports for a mapped total, given the carried correction.
size_t GpuBytesForMapped(const GpuMmapCache& cache, size_t mapped);

// Reads the GPU mapping figure, honouring the GpuMmapCache gate. A null cache
// forces the read, which is what a one-shot sample and every test wants.
size_t ReadSelfGpuMmapBytes(ObservedMemSample* into);
size_t ReadSelfGpuMmapBytesGated(
        ObservedMemSample* into, GpuMmapCache* cache, size_t peak_total_bytes);

// Carried state that lets the mapping pass be skipped on samples that cannot
// move the peak.
//
// The two dmabuf passes cost very different amounts. Measured on an arm64
// workload holding ~935 MB of buffers, sampling every millisecond: the
// descriptor pass took 354 us a sample, the mapping pass 665 us. Paying two
// thirds of the read budget for the mapping pass on every sample made the
// sampler run at 2.2 ms instead of the requested 1 ms, which is a fidelity
// loss, not just a cost.
//
// So the expensive pass runs only when it can matter: when the cheap passes
// alone already put this sample at or above the peak, or when the carried
// figure has gone stale. On the same workload that ran it on 12.5% of samples
// and brought the cadence to 1.58 ms with the peak instant unchanged.
//
// It is *not* skipped because it finds nothing -- it contributed as much as
// 24 MB on that run. It is skipped because a sample already below the peak
// cannot become the snapshot instant. A peak can only be missed if that
// contribution grew within the staleness window, and `max_bytes` below reports
// how large it ever got so the bound is visible rather than assumed.
struct DmaMapCache {
    size_t bytes = 0;
    size_t max_bytes = 0;
    size_t refreshes = 0;
    unsigned samples_since_refresh = 0;
    bool ever_ran = false;
};

// Reads a sample, running the mapping and GPU passes only when their caches say
// they are worth running. `peak_total_bytes` is the largest total seen so far;
// pass null caches to force both passes (which is what a one-shot read wants).
ObservedMemSample ReadObservedMemoryGated(
        DmaInodeSet* set, DmaMapCache* map_cache, GpuMmapCache* gpu_cache,
        size_t peak_total_bytes);

// Sum of the distinct dmabuf sizes this process holds, measured the way an
// external evaluator measures it, with the fd/mapping split written into
// `into`. `set` must have been reset for this pass.
size_t ReadSelfDmaBytes(DmaInodeSet* set, ObservedMemSample* into);
size_t ReadSelfDmaBytesGated(
        DmaInodeSet* set, ObservedMemSample* into, DmaMapCache* map_cache,
        size_t peak_total_bytes);

// How much the peak has to grow before the snapshot is refreshed again.
//
// Shared by both peak criteria so there is one policy, not two. A fixed step
// means a process whose peak never grows by another whole step keeps its first,
// much lower snapshot. Scaling to the peak reached so far keeps the snapshot
// within ~20% of the peak on a small run, while on a large peak it still
// evaluates to the configured step so snapshot cost there is unchanged.
size_t NextPeakThreshold(size_t peak_bytes, size_t step_bytes);

// Default and floor for the step above, in bytes.
size_t DefaultPeakStepBytes();
size_t MinPeakStepBytes();

struct ObservedSamplerStats {
    unsigned interval_ms = 0;
    DmaSource dma_source = DmaSource::Unprobed;
    GpuMmapSource gpu_source = GpuMmapSource::Unprobed;
    size_t samples = 0;
    size_t valid_samples = 0;
    size_t snapshots = 0;
    // Cost of reading /proc, excluding the snapshot the reading may trigger, so
    // the two can be judged separately. A sampler whose reads approach the
    // requested interval is perturbing the run it is measuring.
    uint64_t total_sample_us = 0;
    uint64_t max_sample_us = 0;
    uint64_t total_snapshot_us = 0;
    uint64_t max_snapshot_us = 0;
    uint64_t total_status_us = 0;
    uint64_t total_fd_us = 0;
    uint64_t total_map_us = 0;
    uint64_t total_gpu_us = 0;
    // How often the mapping pass actually ran, and the most it ever added, so
    // the gate above can be judged rather than trusted.
    size_t map_passes = 0;
    size_t max_map_only_bytes = 0;
    // Largest GPU figure any sample reported, and what the run concluded about
    // whether VmRSS counts these mappings. The latter is reported because the
    // figure means different things under the two answers, and "Undetermined"
    // says the run never saw the mapped total grow.
    size_t max_gpu_bytes_seen = 0;
    // How many smaps reads the run paid. The only cost this dimension can incur
    // beyond a maps read, so it is reported rather than assumed small.
    size_t gpu_reads = 0;
    // Attempted reads that failed. A carried figure stays labelled as the smaps
    // measurement it is, so this is the only place a failure becomes visible.
    size_t gpu_read_failures = 0;
    // Wall time from the first sample to the last, so the cadence actually
    // achieved is visible next to the one requested.
    uint64_t span_us = 0;
    // Grid slots the sampler never served. Non-zero means the achieved cadence
    // below is not the requested one, and says so directly rather than leaving
    // it to be inferred from the mean.
    uint64_t skipped_slots = 0;
    // Samples after which the duty cap, rather than the requested interval,
    // set the wait. Separates "too expensive to sample this fast" from "the
    // scheduler did not wake us on time".
    uint64_t throttled_samples = 0;
    bool dedup_overflowed = false;
    // Maximum of (rss + dma + gpu) over the run, with every part as it stood at
    // that instant.
    size_t peak_total_bytes = 0;
    size_t peak_total_rss_bytes = 0;
    size_t peak_total_dma_bytes = 0;
    // The dmabuf split at that same instant.
    size_t peak_total_dma_fd_bytes = 0;
    size_t peak_total_dma_map_bytes = 0;
    size_t peak_total_gpu_bytes = 0;
    // Independent maxima, so each can be compared against the matching figure
    // an external sampler reports.
    size_t max_rss_bytes = 0;
    size_t max_dma_bytes = 0;
    size_t max_gpu_bytes = 0;
};

// Samples the evaluator-visible footprint on its own thread and asks for a
// stack snapshot whenever that footprint reaches a new maximum.
//
// Deliberately not driven from the allocation path: the footprint changes
// without any allocator call at all (a page faulting in, a driver mapping a
// buffer), which is exactly why the allocation-path criterion misses the real
// peak.
class ObservedPeakSampler {
public:
    // Returns whether the callee retained a snapshot for this sample. A
    // first-crossing run is only finished once one was actually retained.
    using PeakCallback = bool (*)(const ObservedMemSample&);

    // Constant-initialised, so the process-wide instance below needs no dynamic
    // initialisation and is safe to reach from the interposition path.
    ObservedPeakSampler() = default;

    // `interval_ms` == 0 disables sampling and returns false. `floor_bytes` is
    // the observed total a run must reach before the first snapshot;
    // `step_bytes` bounds how often the snapshot is refreshed afterwards.
    // `one_shot` keeps only the first crossing of the floor: the snapshot is
    // never refreshed, and `step_bytes` is unused. Sampling continues either
    // way, so the observed-peak statistics stay complete.
    //
    // A null `on_new_peak` starts the sampler with no tracker behind it: it keeps
    // its statistics and never asks for a snapshot, so the floor, the step and
    // `one_shot` are all inert. That is the mode the observe-only probe uses when
    // no report is configured (see ObserveOnlyProbe.h).
    bool Start(
            unsigned interval_ms, size_t floor_bytes, size_t step_bytes,
            bool one_shot, PeakCallback on_new_peak);
    // Idempotent. Joins the sampler thread, so after it returns the statistics
    // below are stable and no snapshot can be in flight.
    void Stop();
    // Declares the sampler absent without joining. fork() does not clone the
    // sampler thread, so the child inherits started_ == true describing a
    // thread that does not exist; Stop() would then block forever in
    // pthread_join on a stale id. Child-side use only.
    void ResetAfterForkInChild();

    bool running() const { return running_.load(std::memory_order_acquire); }
    bool started() const { return started_.load(std::memory_order_acquire); }

    // True only when the sampler claims to be running but has stopped
    // delivering samples -- a wedged or dead sampler thread.
    //
    // This exists to give the one-way observed-peak latch a liveness fallback.
    // Once the sampler has taken a snapshot it owns the peak criterion, so if it
    // then dies the report would stay pinned to whatever early snapshot it had
    // already taken, with the allocation-path criterion permanently disabled.
    //
    // Deliberately false for a sampler that was never started or that Stop()
    // shut down: finalization stops the sampler *before* the final dump, and
    // treating that as death would let a late allocation overwrite the observed
    // snapshot the run exists to report.
    bool Stalled() const;

    ObservedSamplerStats stats() const;

private:
    static void* Trampoline(void* self);
    void Run();

    std::atomic<bool> started_{false};
    std::atomic<bool> running_{false};
    pthread_t thread_{};
    unsigned interval_ms_{0};
    size_t floor_bytes_{0};
    size_t step_bytes_{0};
    bool one_shot_{false};
    PeakCallback on_new_peak_{nullptr};

    // Written by the sampler thread only, but read by a report that a signal
    // can trigger on another thread while sampling is still running.
    std::atomic<size_t> samples_{0};
    std::atomic<size_t> valid_samples_{0};
    std::atomic<size_t> snapshots_{0};
    std::atomic<uint64_t> total_sample_us_{0};
    std::atomic<uint64_t> max_sample_us_{0};
    std::atomic<uint64_t> total_snapshot_us_{0};
    std::atomic<uint64_t> max_snapshot_us_{0};
    std::atomic<uint64_t> total_status_us_{0};
    std::atomic<uint64_t> total_fd_us_{0};
    std::atomic<uint64_t> total_map_us_{0};
    std::atomic<uint64_t> total_gpu_us_{0};
    std::atomic<uint64_t> skipped_slots_{0};
    std::atomic<uint64_t> throttled_samples_{0};
    std::atomic<uint64_t> first_sample_us_{0};
    std::atomic<uint64_t> last_sample_us_{0};
    std::atomic<size_t> peak_total_bytes_{0};
    std::atomic<size_t> peak_total_rss_bytes_{0};
    std::atomic<size_t> peak_total_dma_bytes_{0};
    std::atomic<size_t> peak_total_dma_fd_bytes_{0};
    std::atomic<size_t> peak_total_dma_map_bytes_{0};
    std::atomic<size_t> peak_total_gpu_bytes_{0};
    std::atomic<size_t> max_rss_bytes_{0};
    std::atomic<size_t> max_dma_bytes_{0};
    std::atomic<size_t> max_gpu_bytes_{0};
    std::atomic<uint8_t> dma_source_{static_cast<uint8_t>(DmaSource::Unprobed)};
    std::atomic<uint8_t> gpu_source_{static_cast<uint8_t>(GpuMmapSource::Unprobed)};
    std::atomic<bool> dedup_overflowed_{false};

    DmaInodeSet inodes_{};
    DmaMapCache map_cache_{};
    GpuMmapCache gpu_cache_{};

    ObservedPeakSampler(const ObservedPeakSampler&) = delete;
    ObservedPeakSampler& operator=(const ObservedPeakSampler&) = delete;
};

// The process-wide sampler. Lives in .bss with no dynamic initialisation, so
// it is safe to reach from the allocator interposition path that runs before
// this library's static constructors would have.
ObservedPeakSampler& ObservedPeakSamplerInstance();
