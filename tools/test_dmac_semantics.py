# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Source-shape gates for the measured PSP DMAC copy path.

The PSP-visible behavior itself is proven executably by
``src/rt/hle_thread_selftest.c``, which enters both DMAC NIDs through the real
``sr_syscall`` registry and asserts return values and guest memory contents.
These assertions guard the two properties a behavioral test cannot express:

* validation of the complete requested spans before any guest or GPU side
  effect, and
* the measured effective-prefix ceiling, including the fact that the dirty
  notification covers only bytes that were actually transferred.

The concurrent BUSY result and invalid-truncated-tail precedence remain
unknown; this module must not turn either into an invented hardware fact.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parent.parent


def _dmac_region() -> str:
    """The sceDmac copy implementation, from its comment block to h_Memset."""
    text = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
    start = text.index("/* ---- sceDmacMemcpy / sceDmacTryMemcpy")
    end = text.index("static uint32_t h_Memset", start)
    return text[start:end]


class TestDmacValidationOrder(unittest.TestCase):
    def test_measured_error_classes_are_returned(self) -> None:
        region = _dmac_region()
        self.assertIn("#define SCE_DMAC_ERROR_ILLEGAL_ADDR 0x80000103u", region)
        self.assertIn("#define SCE_DMAC_ERROR_ILLEGAL_SIZE 0x80000104u", region)
        self.assertIn("if (n == 0u) return SCE_DMAC_ERROR_ILLEGAL_SIZE;", region)
        self.assertIn(
            "if (dst == 0u || src == 0u) return SCE_DMAC_ERROR_ILLEGAL_ADDR;", region
        )

    def test_complete_requested_spans_precede_copy_and_dirty(self) -> None:
        """A failed request must not move a byte or dirty a GPU range."""
        region = _dmac_region()
        size_check = region.index("if (n == 0u) return SCE_DMAC_ERROR_ILLEGAL_SIZE;")
        null_check = region.index("if (dst == 0u || src == 0u)")
        span_check = region.index(
            "if (!sr_guest_span_readable(src, n) || !sr_guest_span_writable(dst, n))"
        )
        effective = region.index(
            "uint32_t effective = n > SCE_DMAC_EFFECTIVE_MAX ? SCE_DMAC_EFFECTIVE_MAX : n;"
        )
        copy = region.index("memmove(SR_HOST(dst), SR_HOST(src), effective)")
        dirty = region.index("sr_gpu_vram_dirty(dst, effective)")

        self.assertLess(size_check, null_check)
        self.assertLess(null_check, span_check)
        self.assertLess(span_check, effective, "requested spans must be validated before clamping")
        self.assertLess(effective, copy, "the effective length must be selected before copying")
        self.assertLess(copy, dirty, "the GPU is notified only after a real transfer")
        self.assertIn("sr_guest_span_readable(src, n)", region)
        self.assertIn("sr_guest_span_writable(dst, n)", region)

    def test_overlap_safe_primitive(self) -> None:
        """Hardware showed both overlap directions landing memmove-correct."""
        region = _dmac_region()
        self.assertIn("memmove(SR_HOST(dst), SR_HOST(src), effective)", region)
        self.assertNotIn("memcpy(SR_HOST(dst), SR_HOST(src)", region)

    def test_both_copy_nids_register_in_the_shared_bulk_helper(self) -> None:
        """Both NIDs must route through production registration."""
        text = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        start = text.index("static void hle_register_bulk_memory_handlers")
        registration = text[start : text.index("void sr_hle_init", start)]
        self.assertIn('sr_hle_register(0x617f3fe6, "sceDmacMemcpy", h_DmacMemcpy)', registration)
        self.assertIn(
            'sr_hle_register(0xd97f94d8, "sceDmacTryMemcpy", h_DmacTryMemcpy)', registration
        )
        self.assertEqual(text.count('"sceDmacTryMemcpy"'), 1)
        self.assertEqual(text.count('"sceDmacMemcpy"'), 1)


