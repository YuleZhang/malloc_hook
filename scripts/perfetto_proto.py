#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""perfetto_proto —— Perfetto trace 的 protobuf 编解码共享底座。

这是内存分析工具链三个脚本共用的最底层:纯 protobuf varint 编解码、TracePacket
迭代/扫描、时钟对齐分析、以及把新 packet 按时间序并入原 trace 的合并原语。
不含任何 CSV / HTML / 符号化 / CLI 逻辑——那些属于上层(merge / build / process)。

依赖关系(单向,无环):
    process_memory_stack ─▶ build_perfetto_alloc_track ─▶ perfetto_proto ◀─ merge_memory_csv_to_perfetto

之前这些原语寄居在 merge_memory_csv_to_perfetto 里,build 既 import 它、又自己重写了
一份 varint 解析;抽成独立模块后消除重复,三个工具都依赖这一份底座。

无第三方依赖(仅标准库),函数体保持与原实现逐字一致。
"""
from __future__ import annotations

import zlib
import struct
from collections import Counter
from dataclasses import dataclass

# ---- 常量(TracePacket / 序列增量状态 / 时钟) --------------------------------
REALTIME_CLOCK_ID = 1
SEQ_INCREMENTAL_STATE_CLEARED = 1
SEQ_NEEDS_INCREMENTAL_STATE = 2
OFFSET_SPREAD_LIMIT_NS = 50_000_000


# ---- trace 元信息 / 可排序 overlay packet ------------------------------------
@dataclass(frozen=True)
class TraceMetadata:
    primary_clock_id: int
    offset_ns: int
    trace_min_ns: int
    trace_max_ns: int
    overlay_sequence_id: int
    next_uuid: int


@dataclass(frozen=True)
class OverlayPacketEntry:
    root_bytes: bytes
    sort_timestamp_ns: int
    sort_order: int


# ---- protobuf 编码原语 -------------------------------------------------------
def encode_varint(value: int) -> bytes:
    if value < 0:
        raise ValueError("varint must be non-negative")
    out = bytearray()
    while value > 0x7F:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def read_varint(buf: bytes | memoryview, offset: int) -> tuple[int, int]:
    shift = 0
    value = 0
    while True:
        byte = buf[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7


def iter_fields(buf: bytes | memoryview):
    offset = 0
    view = memoryview(buf)
    while offset < len(view):
        key, offset = read_varint(view, offset)
        field_number = key >> 3
        wire_type = key & 0x07
        if wire_type == 0:
            value, offset = read_varint(view, offset)
        elif wire_type == 1:
            value = bytes(view[offset : offset + 8])
            offset += 8
        elif wire_type == 2:
            size, offset = read_varint(view, offset)
            value = bytes(view[offset : offset + size])
            offset += size
        elif wire_type == 5:
            value = bytes(view[offset : offset + 4])
            offset += 4
        else:
            raise ValueError(f"unsupported wire type {wire_type}")
        yield field_number, wire_type, value


def encode_key(field_number: int, wire_type: int) -> bytes:
    return encode_varint((field_number << 3) | wire_type)


def encode_var_field(field_number: int, value: int) -> bytes:
    return encode_key(field_number, 0) + encode_varint(value)


def encode_len_field(field_number: int, payload: bytes) -> bytes:
    return encode_key(field_number, 2) + encode_varint(len(payload)) + payload


def encode_str_field(field_number: int, value: str) -> bytes:
    return encode_len_field(field_number, value.encode("utf-8"))


def encode_double_field(field_number: int, value: float) -> bytes:
    return encode_key(field_number, 1) + struct.pack("<d", value)


# ---- TracePacket 迭代 / 时间戳 / 解压 ----------------------------------------
def iter_trace_packets(trace_bytes: bytes):
    offset = 0
    view = memoryview(trace_bytes)
    while offset < len(view):
        key, offset = read_varint(view, offset)
        if key != 10:
            raise ValueError("trace root contains a non-TracePacket field")
        size, offset = read_varint(view, offset)
        yield bytes(view[offset : offset + size])
        offset += size


def iter_trace_packet_entries(trace_bytes: bytes):
    offset = 0
    view = memoryview(trace_bytes)
    while offset < len(view):
        entry_start = offset
        key, offset = read_varint(view, offset)
        if key != 10:
            raise ValueError("trace root contains a non-TracePacket field")
        size, offset = read_varint(view, offset)
        packet_start = offset
        packet_end = packet_start + size
        yield bytes(view[entry_start:packet_end]), bytes(view[packet_start:packet_end])
        offset = packet_end


def parse_packet_timestamp(packet: bytes) -> int | None:
    for field_number, wire_type, value in iter_fields(packet):
        if field_number == 8 and wire_type == 0:
            return value
    return None


def decompress_packets(payload: bytes) -> bytes:
    for wbits in (zlib.MAX_WBITS, -zlib.MAX_WBITS):
        try:
            return zlib.decompress(payload, wbits)
        except zlib.error:
            continue
    raise ValueError("compressed_packets is present but could not be decompressed")


# ---- 时钟快照 / track 解析 / 全 trace 扫描 -----------------------------------
def parse_clock_snapshot(payload: bytes) -> tuple[int | None, dict[int, int]]:
    primary_clock_id = None
    clocks: dict[int, int] = {}
    for field_number, wire_type, value in iter_fields(payload):
        if field_number == 1 and wire_type == 2:
            clock_id = timestamp = multiplier = None
            for sub_field, sub_wire, sub_value in iter_fields(value):
                if sub_field == 1 and sub_wire == 0:
                    clock_id = sub_value
                elif sub_field == 2 and sub_wire == 0:
                    timestamp = sub_value
                elif sub_field == 4 and sub_wire == 0:
                    multiplier = sub_value
            if clock_id is not None and timestamp is not None:
                clocks[clock_id] = timestamp * (multiplier or 1)
        elif field_number == 2 and wire_type == 0:
            primary_clock_id = value
    return primary_clock_id, clocks


def parse_track_uuid(payload: bytes) -> int | None:
    for field_number, wire_type, value in iter_fields(payload):
        if field_number == 1 and wire_type == 0:
            return value
    return None


def parse_track_name(payload: bytes) -> str | None:
    for field_number, wire_type, value in iter_fields(payload):
        if field_number == 2 and wire_type == 2:
            return value.decode("utf-8")
    return None


def scan_trace_packet(packet: bytes, state: dict[str, object]) -> None:
    snapshot_primary = None
    snapshot_clocks: dict[int, int] | None = None
    for field_number, wire_type, value in iter_fields(packet):
        if field_number == 8 and wire_type == 0:
            state["trace_min_ns"] = min(state["trace_min_ns"], value)
            state["trace_max_ns"] = max(state["trace_max_ns"], value)
        elif field_number == 10 and wire_type == 0 and value:
            state["sequence_ids"].add(value)
        elif field_number == 60 and wire_type == 2:
            uuid = parse_track_uuid(value)
            if uuid is not None:
                state["track_uuids"].add(uuid)
        elif field_number == 6 and wire_type == 2:
            snapshot_primary, snapshot_clocks = parse_clock_snapshot(value)
        elif field_number == 50 and wire_type == 2:
            for nested_packet in iter_trace_packets(decompress_packets(value)):
                scan_trace_packet(nested_packet, state)
    if snapshot_primary and REALTIME_CLOCK_ID in (snapshot_clocks or {}):
        primary_value = snapshot_clocks.get(snapshot_primary)
        if primary_value is not None:
            state["primary_ids"].append(snapshot_primary)
            state["offsets"].append(snapshot_clocks[REALTIME_CLOCK_ID] - primary_value)


def analyze_trace(trace_bytes: bytes) -> TraceMetadata:
    state = {
        "trace_min_ns": 1 << 63,
        "trace_max_ns": 0,
        "sequence_ids": set(),
        "track_uuids": set(),
        "primary_ids": [],
        "offsets": [],
    }
    for packet in iter_trace_packets(trace_bytes):
        scan_trace_packet(packet, state)
    offsets = sorted(state["offsets"])
    if not offsets:
        raise ValueError("trace lacks a usable clock_snapshot with REALTIME and primary trace clock")
    if offsets[-1] - offsets[0] > OFFSET_SPREAD_LIMIT_NS:
        raise ValueError("clock snapshot offsets are too inconsistent for a stable mapping")
    primary_clock_id = Counter(state["primary_ids"]).most_common(1)[0][0]
    return TraceMetadata(
        primary_clock_id=primary_clock_id,
        offset_ns=offsets[len(offsets) // 2],
        trace_min_ns=state["trace_min_ns"],
        trace_max_ns=state["trace_max_ns"],
        overlay_sequence_id=(max(state["sequence_ids"], default=0) + 1),
        next_uuid=(max(state["track_uuids"], default=0) + 1),
    )


# ---- packet 合成 / 时间序合并 ------------------------------------------------
def make_packet(fields: list[bytes], sequence_id: int, flags: int | None, first_packet: bool = False) -> bytes:
    packet = [encode_var_field(10, sequence_id)]
    if flags is not None:
        packet.append(encode_var_field(13, flags))
    if first_packet:
        packet.append(encode_var_field(87, 1))
    packet.extend(fields)
    return encode_len_field(1, b"".join(packet))


def make_descriptor_packet(
    sequence_id: int,
    uuid: int,
    name: str,
    parent_uuid: int | None,
    unit_name: str | None,
    first_packet: bool = False,
) -> bytes:
    descriptor = [encode_var_field(1, uuid), encode_str_field(2, name)]
    if parent_uuid is not None:
        descriptor.append(encode_var_field(5, parent_uuid))
    if unit_name:
        descriptor.append(encode_len_field(8, encode_str_field(6, unit_name)))
    return make_packet(
        [encode_len_field(60, b"".join(descriptor))],
        sequence_id,
        SEQ_INCREMENTAL_STATE_CLEARED if first_packet else SEQ_NEEDS_INCREMENTAL_STATE,
        first_packet,
    )


def merge_overlay_packets(trace_bytes: bytes, overlay_packets: list[OverlayPacketEntry]) -> bytes:
    pending_packets = sorted(overlay_packets, key=lambda packet: (packet.sort_timestamp_ns, packet.sort_order))
    next_overlay_index = 0
    merged_trace = bytearray()
    for root_bytes, packet in iter_trace_packet_entries(trace_bytes):
        packet_timestamp = parse_packet_timestamp(packet)
        if packet_timestamp is not None:
            while (
                next_overlay_index < len(pending_packets)
                and pending_packets[next_overlay_index].sort_timestamp_ns <= packet_timestamp
            ):
                merged_trace.extend(pending_packets[next_overlay_index].root_bytes)
                next_overlay_index += 1
        merged_trace.extend(root_bytes)
    while next_overlay_index < len(pending_packets):
        merged_trace.extend(pending_packets[next_overlay_index].root_bytes)
        next_overlay_index += 1
    return bytes(merged_trace)
