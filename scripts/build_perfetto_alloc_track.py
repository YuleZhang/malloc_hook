#!/usr/bin/env python3
"""Add a dedicated "Memory Top Allocations" track to a Perfetto trace.

背景:liballoc_hook 通过 trace_marker 写入的内存事件是 ftrace `print`,在 Perfetto
里表现为 683+ 条 async slice 轨道,和海量业务 slice 混在一起。把符号化参数追加到
这些事件名尾部(旧方案)在 UI 上几乎不可见——被淹没、且轨道名尾部会被截断。

本方案改为向 trace 追加一条**独立的 native TrackEvent 轨道** "Memory Top Allocations":
只放"有符号化参数的大分配",每个是一个带完整信息(大小/变量名/调用点)的 slice,
时间范围复用该分配在原 trace 里的 begin/end 时刻。UI 上是一条干净、少量、可读的轨道。

数据来源:process_memory_stack.py 符号化 dump 得到的 hash_index -> 参数 映射;
时间戳:纯 protobuf 解析原 trace 的 ftrace print 事件,按 `.h<N>` 配对 S/F 取 ts+dur。
无第三方依赖(不需要 perfetto python 库)。

复用 perfetto_proto 的 protobuf 编码 / 时钟对齐(analyze_trace) / packet 合并原语。
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import perfetto_proto as P  # noqa: E402  protobuf/perfetto 编解码底座

# ---- Perfetto proto 字段号 ----
# TracePacket
FTRACE_BUNDLE_FIELD = 1  # ftrace_events
# FtraceEventBundle
FTRACE_EVENT_FIELD = 2  # event (repeated)
# FtraceEvent
FTRACE_TS_FIELD = 1  # timestamp (ns)
FTRACE_PRINT_FIELD = 3  # print
# PrintFtraceEvent
PRINT_BUF_FIELD = 2  # buf (string)
# TrackEvent 事件类型
TYPE_SLICE_BEGIN = 1
TYPE_SLICE_END = 2
# TrackEvent 字段
TE_TYPE_FIELD = 9  # type
TE_TRACK_UUID_FIELD = 11  # track_uuid
TE_NAME_FIELD = 23  # name (string) —— 已用 trace_processor 实测确认
# TrackDescriptor 子轨道排序字段(控制 UI 里同级轨道显示顺序)
TD_CHILD_ORDERING_FIELD = 11  # child_ordering (enum)
TD_SIBLING_ORDER_RANK_FIELD = 12  # sibling_order_rank (int32,越小越靠前)
CHILD_ORDERING_EXPLICIT = 3  # ChildTracksOrdering.EXPLICIT:按 sibling_order_rank 排

# 事件名里的 hash:".h<digits>",后接 "[caller]"(旧格式)或直接 "|cookie"(新格式,
# 已去掉 caller)。用 lookahead 锚定字段边界,兼容两种格式。
HASH_RE = re.compile(rb"\.h(\d+)(?=[\[|])")

# Peak-snapshot marker written by liballoc_hook to trace_marker:
#   "B|<pid>|malloc_hook_peak_snapshot total_mb=<f> host_mb=<f> dma_mb=<f>"
# Older builds emitted *_bytes instead of *_mb; accept both and normalize to MiB so the
# peak reads as a memory budget on the timeline. Rendered on the synthetic "Memory Top
# Allocations" track rather than trusted on whatever process the ftrace event landed on.
PEAK_RE = re.compile(
    rb"malloc_hook_peak_snapshot\s+total_(mb|bytes)=([\d.]+)\s+"
    rb"host_(?:mb|bytes)=([\d.]+)\s+dma_(?:mb|bytes)=([\d.]+)"
)


# --------------------------------------------------------------------------- #
# 纯 protobuf 解析:取每个 hash 的 (begin_ts, dur)
# --------------------------------------------------------------------------- #
def extract_hash_timings(trace_bytes: bytes) -> dict:
    """Return {hash_index: (begin_ts_ns, dur_ns)} parsed from ftrace print events.

    按完整事件名(name)配对 S/F,而非仅按 hash_index——因为同一个 hash 可能对应多个
    不同指针(@ptr)的分配。name 就是 S/F 之后、cookie 之前的部分。
    """
    # 配对键:去掉首字符 S/F 和末尾 |cookie\n 后的完整名字
    begins = {}  # name -> (hash, ts)
    ends = {}    # name -> ts
    view = memoryview(trace_bytes)
    off = 0
    n = len(view)
    while off < n:
        key, off = P.read_varint(view, off)
        sz, off = P.read_varint(view, off)
        packet = bytes(view[off : off + sz])
        off += sz
        if b"memory_" not in packet:
            continue
        for fn, wt, v in P.iter_fields(packet):
            if fn != FTRACE_BUNDLE_FIELD or wt != 2:
                continue
            for ef, ew, ev in P.iter_fields(v):
                if ef != FTRACE_EVENT_FIELD or ew != 2 or b"memory_" not in ev:
                    continue
                ts = None
                buf = None
                for f, w, val in P.iter_fields(ev):
                    if f == FTRACE_TS_FIELD and w == 0:
                        ts = val
                    elif f == FTRACE_PRINT_FIELD and w == 2:
                        for pf, pw, pv in P.iter_fields(val):
                            if pf == PRINT_BUF_FIELD and pw == 2:
                                buf = bytes(pv)
                if ts is None or buf is None:
                    continue
                m = HASH_RE.search(buf)
                if not m:
                    continue
                hi = int(m.group(1))
                marker = buf[0:1]
                # 完整名字 = 去掉首字符 S/F 和末尾 |digits\n 后的部分
                name = buf[1:].rsplit(b"|", 1)[0]
                if marker == b"S":
                    begins[name] = (hi, ts)
                elif marker == b"F" and name in begins:
                    ends[name] = ts
    # 按 hash 聚合:同一个 hash 取所有配对成功的 (ts, dur),保留最长的
    hash_durs = {}  # hash -> (ts, dur)
    for name, ets in ends.items():
        if name not in begins:
            continue
        hi, bts = begins[name]
        dur = ets - bts if ets >= bts else 0
        if hi not in hash_durs or dur > hash_durs[hi][1]:
            hash_durs[hi] = (bts, dur)
    return hash_durs


def extract_peak_snapshots(trace_bytes: bytes) -> list:
    """Return [(ts_ns, total_mb, host_mb, dma_mb), ...] for each peak-snapshot marker.

    liballoc_hook records one snapshot the first time the live total crosses the
    configured budget and again on each step above it, so the list traces the climb;
    the last / largest entry is the true peak.
    """
    snaps = []
    view = memoryview(trace_bytes)
    off = 0
    n = len(view)
    while off < n:
        key, off = P.read_varint(view, off)
        sz, off = P.read_varint(view, off)
        packet = bytes(view[off : off + sz])
        off += sz
        if b"malloc_hook_peak_snapshot" not in packet:
            continue
        for fn, wt, v in P.iter_fields(packet):
            if fn != FTRACE_BUNDLE_FIELD or wt != 2:
                continue
            for ef, ew, ev in P.iter_fields(v):
                if ef != FTRACE_EVENT_FIELD or ew != 2 or b"malloc_hook_peak_snapshot" not in ev:
                    continue
                ts = None
                buf = None
                for f, w, val in P.iter_fields(ev):
                    if f == FTRACE_TS_FIELD and w == 0:
                        ts = val
                    elif f == FTRACE_PRINT_FIELD and w == 2:
                        for pf, pw, pv in P.iter_fields(val):
                            if pf == PRINT_BUF_FIELD and pw == 2:
                                buf = bytes(pv)
                if ts is None or buf is None:
                    continue
                m = PEAK_RE.search(buf)
                if not m:
                    continue
                unit = m.group(1)
                scale = 1.0 if unit == b"mb" else 1.0 / (1024.0 * 1024.0)
                total = float(m.group(2)) * scale
                host = float(m.group(3)) * scale
                dma = float(m.group(4)) * scale
                snaps.append((ts, total, host, dma))
    snaps.sort(key=lambda s: s[0])
    return snaps


def _peak_slice_name(total_mb: float, host_mb: float, dma_mb: float, is_max: bool) -> str:
    tag = "Peak" if is_max else "peak step"
    return f"{tag}: {total_mb:.1f} MB (host {host_mb:.1f} / dma {dma_mb:.1f})"


# --------------------------------------------------------------------------- #
# native TrackEvent 编码(复用 merge 脚本的 make_packet / encoders)
# --------------------------------------------------------------------------- #
def _make_track_descriptor(
    seq, uuid, name, first=False, parent=None,
    child_ordering=None, sibling_order_rank=None,
):
    desc = [P.encode_var_field(1, uuid), P.encode_str_field(2, name)]
    if parent is not None:
        desc.append(P.encode_var_field(5, parent))
    # 父 track:声明子轨道按 sibling_order_rank 显式排序
    if child_ordering is not None:
        desc.append(P.encode_var_field(TD_CHILD_ORDERING_FIELD, child_ordering))
    # 子 track:自身的排序权重(越小越靠前)
    if sibling_order_rank is not None:
        desc.append(
            P.encode_var_field(TD_SIBLING_ORDER_RANK_FIELD, sibling_order_rank))
    return P.make_packet(
        [P.encode_len_field(60, b"".join(desc))],
        seq,
        P.SEQ_INCREMENTAL_STATE_CLEARED if first else P.SEQ_NEEDS_INCREMENTAL_STATE,
        first,
    )


def _make_slice_packet(seq, track_uuid, ts, clock_id, etype, name=None):
    event = [
        P.encode_var_field(TE_TYPE_FIELD, etype),
        P.encode_var_field(TE_TRACK_UUID_FIELD, track_uuid),
    ]
    if name is not None:
        event.append(P.encode_str_field(TE_NAME_FIELD, name))
    fields = [
        P.encode_var_field(8, ts),
        P.encode_var_field(58, clock_id),
        P.encode_len_field(11, b"".join(event)),
    ]
    return P.make_packet(fields, seq, P.SEQ_NEEDS_INCREMENTAL_STATE)


def _slice_name(info: dict, hi: int) -> str:
    """Build a readable, info-first slice label.

    以 "Top N: " 前缀开头(N 为该分配在所有分配里按大小的排名),便于在 UI 上
    直接看出这是第几大的内存分配。
    """
    top = info.get("top_index")
    mem = info.get("memory") or ""
    var = info.get("variable") or ""
    fn = info.get("code_func") or ""
    site = info.get("call_site") or ""
    parts = []
    if top is not None:
        parts.append(f"Top {top}:")
    if mem:
        parts.append(mem)
    if var and var != "<unknown>":
        parts.append(var)
    if fn and fn != "<unknown>":
        parts.append(f"[{fn}]")
    if site:
        parts.append(f"@{site}")
    label = " ".join(parts).strip()
    return label or f"h{hi}"

def _sub_track_name(info: dict, hi: int) -> str:
    """Build the sub-track display label (shown on the left of the timeline row).

    以 "[memory hook] Top N" 形式命名,便于在 UI 左侧轨道栏一眼识别是内存 hook
    的第几大分配。N 为该分配在所有分配里按大小的排名。
    """
    top = info.get("top_index")
    if top is not None:
        return f"[memory hook] Top {top}"
    return f"[memory hook] h{hi}"

def build_tracks(
    trace_bytes: bytes, groups: list[tuple[str, dict]]
) -> tuple[bytes, list[tuple[str, int, int]]]:
    """Return (new_trace_bytes, [(track_name, n_slices, n_skipped_no_timing), ...]).

    groups 里每项是 (轨道名, hash_index -> 参数映射),各自生成一条独立的父轨道。
    多个 dump(峰值 dump / exit dump / 多进程)映射到同一份 trace 时用这个入口:
    不同进程的 hash_index 空间是独立的,把它们混进一个映射会互相覆盖,所以按 dump
    分轨道、各自独立编号。

    时间戳解析与时钟对齐对整份 trace 只做一次,所有轨道共用;uuid / 排序序号在
    多个轨道间连续分配,避免碰撞。
    """
    timings = extract_hash_timings(trace_bytes)
    peak_snaps = extract_peak_snapshots(trace_bytes)
    meta = P.analyze_trace(trace_bytes)
    seq = meta.overlay_sequence_id
    clk = meta.primary_clock_id

    next_uuid = meta.next_uuid
    packets = []
    stats = []
    order = 0
    for group_index, (track_name, hash_map) in enumerate(groups):
        # 只保留既有参数、又能在原 trace 找到时间戳的 hash;按内存大小降序。
        def _mb(hi, _hash_map=hash_map):
            m = re.search(r"([\d.]+)\s*MB", _hash_map[hi].get("memory", "") or "")
            return float(m.group(1)) if m else 0.0

        usable = [hi for hi in hash_map if hi in timings]
        usable.sort(key=_mb, reverse=True)
        stats.append((track_name, len(usable), len(hash_map) - len(usable)))

        # 分配 UUID:parent + 每个子 track 一个
        parent_uuid = next_uuid
        next_uuid += 1
        child_uuids = {}
        for hi in usable:
            child_uuids[hi] = next_uuid
            next_uuid += 1

        # 只有整份 trace 的第一个追加 packet 能带 INCREMENTAL_STATE_CLEARED,
        # 后续轨道共用同一 sequence,必须接在已建立的增量状态上。
        packets.append(
            P.OverlayPacketEntry(
                _make_track_descriptor(
                    seq, parent_uuid, track_name, first=(order == 0),
                    child_ordering=CHILD_ORDERING_EXPLICIT),
                meta.trace_min_ns,
                order,
            )
        )
        order += 1
        # Peak snapshot(s) as a sibling sub-track pinned to the top of the group. This
        # renders the memory peak on the synthetic track instead of trusting the raw
        # trace_marker event, which is bound to whatever process/thread emitted it.
        if group_index == 0 and peak_snaps:
            peak_uuid = next_uuid
            next_uuid += 1
            packets.append(
                P.OverlayPacketEntry(
                    _make_track_descriptor(
                        seq, peak_uuid, "[memory hook] Peak", first=False,
                        parent=parent_uuid, sibling_order_rank=0),
                    meta.trace_min_ns,
                    order,
                )
            )
            order += 1
            max_total = max(s[1] for s in peak_snaps)
            max_marked = False
            for ts, total_mb, host_mb, dma_mb in peak_snaps:
                is_max = (not max_marked) and total_mb >= max_total
                if is_max:
                    max_marked = True
                nm = _peak_slice_name(total_mb, host_mb, dma_mb, is_max)
                packets.append(
                    P.OverlayPacketEntry(
                        _make_slice_packet(seq, peak_uuid, ts, clk, TYPE_SLICE_BEGIN, nm),
                        ts, order,
                    )
                )
                order += 1
                end_ts = ts + 2_000_000  # 2 ms so the marker stays clickable in the UI
                packets.append(
                    P.OverlayPacketEntry(
                        _make_slice_packet(seq, peak_uuid, end_ts, clk, TYPE_SLICE_END, None),
                        end_ts, order,
                    )
                )
                order += 1
        for hi in usable:
            child_uuid = child_uuids[hi]
            child_name = _slice_name(hash_map[hi], hi)
            sub_track_name = _sub_track_name(hash_map[hi], hi)
            # 子 track 按 top_index 作为排序权重(越小=内存越大=越靠前),
            # 缺失 top_index 时排到最后(用一个大值兜底)。
            rank = hash_map[hi].get("top_index")
            if rank is None:
                rank = 1 << 30
            # 子 track descriptor:uuid=child_uuid(整数),name=sub_track_name(轨道显示名)
            packets.append(
                P.OverlayPacketEntry(
                    _make_track_descriptor(
                        seq, child_uuid, sub_track_name, first=False, parent=parent_uuid,
                        sibling_order_rank=rank),
                    meta.trace_min_ns,
                    order,
                )
            )
            order += 1

            bts, dur = timings[hi]
            end_ts = bts + (dur if dur > 0 else 1_000_000)
            packets.append(
                P.OverlayPacketEntry(
                    _make_slice_packet(seq, child_uuid, bts, clk, TYPE_SLICE_BEGIN, child_name),
                    bts,
                    order,
                )
            )
            order += 1
            packets.append(
                P.OverlayPacketEntry(
                    _make_slice_packet(seq, child_uuid, end_ts, clk, TYPE_SLICE_END, None),
                    end_ts,
                    order,
                )
            )
            order += 1

    new_bytes = P.merge_overlay_packets(trace_bytes, packets)
    return new_bytes, stats


def build_track(
    trace_bytes: bytes, hash_map: dict, track_name: str = "Memory Top Allocations"
) -> tuple[bytes, int, int]:
    """Return (new_trace_bytes, n_slices, n_skipped_no_timing).

    单轨道入口(build_tracks 的常用特例)。每个分配使用独立子 track(挂在父 track 下),
    避免重叠分配的 BEGIN/END 在栈式配对中交错导致生命周期错误。
    """
    new_bytes, stats = build_tracks(trace_bytes, [(track_name, hash_map)])
    _, n_slices, skipped = stats[0]
    return new_bytes, n_slices, skipped


# --------------------------------------------------------------------------- #
def _normalize_map(raw: dict) -> dict:
    """Accept keys as str or int; return {int_hash: info}."""
    out = {}
    for k, v in raw.items():
        try:
            out[int(k)] = v
        except (TypeError, ValueError):
            continue
    return out


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Add a dedicated 'Memory Top Allocations' track (native TrackEvent) to a Perfetto trace."
    )
    p.add_argument("--trace", type=Path, required=True, help="Input .perfetto trace.")
    p.add_argument("--map", type=Path, required=True, help="hash_index -> info JSON (from process_memory_stack.py --export-hash-map).")
    p.add_argument("--output", type=Path, help="Output path. Default <trace>.toptrack.<suffix>.")
    p.add_argument("--track-name", default="Memory Top Allocations", help="Name of the new track.")
    return p


def default_output_path(trace_path: Path) -> Path:
    suffix = trace_path.suffix or ".perfetto"
    return trace_path.with_name(f"{trace_path.stem}.toptrack{suffix}")


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    if not args.trace.is_file():
        print(f"Error: trace not found: {args.trace}")
        return 1
    if not args.map.is_file():
        print(f"Error: map not found: {args.map}")
        return 1
    hash_map = _normalize_map(json.loads(args.map.read_text(encoding="utf-8")))
    if not hash_map:
        print("Error: map JSON empty or unparseable.")
        return 1
    trace_bytes = args.trace.read_bytes()
    new_bytes, n, skipped = build_track(trace_bytes, hash_map, args.track_name)
    out = args.output or default_output_path(args.trace)
    out.write_bytes(new_bytes)
    print(f"wrote {out}")
    print(f"  track            : {args.track_name!r}")
    print(f"  slices added     : {n}")
    print(f"  skipped (no ts)  : {skipped}")
    print(f"  size {len(trace_bytes)} -> {len(new_bytes)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
