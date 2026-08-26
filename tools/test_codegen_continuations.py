# SPDX-License-Identifier: GPL-2.0-or-later

import unittest

import codegen


class FakeElf:
    def __init__(self, words):
        self.words = words

    def read_at_vaddr(self, addr, size):
        if size != 4 or addr not in self.words:
            return None
        return self.words[addr].to_bytes(4, "little")


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def j(target):
    return 0x08000000 | ((target >> 2) & 0x03FFFFFF)


def jalr(rd, rs):
    return (rs << 21) | (rd << 11) | 0x09


def beq(rs, rt, pc, target):
    offset = ((target - (pc + 4)) >> 2) & 0xFFFF
    return 0x10000000 | (rs << 21) | (rt << 16) | offset


class ContinuationFlowTests(unittest.TestCase):
    def test_linked_transfers_carry_the_native_resume_boundary(self):
        linked_jal = "\n".join(
            codegen.emit_function(
                FakeElf({
                    0x1000: jal(0x2000),
                    0x1004: 0x24030011,
                    0x1008: 0x03E00008,
                    0x100C: 0x24000000,
                }),
                0x1000, [(0x1000, 0x1010)], {0x1000},
            )
        )
        self.assertIn("dispatch_call(s, 0x00002000u, 0x00001008u);", linked_jal)

        linked_jalr = "\n".join(
            codegen.emit_function(
                FakeElf({
                    0x1100: jalr(31, 8),
                    0x1104: 0x24030011,
                    0x1108: 0x03E00008,
                    0x110C: 0x24000000,
                }),
                0x1100, [(0x1100, 0x1110)], {0x1100},
            )
        )
        self.assertIn("dispatch_call(s, _t, 0x00001108u);", linked_jalr)

    def test_unlinked_computed_transfer_remains_a_tail_dispatch(self):
        text = "\n".join(
            codegen.emit_function(
                FakeElf({
                    0x1200: jalr(0, 8),
                    0x1204: 0x24030011,
                    0x1208: 0x03E00008,
                    0x120C: 0x24000000,
                }),
                0x1200, [(0x1200, 0x1210)], {0x1200},
            )
        )
        self.assertIn("dispatch(s, _t);", text)
        self.assertNotIn("dispatch_call(s, _t, 0x00001208u);", text)

    def test_omitted_direct_jump_remains_a_tail_dispatch_boundary(self):
        elf = FakeElf(
            {
                0x1000: j(0x1040),
                0x1004: 0x24030011,  # delay slot belongs to the tail transfer
                0x1040: 0x24020022,  # intentionally omitted AOT target
                0x1044: 0x03E00008,
                0x1048: 0x00000000,
            }
        )
        known = {0x1000}
        omitted = {0x1040}
        insns, labels, continuations = codegen.function_flow(
            elf, 0x1000, [(0x1000, 0x104C)], known,
            dispatch_boundaries=omitted,
        )
        self.assertEqual(insns, {0x1000, 0x1004})
        self.assertNotIn(0x1040, labels)
        self.assertEqual(continuations, {})

        text = "\n".join(
            codegen.emit_function(
                elf, 0x1000, [(0x1000, 0x104C)], known,
                dispatch_boundaries=omitted,
            )
        )
        self.assertIn("dispatch(s, _t);", text)
        self.assertNotIn("goto L_00001040", text)
        self.assertNotIn("f_00001040(s);", text)

    def test_call_fallthrough_uses_foreign_entry_before_frame_restore(self):
        elf = FakeElf(
            {
                0x1000: 0x27BDFFF0,  # addiu sp, sp, -16
                0x1004: 0xAFBF0000,  # sw ra, 0(sp)
                0x1008: jal(0x2000),
                0x100C: 0x24040001,  # addiu a0, zero, 1 (delay slot)
                0x1010: 0x00041823,  # foreign adjacent entry
                0x1014: 0x03E00008,
                0x1018: 0x00000000,
                0x2000: 0x03E00008,
                0x2004: 0x00000000,
            }
        )
        known = {0x1000, 0x1010, 0x2000}
        insns, _, continuations = codegen.function_flow(elf, 0x1000, [(0x1000, 0x2008)], known)
        self.assertNotIn(0x1010, insns)
        self.assertEqual(continuations, {0x1008: 0x1010})

        text = "\n".join(codegen.emit_function(elf, 0x1000, [(0x1000, 0x2008)], known))
        boundary = text.index("f_00001010(s);")
        restore = text.index("s->r[29] = _sp_entry; /* synthetic boundary", boundary)
        self.assertLess(boundary, restore)
        self.assertIn("goto _sr_cont_00001010;", text)

    def test_dense_linear_entries_chain_instead_of_truncating(self):
        elf = FakeElf(
            {
                0x1000: 0x24020001,
                0x1004: 0x24420001,
                0x1008: 0x03E00008,
                0x100C: 0x00000000,
            }
        )
        known = {0x1000, 0x1004, 0x1008}
        _, _, first = codegen.function_flow(elf, 0x1000, [(0x1000, 0x1010)], known)
        _, _, second = codegen.function_flow(elf, 0x1004, [(0x1000, 0x1010)], known)
        self.assertEqual(first, {0x1000: 0x1004})
        self.assertEqual(second, {0x1004: 0x1008})

        first_text = "\n".join(codegen.emit_function(elf, 0x1000, [(0x1000, 0x1010)], known))
        second_text = "\n".join(codegen.emit_function(elf, 0x1004, [(0x1000, 0x1010)], known))
        self.assertIn("f_00001004(s);", first_text)
        self.assertIn("f_00001008(s);", second_text)

    def test_conditional_successors_stay_native_to_avoid_host_recursion(self):
        elf = FakeElf(
            {
                0x1000: beq(1, 2, 0x1000, 0x1020),
                0x1004: 0x00000000,
                0x1008: 0x24020007,  # foreign fall-through entry
                0x100C: 0x03E00008,
                0x1010: 0x00000000,
                0x1020: 0x24020009,
                0x1024: 0x03E00008,
                0x1028: 0x00000000,
            }
        )
        known = {0x1000, 0x1008, 0x1020}
        insns, labels, continuations = codegen.function_flow(elf, 0x1000, [(0x1000, 0x102C)], known)
        self.assertIn(0x1020, labels)
        self.assertIn(0x1008, insns)
        self.assertEqual(continuations, {})

        text = "\n".join(codegen.emit_function(elf, 0x1000, [(0x1000, 0x102C)], known))
        taken = text.index("if (_c) { goto L_00001020; }")
        not_taken = text.index("0x00001008u", taken)
        target = text.index("L_00001020:", not_taken)
        self.assertLess(taken, not_taken)
        self.assertLess(not_taken, target)
        self.assertNotIn("_sr_cont_00001008", text)

    def test_conditional_foreign_target_dispatches_without_undefined_label(self):
        elf = FakeElf(
            {
                0x1000: beq(1, 2, 0x1000, 0x2000),
                0x1004: 0x00000000,
                0x1008: 0x03E00008,
                0x100C: 0x00000000,
            }
        )
        known = {0x1000, 0x2000}
        insns, labels, _continuations = codegen.function_flow(
            elf, 0x1000, [(0x1000, 0x1010)], known
        )
        self.assertNotIn(0x2000, insns)
        self.assertNotIn(0x2000, labels)

        text = "\n".join(
            codegen.emit_function(elf, 0x1000, [(0x1000, 0x1010)], known)
        )
        self.assertNotIn("goto L_00002000", text)
        self.assertIn("s->pc = 0x00002000u; dispatch(s, s->pc);", text)


if __name__ == "__main__":
    unittest.main()
