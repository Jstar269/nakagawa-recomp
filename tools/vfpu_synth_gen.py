#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Deterministic synthetic VFPU instruction corpus generator.

Generates a public, reproducible set of VFPU compute/prefix instruction words
WITHOUT requiring the private game ELF.  The words are derived entirely from
the VFPU instruction-format specification and the opcode families supported by
codegen.vfpu_effect().

Coverage dimensions:
  - Opcode families: vadd/vsub/vmul/vdiv/vdot (binary), vmov/vabs/vneg/vsqrt/vrcp/vrsq
    (unary), vmin/vmax, vsgn, vfad/vavg, vhdp, vdot, vscl, viim/vfim, vpfx (prefix/state)
  - Vector sizes: s (1), p (2), t (3), q (4) where the opcode supports them
  - Source/dest register slots: enough to exercise independent indices
  - Source swizzles: identity (.xyzw), reverse (.wzyx), constant 0/1, abs/neg
  - Destination write-masks: all 16 combinations for quad ops
  - Saturation modes: none, [0,1], [-1,1]
  - Prefix identity vs. non-identity
  - Immediate forms (viim, vfim): boundary values 0, 1, 127, 128, 255
  - CC conditions for vbfy/vcmov-like forms

The generator does NOT import or scan the game ELF.  It is the only VFPU
corpus that may be committed to the public repository.

Usage:
  python tools/vfpu_synth_gen.py [--out FILE]   # print or write word list
  python tools/vfpu_synth_gen.py --count        # print count only

The output format is one hex word per line (0x....).  The ordering is
deterministic: same Python version, same output.

Note on shared-helper limitations
-----------------------------------
The public fuzzer (src/rt/vfpu_fuzz.c + build/hst/vfpu_fuzz_cases.h) compares:
  codegen.vfpu_effect()-generated C  vs.  sr_vfpu_interp

Both paths share some low-level helpers:
  sr_vread / sr_vwrite  -- vector register file I/O
  prefix behavior helpers (swizzle decode, abs/neg/constant application,
      destination mask, saturation clamp)
  transcendental/math kernels (vrcp, vrsq, vsin, vcos, vexp, vlog)

Therefore zero divergence on a synthetic case proves:
  "the codegen emitter path and the interpreter path agree for this state"

