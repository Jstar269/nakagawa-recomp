# SPDX-License-Identifier: GPL-2.0-or-later

"""Structural gates for the AOT/interpreter cosimulation fixture.

The executable comparison lives in ``fixtures/cosim/cosim_selftest.c`` and needs a
built toolchain.  These tests are the part that can run anywhere: they check the
properties the comparison silently DEPENDS on, each of which has already been
observed to fail in practice or would make a cell pass vacuously.
"""

import importlib.util
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "fixtures" / "cosim" / "generate.py"
FPU_REFERENCE = ROOT / "fixtures" / "cosim" / "fpu_reference.c"

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
        self.assertEqual(cosim.fpu_corpus_header(), cosim.fpu_corpus_header())

    def test_manifest_header_is_pure_ascii(self):
        cosim.manifest_header().decode("ascii")


class FpuOracleStructureTests(unittest.TestCase):
    def test_reference_does_not_include_or_call_production_helpers(self):
        source = FPU_REFERENCE.read_text(encoding="ascii")
        self.assertNotIn('#include "fp_convert.h"', source)
        self.assertNotIn("#include <math.h>", source)
        self.assertNotRegex(source, r"\b(?:float|double)\b")
        self.assertNotRegex(source, r"\bsr_fpu_[a-z0-9_]+\s*\(")
        self.assertIn("MIPS32", source)

    def test_generated_corpus_covers_required_boundary_classes(self):
        corpus = cosim.fpu_corpus_header().decode("ascii")
        for literal in (
            "0x00000000u", "0x80000000u", "0x7f800000u", "0xff800000u",
            "0x7fc00000u", "0x7f800001u", "0x00000001u", "0x007fffffu",
            "0x00800000u", "0x3f000001u", "0x4effffffu", "0xcf000000u",
            "0x7f7fffffu",
        ):
            self.assertIn(literal, corpus)

    def test_cvt_sw_aot_oracle_loads_the_integer_into_an_fpr(self):
        words = cosim.cell_layout()["fpu_aot"][1]
        mtc1 = cosim._fp(0x04, cosim.T0, 14, 0, 0x00)
        cvt_sw = cosim._fp(0x14, 0, 14, 15, 0x20)
        self.assertIn(mtc1, words)
        self.assertIn(cvt_sw, words)
        self.assertLess(words.index(mtc1), words.index(cvt_sw))

    def test_aot_oracle_cell_is_not_claimed_by_the_two_lane_interpreter(self):
        self.assertIn("fpu_aot", cosim.AOT_ORACLE_ONLY_CELLS)
        self.assertIn("fpu_aot", cosim.NON_ENTRY_CELLS)
        layout = cosim.cell_layout()
        for name, (_offset, words) in layout.items():
            if name in cosim.AOT_ORACLE_ONLY_CELLS:
                continue
            for word in words:
                self.assertNotEqual(
                    cosim.decode_form(word),
                    ("cop1", 0x10, 0x04),
                    f"{name} executes sqrt.s outside the AOT-only oracle",
                )


