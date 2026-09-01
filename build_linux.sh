#!/usr/bin/env bash
# Builds liballoc_hook.so for glibc Linux, either natively or for aarch64.
set -euo pipefail

BUILD_ARCH="${1:-arm64}"
SRC_DIR=$(cd "$(dirname "$0")" && pwd)

usage() {
    cat >&2 <<'EOF'
usage: ./build_linux.sh [arm64|host]

  arm64   cross-compile for aarch64 glibc Linux (default); requires
          ARM_GNU_TOOLCHAIN_PATH.
  host    build natively and run the test suite.

environment:
  ARM_GNU_TOOLCHAIN_PATH   aarch64-none-linux-gnu toolchain root (arm64 only)
  ALLOC_HOOK_BUILD_TESTS   ON/OFF; defaults to OFF for arm64 and ON for host
  ALLOC_HOOK_DMA_CAPTURE   ON/OFF; defaults to ON
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
        exit 1
    fi
    TOOLCHAIN_BIN="${ARM_GNU_TOOLCHAIN_PATH}/bin/aarch64-none-linux-gnu"
    if [[ ! -x "${TOOLCHAIN_BIN}-g++" ]]; then
        echo "ARM_GNU_TOOLCHAIN_PATH is invalid: ${ARM_GNU_TOOLCHAIN_PATH}" >&2
        exit 1
    fi
    CMAKE_ARGS+=(
        -DCMAKE_SYSTEM_NAME=Linux
        -DCMAKE_SYSTEM_PROCESSOR=aarch64
        -DCMAKE_C_COMPILER="${TOOLCHAIN_BIN}-gcc"
        -DCMAKE_CXX_COMPILER="${TOOLCHAIN_BIN}-g++"
    )
    if [[ -x "${TOOLCHAIN_BIN}-readelf" ]]; then
        READELF="${TOOLCHAIN_BIN}-readelf"
    fi
fi

rm -rf "${BUILD_DIR}"
cmake "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --target alloc_hook

ARTIFACT="${BUILD_DIR}/liballoc_hook.so"
if [[ ! -f "${ARTIFACT}" ]]; then
    echo "build reported success but ${ARTIFACT} is missing" >&2
    exit 1
fi

MACHINE=$("${READELF}" -h "${ARTIFACT}" | awk -F: '/Machine:/ {gsub(/^ +/, "", $2); print $2}')
NEEDED=$("${READELF}" -d "${ARTIFACT}" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')

if [[ -n "${EXPECT_MACHINE}" && "${MACHINE}" != *"${EXPECT_MACHINE}"* ]]; then
    echo "ABI check failed: expected ${EXPECT_MACHINE}, got '${MACHINE}'" >&2
    exit 1
fi
if [[ "${NEEDED}" != *"libc.so.6"* ]]; then
    echo "ABI check failed: no libc.so.6 in DT_NEEDED (got: ${NEEDED})" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"
cp -f "${ARTIFACT}" "${OUT_DIR}/"

if [[ "${BUILD_TESTS}" == "ON" && "${BUILD_ARCH}" == "host" ]]; then
    cmake --build "${BUILD_DIR}"
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

cmake --build "${BUILD_DIR}" --target print_build_options
echo "Built: ${OUT_DIR}/liballoc_hook.so"
echo "Machine: ${MACHINE}"
echo "NEEDED: ${NEEDED}"
