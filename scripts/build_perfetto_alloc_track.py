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
无第三方依赖(不需要 perfetto 库)。

**输出默认是精简的。** `MALLOC_HOOK_TRACE_ALLOC=1` 给每个被跟踪的分配写一对
`memory_<type>@<ptr>` 的 trace_marker,名字里嵌了指针所以每个都唯一,Perfetto 会
按名字各建一条 async 轨道。实测一次 28s 的 pipeline run(`BACKTRACE_MIN_SIZE`
取默认 1024):350k 个 print 事件、30 MiB(占 trace 的 68%)、**169,935 条轨道**,
UI 要为此排 17 万行,直接卡死;而业务自己的 atrace 只占 5.8%。这些原始 marker
的唯一用途就是在这里配对取时间戳,本脚本用完之后它们就是死重,所以默认在输出里
删掉(保留 `malloc_hook_peak_snapshot`,它只有几十条且是 Peak 轨道的来源)。
实测 44.6 MiB / 170,340 轨 -> 8.1 MiB / 405 轨,counter、`[memory hook]` 轨道、
业务 atrace、sched 全部不受影响。要保留原始 marker 用 `--keep-alloc-markers`。

注意:因此**输入必须是未精简的原始 trace**。对已精简的 trace 再跑一次会得到
0 条 slice(脚本会就此告警)。

复用 perfetto_proto 的 protobuf 编码 / 时钟对齐(analyze_trace) / packet 合并原语。
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import zlib
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import perfetto_proto as P  # noqa: E402  protobuf/perfetto 编解码底座

# ---- Perfetto proto 字段号 ----
# TracePacket
FTRACE_BUNDLE_FIELD = 1  # ftrace_events
COMPRESSED_PACKETS_FIELD = 50  # compressed_packets (zlib 包裹的内层 TracePacket 流)
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
HASH_RE = re.compile(rb"\.h(\d+)(?=\.s|\[|\||$)")
SIZE_RE = re.compile(rb"\.s(\d+)(?=\[|\||$)")

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
def _iter_trace_packets_recursive(trace_bytes: bytes):
    """Yield top-level and compressed TracePackets.

    Android traces commonly store ftrace packets in ``compressed_packets``. Scanning only the
    outer packet made this tool report zero allocation slices while still producing a valid
    larger trace, which looked like a successful CSV-only overlay.
    """
    for packet in P.iter_trace_packets(trace_bytes):
        yield packet
        for field_number, wire_type, value in P.iter_fields(packet):
            if field_number == 50 and wire_type == 2:
                yield from _iter_trace_packets_recursive(P.decompress_packets(value))


def extract_allocation_timings(trace_bytes: bytes) -> list[dict]:
    """Return every paired allocation lifetime parsed from ftrace print events.

    New markers carry both stack hash and allocation size. Pair by the complete
    async identity so multiple pointers sharing one stack remain independent;
    repeated reuse of the same address is retained as multiple lifetimes too.
    """
    begins = {}  # name -> FIFO list[(hash, size_bytes, ts)]
    lifetimes = []
    for packet in _iter_trace_packets_recursive(trace_bytes):
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
                size_match = SIZE_RE.search(buf)
                size_bytes = int(size_match.group(1)) if size_match else None
                marker = buf[0:1]
                # 完整名字 = 去掉首字符 S/F 和末尾 |digits\n 后的部分
                name = buf[1:].rsplit(b"|", 1)[0]
                if marker == b"S":
                    begins.setdefault(name, []).append((hi, size_bytes, ts))
                elif marker == b"F" and begins.get(name):
                    begin_hi, begin_size, bts = begins[name].pop(0)
                    lifetimes.append({
                        "hash_index": begin_hi,
                        "size_bytes": begin_size,
                        "ts": bts,
                        "dur": ts - bts if ts >= bts else 0,
                    })
    return lifetimes


# Compatibility alias for callers that imported the old helper name. The return
# shape is intentionally the new per-allocation list rather than a lossy hash map.
extract_hash_timings = extract_allocation_timings


