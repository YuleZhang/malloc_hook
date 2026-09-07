# Getting a report

[中文版 / Chinese](get_hook_report.zh-CN.md) · [README](../README.md)

Every mode this library runs measures the same criterion — the **observed
total**, `VmRSS` + dmabuf + GPU mappings, sampled from `/proc` on a dedicated
thread. What differs is whether the run also *tracks allocations* to attribute
that total to call sites, because that is where the cost is. Which one runs is
decided entirely by which environment variables are set:

| Set this | Mode | Interposed calls | Output | Answers |
| --- | --- | --- | --- | --- |
| `ALLOC_HOOK_PEAK_SAMPLE_MS=k` | observe-only probe | forwarded to libc untouched | a log block on stderr at exit | how much did this process hold, split into rss / dma / gpu |
| `DUMP_PEAK_VALUE_MB=N` | first crossing | tracked, one stack walk per run | a report file | what was holding memory when it first passed `N` MB |
| `ALLOC_HOOK_PEAK_SAMPLE_MS=k` + `DUMP_PEAK_STEP_MB=s` | peak chasing | tracked, one stack walk per `s` of growth | a report file | what was holding memory at the run's maximum |

The probe answers *how much*; the two report modes answer *which call sites*,
and charge for it. Nothing else enables a report: an interval without a step, or
a step without an interval, produces the probe or nothing at all.

`0` is how each of these is turned off, uniformly: `DUMP_PEAK_VALUE_MB=0` asks
for no first-crossing snapshot, `DUMP_PEAK_STEP_MB=0` asks for no chasing, and
`ALLOC_HOOK_PEAK_SAMPLE_MS=0` asks for no sampler at all. A run that zeroes both
report switches keeps the probe; a run that zeroes all three tracks allocations
for the on-demand checkpoint and produces nothing on its own.

Both report modes write to `ALLOC_HOOK_DUMP_PREFIX` on normal exit and create the
directory if it does not exist. Setting both variables gives first crossing at
the floor, with the interval you supplied.

## 1. The observe-only probe

Interposition cannot be switched off at runtime -- `LD_PRELOAD` has already bound
these symbols -- but whether an interposed call *does* anything can be, and
tracking only pays for itself if a report consumes it. So a run that asks only for
a cadence never builds the tracker:

```sh
LD_PRELOAD=/path/liballoc_hook.so ALLOC_HOOK_PEAK_SAMPLE_MS=5 ./your_program
```

```text
alloc_hook: ============================================================
alloc_hook:                 Memory Usage Summary
alloc_hook: ------------------------------------------------------------
alloc_hook:   DMA Max (sampling):                              0.00 MB
alloc_hook:   RSS Max (sampling):                             61.27 MB
alloc_hook:   GPU mmap Max (sampling):                        12.05 MB
alloc_hook:   RSS Max (getrusage):                            61.54 MB
alloc_hook:   DMA+RSS+GPU mmap Max (sampling):                73.29 MB
alloc_hook:   not measured, so not a zero: dma
alloc_hook: ------------------------------------------------------------
alloc_hook:   Sampling Period:                                    1 ms
alloc_hook:   Achieved Period:                                22.28 ms
alloc_hook: ============================================================
```

The block keeps the shape, the column widths and the yellow of the summary a host
framework prints for the same three quantities, so the two can be read side by
side in one log; the colour is emitted only when stderr is a terminal. The cost
is one sampler thread reading `/proc` plus one relaxed load per interposed call --
68 ns per `malloc`/`free` pair against 60 ns with nothing preloaded, where a
tracking mode with no size filter costs 2762 ns.

Four rows to read carefully. The first three are each part's own maximum, so they
need not have peaked together and their sum is not the combined row -- that row is
the largest *same-cycle* sum, which is what an external evaluator reports as the
process peak. `RSS Max (getrusage)` is the kernel's own high-water mark: standing
well above the sampled RSS row, it means a resident peak happened between two
samples. `Achieved Period` above `Sampling Period` means the `/proc` reads cost
more than the interval and the sampler throttled itself to stay under half a core.
And a part with no reachable interface is named on the `not measured` line rather
than left to read as a measured zero.

Use the probe to find out whether a process has a memory problem, and how big it
is, without perturbing it: it captures no stacks, so it cannot say which call site
is responsible. It has no live allocation table either, so `checkpoint()` writes
these same figures to the requested path instead of a heap report, and the
checkpoint signal is ignored rather than left to kill a process that is only being
measured. A `fork` child prints nothing, and a process that leaves through
`_exit()` or a fatal signal prints nothing at all -- the same limitation the
tracked report has. When the answer is "yes, and here is how much", add
`DUMP_PEAK_STEP_MB` or `DUMP_PEAK_VALUE_MB` to find out where it goes.

DMA is measured by default: the sampler reads dmabuf accounting from `/proc`
unconditionally (it does not depend on the `MALLOC_HOOK_ENABLE_DMA_CAPTURE` build
flag, which governs the tracked `ioctl`/`close` interposition). A `0.00 MB` DMA
row on a host smoke build simply means the process holds no dmabuf; a real device
pipeline is mostly DMA. The `gpu` term and its `/proc/self/smaps` pitfalls are
covered in [`GPU_MEMORY_ACCOUNTING.md`](GPU_MEMORY_ACCOUNTING.md).

## 2. The report modes

### 2a. `DUMP_PEAK_VALUE_MB` — first crossing (common)

```sh
export DUMP_PEAK_VALUE_MB=300        # first crossing of 300MB; one stack walk
export BACKTRACE_MIN_SIZE=1024
```

A positive floor selects first-crossing: peak recording is enabled, and a single
snapshot is retained -- taken the first time the observed total passes this many
MB. One stack walk for the whole run, so after the crossing no allocating thread
is stalled again, which matters when the pipeline being measured is
timing-sensitive.

