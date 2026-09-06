# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Offline deterministic tests for tools/lint_docs.py."""

import pathlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import lint_docs
from tools.lint_docs import (
    ROOT,
    get_tracked_markdown_files,
    lint_doc_links_and_topology,
    lint_doc_truth,
    lint_readme,
    lint_titles_readme,
    lint_toolchain_baseline_marker,
    run_all_doc_lints,
)


class TestDocFreshnessLinter(unittest.TestCase):
    def test_repository_documentation_freshness(self) -> None:
        errors = run_all_doc_lints(ROOT)
        self.assertEqual(
            errors,
            [],
            "Documentation freshness linter found staleness defects:\n" + "\n".join(errors),
        )

    def test_readme_dated_current_status_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            readme = pathlib.Path(temp_dir) / "README.md"
            readme.write_text("Project status as of 2026-07-25 is green.\n", encoding="utf-8")
            errors = lint_readme(readme)
        self.assertTrue(any("volatile dated status claim" in error for error in errors))

    def test_obsolete_public_topology_wording_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            doc = root / "docs" / "PUBLICATION_READINESS.md"
            doc.parent.mkdir()
            doc.write_text("For the **new public repository**, configure rulesets.\n", encoding="utf-8")
            errors = lint_doc_links_and_topology(doc, root)
        self.assertTrue(any("obsolete private-repository topology" in error for error in errors))

    def test_historical_record_may_preserve_retired_issue_url(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            doc = root / "docs" / "STATUS_HISTORY.md"
            doc.parent.mkdir()
            doc.write_text(
                "Historical tracker: https://github.com/Jstar269/nakagawa-recomp/issues/98\n",
                encoding="utf-8",
            )
            errors = lint_doc_links_and_topology(doc, root)
        self.assertEqual(errors, [])

    def test_missing_repository_relative_link_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            doc = root / "docs" / "README.md"
            doc.parent.mkdir()
            doc.write_text("See [missing](NOT_PRESENT.md).\n", encoding="utf-8")
            errors = lint_doc_links_and_topology(doc, root)
        self.assertTrue(any("missing repository-relative link target" in error for error in errors))

    def test_existing_repository_relative_link_and_fenced_example_are_allowed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            doc = root / "docs" / "README.md"
            target = root / "NOTICE.md"
            doc.parent.mkdir()
            target.write_text("notice\n", encoding="utf-8")
            doc.write_text(
                "See [notice](../NOTICE.md).\n\n```md\n[example](ABSENT.md)\n```\n",
                encoding="utf-8",
            )
            errors = lint_doc_links_and_topology(doc, root)
        self.assertEqual(errors, [])

    def test_recursive_fallback_includes_nested_markdown(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            nested = root / "interface" / "README.md"
            nested.parent.mkdir()
            nested.write_text("# Interface\n", encoding="utf-8")
            with patch("tools.lint_docs.subprocess.run", side_effect=OSError("git missing")):
                files = get_tracked_markdown_files(root)
        self.assertIn(nested, files)


class TestDocTruthInvariants(unittest.TestCase):
    def _write_doc(self, root: pathlib.Path, rel: str, text: str) -> pathlib.Path:
        doc = root / rel
        doc.parent.mkdir(parents=True, exist_ok=True)
        doc.write_text(text, encoding="utf-8")
        return doc

    def test_checked_in_hst_manifest_claim_is_rejected_while_untracked(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            doc = self._write_doc(
                root, "docs/PORTING.md",
                "The manager accepts only the checked-in HST manifest.\n",
            )
            errors = lint_doc_truth(doc, root, hst_manifest_tracked=False)
        self.assertTrue(any("checked-in" in error for error in errors))

    def test_negated_checked_in_wording_is_allowed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            doc = self._write_doc(
                root, "assets/titles/README.md",
                "`hst-ucus98701.json` is intentionally not checked in.\n"
                "The local manifest is publication-excluded, never checked in.\n",
            )
            errors = lint_doc_truth(doc, root, hst_manifest_tracked=False)
        self.assertEqual(errors, [])

    def test_gitignored_hst_manifest_claim_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            doc = self._write_doc(
                root, "docs/SETUP.md",
                "hst-ucus98701.json is the local, Git-ignored HST title manifest.\n",
            )
            errors = lint_doc_truth(doc, root, hst_manifest_tracked=False)
        self.assertTrue(any("Git-ignored" in error for error in errors))

    def test_manifest_checks_stand_down_once_the_file_is_tracked(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            doc = self._write_doc(
                root, "docs/PORTING.md",
                "The manager accepts only the checked-in HST manifest.\n",
            )
            errors = lint_doc_truth(doc, root, hst_manifest_tracked=True)
        self.assertEqual(errors, [])

    def test_frozen_hle_census_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            doc = self._write_doc(
                root, "ISSUES.md",
                "46 title guest addresses across 59 sites in `src/rt/hle.c`.\n",
            )
            errors = lint_doc_truth(doc, root, hst_manifest_tracked=False)
        self.assertTrue(any("frozen HLE census" in error for error in errors))

    def test_dead_repo_commit_link_is_rejected_and_live_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / "a.txt").write_text("x", encoding="utf-8")
            subprocess.run(["git", "-C", str(root), "add", "."], check=True)
            subprocess.run(
                ["git", "-C", str(root), "-c", "user.email=t@t",
                 "-c", "user.name=t", "commit", "-qm", "x"],
                check=True,
            )
            live = subprocess.run(
                ["git", "-C", str(root), "rev-parse", "HEAD"],
                capture_output=True, text=True, check=True,
            ).stdout.strip()
            doc = self._write_doc(
                root, "docs/NOTE.md",
                "base https://github.com/Jstar269/nakagawa-recomp/tree/deadbee1234abcd end\n",
            )
            errors = lint_doc_truth(doc, root, hst_manifest_tracked=False)
            self.assertTrue(any("unavailable in public history" in e for e in errors))
            doc.write_text(
                f"base https://github.com/Jstar269/nakagawa-recomp/tree/{live} end\n",
                encoding="utf-8",
            )
            self.assertEqual(lint_doc_truth(doc, root, hst_manifest_tracked=False), [])

    def test_truth_snapshot_dir_is_excluded(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            doc = self._write_doc(
                root, "docs/research/project-truth/SNAP.md",
                "The checked-in HST manifest held 46 title guest addresses across 59 sites.\n",
            )
            errors = lint_doc_truth(doc, root, hst_manifest_tracked=False)
            self.assertEqual(errors, [])
            self.assertEqual(lint_doc_links_and_topology(doc, root), [])

    def test_titles_readme_must_name_tracked_manifests(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            titles = root / "assets" / "titles"
            titles.mkdir(parents=True)
            (titles / "synthetic.json").write_text("{}\n", encoding="utf-8")
            (titles / "README.md").write_text("no inventory here\n", encoding="utf-8")
            subprocess.run(["git", "-C", str(root), "add", "."], check=True)
            errors = lint_titles_readme(root)
            self.assertTrue(any("synthetic.json" in e for e in errors))
            (titles / "README.md").write_text("inventory: synthetic.json\n", encoding="utf-8")
            self.assertEqual(lint_titles_readme(root), [])

    def test_toolchain_baseline_requires_non_current_marker(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            doc_dir = root / "docs"
            doc_dir.mkdir()
            baseline = doc_dir / "TOOLCHAIN_BASELINE_2026-08.md"
            baseline.write_text("# Toolchain baseline\n", encoding="utf-8")
            self.assertTrue(lint_toolchain_baseline_marker(root))
            baseline.write_text(
                "# Toolchain baseline\n\nSTATUS = HISTORICAL / REFERENCE.\n",
                encoding="utf-8",
            )
            self.assertEqual(lint_toolchain_baseline_marker(root), [])


if __name__ == "__main__":
    unittest.main()


class RetiredIssueDenylistExpiry(unittest.TestCase):
    """GitHub numbers issues and PRs from one sequence, so the public repository
    reallocates the private-era numbers over time. A denylist entry that has been
    reallocated flags a LIVE object and blocks legitimate work, so the module must
    refuse to load with one present."""

    def test_no_denylisted_number_is_already_reallocated(self) -> None:
        self.assertTrue(
            all(n > lint_docs.PUBLIC_ISSUE_NUMBER_FRONTIER
                for n in lint_docs.RETIRED_PRIVATE_ISSUE_NUMBERS),
            "a denylisted number is at or below the public numbering frontier",
        )

    def test_the_frontier_guard_is_load_bearing(self) -> None:
        """MUTATION: reintroduce a reallocated number and the module must refuse to
        load, rather than silently flagging a live public issue."""
        source = (ROOT / "tools" / "lint_docs.py").read_text(encoding="utf-8")
        mutated = source.replace(
            "RETIRED_PRIVATE_ISSUE_NUMBERS = (\n    139,",
            "RETIRED_PRIVATE_ISSUE_NUMBERS = (\n"
            f"    {lint_docs.PUBLIC_ISSUE_NUMBER_FRONTIER}, 139,",
            1,
        )
        self.assertNotEqual(mutated, source, "mutation anchor not found")
        namespace = {"__file__": str(ROOT / "tools" / "lint_docs.py"), "__name__": "x"}
        with self.assertRaises(ValueError) as caught:
            exec(compile(mutated, "lint_docs", "exec"), namespace)  # noqa: S102
        self.assertIn("reallocated", str(caught.exception))

    def test_the_denylist_still_catches_a_genuinely_dead_number(self) -> None:
        """Vacuity guard: the expiry rule must not have emptied the denylist."""
        self.assertTrue(lint_docs.RETIRED_PRIVATE_ISSUE_NUMBERS)
        dead = lint_docs.RETIRED_PRIVATE_ISSUE_NUMBERS[-1]
        url = f"see github.com/Jstar269/nakagawa-recomp/issues/{dead} for context"
        self.assertTrue(lint_docs.RETIRED_PRIVATE_ISSUE_URLS.search(url))
        live = f"see github.com/Jstar269/nakagawa-recomp/issues/{lint_docs.PUBLIC_ISSUE_NUMBER_FRONTIER}"
        self.assertIsNone(lint_docs.RETIRED_PRIVATE_ISSUE_URLS.search(live))
