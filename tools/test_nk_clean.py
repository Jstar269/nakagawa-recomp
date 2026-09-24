# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Preview/allowlist safety contracts for tools/nk_clean.py (#368).

Every test here builds a synthetic workspace in a temporary directory only.
The invariants pinned are: dry-run deletes nothing; only allowlisted,
untracked, in-root files are ever selected; protected targets planted behind a
symlink or Windows junction inside an allowlisted root survive; age bounding
still previews first; and the Makefile / nk.ps1 entry points stay wired to the
same tool.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import nk_clean  # noqa: E402


def _write(path: Path, data: bytes = b"x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def _make_workspace(base: Path) -> Path:
    root = base / "ws"
    for marker in nk_clean.WORKSPACE_ANCHORS:
        _write(root / marker)
    return root


def _plant(root: Path) -> dict[str, Path]:
    planted: dict[str, Path] = {}
    _write(root / "build" / "game.o", b"abcd")
    _write(root / "build" / "sub" / "dir" / "file.bin", b"efghij")
    planted["tracked"] = _write(root / "build" / "keep_me.txt", b"tracked")
    _write(root / "logs" / "build_out_recomp.log", b"log1")
    _write(root / "logs" / "recomp_err.log", b"log2")
    planted["private_log"] = _write(root / "logs" / "oracle_run.log", b"secret run")
    planted["key"] = _write(root / "keys" / "secret.bin", b"key")
    planted["input"] = _write(root / "place_game_here" / "input.bin", b"input")
    _write(root / "scratch" / "stale.tmp", b"tmp")
    _write(root / "compile_commands.json", b"[]")
    return planted


def _make_dir_link(link: Path, target: Path) -> bool:
    link.parent.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        proc = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(target)],
            capture_output=True, text=True, check=False,
        )
        return proc.returncode == 0
    try:
        os.symlink(target, link, target_is_directory=True)
    except OSError:
        return False
    return True


class DryRunTests(unittest.TestCase):
    def test_dry_run_deletes_nothing_and_renders_grouped_sizes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            planted = _plant(root)

            preview = nk_clean.plan_clean(root, tracked=frozenset())
            self.assertFalse(preview.errors, preview.errors)
            text = nk_clean.render(preview, confirm=False)

            self.assertIn("DRY-RUN", text)
            for category in ("build outputs", "logs", "tool scratch"):
                self.assertIn(f"[{category}]", text)
            self.assertIn("build/game.o", text)
            self.assertIn(nk_clean.format_size(4), text)
            self.assertIn("No files were deleted", text)
            for path in planted.values():
                self.assertTrue(path.exists(), f"dry-run touched {path}")
            self.assertTrue((root / "build" / "game.o").exists())

    def test_confirm_gate_accepts_only_explicit_confirmation(self) -> None:
        env = dict(os.environ)
        env.pop("CONFIRM", None)
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertFalse(nk_clean._confirm_requested(False))
        with mock.patch.dict(os.environ, {"CONFIRM": "1"}):
            self.assertTrue(nk_clean._confirm_requested(False))
        with mock.patch.dict(os.environ, {"CONFIRM": "0"}):
            self.assertFalse(nk_clean._confirm_requested(False))
        with mock.patch.dict(os.environ, {"CONFIRM": "1"}):
            self.assertTrue(nk_clean._confirm_requested(True))


