// Covers the evaluator-aligned peak criterion: the /proc readers it is built
// on, the dedup that keeps a buffer from being counted once per reference, the
// snapshot-frequency policy, the interval it adopts from its environment, and
// the sampler thread end to end.
//
// The device-only formats (dmabuf mappings, process-wide ION accounting) are
// exercised against text captured verbatim from real devices, because a host
// test runs on a kernel that emits neither.

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Config.h"
#include "ObservedMemory.h"

namespace {

DmaInodeSet g_set;

size_t MapsLineBytes(const char* text, bool* saw_any) {
    char line[512];
    snprintf(line, sizeof(line), "%s", text);
    return DmaBytesFromMapsLine(line, &g_set, saw_any);
}

std::string WriteTempFile(const char* name, const char* contents) {
    std::string path = std::string("/tmp/") + name + "." + std::to_string(getpid());
    FILE* file = fopen(path.c_str(), "w");
    assert(file != nullptr);
    fputs(contents, file);
    fclose(file);
    return path;
}

std::atomic<int> g_callbacks{0};
std::atomic<size_t> g_last_total{0};

void OnPeak(const ObservedMemSample& sample) {
    g_callbacks.fetch_add(1);
    g_last_total.store(sample.total());
}

void TestStatusFieldParsing() {
    const char* status =
            "Name:\tprobe\nVmRSS:\t  123456 kB\nRssAnon:\t100000 kB\n"
            "RssFile:\t23456 kB\nRssShmem:\t0 kB\n";
    assert(ReadStatusField(status, "VmRSS:") == 123456);
    assert(ReadStatusField(status, "RssAnon:") == 100000);
    assert(ReadStatusField(status, "RssFile:") == 23456);
    assert(ReadStatusField(status, "RssShmem:") == 0);
    // A key this kernel does not publish must read as absent, not as garbage
    // from the next line.
    assert(ReadStatusField(status, "HugetlbPages:") == 0);
}

void TestSelfRss() {
    const RssBreakdown rss = ReadSelfRss();
    // Every kernel this hook targets publishes VmRSS, and a live process always
    // has some.
    assert(rss.valid);
    assert(rss.vm_rss_kb > 0);
    assert(rss.anon_kb <= rss.vm_rss_kb);
}

void TestMapsDmaBufAccounting() {
    ResetDmaInodeSet(&g_set);
    bool saw_any = false;

    // Verbatim from an Android device: a dmabuf mapping names the buffer and
    // carries its inode in the sixth field.
    assert(MapsLineBytes(
                   "756aa19000-756aa20000 rw-s 00000000 00:0a 519             "
                   "               /dmabuf:qcom,system",
                   &saw_any) == 0x7000);
    assert(saw_any);

    // A second mapping of the same buffer must add nothing: an evaluator counts
    // each buffer once, and double counting here would move the apparent peak
    // instant.
    assert(MapsLineBytes(
                   "756f40c000-756f413000 rw-s 00000000 00:0a 519             "
                   "               /dmabuf:qcom,system",
                   &saw_any) == 0);

    // A different buffer does count.
    assert(MapsLineBytes(
                   "756f40c000-756f415000 rw-s 00000000 00:0a 748             "
                   "               /dmabuf:qcom,system",
                   &saw_any) == 0x9000);

    // A file-backed library whose *name* contains "dmabuf" is not a buffer.
    // Rejecting it by name alone would silently inflate the DMA figure by the
    // size of every vendor mapping that happens to be spelled this way.
    bool library_saw_any = false;
    assert(MapsLineBytes(
                   "72cba64000-72cba6c000 r--p 00000000 fe:0b 41709179        "
                   "               /vendor/lib64/libdmabufheap.so",
                   &library_saw_any) == 0);
    assert(!library_saw_any);

    // Anonymous and malformed lines contribute nothing.
    assert(MapsLineBytes("7f0000000000-7f0000001000 rw-p 00000000 00:00 0",
                        &saw_any) == 0);
    assert(MapsLineBytes("garbage /dmabuf:x", &saw_any) == 0);

    // Resetting starts a fresh pass, so the same buffer is counted again. If it
    // were not, a sampler reusing the set would report a falling DMA total
    // after the first sample.
    ResetDmaInodeSet(&g_set);
    bool second_pass = false;
    assert(MapsLineBytes(
                   "756aa19000-756aa20000 rw-s 00000000 00:0a 519             "
                   "               /dmabuf:qcom,system",
                   &second_pass) == 0x7000);
}

void TestIonProcInfoAccounting() {
    // Verbatim shape of the process-wide ION file: only this process's rows
    // count, and a process holding several buffers sums them.
    const std::string path = WriteTempFile(
            "observed_ion_probe",
            "Process ION heap info:\n"
            "----------------------------------------------------\n"
            "otherproc 973 29 3145728 magic\n"
            "selfproc 4242 31 1048576 magic\n"
            "selfproc 4242 32 2097152 magic\n"
            "otherproc 555 7 8388608 magic\n");
    size_t bytes = 0;
    assert(ReadIonProcInfoBytesFrom(path.c_str(), 4242, &bytes));
    assert(bytes == 1048576 + 2097152);

    // A pid holding nothing reads as zero, not as a failure: "present and
    // empty" is a different fact from "no such interface".
    bytes = 12345;
    assert(ReadIonProcInfoBytesFrom(path.c_str(), 999999, &bytes));
    assert(bytes == 0);

    // A missing file must fail so the caller falls through to the descriptor
    // route instead of reporting 0 bytes of device memory.
    assert(!ReadIonProcInfoBytesFrom("/nonexistent/ion_process_info", 1, &bytes));
    unlink(path.c_str());
}

void TestObservedSample() {
    ResetDmaInodeSet(&g_set);
    const ObservedMemSample sample = ReadObservedMemory(&g_set);
    assert(sample.valid);
    assert(sample.rss_bytes > 0);
    assert(sample.total() >= sample.rss_bytes);
    // A host kernel exposes no dmabuf to this process, and that has to be
    // reported as "no such accounting" rather than as a measured zero.
    assert(sample.dma_source == DmaSource::None ||
           sample.dma_source == DmaSource::FdAndMaps);
    if (sample.dma_source == DmaSource::None) {
        assert(sample.dma_bytes == 0);
    }
}

void TestPeakThresholdPolicy() {
    // Step 0 means refresh on every new peak.
    assert(NextPeakThreshold(100 * 1024 * 1024, 0) == 100 * 1024 * 1024);

    // A large peak is bounded by the configured step, so snapshot cost on a big
    // run is unchanged by the scaling.
    const size_t step = DefaultPeakStepBytes();
    const size_t large = 4000ULL * 1024 * 1024;
    assert(NextPeakThreshold(large, step) == large + step);

    // A small peak scales instead, so a run whose peak never grows by a whole
    // step still refreshes its snapshot.
    const size_t small = 8 * 1024 * 1024;
    assert(NextPeakThreshold(small, step) == small + small / 4);

    // Below the floor the scaling stops, so a tiny peak cannot make the
    // snapshot refresh on essentially every sample.
    assert(NextPeakThreshold(1024, step) == 1024 + MinPeakStepBytes());
}

void TestIntervalResolution() {
    unsetenv("ALLOC_HOOK_PEAK_SAMPLE_MS");
    unsetenv("PROBE_AUTO_SHOW_MEM_USE_DURATION_MS");
    unsetenv("DUMP_PEAK_STEP_MB");
    Config config;
    bool initialized = config.Init();
    assert(initialized);
    // Nothing is sampling this process, so there is no external instant to
    // align with and the sampler stays off.
    assert(config.observed_peak_sample_ms() == 0);
    assert(config.peak_record_step_bytes() == DefaultPeakStepBytes());

    // A host framework's interval is adopted automatically, matched by the
    // suffix of the variable name rather than by any particular prefix.
    setenv("PROBE_AUTO_SHOW_MEM_USE_DURATION_MS", "1", 1);
    initialized = config.Init();
    assert(initialized);
    assert(config.observed_peak_sample_ms() == 1);

    // That framework's "not sampling" value must not switch the sampler on.
    setenv("PROBE_AUTO_SHOW_MEM_USE_DURATION_MS", "0", 1);
    initialized = config.Init();
    assert(initialized);
    assert(config.observed_peak_sample_ms() == 0);

    // An explicit interval wins over the adopted one.
    setenv("PROBE_AUTO_SHOW_MEM_USE_DURATION_MS", "5", 1);
    setenv("ALLOC_HOOK_PEAK_SAMPLE_MS", "20", 1);
    initialized = config.Init();
    assert(initialized);
    assert(config.observed_peak_sample_ms() == 20);

    // Including an explicit 0, which has to be able to turn the sampler off
    // even while a framework is sampling.
    setenv("ALLOC_HOOK_PEAK_SAMPLE_MS", "0", 1);
    initialized = config.Init();
    assert(initialized);
    assert(config.observed_peak_sample_ms() == 0);

    setenv("DUMP_PEAK_STEP_MB", "3", 1);
    initialized = config.Init();
    assert(initialized);
    assert(config.peak_record_step_bytes() == 3ULL * 1024 * 1024);
    (void)initialized;

    unsetenv("ALLOC_HOOK_PEAK_SAMPLE_MS");
    unsetenv("PROBE_AUTO_SHOW_MEM_USE_DURATION_MS");
    unsetenv("DUMP_PEAK_STEP_MB");
}

void TestSamplerLifecycle() {
    ObservedPeakSampler& sampler = ObservedPeakSamplerInstance();
    // A disabled interval must not start a thread.
    assert(!sampler.Start(0, 0, DefaultPeakStepBytes(), &OnPeak));
    assert(!sampler.started());

    // Floor 0 means the first sample already qualifies as a peak, so one
    // callback is guaranteed without depending on the process growing.
    assert(sampler.Start(1, 0, DefaultPeakStepBytes(), &OnPeak));
    assert(sampler.started());
    for (int waited_ms = 0; waited_ms < 5000 && g_callbacks.load() == 0;
         waited_ms += 10) {
        usleep(10000);
    }
    sampler.Stop();
    assert(!sampler.started());

    // Stop() joins, so everything below is stable.
    assert(g_callbacks.load() >= 1);
    assert(g_last_total.load() > 0);
    const ObservedSamplerStats stats = sampler.stats();
    assert(stats.interval_ms == 1);
    assert(stats.samples >= 1);
    assert(stats.valid_samples >= 1);
    assert(stats.snapshots >= 1);
    assert(stats.peak_total_bytes > 0);
    assert(stats.peak_total_bytes ==
           stats.peak_total_rss_bytes + stats.peak_total_dma_bytes);
    // The maximum of the sum can only be reached with each half at or below its
    // own maximum.
    assert(stats.peak_total_rss_bytes <= stats.max_rss_bytes);
    assert(stats.peak_total_dma_bytes <= stats.max_dma_bytes);
    assert(!stats.dedup_overflowed);

    // Stop() is idempotent; finalization calls it on paths that may never have
    // started a sampler.
    sampler.Stop();
}

void TestMapPassGate() {
    // A null cache forces the mapping pass, which is the contract every
    // one-shot read and every check above relies on.
    ResetDmaInodeSet(&g_set);
    ObservedMemSample forced;
    forced.rss_bytes = 1;
    ReadSelfDmaBytesGated(&g_set, &forced, nullptr, 0);

    DmaMapCache cache;
    // First gated read must run the pass: nothing is carried yet, so skipping
    // would report a DMA figure of zero for a process that has buffers.
    ResetDmaInodeSet(&g_set);
    ObservedMemSample first;
    first.rss_bytes = 1;
    ReadSelfDmaBytesGated(&g_set, &first, &cache, 0);
    assert(cache.ever_ran);
    assert(cache.refreshes == 1);
    assert(cache.samples_since_refresh == 0);

    // A sample far below the peak skips the pass and carries the last figure
    // forward, so the reported total never drops just because the pass was
    // skipped.
    const size_t high_peak = static_cast<size_t>(1) << 40;
    ResetDmaInodeSet(&g_set);
    ObservedMemSample skipped;
    skipped.rss_bytes = 1;
    ReadSelfDmaBytesGated(&g_set, &skipped, &cache, high_peak);
    assert(cache.refreshes == 1);
    assert(cache.samples_since_refresh == 1);
    assert(skipped.dma_map_bytes == cache.bytes);

    // A sample at or above the peak must run the pass: this is the sample whose
    // total decides the snapshot instant, so it cannot be the one measured with
    // a stale figure.
    ResetDmaInodeSet(&g_set);
    ObservedMemSample candidate;
    candidate.rss_bytes = 1;
    ReadSelfDmaBytesGated(&g_set, &candidate, &cache, 0);
    assert(cache.refreshes == 2);
    assert(cache.samples_since_refresh == 0);

    // The carried figure is refreshed on its own after a bounded number of
    // skips, so it cannot go stale for the whole run on a process that stays
    // below its peak.
    size_t before = cache.refreshes;
    for (int i = 0; i < 32; ++i) {
        ResetDmaInodeSet(&g_set);
        ObservedMemSample sample;
        sample.rss_bytes = 1;
        ReadSelfDmaBytesGated(&g_set, &sample, &cache, high_peak);
    }
    assert(cache.refreshes > before);
    // ...and far less often than every sample, which is the point.
    assert(cache.refreshes - before <= 8);
}

void TestGpuMmapAccounting() {
    // Verbatim region shape from a Qualcomm/Adreno device: the OpenCL driver
    // mmaps device memory from the kgsl node, and the kernel reports Rss 0 for it
    // because the pages are PFN/IO mapped and have no struct page to account. Such
    // a region is in neither VmRSS nor any dmabuf interface, so its whole size is
    // the part this dimension exists to recover.
    char pfn_mapped[] =
            "7a1c000000-7a1c800000 rw-s 00000000 00:0d 12345      /dev/kgsl-3d0\n"
            "Size:               8192 kB\n"
            "Rss:                   0 kB\n"
            "Pss:                   0 kB\n"
            "VmFlags: rd wr sh mr mw me ms io de dd\n";
    assert(GpuMmapBytesFromSmapsText(pfn_mapped) == 8u * 1024 * 1024);

    // A cacheable kgsl heap is backed by normal pages, so those pages are already
    // inside rss_bytes. Only the unaccounted remainder may be added -- summing the
    // region's plain size here would count them twice and inflate the total the
    // snapshot instant is chosen on.
    char partly_resident[] =
            "7a2c000000-7a2c400000 rw-s 00000000 00:0d 12345      /dev/kgsl-3d0\n"
            "Size:               4096 kB\n"
            "Rss:                1024 kB\n";
    assert(GpuMmapBytesFromSmapsText(partly_resident) == 3u * 1024 * 1024);

    // Fully resident: nothing to add, and in particular not a negative that would
    // wrap a size_t.
    char fully_resident[] =
            "7a3c000000-7a3c400000 rw-s 00000000 00:0d 12345      /dev/kgsl-3d0\n"
            "Rss:                4096 kB\n";
    assert(GpuMmapBytesFromSmapsText(fully_resident) == 0);
    char over_resident[] =
            "7a4c000000-7a4c400000 rw-s 00000000 00:0d 12345      /dev/kgsl-3d0\n"
            "Rss:                8192 kB\n";
    assert(GpuMmapBytesFromSmapsText(over_resident) == 0);

    // Several device regions sum. Unlike dmabufs these must *not* be deduplicated:
    // every mapping of a character device shares one inode, so keying on inode
    // would collapse a driver's whole working set into its first region.
    char two_regions[] =
            "7a1c000000-7a1c800000 rw-s 00000000 00:0d 12345      /dev/kgsl-3d0\n"
            "Rss:                   0 kB\n"
            "7a2c000000-7a2c800000 rw-s 00000000 00:0d 12345      /dev/kgsl-3d0\n"
            "Rss:                   0 kB\n";
    assert(GpuMmapBytesFromSmapsText(two_regions) == 16u * 1024 * 1024);

    // Only the first Rss line of a region counts. Pss/Private_Dirty and the rest
    // follow it and must not each be taken for another region's residency.
    char many_fields[] =
            "7a1c000000-7a1c800000 rw-s 00000000 00:0d 12345      /dev/kgsl-3d0\n"
            "Rss:                   0 kB\n"
            "Pss:                   0 kB\n"
            "Private_Dirty:         0 kB\n";
    assert(GpuMmapBytesFromSmapsText(many_fields) == 8u * 1024 * 1024);

    // A Mali device mapping is deliberately not counted. Mali reserves one very
    // large sparse range -- 512 MB of address space here -- whose mapped size has
    // no relation to the pages backed, and its real working buffers are dmabufs
    // that dma_bytes already holds. Counting it would add half a gigabyte of
    // fiction to every sample on every Mali device.
    char mali[] =
            "7000000000-7020000000 rw-s 00000000 00:0e 4242       /dev/mali0\n"
            "Rss:                   0 kB\n";
    assert(GpuMmapBytesFromSmapsText(mali) == 0);

    // Ordinary anonymous and file-backed regions contribute nothing.
    char ordinary[] =
            "5566000000-5566100000 rw-p 00000000 00:00 0 \n"
            "Rss:                1024 kB\n"
            "72cba64000-72cba6c000 r--p 00000000 fe:0b 41709179 /vendor/lib64/"
            "libkgsl_helper.so\n"
            "Rss:                  32 kB\n";
    assert(GpuMmapBytesFromSmapsText(ordinary) == 0);

    // The case that forces the "/dev/" prefix in the needle: a file mapping under
    // a *directory* named kgsl. Matching on "kgsl" alone accepts it, and because
    // it is file-backed with only part of it resident, the non-resident remainder
    // would be added as if it were unaccounted device memory -- a silent inflation
    // of the quantity the snapshot instant is chosen on. The device node is always
    // under /dev, so requiring that prefix rejects this and costs nothing.
    char kgsl_named_directory[] =
            "72cba64000-72cbb64000 r--p 00000000 fe:0b 41709180 /vendor/lib64/kgsl/"
            "libfoo.so\n"
            "Rss:                  64 kB\n";
    assert(GpuMmapBytesFromSmapsText(kgsl_named_directory) == 0);

    // The node's "(deleted)" spelling must still be counted: the kernel appends
    // that when the node has been unlinked, and the mapping is just as resident.
    char deleted_node[] =
            "7a5c000000-7a5c800000 rw-s 00000000 00:0d 12345      /dev/kgsl-3d0 "
            "(deleted)\n"
            "Rss:                   0 kB\n";
    assert(GpuMmapBytesFromSmapsText(deleted_node) == 8u * 1024 * 1024);

    // Empty and header-only text must not fault or invent a figure.
    char empty[] = "";
    assert(GpuMmapBytesFromSmapsText(empty) == 0);
    assert(GpuMmapBytesFromSmapsText(nullptr) == 0);
    char header_only[] =
            "7a1c000000-7a1c800000 rw-s 00000000 00:0d 12345      /dev/kgsl-3d0\n";
    assert(GpuMmapBytesFromSmapsText(header_only) == 0);

    // A region's header and its Rss line arrive in different reads whenever the
    // file reader's buffer refills between them, so the scan state has to survive
    // the boundary. Feeding the two lines as separate calls is exactly what the
    // streaming reader does across a refill.
    GpuRegionScan scan;
    char split_header[] =
            "7a1c000000-7a1c800000 rw-s 00000000 00:0d 12345      /dev/kgsl-3d0";
    char split_rss[] = "Rss:                   0 kB";
    assert(GpuMmapBytesFromSmapsLine(split_header, &scan) == 0);
    assert(GpuMmapBytesFromSmapsLine(split_rss, &scan) == 8u * 1024 * 1024);
    // And a fresh scan must not credit an Rss line to a region it never saw the
    // header of, which is what a dropped over-long line leaves behind.
    GpuRegionScan orphaned;
    char orphan_rss[] = "Rss:                8192 kB";
    assert(GpuMmapBytesFromSmapsLine(orphan_rss, &orphaned) == 0);
}

void TestGpuPassGate() {
    // On a host with no such device node the pass must report itself structurally
    // inapplicable and, critically, never open /proc/self/smaps: the per-region
    // name filter can only reject regions the kernel has already walked page
    // tables to produce, so a "no match" answer there still costs the whole walk.
    ObservedMemSample sample;
    sample.rss_bytes = 1;
    GpuMmapCache cache;
    const size_t bytes = ReadSelfGpuMmapBytesGated(&sample, &cache, 0);
    if (sample.gpu_source == GpuMmapSource::NotApplicable) {
        assert(bytes == 0);
        assert(sample.gpu_bytes == 0);
        assert(cache.probed_absent);
        assert(cache.refreshes == 0);
        // Once probed absent the pass is disabled for the run, so a later sample
        // costs nothing at all rather than one access() each.
        ObservedMemSample again;
        again.rss_bytes = 1;
        assert(ReadSelfGpuMmapBytesGated(&again, &cache, 0) == 0);
        assert(cache.refreshes == 0);
        return;
    }

    // On a machine that does have the node, the gate must behave like the mapping
    // pass: skip samples that cannot become the snapshot instant, refresh the ones
    // that can, and never let the carried figure go stale for a whole run.
    assert(cache.ever_ran);
    assert(cache.refreshes == 1);
    const size_t high_peak = static_cast<size_t>(1) << 60;
    ObservedMemSample skipped;
    skipped.rss_bytes = 1;
    ReadSelfGpuMmapBytesGated(&skipped, &cache, high_peak);
    assert(cache.refreshes == 1);
    assert(skipped.gpu_bytes == cache.bytes);

    ObservedMemSample candidate;
    candidate.rss_bytes = 1;
    ReadSelfGpuMmapBytesGated(&candidate, &cache, 0);
    assert(cache.refreshes == 2);
    assert(cache.samples_since_refresh == 0);

    const size_t before = cache.refreshes;
    for (int i = 0; i < 128; ++i) {
        ObservedMemSample s;
        s.rss_bytes = 1;
        ReadSelfGpuMmapBytesGated(&s, &cache, high_peak);
    }
    // Refreshed on its own, but far less often than every sample: this pass is the
    // most expensive of the three and the quantity it reads moves in large steps.
    assert(cache.refreshes > before);
    assert(cache.refreshes - before <= 128 / 32 + 1);
}

}  // namespace


