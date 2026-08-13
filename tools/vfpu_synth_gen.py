#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Deterministic synthetic VFPU instruction corpus generator.

Generates a public, reproducible set of VFPU compute/prefix instruction words
WITHOUT requiring the private game ELF.  The words are derived entirely from
the VFPU instruction-format specification and the opcode families supported by
codegen.vfpu_effect().

Every word is encoded against the REAL decoder (codegen.vfpu_effect /
sr_vfpu_interp), so a word in the corpus decodes to the op its comment names.

Coverage dimensions:
  - Binary vector ops (0x18/0x19): vadd/vsub/vdiv (0x18), vmuls/vdot/vscl/vhdp/
    vcrs (0x19)
  - Unary/conversion ops (0x34): vmov/vabs/vneg/vidt/vsat0/vsat1/vzero/vone,
    vrcp/vrsq/vsin/vcos/vexp2/vlog2/vsqrt/vasin, vocp, vcst, vcmov, vf2in/z/u/d,
    vi2f, vs2i/vi2uc/vi2c/vi2us/vi2s
  - Compare/minmax (0x1B): vcmp, vmin, vmax
  - Matrix ops (0x3C): vmmul, vtfm, vmscl, vcrsp.t/vqmul.q, vmmov/vmidt/vmzero/
    vmone and the vmscl alias (VFPUMatrix1)
  - Prefix/state (0x37): vpfxs/vpfxt/vpfxd, viim/vfim
  - Vector sizes: s (1), p (2), t (3), q (4) where the opcode supports them
  - Source/dest register slots: enough to exercise independent indices
  - Source swizzles: identity (.xyzw), reverse (.wzyx), constant 0/1, abs/neg
  - Destination write-masks: all 16 combinations for quad ops
  - Saturation modes: none, [0,1], [-1,1]
  - Prefix identity vs. non-identity
  - Immediate forms (viim, vfim): boundary values 0, 1, 127, 128, 255
  - CC conditions for vcmov

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

# IMPORTANT decode contract: every word below is constructed against the REAL
# decode implemented by codegen.vfpu_effect()/vec_size() and sr_vfpu_interp(),
# not against a generic "Allegrex layout" guess.  The historical iterators
# placed sub-op/register fields per the PSP assembler's logical notation and
# produced words that the runtime decoder reads as completely different ops
# (e.g. every "0x19 unary" word actually decoded as vmuls, and 0x1B/0x34/0x3C
# words landed in Unsupported/skewed sub-ops), so the public fuzzer never
# exercised vdot/vhdp/vcrs/vscl/vmmul/vtfm/vmscl/vmmov/vqmul/vcrsp through the
# real decoder.  The builders below are verified by
# test_vfpu_synth_corpus.py (decode assertions) and by running the synthetic
# fuzzer itself.


def _vs_pool(size: int) -> tuple[int, ...]:
    """Source-register candidates for a vector size.

    The size bits are bit 7 (sizelo) and bit 15 (sizehi); neither aliases the
    7-bit vs field (bits 14:8), so every register is usable at every size.  The
    triple-width register decode still reads the register's own bit 6 for its
    row, which the pool spans both ways."""
    return (0, 4, 8, 12, 32, 64, 68)


def _bin_word(op6: int, sub: int, size: int, vd: int, vs: int, vt: int) -> int:
    """Build a 0x18/0x19/0x1B/0x3C compute word that codegen.vfpu_effect decodes
    as (op6, sub, size) with registers (vd, vs, vt).

    Runtime decode (codegen.vfpu_effect / vec_size, sr_vfpu_interp):
      op6 = bits 31:26; sub = bits 25:23; vt = bits 22:16; vs = bits 14:8;
      vd = bits 6:0; n = ((bit 7) | (bit 15 << 1)) + 1."""
    lo, hi = (size - 1) & 1, (size - 1) >> 1
    return (
        ((op6 & 0x3F) << 26)
        | ((sub & 7) << 23)
        | ((vt & 0x7F) << 16)
        | ((vs & 0x7F) << 8)
        | (vd & 0x7F)
        | (lo << 7)
        | (hi << 15)
    )


def _vfp4_word(jump: int, optype: int, size: int, vd: int, vs: int) -> int:
    """Build a 0x34 (VFPU4) word: jump = bits 25:21, optype = bits 20:16.
    vd/vs/size decode as in _bin_word (size bits 7/15)."""
    lo, hi = (size - 1) & 1, (size - 1) >> 1
    return (
        (0x34 << 26)
        | ((jump & 0x1F) << 21)
        | ((optype & 0x1F) << 16)
        | ((vs & 0x7F) << 8)
        | (vd & 0x7F)
        | (lo << 7)
        | (hi << 15)
    )


