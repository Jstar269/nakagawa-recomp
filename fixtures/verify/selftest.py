# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Drive the verify harness over the synthetic module and check every report (#1843).

``mingw32-make verify-selftest`` (``make CC=gcc verify-selftest`` on Linux) generates the
module with ``fixtures/verify/generate.py``, translates it, links ``src/rt/verify_harness.c``
and ``src/rt/verify.c`` against the generated C in a verify build (``-DSR_VERIFY``) and then
runs this script:

``run``              runs the harness under each sampling configuration and checks the
                     per-function report it writes: the power-of-two, first-k and every-n
                     samplers check exactly the expected call numbers, every comparable
                     function reports MATCH, a callee entered during a check, a form the
                     reference does not have, a yielding call and an exhausted rate budget
                     are SKIP with their reason, and a malformed setting stops the run.
``find-chunk``       prints the generated chunk that holds the ``alu`` body.
``mutant``           runs a harness whose generated C has one changed immediate (executed,
                     never build-only) and requires MISMATCH on exactly the register the
                     immediate reaches, a capture, and the same first difference when the
                     snapshot runner replays that capture.
``default-unchanged`` requires that the default build's preprocessed ``recomp.c`` has no
                     verify hook.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "snapshot_selftest", ROOT / "fixtures" / "snapshot" / "selftest.py")
assert _SPEC is not None and _SPEC.loader is not None
snapshot_selftest = importlib.util.module_from_spec(_SPEC)
sys.modules["snapshot_selftest"] = snapshot_selftest
_SPEC.loader.exec_module(snapshot_selftest)

Failure = snapshot_selftest.Failure
DATA = 0x09000000


@dataclass
class Row:
    addr: int
    calls: int
    checks: int
    match: int
    mismatch: int
    skip: int
    first: str
    skips: dict[str, int]
    checked_at: list[int]


@dataclass
class Report:
    header: dict[str, int]
    skips: dict[str, int]
    rows: dict[int, Row]


def parse_report(text: str) -> Report:
    header: dict[str, int] = {}
    skips: dict[str, int] = {}
    rows: dict[int, Row] = {}
    lines = text.splitlines()
    if not lines or lines[0] != "# nk-verify report v1":
        raise Failure(f"not a verify report: {lines[:1]}")
    for line in lines[1:]:
        if line.startswith("# functions_entered="):
            header = {k: int(v) for k, v in (kv.split("=") for kv in line[2:].split())}
        elif line.startswith("# skip "):
            _hash, _skip, reason, count = line.split()
            skips[reason] = int(count)
        elif line.startswith("#"):
            continue
        else:
            f = line.split("\t")
            if len(f) != 9:
                raise Failure(f"bad report row: {line!r}")
            by_reason = {}
            if f[7] != "-":
                for item in f[7].split(","):
                    name, _sep, n = item.rpartition(":")
                    by_reason[name] = int(n)
            at = [] if f[8] == "-" else [int(x) for x in f[8].split(",")]
            addr = int(f[0], 16)
            rows[addr] = Row(addr, int(f[1]), int(f[2]), int(f[3]), int(f[4]), int(f[5]),
                             f[6], by_reason, at)
    if not header:
        raise Failure("report has no totals line")
    return Report(header, skips, rows)


def run_harness(harness: Path, image: Path, base: int, work: Path, name: str,
                env: dict[str, str], *args: str) -> tuple[Report | None, subprocess.CompletedProcess]:
    report = work / f"{name}.report"
    report.unlink(missing_ok=True)
    full_env = {k: v for k, v in os.environ.items() if not k.startswith("SR_VERIFY")}
    full_env.update(env)
    full_env["SR_VERIFY_REPORT"] = str(report)
    command = [str(harness.resolve()), *args, str(image), f"{base:08x}"]
    done = subprocess.run(command, capture_output=True, text=True, timeout=300,
                          env=full_env, check=False)
    if not report.is_file():
        return None, done
    return parse_report(report.read_text(encoding="ascii")), done


def expect(cond: bool, message: str) -> None:
    if not cond:
        raise Failure(message)


def only_row(report: Report, addr: int, name: str) -> Row:
    expect(addr in report.rows, f"{name}: no row for 0x{addr:08x}: {sorted(report.rows)}")
    return report.rows[addr]


