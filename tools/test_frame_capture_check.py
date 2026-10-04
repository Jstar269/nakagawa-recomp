#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#
# Unit tests for tools/frame_capture_check.py: frame numbering, black/stale detection,
# capture-result accounting, present-gap classification. Synthetic frames only.

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
        """Identical consecutive frames must be reported as duplicate_consecutive.

        This asserts through run_check, which is where the stale-frame signal is
        produced. Asserting only that two files hash equal would leave
        duplicate_consecutive uncovered, so deleting that logic would keep the
        suite green.
        """
        import json

        with tempfile.TemporaryDirectory() as d:
            write_ppm(Path(d) / "frame_0000.ppm", 8, 8, 77)
            write_ppm(Path(d) / "frame_0002.ppm", 8, 8, 77)     # identical content
            out = Path(d) / "report.json"
            fcc.run_check(d, None, str(out))
            report = json.loads(out.read_text())
            pairs = [
                (p["first"], p["second"])
                for p in report["duplicate_consecutive"]
            ]
            self.assertIn(("frame_0000.ppm", "frame_0002.ppm"), pairs)

    def test_distinct_frames_are_not_duplicates(self):
        """The mirror image: genuinely different frames must NOT be flagged."""
        import json

        with tempfile.TemporaryDirectory() as d:
            write_ppm(Path(d) / "frame_0000.ppm", 8, 8, 77)
            write_ppm(Path(d) / "frame_0002.ppm", 8, 8, 200)   # different content
            out = Path(d) / "report.json"
            fcc.run_check(d, None, str(out))
            report = json.loads(out.read_text())
            self.assertEqual(report["duplicate_consecutive"], [])

    def test_duplicates_are_not_compared_across_capture_sequences(self):
        """frame_<n> and frame_v<n> are different sequences; never compare across.

        analyze_frames() sorts by (style, frame), so identical content in a
        rotating frame and a window frame would otherwise be reported as a
        duplicate pair even though they were never adjacent in one run.
        """
        import json

        with tempfile.TemporaryDirectory() as d:
            write_ppm(Path(d) / "frame_0000.ppm", 8, 8, 55)     # rotating
            write_ppm(Path(d) / "frame_v8300.ppm", 8, 8, 55)    # window, same bytes
            out = Path(d) / "report.json"
            fcc.run_check(d, None, str(out))
            report = json.loads(out.read_text())
            self.assertEqual(report["duplicate_consecutive"], [])


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

    def test_missing_log_is_an_error_not_an_empty_report(self):
        """A --log path that does not exist must fail loudly.

        Silently treating it as an empty log reports "no capture lines seen" for
        a mistyped path, which is indistinguishable from a clean run.
        """
        with tempfile.TemporaryDirectory() as d:
            missing = Path(d) / "typo.log"
            with self.assertRaises(FileNotFoundError):
                fcc.classify_log(str(missing))

    def test_absent_log_argument_is_not_an_error(self):
        """No --log at all is legitimate: the tool then only sees the frames."""
        report = fcc.classify_log(None)
        self.assertEqual(report["capture_results"], [])
        self.assertEqual(report["watchdogs"], [])

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


