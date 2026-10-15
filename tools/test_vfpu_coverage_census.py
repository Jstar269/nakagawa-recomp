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
from test_codegen_fp_convert import _synthetic_elf

# A VFPU0 word the AOT translates directly and the interpreter accepts.
ROUTE_CLEAN_WORD = 0x60000000
# Reserved VFPU4 jump form: codegen raises Unsupported and sr_vfpu_interp
# returns SR_VFPU_OTHER, so a route containing it must fail the route census.
ROUTE_OTHER_WORD = 0xD3E00000
# jr ra; nop -- ends the synthetic route body; neither word is VFPU.
ROUTE_TAIL = (0x03E00008, 0x00000000)


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

    def test_vfpu3_compare_forms_are_supported_in_both_lanes(self):
        # Major 0x1B sub-ops 5, 6 and 7 are vscmp, vsge and vslt (issue #69).
        # Both lanes decode them now; neither lane may route them to vcmov.
        words = (
            (0x1B << 26) | (5 << 23) | (7 << 16),
            (0x1B << 26) | (6 << 23) | (7 << 16),
            (0x1B << 26) | (7 << 23) | (7 << 16),
        )
        records = census.classify_words(words)
        self.assertEqual(len(records), 3)
        for record in records:
            self.assertTrue(record["interpreter_supported"])
            self.assertEqual(record["interpreter_kind"], "compute")
            self.assertEqual(record["aot_disposition"], "aot-direct")
            self.assertEqual(census.compatibility_disposition(record), "aot-direct")
            self.assertEqual(census.agreement_status(record), "agree-supported-direct")

    def test_reserved_vcmov_form_is_unmodeled_and_fail_closed(self):
        # vcmov lives in VFPU4 (jump 21); imm3 7 is the reserved selector.
        words = ((0x34 << 26) | (21 << 21) | (7 << 16),)
        records = census.classify_words(words)
        self.assertEqual(len(records), 1)
        for record in records:
            self.assertFalse(record["interpreter_supported"])
            self.assertEqual(record["aot_disposition"], "aot-unsupported")
            self.assertEqual(
                census.compatibility_disposition(record),
                "supported-opcode-form-unmodeled",
            )
            self.assertEqual(census.agreement_status(record), "agree-unsupported")

    def test_agreement_gate_catches_mutated_support_disagreement(self):
        word = (0x34 << 26) | (21 << 21) | (7 << 16)
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


