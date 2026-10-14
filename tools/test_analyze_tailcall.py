# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression tests for analyze.py's tail-call promotion (ISSUES.md 2026-07-18).

Background: 16 functions reached only by cross-function `j` tail jumps were
absorbed into whichever function linearly covered them, so the tail `j` had no
dispatch entry at runtime (silent NONPLT_MISS, same class as 0x000e1724).
Found by tools/ghidra_crosscheck.py against a ghidra-allegrex analysis.

The pure predicate is tested everywhere; the end-to-end discovery assertions
need explicit private-title opt-in and the real decrypted EBOOT.
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
        self.assertTrue(analyze._is_hard_terminator(0x00400009))  # jalr $zero, $v0 (unlinked)
        self.assertTrue(analyze._is_hard_terminator(0x0804742b))  # j 0x11d0ac
        self.assertTrue(analyze._is_hard_terminator(0x10000005))  # b +0x18 (beq $0,$0)

    def test_non_terminators(self):
        self.assertFalse(analyze._is_hard_terminator(0x0c04742b))  # jal (returns)
        self.assertFalse(analyze._is_hard_terminator(0x0040f809))  # jalr (returns)
        self.assertFalse(analyze._is_hard_terminator(0x10400005))  # beqz $v0 (conditional)
        self.assertFalse(analyze._is_hard_terminator(0x10220005))  # beq $at,$v0 (conditional)
        self.assertFalse(analyze._is_hard_terminator(0xac650004))  # sw (plain store)
        self.assertFalse(analyze._is_hard_terminator(0x27bdffe8))  # addiu $sp (prologue)


@unittest.skipUnless(
    os.environ.get("NK_PRIVATE_TITLE_TESTS") == "1" and os.path.isfile(ELF),
    "set NK_PRIVATE_TITLE_TESTS=1 and provide decrypted EBOOT.elf",
)
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


class FakeElf:
    def __init__(self, words, entry=0x1000):
        self.entry = entry
        self.words = words
        max_addr = max(words.keys())
        size = max_addr - 0x1000 + 4
        data = bytearray(size)
        for addr, w in words.items():
            off = addr - 0x1000
            data[off:off + 4] = w.to_bytes(4, "little")
        self.data = bytes(data)
        self.reloc = None
        self.shnum = 1
        self.shentsize = 40
        self.shstrndx = 0
        self.base = 0
        self.segments = [{"type": 1, "vaddr": 0x1000, "filesz": size, "memsz": size, "off": 0, "flags": 7}]
        self.sections = [{"nm": ".text", "typ": 1, "flags": 6, "addr": 0x1000, "off": 0, "size": size}]

    def read_at_vaddr(self, addr, size):
        res = bytearray()
        for a in range(addr, addr + size, 4):
            if a in self.words:
                res.extend(self.words[a].to_bytes(4, "little"))
            else:
                return None
        return bytes(res[:size])

    def sec(self, name):
        for s in self.sections:
            if s["nm"] == name:
                return s
        return None


class TestSyntheticTailcallPromotion(unittest.TestCase):
    def test_jalr_zero_tailcall_and_delay_slot_register_write_promotes_target(self):
        # Function 1 (0x1000): jumps to 0x1018, then ends with jalr $zero, $a0 and
        # a delay slot register write (addiu $a0, $a0, 4).
        # Target 0x1018 is preceded by 0x1010 (jalr $zero, $a0) and its delay slot.
        # It must be promoted as a function entry.
        j_1018 = (2 << 26) | ((0x1018 >> 2) & 0x03FFFFFF)
        words = {
            0x1000: 0x27BDFF30,  # addiu $sp, $sp, -16
            0x1004: j_1018,      # j 0x1018
            0x1008: 0x00000000,  # nop
            0x100C: 0x00000000,  # nop
            0x1010: 0x00400009,  # jalr $zero, $a0 (unlinked tail call)
            0x1014: 0x24840004,  # addiu $a0, $a0, 4 (delay slot register write)
            0x1018: 0x03E00008,  # jr $ra
            0x101C: 0x00000000,  # nop
        }
        elf = FakeElf(words)
        starts, _ = analyze.analyze(elf)
        self.assertIn(0x1018, starts, "jalr $zero tailcall target not promoted as function entry")

    def test_jr_tailcall_and_delay_slot_register_write_promotes_target(self):
        # Function 1 (0x1000): jumps to 0x1018, then ends with jr $a0 and
        # a delay slot register write (addiu $a0, $a0, 4).
        # Target 0x1018 is preceded by 0x1010 (jr $a0) and its delay slot.
        j_1018 = (2 << 26) | ((0x1018 >> 2) & 0x03FFFFFF)
        words = {
            0x1000: 0x27BDFF30,  # addiu $sp, $sp, -16
            0x1004: j_1018,      # j 0x1018
            0x1008: 0x00000000,  # nop
            0x100C: 0x00000000,  # nop
            0x1010: 0x00400008,  # jr $a0 (unlinked tail call)
            0x1014: 0x24840004,  # addiu $a0, $a0, 4 (delay slot register write)
            0x1018: 0x03E00008,  # jr $ra
            0x101C: 0x00000000,  # nop
        }
        elf = FakeElf(words)
        starts, _ = analyze.analyze(elf)
        self.assertIn(0x1018, starts, "jr tailcall target not promoted as function entry")
