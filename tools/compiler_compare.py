#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Compare host C compilers on the public generated corpus, veto first.

Issue #317 asks whether Clang/LLVM materially improves Nakagawa's generated-C
compile quality, runtime performance, build time, or portability against the
supported GCC/UCRT64 route.  That question is only meaningful if a candidate
compiler is first shown to preserve the accepted guest contract, so this
harness makes semantics a hard veto rather than a footnote:

* each requested compiler that resolves on ``PATH`` builds the same public
  source-owned corpus into its own build root, then runs the cosimulation
  comparison, the cosimulation negative corpus, and the stale-code and
  COP0/exception selftests under that same compiler;
* the first failing gate stops the chain and the compiler is reported
  ``VETOED`` naming that gate;
* build time, binary size and the ``run_perf_benchmarks.py`` public workload
  timings are recorded only after the whole veto chain passes, and
  :func:`validate_report` refuses to accept a report that carries a timing
  under a vetoed or unavailable compiler;
* a compiler that does not resolve on ``PATH`` is reported ``NOT_AVAILABLE``,
  never a pass.

No compiler default is changed by running this, and no compiler-specific
workaround enters guest semantics.  Findings belong in issue #317.

Corpus and gates are public, source-owned and synthetic only: the
``production-smoke-gap`` AOT-gap fixture, the generated ``cosim`` fixture, and
the host-neutral ``stale_code``/``cpu_lle`` selftest binaries.  No title, ISO,
ELF or PRX input is read, and every write stays inside the ``--output`` root,
which defaults to ``build/compiler-compare``.

Usage::

    python tools/compiler_compare.py                        # gcc then clang
    python tools/compiler_compare.py --compilers gcc
    python tools/compiler_compare.py --output build/compiler-compare

Compilers run one at a time and each owns a disjoint build root, so a
comparison never mixes objects.  ``CC`` reaches make on the command line,
which GNU Make propagates to the recursive ``$(MAKE)`` invocations and, through
``MAKEFLAGS`` (``-- VAR=value`` entries), into the nested campaign
``cosim-mutants`` drives itself; the build roots are passed with forward
slashes because make hands them to a shell.

One boundary of that isolation belongs to the platform ladder, not to this
harness: its Makefile targets name their own ``build/platform-ladder/<workload>``
directory instead of ``BUILD_DIR``, so three of the eight #282 workloads
compile into the same tree whichever compiler asked for them.  Compilers still
run sequentially and the build-profile stamp carries the compiler identity, so
no object is reused across compilers; the shared tree is named here rather than
hidden.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = "nakagawa-compiler-compare-v1"
PERF_SCHEMA = "nakagawa-perf-benchmark-v1"
ISSUE = 317

DEFAULT_COMPILERS = ("gcc", "clang")
CORPUS_GATE = "corpus-build"
CORPUS_TARGET = "production-smoke-gap"
#: ``GAME_NAME`` the Makefile gives the corpus build; its executable is the
#: binary whose size the comparison records.
CORPUS_GAME = "production_smoke_gap"
CORPUS_OVERRIDES = (("PRODUCTION_SMOKE_GAP_DIR", "corpus"),)

#: Gate name -> (make target, make variable overrides).  The order is the veto
#: order and is part of the contract: the corpus compiles first because a
#: candidate that cannot build the generated code has nothing to compare, and
#: the cosimulation runs before the negative corpus because a candidate that
#: already diverges from the interpreter must not be handed the mutation
#: campaign.
VETO_GATES: tuple[tuple[str, str, tuple[tuple[str, str], ...]], ...] = (
    ("cosim-selftest", "cosim-selftest", (("COSIM_DIR", "cosim"),)),
    ("cosim-mutants", "cosim-mutants", (("COSIM_DIR", "cosim"),)),
    ("stale-code-selftest", "stale-code-selftest", (("BUILD_DIR", "gates"),)),
    ("cpu-lle-selftest", "cpu-lle-selftest", (("BUILD_DIR", "gates"),)),
)
GATE_CHAIN: tuple[tuple[str, str, tuple[tuple[str, str], ...]], ...] = (
    (CORPUS_GATE, CORPUS_TARGET, CORPUS_OVERRIDES),
    *VETO_GATES,
)
GATE_ORDER: tuple[str, ...] = tuple(name for name, _, _ in GATE_CHAIN)

