import argparse
import logging
import os
import platform
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

from tabulate import tabulate

from dataclasses import dataclass, field
from typing import Tuple


# ---------------------------------------------------------------------------
# Symbol- and layout-matching policy.
#
# Which symbols mark an "entry point", and which workspace directories hold
# third-party code, are properties of the project being profiled -- not of this
# script. They are therefore configuration with neutral defaults rather than
# hardcoded names, so the script can be shared without carrying one project's
# internal namespace and function names.
#
# Set them with --namespace / --entry-prefix / --fallback-prefix /
# --toolkit-dir / --exclude-path, or via MEMSTACK_* environment variables.
# ---------------------------------------------------------------------------
@dataclass
class SymbolPolicy:
    # C++ namespace prefix (e.g. "myproj::") stripped from reported names.
    # Empty means "do not filter by namespace".
    namespace: str = ""
    # Symbols starting with this are treated as the allocation entry point.
    # Empty disables the strong match and leaves only the fallbacks.
    entry_prefix: str = ""
    # Secondary matches, used when no entry_prefix frame is present.
    fallback_prefixes: Tuple[str, ...] = ()
    # Workspace subdirectory holding vendored/third-party sources.
    toolkit_dir: str = "third_party"
    # Workspace-relative paths never chosen as a fallback frame. A path with a
    # file extension matches exactly; anything else matches as a directory.
    excluded_paths: Tuple[str, ...] = ()


_SYMBOL_POLICY = SymbolPolicy()


def symbol_policy() -> SymbolPolicy:
    return _SYMBOL_POLICY


def set_symbol_policy(policy: SymbolPolicy) -> None:
    global _SYMBOL_POLICY
    _SYMBOL_POLICY = policy


def strip_namespace(symbol: str, namespace: str) -> str:
    """Drop the configured namespace prefix, if the symbol carries it."""
    if namespace and symbol.startswith(namespace):
        return symbol.split(namespace, 1)[1]
    return symbol


def in_project_namespace(symbol: str) -> bool:
    """Whether a frame belongs to the project, for call-path reporting.

    With no namespace configured every frame qualifies: that is the neutral
    default, and filtering to nothing would silently empty the call path.
    """
    namespace = symbol_policy().namespace
    return symbol.startswith(namespace) if namespace else True


def _env_tuple(name: str) -> Tuple[str, ...]:
    raw = os.environ.get(name, "")
    return tuple(item for item in (part.strip() for part in raw.split(",")) if item)


def policy_from_args(args) -> SymbolPolicy:
    """CLI wins over environment, environment over the neutral default."""
    default = SymbolPolicy()
    return SymbolPolicy(
        namespace=args.namespace
        if args.namespace is not None
        else os.environ.get("MEMSTACK_NAMESPACE", default.namespace),
        entry_prefix=args.entry_prefix
        if args.entry_prefix is not None
        else os.environ.get("MEMSTACK_ENTRY_PREFIX", default.entry_prefix),
        fallback_prefixes=tuple(args.fallback_prefix)
        if args.fallback_prefix
        else _env_tuple("MEMSTACK_FALLBACK_PREFIXES") or default.fallback_prefixes,
        toolkit_dir=args.toolkit_dir
        if args.toolkit_dir is not None
        else os.environ.get("MEMSTACK_TOOLKIT_DIR", default.toolkit_dir),
        excluded_paths=tuple(args.exclude_path)
        if args.exclude_path
        else _env_tuple("MEMSTACK_EXCLUDE_PATHS") or default.excluded_paths,
    )


def resolve_log_file(file_arg: str) -> str:
    """
    Return the log file path. If not provided, pick the last file in ./trace/ (sorted).
    """
    if file_arg:
        return file_arg
    trace_dir = os.path.join(os.getcwd(), "trace")
    if not os.path.isdir(trace_dir):
        print("Error: --file未指定且默认trace目录不存在: ./trace/")
        sys.exit(1)
    files = [f for f in os.listdir(trace_dir) if os.path.isfile(os.path.join(trace_dir, f))]
    if not files:
        print("Error: ./trace/ 目录下没有可用的日志文件")
        sys.exit(1)
    files.sort()
    default_file = os.path.join(trace_dir, files[-1])
    print(f"未指定--file，使用默认日志文件: {default_file}")
    return default_file


