#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Compare the flight recorder linked-but-disarmed against compiled out on the GE draw path.

The ge-flight-overhead Makefile target builds two copies of the
ge-cpu-profile-validate-selftest harness around the production ge.c, one with the recorder
linked and one with -USR_FLIGHT_RECORDER_LINKED, and this script runs them in alternating
pairs through their `--bench` mode (issue #320, acceptance item 4). It checks the two builds
draw the same pixels, that the compiled-out ge.o references no sr_flight_ symbol, and
reports the per-workload difference (linked minus absent) with a 95% percentile bootstrap
interval over the pairs.
"""

from __future__ import annotations

import argparse
import random
import re
import statistics
import subprocess
import sys
from pathlib import Path

LINE = re.compile(r"^GE_FLIGHT_BENCH (?P<body>.+)$", re.MULTILINE)
BOOTSTRAP_RESAMPLES = 2000
BOOTSTRAP_SEED = 320


def parse_bench(text: str) -> dict[str, dict[str, str]]:
    """Return {workload: {key: value}} from one run's output; class_check is a workload."""
    found: dict[str, dict[str, str]] = {}
    for match in LINE.finditer(text):
        fields = match.group("body").split()
        pairs = dict(item.split("=", 1) for item in fields if "=" in item)
        name = pairs.get("workload") or ("class_check" if "class_check" in fields else None)
        if name is None:
            raise ValueError(f"bench line without a workload: {match.group(0)!r}")
        if name in found:
            raise ValueError(f"workload {name} reported twice")
        found[name] = pairs
    return found


def bootstrap_interval(diffs: list[float], seed: int = BOOTSTRAP_SEED) -> tuple[float, float]:
    """95% percentile bootstrap interval on the median of `diffs`."""
    if not diffs:
        raise ValueError("no differences")
    rng = random.Random(seed)
    medians = sorted(
        statistics.median(rng.choices(diffs, k=len(diffs))) for _ in range(BOOTSTRAP_RESAMPLES)
    )
    return medians[int(0.025 * BOOTSTRAP_RESAMPLES)], medians[int(0.975 * BOOTSTRAP_RESAMPLES) - 1]


def summarize(runs: dict[str, list[dict[str, dict[str, str]]]], key: str) -> list[str]:
    """Report lines for each workload; `runs` maps flavour to its per-run parsed output."""
    linked, absent = runs["linked"], runs["absent"]
    if len(linked) != len(absent) or not linked:
        raise ValueError("linked and absent need the same non-zero number of runs")
    lines = []
    for workload in (w for w in linked[0] if w != "class_check"):
        hashes = {run[workload]["fb_hash"] for run in linked + absent}
        if len(hashes) != 1:
            raise ValueError(f"{workload}: builds drew different pixels ({sorted(hashes)})")
        a = [float(run[workload][key]) for run in absent]
        b = [float(run[workload][key]) for run in linked]
        diffs = [x - y for x, y in zip(b, a, strict=True)]
        low, high = bootstrap_interval(diffs)
        if low > 0.0:
            verdict = "linked slower"
        elif high < 0.0:
            verdict = "linked faster (code layout or drift, not a recorder saving)"
        else:
            verdict = "not distinguishable from noise"
        lines.append(
            f"{workload}: prims={linked[0][workload]['prims']} fb_hash={hashes.pop()} "
            f"{key} linked={statistics.median(b):.0f} absent={statistics.median(a):.0f} "
            f"delta=[{low:+.0f}, {high:+.0f}] ns over {len(diffs)} pairs: {verdict}"
        )
    check = linked[0].get("class_check")
    if check:
        lines.append(
            f"class_check: {int(check['ns_per_call_x1000']) / 1000.0:.3f} ns per disarmed "
            f"sr_flight_class_enabled call"
        )
    return lines


def undefined_flight_symbols(nm_output: str) -> list[str]:
    return sorted({m for m in re.findall(r"\bsr_flight_\w+", nm_output)})


def run_bench(exe: Path, rounds: int) -> dict[str, dict[str, str]]:
    result = subprocess.run(
        [str(exe.resolve()), "--bench", str(rounds)], capture_output=True, text=True, check=True
    )
    return parse_bench(result.stdout)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dir", type=Path, required=True)
    ap.add_argument("--rounds", type=int, default=30)
    ap.add_argument("--pairs", type=int, default=5)
    ap.add_argument("--key", default="ns_per_list_median")
    args = ap.parse_args(argv)
    if args.rounds < 1 or args.pairs < 2:
        ap.error("--rounds must be at least 1 and --pairs at least 2")

    for flavour, expect_symbols in (("linked", True), ("absent", False)):
        nm = subprocess.run(
            ["nm", str(args.dir / flavour / "ge.o")], capture_output=True, text=True, check=True
        )
        symbols = undefined_flight_symbols(nm.stdout)
        print(f"ge.o ({flavour}): {len(symbols)} sr_flight_ symbol(s)")
        if bool(symbols) != expect_symbols:
            print(f"error: ge.o ({flavour}) has the wrong sr_flight_ references", file=sys.stderr)
            return 1

    runs: dict[str, list[dict[str, dict[str, str]]]] = {"linked": [], "absent": []}
    for pair in range(args.pairs):
        order = ("linked", "absent") if pair % 2 == 0 else ("absent", "linked")
        for flavour in order:
            exe = args.dir / flavour / "bench.exe"
            if not exe.exists():
                exe = args.dir / flavour / "bench"
            runs[flavour].append(run_bench(exe, args.rounds))
    try:
        for line in summarize(runs, args.key):
            print(line)
    except ValueError as err:
        print(f"error: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
