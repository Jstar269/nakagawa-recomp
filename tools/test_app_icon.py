#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The app icon: the committed ``.ico``, the C drawer and the build wiring agree."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_app_icon  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
WIDGETS_C = ROOT / "src" / "ui" / "nk_ui_widgets.c"

_DUMP_C = r"""
#include "nk_ui_widgets.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    static unsigned char rgba[NK_UI_APP_ICON_MAX * NK_UI_APP_ICON_MAX * 4];
    int size = argc > 2 ? atoi(argv[1]) : 0;
    FILE *out;
    if (!nk_ui_app_icon_rgba(size, rgba, (size_t)size * 4u)) return 2;
    out = fopen(argv[2], "wb");
    if (out == NULL) return 3;
    fwrite(rgba, 1, (size_t)size * (size_t)size * 4u, out);
    return fclose(out) == 0 ? 0 : 4;
}
"""


def _make_list(name: str) -> list[str]:
    """The sources a mk/nk_ui.mk variable names, following $(VAR) references."""
    text = (ROOT / "mk" / "nk_ui.mk").read_text(encoding="utf-8").replace("\\\n", " ")
    match = re.search(rf"^{name} :=(.*)$", text, re.M)
    if match is None:
        raise AssertionError(f"{name} is not in mk/nk_ui.mk")
    sources: list[str] = []
    for word in match.group(1).split():
        reference = re.fullmatch(r"\$\((\w+)\)", word)
        sources += _make_list(reference.group(1)) if reference else [word]
    return sources


class CommittedIconTests(unittest.TestCase):
    def setUp(self) -> None:
        self.images = make_app_icon.read_ico((ROOT / make_app_icon.OUTPUT).read_bytes())

    def test_every_size_is_present_in_the_expected_format(self) -> None:
        self.assertEqual([size for size, _, _ in self.images], list(make_app_icon.SIZES))
        for size, kind, _ in self.images:
            self.assertEqual(kind, "bmp" if size <= make_app_icon.LARGEST_BMP else "png", size)

    def test_committed_icon_is_a_fresh_drawing_of_the_brand_svg(self) -> None:
        for size, _, rgba in self.images:
            self.assertEqual(rgba, make_app_icon.render(size),
                             f"{size} px differs; run python tools/make_app_icon.py")

    def test_the_mark_sits_on_a_rounded_ink_tile(self) -> None:
        size, _, rgba = self.images[make_app_icon.SIZES.index(64)]

        def pixel(x: int, y: int) -> tuple[int, ...]:
            return tuple(rgba[(y * size + x) * 4:(y * size + x) * 4 + 4])

        self.assertEqual(pixel(0, 0)[3], 0, "the rounded corner stays transparent")
        self.assertEqual(pixel(32, 3), (0x10, 0x15, 0x1C, 255), "ink above the mark")
        self.assertEqual(pixel(15, 50), (0x6B, 0x9F, 0xF0, 255), "blue block, bottom left")
        self.assertEqual(pixel(47, 32), (0xF5, 0xF7, 0xFA, 255), "paper bar, right")
        self.assertEqual(pixel(14, 14), (0xF5, 0xF7, 0xFA, 255),
                         "the diagonal crosses the top block, as in the SVG")

    def test_bmp_images_mask_only_transparent_pixels(self) -> None:
        data = (ROOT / make_app_icon.OUTPUT).read_bytes()
        rgba = make_app_icon.render(16)
        self.assertIn(make_app_icon._bmp(16, rgba), data)


