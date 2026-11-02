# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Drive the snapshot runner over the synthetic module and check every verdict (#1842).

``mingw32-make snapshot-selftest`` (``make CC=gcc snapshot-selftest`` on Linux) generates
the module with ``fixtures/snapshot/generate.py``, translates it, links
``src/rt/snapshot_runner.c`` against the generated C and then runs this script:

``run``       writes one snapshot per case (``tools/nksnap.py``), runs the runner on each
              and requires the exact verdict: MATCH for the comparable cases, the named
              NOT_CHECKED and MISMATCH lines for the negative ones, and REFUSED with the
              named reason for each malformed snapshot file.
``find-chunk`` prints the generated chunk that holds the ``alu`` case's body.
``mutate``    copies that chunk with the ``ori`` immediate changed, for a mutant runner.
``mutant``    runs the mutant runner on the ``alu`` snapshot and requires
              ``MISMATCH r11`` with the exact values the changed immediate produces: a
              semantic kill, built and executed, never a build-only failure.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import nksnap  # noqa: E402  (tools/ is the import root for tool modules)

DATA = 0x09000000
STACK_TOP = 0x09100000
STACK_BLOCK = 0x100
#: The immediate the mutant writes in place of generate.ALU_ORI_IMMEDIATE.
MUTANT_IMMEDIATE = 0x5A1F

ALU_REGS = {4: 0x13572468, 5: 0x0BADF00D, 6: 0x7FFF0001}


class Failure(Exception):
    pass


def _data_block(size: int = 0x40) -> nksnap.Block:
    return nksnap.Block(DATA, bytes((i * 37 + 11) & 0xFF for i in range(size)))


def _stack_block() -> nksnap.Block:
    return nksnap.Block(STACK_TOP - STACK_BLOCK, bytes(STACK_BLOCK))


def _snapshot(entry: int, regs: dict[int, int], blocks: list[nksnap.Block]) -> nksnap.Snapshot:
    return nksnap.Snapshot(nksnap.cpu_state(r=regs), entry, blocks)


