# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Offline deterministic tests for tools/lint_docs.py and the tools/README.md index."""

import pathlib
import re
import sys
import subprocess
import tempfile
import unittest
from pathlib import PurePosixPath
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools import lint_docs
from tools.nk_core.git_isolation import run_git
from tools.lint_docs import (
    ROOT,
    get_tracked_markdown_files,
    lint_capability_disposition_table,
    lint_doc_links_and_topology,
    lint_doc_truth,
    lint_docs_index_completeness,
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
            nested = root / "nested" / "README.md"
            nested.parent.mkdir()
            nested.write_text("# Interface\n", encoding="utf-8")
            with patch("tools.lint_docs.subprocess.run", side_effect=OSError("git missing")):
                files = get_tracked_markdown_files(root)
        self.assertIn(nested, files)

    def test_tracked_markdown_listing_ignores_worktree_deletions(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            docs = root / "docs"
            docs.mkdir()
            (docs / "README.md").write_text(
                "# Documentation\n\n| Path | Status |\n| --- | --- |\n", encoding="utf-8")
            (docs / "gone.md").unlink(missing_ok=True)
            result = subprocess.CompletedProcess(
                args=["git", "ls-files"], returncode=0,
                stdout="docs/README.md\ndocs/gone.md\n", stderr="")
            with patch("tools.lint_docs.subprocess.run", return_value=result):
                self.assertEqual(get_tracked_markdown_files(root), [docs / "README.md"])
                self.assertEqual(lint_docs_index_completeness(root), [])

    def test_docs_index_completeness_passes_on_repo(self) -> None:
        errors = lint_docs_index_completeness(ROOT)
        self.assertEqual(errors, [])

    def test_docs_index_completeness_fails_when_entry_missing(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            docs_dir = root / "docs"
            docs_dir.mkdir()
            (docs_dir / "README.md").write_text(
                "# Documentation\n\n| Path | Status |\n| --- | --- |\n| `ALPHA.md` | CURRENT |\n",
                encoding="utf-8",
            )
            (docs_dir / "ALPHA.md").write_text("# Alpha\n", encoding="utf-8")
            (docs_dir / "BETA.md").write_text("# Beta\n", encoding="utf-8")
            errors = lint_docs_index_completeness(root)
            self.assertTrue(any("docs/BETA.md is not indexed" in e for e in errors))


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
            run_git(["init", "-q"], cwd=root, check=True)
            (root / "a.txt").write_text("x", encoding="utf-8")
            run_git(["add", "."], cwd=root, check=True)
            run_git(["commit", "-qm", "x"], cwd=root, check=True)
            live = run_git(
                ["rev-parse", "HEAD"],
                cwd=root,
                capture_output=True,
                text=True,
                check=True,
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
            run_git(["init", "-q"], cwd=root, check=True)
            titles = root / "assets" / "titles"
            titles.mkdir(parents=True)
            (titles / "synthetic.json").write_text("{}\n", encoding="utf-8")
            (titles / "README.md").write_text("no inventory here\n", encoding="utf-8")
            run_git(["add", "."], cwd=root, check=True)
            errors = lint_titles_readme(root)
            self.assertTrue(any("synthetic.json" in e for e in errors))
            (titles / "README.md").write_text("inventory: synthetic.json\n", encoding="utf-8")
            self.assertEqual(lint_titles_readme(root), [])

    def test_toolchain_baseline_requires_non_current_marker(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            doc_dir = root / "docs"
            doc_dir.mkdir()
            baseline = doc_dir / "archive" / "TOOLCHAIN_BASELINE_2026-08.md"
            baseline.parent.mkdir()
            baseline.write_text("# Toolchain baseline\n", encoding="utf-8")
            self.assertTrue(lint_toolchain_baseline_marker(root))
            baseline.write_text(
                "# Toolchain baseline\n\nSTATUS = HISTORICAL / REFERENCE.\n",
                encoding="utf-8",
            )
            self.assertEqual(lint_toolchain_baseline_marker(root), [])


class TestCapabilityDispositionTable(unittest.TestCase):
    """The per-capability disposition of the retired web UI must name real evidence.

    Every row answers one question a consumer asks before adding an ISO: what does
    this product actually do, on which surface, and which test proves it. A row that
    cannot answer is the failure this table exists to prevent, so the linter rejects
    a missing section, an unknown status, a PASS/PARTIAL row whose test path does not
    exist, and an UNBUILT/DROPPED row with no tracking issue.
    """

    HEADER = (
        "| ID | Capability | Status | Surface | Owning evidence |\n"
        "| --- | :--- | :---: | :--- | :--- |\n"
    )

    @staticmethod
    def _matrix(root: pathlib.Path, rows: str) -> pathlib.Path:
        root.mkdir(parents=True, exist_ok=True)
        (root / "tools").mkdir(exist_ok=True)
        (root / "tools" / "test_nk_core.py").write_text("# scratch\n", encoding="utf-8")
        doc = root / lint_docs.CAPABILITY_MATRIX_DOC
        doc.parent.mkdir(parents=True, exist_ok=True)
        doc.write_text(
            "# Native UI Regression Matrix\n\n"
            f"{lint_docs.CAPABILITY_TABLE_BEGIN}\n\n"
            f"{TestCapabilityDispositionTable.HEADER}{rows}"
            f"{lint_docs.CAPABILITY_TABLE_END}\n",
            encoding="utf-8",
        )
        return doc

    def test_repository_disposition_table_is_complete(self) -> None:
        errors = lint_capability_disposition_table(ROOT)
        self.assertEqual(
            errors,
            [],
            "the capability disposition table declares no valid status or evidence:\n"
            + "\n".join(errors),
        )

    def test_pass_row_naming_an_existing_test_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            self._matrix(
                root,
                "| `ISO_ADD` | Add an ISO | **PASS** | native player | "
                "`tools/test_nk_core.py` |\n",
            )
            errors = lint_capability_disposition_table(root)
        self.assertEqual(errors, [])

    def test_unknown_status_is_rejected(self) -> None:
        """The older vocabularies (`IN_PROGRESS`, `NOT_SUPPORTED`) escape both rules."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            self._matrix(
                root,
                "| `MULTI_TITLE` | Many discs | IN_PROGRESS | native player | "
                "`tools/test_nk_core.py` |\n",
            )
            errors = lint_capability_disposition_table(root)
        self.assertTrue(
            any("has status 'IN_PROGRESS'" in error for error in errors), errors)

    def test_pass_row_naming_a_test_that_does_not_exist_is_rejected(self) -> None:
        """A named test that was never written is an unbacked claim, so it must fail."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            self._matrix(
                root,
                "| `HUD` | Frame overlay | PASS | native runtime | "
                "`tools/test_never_written.py` |\n",
            )
            errors = lint_capability_disposition_table(root)
            self.assertTrue(
                any("names no repository path that exists" in error for error in errors), errors)
            # The same row becomes valid once the path it names actually exists.
            (root / "tools" / "test_never_written.py").write_text("# scratch\n", encoding="utf-8")
            self.assertEqual(lint_capability_disposition_table(root), [])

    def test_pass_row_naming_only_a_command_is_rejected(self) -> None:
        """A backticked make target is not a repository path and proves nothing."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            self._matrix(
                root,
                "| `HUD` | Frame overlay | PASS | native runtime | "
                "`mingw32-make display-smoke-player` |\n",
            )
            errors = lint_capability_disposition_table(root)
        self.assertTrue(any("names no backticked repository test path" in error for error in errors),
                        errors)

    def test_unbuilt_row_without_an_issue_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            self._matrix(
                root,
                "| `VRAM_VIEWER` | Guest VRAM inspection | UNBUILT | none | "
                "Not implemented yet. |\n",
            )
            errors = lint_capability_disposition_table(root)
            self.assertTrue(
                any("UNBUILT row VRAM_VIEWER names no tracking issue" in error for error in errors),
                errors)
            doc = root / lint_docs.CAPABILITY_MATRIX_DOC
            doc.write_text(
                doc.read_text(encoding="utf-8").replace(
                    "Not implemented yet. |",
                    "In the works: [#314](https://github.com/Jstar269/nakagawa-recomp/issues/314) |"),
                encoding="utf-8",
            )
            self.assertEqual(lint_capability_disposition_table(root), [])

    def test_dropped_row_without_a_reference_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            self._matrix(
                root,
                "| `DASHBOARD` | Localhost web dashboard | DROPPED | none | "
                "The browser UI is gone. |\n",
            )
            errors = lint_capability_disposition_table(root)
        self.assertTrue(
            any("DROPPED row DASHBOARD names no tracking issue" in error for error in errors),
            errors)

    def test_missing_or_empty_table_fails_closed(self) -> None:
        """No table, and an empty table, must both be errors rather than an agreeing set."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            (root / "docs").mkdir()
            doc = root / lint_docs.CAPABILITY_MATRIX_DOC
            doc.write_text("# Native UI Regression Matrix\n\n## 1. Status\n\nNo table yet.\n",
                           encoding="utf-8")
            errors = lint_capability_disposition_table(root)
            self.assertTrue(
                any("must be delimited by exactly one" in error for error in errors), errors)
            self._matrix(root, "")
            errors = lint_capability_disposition_table(root)
            self.assertTrue(any("declares no rows" in error for error in errors), errors)

    def test_wrong_column_count_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            self._matrix(
                root,
                "| `HUD` | Frame overlay | PASS | native runtime |\n",
            )
            errors = lint_capability_disposition_table(root)
        self.assertTrue(any("has 4 columns, not 5" in error for error in errors), errors)


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

    ANCHOR = "RETIRED_PRIVATE_ISSUE_NUMBERS: tuple[int, ...] = ()"

    def _load_with_denylist(self, numbers: str) -> dict:
        source = (ROOT / "tools" / "lint_docs.py").read_text(encoding="utf-8")
        mutated = source.replace(
            self.ANCHOR, f"RETIRED_PRIVATE_ISSUE_NUMBERS: tuple[int, ...] = ({numbers},)", 1)
        self.assertNotEqual(mutated, source, "mutation anchor not found")
        namespace = {"__file__": str(ROOT / "tools" / "lint_docs.py"), "__name__": "x"}
        exec(compile(mutated, "lint_docs", "exec"), namespace)  # noqa: S102
        return namespace

    def test_the_frontier_guard_is_load_bearing(self) -> None:
        """MUTATION: reintroduce a reallocated number and the module must refuse to
        load, rather than silently flagging a live public issue."""
        with self.assertRaises(ValueError) as caught:
            self._load_with_denylist(str(lint_docs.PUBLIC_ISSUE_NUMBER_FRONTIER))
        self.assertIn("reallocated", str(caught.exception))

    def test_a_denylisted_number_is_caught_and_a_live_one_is_not(self) -> None:
        """Every private-era number has been reallocated, so the live denylist is empty.
        The mechanism must still flag a number above the frontier and nothing else."""
        dead = lint_docs.PUBLIC_ISSUE_NUMBER_FRONTIER + 1000
        pattern = self._load_with_denylist(str(dead))["RETIRED_PRIVATE_ISSUE_URLS"]
        self.assertTrue(pattern.search(f"see github.com/Jstar269/nakagawa-recomp/issues/{dead} x"))
        live = f"github.com/Jstar269/nakagawa-recomp/issues/{lint_docs.PUBLIC_ISSUE_NUMBER_FRONTIER}"
        self.assertIsNone(pattern.search(live))

    def test_an_empty_denylist_matches_no_issue_url(self) -> None:
        self.assertEqual(lint_docs.RETIRED_PRIVATE_ISSUE_NUMBERS, ())
        self.assertIsNone(lint_docs.RETIRED_PRIVATE_ISSUE_URLS.search(
            "https://github.com/Jstar269/nakagawa-recomp/issues/301"))


# --- tools/README.md module index -------------------------------------------------
#
# The index is the discoverability contract for tools/: a reader must be able to find
# every top-level module and what it does, and a module that exists but is not indexed
# is invisible to the next contributor. The subpackage list is the same contract one
# level down. Both sets are therefore compared against the tracked tree exactly, in
# both directions.

TOOLS_README = pathlib.Path(__file__).resolve().parent / "README.md"
TOOLS_DIR = "tools"
TOOLS_INDEX_BEGIN = "<!-- tools-index:begin -->"
TOOLS_INDEX_END = "<!-- tools-index:end -->"
TOOLS_SUBPACKAGES_BEGIN = "<!-- tools-subpackages:begin -->"
TOOLS_SUBPACKAGES_END = "<!-- tools-subpackages:end -->"

# An index entry is a list item or a table row inside a delimited section. The first
# backticked ``*.py`` token on such a line is the indexed module; later tokens are
# descriptive cross-references, so a row may mention a sibling tool freely. A
# backticked *path* such as ``nk_core/fonts.py`` matches no token at all rather than
# being read as the bare ``fonts.py``, and the lookbehind rejects a token still glued
# to a path or an identifier. At most three leading spaces count: four spaces or a tab
# opens an indented code block, which documents the row format rather than declaring
# a row, and fenced blocks are examples too, so neither kind of example is an entry.
TOOLS_INDEX_ENTRY_PAT = re.compile(r"^ {0,3}(?:[-*+]\s|\d+[.)]\s|\|)")
TOOLS_MODULE_TOKEN_PAT = re.compile(r"(?<![\w./-])`([A-Za-z0-9_]+\.py)`")
TOOLS_SUBPACKAGE_TOKEN_PAT = re.compile(r"(?<![\w./-])`([A-Za-z0-9_]+)/`")
TOOLS_FENCE_PAT = re.compile(r"^\s*(?:```|~~~)")


def marked_section(readme_text: str, begin: str, end: str, label: str) -> tuple[list[str], int, list[str]]:
    """Return a marker-delimited section's lines, its 1-based first line, and errors.

    Fails closed: a missing, duplicated or inverted marker pair is an error rather
    than an empty section, because an empty section would silently agree with an
    empty inventory.
    """
    errors: list[str] = []
    lines = readme_text.splitlines()
    begins = [i for i, line in enumerate(lines) if line.strip() == begin]
    ends = [i for i, line in enumerate(lines) if line.strip() == end]
    if len(begins) != 1 or len(ends) != 1:
        errors.append(
            f"{TOOLS_README.name}: the {label} must be delimited by exactly one "
            f"'{begin}' / '{end}' pair"
        )
        return [], 0, errors
    if ends[0] <= begins[0]:
        errors.append(f"{TOOLS_README.name}: the {label} end marker appears before its begin marker")
        return [], 0, errors
    return lines[begins[0] + 1:ends[0]], begins[0] + 2, errors


def marked_entries(section_lines: list[str], first_line: int, token_pat: re.Pattern[str]) -> list[tuple[str, int]]:
    """Return the (token, line) pairs a marker-delimited section declares as entries."""
    entries: list[tuple[str, int]] = []
    in_fence = False
    for offset, line in enumerate(section_lines):
        if TOOLS_FENCE_PAT.match(line):
            in_fence = not in_fence
            continue
        if in_fence or not TOOLS_INDEX_ENTRY_PAT.match(line):
            continue
        match = token_pat.search(line)
        if match:
            entries.append((match.group(1), first_line + offset))
    return entries


def parse_tools_index(readme_text: str) -> tuple[list[tuple[str, int]], list[str]]:
    """Return the indexed tool modules (name, line) in file order, plus structural errors.

    Fails closed: an absent, duplicated, inverted or empty index section is reported as
    an error rather than as an empty set, because an empty set would silently agree with
    an empty module list.
    """
    section, first_line, errors = marked_section(
        readme_text, TOOLS_INDEX_BEGIN, TOOLS_INDEX_END, "module index")
    entries = marked_entries(section, first_line, TOOLS_MODULE_TOKEN_PAT)
    if not errors and not entries:
        errors.append(f"{TOOLS_README.name}: the module index section lists no tool modules")
    return entries, errors


def parse_tools_subpackages(readme_text: str) -> tuple[list[tuple[str, int]], list[str]]:
    """Return the listed tool subpackages (name, line) in file order, plus errors.

    An empty list is not an error here: whether the list must be non-empty depends on
    the tracked tree, so :func:`lint_tools_index` decides that.
    """
    section, first_line, errors = marked_section(
        readme_text, TOOLS_SUBPACKAGES_BEGIN, TOOLS_SUBPACKAGES_END, "subpackage list")
    return marked_entries(section, first_line, TOOLS_SUBPACKAGE_TOKEN_PAT), errors


def tracked_tools_paths(repo_root: pathlib.Path) -> set[str]:
    """Return every tracked path under ``tools/``, normalised to POSIX separators.

    Tracked rather than on-disk so a developer's scratch file is not a documentation
    obligation, and read from the index rather than the worktree so a local run before
    staging reports the set hosted CI will see.
    """
    result = run_git(
        ["ls-files", "--", TOOLS_DIR],
        cwd=repo_root,
        capture_output=True,
        text=True,
        check=True,
    )
    return {
        line.strip().replace("\\", "/")
        for line in result.stdout.splitlines()
        if line.strip()
    }


def tracked_tool_modules(repo_root: pathlib.Path) -> set[str]:
    """Return the tracked top-level ``tools/*.py`` module names.

    Subpackage files are excluded by the depth filter and are the subject of
    :func:`tracked_tool_subpackages` instead.
    """
    return {
        PurePosixPath(path).name
        for path in tracked_tools_paths(repo_root)
        if path.count("/") == 1 and path.endswith(".py")
    }


def tracked_tool_subpackages(repo_root: pathlib.Path) -> set[str]:
    """Return the names of the tracked ``tools/`` subdirectories.

    Derived from every tracked file at any depth rather than from ``*.py``, so a
    subpackage holding only data or only scripts (``tools/psp_oracle/manifest.json``,
    ``tools/ghidra_scripts/*.java``) is still a subpackage a reader must be able to
    find. Only the top-level directory is named, which is what the README lists.
    """
    return {
        PurePosixPath(path).parts[1]
        for path in tracked_tools_paths(repo_root)
        if path.count("/") >= 2
    }


def lint_tools_index(repo_root: pathlib.Path = ROOT) -> list[str]:
    """Every tracked top-level tools module and tools/ subdirectory must be listed once."""
    readme = repo_root / TOOLS_DIR / "README.md"
    if not readme.is_file():
        return [f"{TOOLS_DIR}/README.md is missing; the tool module index cannot be verified"]

    readme_text = readme.read_text(encoding="utf-8")
    entries, errors = parse_tools_index(readme_text)
    subpackages, subpackage_errors = parse_tools_subpackages(readme_text)
    errors.extend(subpackage_errors)
    try:
        modules = tracked_tool_modules(repo_root)
        tracked_subpackages = tracked_tool_subpackages(repo_root)
    except (OSError, subprocess.SubprocessError) as error:
        errors.append(
            f"{TOOLS_DIR}/README.md: tracked {TOOLS_DIR} modules could not be enumerated "
            f"({error}); the module index is unverified"
        )
        return errors
    if not modules:
        errors.append(
            f"{TOOLS_DIR}/README.md: no tracked {TOOLS_DIR} modules were found; the module index is unverified"
        )
        return errors

    first_line: dict[str, int] = {}
    for name, lineno in entries:
        if name in first_line:
            errors.append(
                f"{TOOLS_DIR}/README.md:{lineno}: {name} is indexed more than once "
                f"(first at line {first_line[name]})"
            )
            continue
        first_line[name] = lineno
    for name in sorted(modules - set(first_line)):
        errors.append(
            f"{TOOLS_DIR}/README.md: tracked module {TOOLS_DIR}/{name} is not indexed in the "
            "tool module index; add it to the marked index section"
        )
    for name in sorted(set(first_line) - modules):
        errors.append(
            f"{TOOLS_DIR}/README.md:{first_line[name]}: indexed module {name} is not a tracked "
            f"top-level {TOOLS_DIR} module"
        )

    listed_subpackages: dict[str, int] = {}
    for name, lineno in subpackages:
        if name in listed_subpackages:
            errors.append(
                f"{TOOLS_DIR}/README.md:{lineno}: subpackage {name}/ is listed more than once "
                f"(first at line {listed_subpackages[name]})"
            )
            continue
        listed_subpackages[name] = lineno
    # Only a tracked subdirectory makes an empty list a defect: a tools/ with no
    # subdirectory at all is correctly documented by an empty section.
    if tracked_subpackages and not listed_subpackages:
        errors.append(
            f"{TOOLS_DIR}/README.md: the tool subpackage list names no {TOOLS_DIR} subdirectory; "
            "restore the marked subpackage section"
        )
        return errors
    for name in sorted(tracked_subpackages - set(listed_subpackages)):
        errors.append(
            f"{TOOLS_DIR}/README.md: tracked subpackage {TOOLS_DIR}/{name}/ is not listed in the "
            "tool subpackage list; add it to the marked subpackage section"
        )
    for name in sorted(set(listed_subpackages) - tracked_subpackages):
        errors.append(
            f"{TOOLS_DIR}/README.md:{listed_subpackages[name]}: listed subpackage {name}/ is not a "
            f"tracked {TOOLS_DIR} subdirectory"
        )
    return errors


class TestToolsModuleIndex(unittest.TestCase):
    @staticmethod
    def _readme_text(index_body: str, subpackage_body: str = "") -> str:
        """Build a scratch tools/README.md with both marked sections."""
        return (
            f"# tools\n\n{TOOLS_INDEX_BEGIN}\n\n{index_body}\n{TOOLS_INDEX_END}\n\n"
            f"{TOOLS_SUBPACKAGES_BEGIN}\n{subpackage_body}\n{TOOLS_SUBPACKAGES_END}\n"
        )

    @staticmethod
    def _make_repo(root: pathlib.Path, *relative_paths: str) -> None:
        """Create and stage a scratch repository holding the given tools/ files."""
        for relative_path in relative_paths:
            target = root / relative_path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("# scratch\n", encoding="utf-8")
        run_git(["init", "-q"], cwd=root, check=True)
        run_git(["add", "tools"], cwd=root, check=True)

    def test_repository_tools_index_is_complete(self) -> None:
        errors = lint_tools_index(ROOT)
        self.assertEqual(
            errors,
            [],
            "tools/README.md module index does not match the tracked tools modules:\n"
            + "\n".join(errors),
        )

    def test_new_module_without_an_index_row_is_reported(self) -> None:
        """A module that exists but is unindexed is undiscoverable, so it must fail closed."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            tools = root / TOOLS_DIR
            tools.mkdir()
            (tools / "alpha.py").write_text("ALPHA = 1\n", encoding="utf-8")
            (tools / "beta.py").write_text("BETA = 1\n", encoding="utf-8")
            (tools / "nk_core").mkdir()
            (tools / "nk_core" / "gamma.py").write_text("GAMMA = 1\n", encoding="utf-8")
            run_git(["init", "-q"], cwd=root, check=True)
            run_git(["add", "tools"], cwd=root, check=True)
            readme = tools / "README.md"
            readme.write_text(
                self._readme_text(
                    "| Module | Purpose |\n| --- | --- |\n| `alpha.py` | does alpha |",
                    "| Subpackage | Purpose |\n| --- | --- |\n| `nk_core/` | the library |",
                ),
                encoding="utf-8",
            )
            self.assertEqual(
                lint_tools_index(root),
                [f"{TOOLS_DIR}/README.md: tracked module {TOOLS_DIR}/beta.py is not indexed in the "
                 "tool module index; add it to the marked index section"],
            )

            # The subpackage module is out of the top-level set, so indexing beta clears
            # the report; the subpackage itself is already listed.
            readme.write_text(
                self._readme_text(
                    "| Module | Purpose |\n| --- | --- |\n"
                    "| `alpha.py` | does alpha, with `beta.py` |\n| `beta.py` | does beta |",
                    "| Subpackage | Purpose |\n| --- | --- |\n| `nk_core/` | the library |",
                ),
                encoding="utf-8",
            )
            self.assertEqual(lint_tools_index(root), [])

    def test_unlisted_subpackage_is_reported(self) -> None:
        """A new tools/<package>/ is as undiscoverable as a new top-level module."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            self._make_repo(
                root,
                f"{TOOLS_DIR}/alpha.py",
                f"{TOOLS_DIR}/nk_core/beta.py",
                f"{TOOLS_DIR}/brandnewpkg/gamma.py",
                f"{TOOLS_DIR}/brandnewpkg/data.json",
            )
            readme = root / TOOLS_DIR / "README.md"
            readme.write_text(
                self._readme_text(
                    "| Module | Purpose |\n| --- | --- |\n| `alpha.py` | does alpha |",
                    "| Subpackage | Purpose |\n| --- | --- |\n| `nk_core/` | the library |",
                ),
                encoding="utf-8",
            )
            self.assertEqual(
                lint_tools_index(root),
                [f"{TOOLS_DIR}/README.md: tracked subpackage {TOOLS_DIR}/brandnewpkg/ is not "
                 "listed in the tool subpackage list; add it to the marked subpackage section"],
            )

            # Listing it, naming a subdirectory that does not exist, and duplicating a
            # row are all reported rather than accepted.
            readme.write_text(
                self._readme_text(
                    "| Module | Purpose |\n| --- | --- |\n| `alpha.py` | does alpha |",
                    "| Subpackage | Purpose |\n| --- | --- |\n| `nk_core/` | the library |\n"
                    "| `brandnewpkg/` | the new package |\n| `ghostpkg/` | does not exist |\n"
                    "| `nk_core/` | again |",
                ),
                encoding="utf-8",
            )
            errors = lint_tools_index(root)
            self.assertTrue(any("subpackage nk_core/ is listed more than once" in error
                                for error in errors), errors)
            self.assertTrue(any("listed subpackage ghostpkg/ is not a tracked tools subdirectory"
                                in error for error in errors), errors)

            # Removing the section is fail-closed rather than an empty, agreeing set.
            readme.write_text(
                f"# tools\n\n{TOOLS_INDEX_BEGIN}\n\n| Module | Purpose |\n| --- | --- |\n"
                "| `alpha.py` | does alpha |\n\n" + f"{TOOLS_INDEX_END}\n",
                encoding="utf-8",
            )
            errors = lint_tools_index(root)
            self.assertTrue(any("subpackage list must be delimited by exactly one" in error
                                for error in errors), errors)
            self.assertTrue(any("names no tools subdirectory" in error
                                for error in errors), errors)

    def test_stale_row_and_duplicate_row_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            self._make_repo(root, f"{TOOLS_DIR}/alpha.py")
            readme = root / TOOLS_DIR / "README.md"
            readme.write_text(
                self._readme_text(
                    "| Module | Purpose |\n| --- | --- |\n| `alpha.py` | does alpha |\n"
                    "| `alpha.py` | again |\n| `retired.py` | gone |"),
                encoding="utf-8",
            )
            errors = lint_tools_index(root)
        self.assertTrue(any("alpha.py is indexed more than once" in error for error in errors))
        self.assertTrue(any("retired.py is not a tracked top-level tools module" in error
                            for error in errors))

    def test_indented_code_block_is_not_an_index_entry(self) -> None:
        """Documenting the row format must not become a row: four spaces open a code block."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            self._make_repo(root, f"{TOOLS_DIR}/alpha.py")
            readme = root / TOOLS_DIR / "README.md"
            readme.write_text(
                self._readme_text("A row looks like this:\n\n    | `alpha.py` | does alpha |"),
                encoding="utf-8",
            )
            self.assertEqual(
                [error for error in lint_tools_index(root) if "lists no tool modules" in error],
                [f"{TOOLS_README.name}: the module index section lists no tool modules"],
            )

            # A documented example alongside the real row is not a duplicate either.
            readme.write_text(
                self._readme_text(
                    "A row looks like this:\n\n    | `alpha.py` | example row |\n\n"
                    "| Module | Purpose |\n| --- | --- |\n| `alpha.py` | does alpha |"),
                encoding="utf-8",
            )
            self.assertEqual(lint_tools_index(root), [])

    def test_missing_or_empty_index_section_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            self._make_repo(root, f"{TOOLS_DIR}/alpha.py")
            readme = root / TOOLS_DIR / "README.md"
            readme.write_text("# tools\n\n`alpha.py` is mentioned in prose only.\n", encoding="utf-8")
            self.assertTrue(any("must be delimited by exactly one" in error
                                for error in lint_tools_index(root)))
            readme.write_text(
                self._readme_text("Prose mentions `alpha.py`, and this fenced\nexample is "
                                  "documentation, not an entry:\n\n```text\n"
                                  "| `alpha.py` | example row |\n```"),
                encoding="utf-8",
            )
            self.assertEqual(
                [error for error in lint_tools_index(root) if "lists no tool modules" in error],
                [f"{TOOLS_README.name}: the module index section lists no tool modules"],
            )
