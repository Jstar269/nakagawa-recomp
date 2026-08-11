# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Deterministic offline unit tests for tools/audit_public_issue_links.py."""

import json
import io
import pathlib
import tempfile
import unittest
from unittest.mock import patch, MagicMock

from tools.audit_public_issue_links import (
    fetch_public_issues_map,
    audit_markdown_files,
    main,
)


class TestAuditPublicIssueLinks(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.repo_path = pathlib.Path(self.temp_dir.name)

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_live_issue_and_pr_pass(self) -> None:
        """Verify that live issue and PR URLs pass validation."""
        doc = self.repo_path / "README.md"
        doc.write_text(
            "See https://github.com/Jstar269/nakagawa-recomp/issues/23 and https://github.com/Jstar269/nakagawa-recomp/pull/20\n",
            encoding="utf-8",
        )

        issues_map = {
            23: {"number": 23, "is_pr": False, "type": "Issue", "state": "open", "title": "DMA Issue", "url": ""},
            20: {"number": 20, "is_pr": True, "type": "PR", "state": "closed", "title": "SAS PR", "url": ""},
        }

        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertEqual(len(findings), 2)
        self.assertTrue(all(f[4] for f in findings))

    def test_type_mismatch_fails(self) -> None:
        """Verify that a URL type mismatch (e.g., /issues/ pointing to a PR) is flagged as a failure."""
        doc = self.repo_path / "README.md"
        doc.write_text(
            "See https://github.com/Jstar269/nakagawa-recomp/issues/20\n",  # #20 is a PR, not issue
            encoding="utf-8",
        )

        issues_map = {
            20: {"number": 20, "is_pr": True, "type": "PR", "state": "closed", "title": "SAS PR", "url": ""},
        }

        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("TYPE MISMATCH", findings[0][3])

    def test_missing_number_fails(self) -> None:
        """Verify that a 404/missing issue number is reported as DEAD / UNRESOLVED PUBLIC REFERENCE."""
        doc = self.repo_path / "README.md"
        doc.write_text(
            "See https://github.com/Jstar269/nakagawa-recomp/issues/999\n",
            encoding="utf-8",
        )

        issues_map = {}
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("DEAD / UNRESOLVED PUBLIC REFERENCE #999", findings[0][3])

    def test_shorthand_dead_number_in_current_doc_fails(self) -> None:
        """Verify that shorthand #N in current-facing documents fails if unresolved."""
        doc = self.repo_path / "NOTICE.md"
        doc.write_text("Blocked on #98\n", encoding="utf-8")

        issues_map = {}
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("DEAD / UNRESOLVED SHORTHAND", findings[0][3])

    def test_pagination_beyond_100(self) -> None:
        """Verify API pagination logic correctly fetches multiple pages when >100 issues exist."""
        page1 = [{"number": i, "title": f"Issue {i}", "state": "open", "html_url": f"https://github.com/issues/{i}"} for i in range(1, 101)]
        page2 = [{"number": 101, "title": "Issue 101", "state": "open", "html_url": "https://github.com/issues/101"}]

        resp1 = MagicMock()
        resp1.read.return_value = json.dumps(page1).encode("utf-8")
        resp1.__enter__.return_value = resp1

        resp2 = MagicMock()
        resp2.read.return_value = json.dumps(page2).encode("utf-8")
        resp2.__enter__.return_value = resp2

        with patch("urllib.request.urlopen", side_effect=[resp1, resp2]):
            issues_map = fetch_public_issues_map()
            self.assertIsNotNone(issues_map)
            self.assertEqual(len(issues_map), 101)
            self.assertIn(101, issues_map)

    def test_network_failure_optional_vs_strict(self) -> None:
        """Verify exit code behavior when network is unavailable (optional mode=0, strict mode=1)."""
        with patch("tools.audit_public_issue_links.fetch_public_issues_map", return_value=None):
            with patch("sys.argv", ["audit_public_issue_links.py", "--repo-root", str(self.repo_path)]):
                exit_code_optional = main()
                self.assertEqual(exit_code_optional, 0)

            with patch("sys.argv", ["audit_public_issue_links.py", "--repo-root", str(self.repo_path), "--strict"]):
                exit_code_strict = main()
                self.assertEqual(exit_code_strict, 1)


if __name__ == "__main__":
    unittest.main()
