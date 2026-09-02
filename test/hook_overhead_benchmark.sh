#!/usr/bin/env bash
# Guards against allocation-path performance regressions.
#
# Runs one fixed workload on a device twice -- once without the hook, once with
# it preloaded -- and fails when the hooked run is more than a configured
# percentage slower. The tracker sits on every malloc and every free, so a
# change that looks harmless can multiply the cost of the hot path; this is the
# gate that catches that before it is committed.
#
# The workload is deliberately NOT part of this repository. It is an
# application-sized binary plus its inputs, which is far too large to vendor and
# specific to whoever is measuring. Everything site-specific -- which device,
# which workload, how to reach the adb server -- is supplied through the
# environment or through a local, untracked config file. Nothing in this script
# names a host, a network address, or a user's directory.
#
# Usage:
#   test/hook_overhead_benchmark.sh [--require] [--rounds N] [--max-regression PCT]
#
# Configuration (environment, or the config file below):
#   ALLOC_HOOK_BENCH_CONFIG     Shell fragment sourced before anything else.
#                               Default: <repo>/.bench.local (untracked).
#                               Use it for the site-specific values, including
#                               ADB_SERVER_SOCKET if the adb server is remote.
#   ALLOC_HOOK_BENCH_WORKLOAD   Required. tar archive, or a directory, holding
#                               everything the workload needs on the device.
#   ALLOC_HOOK_BENCH_CMD        Required. Command run inside the staged
#                               directory, e.g. './app -n 3 -p input'.
#   ALLOC_HOOK_BENCH_METRIC_RE  Required. Extended regex with one capture group
#                               selecting the number to compare, e.g.
#                               'total_time: ([0-9]+)ms'. Lower must be better.
#   ALLOC_HOOK_BENCH_ENV        Optional. Extra shell run before the command,
#                               e.g. 'export FOO=1; export BAR=2;'.
#   ALLOC_HOOK_BENCH_DEVICE     adb target: a serial, or host:port. Skips
#                               discovery when set.
#   ALLOC_HOOK_BENCH_MOTD       File scanned for candidate devices when no
#                               device is set, matching lines of the form
#                               '<name>: adb connect <host>:<port>'.
#                               Default: /etc/motd.
#   ALLOC_HOOK_BENCH_LIB        Hook library to preload.
#                               Default: <repo>/out/lib/liballoc_hook.so.
#   ALLOC_HOOK_BENCH_REMOTE_DIR Staging directory on the device.
#                               Default: /data/local/tmp/alloc_hook_bench.
#   ALLOC_HOOK_BENCH_ROUNDS     Paired runs per arm. Default 3.
#   ALLOC_HOOK_BENCH_MAX_REGRESSION_PCT
#                               Failure threshold in percent. Default 35.
#
# Exit status:
#   0  within the threshold, or skipped because nothing is configured
#   1  regression above the threshold, or a run/setup error
#
# Not every machine has a device list or a staged workload, so an unconfigured
# run reports SKIP and succeeds. Pass --require to turn that into a failure,
# which is what an automated gate should do.

set -uo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

require_config=0
cli_rounds=""
cli_max=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --require) require_config=1; shift ;;
        --rounds) cli_rounds="${2:-}"; shift 2 ;;
        --max-regression) cli_max="${2:-}"; shift 2 ;;
        -h|--help) sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

config_file="${ALLOC_HOOK_BENCH_CONFIG:-${REPO_ROOT}/.bench.local}"
if [[ -f "${config_file}" ]]; then
    # shellcheck disable=SC1090
    source "${config_file}"
fi

workload="${ALLOC_HOOK_BENCH_WORKLOAD:-}"
run_cmd="${ALLOC_HOOK_BENCH_CMD:-}"
metric_re="${ALLOC_HOOK_BENCH_METRIC_RE:-}"
extra_env="${ALLOC_HOOK_BENCH_ENV:-}"
device="${ALLOC_HOOK_BENCH_DEVICE:-}"
motd="${ALLOC_HOOK_BENCH_MOTD:-/etc/motd}"
hook_lib="${ALLOC_HOOK_BENCH_LIB:-${REPO_ROOT}/out/lib/liballoc_hook.so}"
remote_dir="${ALLOC_HOOK_BENCH_REMOTE_DIR:-/data/local/tmp/alloc_hook_bench}"
rounds="${cli_rounds:-${ALLOC_HOOK_BENCH_ROUNDS:-3}}"
max_regression="${cli_max:-${ALLOC_HOOK_BENCH_MAX_REGRESSION_PCT:-35}}"

skip() {
    if [[ ${require_config} -eq 1 ]]; then
        echo "FAIL: $* (--require was given)" >&2
        exit 1
    fi
    echo "SKIP: $*"
    echo "      See the configuration block at the top of this script."
    exit 0
}

die() { echo "FAIL: $*" >&2; exit 1; }

command -v adb >/dev/null 2>&1 || skip "adb not found in PATH"
[[ -n "${workload}" ]] || skip "ALLOC_HOOK_BENCH_WORKLOAD is not set"
[[ -e "${workload}" ]] || skip "workload not found: ${workload}"
[[ -n "${run_cmd}" ]] || skip "ALLOC_HOOK_BENCH_CMD is not set"
[[ -n "${metric_re}" ]] || skip "ALLOC_HOOK_BENCH_METRIC_RE is not set"
[[ -f "${hook_lib}" ]] || skip "hook library not found: ${hook_lib} (build it first)"

