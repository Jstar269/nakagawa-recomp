#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Behavioral checks for runtime debug watchpoints."""

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
class TestDebugC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        cls.tmp = Path(tempfile.mkdtemp(prefix="debugwatch_"))
        cls.exe = cls.tmp / "debug_selftest.exe"
        result = subprocess.run(
            # debug.c depends on the derived watchpoints.json parser
            # (issue #188); the harness compiles the real implementation.
            [CC, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             f"-I{RT}", "-o", os.fspath(cls.exe),
             os.fspath(RT / "debug_selftest.c"), os.fspath(RT / "debug.c"),
             os.fspath(RT / "watchpoints_file.c")],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise AssertionError("debug selftest did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_value_watch_matches_dynamic_address(self):
        result = subprocess.run(
            [os.fspath(self.exe)], cwd=self.tmp, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("debug value watch selftest: OK", result.stdout)
        self.assertEqual(result.stderr.count("MEM_VALUE_WATCH[PANEL_X0]"), 1)
        self.assertIn("addr=0x0a123404 val=0x440b4000 pc=0x00001234", result.stderr)
        self.assertEqual(result.stderr.count("MEM_WATCH_CONTEXT pc="), 1)
        self.assertIn("pc=0x00001234 hit=1 last_yield_pc=0x00005678", result.stderr)
        self.assertIn("r17=0x09abcdef", result.stderr)
        self.assertIn("f22=0x440b4000", result.stderr)
        self.assertEqual(result.stderr.count("STORE_CONTEXT pc="), 1)
        self.assertIn(
            "pc=0x00005678 hit=1 addr=0x09000000 width=4 val=0x43110000",
            result.stderr,
        )
        self.assertIn(
            "STORE_CONTEXT_MEM r16+0x00000024 base=0x08000024 "
            "w0=0x00000001 w1=0x00000005 w2=0x00000002",
            result.stderr,
        )


if __name__ == "__main__":
    unittest.main()