def run(args: argparse.Namespace) -> None:
    cases = json.loads(Path(args.cases).read_text(encoding="ascii"))
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    harness, image, base = Path(args.harness), Path(args.image), cases["base"]
    fn = cases["functions"]
    leaf, alu, ldst = fn["leaf"], fn["alu"], fn["ldst"]

    def go(name, env, *extra):
        report, done = run_harness(harness, image, base, work, name, env, *extra)
        if done.returncode != 0 or report is None:
            raise Failure(f"{name}: exit {done.returncode}\n{done.stderr}")
        return report

    # Samplers. The leaf is the only function in range, so the caller's own check does not
    # hold the one-check-in-flight slot while its callee runs.
    focus = {"SR_VERIFY_RANGE": f"{leaf:x}-{leaf:x}"}
    for name, env, want in (
        ("pow2-k2", {"SR_VERIFY": "2"}, [1, 2, 4, 8, 16, 32]),
        ("pow2-k4", {"SR_VERIFY": "4"}, [1, 2, 3, 4, 8, 16, 32]),
        ("every-10", {"SR_VERIFY": "1", "SR_VERIFY_EVERY": "10"}, [1, 10, 20, 30, 40]),
    ):
        report = go(name, {**focus, **env}, "--call", f"{fn['drive']:x}:1:28")
        row = only_row(report, leaf, name)
        expect(row.calls == 40, f"{name}: leaf calls {row.calls}")
        expect(row.checked_at == want, f"{name}: checked {row.checked_at}, want {want}")
        expect(row.checks == len(want) and row.match == len(want) and row.mismatch == 0 and row.skip == 0,
               f"{name}: {row}")
        expect(report.header["functions_entered"] == 1 and report.header["functions_checked"] == 1,
               f"{name}: {report.header}")
        print(f"PASS {name}: leaf checked at calls {','.join(map(str, want))} of 40")

    # Every comparable case reports MATCH; the others are SKIP with their reason.
    calls = []
    for name, spec in (("alu", ""), ("ldst", ""), ("call", ""),
                       ("fpu", ":3fc00000:40200000:09000000"), ("syscall", ":10:20"),
                       ("import", ""), ("vfpu", ""), ("drive", ":3"), ("spin", ":5")):
        calls += ["--call", f"{fn[name]:x}:1{spec}"]
    report = go("cases", {"SR_VERIFY": "1"}, *calls)
    for name in ("alu", "ldst", "call", "fpu", "syscall", "import", "drive", "spin"):
        row = only_row(report, fn[name], name)
        expect(row.calls == 1 and row.checks == 1 and row.match == 1 and row.mismatch == 0
               and row.skip == 0 and row.checked_at == [1], f"{name}: {row}")
        print(f"PASS case-{name}: calls=1 checks=1 MATCH")
    row = only_row(report, fn["vfpu"], "vfpu")
    expect(row.checks == 0 and row.skips == {"ref-no-vfpu": 1}, f"vfpu: {row}")
    print("PASS case-vfpu: SKIP ref-no-vfpu, not a match")
    row = only_row(report, leaf, "leaf")
    expect(row.calls == 4 and row.checks == 0 and row.skips == {"busy": 3},
           f"leaf entered inside a check: {row}")
    print("PASS case-leaf: calls entered while a check was in flight are SKIP busy")
    expect(report.header["mismatch"] == 0 and report.header["match"] == 8
           and report.skips == {"busy": 4, "ref-no-vfpu": 1}, f"totals: {report}")  # busy: 3 leaf + the import stub
    print("PASS totals: 8 match, 0 mismatch, skips counted by reason (the import stub is the 4th busy)")

    # A call whose slice expires inside it is preempted, not a match.
    report = go("preempted", {"SR_VERIFY": "1"}, "--sched-slice", "1", "--call", f"{fn['spin']:x}:1:5")
    row = only_row(report, fn["spin"], "preempted")
    expect(row.checks == 0 and row.match == 0 and row.skips == {"preempted": 1}, f"spin: {row}")
    print("PASS preempted: a call that yields is SKIP preempted")

    # The rate budget: one check per second, burst one, clock frozen. The first call of
    # each function may overdraw the budget only once the bucket is not already empty.
    report = go("rate", {"SR_VERIFY": "100", "SR_VERIFY_RATE": "1"}, "--frozen-clock",
                "--call", f"{alu:x}:5", "--call", f"{ldst:x}:5",
                "--call", f"{fn['fpu']:x}:5:3fc00000:40200000:09000000")
    for name, addr, checks in (("alu", alu, 1), ("ldst", ldst, 1), ("fpu", fn["fpu"], 0)):
        row = only_row(report, addr, name)
        expect(row.checks == checks and row.skips == {"rate": 5 - checks}, f"rate {name}: {row}")
    print("PASS rate: budget spent -> SKIP rate; a function's first check may overdraw once")

    report = go("range", {"SR_VERIFY": "1", "SR_VERIFY_RANGE": f"{alu:x}-{alu:x}"},
                "--call", f"{alu:x}:3", "--call", f"{ldst:x}:3")
    expect(list(report.rows) == [alu] and report.header["functions_entered"] == 1,
           f"range: {sorted(report.rows)}")
    print("PASS range: only functions inside SR_VERIFY_RANGE are entered")

    # Disabled: no setting, no checks, an empty report.
    report = go("off", {}, "--call", f"{alu:x}:3")
    expect(not report.rows and report.header["checks"] == 0, f"off: {report}")
    print("PASS off: without SR_VERIFY nothing is checked")

    for bad in ({"SR_VERIFY": "abc"}, {"SR_VERIFY": "1", "SR_VERIFY_RANGE": "10"},
                {"SR_VERIFY": "1", "SR_VERIFY_RATE": "0"}, {"SR_VERIFY_EVERY": "0"}):
        # The harness reads the environment itself, before any call.
        _report, done = run_harness(harness, image, base, work, "bad", bad, "--call", f"{alu:x}:1")
        expect(done.returncode == 2 and "malformed" in done.stderr, f"{bad}: {done.returncode} {done.stderr}")
    print("PASS malformed: a malformed setting stops the run with status 2")