def extract_peak_snapshots(trace_bytes: bytes) -> list:
    """Return [(ts_ns, total_mb, host_mb, dma_mb), ...] for each peak-snapshot marker.

    liballoc_hook records one snapshot the first time the live total crosses the
    configured budget and again on each step above it, so the list traces the climb;
    the last / largest entry is the true peak.
    """
    snaps = []
    for packet in _iter_trace_packets_recursive(trace_bytes):
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
# 精简:删掉逐分配的 memory_* marker(默认行为,见模块 docstring)
# --------------------------------------------------------------------------- #
ALLOC_MARKER_TAG = b"memory_"
PEAK_MARKER_TAG = b"malloc_hook_peak_snapshot"
# field 50 / wire type 2 的 key,用来便宜地判断一个 packet 是否可能内嵌压缩流
_COMPRESSED_KEY = P.encode_key(COMPRESSED_PACKETS_FIELD, 2)


def _copy_field(field_number: int, wire_type: int, value) -> bytes:
    """Re-encode one protobuf field exactly as iter_fields produced it."""
    if wire_type == 0:
        return P.encode_var_field(field_number, value)
    if wire_type == 2:
        return P.encode_len_field(field_number, value)
    if wire_type in (1, 5):
        return P.encode_key(field_number, wire_type) + value
    raise ValueError(f"unsupported wire type {wire_type}")


def _inflate(payload: bytes) -> tuple[bytes, int]:
    """Decompress compressed_packets, returning (data, wbits) so we can re-deflate
    in whatever framing the producer used."""
    for wbits in (zlib.MAX_WBITS, -zlib.MAX_WBITS):
        try:
            return zlib.decompress(payload, wbits), wbits
        except zlib.error:
            continue
    raise ValueError("compressed_packets is present but could not be decompressed")


def _deflate(payload: bytes, wbits: int) -> bytes:
    compressor = zlib.compressobj(wbits=wbits)
    return compressor.compress(payload) + compressor.flush()


def _strip_packet(packet: bytes) -> tuple[bytes, int]:
    """Rebuild one TracePacket without the per-allocation markers.

    Returns (packet_bytes, n_dropped). When nothing matched, the original bytes are
    returned unchanged so untouched packets stay byte-identical.
    """
    dropped = 0
    parts = []
    for field_number, wire_type, value in P.iter_fields(packet):
        if field_number == FTRACE_BUNDLE_FIELD and wire_type == 2:
            kept = []
            for ef, ew, event in P.iter_fields(value):
                if (
                    ef == FTRACE_EVENT_FIELD
                    and ew == 2
                    and ALLOC_MARKER_TAG in event
                    and PEAK_MARKER_TAG not in event
                ):
                    dropped += 1
                    continue
                kept.append(_copy_field(ef, ew, event))
            parts.append(P.encode_len_field(FTRACE_BUNDLE_FIELD, b"".join(kept)))
        elif field_number == COMPRESSED_PACKETS_FIELD and wire_type == 2:
            inner, wbits = _inflate(value)
            new_inner, inner_dropped = strip_alloc_markers(inner)
            dropped += inner_dropped
            if inner_dropped:
                parts.append(
                    P.encode_len_field(field_number, _deflate(new_inner, wbits)))
            else:
                parts.append(_copy_field(field_number, wire_type, value))
        else:
            parts.append(_copy_field(field_number, wire_type, value))
    if not dropped:
        return packet, 0
    return b"".join(parts), dropped


def strip_alloc_markers(trace_bytes: bytes) -> tuple[bytes, int]:
    """Drop liballoc_hook's per-allocation `memory_*` ftrace print events.

    Keeps everything else byte-for-byte, including the `malloc_hook_peak_snapshot`
    markers, counters, native TrackEvents, compact_sched and the app's own atrace.
    Recurses into `compressed_packets`, re-deflating only the blobs that changed.

    Returns (new_trace_bytes, n_dropped_events).
    """
    out = bytearray()
    total = 0
    for root_bytes, packet in P.iter_trace_packet_entries(trace_bytes):
        if ALLOC_MARKER_TAG not in packet and _COMPRESSED_KEY not in packet:
            out += root_bytes  # cheap path: cannot hold a marker
            continue
        new_packet, dropped = _strip_packet(packet)
        if dropped:
            total += dropped
            out += P.encode_len_field(1, new_packet)  # Trace.packet (field 1 -> key 10)
        else:
            out += root_bytes
    return bytes(out), total


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


