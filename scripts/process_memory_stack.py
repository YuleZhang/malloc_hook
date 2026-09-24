"""解析 liballoc_hook 的内存 dump,符号化堆栈,并把大分配信息回填到 Perfetto。

liballoc_hook 会产出两样东西:
  1. dump 文件  backtrace_heap.*.txt —— 每条分配带 hash_index + 原始堆栈(PC 地址)
  2. perfetto trace —— 内存事件名里带 `.h<hash_index>`,作为与 dump 的连接键

本脚本把 dump 符号化(变量名/函数/调用点/调用路径),再按 hash_index 把这些信息
以一条独立的 "Memory Top Allocations" 轨道写进 perfetto,便于在 UI 上一眼定位大户。

--------------------------------------------------------------------------------
用法一:一步式(dump -> 符号化 -> 直接写回 perfetto,推荐)
--------------------------------------------------------------------------------
    python3 process_memory_stack.py \
        -f backtrace_heap.exit.<ts>.txt \
        --annotate-perfetto trace.perfetto \
        --annotate-output perfetto_top_alloc.perfetto

  运行时会交互提示选择要符号化的动态库(选带符号的业务 .so,如
  libyour_module.so),需要 llvm-symbolizer(在 PATH 或 NDK_ROOT 里)。
  产物是新增了 "Memory Top Allocations" 轨道的 perfetto,拖进 https://ui.perfetto.dev 打开。

  常用附加参数:
    -w <workspace_root>  指定源码根,用于读取变量名(源码不在本地则显示 <unknown>)
    --permissive         找不到业务栈帧/pipeline 函数时只告警不中断
    --debug              详细日志

--------------------------------------------------------------------------------
用法一(多 dump):一次分析多个 backtrace 文件
--------------------------------------------------------------------------------
  liballoc_hook 一次跑图可能产出多份 dump:峰值触发的
  backtrace_heap.pid.<pid>.time.<ts>.txt 和进程退出时的
  backtrace_heap.exit.pid.<pid>.time.<ts>.txt。-f 接受多个文件,也接受目录
  (自动展开该目录下的 backtrace_heap*.txt):

    python3 process_memory_stack.py -f trace/ --report            # 目录展开
    python3 process_memory_stack.py -f dump_a.txt dump_b.txt -r   # 显式多文件

  多 dump 时每个文件独立分析、独立成节,统计口径互不干扰——同一块内存的
  hash_index 会在峰值 dump 和 exit dump 里各出现一次,跨文件相加会重复计算,
  所以本脚本不做跨 dump 合并。产物按 dump 拆分:
    --report            一份 Markdown,每个 dump 一个 "# Dump N/M" 章节
    --export-hash-map   每个 dump 一个 JSON(文件名插入 dump 标签)
    --annotate-perfetto 每个 dump 一条独立轨道 "Memory Top Allocations (<标签>)"
  标签取自文件名的触发方式/pid/时间戳,只保留区分彼此所需的最短形式
  (常见两文件场景就是 peak / exit)。

--------------------------------------------------------------------------------
用法二:分步式(先导出 JSON 映射,再单独建轨道)
--------------------------------------------------------------------------------
    # 1) 符号化 dump,导出 hash_index -> 信息 的 JSON
    python3 process_memory_stack.py -f backtrace_heap.exit.<ts>.txt \
        --export-hash-map hash_index_map.json
    # 2) 用该 JSON 向 perfetto 建轨道(见 build_perfetto_alloc_track.py)
    python3 build_perfetto_alloc_track.py \
        --trace trace.perfetto --map hash_index_map.json \
        --output perfetto_top_alloc.perfetto

--------------------------------------------------------------------------------
用法三:仅生成 Markdown 分析报告(不碰 perfetto)
--------------------------------------------------------------------------------
    python3 process_memory_stack.py -f backtrace_heap.exit.<ts>.txt --report

--------------------------------------------------------------------------------
在 Perfetto UI 里查看结果
--------------------------------------------------------------------------------
  打开注解后的 perfetto,目标进程附近会出现 "Memory Top Allocations" 轨道,
  里面每个 slice 是一个大分配(大小 + 函数 + 调用点),生命周期与真实分配一致。
  也可用左下角 Query(SQL)确认:
    select t.name, s.name, s.dur from slice s join track t on s.track_id=t.id
    where t.name = 'Memory Top Allocations' order by s.dur desc;
"""

import argparse
import glob
import json
import logging
import os
import platform
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

from tabulate import tabulate

