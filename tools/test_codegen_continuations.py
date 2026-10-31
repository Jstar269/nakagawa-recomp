# SPDX-License-Identifier: GPL-3.0-or-later

from contextlib import contextmanager
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


class ScanCountingSpans(list):
    """A span list that counts every full scan of itself.

    ``in_ranges`` over a plain list scans it once per membership test; an index
    built from it scans it once.
    """

    def __init__(self, spans):
        super().__init__(spans)
        self.scans = 0

    def __iter__(self):
        self.scans += 1
        return super().__iter__()


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def j(target):
    return 0x08000000 | ((target >> 2) & 0x03FFFFFF)


def jalr(rd, rs):
    return (rs << 21) | (rd << 11) | 0x09


def beq(rs, rt, pc, target):
    offset = ((target - (pc + 4)) >> 2) & 0xFFFF
    return 0x10000000 | (rs << 21) | (rt << 16) | offset


def bne(rs, rt, pc, target):
    return beq(rs, rt, pc, target) | 0x04000000


def beql(rs, rt, pc, target):
    return (beq(rs, rt, pc, target) & 0x03FFFFFF) | 0x50000000


@contextmanager
def region_split_threshold(value):
    """Run with codegen.REGION_SPLIT_THRESHOLD set, restoring it afterwards."""
    saved = codegen.REGION_SPLIT_THRESHOLD
    codegen.REGION_SPLIT_THRESHOLD = value
    try:
        yield
    finally:
        codegen.REGION_SPLIT_THRESHOLD = saved


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

    def test_flow_and_emission_scan_the_span_list_once_per_body(self):
        # An image whose owned code is split into hundreds of spans (each run of code
        # reached outside the named sections is its own span) made every membership
        # test of the walk scan them all: one profiled executable with 284 spans spent
        # 61% of its code-generation time in that scan. The walk indexes them once.
        body = 64
        words = {0x1000 + 4 * i: 0x24420001 for i in range(body)}  # addiu v0, v0, 1
        words[0x1000 + 4 * body] = 0x03E00008                        # jr ra
        words[0x1004 + 4 * body] = 0x00000000
        spans = [(0x1000, 0x1008 + 4 * body)]
        spans += [(0x100000 + 0x100 * i, 0x100080 + 0x100 * i) for i in range(300)]
        elf, known = FakeElf(words), {0x1000}

        ranges = ScanCountingSpans(spans)
        insns, _labels, _continuations = codegen.function_flow(elf, 0x1000, ranges, known)
        self.assertEqual(len(insns), body + 2)
        self.assertEqual(ranges.scans, 1)

        ranges = ScanCountingSpans(spans)
        indexed = "\n".join(codegen.emit_function(elf, 0x1000, list(spans), known))
        text = "\n".join(codegen.emit_function(elf, 0x1000, ranges, known))
        self.assertEqual(text, indexed)
        self.assertEqual(ranges.scans, 1)

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
        if resume > slot + 1 and "SR_AOT_RETURN_CAPTURE" in lines[resume - 1]:
            # A `jr $ra` continuation latches its target immediately before its
            # own trace/slot lines; that return block is after the caller's
            # continuation boundary, not part of the linked-call statement.
            resume -= 2
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
        self.assertIn(
            "if (_c) { if (sr_aot_return_boundary_begin(s, 0x00001008u, "
            "0x00001000u, 1u)) return; f_00002000(s); SR_TAIL_DRAIN(s); "
            "if (sr_aot_return_boundary_end(s)) return; } }",
            known,
        )

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
                with self.assertRaises(codegen.Unsupported):
                    self._in_body_site(regimm(selector, 4, 0x1000, 0x1010), {0x1000, 0x1010})

    # #968 in the body: the target lies inside the caller's own span. function_flow used
    # to make it a label and the taken path a `goto`, so the callee's `jr $ra` was the
    # caller's host return and the continuation at the link never ran.
    _LEAF_WORDS = {
        0x1010: 0x24420001,  # leaf: addiu v0, v0, 1
        0x1014: 0x03E00008,  # jr ra
        0x1018: 0x00000000,
    }

    def _in_body_site(self, first, known, extra=None):
        """`first` at 0x1000, its slot, the continuation `jr ra` at 0x1008, a leaf at 0x1010."""
        words = {
            0x1000: first,
            0x1004: 0x24030011,  # addiu v1, zero, 0x11 (delay slot)
            0x1008: 0x03E00008,  # continuation: jr ra
            0x100C: 0x00000000,
            **self._LEAF_WORDS,
        }
        words.update(extra or {})
        return codegen.emit_function(FakeElf(words), 0x1000, [(0x1000, 0x101C)], known)

    def test_linked_branch_inside_the_body_is_the_same_call_as_jal(self):
        for known in ({0x1000}, {0x1000, 0x1010}):
            jal_call = self._between_slot_and_continuation(self._in_body_site(jal(0x1010), known))
            for name, word in (("bal", regimm(0x11, 0, 0x1000, 0x1010)),
                               ("bltzal", regimm(0x10, 4, 0x1000, 0x1010))):
                with self.subTest(known=sorted(known), branch=name):
                    lines = self._in_body_site(word, known)
                    text = "\n".join(lines)
                    self.assertEqual(
                        self._between_slot_and_continuation(lines),
                        f"if (_c) {{ {jal_call} }} }}",
                    )
                    self.assertNotIn("L_00001010", text, "the callee is not a label")
                    self.assertNotIn("0x00001010u, 0x24420001u", text, "nor part of this body")
        elf = FakeElf({0x1000: regimm(0x11, 0, 0x1000, 0x1010), 0x1004: 0,
                       0x1008: 0x03E00008, 0x100C: 0, **self._LEAF_WORDS})
        insns, labels, _ = codegen.function_flow(elf, 0x1000, [(0x1000, 0x101C)], {0x1000})
        self.assertEqual(sorted(insns), [0x1000, 0x1004, 0x1008, 0x100C])
        self.assertEqual(labels, set())

    def test_linked_call_keeps_its_call_when_another_branch_labels_the_target(self):
        # `bne` makes 0x1018 a native label of this body; the `bal` to the same address
        # must still call it rather than jump to that label.
        lines = codegen.emit_function(
            FakeElf({
                0x1000: (0x05 << 26) | (4 << 21) | ((0x1018 - 0x1004) >> 2),  # bne a0, zero
                0x1004: 0x00000000,
                0x1008: regimm(0x11, 0, 0x1008, 0x1018),                      # bal 0x1018
                0x100C: 0x24030011,
                0x1010: 0x03E00008,
                0x1014: 0x00000000,
                0x1018: 0x24420001,
                0x101C: 0x03E00008,
                0x1020: 0x00000000,
            }),
            0x1000, [(0x1000, 0x1024)], {0x1000, 0x1018},
        )
        text = "\n".join(lines)
        self.assertIn("if (_c) { goto L_00001018; } }", text)
        self.assertIn(
            "if (_c) { if (sr_aot_return_boundary_begin(s, 0x00001010u, "
            "0x00001008u, 1u)) return; f_00001018(s); SR_TAIL_DRAIN(s); "
            "if (sr_aot_return_boundary_end(s)) return; } }",
            text,
        )
        self.assertEqual(text.count("goto L_00001018;"), 1)

    def test_linked_call_whose_link_is_an_entry_continues_there(self):
        # The link is a known entry: after the call (and on the not-taken path) the body
        # ends at the ordinary continuation boundary, exactly as after a `jal`.
        for word in (jal(0x1010), regimm(0x11, 0, 0x1000, 0x1010)):
            with self.subTest(word=hex(word)):
                text = "\n".join(self._in_body_site(word, {0x1000, 0x1008, 0x1010}))
                call = text.index("f_00001010(s); SR_TAIL_DRAIN(s);")
                boundary = text.index("    goto _sr_cont_00001008;")
                self.assertLess(call, boundary)
                self.assertIn("_sr_cont_00001008: ;", text)
                self.assertNotIn("sr_begin(s, 0x00001008u", text)

    def test_linked_branch_into_a_frame_releasing_tail_stays_a_goto(self):
        # The target releases the frame it never allocated (a continuation), so its
        # `jr $ra` returns for this body: a jump that writes $ra, unchanged by #968.
        text = "\n".join(codegen.emit_function(
            FakeElf({
                0x1000: 0x27BDFFF0,                      # addiu sp, sp, -16
                0x1004: 0xAFBF0000,                      # sw ra, 0(sp)
                0x1008: regimm(0x11, 0, 0x1008, 0x1014),  # bal 0x1014
                0x100C: 0x24030011,
                0x1010: 0x24424000,                      # inline data, never executed
                0x1014: 0x03E02021,                      # addu a0, ra, zero
                0x1018: 0x8FBF0000,                      # lw ra, 0(sp)
                0x101C: 0x03E00008,                      # jr ra
                0x1020: 0x27BD0010,                      # addiu sp, sp, 16
            }),
            0x1000, [(0x1000, 0x1024)], {0x1000},
        ))
        self.assertIn("if (_c) { goto L_00001014; } }", text)
        self.assertNotIn("dispatch_call", text)
        self.assertNotIn("f_00001014(s)", text)

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
        for call in ("f_000468c8_real(s);",):
            with self.subTest(call=call):
                at = source.index(call)
                following = source[at + len(call):at + len(call) + 60]
                self.assertEqual(following.split()[0], "SR_TAIL_DRAIN(s);", following)

    def test_conditional_successors_stay_native_below_the_split_threshold(self):
        # Changed deliberately with entry splitting (owner decision, 2026-10-29): a body
        # copies another entry's code over conditional edges only while its walk stays
        # within REGION_SPLIT_THRESHOLD, and never with the threshold off.
        # EntrySplitTests covers the same shape split.
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
        for threshold in (None, codegen.REGION_SPLIT_THRESHOLD):
            with self.subTest(threshold=threshold), region_split_threshold(threshold):
                insns, labels, continuations = codegen.function_flow(
                    elf, 0x1000, [(0x1000, 0x102C)], known)
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


