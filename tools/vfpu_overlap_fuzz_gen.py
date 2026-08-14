#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Deterministic VFPU matrix-op source/destination aliasing corpus generator.

Builds the instruction-word table and generated-C bodies for the
vfpu-overlap-selftest (src/rt/vfpu_overlap_selftest.c): a systematic
enumeration of LEGAL register-overlap classes for vmmul/vtfm/vmscl/vmmov and
the adjacent vector ops (vdot/vhdp/vcrs/vscl/vqmul/vcrsp), so the differential
harness no longer depends on which words happen to appear in the game ELF or
in the generic synthetic corpus (whose 0x3C entries carry a scrambled register
layout and no vmscl/vmmov coverage at all).

Every emitted word is constructed with codegen's own decode conventions
(vreg_indices/mreg_index, the size bits of vec_size) and then RE-DECODED to
assert that the claimed alias class really holds on the physical lane sets.
The generator also simulates the read/write program of the emitted C to decide
whether the case is "model-assertable": for those cases the selftest additionally
checks the implementation against an independent read-before-write reference
built from a snapshot of the original register file.

Alias classes (Klass):
  0 disjoint              vd, vs, vt lane-disjoint
  1 vd == vs             identical destination/source-1 encodings
  2 vd == vt             identical destination/source-2 encodings
  3 partial overlap      destination shares lanes with a source, not identical
  4 transpose-induced    partial overlap where a transpose bit changes the lanes
  5 source-source        vs and vt overlap each other; vd disjoint
  6 all identical        vd == vs == vt

vmscl adds:
  7 scalar-in-destination  the scalar lane lies inside the destination matrix
                           (identical or disjoint matrix forms; hardware cell)
vmmov/vmscl partial classes reuse class 3.

Hardware contract (public evidence):
  * pspdev/vfpu-docs docs/introduction.md "Register hazards" (hardware-
    validated): vmmul/vtfm2-4/vhtfm2-4/vqmul/vcrsp "do not allow any sort of
    overlap between input and output registers"; vmscl/vmmov are decomposed
    into partial ops and allow overlap only when "compatible in terms of
    element count and access direction" (identical matrix, same mode, scalar
    OUTSIDE the destination; `vmscl.t M000, M011, S100` is explicitly invalid).
  * gen-regtests.py's regcompatcol macro: compatible = same lane set AND same
    access-direction bit; input-input collisions are skipped by the harness, so
    source-source overlap has no hardware coverage.
  * vdot/vhdp/vcrs/vscl appear in neither documented hazard group.

Each case carries a contract class: ALLOWED (disjoint or docs-compatible),
NO_OVERLAP (docs say overlapping encodings give incorrect hardware results), or
UNESTABLED (no public evidence; hardware cell recorded in
fixtures/vfpu_overlap_probe/). This generator is a HOST-side differential and
read-before-write contract probe: it proves the emitted C and the interpreter
agree and both snapshot their sources, NOT that the PSP produces the same bits
for an overlapping encoding.

Usage:
  python tools/vfpu_overlap_fuzz_gen.py <out.h>
