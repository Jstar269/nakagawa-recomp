#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Synthetic regressions for tools/perf_attribution.py.

Every input is generated here. The source pins read the emitter's own printf literals in
src/rt/hle.c and src/rt/perf.c and render them with synthetic values, so a format change in the
runtime fails this module rather than silently changing what the report means.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

try:
    import perf_attribution
except ModuleNotFoundError:
    from tools import perf_attribution

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "perf_attribution.py"
BUF_A = 0x04000000
BUF_B = 0x04044000


def summary_doc(*, wall_ns: int = 10_000_000_000, guest_ns: int = 6_000_000_000,
                aot_ns: int = 3_000_000_000, ge_cpu_ns: int = 1_000_000_000,
                vblank_count: int = 600, presents: int = 60) -> dict[str, Any]:
    """A nakagawa-perf-v1 document that passes perf_summary_diff.validate_summary."""
    return {
        "schema": "nakagawa-perf-v1",
        "build": {"aot_instruction_hook": False},
        "wall_ns": wall_ns,
        "interval_count": 10,
        "vblanks": {"count": vblank_count, "presents": presents, "present_skips": 0},
        "guest": {"ns": guest_ns, "idle_ns": 0, "aot_ns": aot_ns, "aot_calls": 1,
                  "aot_instruction_count": None, "interpreter_ns": 0, "interpreter_calls": 0,
                  "interpreter_instructions": 0},
        "transitions": {"aot_to_interpreter_count": 0, "interpreter_to_aot_count": 0,
                        "top_pcs": {"aot_to_interpreter": [], "interpreter_to_aot": []}},
        "scheduler": {"running_ns": 0, "runnable_ns": 0, "blocked_ns": 0, "idle_ns": 0,
                      "context_switches": 0, "blocked_transitions": 0, "wake_transitions": 0},
        "vfpu": {"ns": 0, "count": 0, "interpreter_count": 0, "aot_instruction_count": 0,
                 "interpreter_ns": 0, "aot_ns": 0, "errors": 0,
                 "families": {"load": 0, "store": 0, "arithmetic": 0, "prefix": 0, "other": 0}},
        "ge": {"cpu_ns": ge_cpu_ns, "cpu_calls": 1, "transform_sample_ns": 0, "primitive_ns": 0,
               "submits": 0, "waits": 0, "wait_ns": 0, "present_submits": 0, "present_waits": 0,
               "present_wait_ns": 0},
        "vulkan": {"submit_ns": 0, "submits": 0, "wait_ns": 0, "waits": 0, "readback_ns": 0,
                   "readbacks": 0, "readback_bytes": 0, "pipeline_creation_ns": 0,
                   "pipeline_creations": 0},
        "textures": {"decode_ns": 0, "decodes": 0, "decoded_bytes": 0, "cache_hits": 0,
                     "cache_misses": 0},
        "storage": {"iso_read_ns": 0, "iso_reads": 0, "iso_read_bytes": 0, "iso_failures": 0,
                    "vfs_read_ns": 0, "vfs_reads": 0, "vfs_read_bytes": 0, "vfs_failures": 0},
        "media": {"h264_decode_ns": 0, "h264_decodes": 0, "h264_failures": 0,
                  "atrac_decode_ns": 0, "atrac_decodes": 0, "atrac_failures": 0},
        "audio": {"mix_ns": 0, "mix_calls": 0, "output_ns": 0, "output_calls": 0,
                  "output_frames": 0},
        "boundaries": [],
    }


def present_line(f: int, buf: int, fmt: int = 3, stride: int = 512) -> str:
    return f"HOST_PRESENT_SUBMITTED f={f} buf=0x{buf:08x} fmt={fmt} stride={stride}"


def perf_line(wall_ms: float, cpu_ms: float, *, vblank_total: int = 0) -> str:
    """A PERF line in the key order report_if_due prints (src/rt/perf.c)."""
    return (f"PERF vblank_total={vblank_total} wall_ms={wall_ms:.3f} fps=59.940 frame_ms=16.680 "
            f"vblank_hz=59.940 cpu_ms={cpu_ms:.3f} ge_wait_ms=0.000 present_ms=0.000 "
            "idle_ms=0.000 submits=0 ge_submits=0 present_submits=0 waits=0 readback_waits=0 "
            "present_skips=0 present_wait_ms=0.000 target30=yes")


