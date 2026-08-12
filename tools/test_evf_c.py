# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression coverage for PSP EventFlag semantics (issue #4).

Two layers:

1. Compile and run ``src/rt/evf_selftest.c`` against the pure helpers in
   ``src/rt/evf.h`` — keep-mask clear, AND/OR matching, WAITCLEAR/WAITCLEARALL
   consumption, and PSP-style wait/poll argument validation errors.
2. Source-level wiring checks that ``src/rt/hle.c`` (Windows-only TU, not
   compilable on the Linux CI host) actually routes its EventFlag handlers
   through those helpers instead of re-deriving the semantics inline.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SELFTEST_C = ROOT / "src" / "rt" / "evf_selftest.c"
HLE_SOURCE = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


def function_body(name):
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", HLE_SOURCE)
    if not match:
        raise AssertionError(f"{name} not found in src/rt/hle.c")

    start = match.end() - 1
    depth = 0
    for pos in range(start, len(HLE_SOURCE)):
        char = HLE_SOURCE[pos]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return HLE_SOURCE[start : pos + 1]
    raise AssertionError(f"{name} has no closing brace")


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestEvfSelftestC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="evfc_")
        cls.exe = os.path.join(cls.tmp, "evf_selftest.exe")
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
            raise AssertionError("evf_selftest.c did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_pure_helper_invariants_hold(self):
        result = subprocess.run([self.exe], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("evf selftest: OK", result.stdout)


class TestHleEventFlagWiring(unittest.TestCase):
    def test_clear_uses_keep_mask_helper(self):
        body = function_body("h_ClearEventFlag")
        self.assertIn("sr_evf_clear_pattern(m->pattern, A1)", body)
        self.assertNotIn("~A1", body, "clear must keep A1 bits, not remove them")

    def test_wait_validates_args_and_consumes_via_helpers(self):
        body = function_body("h_WaitEventFlag")
        self.assertIn("sr_evf_check_wait_args(bits, mode)", body)
        self.assertIn("sr_evf_matches(m->pattern, bits, mode)", body)
        self.assertIn("sr_evf_consume(m->pattern, bits, mode)", body)
        self.assertNotRegex(body, r"mode\s*&\s*0x[12]0", "no inline clear-bit handling")

    def test_poll_validates_consumes_and_returns_evf_cond(self):
        body = function_body("h_PollEventFlag")
        self.assertIn("sr_evf_check_poll_args(bits, mode)", body)
        self.assertIn("sr_evf_matches(m->pattern, bits, mode)", body)
        self.assertIn("sr_evf_consume(m->pattern, bits, mode)", body)
        self.assertIn("SR_EVF_ERR_COND", body)
        self.assertNotIn("0x80020021", body, "poll condition failure must be EVF_COND")

    def test_poll_writes_outbits_on_both_outcomes(self):
        body = function_body("h_PollEventFlag")
        self.assertEqual(
            len(re.findall(r"MEM_W32\(outp, m->pattern\)", body)),
            2,
            "poll must report the current pattern on failure and the "
            "pre-consume pattern on success",
        )


if __name__ == "__main__":
    unittest.main()
