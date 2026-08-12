# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for the defensive PSP import-table parser, classifier, and CI gate.

All ELF inputs are synthetic in-memory fixtures from tools/import_fixtures.py;
no game-derived bytes exist in or near these tests.
"""

from __future__ import annotations

import json
from pathlib import Path
import struct
import unittest

import import_audit
import import_audit_gate
from import_fixtures import (
    BASE_VADDR,
    INTERLEAVED_NIDS,
    INTERLEAVED_SHAPE,
    MIXED_FIXTURE_LIBS,
    build_import_elf,
    build_interleaved_import_elf,
)
import psp_import_table
from psp_import_table import ImportTableError, UNATTRIBUTED_LIBRARY, parse_import_table

ROOT = Path(__file__).resolve().parents[1]

SIMPLE_LIBS = [
    ("SynthAlpha", [0x11111111, 0x22222222]),
    ("SynthBeta", [0x33333333]),
]

# A synthetic manifest exercising all four classifications without depending
# on the live hle.c contents.
SYNTH_MANIFEST = {
    "schema": 1,
    "source": "synthetic",
    "registrations": [
        {"nid": "0x11111111", "name": "synthDedicated", "handler": "h_Synth",
         "origin": "static", "classification": "dedicated", "status": "partial"},
        {"nid": "0x22222222", "name": "synthFake", "handler": "h_ok",
         "origin": "static", "classification": "fake_success", "status": "stub"},
        {"nid": "0x33333333", "name": "synthRefused", "handler": "h_SynthRefuse",
         "origin": "static", "classification": "controlled_unsupported",
         "status": "controlled_unsupported"},
    ],
    "findings": [],
}


class ParserTests(unittest.TestCase):
    def test_well_formed_multi_library(self) -> None:
        table = parse_import_table(build_import_elf(SIMPLE_LIBS))
        self.assertEqual(table.libraries, ["SynthAlpha", "SynthBeta"])
        self.assertEqual(
            [(f.library, f.nid) for f in table.funcs],
            [("SynthAlpha", 0x11111111), ("SynthAlpha", 0x22222222), ("SynthBeta", 0x33333333)],
        )
        for f in table.funcs:
            self.assertGreaterEqual(f.stub_addr, BASE_VADDR)

    def test_six_word_entries_parse(self) -> None:
        table = parse_import_table(build_import_elf(SIMPLE_LIBS, entry_size_words=6))
        self.assertEqual(len(table.funcs), 3)

    def test_every_malformed_fixture_is_rejected_cleanly(self) -> None:
        for corrupt in import_audit_gate.MALFORMED_FIXTURES:
            with self.subTest(corrupt=corrupt):
                blob = build_import_elf(MIXED_FIXTURE_LIBS, corrupt=corrupt)
                with self.assertRaises(ImportTableError) as ctx:
                    parse_import_table(blob)
                self.assertTrue(str(ctx.exception), "error must carry a message")

    def test_malformed_fixtures_fail_for_the_intended_reason(self) -> None:
        """Each focused fixture must trip its own check, not an incidental one."""
        expected_fragment = {
            "zero_entry_size": "entry size 0 words",
            "entry_overrun": "runs past libstubend",
            "entry_header_truncated": "truncated",
            "wrapped_nid_table": "NID table",
            "wrapped_stub_area": "wraps the 32-bit space",
            "bad_name_ptr": "library name",
            "unterminated_name": "unterminated",
            "null_nid_table": "null NID table pointer",
            "nid_table_partially_backed": "NID table",
            "stub_table_partially_backed": "function stub span",
            "stub_area_unmapped": "function stub span",
            "stub_area_misaligned": "not 4-byte aligned",
            "stub_range_reversed": "is above libstubend",
            "stubend_past_segment": "not in any loaded range",
            "sectionless_bad_paddr": "not inside any loaded file range",
        }
        for corrupt, fragment in expected_fragment.items():
            with self.subTest(corrupt=corrupt):
                blob = build_import_elf(MIXED_FIXTURE_LIBS, corrupt=corrupt)
                with self.assertRaises(ImportTableError) as ctx:
                    parse_import_table(blob)
                self.assertIn(fragment, str(ctx.exception))

    def test_sectionless_prx_convention_parses_identically(self) -> None:
        """A stripped input (no section headers) resolves SceModuleInfo via
        phdr[0].p_paddr and must yield the same imports as the sectioned form."""
        sectioned = parse_import_table(build_import_elf(MIXED_FIXTURE_LIBS))
        sectionless = parse_import_table(build_import_elf(MIXED_FIXTURE_LIBS, sectionless=True))
        self.assertEqual(sectioned.funcs, sectionless.funcs)
        self.assertEqual(sectioned.libraries, sectionless.libraries)

    def test_sectionless_zero_paddr_is_rejected(self) -> None:
        blob = bytearray(build_import_elf(SIMPLE_LIBS, sectionless=True))
        struct.pack_into("<I", blob, 52 + 12, 0)  # phdr[0].p_paddr = 0
        with self.assertRaises(ImportTableError) as ctx:
            parse_import_table(bytes(blob))
        self.assertIn("cannot locate SceModuleInfo", str(ctx.exception))

    def test_sectionless_kernel_bit_is_masked(self) -> None:
        blob = bytearray(build_import_elf(SIMPLE_LIBS, sectionless=True))
        (paddr,) = struct.unpack_from("<I", blob, 52 + 12)
        struct.pack_into("<I", blob, 52 + 12, paddr | 0x80000000)
        table = parse_import_table(bytes(blob))
        self.assertEqual(len(table.funcs), 3)

    def test_not_an_elf(self) -> None:
        with self.assertRaises(ImportTableError):
            parse_import_table(b"MZ not an elf")

    def test_empty_file(self) -> None:
        with self.assertRaises(ImportTableError):
            parse_import_table(b"")

    def test_wrong_machine_rejected(self) -> None:
        blob = bytearray(build_import_elf(SIMPLE_LIBS))
        struct.pack_into("<H", blob, 18, 3)  # EM_386
        with self.assertRaises(ImportTableError):
            parse_import_table(bytes(blob))

    def test_64_bit_class_rejected(self) -> None:
        blob = bytearray(build_import_elf(SIMPLE_LIBS))
        blob[4] = 2  # ELFCLASS64
        with self.assertRaises(ImportTableError):
            parse_import_table(bytes(blob))

    def test_oversized_file_refused_without_reading(self) -> None:
        old = psp_import_table.MAX_FILE_SIZE
        psp_import_table.MAX_FILE_SIZE = 64
        try:
            with self.assertRaises(ImportTableError):
                parse_import_table(build_import_elf(SIMPLE_LIBS))
        finally:
            psp_import_table.MAX_FILE_SIZE = old

    def test_truncated_section_headers(self) -> None:
        blob = build_import_elf(SIMPLE_LIBS)
        with self.assertRaises(ImportTableError):
            parse_import_table(blob[: len(blob) - 30])

    def test_forged_function_count_is_capped(self) -> None:
        blob = bytearray(build_import_elf([("SynthAlpha", [0x11111111])]))
        # Locate the single stub entry by its name pointer+nid layout and forge
        # numFuncs to a huge value; the parser must refuse before allocating.
        idx = blob.find(struct.pack("<I", 0x11111111))
        self.assertGreater(idx, 0)
        entry = blob.find(struct.pack("<HBB", 0x0009, 5, 0))
        self.assertGreater(entry, 0)
        struct.pack_into("<H", blob, entry + 4, 0xFFFF)  # numFuncs
        with self.assertRaises(ImportTableError):
            parse_import_table(bytes(blob))


class InterleavedImportTests(unittest.TestCase):
    """The interleaved stub-table shape (35 of 51 slots) this model was
    written for: overlapping window runs plus trailing slots no window claims.

    Attribution expectations come from the loader contract: slots pair with
    NIDs globally by position, the last window reaching a position owns its
    library label, and positions outside every window are never patched (they
    stay visible as UNATTRIBUTED_LIBRARY instead of being dropped).
    """

    # Sectioned fixture: the .sceStub.text/.rodata.sceNid sections bound all
    # 51 positions, so every slot is emitted; 16 slots are unattributed.
    def test_sectioned_pairing_recovers_all_positions(self) -> None:
        table = parse_import_table(build_interleaved_import_elf(INTERLEAVED_SHAPE, INTERLEAVED_NIDS))
        self.assertEqual(len(table.funcs), 51)
        stub_base = table.funcs[0].stub_addr
        for i, f in enumerate(table.funcs):
            self.assertEqual(f.stub_addr, stub_base + i * 8, f"slot {i} stub address")
            self.assertEqual(f.nid, INTERLEAVED_NIDS[i], f"slot {i} NID")
        self.assertEqual(table.libraries, [name for name, _f, _c in INTERLEAVED_SHAPE])

    def test_sectioned_attribution_matches_last_claimer_wins(self) -> None:
        table = parse_import_table(build_interleaved_import_elf(INTERLEAVED_SHAPE, INTERLEAVED_NIDS))
        got = [f.library for f in table.funcs]
        expected = (
            ["sceDisplay"] * 2
            + ["sceGe_user"]
            + ["IoFileMgrForUser"] * 4
            + ["ModuleMgrForUser"]
            + ["ThreadManForUser"] * 3
            + ["LoadExecForUser"]
            + ["StdioForKernel"]
            + ["SysclibForKernel"] * 2
            + ["sceUtility"]
            + ["sceNetInet"] * 4
            + ["ThreadManForUser"] * 6   # positions 20-25: only the ThreadMan run reaches them
            + [UNATTRIBUTED_LIBRARY]    # position 26: no window claims it
            + ["Kernel_Library"] * 2
            + ["StdioForUser"] * 3
            + ["SysMemUserForUser"] * 4
            + [UNATTRIBUTED_LIBRARY] * 15  # positions 36-50
        )
        self.assertEqual(got, expected)

    def test_findings_describe_the_35_of_51_shape(self) -> None:
        table = parse_import_table(build_interleaved_import_elf(INTERLEAVED_SHAPE, INTERLEAVED_NIDS))
        self.assertEqual(
            table.findings,
            [
                "stub slots not covered by any library window: 16 positions "
                "[26, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50]",
                "stub slots claimed by multiple library windows: 13 positions "
                "[7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19]",
            ],
        )

    def test_sectionless_fallback_covers_only_claimed_runs(self) -> None:
        """Without section headers the pairing regions come from the window
        runs, so positions outside every run are invisible (the loader's own
        view): 36 region slots, position 26 unattributed, 36-50 absent."""
        table = parse_import_table(
            build_interleaved_import_elf(INTERLEAVED_SHAPE, INTERLEAVED_NIDS, sectionless=True)
        )
        self.assertEqual(len(table.funcs), 36)
        for i, f in enumerate(table.funcs):
            self.assertEqual(f.nid, INTERLEAVED_NIDS[i])
        self.assertEqual(table.funcs[26].library, UNATTRIBUTED_LIBRARY)
        self.assertEqual(
            table.findings,
            [
                "stub slots not covered by any library window: 1 positions [26]",
                "stub slots claimed by multiple library windows: 13 positions "
                "[7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19]",
            ],
        )

    def test_interleaved_parse_is_deterministic(self) -> None:
        a = parse_import_table(build_interleaved_import_elf(INTERLEAVED_SHAPE, INTERLEAVED_NIDS))
        b = parse_import_table(build_interleaved_import_elf(INTERLEAVED_SHAPE, INTERLEAVED_NIDS))
        self.assertEqual(a.funcs, b.funcs)
        self.assertEqual(a.findings, b.findings)
        self.assertEqual(a.libraries, b.libraries)

    def test_nid_region_mismatch_fails_closed(self) -> None:
        for sectionless in (False, True):
            with self.subTest(sectionless=sectionless):
                blob = build_interleaved_import_elf(
                    INTERLEAVED_SHAPE, INTERLEAVED_NIDS,
                    sectionless=sectionless, corrupt="nid_region_mismatch",
                )
                with self.assertRaises(ImportTableError) as ctx:
                    parse_import_table(blob)
                self.assertIn("does not match NID region size", str(ctx.exception))

    def test_audit_report_surfaces_structural_findings(self) -> None:
        table = parse_import_table(build_interleaved_import_elf(INTERLEAVED_SHAPE, INTERLEAVED_NIDS))
        report = import_audit.classify_imports(table.funcs, SYNTH_MANIFEST, findings=table.findings)
        self.assertEqual(report["findings"], table.findings)
        text = import_audit.render_text(report)
        self.assertIn("structural findings (interleaved stub table):", text)
        self.assertIn("not covered by any library window: 16", text)
        # A tiled table carries no structural findings.
        clean = parse_import_table(build_import_elf(MIXED_FIXTURE_LIBS))
        self.assertEqual(clean.findings, [])
        self.assertEqual(
            import_audit.classify_imports(clean.funcs, SYNTH_MANIFEST)["findings"], []
        )


class ClassifierTests(unittest.TestCase):
    def test_all_four_classes_and_missing(self) -> None:
        libs = [("SynthAlpha", [0x11111111, 0x22222222, 0x33333333, 0x44444444])]
        table = parse_import_table(build_import_elf(libs))
        report = import_audit.classify_imports(table.funcs, SYNTH_MANIFEST)
        got = {r["nid"]: r["classification"] for r in report["imports"]}
        self.assertEqual(
            got,
            {
                "0x11111111": "dedicated",
                "0x22222222": "fake_success",
                "0x33333333": "controlled_unsupported",
                "0x44444444": "missing",
            },
        )
        self.assertEqual(report["summary"]["missing"], 1)
        self.assertEqual(report["imports"][0]["status"], "partial")

    def test_addresses_redacted_by_default(self) -> None:
        table = parse_import_table(build_import_elf(SIMPLE_LIBS))
        report = import_audit.classify_imports(table.funcs, SYNTH_MANIFEST)
        for row in report["imports"]:
            self.assertNotIn("stub", row)
        text = import_audit.render_text(report)
        self.assertNotIn(f"0x{BASE_VADDR:08x}"[:6], text)

    def test_addresses_only_on_request(self) -> None:
        table = parse_import_table(build_import_elf(SIMPLE_LIBS))
        report = import_audit.classify_imports(table.funcs, SYNTH_MANIFEST, with_addresses=True)
        self.assertTrue(all("stub" in row for row in report["imports"]))

    def test_cross_library_duplicate_nids_reported(self) -> None:
        table = parse_import_table(build_import_elf(MIXED_FIXTURE_LIBS))
        report = import_audit.classify_imports(table.funcs, SYNTH_MANIFEST)
        self.assertEqual(
            report["cross_library_duplicate_nids"],
            [{"nid": "0x0badf00d", "libs": ["SynthLibA", "SynthLibB"]}],
        )

    def test_report_is_deterministic(self) -> None:
        def one() -> str:
            table = parse_import_table(build_import_elf(MIXED_FIXTURE_LIBS))
            report = import_audit.classify_imports(table.funcs, SYNTH_MANIFEST)
            return json.dumps(report, indent=2, sort_keys=True) + import_audit.render_text(report)

        self.assertEqual(one(), one())

    def test_render_text_triage_section(self) -> None:
        table = parse_import_table(build_import_elf(MIXED_FIXTURE_LIBS))
        report = import_audit.classify_imports(table.funcs, SYNTH_MANIFEST)
        text = import_audit.render_text(report)
        self.assertIn("Triage:", text)
        self.assertIn("[missing]", text)


class CliTests(unittest.TestCase):
    def test_cli_round_trip_and_clean_malformed_failure(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            good = Path(td) / "fixture_good.bin"
            bad = Path(td) / "fixture_bad.bin"
            good.write_bytes(build_import_elf(MIXED_FIXTURE_LIBS))
            bad.write_bytes(build_import_elf(MIXED_FIXTURE_LIBS, corrupt="wrapped_nid_table"))
            out = Path(td) / "report.json"
            txt = Path(td) / "report.txt"
            rc = import_audit.main(
                ["--elf", str(good), "--out", str(out), "--text", str(txt)]
            )
            self.assertEqual(rc, 0)
            report = json.loads(out.read_text(encoding="ascii"))
            self.assertEqual(report["summary"]["total"], 6)
            self.assertIn("PSP import-coverage audit", txt.read_text(encoding="ascii"))
            self.assertEqual(import_audit.main(["--elf", str(bad), "--out", str(out)]), 1)
            self.assertEqual(import_audit.main(["--elf", str(Path(td) / "absent.bin")]), 1)

    def test_cli_refuses_oversized_file_before_reading(self) -> None:
        import contextlib
        import io
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            big = Path(td) / "fixture_big.bin"
            big.write_bytes(build_import_elf(MIXED_FIXTURE_LIBS))
            old = psp_import_table.MAX_FILE_SIZE
            psp_import_table.MAX_FILE_SIZE = 64
            try:
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    rc = import_audit.main(["--elf", str(big)])
            finally:
                psp_import_table.MAX_FILE_SIZE = old
            self.assertEqual(rc, 1)
            self.assertIn("refusing inputs over 64", err.getvalue())
            self.assertEqual(err.getvalue().count("\n"), 1, "one-line error only")

    def test_read_is_bounded_even_if_stat_lied(self) -> None:
        """The read itself never allocates past the cap, so a file that grows
        between stat() and read() is refused after at most cap+1 bytes."""
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "grown.bin"
            path.write_bytes(b"A" * 100)
            self.assertIsNone(import_audit._read_bounded(path, 64))
            self.assertEqual(import_audit._read_bounded(path, 100), b"A" * 100)
            self.assertEqual(import_audit._read_bounded(path, 1000), b"A" * 100)

    def test_cli_refuses_file_that_defeats_stat(self) -> None:
        """Force the bounded-read path by making stat under-report the size."""
        import contextlib
        import io
        import tempfile
        from unittest import mock

        with tempfile.TemporaryDirectory() as td:
            big = Path(td) / "grown.bin"
            big.write_bytes(build_import_elf(MIXED_FIXTURE_LIBS))
            old = psp_import_table.MAX_FILE_SIZE
            psp_import_table.MAX_FILE_SIZE = 64
            fake = mock.Mock()
            fake.st_size = 10  # stat says tiny; the actual file is larger
            try:
                err = io.StringIO()
                with mock.patch.object(Path, "stat", return_value=fake), contextlib.redirect_stderr(err):
                    rc = import_audit.main(["--elf", str(big)])
            finally:
                psp_import_table.MAX_FILE_SIZE = old
            self.assertEqual(rc, 1)
            self.assertIn("exceeds 64 bytes; refusing", err.getvalue())


class ManifestValidationTests(unittest.TestCase):
    def setUp(self) -> None:
        import copy

        self.manifest = copy.deepcopy(SYNTH_MANIFEST)

    def _check(self, manifest_obj, fragment: str, *, raw: str | None = None) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "manifest.json"
            path.write_text(raw if raw is not None else json.dumps(manifest_obj), encoding="ascii")
            with self.assertRaises(import_audit.ManifestValidationError) as ctx:
                import_audit.load_manifest(path)
            msg = str(ctx.exception)
            self.assertIn(fragment, msg)
            self.assertNotIn("\n", msg, "manifest errors must be one line")

    def test_valid_manifest_loads(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "manifest.json"
            path.write_text(json.dumps(SYNTH_MANIFEST), encoding="ascii")
            self.assertEqual(len(import_audit.load_manifest(path)["registrations"]), 3)

    def test_unreadable_manifest(self) -> None:
        with self.assertRaises(import_audit.ManifestValidationError) as ctx:
            import_audit.load_manifest(Path("definitely-absent-manifest.json"))
        self.assertIn("cannot read", str(ctx.exception))

    def test_invalid_json(self) -> None:
        self._check(None, "not valid JSON", raw="{not json")

    def test_non_object_top_level(self) -> None:
        self._check([1, 2], "top level must be an object")

    def test_wrong_schema(self) -> None:
        self.manifest["schema"] = 99
        self._check(self.manifest, "schema 99")

    def test_missing_required_field(self) -> None:
        del self.manifest["registrations"][0]["handler"]
        self._check(self.manifest, "missing or non-string field 'handler'")

    def test_invalid_nid_string(self) -> None:
        self.manifest["registrations"][0]["nid"] = "0xZZ111111"
        self._check(self.manifest, "invalid NID")

    def test_invalid_classification(self) -> None:
        self.manifest["registrations"][0]["classification"] = "missing"
        self._check(self.manifest, "invalid classification")

    def test_invalid_status(self) -> None:
        self.manifest["registrations"][0]["status"] = "finished"
        self._check(self.manifest, "invalid status")

    def test_duplicate_nid(self) -> None:
        self.manifest["registrations"].append(dict(self.manifest["registrations"][0]))
        self._check(self.manifest, "duplicate NID")

    def test_empty_registrations(self) -> None:
        self.manifest["registrations"] = []
        self._check(self.manifest, "non-empty list")

    def test_cli_reports_invalid_manifest_as_one_line(self) -> None:
        import contextlib
        import io
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            good = Path(td) / "fixture_good.bin"
            good.write_bytes(build_import_elf(MIXED_FIXTURE_LIBS))
            bad_manifest = Path(td) / "manifest.json"
            bad_manifest.write_text("{not json", encoding="ascii")
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = import_audit.main(["--elf", str(good), "--manifest", str(bad_manifest)])
            self.assertEqual(rc, 1)
            self.assertIn("import_audit: invalid manifest:", err.getvalue())

    def test_schema_constant_matches_generator(self) -> None:
        import hle_manifest

        self.assertEqual(import_audit.EXPECTED_MANIFEST_SCHEMA, hle_manifest.MANIFEST_SCHEMA)


class GateWiringTests(unittest.TestCase):
    def test_gate_passes_on_current_tree(self) -> None:
        self.assertEqual(import_audit_gate.main(), 0)

    def test_ci_runs_the_gate(self) -> None:
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        self.assertIn("python tools/import_audit_gate.py", ci)

    def test_gate_detects_classification_downgrade(self) -> None:
        # A dedicated baseline entry degrading to fake_success must trip the
        # regression check even before the generic drift check reports it.
        manifest = {
            "registrations": [
                {"nid": "0x11111111", "name": "synthDedicated", "handler": "h_ok",
                 "origin": "static", "classification": "fake_success", "status": "stub"},
            ],
            "findings": [],
        }
        baseline = {
            "0x11111111": {"name": "synthDedicated", "handler": "h_Synth",
                           "classification": "dedicated", "status": "partial"},
        }
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "baseline.json"
            path.write_text(json.dumps(baseline), encoding="ascii")
            old = import_audit_gate.DEFAULT_BASELINE
            import_audit_gate.DEFAULT_BASELINE = path
            try:
                with self.assertRaises(SystemExit):
                    import_audit_gate.check_baseline(manifest)
            finally:
                import_audit_gate.DEFAULT_BASELINE = old


if __name__ == "__main__":
    unittest.main()
