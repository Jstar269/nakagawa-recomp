# SPDX-License-Identifier: GPL-2.0-or-later

"""Structural gates for the AOT/interpreter cosimulation fixture.

The executable comparison lives in ``fixtures/cosim/cosim_selftest.c`` and needs a
built toolchain.  These tests are the part that can run anywhere: they check the
properties the comparison silently DEPENDS on, each of which has already been
observed to fail in practice or would make a cell pass vacuously.
"""

import importlib.util
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "fixtures" / "cosim" / "generate.py"

_spec = importlib.util.spec_from_file_location("cosim_generate", GENERATOR)
cosim = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cosim)


JR_RA = 0x03E00008


def decode(word: int) -> tuple:
    """Classify one instruction the way both execution lanes dispatch on it."""
    primary = word >> 26
    if primary == 0x00:
        return ("special", word & 0x3F)
    if primary == 0x11:
        return ("cop1", (word >> 21) & 0x1F, word & 0x3F)
    return ("op", primary)


# Every instruction form the cells actually execute. This is the fixture's own
# inventory, not a claim about the interpreter: it exists so a cell edit that
# adds or drops a form is a visible, reviewed change rather than a silent one.
EXPECTED_FORMS = frozenset(
    {
        ("op", 0x09),   # addiu
        ("op", 0x0D),   # ori
        ("op", 0x0F),   # lui
        ("op", 0x02),   # j
        ("op", 0x03),   # jal
        ("op", 0x04),   # beq
        ("op", 0x05),   # bne
        ("op", 0x20),   # lb
        ("op", 0x21),   # lh
        ("op", 0x23),   # lw
        ("op", 0x24),   # lbu
        ("op", 0x25),   # lhu
        ("op", 0x28),   # sb
        ("op", 0x29),   # sh
        ("op", 0x2B),   # sw
        ("op", 0x31),   # lwc1
        ("op", 0x39),   # swc1
        ("special", 0x00),  # sll (and the nop encoding)
        ("special", 0x02),  # srl
        ("special", 0x03),  # sra
        ("special", 0x08),  # jr
        ("special", 0x09),  # jalr
        ("special", 0x10),  # mfhi
        ("special", 0x12),  # mflo
        ("special", 0x18),  # mult
        ("special", 0x19),  # multu
        ("special", 0x21),  # addu
        ("special", 0x23),  # subu
        ("special", 0x24),  # and
        ("special", 0x25),  # or
        ("special", 0x26),  # xor
        ("special", 0x2A),  # slt
        ("special", 0x2B),  # sltu
        ("cop1", 0x00, 0x00),  # mfc1
        ("cop1", 0x04, 0x00),  # mtc1
        ("cop1", 0x10, 0x00),  # add.s
        ("cop1", 0x10, 0x02),  # mul.s
        ("cop1", 0x10, 0x24),  # cvt.w.s
    }
)


class FixtureDeterminismTests(unittest.TestCase):
    def test_prx_and_header_are_byte_deterministic(self):
        """The committed artifact is the recipe, so the recipe must be stable."""
        self.assertEqual(cosim.build_prx(), cosim.build_prx())
        self.assertEqual(cosim.build_psp_header(), cosim.build_psp_header())
        self.assertEqual(cosim.manifest_header(), cosim.manifest_header())

    def test_manifest_header_is_pure_ascii(self):
        cosim.manifest_header().decode("ascii")