def _run(runner: Path, image: Path, base: int, snap: Path, *extra: str) -> tuple[str, str, int]:
    command = [str(runner.resolve()), *extra, str(image), f"{base:08x}", str(snap)]
    done = subprocess.run(command, capture_output=True, text=True, timeout=120, check=False)
    lines = [line for line in done.stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise Failure(f"{snap.name}: expected one verdict line, got {done.stdout!r} "
                      f"(stderr {done.stderr.strip()!r})")
    return lines[0], done.stderr, done.returncode


EXIT_CODES = {"MATCH": 0, "MISMATCH": 1, "REFUSED": 2, "NOT_CHECKED": 3}


def _check(name: str, got: tuple[str, str, int], want: str,
           stops: tuple[str, str] | None = None) -> None:
    line, stderr, code = got
    verdict = nksnap.parse_verdict(line)
    if line != want:
        raise Failure(f"{name}: got {line!r}, want {want!r}\n{stderr}")
    if code != EXIT_CODES[verdict.kind]:
        raise Failure(f"{name}: exit {code} does not match verdict {verdict.kind}")
    if stops is not None:
        aot = re.search(r"^aot: stop=(\w+)", stderr, re.M)
        ref = re.search(r"^reference: stop=(\w+)", stderr, re.M)
        seen = (aot.group(1) if aot else None, ref.group(1) if ref else None)
        if seen != stops:
            raise Failure(f"{name}: stops {seen}, want {stops}\n{stderr}")
    print(f"PASS {name}: {line}")


def case_snapshots(cases: dict) -> dict[str, tuple[nksnap.Snapshot, tuple[str, ...]]]:
    fn = cases["functions"]
    stub, nid = cases["import_stub"], cases["import_nid"]
    import_arg = ("--import", f"{stub:08x}:{nid:08x}")
    stack = {29: STACK_TOP}
    return {
        "alu": (_snapshot(fn["alu"], ALU_REGS, [_data_block()]), ()),
        "ldst": (_snapshot(fn["ldst"], {4: DATA}, [_data_block()]), ()),
        "call": (_snapshot(fn["call"], {4: 0x00000123, 16: 0x5EED0016, **stack},
                           [_stack_block()]), ()),
        "fpu": (_snapshot(fn["fpu"], {4: 0x3FC00000, 5: 0x40200000, 6: DATA},
                          [_data_block()]), ()),
        "syscall": (_snapshot(fn["syscall"], {4: 0x10, 5: 0x20}, [_data_block()]), ()),
        "import": (_snapshot(fn["import"], dict(stack), [_stack_block()]), import_arg),
        "vfpu": (_snapshot(fn["vfpu"], {}, [_data_block()]), ()),
    }


def run(args: argparse.Namespace) -> None:
    cases = json.loads(Path(args.cases).read_text(encoding="ascii"))
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    runner, image, base = Path(args.runner), Path(args.image), cases["base"]
    fn = cases["functions"]
    stub, nid = cases["import_stub"], cases["import_nid"]
    snaps = case_snapshots(cases)
    paths = {}
    for name, (snap, _extra) in snaps.items():
        paths[name] = work / f"{name}.nksnap"
        nksnap.write(paths[name], snap)

    def go(name: str, *extra: str) -> tuple[str, str, int]:
        return _run(runner, image, base, paths[name], *extra)

    # Comparable cases: each must MATCH, and the stop cases must stop where intended.
    for name in ("alu", "ldst", "call", "fpu"):
        _check(name, go(name, *snaps[name][1]), "MATCH", ("return", "return"))
    _check("syscall", go("syscall"), "MATCH", ("syscall", "syscall"))
    _check("import", go("import", *snaps["import"][1]), "MATCH", ("import", "import"))

    # The reference has no VFPU: NOT_CHECKED names the instruction, never a pass.
    _check("vfpu", go("vfpu"),
           f"NOT_CHECKED ref-no-vfpu pc=0x{cases['vfpu_pc']:08x} op=0x{cases['vfpu_op']:08x}")

    # Stop kinds that disagree are a MISMATCH on the stop, before any register.
    _check("import-unnamed", go("import"), "MISMATCH stop aot=import ref=syscall")
    _check("leaf-as-import", go("call", "--import", f"{fn['leaf']:08x}:12345678"),
           "MISMATCH stop aot=return ref=import")
    _check("import-nid", go("import", "--import", f"{stub:08x}:deadbeef"),
           f"MISMATCH import_nid aot=0x{nid:08x} ref=0xdeadbeef")
    _check("step-limit", go("call", "--max-steps", "3"), "NOT_CHECKED ref-step-limit steps=3")

    # Stores outside the snapshot's blocks are reported, never ignored.
    narrow = work / "ldst-narrow.nksnap"
    nksnap.write(narrow, _snapshot(fn["ldst"], {4: DATA}, [_data_block(12)]))
    _check("ldst-narrow", _run(runner, image, base, narrow),
           f"NOT_CHECKED write-outside-snapshot addr=0x{DATA + 16:08x} side=both")
    scratch = work / "scratchpad.nksnap"
    nksnap.write(scratch, _snapshot(fn["alu"], ALU_REGS,
                                    [nksnap.Block(0x00010000, bytes(16))]))
    _check("scratchpad-block", _run(runner, image, base, scratch),
           "NOT_CHECKED block-outside-ref-ram addr=0x00010000 size=0x10")

    # The C writer reproduces the file the Python writer made, byte for byte.
    copy = work / "alu-rewritten.nksnap"
    _check("rewrite", go("alu", "--rewrite", str(copy)), "MATCH")
    if copy.read_bytes() != paths["alu"].read_bytes():
        raise Failure("the C writer's copy of alu.nksnap differs from the original")
    print("PASS rewrite-roundtrip: the C writer's copy is byte-identical")

    # Every refusal reason, through the C loader.
    for reason, data in malformed_snapshots(fn["alu"]).items():
        path = work / f"malformed-{reason}.nksnap"
        path.write_bytes(data)
        line, stderr, code = _run(runner, image, base, path)
        verdict = nksnap.parse_verdict(line)
        if verdict.kind != "REFUSED" or verdict.reason != reason or code != 2:
            raise Failure(f"malformed {reason}: got {line!r} (exit {code})\n{stderr}")
        try:
            nksnap.parse(data)
        except nksnap.SnapshotError as error:
            if error.reason != reason:
                raise Failure(f"malformed {reason}: tools/nksnap.py says {error.reason}") from error
        else:
            raise Failure(f"malformed {reason}: tools/nksnap.py accepted it")
        print(f"PASS malformed-{reason}: {line}")


def malformed_snapshots(entry: int) -> dict[str, bytes]:
    cpu = nksnap.cpu_state(r=ALU_REGS)
    good = [(DATA, 4, b"\x01\x02\x03\x04")]
    valid = nksnap.pack_raw(cpu, entry, good)
    return {
        "bad-magic": nksnap.pack_raw(cpu, entry, good, magic=b"NKSNAP02"),
        "abi-version": nksnap.pack_raw(cpu, entry, good, abi_version=nksnap.ABI_VERSION + 1),
        "cpustate-size": nksnap.pack_raw(cpu[:-4], entry, good),
        "truncated": valid[:-1],
        "block-size": nksnap.pack_raw(cpu, entry, [(DATA, 0, b"")]),
        "block-overlap": nksnap.pack_raw(cpu, entry, [(DATA, 8, bytes(8)), (DATA + 4, 4, bytes(4))]),
        "block-outside-guest": nksnap.pack_raw(cpu, entry, [(0x0C000000, 4, bytes(4))]),
        "trailing-bytes": valid + b"\0",
    }


def _alu_symbol_pattern(cases: dict) -> re.Pattern[str]:
    address = cases["functions"]["alu"]
    return re.compile(rf"^void \w*{address:08x}\w*\(CpuState \*s\)\s*\{{", re.M | re.I)


def find_chunk(args: argparse.Namespace) -> None:
    cases = json.loads(Path(args.cases).read_text(encoding="ascii"))
    pattern = _alu_symbol_pattern(cases)
    found = [p for p in sorted(Path(args.build_dir).glob(f"{args.game}_recomp_*.c"))
             if pattern.search(p.read_text(encoding="utf-8", errors="replace"))]
    if len(found) != 1:
        raise Failure(f"expected one chunk defining the alu body, found {found}")
    print(found[0].as_posix())


def _alu_body(text: str, cases: dict) -> tuple[int, int]:
    start = _alu_symbol_pattern(cases).search(text)
    if start is None:
        raise Failure("the alu body is not in this chunk")
    end = text.find("\n}\n", start.end())
    if end < 0:
        raise Failure("the alu body has no end")
    return start.end(), end


def mutate(args: argparse.Namespace) -> None:
    cases = json.loads(Path(args.cases).read_text(encoding="ascii"))
    text = Path(args.chunk).read_text(encoding="utf-8")
    lo, hi = _alu_body(text, cases)
    body = text[lo:hi]
    immediate = cases["alu_ori_immediate"]
    literal = re.compile(rf"\b0x0*{immediate:x}u?\b", re.I)
    hits = literal.findall(body)
    if len(hits) != 1:
        raise Failure(f"expected one 0x{immediate:x} literal in the alu body, found {hits}")
    mutated = literal.sub(f"0x{MUTANT_IMMEDIATE:x}u", body)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text[:lo] + mutated + text[hi:], encoding="utf-8")
    print(f"mutant: {hits[0]} -> 0x{MUTANT_IMMEDIATE:x}u in the alu body, wrote {out}")


