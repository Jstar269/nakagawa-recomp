#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Judge one run's VBLANK delivery against the display source that owed it.

A run's own artifacts already answer the only question that matters about pacing --
is the guest being told about MORE VBLANK episodes than the 60000/1001 Hz source
produced, which would run the game fast -- but only if they are read together and
over the WHOLE run:

    logs/perf.csv          one-second intervals; ``vblank_total`` is cumulative
                           episodes actually delivered (src/rt/perf.c counts one per
                           delivered episode) and ``wall_ms`` is that interval's
                           host wall time
    logs/stderr_run.log    ``PERF_ATTRIB vblank_owed`` / ``vblank_late`` ledgers

A per-second ``vblank_hz`` column cannot answer it.  A second holds 59, 60 or 61
episodes, so its ratio is quantised: 60 episodes in a 1.0005 s interval reads
59.94 Hz while 61 in the same interval reads 60.94 Hz.  Selecting the best-looking
seconds, or averaging the ratios, therefore reports a rate the run never delivered.
Over the whole run the quantisation averages out and the ratio is exact.

Checks, each with a number:

    presenting   the window judged: seconds that presented a frame, and its wall
                 time.  The pre-presentation index scan is excluded on both sides.
    identity     owed + coalesced == delivered + dropped + in_flight, from the
                 runtime's own ledger.  A mismatch is a bookkeeping bug, reported
                 as such rather than absorbed into a rate.
    rate         delivered / source periods, source periods = presenting wall time
                 x 60000/1001.  A ratio above 1 is over-delivery and below 1 is
                 under-delivery; both fail, with the excess or deficit in episodes
                 and in Hz.
    masked       episodes credited to masked windows minus the periods that
                 actually elapsed with interrupts clear.  Positive beyond the
                 in-flight residual is a masked window credited twice.

Nothing here knows what produced the telemetry: the same command judges a showcase,
a title route or a bare idle soak.  A missing artifact is reported as ``SKIP`` with
its reason, never as a pass, and a run where no check could run is ``NOT_RUN``.

    python tools/vblank_ledger.py --perf logs/perf.csv --stderr logs/stderr_run.log
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

#: The PSP display source, 60000/1001 Hz, as the runtime's rational deadline.
SOURCE_HZ = 60000.0 / 1001.0

LATE_ROW = re.compile(
    r"^PERF_ATTRIB vblank_late total=(\d+) lost_ms=(\d+) masked_periods=(\d+) "
    r"collapsed_periods=(\d+)",
    re.MULTILINE,
)
OWED_ROW = re.compile(
    r"^PERF_ATTRIB vblank_owed owed=(\d+) coalesced=(\d+) delivered=(\d+) "
    r"dropped=(\d+) in_flight=(\d+) late_owed=(\d+) late_delivered=(\d+)",
    re.MULTILINE,
)


@dataclass
class Report:
    checks: list[tuple[str, str | None, str]] = field(default_factory=list)

    def add(self, name: str, ok: bool | None, detail: str) -> None:
        self.checks.append((name, ok, detail))

    @property
    def failures(self) -> int:
        return sum(1 for _, ok, _ in self.checks if ok is False)

    @property
    def judged(self) -> int:
        return sum(1 for _, ok, _ in self.checks if ok is not None)

    def line(self, name: str, ok: bool | None, detail: str) -> str:
        status = "SKIP" if ok is None else ("PASS" if ok else "FAIL")
        return f"VBLANK_CHECK: {name} {status} {detail}"

    def render(self) -> str:
        out = [self.line(*c) for c in self.checks]
        if not self.judged:
            out.append("VBLANK_AUDIT: verdict=NOT_RUN checks=0 failures=0")
        else:
            verdict = "FAIL" if self.failures else "PASS"
            out.append(
                f"VBLANK_AUDIT: verdict={verdict} checks={self.judged} failures={self.failures}"
            )
        return "\n".join(out)


def _number(row: dict[str, str], key: str) -> float | None:
    raw = row.get(key)
    if raw in (None, ""):
        return None
    try:
        return float(raw)
    except ValueError:
        return None


def presenting_window(rows: list[dict[str, str]]) -> list[tuple[int, dict[str, str]]]:
    """Rows that presented a frame, in file order, with the row index.

    The pre-presentation index scan is one enormous interval with ``fps`` 0; keeping
    it would divide delivered episodes by a wall time in which the source was not
    yet running.
    """
    return [(i, r) for i, r in enumerate(rows) if (_number(r, "fps") or 0.0) > 0.0]


