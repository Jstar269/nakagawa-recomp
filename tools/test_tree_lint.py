# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Offline deterministic tests for tools/tree_lint.py.

Every test builds a throwaway repository with ``git init`` inside a temporary
directory and stages synthetic files.  No environment variable selects the
repository: the scratch repository is reached by ``cwd`` alone, so a test can
never read or write the tree it was launched from.
"""

import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools import tree_lint
from tools.nk_core.git_isolation import run_git

ALLOWLIST = ROOT / "tools" / "tree_lint_allow.json"


class TreeLintTests(unittest.TestCase):
    """One synthetic repository per test; every file in it is written here."""

    def test_allowlist_wildcards_stay_inside_one_path_component(self) -> None:
        allowed = {
            "tools/*/__init__.py": "package marker",
            "tools/test_*.py": "unittest discovery",
            "Makefile": "build entry point",
        }
        match = tree_lint._allowing_pattern
        self.assertEqual(match("tools/pkg/__init__.py", allowed), "tools/*/__init__.py")
        self.assertEqual(match("tools/test_x.py", allowed), "tools/test_*.py")
        self.assertIsNone(match("tools/a/b/__init__.py", allowed))
        self.assertIsNone(match("tools/sub/test_x.py", allowed))
        self.assertIsNone(match("makefile", allowed))
        self.assertEqual(match("Makefile", allowed), "Makefile")

    def _repo(self, files: dict[str, str]) -> tempfile.TemporaryDirectory:
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        root = pathlib.Path(holder.name)
        run_git(["init", "-q"], cwd=root, check=True)
        self._write(root, files)
        run_git(["add", "-A", "."], cwd=root, check=True)
        return holder

    def _write(self, root: pathlib.Path, files: dict[str, str]) -> None:
        for relative, text in files.items():
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")

    def _statuses(self, files: dict[str, str], allowlist: pathlib.Path | None = None) -> dict[str, str]:
        holder = self._repo(files)
        root = pathlib.Path(holder.name)
        tracked = tree_lint.tracked_files(root)
        allowed = tree_lint.load_allowlist(allowlist) if allowlist else {}
        return {entry["path"]: str(entry["status"]) for entry in tree_lint.unreferenced(root, tracked, allowed)}

    def _findings(self, files: dict[str, str], allowlist: pathlib.Path) -> list[dict[str, object]]:
        holder = self._repo(files)
        root = pathlib.Path(holder.name)
        tracked = tree_lint.tracked_files(root)
        return tree_lint.unreferenced(root, tracked, tree_lint.load_allowlist(allowlist))

    def _absent_allowlist(self) -> pathlib.Path:
        """A path that does not exist, so a test never depends on the shipped allowlist."""
        return pathlib.Path(self.enterContext(tempfile.TemporaryDirectory())) / "absent.json"

    def _run(self, argv: list[str]) -> tuple[int, str]:
        """Run the CLI and capture its report, so the suite output stays readable."""
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            status = tree_lint.main(argv)
        return status, buffer.getvalue()

    def _allowlist(self, entries: list[dict[str, str]]) -> pathlib.Path:
        path = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory())) / "allow.json"
        path.write_text(json.dumps({"schema_version": 1, "entries": entries}), encoding="utf-8")
        return path

    def test_unreferenced_file_is_reported(self) -> None:
        statuses = self._statuses({
            "index.md": "run helper.py\n",
            "helper.py": "print('helper')\n",
            "src/orphan.c": "int orphan(void) { return 0; }\n",
        })
        self.assertEqual(statuses.get("src/orphan.c"), tree_lint.UNREFERENCED)
        self.assertNotIn("helper.py", statuses)

    def test_file_named_by_its_full_path_is_not_reported(self) -> None:
        statuses = self._statuses({
            "index.md": "compile src/live.c\n",
            "src/live.c": "int live(void) { return 0; }\n",
        })
        self.assertNotIn("src/live.c", statuses)

    def test_file_named_by_its_basename_is_not_reported(self) -> None:
        statuses = self._statuses({
            "docs/PIPELINE.md": "run `python tools/codegen.py` first\n",
            "tools/codegen.py": "print('translate')\n",
        })
        self.assertNotIn("tools/codegen.py", statuses)

    def test_file_named_by_its_dotted_module_name_is_not_reported(self) -> None:
        statuses = self._statuses({
            "tools/test_parser.py": "from tools.parser import parse\n",
            "tools/parser.py": "def parse():\n    return 1\n",
        })
        self.assertNotIn("tools/parser.py", statuses)

    def test_file_named_by_a_relative_suffix_is_not_reported(self) -> None:
        statuses = self._statuses({
            "docs/README.md": "see [census](provenance/HST_PUBLIC_CENSUS.md)\n",
            "docs/provenance/HST_PUBLIC_CENSUS.md": "census\n",
        })
        self.assertNotIn("docs/provenance/HST_PUBLIC_CENSUS.md", statuses)

    def test_file_named_with_sentence_period_is_not_reported(self) -> None:
        statuses = self._statuses({
            "docs/PIPELINE.md": "Run tools/codegen.py. Then proceed.\nSee `tools/helper.py.`\n",
            "tools/codegen.py": "print('translate')\n",
            "tools/helper.py": "print('helper')\n",
        })
        self.assertNotIn("tools/codegen.py", statuses)
        self.assertNotIn("tools/helper.py", statuses)

    def test_a_name_shared_by_several_files_is_its_own_class(self) -> None:
        holder = self._repo({
            "index.md": "see Makefile for the build\n",
            "fixtures/one/Makefile": "all:\n\t@echo one\n",
            "fixtures/two/Makefile": "all:\n\t@echo two\n",
        })
        root = pathlib.Path(holder.name)
        findings = tree_lint.unreferenced(root, tree_lint.tracked_files(root), {})
        self.assertEqual(
            {entry["path"]: entry["status"] for entry in findings if entry["path"].endswith("Makefile")},
            {"fixtures/one/Makefile": tree_lint.AMBIGUOUS, "fixtures/two/Makefile": tree_lint.AMBIGUOUS},
        )
        self.assertEqual(
            [entry["referenced_by"] for entry in findings if entry["path"] == "fixtures/one/Makefile"],
            [["index.md"]],
        )

    def test_a_file_does_not_reference_itself(self) -> None:
        statuses = self._statuses({"tools/lonely.py": "LONELY = 'tools/lonely.py'\n"})
        self.assertEqual(statuses, {"tools/lonely.py": tree_lint.UNREFERENCED})

    def test_generated_inventories_are_not_sources_of_references(self) -> None:
        inventory = json.dumps({"include_paths": ["src/orphan.c", "tools/codegen.py"]})
        statuses = self._statuses({
            "assets/public_source_profile.json": inventory,
            "assets/public_provenance_ledger.json": inventory,
            "PUBLIC_EXPORT.json": inventory,
            "docs/provenance/MODIFIED_FILE_NOTICES.json": inventory,
            "src/orphan.c": "int orphan(void) { return 0; }\n",
            "tools/codegen.py": "print('translate')\n",
        })
        self.assertEqual(statuses.get("src/orphan.c"), tree_lint.UNREFERENCED)
        self.assertEqual(statuses.get("tools/codegen.py"), tree_lint.UNREFERENCED)
        # Each inventory is still judged on its own merit: nothing names it either.
        for inventory_path in tree_lint.GENERATED_INVENTORIES:
            self.assertEqual(statuses.get(inventory_path), tree_lint.UNREFERENCED, inventory_path)

    def test_allowlisted_file_is_reported_as_a_finding_with_its_reason(self) -> None:
        allowlist = self._allowlist([{"path": "src/orphan.c", "reason": "consumed by an external tool"}])
        findings = self._findings({"src/orphan.c": "int orphan(void) { return 0; }\n"}, allowlist)
        self.assertEqual([entry["path"] for entry in findings], ["src/orphan.c"])
        self.assertTrue(findings[0]["allowlisted"])
        self.assertEqual(findings[0]["reason"], "consumed by an external tool")

    def test_allowlist_pattern_covers_a_whole_family(self) -> None:
        allowlist = self._allowlist([{"path": "tools/test_*.py", "reason": "consumed by a discovery glob"}])
        findings = self._findings({
            "tools/test_one.py": "import unittest\n",
            "tools/test_two.py": "import unittest\n",
            "src/orphan.c": "int orphan(void) { return 0; }\n",
        }, allowlist)
        self.assertEqual(
            {entry["path"]: bool(entry["allowlisted"]) for entry in findings},
            {"tools/test_one.py": True, "tools/test_two.py": True, "src/orphan.c": False},
        )

    def test_a_tracked_allowlist_is_not_a_source_of_its_own_references(self) -> None:
        """A committed allowlist must not make its own entries look used.

        ``tools/tree_lint_allow.json`` is tracked in the real tree, so this models
        the post-commit state: every file here is staged, and the only file that
        spells ``.github/CODEOWNERS`` is the allowlist that excuses it.
        """
        entries = [
            {"path": ".github/CODEOWNERS", "reason": "GitHub resolves review ownership by this filename"},
            {"path": "docs/TREE.md", "reason": "the synthetic tree's own index"},
        ]
        holder = self._repo({
            "docs/TREE.md": "The tracked-file index; the reviewed allowlist is `tools/tree_lint_allow.json`.\n",
            ".github/CODEOWNERS": "* @nakagawa/maintainers\n",
            "tools/tree_lint_allow.json": json.dumps({"schema_version": 1, "entries": entries}),
        })
        root = pathlib.Path(holder.name)
        tracked = tree_lint.tracked_files(root)
        self.assertIn("tools/tree_lint_allow.json", tracked, "this tree models the allowlist as tracked")

        scanned = tree_lint.build_reference_index(root, tracked)
        self.assertNotIn(".github/CODEOWNERS", scanned, "the allowlist named it, not a user of the tree")
        self.assertEqual(
            tree_lint.build_reference_index(root, tracked, ignored_sources={"tools/tree_lint_allow.json"})
            [".github/CODEOWNERS"]["status"],
            tree_lint.UNREFERENCED,
        )

        # End to end: the allowlist still excuses both files, so --check passes and
        # each entry is still printed with its reason.
        status, report = self._run([
            "--root", str(root), "--allowlist", str(root / "tools/tree_lint_allow.json"), "--check",
        ])
        self.assertEqual(status, 0, report)
        self.assertIn("allowlisted by .github/CODEOWNERS: 1 file(s)", report)
        self.assertIn("GitHub resolves review ownership by this filename", report)

    def test_malformed_allowlist_fails_closed(self) -> None:
        allowlist = self._allowlist([{"path": "src/orphan.c"}])
        with self.assertRaises(tree_lint.TreeLintError):
            tree_lint.load_allowlist(allowlist)

    def test_allowlist_document_and_entries_are_strictly_validated(self) -> None:
        invalid_documents = [
            {"schema_version": True, "entries": []},
            {"schema_version": 1, "entries": [], "unexpected": "ignored?"},
            {"schema_version": 1, "entries": [{"path": "src/orphan.c", "reason": "reviewed", "extra": True}]},
            {"schema_version": 1, "entries": [{"path": "C:/outside.c", "reason": "reviewed"}]},
            {"schema_version": 1, "entries": [{"path": "src/../outside.c", "reason": "reviewed"}]},
            {"schema_version": 1, "entries": [{"path": "src/*.c?", "reason": "reviewed"}]},
            {"schema_version": 1, "entries": [{"path": "src/orphan.c", "reason": "first line\nsecond line"}]},
            {"schema_version": 1, "entries": [{"path": "src/orphan.c", "reason": "   "}]},
            {"schema_version": 1, "entries": [
                {"path": "src/orphan.c", "reason": "first"},
                {"path": "src/orphan.c", "reason": "duplicate"},
            ]},
        ]
        for document in invalid_documents:
            with self.subTest(document=document):
                allowlist = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory())) / "allow.json"
                allowlist.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaises(tree_lint.TreeLintError):
                    tree_lint.load_allowlist(allowlist)

    def test_a_binary_tracked_file_is_not_read_as_text(self) -> None:
        holder = self._repo({"index.md": "see assets/table.dat\n", "src/live.c": "int live(void) { return 0; }\n"})
        root = pathlib.Path(holder.name)
        (root / "assets").mkdir(parents=True, exist_ok=True)
        (root / "assets" / "table.dat").write_bytes(b"\x00\x01src/live.c\x00")
        run_git(["add", "-A", "."], cwd=root, check=True)
        tracked = tree_lint.tracked_files(root)
        self.assertIn("assets/table.dat", tracked)
        index = tree_lint.build_reference_index(root, tracked)
        self.assertNotIn("assets/table.dat", index, "index.md names it by path")
        self.assertEqual(index.get("src/live.c", {}).get("status"), tree_lint.UNREFERENCED)

    def test_a_tracked_symlink_target_is_not_scanned(self) -> None:
        holder = self._repo({"src/orphan.c": "int orphan(void) { return 0; }\n"})
        root = pathlib.Path(holder.name)
        outside = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory())) / "outside.txt"
        outside.write_text("src/orphan.c\n", encoding="utf-8")
        try:
            (root / "external.py").symlink_to(outside)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"symlink creation is unavailable: {error}")
        run_git(["add", "external.py"], cwd=root, check=True)

        statuses = {
            entry["path"]: entry["status"]
            for entry in tree_lint.unreferenced(root, tree_lint.tracked_files(root), {})
        }
        self.assertEqual(statuses["src/orphan.c"], tree_lint.UNREFERENCED)

    def test_a_symlink_allowlist_is_rejected_before_reading_its_target(self) -> None:
        target = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory())) / "allow.json"
        target.write_text('{"schema_version":1,"entries":[]}', encoding="utf-8")
        link = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory())) / "allow.json"
        try:
            link.symlink_to(target)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"symlink creation is unavailable: {error}")
        with self.assertRaisesRegex(tree_lint.TreeLintError, "must not be a symlink"):
            tree_lint.load_allowlist(link)

    def test_reporting_exits_zero_and_check_exits_one_on_a_finding(self) -> None:
        dirty = self._repo({"src/orphan.c": "int orphan(void) { return 0; }\n"}).name
        absent = self._absent_allowlist()
        self.assertEqual(self._run(["--root", dirty, "--allowlist", str(absent)])[0], 0)
        status, report = self._run(["--root", dirty, "--allowlist", str(absent), "--check"])
        self.assertEqual(status, 1)
        self.assertIn("tree-lint: FAIL", report)

    def test_check_exits_zero_when_the_allowlist_explains_the_finding(self) -> None:
        allowlist = self._allowlist([{"path": "src/orphan.c", "reason": "consumed by an external tool"}])
        dirty = self._repo({"src/orphan.c": "int orphan(void) { return 0; }\n"}).name
        self.assertEqual(self._run(["--root", dirty, "--allowlist", str(allowlist), "--check"])[0], 0)

    def test_check_exits_zero_when_nothing_lacks_a_reference_or_a_reason(self) -> None:
        allowlist = self._allowlist([{"path": "index.md", "reason": "the synthetic tree's own entry point"}])
        clean = self._repo({"index.md": "compile src/live.c\n", "src/live.c": "int live(void) { return 0; }\n"}).name
        status, report = self._run(["--root", clean, "--allowlist", str(allowlist), "--check"])
        self.assertEqual(status, 0, report)
        self.assertIn("tree-lint: OK (--check)", report)

    def test_json_output_names_the_status_of_each_finding(self) -> None:
        holder = self._repo({
            "index.md": "see Makefile for the build\n",
            "fixtures/one/Makefile": "all:\n\t@echo one\n",
            "fixtures/two/Makefile": "all:\n\t@echo two\n",
            "src/orphan.c": "int orphan(void) { return 0; }\n",
        })
        _, report = self._run(["--root", holder.name, "--allowlist", str(self._absent_allowlist()), "--json"])
        document = json.loads(report)
        self.assertEqual(document["tracked"], 4)
        self.assertEqual(
            document["unreferenced"],
            [
                {"path": "fixtures/one/Makefile", "status": tree_lint.AMBIGUOUS},
                {"path": "fixtures/two/Makefile", "status": tree_lint.AMBIGUOUS},
                {"path": "index.md", "status": tree_lint.UNREFERENCED},
                {"path": "src/orphan.c", "status": tree_lint.UNREFERENCED},
            ],
        )

    def test_the_allowlist_is_printed_as_a_grouped_decision(self) -> None:
        allowlist = self._allowlist([
            {"path": "tools/test_*.py", "reason": "consumed by a discovery glob"},
            {"path": "index.md", "reason": "the synthetic tree's own entry point"},
        ])
        holder = self._repo({
            "index.md": "compile src/live.c\n",
            "src/live.c": "int live(void) { return 0; }\n",
            "tools/test_one.py": "import unittest\n",
            "tools/test_two.py": "import unittest\n",
        })
        _, report = self._run(["--root", holder.name, "--allowlist", str(allowlist)])
        self.assertIn("allowlisted by tools/test_*.py: 2 file(s)", report)
        self.assertIn("allowlisted by index.md: 1 file(s)", report)
        self.assertIn("consumed by a discovery glob", report)
        self.assertIn("no un-allowlisted file lacks an unambiguous reference", report)

    def test_an_unreadable_tree_is_an_error_not_an_empty_report(self) -> None:
        absent = self.enterContext(tempfile.TemporaryDirectory())
        with contextlib.redirect_stderr(io.StringIO()):
            status = tree_lint.main(["--root", absent, "--allowlist", str(self._absent_allowlist())])
        self.assertEqual(status, 2)

    def test_the_shipped_allowlist_gives_every_entry_a_reason(self) -> None:
        allowed = tree_lint.load_allowlist(ALLOWLIST)
        self.assertTrue(allowed)
        for pattern, reason in allowed.items():
            self.assertTrue(reason.strip(), f"{pattern} needs a reason")
        self.assertTrue(any("*" in pattern for pattern in allowed), "the discovery glob family must be recorded")


if __name__ == "__main__":
    unittest.main()
