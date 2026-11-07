# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The append-only merge driver and the files .gitattributes hands to it.

Two pull requests that each add a self-test block at the same place in ``mk/selftests.mk`` conflict
in plain Git. ``tools/merge_append.py`` merges them when both sides only inserted lines, and keeps the
ordinary conflict when either side changed or removed one. These tests run the driver on real Git
repositories, so the attribute, the driver command in ``docs/CI.md`` and the result are the real ones,
and they pin which files may use it.
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import merge_append  # noqa: E402

DRIVER = "python tools/merge_append.py %O %A %B %L %P"  # a contributor's own clone
DESK_DRIVER = "python -I /path/to/reviewed/merge_append.py %O %A %B %L %P"  # the landing desk's reviewed copy
# Files whose conflicts are independent items. Prose, code and generated files are not allowed.
ALLOWED = set(merge_append.ALLOWED_PATHS)
FORBIDDEN_SUFFIXES = {".c", ".h", ".inc", ".py", ".json", ".yml", ".yaml", ".sh", ".ps1", ".cpp", ".kts"}


def _git(repo: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(
        ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "core.autocrlf=false", *args],
        cwd=repo, capture_output=True, text=True, check=False,
    )
    if check and proc.returncode != 0:
        raise AssertionError(f"git {' '.join(args)} failed: {proc.stderr}{proc.stdout}")
    return proc


class AttributeListTests(unittest.TestCase):
    def test_nk_append_files_are_the_reviewed_list(self) -> None:
        text = (ROOT / ".gitattributes").read_text(encoding="utf-8")
        listed = set(re.findall(r"(?m)^(\S+)\s+merge=nk-append\s*$", text))
        self.assertEqual(listed, ALLOWED, "the attribute list and ALLOWED_PATHS in merge_append.py must agree")
        self.assertEqual(ALLOWED, {"mk/selftests.mk", "mk/public_targets.mk", "mk/runtime_sources.mk", "CMakeLists.txt", "docs/DEBUGGING.md"})
        self.assertNotIn("merge=union", text, "plain union keeps both versions of an edited line silently")
        for path in listed:
            self.assertTrue((ROOT / path).is_file(), f"{path} is listed for the merge driver but does not exist")
            self.assertNotIn(Path(path).suffix, FORBIDDEN_SUFFIXES, f"{path}: code and data never use the driver")

    def test_the_enhancement_contract_stays_out(self) -> None:
        # Most of its conflicts edit existing lines, and its additions are numbered sections that
        # two pull requests would number alike (docs/CI.md, "Merging parallel additions").
        self.assertNotIn("docs/ENHANCEMENT_CONTRACT.md", ALLOWED)
        self.assertIn("`docs/ENHANCEMENT_CONTRACT.md` conflicts often too, but it stays out.",
                      (ROOT / "docs" / "CI.md").read_text(encoding="utf-8"))

    def test_docs_give_the_driver_command_the_tests_run(self) -> None:
        text = (ROOT / "docs" / "CI.md").read_text(encoding="utf-8")
        self.assertIn(f'git config merge.nk-append.driver "{DRIVER}"', text)
        self.assertIn(DESK_DRIVER, text, "the desk must be told to run a reviewed copy outside every worktree")
        self.assertIn("outside every worktree", text)

    def test_public_target_lists_stay_one_item_per_line(self) -> None:
        text = (ROOT / "mk" / "public_targets.mk").read_text(encoding="utf-8")
        block = re.search(r"(?ms)^PUBLIC_TARGETS := \\\n(.*?)^\n", text)
        self.assertIsNotNone(block)
        lines = [l for l in block.group(1).splitlines()]
        for line in lines[:-1]:
            self.assertRegex(line, r"^\t[A-Za-z0-9_.%/+?-]+ \\$", "one target per line, each ending in a backslash")
        self.assertRegex(lines[-1], r"^\t[A-Za-z0-9_.%/+?-]+$", "the list ends on its last target")
        helps = re.findall(r"(?m)^HELP_DESCRIPTION_\S+ :=.*$", text)
        self.assertGreater(len(helps), 100)
        for line in helps:
            self.assertNotIn("\\", line[-1:], "a help description is one line, with no continuation")


