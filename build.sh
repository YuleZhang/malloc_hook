#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
BACKEND_DIR="${ROOT_DIR}/scripts/build"

usage() {
  cat <<'EOF'
Usage: ./build.sh <os> [architecture] [backend options...]

OS is required. Supported values:
  android  [arm64-v8a|armeabi-v7a]
  linux    [arm64|host]
  ohos     [arm64-v8a|armeabi-v7a|x86_64]

Examples:
  NDK_ROOT=/path/to/android-ndk ./build.sh android arm64-v8a
  ./build.sh linux host
  ARM_GNU_TOOLCHAIN_PATH=/path/to/toolchain ./build.sh linux arm64
  OHOS_NDK_ROOT=/path/to/ohos-sdk/native ./build.sh ohos arm64-v8a

Run ./build.sh <os> --help for platform-specific options.
EOF
}

if [[ $# -lt 1 ]]; then
  echo "Error: target OS is required; refusing to guess." >&2; usage >&2; exit 2
fi
OS="$1"; shift
case "$OS" in
  android|linux|ohos) exec "${BACKEND_DIR}/${OS}.sh" "$@" ;;
  -h|--help) usage; exit 0 ;;
  *) echo "Unsupported OS '${OS}'; expected android, linux, or ohos." >&2; usage >&2; exit 2 ;;
esac