def find_chunk(args: argparse.Namespace) -> None:
    snapshot_selftest.find_chunk(args)


def mutant(args: argparse.Namespace) -> None:
    cases = json.loads(Path(args.cases).read_text(encoding="ascii"))
    work = Path(args.work)
    captures = work / "captures"
    captures.mkdir(parents=True, exist_ok=True)
    for old in captures.glob("*"):
        old.unlink()
    alu = cases["functions"]["alu"]
    report, done = run_harness(
        Path(args.harness), Path(args.image), cases["base"], work, "mutant",
        {"SR_VERIFY": "1", "SR_VERIFY_CAPTURE": str(captures)}, "--call", f"{alu:x}:1")
    if report is None or done.returncode != 0:
        raise Failure(f"mutant harness: exit {done.returncode}\n{done.stderr}")
    row = only_row(report, alu, "mutant")
    register = cases["alu_ori_register"]
    a0 = DATA
    want = (f"r{register} aot=0x{a0 | snapshot_selftest.MUTANT_IMMEDIATE:08x} "
            f"ref=0x{a0 | cases['alu_ori_immediate']:08x}")
    expect(row.checks == 1 and row.mismatch == 1 and row.match == 0 and row.first == want,
           f"mutant: {row}, want first_mismatch {want!r}")
    expect(report.header["mismatch"] == 1, f"mutant totals: {report.header}")
    print(f"PASS mutant-mismatch: MISMATCH {row.first}")
    snaps = sorted(captures.glob("*.nksnap"))
    expect(len(snaps) == 1, f"expected one capture, found {snaps}")
    line, stderr, code = snapshot_selftest._run(
        Path(args.replay), Path(args.image), cases["base"], snaps[0])
    expect(line == f"MISMATCH {row.first}" and code == 1,
           f"replay of {snaps[0].name}: {line!r} (exit {code}), want 'MISMATCH {row.first}'\n{stderr}")
    print(f"PASS mutant-replay: the snapshot runner replays the capture to {line!r}")


def default_unchanged(args: argparse.Namespace) -> None:
    text = Path(args.preprocessed).read_text(encoding="utf-8", errors="replace")
    hits = sorted(set(re.findall(r"\bsr_verify\w*|\bSR_VERIFY\w*", text)))
    expect(not hits, f"the default build's recomp.c carries verify tokens: {hits}")
    print("PASS default-unchanged: the default recomp.c has no verify hook")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("run")
    p.add_argument("--harness", required=True)
    p.add_argument("--image", required=True)
    p.add_argument("--cases", required=True)
    p.add_argument("--work", required=True)
    p = sub.add_parser("find-chunk")
    p.add_argument("--build-dir", required=True)
    p.add_argument("--game", required=True)
    p.add_argument("--cases", required=True)
    p = sub.add_parser("mutant")
    p.add_argument("--harness", required=True)
    p.add_argument("--replay", required=True)
    p.add_argument("--image", required=True)
    p.add_argument("--cases", required=True)
    p.add_argument("--work", required=True)
    p = sub.add_parser("default-unchanged")
    p.add_argument("preprocessed")
    args = parser.parse_args(argv)
    try:
        {"run": run, "find-chunk": find_chunk, "mutant": mutant,
         "default-unchanged": default_unchanged}[args.command](args)
    except Failure as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
