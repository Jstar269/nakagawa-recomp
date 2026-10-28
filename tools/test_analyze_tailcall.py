# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors

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
    "PRIVATE_INPUT: set NK_PRIVATE_TITLE_TESTS=1 and provide decrypted EBOOT.elf",
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


def _jal(target):
    return (3 << 26) | ((target >> 2) & 0x03FFFFFF)


# Function f at 0x1000 ends with `jr $ra` at 0x1018; the leaf `jr $ra; v0 = 0` at
# 0x1020 sits right after that return's delay slot, so no fall-through reaches it.
def _trailing_leaf_words(la_hi, la_lo):
    return {
        0x1000: 0x27BDFFF0,  # addiu $sp, $sp, -16
        0x1004: 0xAFBF0000,  # sw $ra, 0($sp)
        0x1008: la_hi,
        0x100C: _jal(0x1040),  # register_callback(a1)
        0x1010: la_lo,         # delay slot
        0x1014: 0x8FBF0000,  # lw $ra, 0($sp)
        0x1018: 0x03E00008,  # jr $ra
        0x101C: 0x27BD0010,  # addiu $sp, $sp, 16
        0x1020: 0x03E00008,  # jr $ra            <- leaf callback
        0x1024: 0x00001025,  # or $v0, $zero, $zero
        0x1028: 0x00000000,
        0x102C: 0x00000000,
        0x1030: 0x00000000,
        0x1034: 0x00000000,
        0x1038: 0x00000000,
        0x103C: 0x00000000,
        0x1040: 0x03E00008,  # register_callback: jr $ra
        0x1044: 0x00000000,
    }


LUI_A1_0 = 0x3C050000          # lui $a1, 0
ADDIU_A1_LEAF = 0x24A51020     # addiu $a1, $a1, 0x1020
NOP = 0x00000000


class TestAddressTakenEntries(unittest.TestCase):
    """A code pointer materialized by `lui` + `addiu` names a function entry.

    Retail callbacks and thread entries are often a leaf `jr $ra` placed right
    after the previous function's return. The analyzer folded every such stub
    into the previous function as a "trailing epilogue" and lost an `la` whose
    low half sat in a `jal` delay slot, so the pointer had no dispatch entry and
    the runtime stopped with UNRESOLVED_DISPATCH_TARGET.
    """

    def test_leaf_after_a_return_is_an_entry_when_its_address_is_taken(self):
        for name, words in (
            ("la low half in the jal delay slot", _trailing_leaf_words(LUI_A1_0, ADDIU_A1_LEAF)),
            ("la before the jal", {**_trailing_leaf_words(NOP, NOP),
                                   0x1004: LUI_A1_0, 0x1008: ADDIU_A1_LEAF}),
        ):
            with self.subTest(name):
                starts, _ = analyze.analyze(FakeElf(words))
                self.assertIn(0x1020, starts)

    def test_leaf_after_a_return_without_a_pointer_stays_folded(self):
        # The edge of the widened rule: nothing takes the stub's address, so it is
        # still covered by f and gets no entry of its own.
        starts, _ = analyze.analyze(FakeElf(_trailing_leaf_words(LUI_A1_0, NOP)))
        self.assertNotIn(0x1020, starts)

    def test_pointer_to_a_non_entry_word_stays_a_weak_signal(self):
        # An `la` naming a word that is neither `jr $ra` nor a prologue (here a
        # store in the middle of f) is not promoted, so f is not split there.
        words = _trailing_leaf_words(LUI_A1_0, 0x24A51004)  # addiu $a1, $a1, 0x1004
        starts, _ = analyze.analyze(FakeElf(words))
        self.assertNotIn(0x1004, starts)

    def _fini_elf(self, target_word):
        # Named .text is 0x1000..0x1040; the executable segment continues to
        # 0x1060, where a `.fini`-style routine at 0x1048 is registered by
        # pointer and never called directly.
        words = {
            0x1000: 0x27BDFFF0,  # addiu $sp, $sp, -16
            0x1004: 0xAFBF0000,  # sw $ra, 0($sp)
            0x1008: 0x3C040000,  # lui $a0, 0
            0x100C: _jal(0x1030),  # atexit(a0)
            0x1010: 0x24841048,  # addiu $a0, $a0, 0x1048 (delay slot)
            0x1014: 0x8FBF0000,  # lw $ra, 0($sp)
            0x1018: 0x03E00008,  # jr $ra
            0x101C: 0x27BD0010,  # addiu $sp, $sp, 16
            **{a: 0 for a in range(0x1020, 0x1030, 4)},
            0x1030: 0x03E00008,  # atexit: jr $ra
            0x1034: 0x00000000,
            0x1038: 0x00000000,
            0x103C: 0x00000000,
            0x1040: 0x00000000,
            0x1044: 0x00000000,
            0x1048: target_word,
            0x104C: 0xAFBF0000,  # sw $ra, 0($sp)
            0x1050: 0x8FBF0000,  # lw $ra, 0($sp)
            0x1054: 0x03E00008,  # jr $ra
            0x1058: 0x27BD0010,  # addiu $sp, $sp, 16
            0x105C: 0x00000000,
        }
        elf = FakeElf(words)
        elf.sections[0]["size"] = 0x40
        return elf

    def test_pointer_into_executable_bytes_outside_text_is_an_entry(self):
        starts, ranges = analyze.analyze(self._fini_elf(0x27BDFFF0))
        self.assertIn(0x1048, starts)
        self.assertTrue(analyze.in_ranges(0x1054, ranges))

    def test_pointer_outside_text_to_a_non_entry_word_is_ignored(self):
        starts, _ = analyze.analyze(self._fini_elf(0x8C820000))  # lw $v0, 0($a0)
        self.assertNotIn(0x1048, starts)


