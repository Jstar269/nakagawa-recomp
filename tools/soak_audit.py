# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Judge one soak run from the telemetry the runtime already writes.

A long run is only evidence if something states what "healthy" meant. This tool takes the
three artifacts a run already produces -- ``logs/perf.csv`` (one row per wall second),
``logs/stderr_run.log`` (PERF, route narration, audio totals, fatal markers) and, when the
caller samples it, a process-metrics CSV -- and answers one question per assertion with a
number, so a reader never has to trust a summary sentence.

    presenting   fraction of seconds that presented a frame, and the longest stall
    cadence      vblank Hz over the presenting window (mean, worst second, p5)
    memory       working set / private bytes growth after warm-up
    handles      handle-count growth after warm-up
    audio        underruns / overruns / pushed frames from the runtime's own counters
    route        ROUTE_OK present when a route program was loaded
    fatal        FATAL / ROUTE_FAIL / watchdog / access-violation markers

Every threshold is a flag; the defaults are the ones the flagship campaign uses. Nothing here
knows what game produced the telemetry, and no assertion is derived from a title: the same
command judges a showcase, a title route or a bare idle soak.

Machine-readable output, one line per assertion, then one summary::

    SOAK_CHECK: cadence PASS mean_hz=59.94 worst_s=58.90 p5_hz=59.10
    SOAK_AUDIT: verdict=PASS checks=7 failures=0 presenting_s=458/460

Exit status is 0 only when every enabled assertion passed. A missing input is reported as
``SKIP`` with the reason, never as a pass: an absent artifact is not a healthy run.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

FATAL_PATTERNS = (
    "FATAL",
    "ROUTE_FAIL",
    "UNRESOLVED_DISPATCH",
    "watchdog fired",
    "access violation",
    "stack overflow",
    "assertion failed",
)

AUDIOSTAT_HOST = re.compile(
    r"AUDIOSTAT_HOST:\s+state=(\S+)\s+driver=(\S+)\s+pushed=(\d+)\s+underruns=(\d+)\s+"
    r"overruns=(\d+)\s+backpressure=(\d+)\s+peak_q=(\d+)"
)
AUDIOSTAT_PUSH = re.compile(
    r"AUDIOSTAT_PUSH:\s+ch=(\d+)\s+calls=(\d+)\s+nonzero_calls=(\d+)\s+frames=(\d+)\s+"
    r"peak=(\d+)\s+clamps=(\d+)\s+dropped=(\d+)\s+snaps=(\d+)"
)
AUDIOSTAT_CB = re.compile(
    r"AUDIOSTAT_CB:\s+calls=(\d+)\s+frames=(\d+)\s+nonzero_frames=(\d+)\s+"
    r"silent_frames=(\d+)\s+peak=(\d+)\s+put_fail=(\d+)\s+play=(\d+)"
)
AUDIOSTAT_WIN = re.compile(
    r"AUDIOSTAT_WIN:\s+vbl=(\d+)\s+frames=(\d+)\s+nonzero=(\d+)\s+duty=(\d+)%\s+pushed_total=(\d+)"
)
ROUTE_OK = re.compile(r"ROUTE_OK: (\d+) steps completed by vblank (\d+)")


@dataclass
class Check:
    name: str
    status: str          # PASS | FAIL | SKIP
    detail: str = ""

    def line(self) -> str:
        return f"SOAK_CHECK: {self.name} {self.status}" + (f" {self.detail}" if self.detail else "")


@dataclass
class Report:
    checks: list[Check] = field(default_factory=list)

    def add(self, name: str, ok: bool | None, detail: str = "") -> None:
        if ok is None:
            self.add_check(Check(name, "SKIP", detail or "not measured"))
        else:
            self.add_check(Check(name, "PASS" if ok else "FAIL", detail))

    def add_check(self, check: Check) -> None:
        self.checks.append(check)

    @property
    def failures(self) -> int:
        return sum(1 for c in self.checks if c.status == "FAIL")

    def verdict(self) -> str:
        return "FAIL" if self.failures else "PASS"

    def render(self) -> str:
        lines = [c.line() for c in self.checks]
        lines.append(
            f"SOAK_AUDIT: verdict={self.verdict()} checks={len(self.checks)} "
            f"failures={self.failures}"
        )
        return "\n".join(lines)

    def as_json(self) -> str:
        return json.dumps(
            {
                "verdict": self.verdict(),
                "checks": [
                    {"name": c.name, "status": c.status, "detail": c.detail} for c in self.checks
                ],
            },
            sort_keys=True,
        )


def _number(row: dict[str, str], key: str, default: float | None = None) -> float | None:
    raw = row.get(key)
    if raw is None or raw == "":
        return default
    try:
        return float(raw)
    except ValueError:
        return default


