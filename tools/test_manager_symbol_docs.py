# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Keep hst_manager FindSymbol guidance aligned with public documentation."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANAGER = ROOT / "hst_manager.ps1"
GITIGNORE = ROOT / ".gitignore"


class ManagerSymbolDocumentationTests(unittest.TestCase):
    def test_find_symbol_setup_document_exists_and_matches_candidates(self) -> None:
        manager = MANAGER.read_text(encoding="utf-8-sig")

        warning = re.search(
            r'functions\.csv not found\. See ([^" ]+) for setup\.', manager
        )
        self.assertIsNotNone(warning, "FindSymbol must name its maintained setup page")
        assert warning is not None

        doc_path = warning.group(1).replace("\\", "/")
        document = ROOT / doc_path
        self.assertTrue(document.is_file(), f"missing FindSymbol setup page: {doc_path}")

        docs = document.read_text(encoding="utf-8")
        expected_candidates = (
            "docs/opengrip_ref/functions.csv",
            "OpenGrip_For_Inspiration/functions.csv",
        )
        for candidate in expected_candidates:
            manager_spelling = candidate.replace("/", "\\")
            self.assertIn(manager_spelling, manager)
            self.assertIn(candidate, docs)

        self.assertIn("optional reverse-engineering aid", docs)
        self.assertIn("untracked", docs)

    def test_supported_symbol_reference_directories_are_ignored(self) -> None:
        ignored = GITIGNORE.read_text(encoding="utf-8")
        self.assertIn("/docs/opengrip_ref/", ignored)
        self.assertIn("/OpenGrip_For_Inspiration/", ignored)


if __name__ == "__main__":
    unittest.main()
