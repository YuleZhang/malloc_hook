#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "Config.h"
#include "ObserveOnlyProbe.h"
#include "ObservedMemory.h"

extern "C" char** environ;

namespace observe_only {

std::atomic<int> g_mode{kUndecided};

namespace {

// The process the sampler thread belongs to. fork() clones neither that thread
// nor the figures it produced, but the child *does* inherit this library's exit
// handlers -- so the pid is what keeps a child from printing the parent's peaks
// a second time under its own name.
pid_t g_probe_pid = 0;

// The report is reachable from two exit paths that both fire on glibc (see
// StartProbe), so whichever runs first is the one that reports.
std::atomic<bool> g_reported{false};

void WriteLine(int fd, const char* message) {
    const size_t length = strlen(message);
    ssize_t written = write(fd, message, length);
    (void)written;
}

// Geometry of the summary block below. It reproduces the box a host framework
// prints for the same three quantities, down to the column widths, so the two
// summaries can be read side by side in one log without re-aligning anything by
// eye: 60-column rules, a 36-column centred title, and each row's value right
// aligned so its last digit lands in column 55.
constexpr char kTag[] = "alloc_hook: ";
constexpr int kRuleWidth = 60;
constexpr int kLabelWidth = 33;
constexpr int kValueWidth = 20;
constexpr char kTitle[] = "                Memory Usage Summary";

// Yellow marks the block the way that framework's own summary is marked, which
// is the point of matching it: one colour to look for in a long log.
//
// Emitted only when the destination is a terminal. A redirected log or a
// checkpoint file gets plain text, because an escape sequence in a file is
// noise to every reader and every parser of it.
struct LineStyle {
    const char* on = "";
    const char* off = "";
};

LineStyle StyleFor(int fd) {
    if (isatty(fd) == 1) {
        return LineStyle{"\033[33m", "\033[0m"};
    }
    return LineStyle{};
}

void WriteText(int fd, const LineStyle& style, const char* text) {
    dprintf(fd, "%s%s%s%s\n", style.on, kTag, text, style.off);
}

void WriteRule(int fd, const LineStyle& style, char fill) {
    char rule[kRuleWidth + 1];
    memset(rule, fill, kRuleWidth);
    rule[kRuleWidth] = '\0';
    WriteText(fd, style, rule);
}

void WriteRow(
        int fd, const LineStyle& style, const char* label, const char* value,
        const char* unit) {
    dprintf(fd, "%s%s  %-*s%*s %s%s\n", style.on, kTag, kLabelWidth, label,
            kValueWidth, value, unit, style.off);
}

void WriteMegabyteRow(
        int fd, const LineStyle& style, const char* label, size_t bytes) {
    char value[32];
    snprintf(value, sizeof(value), "%.2f", bytes / 1024.0 / 1024.0);
    WriteRow(fd, style, label, value, "MB");
}

// The kernel's own resident high-water mark, in bytes, or 0 where it is not
// reported.
//
// Printed next to the sampled RSS maximum because it is the one figure here that
// no sampling interval can miss: if it stands well above "RSS Max (sampling)",
// the process had a resident peak between two samples and the sampled column is
// understating it. The two are still not the same measurement -- this one is a
// mark the kernel keeps for the whole process lifetime, including this library's
// own footprint.
size_t ReadMaxRssBytes() {
    struct rusage usage = {};
    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss <= 0) {
        return 0;
    }
    return static_cast<size_t>(usage.ru_maxrss) * 1024;
}

// The observed figures: a human summary in the host framework's own shape,
// followed by the machine-readable lines the full report uses, so one pattern
// finds either mode's numbers.
void WriteObservedBlock(int fd, const char* reason) {
    const ObservedSamplerStats sampler = ObservedPeakSamplerInstance().stats();
    const LineStyle style = StyleFor(fd);
    if (sampler.samples == 0) {
        // Never silent: a probe that produced nothing has to say so, otherwise
        // "no output" reads as "this process held no memory".
        dprintf(fd,
                "%s%sobserve_only probe (%s): interval_ms=%u samples=0, the "
                "sampler never delivered a sample%s\n",
                style.on, kTag, reason, sampler.interval_ms, style.off);
        return;
    }

    WriteRule(fd, style, '=');
    WriteText(fd, style, kTitle);
    WriteRule(fd, style, '-');
    // The three parts, each at its own maximum over the run. They are not
    // required to have peaked together, which is why the combined row below is a
    // separate measurement and not their sum.
    WriteMegabyteRow(fd, style, "DMA Max (sampling):", sampler.max_dma_bytes);
    WriteMegabyteRow(fd, style, "RSS Max (sampling):", sampler.max_rss_bytes);
    WriteMegabyteRow(fd, style, "GPU mmap Max (sampling):", sampler.max_gpu_bytes);
    const size_t max_rss_bytes = ReadMaxRssBytes();
    if (max_rss_bytes != 0) {
        WriteMegabyteRow(fd, style, "RSS Max (getrusage):", max_rss_bytes);
    }
    // The largest same-cycle sum: every part as it stood in one sampling cycle,
    // which is the quantity an external evaluator reports as the process peak.
    WriteMegabyteRow(
            fd, style, "DMA+RSS+GPU mmap Max (sampling):", sampler.peak_total_bytes);
    // A part with no reachable interface reads 0.00 above, and that is not a
    // measurement of zero. Say which ones, rather than letting the box imply it.
    char unmeasured[64];
    unmeasured[0] = '\0';
    if (sampler.dma_source == DmaSource::None ||
        sampler.dma_source == DmaSource::Unprobed) {
        strncat(unmeasured, " dma", sizeof(unmeasured) - strlen(unmeasured) - 1);
    }
    if (sampler.gpu_source == GpuMmapSource::NotApplicable ||
        sampler.gpu_source == GpuMmapSource::Unprobed) {
        strncat(unmeasured, " gpu", sizeof(unmeasured) - strlen(unmeasured) - 1);
    }
    if (unmeasured[0] != '\0') {
        char note[96];
        snprintf(
                note, sizeof(note), "  not measured, so not a zero:%s", unmeasured);
        WriteText(fd, style, note);
    }
    WriteRule(fd, style, '-');
    char period[32];
    snprintf(period, sizeof(period), "%u", sampler.interval_ms);
    WriteRow(fd, style, "Sampling Period:", period, "ms");
    // What the sampler actually achieved. Above the requested period means the
    // /proc reads cost more than the interval and the sampler throttled itself
    // to stay under half a core; it never claims a cadence it did not reach.
    if (sampler.samples > 1) {
        char achieved[32];
        snprintf(
                achieved, sizeof(achieved), "%.2f",
                sampler.span_us / 1000.0 / (sampler.samples - 1));
        WriteRow(fd, style, "Achieved Period:", achieved, "ms");
    }
    WriteRule(fd, style, '=');

    dprintf(fd,
            "%s%sobserve_only probe (%s): no allocation tracking, no stacks%s\n",
            style.on, kTag, reason, style.off);
    // The peak instant's own composition, which the independent maxima above
    // cannot give: these are the parts as they stood in the one cycle that
    // produced the combined maximum.
    dprintf(fd,
            "%s%sobserved_peak(max_of_sum): rss=%fMB dma=%fMB gpu=%fMB total=%fMB "
            "(dma_fd=%fMB dma_map=%fMB)%s\n",
            style.on, kTag, sampler.peak_total_rss_bytes / 1024.0 / 1024.0,
            sampler.peak_total_dma_bytes / 1024.0 / 1024.0,
            sampler.peak_total_gpu_bytes / 1024.0 / 1024.0,
            sampler.peak_total_bytes / 1024.0 / 1024.0,
            sampler.peak_total_dma_fd_bytes / 1024.0 / 1024.0,
            sampler.peak_total_dma_map_bytes / 1024.0 / 1024.0, style.off);
    dprintf(fd,
            "%s%sobserved_sampler: interval_ms=%u achieved_ms=%f dma_source=%s "
            "gpu_source=%s samples=%zu valid=%zu sample_mean_us=%llu "
            "sample_max_us=%llu map_passes=%zu gpu_reads=%zu gpu_read_failures=%zu "
            "skipped_slots=%llu throttled_samples=%llu%s\n",
            style.on, kTag, sampler.interval_ms,
            sampler.samples > 1
                    ? sampler.span_us / 1000.0 / (sampler.samples - 1)
                    : 0.0,
            DmaSourceName(sampler.dma_source),
            GpuMmapSourceName(sampler.gpu_source), sampler.samples,
            sampler.valid_samples,
            static_cast<unsigned long long>(
                    sampler.total_sample_us / sampler.samples),
            static_cast<unsigned long long>(sampler.max_sample_us),
            sampler.map_passes, sampler.gpu_reads, sampler.gpu_read_failures,
            static_cast<unsigned long long>(sampler.skipped_slots),
            static_cast<unsigned long long>(sampler.throttled_samples), style.off);
}

}  // namespace

