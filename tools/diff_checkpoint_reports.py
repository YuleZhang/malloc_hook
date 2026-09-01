#!/usr/bin/env python3
"""Compare two alloc_hook checkpoint reports and show what grew between them.

The reports this consumes are the ones the hook writes when it receives its
checkpoint signal (``<prefix>.signal.pid_<pid>.seq_<n>.time_<t>.txt``).  Take one
at the end of an early iteration of a repeating workload and one at the end of a
later iteration; whatever grew in between is what that workload does not release
per iteration.

Both reports must come from the same process, i.e. carry the same ``pid_``: live
allocations from two different processes describe two different heaps and their
difference means nothing.

Usage:
    diff_checkpoint_reports.py EARLY.txt LATE.txt [--iterations N] [--top N]

``--iterations`` is the number of workload iterations between the two
checkpoints; passing it turns the totals into a per-iteration figure, which is
the number worth acting on.
"""

import argparse
import re
import sys
from collections import OrderedDict

ENTRY_RE = re.compile(
    r"^alloc_size:(?P<kb>[0-9.]+)KB\s+"
    r"alloc_type:(?P<type>\S+)\s+"
    r"alloc_num:(?P<num>\d+)\s+"
    r"alloc_time:(?P<time>.*)$"
)
HEADER_KEYS = (
    "current host used",
    "omitted_without_stack",
    "rss_breakdown",
    "rss_by_mapping",
    "hook_overhead",
)


def parse(path):
    """-> (header_lines, {stack: [bytes, count]})."""
    header = []
    groups = OrderedDict()
    pending_key = None
    pending = None
    in_body = False

    with open(path, "r", errors="replace") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            if not in_body:
                if line.startswith("+++"):
                    in_body = True
                elif any(line.startswith(key) for key in HEADER_KEYS):
                    header.append(line)
                continue

            match = ENTRY_RE.match(line)
            if match:
                if pending_key is not None:
                    _commit(groups, pending_key, pending)
                pending = (
                    float(match.group("kb")) * 1024.0 * int(match.group("num")),
                    int(match.group("num")),
                    match.group("type"),
                )
                pending_key = []
                continue

            if pending_key is None:
                continue
            if line.startswith("#") or line.startswith("<no stack"):
                pending_key.append(line)
            elif not line.strip():
                _commit(groups, pending_key, pending)
                pending_key = None
                pending = None

    if pending_key is not None:
        _commit(groups, pending_key, pending)
    return header, groups


def _commit(groups, frames, entry):
    if entry is None:
        return
    total_bytes, count, alloc_type = entry
    # Frames alone would merge a host allocation with a same-stack mmap.
    key = (alloc_type, "\n".join(frames))
    slot = groups.setdefault(key, [0.0, 0])
    slot[0] += total_bytes
    slot[1] += count


def header_value(header, prefix):
    for line in header:
        if line.startswith(prefix):
            return line
    return None


def totals_of(header):
    """-> (host_mb, dma_mb) from the "current host used" line, or (None, None)."""
    line = header_value(header, "current host used")
    if line is None:
        return (None, None)
    match = re.search(
        r"current host used: ([0-9.]+)MB, current dma used ([0-9.]+)MB", line
    )
    if match is None:
        return (None, None)
    return (float(match.group(1)), float(match.group(2)))