class VfpuRouteCensusTests(unittest.TestCase):
    """Join the census to the VFPU words a route's AOT actually emits."""

    def _run_route(self, words):
        with tempfile.TemporaryDirectory(prefix="vfpu_route_census_") as tmp:
            elf = Path(tmp) / "route.elf"
            elf.write_bytes(_synthetic_elf(list(words)))
            output = io.StringIO()
            with redirect_stdout(output):
                rc = census.main(["--route", str(elf)])
            return rc, json.loads(output.getvalue())

    def test_route_census_passes_clean_synthetic_route(self):
        rc, report = self._run_route([ROUTE_CLEAN_WORD, *ROUTE_TAIL])
        self.assertEqual(rc, 0)
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["failures"], [])
        self.assertEqual(report["route_emitted_words"], 3)
        self.assertEqual(report["route_words"], 1)
        entries = {entry["word"]: entry for entry in report["entries"]}
        clean = entries[f"0x{ROUTE_CLEAN_WORD:08x}"]
        self.assertEqual(clean["disposition"], "aot-direct")
        self.assertEqual(clean["status"], "pass")
        self.assertIsNone(clean["tracking_issue"])
        self.assertNotIn(f"0x{ROUTE_TAIL[0]:08x}", entries)

    def test_route_census_fails_closed_on_other_word(self):
        rc, report = self._run_route(
            [ROUTE_CLEAN_WORD, ROUTE_OTHER_WORD, *ROUTE_TAIL]
        )
        self.assertEqual(rc, 1)
        self.assertEqual(report["status"], "fail")
        self.assertEqual(report["failures"], [f"0x{ROUTE_OTHER_WORD:08x}"])
        entries = {entry["word"]: entry for entry in report["entries"]}
        clean = entries[f"0x{ROUTE_CLEAN_WORD:08x}"]
        self.assertEqual(clean["status"], "pass")
        other = entries[f"0x{ROUTE_OTHER_WORD:08x}"]
        self.assertEqual(other["disposition"], "unsupported-encoding")
        self.assertEqual(other["aot"], "aot-unsupported")
        self.assertEqual(other["interpreter"], "unsupported")
        self.assertEqual(other["status"], "boundary")
        self.assertEqual(other["tracking_issue"], 326)

    def test_route_census_scans_generated_route_files(self):
        with tempfile.TemporaryDirectory(prefix="vfpu_route_gen_") as tmp:
            main_c = Path(tmp) / "route.c"
            chunk_c = Path(tmp) / "route_0.c"
            main_c.write_text(
                f"    sr_begin(s, 0x00001000u, 0x{ROUTE_CLEAN_WORD:08x}u); sr_end(s, 0u, 0);\n",
                encoding="ascii",
            )
            chunk_c.write_text(
                f"    sr_begin(s, 0x00001004u, 0x{ROUTE_OTHER_WORD:08x}u); sr_end(s, 0u, 0);\n",
                encoding="ascii",
            )
            output = io.StringIO()
            with redirect_stdout(output):
                rc = census.main(["--route", str(main_c)])
            report = json.loads(output.getvalue())
        self.assertEqual(rc, 1)
        self.assertEqual(report["status"], "fail")
        self.assertEqual(report["route_emitted_words"], 2)
        self.assertEqual(report["failures"], [f"0x{ROUTE_OTHER_WORD:08x}"])
        self.assertEqual(
            {entry["word"] for entry in report["entries"]},
            {f"0x{ROUTE_CLEAN_WORD:08x}", f"0x{ROUTE_OTHER_WORD:08x}"},
        )

    def test_route_arg_must_be_a_codegen_option(self):
        with tempfile.TemporaryDirectory(prefix="vfpu_route_arg_") as tmp:
            elf = Path(tmp) / "route.elf"
            elf.write_bytes(_synthetic_elf([ROUTE_CLEAN_WORD, *ROUTE_TAIL]))
            for bad in ("88000000", "-base=0x1000", "--", ""):
                with self.subTest(bad=bad):
                    output = io.StringIO()
                    with redirect_stdout(output):
                        rc = census.main(["--route", str(elf), f"--route-arg={bad}"])
                    self.assertEqual(rc, 2)
                    self.assertEqual(output.getvalue(), "")

    def test_route_with_a_vfpu_stub_fails_closed_and_other_stubs_are_counted(self):
        with tempfile.TemporaryDirectory(prefix="vfpu_route_stub_") as tmp:
            main_c = Path(tmp) / "route.c"
            main_c.write_text(
                f"    sr_begin(s, 0x00001000u, 0x{ROUTE_CLEAN_WORD:08x}u); sr_end(s, 0u, 0);" + chr(10),
                encoding="ascii",
            )
            stubs = Path(tmp) / "route_stubs.txt"

            def run():
                output = io.StringIO()
                with redirect_stdout(output):
                    rc = census.main(["--route", str(main_c)])
                return rc, output.getvalue()

            # A function stubbed on a VFPU-routed opcode hides its VFPU words.
            stubs.write_text("0x00001040 opcode 0x36 at 0x00001040" + chr(10), encoding="ascii")
            rc, text = run()
            self.assertEqual((rc, text), (2, ""))
            # An unrelated stub is reported, not hidden: the pass covers translated functions only.
            stubs.write_text("0x00001040 control in delay slot at 0x00001040" + chr(10), encoding="ascii")
            rc, text = run()
            self.assertEqual(rc, 0)
            self.assertEqual(json.loads(text)["route_stubbed_functions"], 1)
            stubs.write_text("", encoding="ascii")
            rc, text = run()
            self.assertEqual(rc, 0)
            self.assertEqual(json.loads(text)["route_stubbed_functions"], 0)

    def test_route_report_names_emitted_occurrences_and_classification_flags(self):
        rc, report = self._run_route([ROUTE_CLEAN_WORD, *ROUTE_TAIL])
        self.assertEqual(rc, 0)
        self.assertIn("route_emitted_occurrences", report)
        self.assertNotIn("route_occurrences", report)
        self.assertFalse(report["classification_lle_cpu"])
        for entry in report["entries"]:
            self.assertIn("emitted_occurrences", entry)
            self.assertNotIn("count", entry)

    def test_lle_cpu_route_args_reach_the_classification(self):
        """lv.q/sv.q route to the interpreter in the generated C under --lle-cpu."""
        from unittest import mock

        seen = []
        original = census._classify_aot

        def spy(word, *, lle_cpu=False):
            seen.append(lle_cpu)
            return original(word, lle_cpu=lle_cpu)

        with tempfile.TemporaryDirectory(prefix="vfpu_route_lle_") as tmp:
            main_c = Path(tmp) / "route.c"
            main_c.write_text(
                f"    sr_begin(s, 0x00001000u, 0x{ROUTE_CLEAN_WORD:08x}u); sr_end(s, 0u, 0);\n",
                encoding="ascii",
            )
            with mock.patch.object(census, "_classify_aot", spy):
                report = census.build_route_report(main_c)
        self.assertEqual(seen, [False])
        self.assertFalse(report["classification_lle_cpu"])
        words = mock.MagicMock(return_value=(__import__("collections").Counter({ROUTE_CLEAN_WORD: 1}), 1, 0))
        with mock.patch.object(census, "_route_words", words), \
                mock.patch.object(census, "_classify_aot", spy):
            report = census.build_route_report(Path("route.elf"), ["--lle-cpu"])
        self.assertEqual(seen[-1], True)
        self.assertTrue(report["classification_lle_cpu"])

    def test_route_census_rejects_non_route_input(self):
        with tempfile.TemporaryDirectory(prefix="vfpu_route_bad_") as tmp:
            binary = Path(tmp) / "route.bin"
            binary.write_bytes(b"\x00\x01\x02\x03")
            output = io.StringIO()
            with redirect_stdout(output):
                rc = census.main(["--route", str(binary)])
            self.assertEqual(rc, 2)
            self.assertEqual(output.getvalue(), "")
            # A parseable C file with no emitted guest words must not report a pass.
            empty_c = Path(tmp) / "route.c"
            empty_c.write_text("/* no emitted instructions */\n", encoding="ascii")
            output = io.StringIO()
            with redirect_stdout(output):
                rc = census.main(["--route", str(empty_c)])
            self.assertEqual(rc, 2)
            self.assertEqual(output.getvalue(), "")

    def test_profile_zero_route_has_no_boundary_vfpu_words(self):
        """The committed public profile-zero route emits no Unsupported/OTHER word."""
        elf = ROOT / "fixtures" / "profile_zero" / "prebuilt" / "profile_zero_guest.prx"
        self.assertTrue(elf.is_file(), f"committed profile-zero fixture is missing: {elf}")
        manifest = json.loads(
            (ROOT / "assets" / "titles" / "synthetic.json").read_text(encoding="utf-8")
        )
        base = manifest["executable"]["base"]
        output = io.StringIO()
        with redirect_stdout(output):
            rc = census.main(["--route", str(elf), f"--route-arg=--base={base:x}"])
        report = json.loads(output.getvalue())
        self.assertEqual(rc, 0, report)
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["failures"], [])
        self.assertGreater(report["route_emitted_words"], 0)
        # The committed fixture executes VFPU memory operations; the route census
        # must see them (and only boundary-free ones), not an empty scan.
        self.assertGreaterEqual(report["route_words"], 1)


if __name__ == "__main__":
    unittest.main()
