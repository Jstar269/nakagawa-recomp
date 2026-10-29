# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for tools/purity_gate.py.

Synthetic trees are written to a temporary directory; one test runs the gate on this
checkout, which is the CI contract: main passes with the reviewed allowlist.
"""

import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools import purity_gate
from tools.purity_gate import Allowed


class PurityGateTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        (self.root / "src/rt").mkdir(parents=True)

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, rel, text):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_checkout_passes_with_reviewed_allowlist(self):
        self.assertEqual(purity_gate.scan(ROOT), [])

    def test_every_allowlist_entry_has_a_reason_and_is_in_range(self):
        for entry in purity_gate.ALLOWLIST:
            self.assertTrue(entry.path.startswith("src/rt/"), entry)
            self.assertGreater(len(entry.reason), 20, entry)
            self.assertGreater(entry.count, 0, entry)
            self.assertTrue(purity_gate.USER_RAM_FIRST <= entry.value <= purity_gate.USER_RAM_LAST, entry)

    def test_game_address_in_core_fails(self):
        self.write("src/rt/hle.c", "static const uint32_t k = 0x0891A2B4u;\n")
        messages = purity_gate.scan(self.root, ())
        self.assertEqual(len(messages), 1)
        self.assertIn("src/rt/hle.c:1:", messages[0])
        self.assertIn("0x0891a2b4", messages[0])

    def test_range_edges(self):
        self.write("src/rt/a.c", "a = 0x087fffff; b = 0x08800000; c = 0x09ffffff; d = 0x0a000000;\n"
                                 "e = 0x88000000; f = 0x8800000;\n")
        values = sorted(int(m.split("literal 0x", 1)[1][:8], 16) for m in purity_gate.scan(self.root, ()))
        self.assertEqual(values, [0x08800000, 0x08800000, 0x09FFFFFF])

    def test_subdirectories_and_headers_are_scanned(self):
        self.write("src/rt/gpu_sdl3vk/x.h", "#define BASE 0x09000000u\n")
        self.assertEqual(len(purity_gate.scan(self.root, ())), 1)

    def test_comments_and_strings_are_not_code(self):
        self.write("src/rt/a.c", "/* was 0x09df8000\n   and 0x08804000 */\n"
                                 "// 0x08900000\n"
                                 "puts(\"m32[0x09fffe84]\"); char c = '\\'';\n"
                                 "int x = 1;\n")
        self.assertEqual(purity_gate.scan(self.root, ()), [])

    def test_line_numbers_survive_comment_stripping(self):
        self.write("src/rt/a.c", "/* one\n two\n */\nint x = 0x08a00000;\n")
        self.assertIn("src/rt/a.c:4:", purity_gate.scan(self.root, ())[0])

    def test_title_config_selftests_and_nid_table_are_exempt(self):
        body = "uint32_t a = 0x08A00000u;\n"
        self.write("src/rt/title_config.c", body)
        self.write("src/rt/title_config.h", body)
        self.write("src/rt/sched_selftest.c", body)
        self.assertEqual(purity_gate.scan(self.root, ()), [])

    def test_registered_nids_are_not_addresses(self):
        self.write("src/rt/nid_names.h", "    {0x092968f4u, \"sceKernelCpuSuspendIntr\"},\n")
        self.write("src/rt/hle.c", "sr_hle_register(0x092968f4, \"sceKernelCpuSuspendIntr\", h);\n"
                                   "x = 0x092968f5;\n")
        messages = purity_gate.scan(self.root, ())
        self.assertEqual(len(messages), 1)
        self.assertIn("0x092968f5", messages[0])

    def test_allowlist_entry_suppresses_exact_count_only(self):
        self.write("src/rt/sched.c", "#define FLOOR 0x08800000u\n")
        allow = (Allowed("src/rt/sched.c", 0x08800000, 1, "user partition floor for the test"),)
        self.assertEqual(purity_gate.scan(self.root, allow), [])
        self.write("src/rt/sched.c", "#define FLOOR 0x08800000u\nint y = 0x08800000u;\n")
        messages = purity_gate.scan(self.root, allow)
        self.assertEqual(len(messages), 1)
        self.assertIn("expects 1 occurrence(s), found 2", messages[0])

    def test_stale_allowlist_entry_fails(self):
        allow = (Allowed("src/rt/gone.c", 0x08800000, 1, "a literal that was removed later"),)
        messages = purity_gate.scan(self.root, allow)
        self.assertEqual(len(messages), 1)
        self.assertIn("found 0", messages[0])

    def test_cli_exit_codes(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(purity_gate.main(["--root", str(ROOT)]), 0)
        self.assertIn("PASS", out.getvalue())
        self.write("src/rt/a.c", "int x = 0x08a00000;\n")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(purity_gate.main(["--root", str(self.root)]), 1)
        self.assertIn("FAIL", out.getvalue())


if __name__ == "__main__":
    unittest.main()
