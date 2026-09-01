#!/usr/bin/env bash
# Builds liballoc_hook.so for glibc Linux: either cross-compiled for an aarch64
# device, or natively for this host.
#
# There was no script for this target, so the only documented Linux recipe was the
# native host build. That is a trap on two counts: the artifact it produces cannot
# run on an arm64 device, and it installs into out/lib, which build_android.sh and
# build_ohos.sh also install into. The last build wins there, so preloading
# out/lib/liballoc_hook.so on a glibc device can pick up a Bionic build whose
# DT_NEEDED says `libc.so` instead of `libc.so.6` -- the loader then fails the
# whole process with "libm.so: cannot open shared object file", which says nothing
# about the real cause. This script installs per target and verifies the ABI of
# what it produced.
set -euo pipefail

BUILD_ARCH="${1:-arm64}"
SRC_DIR=$(cd "$(dirname "$0")" && pwd)

usage() {
    cat >&2 <<'EOF'
usage: ./build_linux.sh [arm64|host]

  arm64   cross-compile for an aarch64 glibc device (default).
          Requires ARM_GNU_TOOLCHAIN_PATH to point at an
          aarch64-none-linux-gnu toolchain.
  host    build natively for this machine and run the test suite.

environment:
  ARM_GNU_TOOLCHAIN_PATH   aarch64-none-linux-gnu toolchain root (arm64 only)
  ALLOC_HOOK_BUILD_TESTS   ON/OFF. Defaults to OFF when cross-compiling (the
                           binaries cannot run here) and ON for host. Turn it ON
                           for arm64 to get device-runnable test binaries under
                           the build directory.
  ALLOC_HOOK_DMA_CAPTURE   ON/OFF, default ON. Leave it ON for a real device:
                           most of a pipeline's memory is DMA, and with it off
                           the reports look almost empty.
EOF
    exit 1
}

DMA_CAPTURE="${ALLOC_HOOK_DMA_CAPTURE:-ON}"

case "${BUILD_ARCH}" in
    arm64)
        BUILD_DIR="${SRC_DIR}/build_linux"
        OUT_DIR="${SRC_DIR}/out/linux-arm64/lib"
        BUILD_TESTS="${ALLOC_HOOK_BUILD_TESTS:-OFF}"
        EXPECT_MACHINE="AArch64"
        ;;
    host)
        BUILD_DIR="${SRC_DIR}/build_linux_host"
        OUT_DIR="${SRC_DIR}/out/linux-host/lib"
        BUILD_TESTS="${ALLOC_HOOK_BUILD_TESTS:-ON}"
        EXPECT_MACHINE=""
        ;;
    -h|--help)
        usage
        ;;
    *)
        echo "unknown target: ${BUILD_ARCH}" >&2
        usage
        ;;
esac

CMAKE_ARGS=(
    -S "${SRC_DIR}" -B "${BUILD_DIR}" -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DMALLOC_HOOK_ENABLE_DMA_CAPTURE="${DMA_CAPTURE}"
    -DMALLOC_HOOK_BUILD_TESTS="${BUILD_TESTS}"
)

READELF=readelf

if [[ "${BUILD_ARCH}" == "arm64" ]]; then
    if [[ -z "${ARM_GNU_TOOLCHAIN_PATH:-}" ]]; then
        echo "ARM_GNU_TOOLCHAIN_PATH is not set." >&2
        echo "Point it at an aarch64-none-linux-gnu toolchain root." >&2
        exit 1
    fi
    TOOLCHAIN_BIN="${ARM_GNU_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu"
    if [[ ! -x "${TOOLCHAIN_BIN}-g++" ]]; then
        echo "ARM_GNU_TOOLCHAIN_PATH is invalid: ${ARM_GNU_TOOLCHAIN_PATH}" >&2
        echo "Expected ${TOOLCHAIN_BIN}-g++ to exist and be executable." >&2
        exit 1
    fi
    # No toolchain file: the target only differs from the host by the compiler, and
    # CMakeLists.txt derives os=linux/libc=glibc from the compiler itself.
    CMAKE_ARGS+=(
        -DCMAKE_SYSTEM_NAME=Linux
        -DCMAKE_SYSTEM_PROCESSOR=aarch64
        -DCMAKE_C_COMPILER="${TOOLCHAIN_BIN}-gcc"
        -DCMAKE_CXX_COMPILER="${TOOLCHAIN_BIN}-g++"
    )
    # Reads the cross ELF correctly even where the host binutils would not.
    if [[ -x "${TOOLCHAIN_BIN}-readelf" ]]; then
        READELF="${TOOLCHAIN_BIN}-readelf"
    fi
    echo "Using toolchain: ${ARM_GNU_TOOLCHAIN_PATH}"
fi

echo "Target: linux/${BUILD_ARCH}  DMA capture: ${DMA_CAPTURE}  tests: ${BUILD_TESTS}"

rm -rf "${BUILD_DIR}"
cmake "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --target alloc_hook

ARTIFACT="${BUILD_DIR}/liballoc_hook.so"
if [[ ! -f "${ARTIFACT}" ]]; then
    echo "build reported success but ${ARTIFACT} is missing" >&2
    exit 1
fi

# Verify what was actually produced, so a wrong-platform artifact is caught here
# rather than by the target's loader.
MACHINE=$("${READELF}" -h "${ARTIFACT}" | awk -F: '/Machine:/ {gsub(/^ +/, "", $2); print $2}')
NEEDED=$("${READELF}" -d "${ARTIFACT}" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')

if [[ -n "${EXPECT_MACHINE}" && "${MACHINE}" != *"${EXPECT_MACHINE}"* ]]; then
    echo "ABI check failed: expected a ${EXPECT_MACHINE} object, got '${MACHINE}'" >&2
    exit 1
fi
# glibc versions its sonames; Bionic does not. An unversioned libc.so here means a
# Bionic build leaked in, and it will not load on a glibc target.
if [[ "${NEEDED}" != *"libc.so.6"* ]]; then
    echo "ABI check failed: no libc.so.6 in DT_NEEDED (got: ${NEEDED})" >&2
    echo "An unversioned libc.so/libm.so means this is a Bionic (Android) build," >&2
    echo "which cannot load on a glibc target." >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"
cp -f "${ARTIFACT}" "${OUT_DIR}/"

if [[ "${BUILD_TESTS}" == "ON" && "${BUILD_ARCH}" == "host" ]]; then
    cmake --build "${BUILD_DIR}"
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

echo
echo "Built:    ${OUT_DIR}/liballoc_hook.so"
echo "Machine:  ${MACHINE}"
echo "NEEDED:   ${NEEDED}"
if [[ "${BUILD_ARCH}" == "arm64" ]]; then
    echo
    echo "Install per target on purpose: out/lib is shared with the Android and"
    echo "OHOS scripts, where the last build wins."
    if [[ "${BUILD_TESTS}" == "ON" ]]; then
        echo "Device-runnable test binaries: ${BUILD_DIR}/test/"
    fi
fi