class DriverUnitTests(unittest.TestCase):
    def _merge(self, base: str, ours: str, theirs: str, path: str = "mk/selftests.mk") -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            for name, body in (("base", base), ("cur", ours), ("other", theirs)):
                (d / name).write_bytes(body.encode())
            rc = merge_append.merge(d / "base", d / "cur", d / "other", 7, path)
            return rc, (d / "cur").read_text()

    def test_both_sides_insert_at_one_spot_keeps_both(self) -> None:
        rc, out = self._merge("a\nb\n", "a\nours1\nours2\nb\n", "a\ntheirs1\nb\n")
        self.assertEqual((rc, out), (0, "a\nours1\nours2\ntheirs1\nb\n"))

    def test_two_separate_spots_both_union(self) -> None:
        base = "a\nb\nc\nd\n"
        rc, out = self._merge(base, "a\nO1\nb\nc\nO2\nd\n", "a\nT1\nb\nc\nT2\nd\n")
        self.assertEqual((rc, out), (0, "a\nO1\nT1\nb\nc\nO2\nT2\nd\n"))

    def test_both_sides_edit_one_line_stays_a_conflict(self) -> None:
        rc, out = self._merge("a\nold\nb\n", "a\nours\nb\n", "a\ntheirs\nb\n")
        self.assertEqual(rc, 1)
        self.assertIn("<<<<<<<", out)
        self.assertIn("ours", out)
        self.assertIn("theirs", out)
        self.assertNotIn("|||||||", out, "the ordinary conflict style, not diff3")

    def test_edit_against_insert_beside_it_is_decided_by_git(self) -> None:
        # Git merges an edit and an insert on adjacent lines, or conflicts. Either way there is no union.
        rc, out = self._merge("a\nold\nb\n", "a\nnew\nb\n", "a\nold\nadded\nb\n")
        self.assertEqual(rc, 1)
        self.assertIn("<<<<<<<", out)

    def test_one_conflict_with_an_edit_blocks_the_other_pure_insert(self) -> None:
        base = "a\nold\nb\nc\nd\n"
        rc, out = self._merge(base, "a\nours\nb\nc\nO\nd\n", "a\ntheirs\nb\nc\nT\nd\n")
        self.assertEqual(rc, 1)
        self.assertIn("<<<<<<<", out)

    def test_clean_merge_is_unchanged(self) -> None:
        rc, out = self._merge("a\nb\nc\n", "A\nb\nc\n", "a\nb\nC\n")
        self.assertEqual((rc, out), (0, "A\nb\nC\n"))

    def test_deleted_line_against_insert_beside_it_is_a_conflict(self) -> None:
        rc, out = self._merge("a\nold\nb\n", "a\nb\n", "a\nold\nnew\nb\n")
        self.assertEqual(rc, 1)
        self.assertIn("<<<<<<<", out)

    def test_inserts_sharing_a_line_stay_a_conflict(self) -> None:
        for name, ours, theirs in (
            ("X against X,Y", "a\nX\nz\n", "a\nX\nY\nz\n"),
            ("shared first line", "a\nSHARED\nA\nz\n", "a\nSHARED\nB\nz\n"),
            ("shared last line", "a\nA\nSHARED\nz\n", "a\nB\nSHARED\nz\n"),
            ("shared middle line", "a\nA\nSHARED\nA2\nz\n", "a\nB\nSHARED\nB2\nz\n"),
        ):
            rc, out = self._merge("a\nz\n", ours, theirs)
            self.assertEqual(rc, 1, f"{name}: {out!r}")
            self.assertIn("<<<<<<<", out, name)

    def test_blank_lines_may_repeat(self) -> None:
        rc, out = self._merge("a\nz\n", "a\n\nA\nz\n", "a\n\nB\nz\n")
        self.assertEqual((rc, out), (0, "a\n\nA\n\nB\nz\n"))

    def test_recipe_lines_inserted_in_a_recipe_stay_a_conflict(self) -> None:
        base = "t:\n\tone\n\nnext:\n"
        rc, out = self._merge(base, "t:\n\tone\n\ttwo\n\nnext:\n", "t:\n\tone\n\tthree\n\nnext:\n")
        self.assertEqual(rc, 1)
        self.assertIn("<<<<<<<", out)

    def test_tab_indented_list_items_still_merge(self) -> None:
        base = "LIST := \\\n\tone \\\n\ttwo\n"
        ours = "LIST := \\\n\tone \\\n\tA \\\n\ttwo\n"
        theirs = "LIST := \\\n\tone \\\n\tB \\\n\ttwo\n"
        rc, out = self._merge(base, ours, theirs)
        self.assertEqual((rc, out), (0, "LIST := \\\n\tone \\\n\tA \\\n\tB \\\n\ttwo\n"))

    def test_new_targets_between_targets_merge(self) -> None:
        base = "a:\n\tx\n\nz:\n\tw\n"
        rc, out = self._merge(base, "a:\n\tx\n\nb:\n\ty\n\nz:\n\tw\n", "a:\n\tx\n\nc:\n\tq\n\nz:\n\tw\n")
        self.assertEqual((rc, out), (0, "a:\n\tx\n\nb:\n\ty\n\nc:\n\tq\n\nz:\n\tw\n"))

    def test_a_path_outside_the_allowlist_is_never_unioned(self) -> None:
        for path in ("src/rt/ge.c", "tools/codegen.py", "docs/CI.md", "", "../mk/selftests.mk"):
            rc, out = self._merge("a\nb\n", "a\nours\nb\n", "a\ntheirs\nb\n", path)
            self.assertEqual(rc, 1, path)
            self.assertIn("<<<<<<<", out)

    def test_windows_separators_in_the_path_are_accepted(self) -> None:
        rc, _ = self._merge("a\nb\n", "a\nours\nb\n", "a\ntheirs\nb\n", "mk\\selftests.mk")
        self.assertEqual(rc, 0)