def _computed_goto_words(index_def=0x30C3000C, inserted=(), la_low=0x24211030):
    # memset tail: v1 = n & 12; jr (pad_base - v1) into a ladder of word stores.
    # `inserted` words go between the index definition and the `la`.
    head = [index_def, *inserted,
            0x3C010000,  # lui $at, 0
            la_low,      # addiu $at, $at, 0x1030
            0x00230823,  # subu $at, $at, $v1
            0x00200008,  # jr $at
            0x30C60003]  # andi $a2, $a2, 3
    words = {0x1000 + 4 * i: 0 for i in range(9)}
    words.update({0x1000 + 4 * i: word for i, word in enumerate(head)})
    words.update({
        0x1024: 0xAC850008,  # sw $a1, 8($a0)
        0x1028: 0xAC850004,  # sw $a1, 4($a0)
        0x102C: 0xAC850000,  # sw $a1, 0($a0)
        0x1030: 0x00832021,  # addu $a0, $a0, $v1
        0x1034: 0x03E00008,  # jr $ra
        0x1038: 0x00000000,
    })
    return words


class TestGprWrites(unittest.TestCase):
    """The register-write decoder the bounded-goto match walks backwards with."""

    def test_classes(self):
        cases = {
            0x0C000400: None,                  # jal
            0x10400002: None,                  # beqz $v0
            0x00200008: None,                  # jr $at
            0x0040F809: None,                  # jalr $v0
            0x45000002: None,                  # bc1f
            0x0000000C: frozenset(),           # syscall
            0x00850018: frozenset(),           # mult $a0, $a1
            0x00000011: frozenset(),           # mthi $zero
            0x00852021: frozenset({4}),        # addu $a0, $a0, $a1
            0x00001810: frozenset({3}),        # mfhi $v1
            0x30C3000C: frozenset({3}),        # andi $v1, $a2, 12
            0x8C820000: frozenset({2}),        # lw $v0, 0($a0)
            0xC0820000: frozenset({2}),        # ll $v0, 0($a0)
            0xAC850000: frozenset(),           # sw $a1, 0($a0)
            0xC4800000: frozenset(),           # lwc1 $f0, 0($a0)
            0x44026000: frozenset({2}),        # mfc1 $v0, $f12
            0x4442F800: frozenset({2}),        # cfc1 $v0, $31
            0x44826000: frozenset(),           # mtc1 $v0, $f12
            0x46000000: frozenset(),           # add.s
            0x00801016: frozenset({2}),        # clz $v0, $a0 (SPECIAL on Allegrex)
            0x70020024: "unknown",             # Allegrex mfic/mtic opcode space
            0x7C8218C0: frozenset({2}),        # ext $v0, $a0, 3, 4
            0x7C021420: frozenset({2}),        # seb $v0, $v0
            0x7C02143B: "unknown",             # an unclassified SPECIAL3 form
            0x48000000: "unknown",             # VFPU/COP2 transfer
        }
        for word, expected in cases.items():
            with self.subTest(f"0x{word:08x}"):
                self.assertEqual(analyze._gpr_writes(word), expected)

    def test_unknown_or_unmapped_words_end_the_backward_walk(self):
        words = {0x1000: 0x30C3000C, 0x1004: 0x48000000, 0x1008: 0x00000000}
        self.assertIsNone(analyze._defining_word(FakeElf(words), 0x1000, 0x1008, 3))
        sparse = FakeElf({0x1000: 0x30C3000C, 0x1008: 0x00000000})
        self.assertIsNone(analyze._defining_word(sparse, 0x1000, 0x1008, 3))


class TestBoundedComputedGoto(unittest.TestCase):
    PADS = {0x1024, 0x1028, 0x102C, 0x1030}
    RANGES = [(0x1000, 0x103C)]

    def test_every_pad_of_a_masked_goto_is_an_entry(self):
        starts, _ = analyze.analyze(FakeElf(_computed_goto_words()))
        self.assertEqual(self.PADS - set(starts), set())

    def test_shifted_index_names_the_scaled_pads(self):
        # andi $v1, $a2, 3; sll $v1, $v1, 2 -> the same four pads.
        words = _computed_goto_words(0x30C30003, inserted=(0x00031880,))  # sll $v1, $v1, 2
        targets = analyze.bounded_computed_goto_targets(FakeElf(words), self.RANGES)
        self.assertEqual(set(targets), self.PADS)

    def test_unbounded_or_clobbered_gotos_are_left_unresolved(self):
        cases = {
            # 256 pads: wider than COMPUTED_GOTO_MAX_TARGETS.
            "wide mask": _computed_goto_words(0x30C303FC),
            # The index is rewritten between the mask and the jump.
            "clobbered index": _computed_goto_words(inserted=(0x24630004,)),  # addiu $v1, $v1, 4
            # A pad would fall before the owned range.
            "pad outside the range": _computed_goto_words(la_low=0x24211004),
            # A branch splits the definition from the jump.
            "control transfer in between": _computed_goto_words(
                inserted=(0x10400002, 0x00000000)),  # beqz $v0, +2; nop
            # The index is not a masked value at all.
            "unmasked index": _computed_goto_words(0x00C01821),  # addu $v1, $a2, $zero
        }
        for name, words in cases.items():
            with self.subTest(name):
                self.assertEqual(
                    analyze.bounded_computed_goto_targets(FakeElf(words), self.RANGES),
                    frozenset())


if __name__ == "__main__":
    unittest.main()
