#!/usr/bin/env python3
"""Wrap a raw OHOS ftrace and overlay HAIO CSV counters for Perfetto."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys


def wrap_ftrace(template: Path, raw: Path, output: Path) -> None:
    text = template.read_text(errors="replace")
    marker = "<!-- BEGIN TRACE -->"
    if marker not in text:
        raise ValueError(f"template lacks {marker}")
    ftrace = raw.read_text(errors="replace")
    if "</script>" in ftrace.lower():
        raise ValueError("raw ftrace contains </script>")
    prefix = text.split(marker, 1)[0] + marker + "\n"
    output.write_text(
        prefix
        + '  <script class="trace-data" type="application/text">\n'
        + ftrace.rstrip("\n")
        + "\n  </script>\n"
        + '  <script class="trace-data" type="application/text">\n'
        + '{"traceEvents": [], "metadata": {"clock-domain": "SYSTRACE"}}\n'
        + "  </script>\n<!-- END TRACE -->\n</body>\n</html>\n"
    )


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--trace", type=Path, required=True, help="raw .ftrace or existing trace HTML")
    p.add_argument("--csv", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True, help="merged Perfetto HTML")
    p.add_argument("--template", type=Path, default=Path(__file__).with_name("output.html"))
    p.add_argument("--pid", type=int, help="workload PID for HTML counter attachment")
    p.add_argument("--group-name", default="Memory Counters")
    p.add_argument("--track-prefix")
    args = p.parse_args(argv)
    wrapped = args.trace
    if args.trace.suffix.lower() not in {".html", ".htm"}:
        wrapped = args.output.with_name(args.output.stem + ".raw.html")
        wrap_ftrace(args.template, args.trace, wrapped)
    cmd = [sys.executable, str(Path(__file__).with_name("merge_csv_to_perfetto.py")),
           "--trace", str(wrapped), "--csv", str(args.csv), "--output", str(args.output),
           "--group-name", args.group_name]
    if args.pid is not None:
        cmd += ["--html-pid", str(args.pid)]
    if args.track_prefix:
        cmd += ["--track-prefix", args.track_prefix]
    return subprocess.call(cmd)


if __name__ == "__main__":
    raise SystemExit(main())
