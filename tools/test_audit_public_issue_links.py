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

    def test_reused_live_number_in_historical_doc_fails(self) -> None:
        doc = self.repo_path / "docs" / "HARDWARE_ORACLE.md"
        doc.parent.mkdir()
        doc.write_text(
            "Historical PR https://github.com/Jstar269/nakagawa-recomp/pull/35\n",
            encoding="utf-8",
        )
        issues_map = {
            35: {
                "number": 35,
                "is_pr": True,
                "type": "PR",
                "state": "open",
                "merged_at": None,
                "title": "Unrelated sanitized-era scanner migration",
                "url": "",
            }
        }
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("HISTORICAL NUMBER COLLISION", findings[0][3])

    def test_shorthand_dead_number_in_current_doc_fails(self) -> None:
        doc = self.repo_path / "NOTICE.md"
        doc.write_text("Blocked on #98\n", encoding="utf-8")
        findings = audit_markdown_files(self.repo_path, {})
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("DEAD / UNRESOLVED SHORTHAND", findings[0][3])

    def test_shorthand_ignored_in_fenced_code_and_heading(self) -> None:
        doc = self.repo_path / "README.md"
        doc.write_text(
            "# 98 Example heading\n\n"
            "```text\n"
            "#98 fixture token\n"
            "```\n",
            encoding="utf-8",
        )
        findings = audit_markdown_files(self.repo_path, {})
        self.assertEqual(findings, [])

    def test_external_github_tracker_url_does_not_become_local_shorthand(self) -> None:
        doc = self.repo_path / "assets" / "README.md"
        doc.parent.mkdir()
        doc.write_text(
            "See [#16946](https://github.com/hrydgard/ppsspp/issues/16946)\n",
            encoding="utf-8",
        )
        findings = audit_markdown_files(self.repo_path, {})
        self.assertEqual(findings, [])

    def test_at_a_glance_open_row_linking_closed_issue_fails(self) -> None:
        # Regression: ISSUES.md used to declare an "Open" portable-float-to-word row
        # that pointed at #38 after the fix had merged (the follow-on was #40).
        doc = self.repo_path / "ISSUES.md"
        doc.write_text(
            "# Status\n\n"
            "## At a glance\n\n"
            "| Priority | State | Public work item |\n"
            "| --- | --- | --- |\n"
            "| P1 | Open | Float-to-word: [issue #38](https://github.com/Jstar269/nakagawa-recomp/issues/38) |\n",
            encoding="utf-8",
        )
        issues_map = {
            38: {
                "number": 38,
                "is_pr": False,
                "type": "Issue",
                "state": "closed",
                "merged_at": None,
                "title": "portable float-to-word (merged via PR #39)",
                "url": "",
            }
        }
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertTrue(any(not finding[4] for finding in findings))
        self.assertTrue(any("STALE TRACKER STATUS" in finding[3] for finding in findings))
        self.assertTrue(any("State 'Open'" in finding[3] for finding in findings))

    def test_at_a_glance_open_row_linking_open_issue_passes(self) -> None:
        doc = self.repo_path / "ISSUES.md"
        doc.write_text(
            "# Status\n\n"
            "## At a glance\n\n"
            "| Priority | State | Public work item |\n"
            "| --- | --- | --- |\n"
            "| P1 | Open | Exotic VFPU: [issue #40](https://github.com/Jstar269/nakagawa-recomp/issues/40) |\n",
            encoding="utf-8",
        )
        issues_map = {
            40: {
                "number": 40,
                "is_pr": False,
                "type": "Issue",
                "state": "open",
                "merged_at": None,
                "title": "Exotic VFPU NaN/Inf divergences",
                "url": "",
            }
        }
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertTrue(all(finding[4] for finding in findings))

    def test_at_a_glance_non_open_rows_are_not_checked(self) -> None:
        doc = self.repo_path / "ISSUES.md"
        doc.write_text(
            "# Status\n\n"
            "## At a glance\n\n"
            "| Priority | State | Public work item |\n"
            "| --- | --- | --- |\n"
            "| P1 | Blocked | [issue #23](https://github.com/Jstar269/nakagawa-recomp/issues/23) |\n",
            encoding="utf-8",
        )
        issues_map = {
            23: {
                "number": 23,
                "is_pr": False,
                "type": "Issue",
                "state": "closed",
                "merged_at": None,
                "title": "Closed thing",
                "url": "",
            }
        }
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertTrue(all(finding[4] for finding in findings))

    def test_copilot_instructions_is_current_facing(self) -> None:
        # copilot-instructions.md carried pre-export shorthand numbers (#20 as an
        # "issue", #32 for the ATRAC bridge) that misresolved to unrelated public
        # objects. It is developer-facing, so its shorthand references must now be
        # liveness-audited like the other current-facing docs.
        doc = self.repo_path / ".github" / "copilot-instructions.md"
        doc.parent.mkdir()
        doc.write_text("See GitHub issue #999\n", encoding="utf-8")
        findings = audit_markdown_files(self.repo_path, {})
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("DEAD / UNRESOLVED SHORTHAND", findings[0][3])

    def test_osps_baseline_and_dated_records_are_historical_evidence(self) -> None:
        # Self-declared pre-republication snapshots and dated audit records keep
        # their pre-export tracker numbers as plain historical evidence; a 404 on
        # the public tracker must be reported as preserved historical evidence, not
        # as a dead current reference.
        cases = (
            "docs/OSPS_BASELINE.md",
            "docs/ISSUE196_DIRECT_XB.md",
            "docs/provenance/MODIFIED_FILE_NOTICES.md",
        )
        for index, rel in enumerate(cases):
            with self.subTest(rel=rel):
                root = self.repo_path / f"case{index}"
                doc = root / rel
                doc.parent.mkdir(parents=True, exist_ok=True)
                doc.write_text(
                    "See https://github.com/Jstar269/nakagawa-recomp/issues/999\n",
                    encoding="utf-8",
                )
                findings = audit_markdown_files(root, {})
                self.assertEqual(len(findings), 1)
                self.assertTrue(findings[0][4])
                self.assertIn("HISTORICAL EVIDENCE REFERENCE", findings[0][3])

    def test_tracker_state_label_mismatch_fails(self) -> None:
        doc = self.repo_path / "ISSUES.md"
        doc.write_text(
            "# Status\n\n"
            "## Current public tracker\n\n"
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

    def test_open_pr_tracker_label_passes(self) -> None:
        doc = self.repo_path / "ISSUES.md"
        doc.write_text(
            "# Status\n\n"
            "## Current public tracker\n\n"
            "- [PR #1](https://github.com/Jstar269/nakagawa-recomp/pull/1) [OPEN PR]\n",
            encoding="utf-8",
        )
        issues_map = {
            1: {
                "number": 1,
                "is_pr": True,
                "type": "PR",
                "state": "open",
                "merged_at": None,
                "title": "Dependency PR",
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
