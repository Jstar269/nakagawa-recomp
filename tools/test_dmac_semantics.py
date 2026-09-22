# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Source-shape gates for the measured PSP DMAC copy path.

The PSP-visible behavior itself is proven executably by
``src/rt/hle_thread_selftest.c``, which enters both DMAC NIDs through the real
``sr_syscall`` registry and asserts return values and guest memory contents.
These assertions guard the two properties a behavioral test cannot express:

* complete valid spans must not be silently clamped to the old 0xC000
  allocator-boundary observation, and
* the measured one-byte invalid-tail prefix is the only partial shape admitted
  before a larger overrun is measured.

The concurrent BUSY result remains hardware-measured but runtime-unimplemented;
this module must not turn it into an invented synchronous state machine.
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

    def test_prefix_policy_precedes_copy_and_dirty(self) -> None:
        """The bounded prefix is selected before a host pointer is formed."""
        region = _dmac_region()
        size_check = region.index("if (n == 0u) return SCE_DMAC_ERROR_ILLEGAL_SIZE;")
        null_check = region.index("if (dst == 0u || src == 0u)")
        prefix = region.index("const uint32_t src_prefix = sr_guest_span_prefix(src, n);")
        policy = region.index("n - src_prefix != 1u")
        effective = region.index("uint32_t effective = src_prefix < dst_prefix ? src_prefix : dst_prefix;")
        copy = region.index("memmove(SR_HOST(dst), SR_HOST(src), effective)")
        dirty = region.index("sr_gpu_vram_dirty(dst, effective)")

        self.assertLess(size_check, null_check)
        self.assertLess(null_check, prefix)
        self.assertLess(prefix, effective)
        self.assertLess(effective, policy)
        self.assertLess(effective, copy, "the effective length must be selected before copying")
        self.assertLess(copy, dirty, "the GPU is notified only after a real transfer")
        self.assertIn("sr_guest_span_readable(src, effective)", region)
        self.assertIn("sr_guest_span_writable(dst, effective)", region)

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


class TestDmacMeasuredBoundary(unittest.TestCase):
    def test_no_api_wide_effective_ceiling_is_encoded(self) -> None:
        region = _dmac_region()
        self.assertNotIn("SCE_DMAC_EFFECTIVE_MAX", region)
        self.assertIn("there is no\n *     API-wide 0xC000 ceiling", region)
        self.assertIn("fully valid RAM/VRAM spans copy their complete", region)

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

    def test_measured_invalid_tail_policy_is_explicit(self) -> None:
        region = _dmac_region()
        self.assertIn("one-byte arena-end tail", region)
        self.assertIn("larger or ambiguous overruns remain fail-closed", region)
        self.assertIn("n - src_prefix != 1u", region)


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
            "PSP: a one-byte invalid destination tail returns success",
            "PSP: a one-byte invalid source tail returns success",
            "PSP: a same-pointer self copy leaves the buffer unchanged",
            "PSP: a forward-overlapping copy is memmove-correct across the span",
            "PSP: a backward-overlapping copy is memmove-correct across the span",
            "RAM-to-VRAM",
            "VRAM-to-RAM",
            "VRAM-to-VRAM",
            "aliased VRAM destination",
            "PSP: a hardware-verified transfer size copies every byte",
            "unmeasured multi-byte invalid tail remains fail-closed",
        ):
            self.assertIn(needle, text)

    def test_code_invalidation_boundary_is_explicit(self) -> None:
        region = _dmac_region()
        self.assertIn("does not currently translate guest self-modifying code", region)


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
