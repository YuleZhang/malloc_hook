#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
    cat >&2 <<'USAGE'
Usage: ./run_on_ohos.sh <remote-workdir> <command...>

Example:
  ./run_on_ohos.sh /data/local/tmp/alloc_test ./demo arg1

Environment:
  HDC                         hdc executable, default: hdc
  LOCAL_LIB                   local hook library, default: out/lib/liballoc_hook.so
  BACKTRACE_MIN_SIZE          minimum allocation size captured, default: 40960
  BACKTRACE_DUMP_SIGNAL       signal used by checkpoint handler, default: 46
  ALLOC_HOOK_TRACE_DIR        trace directory on device, default: /data/local/tmp/trace
USAGE
    exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
HDC_BIN="${HDC:-hdc}"
LOCAL_LIB="${LOCAL_LIB:-${SCRIPT_DIR}/out/lib/liballoc_hook.so}"
REMOTE_DIR="$1"
shift
REMOTE_CMD="$*"
TRACE_DIR="${ALLOC_HOOK_TRACE_DIR:-/data/local/tmp/trace}"
MIN_SIZE="${BACKTRACE_MIN_SIZE:-40960}"
DUMP_SIGNAL="${BACKTRACE_DUMP_SIGNAL:-46}"

if [[ ! -f "${LOCAL_LIB}" ]]; then
    echo "missing hook library: ${LOCAL_LIB}" >&2
    echo "run ./build_ohos.sh first" >&2
    exit 1
fi

"${HDC_BIN}" shell "mkdir -p '${REMOTE_DIR}' '${TRACE_DIR}'"
"${HDC_BIN}" file send "${LOCAL_LIB}" "${REMOTE_DIR}/liballoc_hook.so" >/dev/null
"${HDC_BIN}" shell "cd '${REMOTE_DIR}' && chmod 755 ./liballoc_hook.so && \
    export LD_LIBRARY_PATH=.; \
    export BACKTRACE_MIN_SIZE='${MIN_SIZE}'; \
    export BACKTRACE_DUMP_SIGNAL='${DUMP_SIGNAL}'; \
    LD_PRELOAD=./liballoc_hook.so ${REMOTE_CMD}"
