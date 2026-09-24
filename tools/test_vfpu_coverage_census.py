# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

import copy
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import vfpu_coverage_report as census


class VfpuCoverageCensusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.census = census.build_census()
        cls.rows = {row["form_id"]: row for row in cls.census["rows"]}

    def test_schema_and_systematic_decoder_coverage(self):
        self.assertEqual(self.census["schema"], census.CENSUS_SCHEMA)
        self.assertGreater(self.census["systematic_space"]["words_visited"], 50000)
        source = (ROOT / "src" / "rt" / "vfpu_interp.c").read_text(encoding="utf-8")
        self.assertEqual(
            self.census["systematic_space"]["production_decoder_return_sites"],
            len(census.RETURN_PATTERN.findall(source)),
        )
        self.assertIn("0x18", self.census["systematic_space"]["opcode_families"])
        self.assertIn("0x3c", self.census["systematic_space"]["opcode_families"])

    def test_json_and_markdown_are_deterministic(self):
        rebuilt = census.build_census()
        self.assertEqual(
            census._canonical_json(self.census),
            census._canonical_json(rebuilt),
        )
        self.assertEqual(census.render_markdown(self.census), census.render_markdown(rebuilt))

    def test_rows_have_explicit_dispositions_and_evidence(self):
        required = {
            "aot_disposition",
            "compatibility_disposition",
            "vector_size",
            "differential_tests",
            "hardware_measured",
            "evidence_tier",
            "sufficiently_verified_for_acceleration",
        }
        for row in self.census["rows"]:
            self.assertTrue(required <= set(row), row["form_id"])
            self.assertIn(row["evidence_tier"], {"U", "S", "D", "HP", "H"})
            self.assertIn(row["compatibility_disposition"], {
                "aot-direct",
                "aot-routes-to-sr-vfpu-interp",
                "interpreter-only",
                "supported-opcode-form-unmodeled",
                "unsupported-encoding",
            })
            if row["compatibility_disposition"] in {
                "interpreter-only",
                "supported-opcode-form-unmodeled",
                "unsupported-encoding",
            }:
                self.assertEqual(row["boundary_issue"], 326)

    def test_vector_widths_remain_distinct_forms(self):
        vfpu0_rows = [
            row for row in self.census["rows"]
            if row["opcode"] == "0x18" and row["compatibility_disposition"] == "aot-direct"
        ]
        self.assertEqual({row["vector_size"] for row in vfpu0_rows}, {1, 2, 3, 4})
        for row in vfpu0_rows:
            self.assertEqual(len(row["size_classes"]), 1)

    def test_agreement_gate_passes_and_vflush_is_explicit(self):
        self.assertEqual(self.census["agreement_gate"]["status"], "pass")
        controls = [
            row for row in self.census["rows"]
            if row["agreement"] == "explicit-aot-direct-control"
        ]
        self.assertTrue(controls)
        self.assertEqual({row["opcode"] for row in controls}, {"0x3f"})

    def test_reserved_vcmov_forms_are_unmodeled_and_fail_closed(self):
        words = (
            (0x1B << 26) | (6 << 23) | (7 << 16),
            (0x1B << 26) | (7 << 23) | (7 << 16),
            (0x34 << 26) | (21 << 21) | (7 << 16),
        )
        records = census.classify_words(words)
        for record in records:
            self.assertFalse(record["interpreter_supported"])
            self.assertEqual(record["aot_disposition"], "aot-unsupported")
            self.assertEqual(
                census.compatibility_disposition(record),
                "supported-opcode-form-unmodeled",
            )
            self.assertEqual(census.agreement_status(record), "agree-unsupported")

    def test_agreement_gate_catches_mutated_support_disagreement(self):
        word = (0x1B << 26) | (6 << 23) | (7 << 16)
        record = census.classify_words((word,))[0]
        mutated = copy.deepcopy(record)
        mutated["interpreter_supported"] = True
        mutated["interpreter_kind"] = "compute"
        with self.assertRaises(census.CensusAgreementError):
            census.validate_agreement((mutated,))

    def test_agreement_gate_catches_mutated_fail_open_fallback(self):
        word = (0x35 << 26) | (4 << 21)
        record = census.classify_words((word,))[0]
        mutated = copy.deepcopy(record)
        mutated["aot_emitted"] = mutated["aot_emitted"].replace("SR_VFPU_OTHER", "0")
        with self.assertRaises(census.CensusAgreementError):
            census.validate_agreement((mutated,))

    def test_agreement_gate_catches_unsupported_form_with_no_fallback_call(self):
        word = (0x34 << 26) | (31 << 21)
        record = census.classify_words((word,))[0]
        mutated = copy.deepcopy(record)
        mutated["aot_emitted"] = ""
        with self.assertRaises(census.CensusAgreementError):
            census.validate_agreement((mutated,))

    def test_acceleration_gate_is_empty_and_evidence_derived(self):
        gate = self.census["acceleration_gate"]
        self.assertEqual(gate["accelerated_forms"], [])
        self.assertEqual(gate["violations"], [])
        by_id = self.rows
        for form_id in gate["accelerated_forms"]:
            self.assertTrue(by_id[form_id]["sufficiently_verified_for_acceleration"])
        row = copy.deepcopy(next(row for row in self.census["rows"] if row["differential_tests"]))
        row["hardware_sensitive"] = True
        row["evidence_tier"] = "D"
        self.assertFalse(census.acceleration_verified(row))
        row["evidence_tier"] = "H"
        self.assertTrue(census.acceleration_verified(row))
        row["compatibility_disposition"] = "unsupported-encoding"
        self.assertFalse(census.acceleration_verified(row))

    def test_acceleration_gate_rejects_unknown_form_ids(self):
        original = census.ACCELERATED_FORMS
        census.ACCELERATED_FORMS = ("vfpu-unknown",)
        try:
            with self.assertRaises(census.CensusAgreementError):
                census.build_census()
        finally:
            census.ACCELERATED_FORMS = original

    def test_vfpu_fuzz_uncovered_cases_are_fatal(self):
        source = (ROOT / "src" / "rt" / "vfpu_fuzz.c").read_text(encoding="utf-8")
        self.assertIn("if (trials <= 0)", source)
        self.assertIn("if (FUZZ_NCASES <= 0)", source)
        self.assertIn(
            "return (bad_cases || skipped || tested != FUZZ_NCASES) ? 1 : 0;",
            source,
        )

    def test_hardware_ids_are_row_level_not_inherited(self):
        memory_rows = [
            row for row in self.census["rows"]
            if row["opcode"] in {"0x32", "0x35", "0x36", "0x3a", "0x3e"}
        ]
        self.assertTrue(memory_rows)
        self.assertTrue(any(row["hardware_measured"] for row in memory_rows))
        self.assertTrue(any(not row["hardware_measured"] for row in memory_rows))
        ids = {
            item["oracle_id"]
            for row in memory_rows
            for item in row["hardware_measured"]
        }
        self.assertIn("PSP-A3-08", ids)
        self.assertIn("PSP-A3-09", ids)
        self.assertIn("PSP-A3-15", ids)
        self.assertNotIn("PSP-A3-14", ids)
        self.assertIn("PSP-A3-14", self.census["hardware_oracle_boundary"]["context_only_not_inherited"])

    def test_encounter_input_accepts_only_words_and_counts(self):
        with tempfile.TemporaryDirectory(prefix="vfpu_encounter_") as tmp:
            path = Path(tmp) / "words.json"
            path.write_text(
                json.dumps([
                    {"word": "0x60000000", "count": 4},
                    {"word": "0xd3e00000", "count": 2},
                ]),
                encoding="ascii",
            )
            report = census.build_encounter_report(path)
            self.assertEqual(report["total_words"], 2)
            self.assertEqual(report["total_count"], 6)
            dispositions = {entry["word"]: entry["disposition"] for entry in report["entries"]}
            self.assertEqual(dispositions["0x60000000"], "aot-direct")
            self.assertEqual(dispositions["0xd3e00000"], "unsupported-encoding")
            output = io.StringIO()
            with redirect_stdout(output):
                rc = census.main(["--encodings", str(path)])
            self.assertEqual(rc, 0)
            self.assertEqual(json.loads(output.getvalue())["total_count"], 6)

    def test_encounter_input_rejects_context_fields(self):
        invalid = (
            [{"word": "0x60000000", "count": 1, "pc": "0x1000"}],
            [{"word": "0x60000000", "count": 1, "address": 4096}],
            [{"word": "0x60000000", "count": 1, "mnemonic": "vadd.s"}],
            [{"word": "0x60000000", "count": 1, "title": "retail"}],
            [{"word": "0x00000000", "count": 1}],
            [{"word": "0x60000000", "count": 0}],
        )
        with tempfile.TemporaryDirectory(prefix="vfpu_encounter_reject_") as tmp:
            path = Path(tmp) / "words.json"
            for entries in invalid:
                with self.subTest(entries=entries):
                    path.write_text(json.dumps(entries), encoding="ascii")
                    with self.assertRaises(census.CensusError):
                        census.build_encounter_report(path)

    def test_cli_writes_requested_json_and_markdown(self):
        with tempfile.TemporaryDirectory(prefix="vfpu_census_cli_") as tmp:
            json_path = Path(tmp) / "census.json"
            markdown_path = Path(tmp) / "census.md"
            rc = census.main(["--json", str(json_path), "--markdown", str(markdown_path)])
            self.assertEqual(rc, 0)
            loaded = json.loads(json_path.read_text(encoding="ascii"))
            self.assertEqual(loaded, self.census)
            markdown = markdown_path.read_text(encoding="utf-8")
            self.assertIn("# VFPU Compatibility Census", markdown)
            self.assertIn("issue #326", markdown)
            self.assertIn("No SIMD path exists", markdown)


if __name__ == "__main__":
    unittest.main()