# ---------------------------------------------------------------------------
# Encoding families for op major = 0x18 (VFPU0), 0x19 (VFPU1), 0x1B (VFPU3),
#   0x34 (VFPU4), 0x37 (VFPU7 / vpfx), 0x3C (VFPU12)
# We construct raw encodings covering the semantic dimensions documented above.
# ---------------------------------------------------------------------------

def _iter_vfpu0() -> Iterator[int]:
    """Major opcode 0x18 — VFPU0: vadd (sub 0), vsub (sub 1), vdiv (sub 7)."""
    for sub in (0, 1, 7):
        for size in (1, 2, 3, 4):
            for vs in _vs_pool(size):
                for vt in (0, 4, 32, 64):
                    vd = (vs + 8) & 0x7F
                    yield _bin_word(0x18, sub, size, vd, vs, vt)


def _iter_vfpu1() -> Iterator[int]:
    """Major opcode 0x19 — VFPU1 binary vector ops: vmuls (sub 0), vdot (sub 1),
    vscl (sub 2, vt is the scalar), vhdp (sub 4), vcrs (sub 5, triple only)."""
    # vmuls: elementwise multiply, s..q
    for size in (1, 2, 3, 4):
        for vs in _vs_pool(size):
            for vt in (0, 4, 32, 64):
                vd = (vs + 8) & 0x7F
                yield _bin_word(0x19, 0, size, vd, vs, vt)
    # vdot / vhdp: scalar destination, p..q
    for sub in (1, 4):
        for size in (2, 3, 4):
            for vs in _vs_pool(size):
                for vt in (0, 4, 32, 64):
                    vd = (vs + 8) & 0x7F
                    yield _bin_word(0x19, sub, size, vd, vs, vt)
    # vscl: vector * scalar, p..q
    for size in (2, 3, 4):
        for vs in _vs_pool(size):
            for vt in (0, 1, 4, 32):
                vd = (vs + 8) & 0x7F
                yield _bin_word(0x19, 2, size, vd, vs, vt)
    # vcrs: triple-vector form only (codegen/interp reject other widths)
    for vs in (64, 68, 72):
        for vt in (0, 4, 32):
            vd = (vs + 8) & 0x7F
            yield _bin_word(0x19, 5, 3, vd, vs, vt)


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
       vfim (half-float immediate).  Both are op 0x37 with regnum 3 (bits 25:24
       = 11); bit 23 selects vfim.  The destination register is bits 22:16, the
       immediate is the low 16 bits (codegen.vfpu_effect / sr_vfpu_interp)."""
    for vd in (0, 1, 32, 64, 127):
        for imm in (0, 1, 0x7F, 0x80, 0xFF, 0x40, 0x3F):
            yield ((0xDF << 24) | ((vd & 0x7F) << 16) | (imm & 0xFFFF)) & 0xFFFFFFFF
            yield ((0xDF << 24) | (1 << 23) | ((vd & 0x7F) << 16) | (imm & 0xFFFF)) & 0xFFFFFFFF


def _iter_vfpu3() -> Iterator[int]:
    """Major opcode 0x1B — VFPU3: vcmp (sub 0), vmin (sub 2), vmax (sub 3).
    (vscmp/vsge/vslt/vsgn/vbfy/vcmov as named here have no static emitter or
    interpreter path in this runtime and are intentionally not emitted.)"""
    for sub in (0, 2, 3):
        for size in (1, 2, 3, 4):
            for vs in _vs_pool(size):
                for vt in (0, 4, 32, 64):
                    vd = (vs + 8) & 0x7F
                    yield _bin_word(0x1B, sub, size, vd, vs, vt)


def _iter_vfpu4() -> Iterator[int]:
    """Major opcode 0x34 — VFPU4 unary/conversion family: vmov/vabs/vneg/vidt/
    vsat0/vsat1/vzero/vone (jump 0, optype 0-7), transcendental ops (jump 0,
    optype 16-23), vocp (jump 2), vcst (jump 3), vcmov (jump 0x15), vf2in/z/u/d
    (jumps 16-19), vi2f (jump 20), vs2i/vi2uc/vi2c/vi2us/vi2s (jump 1, idx7
    27-31).  The historical docstring called 0x34 the matrix family -- matrix
    ops are 0x3C; those words decoded as unsupported/skewed ops, so conversions
    were never fuzzed either."""
    # VV2Op: vmov(0) vabs(1) vneg(2) vidt(3) vsat0(4) vsat1(5) vzero(6) vone(7)
    for optype in (0, 1, 2, 3, 4, 5, 6, 7):
        for size in (1, 2, 3, 4):
            for vs in _vs_pool(size):
                vd = (vs + 8) & 0x7F
                yield _vfp4_word(0, optype, size, vd, vs)
    # transcendental ops: vrcp(16) vrsq(17) vsin(18) vcos(19) vexp2(20) vlog2(21)
    # vsqrt(22) vasin(23)
    for optype in (16, 17, 18, 19, 20, 21, 22, 23):
        for size in (1, 2, 3, 4):
            for vs in _vs_pool(size):
                vd = (vs + 8) & 0x7F
                yield _vfp4_word(0, optype, size, vd, vs)
    # vocp (jump 2, op9 4): d = 1 - s via forced prefixes
    for size in (1, 2, 3, 4):
        for vs in _vs_pool(size):
            vd = (vs + 8) & 0x7F
            yield _vfp4_word(2, 4, size, vd, vs)
    # vcst (jump 3): constant broadcast, representative indices
    for size in (1, 2, 3, 4):
        for vs in _vs_pool(size):
            vd = (vs + 8) & 0x7F
            for cst in (0, 1, 16, 31):
                yield _vfp4_word(3, cst, size, vd, vs)
    # vcmov (jump 0x15): imm3 selects the CC bit (0..6)
    for size in (1, 2, 3, 4):
        for vs in _vs_pool(size):
            vd = (vs + 8) & 0x7F
            for imm3 in (0, 1, 5, 6):
                yield _vfp4_word(0x15, imm3, size, vd, vs)
    # vf2in/vf2iz/vf2iu/vf2id (jumps 16-19) and vi2f (jump 20): representative scales
    for jump in (16, 17, 18, 19, 20):
        for size in (1, 2, 3, 4):
            for vs in _vs_pool(size):
                vd = (vs + 8) & 0x7F
                for imm in (0, 1, 15, 31):
                    yield _vfp4_word(jump, imm, size, vd, vs)
    # vs2i (idx7 27) / vi2uc(28) / vi2c(29) / vi2us(30) / vi2s(31)
    for size in (1, 2, 3, 4):
        for vs in _vs_pool(size):
            vd = (vs + 8) & 0x7F
            for idx7 in (27, 28, 29, 30, 31):
                yield _vfp4_word(1, idx7, size, vd, vs)


def _iter_vfpu12() -> Iterator[int]:
    """Major opcode 0x3C — VFPU12 matrix family: vmmul (sub 0), vtfm (subs 1-3),
    vmscl (sub 4), vcrsp.t/vqmul.q (sub 5), and VFPUMatrix1 (sub 7, idx 28):
    vmmov (which 0), vmscl alias (which 1,2,4,5), vmidt (3), vmzero (6),
    vmone (7)."""
    # vmmul: matrix multiply, p..q
    for size in (2, 3, 4):
        for vs in _vs_pool(size):
            for vt in (0, 4, 32, 64):
                vd = (vs + 8) & 0x7F
                yield _bin_word(0x3C, 0, size, vd, vs, vt)
    # vtfm: vector transform; matrix side and dest vector size = ins + 1
    for ins in (1, 2, 3):
        side = ins + 1
        for vs in _vs_pool(side):
            for vt in (0, 4, 32, 64):
                vd = (vs + 8) & 0x7F
                yield _bin_word(0x3C, ins, side, vd, vs, vt)
    # vmscl: matrix * scalar (vt is the scalar), p..q
    for size in (2, 3, 4):
        for vs in _vs_pool(size):
            for vt in (0, 1, 4, 32):
                vd = (vs + 8) & 0x7F
                yield _bin_word(0x3C, 4, size, vd, vs, vt)
    # vcrsp.t (size 3) / vqmul.q (size 4); identity-prefix family in the fuzzer
    for size in (3, 4):
        for vs in _vs_pool(size):
            for vt in (0, 4, 32):
                vd = (vs + 8) & 0x7F
                yield _bin_word(0x3C, 5, size, vd, vs, vt)
    # VFPUMatrix1 (sub 7, idx 28): which = bits 20:16; size bits 7/15 as usual.
    for size in (2, 3, 4):
        for vs in _vs_pool(size):
            vd = (vs + 8) & 0x7F
            for which in (0, 1, 2, 3, 4, 5, 6, 7):
                lo, hi = (size - 1) & 1, (size - 1) >> 1
                yield (
                    (0x3C << 26)
                    | (28 << 21)
                    | ((which & 0xF) << 16)
                    | ((vs & 0x7F) << 8)
                    | (vd & 0x7F)
                    | (lo << 7)
                    | (hi << 15)
                )


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