// ---------------------------------------------------------------------------
// Sampler cadence. These pin the properties the fixed-rate scheduler exists to
// provide; a fixed-delay loop (sleep interval *after* the work) fails all of
// them, which is what made the cadence drift with process size.
// ---------------------------------------------------------------------------

void TestCadenceAbsorbsWorkTime() {
    constexpr uint64_t kInterval = 1000;  // 1ms
    // The whole point: a sample that took 400us still advances the deadline by
    // exactly one interval. A fixed-delay loop would produce 1400us here, and
    // that error is what accumulates without bound.
    // Up to half the interval, which is exactly the range in which the duty cap
    // below is satisfiable at the requested cadence.
    for (uint64_t work : {uint64_t{0}, uint64_t{1}, uint64_t{400}, uint64_t{500}}) {
        const uint64_t deadline = 10000;
        const SampleSchedule schedule =
                NextSampleSchedule(deadline, deadline + work, work, kInterval);
        assert(schedule.next_deadline_us == deadline + kInterval);
        assert(schedule.skipped_slots == 0);
        assert(!schedule.throttled);
    }
}

void TestCadenceDoesNotAccumulateDrift() {
    constexpr uint64_t kInterval = 5000;
    constexpr uint64_t kAnchor = 1234567;
    // Work time varies per sample, as it does in a real run. After N samples the
    // grid must be exactly N intervals on from the anchor, with no residue.
    const uint64_t work_pattern[] = {10, 900, 2500, 3, 1200, 1};
    uint64_t deadline = kAnchor;
    unsigned served = 0;
    for (unsigned round = 0; round < 60; ++round) {
        const uint64_t work = work_pattern[round % 6];
        const SampleSchedule schedule =
                NextSampleSchedule(deadline, deadline + work, work, kInterval);
        assert(schedule.skipped_slots == 0);
        deadline = schedule.next_deadline_us;
        ++served;
    }
    assert(deadline == kAnchor + static_cast<uint64_t>(served) * kInterval);
}

