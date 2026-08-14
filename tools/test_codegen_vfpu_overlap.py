# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Static shape regressions for VFPU source/destination aliasing fixes.

These are SOURCE-SHAPE/STATIC assertions (evidence category 4): they inspect
the C body that codegen.vfpu_effect emits.  They are the
failing-before half of the fix: before the vhdp fix, the emitted C folded the dot product as a single chained
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
