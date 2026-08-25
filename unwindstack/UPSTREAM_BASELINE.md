# Vendored unwindstack boundary

## Upstream baseline

This directory is a vendored snapshot of Android Open Source Project
`libunwindstack`. The source layout, Android release notes, and license headers
are retained from the upstream tree. The historical import did not record an
AOSP Git SHA, so the reproducible project baseline is the repository's
`eb3a154` initial Android NDK import plus the `8c468e0` unwindstack update.

## Local deviations

The project builds the snapshot as a private static `unwindstack` target from
`unwindstack/cmake/CMakeLists.txt`; it is not installed or exposed as a public
library. `backtrace/src/UnwindBacktrace.cpp` is the only capture adapter that
invokes Android's `AndroidLocalUnwinder`; Linux and OHOS capture paths use
project-owned raw records and do not require the Android adapter at runtime.

Fast capture's public contract is checked independently by
`fast_capture_boundary_compile`, which compiles and preprocesses only
`backtrace/include/UnwindBacktrace.h`. The legacy Accurate adapter types are
enabled privately for `helper` with
`MALLOC_HOOK_ENABLE_LEGACY_UNWINDSTACK_ADAPTER`; they are absent from ordinary
Fast consumers. The vendored target remains a private dependency of `helper`.

Changes to this directory must preserve AOSP license headers and should be
recorded here when a source file is added, removed, or materially adapted.
