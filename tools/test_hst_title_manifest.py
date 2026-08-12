# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import re
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import title_manifest


class HstTitleManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.path = ROOT / "assets" / "titles" / "hst-ucus98701.json"
        if not cls.path.is_file():
            raise unittest.SkipTest(
                "private HST title manifest is unavailable in the sanitized public tree"
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

    def test_module_names_and_load_addresses_match_makefile(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        configured = {
            name: int(address, 0)
            for name, address in re.findall(
                r"([A-Za-z0-9_]+\.prx)@(0x[0-9A-Fa-f]+)",
                makefile,
            )
        }
        declared = {
            module["name"]: module["load_address"]
            for module in self.manifest["modules"]
        }
        self.assertEqual(declared, configured)
        self.assertIn("CODEGEN_PROFILE_ARG := --profile=hst", makefile)
        self.assertIn(
            "GAME_PSP_HEADER ?= place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN",
            makefile,
        )

    def test_extra_executable_span_matches_analyzer(self) -> None:
        analyzer = (ROOT / "tools" / "analyze.py").read_text(encoding="utf-8")
        match = re.search(
            r'DEFAULT_HST_EXTRA_SPANS\s*=\s*"(0x[0-9A-Fa-f]+),(0x[0-9A-Fa-f]+)"',
            analyzer,
        )
        self.assertIsNotNone(match)
        assert match is not None
        expected = [{"start": int(match.group(1), 0), "end": int(match.group(2), 0)}]
        self.assertEqual(self.manifest["executable"]["extra_executable_spans"], expected)

    def test_zero_base_and_entry_match_manager_contract(self) -> None:
        manager = (ROOT / "hst_manager.ps1").read_text(encoding="utf-8")
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
