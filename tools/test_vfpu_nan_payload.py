# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Contract tests for the VFPU NaN/Inf matrix/multiply probe fixture
(issue #40).

These guard the properties that keep the probe meaningful.  They deliberately
do not assert any numeric result: the correct PSP output words are exactly
what this probe exists to discover, and baking an expectation in would promote
host-specific NaN behavior to a fabricated hardware contract.
"""

from __future__ import annotations

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ROOT / "fixtures" / "vfpu_nan_payload" / "vfpu_nan_cases.h"
PROBE = ROOT / "fixtures" / "vfpu_nan_payload" / "vfpu_nan_probe.c"
MAKEFILE = ROOT / "fixtures" / "vfpu_nan_payload" / "Makefile"


class SharedInputVectorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.cases = CASES.read_text(encoding="utf-8")

    def test_declared_count_matches_the_literal_list(self) -> None:
        declared = int(re.search(r"#define VFPU_NAN_CASE_COUNT (\d+)", self.cases).group(1))
        body = self.cases.split("VFPU_NAN_CASES[VFPU_NAN_CASE_COUNT] = {", 1)[1].split("};", 1)[0]
        actual = len(re.findall(r"0x[0-9A-Fa-f]{8}u", body))
        self.assertEqual(declared * 18, actual,
                         "a count/list mismatch would silently change the question asked")

    def test_inputs_are_raw_bit_patterns(self) -> None:
        body = self.cases.split("VFPU_NAN_CASES[VFPU_NAN_CASE_COUNT] = {", 1)[1].split("};", 1)[0]
        stripped = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        self.assertNotRegex(stripped, r"\d+\.\d+[fF]?",
                            "decimal literals are re-rounded per compiler; use raw IEEE-754 bits")

    def test_case_ids_are_stable_and_unique(self) -> None:
        ids = re.findall(r'"([a-z0-9-]+)", \d+,', self.cases)
        self.assertEqual(len(ids), 8, "expected the 8 reduced issue-#40 cases")
        self.assertEqual(len(ids), len(set(ids)), "case ids must be unique")

    def test_reduced_vector_words_present(self) -> None:
        # The exact words from the reduced vector set and its near neighbours.
        for word in ("0x7F800001u", "0x7F800000u", "0xFF800000u",
                     "0x00000001u", "0xFF800001u", "0x3F800000u"):
            self.assertIn(word, self.cases, "reduced input word missing from the shared table")


class ProbeContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.probe = PROBE.read_text(encoding="utf-8")

    def test_probe_includes_the_shared_vector(self) -> None:
        self.assertIn("vfpu_nan_cases.h", self.probe)

    def test_vfpu_thread_attribute_is_declared(self) -> None:
        self.assertIn("THREAD_ATTR_VFPU", self.probe)

    def test_probe_distinguishes_emulator_from_hardware(self) -> None:
        self.assertIn('emulated ? "ppsspp" : "psp"', self.probe)
        self.assertIn("EMULATOR_DEVCTL_IS_EMULATOR", self.probe)

    def test_bits_move_without_an_fpu_round_trip(self) -> None:
        self.assertIn("mtv", self.probe)
        self.assertIn("mfv", self.probe)

    def test_both_ops_are_covered(self) -> None:
        self.assertIn("vmmul.t M200, M000, M100", self.probe)
        self.assertIn("vtfm3.t C200, M100, C000", self.probe)


class ProbeBuildContractTests(unittest.TestCase):
    def test_makefile_requires_pspdev(self) -> None:
        text = MAKEFILE.read_text(encoding="utf-8")
        self.assertIn("psp-config --pspsdk-path", text)
        self.assertIn("PSP_FW_VERSION", text)

    def test_emulator_route_is_labelled_corroboration(self) -> None:
        text = MAKEFILE.read_text(encoding="utf-8")
        self.assertIn("corroboration", text)


if __name__ == "__main__":
    unittest.main()
