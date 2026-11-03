#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The player and title executables carry a VERSIONINFO that names them for Windows.

Task Manager and Explorer show the FileDescription of the executable's VERSIONINFO and
fall back to the file name when there is none (Tracks #2001).
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import win_version_resource as wvr  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
TITLE_MANIFEST = ROOT / "assets" / "titles" / "hst-ucus98701.json"


class RenderTests(unittest.TestCase):
    def test_version_is_the_one_the_about_page_shows(self) -> None:
        version = wvr.read_version()
        header = (ROOT / "src" / "player" / "ui" / "player_ui_nk.h").read_text(encoding="utf-8")
        self.assertIn(f'#define NK_PLAYER_VERSION "{version}"', header)
        manifest = json.loads((ROOT / "assets" / "release_manifest.json").read_text("utf-8"))
        self.assertEqual(manifest["version"], version, "the release manifest and the player disagree")

    def test_numeric_version_has_four_fields(self) -> None:
        self.assertEqual(wvr.numeric_version("0.0.2-dev"), (0, 0, 2, 0))
        self.assertEqual(wvr.numeric_version("1.20.300"), (1, 20, 300, 0))
        for bad in ("dev", "1.2", "1.2.70000"):
            with self.assertRaises(wvr.ResourceError, msg=bad):
                wvr.numeric_version(bad)

    def test_player_script_names_every_field(self) -> None:
        text = wvr.render("Nakagawa Recomp", "nakagawa_player.exe", "0.0.2-dev")
        for expected in (
            '#include "nakagawa.rc"',
            "1 VERSIONINFO",
            "FILEVERSION 0,0,2,0",
            "PRODUCTVERSION 0,0,2,0",
            'VALUE "FileDescription", "Nakagawa Recomp"',
            'VALUE "ProductName", "Nakagawa Recomp"',
            'VALUE "CompanyName", "the Nakagawa Recomp authors"',
            'VALUE "LegalCopyright", "Copyright (C) 2026 the Nakagawa Recomp authors"',
            'VALUE "OriginalFilename", "nakagawa_player.exe"',
            'VALUE "FileVersion", "0.0.2-dev"',
            'VALUE "ProductVersion", "0.0.2-dev"',
        ):
            self.assertIn(expected, text)
        self.assertIn("FILEFLAGS 0x2L", text, "a -dev build is marked pre-release")
        self.assertIn("FILEFLAGS 0x0L", wvr.render("x", "x.exe", "1.0.0"))

    def test_script_is_reproducible_and_plain_ascii(self) -> None:
        first = wvr.render("Nakagawa Recomp", "nakagawa_player.exe", "0.0.2-dev")
        self.assertEqual(first, wvr.render("Nakagawa Recomp", "nakagawa_player.exe", "0.0.2-dev"))
        first.encode("ascii")
        self.assertNotIn("\r", first)
        self.assertNotIn(str(ROOT), first, "no absolute path in the script")

    def test_quotes_and_backslashes_are_escaped_and_odd_text_refused(self) -> None:
        self.assertEqual(wvr._rc_string('a "b" c\\d'), '"a ""b"" c\\\\d"')
        for bad in ("", "tab\there", "café"):
            with self.assertRaises(wvr.ResourceError, msg=repr(bad)):
                wvr._rc_string(bad)


class CommandLineTests(unittest.TestCase):
    def run_tool(self, out: Path, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-I", str(ROOT / "tools" / "win_version_resource.py"),
             "--output", str(out), *args],
            capture_output=True, text=True, check=False)

    def test_title_description_comes_from_the_manifest(self) -> None:
        display_name = json.loads(TITLE_MANIFEST.read_text("utf-8"))["display_name"]
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "sub" / "nakagawa_icon.rc"
            done = self.run_tool(out, "--manifest", str(TITLE_MANIFEST),
                                 "--original-filename", "hst-ucus98701-v1.exe")
            self.assertEqual(done.returncode, 0, done.stderr)
            text = out.read_text("ascii")
        self.assertIn(f'VALUE "FileDescription", "{display_name} (Nakagawa Recomp)"', text)
        self.assertIn('VALUE "OriginalFilename", "hst-ucus98701-v1.exe"', text)
        self.assertIn('VALUE "ProductName", "Nakagawa Recomp"', text)

    def test_title_without_a_manifest_uses_the_given_name(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "t.rc"
            self.assertEqual(self.run_tool(out, "--title-name", "mygame",
                                           "--original-filename", "mygame.exe").returncode, 0)
            self.assertIn('"mygame (Nakagawa Recomp)"', out.read_text("ascii"))

    def test_a_manifest_without_a_name_fails_the_build(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "title.json"
            bad.write_text("{}", encoding="utf-8")
            done = self.run_tool(Path(tmp) / "t.rc", "--manifest", str(bad),
                                 "--original-filename", "x.exe")
            self.assertEqual(done.returncode, 1)
            self.assertIn("no display_name", done.stderr)
            self.assertFalse((Path(tmp) / "t.rc").exists())

    def test_unchanged_script_is_not_rewritten(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "t.rc"
            args = ("--player", "--original-filename", "nakagawa_player.exe")
            self.assertEqual(self.run_tool(out, *args).returncode, 0)
            before = out.stat().st_mtime_ns
            self.assertEqual(self.run_tool(out, *args).returncode, 0)
            self.assertEqual(out.stat().st_mtime_ns, before)


class MakefileWiringTests(unittest.TestCase):
    def test_player_and_titles_get_their_own_generated_script(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("$(BUILD_ROOT)/nakagawa_icon.rc: $(NK_VERSION_RESOURCE_DEPS)", makefile)
        self.assertIn("$(BUILD_DIR)/nakagawa_icon.rc: $(NK_VERSION_RESOURCE_DEPS)", makefile)
        self.assertIn('--player --original-filename "$(notdir $(PLAYER_EXE))"', makefile)
        self.assertIn('--manifest "$(strip $(TITLE_MANIFEST))"', makefile)
        self.assertIn('--original-filename "$(GAME_NAME)$(EXE_EXT)"', makefile)
        self.assertIn("%/nakagawa_icon.o: %/nakagawa_icon.rc assets/brand/nakagawa.rc", makefile)
        self.assertIn("--include-dir=assets/brand", makefile)


@unittest.skipUnless(sys.platform == "win32", "SKIP: Windows reads VERSIONINFO; other hosts link no resource")
class WindowsExecutableTests(unittest.TestCase):
    """Compile the generated script and link a tiny program, as the Makefile does."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.windres, cls.gcc = shutil.which("windres"), shutil.which("gcc")
        cls.powershell = shutil.which("pwsh") or shutil.which("powershell")
        if not (cls.windres and cls.gcc and cls.powershell):
            raise unittest.SkipTest("SKIP: windres, gcc and PowerShell are needed")

    def build_exe(self, tmp: Path, name: str, *tool_args: str) -> Path:
        rc, obj, src, exe = (tmp / "nakagawa_icon.rc", tmp / "nakagawa_icon.o",
                             tmp / "main.c", tmp / name)
        subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "win_version_resource.py"),
                        "--output", str(rc), "--original-filename", name, *tool_args], check=True)
        subprocess.run([self.windres, "--input-format=rc", "--output-format=coff",
                        f"--include-dir={ROOT / 'assets' / 'brand'}", "-i", str(rc), "-o", str(obj)],
                       check=True)
        src.write_text("int main(void) { return 0; }\n", encoding="ascii")
        subprocess.run([self.gcc, str(src), str(obj), "-o", str(exe)], check=True)
        return exe

    def version_info(self, exe: Path) -> dict[str, str]:
        command = ("(Get-Item -LiteralPath $env:NK_VI_EXE).VersionInfo | "
                   "Select-Object FileDescription,ProductName,FileVersion,ProductVersion,"
                   "CompanyName,LegalCopyright,OriginalFilename | ConvertTo-Json")
        done = subprocess.run([self.powershell, "-NoProfile", "-Command", command],
                              capture_output=True, text=True, check=True,
                              env={**os.environ, "NK_VI_EXE": str(exe)})
        return json.loads(done.stdout)

    def test_player_executable_reports_its_friendly_name(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            exe = self.build_exe(Path(tmp), "nakagawa_player.exe", "--player")
            info = self.version_info(exe)
            self.assertEqual(info["FileDescription"], "Nakagawa Recomp")
            self.assertEqual(info["ProductName"], "Nakagawa Recomp")
            self.assertEqual(info["FileVersion"], wvr.read_version())
            self.assertEqual(info["ProductVersion"], wvr.read_version())
            self.assertEqual(info["CompanyName"], "the Nakagawa Recomp authors")
            self.assertEqual(info["OriginalFilename"], "nakagawa_player.exe")

    def test_title_executable_reports_the_title_name(self) -> None:
        display_name = json.loads(TITLE_MANIFEST.read_text("utf-8"))["display_name"]
        with tempfile.TemporaryDirectory() as tmp:
            exe = self.build_exe(Path(tmp), "hst-ucus98701-v1.exe", "--manifest", str(TITLE_MANIFEST))
            info = self.version_info(exe)
        self.assertEqual(info["FileDescription"], f"{display_name} (Nakagawa Recomp)")
        self.assertEqual(info["OriginalFilename"], "hst-ucus98701-v1.exe")

    def test_the_icon_survives_next_to_the_version(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.build_exe(Path(tmp), "nakagawa_player.exe", "--player")
            dump = subprocess.run([self.windres, "--input-format=coff", "--output-format=rc",
                                   "-i", str(Path(tmp) / "nakagawa_icon.o")],
                                  capture_output=True, text=True, check=True).stdout
        self.assertIn("RT_GROUP_ICON", dump)
        self.assertIn("VERSIONINFO", dump)


if __name__ == "__main__":
    unittest.main()
