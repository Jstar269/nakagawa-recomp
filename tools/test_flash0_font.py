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
    def test_native_device_contract_on_synthetic_fonts(self) -> None:
        make = shutil.which("mingw32-make")
        if not make:
            raise unittest.SkipTest("mingw32-make is not available")
        with tempfile.TemporaryDirectory(prefix="flash0-font-") as tmp:
            root = Path(tmp)
            write_fixture(root)
            build = subprocess.run(
                [make, "--no-print-directory", f"BUILD_DIR={BUILD_DIR}", "hle-thread-selftest-build"],
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
                [str(ROOT / BUILD_DIR / "hle_thread_selftest.exe"), "--flash0-font"],
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