def _slice_name(info: dict, hi: int, size_bytes: int | None = None) -> str:
    """Build a readable, info-first slice label.

    以 "Top N: " 前缀开头(N 为该分配在所有分配里按大小的排名),便于在 UI 上
    直接看出这是第几大的内存分配。
    """
    top = info.get("top_index")
    mem = (f"{size_bytes / (1024.0 * 1024.0):.2f} MB"
           if size_bytes is not None else info.get("memory") or "")
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

def _sub_track_name(info: dict, hi: int, allocation_rank: int | None = None) -> str:
    """Build the sub-track display label (shown on the left of the timeline row).

    以 "[memory hook] Top N" 形式命名,便于在 UI 左侧轨道栏一眼识别是内存 hook
    的第几大分配。N 为该分配在所有分配里按大小的排名。
    """
    if allocation_rank is not None:
        return f"[memory hook] Allocation {allocation_rank}"
    return f"[memory hook] h{hi}"

def build_tracks(
    trace_bytes: bytes,
    groups: list[tuple[str, dict]],
    strip_markers: bool = True,
) -> tuple[bytes, list[tuple[str, int, int]], int]:
    """Return (new_trace_bytes, [(track_name, n_slices, n_skipped_no_timing), ...], n_stripped).

    groups 里每项是 (轨道名, hash_index -> 参数映射),各自生成一条独立的父轨道。
    多个 dump(峰值 dump / exit dump / 多进程)映射到同一份 trace 时用这个入口:
    不同进程的 hash_index 空间是独立的,把它们混进一个映射会互相覆盖,所以按 dump
    分轨道、各自独立编号。

    时间戳解析与时钟对齐对整份 trace 只做一次,所有轨道共用;uuid / 排序序号在
    多个轨道间连续分配,避免碰撞。

    `strip_markers`(默认 True)在时间戳解析之后、合并之前删掉逐分配的 memory_*
    marker——它们已经被转成干净的 native 轨道,留着只会让 Perfetto 多排十几万条
    async 轨道。见模块 docstring。
    """
    timings = extract_allocation_timings(trace_bytes)
    peak_snaps = extract_peak_snapshots(trace_bytes)
    n_stripped = 0
    if strip_markers:
        # 顺序很重要:先取完时间戳再删,否则就没东西可配对了。
        trace_bytes, n_stripped = strip_alloc_markers(trace_bytes)
    meta = P.analyze_trace(trace_bytes)
    seq = meta.overlay_sequence_id
    clk = meta.primary_clock_id

    next_uuid = meta.next_uuid
    packets = []
    stats = []
    order = 0
    for group_index, (track_name, hash_map) in enumerate(groups):
        # One output slice per concrete allocation. Stack metadata still comes
        # from the dump's hash map, but size/lifetime come from that pointer's
        # marker; never label one representative lifetime with an aggregate size.
        usable = [t for t in timings if t["hash_index"] in hash_map]
        usable.sort(
            key=lambda t: (t["size_bytes"] if t["size_bytes"] is not None else 0),
            reverse=True,
        )
        matched_hashes = {t["hash_index"] for t in usable}
        stats.append((track_name, len(usable), len(hash_map) - len(matched_hashes)))

        # 分配 UUID:parent + 每个子 track 一个
        parent_uuid = next_uuid
        next_uuid += 1
        child_uuids = []
        for _ in usable:
            child_uuids.append(next_uuid)
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
        for allocation_rank, timing in enumerate(usable, 1):
            hi = timing["hash_index"]
            child_uuid = child_uuids[allocation_rank - 1]
            child_name = _slice_name(hash_map[hi], hi, timing["size_bytes"])
            sub_track_name = _sub_track_name(hash_map[hi], hi, allocation_rank)
            # 子 track 按 top_index 作为排序权重(越小=内存越大=越靠前),
            # 缺失 top_index 时排到最后(用一个大值兜底)。
            rank = allocation_rank
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

            bts, dur = timing["ts"], timing["dur"]
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
    return new_bytes, stats, n_stripped


