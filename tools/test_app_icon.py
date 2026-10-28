#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The app icon: the committed ``.ico``, the C drawer and the build wiring agree."""

from __future__ import annotations

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
        self.assertEqual(pixel(15, 50), (0xD7, 0xF2, 0x4A, 255), "lime block, bottom left")
        self.assertEqual(pixel(47, 32), (0xF5, 0xF7, 0xFA, 255), "paper bar, right")
        self.assertEqual(pixel(14, 14), (0xF5, 0xF7, 0xFA, 255),
                         "the diagonal crosses the top block, as in the SVG")

    def test_bmp_images_mask_only_transparent_pixels(self) -> None:
        data = (ROOT / make_app_icon.OUTPUT).read_bytes()
        rgba = make_app_icon.render(16)
        self.assertIn(make_app_icon._bmp(16, rgba), data)

    def test_build_ico_formatting_uses_list_join(self) -> None:
        """Verify build_ico directory entry formatting produces valid ico bytes."""
        ico = make_app_icon.build_ico()
        self.assertTrue(ico.startswith(bytes([0, 0, 1, 0])))


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
            colour = {"NK_UI_ACCENT_RGBA": (0xD7, 0xF2, 0x4A), "NK_UI_PAPER_RGBA": (0xF5, 0xF7, 0xFA)}[rgba]
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
        self.assertIn("%/nakagawa_icon.o: assets/brand/nakagawa.rc assets/brand/nakagawa.ico", makefile)

        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("target_sources(nakagawa_player PRIVATE assets/brand/nakagawa.rc)", cmake)

    def test_other_systems_set_the_window_icon_at_run_time(self) -> None:
        main = (ROOT / "src" / "player" / "main.c").read_text(encoding="utf-8")
        self.assertIn("nk_ui_app_icon_rgba(sizes[i]", main)
        self.assertIn("SDL_SetWindowIcon(window, icon)", main)
        self.assertIn("    player_set_window_icon(window);\n", main)


if __name__ == "__main__":
    unittest.main()
