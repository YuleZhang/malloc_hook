// Observe-only probe contract: which environment selects it, what the gate
// latches, and that the sampler runs with no tracker behind it.
//
// The point of the mode is that cost follows output, so the checks below are
// mostly about the *decision*: every environment that produces a report must
// leave tracking on, and every environment that produces none must not pay for
// it.
#include <signal.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Config.h"
#include "ObserveOnlyProbe.h"
#include "ObservedMemory.h"

namespace {

constexpr char kReportPath[] = "observe_only_probe_test_report.txt";
// Any prefix works; the interval is matched by the suffix of the variable name.
constexpr char kExternalIntervalEnv[] = "PROBE_AUTO_SHOW_MEM_USE_DURATION_MS";

void ClearEnv() {
    unsetenv("ALLOC_HOOK_PEAK_SAMPLE_MS");
    unsetenv("DUMP_PEAK_VALUE_MB");
    unsetenv("DUMP_PEAK_STEP_MB");
    unsetenv("BACKTRACE_DUMP_SIGNAL");
    unsetenv(kExternalIntervalEnv);
}

void TestModeSelection() {
    unsigned interval_ms = 12345;
    ClearEnv();
    // Nothing configured at all: the on-demand checkpoint still needs a live
    // allocation table, so this stays in the tracking mode it has always had.
    assert(!Config::ObserveOnlyRequested(&interval_ms));
    assert(interval_ms == 0);

    // The sampler is configured and no report is: nothing consumes tracking.
    setenv("ALLOC_HOOK_PEAK_SAMPLE_MS", "5", 1);
    assert(Config::ObserveOnlyRequested(&interval_ms));
    assert(interval_ms == 5);

    // A report is configured, so tracking has something to produce and the probe
    // must stay out of the way. Either way of asking counts.
    setenv("DUMP_PEAK_VALUE_MB", "64", 1);
    assert(!Config::ObserveOnlyRequested(&interval_ms));
    unsetenv("DUMP_PEAK_VALUE_MB");
    setenv("DUMP_PEAK_STEP_MB", "8", 1);
    assert(!Config::ObserveOnlyRequested(&interval_ms));
    unsetenv("DUMP_PEAK_STEP_MB");

    // 0 is how each of those is turned off, so neither spelling of it asks for a
    // report and both leave the probe selected. This is the pair a deployment
    // reaches for to say "measure, do not attribute".
    setenv("DUMP_PEAK_VALUE_MB", "0", 1);
    assert(Config::ObserveOnlyRequested(&interval_ms));
    assert(interval_ms == 5);
    unsetenv("DUMP_PEAK_VALUE_MB");
    setenv("DUMP_PEAK_STEP_MB", "0", 1);
    assert(Config::ObserveOnlyRequested(&interval_ms));
    assert(interval_ms == 5);
    unsetenv("DUMP_PEAK_STEP_MB");

    // A step without an interval asks for nothing: it only bounds a cadence that
    // was never requested.
    assert(Config::ObserveOnlyRequested(&interval_ms));
    assert(interval_ms == 5);

    // A value Config::Init() cannot parse enables no report there, so it must
    // not count as one here either: the two would otherwise disagree about which
    // mode the process is in, and the run would pay for a report it never gets.
    setenv("DUMP_PEAK_VALUE_MB", "not-a-number", 1);
    assert(Config::ObserveOnlyRequested(&interval_ms));
    assert(interval_ms == 5);
    unsetenv("DUMP_PEAK_VALUE_MB");

    // An explicit 0 turns sampling off, which leaves nothing to observe.
    setenv("ALLOC_HOOK_PEAK_SAMPLE_MS", "0", 1);
    assert(!Config::ObserveOnlyRequested(&interval_ms));

    // A host framework's published interval selects the probe on its own, so a
    // process that is already being sampled externally needs no new variable.
    unsetenv("ALLOC_HOOK_PEAK_SAMPLE_MS");
    setenv(kExternalIntervalEnv, "7", 1);
    assert(Config::ObserveOnlyRequested(&interval_ms));
    assert(interval_ms == 7);

    // That framework's "not sampling" value must not select it.
    setenv(kExternalIntervalEnv, "0", 1);
    assert(!Config::ObserveOnlyRequested(&interval_ms));

    // An explicit interval still wins over the adopted one.
    setenv(kExternalIntervalEnv, "7", 1);
    setenv("ALLOC_HOOK_PEAK_SAMPLE_MS", "5", 1);
    assert(Config::ObserveOnlyRequested(&interval_ms));
    assert(interval_ms == 5);

    // And a report configured alongside it still wins over both.
    setenv("DUMP_PEAK_VALUE_MB", "64", 1);
    assert(!Config::ObserveOnlyRequested(&interval_ms));
    unsetenv("DUMP_PEAK_VALUE_MB");

    // A framework's interval supplies cadence only, so a step alongside it does
    // not turn it into a report: nothing in this process asked for one.
    unsetenv("ALLOC_HOOK_PEAK_SAMPLE_MS");
    setenv("DUMP_PEAK_STEP_MB", "8", 1);
    assert(Config::ObserveOnlyRequested(&interval_ms));
    assert(interval_ms == 7);
    ClearEnv();
}

void TestSignalSelection() {
    ClearEnv();
    const int default_signal = Config::DumpSignal();
    assert(default_signal > 0);
    setenv("BACKTRACE_DUMP_SIGNAL", "44", 1);
    assert(Config::DumpSignal() == 44);
    unsetenv("BACKTRACE_DUMP_SIGNAL");
    assert(Config::DumpSignal() == default_signal);

    Config config;
    const bool initialized = config.Init();
    assert(initialized);
    (void)initialized;
    assert(config.backtrace_dump_signal() == default_signal);
}

void TestGateLatches() {
    ClearEnv();
    setenv("ALLOC_HOOK_PEAK_SAMPLE_MS", "1", 1);
    // Resolving the mode reads the environment, and that read must not be
    // visible through errno: POSIX requires free() to preserve it, and the first
    // free() in a preloaded process can be the call that resolves the mode. The
    // sentinel is a value the parse would otherwise clear.
    errno = ERANGE;
    assert(observe_only::Bypassed());
    assert(errno == ERANGE);
    errno = 0;
    assert(observe_only::g_mode.load() == observe_only::kBypass);

    // The mode is a property of the process, not of the environment at the
    // moment of the call: a target that edits its own environment mid-run must
    // not end up with half its allocations tracked and half not.
    setenv("DUMP_PEAK_VALUE_MB", "64", 1);
    assert(observe_only::Bypassed());
    unsetenv("DUMP_PEAK_VALUE_MB");
}

void TestSamplerWithoutTracker() {
    ObservedPeakSampler& sampler = ObservedPeakSamplerInstance();
    // A null callback is the observe-only form: it samples, and nothing it
    // measures asks for a stack snapshot.
    assert(sampler.Start(
            1, /*floor_bytes=*/0, /*step_bytes=*/0, /*one_shot=*/false, nullptr));
    for (int waited_ms = 0; waited_ms < 5000 && sampler.stats().valid_samples == 0;
         waited_ms += 10) {
        usleep(10000);
    }
    sampler.Stop();

    const ObservedSamplerStats stats = sampler.stats();
    assert(stats.interval_ms == 1);
    assert(stats.valid_samples >= 1);
    assert(stats.peak_total_bytes > 0);
    assert(stats.max_rss_bytes > 0);
    // The floor above would have qualified the very first sample as a peak. With
    // no callback there is nothing to snapshot, so none was taken and none was
    // charged to the sampler's budget.
    assert(stats.snapshots == 0);
    assert(stats.total_snapshot_us == 0);
}

std::string ReadFile(const char* path) {
    FILE* file = fopen(path, "r");
    if (file == nullptr) {
        return std::string();
    }
    std::string contents;
    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        contents.append(buffer, bytes);
    }
    fclose(file);
    return contents;
}