def attrib_line(aot_ms: float, ge_cpu_ms: float) -> str:
    """A PERF_ATTRIB line in the key order report_if_due prints (src/rt/perf.c)."""
    return (f"PERF_ATTRIB aot_ms={aot_ms:.3f} aot_calls=0 aot_instructions=0 interp_ms=0.000 "
            "interp_calls=0 interp_instructions=0 aot_to_interp=0 interp_to_aot=0 vfpu_ms=0.000 "
            f"ge_cpu_ms={ge_cpu_ms:.3f} vk_submit_ms=0.000 vk_wait_ms=0.000 "
            "texture_decode_ms=0.000 iso_read_ms=0.000 vfs_read_ms=0.000 h264_ms=0.000 "
            "atrac_ms=0.000 mix_ms=0.000 output_ms=0.000")


def write_log(directory: Path, lines: list[str], name: str = "stderr.log") -> Path:
    path = directory / name
    path.write_text("".join(f"{line}\n" for line in lines), encoding="utf-8")
    return path


def write_summary(directory: Path, doc: dict[str, Any], name: str = "perf.json") -> Path:
    path = directory / name
    path.write_text(json.dumps(doc), encoding="utf-8")
    return path


def alternating_frames(f_values: list[int]) -> list[str]:
    """One present per f, with a different buf each time, so every present is a new frame."""
    return [present_line(f, BUF_A if index % 2 == 0 else BUF_B) for index, f in enumerate(f_values)]


def analyze_lines(lines: list[str], doc: dict[str, Any] | None = None) -> dict[str, Any]:
    with tempfile.TemporaryDirectory() as scratch:
        directory = Path(scratch)
        perf = write_summary(directory, doc if doc is not None else summary_doc())
        log = write_log(directory, lines)
        return perf_attribution.analyze(perf, log)


def run_cli(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(TOOL), *args], check=False, capture_output=True,
                          text=True, cwd=ROOT)


def c_literal_run(source: str, start_marker: str) -> str:
    """Concatenate the adjacent C string literals that begin at ``start_marker``."""
    start = source.index(start_marker)
    literal = re.compile(r'\s*"((?:[^"\\\n]|\\.)*)"')
    parts: list[str] = []
    position = start
    while True:
        match = literal.match(source, position)
        if match is None:
            break
        parts.append(match.group(1))
        position = match.end()
    return "".join(parts)


SAMPLE_VALUES = {"%llu": "7", "%u": "12", "%08x": "04000000", "%.3f": "1.500", "%s": "yes", "%d": "3"}


def render_c_format(literal: str) -> str:
    """Render a C printf format with sample values, the way the emitter would fill it."""
    text = literal.replace("\\n", "")
    for conversion in ("%llu", "%08x", "%.3f", "%u", "%s", "%d"):
        text = text.replace(conversion, SAMPLE_VALUES[conversion])
    return text