def warn_if_phase_mismatched(early_header, late_header):
    """Two reports are only comparable if taken at the same point of the cycle.

    A checkpoint is delivered asynchronously, so a trigger that sits next to a
    large allocation can land on either side of it. When that happens to only one
    of the two reports, the difference is dominated by that allocation rather than
    by anything the workload leaked -- measured on a real run: 12.7MB dma in one
    report and 214.8MB in the other, from the same trigger line.

    Heuristic on purpose: there is no way to know the intended instant, so this
    reports the step and leaves the judgement to the reader.
    """
    early_host, early_dma = totals_of(early_header)
    late_host, late_dma = totals_of(late_header)
    if early_host is None or late_host is None:
        return
    print(
        "  delta: host %+.3fMB  dma %+.3fMB"
        % (late_host - early_host, late_dma - early_dma)
    )
    for name, before, after in (
        ("host", early_host, late_host),
        ("dma", early_dma, late_dma),
    ):
        smaller = min(before, after)
        if smaller > 1.0 and abs(after - before) > smaller:
            print(
                "  WARNING: %s totals differ by more than 100%% (%.3f -> %.3f MB)."
                % (name, before, after)
            )
            print(
                "           That is a step, not a leak: the two reports were most"
                " likely taken at\n"
                "           different points of the cycle. Pick a trigger further"
                " from any large\n"
                "           allocation and confirm both reports show a comparable"
                " total."
            )
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Show what grew between two alloc_hook checkpoint reports."
    )
    parser.add_argument("early", help="report from the earlier checkpoint")
    parser.add_argument("late", help="report from the later checkpoint")
    parser.add_argument(
        "--iterations",
        type=int,
        default=0,
        help="workload iterations between the two checkpoints; enables a "
        "per-iteration column",
    )
    parser.add_argument("--top", type=int, default=20, help="how many stacks to print")
    args = parser.parse_args()

    early_pid = re.search(r"\.pid_(\d+)\.", args.early)
    late_pid = re.search(r"\.pid_(\d+)\.", args.late)
    if early_pid and late_pid and early_pid.group(1) != late_pid.group(1):
        sys.stderr.write(
            "refusing to compare reports from different processes: pid %s vs %s\n"
            % (early_pid.group(1), late_pid.group(1))
        )
        return 2

    # The peak report describes the instant the process topped out and lists only
    # the snapshot, so differencing it against a live report measures nothing.
    for path in (args.early, args.late):
        if ".exit." in path:
            sys.stderr.write(
                "refusing to use %s: an .exit. report is the peak snapshot, not a "
                "live set. Use .signal. or .exit_live.\n" % path
            )
            return 2

    early_header, early_groups = parse(args.early)
    late_header, late_groups = parse(args.late)

    print("== totals ==")
    for prefix in HEADER_KEYS:
        before = header_value(early_header, prefix)
        after = header_value(late_header, prefix)
        if before is None and after is None:
            continue
        print("  early: %s" % before)
        print("  late : %s" % after)
    warn_if_phase_mismatched(early_header, late_header)

    deltas = []
    shrunk_bytes = 0.0
    for key in set(early_groups) | set(late_groups):
        before = early_groups.get(key, [0.0, 0])
        after = late_groups.get(key, [0.0, 0])
        byte_delta = after[0] - before[0]
        count_delta = after[1] - before[1]
        if byte_delta > 0 or count_delta > 0:
            deltas.append((byte_delta, count_delta, before, after, key))
        elif byte_delta < 0:
            shrunk_bytes += byte_delta
    deltas.sort(key=lambda item: item[0], reverse=True)

    grown_bytes = sum(item[0] for item in deltas)
    print(
        "== %d stack(s) grew, %+.1f KB total%s ==" % (
            len(deltas),
            grown_bytes / 1024.0,
            ""
            if not args.iterations
            else " (%+.1f KB per iteration)"
            % (grown_bytes / 1024.0 / args.iterations),
        )
    )
    # Printed next to it because growth alone is easy to misread as a net leak: a
    # workload that reallocates the same buffers at different sizes shows large
    # growth on some stacks and matching shrinkage on others. Only the net figure
    # is comparable with the "current host used" line above.
    print(
        "   other stacks shrank %+.1f KB; net across all stacks %+.1f KB"
        % (shrunk_bytes / 1024.0, (grown_bytes + shrunk_bytes) / 1024.0)
    )
    print()

    for byte_delta, count_delta, before, after, key in deltas[: args.top]:
        alloc_type, frames = key
        per_iter = (
            ""
            if not args.iterations
            else "  per_iter=%+.2fKB/%+.1f" % (
                byte_delta / 1024.0 / args.iterations,
                count_delta / float(args.iterations),
            )
        )
        print(
            "%+.2fKB  count %d -> %d (%+d)  type=%s%s"
            % (
                byte_delta / 1024.0,
                before[1],
                after[1],
                count_delta,
                alloc_type,
                per_iter,
            )
        )
        print(frames)
        print()

    if not deltas:
        print("Nothing grew. Either the workload releases what it takes, or the "
              "leak is below BACKTRACE_MIN_SIZE and is only visible in the "
              "omitted_without_stack line above.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