class SelectionTests(unittest.TestCase):
    def test_confirmed_delete_removes_exactly_the_previewed_set(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            _plant(root)
            tracked = frozenset({"build/keep_me.txt"})

            preview = nk_clean.plan_clean(root, tracked=tracked, include_title_outputs=True)
            selected = {f.rel for f in preview.files}
            self.assertEqual(
                selected,
                {
                    "build/game.o",
                    "build/sub/dir/file.bin",
                    "logs/build_out_recomp.log",
                    "logs/recomp_err.log",
                    "scratch/stale.tmp",
                    "compile_commands.json",
                },
            )
            self.assertNotIn("build/keep_me.txt", selected)
            self.assertNotIn("logs/oracle_run.log", selected)

            failures = nk_clean.execute(preview)
            self.assertEqual(failures, [])

            for rel in selected:
                self.assertFalse((root / rel).exists(), f"{rel} was previewed but not deleted")
            self.assertTrue((root / "build" / "keep_me.txt").exists(), "tracked file deleted")
            self.assertTrue((root / "logs" / "oracle_run.log").exists(), "private-run log deleted")
            self.assertTrue((root / "keys" / "secret.bin").exists(), "protected input deleted")
            self.assertTrue((root / "place_game_here" / "input.bin").exists(), "private input deleted")
            self.assertTrue((root / "build").exists(), "allowlisted root directory itself must remain")
            self.assertFalse((root / "build" / "sub").exists(), "emptied subdirectory should be pruned")
            self.assertTrue((root / "scratch").exists(), "rule roots are not removed")

    def test_older_than_bounds_the_selection_and_still_previews(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            _plant(root)
            old = _write(root / "build" / "old.o", b"old-object")
            now = time.time()
            stale = now - 10 * 86400
            os.utime(old, (stale, stale))

            preview = nk_clean.plan_clean(
                root, tracked=frozenset({"build/keep_me.txt"}), older_than_days=7, now=now,
                include_title_outputs=True,
            )
            selected = {f.rel for f in preview.files}
            self.assertIn("build/old.o", selected)
            self.assertNotIn("build/game.o", selected, "fresh file must be age-excluded")
            self.assertGreaterEqual(preview.age_excluded, 1)
            text = nk_clean.render(preview, confirm=False)
            self.assertIn("Age bound", text)

            failures = nk_clean.execute(preview)
            self.assertEqual(failures, [])
            self.assertFalse(old.exists())
            self.assertTrue((root / "build" / "game.o").exists(), "fresh file deleted by age-bounded run")


class TitleOutputTests(unittest.TestCase):
    def test_title_outputs_and_run_logs_need_explicit_opt_in(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            _write(root / "build" / "hst" / "hst_recomp.c")          # private title output
            _write(root / "build" / "mygame" / "recomp.o")           # public default build
            _write(root / "build" / "synthetic2" / "x.o")            # public fixture build
            _write(root / "logs" / "stdout_run.log")                 # title run log
            _write(root / "logs" / "link_err.log")                   # ordinary build log
            default = nk_clean.plan_clean(root, tracked=frozenset())
            chosen = {entry.rel for entry in default.files}
            self.assertNotIn("build/hst/hst_recomp.c", chosen)
            self.assertNotIn("logs/stdout_run.log", chosen)
            self.assertIn("build/mygame/recomp.o", chosen)
            self.assertIn("build/synthetic2/x.o", chosen)
            self.assertIn("logs/link_err.log", chosen)
            skipped = {entry.rel for entry in default.skipped}
            self.assertIn("build/hst", skipped)
            opted = nk_clean.plan_clean(root, tracked=frozenset(), include_title_outputs=True)
            chosen_opted = {entry.rel for entry in opted.files}
            self.assertIn("build/hst/hst_recomp.c", chosen_opted)
            self.assertIn("logs/stdout_run.log", chosen_opted)


class ReparsePointTests(unittest.TestCase):
    def test_junction_target_outside_repo_is_never_selected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = _make_workspace(base)
            outside_dir = base / "outside" / "protected_dir"
            secret = _write(outside_dir / "secret.bin", b"secret")
            link = root / "build" / "link_to_protected"
            if not _make_dir_link(link, outside_dir):
                self.skipTest("cannot create a directory symlink/junction on this host")

            preview = nk_clean.plan_clean(root, tracked=frozenset())
            for entry in preview.files:
                self.assertNotIn("secret", entry.rel)
                self.assertNotIn("outside", entry.rel)
            skipped = [entry for entry in preview.skipped if "link_to_protected" in entry.rel]
            self.assertTrue(skipped, "reparse point must be reported as skipped")
            self.assertIn("never followed", skipped[0].reason)

            failures = nk_clean.execute(preview)
            self.assertEqual(failures, [])
            self.assertTrue(secret.exists(), "protected target behind the junction was deleted")
            self.assertTrue(link.exists(), "the junction entry itself is left in place")

    def test_junction_to_protected_root_inside_repo_is_never_followed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            vault = root / "keys" / "vault"
            secret = _write(vault / "secret.bin", b"secret")
            link = root / "build" / "vault_link"
            if not _make_dir_link(link, vault):
                self.skipTest("cannot create a directory symlink/junction on this host")

            preview = nk_clean.plan_clean(root, tracked=frozenset())
            self.assertFalse(any("secret" in entry.rel for entry in preview.files))

            failures = nk_clean.execute(preview)
            self.assertEqual(failures, [])
            self.assertTrue(secret.exists(), "junction into keys/ was followed")

    def test_allowlist_root_replaced_by_junction_selects_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = _make_workspace(base)
            elsewhere = base / "elsewhere"
            outside_file = _write(elsewhere / "payload.bin", b"payload")
            build = root / "build"
            build.mkdir(parents=True, exist_ok=True)
            link_tmp = base / "build-link"
            if not _make_dir_link(link_tmp, elsewhere):
                self.skipTest("cannot create a directory symlink/junction on this host")
            os.rmdir(build)
            os.rename(link_tmp, build)

            preview = nk_clean.plan_clean(root, tracked=frozenset())
            self.assertFalse(
                any(entry.rel.startswith("build/") for entry in preview.files),
                "contents behind a junctioned allowlisted root must not be selected",
            )
            failures = nk_clean.execute(preview)
            self.assertEqual(failures, [])
            self.assertTrue(outside_file.exists())


class ContainmentTests(unittest.TestCase):
    def test_path_outside_repo_root_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = _make_workspace(base)
            root_real = os.path.realpath(os.path.abspath(root))
            outside = _write(base / "elsewhere" / "file.bin", b"out")
            inside = _write(root / "build" / "ok.o", b"in")

            nk_clean._require_within(root_real, inside)
            with self.assertRaises(nk_clean.CleanRefusedError):
                nk_clean._require_within(root_real, outside)


class CheckoutDryRunTests(unittest.TestCase):
    def test_cli_dry_run_against_this_checkout_deletes_nothing(self) -> None:
        env = dict(os.environ)
        env.pop("CONFIRM", None)
        proc = subprocess.run(
            [sys.executable, str(TOOLS / "nk_clean.py")],
            cwd=ROOT, capture_output=True, text=True, env=env, timeout=600, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("DRY-RUN", proc.stdout)
        self.assertTrue((ROOT / "AGENTS.md").is_file())
        self.assertTrue((ROOT / "Makefile").is_file())


class EntryPointWiringTests(unittest.TestCase):
    def test_makefile_exposes_preview_first_clean_target(self) -> None:
        text = (ROOT / "Makefile").read_text(encoding="utf-8")
        catalog = text.split("PUBLIC_TARGETS :=", 1)[1].split("INTERNAL_TARGETS :=", 1)[0]
        self.assertIn("clean-preview", catalog)
        self.assertIn("HELP_DESCRIPTION_clean-preview :=", text)
        self.assertIn("tools/nk_clean.py", text)
        recipe = next(
            line for line in text.splitlines()
            if line.startswith("\t") and "tools/nk_clean.py" in line
        )
        self.assertIn("--yes", recipe)
        self.assertIn("$(filter 1,$(CONFIRM))", recipe)
        self.assertIn("--older-than", recipe)

    def test_frontend_exposes_clean_action(self) -> None:
        text = (ROOT / "nk.ps1").read_text(encoding="utf-8-sig")
        self.assertIn('"Clean"', text)
        self.assertIn("nk_clean.py", text)
        self.assertIn("[switch]$Yes", text)


if __name__ == "__main__":
    unittest.main()
