#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import time
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
WORKLOADS = (
    ("production-smoke-gap", ("production-smoke-gap",)),
    ("cosim", ("cosim-selftest",)),
    ("psmf-media", ("psmf-media-selftest",)),
    ("audio", ("audio-selftest",)),
    ("atrac3p", ("atrac3p-bridge-selftest",)),
    ("platform-ladder-gap", ("platform-ladder-gap",)),
    ("platform-ladder-sched", ("platform-ladder-sched",)),
    ("platform-ladder-fs", ("platform-ladder-fs",)),
)


def _make_name(value: str) -> str:
    return shutil.which(value) or value


def _clear_outputs(output_dir: Path) -> None:
    for name in ("perf.json", "perf.csv"):
        try:
            (output_dir / name).unlink()
        except FileNotFoundError:
            pass


def _run_one(make: str, python: str, name: str, targets: tuple[str, ...],
             output_root: Path, enabled: bool) -> dict[str, Any]:
    output_dir = output_root / name
    output_dir.mkdir(parents=True, exist_ok=True)
    _clear_outputs(output_dir)
    env = os.environ.copy()
    env["PYTHON"] = python
    env["SR_PERF"] = "1" if enabled else "0"
    env["SR_PERF_JSON"] = str((output_dir / "perf.json").resolve())
    env["SR_PERF_CSV"] = str((output_dir / "perf.csv").resolve())
    command = [make, f"BUILD_DIR=build/perf-benchmark/{name}", *targets]
    started = time.perf_counter()
    completed = subprocess.run(command, cwd=ROOT, env=env, check=False,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True)
    duration = time.perf_counter() - started
    summary_path = output_dir / "perf.json"
    output = completed.stdout[-4000:]
    status = "PASS" if completed.returncode == 0 else "SKIP" if completed.returncode == 77 else "FAIL"
    if enabled and completed.returncode == 0 and not summary_path.exists():
        status = "FAIL"
        output += "\nSR_PERF summary was not produced"
    try:
        summary = str(summary_path.relative_to(ROOT)) if summary_path.exists() else None
    except ValueError:
        summary = str(summary_path) if summary_path.exists() else None
    return {
        "name": name,
        "targets": list(targets),
        "status": status,
        "returncode": completed.returncode,
        "duration_s": duration,
        "summary": summary,
        "output": output,
    }


def _run_direct(command: list[str], output_dir: Path, enabled: bool) -> dict[str, Any]:
    output_dir.mkdir(parents=True, exist_ok=True)
    _clear_outputs(output_dir)
    env = os.environ.copy()
    env["SR_PERF"] = "1" if enabled else "0"
    env["SR_PERF_JSON"] = str((output_dir / "perf.json").resolve())
    env["SR_PERF_CSV"] = str((output_dir / "perf.csv").resolve())
    started = time.perf_counter()
    completed = subprocess.run(command, cwd=ROOT, env=env, check=False,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True)
    duration = time.perf_counter() - started
    summary_path = output_dir / "perf.json"
    status = "PASS" if completed.returncode == 0 else "SKIP" if completed.returncode == 77 else "FAIL"
    if enabled and completed.returncode == 0 and not summary_path.exists():
        status = "FAIL"
    return {
        "status": status,
        "returncode": completed.returncode,
        "duration_s": duration,
        "summary_exists": summary_path.exists(),
        "output": completed.stdout[-4000:],
    }


def _run_overhead(make: str, python: str, output_root: Path, samples: int) -> dict[str, Any]:
    build = _run_one(make, python, "overhead-build", ("production-smoke-gap",), output_root, False)
    if build["status"] != "PASS":
        return {"status": "BLOCKED", "build": build, "ratio": None}
    command = [
        python,
        str(ROOT / "fixtures" / "production_smoke" / "generate.py"),
        "run",
        "--build-dir",
        str(ROOT / "build" / "production-smoke-gap"),
        "--mode",
        "aot-gap",
    ]
    measured: dict[str, Any] = {}
    for enabled in (False, True):
        runs = [
            _run_direct(command, output_root / f"overhead-{'enabled' if enabled else 'disabled'}-{index}", enabled)
            for index in range(samples)
        ]
        measured["enabled" if enabled else "disabled"] = runs
    if any(run["status"] != "PASS" for group in measured.values() for run in group):
        return {"status": "BLOCKED", "build": build, **measured, "ratio": None}
    disabled_median = statistics.median(run["duration_s"] for run in measured["disabled"])
    enabled_median = statistics.median(run["duration_s"] for run in measured["enabled"])
    return {
        "status": "PASS",
        "build": build,
        **measured,
        "disabled_median_s": disabled_median,
        "enabled_median_s": enabled_median,
        "ratio": enabled_median / disabled_median if disabled_median else None,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run the public source-owned SR_PERF benchmark matrix")
    parser.add_argument("--make", default=os.environ.get("MAKE", "mingw32-make"))
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "perf-benchmark")
    parser.add_argument("--overhead", action="store_true")
    parser.add_argument("--samples", type=int, default=3)
    args = parser.parse_args(argv)
    make = _make_name(args.make)
    if shutil.which(make) is None:
        print(f"error: make executable not found: {args.make}", file=sys.stderr)
        return 2
    if args.samples < 1:
        parser.error("--samples must be positive")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    runs = [_run_one(make, args.python, name, targets, args.output, True)
            for name, targets in WORKLOADS]
    result: dict[str, Any] = {
        "schema": "nakagawa-perf-benchmark-v1",
        "public_source_owned": True,
        "runs": runs,
    }
    if args.overhead:
        result["overhead"] = _run_overhead(make, args.python, args.output, args.samples)
    report_path = args.output / "benchmark.json"
    report_path.write_text(json.dumps(result, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(result, indent=2, sort_keys=True))
    failed = any(run["status"] == "FAIL" for run in runs)
    if args.overhead and result["overhead"]["status"] != "PASS":
        failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
