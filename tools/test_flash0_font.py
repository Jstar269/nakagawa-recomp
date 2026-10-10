# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regression for the read-only flash0: font device (src/rt/flash0_font.c).

The driver writes synthetic PGFs with tools/pgf_writer.py into a temporary root: the per-user
cache copy of the latin slot, and the project fonts for the latin and japanese slots. The file
names and the cache layout are read from src/core/nk_font_slots.h, the header the device and the
import flow share, so this driver cannot drift from it. It then runs the HLE selftest's
``--flash0-font`` mode, which drives the production hle.c IO entry points (open, read, seek,
getstat, dopen, dread, dclose and the write-side refusals) against that root. The native test marks
slots measured through its seam, because the console measurement is not in the tree yet.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import pgf_writer  # noqa: E402

SLOT_HEADER = ROOT / "src" / "core" / "nk_font_slots.h"


def header_string(name: str) -> str:
    """The string value of ``#define <name> "<value>"`` in the shared slot header."""
    match = re.search(
        rf'^#define {name} "([^"]+)"$', SLOT_HEADER.read_text(encoding="utf-8"), re.MULTILINE
    )
    if match is None:
        raise AssertionError(f"{name} is not a string define in {SLOT_HEADER.name}")
    return match.group(1)


# The Makefile default (GAME_NAME ?= mygame). It is passed explicitly, spelled the same way,
# so make reuses the ordinary objects and the selftest binary lands at a known path.
BUILD_DIR = "build/mygame"

# hle-thread-selftest-build writes hle_thread_selftest.exe on every host: the output name is
# spelled in the recipe, not taken from EXE_EXT. Only the make program varies by platform.
# The harness includes <windows.h> without a _WIN32 guard and calls Win32 file APIs, so it does
# not build off Windows. Until it is ported, that is a named SKIP there, decided from the source
# before any build (docs/LINUX_DEVELOPMENT.md lists the gap). The check retires itself: once the
# include is guarded, the same test builds the selftest with make.
SELFTEST_SOURCE = ROOT / "src" / "rt" / "hle_thread_selftest.c"


def unguarded_windows_include(source: str) -> bool:
    """True when ``#include <windows.h>`` is outside every conditional that names ``_WIN32``."""
    guards: list[bool] = []
    for line in source.splitlines():
        stripped = line.strip()
        if not stripped.startswith("#"):
            continue
        directive = stripped[1:].strip()
        if re.match(r"if(n?def)?\b", directive):
            guards.append("_WIN32" in directive)
        elif re.match(r"endif\b", directive):
            if guards:
                guards.pop()
        elif re.match(r"include\s*<windows\.h>", directive, re.IGNORECASE) and not any(guards):
            return True
    return False


def synthetic_pgf(glyph_count: int, first_code: int = 0x41) -> bytes:
    """A dense PGF with ``glyph_count`` glyphs starting at ``first_code``."""
    glyphs = [
        pgf_writer.Glyph(
            code=first_code + index,
            width=4,
            height=4,
            samples=tuple((index + sample) % 16 for sample in range(16)),
        )
        for index in range(glyph_count)
    ]
    return pgf_writer.build_pgf(glyphs)


def write_fixture(root: Path) -> dict[str, bytes]:
    """Lay out the cache and project directories; return the bytes each file must serve."""
    cache_parent = header_string("NK_FONT_CACHE_PARENT")
    cache_subdir = header_string("NK_FONT_CACHE_SUBDIR")
    latin = header_string("NK_FONT_SLOT_LATIN_FILE")
    japanese = header_string("NK_FONT_SLOT_JAPANESE_FILE")
    files = {
        # User-imported cache: the source the device must prefer for latin.
        root / "data" / cache_parent / cache_subdir / latin: synthetic_pgf(5),
        # Project fonts: latin is a different size, so the source choice is observable.
        root / "project" / latin: synthetic_pgf(10),
        root / "project" / japanese: synthetic_pgf(3, first_code=0x30),
    }
    for path, data in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return files


class Flash0FontDeviceTests(unittest.TestCase):
    def test_windows_include_detector(self) -> None:
        """The skip below is decided by this reader, so it must tell a guarded include apart."""
        cases = {
            "#include <windows.h>\n": True,
            "#ifdef _WIN32\n#include <windows.h>\n#endif\n": False,
            "#if defined(_WIN32) && !defined(X)\n# include <Windows.h>\n#endif\n": False,
            "#ifdef _WIN32\n#if X\n#endif\n#include <windows.h>\n#endif\n": False,
            "#ifdef _WIN32\n#endif\n#include <windows.h>\n": True,
            "#if SR_FEATURE\n#include <windows.h>\n#endif\n": True,
            "/* #include <windows.h> */\n#include <stdio.h>\n": False,
        }
        for source, expected in cases.items():
            with self.subTest(source=source):
                self.assertIs(unguarded_windows_include(source), expected)

    def test_native_device_contract_on_synthetic_fonts(self) -> None:
        if sys.platform != "win32" and unguarded_windows_include(
            SELFTEST_SOURCE.read_text(encoding="utf-8")
        ):
            raise unittest.SkipTest(
                f"{SELFTEST_SOURCE.name} includes <windows.h> without a _WIN32 guard, so the "
                f"selftest does not build on {sys.platform} yet "
                "(docs/LINUX_DEVELOPMENT.md, 'What doesn't yet')"
            )
        make_tool = "mingw32-make" if sys.platform == "win32" else "make"
        make = shutil.which(make_tool)
        if not make:
            raise unittest.SkipTest(f"{make_tool} is not available")
        with tempfile.TemporaryDirectory(prefix="flash0-font-") as tmp:
            root = Path(tmp)
            write_fixture(root)
            # Build beneath the scratch tree, never the checkout's build/.
            build_root = root / "build"
            build_dir = build_root / "flash0-font"
            build = subprocess.run(
                [make, "--no-print-directory", f"BUILD_ROOT={build_root.as_posix()}",
                 f"BUILD_DIR={build_dir.as_posix()}", "hle-thread-selftest-build"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(build.returncode, 0, build.stderr[-4000:] or build.stdout[-4000:])
            env = dict(os.environ)
            env["SR_FLASH0_TEST_ROOT"] = str(root)
            env["SR_FONTDIR"] = str(root / "project")
            env.pop("SR_FONTLOG", None)
            run = subprocess.run(
                [str(build_dir / "hle_thread_selftest.exe"), "--flash0-font"],
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                check=False,
                timeout=300,
            )
            self.assertIn("flash0-font:", run.stderr)
            self.assertEqual(run.returncode, 0, run.stderr[-6000:])


if __name__ == "__main__":
    unittest.main()
