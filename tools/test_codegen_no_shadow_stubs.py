# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Guard the 2026-07-18 removal of two code-shadowing custom stubs.

0x0001a5f8 and 0x0001c008 carried constant-return custom stubs
("unexplained" in tools/compat_overrides.py) that a Ghidra-assisted review
proved were shadowing real, fully-translatable guest code:

  0x0001c008:  jr ra; sw a1,0x4028(a0)   -- the stub dropped the delay-slot
               store and faked v0=0x30ab9c; jal-called from 0x46c4c/0x46cc4.
  0x0001a5f8:  andi v0,s5,0x80; ...      -- a computed-goto resume point
               reached via .data pointer tables at +0x21f8/+0x2200.

These tests fail if either address stops being emitted as a real translation
(stub reintroduced, or discovery loses the entry). They need the decrypted
EBOOT and skip when it is absent (CI has no game data).
"""

import os
import inspect
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import analyze
import codegen

ELF = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "place_game_here", "EBOOT.elf")


@unittest.skipUnless(os.path.isfile(ELF), "decrypted EBOOT.elf not present")
class TestNoShadowStubs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.elf = analyze.Elf(ELF)
        cls.starts, cls.ranges = analyze.analyze(cls.elf)

    def emit(self, addr):
        self.assertIn(addr, self.starts,
                      "0x%08x no longer a discovered entry" % addr)
        return "\n".join(codegen.emit_function(self.elf, addr, self.ranges,
                                               self.starts))

    def test_1c008_is_real_setter(self):
        body = self.emit(0x0001c008)
        # The whole point of the function: the delay-slot store to a0+0x4028
        # (emitted zero-padded, e.g. `s->r[4] + 0x00004028u`).
        self.assertTrue(re.search(r"0x0*4028u", body),
                        "f_0001c008 lost its +0x4028 store (stub shadowing again?)\n" + body)
        self.assertNotIn("0x30ab9c", body,
                         "f_0001c008 contains the old fake constant return")

    def test_1a5f8_is_real_code(self):
        body = self.emit(0x0001a5f8)
        # Real code tests bit 0x80 of s5 (r21); the old stub returned 0x1000000.
        self.assertTrue(re.search(r"0x0*80u", body),
                        "f_0001a5f8 lost its andi bit-test (stub shadowing again?)\n" + body)
        self.assertNotIn("0x1000000", body,
                         "f_0001a5f8 contains the old fake constant return")

    def test_2688_is_emitted_as_real_varint_parser(self):
        body = self.emit(0x00002688)
        self.assertGreaterEqual(
            body.count("f_00001040(s);"),
            2,
            "f_00002688 lost its calls to the varint decoder\n" + body,
        )
        self.assertNotIn(
            "known.discard(0x00002688)",
            inspect.getsource(codegen),
            "main code generation still drops the Ghidra-confirmed callable entry",
        )


if __name__ == "__main__":
    unittest.main()
