# Usage

[中文使用说明](USAGE.zh-CN.md) · [Architecture](ARCHITECTURE.md) · [English README](../README.md)

## 1. Prerequisites

- CMake 3.23 or newer and a C++17 compiler for host builds.
- A matching Android NDK for Android builds, supplied through `NDK_ROOT`.
- An OpenHarmony native SDK for OHOS builds, supplied through `OHOS_NDK_ROOT`
  (or `NDK_ROOT`).
- A native target process. This library does not make managed Java/Kotlin,
  ART/Dex, Ark/JSVM, or other-thread stacks part of the core contract.

The host build intentionally supports glibc Linux. OHOS is selected by its
explicit toolchain identity and is not inferred from a generic musl build.

## 2. Build

### Host Linux

```sh
./build_linux.sh host
```

The script builds natively, runs the tests, installs the library under
`out/linux-host/lib`, and prints the effective build options. The equivalent
manual build is:

```sh
cmake -S . -B build-host \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-host --target alloc_hook
ctest --test-dir build-host --output-on-failure
cmake --install build-host --prefix "$PWD"
```

The resulting library is `out/lib/liballoc_hook.so`.

For a glibc Linux/aarch64 cross-build, set `ARM_GNU_TOOLCHAIN_PATH` to an
`aarch64-none-linux-gnu` toolchain root and run `./build_linux.sh arm64`.

### Android

```sh
export NDK_ROOT=/path/to/android-ndk
./build_android.sh arm64-v8a
```

The script uses API level 21 for the arm64 and armeabi-v7a targets, installs the
library under `out/lib`, and then runs the bundled smoke workload through
`adb`. Keep a matching device connected and make `adb` available on `PATH`, or
invoke the CMake build directly when only the library artifact is needed.
After a successful build it prints the effective CMake options and derived
export policy.

### OHOS

```sh
export OHOS_NDK_ROOT=/path/to/ohos-sdk/native
./build_ohos.sh arm64-v8a
```

`OHOS_ENABLE_MMAP_HOOK=ON` is an opt-in switch for controlled reproductions;
`build_ohos.sh` forwards this environment variable to the
`MALLOC_HOOK_OHOS_MMAP_HOOK` CMake option.
The default OHOS build leaves mmap interposition disabled.
After a successful build the script prints the effective CMake options and
derived export policy.

## 3. Select capture and sampling

```sh
export ALLOC_HOOK_CAPTURE_MODE=fast       # fast or accurate
export ALLOC_HOOK_SAMPLING_INTERVAL_BYTES=4096
export BACKTRACE_MIN_SIZE=4096
```

Fast is the default. It captures bounded raw PCs with `_Unwind_Backtrace` when
the compiler runtime exposes that capability, then resolves modules and symbols
asynchronously. `ALLOC_HOOK_SAMPLING_INTERVAL_BYTES` is Fast-only; values `0`
and `1` disable sampling. Accurate selects the platform backend and disables
host byte sampling.

Additional public controls:

| Variable | Purpose |
| --- | --- |
| `DUMP_PEAK_VALUE_MB` | Enable peak recording, dump on exit, and snapshots after the selected peak criterion exceeds this floor. |
| `DUMP_PEAK_STEP_MB` | Upper bound on growth before another peak snapshot; small peaks use 25% growth with a 64 KB floor. `0` snapshots every new peak. |
| `ALLOC_HOOK_PEAK_SAMPLE_MS` | Use the observed `VmRSS + DMA + GPU` total sampled at this positive millisecond interval as the peak criterion. Requires `DUMP_PEAK_VALUE_MB`; `0` forces it off. |
| `ALLOC_HOOK_DUMP_PREFIX` | Set the report path prefix. |
| `BACKTRACE_DUMP_SIGNAL` | Override the platform-selected checkpoint signal. |
| `ALLOC_HOOK_DEBUG` | Enable hook diagnostics on stderr. |

Sampling affects host allocation attribution only. mmap, DMA, and ioctl
resource accounting remains exact for events that pass the platform export and
resource filters.

`ALLOC_HOOK_SAMPLING_INTERVAL_BYTES` and `ALLOC_HOOK_PEAK_SAMPLE_MS` control
different mechanisms. The first probabilistically reduces host stack capture.
The second starts a dedicated observer thread and does not change which
allocations are tracked.

With no positive peak interval, snapshots follow tracked live allocation bytes.
With a positive interval, each observer cycle reads current `VmRSS` from
`/proc/self/status`, dmabuf bytes, and GPU device mappings covered by neither;
the maximum of that same-cycle sum selects the peak window. A host framework's
positive environment value whose name ends in `AUTO_SHOW_MEM_USE_DURATION_MS`
is adopted automatically unless `ALLOC_HOOK_PEAK_SAMPLE_MS` is explicitly set,
including to `0`.

