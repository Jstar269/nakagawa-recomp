# SPDX-License-Identifier: GPL-3.0-or-later

from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import codegen


class FakeElf:
    def __init__(self, words):
        self.words = words

    def read_at_vaddr(self, addr, size):
        if size != 4 or addr not in self.words:
            return None
        return self.words[addr].to_bytes(4, "little")


class IterationTrackingKnownEntries:
    """Membership-only view that detects a full entry-set copy per function."""

    def __init__(self, entries):
        self.entries = set(entries)
        self.iterations = 0

    def __contains__(self, entry):
        return entry in self.entries

    def __iter__(self):
        self.iterations += 1
        return iter(self.entries)


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def j(target):
    return 0x08000000 | ((target >> 2) & 0x03FFFFFF)


def jalr(rd, rs):
    return (rs << 21) | (rd << 11) | 0x09


def beq(rs, rt, pc, target):
    offset = ((target - (pc + 4)) >> 2) & 0xFFFF
    return 0x10000000 | (rs << 21) | (rt << 16) | offset


def regimm(selector, rs, pc, target):
    """REGIMM branch: 0x10 bltzal, 0x11 bgezal (bal is bgezal $zero), 0x12/0x13 likely."""
    offset = ((target - (pc + 4)) >> 2) & 0xFFFF
    return 0x04000000 | (rs << 21) | (selector << 16) | offset