class CDrawerTests(unittest.TestCase):
    def test_c_drawer_uses_the_generator_numbers(self) -> None:
        text = WIDGETS_C.read_text(encoding="utf-8")

        def define(name: str) -> str:
            match = re.search(rf"^#define {name} (\S+)$", text, re.M)
            self.assertIsNotNone(match, name)
            assert match is not None
            return match.group(1)

        tile = int(define("NK_UI_APP_ICON_TILE_RGB").rstrip("u"), 16)
        self.assertEqual(tile, int.from_bytes(bytes(make_app_icon.TILE_RGB), "big"))
        self.assertEqual(float(define("NK_UI_APP_ICON_CORNER")), make_app_icon.CORNER_FRACTION)
        self.assertEqual(float(define("NK_UI_APP_ICON_INSET")), make_app_icon.INSET_FRACTION)
        self.assertEqual(int(define("NK_UI_APP_ICON_CORNER_SAMPLES")), make_app_icon.CORNER_SAMPLES)

    def test_c_mark_is_the_brand_svg(self) -> None:
        text = WIDGETS_C.read_text(encoding="utf-8")
        block = text.split("static const NkUiShape g_n1_mark[] = {", 1)[1].split("};", 1)[0]
        shapes = []
        for kind, rgba, numbers in re.findall(
                r"\{NK_UI_SHAPE_(RECT|POLY), (\w+), 0\.0f, \d+u,\s*\{([^}]*)\}\}", block):
            values = [float(v.strip().rstrip("f")) for v in numbers.split(",")]
            if kind == "RECT":
                x, y, w, h = values
                points = [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]
            else:
                points = list(zip(values[0::2], values[1::2], strict=True))
            value = re.search(r"#define " + rgba + r" 0x([0-9A-Fa-f]{6})FFu", text)
            self.assertIsNotNone(value, rgba)
            assert value is not None
            colour = tuple(bytes.fromhex(value.group(1)))
            shapes.append((colour, points))
        _, svg_shapes = make_app_icon.load_mark()
        self.assertEqual(len(shapes), len(svg_shapes))
        for (colour, points), (svg_colour, svg_points) in zip(shapes, svg_shapes, strict=True):
            self.assertEqual(colour, svg_colour)
            for (x, y), (sx, sy) in zip(points, svg_points, strict=True):
                self.assertAlmostEqual(x, sx, places=4)
                self.assertAlmostEqual(y, sy, places=4)

    def test_c_drawer_draws_the_committed_pixels(self) -> None:
        compiler = shutil.which("gcc") or shutil.which("cc")
        if compiler is None:
            self.skipTest("SKIP: no C compiler on PATH to build the nk_ui drawer")
        sources = [str(ROOT / path) for path in _make_list("NK_UI_NATIVE_CORE_SRCS")]
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            driver = work / "icon_dump.c"
            driver.write_text(_DUMP_C, encoding="utf-8")
            exe = work / ("icon_dump.exe" if sys.platform == "win32" else "icon_dump")
            subprocess.run([compiler, "-std=c99", "-O1", f"-I{ROOT / 'src' / 'ui'}",
                            f"-I{ROOT / 'third_party' / 'clay'}", *sources, str(driver), "-lm",
                            "-o", str(exe)], check=True, capture_output=True)
            committed = {size: rgba for size, _, rgba in
                         make_app_icon.read_ico((ROOT / make_app_icon.OUTPUT).read_bytes())}
            for size in make_app_icon.SIZES:
                out = work / f"{size}.rgba"
                subprocess.run([str(exe), str(size), str(out)], check=True)
                drawn = out.read_bytes()
                self.assertEqual(len(drawn), len(committed[size]))
                # The C mark stores floats and the SVG decimals, so a channel may round
                # the other way; nothing more.
                worst = max((abs(a - b) for a, b in zip(drawn, committed[size], strict=True)), default=0)
                self.assertLessEqual(worst, 1, f"{size} px")


