# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for tools/verification_ledger.py, the HLE and VFPU evidence-class ledger."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hle_manifest  # noqa: E402
import verification_ledger as ledger_mod  # noqa: E402

ROOT = ledger_mod.ROOT
FIVE = ("hardware-measured", "ppsspp-derived", "game-route", "synthetic", "unverified")


class LedgerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.ledger = ledger_mod.build_ledger()
        cls.manifest = hle_manifest.build_manifest()
        cls.sources = hle_manifest.runtime_hle_sources()
        cls.registered = {r["handler"] for r in cls.manifest["registrations"]}

    def test_committed_outputs_are_current(self) -> None:
        for path, text in (
            (ledger_mod.DOC, ledger_mod.render_markdown(self.ledger)),
            (ledger_mod.HEADER, ledger_mod.render_header(self.ledger)),
        ):
            with self.subTest(path=path.name):
                self.assertEqual(
                    path.read_bytes(),
                    text.encode("utf-8"),
                    f"{path.relative_to(ROOT).as_posix()} is stale; run python tools/regen_all.py",
                )

    def test_rendering_is_deterministic(self) -> None:
        again = ledger_mod.build_ledger()
        self.assertEqual(json.dumps(again, sort_keys=True), json.dumps(self.ledger, sort_keys=True))

    def test_every_class_is_one_of_the_five(self) -> None:
        self.assertEqual(ledger_mod.CLASSES, FIVE)
        for row in (*self.ledger["hle"], *self.ledger["vfpu"]):
            self.assertIn(row["class"], FIVE, row)
        for side in ("hle", "vfpu"):
            self.assertEqual(tuple(self.ledger["summary"][side]), FIVE)
            self.assertEqual(sum(self.ledger["summary"][side].values()), len(self.ledger[side]))

    def test_every_registration_has_exactly_one_row(self) -> None:
        nids = [r["nid"] for r in self.ledger["hle"]]
        self.assertEqual(len(nids), len(set(nids)))
        self.assertEqual(set(nids), {r["nid"] for r in self.manifest["registrations"]})

    def test_generic_success_handlers_stay_unverified(self) -> None:
        fake = {r["nid"] for r in self.manifest["registrations"] if r["classification"] == "fake_success"}
        self.assertTrue(fake)
        for row in self.ledger["hle"]:
            if row["nid"] in fake:
                self.assertEqual(row["class"], "unverified", row)

    def test_game_route_has_no_public_source(self) -> None:
        self.assertEqual(self.ledger["summary"]["hle"]["game-route"], 0)
        self.assertEqual(self.ledger["summary"]["vfpu"]["game-route"], 0)

    def test_header_lists_exactly_the_unverified_nids_sorted(self) -> None:
        text = ledger_mod.HEADER.read_text(encoding="utf-8")
        body = text.split("sr_hle_unverified_nids[", 1)[1]
        listed = [int(tok[:-2], 16) for tok in body.split() if tok.startswith("0x") and tok.endswith("u,")]
        expected = sorted(int(r["nid"], 16) for r in self.ledger["hle"] if r["class"] == "unverified")
        self.assertEqual(listed, expected)
        self.assertIn(f"#define SR_HLE_UNVERIFIED_NID_COUNT {len(expected)}u", text)

    def test_vfpu_rows_cover_every_executed_census_form(self) -> None:
        refused = self.ledger["summary"]["vfpu_refused_forms"]
        self.assertGreater(refused, 0)
        dispositions = {r["disposition"] for r in self.ledger["vfpu"]}
        self.assertFalse(dispositions & ledger_mod.VFPU_REFUSED_DISPOSITIONS)

    # Curated tables: each validates against the live sources in both directions.

    def test_ppsspp_exclusions_validate(self) -> None:
        mentions = ledger_mod.ppsspp_handler_comments(self.sources)
        ledger_mod.validate_ppsspp_exclusions(self.registered, mentions)
        for handler in ledger_mod.PPSSPP_MENTIONS_NOT_A_SOURCE:
            self.assertIn(handler, self.registered)
            self.assertIn(handler, mentions)

    def test_ppsspp_exclusion_for_an_unknown_handler_fails(self) -> None:
        mentions = ledger_mod.ppsspp_handler_comments(self.sources)
        with self.assertRaisesRegex(ledger_mod.LedgerError, "no registration routes to"):
            ledger_mod.validate_ppsspp_exclusions(self.registered, mentions, {"h_NoSuchHandler": "reason"})

    def test_stale_ppsspp_exclusion_fails(self) -> None:
        mentions = ledger_mod.ppsspp_handler_comments(self.sources)
        quiet = sorted(self.registered - set(mentions))[0]
        with self.assertRaisesRegex(ledger_mod.LedgerError, "stale"):
            ledger_mod.validate_ppsspp_exclusions(self.registered, mentions, {quiet: "reason"})

    def test_ppsspp_blocks_cover_only_their_family(self) -> None:
        covered = ledger_mod.ppsspp_block_handlers(self.sources, self.registered)
        prefixes = tuple(b["prefix"] for b in ledger_mod.PPSSPP_BLOCKS)
        self.assertTrue(covered)
        for handler in covered:
            self.assertTrue(handler.startswith(prefixes), handler)
            self.assertIn(handler, self.registered)

    def test_ppsspp_block_with_a_missing_anchor_fails(self) -> None:
        block = ({"anchor": "no such comment text anywhere", "prefix": "h_Mp3", "reason": "r"},)
        with self.assertRaisesRegex(ledger_mod.LedgerError, "exactly once"):
            ledger_mod.ppsspp_block_handlers(self.sources, self.registered, block)

    def test_ppsspp_block_that_covers_no_registered_handler_fails(self) -> None:
        anchor = ledger_mod.PPSSPP_BLOCKS[0]["anchor"]
        block = ({"anchor": anchor, "prefix": "h_NoSuchFamily", "reason": "r"},)
        with self.assertRaisesRegex(ledger_mod.LedgerError, "covers no registered"):
            ledger_mod.ppsspp_block_handlers(self.sources, self.registered, block)

    def test_vfpu_measured_selftests_validate(self) -> None:
        import vfpu_coverage_report

        census = vfpu_coverage_report.build_census()
        tests = {t for r in census["rows"] for t in r["differential_tests"]}
        oracle = json.loads(hle_manifest.PSP_ORACLE_MANIFEST.read_text(encoding="utf-8"))
        ledger_mod.validate_vfpu_measured_selftests(tests, oracle)
        with self.assertRaisesRegex(ledger_mod.LedgerError, "no census row lists"):
            ledger_mod.validate_vfpu_measured_selftests(
                tests,
                oracle,
                {"vfpu-interp-selftest:no_such_check": {"oracle_id": "PSP-VFPU-FORMS-001", "reason": "r"}},
            )
        not_measured = next(t["id"] for t in oracle["tests"] if t.get("hardware_evidence") != "MEASURED")
        some_test = next(iter(ledger_mod.VFPU_MEASURED_SELFTESTS))
        with self.assertRaisesRegex(ledger_mod.LedgerError, "not a MEASURED row"):
            ledger_mod.validate_vfpu_measured_selftests(
                tests, oracle, {some_test: {"oracle_id": not_measured, "reason": "r"}}
            )

    def test_ppsspp_kernel_section_is_found_and_fails_closed(self) -> None:
        text = ledger_mod.RECOMP_C.read_text(encoding="utf-8")
        kernels = ledger_mod.ppsspp_vfpu_kernels(text)
        self.assertIn("sr_vfpu_sin", kernels)
        with self.assertRaises(ledger_mod.LedgerError):
            ledger_mod.ppsspp_vfpu_kernels(text.replace(ledger_mod.VFPU_PPSSPP_KERNEL_SECTION, "renamed"))


