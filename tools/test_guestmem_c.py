#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Behavioral + wiring coverage for the guest-memory bounds contract (issue #15).

Layer 1 (behavioral): compile and run ``src/rt/guestmem_selftest.c`` — real
assertions, including an overflow differential fuzz over the full 32-bit range —
against the helpers in ``src/rt/recomp.h``.

Layer 2 (wiring/consistency): source checks that the span helpers delegate to the
overflow-safe ``sr_inrange_n`` (never naive ``addr + size`` arithmetic) and that
the arena bound the selftest references matches the header, so the two cannot
silently drift.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RT = ROOT / "src" / "rt"
SELFTEST_C = RT / "guestmem_selftest.c"
RECOMP_H = (RT / "recomp.h").read_text(encoding="utf-8")
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


def _body(src: str, name: str) -> str:
    """Return the brace-matched body of the first definition of `name`."""
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", src)
    if not match:
        raise AssertionError(f"{name} not found in recomp.h")
    start = match.end() - 1
    depth = 0
    for pos in range(start, len(src)):
        if src[pos] == "{":
            depth += 1
        elif src[pos] == "}":
            depth -= 1
            if depth == 0:
                return src[start:pos + 1]
    raise AssertionError(f"{name} has no closing brace")


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestGuestmemSelftestC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="guestmemc_")
        cls.exe = os.path.join(cls.tmp, "guestmem_selftest.exe")
        result = subprocess.run(
            [CC, "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
             f"-I{RT}", "-o", cls.exe, str(SELFTEST_C)],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise AssertionError("guestmem_selftest.c did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_behavioral_assertions_pass(self):
        result = subprocess.run([self.exe], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("guestmem selftest: OK", result.stdout)


class TestSpanHelperWiring(unittest.TestCase):
    def test_span_helpers_delegate_to_overflow_safe_check(self):
        for name in ("sr_guest_span_readable", "sr_guest_span_writable"):
            body = _body(RECOMP_H, name)
            self.assertIn("sr_inrange_n", body,
                          f"{name} must delegate to the overflow-safe sr_inrange_n")
            self.assertNotRegex(body, r"addr\s*\+\s*size",
                                f"{name} must not use naive addr+size arithmetic")

    def test_checked_arith_helpers_exist(self):
        self.assertIn("sr_size_add_ok", RECOMP_H)
        self.assertIn("sr_size_mul_ok", RECOMP_H)

    def test_arena_bound_matches_selftest_reference(self):
        # sr_inrange_n is the source of truth for the arena end; the selftest's
        # ARENA_END must match it so the fuzz validates against the right bound.
        body = _body(RECOMP_H, "sr_inrange_n")
        self.assertIn("0x0c000000", body)
        selftest = SELFTEST_C.read_text(encoding="utf-8")
        self.assertIn("ARENA_END 0x0c000000", selftest)


if __name__ == "__main__":
    unittest.main()
