# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""tools/stage_install.py: the staged player install and its icon."""

from __future__ import annotations

import configparser
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile
from unittest import mock
import unittest
import zipfile

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import stage_install  # noqa: E402
from makefile_text import read_makefile  # noqa: E402

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
    # The picture itself is tested in tools/test_app_icon.py.
    def test_install_icon_is_the_app_icon_the_executables_carry(self):
        committed = (stage_install.ROOT / "assets" / "brand" / "nakagawa.ico").read_bytes()
        self.assertEqual(stage_install.app_icon(), committed)


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

    def test_refuses_a_folder_the_windows_installer_set_up(self):
        """#2208: Setup.exe leaves unins000.exe beside the program and has its own uninstall; the stager
        must never share the folder, even though it holds no stager marker."""
        for name in stage_install.INSTALLER_UNINSTALL_FILES:
            with self.subTest(name=name):
                dest = self.root / f"installed-{name}"
                dest.mkdir()
                (dest / name).write_bytes(b"inno")
                (dest / "bin").mkdir()
                with self.assertRaisesRegex(stage_install.InstallError, "INSTALL_DESTINATION_INSTALLER_OWNED"):
                    stage_install.stage_install(dest, self.player, repo=self.repo, stage_dlls=_fake_stager)
                self.assertEqual(sorted(path.name for path in dest.iterdir()), sorted([name, "bin"]))
        # Also when the folder carries a stager marker as well (a mixed folder is never safe to share).
        dest = self.root / "mixed"
        stage_install.stage_install(dest, self.player, repo=self.repo, stage_dlls=_fake_stager)
        (dest / "unins000.exe").write_bytes(b"inno")
        with self.assertRaisesRegex(stage_install.InstallError, "INSTALL_DESTINATION_INSTALLER_OWNED"):
            stage_install.stage_install(dest, self.player, repo=self.repo, stage_dlls=_fake_stager)

    def test_missing_player_is_named(self):
        with self.assertRaisesRegex(stage_install.InstallError, "INSTALL_PLAYER_MISSING"):
            stage_install.stage_install(self.root / "x", self.root / "absent.exe",
                                        repo=self.repo, stage_dlls=_fake_stager)

    def test_refuses_an_install_inside_the_source_tree(self):
        with self.assertRaisesRegex(stage_install.InstallError, "outside the source tree"):
            stage_install.stage_install(stage_install.ROOT / "build" / "install-test",
                                        self.player, repo=self.repo,
                                        stage_dlls=_fake_stager)

    def test_windows_install_writes_the_runtime_manifest(self):
        """The installed bin/ carries the same runtime identity record as the portable ZIP (#324)."""
        calls = []

        def fake_stage(target, **kwargs):
            calls.append(kwargs)
            return _fake_stager(target)

        with mock.patch.object(stage_install, "IS_WINDOWS", True), \
                mock.patch.object(stage_install.stage_runtime_dlls, "stage_runtime_dlls", fake_stage):
            stage_install.stage_install(self.root / "install", self.player, repo=self.repo)
        self.assertEqual(len(calls), 1)
        self.assertIs(calls[0].get("runtime_manifest"), True)


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

    @unittest.skipUnless(os.name == "nt", "Windows shortcuts")
    def test_shortcut_carries_the_player_app_id(self):
        """#2006: the .lnk's System.AppUserModel.ID is the launcher's and the game window's, so a
        pinned shortcut and both windows make one taskbar group."""
        dest = self.root / "install"
        stage_install.stage_install(dest, self.player, repo=self.repo, stage_dlls=_fake_stager)
        link = self.root / "Nakagawa Recomp.lnk"
        stage_install.write_shortcut(link, dest)
        self.assertEqual(stage_install.read_shortcut_app_id(link), "Nakagawa.Recomp")


    @unittest.skipUnless(os.name == "nt", "Windows shortcuts")
    def test_a_shortcut_that_starts_another_copy_is_not_replaced(self):
        """#2208: the installer writes the same shortcut name; replacing it would break that copy's shortcut."""
        first, second = self.root / "first", self.root / "second"
        for dest in (first, second):
            (dest / "bin").mkdir(parents=True)
            (dest / "bin" / "nakagawa_player.exe").write_bytes(b"MZ")
            (dest / "nakagawa.ico").write_bytes(b"ico")
        link = self.root / "Nakagawa Recomp.lnk"
        stage_install.write_shortcut(link, first)
        before = link.read_bytes()
        with self.assertRaisesRegex(stage_install.InstallError, "INSTALL_SHORTCUT_OTHER_COPY"):
            stage_install.write_shortcut(link, second)
        self.assertEqual(link.read_bytes(), before)
        # The same copy re-stages over its own shortcut.
        stage_install.write_shortcut(link, first)
        # A stale shortcut (the other copy's program is gone) is replaced.
        (first / "bin" / "nakagawa_player.exe").unlink()
        stage_install.write_shortcut(link, second)
        self.assertEqual(Path(stage_install.read_shortcut_target(link)),
                         (second / "bin" / "nakagawa_player.exe").resolve())


