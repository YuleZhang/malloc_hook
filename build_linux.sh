#!/usr/bin/env bash
set -euo pipefail
set -x

DEFAULT_ARM_GNU_TOOLCHAIN_PATH="/home/zhangyu/pipeline/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu"
export ARM_GNU_TOOLCHAIN_PATH="${ARM_GNU_TOOLCHAIN_PATH:-${DEFAULT_ARM_GNU_TOOLCHAIN_PATH}}"

TARGET_TRIPLE="aarch64-none-linux-gnu"
SRC_DIR=$(readlink -f "$(dirname "$0")")
BUILD_DIR="${SRC_DIR}/build_linux"
INSTALL_PREFIX="${SRC_DIR}"

CC="${ARM_GNU_TOOLCHAIN_PATH}/bin/${TARGET_TRIPLE}-gcc"
CXX="${ARM_GNU_TOOLCHAIN_PATH}/bin/${TARGET_TRIPLE}-g++"
AR="${ARM_GNU_TOOLCHAIN_PATH}/bin/${TARGET_TRIPLE}-ar"
RANLIB="${ARM_GNU_TOOLCHAIN_PATH}/bin/${TARGET_TRIPLE}-ranlib"

if [ ! -x "${CC}" ] || [ ! -x "${CXX}" ]; then
    echo "ARM_GNU_TOOLCHAIN_PATH is invalid: ${ARM_GNU_TOOLCHAIN_PATH}"
    exit 1
fi

SYSROOT="$("${CC}" -print-sysroot)"
if [ -z "${SYSROOT}" ] || [ ! -d "${SYSROOT}" ]; then
    echo "failed to resolve sysroot from ${CC}"
    exit 1
fi

echo "use ${ARM_GNU_TOOLCHAIN_PATH} to build for linux (${TARGET_TRIPLE})"

cd "${SRC_DIR}"
rm -rf "${BUILD_DIR}"
rm -rf out
mkdir -p "${BUILD_DIR}" out/lib out/bin

cmake_args=(
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_SYSTEM_NAME=Linux
    -DCMAKE_SYSTEM_PROCESSOR=aarch64
    -DCMAKE_C_COMPILER="${CC}"
    -DCMAKE_CXX_COMPILER="${CXX}"
    -DCMAKE_AR="${AR}"
    -DCMAKE_RANLIB="${RANLIB}"
    -DCMAKE_SYSROOT="${SYSROOT}"
    -DALLOC_HOOK_BUILD_TESTS=OFF
    -G
    Ninja
)

cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" "${cmake_args[@]}" "$@"
cmake --build "${BUILD_DIR}" --target install -v

echo "build finished"
echo "library output: ${SRC_DIR}/out/lib/liballoc_hook.so"
