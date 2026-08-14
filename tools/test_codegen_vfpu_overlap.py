# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Static shape regressions for VFPU source/destination aliasing fixes.

These are SOURCE-SHAPE/STATIC assertions (evidence category 4): they inspect
the C body that codegen.vfpu_effect emits for the overlap-sensitive matrix/vec
ops and pin the read-before-write and accumulation shapes that the executable
selftest (src/rt/vfpu_overlap_selftest.c) proves dynamically.  They are the
failing-before half of the fix: before the vmscl scalar snapshot, the emitted
C for vmscl read ``s->v[scalar]`` fresh inside every row write, so a scalar
lane inside the destination matrix consumed a destination-clobbered value;
before the vhdp fix, the emitted C folded the dot product as a single chained
expression that starts from the first product instead of +0.0f, flipping -0
vs +0 and selecting a different NaN payload than sr_vfpu_interp's loop fold.

Every word below is packed with the same decode conventions the overlap corpus
uses (size bits 7/15, sub bits 25:23, vd/vs/vt at 6:0/14:8/22:16).
"""

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))


def _word(op6: int, sub: int, size: int, vd: int, vs: int, vt: int) -> int:
    lo, hi = (size - 1) & 1, (size - 1) >> 1
    return (
        (op6 << 26)
        | ((sub & 7) << 23)
        | ((vt & 0x7F) << 16)
        | ((vs & 0x7F) << 8)
        | (vd & 0x7F)
        | (lo << 7)
        | (hi << 15)
    )


class VmsclScalarSnapshotTests(unittest.TestCase):
    """vmscl (0x3C sub 4) and its VFPUMatrix1 alias (0x3C sub 7, idx 28,
    which 1..7) must snapshot the scalar into a local before the first
    destination write; the row writes must reference the snapshot, and the
    final source row must be read before any destination lane is written."""

    def _body(self, w: int) -> str:
        import codegen
        body, _, _ = codegen.vfpu_effect(0x08900000, w)
        self.assertNotIn("sr_vfpu_interp", body)
        return body

    def test_sub4_snapshots_scalar_before_row_writes(self) -> None:
        # vmscl.q, vd/vs/vt chosen so the scalar lane lies inside the
        # destination matrix (the class the old emission clobbered).
        w = _word(0x3C, 4, 4, 0x20, 0x40, 0x22)
        body = self._body(w)
        # The scalar is read exactly once, into a local, before any write.
        self.assertIn("float _sc = s->v[", body)
        # Row writes use the snapshot, never a fresh register-file read.
        self.assertRegex(body, r"s->v\[\d+\] = s->v\[\d+\] \* _sc;")
        # No per-row fresh scalar read remains.
        self.assertNotRegex(body, r"s->v\[\d+\] = s->v\[\d+\] \* s->v\[")
        # The final source row is read before the last-row destination write.
        self.assertLess(body.index("sr_vread(_s"), body.index("sr_vwrite(s"))

    def test_matrix1_alias_snapshots_scalar(self) -> None:
        # vmscl alias: sub 7, idx 28, which 1 (scalar register 1), size 3.
        lo, hi = (3 - 1) & 1, (3 - 1) >> 1
        w = (
            (0x3C << 26)
            | (28 << 21)
            | (1 << 16)
            | (0x40 << 8)
            | 0x20
            | (lo << 7)
            | (hi << 15)
        )
        body = self._body(w)
        self.assertIn("float _sc = s->v[", body)
        self.assertRegex(body, r"s->v\[\d+\] = s->v\[\d+\] \* _sc;")
        self.assertNotRegex(body, r"s->v\[\d+\] = s->v\[\d+\] \* s->v\[")


class VhdpAccumulationShapeTests(unittest.TestCase):
    """vhdp (0x19 sub 4) must fold from +0.0f with one += per term, matching
    sr_vfpu_interp; the old single chained expression diverged on -0/+0 and
    NaN payloads."""

    def _body(self, w: int) -> str:
        import codegen
        body, _, _ = codegen.vfpu_effect(0x08900000, w)
        self.assertNotIn("sr_vfpu_interp", body)
        return body

    def test_vhdp_folds_from_zero(self) -> None:
        w = _word(0x19, 4, 4, 0x20, 0x40, 0)
        body = self._body(w)
        self.assertIn("float _d=0.0f;", body)
        self.assertRegex(body, r"for\(int _i=0;_i<3;_i\+\+\) _d\+=_s\[_i\]\*_t\[_i\];")
        self.assertIn("_d+=1.0f*_t[3];", body)
        # The old chained expression form is gone (no bare product chain).
        self.assertNotRegex(body, r"float _d=_s\[0\]\*_t\[0\]")

    def test_vhdp_triple_and_pair_fold(self) -> None:
        w = _word(0x19, 4, 3, 0x20, 0x40, 0)
        body = self._body(w)
        self.assertRegex(body, r"for\(int _i=0;_i<2;_i\+\+\) _d\+=_s\[_i\]\*_t\[_i\];")
        self.assertIn("_d+=1.0f*_t[2];", body)


class VmmovLastRowReadTests(unittest.TestCase):
    """vmmov (0x3C sub 7, idx 28, which 0) must read the final source row
    before any destination lane is written (the identical-overlap encoding
    must not consume a clobbered last row)."""

    def test_vmmov_reads_last_row_before_write(self) -> None:
        lo, hi = (4 - 1) & 1, (4 - 1) >> 1
        w = (
            (0x3C << 26)
            | (28 << 21)
            | (0x40 << 8)
            | 0x20
            | (lo << 7)
            | (hi << 15)
        )
        import codegen
        body, _, _ = codegen.vfpu_effect(0x08900000, w)
        self.assertNotIn("sr_vfpu_interp", body)
        self.assertLess(body.index("sr_vread(_s"), body.index("sr_vwrite(s"))


if __name__ == "__main__":
    unittest.main()
