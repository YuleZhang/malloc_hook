from __future__ import annotations

import argparse
import csv
import re
import struct
import zlib
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

REALTIME_CLOCK_ID = 1
DEFAULT_GROUP_NAME = "Memory Counters"
DEFAULT_UNIT_NAME = "MiB"
SEQ_INCREMENTAL_STATE_CLEARED = 1
SEQ_NEEDS_INCREMENTAL_STATE = 2
OFFSET_SPREAD_LIMIT_NS = 50_000_000
COUNTER_EVENT_TYPE = 4
DOUBLE_COUNTER_VALUE_FIELD = 44
HTML_OVERLAY_PID = 9_999_001
HTML_OVERLAY_THREAD_NAME = "memory_overlay"
HTML_AUTO_PID_COUNTER_NAME = "MemPool Free Memory"
HTML_TRACE_DATA_SCRIPT_RE = re.compile(
    r'(?P<open>^[ \t]*<script class="trace-data" type="application/text">\n)'
    r"(?P<body>.*?)"
    r"(?P<close>[ \t]*</script>)",
    re.MULTILINE | re.DOTALL,
)
FTRACE_LINE_RE = re.compile(
    r"^\s*(?P<thread>.+)-(?P<pid>\d+)\s+"
    r"(?:\(\s*(?:\d+|-+)\)\s+)?\[(?P<cpu>\d+)\]\s+\S+\s+"
    r"(?P<timestamp>\d+\.\d+):\s+(?P<event>\S+):(?P<details>.*)$",
    re.MULTILINE,
)
FTRACE_MARKER_EVENTS = {"tracing_mark_write", "0"}
ATRACE_COUNTER_RE = re.compile(r"\s*C\|(?P<pid>\d+)\|(?P<name>[^|]+)\|")
ATRACE_TARGET_PID_RE = re.compile(r"\s*[BCSFX]\|(?P<pid>\d+)\|")


@dataclass(frozen=True)
class TraceMetadata:
    primary_clock_id: int
    offset_ns: int
    trace_min_ns: int
    trace_max_ns: int
    overlay_sequence_id: int
    next_uuid: int


@dataclass(frozen=True)
class HtmlTraceMetadata:
    trace_min_us: int
    trace_max_us: int
    anchor_us: int
    overlay_pid: int
    overlay_thread_name: str
    overlay_cpu: int


@dataclass(frozen=True)
class HtmlMarkerContext:
    timestamp_us: int
    overlay_pid: int
    overlay_thread_name: str
    overlay_cpu: int


@dataclass(frozen=True)
class OverlayLayout:
    sequence_id: int
    parent_uuid: int
    track_uuids: dict[str, int]
    row_count: int
    mapped_min_ns: int
    mapped_max_ns: int
    timestamps: tuple[int, ...]


@dataclass(frozen=True)
class OverlayPacketEntry:
    root_bytes: bytes
    sort_timestamp_ns: int
    sort_order: int


@dataclass(frozen=True)
class OverlaySummary:
    output_path: Path
    columns: list[str]
    row_count: int
    primary_clock_id: int
    trace_range_ns: tuple[int, int]
    overlay_range_ns: tuple[int, int]
    descriptor_count: int
    counter_event_count: int


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


def validate_overlay_range(metadata: TraceMetadata, mapped_min_ns: int, mapped_max_ns: int) -> None:
    if mapped_max_ns < metadata.trace_min_ns or mapped_min_ns > metadata.trace_max_ns:
        raise ValueError(
            "mapped CSV timestamps do not overlap the trace time range; "
            "please confirm the CSV and trace belong to the same capture"
        )


def split_columns(raw_columns: list[str] | None) -> list[str] | None:
    if not raw_columns:
        return None
    return [item.strip() for chunk in raw_columns for item in chunk.split(",") if item.strip()]


def load_csv_rows(csv_path: Path) -> tuple[list[str], str, list[dict[str, str]]]:
    with csv_path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames:
            raise ValueError("CSV has no header row")
        fieldnames = list(reader.fieldnames)
        field_map = {name.strip().lower(): name for name in fieldnames}
        time_column = field_map.get("time")
        if not time_column:
            raise ValueError("CSV must include a 'time' column containing epoch milliseconds")
        rows = list(reader)
    return fieldnames, time_column, rows


