#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Deterministic synthetic VFPU instruction corpus generator.

Generates a public, reproducible set of VFPU compute/prefix instruction words
WITHOUT requiring the private game ELF.  The words are derived entirely from
the VFPU instruction-format specification and the opcode families supported by
codegen.vfpu_effect().

Independent source packet (Freebuff, 2026-08-28) — fixed regression anchors
-------------------------------------------------------------------------
These literals are from an independent source (PSP hardware manual + PSP
assembler reference), NOT from Nakagawa's own decoder.  They are used as
fixed-vector regression anchors; a generated word must equal the literal
when the same logical operation and minimal registers (vd=vs=vt=0) are used.
Do NOT verify them by decoding with codegen.vfpu_effect and calling that
proof — compare the integer word directly.

    vadd.s  0x60000000
    vadd.p  0x60000080
    vadd.t  0x60008000
    vadd.q  0x60008080

    vmul.s  0x64000000
    vmul.p  0x64000080
    vmul.t  0x64008000
    vmul.q  0x64008080

    vmin.s  0x6D000000
    vmax.s  0x6D800000

    vcst.s  0xD0600000
    vmov.s  0xD0000000

    vmmul.p 0xF0000080
    vmmul.t 0xF0008000
    vmmul.q 0xF0008080

    vtfm2.p 0xF0800080
    vtfm3.t 0xF1008000
    vtfm4.q 0xF1808080

    vcrsp.t 0xF2808000
    vqmul.q 0xF2808080

    vpfxs   0xDC000000
    vpfxt   0xDD000000
    vpfxd   0xDE000000
    viim.s  0xDF000000
    vfim.s  0xDF800000

Independent size encoding:
    bit15 = sizehi
    bit7  = sizelo
    S=00 P=01 T=10 Q=11   (n = ((bit7)|(bit15<<1))+1)

Critical invariant:
    family 0x3C:
        vcrsp.t -> sub=(word>>23)&7 == 5, size=T (code 2)
        vqmul.q -> sub=(word>>23)&7 == 5, size=Q (code 3)

    Production selects family-0x3C suboperation from bits 25:23 (3 bits).
    A prior _encode_vfpu12(5, ...) incorrectly set the 5-bit field at
    bits 25:21 (5<<21 = 0x00A00000), which decodes as sub=1 (vtfm)
    rather than sub=5.

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
  prefix behavior helpers (swizzle decode, abs/neg,constant application,
      destination mask, saturation clamp)
  transcendental/math kernels (vrcp, vrsq, vsin, vcos, vexp, vlog)

Therefore zero divergence on a synthetic case proves:
  "the codegen emitter path and the interpreter path agree for this state"

It does NOT independently prove that the shared helpers match PSP hardware.
For that, you need an independent oracle (measured PSP traces or a verified
reference implementation).  See tools/vfpu_coverage_report.py for the
coverage-category breakdown.

Protected contract (verified by tests):
  - Canonical independent anchors (vpfxs 0xDC000000, vpfxt 0xDD000000,
    vpfxd 0xDE000000, viim 0xDF000000, vfim 0xDF800000) are pinned by direct
    integer membership in the iterator/generator output.
  - Per-category cardinalities (family, suboperation, legal size sets,
    VFPU4 jump classes, VFPU12 idx/which, prefix/immediate categories) exactly
    match the spec-derived INTENDED_* tables.
  - Operation/suboperation/size coverage exactly matches the intended spec.
  Non-anchor intra-category substitutions that preserve the above aggregates
  (e.g., swapping one vpfxs swizzle 0xE4 for 0xB1 while keeping total 32) are
  an explicitly acknowledged operand-distribution boundary and are NOT claimed
  to be killed. The guarantee is anchors + counts + coverage, not every
  individual swizzle/register/immediate tuple.