int ResolveMode() {
    // The loader publishes `environ` before it runs any constructor, but an
    // allocation can reach this earlier still. Staying undecided costs one
    // getenv on the next allocation; latching kTrack from an empty environment
    // would cost the run the mode it asked for.
    if (environ == nullptr) {
        return kUndecided;
    }
    // The environment parse below must be invisible to the caller. POSIX
    // requires free() to preserve errno, and the first free() in a process can
    // be the call that resolves this -- a caller that frees a buffer before
    // perror() would otherwise report this getenv's errno instead of its own.
    const int saved_errno = errno;
    unsigned interval_ms = 0;
    const int mode = Config::ObserveOnlyRequested(&interval_ms) ? kBypass : kTrack;
    g_mode.store(mode, std::memory_order_relaxed);
    errno = saved_errno;
    return mode;
}

bool StartProbe() {
    unsigned interval_ms = 0;
    if (!Bypassed() || !Config::ObserveOnlyRequested(&interval_ms)) {
        return false;
    }
    g_probe_pid = getpid();

    // A deployment script sends the checkpoint signal without knowing which mode
    // the hook ended up in. There is no live allocation table to dump here, and
    // the real-time signal this library uses terminates a process that has no
    // handler for it -- so it is ignored rather than left to kill a run that was
    // only being measured. The exported checkpoint() still answers on demand.
    struct sigaction ignore_act = {};
    ignore_act.sa_handler = SIG_IGN;
    sigemptyset(&ignore_act.sa_mask);
    sigaction(Config::DumpSignal(), &ignore_act, nullptr);

    // A null callback is what makes this observe-only: the sampler keeps its
    // statistics and never asks anything to snapshot stacks, so the floor, the
    // step and the retention mode have nothing to act on.
    if (!ObservedPeakSamplerInstance().Start(
                interval_ms, /*floor_bytes=*/0, /*step_bytes=*/0,
                /*one_shot=*/false, /*on_new_peak=*/nullptr)) {
        WriteLine(STDERR_FILENO,
                  "alloc_hook: observe_only probe requested but the memory "
                  "sampler could not start; this run measures nothing\n");
        g_probe_pid = 0;
        return false;
    }

    // Registered here rather than relying on this library's .fini_array alone:
    // Bionic runs a preloaded library's destructors on dlclose, not at process
    // exit, so on Android the destructor never fires and the run would measure
    // the whole process and then report nothing. atexit() fires on all three
    // supported libcs -- which is also why the tracker's own exit report has
    // always come from a static destructor (__cxa_atexit) rather than from
    // .fini_array. A process that leaves through _exit() or a fatal signal runs
    // neither, exactly as it produces no tracked report today.
    atexit(&ReportAtExit);
    return true;
}