def locate_symbol_binary(binary_name: str, start_dir: str, prefer: str = "",
                         fallback_dir: str = "") -> str:
    """
    Locate a non-stripped binary by name, searching start_dir (then fallback_dir).
    Exits the program with an error if the binary or a symbolized version is not found.

    start_dir should be the workspace root, not the current working directory: the caller
    knows which tree the trace came from, and searching from wherever the process happens
    to have been launched either finds nothing or finds an unrelated build.

    prefer is an optional substring; when several unstripped copies exist (a build tree and
    an install tree commonly both keep one) it selects among them without prompting.
    """
    if not binary_name:
        return ""

    searched = []
    candidates = []
    for root in [d for d in (start_dir, fallback_dir) if d]:
        if root in searched or not os.path.isdir(root):
            continue
        searched.append(root)
        find_cmd = ["find", root, "-name", binary_name]
        try:
            output = subprocess.check_output(find_cmd, stderr=subprocess.STDOUT, text=True)
        except subprocess.CalledProcessError as exc:
            print(f"Warning: search for {binary_name} under {root} failed: {exc.output.strip()}")
            continue
        candidates = [line.strip() for line in output.splitlines() if line.strip()]
        if candidates:
            if root != start_dir:
                print(f"Note: '{binary_name}' not under {start_dir}; used fallback {root}")
            break

    if not candidates:
        print(f"Error: could not find '{binary_name}' under {' or '.join(searched)}")
        sys.exit(1)

    symbolized = []
    for path in candidates:
        try:
            file_output = subprocess.check_output(["file", path], stderr=subprocess.STDOUT, text=True)
        except subprocess.CalledProcessError as exc:
            print(f"Warning: failed to inspect {path}: {exc.output.strip()}")
            continue
        if "not stripped" in file_output:
            symbolized.append(path)

    if symbolized:
        if len(symbolized) == 1:
            return symbolized[0]
        if prefer:
            narrowed = [p for p in symbolized if prefer in p]
            if len(narrowed) == 1:
                print(f"Selected symbol binary matching --select-binary {prefer!r}")
                return narrowed[0]
            if not narrowed:
                print(f"Error: --select-binary {prefer!r} matches none of:")
                for path in symbolized:
                    print(f"  {path}")
                sys.exit(1)
            symbolized = narrowed
        print(f"发现多个包含符号信息的 {binary_name}:")
        return prompt_user_select_binary(symbolized, flag="--select-binary")

    print(
        f"Error: found {len(candidates)} instance(s) of '{binary_name}' "
        "but none contain symbols (file output missing 'not stripped')."
    )
    # sys.exit(1)


def prompt_user_select_binary(candidates, flag: str = ""):
    """Prompt user to select a library from candidates.

    When stdin is not a terminal (a subprocess, a CI job, an agent), prompting would raise
    EOFError *after* the log has already been parsed, discarding all of that work with a
    traceback instead of a diagnosis. So a non-interactive caller is told exactly which flag
    to pass and what the valid values are.
    """
    print("请选择需要解析的动态库:")
    for idx, name in enumerate(candidates, 1):
        print(f"{idx}. {name}")

    if not sys.stdin.isatty():
        hint = f" Pass {flag}=<value> to choose non-interactively." if flag else ""
        print(
            f"Error: stdin is not a terminal, so the selection above cannot be prompted for.{hint}"
        )
        sys.exit(2)

    while True:
        try:
            choice = input("输入序号并回车: ").strip()
        except EOFError:
            hint = f" Pass {flag}=<value> instead." if flag else ""
            print(f"\nError: stdin closed while waiting for a selection.{hint}")
            sys.exit(2)
        if not choice.isdigit():
            print("请输入有效序号")
            continue
        choice_num = int(choice)
        if 1 <= choice_num <= len(candidates):
            return candidates[choice_num - 1]
        print("序号超出范围，请重新输入")


def find_llvm_symbolizer() -> str:
    """
    Locate llvm-symbolizer.

    1) Prefer a binary available in PATH (via `which` semantics).
    2) Fallback to NDK_ROOT/toolchains/llvm/prebuilt/<tag>/bin/llvm-symbolizer,
       where <tag> is derived from the current platform (e.g., linux-x86_64, darwin-x86_64).
    Exits with an error message when the tool cannot be found.
    """
    path_in_env = shutil.which("llvm-symbolizer")
    if path_in_env:
        return path_in_env

    ndk_root = os.getenv("NDK_ROOT")
    if not ndk_root:
        print("Error: llvm-symbolizer not found in PATH and NDK_ROOT is not set.")
        sys.exit(1)

    system = platform.system().lower()
    machine = platform.machine().lower()
    arch_map = {"x86_64": "x86_64", "amd64": "x86_64", "aarch64": "aarch64", "arm64": "aarch64"}
    arch = arch_map.get(machine, machine)
    tag_primary = f"{system}-{arch}"

    prebuilt_root = os.path.join(ndk_root, "toolchains", "llvm", "prebuilt")
    candidate_tags = [tag_primary]
    # Older / Intel-only NDK distributions on macOS ship darwin-x86_64 only.
    if system == "darwin" and arch == "aarch64":
        candidate_tags.append("darwin-x86_64")

    for tag in candidate_tags:
        candidate = os.path.join(prebuilt_root, tag, "bin", "llvm-symbolizer")
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate

    tried = ", ".join(candidate_tags)
    print(
        "Error: llvm-symbolizer not found. "
        f"Tried PATH and {prebuilt_root} with tag(s): {tried}. "
        "Ensure llvm-symbolizer is installed or NDK_ROOT is correctly set."
    )
    sys.exit(1)


def symbolize_frame(llvm_symbolizer_path: str, binary_file: str, address: str) -> str:
    cmd = [llvm_symbolizer_path, "-p", "--obj", binary_file, "0x" + address]
    try:
        output = subprocess.check_output(cmd, stderr=subprocess.STDOUT).decode().strip()
    except subprocess.CalledProcessError as exc:
        output = f"0x{address} <symbolize failed: {exc.output.strip()}>"
    return output


