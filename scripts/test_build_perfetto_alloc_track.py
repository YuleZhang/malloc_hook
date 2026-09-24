#!/usr/bin/env python3
"""Focused regression tests for allocation marker pairing."""

import os
import sys
import unittest

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


if __name__ == "__main__":
    unittest.main()
