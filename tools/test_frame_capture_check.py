#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#
# Unit tests for tools/frame_capture_check.py: frame numbering, black/stale detection,
# capture-result accounting, present-gap classification. Synthetic frames only.

import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import frame_capture_check as fcc  # noqa: E402


def write_ppm(path, w, h, fill):
    """P6 PPM where every pixel's R,G,B are the byte `fill`."""
    body = bytes([fill]) * (w * h * 3)
    Path(path).write_bytes(b"P6\n%d %d\n255\n" % (w, h) + body)


class FrameNumberTest(unittest.TestCase):
    def test_rotating_names(self):
        self.assertEqual(fcc.frame_number("frame_0012.ppm"), ("n", 12))
        self.assertEqual(fcc.frame_number("frame_0000.ppm"), ("n", 0))
        self.assertEqual(fcc.frame_number("frame_12345.ppm"), ("n", 12345))

    def test_windows_names(self):
        self.assertEqual(fcc.frame_number("frame_v8300.ppm"), ("v", 8300))

    def test_non_capture_names_ignored(self):
        self.assertIsNone(fcc.frame_number("snap_1.ppm"))
        self.assertIsNone(fcc.frame_number("present_source.ppm"))
        self.assertIsNone(fcc.frame_number("stderr.log"))


class PpmParseTest(unittest.TestCase):
    def test_parse_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "frame_0001.ppm"
            write_ppm(p, 4, 3, 128)
            w, h, body = fcc.parse_ppm(p)
            self.assertEqual((w, h), (4, 3))
            self.assertEqual(len(body), 4 * 3 * 3)
            self.assertEqual(body, b"\x80" * (4 * 3 * 3))

    def test_malformed_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "frame_0001.ppm"
            p.write_bytes(b"not a ppm")
            self.assertIsNone(fcc.parse_ppm(p))


class BlackAndStaleTest(unittest.TestCase):
    def test_black_and_distinct_frames(self):
        with tempfile.TemporaryDirectory() as d:
            write_ppm(Path(d) / "frame_0000.ppm", 8, 8, 0)      # black
            write_ppm(Path(d) / "frame_0002.ppm", 8, 8, 200)    # bright
            frames = fcc.analyze_frames(d)
            self.assertEqual(len(frames), 2)
            self.assertTrue(frames[0]["black"])
            self.assertFalse(frames[1]["black"])

    def test_stale_duplicate_detected(self):
        with tempfile.TemporaryDirectory() as d:
            write_ppm(Path(d) / "frame_0000.ppm", 8, 8, 77)
            write_ppm(Path(d) / "frame_0002.ppm", 8, 8, 77)     # identical content
            frames = fcc.analyze_frames(d)
            hashes = [f["sha256"] for f in frames]
            self.assertEqual(hashes[0], hashes[1])


class LogClassificationTest(unittest.TestCase):
    LOG = """\
FBSNAP f=100 swapchain capture -> build/snapshots/frame_0100.ppm (result=1)
FBSNAP f=101 swapchain capture -> SKIPPED (no present serviced this frame)
FBSNAP f=102 swapchain capture -> build/snapshots/frame_0102.ppm (result=-1)
PRESENT_GAP: vcount=5000 last_host_present=4700 gap=300 (~5s) -- guest running, no host present serviced
WATCHDOG: no frame presented for 900 vblanks (~15s)
FBSNAP f=200 -> snap_1.ppm
"""

    def test_classify(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "stderr.log"
            log.write_text(self.LOG, encoding="utf-8")
            r = fcc.classify_log(str(log))
            self.assertEqual([c["result"] for c in r["capture_results"]], [1, -1])
            self.assertEqual(r["skipped"], [101])
            self.assertEqual(r["present_gaps"][0]["gap"], 300)
            self.assertEqual(r["watchdogs"], [{"vblanks": 900}])
            self.assertEqual(len(r["legacy"]), 1)

    def test_gap_classification(self):
        self.assertEqual(
            fcc.present_gap_classification(
                {"present_gaps": [{"gap": 300}], "watchdogs": []}
            ),
            "guest-running-no-present",
        )
        self.assertEqual(
            fcc.present_gap_classification(
                {"present_gaps": [], "watchdogs": [{"vblanks": 900}]}
            ),
            "guest-stalled-no-flip",
        )
        self.assertEqual(
            fcc.present_gap_classification({"present_gaps": [], "watchdogs": []}),
            "no-gap-recorded",
        )


class RunCheckTest(unittest.TestCase):
    def test_missing_file_for_reported_success_is_hard_error(self):
        with tempfile.TemporaryDirectory() as d:
            write_ppm(Path(d) / "frame_0001.ppm", 8, 8, 90)
            log = Path(d) / "stderr.log"
            log.write_text(
                "FBSNAP f=1 swapchain capture -> build/snapshots/frame_0001.ppm (result=1)\n"
                "FBSNAP f=2 swapchain capture -> build/snapshots/frame_0002.ppm (result=1)\n",
                encoding="utf-8",
            )
            out = Path(d) / "capture_check.json"
            res = fcc.run_check(d, str(log), str(out))
            self.assertEqual(res["verdict"], "hard-error")
            self.assertEqual(len(res["missing_for_success"]), 1)
            self.assertTrue(out.exists())
            # The manifest records a content hash per frame, never pixel bytes.
            self.assertIn("sha256", res["frames"][0])
            self.assertEqual(len(res["frames"][0]["sha256"]), 64)

    def test_capture_failure_is_warn_not_hard_error(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "stderr.log"
            log.write_text(
                "FBSNAP f=5 swapchain capture -> build/snapshots/frame_0005.ppm (result=-1)\n",
                encoding="utf-8",
            )
            res = fcc.run_check(d, str(log), None)
            self.assertEqual(res["verdict"], "warn")
            self.assertEqual(len(res["capture_failures"]), 1)

    def test_clean_sequence(self):
        with tempfile.TemporaryDirectory() as d:
            write_ppm(Path(d) / "frame_0000.ppm", 8, 8, 10)
            write_ppm(Path(d) / "frame_0002.ppm", 8, 8, 20)
            write_ppm(Path(d) / "frame_0004.ppm", 8, 8, 30)
            res = fcc.run_check(d, None, None)
            self.assertEqual(res["verdict"], "clean")
            self.assertEqual(res["total_frames"], 3)
            # A step of 2 is a normal 30 Hz present pattern: reported, not a warning.
            self.assertEqual(len(res["frame_number_gaps"]), 2)

    def test_windows_named_frames(self):
        with tempfile.TemporaryDirectory() as d:
            write_ppm(Path(d) / "frame_v8300.ppm", 8, 8, 10)
            write_ppm(Path(d) / "frame_v8302.ppm", 8, 8, 20)
            res = fcc.run_check(d, None, None)
            self.assertEqual(res["total_frames"], 2)
            self.assertEqual(res["frames"][0]["frame"], 8300)


if __name__ == "__main__":
    unittest.main()