def read_perf(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8", errors="replace") as fh:
        return [row for row in csv.DictReader(fh) if row]


def presenting_window(rows: list[dict[str, str]]) -> tuple[int, list[dict[str, str]]]:
    """Index of the first presenting second and the rows from there on.

    The runtime's own index scan runs before the guest owns a frame, so those seconds carry
    fps 0 by design; counting them as a stall would make every run fail. The first second
    that presented a frame is the boundary, exactly as the flagship baseline check does.
    """
    first = -1
    for i, row in enumerate(rows):
        fps = _number(row, "fps", 0.0) or 0.0
        if fps > 0:
            first = i
            break
    if first < 0:
        return -1, []
    return first, rows[first:]


def check_presenting(report: Report, rows: list[dict[str, str]], min_ratio: float,
                     max_stall_s: float) -> None:
    first, window = presenting_window(rows)
    if first < 0:
        report.add("presenting", False, f"no second presented a frame in {len(rows)} seconds")
        return
    presenting = sum(1 for r in window if (_number(r, "fps", 0.0) or 0.0) > 0)
    ratio = presenting / max(1, len(window))
    longest, run = 0, 0
    for r in window:
        if (_number(r, "fps", 0.0) or 0.0) > 0:
            run = 0
        else:
            run += 1
            longest = max(longest, run)
    ok = ratio >= min_ratio and longest <= max_stall_s
    report.add(
        "presenting",
        ok,
        f"presenting_s={presenting}/{len(window)} ratio={ratio:.3f} min={min_ratio:.2f} "
        f"longest_stall_s={longest} max_stall_s={max_stall_s:.0f} first_row={first}",
    )


def check_cadence(report: Report, rows: list[dict[str, str]], min_hz: float,
                  tail_s: int) -> None:
    _, window = presenting_window(rows)
    hz = sorted((_number(r, "vblank_hz", 0.0) or 0.0) for r in window
                if (_number(r, "vblank_hz", 0.0) or 0.0) > 0)
    if len(hz) < max(1, tail_s):
        report.add("cadence", None, f"only {len(hz)} presenting seconds with a cadence sample")
        return
    tail = hz[-tail_s:] if tail_s else hz
    mean = sum(tail) / len(tail)
    worst = tail[0]
    p5 = tail[max(0, int(len(tail) * 0.05))]
    ok = p5 >= min_hz
    report.add(
        "cadence",
        ok,
        f"mean_hz={mean:.2f} worst_s={worst:.2f} p5_hz={p5:.2f} min={min_hz:.2f} "
        f"window_s={len(tail)}",
    )


def _read_proc(path: Path) -> list[dict[str, str]]:
    return read_perf(path)


def check_growth(report: Report, rows: list[dict[str, str]], key: str, name: str,
                 max_growth_pct: float, warmup_s: int) -> None:
    series = [(i, _number(r, key)) for i, r in enumerate(rows)]
    series = [(i, v) for i, v in series if v is not None and v > 0]
    if len(series) < 2:
        report.add(name, None, f"no {key} samples")
        return
    if len(series) <= warmup_s:
        report.add(name, None, f"only {len(series)} {key} samples, warm-up is {warmup_s}")
        return
    start = series[warmup_s][1]
    end = series[-1][1]
    peak = max(v for _, v in series[warmup_s:])
    growth = (end - start) / start * 100.0
    peak_growth = (peak - start) / start * 100.0
    ok = growth <= max_growth_pct and peak_growth <= max_growth_pct
    report.add(
        name,
        ok,
        f"{key}_start={start:.0f} {key}_end={end:.0f} growth_pct={growth:.2f} "
        f"peak_pct={peak_growth:.2f} max={max_growth_pct:.1f} warmup_s={warmup_s}",
    )


ROUTE_NARRATION = re.compile(r"^ROUTE", re.MULTILINE)


def check_route(report: Report, text: str) -> None:
    ok_line = ROUTE_OK.search(text)
    if ok_line:
        report.add("route", True,
                   f"steps={ok_line.group(1)} by_vblank={ok_line.group(2)}")
        return
    if ROUTE_NARRATION.search(text):
        report.add("route", False, "a route program ran but never reported ROUTE_OK")
        return
    report.add("route", None, "no route program in this run")


def check_audio(report: Report, text: str, max_underruns: int) -> None:
    """Judge the audio chain from the run's own counters.

    Two backends print here and they do not print the same thing, so the audit reads both
    rather than assuming one: the host audio backend emits a per-window duty line and
    end-of-run push/callback totals, while the no-host-audio backend emits a single host
    line carrying the underrun counters. Queue depth and per-push drift are NOT in either
    set, so they are reported as absent rather than guessed at (a drift number invented from
    push counts would be a measurement nobody took).
    """
    host = AUDIOSTAT_HOST.findall(text)
    pushes = AUDIOSTAT_PUSH.findall(text)
    cb = AUDIOSTAT_CB.findall(text)
    windows = AUDIOSTAT_WIN.findall(text)
    if not (host or pushes or cb or windows):
        report.add("audio", None, "no audio counters in this run (run with SR_AUDIOSTAT=1)")
        return
    dropped = sum(int(p[6]) for p in pushes)
    clamps = sum(int(p[5]) for p in pushes)
    put_fail = sum(int(c[5]) for c in cb)
    under = max((int(h[3]) for h in host), default=0)
    over = max((int(h[4]) for h in host), default=0)
    duty = [int(w[3]) for w in windows]
    silent = sum(1 for d in duty[1:] if d == 0)
    ok = under <= max_underruns and over <= max_underruns and dropped == 0 and put_fail == 0
    parts = [
        f"underruns={under if host else 'n/a'}",
        f"overruns={over if host else 'n/a'}",
        f"dropped_frames={dropped}",
        f"clamps={clamps}",
        f"cb_put_fail={put_fail}",
        f"max_underruns={max_underruns}",
    ]
    if duty:
        parts.append(f"windows={len(duty)} duty_mean={sum(duty) / len(duty):.1f}%")
        parts.append(f"silent_windows_after_first={silent}")
    if host:
        state, driver, pushed, _, _, back, peak = host[-1]
        parts.insert(0, f"state={state} driver={driver} pushed={pushed} peak_q={peak}")
    else:
        parts.insert(0, "queue_depth=n/a (not in this backend's telemetry)")
    report.add("audio", ok, " ".join(parts))


def check_fatal(report: Report, text: str) -> None:
    hits = sorted({line[:120] for line in text.splitlines()
                   if any(p in line for p in FATAL_PATTERNS)})
    if hits:
        report.add("fatal", False, f"markers={len(hits)} first={hits[0]!r}")
    else:
        report.add("fatal", True, "no fatal marker")


def audit(args: argparse.Namespace) -> Report:
    report = Report()
    if args.perf:
        rows = read_perf(Path(args.perf))
        check_presenting(report, rows, args.min_presenting, args.max_stall_s)
        check_cadence(report, rows, args.min_hz, args.cadence_tail_s)
    else:
        for name in ("presenting", "cadence"):
            report.add(name, None, "no perf.csv given")
    if args.proc:
        prows = _read_proc(Path(args.proc))
        check_growth(report, prows, "working_set_kb", "memory_working_set",
                     args.max_growth_pct, args.warmup_s)
        check_growth(report, prows, "private_bytes_kb", "memory_private",
                     args.max_growth_pct, args.warmup_s)
        check_growth(report, prows, "handles", "handles", args.max_growth_pct, args.warmup_s)
    else:
        for name in ("memory_working_set", "memory_private", "handles"):
            report.add(name, None, "no process-metrics CSV given (--proc)")
    if args.stderr:
        text = Path(args.stderr).read_text(encoding="utf-8", errors="replace")
        check_fatal(report, text)
        check_route(report, text)
        if args.audio:
            check_audio(report, text, args.max_underruns)
        else:
            report.add("audio", None, "not judged (pass --audio to read the run's own counters)")
    else:
        for name in ("fatal", "route", "audio"):
            report.add(name, None, "no stderr log given")
    return report


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--perf", help="logs/perf.csv from the run")
    ap.add_argument("--proc", help="process-metrics CSV: t_s,working_set_kb,private_bytes_kb,handles")
    ap.add_argument("--stderr", help="logs/stderr_run.log from the run")
    ap.add_argument("--min-presenting", type=float, default=0.95)
    ap.add_argument("--max-stall-s", type=float, default=20.0)
    ap.add_argument("--min-hz", type=float, default=59.5)
    ap.add_argument("--cadence-tail-s", type=int, default=30,
                    help="cadence is judged over the last N presenting seconds")
    ap.add_argument("--max-growth-pct", type=float, default=20.0)
    ap.add_argument("--warmup-s", type=int, default=3,
                    help="process samples ignored before the growth baseline")
    ap.add_argument("--max-underruns", type=int, default=0)
    ap.add_argument("--audio", action="store_true", help="judge the audio counters")
    ap.add_argument("--json", action="store_true", help="also print the report as JSON")
    args = ap.parse_args(argv)

    report = audit(args)
    print(report.render())
    if args.json:
        print(report.as_json())
    return 1 if report.failures else 0


if __name__ == "__main__":
    sys.exit(main())