class FixtureLayoutTests(unittest.TestCase):
    def setUp(self):
        self.layout = cosim.cell_layout()
        self.text = cosim.build_text_segment()

    def test_cells_do_not_overlap_and_stay_inside_the_text(self):
        placed = sorted(
            (offset, offset + len(words) * 4, name)
            for name, (offset, words) in self.layout.items()
        )
        previous_end = 0
        for start, end, name in placed:
            self.assertGreaterEqual(start, previous_end, f"cell {name} overlaps its predecessor")
            self.assertLessEqual(end, len(self.text), f"cell {name} runs past the text extent")
            previous_end = end

    def test_entry_function_reaches_every_discoverable_cell(self):
        """Discovery is what turns a cell into a translated f_<addr> body.

        A cell the analyzer never claims is not translated at all, and its lane
        AOT run would silently become a second interpreter run.
        """
        head = cosim.entry_words(self.layout)
        targets = {
            ((word & 0x03FFFFFF) << 2) for word in head if (word >> 26) == 0x03
        }
        for name, (offset, _words) in self.layout.items():
            if name in cosim.UNDISCOVERED_CELLS:
                self.assertNotIn(offset, targets, f"{name} must not be reachable from entry")
            else:
                self.assertIn(offset, targets, f"cell {name} is never called from the entry")

    def test_every_cell_ends_in_a_register_transfer_with_a_delay_slot(self):
        """Leaving through `jr` IS the cosim synchronization point.

        Most cells end in `jr $ra`; the tail-call cell ends in `jr $rs` and
        relinquishes control through its callee's `jr $ra` instead. Either way the
        last two words must be a register transfer and its delay slot, or the cell
        has no defined exit for either lane.
        """
        for name, (_offset, words) in self.layout.items():
            self.assertGreaterEqual(len(words), 2, f"cell {name} is too short to have an exit")
            terminator = words[-2]
            self.assertEqual(terminator >> 26, 0x00, f"cell {name} does not end in a transfer")
            self.assertEqual(terminator & 0x3F, 0x08, f"cell {name} does not end in jr")
            if name != "jrtail":
                self.assertEqual(terminator, JR_RA, f"cell {name} does not end in jr $ra")

    def test_return_trampoline_is_not_a_discovered_function(self):
        """The harness registers its own inert body at this address in BOTH lanes.

        If the analyzer claimed it, lane AOT would register generated code there
        and the two lanes would stop at different things.
        """
        self.assertIn("ret", cosim.UNDISCOVERED_CELLS)

    def test_r0_cell_does_not_end_with_an_instruction_that_writes_r0(self):
        """Regression for a defect the mutation campaign found.

        `nop` encodes as `sll $zero, $zero, 0`.  As the r0 cell's return delay
        slot it REPAIRS $r0 in a lane that has lost $r0 suppression, and since no
        guest read can observe $r0 either, the whole cell became vacuous: the
        `allow-r0-write` mutant survived until this slot stopped writing $r0.
        """
        _offset, words = self.layout["r0"]
        final = words[-1]
        kind = decode(final)
        writes_r0 = False
        if kind[0] == "special":
            writes_r0 = ((final >> 11) & 0x1F) == 0
        elif kind[0] == "op":
            writes_r0 = ((final >> 16) & 0x1F) == 0
        self.assertFalse(
            writes_r0,
            "the r0 cell's return delay slot writes $r0 and would mask lost "
            "suppression",
        )


class FixtureRelocationTests(unittest.TestCase):
    def test_every_transfer_word_has_a_relocation_record(self):
        """An unrelocated `j`/`jal` decodes to a segment-relative target.

        Nothing downstream would reject it: the guest would simply transfer
        somewhere else, in BOTH lanes identically, and the cell would pass while
        testing nothing.
        """
        text = cosim.build_text_segment()
        recorded = {offset for offset, _info in cosim.relocation_records()}
        for offset in range(0, len(text), 4):
            word = struct.unpack_from("<I", text, offset)[0]
            if (word >> 26) in (0x02, 0x03):
                self.assertIn(
                    offset, recorded,
                    f"transfer at text+0x{offset:x} has no R_MIPS_26 record",
                )

    def test_relocation_records_are_only_for_transfers(self):
        text = cosim.build_text_segment()
        for offset, _info in cosim.relocation_records():
            word = struct.unpack_from("<I", text, offset)[0]
            self.assertIn((word >> 26), (0x02, 0x03))


