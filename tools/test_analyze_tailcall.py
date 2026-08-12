# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression tests for analyze.py's tail-call promotion (ISSUES.md 2026-07-18).

Background: 16 functions reached only by cross-function `j` tail jumps were
absorbed into whichever function linearly covered them, so the tail `j` had no
dispatch entry at runtime (silent NONPLT_MISS, same class as 0x000e1724).
Found by tools/ghidra_crosscheck.py against a ghidra-allegrex analysis.

The pure predicate is tested everywhere; the end-to-end discovery assertions
need the real decrypted EBOOT and skip when it is absent (CI has no game data).
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import analyze

ELF = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "place_game_here", "EBOOT.elf")

# Ghidra-confirmed function entries, each preceded by a hard terminator and
# reached via `j` from a different function (see logs/ghidra_refs.log).
PROMOTED = [
    0x0003d1e4, 0x0003d27c, 0x00041cd4, 0x000629c4,
    0x0011d0ac, 0x0011d314, 0x00126864, 0x0018bd64,
    0x001b36ec, 0x001b39f4, 0x001b3b64, 0x001b4984,
    0x001e9cb8, 0x001ef2e0,
]

# Bottom-tested-loop artifacts: `j` targets inside their OWN function that sit
# after ordinary stores (fallthrough reaches them). Must NOT become entries.
NOT_PROMOTED = [0x0003d334, 0x0003d370]


class TestHardTerminator(unittest.TestCase):
    def test_terminators(self):
        self.assertTrue(analyze._is_hard_terminator(0x03e00008))  # jr $ra
        self.assertTrue(analyze._is_hard_terminator(0x00400008))  # jr $v0
        self.assertTrue(analyze._is_hard_terminator(0x0804742b))  # j 0x11d0ac
        self.assertTrue(analyze._is_hard_terminator(0x10000005))  # b +0x18 (beq $0,$0)

    def test_non_terminators(self):
        self.assertFalse(analyze._is_hard_terminator(0x0c04742b))  # jal (returns)
        self.assertFalse(analyze._is_hard_terminator(0x0040f809))  # jalr (returns)
        self.assertFalse(analyze._is_hard_terminator(0x10400005))  # beqz $v0 (conditional)
        self.assertFalse(analyze._is_hard_terminator(0x10220005))  # beq $at,$v0 (conditional)
        self.assertFalse(analyze._is_hard_terminator(0xac650004))  # sw (plain store)
        self.assertFalse(analyze._is_hard_terminator(0x27bdffe8))  # addiu $sp (prologue)


@unittest.skipUnless(os.path.isfile(ELF), "decrypted EBOOT.elf not present")
class TestTailcallPromotionOnEboot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        elf = analyze.Elf(ELF)
        cls.starts, cls.ranges = analyze.analyze(elf)

    def test_ghidra_confirmed_tail_targets_are_entries(self):
        missing = ["0x%08x" % a for a in PROMOTED if a not in self.starts]
        self.assertEqual(missing, [],
                         "tail-call targets lost again (NONPLT_MISS risk): %s" % missing)

    def test_loop_test_blocks_are_not_promoted(self):
        wrongly = ["0x%08x" % a for a in NOT_PROMOTED if a in self.starts]
        self.assertEqual(wrongly, [],
                         "intra-function loop tests wrongly promoted: %s" % wrongly)


if __name__ == "__main__":
    unittest.main()