# 复用同目录 build_perfetto_alloc_track 的 protobuf 逻辑(--annotate-perfetto 用),
# 使 dump -> 符号化 -> 写回 perfetto 可一步完成,无需中间 JSON。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_perfetto_alloc_track as alloc_track

# liballoc_hook 的 dump 文件名:backtrace_heap[.exit].pid.<pid>.time.<ts>.txt
DUMP_GLOB = "backtrace_heap*.txt"

# --------------------------------------------------------------------------- #
# 项目符号化配置 (maps.json) —— 把工程特定的源码布局/命名从脚本里外置出去。
#
# 入库脚本保持通用;工程特定项(workspace 根、哪些帧算业务/需排除、pipeline 函数
# 命名)放在 hook 根目录一个 gitignore 的 maps.json 里,脚本启动时自动加载,于是
#   python <hook>/scripts/process_memory_stack.py ...
# 无需再手动传 -w / 排除项即可解析当前代码库的 trace。schema 见 scripts/maps.example.json。
# --------------------------------------------------------------------------- #
HOOK_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_PROJECT_CONFIG = {
    # workspace_root: 源码根;None -> 用 -w 或当前目录。
    "workspace_root": None,
    # 命中即视为“业务帧”并优先选中(相对 workspace_root)。
    "workspace_subdirs": ["src", "modules"],
    # 次选来源目录:命中且不在 excluded_frame_relpaths 里的作为兜底帧。
    "dev_toolkit_subdir": "dev_toolkit",
    # 分配器/底层算子等“转发帧”,永不作为分配点(相对 workspace_root 的目录或文件)。
    "excluded_frame_relpaths": [],
    # pipeline 函数识别;pipeline_prefix 为空则跳过该启发式(纯通用)。
    "pipeline_namespace": "",
    "pipeline_prefix": "",
    "pipeline_backup_prefixes": [],
}


def load_project_config():
    """Load project symbolization config from <hook_root>/maps.json (or $MALLOC_HOOK_MAPS).

    Missing file -> generic defaults, so a fresh clone still runs (just with less
    project-aware frame selection). The file is gitignored; scripts/maps.example.json
    documents the schema.
    """
    cfg = dict(DEFAULT_PROJECT_CONFIG)
    path = os.environ.get("MALLOC_HOOK_MAPS") or os.path.join(HOOK_ROOT, "maps.json")
    if os.path.isfile(path):
        try:
            with open(path, "r") as fp:
                cfg.update({k: v for k, v in json.load(fp).items() if k in cfg})
            logging.info("loaded project config: %s", path)
        except (OSError, ValueError) as exc:
            logging.warning("failed to read %s: %s (using defaults)", path, exc)
    return cfg


def resolve_log_files(file_args) -> list:
    """Return the list of dump files to analyze.

    每个 -f 参数可以是文件、目录(展开其中的 backtrace_heap*.txt)或通配(被引号挡住、
    shell 没展开时在这里展开)。不给 -f 时默认展开 ./trace/ 下的 dump;该目录没有符合
    命名的文件时退回旧行为(取排序后最后一个文件),避免老的 trace 目录直接报错。
    """
    if file_args:
        resolved = []
        for arg in file_args:
            if os.path.isdir(arg):
                matched = sorted(glob.glob(os.path.join(arg, DUMP_GLOB)))
                if not matched:
                    print(f"Error: 目录下没有 {DUMP_GLOB}: {arg}")
                    sys.exit(1)
                resolved.extend(matched)
            elif any(ch in arg for ch in "*?["):
                matched = sorted(glob.glob(arg))
                if not matched:
                    print(f"Error: 通配未匹配到任何文件: {arg}")
                    sys.exit(1)
                resolved.extend(matched)
            else:
                resolved.append(arg)
        # 同一文件被目录/通配/显式路径重复给出时去重,保持首次出现的顺序
        seen = set()
        unique = []
        for path in resolved:
            key = os.path.abspath(path)
            if key not in seen:
                seen.add(key)
                unique.append(path)
        return unique

    trace_dir = os.path.join(os.getcwd(), "trace")
    if not os.path.isdir(trace_dir):
        print("Error: --file未指定且默认trace目录不存在: ./trace/")
        sys.exit(1)
    dumps = sorted(glob.glob(os.path.join(trace_dir, DUMP_GLOB)))
    if dumps:
        print(f"未指定--file，使用默认trace目录下 {len(dumps)} 个 dump:")
        for path in dumps:
            print(f"  {path}")
        return dumps
    files = [f for f in os.listdir(trace_dir) if os.path.isfile(os.path.join(trace_dir, f))]
    if not files:
        print(f"Error: ./trace/ 目录下没有 {DUMP_GLOB},也没有其它可用的日志文件")
        sys.exit(1)
    files.sort()
    default_file = os.path.join(trace_dir, files[-1])
    print(f"未指定--file，使用默认日志文件: {default_file}")
    return [default_file]