def detect_numeric_columns(
    fieldnames: list[str],
    time_column: str,
    rows: list[dict[str, str]],
) -> tuple[list[str], dict[str, tuple[int, str]]]:
    numeric_columns: list[str] = []
    invalid_columns: dict[str, tuple[int, str]] = {}
    for name in fieldnames:
        if name == time_column:
            continue
        for row_index, row in enumerate(rows, start=2):
            raw_value = row.get(name, "")
            try:
                float(raw_value)
            except (TypeError, ValueError):
                invalid_columns[name] = (row_index, raw_value)
                break
        else:
            numeric_columns.append(name)
    return numeric_columns, invalid_columns


def load_csv_samples(csv_path: Path, selected_columns: list[str] | None):
    fieldnames, time_column, rows = load_csv_rows(csv_path)
    numeric_columns, invalid_columns = detect_numeric_columns(fieldnames, time_column, rows)
    if selected_columns:
        missing = [name for name in selected_columns if name not in fieldnames or name == time_column]
        if missing:
            raise ValueError(f"unknown CSV columns: {', '.join(missing)}")
        invalid_selection = [name for name in selected_columns if name not in numeric_columns]
        if invalid_selection:
            name = invalid_selection[0]
            row_index, raw_value = invalid_columns.get(name, (2, ""))
            raise ValueError(
                f"CSV column '{name}' is not fully numeric; "
                f"row {row_index} contains {raw_value!r}"
            )
        numeric_columns = selected_columns
    if not numeric_columns:
        raise ValueError("CSV must contain at least one numeric value column besides 'time'")
    samples = []
    for row_index, row in enumerate(rows, start=2):
        raw_time = row.get(time_column, "")
        try:
            timestamp_ms = int(raw_time)
        except (TypeError, ValueError):
            raise ValueError(f"CSV time value at row {row_index} is invalid: {raw_time!r}") from None
        values = []
        for name in numeric_columns:
            raw_value = row.get(name, "")
            try:
                values.append(float(raw_value))
            except (TypeError, ValueError):
                raise ValueError(
                    f"CSV column '{name}' has a non-numeric value at row {row_index}: {raw_value!r}"
                ) from None
        samples.append((timestamp_ms, values))
    if not samples:
        raise ValueError("CSV must contain at least one data row")
    return numeric_columns, samples


def map_sample_timestamps(samples: list[tuple[int, list[float]]], offset_ns: int) -> list[int]:
    return [timestamp_ms * 1_000_000 - offset_ns for timestamp_ms, _ in samples]


def parse_ftrace_timestamp_us(timestamp: str) -> int:
    seconds, fractional = timestamp.split(".", 1)
    return int(seconds) * 1_000_000 + int((fractional + "000000")[:6])


def parse_atrace_target_pid(details: str, fallback_pid: int) -> int:
    match = ATRACE_TARGET_PID_RE.match(details)
    return int(match.group("pid")) if match else fallback_pid


def parse_atrace_counter(details: str) -> tuple[int, str] | None:
    match = ATRACE_COUNTER_RE.match(details)
    if not match:
        return None
    return int(match.group("pid")), match.group("name")


def html_marker_context(line_match: re.Match[str]) -> HtmlMarkerContext:
    line_pid = int(line_match.group("pid"))
    return HtmlMarkerContext(
        parse_ftrace_timestamp_us(line_match.group("timestamp")),
        parse_atrace_target_pid(line_match.group("details"), line_pid),
        line_match.group("thread").strip(),
        int(line_match.group("cpu")),
    )


def select_html_marker_context(
    trace_min_us: int,
    first_marker: HtmlMarkerContext | None,
    auto_counter_marker: HtmlMarkerContext | None,
    requested_pid_marker: HtmlMarkerContext | None,
    html_pid: int | None,
) -> HtmlMarkerContext:
    if html_pid is not None:
        if requested_pid_marker is not None:
            return requested_pid_marker
        fallback = first_marker or HtmlMarkerContext(
            trace_min_us,
            html_pid,
            HTML_OVERLAY_THREAD_NAME,
            0,
        )
        return HtmlMarkerContext(
            fallback.timestamp_us,
            html_pid,
            fallback.overlay_thread_name,
            fallback.overlay_cpu,
        )
    if auto_counter_marker is not None:
        return auto_counter_marker
    if first_marker is not None:
        return first_marker
    return HtmlMarkerContext(trace_min_us, HTML_OVERLAY_PID, HTML_OVERLAY_THREAD_NAME, 0)


