# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Offline deterministic tests for tools/lint_docs.py."""

import pathlib
import tempfile
import unittest
from unittest.mock import patch

from tools import lint_docs
from tools.lint_docs import (
    ROOT,
    get_tracked_markdown_files,
    lint_doc_links_and_topology,
    lint_readme,
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