class ContinuationFlowTests(unittest.TestCase):
    def test_emission_does_not_copy_the_complete_known_entry_set(self):
        known = IterationTrackingKnownEntries(
            {0x1000, *range(0x4000, 0x8000, 4)}
        )
        text = codegen.emit_function(
            FakeElf({0x1000: 0x03E00008, 0x1004: 0}),
            0x1000, [(0x1000, 0x1008)], known,
        )

        self.assertTrue(any("void f_00001000" in line for line in text))
        self.assertEqual(known.iterations, 0)

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

    # #968: bltzal/bgezal (bal is bgezal $zero) write $ra and CALL their target. Out of
    # the body the taken path used to be a TAIL (`s->pc = target; dispatch; return`), so
    # the callee's `jr $ra` left this body and the continuation at addr + 8 never ran.
    @staticmethod
    def _linked_site(first, known, lle_cpu=False):
        """`first` at 0x1000, a counted delay slot, and the continuation at 0x1008."""
        return codegen.emit_function(
            FakeElf({
                0x1000: first,
                0x1004: 0x24030011,  # addiu v1, zero, 0x11 (delay slot)
                0x1008: 0x03E00008,  # continuation: jr ra
                0x100C: 0x00000000,
            }),
            0x1000, [(0x1000, 0x1010)], known, lle_cpu=lle_cpu,
        )

    @staticmethod
    def _between_slot_and_continuation(lines):
        slot = next(i for i, line in enumerate(lines) if "sr_begin(s, 0x00001004u" in line)
        resume = next(i for i, line in enumerate(lines) if "sr_begin(s, 0x00001008u" in line)
        return " ".join(line.strip() for line in lines[slot + 1:resume])

    def test_linked_branch_out_of_the_body_is_the_same_call_as_jal(self):
        # The taken path must be exactly what `jal` emits for the same target: a direct
        # body call that completes the callee's tail chain (#855) for a known entry,
        # dispatch_call with the link as resume otherwise. Never a recorded TAIL.
        for known in ({0x1000}, {0x1000, 0x2000}):
            for lle_cpu in (False, True):
                jal_call = self._between_slot_and_continuation(
                    self._linked_site(jal(0x2000), known, lle_cpu))
                for name, word in (("bal", regimm(0x11, 0, 0x1000, 0x2000)),
                                   ("bltzal", regimm(0x10, 4, 0x1000, 0x2000))):
                    with self.subTest(known=sorted(known), lle_cpu=lle_cpu, branch=name):
                        lines = self._linked_site(word, known, lle_cpu)
                        text = "\n".join(lines)
                        self.assertEqual(
                            self._between_slot_and_continuation(lines),
                            f"if (_c) {{ {jal_call} }} }}",
                        )
                        self.assertNotIn("s->pc = 0x00002000u", text)
                        self.assertNotIn("sr_tail_record", text)
        unknown = "\n".join(self._linked_site(regimm(0x11, 0, 0x1000, 0x2000), {0x1000}))
        self.assertIn("if (_c) { dispatch_call(s, 0x00002000u, 0x00001008u); } }", unknown)
        known = "\n".join(self._linked_site(regimm(0x11, 0, 0x1000, 0x2000), {0x1000, 0x2000}))
        self.assertIn("if (_c) { f_00002000(s); SR_TAIL_DRAIN(s); } }", known)

    def test_linked_branch_out_of_the_body_links_and_runs_its_slot_once(self):
        # MIPS writes the link on both paths, before the slot; the slot runs once on
        # both paths; the not-taken path is a plain fall-through into the continuation.
        text = "\n".join(self._linked_site(regimm(0x10, 4, 0x1000, 0x2000), {0x1000}))
        call = "if (_c) { dispatch_call(s, 0x00002000u, 0x00001008u); } }"
        self.assertIn(call, text)
        link = text.index("s->r[31] = 0x00001008u;")
        slot = text.index("s->r[3] = ")
        taken = text.index(call)
        resume = text.index("sr_begin(s, 0x00001008u")
        self.assertLess(link, slot)
        self.assertLess(slot, taken)
        self.assertLess(taken, resume)
        self.assertEqual(text.count("sr_begin(s, 0x00001004u"), 1)
        self.assertNotIn("_sr_cont_", text)

    def test_linked_branch_to_its_own_continuation_stays_a_goto(self):
        # `bal .+8` reads the PC; its target is this body's own next pair, so the
        # in-body goto (no call) is exact and unchanged by #968.
        text = "\n".join(self._linked_site(regimm(0x11, 0, 0x1000, 0x1008), {0x1000}))
        self.assertIn("if (_c) { goto L_00001008; } }", text)
        self.assertNotIn("dispatch_call", text)

    def test_branch_likely_linked_forms_stay_refused(self):
        # bltzall/bgezall have no cond_expr case; #968 adds no opcode support.
        for selector in (0x12, 0x13):
            with self.subTest(selector=hex(selector)):
                with self.assertRaises(codegen.Unsupported):
                    self._linked_site(regimm(selector, 4, 0x1000, 0x2000), {0x1000})

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
        self.assertIn("sr_tail_record(s, _t, NULL,", text)
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
        self.assertIn("sr_tail_record(s, _t, NULL,", text)
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
        # The foreign entry runs as a TAIL transfer (#855): the boundary records it with
        # the owning entry's SP restore and returns; the invoker's drain runs the foreign
        # body and only then applies the restore, the order the nested call had.
        self.assertIn(
            "sr_tail_record(s, 0x00001010u, f_00001010, _sp_entry, 1u); return; "
            "/* synthetic boundary",
            text,
        )
        self.assertNotIn("f_00001010(s);", text)
        self.assertNotIn("s->r[29] = _sp_entry; /* synthetic boundary", text)
        self.assertIn("goto _sr_cont_00001010;", text)

    def test_stack_census_keeps_synthetic_boundary_annotation(self):
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
        text = "\n".join(
            codegen.emit_function(
                elf, 0x1000, [(0x1000, 0x2008)], known, stack_census=True,
            )
        )
        # The census wrapper owns SP restoration (after its own drain), so the boundary
        # records no restore, but the generated source must still mark it as synthetic.
        self.assertIn(
            "sr_tail_record(s, 0x00001010u, f_00001010, 0u, 0u); return; "
            "/* synthetic boundary",
            text,
        )
        self.assertNotIn("s->r[29] = _sp_entry; /* synthetic boundary", text)

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
        self.assertIn("sr_tail_record(s, 0x00001004u, f_00001004,", first_text)
        self.assertIn("sr_tail_record(s, 0x00001008u, f_00001008,", second_text)

    def test_every_direct_body_call_completes_its_tail_chain(self):
        # #855: a callee may leave through a recorded TAIL transfer, so every direct
        # CALL of a native body must drain it before the caller goes on.
        elf = FakeElf(
            {
                0x1000: jal(0x2000),
                0x1004: 0x00000000,
                0x1008: jal(0x3000),
                0x100C: 0x00000000,
                0x1010: 0x03E00008,
                0x1014: 0x00000000,
                0x2000: 0x08000C00,  # j 0x3000 (a direct tail)
                0x2004: 0x00000000,
                0x3000: 0x03E00008,
                0x3004: 0x00000000,
            }
        )
        known = {0x1000, 0x2000, 0x3000}
        ranges = [(0x1000, 0x1018), (0x2000, 0x2008), (0x3000, 0x3008)]
        text = "\n".join(
            "\n".join(codegen.emit_function(elf, entry, ranges, known))
            for entry in sorted(known)
        )
        calls = re.findall(r"\b(f_[0-9a-f]{8}(?:_real)?)\(s\);( SR_TAIL_DRAIN\(s\);)?", text)
        self.assertEqual(len(calls), 2, text)
        for symbol, drain in calls:
            self.assertTrue(drain, f"{symbol} is called without draining its tail chain")
        self.assertIn("sr_tail_record(s, 0x00003000u, f_00003000, _sp_entry, 1u); return;", text)

    def test_profile_wrappers_drain_the_bodies_they_call(self):
        # The two profile wrappers call their real bodies directly (#855 review).
        source = Path(codegen.__file__).read_text(encoding="utf-8")
        for call in ("f_000468c8_real(s);", "f_001d9eb0_real(s);"):
            with self.subTest(call=call):
                at = source.index(call)
                following = source[at + len(call):at + len(call) + 60]
                self.assertEqual(following.split()[0], "SR_TAIL_DRAIN(s);", following)

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
        self.assertIn("s->pc = 0x00002000u; sr_tail_record(s, s->pc, NULL,", text)


if __name__ == "__main__":
    unittest.main()
