# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""tools/stage_install.py: the staged player install and its icon."""

from __future__ import annotations

import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import stage_install  # noqa: E402

_GIT_ENV = {
    "GIT_AUTHOR_NAME": "fixture", "GIT_AUTHOR_EMAIL": "fixture@example.invalid",
    "GIT_COMMITTER_NAME": "fixture", "GIT_COMMITTER_EMAIL": "fixture@example.invalid",
    "GIT_CONFIG_NOSYSTEM": "1",
}


def _fixture_repo(root: Path) -> Path:
    repo = root / "repo"
    (repo / "tools").mkdir(parents=True)
    (repo / "tools" / "nk_cli.py").write_text("print('cli')\n", encoding="utf-8")
    (repo / "assets").mkdir()
    (repo / "assets" / "prereq_manifest.json").write_text("{}\n", encoding="utf-8")
    env = {**os.environ, **_GIT_ENV}
    for command in (["init", "-q"], ["add", "-A"], ["commit", "-q", "-m", "fixture"]):
        subprocess.run(["git", "-C", str(repo), *command], check=True, env=env,
                       capture_output=True)
    return repo


def _fake_stager(target: Path) -> list[str]:
    (target / "SDL3.dll").write_bytes(b"fixture")
    return ["SDL3.dll"]


class IconTests(unittest.TestCase):
    def test_icon_is_a_png_ico_with_every_size(self):
        data = stage_install.build_icon((16, 32))
        reserved, kind, count = struct.unpack_from("<HHH", data, 0)
        self.assertEqual((reserved, kind, count), (0, 1, 2))
        for index, size in enumerate((16, 32)):
            width, height, _, _, planes, bits, length, offset = struct.unpack_from(
                "<BBBBHHII", data, 6 + 16 * index)
            self.assertEqual((width, height, planes, bits), (size, size, 1, 32))
            self.assertEqual(data[offset:offset + 8], b"\x89PNG\r\n\x1a\n")
            self.assertLessEqual(offset + length, len(data))

    def test_icon_draws_the_mark_on_a_dark_tile(self):
        size = 64
        rgba = stage_install.render_icon_rgba(size, samples=1)
        self.assertEqual(len(rgba), size * size * 4)

        def pixel(x, y):
            return tuple(rgba[(y * size + x) * 4:(y * size + x) * 4 + 4])

        self.assertEqual(pixel(0, 0)[3], 0, "rounded corner stays transparent")
        self.assertEqual(pixel(size // 2, 3), (0x10, 0x15, 0x1C, 255))
        self.assertEqual(pixel(14, 14), (0xD7, 0xF2, 0x4A, 255), "lime block, top left")
        self.assertEqual(pixel(size - 14, size // 2), (0xF5, 0xF7, 0xFA, 255))


class InstallTests(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        self.repo = _fixture_repo(self.root)
        self.player = self.root / "nakagawa_player.exe"
        self.player.write_bytes(b"player")

    def tearDown(self):
        self._temp.cleanup()

    def test_install_has_release_layout_and_record(self):
        dest = self.root / "Nakagawa Recomp"
        record = stage_install.stage_install(dest, self.player, repo=self.repo,
                                             stage_dlls=_fake_stager)
        self.assertTrue((dest / "bin" / "nakagawa_player.exe").is_file())
        self.assertTrue((dest / "bin" / "SDL3.dll").is_file())
        self.assertTrue((dest / "source" / "tools" / "nk_cli.py").is_file())
        self.assertTrue((dest / "source" / "assets" / "prereq_manifest.json").is_file())
        self.assertTrue((dest / "nakagawa.ico").is_file())
        marker = json.loads((dest / stage_install.MARKER).read_text(encoding="utf-8"))
        self.assertEqual(marker["source_commit"], record["source_commit"])
        self.assertEqual(len(marker["source_commit"]), 40)
        self.assertEqual(marker["player"], "bin/nakagawa_player.exe")
        self.assertEqual([p.name for p in dest.iterdir() if p.name.startswith(".staging")], [])

    def test_reinstall_replaces_its_own_install(self):
        dest = self.root / "install"
        stage_install.stage_install(dest, self.player, repo=self.repo, stage_dlls=_fake_stager)
        (dest / "bin" / "stale.txt").write_text("old", encoding="utf-8")
        self.player.write_bytes(b"player v2")
        stage_install.stage_install(dest, self.player, repo=self.repo, stage_dlls=_fake_stager)
        self.assertFalse((dest / "bin" / "stale.txt").exists())
        self.assertEqual((dest / "bin" / "nakagawa_player.exe").read_bytes(), b"player v2")
        self.assertFalse((dest / "bin.old").exists())

    def test_refuses_a_folder_it_did_not_create(self):
        dest = self.root / "someone-elses"
        dest.mkdir()
        (dest / "keep.txt").write_text("keep", encoding="utf-8")
        with self.assertRaisesRegex(stage_install.InstallError, "INSTALL_DESTINATION_NOT_OWNED"):
            stage_install.stage_install(dest, self.player, repo=self.repo,
                                        stage_dlls=_fake_stager)
        self.assertEqual((dest / "keep.txt").read_text(encoding="utf-8"), "keep")

    def test_missing_player_is_named(self):
        with self.assertRaisesRegex(stage_install.InstallError, "INSTALL_PLAYER_MISSING"):
            stage_install.stage_install(self.root / "x", self.root / "absent.exe",
                                        repo=self.repo, stage_dlls=_fake_stager)

    def test_refuses_an_install_inside_the_source_tree(self):
        with self.assertRaisesRegex(stage_install.InstallError, "outside the source tree"):
            stage_install.stage_install(stage_install.ROOT / "build" / "install-test",
                                        self.player, repo=self.repo,
                                        stage_dlls=_fake_stager)

    @unittest.skipUnless(os.name == "nt", "Windows shortcuts")
    def test_shortcut_targets_the_installed_player(self):
        dest = self.root / "install"
        stage_install.stage_install(dest, self.player, repo=self.repo, stage_dlls=_fake_stager)
        link = self.root / "Nakagawa Recomp.lnk"
        stage_install.write_shortcut(link, dest)
        script = ("$s=(New-Object -ComObject WScript.Shell).CreateShortcut($env:NK_LNK); "
                  "$s.TargetPath; $s.Arguments; $s.WorkingDirectory")
        result = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
            capture_output=True, text=True, check=True,
            env={**os.environ, "NK_LNK": str(link)})
        target, arguments, workdir = result.stdout.strip().splitlines()
        self.assertEqual(Path(target), (dest / "bin" / "nakagawa_player.exe").resolve())
        self.assertEqual(arguments, "--ui=next")
        self.assertEqual(Path(workdir), (dest / "bin").resolve())


if __name__ == "__main__":
    unittest.main()