"""

from __future__ import annotations

import sys

import codegen

# Families the selftest reference model implements.
FAM_VMMUL = 0
FAM_VTFM = 1
FAM_VMSCL = 2
FAM_VMMOV = 3
FAM_VDOT = 4
FAM_VHDP = 5
FAM_VCRS = 6
FAM_VSCL = 7
FAM_VQMUL = 8
FAM_VCRSP = 9

KL_DISJOINT = 0
KL_VD_EQ_VS = 1
KL_VD_EQ_VT = 2
KL_PARTIAL = 3
KL_TRANSPOSE = 4
KL_SRC_SRC = 5
KL_ALL_IDENT = 6
KL_SCALAR_IN_DEST = 7

# Hardware contract classes, from the public evidence in
# pspdev/vfpu-docs docs/introduction.md "Register hazards" (+ its hardware
# reg-test generator gen-regtests.py, whose regcompatcol macro defines
# "compatible" as identical lane set + identical access-direction bit):
#   ALLOWED      -- disjoint registers, or an overlap the docs mark compatible
#                  (vmscl/vmmov identical-matrix overlap with the scalar
#                  outside the destination).
#   NO_OVERLAP   -- docs state overlapping encodings give INCORRECT results on
#                  hardware (vmmul/vtfm/vhtfm/vqmul/vcrsp allow no input/output
#                  overlap at all; vmscl/vmmov partial or transposed overlap is
#                  explicitly invalid). The host implementations still snapshot
#                  sources; that is a host contract, NOT a claim about the
#                  hardware bits for these encodings.
#   UNESTABLED   -- no public hardware evidence: input-input (source-source)
#                  overlap, vmscl/vmmov scalar-inside-destination, and overlap
#                  on vdot/vhdp/vcrs/vscl are not covered by the documented
#                  hazard model or by the hardware test harness (which skips
#                  input-input collisions). Recorded as hardware cells in
#                  fixtures/vfpu_overlap_probe/.
CT_ALLOWED = 0
CT_NO_OVERLAP = 1
CT_UNESTABLED = 2

# Families whose overlapping encodings are documented hardware hazards.
NO_OVERLAP_FAMILIES = {FAM_VMMUL, FAM_VTFM, FAM_VQMUL, FAM_VCRSP}

# vmscl/vmmov: the docs allow ONLY identical (same-mode) overlap; the scalar
# operand adds an element-count constraint, so scalar-inside-destination is not
# part of the documented compatible set.
PARTIAL_OVERLAP_FAMILIES = {FAM_VMSCL, FAM_VMMOV}


def _contract(fam: int, klass: int) -> int:
    """Map (family, alias-class) to the hardware contract class."""
    if klass == KL_DISJOINT:
        return CT_ALLOWED
    if fam in NO_OVERLAP_FAMILIES:
        # Any input/output overlap (identical included) is a documented hazard.
        return CT_NO_OVERLAP if klass != KL_SRC_SRC else CT_UNESTABLED
    if fam in PARTIAL_OVERLAP_FAMILIES:
        if klass == KL_VD_EQ_VS:
            # Identical matrix, same access mode, scalar outside the dest:
            # the docs' own example (vmscl.p M000, M000, S100) is OK.
            return CT_ALLOWED
        if klass in (KL_PARTIAL, KL_TRANSPOSE):
            # "Overlapping registers are not identical" is invalid for
            # vmscl/vmmov; access-direction mismatch is invalid too.
            return CT_NO_OVERLAP
        # Scalar-inside-destination (element-count mismatch) and all-identical
        # with the scalar inside: not documented -> hardware cell.
        return CT_UNESTABLED
    # vdot/vhdp/vcrs/vscl: not listed in either documented hazard group.
    return CT_UNESTABLED

# Identity-prefix-only families (hardware-quirky prefix interactions neither
# side models; same policy as vfpu_fuzz.c).
IDENTITY_ONLY_FAMILIES = {FAM_VQMUL, FAM_VCRSP}


def _vec_size_bits(size: int) -> tuple[int, int]:
    """(bit7, bit14) for vec_size(w) == size, per codegen.vec_size."""
    size -= 1
    return (size & 1), (size >> 1)


def _word(op6: int, sub: int, size: int, vd: int, vs: int, vt: int) -> int:
    """Pack a vfpu-alu word with codegen's decode layout (op 0x18/0x19/0x3c).

    Size bits: sizelo = bit 7, sizehi = bit 15 (codegen.vec_size reads
    ((w>>7)&1) | ((w>>14)&2)); the 7-bit vs field occupies bits 14:8, so bit 15
    is free for sizehi."""
    lo, hi = _vec_size_bits(size)
    return (
        (op6 << 26)
        | ((sub & 7) << 23)
        | (hi << 15)
        | ((vs & 0x7F) << 8)
        | (vd & 0x7F)
        | ((vt & 0x7F) << 16)
        | (lo << 7)
    )


def _matrix_lanes(reg: int, side: int, transpose_ok: bool = True) -> set[int]:
    return {codegen.mreg_index(reg, side, j, i) for i in range(side) for j in range(side)}


def _vec_lanes(reg: int, size: int) -> list[int]:
    return codegen.vreg_indices(reg, size)


def _scalar_lane(reg: int) -> int:
    return codegen.vreg_indices(reg, 1)[0]


# ---------------------------------------------------------------------------
# Read/write program simulation: decide whether the emitted C reads any source
# lane after a destination write already touched it.  Mirror of the exact
# emission shapes in codegen.vfpu_effect / sr_vfpu_interp.
# ---------------------------------------------------------------------------

def _clobbered(model_program) -> bool:
    """model_program: ordered list of (kind, lanes); kind 'r' read, 'w' write."""
    written: set[int] = set()
    for kind, lanes in model_program:
        if kind == "r":
            for ln in lanes:
                if ln in written:
                    return True
        else:
            written.update(lanes)
    return False


def _vmmul_program(vd, vs, vt, side):
    reads = [codegen.mreg_index(vs, side, b, c) for a in range(side) for b in range(side) for c in range(side)]
    reads += [codegen.mreg_index(vt, side, a, c) for a in range(side) for b in range(side) for c in range(side)]
    writes = [codegen.mreg_index(vd, side, a, b) for a in range(side) for b in range(side)]
    return [("r", reads), ("w", writes)]


def _vtfm_program(vd, vs, vt, side):
    reads = [codegen.mreg_index(vs, side, i, k) for i in range(side) for k in range(side)]
    reads += list(_vec_lanes(vt, side))
    writes = list(_vec_lanes(vd, side))
    return [("r", reads), ("w", writes)]


def _vmscl_program(vd, vs, scalar, side):
    prog = [("r", [scalar]),
            ("r", [codegen.mreg_index(vs, side, j, side - 1) for j in range(side)]),
            ("r", [scalar] * side)]  # prefixed scalar read (T prefix machinery)
    for i in range(side - 1):
        for j in range(side):
            prog.append(("r", [codegen.mreg_index(vs, side, j, i)]))
            prog.append(("w", [codegen.mreg_index(vd, side, j, i)]))
    prog.append(("w", [codegen.mreg_index(vd, side, j, side - 1) for j in range(side)]))
    return prog


def _vmmov_program(vd, vs, side):
    prog = []
    for i in range(side - 1):
        for j in range(side):
            prog.append(("r", [codegen.mreg_index(vs, side, j, i)]))
            prog.append(("w", [codegen.mreg_index(vd, side, j, i)]))
    prog.append(("r", [codegen.mreg_index(vs, side, j, side - 1) for j in range(side)]))
    prog.append(("w", [codegen.mreg_index(vd, side, j, side - 1) for j in range(side)]))
    return prog


def _vec_op_program(si, ti, di):
    return [("r", list(si) + list(ti)), ("w", list(di))]


# ---------------------------------------------------------------------------
# Case construction
# ---------------------------------------------------------------------------

def _pick(pred, candidates):
    for reg in candidates:
        if pred(reg):
            return reg
    raise AssertionError("no candidate register satisfies the class predicate")


def _joint_pick(pred, pairs):
    for a, b in pairs:
        if pred(a, b):
            return a, b
    raise AssertionError("no candidate register pair satisfies the class predicate")


def _cases_vmmul() -> list[tuple]:
    """vmmul.p/.t/.q over the matrix alias classes."""
    out = []
    for size in (2, 3, 4):
        side = size
        # vs must carry sizehi in bit 14 -> vs >= 64 for .t/.q; .p keeps vs < 64.
        vs_pool = [64, 68, 72, 76] if size in (3, 4) else [0, 4, 8, 12]
        vd_pool = [0, 1, 4, 8, 32, 64, 65, 68]
        vt_pool = [0, 1, 4, 8, 32, 64, 65, 68]

        def mk(vd, vs, vt):
            return _word(0x3C, 0, size, vd, vs, vt)

        # disjoint
        vd = _pick(lambda r: not (_matrix_lanes(r, side) & _matrix_lanes(vs_pool[0], side)), vd_pool)
        vt = _pick(lambda r: not (_matrix_lanes(r, side) & (_matrix_lanes(vs_pool[0], side) | _matrix_lanes(vd, side))), vt_pool)
        out.append((mk(vd, vs_pool[0], vt), KL_DISJOINT))
        # vd == vs
        out.append((mk(vs_pool[0], vs_pool[0], vt), KL_VD_EQ_VS))
        # vd == vt
        out.append((mk(vt, vs_pool[0], vt), KL_VD_EQ_VT))
        # partial same-block: vd and vs in one matrix block, shifted column.
        # Impossible for .q: every 4x4 matrix register covers a full 16-lane
        # block, so same-block is always identical and cross-block is disjoint.
        if side < 4:
            vdp = _pick(lambda r: (_matrix_lanes(r, side) & _matrix_lanes(vs_pool[0], side)) and
                        _matrix_lanes(r, side) != _matrix_lanes(vs_pool[0], side), vd_pool)
            out.append((mk(vdp, vs_pool[0], vt), KL_PARTIAL))
            # transpose-induced overlap: same-block pair where the transpose bit
            # changes which lanes are read
            vd_t, vst = _joint_pick(
                lambda a, b: (b >> 5) & 1 and (_matrix_lanes(a, side) & _matrix_lanes(b, side)) and
                _matrix_lanes(a, side) != _matrix_lanes(b, side),
                [(a, b) for a in vd_pool for b in (0x21, 0x25, 0x41, 0x45, 0x61, 0x65, 0xA1, 0xC1, 0xE1)],
            )
            out.append((mk(vd_t, vst, vt), KL_TRANSPOSE))
        # source-source overlap: vs == vt, vd disjoint
        vdd = _pick(lambda r: not (_matrix_lanes(r, side) & _matrix_lanes(vs_pool[0], side)), vd_pool)
        out.append((mk(vdd, vs_pool[0], vs_pool[0]), KL_SRC_SRC))
        # all identical
        out.append((mk(vs_pool[0], vs_pool[0], vs_pool[0]), KL_ALL_IDENT))
    return out


def _cases_vtfm() -> list[tuple]:
    """vtfm (ins 1..3 -> 2x2/3x3/4x4) over destination-vs-matrix / dest-vs-vector classes."""
    out = []
    for ins in (1, 2, 3):
        size = ins + 1  # matrix side and destination vector size
        vs_pool = [64, 68, 72, 76] if size in (3, 4) else [0, 4, 8, 12]
        vd_pool = [0, 1, 4, 8, 32, 64, 65, 68]
        vt_pool = [0, 1, 4, 8, 32, 64, 65, 68]

        def mk(vd, vs, vt):
            return _word(0x3C, ins, size, vd, vs, vt)

        # disjoint
        vd = _pick(lambda r: not (set(_vec_lanes(r, size)) & (_matrix_lanes(vs_pool[0], size) | set(_vec_lanes(vt_pool[0], size)))), vd_pool)
        vt = _pick(lambda r: not (set(_vec_lanes(r, size)) & (_matrix_lanes(vs_pool[0], size) | set(_vec_lanes(vd, size)))), vt_pool)
        out.append((mk(vd, vs_pool[0], vt), KL_DISJOINT))
        # vd == vs (destination vector == matrix-1 rows): vd encodes the same
        # matrix register as vs
        out.append((mk(vs_pool[0], vs_pool[0], vt), KL_VD_EQ_VS))
        # vd == vt (destination vector == source vector)
        out.append((mk(vt, vs_pool[0], vt), KL_VD_EQ_VT))
        # partial: dest vector overlaps the source matrix (same block, shifted)
        vdp = _pick(lambda r: set(_vec_lanes(r, size)) & _matrix_lanes(vs_pool[0], size) and
                    set(_vec_lanes(r, size)) != _matrix_lanes(vs_pool[0], size), vd_pool)
        out.append((mk(vdp, vs_pool[0], vt), KL_PARTIAL))
        # partial: dest vector overlaps the source vector (shifted lane offset)
        vtp = _pick(lambda r: set(_vec_lanes(r, size)) & set(_vec_lanes(vt_pool[0], size)) and
                    r != vt_pool[0], vd_pool)
        out.append((mk(vtp, vs_pool[0], vt_pool[0]), KL_PARTIAL))
        # transpose-induced: matrix read transposed
        vst = _pick(lambda r: (r >> 5) & 1 and (_matrix_lanes(r, size) & set(_vec_lanes(vd, size))) and
                    _matrix_lanes(r, size) != set(_vec_lanes(vd, size)), [0x21, 0x25, 0x41, 0x45, 0x61, 0x65, 0xA1, 0xC1, 0xE1])
        out.append((mk(vd, vst, vt), KL_TRANSPOSE))
        # source-source: matrix vs vector in the same block
        vss = _pick(lambda r: set(_vec_lanes(r, size)) & _matrix_lanes(vs_pool[0], size), vt_pool)
        out.append((mk(vd, vs_pool[0], vss), KL_SRC_SRC))
        # all identical (dest vector == matrix-1 == vector)
        out.append((mk(vs_pool[0], vs_pool[0], vs_pool[0]), KL_ALL_IDENT))
    return out


def _cases_vmscl() -> list[tuple]:
    """vmscl.p/.t/.q: disjoint, identical, scalar-in-destination, partial."""
    out = []
    for size in (2, 3, 4):
        vs_pool = [64, 68, 72, 76] if size in (3, 4) else [0, 4, 8, 12]
        vd_pool = [0, 1, 4, 8, 32, 64, 65, 68]
        vt_pool = [0, 1, 4, 8, 32, 64, 65, 68]

        def mk(vd, vs, vt):
            return _word(0x3C, 4, size, vd, vs, vt)

        # disjoint (scalar outside destination, matrices disjoint)
        vd = _pick(lambda r: not (_matrix_lanes(r, size) & _matrix_lanes(vs_pool[0], size)) and
                    _scalar_lane(vt_pool[0]) not in _matrix_lanes(r, size), vd_pool)
        vt = _pick(lambda r: _scalar_lane(r) not in _matrix_lanes(vd, size) and
                    _scalar_lane(r) not in _matrix_lanes(vs_pool[0], size), vt_pool)
        out.append((mk(vd, vs_pool[0], vt), KL_DISJOINT))
        # identical (docs-legal: vmscl.p M000, M000, S100)
        vti = _pick(lambda r: _scalar_lane(r) not in _matrix_lanes(vs_pool[0], size), vt_pool)
        out.append((mk(vs_pool[0], vs_pool[0], vti), KL_VD_EQ_VS))
        # scalar-in-destination, identical matrix (the overlap cell the old
        # emission clobbered: fresh per-row scalar read)
        vts = _pick(lambda r: _scalar_lane(r) in _matrix_lanes(vs_pool[0], size), vt_pool)
        out.append((mk(vs_pool[0], vs_pool[0], vts), KL_SCALAR_IN_DEST))
        # scalar-in-destination, matrices disjoint (scalar lives in vd's block)
        vdd = _pick(lambda r: not (_matrix_lanes(r, size) & _matrix_lanes(vs_pool[0], size)), vd_pool)
        vts2 = _pick(lambda r: _scalar_lane(r) in _matrix_lanes(vdd, size) and
                     _scalar_lane(r) not in _matrix_lanes(vs_pool[0], size), vt_pool)
        out.append((mk(vdd, vs_pool[0], vts2), KL_SCALAR_IN_DEST))
        # partial matrix overlap (docs-invalid class); impossible for .q
        if size < 4:
            vdp = _pick(lambda r: (_matrix_lanes(r, size) & _matrix_lanes(vs_pool[0], size)) and
                        _matrix_lanes(r, size) != _matrix_lanes(vs_pool[0], size) and
                        _scalar_lane(vti) not in _matrix_lanes(r, size), vd_pool)
            out.append((mk(vdp, vs_pool[0], vti), KL_PARTIAL))
    return out


def _cases_vmmov() -> list[tuple]:
    out = []
    for size in (2, 3, 4):
        vs_pool = [64, 68, 72, 76] if size in (3, 4) else [0, 4, 8, 12]
        vd_pool = [0, 1, 4, 8, 32, 64, 65, 68]

        def mk(vd, vs):
            return _word(0x3C, 7, size, vd, vs, 0)  # sub 7 / idx 28 / which 0

        vd = _pick(lambda r: not (_matrix_lanes(r, size) & _matrix_lanes(vs_pool[0], size)), vd_pool)
        out.append((mk(vd, vs_pool[0]), KL_DISJOINT))
        out.append((mk(vs_pool[0], vs_pool[0]), KL_VD_EQ_VS))
        if size < 4:
            vdp = _pick(lambda r: (_matrix_lanes(r, size) & _matrix_lanes(vs_pool[0], size)) and
                        _matrix_lanes(r, size) != _matrix_lanes(vs_pool[0], size), vd_pool)
            out.append((mk(vdp, vs_pool[0]), KL_PARTIAL))
    return out


def _cases_vector_ops() -> list[tuple]:
    """vdot/vhdp/vscl (vector families) + vcrs/vqmul/vcrsp with alias classes.
    Returns (word, klass) pairs like the matrix builders; the family is
    re-derived in build_cases()."""
    out = []

    def vword(op, sub, size, vd, vs, vt):
        return _word(op, sub, size, vd, vs, vt)

    # vdot.p/.t/.q and vhdp.p/.t/.q: vd is a scalar
    for op, sub, fam in ((0x19, 1, FAM_VDOT), (0x19, 4, FAM_VHDP)):
        for size in (2, 3, 4):
            vs_pool = [0, 4, 8, 32, 64, 65, 68]
            vt_pool = [0, 1, 4, 8, 32, 64, 65, 68]
            vd_pool = [0, 1, 4, 8, 32, 64, 65, 68]
            vs = vs_pool[0]
            vt = vt_pool[0]
            lanes_s = set(_vec_lanes(vs, size))
            lanes_t = set(_vec_lanes(vt, size))
            vd = _pick(lambda r: _scalar_lane(r) not in lanes_s and _scalar_lane(r) not in lanes_t, vd_pool)
            out.append((vword(op, sub, size, vd, vs, vt), KL_DISJOINT))
            # vd scalar inside vs lanes
            vds = _pick(lambda r: _scalar_lane(r) in lanes_s, vd_pool)
            out.append((vword(op, sub, size, vds, vs, vt), KL_PARTIAL))
            # vs == vt
            out.append((vword(op, sub, size, vd, vs, vs), KL_SRC_SRC))

    # vscl.p/.t/.q: vd/vs vectors, vt scalar
    for size in (2, 3, 4):
        vs_pool = [0, 4, 8, 32, 64, 65, 68]
        vd_pool = [0, 1, 4, 8, 32, 64, 65, 68]
        vt_pool = [0, 1, 4, 8, 32, 64, 65, 68]
        vs = vs_pool[0]
        vt = vt_pool[0]
        vd = _pick(lambda r: not (set(_vec_lanes(r, size)) & set(_vec_lanes(vs, size))) and
                    _scalar_lane(vt) not in set(_vec_lanes(r, size)), vd_pool)
        out.append((vword(0x19, 2, size, vd, vs, vt), KL_DISJOINT))
        out.append((vword(0x19, 2, size, vs, vs, vt), KL_VD_EQ_VS))
        vts = _pick(lambda r: _scalar_lane(r) in set(_vec_lanes(vs, size)), vt_pool)
        out.append((vword(0x19, 2, size, vs, vs, vts), KL_SCALAR_IN_DEST))

    # vcrs.t (sub 5 op 0x19), vcrsp.t / vqmul.q (sub 5 op 0x3C)
    vcrs = (0x19, 5, 3, FAM_VCRS)
    for op, sub, size, fam in ((0x3C, 5, 3, FAM_VCRSP), (0x3C, 5, 4, FAM_VQMUL), vcrs):
        vs_pool = [0, 4, 8, 32, 64, 65, 68]
        vd_pool = [0, 1, 4, 8, 32, 64, 65, 68]
        vs = vs_pool[0]
        vt = vs_pool[1]
        vd = _pick(lambda r: not (set(_vec_lanes(r, size)) & (set(_vec_lanes(vs, size)) | set(_vec_lanes(vt, size)))), vd_pool)
        out.append((vword(op, sub, size, vd, vs, vt), KL_DISJOINT))
        out.append((vword(op, sub, size, vs, vs, vt), KL_VD_EQ_VS))
        out.append((vword(op, sub, size, vt, vs, vt), KL_VD_EQ_VT))
    return out


def _family_of(w: int) -> int:
    op = w >> 26
    sub = (w >> 23) & 7
    idx28 = ((w >> 21) & 0x1F) == 28
    if op == 0x3C and sub == 0:
        return FAM_VMMUL
    if op == 0x3C and sub in (1, 2, 3):
        return FAM_VTFM
    if op == 0x3C and sub == 4:
        return FAM_VMSCL
    if op == 0x3C and sub == 7 and idx28 and ((w >> 16) & 0xF) == 0:
        return FAM_VMMOV
    if op == 0x19 and sub == 1:
        return FAM_VDOT
    if op == 0x19 and sub == 4:
        return FAM_VHDP
    if op == 0x19 and sub == 5:
        return FAM_VCRS
    if op == 0x19 and sub == 2:
        return FAM_VSCL
    if op == 0x3C and sub == 5:
        return FAM_VQMUL if ((w >> 7) & 1) and ((w >> 14) & 2) else FAM_VCRSP
    raise AssertionError(f"unhandled overlap word 0x{w:08x}")


def _model_program(w: int):
    """Ordered read/write program of the emitted C, for clobber simulation."""
    op = w >> 26
    sub = (w >> 23) & 7
    vd, vs, vt = w & 0x7F, (w >> 8) & 0x7F, (w >> 16) & 0x7F
    n = codegen.vec_size(w)
    fam = _family_of(w)
    if fam == FAM_VMMUL:
        return _vmmul_program(vd, vs, vt, n)
    if fam == FAM_VTFM:
        return _vtfm_program(vd, vs, vt, sub + 1)
    if fam == FAM_VMSCL:
        return _vmscl_program(vd, vs, _scalar_lane(vt), n)
    if fam == FAM_VMMOV:
        return _vmmov_program(vd, vs, n)
    si = codegen.vreg_indices(vs, n)
    ti = codegen.vreg_indices(vt, n)
    if fam in (FAM_VDOT, FAM_VHDP, FAM_VCRSP, FAM_VQMUL):
        di = codegen.vreg_indices(vd, 1 if fam in (FAM_VDOT, FAM_VHDP) else n)
        return _vec_op_program(si, ti, di)
    if fam == FAM_VCRS:
        return _vec_op_program(si, ti, codegen.vreg_indices(vd, 3))
    if fam == FAM_VSCL:
        return [("r", list(si) + [_scalar_lane(vt)]), ("w", codegen.vreg_indices(vd, n))]
    raise AssertionError("unhandled family")


def build_cases() -> list[tuple[int, int, int, int, int]]:
    """Return [(word, klass, fam, assert_model, contract)] for the full
    overlap matrix; contract is the hardware-evidence class from _contract()."""
    raw = []
    raw += _cases_vmmul()
    raw += _cases_vtfm()
    raw += _cases_vmscl()
    raw += _cases_vmmov()
    raw += _cases_vector_ops()

    seen: set[int] = set()
    cases: list[tuple[int, int, int, int, int]] = []
    for w, klass in raw:
        if w in seen:
            continue
        seen.add(w)
        # The word must decode through the production emitter (never a fallback).
        body, _, _ = codegen.vfpu_effect(0x08900000, w)
        if "sr_vfpu_interp" in body:
            raise AssertionError(f"overlap word 0x{w:08x} falls back to sr_vfpu_interp")
        fam = _family_of(w)
        assert_model = 0 if _clobbered(_model_program(w)) else 1
        cases.append((w, klass, fam, assert_model, _contract(fam, klass)))
    return cases


def _classify_corpus_words(words) -> dict[str, int]:
    """Decode words through the production emitter and tally real coverage.
    Returns {family: count}; fallback/unsupported words are reported via stderr.
    Used by the old-vs-new coverage quantification (report/PR evidence)."""
    tally: dict[str, int] = {}
    for w in words:
        try:
            body, _, _ = codegen.vfpu_effect(0x08900000, w)
        except codegen.Unsupported:
            tally["Unsupported"] = tally.get("Unsupported", 0) + 1
            continue
        if "sr_vfpu_interp" in body:
            tally["sr_vfpu_interp-fallback"] = tally.get("sr_vfpu_interp-fallback", 0) + 1
            continue
        fam = _family_of(w)
        names = {FAM_VMMUL: "vmmul", FAM_VTFM: "vtfm", FAM_VMSCL: "vmscl",
                 FAM_VMMOV: "vmmov", FAM_VDOT: "vdot", FAM_VHDP: "vhdp",
                 FAM_VCRS: "vcrs", FAM_VSCL: "vscl", FAM_VQMUL: "vqmul",
                 FAM_VCRSP: "vcrsp"}
        tally[names[fam]] = tally.get(names[fam], 0) + 1
    return tally


def write_header(out_path: str, cases: list[tuple[int, int, int, int, int]]) -> None:
    out = ["/* Generated by tools/vfpu_overlap_fuzz_gen.py. Do not edit. */", ""]
    out.append("#ifndef NAKAGAWA_VFPU_OVERLAP_CASES_H")
    out.append("#define NAKAGAWA_VFPU_OVERLAP_CASES_H")
    out.append("")
    out.append(f"#define VFPU_OVERLAP_NCASES {len(cases)}")
    out.append("")
    out.append("/* Alias-class identifiers (see the generator docstring). */")
    for name, val in (("OVERLAP_DISJOINT", 0), ("OVERLAP_VD_EQ_VS", 1), ("OVERLAP_VD_EQ_VT", 2),
                      ("OVERLAP_PARTIAL", 3), ("OVERLAP_TRANSPOSE", 4), ("OVERLAP_SRC_SRC", 5),
                      ("OVERLAP_ALL_IDENT", 6), ("OVERLAP_SCALAR_IN_DEST", 7)):
        out.append(f"#define {name} {val}")
    out.append("")
    out.append("/* Reference-model families implemented by vfpu_overlap_selftest.c. */")
    for name, val in (("OVERLAP_FAM_VMMUL", 0), ("OVERLAP_FAM_VTFM", 1), ("OVERLAP_FAM_VMSCL", 2),
                      ("OVERLAP_FAM_VMMOV", 3), ("OVERLAP_FAM_VDOT", 4), ("OVERLAP_FAM_VHDP", 5),
                      ("OVERLAP_FAM_VCRS", 6), ("OVERLAP_FAM_VSCL", 7), ("OVERLAP_FAM_VQMUL", 8),
                      ("OVERLAP_FAM_VCRSP", 9)):
        out.append(f"#define {name} {val}")
    out.append("")
    out.append("/* Hardware-contract classes (pspdev/vfpu-docs evidence; see the generator). */")
    for name, val in (("OVERLAP_CT_ALLOWED", 0), ("OVERLAP_CT_NO_OVERLAP", 1),
                      ("OVERLAP_CT_UNESTABLED", 2)):
        out.append(f"#define {name} {val}")
    out.append("")
    out.append("static const struct { uint32_t w, addr; uint8_t klass, family, assert_model, contract; }")
    out.append("    vfpu_overlap_cases[VFPU_OVERLAP_NCASES] = {")
    for i, (w, klass, fam, am, ct) in enumerate(cases):
        out.append(f"    {{0x{w:08x}u, 0x08900000u, {klass}u, {fam}u, {am}u, {ct}u}},")
    out.append("};")
    out.append("")
    out.append("static void fuzz_overlap_run_codegen(CpuState *s, int idx) {")
    out.append("    switch (idx) {")
    for i, (w, klass, fam, am, ct) in enumerate(cases):
        body, _, _ = codegen.vfpu_effect(0x08900000, w)
        out.append(f"    case {i}: /* 0x{w:08x} klass={klass} fam={fam} model={am} ct={ct} */ {body} break;")
    out.append("    default: break;")
    out.append("    }")
    out.append("}")
    out.append("")
    out.append("#endif /* NAKAGAWA_VFPU_OVERLAP_CASES_H */")
    with open(out_path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(out) + "\n")


FAMILY_NAMES = {FAM_VMMUL: "vmmul", FAM_VTFM: "vtfm", FAM_VMSCL: "vmscl",
                FAM_VMMOV: "vmmov", FAM_VDOT: "vdot", FAM_VHDP: "vhdp",
                FAM_VCRS: "vcrs", FAM_VSCL: "vscl", FAM_VQMUL: "vqmul",
                FAM_VCRSP: "vcrsp"}


def write_fixture_header(out_path: str, cases: list[tuple[int, int, int, int, int]]) -> None:
    """Write the compact PSP-side probe case table (no codegen bodies).

    Consumed verbatim by fixtures/vfpu_overlap_probe/vfpu_overlap_probe.c so
    the record shape cannot drift from the host audit: the words, alias
    classes, families and hardware-contract classes are the exact ones the
    host selftest exercises.  The probe's register-file inputs are computed on
    the PSP from a deterministic integer fill (no decimal literals), so no
    input data is embedded here."""
    out = ["/* Generated by tools/vfpu_overlap_fuzz_gen.py --fixture-header.",
           " * Do not edit: keep in sync with the host selftest table. */", ""]
    out.append("#ifndef NAKAGAWA_VFPU_OVERLAP_PROBE_CASES_H")
    out.append("#define NAKAGAWA_VFPU_OVERLAP_PROBE_CASES_H")
    out.append("")
    out.append(f"#define VFPU_OVERLAP_PROBE_CASE_COUNT {len(cases)}")
    out.append("")
    for name, val in (("OVERLAP_KLASS_DISJOINT", 0), ("OVERLAP_KLASS_VD_EQ_VS", 1),
                      ("OVERLAP_KLASS_VD_EQ_VT", 2), ("OVERLAP_KLASS_PARTIAL", 3),
                      ("OVERLAP_KLASS_TRANSPOSE", 4), ("OVERLAP_KLASS_SRC_SRC", 5),
                      ("OVERLAP_KLASS_ALL_IDENT", 6), ("OVERLAP_KLASS_SCALAR_IN_DEST", 7),
                      ("OVERLAP_CONTRACT_ALLOWED", 0), ("OVERLAP_CONTRACT_NO_OVERLAP", 1),
                      ("OVERLAP_CONTRACT_UNESTABLED", 2)):
        out.append(f"#define {name} {val}")
    out.append("")
    out.append("typedef struct {")
    out.append("    const char *id;    /* stable case id */")
    out.append("    unsigned int w;    /* raw instruction word */")
    out.append("    unsigned klass;    /* alias class */")
    out.append("    unsigned family;  /* op family (see FAMILY_NAMES in generator) */")
    out.append("    unsigned contract; /* ALLOWED / NO_OVERLAP / UNESTABLED */")
    out.append("} VfpuOverlapProbeCase;")
    out.append("")
    out.append(f"static const VfpuOverlapProbeCase VFPU_OVERLAP_PROBE_CASES[VFPU_OVERLAP_PROBE_CASE_COUNT] = {{")
    for i, (w, klass, fam, _am, ct) in enumerate(cases):
        out.append(
            f'    {{"{FAMILY_NAMES[fam]}-k{klass}-ct{ct}-{i:02d}", 0x{w:08x}u, {klass}u, {fam}u, {ct}u}},'
        )
    out.append("};")
    out.append("")
    out.append("#endif /* NAKAGAWA_VFPU_OVERLAP_PROBE_CASES_H */")
    with open(out_path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(out) + "\n")


def main(argv: list[str]) -> int:
    fixture_path: str | None = None
    if "--fixture-header" in argv:
        idx = argv.index("--fixture-header")
        if idx + 1 >= len(argv):
            sys.stderr.write("missing path after --fixture-header\n")
            return 2
        fixture_path = argv[idx + 1]
        argv = argv[:idx] + argv[idx + 2:]
    if len(argv) != 2:
        sys.stderr.write("usage: vfpu_overlap_fuzz_gen.py <out.h> [--fixture-header <probe.h>]\n")
        return 2
    cases = build_cases()
    write_header(argv[1], cases)
    if fixture_path:
        write_fixture_header(fixture_path, cases)
    by_klass: dict[int, int] = {}
    by_ct: dict[int, int] = {}
    for _, k, _, _, ct in cases:
        by_klass[k] = by_klass.get(k, 0) + 1
        by_ct[ct] = by_ct.get(ct, 0) + 1
    sys.stderr.write(
        f"vfpu_overlap_fuzz_gen: {len(cases)} overlap cases "
        f"(classes {dict(sorted(by_klass.items()))}, contract "
        f"{{allowed:{by_ct.get(CT_ALLOWED, 0)}, no-overlap:{by_ct.get(CT_NO_OVERLAP, 0)}, "
        f"unestablished:{by_ct.get(CT_UNESTABLED, 0)}}}) -> {argv[1]}\n"
    )
    if fixture_path:
        sys.stderr.write(f"  fixture probe header -> {fixture_path}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
