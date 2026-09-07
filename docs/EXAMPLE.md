# Example

[中文示例](EXAMPLE.zh-CN.md) · [English README](../README.md) · [Getting a report](get_hook_report.md)

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

The default OHOS build leaves mmap interposition disabled. For a controlled
reproduction, set `ENABLE_MMAP_HOOK_EXPORT=ON`; `build_ohos.sh` forwards it to
the `ENABLE_MMAP_HOOK_EXPORT` CMake option. After a successful build the script
prints the effective CMake options and derived export policy.

## 3. Select capture mode

```sh
export ALLOC_HOOK_CAPTURE_MODE=fast       # fast or accurate
export BACKTRACE_MIN_SIZE=4096
```

Fast is the default: it captures bounded raw PCs on the allocation thread (a
frame-pointer walk on aarch64), then resolves modules and symbols
asynchronously on a worker. Accurate selects the platform backend. `BACKTRACE_MIN_SIZE`
is the cost control — allocations smaller than it are not stack-captured.

Whether a run also produces a report, and which one, is decided by the peak
variables (`ALLOC_HOOK_PEAK_SAMPLE_MS`, `DUMP_PEAK_VALUE_MB`, `DUMP_PEAK_STEP_MB`).
Those modes — the observe-only probe, first crossing, and peak chasing — with
their command lines, output, and report fields, are documented separately in
[`get_hook_report.md`](get_hook_report.md).

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
report to the requested path. The platform's backtrace signal queues the same
work onto a dedicated worker (Bionic's reserved backtrace signal on Android,
`46` on OHOS, `SIGRTMIN+6` elsewhere):

```sh
kill -46 <pid>   # OHOS; use the platform's backtrace signal
```

The report contains host/resource totals, allocation sizes and types, timestamps,
capture state/error, resolution state, and symbolized frames when the module
snapshot and symbolizer can resolve them. A normal shutdown flushes pending
unique stacks before the report is emitted. `_exit`, fatal signals, and
`SIGKILL` cannot provide a normal worker flush.

Interpret the values carefully:

- Every eligible host allocation is tracked at its exact requested size.
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

See [`get_hook_report.md`](get_hook_report.md) for the full set of report fields.

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
`ENABLE_MMAP_HOOK_EXPORT=ON` only for a controlled reproduction.

### Queue or worker errors appear

Inspect the report's resolution state and queue counters. The queue is bounded
to protect the allocator path; dropped work is preferable to blocking an
allocation indefinitely. Raise `BACKTRACE_MIN_SIZE` to reduce capture volume, or
increase the report/checkpoint frequency.

## 7. Known limitations

- Core coverage is native C/C++ and current-thread capture.
- ART/Dex, Ark/JSVM, managed-runtime frames, remote-thread register capture,
  and offline unwind input are optional future/backend capabilities.
- ioctl capture records the calling userspace stack only; it does not represent
  kernel or asynchronous device execution stacks.
- Direct syscalls bypass symbol interposition.
- Accurate backend availability and frame quality are platform- and toolchain-
  dependent.