class RealMergeTests(unittest.TestCase):
    """Run through Git itself: the attribute, the configured driver and a merge of two branches."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name) / "repo"
        (self.repo / "tools").mkdir(parents=True)
        (self.repo / "mk").mkdir()
        _git(self.repo, "init", "-q", "-b", "main")
        (self.repo / "tools" / "merge_append.py").write_bytes((ROOT / "tools" / "merge_append.py").read_bytes())
        (self.repo / ".gitattributes").write_text(
            "mk/selftests.mk merge=nk-append\nplain.txt merge=nk-append\nother.txt merge=nk-append\n", encoding="utf-8")
        (self.repo / "mk" / "selftests.mk").write_text(
            "alpha-selftest:\n\t@echo alpha\n\nnative-core-tests: alpha-selftest\n", encoding="utf-8")
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "-m", "base")

    def _enable_driver(self) -> None:
        _git(self.repo, "config", "merge.nk-append.name", "append-only list merge")
        _git(self.repo, "config", "merge.nk-append.driver", DRIVER)

    def _branch_with(self, name: str, text: str) -> None:
        _git(self.repo, "checkout", "-q", "-b", name, "main")
        path = self.repo / "mk" / "selftests.mk"
        path.write_text(path.read_text(encoding="utf-8") + text, encoding="utf-8")
        _git(self.repo, "commit", "-q", "-am", name)

    def _two_branches(self) -> None:
        self._branch_with("pr-a", "\nbeta-selftest:\n\t@echo beta\n\nnative-core-tests: beta-selftest\n")
        self._branch_with("pr-b", "\ngamma-selftest:\n\t@echo gamma\n\nnative-core-tests: gamma-selftest\n")

    def test_without_the_setting_the_merge_conflicts_as_before(self) -> None:
        self._two_branches()
        proc = _git(self.repo, "merge", "--no-edit", "pr-a", check=False)
        self.assertNotEqual(proc.returncode, 0)

    def test_with_the_setting_both_blocks_land(self) -> None:
        self._enable_driver()
        self._two_branches()
        _git(self.repo, "checkout", "-q", "pr-b")
        _git(self.repo, "merge", "--no-edit", "pr-a")
        merged = (self.repo / "mk" / "selftests.mk").read_text(encoding="utf-8")
        self.assertNotIn("<<<<<<<", merged)
        for name in ("alpha", "beta", "gamma"):
            self.assertEqual(merged.count(f"{name}-selftest:\n"), 1)
            self.assertEqual(merged.count(f"native-core-tests: {name}-selftest\n"), 1)

    def test_squash_merge_of_a_batch_uses_the_driver_too(self) -> None:
        self._enable_driver()
        self._two_branches()
        _git(self.repo, "checkout", "-q", "main")
        _git(self.repo, "merge", "--squash", "--no-commit", "pr-a")
        _git(self.repo, "commit", "-q", "-m", "fold pr-a")  # the desk commits each folded PR in turn
        proc = _git(self.repo, "merge", "--squash", "--no-commit", "pr-b", check=False)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(_git(self.repo, "diff", "--name-only", "--diff-filter=U").stdout.strip(), "")
        merged = (self.repo / "mk" / "selftests.mk").read_text(encoding="utf-8")
        self.assertIn("beta-selftest:", merged)
        self.assertIn("gamma-selftest:", merged)

    def test_an_edit_of_the_same_line_still_conflicts(self) -> None:
        self._enable_driver()
        _git(self.repo, "checkout", "-q", "-b", "pr-a", "main")
        path = self.repo / "mk" / "selftests.mk"
        path.write_text(path.read_text(encoding="utf-8").replace("echo alpha", "echo alpha one"), encoding="utf-8")
        _git(self.repo, "commit", "-q", "-am", "a")
        _git(self.repo, "checkout", "-q", "-b", "pr-b", "main")
        path.write_text(path.read_text(encoding="utf-8").replace("echo alpha", "echo alpha two"), encoding="utf-8")
        _git(self.repo, "commit", "-q", "-am", "b")
        proc = _git(self.repo, "merge", "--no-edit", "pr-a", check=False)
        self.assertNotEqual(proc.returncode, 0)
        text = path.read_text(encoding="utf-8")
        self.assertIn("<<<<<<<", text)
        self.assertIn("alpha one", text)
        self.assertIn("alpha two", text)


class RealGitEdgeCaseTests(unittest.TestCase):
    """Byte-level cases through a real `git merge` with the driver configured."""

    PATH = "mk/selftests.mk"

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name) / "repo"
        (self.repo / "tools").mkdir(parents=True)
        (self.repo / "mk").mkdir()
        _git(self.repo, "init", "-q", "-b", "main")
        (self.repo / "tools" / "merge_append.py").write_bytes((ROOT / "tools" / "merge_append.py").read_bytes())
        _git(self.repo, "config", "merge.nk-append.driver", DRIVER)
        _git(self.repo, "config", "core.autocrlf", "false")
        self.attributes = f"{self.PATH} merge=nk-append\n"

    def _run(self, base: bytes, ours: bytes, theirs: bytes, attributes: str | None = None) -> tuple[int, bytes]:
        path = self.repo / self.PATH
        (self.repo / ".gitattributes").write_text(attributes or self.attributes, encoding="utf-8")
        path.write_bytes(base)
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "--allow-empty", "-m", "base")
        for name, body in (("pr-a", ours), ("pr-b", theirs)):
            _git(self.repo, "checkout", "-q", "-b", name, "main")
            path.write_bytes(body)
            _git(self.repo, "commit", "-q", "-am", name)
        _git(self.repo, "checkout", "-q", "pr-b")
        proc = _git(self.repo, "merge", "--no-edit", "pr-a", check=False)
        return proc.returncode, path.read_bytes()

    def test_crlf_lines_union_and_keep_their_endings(self) -> None:
        rc, out = self._run(b"a\r\nz\r\n", b"a\r\nA\r\nz\r\n", b"a\r\nB\r\nz\r\n")
        self.assertEqual((rc, out), (0, b"a\r\nB\r\nA\r\nz\r\n"))  # ours is pr-b, so its line comes first

    def test_a_missing_final_newline_never_joins_lines(self) -> None:
        # pr-b is the branch merged into: "ours" lacks the newline in the second case, "theirs" in the first.
        rc, out = self._run(b"a\nz\n", b"a\nz\nA", b"a\nz\nB\n")
        self.assertEqual((rc, out), (0, b"a\nz\nB\nA\n"))
        self._reset()
        rc, out = self._run(b"a\nz\n", b"a\nz\nA\n", b"a\nz\nB")
        self.assertEqual((rc, out), (0, b"a\nz\nB\nA\n"))

    def test_non_utf8_bytes_survive_a_union(self) -> None:
        rc, out = self._run(b"a\nz\n", b"a\n\xff\xfeOURS\nz\n", b"a\n\xe9THEIRS\nz\n")
        self.assertEqual((rc, out), (0, b"a\n\xe9THEIRS\n\xff\xfeOURS\nz\n"))

    def test_a_conflict_marker_size_other_than_7(self) -> None:
        attributes = f"{self.PATH} merge=nk-append conflict-marker-size=9\n"
        rc, out = self._run(b"a\nold\nz\n", b"a\nours\nz\n", b"a\ntheirs\nz\n", attributes)
        self.assertNotEqual(rc, 0)
        self.assertIn(b"<" * 9 + b" ", out)
        self.assertNotIn(b"<" * 10, out)
        rc, out = self._run_second_union(attributes)
        self.assertEqual(rc, 0)
        self.assertNotIn(b"<<<<<<<<<", out)

    def _run_second_union(self, attributes: str) -> tuple[int, bytes]:
        _git(self.repo, "merge", "--abort")
        path = self.repo / self.PATH
        _git(self.repo, "checkout", "-q", "main")
        for name, body in (("pr-c", b"a\nold\nz\nC\n"), ("pr-d", b"a\nold\nz\nD\n")):
            _git(self.repo, "checkout", "-q", "-b", name, "main")
            path.write_bytes(body)
            _git(self.repo, "commit", "-q", "-am", name)
        _git(self.repo, "checkout", "-q", "pr-d")
        proc = _git(self.repo, "merge", "--no-edit", "pr-c", check=False)
        return proc.returncode, path.read_bytes()

    def test_a_binary_file_is_left_alone_and_conflicts(self) -> None:
        base = b"a\n\x00\x01z\n"
        rc, out = self._run(base, b"a\n\x00\x01A\nz\n", b"a\n\x00\x01B\nz\n")
        self.assertNotEqual(rc, 0)
        self.assertNotIn(b"A\nB", out)

    def test_a_conflict_exits_nonzero_and_leaves_markers_in_the_file(self) -> None:
        rc, out = self._run(b"a\nold\nz\n", b"a\nours\nz\n", b"a\ntheirs\nz\n")
        self.assertNotEqual(rc, 0)
        self.assertEqual(_git(self.repo, "diff", "--name-only", "--diff-filter=U").stdout.split(), [self.PATH])
        self.assertIn(b"<<<<<<<", out)
        self.assertIn(b">>>>>>>", out)

    def test_shared_first_last_and_middle_lines_conflict_through_git(self) -> None:
        for ours, theirs in (
            (b"a\nSHARED\nA\nz\n", b"a\nSHARED\nB\nz\n"),
            (b"a\nA\nSHARED\nz\n", b"a\nB\nSHARED\nz\n"),
            (b"a\nA\nSHARED\nA2\nz\n", b"a\nB\nSHARED\nB2\nz\n"),
        ):
            with self.subTest(ours=ours):
                rc, out = self._run(b"a\nz\n", ours, theirs)
                self.assertNotEqual(rc, 0)
                self.assertIn(b"<<<<<<<", out)
                self._reset()

    def test_a_file_outside_the_allowlist_is_not_unioned_even_with_the_attribute(self) -> None:
        (self.repo / "src").mkdir()
        (self.repo / ".gitattributes").write_text("src/x.c merge=nk-append\n", encoding="utf-8")
        path = self.repo / "src" / "x.c"
        path.write_bytes(b"a\nz\n")
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "-m", "base")
        for name, body in (("pr-a", b"a\nA\nz\n"), ("pr-b", b"a\nB\nz\n")):
            _git(self.repo, "checkout", "-q", "-b", name, "main")
            path.write_bytes(body)
            _git(self.repo, "commit", "-q", "-am", name)
        proc = _git(self.repo, "merge", "--no-edit", "pr-a", check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn(b"<<<<<<<", path.read_bytes())

    def _reset(self) -> None:
        _git(self.repo, "merge", "--abort", check=False)
        _git(self.repo, "checkout", "-q", "main")
        _git(self.repo, "branch", "-D", "pr-a", "pr-b")
        _git(self.repo, "reset", "-q", "--hard", "main")


if __name__ == "__main__":
    unittest.main()