def dump_tag_parts(log_file: str) -> dict:
    """Split a dump filename into the parts that can distinguish it from others.

    backtrace_heap.pid.123.time.456.txt      -> kind=peak, pid=pid123, time=t456
    backtrace_heap.exit.pid.123.time.789.txt -> kind=exit, pid=pid123, time=t789
    命名不符合该模式时 kind 退回到清洗后的文件名主干,pid/time 为空。
    """
    name = os.path.basename(log_file)
    stem = name[: -len(".txt")] if name.endswith(".txt") else name
    pid_match = re.search(r"pid\.(\d+)", stem)
    time_match = re.search(r"time\.(\d+)", stem)
    if pid_match or time_match:
        kind = "exit" if re.search(r"(^|\.)exit(\.|$)", stem) else "peak"
    else:
        # 兜底:清掉固定前缀和文件名非法字符,仍保证可读、可作文件名片段
        kind = re.sub(r"[^\w.-]+", "_", re.sub(r"^backtrace_heap\.?", "", stem) or stem)
    return {
        "kind": kind,
        "pid": f"pid{pid_match.group(1)}" if pid_match else "",
        "time": f"t{time_match.group(1)}" if time_match else "",
    }


def build_dump_tags(log_files) -> list:
    """Return one short, unique tag per dump — the least verbose form that separates them.

    按"够用就好"逐级加细:两个文件的常见场景(峰值 + exit)标签就是 peak / exit;同一
    进程多次峰值触发才加时间戳,多进程才加 pid。仍撞名(如目录不同、文件同名)时补 #N。
    """
    parts = [dump_tag_parts(path) for path in log_files]
    for keys in (("kind",), ("kind", "pid"), ("kind", "pid", "time")):
        tags = ["-".join(p[k] for k in keys if p[k]) for p in parts]
        if len(set(tags)) == len(tags):
            return tags
    tags = ["-".join(p[k] for k in ("kind", "pid", "time") if p[k]) for p in parts]
    counts = {}
    unique = []
    for tag in tags:
        counts[tag] = counts.get(tag, 0) + 1
        unique.append(tag if counts[tag] == 1 else f"{tag}#{counts[tag]}")
    return unique



def locate_symbol_binary(binary_name: str, start_dir: str, auto_select: bool = False) -> str:
    """
    Locate a non-stripped binary by name starting from start_dir using find.
    Exits the program with an error if the binary or a symbolized version is not found.
    When auto_select is True, automatically picks the first candidate without prompting.
    """
    if not binary_name:
        return ""

    find_cmd = ["find", start_dir, "-name", binary_name]
    try:
        output = subprocess.check_output(find_cmd, stderr=subprocess.STDOUT, text=True)
    except subprocess.CalledProcessError as exc:
        print(f"Error: failed to search for {binary_name}: {exc.output.strip()}")
        sys.exit(1)

    candidates = [line.strip() for line in output.splitlines() if line.strip()]
    if not candidates:
        print(f"Error: could not find '{binary_name}' under {start_dir}")
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
        if len(symbolized) == 1 or auto_select:
            return symbolized[0]
        print(f"发现多个包含符号信息的 {binary_name}:")
        return prompt_user_select_binary(symbolized)

    print(
        f"Error: found {len(candidates)} instance(s) of '{binary_name}' "
        "but none contain symbols (file output missing 'not stripped')."
    )
    sys.exit(1)


def prompt_user_select_binary(candidates):
    """Prompt user to select a library from candidates."""
    print("请选择需要解析的动态库:")
    for idx, name in enumerate(candidates, 1):
        print(f"{idx}. {name}")
    while True:
        choice = input("输入序号并回车: ").strip()
        if not choice.isdigit():
            print("请输入有效序号")
            continue
        choice_num = int(choice)
        if 1 <= choice_num <= len(candidates):
            return candidates[choice_num - 1]
        print("序号超出范围，请重新输入")