void TestSlowSampleSkipsSlotsInsteadOfBursting() {
    constexpr uint64_t kInterval = 1000;
    const uint64_t deadline = 50000;
    // The read took 3.5 intervals. The slots that elapsed are skipped, and the
    // next deadline is in the *future*: firing them back to back would land the
    // sampler's own cost on a process already failing to absorb it.
    const uint64_t work = 3500;
    const uint64_t now = deadline + work;
    const SampleSchedule schedule =
            NextSampleSchedule(deadline, now, work, kInterval);
    assert(schedule.next_deadline_us > now);
    assert(schedule.skipped_slots > 0);
}

void TestSchedulerStarvationIsNotReportedAsThrottling() {
    constexpr uint64_t kInterval = 1000;
    const uint64_t deadline = 50000;
    // A cheap sample -- 100us against a 1ms interval -- that nonetheless
    // finished 3.5 intervals late, because the thread was descheduled rather
    // than because the read was slow. The elapsed slots are still skipped, but
    // the duty cap did not decide this wait, and saying it did would blame the
    // sampler's own cost for a scheduler problem. The two have different fixes,
    // so the report has to tell them apart.
    const uint64_t work = 100;
    const uint64_t now = deadline + 3500;
    const SampleSchedule schedule =
            NextSampleSchedule(deadline, now, work, kInterval);
    assert(schedule.skipped_slots == 3);
    assert(!schedule.throttled);
    assert(schedule.next_deadline_us > now);
}