MEASURED = "MEASURED"
VETOED = "VETOED"
WORKLOAD_FAILED = "WORKLOAD_FAILED"
NOT_AVAILABLE = "NOT_AVAILABLE"
STATUSES = (MEASURED, VETOED, WORKLOAD_FAILED, NOT_AVAILABLE)
#: A vetoed or unavailable compiler carries no timing at all.  This is the key
#: set :func:`validate_report` scans such an entry for, so the ordering
#: invariant survives a later refactor that publishes a measurement by
#: accident instead of by this contract.
TIMING_KEYS = frozenset({
    "build_seconds",
    "duration_s",
    "disabled_median_s",
    "enabled_median_s",
    "ratio",
})
GATE_PASS = "PASS"
GATE_FAIL = "FAIL"
OUTPUT_TAIL = 4000


class CompareError(ValueError):
    """A harness invariant broke; the run fails instead of reporting a number."""


def _fail(message: str) -> None:
    raise CompareError(message)


@dataclass(frozen=True)
class _Outcome:
    returncode: int | None
    output: str
    duration_s: float


def _run(command: list[str], *, env: dict[str, str], cwd: Path) -> _Outcome:
    started = time.perf_counter()
    try:
        completed = subprocess.run(
            command, cwd=cwd, env=env, check=False,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
        )
    except OSError as exc:
        return _Outcome(None, f"{command[0]} could not be executed: {exc}", time.perf_counter() - started)
    return _Outcome(completed.returncode, completed.stdout, time.perf_counter() - started)


def _compiler_version(path: str, *, env: dict[str, str]) -> str | None:
    """First line of ``<cc> --version``; ``None`` when the compiler cannot report one."""
    outcome = _run([path, "--version"], env=env, cwd=ROOT)
    lines = outcome.output.strip().splitlines() if outcome.returncode == 0 else []
    return lines[0].strip() if lines else None


def _executable_bytes(corpus_dir: Path) -> int:
    for name in (f"{CORPUS_GAME}.exe", CORPUS_GAME):
        candidate = corpus_dir / name
        if candidate.is_file():
            return candidate.stat().st_size
    _fail(f"corpus build produced no {CORPUS_GAME} executable in {corpus_dir}")


def _object_bytes(corpus_dir: Path) -> int:
    return sum(path.stat().st_size for path in sorted(corpus_dir.rglob("*.o")))


def _gate_command(make: str, compiler: str, target: str,
                  overrides: tuple[tuple[str, str], ...], build_root: Path) -> list[str]:
    command = [make, f"CC={compiler}", target]
    command.extend(f"{name}={(build_root / value).as_posix()}" for name, value in overrides)
    return command


SETTING_LINE = re.compile(r"^[A-Z][A-Z0-9_]*=")


def _build_settings(make: str, compiler: str, build_root: Path, *,
                    env: dict[str, str]) -> str | None:
    """The effective output-affecting settings make resolved for this compiler.

    Recorded as evidence for issue #317, which asks for the full set of
    output-affecting flags next to the compiler identity.  ``compiler-info``
    echoes one ``KEY=value`` line per setting and also triggers the build-profile
    bookkeeping its parse-time includes imply; only the echoed settings are kept,
    so the flags are recorded whole instead of tail-truncated.  The target
    performs no compilation, so a failure here is reported rather than turned into
    a veto.  It is pointed at this compiler's own build root because of those
    profile writes.
    """
    command = [make, f"CC={compiler}", f"BUILD_DIR={(build_root / 'info').as_posix()}", "compiler-info"]
    outcome = _run(command, env=env, cwd=ROOT)
    if outcome.returncode != 0:
        return None
    settings = "\n".join(line for line in outcome.output.splitlines() if SETTING_LINE.match(line))
    return settings.strip() or None


