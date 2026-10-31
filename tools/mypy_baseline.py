# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Enforce a per-module ratchet for the existing tools/ mypy errors."""

from __future__ import annotations

import argparse
from collections.abc import Mapping
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
BASELINE = Path(__file__).with_name("mypy_baseline.json")
MYPY_VERSION = "2.1.0"
PYTHON_VERSION = "3.15"
PLATFORM = "linux"
MYPY_ARGS = [
    "--no-incremental",
    "--no-error-summary",
    "--no-pretty",
    "--python-version",
    PYTHON_VERSION,
    "--platform",
    PLATFORM,
    "--explicit-package-bases",
    "tools",
]
COMMAND = ["python", "-m", "mypy", *MYPY_ARGS]
ERROR = re.compile(r"^(?P<path>tools[/\\].+?\.py):\d+(?::\d+)?: error:")


def _installed_mypy_version() -> str:
    run = subprocess.run(
        [sys.executable, "-m", "mypy", "--version"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if run.returncode != 0 or run.stderr:
        raise RuntimeError(f"could not read mypy version: {run.stdout}{run.stderr}")
    match = re.match(r"^mypy (?P<version>\S+)", run.stdout.strip())
    if match is None:
        raise RuntimeError(f"unexpected mypy version output: {run.stdout.strip()!r}")
    return match.group("version")


def _collect_errors(output: str) -> dict[str, int]:
    errors: dict[str, int] = {}
    for line in output.splitlines():
        match = ERROR.match(line)
        if match is not None:
            path = match.group("path").replace("\\", "/")
            errors[path] = errors.get(path, 0) + 1
        elif line.strip() and ": note:" not in line:
            raise ValueError(f"unexpected mypy output: {line}")
    return dict(sorted(errors.items()))


def _metadata() -> dict[str, Any]:
    return {
        "schema_version": 1,
        "mypy_version": MYPY_VERSION,
        "python_version": PYTHON_VERSION,
        "platform": PLATFORM,
        "command": COMMAND,
    }


def _read_baseline() -> tuple[dict[str, int], int]:
    try:
        data = json.loads(BASELINE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read baseline: {exc}") from exc
    if not isinstance(data, dict):
        raise ValueError("baseline root must be an object")
    for key, value in _metadata().items():
        if data.get(key) != value:
            raise ValueError(f"baseline {key} must be {value!r}, got {data.get(key)!r}")
    modules = data.get("errors_by_module")
    if not isinstance(modules, dict):
        raise ValueError("baseline errors_by_module must be an object")
    errors: dict[str, int] = {}
    for path, count in modules.items():
        if not isinstance(path, str) or not path.startswith("tools/") or not path.endswith(".py"):
            raise ValueError(f"invalid baseline module: {path!r}")
        if type(count) is not int or count < 0:
            raise ValueError(f"invalid baseline count for {path}: {count!r}")
        errors[path] = count
    total = data.get("total_errors")
    expected_total = sum(errors.values())
    if type(total) is not int or total != expected_total:
        raise ValueError(f"baseline total_errors must be {expected_total}, got {total!r}")
    return errors, total


def _regressions(current: Mapping[str, int], baseline: Mapping[str, int]) -> list[str]:
    return [
        f"{path}: {count} errors (baseline {baseline.get(path, 0)})"
        for path, count in sorted(current.items())
        if count > baseline.get(path, 0)
    ]


def _write_baseline(errors: Mapping[str, int]) -> None:
    data = {
        **_metadata(),
        "total_errors": sum(errors.values()),
        "errors_by_module": dict(sorted(errors.items())),
    }
    temporary = BASELINE.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(BASELINE)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--update",
        action="store_true",
        help="lower the baseline to current counts; increases still fail",
    )
    args = parser.parse_args(argv)

    try:
        actual_version = _installed_mypy_version()
        if actual_version != MYPY_VERSION:
            raise ValueError(f"mypy {MYPY_VERSION} is required, found {actual_version}")
        run = subprocess.run(
            [sys.executable, "-m", "mypy", *MYPY_ARGS],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if run.stderr or run.returncode not in (0, 1):
            raise RuntimeError(f"mypy failed to run (exit {run.returncode}): {run.stdout}{run.stderr}")
        current = _collect_errors(run.stdout)
        if (run.returncode == 1) != bool(current):
            raise ValueError("mypy exit status does not match its error diagnostics")

        if args.update:
            if BASELINE.exists():
                baseline, _baseline_total = _read_baseline()
                failures = _regressions(current, baseline)
                if failures:
                    print("\n".join(failures), file=sys.stderr)
                    return 1
            _write_baseline(current)
            print(f"mypy baseline updated: {sum(current.values())} errors in {len(current)} modules")
            return 0

        baseline, baseline_total = _read_baseline()
        failures = _regressions(current, baseline)
        if failures:
            print("\n".join(failures), file=sys.stderr)
            return 1
        print(
            f"mypy baseline PASS: {sum(current.values())} errors in {len(current)} modules "
            f"(baseline {baseline_total})"
        )
        return 0
    except (OSError, RuntimeError, TypeError, ValueError) as exc:
        print(f"mypy baseline error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