class EmittedFormatContractTest(unittest.TestCase):
    """The parsers must match the strings the runtime ACTUALLY prints.

    This tool was recovered from an unlanded branch and reinstated after its
    watchdog pattern silently stopped matching: the runtime says "no new frame
    presented", the old pattern said "no frame presented", and nothing failed --
    the watchdog class was simply never populated again.

    The samples below are verbatim from the current src/rt/hle.c format strings,
    so a wording change upstream now fails here instead of degrading silently.
    """

    # Verbatim from src/rt/hle.c ("FBSNAP f=%u swapchain capture -> ...").
    LIVE_RESULT_LINES = (
        "FBSNAP f=120 swapchain capture -> build/snapshots/frame_0120.ppm (result=1)\n",
        "FBSNAP f=121 swapchain capture -> build/snapshots/frame_0121.ppm (result=-1)\n",
    )
    # Verbatim from src/rt/hle.c:14285.
    LIVE_SKIPPED_LINE = (
        "FBSNAP f=122 swapchain capture -> SKIPPED (no present serviced this frame)\n"
    )
    # Verbatim from src/rt/hle.c:14587.
    LIVE_WATCHDOG_LINE = (
        "WATCHDOG: no new frame presented for 900 vblanks (~15000us) - neutral "
        "NO-NEW-FLIP observation, not by itself a hang/stall verdict\n"
    )

    def test_result_lines_match_live_format(self):
        for line in self.LIVE_RESULT_LINES:
            m = fcc.RE_RESULT.search(line)
            self.assertIsNotNone(m, f"RE_RESULT stopped matching: {line!r}")
            self.assertIsInstance(int(m.group(1)), int)
            self.assertIsInstance(int(m.group(3)), int)

    def test_skipped_line_matches_live_format(self):
        self.assertIsNotNone(
            fcc.RE_SKIPPED.search(self.LIVE_SKIPPED_LINE),
            "RE_SKIPPED stopped matching the emitted SKIPPED line",
        )

    def test_legacy_skipped_line_is_counted_as_a_skip(self):
        """src/rt/hle.c:14274 emits "FBSNAP f=%u -> SKIPPED (synchronisation failed)".

        It is a dropped present, not a legacy capture of a file named "SKIPPED".
        """
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "stderr.log"
            log.write_text(
                "FBSNAP f=300 -> SKIPPED (synchronisation failed)\n",
                encoding="utf-8",
            )
            r = fcc.classify_log(str(log))
            self.assertEqual(r["skipped"], [300])
            self.assertEqual(r["legacy"], [])

    def test_live_emitter_strings_in_hle_c(self):
        """The hand-copied samples must still agree with the actual emitter.

        A commit that rewords hle.c and updates these constants together would
        otherwise leave the patterns stale with every test green -- the exact
        regression this class exists to prevent. Read the source and rebuild each
        sample from the format string the runtime actually uses.
        """
        src = (Path(__file__).resolve().parents[1] / "src" / "rt" / "hle.c").read_text(
            encoding="utf-8", errors="replace"
        )
        checks = [
            (
                "FBSNAP f=%u swapchain capture -> %s (result=%d)",
                self.LIVE_RESULT_LINES[0],
                fcc.RE_RESULT,
            ),
            (
                "FBSNAP f=%u swapchain capture -> SKIPPED (no present serviced this frame)",
                self.LIVE_SKIPPED_LINE,
                fcc.RE_SKIPPED,
            ),
            (
                "FBSNAP f=%u -> SKIPPED (synchronisation failed)",
                None,
                fcc.RE_LEGACY_SKIPPED,
            ),
        ]
        for fmt, sample, pattern in checks:
            self.assertIn(
                fmt, src,
                f"hle.c no longer contains the format string {fmt!r}; update the "
                f"sample and the pattern together",
            )
            if sample is not None:
                self.assertIsNotNone(
                    pattern.search(sample),
                    f"{pattern.pattern!r} does not match a line rendered from "
                    f"the live format string {fmt!r}",
                )
            else:
                # Render the format string the way the runtime does and require
                # the pattern to classify it as a dropped present.
                self.assertIsNotNone(
                    pattern.search(fmt.replace("%u", "300", 1)),
                    f"{pattern.pattern!r} does not match the line rendered from "
                    f"the live format string {fmt!r}",
                )

    def test_watchdog_line_matches_live_wording(self):
        """Regression: the runtime's wording is 'no NEW frame presented'."""
        m = fcc.RE_WATCHDOG.search(self.LIVE_WATCHDOG_LINE)
        self.assertIsNotNone(
            m,
            "RE_WATCHDOG no longer matches the emitted watchdog line; the "
            "'guest-stalled-no-flip' class would silently never be reported",
        )
        self.assertEqual(m.group(1), "900")

    def test_watchdog_still_accepts_legacy_wording(self):
        """Archived logs use the older 'no frame presented' phrasing."""
        m = fcc.RE_WATCHDOG.search("WATCHDOG: no frame presented for 42 vblanks\n")
        self.assertIsNotNone(m, "legacy watchdog wording must still parse")
        self.assertEqual(m.group(1), "42")

    def test_watchdog_survives_a_full_log_pass(self):
        """End-to-end: a live-format watchdog line must reach the report."""
        with tempfile.TemporaryDirectory() as d:
            write_ppm(Path(d) / "frame_0001.ppm", 8, 8, 90)
            log = Path(d) / "stderr.log"
            log.write_text(
                "FBSNAP f=1 swapchain capture -> build/snapshots/frame_0001.ppm (result=1)\n"
                + self.LIVE_WATCHDOG_LINE,
                encoding="utf-8",
            )
            out = Path(d) / "capture_check.json"
            res = fcc.run_check(d, str(log), str(out))
            self.assertEqual(
                res["watchdog_events"], 1,
                "run_check must count the live-format watchdog line",
            )
            self.assertEqual(
                res["present_gap_classification"], "guest-stalled-no-flip",
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