class TestPresentStream(unittest.TestCase):
    def test_flips_and_re_presents_on_a_known_sequence(self) -> None:
        bufs = [BUF_A, BUF_A, BUF_B, BUF_B, BUF_B, BUF_A]
        lines = [present_line(index, buf) for index, buf in enumerate(bufs)]
        result = analyze_lines(lines)
        self.assertEqual(result["presents"]["well_formed"], 6)
        self.assertEqual(result["presents"]["new_frames"], 3)
        self.assertEqual(result["presents"]["re_presents"], 3)
        self.assertEqual(result["presents"]["malformed_lines"], 0)

    def test_first_present_is_a_new_frame_with_no_interval(self) -> None:
        result = analyze_lines([present_line(42, BUF_A)])
        self.assertEqual(result["presents"]["new_frames"], 1)
        self.assertEqual(result["presents"]["re_presents"], 0)
        self.assertEqual(result["presents"]["first_f"], 42)
        stats = result["vblank_intervals"]
        self.assertEqual(stats["intervals"], 0)
        self.assertIsNone(stats["median"])
        self.assertIsNone(stats["implied_fps_median"])

    def test_empty_log_reports_zeros_and_no_statistics(self) -> None:
        result = analyze_lines([])
        self.assertEqual(result["log"]["lines"], 0)
        self.assertEqual(result["presents"]["new_frames"], 0)
        self.assertEqual(result["presents"]["first_f"], None)
        self.assertIsNone(result["vblank_intervals"]["median"])
        self.assertIsNone(result["cost_share"]["cpu"]["share"])
        warnings = " ".join(result["warnings"])
        self.assertIn("SR_PRESENT_TRACE=1", warnings)
        self.assertIn("SR_PERF=1", warnings)

    def test_malformed_host_lines_are_counted_and_not_skipped(self) -> None:
        lines = [
            present_line(1, BUF_A),
            "HOST_PRESENT_SUBMITTED nonsense",
            "HOST_PRESENT_SUBMITTED f=2 buf=0x0400000 fmt=3 stride=512",
            "HOST_PRESENT_SUBMITTED f=3 buf=0x04000000 fmt=9 stride=512",
            "HOST_PRESENT_SUBMITTED f=4 buf=0x04000000 fmt=3 stride=0",
            "HOST_PRESENT_SUBMITTED f=5 buf=0x00000000 fmt=3 stride=512",
            "HOST_PRESENT_SUBMITTED f=4294967296 buf=0x04000000 fmt=3 stride=512",
            present_line(6, BUF_B) + " trailing",
            present_line(7, BUF_B),
        ]
        result = analyze_lines(lines)
        self.assertEqual(result["presents"]["well_formed"], 2)
        self.assertEqual(result["presents"]["malformed_lines"], 7)
        self.assertEqual(result["presents"]["new_frames"], 2)
        self.assertEqual(result["presents"]["re_presents"], 0)
        self.assertIn("line 2:", " ".join(result["warnings"]))
        self.assertIn("malformed HOST_PRESENT_SUBMITTED", " ".join(result["warnings"]))

    def test_uppercase_hex_buffer_is_accepted_and_compared_as_a_number(self) -> None:
        lines = ["HOST_PRESENT_SUBMITTED f=1 buf=0x0400ABCD fmt=3 stride=512",
                 "HOST_PRESENT_SUBMITTED f=2 buf=0x0400abcd fmt=3 stride=512"]
        result = analyze_lines(lines)
        self.assertEqual(result["presents"]["well_formed"], 2)
        self.assertEqual(result["presents"]["re_presents"], 1)

    def test_lines_without_a_read_tag_are_ignored_and_counted(self) -> None:
        lines = [
            "PERF enabled: 1 Hz aggregate telemetry",
            "DISPLAY_PRESENT: refusing invalid span addr=0x04000000 stride=0 fmt=3",
            "PERF_ATTRIB vblank_late total=1 lost_ms=2 masked_periods=0",
            "BOOT_EVENT phase=frame_present backend=offscreen frame=1",
        ]
        result = analyze_lines(lines)
        self.assertEqual(result["log"]["ignored_lines"], 4)
        self.assertEqual(result["presents"]["malformed_lines"], 0)
        self.assertEqual(result["telemetry"]["malformed_lines"], 0)

    def test_crlf_line_endings_are_read(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            directory = Path(scratch)
            perf = write_summary(directory, summary_doc())
            log = directory / "stderr.log"
            log.write_bytes(b"HOST_PRESENT_SUBMITTED f=1 buf=0x04000000 fmt=3 stride=512\r\n"
                            b"HOST_PRESENT_SUBMITTED f=2 buf=0x04044000 fmt=3 stride=512\r\n")
            result = perf_attribution.analyze(perf, log)
        self.assertEqual(result["presents"]["well_formed"], 2)
        self.assertEqual(result["presents"]["malformed_lines"], 0)

    def test_overlong_lines_are_bounded_and_classified_by_their_head(self) -> None:
        long_tail = "y" * 5000
        lines = [
            "HOST_PRESENT_SUBMITTED f=1 " + long_tail,
            "unrelated junk " + long_tail,
            present_line(2, BUF_A),
        ]
        result = analyze_lines(lines)
        self.assertEqual(result["log"]["lines"], 3)
        self.assertEqual(result["presents"]["malformed_lines"], 1)
        self.assertEqual(result["log"]["ignored_lines"], 1)
        self.assertEqual(result["presents"]["well_formed"], 1)

    def test_non_utf8_log_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            directory = Path(scratch)
            perf = write_summary(directory, summary_doc())
            log = directory / "stderr.log"
            log.write_bytes(b"\xff\xfe not text\n")
            with self.assertRaisesRegex(perf_attribution.PerfAttributionError, "not valid UTF-8"):
                perf_attribution.analyze(perf, log)

    def test_missing_log_is_refused_with_its_path(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            directory = Path(scratch)
            perf = write_summary(directory, summary_doc())
            with self.assertRaisesRegex(perf_attribution.PerfAttributionError, "cannot read stderr log"):
                perf_attribution.analyze(perf, directory / "absent.log")

    def test_tag_inside_a_line_is_malformed_not_silently_skipped(self) -> None:
        lines = ["PERF_ATTRIB aot_ms=0.000 " + present_line(3, BUF_A), present_line(4, BUF_A)]
        result = analyze_lines(lines)
        self.assertEqual(result["presents"]["malformed_lines"], 1)
        self.assertEqual(result["presents"]["well_formed"], 1)

    def test_every_line_is_counted_in_exactly_one_bucket(self) -> None:
        lines = [
            present_line(1, BUF_A),
            "HOST_PRESENT_SUBMITTED bad",
            perf_line(1000.0, 500.0),
            attrib_line(100.0, 50.0),
            "PERF vblank_total=1 wall_ms=1.000",
            "PERF enabled: 1 Hz aggregate telemetry",
            present_line(2, BUF_B),
        ]
        with tempfile.TemporaryDirectory() as scratch:
            directory = Path(scratch)
            log = write_log(directory, lines)
            scan = perf_attribution.scan_log(log)
        self.assertEqual(scan.lines, len(lines))
        self.assertEqual(
            scan.lines,
            scan.presents + scan.malformed_presents + len(scan.perf_rows)
            + len(scan.attrib_rows) + scan.malformed_telemetry + scan.ignored,
        )


class TestVblankIntervals(unittest.TestCase):
    def test_percentiles_and_fps_on_a_known_sequence(self) -> None:
        # Intervals 2, 3, 2, 1, 2: sorted 1, 2, 2, 2, 3.
        result = analyze_lines(alternating_frames([10, 12, 15, 17, 18, 20]))
        stats = result["vblank_intervals"]
        self.assertEqual(stats["intervals"], 5)
        self.assertEqual(stats["median"], 2.0)
        self.assertEqual(stats["p95"], 3.0)
        self.assertEqual(stats["p99"], 3.0)
        self.assertAlmostEqual(stats["mean"], 2.0)
        self.assertAlmostEqual(stats["implied_fps_median"], 59.94 / 2.0)
        self.assertAlmostEqual(stats["implied_fps_mean"], 59.94 / 2.0)

    def test_re_presents_do_not_create_intervals_and_even_median_is_averaged(self) -> None:
        lines = [
            present_line(10, BUF_A),
            present_line(11, BUF_A),
            present_line(12, BUF_B),
            present_line(13, BUF_B),
            present_line(14, BUF_B),
            present_line(15, BUF_A),
        ]
        result = analyze_lines(lines)
        stats = result["vblank_intervals"]
        self.assertEqual(stats["intervals"], 2)
        self.assertEqual(result["presents"]["re_presents"], 3)
        self.assertEqual(stats["median"], 2.5)
        self.assertAlmostEqual(stats["implied_fps_median"], 59.94 / 2.5)

    def test_nearest_rank_percentiles_on_a_hundred_intervals(self) -> None:
        f_values = [0]
        for step in range(1, 101):
            f_values.append(f_values[-1] + step)
        result = analyze_lines(alternating_frames(f_values))
        stats = result["vblank_intervals"]
        self.assertEqual(stats["intervals"], 100)
        self.assertEqual(stats["median"], 50.5)
        self.assertEqual(stats["p95"], 95.0)
        self.assertEqual(stats["p99"], 99.0)
        self.assertAlmostEqual(stats["mean"], 50.5)

    def test_a_single_interval_is_not_a_statistic(self) -> None:
        result = analyze_lines(alternating_frames([5, 8]))
        stats = result["vblank_intervals"]
        self.assertEqual(stats["intervals"], 1)
        self.assertIsNone(stats["median"])
        self.assertIsNone(stats["p95"])
        self.assertIsNone(stats["implied_fps_mean"])
        self.assertIn("need 2 and are n/a", " ".join(result["warnings"]))

    def test_zero_median_gives_no_implied_fps_instead_of_dividing(self) -> None:
        result = analyze_lines(alternating_frames([5, 5, 5]))
        stats = result["vblank_intervals"]
        self.assertEqual(stats["intervals"], 2)
        self.assertEqual(stats["median"], 0.0)
        self.assertIsNone(stats["implied_fps_median"])
        self.assertIsNone(stats["implied_fps_mean"])

    def test_decreasing_f_between_new_frames_is_excluded_and_reported(self) -> None:
        result = analyze_lines(alternating_frames([10, 12, 5, 7]))
        stats = result["vblank_intervals"]
        self.assertEqual(stats["intervals"], 2)
        self.assertEqual(stats["median"], 2.0)
        self.assertEqual(result["presents"]["excluded_intervals"], 1)
        self.assertEqual(result["presents"]["non_monotonic"], 1)
        warnings = " ".join(result["warnings"])
        self.assertIn("below the previous present", warnings)
        self.assertIn("interval(s) excluded", warnings)

    def test_decreasing_f_on_a_re_present_is_reported(self) -> None:
        lines = [present_line(10, BUF_A), present_line(9, BUF_A), present_line(11, BUF_B)]
        result = analyze_lines(lines)
        self.assertEqual(result["presents"]["non_monotonic"], 1)
        self.assertEqual(result["presents"]["excluded_intervals"], 0)
        self.assertEqual(result["vblank_intervals"]["intervals"], 1)

    def test_equal_f_is_not_a_monotonic_violation(self) -> None:
        result = analyze_lines([present_line(10, BUF_A), present_line(10, BUF_B)])
        self.assertEqual(result["presents"]["non_monotonic"], 0)
        # One zero-length interval is recorded, but a single interval is not a statistic.
        self.assertEqual(result["vblank_intervals"]["intervals"], 1)
        self.assertIsNone(result["vblank_intervals"]["median"])


class TestCostShares(unittest.TestCase):
    def test_shares_come_from_paired_one_second_lines(self) -> None:
        lines = [
            perf_line(1000.0, 600.0), attrib_line(400.0, 100.0),
            perf_line(1000.0, 300.0), attrib_line(200.0, 50.0),
        ]
        result = analyze_lines(lines)
        cost = result["cost_share"]
        self.assertEqual(result["telemetry"]["intervals"], 2)
        self.assertAlmostEqual(result["telemetry"]["wall_ms"], 2000.0)
        self.assertAlmostEqual(cost["cpu"]["ms"], 900.0)
        self.assertAlmostEqual(cost["cpu"]["share"], 0.45)
        self.assertAlmostEqual(cost["aot"]["share"], 0.30)
        self.assertAlmostEqual(cost["ge_cpu"]["share"], 0.075)

    def test_unpaired_telemetry_is_reported_and_left_out(self) -> None:
        lines = [
            perf_line(1000.0, 500.0), attrib_line(100.0, 10.0),
            perf_line(1000.0, 500.0), attrib_line(100.0, 10.0),
            perf_line(1000.0, 999.0),
        ]
        result = analyze_lines(lines)
        self.assertEqual(result["telemetry"]["intervals"], 2)
        self.assertEqual(result["telemetry"]["unpaired_lines"], 1)
        self.assertAlmostEqual(result["telemetry"]["wall_ms"], 2000.0)
        self.assertAlmostEqual(result["cost_share"]["cpu"]["share"], 0.5)
        self.assertIn("unpaired PERF or PERF_ATTRIB", " ".join(result["warnings"]))

    def test_malformed_telemetry_line_is_counted_and_not_used(self) -> None:
        lines = [
            perf_line(1000.0, 500.0), attrib_line(100.0, 10.0),
            "PERF vblank_total=1 wall_ms=1000.000 fps=59.940",
        ]
        result = analyze_lines(lines)
        self.assertEqual(result["telemetry"]["malformed_lines"], 1)
        self.assertEqual(result["telemetry"]["intervals"], 1)
        self.assertIn("malformed PERF or PERF_ATTRIB", " ".join(result["warnings"]))

    def test_other_perf_lines_are_ignored_not_malformed(self) -> None:
        lines = [
            "PERF enabled: 1 Hz aggregate telemetry",
            "PERF_ATTRIB vblank_owed owed=1 coalesced=0 delivered=1",
            perf_line(1000.0, 500.0), attrib_line(100.0, 10.0),
        ]
        result = analyze_lines(lines)
        self.assertEqual(result["telemetry"]["malformed_lines"], 0)
        self.assertEqual(result["log"]["ignored_lines"], 2)
        self.assertEqual(result["telemetry"]["intervals"], 1)

    def test_no_telemetry_gives_no_shares_and_says_how_to_get_them(self) -> None:
        result = analyze_lines([present_line(1, BUF_A)])
        self.assertIsNone(result["cost_share"]["cpu"]["ms"])
        self.assertIsNone(result["cost_share"]["cpu"]["share"])
        self.assertIsNone(result["telemetry"]["wall_ms"])
        self.assertIn("SR_PERF=1", " ".join(result["warnings"]))


class TestSummary(unittest.TestCase):
    def test_whole_run_shares_come_from_the_summary(self) -> None:
        result = analyze_lines([present_line(1, BUF_A)], summary_doc())
        summary = result["summary"]
        self.assertAlmostEqual(summary["wall_ms"], 10_000.0)
        self.assertEqual(summary["vblanks"], 600)
        self.assertEqual(summary["presents"], 60)
        self.assertAlmostEqual(summary["guest_share"], 0.6)
        self.assertAlmostEqual(summary["aot_share"], 0.3)
        self.assertAlmostEqual(summary["ge_cpu_share"], 0.1)

    def test_zero_wall_time_gives_no_whole_run_shares(self) -> None:
        result = analyze_lines([], summary_doc(wall_ns=0, guest_ns=0, aot_ns=0, ge_cpu_ns=0))
        self.assertIsNone(result["summary"]["guest_share"])
        self.assertIsNone(result["summary"]["aot_share"])

    def test_wrong_schema_is_refused(self) -> None:
        doc = summary_doc()
        doc["schema"] = "nakagawa-perf-v0"
        with tempfile.TemporaryDirectory() as scratch:
            directory = Path(scratch)
            perf = write_summary(directory, doc)
            log = write_log(directory, [])
            with self.assertRaisesRegex(perf_attribution.PerfSummaryError, "nakagawa-perf-v1"):
                perf_attribution.analyze(perf, log)

    def test_missing_vblank_presents_is_refused_not_defaulted(self) -> None:
        doc = summary_doc()
        del doc["vblanks"]["presents"]
        with tempfile.TemporaryDirectory() as scratch:
            directory = Path(scratch)
            perf = write_summary(directory, doc)
            log = write_log(directory, [])
            with self.assertRaisesRegex(perf_attribution.PerfAttributionError, r"vblanks\.presents"):
                perf_attribution.analyze(perf, log)

    def test_negative_or_boolean_counter_is_refused(self) -> None:
        for bad in (-1, True):
            with self.subTest(value=bad):
                doc = summary_doc(presents=60)
                doc["vblanks"]["presents"] = bad
                with tempfile.TemporaryDirectory() as scratch:
                    directory = Path(scratch)
                    perf = write_summary(directory, doc)
                    log = write_log(directory, [])
                    with self.assertRaisesRegex(perf_attribution.PerfAttributionError, "non-negative"):
                        perf_attribution.analyze(perf, log)


class TestSourcePins(unittest.TestCase):
    """The formats the parser reads are the formats the runtime prints, rendered from its source."""

    def test_host_present_line_parses_as_hle_c_prints_it(self) -> None:
        source = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        literal = c_literal_run(source, '"HOST_PRESENT_SUBMITTED f=')
        self.assertEqual(literal, "HOST_PRESENT_SUBMITTED f=%u buf=0x%08x fmt=%d stride=%d\\n")
        rendered = render_c_format(literal)
        self.assertIsNotNone(perf_attribution.PRESENT_PATTERN.fullmatch(rendered), rendered)

    def test_perf_line_parses_as_perf_c_prints_it(self) -> None:
        source = (ROOT / "src" / "rt" / "perf.c").read_text(encoding="utf-8")
        literal = c_literal_run(source, '"PERF vblank_total=')
        rendered = render_c_format(literal)
        self.assertIsNotNone(perf_attribution.PERF_PATTERN.fullmatch(rendered), rendered)
        keys = re.findall(r"([a-z0-9_]+)=", literal.replace("\\n", ""))
        self.assertEqual(keys, [key for key, _ in perf_attribution.PERF_FIELDS])

    def test_perf_attrib_line_parses_as_perf_c_prints_it(self) -> None:
        source = (ROOT / "src" / "rt" / "perf.c").read_text(encoding="utf-8")
        literal = c_literal_run(source, '"PERF_ATTRIB aot_ms=')
        rendered = render_c_format(literal)
        self.assertIsNotNone(perf_attribution.ATTRIB_PATTERN.fullmatch(rendered), rendered)
        keys = re.findall(r"([a-z0-9_]+)=", literal.replace("\\n", ""))
        self.assertEqual(keys, [key for key, _ in perf_attribution.ATTRIB_FIELDS])

    def test_summary_schema_is_the_one_perf_c_writes(self) -> None:
        source = (ROOT / "src" / "rt" / "perf.c").read_text(encoding="utf-8")
        self.assertIn(r'\"schema\":\"nakagawa-perf-v1\"', source)


class TestCli(unittest.TestCase):
    def _write_run(self, directory: Path, name: str, f_values: list[int], doc: dict[str, Any] | None = None,
                   extra: list[str] | None = None) -> tuple[Path, Path]:
        sub = directory / name
        sub.mkdir()
        perf = write_summary(sub, doc if doc is not None else summary_doc())
        log = write_log(sub, alternating_frames(f_values) + (extra or []))
        return perf, log

    def test_markdown_report_has_well_formed_tables(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            directory = Path(scratch)
            perf, log = self._write_run(directory, "run", [10, 12, 15, 17, 18, 20],
                                        extra=[perf_line(1000.0, 500.0), attrib_line(100.0, 10.0)])
            completed = run_cli("--perf", str(perf), "--stderr", str(log))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("## Definitions", completed.stdout)
        self.assertIn("## Vblanks per new frame", completed.stdout)
        # A table is a run of consecutive lines that start with "|": header, separator, rows.
        blocks: list[list[str]] = []
        current: list[str] = []
        for line in completed.stdout.splitlines():
            if line.startswith("|"):
                current.append(line)
            elif current:
                blocks.append(current)
                current = []
        if current:
            blocks.append(current)
        self.assertGreaterEqual(len(blocks), 4)
        for block in blocks:
            with self.subTest(header=block[0]):
                cells = block[0].count("|") - 1
                self.assertEqual(block[1], "|" + " --- |" * cells)
                self.assertGreater(len(block), 2)
                for row in block:
                    self.assertEqual(row.count("|") - 1, cells)
        self.assertNotIn("warning:", completed.stderr)

    def test_json_report_carries_the_schema_and_the_numbers(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            directory = Path(scratch)
            perf, log = self._write_run(directory, "run", [10, 12, 15])
            completed = run_cli("--perf", str(perf), "--stderr", str(log), "--format", "json")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        document = json.loads(completed.stdout)
        self.assertEqual(document["schema"], perf_attribution.SCHEMA)
        self.assertEqual(document["presents"]["new_frames"], 3)
        self.assertEqual(document["vblank_intervals"]["median"], 2.5)
        self.assertEqual(document["vblank_intervals"]["intervals"], 2)

    def test_diff_reports_both_runs_and_the_difference(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            directory = Path(scratch)
            perf_a, log_a = self._write_run(directory, "a", [0, 2, 4, 6])
            perf_b, log_b = self._write_run(directory, "b", [0, 3, 6, 9])
            json_run = run_cli("--diff", str(perf_a), str(log_a), str(perf_b), str(log_b), "--format", "json")
            markdown_run = run_cli("--diff", str(perf_a), str(log_a), str(perf_b), str(log_b))
        self.assertEqual(json_run.returncode, 0, json_run.stderr)
        document = json.loads(json_run.stdout)
        self.assertEqual(document["schema"], perf_attribution.DIFF_SCHEMA)
        self.assertEqual(document["a"]["vblank_intervals"]["median"], 2.0)
        self.assertEqual(document["b"]["vblank_intervals"]["median"], 3.0)
        self.assertEqual(document["delta"]["vblank_intervals.median"], 1.0)
        self.assertEqual(document["delta"]["presents.new_frames"], 0)
        self.assertEqual(markdown_run.returncode, 0, markdown_run.stderr)
        self.assertIn("| Metric | A | B | B - A |", markdown_run.stdout)
        self.assertIn("| Median (p50) | 2.00 | 3.00 | +1.00 |", markdown_run.stdout)

    def test_warnings_go_to_stderr_and_the_exit_status_stays_zero(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            directory = Path(scratch)
            perf, log = self._write_run(directory, "run", [1, 2, 3], extra=["HOST_PRESENT_SUBMITTED bad"])
            completed = run_cli("--perf", str(perf), "--stderr", str(log), "--format", "json")
        self.assertEqual(completed.returncode, 0)
        self.assertIn("warning: 1 malformed HOST_PRESENT_SUBMITTED", completed.stderr)
        self.assertEqual(json.loads(completed.stdout)["presents"]["malformed_lines"], 1)

    def test_bad_summary_exits_two_with_a_message_and_no_report(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            directory = Path(scratch)
            doc = summary_doc()
            doc["schema"] = "nakagawa-perf-v0"
            perf, log = self._write_run(directory, "run", [1, 2], doc=doc)
            completed = run_cli("--perf", str(perf), "--stderr", str(log))
        self.assertEqual(completed.returncode, 2)
        self.assertEqual(completed.stdout, "")
        self.assertTrue(completed.stderr.startswith("error: "), completed.stderr)

    def test_missing_input_exits_two(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            directory = Path(scratch)
            completed = run_cli("--perf", str(directory / "no.json"), "--stderr", str(directory / "no.log"))
        self.assertEqual(completed.returncode, 2)
        self.assertEqual(completed.stdout, "")
        self.assertTrue(completed.stderr.startswith("error: "), completed.stderr)

    def test_usage_errors_exit_two(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            path = str(Path(scratch) / "x")
            only_perf = run_cli("--perf", path)
            diff_and_perf = run_cli("--diff", path, path, path, path, "--perf", path)
        self.assertEqual(only_perf.returncode, 2)
        self.assertIn("--perf and --stderr are both required", only_perf.stderr)
        self.assertEqual(diff_and_perf.returncode, 2)
        self.assertIn("--diff takes four paths", diff_and_perf.stderr)


if __name__ == "__main__":
    unittest.main()
