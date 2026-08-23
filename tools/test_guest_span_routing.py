# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Source-shape gates for the finite #15 native bulk/parser slice.

The production HLE/fast-path implementations are exercised by the native
selftests and exact-main runtime routes; these assertions keep the reviewed
whole-span preflight from regressing when generated code is regenerated.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parent.parent


def _body(text: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^)]*\)\s*\{{", text)
    if not match:
        raise AssertionError(f"function {name} not found")
    start = match.end() - 1
    depth = 0
    for pos in range(start, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[start:pos + 1]
    raise AssertionError(f"function {name} has no closing brace")


def _range(text: str, name: str) -> tuple[int, int]:
    body = _body(text, name)
    start = text.find(body)
    return start, start + len(body)


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
        self.assertIn("sr_guest_span_readable(src, n)", dmac)
        self.assertIn("sr_guest_span_writable(dst, n)", dmac)
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

    def test_ge_block_transfer_preflights_both_rectangles_before_side_effects(self) -> None:
        text = (ROOT / "src" / "rt" / "ge.c").read_text(encoding="utf-8")
        body = _body(text, "ge_block_transfer")
        readable = body.find("sr_guest_rect_readable")
        writable = body.find("sr_guest_rect_writable")
        gpu_hook = body.find("s_gpu->xfer")
        flush = body.find("s_gpu->flush")
        copy = body.find("memmove")
        dirty = body.find("sr_gpu_vram_dirty")
        self.assertTrue(0 <= readable < gpu_hook < copy)
        self.assertTrue(0 <= writable < flush < copy < dirty)
        self.assertNotIn("if (!sr_inrange(so0)", body)

    def test_ge_direct_guest_host_pointers_have_named_complete_span_owners(self) -> None:
        cases = {
            ROOT / "src" / "rt" / "ge.c": {
                "ge_decode_tex_rgba": ("sr_guest_rect_readable",),
                "ge_block_transfer": ("sr_guest_rect_readable", "sr_guest_rect_writable"),
            },
            ROOT / "src" / "rt" / "ge_capture.c": {
                "ge_capture_begin": ("sr_guest_span_readable",),
                "append_page": ("sr_guest_span_readable",),
                "ge_capture_apply": ("sr_guest_span_writable",),
            },
            ROOT / "src" / "rt" / "gpu_sdl3vk" / "ge_gpu.c": {
                "write_guest_fb": ("gegpu_validate_guest_fb_descriptor",),
                "target_upload": ("gegpu_validate_guest_fb_descriptor",),
                "target_patch_vram_dirty": ("gegpu_validate_guest_fb_descriptor",),
                "tex_source_ptr": ("sr_guest_span_readable",),
            },
        }
        for path, owners in cases.items():
            text = path.read_text(encoding="utf-8")
            if path.name == "ge_gpu.c":
                # Ignore the explicitly synthetic Vulkan selftest block. Production
                # code before and after it remains in the census.
                start = text.find("#ifdef SR_GPU_COHERENCE_SELFTEST")
                end = text.find("#endif", start)
                self.assertGreaterEqual(start, 0)
                self.assertGreater(end, start)
                text = text[:start] + (" " * (end + len("#endif") - start)) + text[end + len("#endif"):]

            ranges = {}
            for owner, required in owners.items():
                ranges[owner] = _range(text, owner)
                body = _body(text, owner)
                for token in required:
                    self.assertIn(token, body, f"{path.name}:{owner} lost {token}")

            for match in re.finditer(r"\bSR_HOST\s*\(", text):
                containing = [name for name, (lo, hi) in ranges.items() if lo <= match.start() < hi]
                self.assertEqual(
                    len(containing), 1,
                    f"{path.name}:{text.count(chr(10), 0, match.start()) + 1} has an unowned direct guest host pointer",
                )

    def test_texture_hash_uses_the_checked_source_capability(self) -> None:
        text = (ROOT / "src" / "rt" / "gpu_sdl3vk" / "ge_gpu.c").read_text(encoding="utf-8")
        body = _body(text, "tex_hash")
        self.assertIn("tex_source_ptr", body)
        self.assertNotIn("SR_HOST", body)

    def test_framebuffer_target_is_validated_before_slot_or_image_allocation(self) -> None:
        text = (ROOT / "src" / "rt" / "gpu_sdl3vk" / "ge_gpu.c").read_text(encoding="utf-8")
        body = _body(text, "target_color_acquire")
        check = body.find("gegpu_validate_guest_fb_descriptor")
        slot = body.find("target_slot_acquire")
        image = body.find("make_image")
        self.assertTrue(0 <= check < slot < image)


if __name__ == "__main__":
    unittest.main()