The observer does not use the historical `VmPeak` or `VmHWM` fields. When a new
observed peak crosses the configured snapshot gates, it immediately re-reads
`VmRSS`, `RssAnon`, `RssFile`, and `RssShmem`, collects the top resident mappings
from `/proc/self/smaps`, and copies the live stack table. These operations are
sequential, so `at_peak` means the same callback window rather than an atomic
kernel snapshot.

To retain a stack snapshot for every newly sampled maximum:

```sh
export DUMP_PEAK_VALUE_MB=0
export ALLOC_HOOK_PEAK_SAMPLE_MS=5   # use the external sampler's interval
export DUMP_PEAK_STEP_MB=0
export BACKTRACE_MIN_SIZE=1024
```

`DUMP_PEAK_STEP_MB=0` is the exact but more expensive mode. Keep the default
step when a bounded gap between `at_snapshot` and `max_of_sum` is acceptable.
Set `BACKTRACE_MIN_SIZE=0` only if stacks for allocations below 1 KB are worth
the extra overhead. Leave both byte-sampling variables unset (effective value
`1`) for exact host attribution and unsuppressed eligible stacks.

## 4. Preload a native process

```sh
mkdir -p ./trace
ALLOC_HOOK_CAPTURE_MODE=fast \
BACKTRACE_MIN_SIZE=4096 \
LD_LIBRARY_PATH="$PWD/out/lib" \
LD_PRELOAD="$PWD/out/lib/liballoc_hook.so" \
./your_program arg1 arg2
```

On Android/OHOS, copy the library to a writable target directory and use the
platform shell's preload mechanism. The process must use the same ABI as the
library.

The interposer covers the exported C allocation family. OHOS additionally
exports the configured C++ new/delete family. mmap and resource hooks are
controlled by the target platform's export policy. Direct system calls and
unexported vendor entry points bypass an interposer and are not reported as
capture failures.

## 5. Checkpoints and output

The exported C function `checkpoint(const char*)` writes a live-allocation
report to the requested path. A configured signal queues the same work onto a
dedicated worker:

```sh
kill -<BACKTRACE_DUMP_SIGNAL> <pid>
```

The report contains host/resource totals, allocation sizes and types, timestamps,
capture state/error, resolution state, and symbolized frames when the module
snapshot and symbolizer can resolve them. A normal shutdown flushes pending
unique stacks before the report is emitted. `_exit`, fatal signals, and
`SIGKILL` cannot provide a normal worker flush.

Interpret the values carefully:

- Fast sampling records an estimated tracked host size, not resident memory.
- `DMA+RSS Max (sampling)` includes resident mappings and runtime overhead that
  are outside this hook's live-allocation table.
- Host and DMA component peaks can occur at different times; use the hook's
  time-consistent combined peak rather than adding independent maxima.
- `observed_peak(at_snapshot)` is the sample that triggered the retained stack
  snapshot; `observed_peak(max_of_sum)` is the largest same-cycle sum seen over
  the run. They can differ when the step gate suppresses a later rebuild.
- `rss_breakdown(at_peak)` and `rss_by_mapping(at_peak)` are the `/proc` state
  collected immediately for that retained peak window; if no peak context was
  captured, their labels say `at_exit` instead.
- A `partial` capture, unresolved module, dropped queue item, or symbolizer
  failure is explicit report metadata, not a fabricated frame.

## 6. Troubleshooting

### The process exits before the library is initialized

Check ABI, loader search paths, and the target's writable directory. Bootstrap
allocations use raw mmap until libc symbols and hook state are ready.

### Fast reports contain few or no symbols

The shipped worker-side path snapshots loaded module ranges and uses `dladdr` for
dynamic symbol names. Matching debug files are useful for a future/custom offline
symbolizer, but they do not make the current `NativeSymbolizer` a complete DWARF
resolver. Fast capture stores raw PCs; symbolization is asynchronous and may
remain unresolved for stripped, unloaded, or non-exported symbols.
Use Accurate mode when a platform-specific backend is required for a difficult
native stack.

### OHOS mmap records are missing

The default OHOS export policy disables mmap hooks. Rebuild with
`OHOS_ENABLE_MMAP_HOOK=ON` only for a controlled reproduction.

### Queue or worker errors appear

Inspect the report's resolution state and queue counters. The queue is bounded
to protect the allocator path; dropped work is preferable to blocking an
allocation indefinitely. Reduce capture volume with Fast sampling or increase
the report/checkpoint frequency.

## 7. Known limitations

- Core coverage is native C/C++ and current-thread capture.
- ART/Dex, Ark/JSVM, managed-runtime frames, remote-thread register capture,
  and offline unwind input are optional future/backend capabilities.
- ioctl capture records the calling userspace stack only; it does not represent
  kernel or asynchronous device execution stacks.
- Direct syscalls bypass symbol interposition.
- Accurate backend availability and frame quality are platform- and toolchain-
  dependent.
