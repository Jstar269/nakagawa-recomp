# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import test_public_title_isolation
import title_manifest


class HstTitleManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.path = ROOT / "assets" / "titles" / "hst-ucus98701.json"
        # Opt-in by *tracked bytes*, never by directory presence (#335): an
        # ignored developer lookalike must not silently stand in for the
        # private fixture.
        if not test_public_title_isolation.private_title_tests_enabled("assets/titles/hst-ucus98701.json"):
            raise unittest.SkipTest(
                "private HST title manifest is unavailable (set NK_PRIVATE_TITLE_TESTS=1 to "
                "opt in with a local ignored manifest)"
            )

    def setUp(self) -> None:
        self.manifest = title_manifest.validate_manifest(title_manifest.load_manifest(self.path))

    def test_public_hst_identity_and_zero_based_executable_policy(self) -> None:
        self.assertEqual(self.manifest["id"], "hst-ucus98701-v1")
        self.assertEqual(self.manifest["kind"], "retail")
        self.assertEqual(self.manifest["disc"], {
            "id": "UCUS98701",
            "region": "NA",
            "revision_policy": "exact-disc-id",
        })
        self.assertEqual(self.manifest["executable"]["base"], 0)
        self.assertEqual(self.manifest["executable"]["entry"], 0)
        self.assertEqual(self.manifest["executable"]["bss_metadata_source"], "psp-header")
        self.assertEqual(self.manifest["codegen_profile"], "hst")

    def test_module_names_and_load_addresses_match_private_adapter(self) -> None:
        adapter = (ROOT / "tools" / "title_manager_plan.ps1").read_text(encoding="utf-8")
        for module in self.manifest["modules"]:
            self.assertIn(module["name"], adapter)
        self.assertNotIn("CODEGEN_PROFILE_ARG := --profile=hst", (ROOT / "Makefile").read_text(encoding="utf-8"))

    def test_extra_executable_span_is_owned_by_the_manifest_plan(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertNotIn("HST_EXTRA_SPANS", makefile)
        self.assertNotIn("0x00303194,0x00306e24", makefile)
        expected = self.manifest["executable"]["extra_executable_spans"]
        self.assertEqual(expected, [{"start": 0x00303194, "end": 0x00306E24}])
        adapter = (ROOT / "tools" / "title_manager_plan.ps1").read_text(encoding="utf-8")
        self.assertIn("3158420; end = 3173924", adapter)

    def test_zero_base_and_entry_match_manager_contract(self) -> None:
        manager = (ROOT / "nk_manager.ps1").read_text(encoding="utf-8")
        self.assertIn('"GAME_BASE=0"', manager)
        self.assertIn('"GAME_ENTRY=0"', manager)

    def test_manifest_contains_no_private_evidence_fields(self) -> None:
        rendered = title_manifest.canonical_json(self.manifest).lower()
        for forbidden in (
            "sha256",
            "private_key",
            "oracle",
            "decompiler_output",
            "savedata",
            "screenshot",
        ):
            self.assertNotIn(forbidden, rendered)
        self.assertIn("-TitleManifest", self.manifest["notes"])


if __name__ == "__main__":
    unittest.main()