def auto_select_adapter_so(candidate_libs):
    """从候选库列表中自动选择 adapter so，无需手动输入。"""
    for lib in candidate_libs:
        if "adapter" in lib.lower():
            print(f"自动选择 adapter so: {lib}")
            return lib
    # 未找到 adapter so，回退到第一个候选
    fallback = candidate_libs[0]
    print(f"未找到 adapter so，使用第一个候选: {fallback}")
    return fallback


def find_build_dir(workspace_root):
    """在 workspace_root 下查找 build 目录（优先 build 符号链接，其次 build-* 目录）。"""
    # 优先检查 'build' 符号链接/目录
    build = os.path.join(workspace_root, "build/")
    if os.path.isdir(build):
        return build
    # 查找 build-* 目录
    try:
        for entry in os.listdir(workspace_root):
            if entry.startswith("build-") and os.path.isdir(os.path.join(workspace_root, entry)):
                return os.path.join(workspace_root, entry)
    except OSError:
        pass
    # 回退到 workspace_root
    print(f"Warning: 未找到 build 目录，使用 {workspace_root}")
    return workspace_root


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
    Parse symbolized output, returning (function, file_path, line_no).

    Handles both formats:
    - llvm-symbolizer:  "func at file:line:col" (single-line, with -p)
    - llvm-addr2line:   "func\nfile:line" (two-line, with -f -C)
    """
    logging.debug(f"Parsing symbolized output:\n{resolved}")
    lines = [line.strip() for line in resolved.splitlines() if line.strip()]
    if not lines:
        return ("", "", None)

    function_name = ""
    location_part = ""

    # Detect addr2line two-line format: line0 = function, line1 = file:line
    if len(lines) >= 2 and " at " not in lines[0] and "/" in lines[1]:
        function_name = lines[0]
        # Skip "??" (unknown function) from addr2line
        if function_name == "??":
            function_name = ""
        location_part = lines[1]
    elif " at " in lines[0]:
        function_name, location_part = lines[0].split(" at ", 1)
    else:
        function_name = lines[0]

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

    # Clean up function name (addr2line may include demangled template cruft)
    function_name = function_name.strip()

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


def select_workspace_frame(frames, workspace_root, project=None):
    """Pick the first frame that belongs to the workspace, with a backup fallback.

    If no frame matches workspace paths, falls back to the first frame with a
    non-empty file (skipping system libs like libc.so). This allows the script
    to produce useful output even when the source tree is not available locally.

    Workspace/exclusion layout comes from the project config (maps.json); with no
    config it degrades to a generic src/modules match and no exclusions.
    """
    project = project or DEFAULT_PROJECT_CONFIG
    selected_frame = None
    backup_frame = None
    workspace_prefixes = [
        os.path.abspath(os.path.join(workspace_root, sub)) + os.sep
        for sub in project.get("workspace_subdirs", [])
    ]
    dev_sub = project.get("dev_toolkit_subdir") or ""
    dev_toolkit_prefix = (
        os.path.abspath(os.path.join(workspace_root, dev_sub)) + os.sep if dev_sub else None
    )
    excluded_backup_prefixes = [
        os.path.abspath(os.path.join(workspace_root, rel)) + (os.sep if not rel.endswith((".cpp", ".hpp", ".h", ".cc")) else "")
        for rel in project.get("excluded_frame_relpaths", [])
    ]
    for frame in frames:
        abs_file = os.path.abspath(frame["file"]) if frame["file"] else ""
        logging.debug(f"Checking frame file: {abs_file}")
        if any(abs_file.startswith(prefix) for prefix in workspace_prefixes):
            selected_frame = frame
            break
        if dev_toolkit_prefix and abs_file.startswith(dev_toolkit_prefix) and not any(
            abs_file.startswith(prefix) for prefix in excluded_backup_prefixes
        ):
            backup_frame = frame
        # 兜底:取第一个有文件路径的非系统帧(用于本地无源码的场景)
        if backup_frame is None and abs_file and "/bionic/" not in abs_file:
            backup_frame = frame
    if selected_frame is None and backup_frame is not None:
        selected_frame = backup_frame
    logging.warning(f"Selected frame: {selected_frame}, Backup frame: {backup_frame}")
    return selected_frame


def find_pipeline_functions(frames, project=None):
    """Locate pipeline function names from frames, with a backup heuristic.

    Naming comes from the project config (maps.json). If pipeline_prefix is empty
    (the generic default), the heuristic is skipped and (None, None) is returned.
    """
    project = project or DEFAULT_PROJECT_CONFIG
    SIQ_NS = project.get("pipeline_namespace", "") or ""
    PIPELINE_PREFIX = project.get("pipeline_prefix", "") or ""
    BACKUP_PREFIXES = tuple(project.get("pipeline_backup_prefixes", []) or ())

    if not PIPELINE_PREFIX:
        return None, None

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

            # 强匹配：Pipeline 命中直接返回
            if head.startswith(PIPELINE_PREFIX):
                logging.debug("original func pipeline name: %s", func_name)
                pipeline_func_name = head.split(SIQ_NS, 1)[1] if SIQ_NS and SIQ_NS in head else head
                return pipeline_func_name, pipeline_func_name_backup

            # 备选匹配：只记录第一个 backup
            if BACKUP_PREFIXES and pipeline_func_name_backup is None and head.startswith(BACKUP_PREFIXES):
                logging.debug("original func siq name: %s", func_name)

                if SIQ_NS and head.startswith(SIQ_NS):
                    pipeline_func_name_backup = head.split(SIQ_NS, 1)[1]
                else:
                    pipeline_func_name_backup = head

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
        if not func_name or not func_name.startswith("siq::"):
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
            "hash_index": alloc.get("hash_index", -1),
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


def build_hash_index_map(report_entries):
    """Build a hash_index -> allocation-info mapping from report entries.

    键为字符串形式的 hash_index(与 perfetto 事件名里的 ".h<N>" 对应),值为可直接
    作为 perfetto slice 注解/追加名的字段集合。hash_index 缺失(-1)的条目跳过,
    因为无法与 perfetto 事件关联。同一 hash_index 若出现多次(不同 top),保留首个
    (即最大分配,report_entries 已按 size 降序)。
    """
    hash_map = {}
    for entry in report_entries or []:
        hi = entry.get("hash_index", -1)
        if hi is None or hi < 0:
            continue
        key = str(hi)
        if key in hash_map:
            continue
        hash_map[key] = {
            "variable": entry["variable"],
            "code_func": entry["code_func"],
            "call_site": entry["call_site"],
            "mem_type": entry["mem_type"],
            "memory": entry["memory"],
            "call_path": entry["call_path"],
            "top_index": entry["top_index"],
        }
    return hash_map


def write_hash_index_map(report_entries, output_path):
    """Serialize the hash_index map to JSON. Returns the map (possibly empty)."""
    hash_map = build_hash_index_map(report_entries)
    try:
        if output_path and os.path.dirname(output_path):
            os.makedirs(os.path.dirname(output_path), exist_ok=True)
        with open(output_path, "w") as jsonfile:
            json.dump(hash_map, jsonfile, ensure_ascii=False, indent=2)
        print(f"hash_index 映射已保存: {output_path} ({len(hash_map)} 条)")
    except OSError as exc:
        print(f"Warning: 保存 hash_index 映射失败: {exc}")
    return hash_map


def annotate_perfetto_from_dumps(dump_results, trace_path, output_path):
    """Add one "Memory Top Allocations" track per dump to a perfetto trace, one step.

    直接用内存里的 report_entries 构建 hash_index -> 参数 映射(无需中间 JSON),
    复用 build_perfetto_alloc_track 向 trace 追加独立的 native TrackEvent 轨道:
    只放有符号化参数的大分配,每个是带大小/变量名/调用点的 slice,时间范围复用原
    trace 里该分配的 begin/end 时刻。UI 上是干净、可读、不淹没的轨道。

    每个 dump 一条轨道(名字带 dump 标签),而不是合并成一条——不同进程的 hash_index
    空间是独立的,混进同一个映射会互相覆盖;同进程的峰值/exit 快照混在一条轨道上也
    分不清哪条来自哪个时刻。单 dump 时轨道名保持原来的 "Memory Top Allocations"。
    trace_path 为空时默认 ./trace.perfetto;output_path 为空时派生
    <trace>.toptrack.<suffix>。
    """
    from pathlib import Path

    trace = Path(trace_path or "trace.perfetto")
    if not trace.is_file():
        print(f"Error: perfetto trace 不存在: {trace}")
        return
    single = len(dump_results) == 1
    groups = []
    for result in dump_results:
        # build_hash_index_map 的 key 是 str(hash_index);轨道构建器按 int 索引,转一下。
        str_map = build_hash_index_map(result["report_entries"])
        if not str_map:
            print(f"Warning: [{result['tag']}] 无可用的 hash_index 映射,该 dump 不生成轨道。")
            continue
        name = "Memory Top Allocations" if single else f"Memory Top Allocations ({result['tag']})"
        groups.append((name, {int(k): v for k, v in str_map.items()}))
    if not groups:
        print("Warning: 所有 dump 都没有可用的 hash_index 映射,跳过 perfetto 轨道生成。")
        return

    trace_bytes = trace.read_bytes()
    new_bytes, stats = alloc_track.build_tracks(trace_bytes, groups)

    out = Path(output_path) if output_path else alloc_track.default_output_path(trace)
    if out.parent and not out.parent.exists():
        out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(new_bytes)

    print(f"perfetto 已生成 Top Allocations 轨道并保存: {out}")
    for (name, hash_map), (_, n_slices, skipped) in zip(groups, stats):
        print(f"  轨道 {name}:")
        print(f"    映射条目            : {len(hash_map)}")
        print(f"    轨道 slice 数       : {n_slices}")
        print(f"    跳过(trace 无时间戳): {skipped}")
    print(f"  大小 {len(trace_bytes)} -> {len(new_bytes)} bytes")


def parse_log(log_file, max_show_len=100, binary_file=""):
    # 定义正则模式以提取所需信息
    # hash_index 为可选字段,兼容未带该列的旧 dump 文件。
    alloc_pattern = re.compile(r"alloc_size:([\d.]+)KB\s+alloc_type:([A-Za-z]+)\s+alloc_num:(\d+)\s+(?:hash_index:(\d+)\s+)?alloc_time:([\d-]+ [\d:.]+)")
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
            hash_index = int(alloc_match.group(4)) if alloc_match.group(4) else -1
            alloc_time = alloc_match.group(5)

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
                        "hash_index": hash_index,
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


def collect_candidate_libs(allocations):
    """Return the distinct non-system .so names seen in stacks, in first-seen order."""
    seen = set()
    candidate_libs = []
    for alloc in allocations:
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
    return candidate_libs


def tagged_output_path(path, tag, multi):
    """Insert a dump tag before the extension when analyzing multiple dumps.

    单 dump 时保持用户给的路径原样(与旧行为一致);多 dump 时每个 dump 各出一份,
    如 hash_index_map.json -> hash_index_map.peak-pid123-t456.json。
    """
    if not multi:
        return path
    root, ext = os.path.splitext(path)
    return f"{root}.{tag}{ext}"


def analyze_dump(dump, cfg):
    """Symbolize one dump and build its report entries / tables / markdown sections.

    逐 dump 独立统计:库汇总、总量、Top 表都只覆盖本文件,不跨 dump 累加——同一块内存
    会在峰值 dump 和 exit dump 里各出现一次,相加会重复计算。返回该 dump 的分析结果,
    由调用方汇总成 markdown / JSON / perfetto 轨道。
    """
    all_allocations = dump["all_allocations"]
    max_show_len = cfg["max_show_len"]
    symbolize = cfg["symbolize"]
    report_requested = cfg["report_requested"]
    workspace_root = cfg["workspace_root"]
    binary_file = cfg["binary_file"]
    top_allocations = all_allocations if max_show_len is None or max_show_len < 0 else all_allocations[:max_show_len]
    alloc_source_full = aggregate_allocations_by_source(all_allocations)
    totals_all_kb = calculate_totals_kb(all_allocations)

    totals_top_kb = {"host": 0.0, "dma": 0.0, "mmap": 0.0}
    pipeline_alloc = {} if symbolize else None
    report_entries = [] if report_requested else None
    top_details = [] if symbolize else None
    markdown_sections = [] if cfg["markdown_requested"] else None

    alloc_source = dict()
    for i, alloc in enumerate(top_allocations, 1):
        update_type_totals_kb(totals_top_kb, alloc)
        detail_lines = build_allocation_header(alloc, i, symbolize)
        target_so_name, detail_lines, symbolized_entries, bin_name = process_stack_lines(
            alloc, symbolize, binary_file, cfg["llvm_symbolizer_path"], cfg["executor"], alloc_source, detail_lines
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
            selected_frame = select_workspace_frame(frames, workspace_root, cfg.get("project"))
            if target_so_name.endswith(".so") and selected_frame is None:
                msg = "No suitable stack frame found in workspace paths."
                if not cfg["permissive"]:
                    raise AssertionError(msg)
                logging.warning(msg)
            logging.warning(f"Selected frame: {selected_frame}")
            pipeline_func_name, pipeline_func_name_backup = find_pipeline_functions(frames, cfg.get("project"))
            logging.debug(f"pipeline_func_name_backup: {pipeline_func_name_backup}")
            if pipeline_func_name is None:
                pipeline_func_name = pipeline_func_name_backup
            pipeline_configured = bool(cfg.get("project", {}).get("pipeline_prefix"))
            if pipeline_configured and target_so_name.endswith(".so") and pipeline_func_name is None:
                msg = "No pipeline function found in stack."
                if not cfg["permissive"]:
                    raise AssertionError(msg)
                logging.warning(msg)
            logging.debug(f"Founding pipeline func name: {pipeline_func_name}")
            append_report_entry(report_entries, alloc, frames, selected_frame, pipeline_func_name, i)
        update_pipeline_alloc_map(pipeline_alloc, pipeline_func_name, alloc, target_so_name, bin_name)
        if should_symbolize and top_details is not None:
            top_details.append(detail_lines)

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
            print("\nPipeline Function Allocations: 未解析到 pipeline 函数调用。")
            if markdown_sections is not None:
                markdown_sections.append("\n## Pipeline Function Allocations")
                markdown_sections.append("未解析到 pipeline 函数调用。")
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
    return {
        "tag": dump["tag"],
        "log_file": dump["log_file"],
        "report_entries": report_entries,
        "markdown_sections": markdown_sections,
    }


def main():
    parser = argparse.ArgumentParser(description="Parse memory allocation log and display top allocations.")
    parser.add_argument("-f", "--file", nargs="+", metavar="DUMP", help="Path(s) to the memory allocation dump file(s), or a directory holding backtrace_heap*.txt. Multiple dumps are analyzed independently (default: every backtrace_heap*.txt under ./trace/).")
    parser.add_argument("-m", "--max_show_len", type=int, default=-1, help="Maximum number of allocations to show.")
    parser.add_argument("-w", "--workspace_root", type=str, default=None, help="Workspace root used to locate project source (defaults to current working directory).")
    parser.add_argument("-r", "--report", nargs="?", const="", default=None, help="Generate memory analysis report (Markdown). Optional path (default: ./memory_report.md). With multiple dumps every dump gets its own section in the one file.")
    parser.add_argument("--export-hash-map", nargs="?", const="", default=None, help="Export a hash_index -> allocation-info JSON map for perfetto annotation. Optional path (default: ./hash_index_map.json); with multiple dumps the dump tag is inserted before the extension. Implies symbolization.")
    parser.add_argument("--annotate-perfetto", nargs="?", const="", default=None, help="Add a dedicated 'Memory Top Allocations' track to a .perfetto trace, using the symbolized dump directly (no intermediate JSON). One track per dump. Value is the input trace path (default: ./trace.perfetto). Implies symbolization.")
    parser.add_argument("--annotate-output", type=str, default=None, help="Output path for --annotate-perfetto. Defaults to <trace>.toptrack.<suffix>.")
    parser.add_argument("--no-symbolize", action="store_true", help="Skip stack symbolization/PC parsing (default: symbolize).")
    parser.add_argument("--permissive", action="store_true", help="Do not assert when stack frame or pipeline function is missing; log a warning instead.")
    parser.add_argument("--debug", action="store_true", help="Enable debug logging.")
    args = parser.parse_args()

    symbolize = not args.no_symbolize

    if args.debug:
        logging.basicConfig(level=logging.DEBUG)
    else:
        logging.basicConfig(level=logging.INFO)

    markdown_requested = args.report is not None
    export_hash_requested = args.export_hash_map is not None
    annotate_requested = args.annotate_perfetto is not None
    # 三者都依赖 report_entries(符号化后逐条生成),用统一开关驱动条目构建;
    # 但 markdown 文件只在 --report 时写出,JSON 只在 --export-hash-map 时写出,
    # perfetto 只在 --annotate-perfetto 时写出。
    report_requested = markdown_requested or export_hash_requested or annotate_requested
    if report_requested and not symbolize:
        print("Error: --report / --export-hash-map / --annotate-perfetto 需要开启符号化（默认开启），否则无法生成带源信息的结果。")
        sys.exit(1)
    log_files = resolve_log_files(args.file)
    missing = [path for path in log_files if not os.path.isfile(path)]
    if missing:
        for path in missing:
            print(f"Error: 指定的日志文件不存在: {path}")
        sys.exit(1)
    tags = build_dump_tags(log_files)
    multi = len(log_files) > 1
    max_show_len = args.max_show_len
    binary_file = ""
    # Project symbolization config (maps.json at the hook root). workspace_root
    # precedence: -w > maps.json > cwd, so a configured checkout needs no -w.
    project = load_project_config()
    if args.workspace_root:
        workspace_root = os.path.abspath(args.workspace_root)
    elif project.get("workspace_root"):
        workspace_root = os.path.abspath(os.path.expanduser(project["workspace_root"]))
    else:
        workspace_root = os.getcwd()
    if markdown_requested:
        report_output_path = (
            os.path.join(os.getcwd(), "memory_report.md")
            if args.report == "" or args.report is None
            else os.path.abspath(args.report)
        )
    else:
        report_output_path = None
    if export_hash_requested:
        hash_map_output_path = (
            os.path.join(HOOK_ROOT, "hash_index_map.json")
            if args.export_hash_map == "" or args.export_hash_map is None
            else os.path.abspath(args.export_hash_map)
        )
    else:
        hash_map_output_path = None
    # 先全部解析,再据全部 dump 的栈选符号库:同一次跑的多份 dump 用同一个业务 .so,
    # 只需定位一次(locate_symbol_binary 会 find 整个 build 目录,不宜每 dump 重跑)。
    dumps = []
    for log_file, tag in zip(log_files, tags):
        allocations = parse_log(log_file, -1, binary_file)
        if not allocations:
            print(f"Warning: [{tag}] {log_file} 中没有 >1KB 的分配记录,跳过。")
            continue
        dumps.append({"log_file": log_file, "tag": tag, "all_allocations": allocations})
    if not dumps:
        print("Error: 所有 dump 都没有可分析的分配记录")
        sys.exit(1)
    if symbolize:
        candidate_libs = collect_candidate_libs(
            alloc
            for dump in dumps
            for alloc in (
                dump["all_allocations"]
                if max_show_len is None or max_show_len < 0
                else dump["all_allocations"][:max_show_len]
            )
        )
        if not candidate_libs:
            print("Error: 日志中未发现可供解析的动态库")
            sys.exit(1)
        selected_name = auto_select_adapter_so(candidate_libs)
        build_dir = find_build_dir(workspace_root)
        binary_file = locate_symbol_binary(selected_name, build_dir)
        print(f"Using symbol binary: {binary_file}")
        llvm_symbolizer_path = find_llvm_symbolizer()
        print(f"Using llvm-symbolizer: {llvm_symbolizer_path}")
        max_workers = min(32, (os.cpu_count() or 4))
        executor = ThreadPoolExecutor(max_workers=max_workers)
    else:
        llvm_symbolizer_path = ""
        executor = None

    cfg = {
        "symbolize": symbolize,
        "max_show_len": max_show_len,
        "workspace_root": workspace_root,
        "binary_file": binary_file,
        "llvm_symbolizer_path": llvm_symbolizer_path,
        "executor": executor,
        "permissive": args.permissive,
        "report_requested": report_requested,
        "markdown_requested": markdown_requested,
        "project": project,
    }
    dump_results = []
    try:
        for idx, dump in enumerate(dumps, 1):
            if multi:
                print(f"\n{'=' * 70}")
                print(f"Dump {idx}/{len(dumps)}: {os.path.basename(dump['log_file'])}  [{dump['tag']}]")
                print(f"{'=' * 70}")
            dump_results.append(analyze_dump(dump, cfg))
    finally:
        if executor:
            executor.shutdown(wait=True)

    if markdown_requested:
        markdown_sections = []
        for idx, result in enumerate(dump_results, 1):
            if multi:
                markdown_sections.append(
                    f"# Dump {idx}/{len(dump_results)}: {os.path.basename(result['log_file'])}"
                )
                markdown_sections.append(f"标签 `{result['tag']}`，来源 `{result['log_file']}`")
            markdown_sections.extend(result["markdown_sections"] or [])
        try:
            if report_output_path and os.path.dirname(report_output_path):
                os.makedirs(os.path.dirname(report_output_path), exist_ok=True)
            with open(report_output_path, "w") as mdfile:
                mdfile.write("\n\n".join(markdown_sections))
            print(f"报告已保存: {report_output_path} ({len(dump_results)} 个 dump)")
        except OSError as exc:
            print(f"Warning: 保存报告失败: {exc}")
    if export_hash_requested:
        for result in dump_results:
            write_hash_index_map(
                result["report_entries"],
                tagged_output_path(hash_map_output_path, result["tag"], multi),
            )
    if annotate_requested:
        annotate_perfetto_from_dumps(
            dump_results, args.annotate_perfetto, args.annotate_output
        )

if __name__ == "__main__":
    main()