def analyze_html_trace(html_text: str, html_pid: int | None = None) -> HtmlTraceMetadata:
    trace_min_us = 1 << 63
    trace_max_us = 0
    first_marker = None
    auto_counter_marker = None
    requested_pid_marker = None
    for match in HTML_TRACE_DATA_SCRIPT_RE.finditer(html_text):
        for line_match in FTRACE_LINE_RE.finditer(match.group("body")):
            timestamp_us = parse_ftrace_timestamp_us(line_match.group("timestamp"))
            trace_min_us = min(trace_min_us, timestamp_us)
            trace_max_us = max(trace_max_us, timestamp_us)
            if line_match.group("event") not in FTRACE_MARKER_EVENTS:
                continue
            context = html_marker_context(line_match)
            if first_marker is None:
                first_marker = context
            if html_pid is not None and requested_pid_marker is None and context.overlay_pid == html_pid:
                requested_pid_marker = context
            counter = parse_atrace_counter(line_match.group("details"))
            if (
                html_pid is None
                and auto_counter_marker is None
                and counter is not None
                and counter[1] == HTML_AUTO_PID_COUNTER_NAME
            ):
                auto_counter_marker = context
    if trace_max_us == 0:
        raise ValueError("HTML trace lacks parseable ftrace timestamp lines")
    selected_marker = select_html_marker_context(
        trace_min_us,
        first_marker,
        auto_counter_marker,
        requested_pid_marker,
        html_pid,
    )
    return HtmlTraceMetadata(
        trace_min_us,
        trace_max_us,
        selected_marker.timestamp_us,
        selected_marker.overlay_pid,
        selected_marker.overlay_thread_name,
        selected_marker.overlay_cpu,
    )


def map_html_sample_timestamps(samples: list[tuple[int, list[float]]], metadata: HtmlTraceMetadata) -> list[int]:
    csv_min_us = min(timestamp_ms for timestamp_ms, _ in samples) * 1_000
    csv_max_us = max(timestamp_ms for timestamp_ms, _ in samples) * 1_000
    if csv_max_us >= metadata.trace_min_us and csv_min_us <= metadata.trace_max_us:
        offset_us = 0
    else:
        offset_us = samples[0][0] * 1_000 - metadata.anchor_us
    return [timestamp_ms * 1_000 - offset_us for timestamp_ms, _ in samples]


def validate_html_overlay_range(metadata: HtmlTraceMetadata, mapped_min_us: int, mapped_max_us: int) -> None:
    if mapped_max_us < metadata.trace_min_us or mapped_min_us > metadata.trace_max_us:
        raise ValueError(
            "mapped CSV timestamps do not overlap the trace time range; "
            "please confirm the CSV and trace belong to the same capture"
        )


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


def make_counter_packet(sequence_id: int, track_uuid: int, timestamp_ns: int, clock_id: int, value: float) -> bytes:
    event = b"".join(
        (
            encode_var_field(9, COUNTER_EVENT_TYPE),
            encode_var_field(11, track_uuid),
            encode_double_field(DOUBLE_COUNTER_VALUE_FIELD, value),
        )
    )
    fields = [encode_var_field(8, timestamp_ns), encode_var_field(58, clock_id), encode_len_field(11, event)]
    return make_packet(fields, sequence_id, SEQ_NEEDS_INCREMENTAL_STATE)


def build_overlay(
    columns: list[str],
    samples: list[tuple[int, list[float]]],
    metadata: TraceMetadata,
    group_name: str,
    track_prefix: str | None,
    unit_name: str,
) -> tuple[list[OverlayPacketEntry], OverlayLayout]:
    parent_uuid = metadata.next_uuid
    track_uuids = {column: parent_uuid + index + 1 for index, column in enumerate(columns)}
    timestamps = tuple(map_sample_timestamps(samples, metadata.offset_ns))
    validate_overlay_range(metadata, min(timestamps), max(timestamps))
    overlay_start_ns = min(timestamps)
    packets = [
        OverlayPacketEntry(
            make_descriptor_packet(
                metadata.overlay_sequence_id,
                parent_uuid,
                group_name,
                None,
                None,
                first_packet=True,
            ),
            overlay_start_ns,
            0,
        )
    ]
    for column in columns:
        track_name = f"{track_prefix}/{column}" if track_prefix else column
        packets.append(
            OverlayPacketEntry(
                make_descriptor_packet(
                    metadata.overlay_sequence_id,
                    track_uuids[column],
                    track_name,
                    parent_uuid,
                    unit_name,
                ),
                overlay_start_ns,
                len(packets),
            )
        )
    for timestamp_ns, (_, values) in zip(timestamps, samples):
        for column, value in zip(columns, values):
            packets.append(
                OverlayPacketEntry(
                    make_counter_packet(
                        metadata.overlay_sequence_id,
                        track_uuids[column],
                        timestamp_ns,
                        metadata.primary_clock_id,
                        value,
                    ),
                    timestamp_ns,
                    len(packets),
                )
            )
    layout = OverlayLayout(
        metadata.overlay_sequence_id,
        parent_uuid,
        track_uuids,
        len(samples),
        min(timestamps),
        max(timestamps),
        timestamps,
    )
    return packets, layout