class TestDmacMeasuredCeiling(unittest.TestCase):
    def test_measured_effective_ceiling_is_encoded(self) -> None:
        region = _dmac_region()
        self.assertIn("#define SCE_DMAC_EFFECTIVE_MAX 0xC000u", region)
        self.assertIn(
            "uint32_t effective = n > SCE_DMAC_EFFECTIVE_MAX ? SCE_DMAC_EFFECTIVE_MAX : n;",
            region,
        )
        self.assertNotIn("SR_DMAC_VERIFIED_FULL_MAX", region)
        self.assertNotIn("sr_dmac_note_unverified_size", region)
        self.assertNotIn("s_dmac_unverified", region)

    def test_only_effective_prefix_has_side_effects(self) -> None:
        region = _dmac_region()
        self.assertIn("memmove(SR_HOST(dst), SR_HOST(src), effective)", region)
        self.assertIn("sr_gpu_vram_dirty(dst, effective)", region)
        self.assertIn("sr_heap_note_bulk_write(dst, effective, 0u)", region)

    def test_no_fabricated_busy_result(self) -> None:
        """No concurrent probe established a BUSY return code."""
        code = re.sub(r"/\*.*?\*/", "", _dmac_region(), flags=re.S)
        code = re.sub(r"//[^\n]*", "", code)
        self.assertNotIn("0x80000021", code)
        self.assertNotIn("SCE_DMAC_BUSY", code)

    def test_conservative_invalid_tail_policy_is_explicit(self) -> None:
        region = _dmac_region()
        self.assertIn("validating the requested range is the conservative memory-safety", region)
        self.assertIn("Hardware has not yet settled whether an invalid truncated tail", region)
        self.assertIn("sr_guest_span_readable(src, n)", region)
        self.assertIn("sr_guest_span_writable(dst, n)", region)


class TestDmacExecutableCoverage(unittest.TestCase):
    def test_regression_covers_both_nids_through_production_dispatch(self) -> None:
        text = (ROOT / "src" / "rt" / "hle_thread_selftest.c").read_text(encoding="utf-8")
        self.assertIn("test_dmac_semantics();", text)
        self.assertIn("test_dmac_hardware_semantics(NID_SCE_DMAC_MEMCPY", text)
        self.assertIn("test_dmac_hardware_semantics(NID_SCE_DMAC_TRY_MEMCPY", text)

    def test_regression_asserts_measured_and_policy_cases(self) -> None:
        text = (ROOT / "src" / "rt" / "hle_thread_selftest.c").read_text(encoding="utf-8")
        for needle in (
            "PSP: zero size returns the illegal-size error",
            "PSP: a NULL destination returns the illegal-address error",
            "PSP: a NULL source returns the illegal-address error",
            "PSP: a rejected span leaves both buffers byte-for-byte unchanged",
            "PSP: a same-pointer self copy leaves the buffer unchanged",
            "PSP: a forward-overlapping copy is memmove-correct across the span",
            "PSP: a backward-overlapping copy is memmove-correct across the span",
            "a measured-ceiling request copies the complete effective prefix",
            "a measured-ceiling request leaves the truncated tail untouched",
            "the conservative policy rejects an invalid requested tail",
        ):
            self.assertIn(needle, text)


class TestDmacGpuAliasBoundary(unittest.TestCase):
    def test_renderer_canonicalizes_cpu_dirty_aliases(self) -> None:
        """DMA dirty notifications must invalidate aliased texture ranges."""
        text = (ROOT / "src" / "rt" / "gpu_sdl3vk" / "ge_gpu.c").read_text(
            encoding="utf-8"
        )
        start = text.index("static void hook_vram_dirty")
        region = text[start : text.index("/* ---- GE block transfer", start)]
        self.assertIn("SR_PHYS(addr)", region)
        self.assertIn("SR_PHYS(e->addr)", region)
        self.assertIn("vram_off(addr)", region)
        self.assertIn('"alias-vram"', text)
        self.assertIn("if (tc->dirty_alias)", text)


if __name__ == "__main__":
    unittest.main()
