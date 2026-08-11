# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Deterministic offline unit tests for tools/audit_public_issue_links.py."""

import json
import pathlib
import tempfile
import unittest
from unittest.mock import MagicMock, patch

from tools.audit_public_issue_links import (
    audit_markdown_files,
    fetch_public_issues_map,
    get_tracked_markdown_files,
    main,
)


class TestAuditPublicIssueLinks(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.repo_path = pathlib.Path(self.temp_dir.name)

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_live_issue_and_pr_pass(self) -> None:
        doc = self.repo_path / "README.md"
        doc.write_text(
            "See https://github.com/Jstar269/nakagawa-recomp/issues/23 and "
            "https://github.com/Jstar269/nakagawa-recomp/pull/20\n",
            encoding="utf-8",
        )
        issues_map = {
            23: {
                "number": 23,
                "is_pr": False,
                "type": "Issue",
                "state": "open",
                "merged_at": None,
                "title": "DMA Issue",
                "url": "",
            },
            20: {
                "number": 20,
                "is_pr": True,
                "type": "PR",
                "state": "closed",
                "merged_at": "2026-08-11T00:00:00Z",
                "title": "SAS PR",
                "url": "",
            },
        }
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertEqual(len(findings), 2)
        self.assertTrue(all(finding[4] for finding in findings))

    def test_type_mismatch_fails(self) -> None:
        doc = self.repo_path / "README.md"
        doc.write_text(
            "See https://github.com/Jstar269/nakagawa-recomp/issues/20\n",
            encoding="utf-8",
        )
        issues_map = {
            20: {
                "number": 20,
                "is_pr": True,
                "type": "PR",
                "state": "closed",
                "merged_at": "2026-08-11T00:00:00Z",
                "title": "SAS PR",
                "url": "",
            }
        }
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("TYPE MISMATCH", findings[0][3])

    def test_missing_number_fails(self) -> None:
        doc = self.repo_path / "README.md"
        doc.write_text(
            "See https://github.com/Jstar269/nakagawa-recomp/issues/999\n",
            encoding="utf-8",
        )
        findings = audit_markdown_files(self.repo_path, {})
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("DEAD / UNRESOLVED PUBLIC REFERENCE #999", findings[0][3])

    def test_shorthand_dead_number_in_current_doc_fails(self) -> None:
        doc = self.repo_path / "NOTICE.md"
        doc.write_text("Blocked on #98\n", encoding="utf-8")
        findings = audit_markdown_files(self.repo_path, {})
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("DEAD / UNRESOLVED SHORTHAND", findings[0][3])

    def test_tracker_state_label_mismatch_fails(self) -> None:
        doc = self.repo_path / "ISSUES.md"
        doc.write_text(
            "# Status\n\n"
            "## Public tracker and implementation references\n\n"
            "- [Issue #23](https://github.com/Jstar269/nakagawa-recomp/issues/23) "
            "[CLOSED ISSUE]\n",
            encoding="utf-8",
        )
        issues_map = {
            23: {
                "number": 23,
                "is_pr": False,
                "type": "Issue",
                "state": "open",
                "merged_at": None,
                "title": "DMA Issue",
                "url": "",
            }
        }
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertTrue(any(not finding[4] for finding in findings))
        self.assertTrue(any("STALE TRACKER STATUS" in finding[3] for finding in findings))

    def test_merged_pr_tracker_label_passes(self) -> None:
        doc = self.repo_path / "ISSUES.md"
        doc.write_text(
            "# Status\n\n"
            "## Public tracker and implementation references\n\n"
            "- [PR #27](https://github.com/Jstar269/nakagawa-recomp/pull/27) [MERGED PR]\n",
            encoding="utf-8",
        )
        issues_map = {
            27: {
                "number": 27,
                "is_pr": True,
                "type": "PR",
                "state": "closed",
                "merged_at": "2026-08-11T00:00:00Z",
                "title": "Notice PR",
                "url": "",
            }
        }
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertTrue(all(finding[4] for finding in findings))

    def test_pagination_beyond_100(self) -> None:
        page1 = [
            {
                "number": i,
                "title": f"Issue {i}",
                "state": "open",
                "html_url": f"https://github.com/issues/{i}",
            }
            for i in range(1, 101)
        ]
        page2 = [
            {
                "number": 101,
                "title": "Issue 101",
                "state": "open",
                "html_url": "https://github.com/issues/101",
            }
        ]

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

    def test_recursive_fallback_includes_markdown_outside_docs(self) -> None:
        nested = self.repo_path / "interface" / "README.md"
        nested.parent.mkdir()
        nested.write_text("# Interface\n", encoding="utf-8")
        with patch("tools.audit_public_issue_links.subprocess.run", side_effect=OSError("git missing")):
            files = get_tracked_markdown_files(self.repo_path)
        self.assertIn(nested, files)

    def test_network_failure_optional_vs_strict(self) -> None:
        with patch("tools.audit_public_issue_links.fetch_public_issues_map", return_value=None):
            with patch(
                "sys.argv",
                ["audit_public_issue_links.py", "--repo-root", str(self.repo_path)],
            ):
                self.assertEqual(main(), 0)
            with patch(
                "sys.argv",
                [
                    "audit_public_issue_links.py",
                    "--repo-root",
                    str(self.repo_path),
                    "--strict",
                ],
            ):
                self.assertEqual(main(), 1)


if __name__ == "__main__":
    unittest.main()
