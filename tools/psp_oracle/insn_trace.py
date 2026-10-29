# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Real-PSP instruction-trace differential (#312).

The fixtures in ``fixtures/psp_oracle/insn_trace_fixtures.h`` run one instruction at a
time on a PSP (``CASE=insn-trace``, ``probe_insn_trace.c``) and through Nakagawa's own
code generator. This module turns both into v2 traces (``tools/TRACE_FORMAT.md``) and
compares them with ``tracediff.strict_hardware_diff``, which names the first record that
differs.

    insn_trace.py psp <host0-log> <out-dir> --model M --firmware F
        one PSP_HARDWARE trace per fixture from the probe's host0 log
    insn_trace.py local <out-dir> --source-commit C --model M --firmware F [--fixture ID]
        one LOCAL_COSIM trace per fixture: tools/codegen.py on the fixture's words, the
        headless traced driver of tools/codegen_gate.py, the fixture's initial state
    insn_trace.py compare <psp.trace> <local.trace>
        exit 0 on STRICT_V2_AGREEMENT, 1 on the first divergence, 2 on a rejected input

Both sides record, per executed instruction (delay slots included, an annulled likely
delay slot not), the traced registers that changed (r1..r25, r29..r31, hi, lo) and the
store the instruction made. r26..r28 are never traced: the probe's trampoline owns them
and no fixture names them.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent
FIXTURES_H = ROOT / "fixtures" / "psp_oracle" / "insn_trace_fixtures.h"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import tracediff  # noqa: E402

FIXTURE_ID = "insn-trace"
TEST_ID = "PSP-TRACE-001"
EXIT_SYSCALL_WORD = 0x0008430C  # syscall 0x210c: the gate build's stop (tools/gate_stub.c)
TRACED_GPRS = tuple(range(1, 26)) + (29, 30, 31)
RECORD_WORDS = 2 + len(TRACED_GPRS) + 2 + 3   # pc, op, registers, hi, lo, store kind/addr/value
END_REACHED = 0


class InsnTraceError(ValueError):
    """An input does not satisfy the instruction-trace contract."""


def parse_fixtures(path: Path = FIXTURES_H) -> dict:
    """Read the fixtures header: constants, word arrays and the fixture table."""
    text = path.read_text(encoding="utf-8")
    consts = {name: int(value, 16) for name, value in
              re.findall(r"#define (NT_[A-Z_]+) (0x[0-9a-fA-F]+)u", text)}
    arrays = {}
    for name, body in re.findall(r"static const uint32_t (nt_\w+)_words\[\] = \{(.*?)\};", text, re.S):
        arrays[name] = [(int(word, 16), asm.strip()) for word, asm in
                        re.findall(r"(0x[0-9a-fA-F]{8})u,\s*/\*\s*(.*?)\s*\*/", body)]
    fixtures = []
    table = text[text.index("static const NtFixture nt_fixtures"):]
    for fid, base, count, array in re.findall(
            r'\{"([\w-]+)", (0x[0-9a-fA-F]+)u, (\d+)u, (nt_\w+)_words\}', table):
        words = arrays[array]
        if len(words) != int(count):
            raise InsnTraceError(f"{fid}: table count {count} != {len(words)} words")
        fixtures.append({"id": fid, "base": int(base, 16), "words": [w for w, _ in words],
                         "asm": [a for _, a in words]})
    declared = int(re.search(r"#define NT_FIXTURE_COUNT (\d+)u", text).group(1))
    if declared != len(fixtures):
        raise InsnTraceError(f"NT_FIXTURE_COUNT {declared} != {len(fixtures)} table rows")
    return {"consts": consts, "fixtures": fixtures}


def initial_state(consts: dict) -> dict:
    """The register state every fixture starts from (the header's contract)."""
    regs = {f"r{i}": (0x01010101 * i) & 0xFFFFFFFF for i in range(1, 32)}
    regs["r4"] = consts["NT_DATA_BASE"]
    regs["r29"] = consts["NT_DATA_BASE"] + 0x800
    for i in (26, 27, 28):
        regs[f"r{i}"] = 0
    regs["hi"] = consts["NT_HI_INIT"]
    regs["lo"] = consts["NT_LO_INIT"]
    return regs


def image_sha256(fixture: dict) -> str:
    """binary_sha256 of a fixture: its words, little-endian (the executed guest image)."""
    return hashlib.sha256(b"".join(struct.pack("<I", w) for w in fixture["words"])).hexdigest()


def words_fnv(words) -> int:
    """FNV-1a over the words' little-endian bytes, as the probe reports it."""
    h = 0x811C9DC5
    for word in words:
        for byte in struct.pack("<I", word):
            h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h