void ReportAtExit() {
    if (g_probe_pid == 0 || getpid() != g_probe_pid) {
        // Either the probe never started here, or this is a fork child: it
        // inherited this handler but not the sampler thread, so the figures it
        // would print are the parent's.
        return;
    }
    if (g_reported.exchange(true)) {
        return;
    }
    // Joins, so the statistics are stable by the time they are formatted.
    ObservedPeakSamplerInstance().Stop();
    WriteObservedBlock(STDERR_FILENO, "at_exit");
}

bool WriteReport(const char* file_name) {
    if (file_name == nullptr) {
        WriteObservedBlock(STDERR_FILENO, "checkpoint");
        return true;
    }
    const int fd =
            open(file_name, O_RDWR | O_CREAT | O_NOFOLLOW | O_TRUNC | O_CLOEXEC, 0644);
    if (fd == -1) {
        // Same contract as the full report: a missing file must never be
        // indistinguishable from a probe that measured nothing.
        dprintf(STDERR_FILENO,
                "alloc_hook: cannot write observe_only report to %s: %s\n",
                file_name, strerror(errno));
        WriteObservedBlock(STDERR_FILENO, "checkpoint");
        return false;
    }
    WriteObservedBlock(fd, "checkpoint");
    close(fd);
    return true;
}

}  // namespace observe_only