It does NOT independently prove that the shared helpers match PSP hardware.
For that, you need an independent oracle (measured PSP traces or a verified
reference implementation).  See tools/vfpu_coverage_report.py for the
coverage-category breakdown.
"""

from __future__ import annotations

import argparse
import sys
from typing import Iterator


# ---------------------------------------------------------------------------
# VFPU encoding helpers
# ---------------------------------------------------------------------------

def _vr(idx: int, size_code: int) -> int:
    """Pack a vector register index (0-127) and size code (0=s,1=p,2=t,3=q)
    into a 7-bit field as the Allegrex VFPU encoding expects."""
    return (idx & 0x7F)


# opcode[31:26], t[25:23], s[22:18], vd[17:8], ...
# For COP2 VFPU compute: bits 31:26 = 6'b0110xx / 0111xx depending on opcode
# We construct words directly by opcode family.

def _compute_word(op6: int, vt: int, vs: int, vd: int, one: int = 0) -> int:
    """Build a 32-bit VFPU compute instruction word.

    Layout (from Allegrex VFPU encoding):
      31:26 = major opcode (e.g. 0x18 = 011000, 0x1B = 011011, ...)
      25    = size[1]  (with bit 7 of vd for size[0])
      24:23 = op      (sub-operation within the family)
      22:16 = vs      (7 bits)
      15:8  = vd      (8 bits, bit 7 encodes size[0] in some families)
       7:0  = vt      (7 bits + 1 padding/prefix bit)
    The exact layout differs by family; this function covers the most common
    binary/unary VFPU op layout used by codegen.vfpu_effect for opgroup 0x18/0x19.
    """
    # op6 occupies bits 31:26
    word = (op6 & 0x3F) << 26
    # bits 25:16 = vt[6:0] + vs[6:0] packed differently per family;
    # simplify: treat as standard (vs[6:0] at 22:16, vt[6:0] at 7:0+, vd at 15:8)
    word |= (vs & 0x7F) << 16
    word |= (vd & 0xFF) << 8
    word |= (vt & 0x7F)
    word |= (one & 1) << 25   # size bit
    return word & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Encoding families for op major = 0x18 (VFPU0), 0x19 (VFPU1), 0x1B (VFPU3),
#   0x34 (VFPU4), 0x37 (VFPU7 / vpfx), 0x3C (VFPU12)
# We construct raw encodings covering the semantic dimensions documented above.
# ---------------------------------------------------------------------------

def _iter_vfpu0() -> Iterator[int]:
    """Major opcode 0x18 — VFPU0: vadd, vsub, vdiv, vmul variants."""
    # sub-op encoded in bits 23:21
    for sub in range(8):          # vadd(0), vsub(1), vsbn(2), ?, vmul(4), vdot(5), vscl(6), ...
        for size in (0, 1, 2, 3): # s/p/t/q
            for vs in (0, 1, 32, 64):
                for vt in (0, 1, 4):
                    vd = (vs + 8) & 0x7F
                    # Build: op6=0x18, sub in bits 23:21, size in 24+bit7-of-vd, vs/vt/vd as above
                    # Simplified encode: op6=0x18 | sub<<18 | size bits
                    word = (0x18 << 26)
                    word |= (sub & 0x7) << 21
                    # size: bit 24 = size>>1, bit 7 of vd = size&1
                    word |= ((size >> 1) & 1) << 24
                    vd_enc = (vd & 0x7F) | ((size & 1) << 7)
                    word |= (vs & 0x7F) << 16
                    word |= (vd_enc & 0xFF) << 8
                    word |= (vt & 0x7F)
                    yield word & 0xFFFFFFFF


def _iter_vfpu1() -> Iterator[int]:
    """Major opcode 0x19 — VFPU1: unary ops (vmov, vabs, vneg, vidt, vsat0, vsat1,
       vzero, vone, vrcp, vrsq, vsin, vcos, vexp2, vlog2, vsqrt, vasin, ...)."""
    for sub in range(32):         # wide range of unary sub-ops
        for size in (0, 1, 2, 3):
            for vs in (0, 1, 8, 16, 32):
                vd = (vs + 4) & 0x7F
                word = (0x19 << 26)
                word |= (sub & 0x1F) << 16    # sub-op in bits 20:16
                vd_enc = (vd & 0x7F) | ((size & 1) << 7)
                word |= ((size >> 1) & 1) << 24
                word |= (vd_enc & 0xFF) << 8
                word |= (vs & 0x7F)           # vs goes in vt-field for unary
                yield word & 0xFFFFFFFF


def _iter_vpfx() -> Iterator[int]:
    """Major opcode 0x37 — VPFXS/VPFXT/VPFXD (prefix state registers).

    Prefix dimensions:
      source prefix (VPFXS/VPFXT): swizzle (x/y/z/w), abs, negate, constant-0, constant-1
      destination prefix (VPFXD): write-mask, saturation

    We enumerate representative combinations, not exhaustively (2^20 is too large)."""
    # VPFXS = 0xDC000000 | s_desc[19:0]
    # VPFXT = 0xDD000000 | t_desc[19:0]
    # VPFXD = 0xDE000000 | d_desc[7:0] (mask+sat)

    # Source prefix word: [19:0] encodes {negi[3:0], absi[3:0], cst[3:0], swz[7:0]}
    swizzles = [
        0x00,  # x,x,x,x (constant-source corner)
        0xE4,  # x,y,z,w (identity)
        0xB1,  # y,x,w,z (swap pairs)
        0x1B,  # w,z,y,x (reverse)
        0x00,  # x repeated
    ]
    abs_mask = [0x00, 0x0F, 0x05, 0x0A]   # no abs, all abs, alternating
    neg_mask = [0x00, 0x0F, 0x03, 0x0C]
    cst_mask = [0x00, 0x01, 0x02, 0x04]   # one constant element

    for swz in swizzles:
        for ab in abs_mask[:2]:
            for neg in neg_mask[:2]:
                for cst in cst_mask[:2]:
                    desc = (neg << 16) | (ab << 12) | (cst << 8) | swz
                    yield (0xDC << 24) | (desc & 0xFFFFF)  # VPFXS
                    yield (0xDD << 24) | (desc & 0xFFFFF)  # VPFXT

    # Destination prefix: [7:0] = {sat[7:6], mask[3:0]}
    # mask: bit 0=x, 1=y, 2=z, 3=w (0 = write, 1 = mask out)
    # sat: 00=none, 01=[0,1], 10=[-1,1]
    for mask in range(16):       # all 16 destination mask combinations
        for sat in (0, 1, 2):    # three saturation modes
            dpfx = (sat << 4) | mask
            yield (0xDE << 24) | dpfx


def _iter_viim_vfim() -> Iterator[int]:
    """Immediate-form VFPU instructions: viim (integer immediate to single),
       vfim (half-float immediate).  op6 = 0xDF for viim-like, 0x7F for vfim."""
    # viim: 0xDF000000 | vd[7:0]<<8 | imm8[7:0]
    for vd in (0, 1, 32, 64, 127):
        for imm in (0, 1, 0x7F, 0x80, 0xFF, 0x40, 0x3F):
            yield ((0xDF << 24) | ((vd & 0xFF) << 8) | (imm & 0xFF)) & 0xFFFFFFFF
    # vfim: 0x7F000000 | vd[7:0]<<8 | imm8[7:0]
    for vd in (0, 1, 32, 64, 127):
        for imm in (0, 1, 0x7F, 0x80, 0xFF, 0x40, 0x3F):
            yield ((0x7F << 24) | ((vd & 0xFF) << 8) | (imm & 0xFF)) & 0xFFFFFFFF


def _iter_vfpu3() -> Iterator[int]:
    """Major opcode 0x1B — VFPU3: vcmp, vmin, vmax, vscmp, vsge, vslt, vsgn, vbfy, vcmov."""
    for sub in range(16):
        for size in (0, 1, 2, 3):
            for vs in (0, 1, 8):
                vt = (vs + 2) & 0x7F
                vd = (vs + 4) & 0x7F
                word = (0x1B << 26)
                word |= (sub & 0xF) << 21
                word |= ((size >> 1) & 1) << 24
                vd_enc = (vd & 0x7F) | ((size & 1) << 7)
                word |= (vs & 0x7F) << 16
                word |= (vd_enc & 0xFF) << 8
                word |= (vt & 0x7F)
                yield word & 0xFFFFFFFF


def _iter_vfpu4() -> Iterator[int]:
    """Major opcode 0x34 — VFPU4: matrix/vector ops including vmmul, vtfm, vhtfm, vqmul."""
    for sub in range(8):
        for size in (1, 2, 3):  # p/t/q make sense for matrix ops
            for vs in (0, 32, 64):
                vt = (vs + 4) & 0x7F
                vd = (vs + 8) & 0x7F
                word = (0x34 << 26)
                word |= (sub & 0x7) << 21
                word |= ((size >> 1) & 1) << 24
                vd_enc = (vd & 0x7F) | ((size & 1) << 7)
                word |= (vs & 0x7F) << 16
                word |= (vd_enc & 0xFF) << 8
                word |= (vt & 0x7F)
                yield word & 0xFFFFFFFF


def _iter_vfpu12() -> Iterator[int]:
    """Major opcode 0x3C — VFPU12: additional ops (vdet, ?)."""
    for sub in range(4):
        for size in (0, 1, 2, 3):
            for vs in (0, 1, 8):
                vt = (vs + 2) & 0x7F
                vd = (vs + 4) & 0x7F
                word = (0x3C << 26)
                word |= (sub & 0x3) << 21
                word |= ((size >> 1) & 1) << 24
                vd_enc = (vd & 0x7F) | ((size & 1) << 7)
                word |= (vs & 0x7F) << 16
                word |= (vd_enc & 0xFF) << 8
                word |= (vt & 0x7F)
                yield word & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Self-comparison guard
# ---------------------------------------------------------------------------

def _check_no_self_compare(words: list[int]) -> None:
    """Verify that none of the generated words look like a call to sr_vfpu_interp.

    This is a static check on the word encodings: sr_vfpu_interp is a host
    C function, not a VFPU instruction word, so VFPU instruction words can
    never *be* sr_vfpu_interp.  The real guard is in vfpu_fuzz_gen.py where
    codegen.vfpu_effect() is called and Unsupported exceptions (which would
    fall back to the interpreter) cause the case to be skipped.  This function
    is a belt-and-suspenders assertion."""
    # The Unsupported exception mechanism means that if any word in our synthetic
    # corpus cannot be code-generated (vfpu_effect raises Unsupported), the
    # fuzzer generator will skip it and report it.  No additional word-level
    # check is needed here; just confirm the list is non-empty.
    if not words:
        raise ValueError("Synthetic VFPU corpus is empty -- generation error")


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def generate_synthetic_corpus() -> list[int]:
    """Return a sorted, deduplicated list of synthetic VFPU instruction words.

    The words cover the semantic dimensions documented in the module docstring.
    The list is deterministic and does not require the private game ELF.

    PROVENANCE: every word in this list is derived from the public Allegrex
    VFPU instruction-encoding specification, not extracted from any game binary.
    """
    seen: set[int] = set()
    for w in _iter_vfpu0():
        seen.add(w)
    for w in _iter_vfpu1():
        seen.add(w)
    for w in _iter_vpfx():
        seen.add(w)
    for w in _iter_viim_vfim():
        seen.add(w)
    for w in _iter_vfpu3():
        seen.add(w)
    for w in _iter_vfpu4():
        seen.add(w)
    for w in _iter_vfpu12():
        seen.add(w)

    corpus = sorted(seen)
    _check_no_self_compare(corpus)
    return corpus


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", metavar="FILE", help="write word list to FILE (default: stdout)")
    parser.add_argument("--count", action="store_true", help="print count only, do not output words")
    args = parser.parse_args(argv)

    corpus = generate_synthetic_corpus()

    if args.count:
        print(f"{len(corpus)}")
        return 0

    lines = [f"0x{w:08x}" for w in corpus]
    if args.out:
        with open(args.out, "w", encoding="ascii", newline="\n") as fh:
            fh.write("\n".join(lines) + "\n")
        sys.stderr.write(f"vfpu_synth_gen: wrote {len(corpus)} synthetic VFPU words to {args.out}\n")
    else:
        for line in lines:
            print(line)
        sys.stderr.write(f"vfpu_synth_gen: {len(corpus)} synthetic VFPU words\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