def parse_symbolized_output(resolved: str):
    """
    Parse llvm-symbolizer output, returning (function, file_path, line_no).
    Supports both single-line "func at file:line:col" and two-line outputs.
    """
    logging.debug(f"Parsing symbolized output:\n{resolved}")
    lines = [line.strip() for line in resolved.splitlines() if line.strip()]
    if not lines:
        return ("", "", None)
    function_name = lines[0].split(" ")
    if len(function_name) > 1:
        function_name = function_name[1]
        location_part = function_name
    else:
        location_part = function_name
    if " at " in lines[0]:
        function_name, location_part = lines[0].split(" at ", 1)
    if location_part:
        location_part = location_part.split(" (", 1)[0]
    file_path = ""
    line_no = None
    if location_part:
        parts = location_part.rsplit(":", 2)
        if len(parts) == 3 and parts[-1].isdigit() and parts[-2].isdigit():
            file_path = parts[0]
            line_no = int(parts[-2])
        elif len(parts) >= 2 and parts[-1].isdigit():
            file_path = ":".join(parts[:-1])
            line_no = int(parts[-1])
    sub_file_path = file_path.split(" ")
    if len(sub_file_path) > 1:
        file_path = sub_file_path[-1]
    logging.debug(f"Parsed frame: function='{function_name}', file='{file_path}', line={line_no}")
    return (function_name, file_path, line_no)


def read_code_line(file_path: str, line_no: int) -> str:
    """
    Safely read a specific line from a file. Returns an empty string on failure.
    """
    if not file_path or not line_no:
        return ""
    try:
        with open(file_path, "r") as fp:
            for idx, line in enumerate(fp, 1):
                if idx == line_no:
                    return line.strip()
    except OSError:
        return ""
    return ""


def _strip_comments(code: str) -> str:
    """Remove C/C++ style // and /* */ comments from a single line best-effort."""
    if not code:
        return ""
    # remove // comments
    code_no_line = re.sub(r"//.*", "", code)
    # remove /* ... */ on the same line
    code_no_block = re.sub(r"/\\*.*?\\*/", "", code_no_line)
    return code_no_block


def _split_call_arguments(arg_string: str):
    """Split a function call argument string into top-level arguments."""
    args = []
    current = []
    depth = 0
    in_string = False
    string_char = ""
    for ch in arg_string:
        if ch in ("'", '"'):
            if in_string and ch == string_char:
                in_string = False
            elif not in_string:
                in_string = True
                string_char = ch
        if in_string:
            current.append(ch)
            continue
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth = max(0, depth - 1)
        if ch == "," and depth == 0:
            args.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    if current:
        args.append("".join(current).strip())
    return args


def _clean_token(token: str) -> str:
    """Normalize a token by stripping casts, pointers, and outer parentheses."""
    if not token:
        return ""
    t = token.strip()
    while t.startswith("(") and t.endswith(")") and len(t) > 2:
        t = t[1:-1].strip()
    t = t.lstrip("&*")
    return t


def _is_literal(token: str) -> bool:
    """Check if a token looks like a literal or initializer."""
    if not token:
        return True
    t = token.strip()
    if t in {"true", "false", "nullptr", "NULL"}:
        return True
    if re.fullmatch(r"[+-]?\d+(\.\d+)?([eE][+-]?\d+)?", t):
        return True
    if t.startswith('"') and t.endswith('"'):
        return True
    if t.startswith("'") and t.endswith("'"):
        return True
    if t.startswith("{") and t.endswith("}"):
        return True
    tail = t.split("::")[-1]
    if re.fullmatch(r"[A-Z0-9_]+", tail):
        return True
    if tail.startswith(("CV_", "COLOR_")):
        return True
    return False


def _find_assignment_target(text: str) -> str:
    """Find assignment target on LHS of '=' avoiding comparisons."""
    if not text or "=" not in text:
        return ""
    if "==" in text or ">=" in text or "<=" in text or "!=" in text:
        # crude skip of comparisons
        return ""
    stripped = text.lstrip()
    if stripped.startswith("for(") or stripped.startswith("for "):
        return ""
    lhs = text.split("=", 1)[0].strip()
    if not lhs:
        return ""
    tokens = lhs.split()
    candidate = tokens[-1] if tokens else lhs
    candidate = candidate.rstrip("&*")
    candidate = candidate.split("::")[-1]
    candidate = candidate.replace("->", ".")
    candidate = candidate.split("<")[0]
    return candidate


def extract_variable_name(code_line: str, prev_lines=None) -> str:
    """
    Heuristically extract a variable name from a line of code, favoring outputs.
    """
    prev_lines = prev_lines or []
    if not code_line and not prev_lines:
        return ""
    line = _strip_comments(code_line).strip().rstrip(";") if code_line else ""
    cleaned_prev = []
    for pl in prev_lines:
        raw = _strip_comments(pl).strip() if pl else ""
        if not raw:
            continue
        cleaned_prev.append((raw.rstrip(";"), raw.endswith(";")))
    combined_parts = [line]
    for cleaned, had_semicolon in cleaned_prev:
        if not cleaned:
            continue
        if had_semicolon:
            break
        combined_parts.insert(0, cleaned)
    combined = " ".join(combined_parts).strip()

    # Assignment: prefer current line, then combined for multi-line cases
    for text in (line, combined):
        search_text = text
        if ";" in search_text:
            search_text = search_text.rsplit(";", 1)[-1].strip()
        lhs = _find_assignment_target(search_text)
        if lhs:
            return lhs

    # Function or method call: pick the most likely output argument
    call_line = line if "(" in line else combined
    open_idx = call_line.find("(")
    close_idx = call_line.rfind(")")
    if open_idx != -1:
        func_name = call_line[:open_idx].strip()
        if func_name.startswith("cv::Mat "):
            return func_name.split()[-1]
        func_base = func_name.replace("->", ".").split("::")[-1]
        func_base = func_base.split(".")[-1] if "." in func_base else func_base
        norm_func = func_name.replace("->", ".")
        receiver_name_raw = func_name.rsplit(".", 1)[0] if "." in func_name else ""
        if close_idx == -1 or close_idx <= open_idx:
            args_str = call_line[open_idx + 1 :]
        else:
            args_str = call_line[open_idx + 1 : close_idx]
        args = _split_call_arguments(args_str)
        output_first_funcs = {"resize", "convertTo", "alloc_mat", "alloc_tensor", "alloc_buffer", "clone"}
        receiver_output_funcs = {"create", "resize", "clone", "alloc", "allocate"}
        output_arg_preference = {"cvtColor": 1}
        if receiver_name_raw and func_base in receiver_output_funcs:
            return receiver_name_raw
        candidates = []
        for arg in args:
            token = _clean_token(arg)
            if not token or "(" in token:  # skip nested calls
                continue
            if _is_literal(token):
                continue
            token_output = token.lstrip("&*")
            token_norm = token_output.replace("->", ".")
            candidates.append((token_output or token, token_norm))
        if candidates:
            preferred_idx = output_arg_preference.get(func_base)
            if preferred_idx is not None and preferred_idx < len(candidates):
                return candidates[preferred_idx][0]
            if func_base in output_first_funcs:
                return candidates[0][0]
            return candidates[-1][0]
        if receiver_name_raw:
            return receiver_name_raw

    # Fallback: grab trailing identifier
    tail_match = re.search(r"([A-Za-z_][\w\.->:]*)\s*$", line)
    if tail_match:
        token = tail_match.group(1)
        token = token.replace("->", ".").lstrip("&*")
        return token
    return ""


