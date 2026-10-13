# SPDX-License-Identifier: GPL-3.0-or-later
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
import shutil
import subprocess
import tempfile
import unittest

try:
    from .psp_oracle.parse_golden import parse_dmac_invalid_tail_output
    from .psp_oracle.protocol import ProtocolError
except ImportError:
    from psp_oracle.parse_golden import parse_dmac_invalid_tail_output
    from psp_oracle.protocol import ProtocolError


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


class TestDmacSkipProducer(unittest.TestCase):
    """Compile the real C producer, not Python lookalike protocol rows.

    Only emit(), the durable-writer seam probe_emit_durable() (which forwards
    to emit() here) and the launch constants are synthetic. No PSP SDK, hardware,
    private input, allocation or DMAC call is involved in this setup-failure path.
    """

    def test_s0_and_b1_b4_producer_rows_satisfy_shared_skip_shape(self) -> None:
        compiler = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
        if not compiler:
            self.skipTest("a native C compiler is required for producer execution")
        probe = (ROOT / "fixtures/psp_oracle/probe.c").read_text(encoding="utf-8")
        record_start = probe.index("static void dmac_invalid_emit_record(")
        record = probe[record_start:probe.index("static int dmac_invalid_probe_neighbor", record_start)]
        skip_start = probe.index("static void dmac_invalid_emit_skips(")
        skips = probe[skip_start:probe.index("static void run_dmac_invalid_tier_s", skip_start)]
        macro_start = probe.index("#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST", probe.index("#define DMAC_INVALID_MAX_DELTA"))
        macros = probe[macro_start:probe.index("static uint8_t s_dmac_invalid_io", macro_start)]
        constants = "\n".join(re.findall(
            r"^#define (?:PSP_ORACLE_CASE_DMAC_INVALID_TAIL_\w+|DMAC_INVALID_PAYLOAD_BYTES) .+$",
            probe, re.MULTILINE,
        ))
        launches = (
            ("dma-invalid-tail-s0", "S0", 8),
            ("dma-invalid-tail-memcpy-dst", "MEMCPY_DST", 4),
            ("dma-invalid-tail-memcpy-src", "MEMCPY_SRC", 4),
            ("dma-invalid-tail-try-dst", "TRY_DST", 4),
            ("dma-invalid-tail-try-src", "TRY_SRC", 4),
        )
        with tempfile.TemporaryDirectory(prefix="dmac-skip-producer-") as directory:
            root = Path(directory)
            for campaign, case, count in launches:
                for mutation in (False, True):
                    with self.subTest(campaign=campaign, integrity_mutation=mutation):
                        body = skips
                        if mutation:
                            # Generate/compile/execute the wrong source_intact argument
                            # for BOTH tiers; never mutate the tracked producer in place.
                            body = body.replace(
                                "0u, 0u, 0u, 0u, setup_mask",
                                "0u, 0u, 0u, 1u, setup_mask",
                            )
                            self.assertNotEqual(body, skips)
                        source = root / "producer.c"
                        binary = root / "producer.exe"
                        source.write_text(
                            "#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n" + constants +
                            f"\n#define PSP_ORACLE_CASE PSP_ORACLE_CASE_DMAC_INVALID_TAIL_{case}\n" +
                            macros +
                            "static void emit(int emulated, const char *line) { (void)emulated; fputs(line, stdout); }\n" +
                            "static void probe_emit_durable(int emulated, const char *line, size_t length) "
                            "{ (void)length; emit(emulated, line); }\n" +
                            record + body +
                            "int main(void) { dmac_invalid_emit_skips(1, 7u, 0x80020190u); return 0; }\n",
                            encoding="utf-8",
                        )
                        built = subprocess.run(
                            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                             str(source), "-o", str(binary)],
                            capture_output=True, text=True, timeout=30,
                        )
                        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                        emitted = subprocess.run(
                            [str(binary)], capture_output=True, text=True, timeout=10,
                        )
                        self.assertEqual(emitted.returncode, 0, emitted.stderr)
                        self.assertEqual(len(emitted.stdout.splitlines()), count)
                        stream = (
                            "NAKAGAWA_PSP_META schema=1 source=nakagawa model=synthetic "
                            "firmware=test binary_sha256=" + "0" * 64 +
                            " source_commit=" + "0" * 40 + "\n" + emitted.stdout
                        )
                        if mutation:
                            with self.assertRaisesRegex(ProtocolError, "SKIP cannot claim source integrity"):
                                parse_dmac_invalid_tail_output(stream, campaign)
                        else:
                            parsed = parse_dmac_invalid_tail_output(stream, campaign)
                            self.assertEqual(len(parsed.results), count)
                            for result in parsed.results:
                                values = dict(result.values)
                                self.assertEqual(result.status, "SKIP")
                                for field in ("source_intact", "executed", "cache_discipline",
                                              "P", "matches", "guards_outside", "post_guard",
                                              "overflow_band", "payload_mutations"):
                                    self.assertEqual(int(values[field], 0), 0, field)


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
