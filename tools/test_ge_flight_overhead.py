# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import unittest

import ge_flight_overhead as g


def bench_text(scene: int, prims: int, fb: str = "00ff", check: bool = True) -> str:
    lines = []
    if check:
        lines.append(
            "GE_FLIGHT_BENCH flavour=linked class_check calls=20000000 "
            "ns_per_call_x1000=1800 sink=0"
        )
    lines.append(
        f"GE_FLIGHT_BENCH flavour=x workload=scene prims=0 rounds=3 "
        f"ns_per_list_median={scene} ns_per_list_min=1 fb_hash={fb}"
    )
    lines.append(
        f"GE_FLIGHT_BENCH flavour=x workload=prims prims=1024 rounds=3 "
        f"ns_per_list_median={prims} ns_per_list_min=1 fb_hash={fb}"
    )
    return "noise line\n" + "\n".join(lines) + "\n"


class ParseTests(unittest.TestCase):
    def test_parse_reads_every_workload_and_ignores_other_lines(self) -> None:
        parsed = g.parse_bench(bench_text(10, 20))
        self.assertEqual(set(parsed), {"class_check", "scene", "prims"})
        self.assertEqual(parsed["prims"]["ns_per_list_median"], "20")
        self.assertEqual(parsed["class_check"]["ns_per_call_x1000"], "1800")

    def test_parse_refuses_a_duplicate_workload(self) -> None:
        with self.assertRaises(ValueError):
            g.parse_bench(bench_text(1, 2) + bench_text(1, 2))

    def test_parse_refuses_a_line_without_a_workload(self) -> None:
        with self.assertRaises(ValueError):
            g.parse_bench("GE_FLIGHT_BENCH flavour=x foo=1\n")

    def test_flight_symbols_are_listed_once(self) -> None:
        text = "         U sr_flight_record\n         U sr_flight_record\n  U sr_flight_class_enabled\n"
        self.assertEqual(g.undefined_flight_symbols(text), ["sr_flight_class_enabled", "sr_flight_record"])
        self.assertEqual(g.undefined_flight_symbols("T ge_run_list\n"), [])


class StatisticsTests(unittest.TestCase):
    def test_interval_is_deterministic_and_brackets_the_median(self) -> None:
        diffs = [5.0, -3.0, 2.0, 8.0, -1.0, 4.0]
        first = g.bootstrap_interval(diffs)
        self.assertEqual(first, g.bootstrap_interval(diffs))
        self.assertLessEqual(first[0], 3.0)
        self.assertGreaterEqual(first[1], 3.0)

    def test_constant_difference_has_a_degenerate_interval(self) -> None:
        self.assertEqual(g.bootstrap_interval([7.0] * 4), (7.0, 7.0))

    def test_identical_runs_are_not_distinguishable(self) -> None:
        run = g.parse_bench(bench_text(100, 200))
        report = g.summarize({"linked": [run, run], "absent": [run, run]}, "ns_per_list_median")
        self.assertTrue(any("scene" in line and "not distinguishable" in line for line in report))
        self.assertTrue(any(line.startswith("class_check: 1.800 ns") for line in report))

    def test_a_consistent_slowdown_is_reported_as_a_difference(self) -> None:
        slow = [g.parse_bench(bench_text(150 + i, 200)) for i in range(5)]
        fast = [g.parse_bench(bench_text(100 + i, 200, check=False)) for i in range(5)]
        report = g.summarize({"linked": slow, "absent": fast}, "ns_per_list_median")
        scene = next(line for line in report if line.startswith("scene"))
        self.assertIn("linked slower", scene)
        self.assertIn("delta=[+50, +50]", scene)

    def test_a_consistent_speedup_is_not_called_a_saving(self) -> None:
        fast = [g.parse_bench(bench_text(100 + i, 200)) for i in range(5)]
        slow = [g.parse_bench(bench_text(150 + i, 200, check=False)) for i in range(5)]
        scene = next(
            line
            for line in g.summarize({"linked": fast, "absent": slow}, "ns_per_list_median")
            if line.startswith("scene")
        )
        self.assertIn("linked faster (code layout or drift", scene)

    def test_different_pixels_refuse_the_comparison(self) -> None:
        a = g.parse_bench(bench_text(100, 200, fb="aa"))
        b = g.parse_bench(bench_text(100, 200, fb="bb"))
        with self.assertRaises(ValueError):
            g.summarize({"linked": [a], "absent": [b]}, "ns_per_list_median")

    def test_unequal_run_counts_are_refused(self) -> None:
        run = g.parse_bench(bench_text(1, 2))
        with self.assertRaises(ValueError):
            g.summarize({"linked": [run, run], "absent": [run]}, "ns_per_list_median")


if __name__ == "__main__":
    unittest.main()