def to_markdown_table(headers, rows):
    """
    Build a Markdown table string from headers and rows.
    """
    if not headers:
        return ""
    lines = []
    lines.append("| " + " | ".join(headers) + " |")
    lines.append("| " + " | ".join("---" for _ in headers) + " |")
    if rows:
        for row in rows:
            cells = [str(cell) if cell is not None else "" for cell in row]
            lines.append("| " + " | ".join(cells) + " |")
    else:
        lines.append("| " + " | ".join("" for _ in headers) + " |")
    return "\n".join(lines)

def first_non_system_so(stack_info):
    """Return the first non-libc/libdmabufheap shared object name from a stack trace."""
    for stack_line in stack_info:
        parts = stack_line.split()
        if len(parts) < 3:
            continue
        so_name = os.path.basename(parts[2])
        if "libc.so" in so_name or "libdmabufheap.so" in so_name:
            continue
        return so_name
    return ""


def aggregate_allocations_by_source(allocations):
    """Aggregate allocation sizes (in MB) by source library for the given allocations."""
    alloc_source = {}
    for alloc in allocations:
        so_name = first_non_system_so(alloc.get("stack_info") or [])
        if not so_name:
            continue
        if so_name not in alloc_source:
            alloc_source[so_name] = {"host": 0, "dma": 0, "mmap": 0}
        alloc_source[so_name][alloc["alloc_type"]] += alloc["alloc_size"] * alloc["alloc_num"] / 1024.0
    return alloc_source


def build_alloc_source_rows(alloc_source):
    """Build tabular rows with a total row for allocation source summary."""
    table_data = []
    for library, values in alloc_source.items():
        row = [library] + [values.get("host", 0), values.get("dma", 0), values.get("mmap", 0)]
        row.append(sum(values.values()))
        table_data.append(row)
    table_data.append(
        [
            "Total",
            sum(v.get("host", 0) for v in alloc_source.values()),
            sum(v.get("dma", 0) for v in alloc_source.values()),
            sum(v.get("mmap", 0) for v in alloc_source.values()),
            sum(sum(v.values()) for v in alloc_source.values()),
        ]
    )
    return table_data


def calculate_totals_kb(allocations):
    """Calculate aggregate allocation sizes (in KB) by type."""
    totals = {"host": 0.0, "dma": 0.0, "mmap": 0.0}
    for alloc in allocations:
        alloc_type = alloc.get("alloc_type")
        if alloc_type in totals:
            totals[alloc_type] += alloc["alloc_size"] * alloc["alloc_num"]
    return totals


def format_total_summary(host_kb, dma_kb, mmap_kb, label):
    """Format a summary string for total allocation sizes."""
    def _fmt(size_kb):
        if size_kb > 1024.0:
            return str(size_kb / 1024.0) + "MB"
        return str(size_kb / 1024.0) + "KB"

    total_allocated_size = host_kb + dma_kb + mmap_kb
    return (
        f"Total Alloc Size of {label}:\nhost={_fmt(host_kb)}, "
        f"mmap={_fmt(mmap_kb)}, dma={_fmt(dma_kb)}, \ntotal_allocated_size={total_allocated_size/1024.0}MB"
    )


def update_type_totals_kb(totals, alloc):
    """Accumulate allocation size (in KB) by type."""
    alloc_type = alloc.get("alloc_type")
    if alloc_type in totals:
        totals[alloc_type] += alloc["alloc_size"] * alloc["alloc_num"]


def build_allocation_header(alloc, index, include_details):
    """Construct the header lines for a single allocation block."""
    if not include_details:
        return []
    return [
        f"Top {index}:",
        f"Alloc Size: {alloc['alloc_size'] * alloc['alloc_num'] /1024.0}MB",
        f"Alloc Type: {alloc['alloc_type']}",
        f"Alloc Num: {alloc['alloc_num']}",
        f"Alloc Time: {alloc['alloc_time']}",
        "Stack Trace:",
    ]