def build_track(
    trace_bytes: bytes,
    hash_map: dict,
    track_name: str = "Memory Top Allocations",
    strip_markers: bool = True,
) -> tuple[bytes, int, int, int]:
    """Return (new_trace_bytes, n_slices, n_skipped_no_timing, n_stripped).

    单轨道入口(build_tracks 的常用特例)。每个分配使用独立子 track(挂在父 track 下),
    避免重叠分配的 BEGIN/END 在栈式配对中交错导致生命周期错误。
    """
    new_bytes, stats, n_stripped = build_tracks(
        trace_bytes, [(track_name, hash_map)], strip_markers=strip_markers)
    _, n_slices, skipped = stats[0]
    return new_bytes, n_slices, skipped, n_stripped


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
    p.add_argument("--map", type=Path, default=None, help="hash_index -> info JSON (from process_memory_stack.py --export-hash-map). Defaults to <hook_root>/hash_index_map.json when omitted.")
    p.add_argument("--output", type=Path, help="Output path. Default <trace>.toptrack.<suffix>.")
    p.add_argument("--track-name", default="Memory Top Allocations", help="Name of the new track.")
    p.add_argument(
        "--keep-alloc-markers",
        action="store_true",
        help="Keep liballoc_hook's raw per-allocation memory_* trace_marker events in the "
             "output. They are dropped by default: once this tool has turned them into the "
             "native track they only cost size and force Perfetto to lay out one async track "
             "per allocation (170k+ on a default BACKTRACE_MIN_SIZE run).",
    )
    return p


# scripts/ lives directly under the hook root; process_memory_stack.py --export-hash-map
# writes its JSON there by default, so we look for it in the same place.
HOOK_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_MAP_PATH = Path(HOOK_ROOT) / "hash_index_map.json"


def default_output_path(trace_path: Path) -> Path:
    suffix = trace_path.suffix or ".perfetto"
    return trace_path.with_name(f"{trace_path.stem}.toptrack{suffix}")


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    if not args.trace.is_file():
        print(f"Error: trace not found: {args.trace}")
        return 1
    map_path = args.map or DEFAULT_MAP_PATH
    if not map_path.is_file():
        hint = "" if args.map else f" (default {DEFAULT_MAP_PATH}; pass --map or run process_memory_stack.py --export-hash-map first)"
        print(f"Error: map not found: {map_path}{hint}")
        return 1
    hash_map = _normalize_map(json.loads(map_path.read_text(encoding="utf-8")))
    if not hash_map:
        print("Error: map JSON empty or unparseable.")
        return 1
    trace_bytes = args.trace.read_bytes()
    new_bytes, n, skipped, n_stripped = build_track(
        trace_bytes, hash_map, args.track_name,
        strip_markers=not args.keep_alloc_markers)
    out = args.output or default_output_path(args.trace)
    out.write_bytes(new_bytes)
    print(f"wrote {out}")
    print(f"  track            : {args.track_name!r}")
    print(f"  slices added     : {n}")
    print(f"  skipped (no ts)  : {skipped}")
    if args.keep_alloc_markers:
        print("  alloc markers    : kept (--keep-alloc-markers)")
    else:
        print(f"  alloc markers    : stripped {n_stripped}")
    print(f"  size {len(trace_bytes)} -> {len(new_bytes)} bytes")
    if n == 0:
        # Stripping is the default, so the most likely cause is being handed an output
        # of a previous run. Say so instead of silently writing a trace with an empty track.
        print(
            "Warning: no allocation lifetimes matched. The input trace carries no "
            "memory_* markers — either the run had MALLOC_HOOK_TRACE_ALLOC unset, or "
            "this trace is already an output of this tool (markers are stripped by "
            "default). Re-run against the original trace from the device.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