class ShortcutFlagTests(unittest.TestCase):
    """--desktop-shortcut and --start-menu-shortcut through main(), with the install and the
    shortcut writer stubbed, so the routing is checked on any host."""

    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.root = Path(self._temp.name)
        self.desktop = self.root / "Desktop"
        self.programs = self.root / "Start Menu" / "Programs"
        self.written: list[Path] = []
        self.recorded: list[list[Path]] = []

        def fake_write(shortcut, dest, arguments=stage_install.DEFAULT_ARGUMENTS):
            self.written.append(Path(shortcut))

        def fake_record(dest, paths):
            self.recorded.append([Path(path) for path in paths])

        for patcher in (
            mock.patch.object(stage_install, "stage_install",
                              return_value={"player": "bin/nakagawa_player.exe",
                                            "source_commit": "0" * 40}),
            mock.patch.object(stage_install, "desktop_folder", return_value=self.desktop),
            mock.patch.object(stage_install, "start_menu_folder", return_value=self.programs),
            mock.patch.object(stage_install, "write_shortcut", side_effect=fake_write),
            mock.patch.object(stage_install, "record_integration", side_effect=fake_record),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

    def _main(self, *flags: str) -> int:
        with contextlib.redirect_stdout(io.StringIO()):
            return int(stage_install.main(["--dest", str(self.root / "install"), *flags]))

    def test_start_menu_shortcut_goes_to_the_programs_folder(self):
        with mock.patch.object(stage_install, "IS_WINDOWS", True):
            self.assertEqual(self._main("--start-menu-shortcut"), 0)
        self.assertEqual(self.written, [self.programs / "Nakagawa Recomp.lnk"])
        self.assertEqual(self.recorded, [[self.programs / "Nakagawa Recomp.lnk"]])

    def test_both_shortcuts_are_written_and_recorded_together(self):
        with mock.patch.object(stage_install, "IS_WINDOWS", True):
            self.assertEqual(self._main("--desktop-shortcut", "--start-menu-shortcut"), 0)
        both = [self.desktop / "Nakagawa Recomp.lnk", self.programs / "Nakagawa Recomp.lnk"]
        self.assertEqual(self.written, both)
        self.assertEqual(self.recorded, [both], "one marker update lists both shortcuts")

    def test_no_flag_writes_no_shortcut(self):
        with mock.patch.object(stage_install, "IS_WINDOWS", True):
            self.assertEqual(self._main(), 0)
        self.assertEqual(self.written, [])
        self.assertEqual(self.recorded, [])

    def test_start_menu_flag_on_linux_writes_the_application_entry(self):
        entry = self.root / "applications" / "nakagawa-recomp.desktop"
        icon = self.root / "icons" / "nakagawa-recomp.png"
        with mock.patch.object(stage_install, "IS_WINDOWS", False), \
                mock.patch.object(stage_install, "write_desktop_entry",
                                  return_value=[entry, icon]) as write_entry:
            self.assertEqual(self._main("--start-menu-shortcut"), 0)
        write_entry.assert_called_once()
        self.assertEqual(self.written, [])
        self.assertEqual(self.recorded, [[entry, icon]])


class AppIdentityTests(unittest.TestCase):
    """#2006: the launcher (src/core/nk_launch.h), the game window (sdl3vk.c, which does not
    include that header) and the shortcuts this tool writes carry one identity."""

    @staticmethod
    def _defines(path: Path, prefix: str) -> dict[str, str]:
        text = path.read_text(encoding="utf-8")
        return {name: value for name, value in
                re.findall(rf'#define {prefix}(\w+) "([^"]*)"', text)}

    def test_launcher_game_and_shortcuts_share_one_identity(self):
        launcher = self._defines(stage_install.ROOT / "src" / "core" / "nk_launch.h", "NK_APP_")
        game = self._defines(stage_install.ROOT / "src" / "rt" / "gpu_sdl3vk" / "sdl3vk.c",
                             "SDL3VK_APP_")
        self.assertEqual(launcher, {"USER_MODEL_ID": "Nakagawa.Recomp",
                                    "DESKTOP_ID": "nakagawa-recomp",
                                    "NAME": "Nakagawa Recomp"})
        self.assertEqual(game, launcher, "the game window's copies equal the launcher's")
        self.assertEqual(stage_install.APP_USER_MODEL_ID, launcher["USER_MODEL_ID"])
        self.assertEqual(stage_install.APP_DESKTOP_ID, launcher["DESKTOP_ID"])
        self.assertEqual(stage_install.DESKTOP_ENTRY_NAME, launcher["DESKTOP_ID"] + ".desktop")

    def test_launcher_sets_the_identity_before_anything_else_in_main(self):
        # The launcher's process takes the shared ID (player_host.c) as main()'s first act after
        # the UTF-8 setup, before any window or SDL message box can open under another ID.
        host = (stage_install.ROOT / "src" / "player" / "player_host.c").read_text(encoding="utf-8")
        body = host[host.index("void player_host_set_app_identity(void) {"):]
        body = body[:body.index("\n}\n")]
        self.assertIn('SetCurrentProcessExplicitAppUserModelID(L"" NK_APP_USER_MODEL_ID)', body)
        self.assertIn("SDL_SetAppMetadata(NK_APP_NAME, NULL, NK_APP_DESKTOP_ID)", body)
        main = (stage_install.ROOT / "src" / "player" / "main.c").read_text(encoding="utf-8")
        start = main[main.index("int main(int argc, char *argv[]) {"):]
        calls = [line.strip() for line in start.splitlines()[1:]
                 if line.strip() and not line.strip().startswith(("/*", "*", "//"))
                 and not line.strip().endswith("*/")]
        self.assertEqual(calls[:2], ["(void)nk_platform_init_utf8_main(&argc, &argv);",
                                     "player_host_set_app_identity();"])

    def test_shortcut_path_reaches_the_shell_with_backslashes(self):
        # MSYS2's Python joins paths with '/', and the shell's property store refuses those.
        from pathlib import PurePosixPath
        self.assertEqual(stage_install._shell_path(PurePosixPath("D:/Games/My Apps/Nakagawa Recomp.lnk")),
                         "D:\\Games\\My Apps\\Nakagawa Recomp.lnk")

    def test_app_user_model_id_follows_the_windows_rules(self):
        # Windows: at most 128 characters, no spaces, dot-separated sections.
        value = stage_install.APP_USER_MODEL_ID
        self.assertLessEqual(len(value), 128)
        self.assertNotIn(" ", value)
        self.assertGreaterEqual(value.count("."), 1)


class LinuxInstallTests(unittest.TestCase):
    """The Linux branch, exercised on any host by patching the platform flag."""

    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.root = Path(self._temp.name)
        self.repo = _fixture_repo(self.root)
        self.player = self.root / "nakagawa_player"
        self.player.write_bytes(b"player")
        # A space in the data home exercises quoting of every path the entry holds.
        self.data = self.root / "xdg data"
        for patcher in (
            mock.patch.object(stage_install, "IS_WINDOWS", False),
            mock.patch.dict(os.environ, {"XDG_DATA_HOME": str(self.data)}),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_default_player_follows_the_host(self):
        self.assertEqual(stage_install.default_player_path(False).name, "nakagawa_player")
        self.assertEqual(stage_install.default_player_path(True).name, "nakagawa_player.exe")

    def test_missing_player_hint_names_make(self):
        with self.assertRaisesRegex(stage_install.InstallError,
                                    r"build it with `make player`"):
            stage_install.stage_install(self.root / "x", self.root / "absent",
                                        repo=self.repo)

    def test_integration_list_keeps_earlier_shortcuts(self):
        # #2006 review: a second run with only one shortcut flag, or staging again, must not
        # forget a shortcut an earlier run wrote outside the install folder.
        dest = self.root / "install"
        stage_install.stage_install(dest, self.player, repo=self.repo)
        first, second = self.root / "Desktop.lnk", self.root / "Programs.lnk"
        stage_install.record_integration(dest, [first])
        stage_install.record_integration(dest, [second, first])
        marker = dest / stage_install.MARKER
        self.assertEqual(json.loads(marker.read_text(encoding="utf-8"))["integration"],
                         [str(first), str(second)])
        record = stage_install.stage_install(dest, self.player, repo=self.repo)
        self.assertEqual(record["integration"], [str(first), str(second)])
        self.assertEqual(json.loads(marker.read_text(encoding="utf-8"))["integration"],
                         [str(first), str(second)])

    def test_linux_stage_skips_the_pe_runtime(self):
        dest = self.root / "install"
        record = stage_install.stage_install(dest, self.player, repo=self.repo)
        self.assertEqual(record["runtime_stage"], "SKIPPED_NOT_PE")
        self.assertEqual(record["runtime_files"], [])
        self.assertEqual(record["player"], "bin/nakagawa_player")
        self.assertTrue((dest / "bin" / "nakagawa_player").is_file())
        self.assertEqual([p.name for p in (dest / "bin").iterdir()], ["nakagawa_player"])
        self.assertTrue((dest / "source" / "tools" / "nk_cli.py").is_file())

    def test_desktop_entry_is_a_parsable_ini_with_quoted_exec(self):
        dest = self.root / "Nakagawa Recomp"
        stage_install.stage_install(dest, self.player, repo=self.repo)
        entry, icon = stage_install.write_desktop_entry(dest)
        self.assertEqual(entry, self.data / "applications" / "nakagawa-recomp.desktop")
        parser = configparser.ConfigParser(interpolation=None)
        parser.optionxform = str
        parser.read_string(entry.read_text(encoding="utf-8"))
        section = parser["Desktop Entry"]
        player = str((dest / "bin" / "nakagawa_player").resolve())
        self.assertEqual(section["Type"], "Application")
        self.assertEqual(shlex.split(section["Exec"]), [player, "--ui=next"])
        self.assertEqual(section["TryExec"], player)
        self.assertTrue(Path(section["TryExec"]).is_file())
        self.assertEqual(Path(section["Icon"]), icon)
        self.assertTrue(icon.is_file())
        # #2006: the window class SDL gives the launcher and the game, so the desktop puts
        # their windows under this entry.
        self.assertEqual(section["StartupWMClass"], "nakagawa-recomp")

    def test_desktop_icon_is_a_256_pixel_png(self):
        _, icon = stage_install.write_desktop_entry(self.root / "install")
        data = icon.read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(struct.unpack_from(">II", data, 16), (256, 256))
        self.assertIn(data, stage_install.app_icon(), "the app icon's own 256 px image")

    def test_exec_escapes_quotes_and_field_codes(self):
        self.assertEqual(stage_install._exec_quote('a"b$c`d\\e%f'), '"a\\"b\\$c\\`d\\\\e%%f"')

    def test_marker_lists_both_integration_paths(self):
        dest = self.root / "install"
        stage_install.stage_install(dest, self.player, repo=self.repo)
        paths = stage_install.write_desktop_entry(dest)
        stage_install.record_integration(dest, paths)
        marker = json.loads((dest / stage_install.MARKER).read_text(encoding="utf-8"))
        self.assertEqual(marker["schema_version"], 2)
        self.assertEqual(marker["integration"], [str(p) for p in paths])
        self.assertEqual(len(marker["integration"]), 2)

    def test_relative_xdg_data_home_is_ignored(self):
        home = self.root / "home"
        with mock.patch.dict(os.environ, {"XDG_DATA_HOME": "relative/data"}), \
                mock.patch.object(stage_install.Path, "home", return_value=home):
            self.assertEqual(stage_install.xdg_data_home(), home / ".local" / "share")

    def test_windows_only_shortcut_flag_is_refused_on_linux(self):
        with contextlib.redirect_stderr(io.StringIO()) as stderr:
            status = stage_install.main(["--dest", str(self.root / "x"),
                                         "--player", str(self.player),
                                         "--shortcut", str(self.root / "x.lnk")])
        self.assertEqual(status, 1)
        self.assertIn("INSTALL_SHORTCUT_UNSUPPORTED", stderr.getvalue())
        self.assertFalse((self.root / "x").exists())


# 2026-10-01T00:00:00Z, an even second (MS-DOS times have two-second resolution).
_EPOCH = 1790812800
# Every kind of stage entry: nested files, a space and a non-ASCII name, an empty folder.
_STAGE_FILES = {
    "START HERE.txt": b"start\n",
    "bin/nakagawa_player.exe": b"MZplayer",
    "bin/SDL3.dll": b"MZsdl",
    "bin/runtime-manifest.json": b"{}\n",
    "docs/SETUP.md": b"# Setup\n",
    "prereqs/msys64/ucrt64/bin/gcc.exe": b"MZgcc" * 4000,
    "source/fixtures/café.txt": "café\n".encode("utf-8"),
    "source/tools/nk_cli.py": b"print('cli')\n",
}
_STAGE_EMPTY = ("prereqs/msys64/tmp",)


def _no_runtime_check(_bin_dir):
    return None


def _make_stage(root: Path, *, reverse: bool, mtime: float) -> Path:
    """A staged release folder; creation order and file times vary, contents do not."""
    stage = root / "nakagawa-recomp-0.0.2-windows-x64"
    names = sorted(_STAGE_FILES, reverse=reverse)
    for name in names:
        path = stage / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(_STAGE_FILES[name])
        os.utime(path, (mtime, mtime))
    for name in _STAGE_EMPTY:
        (stage / name).mkdir(parents=True)
    # A stale SHA256SUMS.txt from an earlier step is rewritten, never trusted.
    (stage / "SHA256SUMS.txt").write_text("stale\r\n", encoding="utf-8")
    return stage


class PortableZipTests(unittest.TestCase):
    """--portable-zip: one commit and one staged folder give one ZIP, byte for byte (#324)."""

    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)

    def tearDown(self):
        self._temp.cleanup()

    def _zip(self, stage: Path, name: str = "out.zip", **kwargs) -> Path:
        out = self.root / name
        stage_install.write_portable_zip(stage, out, source_date_epoch=kwargs.pop("epoch", _EPOCH),
                                         verify_runtime=kwargs.pop("verify", _no_runtime_check))
        returned: Path = out
        return returned

    def test_two_builds_of_the_same_stage_are_byte_identical(self):
        first = _make_stage(self.root / "a", reverse=False, mtime=1_700_000_000)
        second = _make_stage(self.root / "b", reverse=True, mtime=1_800_000_000)
        one = self._zip(first, "one.zip")
        two = self._zip(second, "two.zip")
        self.assertEqual(one.read_bytes(), two.read_bytes())

    def test_entries_are_sorted_dated_normalised_and_relative(self):
        stage = _make_stage(self.root / "a", reverse=True, mtime=1_700_000_000)
        with zipfile.ZipFile(self._zip(stage)) as archive:
            infos = archive.infolist()
            names = [info.filename for info in infos]
            self.assertEqual(names, sorted(names, key=lambda name: name.encode("utf-8")))
            expected = sorted([*_STAGE_FILES, "SHA256SUMS.txt", *(f"{d}/" for d in _STAGE_EMPTY)],
                              key=lambda name: name.encode("utf-8"))
            self.assertEqual(names, expected)
            for info in infos:
                self.assertEqual(info.date_time, (2026, 10, 1, 0, 0, 0), info.filename)
                self.assertEqual(info.create_system, 3, info.filename)
                self.assertEqual(info.extra, b"", info.filename)
                self.assertFalse(info.filename.startswith("/") or ":" in info.filename)
                self.assertNotIn("..", info.filename.split("/"))
                mode = info.external_attr >> 16
                self.assertEqual(mode, 0o040755 if info.is_dir() else 0o100644, info.filename)
            self.assertEqual(archive.comment, b"")
            self.assertEqual(archive.read("source/fixtures/café.txt"), "café\n".encode())
            sums = archive.read("SHA256SUMS.txt").decode("utf-8").splitlines()
        listed = [line.split("  ", 1)[1] for line in sums]
        self.assertEqual(listed, sorted(_STAGE_FILES, key=lambda name: name.encode("utf-8")))
        for line in sums:
            digest, name = line.split("  ", 1)
            self.assertEqual(digest, hashlib.sha256(_STAGE_FILES[name]).hexdigest())

    def test_times_before_1980_are_clamped_not_rejected(self):
        stage = _make_stage(self.root / "a", reverse=False, mtime=1_700_000_000)
        with zipfile.ZipFile(self._zip(stage, epoch=0)) as archive:
            self.assertEqual({info.date_time for info in archive.infolist()}, {(1980, 1, 1, 0, 0, 0)})

    def test_epoch_comes_from_the_argument_then_the_environment_then_the_commit(self):
        repo = _fixture_repo(self.root)
        env = {**os.environ, **_GIT_ENV, "GIT_COMMITTER_DATE": "@1790000000 +0000"}
        subprocess.run(["git", "-C", str(repo), "commit", "-q", "--allow-empty", "-m", "dated"],
                       check=True, env=env, capture_output=True)
        resolve = stage_install.resolve_source_date_epoch
        self.assertEqual(resolve("1234", repo=repo, env={"SOURCE_DATE_EPOCH": "99"}), 1234)
        self.assertEqual(resolve(None, repo=repo, env={"SOURCE_DATE_EPOCH": "99"}), 99)
        self.assertEqual(resolve(None, repo=repo, env={}), 1790000000)
        for bad in ("-5", "1.5", "now", "٣"):
            with self.assertRaisesRegex(stage_install.InstallError, "PORTABLE_EPOCH_INVALID"):
                resolve(bad, repo=repo, env={})

    def test_a_link_in_the_stage_is_refused(self):
        stage = _make_stage(self.root / "a", reverse=False, mtime=1_700_000_000)
        try:
            os.symlink(stage / "docs" / "SETUP.md", stage / "docs" / "link.md")
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"this host cannot create a symbolic link: {exc}")
        with self.assertRaisesRegex(stage_install.InstallError, "PORTABLE_STAGE_LINK"):
            self._zip(stage)
        self.assertFalse((self.root / "out.zip").exists())

    def test_git_metadata_in_the_stage_is_refused(self):
        stage = _make_stage(self.root / "g", reverse=False, mtime=1_700_000_000)
        (stage / "source" / ".git" / "objects").mkdir(parents=True)
        (stage / "source" / ".git" / "HEAD").write_text("ref: refs/heads/master\n", encoding="utf-8")
        with self.assertRaises(stage_install.InstallError) as caught:
            self._zip(stage)
        self.assertIn("PORTABLE_STAGE_GIT_METADATA", str(caught.exception))
        # Only an entry named exactly `.git`: `.github` and `.gitignore` are source files and stay.
        (stage / "source" / ".git" / "objects").rmdir()
        (stage / "source" / ".git" / "HEAD").unlink()
        (stage / "source" / ".git").rmdir()
        (stage / "source" / ".github").mkdir()
        (stage / "source" / ".github" / "x.yml").write_text("a: 1\n", encoding="utf-8")
        (stage / "source" / ".gitignore").write_text("build/\n", encoding="utf-8")
        names = zipfile.ZipFile(self._zip(stage, "kept.zip")).namelist()
        self.assertIn("source/.github/x.yml", names)
        self.assertIn("source/.gitignore", names)

    def test_existing_output_and_output_inside_the_stage_are_refused(self):
        stage = _make_stage(self.root / "a", reverse=False, mtime=1_700_000_000)
        (self.root / "out.zip").write_bytes(b"keep")
        with self.assertRaisesRegex(stage_install.InstallError, "PORTABLE_ZIP_EXISTS"):
            self._zip(stage)
        self.assertEqual((self.root / "out.zip").read_bytes(), b"keep")
        with self.assertRaisesRegex(stage_install.InstallError, "PORTABLE_ZIP_INSIDE_STAGE"):
            stage_install.write_portable_zip(stage, stage / "self.zip", source_date_epoch=_EPOCH,
                                             verify_runtime=_no_runtime_check)

    def test_unverified_runtime_folder_is_refused(self):
        stage = _make_stage(self.root / "a", reverse=False, mtime=1_700_000_000)
        (stage / "bin" / "runtime-manifest.json").unlink()
        with self.assertRaisesRegex(stage_install.InstallError,
                                    "PORTABLE_RUNTIME_UNVERIFIED: RUNTIME_MANIFEST_MISSING"):
            stage_install.write_portable_zip(stage, self.root / "out.zip", source_date_epoch=_EPOCH)
        self.assertFalse((self.root / "out.zip").exists())

    def test_cli_writes_the_zip_and_reports_it(self):
        stage = _make_stage(self.root / "a", reverse=False, mtime=1_700_000_000)
        out = self.root / "cli.zip"
        with mock.patch.object(stage_install.stage_runtime_dlls, "verify_runtime_manifest",
                               _no_runtime_check), \
                contextlib.redirect_stdout(io.StringIO()) as stdout:
            status = stage_install.main(["--portable-zip", str(out), "--zip-stage", str(stage),
                                         "--source-date-epoch", str(_EPOCH)])
        self.assertEqual(status, 0)
        report = json.loads(stdout.getvalue())
        self.assertEqual(report["event"], "portable-zip")
        self.assertEqual(report["sha256"], hashlib.sha256(out.read_bytes()).hexdigest())
        self.assertEqual(report["source_date_epoch"], _EPOCH)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            stage_install.main(["--portable-zip", str(out)])