class EntrySplitTests(unittest.TestCase):
    """A split body leaves every other entry's code to that entry's own body.

    Entries 0x1000, 0x1010 and 0x1020 are adjacent. 0x1000 branches into 0x1020 and,
    when the branch is not taken, falls through into 0x1010; 0x1010 branches back to
    its own start. RANGES covers all three.
    """

    RANGES = [(0x1000, 0x1030)]
    KNOWN = {0x1000, 0x1010, 0x1020}

    def elf(self, branch=bne):
        return FakeElf({
            0x1000: 0x24020001,                       # addiu v0, zero, 1
            0x1004: 0x24030002,                       # addiu v1, zero, 2
            0x1008: branch(1, 2, 0x1008, 0x1020),     # into entry 0x1020
            0x100C: 0x24420004,                       # addiu v0, v0, 4 (slot)
            0x1010: 0x24420008,                       # entry: addiu v0, v0, 8
            0x1014: beq(2, 0, 0x1014, 0x1010),        # back to its own start
            0x1018: 0x00000000,
            0x101C: 0x00000000,
            0x1020: 0x24420010,                       # entry: addiu v0, v0, 16
            0x1024: 0x03E00008,                       # jr ra
            0x1028: 0x00000000,
        })

    def flow(self, elf, start, threshold=0, **kwargs):
        with region_split_threshold(threshold):
            return codegen.function_flow(elf, start, self.RANGES, self.KNOWN, **kwargs)

    def emit(self, elf, start, threshold=0, **kwargs):
        with region_split_threshold(threshold):
            return "\n".join(codegen.emit_function(elf, start, self.RANGES, self.KNOWN, **kwargs))

    def test_split_body_owns_no_other_entry(self):
        insns, labels, continuations = self.flow(self.elf(), 0x1000)
        self.assertEqual(insns, {0x1000, 0x1004, 0x1008, 0x100C})
        self.assertEqual(labels, set())
        self.assertEqual(continuations, {0x1008: 0x1010})

    def test_unsplit_body_copies_both_entries(self):
        insns, labels, continuations = self.flow(self.elf(), 0x1000, threshold=None)
        self.assertTrue({0x1010, 0x1014, 0x1020, 0x1024} <= insns)
        self.assertIn(0x1020, labels)
        # Straight-line fall-through into an entry was a continuation before splitting.
        self.assertEqual(continuations, {0x101C: 0x1020})

    def test_taken_edge_is_a_tail_transfer_after_the_delay_slot(self):
        text = self.emit(self.elf(), 0x1000)
        slot = text.index("s->r[2] = (s->r[2] + 0x00000004u)")
        tail = text.index("if (_c) { sr_tail_record(s, 0x00001020u, f_00001020, _sp_entry, 1u); return; }")
        self.assertLess(slot, tail)
        self.assertNotIn("L_00001020", text)

    def test_not_taken_edge_continues_into_the_next_entry(self):
        text = self.emit(self.elf(), 0x1000)
        branch = text.index("0x00001008u, 0x")
        cont = text.index("goto _sr_cont_00001010;", branch)
        self.assertLess(branch, cont)
        target = text.index("  _sr_cont_00001010: ;")
        self.assertIn("sr_tail_record(s, 0x00001010u, f_00001010, _sp_entry, 1u); return;",
                      text[target:])

    def test_likely_edge_runs_its_slot_only_when_taken(self):
        text = self.emit(self.elf(beql), 0x1000)
        self.assertIn("if (_c) { sr_begin(s, 0x0000100cu,", text)
        self.assertIn("sr_tail_record(s, 0x00001020u, f_00001020, _sp_entry, 1u); return; }", text)
        self.assertEqual(text.count("0x0000100cu"), 1, text)
        self.assertIn("goto _sr_cont_00001010;", text)

    def test_branch_to_its_own_start_stays_a_loop_edge(self):
        insns, labels, continuations = self.flow(self.elf(), 0x1010)
        self.assertIn(0x1010, labels)
        self.assertEqual(continuations, {0x101C: 0x1020})
        text = self.emit(self.elf(), 0x1010)
        self.assertIn("goto L_00001010;", text)

    def test_resume_body_is_entered_through_its_own_symbol(self):
        # 0x1020 as a resume label owned by 0x1010: another body's crossing enters it
        # through r_00001020; its owner still reaches it natively.
        owners = {0x1020: 0x1010}
        known = self.KNOWN - {0x1020}
        with region_split_threshold(0):
            text = "\n".join(codegen.emit_function(
                self.elf(), 0x1000, self.RANGES, known, resume_owners=owners))
            owner_flow = codegen.function_flow(
                self.elf(), 0x1010, self.RANGES, known, resume_owners=owners)
        self.assertIn("sr_tail_record(s, 0x00001020u, r_00001020, _sp_entry, 1u); return;", text)
        self.assertIn(0x1020, owner_flow[0])

    def test_dispatch_boundary_target_is_dispatched_by_address(self):
        with region_split_threshold(0):
            text = "\n".join(codegen.emit_function(
                self.elf(), 0x1000, self.RANGES, self.KNOWN - {0x1020},
                dispatch_boundaries={0x1020}))
        self.assertIn("s->pc = 0x00001020u; sr_tail_record(s, s->pc, NULL,", text)

    def test_threshold_splits_only_bodies_whose_walk_exceeds_it(self):
        elf = self.elf()
        unsplit = self.emit(elf, 0x1000, threshold=None)
        walked = len(self.flow(elf, 0x1000, threshold=None)[0])
        self.assertEqual(self.emit(elf, 0x1000, threshold=walked), unsplit)
        self.assertEqual(self.emit(elf, 0x1000, threshold=4), self.emit(elf, 0x1000))
        self.assertNotEqual(self.emit(elf, 0x1000), unsplit)

    def test_every_ownership_reader_sees_the_split_body(self):
        with region_split_threshold(0):
            insns, _labels, _continuations = codegen.function_flow(
                self.elf(), 0x1000, self.RANGES, self.KNOWN)
            record = codegen.stale_block_for_function(
                self.elf(), 0x1000, self.RANGES, self.KNOWN)
        self.assertEqual(record[1], len(insns))

    def test_threshold_option_is_parsed_and_checked(self):
        saved = codegen.REGION_SPLIT_THRESHOLD
        try:
            self.assertEqual(codegen.main(["codegen.py", "missing.elf", "out.c",
                                           "--region-split-threshold=-1"]), 2)
            self.assertEqual(codegen.main(["codegen.py", "missing.elf", "out.c",
                                           "--region-split-threshold=many"]), 2)
        finally:
            codegen.REGION_SPLIT_THRESHOLD = saved