def format_ftrace_timestamp(timestamp_us: int) -> str:
    return f"{timestamp_us // 1_000_000}.{timestamp_us % 1_000_000:06d}"


def html_counter_value(value: float) -> int:
    return int(round(value))


def html_track_name(group_name: str, track_prefix: str | None, name: str) -> str:
    parts = [group_name]
    if track_prefix:
        parts.append(track_prefix)
    parts.append(name)
    return "/".join(part for part in parts if part)


def make_html_counter_line(metadata: HtmlTraceMetadata, timestamp_us: int, track_name: str, value: float) -> str:
    return (
        f"{metadata.overlay_thread_name[:16]:>16}-{metadata.overlay_pid:<5} [{metadata.overlay_cpu:03d}] ..... "
        f"{format_ftrace_timestamp(timestamp_us)}: tracing_mark_write: "
        f"C|{metadata.overlay_pid}|{track_name}|{html_counter_value(value)}"
    )


def build_html_overlay_lines(
    columns: list[str],
    samples: list[tuple[int, list[float]]],
    metadata: HtmlTraceMetadata,
    group_name: str,
    track_prefix: str | None,
) -> tuple[list[tuple[int, str]], tuple[int, int]]:
    timestamps = map_html_sample_timestamps(samples, metadata)
    validate_html_overlay_range(metadata, min(timestamps), max(timestamps))
    overlay_lines: list[tuple[int, str]] = []
    for timestamp_us, (_, values) in zip(timestamps, samples):
        for column, value in zip(columns, values):
            track_name = html_track_name(group_name, track_prefix, column)
            overlay_lines.append((timestamp_us, make_html_counter_line(metadata, timestamp_us, track_name, value)))
    return overlay_lines, (min(timestamps), max(timestamps))


def merge_html_overlay_lines(trace_body: str, overlay_lines: list[tuple[int, str]]) -> str:
    pending_lines = sorted(enumerate(overlay_lines), key=lambda item: (item[1][0], item[0]))
    next_overlay_index = 0
    merged_lines = []
    for line in trace_body.splitlines(keepends=True):
        line_match = FTRACE_LINE_RE.match(line)
        if line_match:
            timestamp_us = parse_ftrace_timestamp_us(line_match.group("timestamp"))
            while (
                next_overlay_index < len(pending_lines)
                and pending_lines[next_overlay_index][1][0] <= timestamp_us
            ):
                merged_lines.append(pending_lines[next_overlay_index][1][1] + "\n")
                next_overlay_index += 1
        merged_lines.append(line)
    while next_overlay_index < len(pending_lines):
        merged_lines.append(pending_lines[next_overlay_index][1][1] + "\n")
        next_overlay_index += 1
    return "".join(merged_lines)


def insert_html_overlay_lines(html_text: str, overlay_lines: list[tuple[int, str]]) -> str:
    for match in HTML_TRACE_DATA_SCRIPT_RE.finditer(html_text):
        body = match.group("body")
        if FTRACE_LINE_RE.search(body):
            merged_body = merge_html_overlay_lines(body, overlay_lines)
            return html_text[: match.start("body")] + merged_body + html_text[match.end("body") :]
    raise ValueError("HTML trace lacks a parseable ftrace trace-data block")


def verify_html_overlay(
    html_text: str,
    columns: list[str],
    row_count: int,
    metadata: HtmlTraceMetadata,
    group_name: str,
) -> tuple[int, int]:
    marker = f"C|{metadata.overlay_pid}|{group_name}"
    overlay_counter_count = html_text.count(marker)
    expected_counter_events = row_count * len(columns)
    if overlay_counter_count != expected_counter_events:
        raise ValueError("HTML overlay verification failed: counter event count mismatch")
    descriptor_count = len(columns)
    return descriptor_count, expected_counter_events


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