def read_perf(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", errors="ignore", newline="") as handle:
        return list(csv.DictReader(handle))


@dataclass
class Cadence:
    presenting_s: int
    wall_s: float
    source_periods: float
    delivered: int
    ratio: float
    per_second_mean_hz: float
    per_second_counts: Counter


def measure_cadence(rows: list[dict[str, str]]) -> Cadence | None:
    window = presenting_window(rows)
    if not window:
        return None
    first_index = window[0][0]
    base = 0
    if first_index > 0:
        base = int(_number(rows[first_index - 1], "vblank_total") or 0.0)
    wall_s = 0.0
    last = base
    for _, row in window:
        wall_s += (_number(row, "wall_ms") or 0.0) / 1000.0
        last = int(_number(row, "vblank_total") or 0.0)
    # ``vblank_total`` is cumulative, so the window delivered the last reading less
    # whatever the interval before it had already banked.
    delivered = last - base
    # Per-second episode counts are the differences of the cumulative column; they
    # are what the quantisation argument is about, so they are reported, not
    # smoothed away.
    counts: Counter = Counter()
    previous = base
    for row in rows[first_index:]:
        total = int(_number(row, "vblank_total") or 0.0)
        counts[total - previous] += 1
        previous = total
    periods = wall_s * SOURCE_HZ
    per_second = [
        (_number(row, "vblank_hz") or 0.0)
        for _, row in window
        if (_number(row, "vblank_hz") or 0.0) > 0.0
    ]
    return Cadence(
        presenting_s=len(window),
        wall_s=wall_s,
        source_periods=periods,
        delivered=delivered,
        ratio=(delivered / periods) if periods > 0.0 else 0.0,
        per_second_mean_hz=(sum(per_second) / len(per_second)) if per_second else 0.0,
        per_second_counts=counts,
    )


def check_presenting(report: Report, cadence: Cadence | None) -> None:
    if cadence is None:
        report.add("presenting", None, "no perf.csv second presented a frame")
        return
    report.add(
        "presenting",
        True,
        f"presenting_s={cadence.presenting_s} wall_s={cadence.wall_s:.3f} "
        f"source_periods={cadence.source_periods:.1f} delivered={cadence.delivered}",
    )


def check_rate(report: Report, cadence: Cadence | None, max_ratio: float, min_ratio: float) -> None:
    if cadence is None or cadence.source_periods <= 0.0:
        report.add("rate", None, "no presenting wall time to compare against")
        return
    ok = min_ratio <= cadence.ratio <= max_ratio
    excess = cadence.delivered - cadence.source_periods
    detail = (
        f"ratio={cadence.ratio:.5f} band=[{min_ratio:.5f},{max_ratio:.5f}] "
        f"excess_episodes={excess:+.0f} excess_hz={excess / cadence.wall_s:+.3f} "
        f"per_second_mean_hz={cadence.per_second_mean_hz:.2f} "
        f"per_second_delivered={dict(sorted(cadence.per_second_counts.items()))}"
    )
    report.add("rate", ok, detail)


def check_identity(report: Report, text: str | None) -> None:
    if text is None:
        report.add("identity", None, "no stderr log given (--stderr)")
        return
    owed = OWED_ROW.findall(text)
    if not owed:
        report.add(
            "identity", None, "no PERF_ATTRIB vblank_owed ledger in the stderr log (run with SR_PERF=1)"
        )
        return
    o, coal, deliv, drop, inflight, late_owed, late_deliv = (int(g) for g in owed[-1])
    residual = o + coal - deliv - drop
    ok = residual == inflight
    report.add(
        "identity",
        ok,
        f"owed={o} coalesced={coal} delivered={deliv} dropped={drop} in_flight={inflight} "
        f"residual={residual} late_owed={late_owed} late_delivered={late_deliv}",
    )


def check_masked(report: Report, text: str | None) -> None:
    if text is None:
        report.add("masked", None, "no stderr log given (--stderr)")
        return
    owed = OWED_ROW.findall(text)
    late = LATE_ROW.findall(text)
    if not owed or not late:
        report.add("masked", None, "no vblank ledger in the stderr log (run with SR_PERF=1)")
        return
    o, coal, _deliv, _drop, inflight, _lo, _ld = (int(g) for g in owed[-1])
    masked = int(late[-1][2])
    credit = coal - masked
    ok = credit <= inflight
    report.add(
        "masked",
        ok,
        f"coalesced={coal} masked_periods={masked} credit={credit:+d} in_flight={inflight}",
    )


def audit(args: argparse.Namespace) -> Report:
    report = Report()
    perf: Path | None = args.perf
    rows = read_perf(perf) if perf else []
    if perf and not rows:
        report.add("presenting", None, f"{perf} has no rows")
    cadence = measure_cadence(rows) if rows else None
    check_presenting(report, cadence)
    check_rate(report, cadence, args.max_ratio, args.min_ratio)
    text = args.stderr.read_text(encoding="utf-8", errors="ignore") if args.stderr else None
    check_identity(report, text)
    check_masked(report, text)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--perf", type=Path, help="logs/perf.csv from the run")
    parser.add_argument("--stderr", type=Path, help="logs/stderr_run.log from the same run")
    parser.add_argument(
        "--max-ratio",
        type=float,
        default=1.005,
        help="largest delivered/source ratio that is still anchored (default 1.005)",
    )
    parser.add_argument(
        "--min-ratio",
        type=float,
        default=0.995,
        help="smallest delivered/source ratio that is still anchored (default 0.995)",
    )
    args = parser.parse_args(argv)
    for name in ("perf", "stderr"):
        path = getattr(args, name)
        if path is not None and not path.is_file():
            print(f"VBLANK_LEDGER: {name} {path} is not a file", file=sys.stderr)
            return 2
    report = audit(args)
    print(report.render())
    return 1 if (report.failures or not report.judged) else 0


if __name__ == "__main__":
    sys.exit(main())