In exchange the stacks describe the floor, not the maximum, so the floor has to be
set near the peak to answer "what is holding memory at the peak" -- typically from
an earlier run's report. Read `snapshot_lag` to tune it: it is exactly how much
higher the floor could have been set.

```text
peak_retention: first_crossing floor=200.000000MB (single snapshot; step unused)
snapshot_lag: observed=+117.800781MB (of 323.628906MB peak)
```

### 2b. `ALLOC_HOOK_PEAK_SAMPLE_MS` + `DUMP_PEAK_STEP_MB` — peak chasing

```sh
export ALLOC_HOOK_PEAK_SAMPLE_MS=5   # peak chasing; preferably match the external sampler
export DUMP_PEAK_STEP_MB=1           # smallest useful step: closest to the maximum, most walks
export BACKTRACE_MIN_SIZE=1024       # use 0 only when every small stack is required
```

A positive step *together with* an interval selects peak-chasing; on its own it
does nothing. `DUMP_PEAK_STEP_MB` is the upper bound on the growth required before
the peak snapshot is rebuilt: a smaller step keeps the snapshot nearer the maximum
and pays more stack walks, and for small peaks the code uses 25% growth with a
64 KB floor.

Peak chasing needs no prior knowledge of the peak, so a first run gets a correct
peak snapshot straight away, at the cost of a stack walk every time the peak grows
past the step.

## 3. Sampling cadence

`ALLOC_HOOK_PEAK_SAMPLE_MS` needs no value in the common case. A host framework
that samples this process's memory publishes the interval it uses in a variable
whose name ends in `AUTO_SHOW_MEM_USE_DURATION_MS`; when the hook finds one set
to a positive value it adopts that interval, so the snapshot lands at the instant
such a framework calls the peak and the cadence does not need to be kept in sync
by hand. Failing that, peak recording samples every 50 ms. Setting
`ALLOC_HOOK_PEAK_SAMPLE_MS` explicitly overrides both, including to `0`, which
runs no sampler at all and compares the floor against tracked allocation bytes
instead -- a different quantity, which the report labels as such.

A framework's variable only ever supplies the cadence. It never enables peak
recording on its own: a process that set none of these variables must not gain a
sampler thread and an exit report from its environment.

This does not read the historical `VmPeak` or `VmHWM` fields. They cannot tell
the hook when to copy live stacks. Each sampling cycle instead reads the current
`VmRSS`, then DMA and GPU memory, and compares that same-cycle sum with the
largest sum seen so far. When the sum also crosses the `DUMP_PEAK_VALUE_MB` and
`DUMP_PEAK_STEP_MB` gates, the callback immediately re-reads
`VmRSS`/`RssAnon`/`RssFile`/`RssShmem`, collects the top resident mappings from
`/proc/self/smaps`, and copies the live stack table. These reads and the stack
copy are sequential, not an atomic kernel snapshot; report labels such as
`at_peak` mean the same peak callback window.

## 4. Reading the report fields

The report says which watermark the snapshot describes and which criterion
produced it, and — when it was the observed footprint — what that footprint read
at the snapshot instant, what its maximum over the run was, and what the sampler
cost:

```text
peak_retention: chase_max (snapshot refreshed per step)
peak_criterion: observed_host_rss_plus_dma_plus_gpu (from /proc, aligned with an external sampler)
observed_peak(at_snapshot):     rss=291.75MB dma=935.12MB gpu=24.00MB total=1250.87MB
observed_peak(max_of_sum):      rss=291.75MB dma=935.12MB gpu=24.00MB total=1250.87MB (...)
observed_peak(independent_max): rss=369.54MB dma=935.12MB gpu=24.00MB
observed_sampler: interval_ms=1 achieved_ms=1.58 dma_source=fd+maps gpu_source=smaps samples=9516 ...
```

`at_snapshot` equal to `max_of_sum` is the goal state: the stacks were captured
at the maximum, not at some earlier step of it. In first-crossing mode they are
not equal by design, and `snapshot_lag` names the difference.

A run whose floor was never reached has no snapshot at all. Rather than emit an
empty stack section, which reads as a hook that captured nothing, the report
falls back to the live allocations at report time and says so:

```text
peak_snapshot: none (criterion never passed the floor; the list above is live at report time)
peak_criterion: none (nothing was snapshotted)
```

`achieved_ms` above the requested interval means the `/proc` reads cost more than
the interval and the sampler throttled itself to stay under half a core — it never
silently claims a cadence it did not reach. `independent_max` is higher than any
single part of `max_of_sum` whenever host and device memory peak at different
moments, which is the situation this whole mechanism exists for.

On a platform with a supported GPU device node the observed criterion is
`rss + dma + gpu`; there is no runtime switch for `rss + dma` while excluding
only that otherwise-unaccounted GPU term. What the `gpu` term counts, why it
escapes both `rss` and `dma`, and the `/proc/self/smaps` cost of reading it are
described in [`GPU_MEMORY_ACCOUNTING.md`](GPU_MEMORY_ACCOUNTING.md).

## 5. Limits shared by every mode

The sampler reads `/proc`, so it sees the process at sample instants only; a peak
that exists for less than one interval is missed by it exactly as it is missed by
the external sampler being aligned with. `dma_source=none` means this kernel
exposes no reachable dmabuf accounting, which is not the same as the process
holding no device memory. And `gpu_source` is `not_applicable` on any platform
without the device node this pass counts, which is likewise not a measurement of
zero: it is the reason no measurement was attempted.

Reports are written on normal exit or on the checkpoint signal. `_exit()`, a
fatal signal, and `SIGKILL` cannot guarantee the normal worker flush.