def process_stack_lines(alloc, include_details, binary_file, llvm_symbolizer_path, executor, alloc_source, detail_lines):
    """Process stack trace lines, scheduling symbolization when needed."""
    symbolized_entries = []
    target_so_name = ""
    bin_name = os.path.basename(binary_file) if binary_file else ""
    avoid_dynamic_lib = ["libdmabufheap.so", "libc.so", "libGLES_mali.so", "libOpenCL.so"]
    for idx, stack_line in enumerate(alloc["stack_info"]):
        parts = stack_line.split()
        if len(parts) < 3:
            if include_details:
                detail_lines.append(stack_line)
            continue
        address = parts[1]
        so_path = parts[2]
        so_name = os.path.basename(so_path)
        if not target_so_name and so_name not in avoid_dynamic_lib:
            target_so_name = so_name
            if so_name not in alloc_source:
                alloc_source[so_name] = {"host": 0, "dma": 0, "mmap": 0}
            alloc_source[so_name][alloc["alloc_type"]] += alloc["alloc_size"] * alloc["alloc_num"] / 1024.0
        if not include_details or not binary_file or so_name != bin_name:
            if include_details:
                detail_lines.append(stack_line)
            continue
        future = executor.submit(symbolize_frame, llvm_symbolizer_path, binary_file, address)
        detail_lines.append(future)
        symbolized_entries.append({"future": future, "stack_index": idx})
    return target_so_name, detail_lines, symbolized_entries, bin_name


def resolve_detail_lines(detail_lines):
    """Resolve any futures in detail lines."""
    future_results = {}
    resolved_lines = []
    for line in detail_lines:
        if hasattr(line, "result"):
            resolved = line.result()
            future_results[line] = resolved
        else:
            resolved = line
        resolved_lines.append(resolved)
    return resolved_lines, future_results


def build_frames_from_symbolized(symbolized_entries, future_results):
    """Build ordered frames from symbolization results."""
    frames = []
    for idx, entry in enumerate(symbolized_entries):
        resolved = future_results.get(entry["future"])
        if resolved is None:
            resolved = entry["future"].result()
        func, file_path, line_no = parse_symbolized_output(resolved)
        frames.append(
            {
                "index": entry["stack_index"],
                "function": func,
                "file": file_path,
                "line": line_no,
            }
        )
    frames.sort(key=lambda x: x["index"])
    logging.debug(f"Built frames: \n{frames}")
    return frames


def select_workspace_frame(frames, workspace_root):
    """Pick the first frame that belongs to the workspace, with a backup fallback."""
    selected_frame = None
    backup_frame = None
    workspace_prefixes = [
        os.path.abspath(os.path.join(workspace_root, "src")) + os.sep,
        os.path.abspath(os.path.join(workspace_root, "modules")) + os.sep,
    ]
    policy = symbol_policy()
    dev_toolkit_prefix = (
            os.path.abspath(os.path.join(workspace_root, policy.toolkit_dir)) + os.sep)
    excluded_backup_prefixes = [
        os.path.abspath(os.path.join(workspace_root, relative)) +
        (os.sep if not os.path.splitext(relative)[1] else "")
        for relative in policy.excluded_paths
    ]
    for frame in frames:
        abs_file = os.path.abspath(frame["file"]) if frame["file"] else ""
        logging.debug(f"Checking frame file: {abs_file}")
        if any(abs_file.startswith(prefix) for prefix in workspace_prefixes):
            selected_frame = frame
            break
        if abs_file.startswith(dev_toolkit_prefix) and not any(
            abs_file.startswith(prefix) for prefix in excluded_backup_prefixes
        ):
            backup_frame = frame
    if backup_frame is not None:
        selected_frame = backup_frame
    logging.warning(f"Selected frame: {selected_frame}, Backup frame: {backup_frame}")
    return selected_frame


def find_pipeline_functions(frames):
    """Locate pipeline function names from frames, with a backup heuristic."""

    # Which symbols count as an "entry point" is project-specific, so it is
    # configuration rather than something baked into this script. See
    # SymbolPolicy for the flags that set these.
    policy = symbol_policy()
    SIQ_NS = policy.namespace
    PIPELINE_PREFIX = policy.entry_prefix
    BACKUP_PREFIXES = policy.fallback_prefixes

    pipeline_func_name = None
    pipeline_func_name_backup = None

    for frame in frames:
        func_name = (frame.get("function") or "").strip()
        logging.debug("Checking frame func: %s", func_name)
        if not func_name:
            continue

        for token in func_name.split():
            # 统一：先裁掉参数部分，避免到处 split("(")
            head = token.split("(", 1)[0]

            # 强匹配：入口前缀命中直接返回。空前缀不参与匹配，否则
            # startswith("") 会让第一个符号都算命中。
            if PIPELINE_PREFIX and head.startswith(PIPELINE_PREFIX):
                logging.debug("original func pipeline name: %s", func_name)
                pipeline_func_name = strip_namespace(head, SIQ_NS)
                return pipeline_func_name, pipeline_func_name_backup

            # 备选匹配：只记录第一个 backup
            if pipeline_func_name_backup is None and head.startswith(BACKUP_PREFIXES):
                logging.debug("original fallback func name: %s", func_name)

                pipeline_func_name_backup = strip_namespace(head, SIQ_NS)

                break

    return pipeline_func_name, pipeline_func_name_backup