def fixture_elf(fixture: dict) -> bytes:
    """A fixed-address ELF32 image of the fixture's words followed by the exit syscall."""
    text = b"".join(struct.pack("<I", w) for w in fixture["words"] + [EXIT_SYSCALL_WORD])
    base = fixture["base"]
    names = b"\0.text\0.shstrtab\0"
    image_off = 0x60
    names_off = image_off + len(text)
    shoff = (names_off + len(names) + 3) & ~3
    sections = [(0, 0, 0, 0, 0, 0), (1, 1, 0x6, base, image_off, len(text)),
                (7, 3, 0, 0, names_off, len(names))]
    out = bytearray(shoff + 40 * len(sections))
    out[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", out, 16, 2, 8, 1, base, 52, shoff, 0,
                     52, 32, 1, 40, len(sections), len(sections) - 1)
    struct.pack_into("<8I", out, 52, 1, image_off, base, base, len(text), len(text), 5, 4)
    out[image_off:image_off + len(text)] = text
    out[names_off:names_off + len(names)] = names
    for index, (name, typ, flags, addr, off, size) in enumerate(sections):
        struct.pack_into("<10I", out, shoff + 40 * index, name, typ, flags, addr, off, size,
                         0, 0, 4 if typ else 0, 0)
    return bytes(out)


def v2_header(source_tier: str, fixture: dict, steps: int, *, source_commit: str,
              model: str, firmware: str) -> str:
    return (f"# psp-recomp trace v2 source_tier={source_tier} fixture_id={FIXTURE_ID} "
            f"cell_id={fixture['id']} binary_sha256={image_sha256(fixture)} "
            f"source_commit={source_commit} model={model} firmware={firmware} "
            f"start_pc=0x{fixture['base']:08x} steps={steps} complete=1")


def _state_tokens(before: dict, after: dict) -> list[str]:
    names = [f"r{i}" for i in TRACED_GPRS] + ["hi", "lo"]
    return [f"{name}=0x{after[name]:08x}" for name in names if after[name] != before[name]]


def psp_records(log_text: str, fixture: dict, consts: dict) -> tuple[list[str], str]:
    """Step lines for one fixture from the probe log, and the log's source commit."""
    meta = re.search(r"^NAKAGAWA_PSP_META .*source_commit=([0-9a-f]{40})", log_text, re.M)
    if not meta:
        raise InsnTraceError("the log has no NAKAGAWA_PSP_META line with a source commit")
    fid = fixture["id"]
    records = {}
    for line in log_text.splitlines():
        m = re.match(rf"NAKAGAWA_PSP_TEST schema=1 test_id={TEST_ID} case_id={re.escape(fid)}-(\S+) "
                     r"status=(\S+) result=0x([0-9a-f]{8})((?: out\d+=0x[0-9a-f]{8})*)$", line)
        if m:
            records[m.group(1)] = (m.group(2), int(m.group(3), 16),
                                   [int(v, 16) for v in re.findall(r"=0x([0-9a-f]{8})", m.group(4))])
    if "init" not in records or "end" not in records:
        raise InsnTraceError(f"{fid}: the log has no init or end record")
    init = records["init"][2]
    if len(init) != 4 + len(TRACED_GPRS) + 2:
        raise InsnTraceError(f"{fid}: the init record is malformed")
    if init[0] != fixture["base"] or init[1] != len(fixture["words"]):
        raise InsnTraceError(f"{fid}: the probe ran a different fixture (base/count)")
    if init[2] != words_fnv(fixture["words"]):
        raise InsnTraceError(f"{fid}: the probe's words differ from the header (FNV)")
    status, result, end = records["end"]
    if status != "PASS" or result != END_REACHED:
        raise InsnTraceError(f"{fid}: the probe did not reach the end address (status {result})")
    steps = end[0]
    names = [f"r{i}" for i in TRACED_GPRS] + ["hi", "lo"]
    state = dict(zip(names, init[4:], strict=True))
    expected = initial_state(consts)
    if any(state[n] != expected[n] for n in names) or init[3] != consts["NT_DATA_BASE"]:
        raise InsnTraceError(f"{fid}: the probe's initial state differs from the header contract")
    lines = []
    for step in range(steps):
        key = f"s{step:04d}"
        if key not in records or len(records[key][2]) != RECORD_WORDS:
            raise InsnTraceError(f"{fid}: step record {key} is missing or malformed")
        out = records[key][2]
        after = dict(zip(names, out[2:2 + len(names)], strict=True))
        tokens = _state_tokens(state, after)
        kind, addr, value = out[-3:]
        if kind:
            width = {8: 2, 16: 4, 32: 8}[kind]
            tokens.append(f"m{kind}[0x{addr:08x}]=0x{value:0{width}x}")
        lines.append(f"{step} pc=0x{out[0]:08x} op=0x{out[1]:08x}" + "".join(f" {t}" for t in tokens))
        state = after
    return lines, meta.group(1)


