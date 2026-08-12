# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Host-neutral VFS path joining regression test (issue #19).

Proves that:
1. Executable selftest (src/rt/vfs_selftest.c) passes cleanly, exercising all path
   join edge cases (trailing slashes, device prefix stripping, traversal prevention,
   overflow limits).
2. Joining root="fs" and guest="ms0:/PSP/SAVEDATA" produces "fs/PSP/SAVEDATA" or
   "fs\\PSP\\SAVEDATA", never "fsPSP...".
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SELFTEST_C = ROOT / "src" / "rt" / "vfs_selftest.c"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestVfsSelftestC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="vfsc_")
        cls.exe = os.path.join(cls.tmp, "vfs_selftest.exe")
        result = subprocess.run(
            [
                CC,
                "-std=c11",
                "-O0",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'src' / 'rt'}",
                "-o",
                cls.exe,
                str(SELFTEST_C),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise AssertionError("vfs_selftest.c did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_vfs_path_join_invariants_hold(self):
        result = subprocess.run([self.exe], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("vfs selftest: OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
