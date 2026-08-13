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
    def test_psp_abi_behavior(self):
        """Run the PSP-EABI behavioral regressions in guest_printf_selftest.c."""
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

    def test_no_guest_controlled_host_format(self):
        """The bridge must never pass a runtime-built format to a host printf.

        The guest owns every byte of the format string, so a non-literal host
        format is exactly the defect this file exists to prevent: guest length
        modifiers would select a host variadic type that the fixed C argument
        does not match. Compiling with -Werror=format-nonliteral makes that a
        build failure rather than a review question. The pre-rewrite bridge
        assembled guest flags/width/precision/length bytes into a `conv[]`
        buffer and failed this gate at four snprintf() call sites.
        """
        assert CC is not None
        with tempfile.TemporaryDirectory(prefix="guestprintf_fmt_") as tmp:
            obj = Path(tmp) / "guest_printf.o"
            build = subprocess.run(
                [
                    CC,
                    "-std=c11",
                    "-O2",
                    "-c",
                    "-Wall",
                    "-Wextra",
                    "-Wformat=2",
                    "-Wformat-nonliteral",
                    "-Wformat-security",
                    "-Werror",
                    f"-I{RT}",
                    "-o",
                    os.fspath(obj),
                    os.fspath(RT / "guest_printf.c"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                build.returncode,
                0,
                "guest_printf.c must contain only compile-time literal host "
                "formats:\n" + build.stdout + build.stderr,
            )


if __name__ == "__main__":
    unittest.main()
