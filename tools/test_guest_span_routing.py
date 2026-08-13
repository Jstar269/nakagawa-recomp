# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Source-shape gates for the finite #15 native bulk/parser slice.

The production HLE/fast-path implementations are exercised by the native
selftests and exact-main runtime routes; these assertions keep the reviewed
whole-span preflight from regressing when generated code is regenerated.
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parent.parent


class TestGuestSpanRouting(unittest.TestCase):
    def test_generated_bulk_stubs_use_complete_span_helpers(self) -> None:
        text = (ROOT / "tools" / "codegen.py").read_text(encoding="utf-8")
        self.assertIn("sr_guest_span_readable(src, size)", text)
        self.assertIn("sr_guest_span_writable(dest, size)", text)
        self.assertIn("sr_guest_span_writable(_mf_base, _mf_cnt)", text)
        self.assertIn("sr_oor(dest, 0u, 1); sr_oor(src, 0u, 0);", text)
        self.assertNotIn("memmove(SR_HOST(dest), SR_HOST(src), size);\n        }} else {{", text)

    def test_generic_memcpy_memset_preflight_before_sr_host(self) -> None:
        text = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        dmac_start = text.find("static uint32_t h_DmacMemcpy")
        dmac_end = text.find("static uint32_t h_Memset", dmac_start)
        dmac = text[dmac_start:dmac_end]
        self.assertIn("sr_guest_span_readable(src, effective)", dmac)
        self.assertIn("sr_guest_span_writable(dst, effective)", dmac)
        self.assertIn("uint32_t effective = n > SCE_DMAC_EFFECTIVE_MAX ? SCE_DMAC_EFFECTIVE_MAX : n;", dmac)
        self.assertIn("memmove(SR_HOST(dst), SR_HOST(src), effective)", dmac)
        memcpy_start = text.find("static uint32_t h_Memcpy")
        memcpy_end = text.find("static uint32_t", memcpy_start + 1)
        memcpy = text[memcpy_start:memcpy_end]
        self.assertIn("sr_guest_span_readable(src, n)", memcpy)
        self.assertIn("sr_guest_span_writable(dst, n)", memcpy)
        self.assertIn("memmove(SR_HOST(dst), SR_HOST(src), n)", memcpy)
        memset = text[text.find("static uint32_t h_Memset"):text.find("static uint32_t h_Memcpy")]
        self.assertIn("sr_guest_span_writable(dst, n)", memset)
        self.assertIn("memset(SR_HOST(dst)", memset)
        registration = text[text.find("static void hle_register_bulk_memory_handlers") : text.find("void sr_hle_init")]
        self.assertIn("sceKernelMemcpy", registration)
        self.assertIn("sceKernelMemset", registration)

    def test_funcdiff_register_parser_has_per_file_register_limits(self) -> None:
        text = (ROOT / "src" / "rt" / "funcdiff.c").read_text(encoding="utf-8")
        self.assertIn("parse_indexed_register(name, 'r', 31", text)
        self.assertIn("parse_indexed_register(name, 'f', 31", text)
        self.assertIn("parse_indexed_register(name, 'v', 127", text)
        self.assertNotIn("s->r[atoi(name + 1)]", text)
        self.assertNotIn("s->fi[atoi(name + 1)]", text)
        self.assertNotIn("s->vi[atoi(name + 1)]", text)

    def test_loader_uses_shared_span_for_the_full_segment(self) -> None:
        text = (ROOT / "src" / "rt" / "recomp.c").read_text(encoding="utf-8")
        load = text[text.find("void sr_load_segment"):text.find("/* Newlib malloc", text.find("void sr_load_segment"))]
        self.assertIn("sr_guest_span_writable(vaddr, len)", load)
        self.assertNotIn("SR_PHYS(end_vaddr) >", load)


if __name__ == "__main__":
    unittest.main()