def parse_counter_event(payload: bytes) -> tuple[int | None, bool, float | None]:
    track_uuid = None
    is_counter = False
    counter_value = None
    for field_number, wire_type, value in iter_fields(payload):
        if field_number == 9 and wire_type == 0:
            is_counter = value == COUNTER_EVENT_TYPE
        elif field_number == 11 and wire_type == 0:
            track_uuid = value
        elif field_number == 30 and wire_type == 0:
            counter_value = float(value)
        elif field_number == DOUBLE_COUNTER_VALUE_FIELD and wire_type == 1:
            counter_value = struct.unpack("<d", value)[0]
    return track_uuid, is_counter and counter_value is not None, counter_value


def verify_overlay(trace_bytes: bytes, layout: OverlayLayout, primary_clock_id: int) -> tuple[int, int]:
    expected_tracks = {layout.parent_uuid, *layout.track_uuids.values()}
    observed_events = {uuid: [] for uuid in layout.track_uuids.values()}
    seen_descriptors: set[int] = set()
    overlay_event_positions: list[int] = []
    original_timestamp_positions: list[int] = []
    for packet_index, packet in enumerate(iter_trace_packets(trace_bytes)):
        packet_sequence = packet_clock_id = packet_timestamp = None
        event_payload = None
        for field_number, wire_type, value in iter_fields(packet):
            if field_number == 8 and wire_type == 0:
                packet_timestamp = value
            elif field_number == 10 and wire_type == 0:
                packet_sequence = value
            elif field_number == 58 and wire_type == 0:
                packet_clock_id = value
            elif field_number == 60 and wire_type == 2 and packet_sequence == layout.sequence_id:
                uuid = parse_track_uuid(value)
                if uuid in expected_tracks:
                    seen_descriptors.add(uuid)
            elif field_number == 11 and wire_type == 2 and packet_sequence == layout.sequence_id:
                event_payload = value
        if packet_timestamp is not None and packet_sequence != layout.sequence_id:
            original_timestamp_positions.append(packet_index)
        if event_payload and packet_timestamp is not None:
            track_uuid, valid_counter, counter_value = parse_counter_event(event_payload)
            if valid_counter and track_uuid in observed_events:
                overlay_event_positions.append(packet_index)
                observed_events[track_uuid].append((packet_timestamp, packet_clock_id, counter_value))
    if seen_descriptors != expected_tracks:
        raise ValueError("overlay verification failed: missing track descriptors in output trace")
    for track_uuid in layout.track_uuids.values():
        track_events = observed_events[track_uuid]
        if len(track_events) != layout.row_count:
            raise ValueError("overlay verification failed: counter event count mismatch")
        if any(clock_id != primary_clock_id for _, clock_id, _ in track_events):
            raise ValueError("overlay verification failed: counter clock id mismatch")
        observed_timestamps = tuple(timestamp_ns for timestamp_ns, _, _ in track_events)
        if observed_timestamps != layout.timestamps:
            raise ValueError("overlay verification failed: counter timestamps changed during overlay encoding")
    if overlay_event_positions and original_timestamp_positions:
        if max(overlay_event_positions) > max(original_timestamp_positions):
            raise ValueError("overlay verification failed: overlay events were appended as a late tail block")
    counter_event_count = sum(len(observed_events[uuid]) for uuid in layout.track_uuids.values())
    return len(seen_descriptors), counter_event_count


def default_output_path(trace_path: Path) -> Path:
    suffix = trace_path.suffix or ".perfetto"
    return trace_path.with_name(f"{trace_path.stem}.memory-overlay{suffix}")


def is_html_trace_path(trace_path: Path) -> bool:
    return trace_path.suffix.lower() in {".html", ".htm"}