def local_records(fixture: dict, consts: dict, workdir: Path, mutate=None) -> list[str]:
    """Step lines for one fixture from Nakagawa's generated code (the traced gate driver)."""
    import codegen_gate  # noqa: PLC0415 - tools/ is on sys.path above

    workdir.mkdir(parents=True, exist_ok=True)
    elf = workdir / f"{fixture['id']}.elf"
    elf.write_bytes(fixture_elf(fixture))
    init = workdir / "init.trace"
    state = initial_state(consts)
    init.write_text("# init " + " ".join(f"{k}=0x{v:08x}" for k, v in state.items()) + "\n",
                    encoding="utf-8", newline="\n")
    env = codegen_gate.gate_env()
    drv = codegen_gate.build_traced_driver(str(elf), str(workdir), env, mutate=mutate)
    if drv is None:
        raise InsnTraceError(f"{fixture['id']}: the traced driver did not build")
    out = workdir / "local.trace"
    # The exit syscall at the end address is the only way the gate build's sr_syscall is
    # reached (tools/gate_stub.c); its SR_PPM_DUMP side file is the proof that the
    # generated code ran to the end address rather than stopping anywhere else.
    marker = workdir / "exit-reached.ppm"
    env = dict(env, SR_PPM_DUMP=str(marker))
    proc = subprocess.run([drv, str(elf), str(init), str(out)], capture_output=True, text=True,
                          env=env, check=False)
    if proc.returncode != 0 or not out.exists():
        raise InsnTraceError(f"{fixture['id']}: the traced driver failed ({proc.returncode})")
    if not marker.exists():
        raise InsnTraceError(f"{fixture['id']}: the generated code did not reach the end address")
    end = fixture["base"] + 4 * len(fixture["words"])
    lines = []
    for raw in out.read_text(encoding="utf-8").splitlines():
        if not raw or raw.startswith("#"):
            continue
        parts = raw.split()
        if int(parts[1][3:], 16) == end:
            break
        kept = [p for p in parts[3:] if not re.fullmatch(r"r2[678]=.*", p)]
        lines.append(" ".join([str(len(lines))] + parts[1:3] + kept))
    return lines


def write_trace(path: Path, header: str, lines: list[str]) -> None:
    path.write_text(header + "\n" + "".join(line + "\n" for line in lines), encoding="utf-8",
                    newline="\n")


def compare(psp_trace: str, local_trace: str) -> int:
    try:
        divergence = tracediff.strict_hardware_diff(psp_trace, local_trace)
    except (tracediff.HardwareTraceError, OSError, UnicodeDecodeError) as exc:
        print(f"REJECTED: {exc}")
        return 2
    if divergence is None:
        print("STRICT_V2_AGREEMENT: PSP_HARDWARE == LOCAL_COSIM")
        return 0
    step, pc, detail = divergence
    print(f"FIRST_DIVERGENCE: step {step}, pc {pc}: {detail}")
    return 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    psp = sub.add_parser("psp")
    psp.add_argument("log")
    psp.add_argument("out")
    local = sub.add_parser("local")
    local.add_argument("out")
    local.add_argument("--source-commit", required=True)
    local.add_argument("--fixture", action="append")
    for p in (psp, local):
        p.add_argument("--model", required=True)
        p.add_argument("--firmware", required=True)
    cmp_ = sub.add_parser("compare")
    cmp_.add_argument("psp")
    cmp_.add_argument("local")
    args = parser.parse_args(argv)
    if args.command == "compare":
        return compare(args.psp, args.local)
    data = parse_fixtures()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    try:
        if args.command == "psp":
            text = Path(args.log).read_text(encoding="utf-8")
            for fixture in data["fixtures"]:
                lines, commit = psp_records(text, fixture, data["consts"])
                header = v2_header("PSP_HARDWARE", fixture, len(lines), source_commit=commit,
                                   model=args.model, firmware=args.firmware)
                write_trace(out / f"{fixture['id']}.psp.trace", header, lines)
                print(f"{fixture['id']}: {len(lines)} PSP_HARDWARE records")
            return 0
        for fixture in data["fixtures"]:
            if args.fixture and fixture["id"] not in args.fixture:
                continue
            with tempfile.TemporaryDirectory(dir=os.environ.get("NK_INSN_TRACE_TMP")) as tmp:
                lines = local_records(fixture, data["consts"], Path(tmp))
            header = v2_header("LOCAL_COSIM", fixture, len(lines), source_commit=args.source_commit,
                               model=args.model, firmware=args.firmware)
            write_trace(out / f"{fixture['id']}.local.trace", header, lines)
            print(f"{fixture['id']}: {len(lines)} LOCAL_COSIM records")
        return 0
    except InsnTraceError as exc:
        print(f"REJECTED: {exc}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