def mutant(args: argparse.Namespace) -> None:
    cases = json.loads(Path(args.cases).read_text(encoding="ascii"))
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    snap = work / "alu-mutant.nksnap"
    nksnap.write(snap, case_snapshots(cases)["alu"][0])
    register = cases["alu_ori_register"]
    a0 = ALU_REGS[4]
    want = (f"MISMATCH r{register} aot=0x{a0 | MUTANT_IMMEDIATE:08x} "
            f"ref=0x{a0 | cases['alu_ori_immediate']:08x}")
    _check("mutant-alu-ori", _run(Path(args.runner), Path(args.image), cases["base"], snap), want)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("run")
    p.add_argument("--runner", required=True)
    p.add_argument("--image", required=True)
    p.add_argument("--cases", required=True)
    p.add_argument("--work", required=True)
    p = sub.add_parser("find-chunk")
    p.add_argument("--build-dir", required=True)
    p.add_argument("--game", required=True)
    p.add_argument("--cases", required=True)
    p = sub.add_parser("mutate")
    p.add_argument("--chunk", required=True)
    p.add_argument("--cases", required=True)
    p.add_argument("--out", required=True)
    p = sub.add_parser("mutant")
    p.add_argument("--runner", required=True)
    p.add_argument("--image", required=True)
    p.add_argument("--cases", required=True)
    p.add_argument("--work", required=True)
    args = parser.parse_args(argv)
    try:
        {"run": run, "find-chunk": find_chunk, "mutate": mutate, "mutant": mutant}[args.command](args)
    except Failure as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