void TestDutyCapBoundsSamplerCost() {
    constexpr uint64_t kInterval = 1000;
    const uint64_t deadline = 20000;
    // A 25ms read against a 1ms interval: the measured smaps cost on a ~460MB
    // arm64 process. The next sample must not start before the last one's cost
    // has been matched by idle time, so the sampler stays under half a core.
    const uint64_t work = 25000;
    const uint64_t now = deadline + work;
    const SampleSchedule schedule =
            NextSampleSchedule(deadline, now, work, kInterval);
    assert(schedule.throttled);
    assert(schedule.next_deadline_us >= now + work);
    // Still on the original grid, so throttling shifts which slot is served
    // rather than shifting the grid itself.
    assert((schedule.next_deadline_us - deadline) % kInterval == 0);
}

void TestDutyCapBoundaryIsHalfTheInterval() {
    constexpr uint64_t kInterval = 1000;
    const uint64_t deadline = 30000;
    // At exactly half the interval the requested cadence is still met: the
    // sample and the idle time are equal, which is the half-core limit.
    const SampleSchedule at_limit =
            NextSampleSchedule(deadline, deadline + 500, 500, kInterval);
    assert(!at_limit.throttled);
    assert(at_limit.next_deadline_us == deadline + kInterval);
    // One microsecond past it the cadence can no longer be honoured, and the
    // scheduler says so instead of quietly running at half rate.
    const SampleSchedule past_limit =
            NextSampleSchedule(deadline, deadline + 501, 501, kInterval);
    assert(past_limit.throttled);
    assert(past_limit.next_deadline_us == deadline + 2 * kInterval);
}

