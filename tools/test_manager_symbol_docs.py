# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Keep hst_manager FindSymbol guidance aligned with public documentation."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANAGER = ROOT / "nk_manager.ps1"
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
        doc_file, _, doc_anchor = doc_path.partition("#")
        document = ROOT / doc_file
        self.assertTrue(document.is_file(), f"missing FindSymbol setup page: {doc_file}")
        if doc_anchor:
            headings = re.findall(r"^#{1,6} (.+)$", document.read_text(encoding="utf-8"), re.MULTILINE)
            slugs = {
                re.sub(r"[^a-z0-9 -]", "", h.lower()).replace(" ", "-")
                for h in headings
            }
            self.assertIn(doc_anchor, slugs, f"FindSymbol setup anchor is not a heading in {doc_file}")

        docs = document.read_text(encoding="utf-8")
        expected_candidates = (
            "docs/symbol_ref/functions.csv",
            "functions.csv",
        )
        for candidate in expected_candidates:
            manager_spelling = candidate.replace("/", "\\")
            self.assertIn(manager_spelling, manager)
            self.assertIn(candidate, docs)

        blocks = [
            " ".join(line.strip() for line in block.splitlines() if line.strip())
            for block in re.split(r"\n\s*\n", docs)
            if block.strip()
        ]
        self.assertTrue(
            any(
                all(term in block.lower() for term in ("optional", "reverse-engineering", "functions.csv"))
                for block in blocks
            ),
            "the symbol-reference guidance must describe the local CSV as optional",
        )
        self.assertTrue(
            any(
                all(term in block.lower() for term in ("functions.csv", "ignored", "untracked"))
                for block in blocks
            ),
            "the symbol-reference guidance must keep local files ignored and untracked",
        )

    def test_supported_symbol_reference_directories_are_ignored(self) -> None:
        ignored = GITIGNORE.read_text(encoding="utf-8")
        self.assertIn("/docs/symbol_ref/", ignored)
        self.assertIn("functions.csv", ignored)


if __name__ == "__main__":
    unittest.main()
