# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Generate the source-owned guest module for the verify selftest (#1843).

The module is the snapshot-execution module (``fixtures/snapshot/generate.py``, #1842) with
two more functions, built by the same PRX writer:

  drive    calls ``leaf`` ``a0`` times with a direct ``jal`` inside a counted loop, so the
           verifier sees many direct calls of one callee from one caller
  spin     a counted loop of ``a0`` iterations, so its back-edge reaches ``SR_YIELD``
           and a scheduler slice that expires inside the call is a yield

Names, addresses, instructions and values are project-authored test values; nothing here
contains or derives from a retail title.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "snapshot_generate", ROOT / "fixtures" / "snapshot" / "generate.py")
assert _SPEC is not None and _SPEC.loader is not None
base = importlib.util.module_from_spec(_SPEC)
sys.modules["snapshot_generate"] = base
_SPEC.loader.exec_module(base)

CASE_ORDER = base.CASE_ORDER + ("drive", "spin")


def _drive(leaf_offset: int) -> list[int]:
    i, r = base._i, base._r
    return [
        i(0x09, base.SP, base.SP, -16),        # addiu sp, sp, -16
        i(0x2B, base.SP, base.RA, 12),         # sw    ra, 12(sp)
        i(0x2B, base.SP, base.S0, 8),          # sw    s0, 8(sp)
        r(base.A0, 0, base.S0, 0, 0x21),       # addu  s0, a0, zero
        base._j(0x03, leaf_offset),            # L: jal leaf
        r(base.S0, 0, base.A0, 0, 0x21),       # addu  a0, s0, zero (delay slot)
        i(0x09, base.S0, base.S0, -1),         # addiu s0, s0, -1
        i(0x07, base.S0, 0, -4),               # bgtz  s0, L
        base.NOP,
        i(0x23, base.SP, base.S0, 8),          # lw    s0, 8(sp)
        i(0x23, base.SP, base.RA, 12),         # lw    ra, 12(sp)
        base.JR_RA,
        i(0x09, base.SP, base.SP, 16),         # addiu sp, sp, 16 (delay slot)
    ]


def _spin() -> list[int]:
    i, r = base._i, base._r
    return [
        i(0x09, base.A0, base.A0, -1),         # L: addiu a0, a0, -1
        i(0x07, base.A0, 0, -2),               # bgtz  a0, L
        base.NOP,
        base.JR_RA,
        r(base.A0, 0, base.V0, 0, 0x21),       # addu  v0, a0, zero (delay slot)
    ]


def layout():
    """Segment-relative offset and words of every function, the stub offset, the entry words."""
    sizes = {
        "alu": len(base._alu()), "ldst": len(base._ldst()), "call": len(base._call(0)),
        "leaf": len(base._leaf()), "fpu": len(base._fpu()), "syscall": len(base._syscall()),
        "import": len(base._import(0)), "vfpu": len(base._vfpu()),
        "drive": len(_drive(0)), "spin": len(_spin()),
    }
    entry_callees = [name for name in CASE_ORDER if name != "leaf"]
    offset = (len(entry_callees) * 2 + 2) * 4
    offsets: dict[str, int] = {}
    for name in CASE_ORDER:
        offsets[name] = offset
        offset += sizes[name] * 4
    stub = (offset + 15) & ~15
    functions = {
        "alu": base._alu(), "ldst": base._ldst(), "call": base._call(offsets["leaf"]),
        "leaf": base._leaf(), "fpu": base._fpu(), "syscall": base._syscall(),
        "import": base._import(stub), "vfpu": base._vfpu(),
        "drive": _drive(offsets["leaf"]), "spin": _spin(),
    }
    placed = {name: (offsets[name], functions[name]) for name in CASE_ORDER}
    entry: list[int] = []
    for name in entry_callees:
        entry += [base._j(0x03, offsets[name]), base.NOP]
    entry += [base.JR_RA, base.NOP]
    return placed, stub, entry


def cases() -> dict:
    out = base_cases()
    placed, _stub, _entry = layout()
    out["functions"] = {name: base.BASE + offset for name, (offset, _words) in placed.items()}
    return out


base_cases = base.cases
# The PRX writer looks these up by name at call time.
base.CASE_ORDER = CASE_ORDER
base.layout = layout
base.cases = cases


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("generate", help="write guest.prx, guest.psp and cases.json")
    gen.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    return base.generate(args.out_dir)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
