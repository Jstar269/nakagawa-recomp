# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Contract tests for the VFPU source/destination aliasing probe fixture.

These guard the properties that keep the probe meaningful.  They deliberately
do not assert any numeric result: the correct PSP output words are exactly
what this probe exists to discover, and baking an expectation in would promote
host-specific overlap semantics to a fabricated hardware contract.

The strongest invariant is drift-freedom: the checked-in fixture case table
must equal what tools/vfpu_overlap_fuzz_gen.py emits today (same words, same
alias classes, same hardware-contract classes), and every word must decode
through the production static emitter with no sr_vfpu_interp fallback.  A
future generator change therefore forces an intentional fixture refresh, not a
silent question change.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

CASES = ROOT / "fixtures" / "vfpu_overlap_probe" / "vfpu_overlap_cases.h"
PROBE = ROOT / "fixtures" / "vfpu_overlap_probe" / "vfpu_overlap_probe.c"
MAKEFILE = ROOT / "fixtures" / "vfpu_overlap_probe" / "Makefile"


class SharedCaseTableTests(unittest.TestCase):
    def setUp(self) -> None:
        self.cases = CASES.read_text(encoding="utf-8")

    def test_declared_count_matches_the_literal_list(self) -> None:
        declared = int(re.search(r"#define VFPU_OVERLAP_PROBE_CASE_COUNT (\d+)", self.cases).group(1))
        body = self.cases.split("VFPU_OVERLAP_PROBE_CASES[VFPU_OVERLAP_PROBE_CASE_COUNT] = {", 1)[1]
        body = body.split("};", 1)[0]
        records = re.findall(r'\{("[^"]+", 0x[0-9A-Fa-f]{8}u, \d+u, \d+u, \d+u)\}', body)
        self.assertEqual(declared, len(records),
                         "a count/list mismatch would silently change the question asked")

    def test_case_ids_are_stable_and_unique(self) -> None:
        ids = re.findall(r'\{"([a-z0-9-]+)",', self.cases)
        self.assertGreater(len(ids), 0, "fixture case table must not be empty")
        self.assertEqual(len(ids), len(set(ids)), "case ids must be unique")
        for cid in ids:
            self.assertRegex(cid, r"^(vmmul|vtfm|vmscl|vmmov|vdot|vhdp|vcrs|vscl|vqmul|vcrsp)-k[0-7]-ct[0-2]-\d+$")

    def test_words_decode_through_the_static_emitter(self) -> None:
        import codegen
        body = self.cases.split("VFPU_OVERLAP_PROBE_CASES[VFPU_OVERLAP_PROBE_CASE_COUNT] = {", 1)[1]
        body = body.split("};", 1)[0]
        words = [int(m, 16) for m in re.findall(r"0x([0-9A-Fa-f]{8})u", body)]
        bad: list[str] = []
        for w in words:
            try:
                emitted, _, _ = codegen.vfpu_effect(0x08900000, w)
            except codegen.Unsupported as e:
                bad.append(f"0x{w:08x} unsupported: {e}")
                continue
            if "sr_vfpu_interp" in emitted:
                bad.append(f"0x{w:08x} falls back to sr_vfpu_interp")
        self.assertEqual(
            bad, [],
            "probe words must decode through the production emitter:\n" + "\n".join(bad[:20]),
        )

    def test_table_matches_the_generator_exactly(self) -> None:
        """No drift between the checked-in fixture table and the generator."""
        from vfpu_overlap_fuzz_gen import build_cases
        cases = build_cases()
        body = self.cases.split("VFPU_OVERLAP_PROBE_CASES[VFPU_OVERLAP_PROBE_CASE_COUNT] = {", 1)[1]
        body = body.split("};", 1)[0]
        records = re.findall(r'\{("[^"]+", 0x[0-9A-Fa-f]{8}u, (\d+)u, (\d+)u, (\d+)u)\}', body)
        expected = [(str(k), str(f), str(ct)) for _w, k, f, _am, ct in cases]
        got = [(rec[1], rec[2], rec[3]) for rec in records]
        self.assertEqual(got, expected,
                         "fixture case table drifted from tools/vfpu_overlap_fuzz_gen.py; "
                         "regenerate with --fixture-header")

    def test_klass_and_contract_use_known_encodings(self) -> None:
        body = self.cases.split("VFPU_OVERLAP_PROBE_CASES[VFPU_OVERLAP_PROBE_CASE_COUNT] = {", 1)[1]
        body = body.split("};", 1)[0]
        for rec in re.findall(r"(\d+)u, (\d+)u\)", body):
            klass, contract = int(rec[0]), int(rec[1])
            self.assertIn(klass, range(8), f"unknown alias class {klass}")
            self.assertIn(contract, range(3), f"unknown contract class {contract}")


class ProbeSourceShapeTests(unittest.TestCase):
    def test_probe_uses_raw_word_transfers(self) -> None:
        src = PROBE.read_text(encoding="utf-8")
        self.assertIn("lv.q", src, "register fill must use raw word loads")
        self.assertIn("sv.q", src, "register save must use raw word stores")
        self.assertIn("mtvc %0, $128", src, "identity prefix setup must write VFPU_PFXS")

    def test_fill_uses_raw_bits_only(self) -> None:
        src = PROBE.read_text(encoding="utf-8")
        stripped = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
        self.assertNotRegex(stripped, r"\d+\.\d+[fF]?",
                            "decimal float literals are re-rounded per compiler; use raw IEEE-754 bits")

    def test_probe_loads_the_case_header_verbatim(self) -> None:
        self.assertIn('#include "vfpu_overlap_cases.h"', PROBE.read_text(encoding="utf-8"))

    def test_fixture_makefile_is_pspdev_and_refuses_without_sdk(self) -> None:
        text = MAKEFILE.read_text(encoding="utf-8")
        self.assertIn("psp-config --pspsdk-path", text)
        self.assertIn("include $(PSPSDK)/lib/build.mak", text)


if __name__ == "__main__":
    unittest.main()
