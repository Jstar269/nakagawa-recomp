# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Structural entry-role classification regressions (issue #51).

Every fixture here is an owned synthetic MIPS word image -- no title bytes and
no ELF container -- so the cases run host-neutral and in CI.  They cover the
entry shapes wiki doc 26 sections 25/30 name as the ones a single ``known`` set
conflates: interior epilogues, adjacent functions, address-taken leaves, shared
tails reached from several owners, jump-table landing pads, and ordinary
``jalr`` targets.
"""

from __future__ import annotations

import struct
import sys
import unittest
import unittest.mock
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import analyze  # noqa: E402
import entry_frame_balance as efb  # noqa: E402


# ---- tiny MIPS assembler, only the forms these fixtures need ----------------

def addiu(rt, rs, imm):
    return (0x09 << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def lw(rt, base, off):
    return (0x23 << 26) | (base << 21) | (rt << 16) | (off & 0xFFFF)


def sw(rt, base, off):
    return (0x2B << 26) | (base << 21) | (rt << 16) | (off & 0xFFFF)


def jr(rs=31):
    return (rs << 21) | 0x08


def jalr(rs, rd=31):
    return (rs << 21) | (rd << 11) | 0x09


def j(target):
    return (0x02 << 26) | ((target >> 2) & 0x03FFFFFF)


def jal(target):
    return (0x03 << 26) | ((target >> 2) & 0x03FFFFFF)


def b(delta_insns):
    """Unconditional ``b`` (``beq $zero, $zero, off``)."""
    return (0x04 << 26) | (delta_insns & 0xFFFF)


def bne(rs, rt, delta_insns):
    return (0x05 << 26) | (rs << 21) | (rt << 16) | (delta_insns & 0xFFFF)


def bnel(rs, rt, delta_insns):
    return (0x15 << 26) | (rs << 21) | (rt << 16) | (delta_insns & 0xFFFF)


def bgezal(rs, delta_insns):
    """The linking REGIMM branch (``bal`` is ``bgezal $zero``)."""
    return (1 << 26) | (rs << 21) | (0x11 << 16) | (delta_insns & 0xFFFF)


def bgez(rs, delta_insns):
    return (1 << 26) | (rs << 21) | (0x01 << 16) | (delta_insns & 0xFFFF)


def teqi(rs, code):
    """REGIMM trap-immediate: its immediate field is a code, not a displacement."""
    return (1 << 26) | (rs << 21) | (0x0C << 16) | (code & 0xFFFF)


def lui(rt, imm):
    return (0x0F << 26) | (rt << 16) | (imm & 0xFFFF)


def addu(rd, rs, rt):
    return (rs << 21) | (rt << 16) | (rd << 11) | 0x21


NOP = 0
SP = 29
RA = 31
V0 = 2
A0 = 4
T9 = 25


class WordImage:
    """Minimal ``read_at_vaddr``/``sec`` surface over a word map.

    ``entry_frame_balance`` and ``analyze.trace_function`` only need these two
    methods, so the fixtures avoid building an ELF container entirely.

    ``data_pointers`` optionally adds the one further surface
    ``analyze.code_pointer_evidence`` reads: a non-code section holding words.
    It stays empty by default, so a fixture that says nothing about data says
    nothing about data -- the evidence census must not invent a pointer table.
    """

    def __init__(self, words, data_pointers=()):
        self.words = dict(words)
        self.reloc = None
        self.data = b"".join(
            struct.pack("<I", value & 0xFFFFFFFF) for value in data_pointers
        )
        self.sections = (
            [dict(nm=".rodata", typ=1, addr=0, off=0, size=len(self.data))]
            if self.data else []
        )

    def read_at_vaddr(self, vaddr, n):
        if n != 4 or vaddr % 4 or vaddr not in self.words:
            return None
        return struct.pack("<I", self.words[vaddr] & 0xFFFFFFFF)

    def sec(self, name):
        return None


def image(words, data_pointers=()):
    lo = min(words)
    hi = max(words) + 4
    return WordImage(words, data_pointers), [(lo, hi)]


def emit(base, *words):
    return {base + 4 * i: w for i, w in enumerate(words)}


class FrameBalanceBasicsTests(unittest.TestCase):
    def test_epilogue_in_delay_slot_still_balances(self):
        """The control that a naive walker fails.

        The frame restore lives in the ``jr $ra`` delay slot, which is the
        ordinary MIPS idiom.  A walker that stops at the jump without executing
        its delay instruction reports this balanced callable as a continuation.
        """
        words = emit(
            0x1000,
            addiu(SP, SP, -0x20),   # prologue
            sw(RA, SP, 0x1C),
            lw(RA, SP, 0x1C),
            jr(RA),
            addiu(SP, SP, 0x20),    # epilogue hoisted into the delay slot
        )
        elf, ranges = image(words)
        profile = efb.profile_entry(elf, 0x1000, ranges)
        self.assertEqual(profile.return_deltas, frozenset({0}))
        self.assertEqual(efb.classify(profile), efb.CALLABLE)
        self.assertEqual(profile.frame, 0x20)

    def test_interior_epilogue_is_a_continuation(self):
        """Entering past the prologue releases a frame it never allocated."""
        words = emit(
            0x1000,
            addiu(SP, SP, -0x70),
            sw(RA, SP, 0x6C),
            NOP,
            lw(RA, SP, 0x6C),       # 0x100c: the interior resume PC
            jr(RA),
            addiu(SP, SP, 0x70),
        )
        elf, ranges = image(words)
        owner = efb.profile_entry(elf, 0x1000, ranges)
        resume = efb.profile_entry(elf, 0x100C, ranges)
        self.assertEqual(efb.classify(owner), efb.CALLABLE)
        self.assertEqual(efb.classify(resume), efb.CONTINUATION)
        self.assertEqual(resume.continuation_delta, 0x70)
        self.assertFalse(resume.has_prologue)
        # The owner is exactly 0x70 deep where the continuation begins.
        self.assertEqual(owner.depth_at(0x100C), 0x70)
        self.assertEqual(efb.verify_resume_entry(elf, ranges, 0x100C, 0x1000), [])

    def test_address_taken_tiny_leaf_stays_callable(self):
        """A frameless two-instruction leaf is balanced, not a continuation."""
        words = emit(0x1000, jr(RA), addu(V0, A0, 0))
        elf, ranges = image(words)
        profile = efb.profile_entry(elf, 0x1000, ranges)
        self.assertEqual(efb.classify(profile), efb.CALLABLE)
        self.assertFalse(profile.has_prologue)

    def test_unmodelled_sp_write_is_indeterminate_not_guessed(self):
        """A register-computed stack adjustment must not be silently modelled."""
        words = emit(
            0x1000,
            addiu(SP, SP, -0x20),
            addu(SP, SP, A0),       # dynamic adjustment: not modelled
            jr(RA),
            addiu(SP, SP, 0x20),
        )
        elf, ranges = image(words)
        profile = efb.profile_entry(elf, 0x1000, ranges)
        self.assertTrue(profile.unknown_sp)
        self.assertEqual(efb.classify(profile), efb.INDETERMINATE)
        self.assertFalse(profile.balanced)

    def test_branch_likely_nullifies_its_delay_slot_when_not_taken(self):
        """`bnel` skips the delay slot on the not-taken path.

        Both paths must still net zero; a walker that always applies the slot
        reports the fall-through path as unbalanced.
        """
        words = emit(
            0x1000,
            addiu(SP, SP, -0x10),
            (0x15 << 26) | (A0 << 21) | (0 << 16) | 2,  # bnel a0, zero, +2
            addiu(SP, SP, 0x10),                        # slot: taken path only
            jr(RA),                                     # not-taken return
            NOP,
            jr(RA),                                     # taken-path return
            NOP,
        )
        elf, ranges = image(words)
        profile = efb.profile_entry(elf, 0x1000, ranges)
        # Not-taken path never releases the frame, so the two paths disagree.
        self.assertIn(-0x10, profile.return_deltas)
        self.assertIn(0, profile.return_deltas)
        self.assertEqual(efb.classify(profile), efb.INDETERMINATE)

    #: Every MIPS encoding family that nullifies its delay slot, as
    #: (label, word) with a +2 instruction displacement.
    LIKELY_FORMS = (
        ("beql", (0x14 << 26) | (A0 << 21) | 2),
        ("bnel", (0x15 << 26) | (A0 << 21) | 2),
        ("blezl", (0x16 << 26) | (A0 << 21) | 2),
        ("bgtzl", (0x17 << 26) | (A0 << 21) | 2),
        # REGIMM: the likely bit lives in rt, not the opcode.
        ("bltzl", (1 << 26) | (A0 << 21) | (0x02 << 16) | 2),
        ("bgezl", (1 << 26) | (A0 << 21) | (0x03 << 16) | 2),
        ("bltzall", (1 << 26) | (A0 << 21) | (0x12 << 16) | 2),
        ("bgezall", (1 << 26) | (A0 << 21) | (0x13 << 16) | 2),
        # COP1 BC: rt bit 1 is nullify-delay.
        ("bc1fl", (0x11 << 26) | (8 << 21) | (0x02 << 16) | 2),
        ("bc1tl", (0x11 << 26) | (8 << 21) | (0x03 << 16) | 2),
    )

    def test_every_branch_likely_family_nullifies_its_delay_slot(self):
        """REGIMM and COP1 likely forms nullify too, not just opcodes 20-23.

        Missing a family is unsafe in one direction. The not-taken path wrongly
        applies the slot, so a frame release hoisted there is counted on a path
        that never runs it; the two genuinely disagreeing return paths collapse
        to a single value and the entry is reported as a confident ``callable``
        instead of ``indeterminate``. Before this was fixed, six of the ten
        forms below produced exactly that false ``callable``.
        """
        for label, branch in self.LIKELY_FORMS:
            with self.subTest(form=label):
                self.assertTrue(efb.branch_likely(branch), label)
                words = emit(
                    0x1000,
                    addiu(SP, SP, -0x10),
                    branch,
                    addiu(SP, SP, 0x10),   # slot: releases the frame if taken
                    jr(RA),
                    NOP,
                    jr(RA),
                    NOP,
                )
                elf, ranges = image(words)
                profile = efb.profile_entry(elf, 0x1000, ranges)
                self.assertIn(-0x10, profile.return_deltas, label)
                self.assertIn(0, profile.return_deltas, label)
                self.assertEqual(efb.classify(profile), efb.INDETERMINATE, label)

    def test_non_likely_branches_are_not_misreported_as_likely(self):
        """The plain and ``al`` REGIMM/COP1 forms still apply their slot."""
        for label, word in (
            ("bltz", (1 << 26) | (A0 << 21) | (0x00 << 16) | 2),
            ("bgez", (1 << 26) | (A0 << 21) | (0x01 << 16) | 2),
            ("bltzal", (1 << 26) | (A0 << 21) | (0x10 << 16) | 2),
            ("bgezal", (1 << 26) | (A0 << 21) | (0x11 << 16) | 2),
            ("bc1f", (0x11 << 26) | (8 << 21) | (0x00 << 16) | 2),
            ("bc1t", (0x11 << 26) | (8 << 21) | (0x01 << 16) | 2),
            ("beq", (0x04 << 26) | (A0 << 21) | 2),
            ("bne", (0x05 << 26) | (A0 << 21) | 2),
        ):
            with self.subTest(form=label):
                self.assertFalse(efb.branch_likely(word), label)

    def test_every_special_form_writing_sp_is_unmodelled(self):
        """A SPECIAL write to ``$sp`` must be indeterminate, never a silent no-op.

        ``jalr``/``mfhi``/``mflo``/``slt``/``sltu`` write ``rd`` just as the
        shift and arithmetic forms do; treating them as harmless would let the
        walk continue on a stack model it knows to be wrong.
        """
        for fn, label in (
            (0x00, "sll"), (0x01, "movci"), (0x02, "srl"), (0x03, "sra"),
            (0x04, "sllv"), (0x06, "srlv"), (0x07, "srav"), (0x09, "jalr"),
            (0x0A, "movz"), (0x0B, "movn"), (0x10, "mfhi"), (0x12, "mflo"),
            (0x21, "addu"), (0x23, "subu"), (0x25, "or"),
            (0x2A, "slt"), (0x2B, "sltu"),
        ):
            with self.subTest(form=label):
                word = (A0 << 21) | (V0 << 16) | (SP << 11) | fn
                self.assertEqual(efb.sp_effect(word), (0, True), label)

    def test_special_forms_that_do_not_write_rd_stay_modelled(self):
        """``rd`` is a code field for some SPECIAL forms, not a destination.

        ``syscall`` and ``break`` carry a 20-bit code across those bits, so a
        blanket "any SPECIAL with rd == $sp is unknown" rule would manufacture
        indeterminates for particular codes.
        """
        for fn, label in (
            (0x08, "jr"), (0x0C, "syscall"), (0x0D, "break"), (0x11, "mthi"),
            (0x13, "mtlo"), (0x18, "mult"), (0x1A, "div"),
        ):
            with self.subTest(form=label):
                self.assertEqual(efb.sp_effect((SP << 11) | fn), (0, False), label)


class SharedTailOwnershipTests(unittest.TestCase):
    """Wiki doc 26 section 14: multiple owners must not be silently reduced."""

    def _shared_tail_image(self, owner_b_frame, owner_b_extra=None):
        tail = 0x1200
        owner_a = emit(
            0x1000,
            addiu(SP, SP, -0x20),
            sw(RA, SP, 0x1C),
            j(tail),
            NOP,
        )
        body_b = [addiu(SP, SP, -owner_b_frame), sw(RA, SP, 0x0C)]
        if owner_b_extra:
            body_b.append(addiu(SP, SP, -owner_b_extra))
        body_b += [j(tail), NOP]
        owner_b = emit(0x1100, *body_b)
        shared = emit(
            tail,
            lw(RA, SP, 0x1C),
            jr(RA),
            addiu(SP, SP, 0x20),
        )
        return image({**owner_a, **owner_b, **shared}), tail

    def test_shared_tail_with_matching_depth_has_compatible_owners(self):
        """Two owners at the same depth are an ordinary shared tail."""
        (elf, ranges), tail = self._shared_tail_image(0x20)
        callables = {0x1000, 0x1100}
        owners = efb.find_frame_owners(elf, tail, ranges, callables)
        self.assertEqual(set(owners), callables)
        self.assertEqual(set(owners.values()), {0x20})
        self.assertEqual(efb.verify_resume_entry(elf, ranges, tail, 0x1000), [])

    def test_owner_at_a_mismatched_depth_is_excluded_not_silently_chosen(self):
        """A shared tail with a fixed release admits only one owner depth.

        Owner B reaches the tail 0x30 deep while the tail releases only 0x20,
        so owner B cannot balance -- which is the point: a candidate whose depth
        disagrees with the tail is *rejected*, never silently accepted as the
        owner.  This is also why the ``INCOMPATIBLE MULTIPLE OWNERS`` stop
        cannot fire for a fixed-release tail: every balanced owner is
        necessarily at the released depth, so all discovered owners agree.  The
        stop remains as a guard for path-dependent or indeterminate tails.
        """
        (elf, ranges), tail = self._shared_tail_image(0x20, owner_b_extra=0x10)
        owners = efb.find_frame_owners(elf, tail, ranges, {0x1000, 0x1100})
        self.assertEqual(owners, {0x1000: 0x20})
        self.assertNotIn(0x1100, owners)
        problems = efb.verify_resume_entry(elf, ranges, tail, 0x1100)
        self.assertTrue(
            any("not a balanced callable" in p for p in problems), problems
        )

    def test_owner_covers_distinguishes_reaching_from_adjacency(self):
        (elf, ranges), tail = self._shared_tail_image(0x20)
        self.assertTrue(efb.owner_covers(elf, 0x1000, tail, ranges, {0x1000, 0x1100}))
        # An address that is merely nearby is not covered.
        self.assertFalse(efb.owner_covers(elf, 0x1000, 0x1100, ranges, {0x1000, 0x1100}))


class JumpTableEntryTests(unittest.TestCase):
    """Switch landing pads are interior PCs, not fresh callables."""

    def _switch_image(self):
        owner = 0x1000
        case_a = 0x1020
        case_b = 0x1030
        callee = 0x1100
        words = {}
        words.update(emit(
            owner,
            addiu(SP, SP, -0x40),   # 0x1000
            sw(RA, SP, 0x3C),       # 0x1004
            bne(A0, 0, 5),          # 0x1008: bounds check -> 0x1020
            NOP,                    # 0x100c
            jr(T9),                 # 0x1010: computed switch dispatch
            NOP,                    # 0x1014
            NOP,                    # 0x1018
            NOP,                    # 0x101c
        ))
        words.update(emit(
            case_a,
            addiu(V0, 0, 1),        # 0x1020
            b(2),                   # 0x1024 -> 0x1030
            NOP,                    # 0x1028
            NOP,                    # 0x102c
        ))
        words.update(emit(
            case_b,
            lw(RA, SP, 0x3C),       # 0x1030: the shared epilogue
            jr(RA),                 # 0x1034
            addiu(SP, SP, 0x40),    # 0x1038
        ))
        words.update(emit(
            callee,
            addiu(SP, SP, -0x10),   # a genuine callable, reached by jalr
            addiu(V0, 0, 9),
            jr(RA),
            addiu(SP, SP, 0x10),
        ))
        return image(words), owner, case_a, case_b, callee

    def test_jump_table_landing_pads_classify_as_continuations(self):
        (elf, ranges), owner, case_a, case_b, _ = self._switch_image()
        for pad in (case_a, case_b):
            profile = efb.profile_entry(elf, pad, ranges)
            self.assertEqual(
                efb.classify(profile), efb.CONTINUATION,
                f"pad 0x{pad:08x} should not carry the callable contract",
            )
            self.assertEqual(profile.continuation_delta, 0x40)
            self.assertFalse(profile.has_prologue)

    def test_jump_table_owner_is_balanced_and_covers_its_pads(self):
        (elf, ranges), owner, case_a, case_b, _ = self._switch_image()
        profile = efb.profile_entry(elf, owner, ranges)
        self.assertEqual(efb.classify(profile), efb.CALLABLE)
        # The owner reaches its pads by fall-through past the computed jump,
        # at exactly the depth each pad releases.
        for pad in (case_a, case_b):
            self.assertEqual(profile.depth_at(pad), 0x40)
            self.assertEqual(efb.verify_resume_entry(elf, ranges, pad, owner), [])

    def test_indirectly_called_function_stays_callable(self):
        """`resumable != indirect` (wiki doc 26 section 24)."""
        (elf, ranges), _, _, _, callee = self._switch_image()
        profile = efb.profile_entry(elf, callee, ranges)
        self.assertEqual(efb.classify(profile), efb.CALLABLE)


class ResumeVerificationTests(unittest.TestCase):
    """`verify_resume_entry` must reject a wrong declaration, not pass it."""

    def _pair(self, resume_release=0x70):
        words = emit(
            0x1000,
            addiu(SP, SP, -0x70),
            sw(RA, SP, 0x6C),
            NOP,
            lw(RA, SP, 0x6C),       # 0x100c
            jr(RA),
            addiu(SP, SP, resume_release),
        )
        return image(words)

    def test_correct_pair_reports_no_problems(self):
        elf, ranges = self._pair()
        self.assertEqual(efb.verify_resume_entry(elf, ranges, 0x100C, 0x1000), [])

    def test_release_not_matching_owner_depth_is_reported(self):
        elf, ranges = self._pair(resume_release=0x30)
        problems = efb.verify_resume_entry(elf, ranges, 0x100C, 0x1000)
        self.assertTrue(any("only 0x70 deep there" in p for p in problems), problems)

    def test_resume_with_its_own_prologue_is_rejected(self):
        words = emit(
            0x1000,
            addiu(SP, SP, -0x20),
            NOP,
            jr(RA),
            addiu(SP, SP, 0x20),
        )
        words.update(emit(
            0x1100,
            addiu(SP, SP, -0x10),   # a real prologue: this is a callable
            jr(RA),
            addiu(SP, SP, 0x10),
        ))
        elf, ranges = image(words)
        problems = efb.verify_resume_entry(elf, ranges, 0x1100, 0x1000)
        self.assertTrue(
            any("own frame prologue" in p for p in problems), problems
        )

    def test_owner_that_never_reaches_the_resume_is_reported(self):
        words = emit(
            0x1000,
            addiu(SP, SP, -0x20),
            jr(RA),
            addiu(SP, SP, 0x20),
        )
        words.update(emit(
            0x1100,
            lw(RA, SP, 0x1C),
            jr(RA),
            addiu(SP, SP, 0x20),
        ))
        elf, ranges = image(words)
        problems = efb.verify_resume_entry(elf, ranges, 0x1100, 0x1000)
        self.assertTrue(any("never reaches" in p for p in problems), problems)

    def test_indeterminate_owner_is_not_silently_accepted(self):
        words = emit(
            0x1000,
            addiu(SP, SP, -0x70),
            addu(SP, SP, A0),       # unmodelled $sp write
            lw(RA, SP, 0x6C),
            jr(RA),
            addiu(SP, SP, 0x70),
        )
        elf, ranges = image(words)
        problems = efb.verify_resume_entry(elf, ranges, 0x1008, 0x1000)
        self.assertTrue(any("indeterminate" in p for p in problems), problems)


class DirectJumpAuditTests(unittest.TestCase):
    """Only one-way owner edges earn the direct-j resume contract."""

    def _one_way_tail(self):
        owner = 0x1000
        resume = 0x1010
        words = emit(
            owner,
            addiu(SP, SP, -0x20),
            sw(RA, SP, 0x1C),
            j(resume),
            NOP,
            lw(RA, SP, 0x1C),
            addiu(SP, SP, 0x20),
            jr(RA),
            NOP,
        )
        return image(words), {owner, resume}, owner, resume

    def test_one_way_direct_j_shared_tail_is_decisive(self):
        (elf, ranges), starts, owner, resume = self._one_way_tail()
        records = efb.audit_direct_j_candidates(elf, starts, ranges)
        self.assertEqual(len(records), 1)
        candidate = records[0]
        self.assertEqual(candidate.addr, resume)
        self.assertEqual(candidate.classification, efb.CONTINUATION)
        self.assertEqual(candidate.owners, (owner,))
        self.assertEqual(candidate.sources, (owner + 8,))
        self.assertEqual(
            efb.direct_j_resume_owners(elf, starts, ranges),
            {resume: owner},
        )

    def test_backwards_direct_j_loop_remains_ambiguous(self):
        owner = 0x1000
        resume = 0x1010
        words = emit(
            owner,
            addiu(SP, SP, -0x20),
            sw(RA, SP, 0x1C),
            j(resume),
            NOP,
            # The target has a return path, but its fall-through path loops
            # back to the incoming owner edge.  That is an interior loop
            # label, not evidence for a separately dispatchable resume.
            bne(A0, 0, 3),
            NOP,
            j(owner + 8),
            NOP,
            lw(RA, SP, 0x1C),
            addiu(SP, SP, 0x20),
            jr(RA),
            NOP,
        )
        elf, ranges = image(words)
        records = efb.audit_direct_j_candidates(elf, {owner, resume}, ranges)
        self.assertEqual(len(records), 1)
        candidate = records[0]
        self.assertEqual(candidate.classification, efb.AMBIGUOUS)
        self.assertIn("loop/back-edge", candidate.reason)
        self.assertEqual(efb.direct_j_resume_owners(elf, {owner, resume}, ranges), {})

    def test_owner_reaching_target_without_direct_source_is_ambiguous(self):
        owner = 0x1000
        resume = 0x1010
        foreign = 0x2000
        words = emit(
            owner,
            addiu(SP, SP, -0x20),
            sw(RA, SP, 0x1C),
            NOP,
            NOP,
            lw(RA, SP, 0x1C),
            addiu(SP, SP, 0x20),
            jr(RA),
            NOP,
        )
        words.update(emit(
            foreign,
            j(resume),
            NOP,
        ))
        elf, ranges = image(words)
        records = efb.audit_direct_j_candidates(
            elf, {owner, resume}, ranges
        )
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0].classification, efb.AMBIGUOUS)
        self.assertIn("direct-j source", records[0].reason)

    def test_prologue_bearing_positive_delta_is_not_a_resume(self):
        owner = 0x1000
        target = 0x1010
        words = emit(
            owner,
            addiu(SP, SP, -0x20),
            sw(RA, SP, 0x1C),
            j(target),
            NOP,
            addiu(SP, SP, -0x10),
            lw(RA, SP, 0x1C),
            addiu(SP, SP, 0x20),
            jr(RA),
            NOP,
        )
        elf, ranges = image(words)
        records = efb.audit_direct_j_candidates(elf, {owner, target}, ranges)
        self.assertEqual(len(records), 1)
        candidate = records[0]
        self.assertEqual(candidate.classification, efb.AMBIGUOUS)
        self.assertIn("frame prologue", candidate.reason)
        self.assertEqual(efb.direct_j_resume_owners(elf, {owner, target}, ranges), {})

    def test_callable_direct_j_target_is_outside_the_continuation_slice(self):
        owner = 0x1000
        target = 0x1010
        words = emit(
            owner,
            j(target),
            NOP,
            addiu(SP, SP, -0x10),
            jr(RA),
            addiu(SP, SP, 0x10),
        )
        elf, ranges = image(words)
        self.assertEqual(
            efb.audit_direct_j_candidates(elf, {owner, target}, ranges),
            (),
        )


class BranchEdgeCensusTests(unittest.TestCase):
    """The edge census must report what the encoding says, and nothing else."""

    def test_kinds_are_distinguished(self):
        words = emit(
            0x1000,
            bne(A0, 0, 3),          # 0x1000 -> 0x1010, conditional
            NOP,
            b(2),                   # 0x1008 -> 0x1014, unconditional idiom
            NOP,
            bgezal(A0, 1),          # 0x1010 -> 0x1018, a call
            NOP,
            NOP,
            NOP,
        )
        elf, ranges = image(words)
        edges = analyze.direct_branch_edges(elf, ranges)
        self.assertEqual(edges[0x1010], ((0x1000, analyze.BRANCH_COND),))
        self.assertEqual(edges[0x1014], ((0x1008, analyze.BRANCH_UNCOND),))
        self.assertEqual(edges[0x1018], ((0x1010, analyze.BRANCH_LINK),))

    def test_regimm_trap_immediate_is_not_decoded_as_a_branch(self):
        """A trap code is not a displacement.

        ``analyze.trace_function`` treats every REGIMM word as a branch, which
        is harmless there -- it only over-explores. Carrying that imprecision
        into the evidence census would be different in kind: it would report an
        incoming control-flow edge that does not exist, and an entry-role proof
        built on a fabricated predecessor is worse than no proof.
        """
        words = emit(0x1000, teqi(A0, 3), NOP, NOP, NOP, NOP)
        elf, ranges = image(words)
        self.assertIsNone(analyze.branch_target(0x1000, teqi(A0, 3)))
        self.assertEqual(analyze.direct_branch_edges(elf, ranges), {})
        # The same displacement in a real REGIMM branch *is* an edge.
        self.assertEqual(analyze.branch_target(0x1000, bgez(A0, 3)), 0x1010)

    def test_out_of_range_targets_are_not_reported(self):
        words = emit(0x1000, bne(A0, 0, -0x100), NOP, NOP)
        elf, ranges = image(words)
        self.assertEqual(analyze.direct_branch_edges(elf, ranges), {})

    def test_targets_filter_restricts_without_changing_the_scan(self):
        words = emit(0x1000, bne(A0, 0, 3), NOP, NOP, NOP, NOP, NOP)
        elf, ranges = image(words)
        self.assertIn(0x1010, analyze.direct_branch_edges(elf, ranges))
        self.assertEqual(
            analyze.direct_branch_edges(elf, ranges, targets={0x1234}), {}
        )


class CodePointerEvidenceTests(unittest.TestCase):
    """Each provenance kind must be reported apart from the others."""

    def test_jal_and_linking_branch_targets_are_separate_kinds(self):
        words = emit(
            0x1000,
            jal(0x1020),
            NOP,
            bgezal(A0, 5),          # 0x1008 -> 0x1020 as well
            NOP,
        )
        words.update(emit(0x1020, jr(RA), NOP))
        elf, ranges = image(words)
        evidence = analyze.code_pointer_evidence(elf, ranges)
        self.assertIn(0x1020, evidence["jal"])
        self.assertIn(0x1020, evidence["branch-link"])
        self.assertEqual(evidence["immediate"], frozenset())
        self.assertEqual(evidence["data"], frozenset())

    def test_la_materialized_pointer_is_immediate_evidence(self):
        words = emit(0x1000, lui(T9, 0), addiu(T9, T9, 0x1010), NOP, NOP, NOP)
        elf, ranges = image(words)
        evidence = analyze.code_pointer_evidence(elf, ranges)
        self.assertIn(0x1010, evidence["immediate"])

    def test_a_branch_between_the_halves_breaks_the_pair(self):
        """The high half does not survive a control-flow edge."""
        words = emit(
            0x1000, lui(T9, 0), bne(A0, 0, 1), addiu(T9, T9, 0x1010), NOP, NOP
        )
        elf, ranges = image(words)
        evidence = analyze.code_pointer_evidence(elf, ranges)
        self.assertEqual(evidence["immediate"], frozenset())

    def test_code_pointer_in_a_data_section_is_data_evidence(self):
        words = emit(0x1000, jr(RA), NOP, NOP, NOP)
        elf, ranges = image(words, data_pointers=(0x1008,))
        evidence = analyze.code_pointer_evidence(elf, ranges)
        self.assertIn(0x1008, evidence["data"])

    def test_an_image_without_data_sections_reports_no_data_pointers(self):
        words = emit(0x1000, jr(RA), NOP)
        elf, ranges = image(words)
        self.assertEqual(analyze.code_pointer_evidence(elf, ranges)["data"],
                         frozenset())


class DirectBranchAuditTests(unittest.TestCase):
    """The conditional-branch slice of #51.

    Every case starts from one provable fixture and changes exactly one word.
    That structure is the point: a rule is only shown to be load-bearing if the
    verdict flips when the single fact it inspects flips, with the rest of the
    proof untouched.
    """

    OWNER = 0x1000
    RESUME = 0x1018

    def base_words(self):
        """An if/else join inside one framed callable.

        The join has both a forward conditional-branch predecessor and a linear
        fall-in predecessor, which is the ordinary shape a branch target takes
        and the shape the direct-``j`` slice never had to consider.
        """
        return emit(
            self.OWNER,
            addiu(SP, SP, -0x20),   # 0x1000 prologue
            sw(RA, SP, 0x1C),       # 0x1004
            bne(A0, 0, 3),          # 0x1008 -> 0x1018, forward
            NOP,                    # 0x100c delay slot
            addiu(V0, 0, 1),        # 0x1010 else-body
            NOP,                    # 0x1014 falls into the join
            lw(RA, SP, 0x1C),       # 0x1018 the join / resume PC
            jr(RA),                 # 0x101c
            addiu(SP, SP, 0x20),    # 0x1020 epilogue in the delay slot
        )

    def audit(self, words, starts=None, data_pointers=()):
        elf, ranges = image(words, data_pointers)
        starts = {self.OWNER, self.RESUME} if starts is None else starts
        records = efb.audit_direct_branch_candidates(elf, starts, ranges)
        return elf, ranges, records

    def only(self, records, addr=None):
        addr = self.RESUME if addr is None else addr
        matching = [r for r in records if r.addr == addr]
        self.assertEqual(len(matching), 1, records)
        return matching[0]

    # ---- the provable base case -------------------------------------------

    def test_forward_branch_join_inside_one_owner_is_decisive(self):
        _, _, records = self.audit(self.base_words())
        candidate = self.only(records)
        self.assertEqual(candidate.classification, efb.CONTINUATION)
        self.assertEqual(candidate.role, efb.CONTINUATION)
        self.assertEqual(candidate.owners, (self.OWNER,))
        self.assertEqual(candidate.continuation_delta, 0x20)
        self.assertEqual(candidate.contradictions, ())
        self.assertEqual(candidate.source_pcs, (0x1008,))

    def test_the_owner_extent_actually_covers_the_promoted_target(self):
        """The extent tie, asserted positively.

        A promoted target must lie inside the owner's *recovered function
        boundary*, not merely on a stack walk through it.  The negative side of
        this check is unreachable while B3 and B5 hold -- ``trace_function``
        only stops at a foreign start that has a prologue or is a call target,
        and both are already excluded -- so it is the one rule the mutation
        sweep cannot kill.  Assert the property that is reachable instead.
        """
        elf, ranges = image(self.base_words())
        self.assertTrue(efb.owner_covers(
            elf, self.OWNER, self.RESUME, ranges, {self.OWNER, self.RESUME}
        ))

    def test_projection_matches_the_audit(self):
        elf, ranges = image(self.base_words())
        self.assertEqual(
            efb.direct_branch_resume_owners(
                elf, {self.OWNER, self.RESUME}, ranges
            ),
            {self.RESUME: self.OWNER},
        )

    def assert_flips(self, words, fragment, starts=None, data_pointers=()):
        """The mutated image must be ambiguous *for the stated reason*."""
        _, _, records = self.audit(words, starts, data_pointers)
        candidate = self.only(records)
        self.assertEqual(candidate.classification, efb.AMBIGUOUS, candidate.reason)
        self.assertIn(fragment, candidate.reason)
        return candidate

    # ---- B4 / B5: the call vetoes -----------------------------------------

    def test_linking_branch_source_is_not_continuation_evidence(self):
        """B4. ``bgezal`` is a call wearing a branch encoding."""
        words = self.base_words()
        words[0x1008] = bgezal(A0, 3)
        candidate = self.assert_flips(words, "call evidence")
        self.assertIn("linking-branch-source", candidate.contradictions)
        self.assertIn("linking-branch-target", candidate.contradictions)

    def test_the_link_veto_is_what_stops_the_linking_branch(self):
        """Mutation control for B4/B5.

        With the linking ``rt`` selectors emptied, the same word is read as an
        ordinary conditional branch and the candidate is promoted. That is the
        proof that the veto -- not some other rule -- is doing the work.
        """
        words = self.base_words()
        words[0x1008] = bgezal(A0, 3)
        with unittest.mock.patch.object(analyze, "REGIMM_LINK_RT", frozenset()):
            _, _, records = self.audit(words)
        self.assertEqual(self.only(records).classification, efb.CONTINUATION)

    def test_direct_jal_target_is_not_continuation_evidence(self):
        """B5. A ``jal`` anywhere proves a fresh-call entry on some path."""
        words = self.base_words()
        words.update(emit(0x2000, jal(self.RESUME), NOP))
        candidate = self.assert_flips(words, "call evidence")
        self.assertEqual(candidate.contradictions, ("direct-jal-target",))

    # ---- B3: the target's own prologue ------------------------------------

    def test_target_with_its_own_prologue_is_not_a_resume(self):
        """B3. A positive return delta is not by itself a resume signature.

        This target allocates 0x10 and releases 0x30, so it nets +0x20 and
        classifies as a continuation by stack role alone -- while its first
        instruction is the callable contract.  Every other rule is satisfied
        here, including the owner and depth checks, so only B3 stops it.
        """
        words = emit(
            self.OWNER,
            addiu(SP, SP, -0x20),   # 0x1000
            sw(RA, SP, 0x1C),       # 0x1004
            bne(A0, 0, 3),          # 0x1008 -> 0x1018
            NOP,                    # 0x100c
            addiu(V0, 0, 1),        # 0x1010
            NOP,                    # 0x1014
            addiu(SP, SP, -0x10),   # 0x1018 candidate: its own prologue
            lw(RA, SP, 0x2C),       # 0x101c
            jr(RA),                 # 0x1020
            addiu(SP, SP, 0x30),    # 0x1024
        )
        elf, ranges = image(words)
        target = efb.profile_entry(elf, self.RESUME, ranges)
        self.assertEqual(efb.classify(target), efb.CONTINUATION)
        self.assertTrue(target.has_prologue)
        self.assertTrue(efb.profile_entry(elf, self.OWNER, ranges).balanced)
        self.assert_flips(words, "target has its own frame prologue")

    # ---- B6: address-taken ------------------------------------------------

    def test_address_taken_constant_leaves_the_candidate_ambiguous(self):
        """B6. An `la` of the address means an indirect dispatch may exist."""
        words = self.base_words()
        words.update(emit(0x2000, lui(T9, 0), addiu(T9, T9, self.RESUME), NOP))
        candidate = self.assert_flips(words, "address-taken evidence")
        self.assertEqual(candidate.contradictions, ("address-taken-constant",))

    def test_code_pointer_in_data_leaves_the_candidate_ambiguous(self):
        """B6. A jump table, vtable or callback array is the same problem.

        The entry may well be an interior continuation, but the incoming
        contract of whatever reads that table is not visible here, so the
        address is left unchanged rather than promoted on a guess.
        """
        candidate = self.assert_flips(
            self.base_words(), "address-taken evidence",
            data_pointers=(self.RESUME,),
        )
        self.assertEqual(candidate.contradictions, ("code-pointer-in-data",))

    def test_an_unrelated_data_pointer_does_not_veto(self):
        """Mutation control for B6: only the candidate's own address counts."""
        _, _, records = self.audit(self.base_words(), data_pointers=(0x1010,))
        self.assertEqual(self.only(records).classification, efb.CONTINUATION)

    # ---- B7: delay-slot structure -----------------------------------------

    def test_a_target_that_is_a_delay_slot_is_not_an_entry(self):
        """B7. ``b`` at 0x1014 makes 0x1018 its delay slot, not a boundary."""
        words = self.base_words()
        words[0x1014] = b(1)
        self.assert_flips(words, "delay slot of a hard terminator")

    def test_the_delay_slot_veto_is_load_bearing(self):
        """Mutation control for B7."""
        words = self.base_words()
        words[0x1014] = b(1)
        with unittest.mock.patch.object(
            efb, "is_hard_terminator", lambda word: False
        ):
            _, _, records = self.audit(words)
        self.assertEqual(self.only(records).classification, efb.CONTINUATION)

    # ---- B8 / B9: loops ---------------------------------------------------

    def test_backward_branch_source_is_a_loop_header(self):
        """B8. Every loop compiles to a backward conditional branch."""
        words = emit(
            self.OWNER,
            addiu(SP, SP, -0x20),   # 0x1000
            sw(RA, SP, 0x1C),       # 0x1004
            NOP,                    # 0x1008
            NOP,                    # 0x100c
            addiu(V0, 0, 1),        # 0x1010
            NOP,                    # 0x1014
            lw(RA, SP, 0x1C),       # 0x1018 loop header / candidate
            bne(A0, 0, -2),         # 0x101c -> 0x1018, backward
            NOP,                    # 0x1020
            jr(RA),                 # 0x1024
            addiu(SP, SP, 0x20),    # 0x1028
        )
        self.assert_flips(words, "a loop header has no single resume contract")

    def test_forward_edge_that_closes_a_cycle_is_ambiguous(self):
        """B9. A forward branch can still be a loop entry.

        The source is below the target, so B8 says nothing; the target's own
        CFG reaching that source is what exposes the cycle.
        """
        words = emit(
            self.OWNER,
            addiu(SP, SP, -0x20),   # 0x1000
            sw(RA, SP, 0x1C),       # 0x1004
            bne(A0, 0, 3),          # 0x1008 -> 0x1018, forward
            NOP,                    # 0x100c
            addiu(V0, 0, 1),        # 0x1010
            NOP,                    # 0x1014
            bgez(A0, -5),           # 0x1018 candidate; -> 0x1008, back onto the source
            NOP,                    # 0x101c
            lw(RA, SP, 0x1C),       # 0x1020
            jr(RA),                 # 0x1024
            addiu(SP, SP, 0x20),    # 0x1028
        )
        self.assert_flips(words, "closes a loop rather than entering a tail")

    # ---- B13: every direct predecessor, not only the branch ones ----------

    def test_a_foreign_direct_j_predecessor_blocks_the_branch_proof(self):
        """B13. A branch edge selects the candidate; it does not prove it.

        The owner's forward branch into the join is exactly the base case, and
        on its own it would be decisive.  A second body reaches the same address
        by direct ``j``, which makes it a shared tail with more than one
        reaching owner -- so the branch edge is now evidence about one incoming
        path out of two.  This is the case the first version of this audit
        promoted wrongly, and the direct-``j`` slice had already declined the
        same addresses for the same reason.
        """
        words = self.base_words()
        words.update(emit(0x0F00, j(self.RESUME), NOP))
        candidate = self.assert_flips(words, "does not reach source")
        self.assertIn((0x0F00, efb.EDGE_J), candidate.sources)
        self.assertEqual(candidate.owners, (self.OWNER,))

    def test_an_in_owner_direct_j_makes_the_analyzer_truncate_the_owner(self):
        """The extent check, and why it is a live rule rather than a backstop.

        The extra ``j`` predecessor sits *inside* the owner, so B11 is satisfied
        and every stack-level rule passes.  ``trace_function`` nevertheless
        treats a ``j`` whose target is an analyzer start as a **tail call** and
        stops there, so the owner's recovered extent does not contain the
        target at all.  Promoting it would assert an interior continuation of a
        function the analyzer does not believe contains it, and the two
        decisions would disagree in the generated code.
        """
        words = emit(
            self.OWNER,
            addiu(SP, SP, -0x20),   # 0x1000
            sw(RA, SP, 0x1C),       # 0x1004
            bne(A0, 0, 3),          # 0x1008 -> 0x1018
            NOP,                    # 0x100c
            j(self.RESUME),         # 0x1010 -> 0x1018, from inside the owner
            NOP,                    # 0x1014
            lw(RA, SP, 0x1C),       # 0x1018 candidate
            jr(RA),                 # 0x101c
            addiu(SP, SP, 0x20),    # 0x1020
        )
        elf, ranges = image(words)
        # Every stack-level fact is in order ...
        owner = efb.profile_entry(elf, self.OWNER, ranges)
        self.assertTrue(owner.balanced)
        self.assertEqual(owner.depth_at(self.RESUME), 0x20)
        self.assertEqual(owner.depth_at(0x1010), 0x20)
        # ... but the analyzer's own boundary decision is not.
        self.assertFalse(efb.owner_covers(
            elf, self.OWNER, self.RESUME, ranges, {self.OWNER, self.RESUME}
        ))
        candidate = self.assert_flips(words, "recovered extent does not cover it")
        self.assertIn((0x1010, efb.EDGE_J), candidate.sources)

    def test_a_backward_direct_j_predecessor_is_a_loop_edge(self):
        """B13 composes with B8: the union is what B8 ranges over."""
        words = emit(
            self.OWNER,
            addiu(SP, SP, -0x20),   # 0x1000
            sw(RA, SP, 0x1C),       # 0x1004
            bne(A0, 0, 3),          # 0x1008 -> 0x1018, forward
            NOP,                    # 0x100c
            addiu(V0, 0, 1),        # 0x1010
            NOP,                    # 0x1014
            lw(RA, SP, 0x1C),       # 0x1018 candidate
            bne(A0, 0, 3),          # 0x101c -> 0x102c, skipping the back-edge
            NOP,                    # 0x1020
            j(self.RESUME),         # 0x1024 -> 0x1018, backward
            NOP,                    # 0x1028
            jr(RA),                 # 0x102c
            addiu(SP, SP, 0x20),    # 0x1030
        )
        self.assert_flips(words, "a loop header has no single resume contract")

    # ---- B10 / B11: owners and depth --------------------------------------

    def test_two_balanced_owners_reaching_the_target_stop_the_promotion(self):
        """B10. A second owner is a reason to stop, not to pick one."""
        words = self.base_words()
        # A second framed callable that falls through into the same join.
        words.update(emit(
            0x0F00,
            addiu(SP, SP, -0x20),   # 0x0f00 prologue, same frame
            sw(RA, SP, 0x1C),       # 0x0f04
            b(0x43),                # 0x0f08 -> 0x1018, the shared join
            NOP,                    # 0x0f0c
        ))
        candidate = self.assert_flips(
            words, "multiple balanced owners",
            starts={self.OWNER, self.RESUME, 0x0F00},
        )
        self.assertEqual(candidate.owners, ())

    def test_owner_reaching_the_target_at_two_depths_cannot_be_an_owner(self):
        """B11, and why two of its branches are guards rather than live paths.

        An owner that reaches a fixed-release target at two different depths
        cannot balance on both paths, so it is excluded from the owner index
        before the depth comparison is ever reached -- the same shape the
        existing ``INCOMPATIBLE MULTIPLE OWNERS`` note records for shared tails.
        The ``depth is None`` and ``depth != release`` branches are therefore
        retained as guards for path-dependent tails, and the *reachable*
        property is asserted here instead: such an owner is dropped and named,
        never silently chosen.
        """
        words = emit(
            self.OWNER,
            addiu(SP, SP, -0x20),   # 0x1000
            bne(A0, 0, 4),          # 0x1004 -> 0x1018 arriving 0x20 deep
            NOP,                    # 0x1008
            addiu(SP, SP, -0x10),   # 0x100c fall-through deepens by 0x10 ...
            NOP,                    # 0x1010
            NOP,                    # 0x1014 ... so it arrives 0x30 deep
            lw(RA, SP, 0x1C),       # 0x1018 candidate
            jr(RA),                 # 0x101c
            addiu(SP, SP, 0x20),    # 0x1020
        )
        elf, ranges = image(words)
        owner = efb.profile_entry(elf, self.OWNER, ranges)
        self.assertIsNone(owner.depth_at(self.RESUME))
        self.assertFalse(owner.balanced)
        self.assertEqual(
            efb.index_frame_owners(
                elf, {self.OWNER, self.RESUME}, ranges, {self.RESUME}
            ),
            {self.RESUME: {}},
        )
        self.assert_flips(words, "no balanced framed callable reaches the target")

    def test_owner_that_does_not_reach_a_source_is_ambiguous(self):
        """B11. A cross-boundary edge is not owned by this analysis."""
        words = emit(
            self.OWNER,
            addiu(SP, SP, -0x20),   # 0x1000
            sw(RA, SP, 0x1C),       # 0x1004
            NOP,                    # 0x1008
            NOP,                    # 0x100c
            NOP,                    # 0x1010
            NOP,                    # 0x1014
            lw(RA, SP, 0x1C),       # 0x1018 candidate
            jr(RA),                 # 0x101c
            addiu(SP, SP, 0x20),    # 0x1020
        )
        # A foreign block below the owner branches into its interior.
        words.update(emit(0x0F00, bne(A0, 0, 0x45), NOP))
        self.assert_flips(
            words, "does not reach source",
            starts={self.OWNER, self.RESUME},
        )

    # ---- B12: the fall-in predecessor -------------------------------------

    def fall_in_words(self):
        """An owner that branches over a block which falls into the candidate.

        The owner's only edge to the join is the unconditional branch at 0x1008,
        which has no fall-through, so 0x1010/0x1014 belong to some other body --
        yet 0x1014 still runs into the candidate linearly.  The direct-``j``
        slice never met this shape: a promoted ``j`` target is preceded by a
        hard terminator, so it has no linear predecessor at all.
        """
        return emit(
            self.OWNER,
            addiu(SP, SP, -0x20),   # 0x1000
            sw(RA, SP, 0x1C),       # 0x1004
            b(3),                   # 0x1008 -> 0x1018, and no fall-through
            NOP,                    # 0x100c delay slot
            addiu(V0, 0, 7),        # 0x1010 foreign block, not a terminator
            NOP,                    # 0x1014 foreign, falls into the candidate
            lw(RA, SP, 0x1C),       # 0x1018 candidate
            jr(RA),                 # 0x101c
            addiu(SP, SP, 0x20),    # 0x1020
        )

    def test_fall_in_from_outside_the_owner_is_ambiguous(self):
        """B12. An incoming linear path the proven owner does not have."""
        candidate = self.assert_flips(self.fall_in_words(), "can be fallen into")
        self.assertEqual(candidate.owners, (self.OWNER,))

    def test_the_fall_in_rule_is_what_stops_it(self):
        """Mutation control for B12.

        One word changes: the foreign block now ends in a hard terminator, so
        nothing can fall into the candidate and the owner's branch is its only
        incoming edge.  ``jr`` rather than ``j`` deliberately -- a ``j`` here
        would add a direct predecessor outside the owner and B13 would stop it
        for a different reason, which would not isolate B12.
        """
        words = self.fall_in_words()
        words[0x1010] = jr(RA)
        _, _, records = self.audit(words)
        self.assertEqual(self.only(records).classification, efb.CONTINUATION)

    # ---- B2: stack role ---------------------------------------------------

    def test_balanced_branch_target_is_reported_a_callable_boundary(self):
        """A positive finding, distinct from `ambiguous`."""
        words = emit(
            self.OWNER,
            addiu(SP, SP, -0x20),   # 0x1000
            bne(A0, 0, 4),          # 0x1004 -> 0x1018
            NOP,                    # 0x1008
            NOP,                    # 0x100c
            jr(RA),                 # 0x1010 the owner's own return
            addiu(SP, SP, 0x20),    # 0x1014
            jr(RA),                 # 0x1018 a balanced frameless leaf
            NOP,                    # 0x101c
        )
        _, _, records = self.audit(words)
        candidate = self.only(records)
        self.assertEqual(candidate.classification, efb.CALLABLE_BOUNDARY)
        self.assertEqual(candidate.role, efb.CALLABLE)
        self.assertEqual(candidate.owners, ())

    def test_indeterminate_stack_role_is_ambiguous_not_promoted(self):
        """B2. The role check runs before any owner is looked for."""
        words = self.base_words()
        words[self.RESUME] = addu(SP, SP, A0)   # unmodelled $sp write
        candidate = self.assert_flips(words, "stack role is indeterminate")
        self.assertEqual(candidate.role, efb.INDETERMINATE)

    # ---- branch-likely: the delay-slot model is load-bearing ---------------

    def test_branch_likely_owner_stays_provable(self):
        """The taken path executes the slot; the not-taken path nullifies it."""
        words = emit(
            self.OWNER,
            addiu(SP, SP, -0x20),   # 0x1000
            sw(RA, SP, 0x1C),       # 0x1004
            bnel(A0, 0, 3),         # 0x1008 -> 0x1018
            NOP,                    # 0x100c nullified when not taken
            addiu(V0, 0, 1),        # 0x1010
            NOP,                    # 0x1014
            lw(RA, SP, 0x1C),       # 0x1018 candidate
            jr(RA),                 # 0x101c
            addiu(SP, SP, 0x20),    # 0x1020
        )
        _, _, records = self.audit(words)
        self.assertEqual(self.only(records).classification, efb.CONTINUATION)

    def likely_slot_words(self):
        """A ``$sp`` adjustment hoisted into a branch-likely delay slot.

        The taken path executes the slot and arrives 0x30 deep; the not-taken
        path nullifies it and arrives 0x20 deep.  Since the candidate releases a
        fixed 0x30, the owner's two return paths genuinely disagree and it is
        not a balanced callable -- so there is no owner and nothing to promote.
        """
        return emit(
            self.OWNER,
            addiu(SP, SP, -0x20),   # 0x1000
            sw(RA, SP, 0x1C),       # 0x1004
            bnel(A0, 0, 3),         # 0x1008 -> 0x1018
            addiu(SP, SP, -0x10),   # 0x100c taken-path-only adjustment
            addiu(V0, 0, 1),        # 0x1010
            NOP,                    # 0x1014
            lw(RA, SP, 0x1C),       # 0x1018 candidate, releases 0x30
            jr(RA),                 # 0x101c
            addiu(SP, SP, 0x30),    # 0x1020
        )

    def test_nullified_likely_slot_keeps_the_owner_honestly_unbalanced(self):
        words = self.likely_slot_words()
        elf, ranges = image(words)
        owner = efb.profile_entry(elf, self.OWNER, ranges)
        self.assertEqual(owner.return_deltas, frozenset({0, 0x10}))
        self.assertFalse(owner.balanced)
        self.assert_flips(words, "no balanced framed callable reaches the target")

    def test_ignoring_likely_nullification_manufactures_a_false_owner(self):
        """Mutation: the delay-slot model is what prevents a wrong promotion.

        With nullification ignored, both paths apply the slot, both arrive 0x30
        deep, the two return deltas collapse to zero, and the owner looks
        balanced at exactly the depth the candidate releases.  The audit then
        promotes an entry whose owner does not actually have one stack contract.
        That is the concrete cost of getting delay slots wrong here, and it is
        why the likely families are enumerated rather than approximated.
        """
        words = self.likely_slot_words()
        with unittest.mock.patch.object(efb, "branch_likely", lambda word: False):
            elf, ranges = image(words)
            self.assertTrue(efb.profile_entry(elf, self.OWNER, ranges).balanced)
            records = efb.audit_direct_branch_candidates(
                elf, {self.OWNER, self.RESUME}, ranges
            )
        candidate = self.only(records)
        self.assertEqual(candidate.classification, efb.CONTINUATION)
        self.assertEqual(candidate.continuation_delta, 0x30)


if __name__ == "__main__":
    unittest.main()
