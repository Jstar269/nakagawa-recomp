#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Contract tests for the whole-run VBLANK ledger audit.

Every fixture here is synthetic: a run's own ``perf.csv`` shape and the runtime's
own ledger lines, with no title and no private input.  The point of the tool is
that a catch-up burst cannot read as over-delivery, so the corpus contains exactly
that: runs whose best seconds look fast, whose whole-run ratio is anchored, and
whose ledger identity is broken.
"""

from __future__ import annotations

import csv
import io
import tempfile
import unittest
from pathlib import Path

try:
    import vblank_ledger
except ModuleNotFoundError:
    from tools import vblank_ledger

SOURCE_HZ = 60000.0 / 1001.0

PERF_HEADER = "vblank_total,wall_ms,fps,frame_ms,vblank_hz,cpu_ms,ge_wait_ms,present_ms,idle_ms\n"


def perf_rows(episodes: list[int], wall_ms: float = 1000.5, fps: float = 28.0,
              lead_rows: int = 0) -> str:
    """A perf.csv whose presenting seconds deliver ``episodes`` each."""
    out = io.StringIO()
    out.write(PERF_HEADER)
    total = 0
    if lead_rows:
        # The pre-presentation index scan: one enormous interval that presented
        # nothing and must not enter either side of the ratio.
        out.write(f"0,{lead_rows * 1000.0},0.000,0.000,0.000,0,0,0,0\n")
    for count in episodes:
        hz = count / (wall_ms / 1000.0)
        out.write(
            f"{total + count},{wall_ms},{fps:.3f},35.750,{hz:.3f},47.0,0.2,7.2,945.0\n"
        )
        total = total + count
    return out.getvalue()


def stderr_log(owed: int, coalesced: int, delivered: int, dropped: int, in_flight: int,
               masked: int, late_owed: int = 0, late_delivered: int = 0,
               reports: int = 1) -> str:
    lines = []
    for index in range(reports):
        share = (index + 1) / reports
        o = round(owed * share)
        c = round(coalesced * share)
        d = round(delivered * share)
        drop = round(dropped * share)
        flying = o + c - d - drop
        lo = round(late_owed * share)
        ld = round(late_delivered * share)
        lines.append(
            f"PERF_ATTRIB vblank_late total={lo} lost_ms=0 masked_periods={round(masked * share)} "
            f"collapsed_periods=0"
        )
        lines.append(
            f"PERF_ATTRIB vblank_owed owed={o} coalesced={c} delivered={d} dropped={drop} "
            f"in_flight={flying} late_owed={lo} late_delivered={ld}"
        )
    return "\n".join(lines) + "\n"


class VblankLedgerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def write(self, name: str, text: str) -> Path:
        path = self.dir / name
        path.write_text(text, encoding="utf-8")
        return path

    def run_tool(self, perf: str, log: str, *extra: str) -> tuple[int, str]:
        perf_path = self.write("perf.csv", perf)
        log_path = self.write("stderr_run.log", log)
        out = io.StringIO()
        import contextlib

        with contextlib.redirect_stdout(out):
            code = vblank_ledger.main(
                ["--perf", str(perf_path), "--stderr", str(log_path), *extra]
            )
        return code, out.getvalue()

    def test_anchored_run_passes_on_the_whole_run(self) -> None:
        # 300 anchored seconds: 60 episodes per 1.0005 s interval is 59.97 Hz
        # against a 59.94 Hz source.
        perf = perf_rows([60] * 300)
        log = stderr_log(owed=17983, coalesced=1, delivered=17984, dropped=0,
                         in_flight=0, masked=1, late_owed=100, late_delivered=100)
        code, out = self.run_tool(perf, log)
        self.assertEqual(code, 0, out)
        self.assertIn("verdict=PASS", out)
        self.assertIn("VBLANK_CHECK: rate PASS", out)
        self.assertIn("VBLANK_CHECK: identity PASS", out)

    def test_a_catch_up_burst_cannot_read_as_over_delivery(self) -> None:
        # 58 ordinary seconds, then two seconds that deliver the episodes a long
        # guest stretch held, and four that lag: the burst seconds read 62 Hz and
        # the whole run is still anchored.  This is the case a per-second mean (or a
        # sorted "worst 30 seconds" window) reports as over-delivery.
        episodes = [60] * 58 + [62, 62] + [57] * 4
        perf = perf_rows(episodes)
        log = stderr_log(owed=sum(episodes), coalesced=0, delivered=sum(episodes),
                         dropped=0, in_flight=0, masked=0)
        code, out = self.run_tool(perf, log)
        self.assertEqual(code, 0, out)
        self.assertIn("VBLANK_CHECK: rate PASS", out)
        # The burst is still visible, as a count and not as a rate.
        self.assertIn("62: 2", out)
        self.assertIn("57: 4", out)

    def test_over_delivery_fails_and_names_the_excess(self) -> None:
        # 2.5% fast: 3030 episodes over 300 anchored seconds.
        perf = perf_rows([61] * 300, wall_ms=1000.0)
        log = stderr_log(owed=17983, coalesced=0, delivered=17983 + 1499, dropped=0,
                         in_flight=0, masked=0)
        code, out = self.run_tool(perf, log)
        self.assertEqual(code, 1)
        self.assertIn("VBLANK_CHECK: rate FAIL", out)
        self.assertIn("verdict=FAIL", out)
        # The excess is reported in episodes and in Hz, not as a ratio alone.
        self.assertIn("excess_hz=+", out)

    def test_under_delivery_fails(self) -> None:
        perf = perf_rows([58] * 300, wall_ms=1000.0)
        log = stderr_log(owed=17395, coalesced=0, delivered=17395, dropped=0,
                         in_flight=0, masked=0)
        code, out = self.run_tool(perf, log)
        self.assertEqual(code, 1)
        self.assertIn("VBLANK_CHECK: rate FAIL", out)

    def test_a_broken_ledger_identity_is_a_bookkeeping_failure(self) -> None:
        perf = perf_rows([60] * 300)
        # The runtime's own residual is 0, but the line claims 7 are still owed: a
        # bookkeeping bug, reported as one rather than absorbed into a rate.
        log = (
            "PERF_ATTRIB vblank_late total=0 lost_ms=0 masked_periods=1 collapsed_periods=0\n"
            "PERF_ATTRIB vblank_owed owed=17983 coalesced=1 delivered=17984 dropped=0 "
            "in_flight=7 late_owed=0 late_delivered=0\n"
        )
        code, out = self.run_tool(perf, log)
        self.assertEqual(code, 1)
        self.assertIn("VBLANK_CHECK: identity FAIL", out)
        self.assertIn("residual=0", out)

    def test_a_masked_window_credited_beyond_its_periods_fails(self) -> None:
        perf = perf_rows([60] * 300)
        # 5 coalesced episodes for 1 masked period, with 0 still in flight: a masked
        # window delivered more than the window covered.
        log = stderr_log(owed=17983, coalesced=5, delivered=17988, dropped=0,
                         in_flight=0, masked=1)
        code, out = self.run_tool(perf, log)
        self.assertEqual(code, 1)
        self.assertIn("VBLANK_CHECK: masked FAIL", out)
        self.assertIn("credit=+4", out)

    def test_masked_credit_within_the_in_flight_residual_passes(self) -> None:
        perf = perf_rows([60] * 300)
        log = stderr_log(owed=17983, coalesced=5, delivered=17984, dropped=0,
                         in_flight=4, masked=1)
        code, out = self.run_tool(perf, log)
        self.assertEqual(code, 0, out)
        self.assertIn("VBLANK_CHECK: masked PASS", out)

    def test_absent_ledger_is_skipped_not_passed(self) -> None:
        perf = perf_rows([60] * 300)
        code, out = self.run_tool(perf, "PERF vblank_total=0 wall_ms=1.0\n")
        self.assertIn("VBLANK_CHECK: identity SKIP", out)
        self.assertIn("VBLANK_CHECK: masked SKIP", out)
        # The rate was still judged, and it passed; the skips are named, not silent.
        self.assertIn("VBLANK_CHECK: rate PASS", out)
        self.assertIn("verdict=PASS", out)
        self.assertEqual(code, 0)

    def test_no_perf_csv_is_not_a_verdict(self) -> None:
        out = io.StringIO()
        import contextlib

        with contextlib.redirect_stdout(out):
            code = vblank_ledger.main([])
        self.assertEqual(code, 1)
        self.assertIn("verdict=NOT_RUN", out.getvalue())

    def test_the_index_scan_is_excluded_from_both_sides(self) -> None:
        # 120 leading seconds of scan, then 300 presenting seconds.  Including the
        # scan's wall time would report a large deficit.
        perf = perf_rows([60] * 300, lead_rows=120)
        log = stderr_log(owed=17983, coalesced=0, delivered=17983, dropped=0,
                         in_flight=0, masked=0)
        code, out = self.run_tool(perf, log)
        self.assertEqual(code, 0, out)
        self.assertIn("presenting_s=300", out)
        self.assertIn("VBLANK_CHECK: rate PASS", out)

    def test_a_malformed_column_is_not_silently_zero(self) -> None:
        body = perf_rows([60] * 300)
        body = body.replace("28.000,35.750,59.970", "28.000,35.750,not-a-number", 1)
        path = self.write("perf.csv", body)
        rows = vblank_ledger.read_perf(path)
        self.assertEqual(vblank_ledger._number(rows[0], "vblank_hz"), None)
        cadence = vblank_ledger.measure_cadence(rows)
        assert cadence is not None
        # A second with no readable cadence reading is left out of the per-second
        # mean, not counted as 0 Hz.
        self.assertGreater(cadence.per_second_mean_hz, 0.0)

    def test_source_rate_is_the_rational_one(self) -> None:
        self.assertAlmostEqual(SOURCE_HZ, 59.94005994, places=6)


class LedgerReportShapeTest(unittest.TestCase):
    def test_every_check_line_names_a_status_and_a_number(self) -> None:
        report = vblank_ledger.Report()
        report.add("presenting", True, "presenting_s=3")
        report.add("identity", None, "no ledger")
        report.add("rate", False, "ratio=1.020")
        text = report.render().splitlines()
        self.assertEqual(len(text), 4)
        self.assertTrue(text[0].startswith("VBLANK_CHECK: presenting PASS presenting_s=3"))
        self.assertIn("SKIP", text[1])
        self.assertIn("FAIL", text[2])
        self.assertIn("verdict=FAIL", text[3])
        self.assertEqual(report.failures, 1)
        self.assertEqual(report.judged, 2)


class PerfCsvShapeTest(unittest.TestCase):
    def test_reader_uses_the_header_the_runtime_writes(self) -> None:
        text = perf_rows([60, 59, 61])
        rows = list(csv.DictReader(io.StringIO(text)))
        self.assertEqual([int(r["vblank_total"]) for r in rows], [60, 119, 180])
        self.assertIn("vblank_hz", rows[0])


if __name__ == "__main__":
    unittest.main()