class RuntimeLogTests(unittest.TestCase):
    def test_host_tested_with_no_recorded_host_test_fails(self) -> None:
        # The evidence chain says HOST_TESTED but names no conformance cell or selftest dispatch.
        chain = {
            "_manifest": {"registrations": [{"nid": "0x00000001", "subsystem": "kernel"}]},
            "entries": [{
                "nid": "0x00000001", "canonical_name": "sceExample", "evidence_tier": "HOST_TESTED",
                "exercised": {"conformance_cells": [], "hle_selftest_dispatch": False, "psp_oracle_probes": []},
                "registration": {"handler": "h_example_not_registered", "classification": "implemented"},
            }],
        }
        with self.assertRaisesRegex(ledger_mod.LedgerError, "HOST_TESTED with no conformance cell"):
            ledger_mod.classify_hle(chain, {}, {})

    def test_census_tier_h_without_a_semantic_measurement_fails(self) -> None:
        census = {"rows": [{
            "compatibility_disposition": "supported", "differential_tests": [], "hardware_measured": [],
            "canonical_word": "0x60000000", "evidence_tier": "H",
        }]}
        with self.assertRaisesRegex(ledger_mod.LedgerError, "tier H with no semantic"):
            ledger_mod.classify_vfpu(census, ("no_such_kernel",))

    def test_dispatch_logs_unverified_nids_once_from_the_generated_table(self) -> None:
        source = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        self.assertIn('#include "hle_evidence.h"', source)
        self.assertIn("HLE: unverified: %s (NID 0x%08x) has no recorded evidence (docs/HLE_COVERAGE.md)", source)
        self.assertIn("hle_note_unverified(e);", source)


if __name__ == "__main__":
    unittest.main()
