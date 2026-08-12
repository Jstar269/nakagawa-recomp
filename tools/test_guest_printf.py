#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Behavioral regression checks for the PSP-EABI guest sprintf bridge."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
RT = ROOT / "src" / "rt"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestGuestPrintf(unittest.TestCase):
    def test_float_format_and_double_alignment(self):
        assert CC is not None
        with tempfile.TemporaryDirectory(prefix="guestprintf_") as tmp:
            exe = Path(tmp) / "guest_printf_selftest.exe"
            build = subprocess.run(
                [
                    CC,
                    "-std=c11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{RT}",
                    "-o",
                    os.fspath(exe),
                    os.fspath(RT / "guest_printf_selftest.c"),
                    os.fspath(RT / "guest_printf.c"),
                    os.fspath(RT / "debug.c"),
                    # debug.c depends on the derived watchpoints.json parser
                    # (issue #188); the harness compiles the real implementation.
                    os.fspath(RT / "watchpoints_file.c"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run(
                [os.fspath(exe)], capture_output=True, text=True, cwd=tmp
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("guest printf selftest: OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
