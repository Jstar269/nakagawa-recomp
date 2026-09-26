# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Contract tests for tools/soak_audit.py.

Every fixture is synthetic: the CSVs and log lines are written by this file, so nothing here
depends on a title, a private input or a captured run. What is pinned is the judging logic
and, more importantly, that a *missing* artifact is reported as SKIP with a reason and never
as a pass -- an audit that cannot fail is worse than no audit.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PERF_HEADER = "vblank_total,wall_ms,fps,frame_ms,vblank_hz\n"
PROC_HEADER = "t_s,working_set_kb,private_bytes_kb,handles\n"


def _perf(seconds: list[tuple[float, float]]) -> str:
    rows = [PERF_HEADER]
    for vblank_total, (fps, hz) in enumerate(seconds):
        rows.append(f"{vblank_total},{1000.0},{fps},{33.0},{hz}\n")
    return "".join(rows)


def _proc(samples: list[tuple[int, int, int, int]]) -> str:
    rows = [PROC_HEADER]
    for t, ws, priv, handles in samples:
        rows.append(f"{t},{ws},{priv},{handles}\n")
    return "".join(rows)


class SoakAuditTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self._tmp.cleanup = self._tmp.cleanup  # keep a reference

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write(self, name: str, body: str) -> str:
        path = self.dir / name
        path.write_text(body, encoding="utf-8")
        return str(path)

    def _run(self, *args: str) -> tuple[int, str]:
        proc = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "soak_audit.py"), *args],
            capture_output=True, text=True, timeout=120,
        )
        return proc.returncode, proc.stdout + proc.stderr

    # -- presenting ---------------------------------------------------------
    def test_healthy_run_passes_every_enabled_assertion(self) -> None:
        perf = self._write("perf.csv", _perf([(0.0, 0.0)] * 3 + [(28.0, 59.94)] * 40))
        proc = self._write("proc.csv", _proc([(i * 60, 400_000 + i * 100, 500_000 + i * 100, 900)
                                             for i in range(8)]))
        stderr = self._write("stderr.log", "PERF x\nROUTE_OK: 5 steps completed by vblank 5489\n")
        code, out = self._run("--perf", perf, "--proc", proc, "--stderr", stderr)
        self.assertEqual(code, 0, out)
        self.assertIn("SOAK_CHECK: presenting PASS", out)
        self.assertIn("SOAK_CHECK: cadence PASS", out)
        self.assertIn("SOAK_CHECK: memory_working_set PASS", out)
        self.assertIn("SOAK_CHECK: handles PASS", out)
        self.assertIn("SOAK_CHECK: route PASS steps=5 by_vblank=5489", out)
        self.assertIn("SOAK_AUDIT: verdict=PASS", out)

    def test_boot_seconds_before_the_first_frame_are_not_a_stall(self) -> None:
        perf = self._write("perf.csv", _perf([(0.0, 0.0)] * 120 + [(28.0, 59.9)] * 30))
        code, out = self._run("--perf", perf)
        self.assertEqual(code, 0, out)
        self.assertIn("ratio=1.000", out)

    def test_a_long_gap_between_presented_seconds_fails(self) -> None:
        seconds = [(28.0, 59.9)] * 20 + [(0.0, 59.9)] * 21 + [(28.0, 59.9)] * 20
        code, out = self._run("--perf", self._write("perf.csv", _perf(seconds)))
        self.assertEqual(code, 1)
        self.assertIn("longest_stall_s=21", out)
        self.assertIn("SOAK_CHECK: presenting FAIL", out)

    def test_a_run_that_never_presents_fails(self) -> None:
        code, out = self._run("--perf", self._write("perf.csv", _perf([(0.0, 0.0)] * 90)))
        self.assertEqual(code, 1)
        self.assertIn("no second presented a frame", out)

    # -- cadence ------------------------------------------------------------
    def test_sustained_low_cadence_fails(self) -> None:
        code, out = self._run("--perf", self._write("perf.csv", _perf([(28.0, 56.0)] * 40)))
        self.assertEqual(code, 1)
        self.assertIn("SOAK_CHECK: cadence FAIL", out)
        self.assertIn("p5_hz=56.00", out)

    def test_cadence_is_judged_on_the_tail_not_on_the_boot_average(self) -> None:
        # 30 slow seconds then 30 at the source rate: the soak window is the tail.
        seconds = [(28.0, 40.0)] * 30 + [(28.0, 59.94)] * 30
        code, out = self._run("--perf", self._write("perf.csv", _perf(seconds)))
        self.assertEqual(code, 0, out)
        self.assertIn("SOAK_CHECK: cadence PASS", out)

    def test_cadence_tail_is_chronological_not_the_best_seconds(self) -> None:
        # Catch-up bursts (61 Hz seconds) early in the run must not be selected as
        # "the tail": the last 30 seconds in time order run at the source rate.
        seconds = [(28.0, 61.0)] * 40 + [(28.0, 59.94)] * 30
        code, out = self._run("--perf", self._write("perf.csv", _perf(seconds)))
        self.assertEqual(code, 0, out)
        self.assertIn("mean_hz=59.94", out)
        self.assertIn("run_mean_hz=60.55", out)

    def test_cadence_skips_when_there_is_not_enough_of_a_run(self) -> None:
        code, out = self._run("--perf", self._write("perf.csv", _perf([(28.0, 59.9)] * 5)))
        self.assertEqual(code, 0, out)
        self.assertIn("SOAK_CHECK: cadence SKIP", out)

    # -- growth -------------------------------------------------------------
    def test_monotonic_memory_growth_fails(self) -> None:
        proc = self._write("proc.csv", _proc([(i * 60, 400_000 + i * 40_000, 400_000, 900)
                                              for i in range(8)]))
        code, out = self._run("--proc", proc)
        self.assertEqual(code, 1)
        self.assertIn("SOAK_CHECK: memory_working_set FAIL", out)
        self.assertIn("SOAK_CHECK: memory_private PASS", out)

    def test_handle_growth_fails(self) -> None:
        proc = self._write("proc.csv", _proc([(i * 60, 400_000, 400_000, 900 + i * 100)
                                              for i in range(8)]))
        code, out = self._run("--proc", proc)
        self.assertEqual(code, 1)
        self.assertIn("SOAK_CHECK: handles FAIL", out)

    def test_a_growth_peak_beyond_the_budget_fails_even_if_it_shrinks_again(self) -> None:
        proc = self._write("proc.csv", _proc([(0, 100_000, 100_000, 100), (60, 101_000, 100_000, 100),
                                              (120, 102_000, 100_000, 100), (180, 103_000, 100_000, 100),
                                              (240, 133_000, 100_000, 100), (300, 104_000, 100_000, 100),
                                              (360, 103_500, 100_000, 100)]))
        code, out = self._run("--proc", proc)
        self.assertEqual(code, 1)
        self.assertIn("SOAK_CHECK: memory_working_set FAIL", out)
        self.assertIn("peak_pct=29.13", out)

    def test_warm_up_samples_are_excluded_from_the_baseline(self) -> None:
        proc = self._write("proc.csv", _proc([(0, 100_000, 100_000, 100), (60, 200_000, 200_000, 100),
                                              (120, 205_000, 205_000, 101), (180, 206_000, 206_000, 101),
                                              (240, 206_500, 206_500, 101)]))
        code, out = self._run("--proc", proc, "--warmup-s", "1")
        self.assertEqual(code, 0, out)
        self.assertIn("working_set_kb_start=200000", out)

    # -- route / fatal / audio ---------------------------------------------
    def test_a_route_that_never_completes_fails(self) -> None:
        stderr = self._write("stderr.log", "ROUTE: reached TITLE at vblank 1104\n")
        code, out = self._run("--stderr", stderr)
        self.assertEqual(code, 1)
        self.assertIn("never reported ROUTE_OK", out)

    def test_no_route_in_the_run_is_a_skip_not_a_pass(self) -> None:
        stderr = self._write("stderr.log", "PERF x\n")
        code, out = self._run("--stderr", stderr)
        self.assertEqual(code, 0, out)
        self.assertIn("SOAK_CHECK: route SKIP no route program in this run", out)

    def test_fatal_markers_fail_the_run(self) -> None:
        stderr = self._write("stderr.log", "PERF x\nROUTE_FAIL: line 3: WAIT MAIN_MENU timed out\n")
        code, out = self._run("--stderr", stderr)
        self.assertEqual(code, 1)
        self.assertIn("SOAK_CHECK: fatal FAIL", out)
        self.assertIn("SOAK_CHECK: route FAIL", out)

    def test_no_audio_backend_totals_are_judged(self) -> None:
        good = self._write("good.log", "AUDIOSTAT_HOST: state=active driver=wasapi pushed=1000 "
                                      "underruns=0 overruns=0 backpressure=3 peak_q=1024\n")
        code, out = self._run("--stderr", good, "--audio")
        self.assertEqual(code, 0, out)
        self.assertIn("SOAK_CHECK: audio PASS", out)
        self.assertIn("peak_q=1024", out)

        bad = self._write("bad.log", "AUDIOSTAT_HOST: state=active driver=wasapi pushed=1000 "
                                     "underruns=7 overruns=0 backpressure=3 peak_q=1024\n")
        code, out = self._run("--stderr", bad, "--audio")
        self.assertEqual(code, 1)
        self.assertIn("underruns=7", out)

        none = self._write("none.log", "PERF x\n")
        code, out = self._run("--stderr", none, "--audio")
        self.assertEqual(code, 0, out)
        self.assertIn("SOAK_CHECK: audio SKIP", out)

    def test_host_audio_totals_are_judged_without_a_host_line(self) -> None:
        """The SDL backend prints a per-window duty line and end-of-run push/callback totals
        and NO underrun counter. Dropped frames and a failed callback put are the hard signals
        there, and the missing queue depth is reported as absent rather than invented."""
        good = self._write(
            "sdl.log",
            "AUDIOSTAT_WIN: vbl=327 frames=220500 nonzero=0 duty=0% pushed_total=195072\n"
            "AUDIOSTAT_WIN: vbl=627 frames=220500 nonzero=33043 duty=14% pushed_total=409344\n"
            "AUDIOSTAT_WIN: vbl=927 frames=220500 nonzero=89565 duty=40% pushed_total=622848\n"
            "AUDIOSTAT_PUSH: ch=8 calls=12814 nonzero_calls=12007 frames=9841152 peak=32768 "
            "clamps=0 dropped=0 snaps=262\n"
            "AUDIOSTAT_CB: calls=23312 frames=10280592 nonzero_frames=9213390 "
            "silent_frames=1067202 peak=32768 put_fail=0 play=10280592\n")
        code, out = self._run("--stderr", good, "--audio")
        self.assertEqual(code, 0, out)
        self.assertIn("SOAK_CHECK: audio PASS", out)
        self.assertIn("underruns=n/a", out)
        self.assertIn("underrun_counters=n/a", out)
        self.assertIn("dropped_frames=0", out)
        self.assertIn("windows=3", out)
        self.assertIn("silent_windows_after_first=0", out)

        dropped = self._write(
            "drop.log",
            "AUDIOSTAT_PUSH: ch=8 calls=12814 nonzero_calls=12007 frames=9841152 peak=32768 "
            "clamps=0 dropped=44 snaps=262\n"
            "AUDIOSTAT_CB: calls=23312 frames=10280592 nonzero_frames=9213390 "
            "silent_frames=1067202 peak=32768 put_fail=3 play=10280592\n")
        code, out = self._run("--stderr", dropped, "--audio")
        self.assertEqual(code, 1)
        self.assertIn("dropped_frames=44", out)
        self.assertIn("cb_put_fail=3", out)

    def test_silent_audio_windows_are_reported_as_a_number(self) -> None:
        log = self._write(
            "sil.log",
            "AUDIOSTAT_WIN: vbl=327 frames=220500 nonzero=0 duty=0% pushed_total=195072\n"
            "AUDIOSTAT_WIN: vbl=927 frames=220500 nonzero=0 duty=0% pushed_total=622848\n"
            "AUDIOSTAT_PUSH: ch=8 calls=1 nonzero_calls=0 frames=10 peak=1 clamps=0 dropped=0 "
            "snaps=0\n")
        code, out = self._run("--stderr", log, "--audio")
        self.assertEqual(code, 0, out)
        self.assertIn("silent_windows_after_first=1", out)

    def test_audio_is_not_judled_unless_asked_for(self) -> None:
        stderr = self._write("s.log", "AUDIOSTAT_HOST: state=active driver=wasapi pushed=1 "
                                      "underruns=9 overruns=0 backpressure=0 peak_q=1\n")
        code, out = self._run("--stderr", stderr)
        self.assertEqual(code, 0, out)
        self.assertIn("SOAK_CHECK: audio SKIP not judged", out)
        self.assertIn("SOAK_CHECK: audio_drift SKIP not judged", out)

    # -- audio drift --------------------------------------------------------
    @staticmethod
    def _win(vbl: int, lead_ms: int, queued: int = 4410) -> str:
        return (f"AUDIOSTAT_WIN: vbl={vbl} frames=220500 nonzero=33043 duty=14% "
                f"pushed_total=409344 queued={queued} lead_ms={lead_ms}\n")

    def test_a_bounded_queue_passes_the_drift_check(self) -> None:
        """A queue that rises and falls inside the bound is a working pacing loop."""
        log = self._write("drift_ok.log", "".join([
            self._win(327, 40), self._win(627, 120), self._win(927, 60), self._win(1227, 90)]))
        code, out = self._run("--stderr", log, "--audio")
        self.assertEqual(code, 0, out)
        self.assertIn("SOAK_CHECK: audio_drift PASS", out)
        self.assertIn("lead_peak_ms=120", out)
        self.assertIn("lead_net_ms=50", out)
        self.assertIn("queue_peak_frames=4410", out)

    def test_a_queue_that_only_grows_fails_as_drift(self) -> None:
        log = self._write("drift_grow.log", "".join([
            self._win(327, 10), self._win(627, 60), self._win(927, 180), self._win(1227, 400)]))
        code, out = self._run("--stderr", log, "--audio")
        self.assertEqual(code, 1, out)
        self.assertIn("SOAK_CHECK: audio_drift FAIL", out)
        self.assertIn("rose at every one of 4 windows", out)

    def test_a_queue_past_the_bound_fails_even_without_growth(self) -> None:
        log = self._write("drift_peak.log", "".join([
            self._win(327, 400), self._win(627, 420), self._win(927, 380)]))
        code, out = self._run("--stderr", log, "--audio", "--max-drift-ms", "400")
        self.assertEqual(code, 1, out)
        self.assertIn("peak_lead=420ms > 400ms", out)

    def test_net_growth_is_judged_against_its_own_flag(self) -> None:
        log = self._write("drift_net.log", "".join([
            self._win(327, 20), self._win(627, 40), self._win(927, 35), self._win(1227, 80)]))
        self.assertEqual(self._run("--stderr", log, "--audio")[0], 0)
        code, out = self._run("--stderr", log, "--audio", "--max-drift-net-ms", "10")
        self.assertEqual(code, 1, out)
        self.assertIn("net_growth=60ms > 10ms", out)

    def test_a_missing_queue_reading_skips_the_drift_check(self) -> None:
        """No queue depth is absence of a measurement, never a passing zero."""
        log = self._write("drift_absent.log",
                          "AUDIOSTAT_HOST: state=unavailable driver=none pushed=0 underruns=0 "
                          "overruns=0 backpressure=0 peak_q=0 queued=-1 lead_ms=-1\n")
        code, out = self._run("--stderr", log, "--audio")
        self.assertEqual(code, 0, out)
        self.assertIn("SOAK_CHECK: audio_drift SKIP", out)

        old = self._write("drift_old.log",
                          "AUDIOSTAT_HOST: state=active driver=wasapi pushed=1000 underruns=0 "
                          "overruns=0 backpressure=0 peak_q=1024\n")
        code, out = self._run("--stderr", old, "--audio")
        self.assertEqual(code, 0, out)
        self.assertIn("SOAK_CHECK: audio_drift SKIP", out)

    def test_the_per_channel_backends_single_figure_is_judged(self) -> None:
        log = self._write("host_drift.log",
                          "AUDIOSTAT_HOST: state=active driver=wasapi pushed=1000 underruns=0 "
                          "overruns=0 backpressure=3 peak_q=1024 queued=4410 lead_ms=100\n")
        code, out = self._run("--stderr", log, "--audio")
        self.assertEqual(code, 0, out)
        self.assertIn("SOAK_CHECK: audio_drift PASS", out)
        self.assertIn("samples=1", out)
        self.assertIn("lead_peak_ms=100", out)

    def test_each_public_emitter_prints_the_fields_this_parser_reads(self) -> None:
        """The emitters' own format strings, not a copy of them.

        A drift number nobody can parse is the same as no drift number, so the format each
        public source prints is read out of that source and fed to this module's patterns.
        Renaming a field in C without teaching the audit fails here rather than in a soak.
        """
        sys.path.insert(0, str(ROOT / "tools"))
        import soak_audit

        # A run of the other backend's shape (AUDIOSTAT_WIN, one reading per ~5 s) is read
        # the same way; that source is a private backend overlay and is not this tree's to pin.
        self.assertEqual(soak_audit.lead_series(self._win(327, 100)), [100])
        self.assertEqual(soak_audit.lead_series(self._win(627, 40)), [40])

        per_ch = (ROOT / "src" / "rt" / "audio_unavailable.c").read_text(encoding="utf-8")
        host_fmt = self._format_for(per_ch, "AUDIOSTAT_HOST: state=%s driver=%s pushed=%llu")
        self.assertIn("queued=%d", host_fmt)
        self.assertIn("lead_ms=%ld", host_fmt)
        host_line = self._as_python_format(host_fmt) % ("active", "wasapi", 1000, 0, 0, 3,
                                                        1024, 4410, 100)
        self.assertRegex(host_line, soak_audit.AUDIOSTAT_HOST)
        self.assertEqual(soak_audit.lead_series(host_line), [100])

    def test_the_runtime_s_own_drift_line_is_read_too(self) -> None:
        """AUDIOSTAT_LEAD is the one emitter every build links, mixer or not."""
        sys.path.insert(0, str(ROOT / "tools"))
        import soak_audit

        hle = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        fmt = self._format_for(hle, "AUDIOSTAT_LEAD: vbl=%u ch=%u outputs=%lu")
        self.assertIn("queued=%d", fmt)
        self.assertIn("lead_ms=%ld", fmt)
        line = self._as_python_format(fmt) % (300, 3, 12, 8820, 200)
        self.assertEqual(soak_audit.lead_series(line), [200])
        unmeasured = self._as_python_format(fmt) % (600, 0, 9, -1, -1)
        self.assertEqual(soak_audit.lead_series(unmeasured), [])
        mixed = line + self._as_python_format(fmt) % (900, 0, 9, -1, -1) + unmeasured
        self.assertEqual(soak_audit.lead_series(mixed), [200],
                         "an unmeasured window is dropped, the measured ones keep their order")

    @staticmethod
    def _as_python_format(fmt: str) -> str:
        """A C printf format in the subset Python's % operator understands.

        Only the conversions these two lines use appear: %s, %u, %d, %ld, %llu and %%. A
        literal %% is parked first, so narrowing %lu to %d cannot turn it into a conversion;
        it is restored as %%, which is how both formats spell a literal percent sign.
        """
        parked = fmt.replace("%%", "\\x00")
        for c in ("%llu", "%lu", "%ld"):
            parked = parked.replace(c, "%d")
        return parked.replace("\\x00", "%%")

    @staticmethod
    def _format_for(source: str, marker: str) -> str:
        """The printf format a C source uses for the telemetry line containing ``marker``.

        A C format is written as adjacent string literals, so the pieces are joined from the
        literal that opens before the marker through every literal that follows it in the same
        call. The marker may name only part of the first literal.
        """
        at = source.index(marker)
        literal = source.rindex('"', 0, at)
        parts: list[str] = []
        while True:
            end = source.index('"', literal + 1)
            parts.append(source[literal + 1:end])
            rest = source[end + 1:]
            stripped = rest.lstrip()
            if stripped.startswith('"'):
                literal = end + 1 + (len(rest) - len(stripped))
                continue
            break
        return "".join(parts)

    # -- inputs -------------------------------------------------------------
    def test_missing_inputs_skip_with_a_reason_and_never_pass_silently(self) -> None:
        code, out = self._run()
        self.assertEqual(code, 0, out)
        self.assertNotIn("verdict=FAIL", out)
        for name in ("presenting", "cadence", "memory_working_set", "handles", "fatal", "route"):
            self.assertIn(f"SOAK_CHECK: {name} SKIP", out)
        self.assertIn("SOAK_AUDIT: verdict=PASS", out)

    def test_json_output_carries_the_same_verdict(self) -> None:
        perf = self._write("perf.csv", _perf([(28.0, 40.0)] * 40))
        code, out = self._run("--perf", perf, "--json")
        self.assertEqual(code, 1)
        self.assertIn('"verdict": "FAIL"', out)

    def test_a_row_with_a_non_numeric_value_does_not_crash_the_audit(self) -> None:
        perf = self._write("perf.csv", PERF_HEADER + "0,1000.0,28.0,33.0,n/a\n1,1000.0,28.0,33.0,59.9\n")
        code, out = self._run("--perf", perf)
        self.assertEqual(code, 0, out)


if __name__ == "__main__":
    unittest.main()
