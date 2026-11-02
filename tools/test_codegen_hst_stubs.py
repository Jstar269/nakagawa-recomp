# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The hand-written HST stub bodies in ``codegen_hst_stubs`` reach the generated C unchanged.

``tools/codegen.py`` used to hold these bodies inline; they are data now.  These tests pin the
shape of that data and prove, through the production ``codegen.main`` path over the shared
profile-isolation specimen, that ``--profile=hst`` emits each body verbatim and that
``--profile=none`` emits none of them.
"""

import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import codegen_hst_stubs  # noqa: E402
import test_codegen_profile_isolation as isolation  # noqa: E402
from host_stubs import HST_SIMPLE_STUBS  # noqa: E402

STUBS = codegen_hst_stubs.HST_CUSTOM_STUBS
LOOP = codegen_hst_stubs.HST_GAME_LOOP_ADDR


class StubDataShape(unittest.TestCase):
    def test_each_body_defines_the_function_for_its_own_address(self):
        for addr, text in STUBS.items():
            with self.subTest(addr=f"0x{addr:08x}"):
                self.assertTrue(text.startswith(f"void f_{addr:08x}(CpuState *s) {{"), text[:80])
                self.assertTrue(text.endswith("}"))
                self.assertNotIn("@ADDR@", text)

    def test_the_game_loop_wrapper_replaces_the_translated_body(self):
        text = codegen_hst_stubs.HST_GAME_LOOP_WRAPPER
        self.assertEqual(LOOP, 0x000468C8)
        self.assertTrue(text.startswith(f"void f_{LOOP:08x}(CpuState *s) {{"))
        self.assertIn(f"f_{LOOP:08x}_real(s);", text)
        self.assertNotIn(LOOP, STUBS)

    def test_no_address_has_two_bodies(self):
        self.assertEqual(set(STUBS) & set(HST_SIMPLE_STUBS), set())
        self.assertEqual({LOOP} & set(HST_SIMPLE_STUBS), set())


class StubsReachTheGeneratedC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        extra = {addr: isolation._body_leaf() for addr in [*STUBS, LOOP]}
        words = isolation._build_specimen_words(extra)
        cls.hst = isolation._codegen_functions(words, "hst")
        cls.none = isolation._codegen_functions(words, "none")

    def test_hst_emits_each_body_verbatim(self):
        for addr, text in STUBS.items():
            with self.subTest(addr=f"0x{addr:08x}"):
                self.assertEqual(self.hst[f"f_{addr:08x}"], text)

    def test_hst_wraps_the_game_loop_around_its_translated_body(self):
        self.assertEqual(self.hst[f"f_{LOOP:08x}"], codegen_hst_stubs.HST_GAME_LOOP_WRAPPER)
        self.assertIn(f"f_{LOOP:08x}_real", self.hst)

    def test_profile_none_emits_none_of_them(self):
        for addr in [*STUBS, LOOP]:
            with self.subTest(addr=f"0x{addr:08x}"):
                self.assertNotIn("custom stub", self.none[f"f_{addr:08x}"])
                self.assertNotIn(f"f_{addr:08x}_real", self.none)


if __name__ == "__main__":
    unittest.main()