def append_report_entry(report_entries, alloc, frames, selected_frame, pipeline_func_name, index):
    """Append a single report entry if possible."""
    if report_entries is None:
        return
    if not selected_frame or not selected_frame["file"] or not selected_frame["line"]:
        return
    code_line = read_code_line(selected_frame["file"], selected_frame["line"])
    prev_lines = []
    if selected_frame["line"] > 1:
        prev_lines.append(read_code_line(selected_frame["file"], selected_frame["line"] - 1))
    if selected_frame["line"] > 2:
        prev_lines.append(read_code_line(selected_frame["file"], selected_frame["line"] - 2))
    if selected_frame["line"] > 3:
        prev_lines.append(read_code_line(selected_frame["file"], selected_frame["line"] - 3))
    variable_name = extract_variable_name(code_line, prev_lines) or "<unknown>"
    memory_mb = alloc['alloc_size'] * alloc['alloc_num'] / 1024.0
    logging.debug(f"memory_mb: {memory_mb}, alloc_size: {alloc['alloc_size']}, alloc_num: {alloc['alloc_num']}")
    call_path_funcs = []
    for frame in frames:
        if frame["index"] > selected_frame["index"]:
            continue
        func_name = frame["function"]
        if not func_name or not in_project_namespace(func_name):
            continue
        func_name = func_name.split("(", 1)[0]
        func_name = func_name.split("::")[-1] if "::" in func_name else func_name
        call_path_funcs.append(func_name)
    call_path = " -> ".join(call_path_funcs) if call_path_funcs else "<unknown>"
    code_display = pipeline_func_name or "<unknown>"
    call_site = f"{os.path.basename(selected_frame['file'])}:{selected_frame['line']}"
    report_entries.append(
        {
            "top_index": index,
            "variable": variable_name,
            "code_func": code_display,
            "call_site": call_site,
            "mem_type": alloc["alloc_type"],
            "memory": f"{memory_mb:.2f} MB",
            "call_path": call_path,
        }
    )


def update_pipeline_alloc_map(pipeline_alloc, pipeline_func_name, alloc, target_so_name, bin_name):
    """Update pipeline allocation summary."""
    if pipeline_alloc is None or not pipeline_func_name or target_so_name != bin_name:
        return
    if pipeline_func_name not in pipeline_alloc:
        pipeline_alloc[pipeline_func_name] = {"host": 0, "dma": 0, "mmap": 0}
    pipeline_alloc[pipeline_func_name][alloc['alloc_type']] += alloc['alloc_size'] * alloc["alloc_num"] / 1024.0


def parse_log(log_file, max_show_len=100, binary_file=""):
    # 定义正则模式以提取所需信息
    alloc_pattern = re.compile(r"alloc_size:([\d.]+)KB\s+alloc_type:([A-Za-z]+)\s+alloc_num:(\d+)\s+alloc_time:([\d-]+ [\d:.]+)")
    stack_pattern = re.compile(r"#\d+ .+")

    allocations = []

    with open(log_file, "r") as file:
        lines = file.readlines()

    i = 0
    while i < len(lines):
        alloc_match = alloc_pattern.match(lines[i])
        if alloc_match:
            alloc_size = float(alloc_match.group(1))
            alloc_type = alloc_match.group(2)
            alloc_num = int(alloc_match.group(3))
            alloc_time = alloc_match.group(4)

            stack_info = []
            i += 1
            add_flag = True
            while i < len(lines) and stack_pattern.match(lines[i]):
                stack_info.append(lines[i].strip())
                # if "load_engine_network" in lines[i] or "modelopr::" in lines[i] or "mgb::" in lines[i]:
                #     add_flag = False
                i += 1

            if add_flag and alloc_size > 1.0:
                allocations.append(
                    {
                        "alloc_size": alloc_size,
                        "alloc_type": alloc_type,
                        "alloc_num": alloc_num,
                        "alloc_time": alloc_time,
                        "stack_info": stack_info,
                    }
                )
        else:
            i += 1

    sorted_allocations = sorted(allocations, key=lambda x: x["alloc_size"] * x["alloc_num"], reverse=True)
    if max_show_len is None or max_show_len < 0:
        return sorted_allocations
    return sorted_allocations[:max_show_len]