def create_html_overlay(
    trace_path: Path,
    csv_path: Path,
    output_path: Path | None,
    selected_columns: list[str] | None,
    group_name: str,
    track_prefix: str | None,
    unit_name: str,
    html_pid: int | None = None,
) -> OverlaySummary:
    columns, samples = load_csv_samples(csv_path, selected_columns)
    html_text = trace_path.read_text(encoding="utf-8")
    metadata = analyze_html_trace(html_text, html_pid)
    overlay_lines, overlay_range_us = build_html_overlay_lines(
        columns,
        samples,
        metadata,
        group_name,
        track_prefix,
    )
    final_path = output_path or default_output_path(trace_path)
    final_text = insert_html_overlay_lines(html_text, overlay_lines)
    descriptor_count, counter_event_count = verify_html_overlay(
        final_text,
        columns,
        len(samples),
        metadata,
        group_name,
    )
    final_path.write_text(final_text, encoding="utf-8")
    return OverlaySummary(
        final_path,
        columns,
        len(samples),
        0,
        (metadata.trace_min_us * 1_000, metadata.trace_max_us * 1_000),
        (overlay_range_us[0] * 1_000, overlay_range_us[1] * 1_000),
        descriptor_count,
        counter_event_count,
    )


def create_overlay(
    trace_path: Path,
    csv_path: Path,
    output_path: Path | None,
    selected_columns: list[str] | None,
    group_name: str,
    track_prefix: str | None,
    unit_name: str,
    html_pid: int | None = None,
) -> OverlaySummary:
    if is_html_trace_path(trace_path):
        return create_html_overlay(
            trace_path,
            csv_path,
            output_path,
            selected_columns,
            group_name,
            track_prefix,
            unit_name,
            html_pid,
        )
    columns, samples = load_csv_samples(csv_path, selected_columns)
    trace_bytes = trace_path.read_bytes()
    metadata = analyze_trace(trace_bytes)
    overlay_packets, layout = build_overlay(columns, samples, metadata, group_name, track_prefix, unit_name)
    final_path = output_path or default_output_path(trace_path)
    final_bytes = merge_overlay_packets(trace_bytes, overlay_packets)
    descriptor_count, counter_event_count = verify_overlay(final_bytes, layout, metadata.primary_clock_id)
    final_path.write_bytes(final_bytes)
    return OverlaySummary(
        final_path,
        columns,
        len(samples),
        metadata.primary_clock_id,
        (metadata.trace_min_ns, metadata.trace_max_ns),
        (layout.mapped_min_ns, layout.mapped_max_ns),
        descriptor_count,
        counter_event_count,
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Map CSV memory samples into an existing Perfetto or Android systrace HTML trace without third-party dependencies."
    )
    parser.add_argument("--trace", type=Path, help="Input .perfetto or .html trace file.")
    parser.add_argument("--csv", type=Path, required=True, help="Input CSV file with a 'time' epoch-ms column.")
    parser.add_argument("--output", type=Path, help="Output path. Defaults to <trace>.memory-overlay.<suffix>.")
    parser.add_argument("--column", action="append", help="Selected CSV value columns. Repeat or pass a comma-separated list. Defaults to all numeric columns.")
    parser.add_argument("--group-name", default=DEFAULT_GROUP_NAME, help="Name of the parent track group.")
    parser.add_argument("--track-prefix", help="Optional prefix added to each track name.")
    parser.add_argument("--unit-name", default=DEFAULT_UNIT_NAME, help="Perfetto counter unit name.")
    parser.add_argument(
        "--html-pid",
        type=int,
        help=(
            "For .html traces, attach overlay counters to this atrace PID. "
            f"If omitted, the first PID with '{HTML_AUTO_PID_COUNTER_NAME}' is used when present."
        ),
    )
    parser.add_argument("--list-columns", action="store_true", help="List available numeric columns from the CSV and exit.")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    columns, _ = load_csv_samples(args.csv, None)
    if args.list_columns:
        print("\n".join(columns))
        return 0
    if not args.trace:
        raise SystemExit("--trace is required unless --list-columns is used")
    summary = create_overlay(
        args.trace,
        args.csv,
        args.output,
        split_columns(args.column),
        args.group_name,
        args.track_prefix,
        args.unit_name,
        args.html_pid,
    )
    clock_label = "html" if is_html_trace_path(args.trace) else str(summary.primary_clock_id)
    print(f"wrote {summary.output_path}")
    print(f"columns={','.join(summary.columns)} rows={summary.row_count} clock_id={clock_label}")
    print(f"trace_range_ns={summary.trace_range_ns[0]}..{summary.trace_range_ns[1]}")
    print(f"overlay_range_ns={summary.overlay_range_ns[0]}..{summary.overlay_range_ns[1]}")
    print(f"verified descriptors={summary.descriptor_count} counter_events={summary.counter_event_count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
