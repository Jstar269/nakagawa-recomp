# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Generate the source-owned guest module for the snapshot-execution selftest (#1842).

The committed fixture is this recipe, not a binary. It deterministically writes a small
PSP PRX (``guest.prx``), its ``~PSP`` header (``guest.psp``) and ``cases.json`` (each
case's entry address and the import stub) into the ignored build tree. The ordinary
pipeline (``tools/prxload.py`` -> ``tools/codegen.py``) then translates it, and
``src/rt/snapshot_runner.c`` is linked against the generated C. Names, addresses,
instructions and values are project-authored test values; nothing here contains or derives
from a retail title.

Each case is one guest function that the selftest starts from a snapshot:

  alu      integer ALU, shifts, HI/LO (``mult``), a branch-free compare and an ``ori``
           whose immediate the generated-C mutant changes
  ldst     word/half/byte loads and stores against a snapshot data block
  call     a nested ``jal`` to ``leaf`` with a stack frame in a snapshot stack block
  fpu      ``mtc1``/``add.s``/``mul.s``/``sub.s``/``mfc1``/``swc1``
  syscall  stops at a raw ``syscall`` instruction
  import   calls the module's one import stub, so both sides stop at the import
  vfpu     an ``mtv``, a form the reference interpreter does not have

The module carries one import (library ``SysMemUserForUser``, the same synthetic import
``fixtures/production_smoke`` uses) so the generated code has a real import stub.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Disjoint from fixtures/production_smoke (0x08804000) and fixtures/cosim (0x08900000).
BASE = 0x08A00000
DATA_SEGMENT_OFFSET = 0x1000
TEXT_FILE_OFFSET = 0x100
DATA_FILE_OFFSET = TEXT_FILE_OFFSET + DATA_SEGMENT_OFFSET
DATA_FILE_SIZE = 0x68
DATA_MEMORY_SIZE = 0x70
BSS_SIZE = DATA_MEMORY_SIZE - DATA_FILE_SIZE

LIBRARY = "SysMemUserForUser"
NID = 0x7591C7DB
SYSCALL_CODE = 0x123

SHT_PRX_RELOC = 0x700000A0
R_MIPS_32 = 2
R_MIPS_26 = 4

ZERO, V0, V1, A0, A1, A2 = 0, 2, 3, 4, 5, 6
T0, T1, T2, T3, S0, SP, RA = 8, 9, 10, 11, 16, 29, 31
NOP = 0

#: The ``ori`` immediate in ``alu``; the mutant rewrites it in the generated C.
ALU_ORI_IMMEDIATE = 0x5A0F
#: The register that ``ori`` writes and nothing else reads, so a changed immediate
#: shows up as a MISMATCH on exactly this register.
ALU_ORI_REGISTER = T3


def _r(rs: int, rt: int, rd: int, shift: int, function: int) -> int:
    return (rs << 21) | (rt << 16) | (rd << 11) | (shift << 6) | function


def _i(opcode: int, rs: int, rt: int, immediate: int) -> int:
    return (opcode << 26) | (rs << 21) | (rt << 16) | (immediate & 0xFFFF)


def _j(opcode: int, segment_offset: int) -> int:
    return (opcode << 26) | ((segment_offset >> 2) & 0x03FFFFFF)


def _cop1(fmt: int, ft: int, fs: int, fd: int, function: int) -> int:
    return (0x11 << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | function


JR_RA = _r(RA, 0, 0, 0, 0x08)
MTV_T0_S000 = (0x12 << 26) | (0x07 << 21) | (T0 << 16)   # mtv t0, S000


def _alu() -> list[int]:
    return [
        _i(0x09, A0, T0, 0x1234),             # addiu t0, a0, 0x1234
        _r(0, T0, T1, 3, 0x00),               # sll   t1, t0, 3
        _r(T1, A1, V0, 0, 0x26),              # xor   v0, t1, a1
        _r(V0, A2, V1, 0, 0x23),              # subu  v1, v0, a2
        _r(A0, A1, 0, 0, 0x18),               # mult  a0, a1
        _r(A2, A0, T2, 0, 0x2A),              # slt   t2, a2, a0
        _i(0x0D, A0, T3, ALU_ORI_IMMEDIATE),  # ori   t3, a0, imm
        JR_RA,
        _r(V0, T2, V0, 0, 0x21),              # addu  v0, v0, t2 (delay slot)
    ]


def _ldst() -> list[int]:
    return [
        _i(0x23, A0, T0, 0),                  # lw   t0, 0(a0)
        _i(0x21, A0, T1, 6),                  # lh   t1, 6(a0)
        _i(0x24, A0, T2, 9),                  # lbu  t2, 9(a0)
        _r(T0, T1, T3, 0, 0x21),              # addu t3, t0, t1
        _i(0x2B, A0, T3, 16),                 # sw   t3, 16(a0)
        _i(0x29, A0, T2, 20),                 # sh   t2, 20(a0)
        _i(0x28, A0, T1, 23),                 # sb   t1, 23(a0)
        JR_RA,
        _i(0x23, A0, V0, 4),                  # lw   v0, 4(a0) (delay slot)
    ]


def _call(leaf_offset: int) -> list[int]:
    return [
        _i(0x09, SP, SP, -16),                # addiu sp, sp, -16
        _i(0x2B, SP, RA, 12),                 # sw    ra, 12(sp)
        _i(0x2B, SP, S0, 8),                  # sw    s0, 8(sp)
        _r(A0, ZERO, S0, 0, 0x21),            # addu  s0, a0, zero
        _j(0x03, leaf_offset),                # jal   leaf
        _i(0x09, A0, A0, 5),                  # addiu a0, a0, 5 (delay slot)
        _r(V0, S0, V0, 0, 0x21),              # addu  v0, v0, s0
        _i(0x23, SP, S0, 8),                  # lw    s0, 8(sp)
        _i(0x23, SP, RA, 12),                 # lw    ra, 12(sp)
        JR_RA,
        _i(0x09, SP, SP, 16),                 # addiu sp, sp, 16 (delay slot)
    ]


def _leaf() -> list[int]:
    return [
        _r(0, A0, V0, 2, 0x00),               # sll   v0, a0, 2
        JR_RA,
        _i(0x09, V0, V0, 1),                  # addiu v0, v0, 1 (delay slot)
    ]


def _fpu() -> list[int]:
    return [
        _cop1(0x04, A0, 0, 0, 0),             # mtc1  a0, f0
        _cop1(0x04, A1, 1, 0, 0),             # mtc1  a1, f1
        _cop1(0x10, 1, 0, 2, 0x00),           # add.s f2, f0, f1
        _cop1(0x10, 0, 2, 3, 0x02),           # mul.s f3, f2, f0
        _cop1(0x10, 1, 3, 4, 0x01),           # sub.s f4, f3, f1
        _cop1(0x00, V0, 3, 0, 0),             # mfc1  v0, f3
        _i(0x39, A2, 4, 0),                   # swc1  f4, 0(a2)
        JR_RA,
        NOP,
    ]


def _syscall() -> list[int]:
    return [
        _i(0x09, ZERO, V0, 7),                # addiu v0, zero, 7
        _r(A0, A1, V1, 0, 0x21),              # addu  v1, a0, a1
        (SYSCALL_CODE << 6) | 0x0C,           # syscall SYSCALL_CODE
        JR_RA,
        NOP,
    ]


def _import(stub_offset: int) -> list[int]:
    return [
        _i(0x09, SP, SP, -16),                # addiu sp, sp, -16
        _i(0x2B, SP, RA, 12),                 # sw    ra, 12(sp)
        _i(0x09, ZERO, A0, 3),                # addiu a0, zero, 3
        _j(0x03, stub_offset),                # jal   import stub
        _i(0x09, ZERO, A1, 4),                # addiu a1, zero, 4 (delay slot)
        _i(0x23, SP, RA, 12),                 # lw    ra, 12(sp)
        JR_RA,
        _i(0x09, SP, SP, 16),                 # addiu sp, sp, 16 (delay slot)
    ]


def _vfpu() -> list[int]:
    return [
        _i(0x09, ZERO, T0, 0x55),             # addiu t0, zero, 0x55
        MTV_T0_S000,                          # mtv   t0, S000
        JR_RA,
        NOP,
    ]


CASE_ORDER = ("alu", "ldst", "call", "leaf", "fpu", "syscall", "import", "vfpu")


def layout() -> tuple[dict[str, tuple[int, list[int]]], int, list[int]]:
    """Segment-relative offset and words of every function, the stub offset, the entry words."""
    sizes = {
        "alu": len(_alu()), "ldst": len(_ldst()), "call": len(_call(0)), "leaf": len(_leaf()),
        "fpu": len(_fpu()), "syscall": len(_syscall()), "import": len(_import(0)),
        "vfpu": len(_vfpu()),
    }
    entry_callees = [name for name in CASE_ORDER if name != "leaf"]
    offset = (len(entry_callees) * 2 + 2) * 4
    offsets: dict[str, int] = {}
    for name in CASE_ORDER:
        offsets[name] = offset
        offset += sizes[name] * 4
    stub = (offset + 15) & ~15
    functions = {
        "alu": _alu(), "ldst": _ldst(), "call": _call(offsets["leaf"]), "leaf": _leaf(),
        "fpu": _fpu(), "syscall": _syscall(), "import": _import(stub), "vfpu": _vfpu(),
    }
    placed = {name: (offsets[name], functions[name]) for name in CASE_ORDER}
    # Region A: one `jal` per case so the analyzer claims each as a function start.
    entry: list[int] = []
    for name in entry_callees:
        entry += [_j(0x03, offsets[name]), NOP]
    entry += [JR_RA, NOP]
    return placed, stub, entry


def text_section_size() -> int:
    _placed, stub, _entry = layout()
    return stub


def build_text_segment() -> bytes:
    placed, stub, entry = layout()
    text = bytearray(stub + 8)
    if len(text) > DATA_SEGMENT_OFFSET:
        raise ValueError("the cases outgrew the text segment")
    text[0 : len(entry) * 4] = struct.pack(f"<{len(entry)}I", *entry)
    for offset, words in placed.values():
        text[offset : offset + len(words) * 4] = struct.pack(f"<{len(words)}I", *words)
    text[stub : stub + 8] = struct.pack("<II", JR_RA, 0x0000000C)   # jr ra; syscall
    return bytes(text)


def relocation_records() -> list[tuple[int, int]]:
    """R_MIPS_26 for every j/jal in .text, then the module-info pointers in .data."""
    text = build_text_segment()
    _placed, stub, _entry = layout()
    records = []
    for offset in range(0, stub, 4):
        word = struct.unpack_from("<I", text, offset)[0]
        if (word >> 26) in (2, 3):
            records.append((offset, R_MIPS_26))
    # info = type | (offset segment << 8) | (target segment << 16)
    records += [
        (0x2C, R_MIPS_32 | (1 << 8) | (1 << 16)),   # module libstub
        (0x30, R_MIPS_32 | (1 << 8) | (1 << 16)),   # module libstubend
        (0x50, R_MIPS_32 | (1 << 8) | (1 << 16)),   # library name
        (0x5C, R_MIPS_32 | (1 << 8) | (1 << 16)),   # NID table
        (0x60, R_MIPS_32 | (1 << 8) | (0 << 16)),   # first import stub
    ]
    return records


def build_data_segment() -> bytes:
    _placed, stub, _entry = layout()
    data = bytearray(DATA_FILE_SIZE)
    name = b"snapshot"
    data[0:52] = struct.pack(
        "<HH28s5I", 0, 0x0100, name + b"\0" * (28 - len(name)),
        0,              # gp: unused
        0, 0,           # exports: none
        0x50, 0x64,     # imports: one library entry
    )
    library = LIBRARY.encode("ascii") + b"\0"
    data[0x34 : 0x34 + len(library)] = library
    data[0x50:0x64] = struct.pack("<IHHBBHII", 0x34, 0x0101, 0x0009, 5, 0, 1, 0x64, stub)
    struct.pack_into("<I", data, 0x64, NID)
    return bytes(data)


def build_prx() -> bytes:
    text = build_text_segment()
    data = build_data_segment()
    relocations = b"".join(struct.pack("<II", *r) for r in relocation_records())
    relocation_offset = DATA_FILE_OFFSET + DATA_FILE_SIZE
    section_names = (
        b"\0.text\0.sceStub.text\0.rodata.sceModuleInfo\0.rodata\0.lib.stub\0"
        b".rodata.sceNid\0.reloc.sceModuleInfo\0.bss\0.shstrtab\0"
    )
    names = {
        n: section_names.index(n.encode("ascii"))
        for n in (".text", ".sceStub.text", ".rodata.sceModuleInfo", ".rodata", ".lib.stub",
                  ".rodata.sceNid", ".reloc.sceModuleInfo", ".bss", ".shstrtab")
    }
    shstr_offset = relocation_offset + len(relocations)
    section_table_offset = (shstr_offset + len(section_names) + 3) & ~3
    section_count = 10
    stub = text_section_size()

    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    elf_header = ident + struct.pack(
        "<HHIIIIIHHHHHH", 0xFFA0, 8, 1, 0, 52, section_table_offset, 0x10, 52, 32, 2, 40,
        section_count, section_count - 1,
    )
    program_headers = (
        struct.pack("<8I", 1, TEXT_FILE_OFFSET, 0, 0, len(text), len(text), 5, 0x1000)
        + struct.pack("<8I", 1, DATA_FILE_OFFSET, DATA_SEGMENT_OFFSET, DATA_SEGMENT_OFFSET,
                      DATA_FILE_SIZE, DATA_MEMORY_SIZE, 6, 0x1000)
    )

    def section(name, kind, flags, address, offset, size, align, entry_size=0):
        return struct.pack("<10I", names[name], kind, flags, address, offset, size, 0, 0,
                           align, entry_size)

    d = DATA_SEGMENT_OFFSET
    sections = [
        struct.pack("<10I", *([0] * 10)),
        section(".text", 1, 6, 0, TEXT_FILE_OFFSET, stub, 4),
        section(".sceStub.text", 1, 6, stub, TEXT_FILE_OFFSET + stub, 8, 4),
        section(".rodata.sceModuleInfo", 1, 2, d, DATA_FILE_OFFSET, 52, 4),
        section(".rodata", 1, 2, d + 0x34, DATA_FILE_OFFSET + 0x34, 0x1C, 1),
        section(".lib.stub", 1, 2, d + 0x50, DATA_FILE_OFFSET + 0x50, 20, 4),
        section(".rodata.sceNid", 1, 2, d + 0x64, DATA_FILE_OFFSET + 0x64, 4, 4),
        section(".reloc.sceModuleInfo", SHT_PRX_RELOC, 0, 0, relocation_offset,
                len(relocations), 4, 8),
        section(".bss", 8, 3, d + DATA_FILE_SIZE, DATA_FILE_OFFSET + DATA_FILE_SIZE,
                BSS_SIZE, 16),
        section(".shstrtab", 3, 0, 0, shstr_offset, len(section_names), 1),
    ]
    blob = bytearray(section_table_offset + section_count * 40)
    blob[0 : len(elf_header)] = elf_header
    blob[52 : 52 + len(program_headers)] = program_headers
    blob[TEXT_FILE_OFFSET : TEXT_FILE_OFFSET + len(text)] = text
    blob[DATA_FILE_OFFSET : DATA_FILE_OFFSET + len(data)] = data
    blob[relocation_offset : relocation_offset + len(relocations)] = relocations
    blob[shstr_offset : shstr_offset + len(section_names)] = section_names
    table = b"".join(sections)
    blob[section_table_offset : section_table_offset + len(table)] = table
    return bytes(blob)


def build_psp_header() -> bytes:
    header = bytearray(0x80)
    header[:4] = b"~PSP"
    header[0x27] = 2
    struct.pack_into("<I", header, 0x38, BSS_SIZE)
    struct.pack_into("<4I", header, 0x54, len(build_text_segment()), DATA_MEMORY_SIZE, 0, 0)
    return bytes(header)


def cases() -> dict:
    placed, stub, _entry = layout()
    return {
        "base": BASE,
        "functions": {name: BASE + offset for name, (offset, _words) in placed.items()},
        "import_stub": BASE + stub,
        "import_nid": NID,
        "syscall_code": SYSCALL_CODE,
        "vfpu_op": MTV_T0_S000,
        "vfpu_pc": BASE + placed["vfpu"][0] + 4,
        "alu_ori_immediate": ALU_ORI_IMMEDIATE,
        "alu_ori_register": ALU_ORI_REGISTER,
    }


def write_if_changed(path: Path, data: bytes) -> None:
    """Keep the timestamp of an unchanged output so Make does not regenerate the C."""
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def generate(out_dir: Path) -> int:
    write_if_changed(out_dir / "guest.prx", build_prx())
    write_if_changed(out_dir / "guest.psp", build_psp_header())
    write_if_changed(out_dir / "cases.json",
                     (json.dumps(cases(), indent=2, sort_keys=True) + "\n").encode("ascii"))
    print(f"snapshot fixture: wrote {out_dir}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("generate", help="write guest.prx, guest.psp and cases.json")
    gen.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.command == "generate":
        return generate(args.out_dir)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
