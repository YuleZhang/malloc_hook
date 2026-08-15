#!/usr/bin/env bash
set -euo pipefail

BUILD_ARCH="${1:-arm64-v8a}"
SRC_DIR=$(cd "$(dirname "$0")" && pwd)
NDK_URL="${OHOS_NDK_URL:-}"
DEPS_DIR="${SRC_DIR}/.deps"
AUTO_NDK_DIR="${DEPS_DIR}/ohos_ndk/native"

case "${BUILD_ARCH}" in
    arm64-v8a|armeabi-v7a|x86_64) ;;
    *)
        echo "BUILD_ARCH must be one of: arm64-v8a, armeabi-v7a, x86_64" >&2
        exit 1
        ;;
esac

resolve_ndk_root() {
    local root="${OHOS_NDK_ROOT:-${NDK_ROOT:-}}"
    if [[ -n "${root}" ]]; then
        if [[ -f "${root}/build/cmake/ohos.toolchain.cmake" ]]; then
            echo "${root}"
            return
        fi
        if [[ -f "${root}/native/build/cmake/ohos.toolchain.cmake" ]]; then
            echo "${root}/native"
            return
        fi
        echo "OHOS_NDK_ROOT/NDK_ROOT is invalid: ${root}" >&2
        exit 1
    fi

    if [[ ! -f "${AUTO_NDK_DIR}/build/cmake/ohos.toolchain.cmake" ]]; then
        if [[ -z "${NDK_URL}" ]]; then
            cat >&2 <<'EOF'
OHOS NDK not found. Provide the NDK via one of:
  OHOS_NDK_ROOT=/path/to/ohos-sdk/native   use an already-installed NDK
  OHOS_NDK_URL=https://.../ohos-ndk.zip    download and unpack automatically
Obtain the OpenHarmony/HarmonyOS native SDK (NDK) from your SDK provider.
EOF
            exit 1
        fi
        mkdir -p "${DEPS_DIR}"
        local zip_path="${DEPS_DIR}/ohos_ndk.zip"
        if [[ ! -f "${zip_path}" ]]; then
            echo "Downloading OHOS NDK from ${NDK_URL}" >&2
            curl -L --fail -o "${zip_path}" "${NDK_URL}"
        fi
        rm -rf "${DEPS_DIR}/ohos_ndk"
        unzip -q "${zip_path}" -d "${DEPS_DIR}/ohos_ndk"
    fi

    echo "${AUTO_NDK_DIR}"
}

OHOS_NDK_NATIVE=$(resolve_ndk_root)
TOOLCHAIN_FILE="${OHOS_NDK_NATIVE}/build/cmake/ohos.toolchain.cmake"
BUILD_DIR="${SRC_DIR}/build_ohos/${BUILD_ARCH}"

if [[ ! -x "${OHOS_NDK_NATIVE}/llvm/bin/clang++" ]]; then
    echo "OHOS NDK is invalid, missing llvm/bin/clang++ under ${OHOS_NDK_NATIVE}" >&2
    exit 1
fi

echo "Using OHOS NDK: ${OHOS_NDK_NATIVE}"
echo "Building liballoc_hook.so for ${BUILD_ARCH}"
echo "OHOS mmap hooks: ${OHOS_ENABLE_MMAP_HOOK:-OFF}"

rm -rf "${BUILD_DIR}"
cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DOHOS_ARCH="${BUILD_ARCH}" \
    -DOHOS_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${SRC_DIR}" \
    -DMALLOC_HOOK_OHOS_MMAP_HOOK="${OHOS_ENABLE_MMAP_HOOK:-OFF}" \
    -DMALLOC_HOOK_BUILD_TESTS=OFF
cmake --build "${BUILD_DIR}" --target install -v

echo "Built: ${SRC_DIR}/out/lib/liballoc_hook.so"