void TestSubMillisecondWorkIsNotTruncated() {
    // The previous loop computed its throttle from read_us / 1000, so anything
    // under a millisecond truncated to zero. Sub-ms costs must still be visible
    // to the duty cap when the interval is itself sub-ms.
    constexpr uint64_t kInterval = 100;  // 100us
    const uint64_t deadline = 7000;
    const uint64_t work = 250;           // 0.25ms -> would truncate to 0ms
    const uint64_t now = deadline + work;
    const SampleSchedule schedule =
            NextSampleSchedule(deadline, now, work, kInterval);
    assert(schedule.throttled);
    assert(schedule.next_deadline_us >= now + work);
}

void TestZeroIntervalDoesNotDivideByZero() {
    const SampleSchedule schedule = NextSampleSchedule(1000, 1000, 0, 0);
    assert(schedule.next_deadline_us > 1000);
}

int main() {
    TestStatusFieldParsing();
    TestSelfRss();
    TestMapsDmaBufAccounting();
    TestGpuMmapAccounting();
    TestGpuPassGate();
    TestIonProcInfoAccounting();
    TestObservedSample();
    TestMapPassGate();
    TestPeakThresholdPolicy();
    TestIntervalResolution();
    TestSamplerLifecycle();
    TestCadenceAbsorbsWorkTime();
    TestCadenceDoesNotAccumulateDrift();
    TestSlowSampleSkipsSlotsInsteadOfBursting();
    TestSchedulerStarvationIsNotReportedAsThrottling();
    TestDutyCapBoundsSamplerCost();
    TestDutyCapBoundaryIsHalfTheInterval();
    TestSubMillisecondWorkIsNotTruncated();
    TestZeroIntervalDoesNotDivideByZero();
    return 0;
}
