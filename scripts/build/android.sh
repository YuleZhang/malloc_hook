#!/usr/bin/env bash
set -euo pipefail

BUILD_ARCH="${1:-arm64-v8a}"
SRC_DIR=$(cd "$(dirname "$0")/../.." && pwd)
NDK="${ANDROID_NDK_ROOT:-${NDK_ROOT:-}}"
API="${ANDROID_API_LEVEL:-21}"
BUILD_DIR="${SRC_DIR}/build/android/${BUILD_ARCH}"
OUT_DIR="${SRC_DIR}/out/android-${BUILD_ARCH}"

usage() {
  cat >&2 <<EOF
usage: ./build.sh android [arm64-v8a|armeabi-v7a] [--clean]

Environment:
  ANDROID_NDK_ROOT or NDK_ROOT   Android NDK root (required)
  ANDROID_API_LEVEL              minimum Android API (default: 21)
  MALLOC_HOOK_BUILD_TESTS        ON/OFF (default: OFF)
  MALLOC_HOOK_BUILD_PROBE        ON/OFF (default: ON)
  MALLOC_HOOK_ENABLE_DMA_CAPTURE ON/OFF (default: ON)
  ENABLE_MMAP_HOOK_EXPORT        ON/OFF (default: ON)
  MALLOC_HOOK_BUILD_FDSAN_DEVICE_GATE ON/OFF (default: OFF)
EOF
  exit 2
}

case "$BUILD_ARCH" in
  arm64-v8a|armeabi-v7a) ;;
  -h|--help) usage ;;
  *) echo "Unsupported Android ABI: $BUILD_ARCH" >&2; usage ;;
esac
if [[ "${2:-}" == "--clean" ]]; then rm -rf "$BUILD_DIR"; fi
if [[ -z "$NDK" || ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
  echo "Set ANDROID_NDK_ROOT or NDK_ROOT to a valid Android NDK." >&2; exit 1
fi
if ! [[ "$API" =~ ^[0-9]+$ ]]; then echo "ANDROID_API_LEVEL must be an integer" >&2; exit 1; fi
mkdir -p "$BUILD_DIR" "$OUT_DIR"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_NDK="$NDK" -DANDROID_ABI="$BUILD_ARCH" \
  -DANDROID_PLATFORM="android-$API" -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$OUT_DIR" \
  -DMALLOC_HOOK_BUILD_TESTS="${MALLOC_HOOK_BUILD_TESTS:-OFF}" \
  -DMALLOC_HOOK_BUILD_PROBE="${MALLOC_HOOK_BUILD_PROBE:-ON}" \
  -DMALLOC_HOOK_ENABLE_DMA_CAPTURE="${MALLOC_HOOK_ENABLE_DMA_CAPTURE:-ON}" \
  -DENABLE_MMAP_HOOK_EXPORT="${ENABLE_MMAP_HOOK_EXPORT:-ON}" \
  -DMALLOC_HOOK_BUILD_FDSAN_DEVICE_GATE="${MALLOC_HOOK_BUILD_FDSAN_DEVICE_GATE:-OFF}"
cmake --build "$BUILD_DIR" --parallel
cmake --install "$BUILD_DIR"
cmake --build "$BUILD_DIR" --target print_build_options

echo "Android build complete:"
echo "  library: $OUT_DIR/out/lib/liballoc_hook.so"
if [[ "${MALLOC_HOOK_BUILD_PROBE:-ON}" == "ON" ]]; then echo "  probe: $OUT_DIR/out/bin/alloc_hook_probe"; fi
if [[ "${MALLOC_HOOK_BUILD_FDSAN_DEVICE_GATE:-OFF}" == "ON" ]]; then echo "  fdsan: $OUT_DIR/out/bin/android_fdsan_device_gate"; fi
