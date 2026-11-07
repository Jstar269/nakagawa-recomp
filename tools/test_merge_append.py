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

DRIVER = "python tools/merge_append.py %O %A %B %L %P"
# Files whose conflicts are independent items. Prose, code and generated files are not allowed.
ALLOWED = {
    "mk/selftests.mk",
    "mk/public_targets.mk",
    "mk/runtime_sources.mk",
    "CMakeLists.txt",
    "docs/DEBUGGING.md",
}
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
        self.assertEqual(listed, ALLOWED)
        self.assertNotIn("merge=union", text, "plain union keeps both versions of an edited line silently")
        for path in listed:
            self.assertTrue((ROOT / path).is_file(), f"{path} is listed for the merge driver but does not exist")
            self.assertNotIn(Path(path).suffix, FORBIDDEN_SUFFIXES, f"{path}: code and data never use the driver")

    def test_docs_give_the_driver_command_the_tests_run(self) -> None:
        text = (ROOT / "docs" / "CI.md").read_text(encoding="utf-8")
        self.assertIn(f'git config merge.nk-append.driver "{DRIVER}"', text)

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
    def _merge(self, base: str, ours: str, theirs: str) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            for name, body in (("base", base), ("cur", ours), ("other", theirs)):
                (d / name).write_bytes(body.encode())
            rc = merge_append.merge(d / "base", d / "cur", d / "other", 7)
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

    def test_edit_against_insert_stays_a_conflict(self) -> None:
        rc, out = self._merge("a\nold\nb\n", "a\nnew\nb\n", "a\nold\nadded\nb\n")
        # Git merges this itself or reports a conflict; it must never hold two contradictory versions of "old".
        if rc == 0:
            self.assertEqual(out.count("old"), 1 if "old" in out else 0)
        else:
            self.assertIn("<<<<<<<", out)

    def test_one_conflict_with_an_edit_blocks_the_other_pure_insert(self) -> None:
        base = "a\nold\nb\nc\nd\n"
        rc, out = self._merge(base, "a\nours\nb\nc\nO\nd\n", "a\ntheirs\nb\nc\nT\nd\n")
        self.assertEqual(rc, 1)
        self.assertIn("<<<<<<<", out)

    def test_clean_merge_is_unchanged(self) -> None:
        rc, out = self._merge("a\nb\nc\n", "A\nb\nc\n", "a\nb\nC\n")
        self.assertEqual((rc, out), (0, "A\nb\nC\n"))

    def test_deleted_line_against_insert_beside_it_is_not_a_union(self) -> None:
        rc, out = self._merge("a\nold\nb\n", "a\nb\n", "a\nold\nnew\nb\n")
        if rc == 0:
            self.assertNotIn("<<<<<<<", out)
        else:
            self.assertIn("<<<<<<<", out)


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


if __name__ == "__main__":
    unittest.main()