class FixtureSemanticTests(unittest.TestCase):
    def setUp(self):
        self.layout = cosim.cell_layout()

    def test_aliasing_cell_names_two_distinct_leaves(self):
        """The `jrslot` cell separates a correct call from a late-resolved one.

        Collapsing the two leaves would make both outcomes identical and the cell
        would pass no matter when the target register is read.
        """
        leaf_a = cosim.BASE + self.layout["link_leaf"][0]
        leaf_b = cosim.BASE + self.layout["link_leaf_b"][0]
        self.assertNotEqual(leaf_a, leaf_b)
        text = cosim.build_text_segment()
        base = self.layout["jrslot"][0]
        for slot, expected in ((0x0C, leaf_a), (0x14, leaf_b)):
            hi = struct.unpack_from("<I", text, base + slot)[0] & 0xFFFF
            lo = struct.unpack_from("<I", text, base + slot + 4)[0] & 0xFFFF
            self.assertEqual((hi << 16) | lo, expected)

    def test_the_two_leaves_report_distinguishable_results(self):
        """Both $v0 and $v1 must separate the leaves, so neither can pass alone."""
        _off_a, leaf_a = self.layout["link_leaf"]
        _off_b, leaf_b = self.layout["link_leaf_b"]
        marker_a, marker_b = leaf_a[0] & 0xFFFF, leaf_b[0] & 0xFFFF
        delta_a, delta_b = leaf_a[1] & 0xFFFF, leaf_b[1] & 0xFFFF
        self.assertNotEqual(marker_a, marker_b)
        self.assertNotEqual(delta_a, delta_b)

    def test_branch_cell_keeps_its_condition_readable_before_the_slot(self):
        """The third branch compares registers its own delay slot then changes.

        If the branch offset ever stopped skipping the following instruction, a
        lane that evaluated the condition too late would land in the same place
        and the property would go untested.
        """
        _offset, words = self.layout["branch"]
        index = 9
        branch = words[index]
        self.assertEqual(branch >> 26, 0x04, "expected the third beq at word 9")

        # The slot must write one of the registers the branch compares, or the
        # ordering property is not exercised at all.
        compared = {(branch >> 21) & 0x1F, (branch >> 16) & 0x1F}
        slot = words[index + 1]
        self.assertEqual(slot >> 26, 0x09, "expected an addiu delay slot")
        self.assertIn((slot >> 16) & 0x1F, compared,
                      "the delay slot must rewrite a register the branch compares")

        # Taking the branch must skip at least one instruction, so a lane that
        # evaluated the condition after the slot lands somewhere different.
        target_index = index + 1 + (branch & 0xFFFF)
        self.assertGreater(target_index, index + 2,
                           "taken and not-taken paths converge; nothing is proven")
        skipped = words[index + 2]
        self.assertEqual(skipped >> 26, 0x09, "the skipped word should be the addiu marker")

    def test_scratch_and_stack_live_inside_the_observed_window(self):
        """A cell can only be compared on memory the harness actually seeds."""
        self.assertGreaterEqual(cosim.SCRATCH, cosim.WINDOW_LO)
        self.assertLess(cosim.SCRATCH + cosim.SCRATCH_SIZE, cosim.WINDOW_HI)
        self.assertGreater(cosim.STACK, cosim.WINDOW_LO)
        self.assertLessEqual(cosim.STACK, cosim.WINDOW_HI)

    def test_fixture_instruction_inventory_is_exactly_the_declared_set(self):
        """Every form the cells execute is declared, and every declared form is used.

        The interpreter opcodes added for this gate exist BECAUSE a cell executes
        them. This test is what keeps that true in both directions.
        """
        seen = set()
        for name, (_offset, words) in self.layout.items():
            for word in words:
                seen.add(decode(word))
        for word in cosim.entry_words(self.layout):
            seen.add(decode(word))
        self.assertEqual(
            seen, EXPECTED_FORMS,
            "fixture instruction inventory changed:\n"
            f"  added:   {sorted(seen - EXPECTED_FORMS)}\n"
            f"  dropped: {sorted(EXPECTED_FORMS - seen)}",
        )


if __name__ == "__main__":
    unittest.main()