"""

from __future__ import annotations

import argparse
import sys
from typing import Iterator

# ---------------------------------------------------------------------------
# Independent fixed-vector anchors (Freebuff packet, 2026-08-28)
# ---------------------------------------------------------------------------

INDEPENDENT_LITERALS: dict[str, int] = {
    "vadd.s":  0x60000000,
    "vadd.p":  0x60000080,
    "vadd.t":  0x60008000,
    "vadd.q":  0x60008080,
    "vmul.s":  0x64000000,
    "vmul.p":  0x64000080,
    "vmul.t":  0x64008000,
    "vmul.q":  0x64008080,
    "vmin.s":  0x6D000000,
    "vmax.s":  0x6D800000,
    "vcst.s":  0xD0600000,
    "vmov.s":  0xD0000000,
    "vmmul.p": 0xF0000080,
    "vmmul.t": 0xF0008000,
    "vmmul.q": 0xF0008080,
    "vtfm2.p": 0xF0800080,
    "vtfm3.t": 0xF1008000,
    "vtfm4.q": 0xF1808080,
    "vcrsp.t": 0xF2808000,
    "vqmul.q": 0xF2808080,
    "vpfxs":   0xDC000000,
    "vpfxt":   0xDD000000,
    "vpfxd":   0xDE000000,
    "viim.s":  0xDF000000,
    "vfim.s":  0xDF800000,
}

# INDEPENDENT_LITERALS is documentation of the external source packet; the 5
# canonical prefix/immediate anchors are verified by direct integer membership
# in the generator output (see test_vfpu_synth_corpus.py), not by self-compare.
assert INDEPENDENT_LITERALS["vpfxs"] == 0xDC000000
assert INDEPENDENT_LITERALS["vpfxt"] == 0xDD000000
assert INDEPENDENT_LITERALS["vpfxd"] == 0xDE000000
assert INDEPENDENT_LITERALS["viim.s"] == 0xDF000000
assert INDEPENDENT_LITERALS["vfim.s"] == 0xDF800000

def _size_bits_independent(size_1_4: int) -> tuple[int, int]:
    """Independent size bits: size 1..4 -> (lo,hi) where bit7=lo, bit15=hi, S00 P01 T10 Q11"""
    if size_1_4 not in (1,2,3,4):
        raise ValueError(size_1_4)
    lo = (size_1_4 - 1) & 1
    hi = (size_1_4 - 1) >> 1
    return lo, hi

def _decode_size_independent(word: int) -> int:
    return (((word >> 7) & 1) | ((word >> 15) & 1) << 1) + 1


# ---------------------------------------------------------------------------
# Independent intended-coverage specification (spec-derived, not output-derived)
# ---------------------------------------------------------------------------
# This table is the independent contract: expected counts for each deliberately
# generated dimension.  It is derived from the specification and the iterator
# parameter sets (see _iter_vfpu0 etc), NOT by measuring
# generate_synthetic_corpus().  Tests must compare the actual corpus against
# this table; the table must NOT be computed from the output being tested.
#
# Dimensions covered:
#   - family                     (op6)
#   - suboperation               (3-bit sub for 0x18/0x19/0x1B/0x3C, 5-bit jump for 0x34)
#   - legal size set             (S/P/T/Q via bits 7/15; per-sub legal sets enforced)
#   - VFPU4 subfield/jump class  (bits 25:21)
#   - VFPU12 suboperation        (bits 25:23 sub, plus idx 28/29 for sub==7)
#   - prefix/immediate category  (VPFXS/VPFXT/VPFXD/VIIM/VFIM via top byte & bit23)
#
# Baseline preserves exact accepted facts:
#   total 2742, S718 P682 T677 Q665,
#   0x18:144 0x19:204 0x1B:60 0x34:2020 0x37:182 0x3C:132, actual 0x3C/sub5=6,
#   vcrsp.t 0xF2808000, vqmul.q 0xF2808080.
#
# The table is intentionally verbose and hardcoded to kill:
#   balanced 0x1B/sub6->sub0, balanced VFPU4 redistribution,
#   remove+duplicate in same family/size, size redistribution within same sub,
#   drop family, drop Q, drop sub5, prefix/immediate substitution,
#   VFPU4 denominator inflation, malformed injection.

INTENDED_TOTAL = 2742

INTENDED_FAMILY_COUNTS: dict[int, int] = {
    0x18: 144,
    0x19: 204,
    0x1B: 60,
    0x34: 2020,
    0x37: 182,
    0x3C: 132,
}

INTENDED_SIZE_COUNTS: dict[int, int] = {
    1: 718,  # S
    2: 682,  # P
    3: 677,  # T
    4: 665,  # Q
}

# Family -> sub (3-bit) -> count
INTENDED_FAMILY_SUB_COUNTS: dict[int, dict[int, int]] = {
    0x18: {0: 48, 1: 48, 7: 48},
    0x19: {0: 48, 1: 48, 2: 48, 4: 48, 5: 12},
    0x1B: {0: 12, 2: 12, 3: 12, 6: 12, 7: 12},
    0x3C: {0: 9, 1: 9, 2: 9, 3: 9, 4: 9, 5: 6, 7: 81},
}

# VFPU4 jump (5-bit at 25:21) -> count
INTENDED_VFPU4_JUMP_COUNTS: dict[int, int] = {
    0: 480,
    1: 100,
    2: 20,
    3: 240,
    16: 180,
    17: 180,
    18: 180,
    19: 180,
    20: 180,
    21: 280,
}

# VFPU4 jump -> size -> count  (size 1..4 = S/P/T/Q)
INTENDED_VFPU4_JUMP_SIZE_COUNTS: dict[tuple[int, int], int] = {
    (0, 1): 120, (0, 2): 120, (0, 3): 120, (0, 4): 120,
    (21, 1): 70, (21, 2): 70, (21, 3): 70, (21, 4): 70,
    (3, 1): 60, (3, 2): 60, (3, 3): 60, (3, 4): 60,
    (16, 1): 45, (16, 2): 45, (16, 3): 45, (16, 4): 45,
    (17, 1): 45, (17, 2): 45, (17, 3): 45, (17, 4): 45,
    (18, 1): 45, (18, 2): 45, (18, 3): 45, (18, 4): 45,
    (19, 1): 45, (19, 2): 45, (19, 3): 45, (19, 4): 45,
    (20, 1): 45, (20, 2): 45, (20, 3): 45, (20, 4): 45,
    (1, 1): 25, (1, 2): 25, (1, 3): 25, (1, 4): 25,
    (2, 1): 5, (2, 2): 5, (2, 3): 5, (2, 4): 5,
}

# Family -> size -> count
INTENDED_FAMILY_SIZE_COUNTS: dict[int, dict[int, int]] = {
    0x18: {1: 36, 2: 36, 3: 36, 4: 36},
    0x19: {1: 48, 2: 48, 3: 60, 4: 48},
    0x1B: {1: 15, 2: 15, 3: 15, 4: 15},
    0x34: {1: 505, 2: 505, 3: 505, 4: 505},
    0x37: {1: 114, 2: 36, 3: 16, 4: 16},
    0x3C: {1: 0, 2: 42, 3: 45, 4: 45},
}

# (family, sub) -> size -> count  (size 1..4; for 0x34 sub is jump)
INTENDED_SUB_SIZE_COUNTS: dict[tuple[int, int], dict[int, int]] = {
    (0x18, 0): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x18, 1): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x18, 7): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x19, 0): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x19, 1): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x19, 2): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x19, 4): {1: 12, 2: 12, 3: 12, 4: 12},
    (0x19, 5): {1: 0, 2: 0, 3: 12, 4: 0},
    (0x1B, 0): {1: 3, 2: 3, 3: 3, 4: 3},
    (0x1B, 2): {1: 3, 2: 3, 3: 3, 4: 3},
    (0x1B, 3): {1: 3, 2: 3, 3: 3, 4: 3},
    (0x1B, 6): {1: 3, 2: 3, 3: 3, 4: 3},
    (0x1B, 7): {1: 3, 2: 3, 3: 3, 4: 3},
    (0x3C, 0): {1: 0, 2: 3, 3: 3, 4: 3},
    (0x3C, 1): {1: 0, 2: 3, 3: 3, 4: 3},
    (0x3C, 2): {1: 0, 2: 3, 3: 3, 4: 3},
    (0x3C, 3): {1: 0, 2: 3, 3: 3, 4: 3},
    (0x3C, 4): {1: 0, 2: 3, 3: 3, 4: 3},
    (0x3C, 5): {1: 0, 2: 0, 3: 3, 4: 3},
    (0x3C, 7): {1: 0, 2: 27, 3: 27, 4: 27},
}

# Prefix/immediate categories
INTENDED_PREFIX_COUNTS: dict[str, int] = {
    "vpfxs": 32,
    "vpfxt": 32,
    "vpfxd": 48,
    "viim": 35,
    "vfim": 35,
}
# Canonical anchors are also in INDEPENDENT_LITERALS above; tests verify them
# via direct integer membership, not via table self-comparison.

# VFPU12 sub7 idx breakdown
INTENDED_VFPU12_IDX_COUNTS: dict[int, int] = {
    28: 72,
    29: 9,
}
INTENDED_VFPU12_WHICH_COUNTS: dict[int, int] = {
    0: 9, 1: 9, 2: 9, 3: 9, 4: 9, 5: 9, 6: 9, 7: 9,
}



# ---------------------------------------------------------------------------
# Authoritative VFPU encoding helpers
# ---------------------------------------------------------------------------
#
# The production decoder (src/rt/vfpu_interp.c vsize(), codegen.py vec_size())
# defines the exact bit layout:
#
#   31..26  op6        (6 bits)
#   25..23 sub        (3 bits)   -- for 0x18, 0x19, 0x1B, 0x3C
#   25..21 sub21      (5 bits)   -- for 0x34, 0x3C matrix1/rot
#   22..16 vt         (7 bits)
#   15     size[1]    (value 2 when set)
#   14..8  vs         (7 bits)
#   7      size[0]    (value 1 when set)
#   6..0   vd         (7 bits)
#
# Lane count: n = (((w >> 7) & 1) | ((w >> 14) & 2)) + 1
# ---------------------------------------------------------------------------

def _encode(op6: int, sub: int, vt: int, vs: int, vd: int, size: int) -> int:
    """Encode a VFPU compute instruction with authoritative layout."""
    word = (op6 & 0x3F) << 26
    word |= (sub & 0x7) << 23
    word |= (vt & 0x7F) << 16
    word |= ((size >> 1) & 1) << 15   # size[1] at bit 15
    word |= (vs & 0x7F) << 8
    word |= (size & 1) << 7            # size[0] at bit 7
    word |= (vd & 0x7F)
    return word & 0xFFFFFFFF


def _encode_vfpu4(sub21: int, vt: int, vs: int, vd: int, size: int) -> int:
    """Encode a VFPU4 (0x34) instruction with 5-bit sub21 at bits 25:21."""
    word = (0x34 << 26)
    word |= (sub21 & 0x1F) << 21
    word |= (vt & 0x7F) << 16
    word |= ((size >> 1) & 1) << 15
    word |= (vs & 0x7F) << 8
    word |= (size & 1) << 7
    word |= (vd & 0x7F)
    return word & 0xFFFFFFFF


def _encode_vfpu12(sub21: int, vt: int, vs: int, vd: int, size: int) -> int:
    """Encode a VFPU12 (0x3C) instruction with 5-bit sub21 at bits 25:21."""
    word = (0x3C << 26)
    word |= (sub21 & 0x1F) << 21
    word |= (vt & 0x7F) << 16
    word |= ((size >> 1) & 1) << 15
    word |= (vs & 0x7F) << 8
    word |= (size & 1) << 7
    word |= (vd & 0x7F)
    return word & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Supported encoding families
# ---------------------------------------------------------------------------

def _iter_vfpu0() -> Iterator[int]:
    """Major opcode 0x18 — VFPU0: vadd(0), vsub(1), vdiv(7)."""
    for sub in (0, 1, 7):
        for size in (0, 1, 2, 3):
            for vs in (0, 1, 32, 64):
                for vt in (0, 1, 4):
                    vd = (vs + 8) & 0x7F
                    yield _encode(0x18, sub, vt, vs, vd, size)


def _iter_vfpu1() -> Iterator[int]:
    """Major opcode 0x19 — VFPU1 binary ops:
       vmul(0), vdot(1), vscl(2), vhdp(4), vcrs(5, triple only)."""
    for sub in (0, 1, 2, 4):
        for size in (0, 1, 2, 3):
            for vs in (0, 1, 32, 64):
                for vt in (0, 1, 4):
                    vd = (vs + 8) & 0x7F
                    yield _encode(0x19, sub, vt, vs, vd, size)
    for vs in (0, 1, 32, 64):
        for vt in (0, 1, 4):
            vd = (vs + 8) & 0x7F
            yield _encode(0x19, 5, vt, vs, vd, 2)  # vcrs.t only


def _iter_vpfx() -> Iterator[int]:
    """Major opcode 0x37 — VPFXS/VPFXT/VPFXD (prefix state registers)."""
    swizzles = [0x00, 0xE4, 0xB1, 0x1B]
    abs_mask = [0x00, 0x0F]
    neg_mask = [0x00, 0x0F]
    cst_mask = [0x00, 0x01]

    for swz in swizzles:
        for ab in abs_mask:
            for neg in neg_mask:
                for cst in cst_mask:
                    desc = (neg << 16) | (ab << 12) | (cst << 8) | swz
                    yield (0xDC << 24) | (desc & 0xFFFFF)
                    yield (0xDD << 24) | (desc & 0xFFFFF)

    for mask in range(16):
        for sat in (0, 1, 2):
            dpfx = (sat << 4) | mask
            yield (0xDE << 24) | dpfx


def _iter_viim_vfim() -> Iterator[int]:
    """Immediate-form VFPU instructions: viim and vfim."""
    for vt in (0, 1, 32, 64, 127):
        for imm in (0, 1, 0x7F, 0x80, 0xFF, 0x40, 0x3F):
            yield ((0xDF << 24) | ((vt & 0x7F) << 16) | (imm & 0xFFFF)) & 0xFFFFFFFF
            yield ((0xDF << 24) | (1 << 23) | ((vt & 0x7F) << 16) | (imm & 0xFFFF)) & 0xFFFFFFFF


def _iter_vfpu3() -> Iterator[int]:
    """Major opcode 0x1B — VFPU3: vcmp(0), vmin(2), vmax(3), vcmovt(6), vcmovf(7)."""
    for sub in (0, 2, 3, 6, 7):
        for size in (0, 1, 2, 3):
            for vs in (0, 1, 8):
                vt = (vs + 2) & 0x7F
                vd = (vs + 4) & 0x7F
                yield _encode(0x1B, sub, vt, vs, vd, size)


def _iter_vfpu4() -> Iterator[int]:
    """Major opcode 0x34 — VFPU4: unary ops, conversions, vcst, vocp, vcmov."""
    sizes = (0, 1, 2, 3)
    regs = (0, 1, 8, 16, 32)

    # Unary ops (jump=0, optype in bits 20:16)
    unary_optypes = [0, 1, 2, 3, 4, 5, 6, 7,
                     16, 17, 18, 19, 20, 21, 22, 23,
                     24, 25, 26, 27, 28, 29, 30, 31]
    for optype in unary_optypes:
        for size in sizes:
            for vs in regs:
                vd = (vs + 4) & 0x7F
                yield _encode_vfpu4(0, optype, vs, vd, size)

    # vcst (jump=3)
    for cst_idx in (0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19):
        for size in sizes:
            for vs in regs:
                vd = (vs + 4) & 0x7F
                yield _encode_vfpu4(3, cst_idx, vs, vd, size)

    # vocp (jump=2, op9=4)
    for size in sizes:
        for vs in regs:
            vd = (vs + 4) & 0x7F
            yield _encode_vfpu4(2, 4, vs, vd, size)

    # vcmov (jump=0x15): tf in bit 19, imm3 in bits 18:16
    for tf in (0, 1):
        for imm3 in (0, 1, 2, 3, 4, 5, 6):
            val = (tf << 3) | imm3
            for size in sizes:
                for vs in regs:
                    vd = (vs + 4) & 0x7F
                    yield _encode_vfpu4(0x15, val, vs, vd, size)

    # Conversions: vs2i (jump=1, idx7=27)
    for size in sizes:
        for vs in regs:
            vd = (vs + 4) & 0x7F
            yield _encode_vfpu4(1, 27, vs, vd, size)

    # vi2uc/vi2c/vi2us/vi2s (jump=1, idx7=28-31)
    for idx7 in (28, 29, 30, 31):
        for size in sizes:
            for vs in regs:
                vd = (vs + 4) & 0x7F
                yield _encode_vfpu4(1, idx7, vs, vd, size)

    # vf2i (jump=16-19)
    for jump in (16, 17, 18, 19):
        for size in sizes:
            for vs in regs:
                vd = (vs + 4) & 0x7F
                for scale in (0, 1, 2, 3, 4, 5, 10, 15, 20):
                    yield _encode_vfpu4(jump, scale, vs, vd, size)

    # vi2f (jump=20)
    for size in sizes:
        for vs in regs:
            vd = (vs + 4) & 0x7F
            for scale in (0, 1, 2, 3, 4, 5, 10, 15, 20):
                yield _encode_vfpu4(20, scale, vs, vd, size)


def _iter_vfpu12() -> Iterator[int]:
    """Major opcode 0x3C — VFPU12: vmmul(0), vtfm(1,2,3), vmscl(4),
       vcrsp/vqmul(5), vmmov/vmidt/vmzero/vmone(7,idx=28), vrot(7,idx=29)."""
    matrix_sizes = (1, 2, 3)
    matrix_regs = (0, 32, 64)

    # vmmul: sub21=0
    for size in matrix_sizes:
        for vs in matrix_regs:
            vt = (vs + 4) & 0x7F
            vd = (vs + 8) & 0x7F
            yield _encode(0x3C, 0, vt, vs, vd, size)

    # vtfm: sub21=1,2,3 (ins=side-1)
    for ins in (1, 2, 3):
        for size in matrix_sizes:
            for vs in matrix_regs:
                vt = (vs + 4) & 0x7F
                vd = (vs + 8) & 0x7F
                yield _encode(0x3C, ins, vt, vs, vd, size)

    # vmscl: sub21=4
    for size in matrix_sizes:
        for vs in matrix_regs:
            vt = (vs + 4) & 0x7F
            vd = (vs + 8) & 0x7F
            yield _encode(0x3C, 4, vt, vs, vd, size)

    # vcrsp/vqmul: sub=5 at bits 25:23, sizes T(2) and Q(3) in 0..3 code
    # Corrected: use _encode (3-bit sub) not _encode_vfpu12 (5-bit), and distinct sizes
    for vs in matrix_regs:
        vt = (vs + 4) & 0x7F
        vd = (vs + 8) & 0x7F
        yield _encode(0x3C, 5, vt, vs, vd, 2)  # vcrsp.t (T)
        yield _encode(0x3C, 5, vt, vs, vd, 3)  # vqmul.q (Q)

    # vmmov/vmidt/vmzero/vmone: idx=28, which=0/3/6/7
    for which in (0, 3, 6, 7):
        for size in matrix_sizes:
            for vs in matrix_regs:
                vd = (vs + 8) & 0x7F
                yield _encode_vfpu12(28, which, vs, vd, size)

    # vrot: idx=29
    for size in matrix_sizes:
        for vs in matrix_regs:
            vd = (vs + 8) & 0x7F
            yield _encode_vfpu12(29, 0, vs, vd, size)

    # vmscl alias: idx=28, which=1,2,4,5
    for which in (1, 2, 4, 5):
        for size in matrix_sizes:
            for vs in matrix_regs:
                vd = (vs + 8) & 0x7F
                yield _encode_vfpu12(28, which, vs, vd, size)


# ---------------------------------------------------------------------------
# Self-comparison guard
# ---------------------------------------------------------------------------

def _check_corpus_nonempty(words: list[int]) -> None:
    """Verify the synthetic corpus is non-empty.

    Historical name _check_no_self_compare overstated what was checked; this
    guard only ensures the corpus is non-empty. Deeper production-vs-interpreter
    agreement is tested separately in the fuzzer, not here.
    """
    if not words:
        raise ValueError("Synthetic VFPU corpus is empty -- generation error")

# Backward-compatible alias (deprecated)
_check_no_self_compare = _check_corpus_nonempty


# ---------------------------------------------------------------------------
# Legacy / malformed encodings — NOT part of positive corpus
# ---------------------------------------------------------------------------

def _malformed_vcrsp_old_size_bit(word_correct: int) -> int:
    w = word_correct & ~((1 << 7) | (1 << 15))
    w |= (1 << 8)
    return w & 0xFFFFFFFF

def _malformed_vfim_old_shape(vd: int, imm: int) -> int:
    return ((0xDE << 24) | ((vd & 0x7F) << 16) | (imm & 0xFFFF)) & 0xFFFFFFFF

def _malformed_vfpu12_5bit_field_simple(size_code: int, vd: int, vs: int, vt: int) -> int:
    lo = size_code & 1
    hi = (size_code >> 1) & 1
    return (((0x3C & 0x3F) << 26) | ((5 & 0x1F) << 21) | ((vt & 0x7F) << 16) | ((vs & 0x7F) << 8) | (vd & 0x7F) | (lo << 7) | (hi << 15)) & 0xFFFFFFFF

# Use the simple one as alias
_malformed_vfpu12_5bit_field = _malformed_vfpu12_5bit_field_simple

# Historical malformed VFPU12 5-bit subfield shapes (8 words).  The old buggy
# encoder used 5 << 21 (5-bit field at 25:21) instead of sub << 23 (3-bit at
# 25:23).  Those 8 words are exactly the set produced by the old helper with
# size T/Q (2,3), vd=8, vs 0/32, vt input 0/4.  After the overlapping bit21
# contamination, they decode as sub3==1 (vtfm) with decoded vt 32/36, but are
# historically malformed.  Legitimate vtfm words with vt=36 (e.g., 0xf0a420a8)
# also appear as sub5==5 due to vt bit5 overlap, so a broad
# "op6==0x3C and sub5==5 and sub3!=5" predicate is overbroad and must NOT be
# used.  We match the exact historical set only.
# Caveat: historical-shape classification is not equivalent to production
# rejection — some historical shapes legally decode as another operation today.
_HISTORICAL_MALFORMED_VFPU12_5BIT_WORDS: frozenset[int] = frozenset(
    _malformed_vfpu12_5bit_field_simple(size_code, 8, vs, vt)
    for size_code in (2, 3)
    for vs in (0, 32)
    for vt in (0, 4)
)

def generate_malformed_corpus() -> list[int]:
    out: list[int] = []
    # Need _encode for correct word; size codes 2=T,3=Q
    correct = (0x3C << 26) | (5 << 23) | (0 << 16) | (0 << 8) | 0 | (0 << 7) | (1 << 15)  # vcrsp.t with vd=vs=vt=0, size 2 (T)
    # Use _encode to get correct for malformed test
    from typing import cast
    # Create via _encode
    correct2 = _encode(0x3C, 5, 0, 0, 0, 2)
    correct3 = _encode(0x3C, 5, 0, 0, 0, 3)
    out.append(_malformed_vcrsp_old_size_bit(correct2))
    out.append(_malformed_vcrsp_old_size_bit(correct3))
    for vd, imm in [(0,0x3F),(32,0x80),(64,0xFF)]:
        out.append(_malformed_vfim_old_shape(vd, imm))
    for size_code in (2,3):
        for vs in (0,32):
            for vt in (0,4):
                out.append(_malformed_vfpu12_5bit_field(size_code, 8, vs, vt))
    out.append(0x0000003F)
    out.append((0x01 << 26) | 0x000000)
    out.append((0x18 << 26) | (2 << 23) | 0x0000)
    out.append((0x3C << 26) | (6 << 23) | 0x8080)
    out = sorted(set(out))
    pos = set(generate_synthetic_corpus())
    overlap = [hex(w) for w in out if w in pos]
    if overlap:
        raise AssertionError(f"malformed overlaps positive: {overlap[:5]}")
    return out

def classify_word_production(word: int) -> str:
    op6 = (word >> 26) & 0x3F
    sub3 = (word >> 23) & 7
    # Historical VFPU12 5-bit malformed shapes: match the exact 8-word set,
    # not the contaminated sub5 heuristic (which falsely flags legitimate vt=36
    # vtfm words like 0xf0a420a8).  See _HISTORICAL_MALFORMED_VFPU12_5BIT_WORDS.
    if word in _HISTORICAL_MALFORMED_VFPU12_5BIT_WORDS:
        return "malformed: old 5-bit field for 0x3C/sub5"
    if (word >> 24) == 0xDE and (word & 0xFFFF) in (0x3F,0x80,0xFF):
        if word in set(generate_malformed_corpus()):
            return "malformed: old vfim-family shape"
    try:
        import codegen
        body, _, _ = codegen.vfpu_effect(0x08900000, word)
        if "sr_vfpu_interp" in body:
            return "interpreter_fallback"
        if op6 == 0x3C and sub3 == 5:
            n = _decode_size_independent(word)
            if n == 3:
                return "positive: vcrsp.t"
            if n == 4:
                return "positive: vqmul.q"
            return f"positive: 0x3C/sub5 size {n}"
        return f"positive: op6 0x{op6:02x} sub {sub3}"
    except Exception as e:
        return f"emitter_unsupported: {e}"

# ---------------------------------------------------------------------------
# Raw iterator census (for F3 collision/uniqueness testing)
# ---------------------------------------------------------------------------

def iter_synthetic_corpus_raw() -> Iterator[int]:
    """Yield every intended positive word before deduplication/sorting.

    This is the raw census: each _iter_* is yielded in order without set
    deduplication.  Tests assert raw == unique == 2742, so a mutation where
    two independent generator paths collapse onto the same word is caught as an
    explicit collision, not merely as a final total drop.  This helper must
    NOT introduce production filtering; it is purely the concatenation of the
    spec-derived iterators.
    """
    yield from _iter_vfpu0()
    yield from _iter_vfpu1()
    yield from _iter_vpfx()
    yield from _iter_viim_vfim()
    yield from _iter_vfpu3()
    yield from _iter_vfpu4()
    yield from _iter_vfpu12()

# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def generate_synthetic_corpus() -> list[int]:
    """Return a sorted, deduplicated list of synthetic VFPU instruction words."""
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
    _check_corpus_nonempty(corpus)
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