class BuildWiringTests(unittest.TestCase):
    def test_resource_script_names_the_icon_beside_it(self) -> None:
        script = (ROOT / "assets" / "brand" / "nakagawa.rc").read_text(encoding="utf-8")
        self.assertIn('1 ICON "nakagawa.ico"', script)
        self.assertEqual(Path(make_app_icon.OUTPUT).parent, Path("assets/brand"))

    def test_windows_player_and_titles_link_the_icon(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        windows = makefile.split("ifeq ($(OS),Windows_NT)\nPLAYER_PLATFORM_SRC", 1)[1].split("\nelse\n", 1)
        self.assertIn("PLAYER_ICON_OBJ     := $(BUILD_ROOT)/nakagawa_icon.o", windows[0])
        self.assertIn("TITLE_ICON_OBJ      = $(BUILD_DIR)/nakagawa_icon.o", windows[0])
        self.assertIn("PLAYER_ICON_OBJ     :=\nTITLE_ICON_OBJ      :=\n", windows[1])
        player_rule = makefile.split("\n$(PLAYER_EXE): $(PLAYER_SRCS)", 1)[1].split("\n\n", 1)[0]
        self.assertIn("$(PLAYER_SRCS) $(PLAYER_ICON_OBJ) -lSDL3", player_rule)
        link_objects = makefile.split("\nLINK_OBJECTS = ", 1)[1].split("\n", 1)[0].split()
        self.assertIn("$(TITLE_ICON_OBJ)", link_objects)
        self.assertIn("%/nakagawa_icon.o: %/nakagawa_icon.rc assets/brand/nakagawa.rc assets/brand/nakagawa.ico", makefile)

        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("target_sources(nakagawa_player PRIVATE assets/brand/nakagawa.rc)", cmake)
        self.assertIn("tools/make_app_icon.py", cmake)
        self.assertIn('--version-rc "${NK_PLAYER_RC}"', cmake)

    def test_other_systems_set_the_window_icon_at_run_time(self) -> None:
        # The icon is built in the player's host seam; main.c only asks for it.
        host = (ROOT / "src" / "player" / "player_host.c").read_text(encoding="utf-8")
        self.assertIn("nk_ui_app_icon_rgba(sizes[i]", host)
        self.assertIn("SDL_SetWindowIcon(window, icon)", host)
        main = (ROOT / "src" / "player" / "main.c").read_text(encoding="utf-8")
        self.assertIn("    player_host_set_window_icon(window);\n", main)


TITLE_MANIFEST = ROOT / "assets" / "titles" / "hst-ucus98701.json"


# Tracks #2001: the VERSIONINFO that names the executables for Task Manager and Explorer.
class VersionInfoScriptTests(unittest.TestCase):
    def test_version_is_the_one_the_about_page_shows(self) -> None:
        version = make_app_icon.read_version()
        header = (ROOT / "src" / "player" / "ui" / "player_ui_nk.h").read_text(encoding="utf-8")
        self.assertIn(f'#define NK_PLAYER_VERSION "{version}"', header)
        manifest = json.loads((ROOT / "assets" / "release_manifest.json").read_text("utf-8"))
        self.assertEqual(manifest["version"], version, "the release manifest and the player disagree")

    def test_numeric_version_has_four_fields(self) -> None:
        self.assertEqual(make_app_icon.numeric_version("0.0.2-dev"), (0, 0, 2, 0))
        self.assertEqual(make_app_icon.numeric_version("1.20.300"), (1, 20, 300, 0))
        for bad in ("dev", "1.2", "1.2.70000"):
            with self.assertRaises(make_app_icon.ResourceError, msg=bad):
                make_app_icon.numeric_version(bad)

    def test_player_script_names_every_field(self) -> None:
        text = make_app_icon.render_version_rc("Nakagawa Recomp", "nakagawa_player.exe", "0.0.2-dev")
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
        self.assertIn("FILEFLAGS 0x0L", make_app_icon.render_version_rc("x", "x.exe", "1.0.0"))

    def test_script_is_reproducible_and_holds_no_host_detail(self) -> None:
        first = make_app_icon.render_version_rc("Nakagawa Recomp", "nakagawa_player.exe", "0.0.2-dev")
        self.assertEqual(first, make_app_icon.render_version_rc("Nakagawa Recomp", "nakagawa_player.exe", "0.0.2-dev"))
        first.encode("utf-8")
        self.assertNotIn("\r", first)
        self.assertNotIn(str(ROOT), first, "no absolute path in the script")

    def test_quotes_and_backslashes_are_escaped_and_odd_text_refused(self) -> None:
        self.assertEqual(make_app_icon._rc_string('a "b" c\\d'), '"a ""b"" c\\\\d"')
        for bad in ("", "tab\there", "lone\ud800surrogate"):
            with self.assertRaises(make_app_icon.ResourceError, msg=repr(bad)):
                make_app_icon._rc_string(bad)
        self.assertEqual(make_app_icon._rc_string("café モン"), '"café モン"')

    def test_script_declares_its_utf8_code_page_before_any_string(self) -> None:
        text = make_app_icon.render_version_rc("café (Nakagawa Recomp)", "x.exe", "1.0.0")
        self.assertLess(text.index("#pragma code_page(65001)"), text.index("VALUE"))


class VersionInfoCommandLineTests(unittest.TestCase):
    def run_tool(self, out: Path, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-I", str(ROOT / "tools" / "make_app_icon.py"),
             "--version-rc", str(out), *args],
            capture_output=True, text=True, check=False)

    def test_title_description_comes_from_the_manifest(self) -> None:
        display_name = json.loads(TITLE_MANIFEST.read_text("utf-8"))["display_name"]
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "sub" / "nakagawa_icon.rc"
            done = self.run_tool(out, "--manifest", str(TITLE_MANIFEST),
                                 "--original-filename", "hst-ucus98701-v1.exe")
            self.assertEqual(done.returncode, 0, done.stderr)
            text = out.read_text("utf-8")
        self.assertIn(f'VALUE "FileDescription", "{display_name} (Nakagawa Recomp)"', text)
        self.assertIn('VALUE "OriginalFilename", "hst-ucus98701-v1.exe"', text)
        self.assertIn('VALUE "ProductName", "Nakagawa Recomp"', text)

    def test_title_without_a_manifest_uses_the_given_name(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "t.rc"
            self.assertEqual(self.run_tool(out, "--title-name", "mygame",
                                           "--original-filename", "mygame.exe").returncode, 0)
            self.assertIn('"mygame (Nakagawa Recomp)"', out.read_text("utf-8"))

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


class VersionInfoMakefileTests(unittest.TestCase):
    def test_player_and_titles_get_their_own_generated_script(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("$(BUILD_ROOT)/nakagawa_icon.rc: $(NK_VERSION_RESOURCE_DEPS)", makefile)
        self.assertIn("$(BUILD_DIR)/nakagawa_icon.rc: $(NK_VERSION_RESOURCE_DEPS)", makefile)
        self.assertIn('--version-rc $@ --original-filename "$(notdir $(PLAYER_EXE))" --player', makefile)
        self.assertIn('--manifest "$(strip $(TITLE_MANIFEST))"', makefile)
        self.assertIn('--original-filename "$(GAME_NAME)$(EXE_EXT)"', makefile)
        self.assertIn("%/nakagawa_icon.o: %/nakagawa_icon.rc assets/brand/nakagawa.rc", makefile)
        self.assertIn("--include-dir=assets/brand", makefile)


@unittest.skipUnless(sys.platform == "win32", "SKIP: Windows reads VERSIONINFO; other hosts link no resource")
class VersionInfoWindowsExecutableTests(unittest.TestCase):
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
        subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "make_app_icon.py"),
                        "--version-rc", str(rc), "--original-filename", name, *tool_args], check=True)
        subprocess.run([self.windres, "--input-format=rc", "--output-format=coff",
                        f"--include-dir={ROOT / 'assets' / 'brand'}", "-i", str(rc), "-o", str(obj)],
                       check=True)
        src.write_text("int main(void) { return 0; }\n", encoding="ascii")
        subprocess.run([self.gcc, str(src), str(obj), "-o", str(exe)], check=True)
        return exe

    def version_info(self, exe: Path) -> dict[str, str]:
        command = ("[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
                   "(Get-Item -LiteralPath $env:NK_VI_EXE).VersionInfo | "
                   "Select-Object FileDescription,ProductName,FileVersion,ProductVersion,"
                   "CompanyName,LegalCopyright,OriginalFilename | ConvertTo-Json")
        done = subprocess.run([self.powershell, "-NoProfile", "-Command", command],
                              capture_output=True, text=True, encoding="utf-8", check=True,
                              env={**os.environ, "NK_VI_EXE": str(exe)})
        return json.loads(done.stdout)

    def test_player_executable_reports_its_friendly_name(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            exe = self.build_exe(Path(tmp), "nakagawa_player.exe", "--player")
            info = self.version_info(exe)
            self.assertEqual(info["FileDescription"], "Nakagawa Recomp")
            self.assertEqual(info["ProductName"], "Nakagawa Recomp")
            self.assertEqual(info["FileVersion"], make_app_icon.read_version())
            self.assertEqual(info["ProductVersion"], make_app_icon.read_version())
            self.assertEqual(info["CompanyName"], "the Nakagawa Recomp authors")
            self.assertEqual(info["OriginalFilename"], "nakagawa_player.exe")

    def test_title_executable_reports_the_title_name(self) -> None:
        display_name = json.loads(TITLE_MANIFEST.read_text("utf-8"))["display_name"]
        with tempfile.TemporaryDirectory() as tmp:
            exe = self.build_exe(Path(tmp), "hst-ucus98701-v1.exe", "--manifest", str(TITLE_MANIFEST))
            info = self.version_info(exe)
        self.assertEqual(info["FileDescription"], f"{display_name} (Nakagawa Recomp)")
        self.assertEqual(info["OriginalFilename"], "hst-ucus98701-v1.exe")

    def test_non_ascii_titles_read_back_exactly(self) -> None:
        for title in ("café", "モンスターハンター", "Tennis éè 🎾"):
            with self.subTest(title=title), tempfile.TemporaryDirectory() as tmp:
                exe = self.build_exe(Path(tmp), "title.exe", "--title-name", title)
                self.assertEqual(self.version_info(exe)["FileDescription"],
                                 f"{title} (Nakagawa Recomp)")

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