def main():
    parser = argparse.ArgumentParser(description="Parse memory allocation log and display top allocations.")
    parser.add_argument("-f", "--file", help="Path to the memory allocation log file.")
    parser.add_argument("-m", "--max_show_len", type=int, default=-1, help="Maximum number of allocations to show.")
    parser.add_argument("-w", "--workspace_root", type=str, default=None, help="Workspace root used to locate project source (defaults to current working directory).")
    parser.add_argument("-r", "--report", nargs="?", const="", default=None, help="Generate memory analysis report (Markdown). Optional path (default: ./memory_report.md).")
    parser.add_argument("--no-symbolize", action="store_true", help="Skip stack symbolization/PC parsing (default: symbolize).")
    parser.add_argument("--select-lib", type=str, default=None, help="Name of the shared library to symbolize (e.g. libfoo.so). Skips the interactive prompt; required when stdin is not a terminal and the log names more than one library.")
    parser.add_argument("--select-binary", type=str, default=None, help="Path or path substring of the non-stripped binary to use, when several unstripped copies of --select-lib exist (e.g. a build tree and an install tree).")
    parser.add_argument("--permissive", action="store_true", help="Do not assert when stack frame or pipeline function is missing; log a warning instead.")
    parser.add_argument("--debug", action="store_true", help="Enable debug logging.")
    parser.add_argument("--namespace", type=str, default=None, help="C++ namespace prefix of the project under analysis, e.g. 'myproj::'. Stripped from reported names. Default: none (no namespace filtering).")
    parser.add_argument("--entry-prefix", type=str, default=None, help="Symbol prefix treated as the allocation entry point, e.g. 'myproj::Pipeline'. Default: none, leaving only --fallback-prefix.")
    parser.add_argument("--fallback-prefix", action="append", default=None, metavar="PREFIX", help="Symbol prefix used when no --entry-prefix frame is present. Repeatable.")
    parser.add_argument("--toolkit-dir", type=str, default=None, help="Workspace subdirectory holding third-party sources (default: third_party).")
    parser.add_argument("--exclude-path", action="append", default=None, metavar="PATH", help="Workspace-relative path never chosen as a fallback frame. Repeatable.")
    args = parser.parse_args()
    set_symbol_policy(policy_from_args(args))
    
    symbolize = not args.no_symbolize

    if args.debug:
        logging.basicConfig(level=logging.DEBUG)
    else:
        logging.basicConfig(level=logging.INFO)

    report_requested = args.report is not None
    if report_requested and not symbolize:
        print("Error: --report 需要开启符号化（默认开启），否则无法生成带源信息的报告。")
        sys.exit(1)
    log_file = resolve_log_file(args.file)
    if not os.path.isfile(log_file):
        print(f"Error: 指定的日志文件不存在: {log_file}")
        sys.exit(1)
    max_show_len = args.max_show_len
    binary_file = ""
    workspace_root = os.path.abspath(args.workspace_root) if args.workspace_root else os.getcwd()
    if report_requested:
        report_output_path = (
            os.path.join(os.getcwd(), "memory_report.md")
            if args.report == "" or args.report is None
            else os.path.abspath(args.report)
        )
    else:
        report_output_path = None
    all_allocations = parse_log(log_file, -1, binary_file)
    top_allocations = all_allocations if max_show_len is None or max_show_len < 0 else all_allocations[:max_show_len]
    alloc_source_full = aggregate_allocations_by_source(all_allocations)
    totals_all_kb = calculate_totals_kb(all_allocations)
    candidate_libs = []
    if symbolize:
        seen = set()
        for alloc in top_allocations:
            for stack_line in alloc["stack_info"]:
                parts = stack_line.split()
                if len(parts) < 3:
                    continue
                so_name = os.path.basename(parts[2])
                if "libc.so" in so_name or "libdmabufheap.so" in so_name:
                    continue
                if so_name not in seen:
                    seen.add(so_name)
                    candidate_libs.append(so_name)
        if not candidate_libs:
            print("Error: 日志中未发现可供解析的动态库")
            sys.exit(1)
        if args.select_lib:
            if args.select_lib not in candidate_libs:
                print(f"Error: --select-lib {args.select_lib!r} is not named in this log. Found:")
                for name in candidate_libs:
                    print(f"  {name}")
                sys.exit(1)
            selected_name = args.select_lib
            print(f"Selected library: {selected_name} (--select-lib)")
        elif len(candidate_libs) == 1:
            # No ambiguity to resolve, so do not make the caller resolve it.
            selected_name = candidate_libs[0]
            print(f"Selected library: {selected_name} (only candidate)")
        else:
            selected_name = prompt_user_select_binary(candidate_libs, flag="--select-lib")
        # Search the workspace the trace came from, not wherever this process was launched.
        binary_file = locate_symbol_binary(
            selected_name, workspace_root,
            prefer=args.select_binary or "",
            fallback_dir=os.getcwd(),
        )
        print(f"Using symbol binary: {binary_file}")
        llvm_symbolizer_path = find_llvm_symbolizer()
        print(f"Using llvm-symbolizer: {llvm_symbolizer_path}")
        max_workers = min(32, (os.cpu_count() or 4))
        executor = ThreadPoolExecutor(max_workers=max_workers)
    else:
        llvm_symbolizer_path = ""
        executor = None
    totals_top_kb = {"host": 0.0, "dma": 0.0, "mmap": 0.0}
    pipeline_alloc = {} if symbolize else None
    report_entries = [] if report_requested else None
    top_details = [] if symbolize else None
    markdown_sections = [] if report_requested else None
    
    alloc_source = dict()
    for i, alloc in enumerate(top_allocations, 1):
        update_type_totals_kb(totals_top_kb, alloc)
        detail_lines = build_allocation_header(alloc, i, symbolize)
        target_so_name, detail_lines, symbolized_entries, bin_name = process_stack_lines(
            alloc, symbolize, binary_file, llvm_symbolizer_path, executor, alloc_source, detail_lines
        )
        should_symbolize = symbolize and (target_so_name == bin_name)
        future_results = {}
        frames = []
        pipeline_func_name = None
        if should_symbolize:
            detail_lines, future_results = resolve_detail_lines(detail_lines)
            detail_lines = detail_lines + ["", "-"*50, ""]
        if report_entries is not None and symbolized_entries and should_symbolize:
            frames = build_frames_from_symbolized(symbolized_entries, future_results)
            selected_frame = select_workspace_frame(frames, workspace_root)
            if target_so_name.endswith(".so") and selected_frame is None:
                msg = "No suitable stack frame found in workspace paths."
                if not args.permissive:
                    raise AssertionError(msg)
                logging.warning(msg)
            logging.warning(f"Selected frame: {selected_frame}")
            pipeline_func_name, pipeline_func_name_backup = find_pipeline_functions(frames)
            logging.debug(f"pipeline_func_name_backup: {pipeline_func_name_backup}")
            if pipeline_func_name is None:
                pipeline_func_name = pipeline_func_name_backup
            if target_so_name.endswith(".so") and pipeline_func_name is None:
                msg = "No entry-point function found in stack (see --entry-prefix)."
                if not args.permissive:
                    raise AssertionError(msg)
                logging.warning(msg)
            logging.debug(f"Founding pipeline func name: {pipeline_func_name}")
            append_report_entry(report_entries, alloc, frames, selected_frame, pipeline_func_name, i)
        update_pipeline_alloc_map(pipeline_alloc, pipeline_func_name, alloc, target_so_name, bin_name)
        if should_symbolize and top_details is not None:
            top_details.append(detail_lines)
    
    if executor:
        executor.shutdown(wait=True)

    logging.info("Alloc Size by Source:")
    headers = ['Library', 'Host', 'DMA', 'MMAP', "Total"]
    table_data_all = build_alloc_source_rows(alloc_source_full)
    table_data_top = build_alloc_source_rows(alloc_source)
    total_summary_all = format_total_summary(
        totals_all_kb["host"], totals_all_kb["dma"], totals_all_kb["mmap"], f"Top {len(all_allocations)}"
    )
    total_summary = format_total_summary(
        totals_top_kb["host"],
        totals_top_kb["dma"],
        totals_top_kb["mmap"],
        f"max={max_show_len}" if max_show_len is not None and max_show_len >= 0 else f"Top {len(top_allocations)}",
    )
    logging.info(f"top={len(all_allocations)} (all allocations):")
    logging.info(tabulate(table_data_all, headers=headers, tablefmt="grid"))
    logging.info(total_summary_all)
    show_top_table = max_show_len is not None and max_show_len >= 0
    if show_top_table:
        logging.info(f"\top={max_show_len} (top {len(top_allocations)} allocations):")
        logging.info(tabulate(table_data_top, headers=headers, tablefmt="grid"))
        logging.info(total_summary)
    if markdown_sections is not None:
        markdown_sections.append("## Alloc Size by Source")
        markdown_sections.append(f"top={len(all_allocations)} (all allocations)")
        markdown_sections.append(to_markdown_table(headers, table_data_all))
        markdown_sections.append(total_summary_all)
        if show_top_table:
            markdown_sections.append(f"top={max_show_len} (top {len(top_allocations)} allocations):")
            markdown_sections.append(to_markdown_table(headers, table_data_top))
            markdown_sections.append(total_summary)

    if symbolize:
        if pipeline_alloc:
            headers_func = ['Function', 'Host', 'DMA', 'MMAP', "Total"]
            func_rows = []
            for func, values in pipeline_alloc.items():
                row = [func] + list(values.values())
                row.append(sum(values.values()))
                func_rows.append(row)
            func_rows.append(['Total',
                              sum(v['host'] for v in pipeline_alloc.values()),
                              sum(v['dma'] for v in pipeline_alloc.values()),
                              sum(v['mmap'] for v in pipeline_alloc.values()),
                              sum(sum(v.values()) for v in pipeline_alloc.values())])
            func_table = tabulate(func_rows, headers=headers_func, tablefmt="grid")
            print("\nPipeline Function Allocations:")
            print(func_table)
            if markdown_sections is not None:
                markdown_sections.append("\n## Pipeline Function Allocations")
                markdown_sections.append(to_markdown_table(headers_func, func_rows))
        elif report_requested:
            print("\nPipeline Function Allocations: 未解析到入口函数调用（见 --entry-prefix）。")
            if markdown_sections is not None:
                markdown_sections.append("\n## Pipeline Function Allocations")
                markdown_sections.append("未解析到入口函数调用（见 --entry-prefix）。")
    if report_requested:
        print("\nAllocation Report:")
        report_headers = ["Top Index", "Variable", "Code Function", "Call Site", "Mem Type", "Memory", "Call Path"]
        report_rows = [
            [
                entry["top_index"],
                entry["variable"],
                entry["code_func"],
                entry["call_site"],
                entry["mem_type"],
                entry["memory"],
                entry["call_path"],
            ]
            for entry in (report_entries or [])
        ]
        if report_rows:
            report_table = tabulate(report_rows, headers=report_headers, tablefmt="grid")
            print(report_table)
        else:
            print("未找到满足 ${workspace}/src 或 ${workspace}/modules 条件的栈帧。")
        if markdown_sections is not None:
            markdown_sections.append("\n## Allocation Report")
            markdown_sections.append(to_markdown_table(report_headers, report_rows))
    if symbolize:
        if not report_requested:
            print("\nTop Stack Details:")
            if top_details:
                for block in top_details:
                    print("\n".join(block))
            else:
                print("未解析到可显示的堆栈。")
        if report_requested and markdown_sections is not None:
            markdown_sections.append("\n## Top Stack Details")
            if top_details:
                for block in top_details:
                    markdown_sections.append("```\n" + "\n".join(block) + "\n```")
            else:
                markdown_sections.append("未解析到可显示的堆栈。")
    if report_requested and markdown_sections is not None:
        try:
            if report_output_path and os.path.dirname(report_output_path):
                os.makedirs(os.path.dirname(report_output_path), exist_ok=True)
            with open(report_output_path, "w") as mdfile:
                mdfile.write("\n\n".join(markdown_sections))
            print(f"报告已保存: {report_output_path}")
        except OSError as exc:
            print(f"Warning: 保存报告失败: {exc}")
    # for so_name, sizes in alloc_source.items():
    #     total = sizes['host'] + sizes['mmap'] + sizes['dma']
    #     print(f"{so_name}: host={sizes['host']}MB, mmap={sizes['mmap']}MB, dma={sizes['dma']}MB, total={total}MB")

if __name__ == "__main__":
    main()
