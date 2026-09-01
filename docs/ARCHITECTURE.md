# Architecture

[中文架构 / Chinese architecture](ARCHITECTURE.zh-CN.md) · [English README](../README.md)

## Data path

```text
allocation hook
    -> bounded raw stack capture
    -> PointerData live-allocation table
    -> AsyncStackPipeline queue
       -> module snapshot resolver
       -> symbolizer
       -> completion result
    -> checkpoint / peak report

dedicated observed-memory sampler (optional)
    -> current VmRSS + DMA + uncovered GPU mappings
    -> same-sample combined peak gate
    -> /proc peak context + PointerData live-stack snapshot
```

The allocation hook performs only bounded work. It records the requested size and memory type, captures raw program counters, and queues a copy of the raw record. Module lookup and symbolization are deliberately outside the hook path.

## Capture contract

`CaptureStack()` returns a project-owned `RawStackRecord` containing capture state, mode, backend, terminal error, skipped-frame count, module generation, and bounded PCs.

- **Fast** uses `_Unwind_Backtrace` when the configure-time compiler capability probe succeeds. It is current-thread, native, bounded, and raw-PC-only.
- **Accurate** selects an explicit platform backend. It can preserve useful partial frames and terminal errors; a fallback is explicit rather than silently changing mode or platform.

The core contract covers native C/C++ stacks. Managed-runtime stacks, other-thread contexts, and offline unwind inputs are optional capabilities rather than assumptions.

## Asynchronous resolution

`AsyncStackPipeline` deduplicates records by raw PCs and module generation. The
shipped `NativeModuleResolver` snapshots loaded ELF load segments through
`dl_iterate_phdr`; it is not an offline file reader. The shipped
`NativeSymbolizer` uses worker-side `dladdr` for dynamic symbol names and always
retains raw and module-relative PCs. It does not promise complete DWARF/debug-file
or offline symbolization. A `Symbolizer` converts resolved modules and PCs into
`SymbolizedFrame` values. Queue capacity, duplicate suppression, dropped work,
recursive submissions, worker-start failures, and processed results are exposed
through `AsyncStackStats`.

The completion callback receives a `StackResult` with the raw record, resolution state, and symbolized frames. Hook boundaries use `AsyncStackWorkerThread()` to prevent resolver internals from being recursively tracked.

## Accounting and reports

`PointerData` owns the live-allocation table and peak counters. Host allocations may use Fast-only Poisson byte sampling; resource paths remain exact. A sampled host record stores an estimated tracked size and is removed through the same pointer identity path when the allocation is freed.

Checkpoint reports are emitted by the exported `checkpoint(const char*)` entry point or by the configured signal. Peak snapshots are enabled by either `DUMP_PEAK_VALUE_MB`, which retains only the first crossing of that floor, or `ALLOC_HOOK_PEAK_SAMPLE_MS`, which chases the maximum and is throttled by `DUMP_PEAK_STEP_MB`.

The peak criterion is the maximum same-cycle sum of current `VmRSS`, dmabuf
bytes, and GPU mappings covered by neither. The sampler runs on its own thread
because residency and device ownership can change without an allocation hook
call, and because the criterion exists only in `/proc`. It reads current `VmRSS`,
not the historical `VmPeak` or `VmHWM` fields. Tracked allocation bytes are the
fallback criterion, used when no sampler runs -- an explicit
`ALLOC_HOOK_PEAK_SAMPLE_MS=0`, or a sampler thread that could not start -- and the
report labels which of the two produced the snapshot it retained.

When an observed sample crosses the snapshot gates, the callback collects a
second `/proc/self/status` reading (`VmRSS`, `RssAnon`, `RssFile`, and
`RssShmem`), the top resident mappings from `/proc/self/smaps`, and then the
live stack table. The sequence deliberately keeps expensive `/proc` work off
allocator threads, but it is not atomic: report values tagged `at_peak` belong
to the same callback window, not one kernel snapshot instant.

## Platform boundaries

CMake separates target OS, libc, architecture, compiler-unwind capability, and export policy. Android and glibc Linux export mmap hooks by policy; OHOS disables mmap hooks by default and offers an opt-in build switch for controlled reproductions. Resource hooks are enabled only when the resource-tracking build feature is selected.

## Extension guidance

Keep raw capture records and async resolver interfaces platform-neutral. Add
platform behavior behind `CaptureStack` backends, `ModuleResolver`
implementations, or `Symbolizer` implementations. A future offline ELF/DWARF
symbolizer must consume retained module identity and raw/module-relative PCs on
the worker side. Do not introduce module lookup, dynamic allocation, or
symbolization into the allocation hook path without updating the capture and
recursion contracts.
