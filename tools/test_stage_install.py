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
        return out

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


class ReproducibleBuildTests(unittest.TestCase):
    def test_player_links_carry_the_reproducible_link_flag(self):
        """Without it the PE header holds the link time and two builds of one commit differ (#324)."""
        makefile = (stage_install.ROOT / "Makefile").read_text(encoding="utf-8")
        for target in ("$(PLAYER_EXE):", "$(PLAYER_UI_TEST_EXE):"):
            start = makefile.index(f"\n{target} $(PLAYER_SRCS)")
            recipe = makefile[start:makefile.index("\n\n", start)]
            self.assertIn("$(REPRODUCIBLE_LINK_FLAG)", recipe, target)


if __name__ == "__main__":
    unittest.main()