# Discover a device when one was not named.
#
# Candidates come from a plain-text list in the site's own format -- the same
# file an operator reads to find a device -- rather than from anything baked in
# here. Any unreachable entry is skipped instead of failing the run, because
# such a list routinely outlives the devices on it.
if [[ -z "${device}" ]]; then
    candidates=()
    if [[ -r "${motd}" ]]; then
        # '<name>: adb connect <host>:<port>' -> '<host>:<port>'
        while read -r endpoint; do
            [[ -n "${endpoint}" ]] && candidates+=("${endpoint}")
        done < <(sed -nE 's/.*adb connect[[:space:]]+([^[:space:]]+:[0-9]+).*/\1/p' "${motd}" | sort -u)
    fi
    if [[ ${#candidates[@]} -gt 0 ]]; then
        # Shuffled so repeated runs spread over the pool instead of hammering
        # whichever entry happens to be listed first.
        while read -r endpoint; do
            echo "trying ${endpoint} ..."
            if adb connect "${endpoint}" 2>&1 | grep -qiE "connected to"; then
                if adb -s "${endpoint}" shell true >/dev/null 2>&1; then
                    device="${endpoint}"
                    break
                fi
            fi
        done < <(printf '%s\n' "${candidates[@]}" | shuf)
    fi
fi
if [[ -z "${device}" ]]; then
    # Fall back to an already-attached device, but only when there is exactly
    # one: picking arbitrarily from several would silently measure whichever
    # happened to be enumerated first.
    mapfile -t attached < <(adb devices | awk 'NR>1 && $2=="device" {print $1}')
    if [[ ${#attached[@]} -eq 1 ]]; then
        device="${attached[0]}"
    elif [[ ${#attached[@]} -gt 1 ]]; then
        skip "several devices attached; set ALLOC_HOOK_BENCH_DEVICE to choose one"
    fi
fi
[[ -n "${device}" ]] || skip "no usable device found (looked in ${motd} and adb devices)"

adb_sh() { adb -s "${device}" shell "$@"; }

echo "device:        ${device}"
echo "workload:      ${workload}"
echo "hook library:  ${hook_lib}"
echo "rounds:        ${rounds} per arm"
echo "fail above:    ${max_regression}% slower"
echo

# Stage the workload and the hook.
adb_sh "rm -rf ${remote_dir} && mkdir -p ${remote_dir}" >/dev/null 2>&1 ||
    die "cannot create ${remote_dir} on ${device}"
if [[ -d "${workload}" ]]; then
    adb -s "${device}" push "${workload}/." "${remote_dir}" >/dev/null ||
        die "pushing workload directory failed"
else
    remote_archive="${remote_dir}/$(basename "${workload}")"
    adb -s "${device}" push "${workload}" "${remote_archive}" >/dev/null ||
        die "pushing workload archive failed"
    adb_sh "cd ${remote_dir} && tar -xf $(basename "${workload}") && rm -f $(basename "${workload}")" ||
        die "extracting workload archive failed"
fi
adb -s "${device}" push "${hook_lib}" "${remote_dir}/" >/dev/null ||
    die "pushing hook library failed"
hook_name=$(basename "${hook_lib}")
adb_sh "cd ${remote_dir} && chmod 755 * 2>/dev/null; true" >/dev/null 2>&1

# One timed run. `preload` is empty for the baseline arm.
run_once() {
    local preload="$1" out metric
    out=$(adb_sh "cd ${remote_dir}; unset LD_PRELOAD; export LD_LIBRARY_PATH=.; ${extra_env} ${preload} ${run_cmd}" 2>/dev/null)
    metric=$(printf '%s\n' "${out}" | sed -nE "s/.*${metric_re}.*/\1/p" | head -1)
    if [[ -z "${metric}" ]]; then
        echo "could not extract a metric from the run output" >&2
        printf '%s\n' "${out}" | tail -20 >&2
        return 1
    fi
    printf '%s' "${metric}"
}

baseline_samples=()
hooked_samples=()
for ((round = 1; round <= rounds; round++)); do
    # Interleaved rather than all-baseline-then-all-hooked: a device warms up
    # and throttles over a run of several minutes, and that drift would
    # otherwise land entirely on whichever arm went second.
    b=$(run_once "") || die "baseline run ${round} failed"
    h=$(run_once "LD_PRELOAD=${hook_name}") || die "hooked run ${round} failed"
    baseline_samples+=("${b}")
    hooked_samples+=("${h}")
    printf 'round %d: baseline %s, hooked %s\n' "${round}" "${b}" "${h}"
done

read -r baseline_mean hooked_mean regression <<<"$(
    awk -v b="${baseline_samples[*]}" -v h="${hooked_samples[*]}" '
    BEGIN {
        n = split(b, bs, " "); split(h, hs, " ")
        for (i = 1; i <= n; i++) { bt += bs[i]; ht += hs[i] }
        bm = bt / n; hm = ht / n
        printf "%.1f %.1f %.2f", bm, hm, (hm - bm) / bm * 100
    }'
)"

echo
printf 'baseline mean: %s\n' "${baseline_mean}"
printf 'hooked mean:   %s\n' "${hooked_mean}"
printf 'regression:    %s%% (threshold %s%%)\n' "${regression}" "${max_regression}"

if awk -v r="${regression}" -v m="${max_regression}" 'BEGIN { exit !(r > m) }'; then
    echo "FAIL: the hook slows this workload by more than ${max_regression}%."
    exit 1
fi
echo "PASS"