def _run_veto(make: str, compiler: str, build_root: Path, *,
              env: dict[str, str]) -> tuple[list[dict[str, Any]], str | None, float]:
    """Run the gate chain in the fixed order, stopping at the first failure.

    Returns the gate records, the name of the gate that stopped the chain (or
    ``None``), and the corpus build wall time.  The build time is returned as a
    bare number rather than published here: only the caller may place it in a
    report, and the caller does so only when no gate failed.
    """
    gates: list[dict[str, Any]] = []
    build_seconds = 0.0
    for name, target, overrides in GATE_CHAIN:
        command = _gate_command(make, compiler, target, overrides, build_root)
        outcome = _run(command, env=env, cwd=ROOT)
        passed = outcome.returncode == 0
        if name == CORPUS_GATE:
            build_seconds = outcome.duration_s
        gates.append({
            "name": name,
            "target": target,
            "status": GATE_PASS if passed else GATE_FAIL,
            "returncode": outcome.returncode,
            "command": command,
            "output": outcome.output[-OUTPUT_TAIL:],
        })
        if not passed:
            return gates, name, build_seconds
    return gates, None, build_seconds


def _workload_measurements(python: str, make: str, perf_dir: Path, build_prefix: Path, *,
                           env: dict[str, str]) -> dict[str, Any]:
    """Run the public ``run_perf_benchmarks.py`` matrix under this compiler.

    The matrix gets its own build prefix inside this compiler's root: a second
    compiler must never reuse the first compiler's objects, and
    ``run_perf_benchmarks.py`` otherwise derives one shared ``build/perf-*``
    tree.  (The platform-ladder targets still build into their own directory;
    see the module docstring.)  The SR_PERF overhead check is deliberately not
    run -- it measures the instrumentation's cost in a fixed build directory,
    not a compiler, and it would reintroduce exactly that shared tree.
    """
    command = [
        python, str(ROOT / "tools" / "run_perf_benchmarks.py"),
        "--make", make, "--python", python,
        "--output", str(perf_dir), "--build-prefix", build_prefix.as_posix(),
    ]
    outcome = _run(command, env=env, cwd=ROOT)
    report = perf_dir / "benchmark.json"
    if not report.is_file():
        return {
            "status": GATE_FAIL,
            "detail": f"benchmark report absent (make exit {outcome.returncode}): {outcome.output[-OUTPUT_TAIL:]}",
            "workloads": [],
        }
    try:
        matrix = json.loads(report.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return {"status": GATE_FAIL, "detail": f"{report}: unreadable benchmark report: {exc}", "workloads": []}
    if not isinstance(matrix, dict) or matrix.get("schema") != PERF_SCHEMA:
        return {"status": GATE_FAIL, "detail": f"{report}: unexpected schema", "workloads": []}
    runs = matrix.get("runs")
    if not isinstance(runs, list) or not runs:
        return {"status": GATE_FAIL, "detail": f"{report}: no workload runs recorded", "workloads": []}
    workloads: list[dict[str, Any]] = []
    for index, run in enumerate(runs):
        if not isinstance(run, dict) or not all(
            key in run for key in ("name", "targets", "status", "duration_s", "summary")
        ):
            return {"status": GATE_FAIL, "detail": f"{report}: runs[{index}] is not a workload record",
                    "workloads": []}
        workloads.append({
            "name": run["name"],
            "targets": run["targets"],
            # SKIP is carried through as SKIP: a workload that could not run is
            # never folded into a pass.
            "status": run["status"],
            "duration_s": run["duration_s"],
            "summary": run["summary"],
            # A failing workload keeps its own output: the report is the only
            # evidence a reader gets, so the reason must travel with it.
            "output": str(run.get("output", ""))[-OUTPUT_TAIL:],
        })
    return {
        "status": GATE_FAIL if any(run["status"] == GATE_FAIL for run in workloads) else GATE_PASS,
        "detail": None,
        "workloads": workloads,
    }


def _unavailable(name: str) -> dict[str, Any]:
    return {
        "name": name,
        "path": None,
        "version": None,
        "build_root": None,
        "build_settings": None,
        "veto": None,
        "measurements": None,
        "status": NOT_AVAILABLE,
    }


def _measure_compiler(name: str, path: str, make: str, python: str, build_root: Path, *,
                      env: dict[str, str]) -> dict[str, Any]:
    gates, failed_gate, build_seconds = _run_veto(make, name, build_root, env=env)
    record: dict[str, Any] = {
        "name": name,
        "path": path,
        "version": _compiler_version(path, env=env),
        "build_root": build_root.as_posix(),
        "build_settings": _build_settings(make, name, build_root, env=env),
        "veto": {
            "status": GATE_FAIL if failed_gate is not None else GATE_PASS,
            "failed_gate": failed_gate,
            "gates": gates,
        },
        "measurements": None,
    }
    if failed_gate is not None:
        # The veto stopped the chain, so nothing timed or sized is published.
        record["status"] = VETOED
        return record
    # Size the corpus before the workload matrix runs: the corpus build already
    # happened during the veto, and a missing executable is a harness invariant
    # that must surface immediately rather than after minutes of timing.
    corpus_dir = build_root / "corpus"
    executable_bytes = _executable_bytes(corpus_dir)
    object_bytes = _object_bytes(corpus_dir)
    workloads = _workload_measurements(
        python, make, build_root / "perf", build_root / "perf-build", env=env,
    )
    record["measurements"] = {
        "build_seconds": build_seconds,
        "executable_bytes": executable_bytes,
        "object_bytes": object_bytes,
        "workloads_status": workloads["status"],
        "workloads_detail": workloads["detail"],
        "workloads": workloads["workloads"],
    }
    record["status"] = WORKLOAD_FAILED if workloads["status"] == GATE_FAIL else MEASURED
    return record


def _timing_keys(value: Any, path: str = "$") -> list[str]:
    found: list[str] = []
    if isinstance(value, dict):
        for key, child in value.items():
            found.extend(_timing_keys(child, f"{path}.{key}"))
            if key in TIMING_KEYS:
                found.append(f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            found.extend(_timing_keys(child, f"{path}[{index}]"))
    return found


def validate_report(report: Any) -> None:
    """Fail closed on a report that breaks the veto-before-measurement contract."""
    if not isinstance(report, dict):
        _fail("$: expected an object")
    if report.get("schema") != SCHEMA:
        _fail(f"$.schema: expected {SCHEMA!r}")
    if report.get("issue") != ISSUE:
        _fail(f"$.issue: expected {ISSUE}")
    compilers = report.get("compilers")
    if not isinstance(compilers, list) or not compilers:
        _fail("$.compilers: expected a non-empty array")
    seen: set[str] = set()
    for index, entry in enumerate(compilers):
        path = f"$.compilers[{index}]"
        if not isinstance(entry, dict):
            _fail(f"{path}: expected an object")
        status = entry.get("status")
        if status not in STATUSES:
            _fail(f"{path}.status: expected one of {STATUSES}")
        name = entry.get("name")
        if not isinstance(name, str) or not name:
            _fail(f"{path}.name: expected a non-empty string")
        if name in seen:
            _fail(f"{path}.name: duplicate compiler {name!r}")
        seen.add(name)
        veto = entry.get("veto")
        if status == NOT_AVAILABLE:
            if veto is not None or entry.get("measurements") is not None:
                _fail(f"{path}: an unavailable compiler records neither a veto nor a measurement")
            continue
        if not isinstance(veto, dict):
            _fail(f"{path}.veto: expected an object")
        gates = veto.get("gates")
        if not isinstance(gates, list) or not gates:
            _fail(f"{path}.veto.gates: expected a non-empty array")
        for gate_index, gate in enumerate(gates):
            if not isinstance(gate, dict):
                _fail(f"{path}.veto.gates[{gate_index}]: expected an object")
        observed = [gate.get("name") for gate in gates]
        if any(gate_name not in GATE_ORDER for gate_name in observed):
            _fail(f"{path}.veto.gates: every gate must be named from {GATE_ORDER}")
        if observed != list(GATE_ORDER[:len(observed)]):
            _fail(f"{path}.veto.gates: gates must appear in the fixed veto order")
        if status == VETOED:
            if veto.get("status") != GATE_FAIL or veto.get("failed_gate") != gates[-1]["name"]:
                _fail(f"{path}.veto: a vetoed compiler must name the gate that stopped the chain")
            if gates[-1].get("status") != GATE_FAIL:
                _fail(f"{path}.veto: the named failing gate is not reported FAIL")
            if entry.get("measurements") is not None:
                _fail(f"{path}.measurements: a vetoed compiler records no measurement")
            leaked = _timing_keys(entry)
            if leaked:
                _fail(f"{path}: a vetoed compiler carries timings at {', '.join(sorted(leaked))}")
            continue
        if veto.get("status") != GATE_PASS or veto.get("failed_gate") is not None:
            _fail(f"{path}.veto: a measured compiler records a passing veto with no failing gate")
        measurements = entry.get("measurements")
        if not isinstance(measurements, dict):
            _fail(f"{path}.measurements: expected an object once the veto passes")
        for key in ("build_seconds", "executable_bytes", "object_bytes", "workloads_status", "workloads"):
            if key not in measurements:
                _fail(f"{path}.measurements.{key}: missing required property")
        if status == MEASURED and measurements["workloads_status"] != GATE_PASS:
            _fail(f"{path}: a measured compiler must have a passing workload matrix")
        if status == WORKLOAD_FAILED and measurements["workloads_status"] != GATE_FAIL:
            _fail(f"{path}: a workload-failed compiler must name a failing workload matrix")


def summarize(report: dict[str, Any]) -> dict[str, list[str]]:
    summary: dict[str, list[str]] = {status: [] for status in STATUSES}
    for entry in report["compilers"]:
        summary[entry["status"]].append(entry["name"])
    return summary


def compare(compilers: list[str], *, make: str, python: str, output: Path) -> dict[str, Any]:
    if shutil.which(make) is None:
        _fail(f"make executable not found: {make}")
    records: list[dict[str, Any]] = []
    for name in compilers:
        path = shutil.which(name)
        if path is None:
            records.append(_unavailable(name))
            continue
        build_root = (output / name).resolve()
        env = os.environ.copy()
        env["CC"] = name
        # `cosim-mutants` re-enters `cosim-selftest` in a child process and finds
        # its make through MAKE; name it so a PATH fallback cannot pick another.
        env["MAKE"] = make
        env["PYTHON"] = python
        records.append(_measure_compiler(name, path, make, python, build_root, env=env))
    report = {
        "schema": SCHEMA,
        "issue": ISSUE,
        "public_source_owned": True,
        "compilers": records,
    }
    validate_report(report)
    report["summary"] = summarize(report)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare host C compilers on the public corpus behind a hard semantic veto")
    parser.add_argument("--compilers", default=",".join(DEFAULT_COMPILERS),
                        help="comma-separated compiler command names, resolved against PATH in order")
    parser.add_argument("--make", default=os.environ.get("MAKE", "mingw32-make"))
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "compiler-compare")
    args = parser.parse_args(argv)
    compilers = [name.strip() for name in args.compilers.split(",") if name.strip()]
    if not compilers:
        parser.error("--compilers must name at least one compiler")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    try:
        report = compare(compilers, make=args.make, python=args.python, output=output)
    except CompareError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    report_path = output / "compiler_compare.json"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(report, indent=2, sort_keys=True))
    summary = report["summary"]
    return 0 if summary[MEASURED] and not (summary[VETOED] or summary[WORKLOAD_FAILED]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
