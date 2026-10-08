#!/usr/bin/env python3
"""Focused regression tests for allocation marker pairing."""

import os
import sys
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import build_perfetto_alloc_track as alloc_track
import perfetto_proto as P


def trace_with_markers(markers):
    events = []
    for ts, marker in markers:
        print_event = P.encode_str_field(2, marker)
        event = b"".join([
            P.encode_var_field(1, ts),
            P.encode_len_field(3, print_event),
        ])
        events.append(P.encode_len_field(2, event))
    return P.make_packet(
        [P.encode_len_field(1, b"".join(events))],
        sequence_id=1,
        flags=None,
    )


def clock_snapshot_packet(primary_clock_id=6, primary_ns=1_000, realtime_ns=5_000):
    """A minimal clock_snapshot so perfetto_proto.analyze_trace can map timestamps."""
    clocks = b"".join([
        P.encode_len_field(1, b"".join([
            P.encode_var_field(1, P.REALTIME_CLOCK_ID),
            P.encode_var_field(2, realtime_ns),
        ])),
        P.encode_len_field(1, b"".join([
            P.encode_var_field(1, primary_clock_id),
            P.encode_var_field(2, primary_ns),
        ])),
    ])
    snapshot = clocks + P.encode_var_field(2, primary_clock_id)
    return P.make_packet(
        [P.encode_len_field(6, snapshot)], sequence_id=1, flags=None)


class AllocationTimingTest(unittest.TestCase):
    def test_preserves_each_pointer_for_shared_stack(self):
        trace = trace_with_markers([
            (10, "S|7|memory_host@0x100.h42.s4096|256"),
            (20, "S|7|memory_host@0x200.h42.s8192|512"),
            (40, "F|7|memory_host@0x100.h42.s4096|256"),
            (70, "F|7|memory_host@0x200.h42.s8192|512"),
        ])

        timings = alloc_track.extract_allocation_timings(trace)

        self.assertEqual(2, len(timings))
        self.assertEqual(
            [(42, 4096, 10, 30), (42, 8192, 20, 50)],
            [(t["hash_index"], t["size_bytes"], t["ts"], t["dur"])
             for t in timings],
        )

    def test_preserves_sequential_reuse_of_same_identity(self):
        marker = "memory_mmap@0x100.h9.s4096|256"
        trace = trace_with_markers([
            (10, f"S|7|{marker}"),
            (20, f"F|7|{marker}"),
            (30, f"S|7|{marker}"),
            (50, f"F|7|{marker}"),
        ])

        timings = alloc_track.extract_allocation_timings(trace)

        self.assertEqual([(10, 10), (30, 20)],
                         [(t["ts"], t["dur"]) for t in timings])

    def test_reads_ftrace_events_inside_compressed_packets(self):
        inner = trace_with_markers([
            (10, "S|7|memory_host@0x100.h42.s4096|256"),
            (70, "F|7|memory_host@0x100.h42.s4096|256"),
        ])
        trace = P.make_packet(
            [P.encode_len_field(50, zlib.compress(inner))],
            sequence_id=1,
            flags=None,
        )

        timings = alloc_track.extract_allocation_timings(trace)

        self.assertEqual([(42, 4096, 10, 60)],
                         [(t["hash_index"], t["size_bytes"], t["ts"], t["dur"])
                          for t in timings])


class StripAllocMarkersTest(unittest.TestCase):
    def test_drops_alloc_markers_and_keeps_peak_and_app_slices(self):
        trace = trace_with_markers([
            (10, "S|7|memory_host@0x100.h42.s4096|256"),
            (20, "B|7|my_app_slice"),
            (30, "B|7|malloc_hook_peak_snapshot total_mb=12.5 host_mb=10.0 dma_mb=2.5"),
            (70, "F|7|memory_host@0x100.h42.s4096|256"),
        ])

        stripped, dropped = alloc_track.strip_alloc_markers(trace)

        self.assertEqual(2, dropped)
        self.assertNotIn(b"memory_host@0x100", stripped)
        self.assertIn(b"my_app_slice", stripped)
        # The peak marker survives: it is the source for the "[memory hook] Peak" track.
        self.assertEqual(1, len(alloc_track.extract_peak_snapshots(stripped)))
        self.assertEqual([], alloc_track.extract_allocation_timings(stripped))

    def test_leaves_marker_free_trace_byte_identical(self):
        trace = trace_with_markers([(10, "B|7|only_an_app_slice")])

        stripped, dropped = alloc_track.strip_alloc_markers(trace)

        self.assertEqual(0, dropped)
        self.assertEqual(trace, stripped)

    def test_strips_inside_compressed_packets(self):
        inner = trace_with_markers([
            (10, "S|7|memory_host@0x100.h42.s4096|256"),
            (20, "B|7|my_app_slice"),
            (70, "F|7|memory_host@0x100.h42.s4096|256"),
        ])
        trace = P.make_packet(
            [P.encode_len_field(50, zlib.compress(inner))],
            sequence_id=1,
            flags=None,
        )

        stripped, dropped = alloc_track.strip_alloc_markers(trace)

        self.assertEqual(2, dropped)
        self.assertEqual([], alloc_track.extract_allocation_timings(stripped))
        # The re-deflated blob must still inflate, and keep the non-marker event.
        packet = next(P.iter_trace_packets(stripped))
        blobs = [v for f, w, v in P.iter_fields(packet) if f == 50 and w == 2]
        self.assertEqual(1, len(blobs))
        self.assertIn(b"my_app_slice", P.decompress_packets(blobs[0]))

    def test_build_track_strips_by_default_but_honours_opt_out(self):
        trace = clock_snapshot_packet() + trace_with_markers([
            (10, "S|7|memory_host@0x100.h42.s4096|256"),
            (70, "F|7|memory_host@0x100.h42.s4096|256"),
        ])
        hash_map = {42: {"variable": "buf", "top_index": 1}}

        lean, n_slices, _, n_stripped = alloc_track.build_track(trace, hash_map)
        self.assertEqual(1, n_slices)
        self.assertEqual(2, n_stripped)
        self.assertNotIn(b"memory_host@0x100", lean)

        kept, n_slices, _, n_stripped = alloc_track.build_track(
            trace, hash_map, strip_markers=False)
        self.assertEqual(1, n_slices)
        self.assertEqual(0, n_stripped)
        self.assertIn(b"memory_host@0x100", kept)
        # Stripping is what makes the lean output smaller; the track itself is identical.
        self.assertLess(len(lean), len(kept))


if __name__ == "__main__":
    unittest.main()