void TestProbeReport() {
    ClearEnv();
    setenv("ALLOC_HOOK_PEAK_SAMPLE_MS", "1", 1);
    // The gate latched to the probe in TestGateLatches, which is the state a
    // preloaded process reaches before its constructors run.
    assert(observe_only::StartProbe());

    // The checkpoint signal has no live allocation table to dump in this mode,
    // and killing a process that is only being measured would be worse than
    // answering nothing, so it is ignored rather than left at its default.
    struct sigaction installed = {};
    assert(sigaction(Config::DumpSignal(), nullptr, &installed) == 0);
    assert(installed.sa_handler == SIG_IGN);

    for (int waited_ms = 0;
         waited_ms < 5000 &&
         ObservedPeakSamplerInstance().stats().valid_samples == 0;
         waited_ms += 10) {
        usleep(10000);
    }

    remove(kReportPath);
    assert(observe_only::WriteReport(kReportPath));
    const std::string report = ReadFile(kReportPath);
    // The human summary keeps the shape and the column widths of the host
    // framework's own summary, so the two can be read side by side.
    assert(report.find("                Memory Usage Summary") != std::string::npos);
    assert(report.find("  DMA Max (sampling):") != std::string::npos);
    assert(report.find("  RSS Max (sampling):") != std::string::npos);
    assert(report.find("  GPU mmap Max (sampling):") != std::string::npos);
    assert(report.find("  DMA+RSS+GPU mmap Max (sampling):") != std::string::npos);
    assert(report.find("  Sampling Period:") != std::string::npos);
    // The rules bound the box, so a row that outgrows its column is visible
    // here rather than only to the eye.
    const std::string rule(60, '=');
    assert(report.find(rule) != std::string::npos);
    // A part with no reachable interface must not read as a measured zero.
    assert(report.find("not measured, so not a zero:") != std::string::npos);
    // Followed by the machine-readable lines the full report uses, so one
    // pattern finds either mode's numbers.
    assert(report.find("observe_only probe") != std::string::npos);
    assert(report.find("observed_peak(max_of_sum):") != std::string::npos);
    assert(report.find("observed_sampler: interval_ms=1") != std::string::npos);
    // No allocation stacks are claimed, because none were captured.
    assert(report.find("alloc_size") == std::string::npos);
    // The colour marks the block on a terminal only: a file gets plain text,
    // because an escape sequence in one is noise to whatever reads it next.
    assert(report.find("\033[") == std::string::npos);
    remove(kReportPath);

    // Stops the sampler and prints the same block to stderr, which is what a
    // preloaded run produces at exit.
    observe_only::ReportAtExit();
    assert(!ObservedPeakSamplerInstance().started());
    ClearEnv();
}

}  // namespace

int main() {
    TestModeSelection();
    TestSignalSelection();
    TestGateLatches();
    TestSamplerWithoutTracker();
    TestProbeReport();
    return 0;
}
