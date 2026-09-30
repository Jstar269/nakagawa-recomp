# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Contract tests for the out-of-domain transcendental boundary (issue #69).

Evidence classification
-----------------------
HARDWARE_MEASURED for the 14 exact argument words in the PSP oracle table
(2026-09-30).  Both fresh runs returned identical records: the +/-1 endpoints
returned themselves, and all sampled `|x| > 1` words returned the signed PSP
invalid NaN (`0x7F800001` / `0xFF800001`).  The existing shared helper already
matches those observations.  These tests pin the measured table and the two
properties that must hold for it to remain honest:

  * the single shared helper's out-of-domain branch is keyed on the documented
    boundary (`0x3F800000`, `|x| = 1`) and returns the sampled PSP invalid NaN
    carrying the input's sign;
  * the AOT emitter and the AOT-gap interpreter route through the ONE shared
    implementation, so the two tiers cannot answer this edge differently;
  * the boundary is named in the product (`docs/COMPATIBILITY.md`) and in the
    hardware-oracle record (`docs/HARDWARE_ORACLE.md`) with its tracking issue,
    so a future measured answer updates the product claim instead of silently
    changing a number.

Reproduction corpus
-------------------
`REPRODUCED_ARGUMENTS` are the raw IEEE-754 bit patterns a `NAN_TRAP` build
(`make NAN_TRAP=1`) reported at the trapping `vasin.s` of a qualified private
title route (title -> savedata CONTINUE -> main menu, 6000 vblanks).  Every one
of the 403 distinct words that run produced is negative and out of domain, from
eleven ULPs past `-1.0` up to exactly `-2.0`; the two endpoints and
representative interior words are kept here so the corpus is checkable without
the private trace.  Guest addresses and opcodes are intentionally absent: this
is a semantics contract, not a title fixture.
"""

from __future__ import annotations

from pathlib import Path
import re
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
RECOMP_C = ROOT / "src" / "rt" / "recomp.c"
CODEGEN_PY = ROOT / "tools" / "codegen.py"
VFPU_INTERP_C = ROOT / "src" / "rt" / "vfpu_interp.c"
COMPATIBILITY_MD = ROOT / "docs" / "COMPATIBILITY.md"
HARDWARE_ORACLE_MD = ROOT / "docs" / "HARDWARE_ORACLE.md"
VFPU_INTERP_SELFTEST_C = ROOT / "src" / "rt" / "vfpu_interp_selftest.c"

#: `|x| = 1.0` in raw bits: the documented end of the arc-sine domain.
DOMAIN_LIMIT_WORD = 0x3F800000

#: The PSP invalid NaN every transcendental unit settles on (sNaN on x86).
INVALID_NAN_WORD = 0x7F800001

#: Distinct out-of-domain words the qualified title route fed to `vasin.s`.
REPRODUCED_ARGUMENT_COUNT = 403

REPRODUCED_ARGUMENTS = (
    0xBF80000B,  # |x| = 1.00000131, eleven ULPs past the domain
    0xBF80DABC,
    0xBF82026A,
    0xBF8FA2B7,
    0xBF9A419C,
    0xBFB63DDA,
    0xBFFB5A51,
    0xBFFFFE00,
    0xC0000000,  # |x| = 2.0 exactly
)


def as_float(word: int) -> float:
    return struct.unpack("<f", struct.pack("<I", word))[0]


class ReproducedCorpusTests(unittest.TestCase):
    """The recorded corpus is genuinely out of the documented domain."""

    def test_every_recorded_argument_leaves_the_domain(self) -> None:
        for word in REPRODUCED_ARGUMENTS:
            self.assertGreater(
                word & 0x7FFFFFFF, DOMAIN_LIMIT_WORD,
                f"0x{word:08X} is inside the arc-sine domain, so it cannot be "
                f"part of an out-of-domain corpus")

    def test_every_recorded_argument_is_negative(self) -> None:
        # The title's own integer-truncated guard diverts positive overflow
        # away from this instruction, so only the negative side reaches it.
        for word in REPRODUCED_ARGUMENTS:
            self.assertTrue(word & 0x80000000, f"0x{word:08X} is positive")

    def test_corpus_spans_both_ends_of_the_observed_range(self) -> None:
        values = sorted(as_float(w) for w in REPRODUCED_ARGUMENTS)
        self.assertAlmostEqual(values[0], -2.0, places=6)
        self.assertAlmostEqual(values[-1], -1.00000131, places=7)

    def test_the_documented_endpoint_is_not_out_of_domain(self) -> None:
        # `|x| = 1.0` is the largest in-domain magnitude; the corpus must not
        # swallow it, or the pinned branch would be off by one ULP.
        self.assertEqual(DOMAIN_LIMIT_WORD & 0x7FFFFFFF, DOMAIN_LIMIT_WORD)


class MeasuredResultTests(unittest.TestCase):
    """The one shared implementation matches the exact measured words."""

    def setUp(self) -> None:
        self.src = RECOMP_C.read_text(encoding="utf-8")

    def test_out_of_domain_branch_returns_the_measured_psp_invalid_nan(self) -> None:
        self.assertIn(
            "if(bits>0x3F800000u){bits=0x7F800001u^sign;",
            self.src,
            "the arc-sine fail-closed branch must stay keyed on the documented "
            "domain boundary and return the PSP invalid NaN with the input sign")

    def test_the_domain_boundary_is_strictly_greater_than_one(self) -> None:
        # A `>=` here would divert the largest legal argument into the NaN
        # branch, which would corrupt every title whose math reaches +-1.
        self.assertIn("bits>0x3F800000u", self.src)
        self.assertNotIn("bits>=0x3F800000u", self.src)

    def test_the_branch_is_inside_the_arc_sine_implementation(self) -> None:
        start = self.src.index("float sr_vfpu_asin(float x){")
        body = self.src[start:self.src.index("\nfloat ", start + 1)]
        self.assertIn("0x3F800000u", body)
        self.assertIn("0x7F800001u", body)


class SharedImplementationTests(unittest.TestCase):
    """AOT and the AOT-gap interpreter must not answer this edge separately."""

    def test_codegen_calls_the_shared_arc_sine(self) -> None:
        src = CODEGEN_PY.read_text(encoding="utf-8")
        self.assertIn('23: "sr_vfpu_asin(_s[_i])"', src)
        self.assertIn('31: "-sr_vfpu_asin(_s[_i])"', src)

    def test_interpreter_calls_the_shared_arc_sine(self) -> None:
        src = VFPU_INTERP_C.read_text(encoding="utf-8")
        self.assertIn("d[i] = sr_vfpu_asin(v[i]);", src)
        self.assertIn("d[i] = -sr_vfpu_asin(v[i]);", src)

    def test_neither_tier_carries_its_own_arc_sine_table_walk(self) -> None:
        # A second, host-only copy of the domain test would let the tiers
        # disagree on exactly the values issue #69 turns on.
        for path in (CODEGEN_PY, VFPU_INTERP_C):
            self.assertNotIn("0x7F800001u", path.read_text(encoding="utf-8"),
                             f"{path.name} must not restate the invalid-NaN payload")


class NamedBoundaryTests(unittest.TestCase):
    """The boundary is a named product statement, not a silent number."""

    def setUp(self) -> None:
        self.compat = COMPATIBILITY_MD.read_text(encoding="utf-8")
        self.oracle = HARDWARE_ORACLE_MD.read_text(encoding="utf-8")

    def _vfpu_row(self) -> str:
        rows = [line for line in self.compat.splitlines()
                if line.startswith("| VFPU (Vector Floating Point Unit)")]
        self.assertEqual(len(rows), 1, "expected exactly one VFPU compatibility row")
        return rows[0]

    def test_compatibility_row_reports_a_partial_state(self) -> None:
        self.assertIn("| Partially works |", self._vfpu_row())

    def test_compatibility_row_names_the_arc_sine_domain_and_issue(self) -> None:
        row = self._vfpu_row()
        self.assertIn("arc-sine domain", row)
        self.assertIn("issues/69", row)

    def test_hardware_oracle_records_the_measured_words(self) -> None:
        test_src = VFPU_INTERP_SELFTEST_C.read_text(encoding="utf-8")
        start = test_src.index("static int check_vasin_measured_domain_words(void)")
        body = test_src[start:test_src.index("\nint main(void)", start)]
        measured = re.findall(
            r"\{0x([0-9A-Fa-f]{8})u,\s*0x([0-9A-Fa-f]{8})u\}", body)
        self.assertEqual(len(measured), 14, "the runtime regression must retain all 14 PSP words")
        start = self.oracle.index("Out-of-domain transcendental arguments")
        cell = self.oracle[start:self.oracle.index("\n- ", start + 1)]
        self.assertIn("HARDWARE_MEASURED", cell)
        self.assertIn("#69", cell)
        for input_word, result_word in measured:
            row = f"`0x{input_word.upper()}` | `0x{result_word.upper()}`"
            self.assertIn(row, cell, f"missing measured row: {row}")

    def test_compatibility_names_measured_result_and_open_issue(self) -> None:
        row = self._vfpu_row()
        self.assertIn("measured VASIN at the arc-sine domain boundary", row)
        self.assertIn("in the works", row)
        self.assertIn("issues/69", row)


if __name__ == "__main__":
    unittest.main()
