# SPDX-License-Identifier: GPL-2.0-or-later

import unittest

import codegen


class FakeElf:
    """Minimal read_at_vaddr shim: a dict of addr -> instruction word."""

    def __init__(self, words):
        self.words = words

    def read_at_vaddr(self, addr, size):
        w = self.words.get(addr)
        return w.to_bytes(4, "little") if w is not None else None


def _mk(words, labels: set[int] | frozenset[int] = frozenset()):
    return FakeElf(words), set(words), set(labels)


class StaticVerifyTests(unittest.TestCase):
    def setUp(self):
        codegen.SV_ENABLED = True

    def tearDown(self):
        codegen.SV_ENABLED = False

    def test_lui_addiu_chain_is_predicted(self):
        # lui $a0, 0x2d ; addiu $a0, $a0, -0x518  -> a0 = 0x002cfae8
        words = {
            0x1000: (0x0F << 26) | (4 << 16) | 0x002D,
            0x1004: (0x09 << 26) | (4 << 21) | (4 << 16) | (0xFAE8),
        }
        elf, insns, labels = _mk(words)
        pts = codegen.sv_plan(elf, insns, labels)
        self.assertEqual(pts, {0x1004: [(4, 0x002CFAE8)]})

    def test_unknown_input_is_never_asserted(self):
        # lw $v0, 0($a0) ; addiu $v0, $v0, 8 : v0 stays Unknown, no check.
        # ori $a1, $zero, 5 in the same run IS known.
        words = {
            0x2000: (0x23 << 26) | (4 << 21) | (2 << 16),
            0x2004: (0x09 << 26) | (2 << 21) | (2 << 16) | 8,
            0x2008: (0x0D << 26) | (0 << 21) | (5 << 16) | 5,
        }
        elf, insns, labels = _mk(words)
        pts = codegen.sv_plan(elf, insns, labels)
        self.assertEqual(pts, {0x2008: [(5, 5)]})

    def test_state_resets_at_labels_and_branches(self):
        # ori $a0, $zero, 1 ; beq $zero,$zero,+2 ; nop(delay) ; ori $a1,$zero,2
        # The post-delay-slot instruction starts a fresh run of length 1 -> no
        # flush (run_len < 2), and nothing from before the branch leaks past it.
        words = {
            0x3000: (0x0D << 26) | (4 << 16) | 1,
            0x3004: (0x04 << 26) | 2,          # beq $zero, $zero, +2
            0x3008: 0,                          # delay slot nop
            0x300C: (0x0D << 26) | (5 << 16) | 2,
        }
        elf, insns, labels = _mk(words, labels={0x300C})
        pts = codegen.sv_plan(elf, insns, labels)
        self.assertNotIn(0x300C, pts)
        self.assertNotIn(0x3008, pts)

    def test_sra_and_slt_are_signed(self):
        # lui $t0, 0x8000 ; sra $t1, $t0, 4 -> 0xF8000000 ; slt $t2, $t0, $zero -> 1
        words = {
            0x4000: (0x0F << 26) | (8 << 16) | 0x8000,
            0x4004: (8 << 16) | (9 << 11) | (4 << 6) | 0x03,
            0x4008: (8 << 21) | (0 << 16) | (10 << 11) | 0x2A,
        }
        elf, insns, labels = _mk(words)
        pts = codegen.sv_plan(elf, insns, labels)
        self.assertEqual(pts, {0x4008: [(8, 0x80000000), (9, 0xF8000000), (10, 1)]})

    def test_syscall_resets_lattice(self):
        # ori $a0,$zero,1 ; syscall ; ori $a1,$zero,2 : only a1 asserted... and the
        # run after syscall has length 1, so nothing is asserted at all.
        words = {
            0x5000: (0x0D << 26) | (4 << 16) | 1,
            0x5004: 0x0000000C,
            0x5008: (0x0D << 26) | (5 << 16) | 2,
        }
        elf, insns, labels = _mk(words)
        pts = codegen.sv_plan(elf, insns, labels)
        for addr, checks in pts.items():
            for reg, _ in checks:
                self.assertNotEqual(reg, 4, f"r4 asserted after syscall at 0x{addr:x}")

    def test_disabled_by_default(self):
        codegen.SV_ENABLED = False
        words = {0x6000: (0x0F << 26) | (4 << 16) | 1,
                 0x6004: (0x0D << 26) | (4 << 21) | (4 << 16) | 2}
        elf, insns, labels = _mk(words)
        self.assertEqual(codegen.sv_plan(elf, insns, labels), {})


if __name__ == "__main__":
    unittest.main()