_V = "0.0.2"
_PLAYER = f"nakagawa-recomp-{_V}-windows-x64.zip"
_SOURCES = f"nakagawa-recomp-{_V}-prerequisite-sources.zip"
_GOOD_ASSETS = [_PLAYER, f"{_PLAYER}.sha256", f"{_PLAYER}.sha256.minisig", _SOURCES, f"{_SOURCES}.sha256"]
_SETUP = f"nakagawa-recomp-{_V}-windows-x64-setup.exe"
_SETUP_ASSETS = [_SETUP, f"{_SETUP}.sha256"]


class UninstallTests(unittest.TestCase):
    """--uninstall removes what the install wrote and never the user's data (#2003, #324)."""

    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.root = Path(self._temp.name)
        self.repo = _fixture_repo(self.root)
        self.player = self.root / "nakagawa_player.exe"
        self.player.write_bytes(b"player")
        self.dest = self.root / "Nakagawa Recomp"
        stage_install.stage_install(self.dest, self.player, repo=self.repo,
                                    stage_dlls=_fake_stager)
        # The per-user folders sit beside the install, as %LOCALAPPDATA%\Nakagawa does.
        self.user = self.root / "LocalAppData" / "Nakagawa"
        for leaf, name in (("data", "library.json"), ("saves", "UCUS00000.sav"),
                           ("config", "settings.json"), ("logs", "game.txt")):
            (self.user / leaf).mkdir(parents=True)
            (self.user / leaf / name).write_text(leaf, encoding="utf-8")
        self.shortcuts = self.root / "Programs"
        self.shortcuts.mkdir()
        self.lnk = self.shortcuts / stage_install.SHORTCUT_NAME
        self.lnk.write_bytes(b"lnk")
        stage_install.record_integration(self.dest, [self.lnk])

    def _user_files(self):
        return sorted(str(path.relative_to(self.user)) for path in self.user.rglob("*")
                      if path.is_file())

    def test_removes_the_install_its_marker_and_its_shortcuts(self):
        summary = stage_install.uninstall(self.dest)
        self.assertFalse(self.dest.exists(), "an emptied install folder goes too")
        self.assertFalse(self.lnk.exists())
        self.assertEqual(summary["removed_integration"], [str(self.lnk)])
        self.assertEqual(summary["left_in_folder"], [])

    def test_never_touches_the_per_user_folders(self):
        before = self._user_files()
        self.assertEqual(len(before), 4)
        stage_install.uninstall(self.dest)
        self.assertEqual(self._user_files(), before)

    def test_a_file_the_user_added_beside_the_install_survives(self):
        (self.dest / "notes.txt").write_text("mine", encoding="utf-8")
        summary = stage_install.uninstall(self.dest)
        self.assertEqual((self.dest / "notes.txt").read_text(encoding="utf-8"), "mine")
        self.assertFalse((self.dest / "bin").exists())
        self.assertFalse((self.dest / stage_install.MARKER).exists())
        self.assertEqual(summary["left_in_folder"], ["notes.txt"])

    def test_dry_run_lists_and_removes_nothing(self):
        summary = stage_install.uninstall(self.dest, dry_run=True)
        self.assertTrue(summary["dry_run"])
        self.assertIn(str(self.dest / "bin"), summary["removed"])
        self.assertIn(str(self.dest / "source"), summary["removed"])
        self.assertTrue((self.dest / "bin" / "SDL3.dll").is_file())
        self.assertTrue(self.lnk.is_file())
        self.assertTrue((self.dest / stage_install.MARKER).is_file())

    def test_a_folder_without_the_marker_is_refused_untouched(self):
        other = self.root / "someone-elses"
        (other / "bin").mkdir(parents=True)
        (other / "bin" / "keep.dll").write_bytes(b"keep")
        with self.assertRaisesRegex(stage_install.InstallError, "INSTALL_DESTINATION_NOT_OWNED"):
            stage_install.uninstall(other)
        self.assertTrue((other / "bin" / "keep.dll").is_file())

    def test_an_edited_marker_cannot_delete_another_file(self):
        precious = self.root / "Documents" / "thesis.docx"
        precious.parent.mkdir()
        precious.write_bytes(b"thesis")
        folder = self.root / "somefolder"
        folder.mkdir()
        marker = self.dest / stage_install.MARKER
        record = json.loads(marker.read_text(encoding="utf-8"))
        record["integration"] += [str(precious), str(folder), "relative/Nakagawa Recomp.lnk"]
        marker.write_text(json.dumps(record), encoding="utf-8")
        summary = stage_install.uninstall(self.dest)
        self.assertTrue(precious.is_file())
        self.assertTrue(folder.is_dir())
        self.assertEqual(summary["skipped_integration"],
                         [str(precious), str(folder), "relative/Nakagawa Recomp.lnk"])
        self.assertFalse(self.lnk.exists(), "the entry this tool wrote still goes")

    def test_a_shortcut_already_deleted_by_hand_is_not_an_error(self):
        self.lnk.unlink()
        summary = stage_install.uninstall(self.dest)
        self.assertEqual(summary["removed_integration"], [])
        self.assertFalse(self.dest.exists())

    def test_half_finished_staging_and_old_copies_go(self):
        (self.dest / ".staging-1234").mkdir()
        (self.dest / ".staging-1234" / "x").write_bytes(b"x")
        (self.dest / "bin.old").mkdir()
        stage_install.uninstall(self.dest)
        self.assertFalse(self.dest.exists())

    def test_portable_data_and_markers_stay(self):
        (self.dest / "bin" / "portable.txt").write_text("", encoding="utf-8")
        (self.dest / "bin" / "data" / "saves").mkdir(parents=True)
        save = self.dest / "bin" / "data" / "saves" / "UCUS00000.sav"
        save.write_bytes(b"save")
        summary = stage_install.uninstall(self.dest)
        self.assertEqual(save.read_bytes(), b"save")
        self.assertTrue((self.dest / "bin" / "portable.txt").is_file())
        self.assertFalse((self.dest / "bin" / "SDL3.dll").exists())
        self.assertFalse((self.dest / "bin" / "nakagawa_player.exe").exists())
        self.assertFalse((self.dest / "source").exists())
        self.assertTrue((self.dest / stage_install.MARKER).is_file())
        self.assertEqual(sorted(Path(kept).name for kept in summary["kept"]),
                         ["data", "portable.txt"])

    def test_a_second_uninstall_of_a_finished_one_is_refused(self):
        stage_install.uninstall(self.dest)
        with self.assertRaisesRegex(stage_install.InstallError, "INSTALL_DESTINATION_NOT_OWNED"):
            stage_install.uninstall(self.dest)

    def test_a_link_inside_the_install_is_unlinked_not_followed(self):
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "keep.txt").write_text("keep", encoding="utf-8")
        try:
            os.symlink(outside, self.dest / "source" / "escape", target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("this host cannot create symbolic links")
        stage_install.uninstall(self.dest)
        self.assertEqual((outside / "keep.txt").read_text(encoding="utf-8"), "keep")

    @unittest.skipUnless(os.name == "nt", "directory junctions are a Windows feature")
    def test_a_junction_inside_the_install_is_removed_not_followed(self):
        # A junction needs no privilege on Windows, so this runs where the symlink test is skipped.
        outside = self.root / "junction-target"
        outside.mkdir()
        (outside / "keep.txt").write_text("keep", encoding="utf-8")
        junction = self.dest / "source" / "escape"
        made = subprocess.run(["cmd", "/c", "mklink", "/J", str(junction), str(outside)],
                              capture_output=True, text=True, check=False)
        self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
        stage_install.uninstall(self.dest)
        self.assertEqual((outside / "keep.txt").read_text(encoding="utf-8"), "keep")
        self.assertFalse(junction.exists())

    def _rewrite_marker(self, change):
        marker = self.dest / stage_install.MARKER
        record = json.loads(marker.read_text(encoding="utf-8"))
        change(record)
        marker.write_text(json.dumps(record), encoding="utf-8")

    def _refused_untouched(self, dest):
        before = sorted(str(path) for path in dest.rglob("*"))
        with self.assertRaisesRegex(stage_install.InstallError, "INSTALL_MARKER_INVALID"):
            stage_install.uninstall(dest)
        self.assertEqual(sorted(str(path) for path in dest.rglob("*")), before)
        self.assertTrue(self.lnk.is_file())

    def test_a_marker_that_is_not_json_is_refused_untouched(self):
        (self.dest / stage_install.MARKER).write_text("not json {", encoding="utf-8")
        self._refused_untouched(self.dest)

    def test_a_marker_that_is_not_an_object_is_refused_untouched(self):
        (self.dest / stage_install.MARKER).write_text("[]", encoding="utf-8")
        self._refused_untouched(self.dest)

    def test_a_marker_without_the_keys_the_installer_writes_is_refused_untouched(self):
        (self.dest / stage_install.MARKER).write_text("{}", encoding="utf-8")
        self._refused_untouched(self.dest)

    def test_a_marker_listing_a_path_outside_the_install_is_refused_untouched(self):
        for change in (
            lambda r: r.update(player="bin/../../elsewhere.exe"),
            lambda r: r.update(player="/abs/nakagawa_player.exe"),
            lambda r: r.update(runtime_files=["..\\..\\evil.dll"]),
            lambda r: r.update(runtime_files=["sub/evil.dll"]),
            lambda r: r.update(schema_version="2"),
            lambda r: r.update(schema_version=stage_install.SCHEMA_VERSION + 1),
            lambda r: r.update(integration="not-a-list"),
        ):
            with self.subTest(change=change):
                (self.dest / stage_install.MARKER).unlink()
                stage_install._write_marker(self.dest, {
                    "schema_version": stage_install.SCHEMA_VERSION, "source_commit": "x",
                    "platform": "windows", "player": "bin/nakagawa_player.exe",
                    "runtime_files": [], "integration": [str(self.lnk)]})
                self._rewrite_marker(change)
                self._refused_untouched(self.dest)

    def test_a_drive_root_or_the_home_folder_is_refused(self):
        root = Path(self.root.anchor)
        for folder in (root, Path.home(), Path.home().parent):
            with self.subTest(folder=str(folder)), \
                    self.assertRaisesRegex(stage_install.InstallError, "UNINSTALL_DESTINATION_REFUSED"):
                stage_install.plan_uninstall(folder)

    def test_portable_data_in_a_leftover_old_bin_stays(self):
        old = self.dest / "bin.old"
        (old / "data" / "saves").mkdir(parents=True)
        (old / "data" / "saves" / "UCUS00000.sav").write_bytes(b"save")
        (old / "portable.txt").write_text("", encoding="utf-8")
        (old / "SDL3.dll").write_bytes(b"old")
        summary = stage_install.uninstall(self.dest)
        self.assertEqual((old / "data" / "saves" / "UCUS00000.sav").read_bytes(), b"save")
        self.assertTrue((old / "portable.txt").is_file())
        self.assertFalse((old / "SDL3.dll").exists())
        self.assertTrue((self.dest / stage_install.MARKER).is_file())
        self.assertEqual(sorted(Path(kept).name for kept in summary["kept"]),
                         ["data", "portable.txt"])

    def test_the_install_goes_before_its_shortcuts(self):
        order = []
        real = stage_install._remove_path

        def spy(path):
            order.append(Path(path))
            real(path)

        with mock.patch.object(stage_install, "_remove_path", spy):
            stage_install.uninstall(self.dest)
        self.assertEqual(order[-1], self.lnk)
        self.assertEqual(order[0], self.dest / "bin")

    def test_the_command_line_reports_json_and_flags_misuse(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = stage_install.main(["--uninstall", "--dest", str(self.dest), "--dry-run"])
        self.assertEqual(code, 0)
        event = json.loads(out.getvalue())
        self.assertEqual(event["event"], "uninstall")
        self.assertTrue(event["dry_run"])
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = stage_install.main(["--uninstall", "--dest", str(self.root / "nothing")])
        self.assertEqual(code, 1)
        self.assertIn("INSTALL_DESTINATION_NOT_OWNED", err.getvalue())
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            stage_install.main(["--dry-run", "--dest", str(self.dest)])


class ReleaseAssetContractTests(unittest.TestCase):
    """One player ZIP per platform, with every other file droppable by name (#1876)."""

    def _codes(self, names):
        return sorted(problem.split(":", 1)[0] for problem in stage_install.check_release_assets(names))

    def test_the_documented_release_passes(self):
        self.assertEqual(stage_install.check_release_assets(_GOOD_ASSETS), [])
        # The signature is optional: a release without it falls back to its page in the player.
        self.assertEqual(stage_install.check_release_assets([n for n in _GOOD_ASSETS if "minisig" not in n]), [])

    def test_the_recipe_names_what_the_contract_accepts(self):
        recipe = (TOOLS.parent / "docs" / "PREVIEW_RELEASE.md").read_text(encoding="utf-8")
        for template in ("nakagawa-recomp-$Version-windows-x64.zip", "nakagawa-recomp-$Version-prerequisite-sources.zip"):
            self.assertIn(template, recipe)
        self.assertIn("--check-assets", recipe)
        self.assertIn("nakagawa-recomp-$Version-windows-x64-setup.exe", recipe)

    def test_the_installer_is_allowed_only_beside_the_zip(self):
        # #2208: a launcher ranks a ZIP ahead of an .exe, so the ZIP stays the one candidate.
        self.assertEqual(stage_install.check_release_assets(_GOOD_ASSETS + _SETUP_ASSETS), [])
        self.assertEqual(stage_install.check_release_assets(_GOOD_ASSETS + _SETUP_ASSETS + [f"{_SETUP}.sha256.minisig"]),
                         [])
        self.assertEqual(self._codes(_GOOD_ASSETS + [_SETUP]), ["ASSET_CHECKSUM_MISSING"])
        self.assertEqual(self._codes(_GOOD_ASSETS + [f"{_SETUP}.sha256"]), ["ASSET_ORPHAN"])
        self.assertEqual(self._codes(_GOOD_ASSETS + _SETUP_ASSETS + [f"{_SETUP}.sha256.minisig"][:0]
                                     + ["nakagawa-recomp-0.0.2-windows-x64-setup.exe.bak"]), ["ASSET_NAME_UNKNOWN"])
        # Without the ZIP the installer would be the only download, which no launcher or update flow expects.
        no_zip = [n for n in _GOOD_ASSETS if not n.startswith(_PLAYER)] + _SETUP_ASSETS
        self.assertEqual(self._codes(no_zip), ["ASSET_INSTALLER_WITHOUT_ZIP", "ASSET_PLAYER_MISSING"])
        # The installer carries the ZIP's version.
        other = "nakagawa-recomp-0.0.1-windows-x64-setup.exe"
        self.assertEqual(self._codes(_GOOD_ASSETS + [other, f"{other}.sha256"]),
                         ["ASSET_INSTALLER_WITHOUT_ZIP", "ASSET_VERSION_MIXED"])
        # Only the contract's installer name: a second installer, another platform's or an msi are not listed.
        self.assertEqual(self._codes(_GOOD_ASSETS + _SETUP_ASSETS + ["nakagawa-recomp-0.0.2-windows-x64-setup.msi"]),
                         ["ASSET_FILTER_MISMATCH", "ASSET_SECOND_ARCHIVE"])
        self.assertEqual(self._codes(_GOOD_ASSETS + _SETUP_ASSETS + ["nakagawa-recomp-0.0.2-windows-arm64-setup.exe"]),
                         ["ASSET_FILTER_MISMATCH", "ASSET_SECOND_ARCHIVE"])

    def test_two_archives_for_one_platform_fail(self):
        self.assertEqual(self._codes(_GOOD_ASSETS + ["nakagawa-recomp-0.0.2-windows-x64.zip.bak"]),
                         ["ASSET_NAME_UNKNOWN"])
        self.assertEqual(self._codes(_GOOD_ASSETS + ["nakagawa-recomp-0.0.2-portable.zip"]),
                         ["ASSET_FILTER_MISMATCH", "ASSET_SECOND_ARCHIVE"])
        self.assertEqual(self._codes(_GOOD_ASSETS + ["nakagawa-recomp-0.0.2-windows-x64-installer.exe"]),
                         ["ASSET_FILTER_MISMATCH", "ASSET_SECOND_ARCHIVE"])

    def test_a_missing_platform_token_fails(self):
        names = ["nakagawa-recomp-0.0.2.zip", "nakagawa-recomp-0.0.2.zip.sha256"]
        self.assertIn("ASSET_PLAYER_MISSING", self._codes(names))
        self.assertIn("ASSET_SECOND_ARCHIVE", self._codes(names))

    def test_a_checksum_a_name_filter_would_keep_fails(self):
        # "checksums.txt" is listed nowhere in the contract, and a name without a checksum word is no checksum file.
        self.assertEqual(self._codes(_GOOD_ASSETS + ["hashes.txt"]), ["ASSET_NAME_UNKNOWN"])
        self.assertEqual(self._codes([_PLAYER, "nakagawa-recomp-0.0.2-windows-x64.zip.hash"]),
                         ["ASSET_CHECKSUM_MISSING", "ASSET_NAME_UNKNOWN", "ASSET_SOURCE_MISSING"])

    def test_a_player_name_a_filter_would_drop_fails(self):
        names = ["nakagawa-recomp-0.0.2-windows-x64-debug.zip"]
        self.assertTrue(stage_install.is_auxiliary_asset(names[0]))
        self.assertIn("ASSET_PLAYER_MISSING", self._codes(names))

    def test_missing_checksum_orphans_and_mixed_versions_fail(self):
        self.assertEqual(self._codes(_GOOD_ASSETS[:1] + _GOOD_ASSETS[3:]), ["ASSET_CHECKSUM_MISSING"])
        self.assertEqual(self._codes([n for n in _GOOD_ASSETS if n != _PLAYER]),
                         ["ASSET_ORPHAN", "ASSET_ORPHAN", "ASSET_PLAYER_MISSING"])
        mixed = "nakagawa-recomp-0.0.1-prerequisite-sources.zip"
        self.assertEqual(self._codes(_GOOD_ASSETS[:3] + [mixed, f"{mixed}.sha256"]), ["ASSET_VERSION_MIXED"])

    def test_the_source_archive_and_its_checksum_are_required(self):
        # Review of #1915: a ZIP and its checksum alone, or a source archive without its checksum, passed.
        self.assertEqual(self._codes(_GOOD_ASSETS[:2]), ["ASSET_SOURCE_MISSING"])
        self.assertEqual(self._codes(_GOOD_ASSETS[:4]), ["ASSET_CHECKSUM_MISSING"])
        self.assertEqual(self._codes(_GOOD_ASSETS + ["nakagawa-recomp-0.0.2-prerequisite-sources.zip.bak"]),
                         ["ASSET_NAME_UNKNOWN"])

    def test_a_folder_that_also_holds_the_staged_tree_is_named_by_files_instead(self):
        with tempfile.TemporaryDirectory() as raw:
            folder = Path(raw)
            (folder / "nakagawa-recomp-0.0.2-windows-x64").mkdir()
            files = []
            for name in _GOOD_ASSETS:
                (folder / name).write_bytes(b"")
                files.append(str(folder / name))
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(stage_install.main(["--check-assets", str(folder)]), 1)
            self.assertEqual(stage_install.main(["--check-assets", *files]), 0)

    def test_a_second_platform_is_two_archives_only_when_the_contract_lists_it(self):
        self.assertEqual(self._codes(_GOOD_ASSETS + ["nakagawa-recomp-0.0.2-linux-x64.zip"]),
                         ["ASSET_FILTER_MISMATCH", "ASSET_SECOND_ARCHIVE"])

    def test_the_command_line_reads_a_release_folder(self):
        with tempfile.TemporaryDirectory() as raw:
            folder = Path(raw)
            for name in _GOOD_ASSETS:
                (folder / name).write_bytes(b"")
            self.assertEqual(stage_install.main(["--check-assets", str(folder)]), 0)
            (folder / "nakagawa-recomp-0.0.2-windows-arm64.zip").write_bytes(b"")
            with contextlib.redirect_stderr(io.StringIO()) as stderr:
                self.assertEqual(stage_install.main(["--check-assets", str(folder)]), 1)
            self.assertIn("ASSET_SECOND_ARCHIVE", stderr.getvalue())


class ReproducibleBuildTests(unittest.TestCase):
    def test_player_links_carry_the_reproducible_link_flag(self):
        """Without it the PE header holds the link time and two builds of one commit differ (#324)."""
        makefile = read_makefile(stage_install.ROOT)
        for target in ("$(PLAYER_EXE):", "$(PLAYER_UI_TEST_EXE):"):
            start = makefile.index(f"\n{target} $(PLAYER_SRCS)")
            recipe = makefile[start:makefile.index("\n\n", start)]
            self.assertIn("$(REPRODUCIBLE_LINK_FLAG)", recipe, target)


if __name__ == "__main__":
    unittest.main()