def addiu(rt, rs, imm):
    return (0x09 << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


JR_RA = 0x03E00008
JR_T9 = 0x03200008
NOP = 0


def b(pc, target):
    return beq(0, 0, pc, target)


def reachable(words, entry):
    """Every word the guest runs from ``entry``, as a delay slot or not: the reference model.

    A branch or jump runs its delay slot, then goes to its target (and, if it can fall
    through, to the word after the slot). Any other word goes to the next word. `j`, `jr`
    and `b` (beq $zero, $zero) never fall through. A target is entered as an ordinary
    word, delay slot or not.
    """
    entered, slots, work = set(), set(), [entry]
    while work:
        pc = work.pop()
        if pc in entered or pc not in words:
            continue
        entered.add(pc)
        w = words[pc]
        op, fn = w >> 26, w & 0x3F
        if op == 2:
            slots.add(pc + 4)
            work.append(codegen.jump_target(pc, w))
        elif op == 0 and fn == 0x08:
            slots.add(pc + 4)
        elif op == 4 or op == 5:
            slots.add(pc + 4)
            work.append(codegen.branch_target(pc, w))
            if not (op == 4 and (w >> 16) & 0x3FF == 0):
                work.append(pc + 8)
        else:
            work.append(pc + 4)
    return {pc for pc in entered | slots if pc in words}


class StraightLineEndTests(unittest.TestCase):
    """#1730: a body owns what its entry can run, and nothing after a transfer that never
    falls through (`j`, `jr`, `jalr $zero` and the `b` idiom) unless another edge reaches it."""

    def flow(self, words, start=0x1000, known=None):
        span = (min(words), max(words) + 4)
        with region_split_threshold(None):
            return codegen.function_flow(FakeElf(words), start, [span], known or {start})

    def emit(self, words, start=0x1000, known=None):
        span = (min(words), max(words) + 4)
        with region_split_threshold(None):
            return "\n".join(codegen.emit_function(FakeElf(words), start, [span], known or {start}))

    def test_b_has_no_not_taken_successor(self):
        words = {0x1000: addiu(2, 0, 1), 0x1004: b(0x1004, 0x1014), 0x1008: addiu(2, 2, 2),
                 0x100C: addiu(2, 2, 0x100), 0x1010: JR_RA,  # the next function's code
                 0x1014: JR_RA, 0x1018: NOP}
        insns, labels, continuations = self.flow(words)
        self.assertEqual(insns, {0x1000, 0x1004, 0x1008, 0x1014, 0x1018})
        self.assertEqual(labels, {0x1014})
        self.assertEqual(continuations, {})
        self.assertNotIn("0x0000100cu", self.emit(words))

    def test_any_other_beq_keeps_its_not_taken_successor(self):
        for rs, rt in ((0, 5), (5, 0), (5, 5)):
            with self.subTest(rs=rs, rt=rt):
                words = {0x1000: beq(rs, rt, 0x1000, 0x1010), 0x1004: NOP,
                         0x1008: JR_RA, 0x100C: NOP, 0x1010: JR_RA, 0x1014: NOP}
                insns, _labels, _continuations = self.flow(words)
                self.assertEqual(insns, set(words))

    def test_word_after_b_reached_by_another_edge_keeps_its_label(self):
        words = {0x1000: addiu(8, 0, 2), 0x1004: b(0x1004, 0x1018), 0x1008: addiu(2, 2, 1),
                 0x100C: addiu(2, 2, 0x10),                       # reached from 0x1018 only
                 0x1010: addiu(8, 8, -1), 0x1014: NOP,
                 0x1018: bne(8, 0, 0x1018, 0x100C), 0x101C: addiu(2, 2, 0x100),
                 0x1020: JR_RA, 0x1024: NOP}
        insns, labels, _continuations = self.flow(words)
        self.assertEqual(insns, set(words))
        self.assertIn(0x100C, labels)
        text = self.emit(words)
        self.assertLess(text.index("L_0000100c:"), text.index("0x00001010u"))

    def test_branch_into_a_terminator_slot_continues_after_the_slot(self):
        # The slot runs as an ordinary word on the branch's path, then the next word.
        # Fails before #1730 for `j` and `jr` (the walk dropped slot+4 after their slot was
        # seen), and with a `b` rule that does not continue from a slot label.
        for name, terminator in (("b", lambda pc: b(pc, 0x101C)), ("j", lambda pc: j(0x101C)),
                                 ("jr", lambda pc: JR_T9)):
            with self.subTest(terminator=name):
                words = {0x1000: bne(4, 0, 0x1000, 0x100C), 0x1004: NOP,
                         0x1008: terminator(0x1008), 0x100C: addiu(2, 2, 1),
                         0x1010: addiu(2, 2, 0x10), 0x1014: JR_RA, 0x1018: NOP,
                         0x101C: addiu(2, 2, 0x100), 0x1020: JR_RA, 0x1024: NOP}
                insns, labels, _continuations = self.flow(words)
                self.assertEqual(insns, reachable(words, 0x1000))
                self.assertIn(0x100C, labels)
                self.assertTrue({0x1010, 0x1014, 0x1018} <= insns)
                text = self.emit(words)
                slot_label = text.index("L_0000100c:")
                self.assertLess(slot_label, text.index("0x00001010u", slot_label))
                if 0x101C in insns:
                    self.assertLess(text.index("0x00001010u", slot_label),
                                    text.index("L_0000101c:"))

    def test_emitter_refuses_a_slot_label_without_its_successor(self):
        # A walk that lost slot+4 (the pre-#1730 `j` defect) is a generator bug: the
        # emitter must stop rather than let the label's C fall into the next block.
        words = {0x1000: bne(4, 0, 0x1000, 0x100C), 0x1004: NOP,
                 0x1008: j(0x101C), 0x100C: addiu(2, 2, 1),
                 0x1010: addiu(2, 2, 0x10), 0x1014: JR_RA, 0x1018: NOP,
                 0x101C: addiu(2, 2, 0x100), 0x1020: JR_RA, 0x1024: NOP}
        lost = ({0x1000, 0x1004, 0x1008, 0x100C, 0x101C, 0x1020, 0x1024}, {0x100C, 0x101C}, {})
        with self.assertRaisesRegex(AssertionError, "delay slot of 0x00001008"):
            codegen.emit_function(FakeElf(words), 0x1000, [(0x1000, 0x1028)], {0x1000}, flow=lost)

    def test_slot_label_walked_before_or_after_its_b_gives_the_same_body(self):
        tail = {0x2000: b(0x2000, 0x2014), 0x2004: addiu(2, 2, 1), 0x2008: addiu(2, 2, 0x10),
                0x200C: JR_RA, 0x2010: NOP, 0x2014: JR_RA, 0x2018: NOP}
        label_first = {0x1000: bne(4, 0, 0x1000, 0x1100), 0x1004: NOP,
                       0x1008: bne(5, 0, 0x1008, 0x2004), 0x100C: NOP,
                       0x1010: JR_RA, 0x1014: NOP, 0x1100: j(0x2000), 0x1104: NOP, **tail}
        b_first = {0x1000: bne(5, 0, 0x1000, 0x2004), 0x1004: NOP,
                   0x1008: j(0x2000), 0x100C: NOP, **tail}
        for name, words in (("label first", label_first), ("b first", b_first)):
            with self.subTest(order=name):
                insns, _labels, _continuations = self.flow(words)
                self.assertEqual(insns, reachable(words, 0x1000))
                self.assertTrue({0x2004, 0x2008, 0x200C, 0x2010} <= insns)

    def test_b_to_its_own_slot_runs_the_slot_again_and_continues(self):
        words = {0x1000: b(0x1000, 0x1004), 0x1004: addiu(2, 2, 1),
                 0x1008: addiu(2, 2, 0x10), 0x100C: JR_RA, 0x1010: NOP}
        insns, labels, _continuations = self.flow(words)
        self.assertEqual(insns, set(words))
        self.assertEqual(labels, {0x1004})

    def test_slot_successor_that_is_another_entry_is_a_continuation(self):
        words = {0x1000: bne(4, 0, 0x1000, 0x100C), 0x1004: NOP,
                 0x1008: b(0x1008, 0x1018), 0x100C: addiu(2, 2, 1),
                 0x1010: addiu(2, 2, 0x10), 0x1014: JR_RA,          # 0x1010: another entry
                 0x1018: JR_RA, 0x101C: NOP}
        insns, _labels, continuations = self.flow(words, known={0x1000, 0x1010})
        self.assertNotIn(0x1010, insns)
        self.assertEqual(continuations, {0x100C: 0x1010})
        text = self.emit(words, known={0x1000, 0x1010})
        self.assertIn("goto _sr_cont_00001010;", text)

    def test_patched_b_keeps_its_not_taken_path(self):
        words = {0x1000: b(0x1000, 0x1010), 0x1004: NOP, 0x1008: JR_RA, 0x100C: NOP,
                 0x1010: JR_RA, 0x1014: NOP}
        saved = dict(codegen.GUEST_PATCHES)
        codegen.GUEST_PATCHES[0x1000] = {"kind": "cond", "value": 0, "why": "test"}
        try:
            insns, _labels, _continuations = self.flow(words)
        finally:
            codegen.GUEST_PATCHES.clear()
            codegen.GUEST_PATCHES.update(saved)
        self.assertEqual(insns, set(words))
        saved_sites = dict(codegen.ENHANCEMENT_SITES)
        codegen.ENHANCEMENT_SITES[0x1000] = {"id": "test", "rewrite": {"kind": "word", "word": NOP}}
        try:
            insns, _labels, _continuations = self.flow(words)
        finally:
            codegen.ENHANCEMENT_SITES.clear()
            codegen.ENHANCEMENT_SITES.update(saved_sites)
        self.assertEqual(insns, set(words))

    def test_walk_ends_straight_line_exactly_where_the_analyzer_cfg_does(self):
        # Every direct conditional-branch encoding the walk takes, with rs/rt zero and
        # not: pc+8 is owned iff the canonical CFG gives the branch a fall-through edge.
        import analyze
        encodings = []
        for op in (4, 5, 6, 7, 20, 21, 22, 23):
            for rs in (0, 5):
                for rt in ((0, 6) if op in (4, 5, 20, 21) else (0,)):
                    encodings.append((op << 26) | (rs << 21) | (rt << 16))
        for sel in (0, 1, 2, 3, 0x10, 0x11):
            for rs in (0, 5):
                encodings.append((1 << 26) | (rs << 21) | (sel << 16))
        for op in (0x11, 0x12):
            for tf in range(4):
                encodings.append((op << 26) | (8 << 21) | (tf << 16))
        for base in encodings:
            word = base | 3                                   # target pc + 16
            with self.subTest(word=f"0x{word:08x}"):
                self.assertTrue(codegen.is_cond_branch(word))
                words = {0x1000: word, 0x1004: NOP, 0x1008: JR_RA, 0x100C: NOP,
                         0x1010: JR_RA, 0x1014: NOP}
                insns, _labels, _continuations = self.flow(words)
                falls_through = analyze.branch_kind(word) != analyze.BRANCH_UNCOND
                self.assertEqual(0x1008 in insns, falls_through)
                self.assertEqual(analyze.is_hard_terminator(word), not falls_through)

    def test_random_bodies_own_exactly_what_their_entry_runs(self):
        # Property test against the reference model above: with no other entries and no
        # split, a body is exactly the set of words its entry can run.
        import random
        rng = random.Random(1730)
        for case in range(400):
            n = rng.randrange(6, 40)
            base = 0x1000
            words, i = {}, 0
            while i < n:
                pc = base + 4 * i
                kind = rng.choice(("op", "op", "op", "b", "beq", "bne", "j", "jr"))
                if kind == "op" or i + 1 >= n:
                    words[pc] = addiu(2, 2, i + 1)
                    i += 1
                    continue
                target = base + 4 * rng.randrange(n)
                words[pc] = {"b": lambda: b(pc, target), "beq": lambda: beq(4, 5, pc, target),
                             "bne": lambda: bne(4, 0, pc, target), "j": lambda: j(target),
                             "jr": lambda: JR_RA}[kind]()
                words[pc + 4] = addiu(3, 3, i + 1)                 # delay slot: never a transfer
                i += 2
            with self.subTest(case=case):
                insns, _labels, _continuations = self.flow(words, start=base)
                self.assertEqual(insns, reachable(words, base))
                self.emit(words, start=base)                       # the slot check holds


if __name__ == "__main__":
    unittest.main()