@unittest.skipUnless(shutil.which("gcc"), "gcc is required for the C.cond truth-table regression")
class FpuCcondTruthTableTests(unittest.TestCase):
    def test_predicates_match_the_mips_table_for_all_relation_classes(self):
        with tempfile.TemporaryDirectory(prefix="cosim_ccond_") as tmp:
            work = Path(tmp)
            source = work / "ccond_truth.c"
            executable = work / "ccond_truth.exe"
            source.write_text(
                r'''#include <stdint.h>
#include <stdio.h>

#include "fp_convert.h"
#include "fpu_reference.h"

struct RelationCase {
    uint32_t a;
    uint32_t b;
    const char *name;
    unsigned expected[16];
};

static const struct RelationCase cases[] = {
    {
        0x3f800000u, 0x40000000u, "less",
        {0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u,
         0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u}
    },
    {
        0x3f800000u, 0x3f800000u, "equal",
        {0u, 0u, 1u, 1u, 0u, 0u, 1u, 1u,
         0u, 0u, 1u, 1u, 0u, 0u, 1u, 1u}
    },
    {
        0x40000000u, 0x3f800000u, "greater",
        {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
         0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}
    },
    {
        0x7fc00000u, 0x3f800000u, "unordered",
        {0u, 1u, 0u, 1u, 0u, 1u, 0u, 1u,
         0u, 1u, 0u, 1u, 0u, 1u, 0u, 1u}
    },
};

int main(void) {
    unsigned failures = 0u;
    for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        for (unsigned condition = 0u; condition < 16u; ++condition) {
            const unsigned expected = cases[i].expected[condition];
            const unsigned production = sr_fpu_condition_s(
                condition, cases[i].a, cases[i].b);
            enum FpuReferenceStatus status;
            const unsigned reference = fpu_reference_compare(
                condition, cases[i].a, cases[i].b, &status);
            if (production != expected || reference != expected) {
                fprintf(stderr,
                        "%s condition=%u expected=%u production=%u reference=%u\n",
                        cases[i].name, condition, expected, production, reference);
                ++failures;
            }
        }
    }
    printf("ccond_truth: %s\n", failures == 0u ? "PASS" : "FAIL");
    return failures == 0u ? 0 : 1;
}
''',
                encoding="ascii",
            )
            command = [
                shutil.which("gcc"),
                "-std=c11",
                "-O1",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "src" / "rt"),
                "-I",
                str(ROOT / "fixtures" / "cosim"),
                str(source),
                str(FPU_REFERENCE),
                "-lm",
                "-o",
                str(executable),
            ]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr + compiled.stdout)
            ran = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr + ran.stdout)
            self.assertIn("ccond_truth: PASS", ran.stdout)


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
        """Leaving through a transfer IS the cosim synchronization point.

        Most cells end in `jr $ra`. Three do not, for reasons that are part of
        what they test, so each is named here rather than blanket-exempted:

          jrtail    ends in `jr $rs` and relinquishes control through its
                    callee's `jr $ra`;
          xtail     the same, into the omitted middle of a cross-tier chain;
          xtailmid  ends in a `j` tail, which is the whole point -- it must NOT
                    return, so that the cross-tier tail control isolates guest
                    memory commitment from frame re-entry.

        Anything else without a defined exit has no synchronization point in
        either lane.
        """
        register_tail = {"jrtail", "xtail"}
        jump_tail = {"xtailmid"}
        for name, (_offset, words) in self.layout.items():
            if name in cosim.NON_PROGRAM_CELLS or name in cosim.AOT_ORACLE_ONLY_CELLS:
                continue
            self.assertGreaterEqual(len(words), 2, f"cell {name} is too short to have an exit")
            terminator = words[-2]
            if name in jump_tail:
                self.assertEqual(terminator >> 26, 0x02,
                                 f"cell {name} must end in a `j` tail")
                continue
            self.assertEqual(terminator >> 26, 0x00, f"cell {name} does not end in a transfer")
            self.assertEqual(terminator & 0x3F, 0x08, f"cell {name} does not end in jr")
            if name not in register_tail:
                self.assertEqual(terminator, JR_RA, f"cell {name} does not end in jr $ra")

    def test_cross_tier_cells_omit_a_body_that_is_actually_reachable(self):
        """A cross-tier cell only crosses anything while its omission is real.

        If the entry cell stopped reaching the omitted body -- or the omission
        named a cell that was never registered -- lane MIXED would quietly become
        a second all-native run and every cross-tier assertion would hold
        vacuously. The harness checks the registration side at run time; this
        checks the reachability side without a toolchain.
        """
        self.assertTrue(cosim.CROSS_TIER_OMISSIONS, "no cross-tier cell is declared")
        for entry, omitted in cosim.CROSS_TIER_OMISSIONS.items():
            with self.subTest(entry=entry):
                self.assertIn(entry, self.layout)
                self.assertIn(omitted, self.layout)
                self.assertIn(omitted, cosim.NON_ENTRY_CELLS,
                              "an omitted body must not also be an entry cell")
                self.assertNotIn(omitted, cosim.UNDISCOVERED_CELLS,
                                 "the omitted body must still be discovered, or lane "
                                 "AOT would not have a native body to drop")

    def test_the_negative_corpus_scratch_is_never_executed_by_a_cell(self):
        """negpad is patched at run time; no comparison may depend on its bytes."""
        self.assertIn("negpad", cosim.UNDISCOVERED_CELLS)
        self.assertIn("negpad", cosim.NON_ENTRY_CELLS)
        self.assertIn("negpad", cosim.NON_PROGRAM_CELLS)
        head = cosim.entry_words(self.layout)
        targets = {((word & 0x03FFFFFF) << 2) for word in head if (word >> 26) == 0x03}
        self.assertNotIn(self.layout["negpad"][0], targets)

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

        This is the FIXTURE half of the claim "the interpreter implements exactly
        what a cell executes". The INTERPRETER half is not a list at all: the
        harness probes the production decoder directly (`run_form_census()` in
        fixtures/cosim/cosim_selftest.c) and requires the decoded set to equal the
        set this inventory describes. Neither side is trusted to describe the
        other.
        """
        seen = set()
        for name, (_offset, words) in self.layout.items():
            if name in cosim.NON_PROGRAM_CELLS or name in cosim.AOT_ORACLE_ONLY_CELLS:
                continue
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

    def test_generated_form_list_matches_the_declared_inventory(self):
        """The C census consumes generate.py's list; both must say the same thing.

        The harness compares the interpreter against COSIM_FORM_LIST, which
        generate.py derives from the cells. If that derivation ever drifted from
        the inventory above, the census would still pass -- against the wrong
        question.
        """
        seen = set()
        for name, (_offset, words) in self.layout.items():
            if name in cosim.NON_PROGRAM_CELLS or name in cosim.AOT_ORACLE_ONLY_CELLS:
                continue
            for word in words:
                seen.add(decode(word))
        for word in cosim.entry_words(self.layout):
            seen.add(decode(word))
        self.assertEqual(cosim.fixture_forms(), seen)
        self.assertEqual(cosim.fixture_forms(), EXPECTED_FORMS)

    def test_generated_manifest_header_declares_every_form(self):
        header = cosim.manifest_header().decode("ascii")
        self.assertIn("#define COSIM_FORM_LIST(X)", header)
        self.assertIn(f"#define COSIM_FORM_COUNT    {len(EXPECTED_FORMS)}u", header)
        for name in ("COSIM_NEGPAD", "COSIM_UNOWNED", "COSIM_XSTORE", "COSIM_XTAILMID"):
            self.assertIn(f"#define {name}", header)


if __name__ == "__main__":
    unittest.main()
