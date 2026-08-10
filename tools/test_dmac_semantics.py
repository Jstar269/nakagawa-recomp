# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Source-shape gates for the PSP DMAC copy path (issues #87, #328).

The PSP-visible behavior itself is proven executably by
``src/rt/hle_thread_selftest.c``, which enters both DMAC NIDs through the real
``sr_syscall`` registry and asserts return values and guest memory contents.
These assertions guard the two properties a behavioral test cannot express:

* the validation order in the source (nothing may touch guest memory or notify
  the GPU before every check has passed), and
* the #328 evidence rule -- the large-transfer truncation boundary is *not*
  established by any durable capture, so no ceiling constant may be hard-coded
  into the copy path.
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

    def test_every_check_precedes_any_guest_or_gpu_side_effect(self) -> None:
        """A failed request must not move a byte or dirty a GPU range.

        Guest address 0 is inside this runtime's flat arena, so the explicit
        null check is load-bearing: without it a NULL pointer passes span
        validation and the copy silently proceeds.
        """
        region = _dmac_region()
        size_check = region.index("if (n == 0u) return SCE_DMAC_ERROR_ILLEGAL_SIZE;")
        null_check = region.index("if (dst == 0u || src == 0u)")
        span_check = region.index("if (!sr_guest_span_readable(src, n) || !sr_guest_span_writable(dst, n))")
        copy = region.index("memmove(SR_HOST(dst), SR_HOST(src), n)")
        dirty = region.index("sr_gpu_vram_dirty(dst, n)")

        self.assertLess(size_check, null_check)
        self.assertLess(null_check, span_check)
        self.assertLess(span_check, copy, "spans must be validated before any copy")
        self.assertLess(copy, dirty, "the GPU is notified only after a real transfer")

    def test_overlap_safe_primitive(self) -> None:
        """Hardware showed both overlap directions landing memmove-correct and a
        same-pointer copy leaving the buffer intact, so a forward byte loop or a
        plain memcpy would diverge."""
        region = _dmac_region()
        self.assertIn("memmove(SR_HOST(dst), SR_HOST(src), n)", region)
        self.assertNotIn("memcpy(SR_HOST(dst), SR_HOST(src), n)", region)

    def test_both_copy_nids_register_in_the_shared_bulk_helper(self) -> None:
        """Both NIDs must register in the helper that the executable regression
        also initialises, so the test enters production registration rather
        than a test-only mapping."""
        text = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        start = text.index("static void hle_register_bulk_memory_handlers")
        registration = text[start : text.index("void sr_hle_init", start)]
        self.assertIn('sr_hle_register(0x617f3fe6, "sceDmacMemcpy", h_DmacMemcpy)', registration)
        self.assertIn(
            'sr_hle_register(0xd97f94d8, "sceDmacTryMemcpy", h_DmacTryMemcpy)', registration
        )
        # Exactly one registration each: a duplicate elsewhere would silently
        # win or lose depending on registry order.
        self.assertEqual(text.count('"sceDmacTryMemcpy"'), 1)
        self.assertEqual(text.count('"sceDmacMemcpy"'), 1)


class TestDmacUnresolvedCeiling(unittest.TestCase):
    """#328: the 0xC000 transfer ceiling is NOT an established hardware fact.

    The durable captures prove a ceiling exists (65536-byte transfers do not
    write their final byte) but localise it with a single positional sample at
    a single size, in a probe whose own verdict is FAIL. Sizes 32770..49152
    were never measured and the truncated region was never checked for
    prefix-contiguity. Encoding a ceiling would corrupt the tail of a real
    transfer if the true boundary differs.
    """

    CEILING_LITERALS = (
        "0xC000",
        "0xc000",
        "49152",
        "0xBFFF",
        "0xbfff",
        "49151",
    )

    def test_no_ceiling_constant_is_hard_coded_in_the_copy_path(self) -> None:
        region = _dmac_region()
        # Strip comments: the prose deliberately explains the retracted claim,
        # and must stay readable. Only executable source is under test.
        code = re.sub(r"/\*.*?\*/", "", region, flags=re.S)
        code = re.sub(r"//[^\n]*", "", code)
        for literal in self.CEILING_LITERALS:
            self.assertNotIn(
                literal,
                code,
                f"{literal} is an unproven DMA ceiling constant (#328); the copy "
                f"path must not encode a transfer limit that no durable capture "
                f"establishes",
            )

    def test_transfers_are_never_clamped(self) -> None:
        """The requested size is what gets copied.

        Scoped to the handler body: the requested size must reach memmove
        unmodified, so ``n`` may never be reassigned after it is read out of
        a2. A clamp to any ceiling would have to write to ``n`` (or pass a
        different expression to memmove) and is caught either way.
        """
        region = _dmac_region()
        body = region[region.index("static uint32_t h_DmacMemcpy") :]
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)

        self.assertIn("memmove(SR_HOST(dst), SR_HOST(src), n)", body)
        # The sole assignment to n is the argument read itself.
        assignments = re.findall(r"\bn\s*(?:=[^=]|[-+*/|&^]=|>>=|<<=)", body)
        self.assertEqual(
            len(assignments),
            1,
            f"the requested DMA size must reach memmove unmodified; found {assignments}",
        )
        self.assertIn("uint32_t dst = A0, src = A1, n = A2;", body)
        self.assertNotIn("++n", body)
        self.assertNotIn("--n", body)

    def test_evidence_boundary_is_a_report_not_a_limit(self) -> None:
        """The one size constant present marks where measurement stops, and it
        may only drive a report -- never the transfer length."""
        region = _dmac_region()
        self.assertIn("#define SR_DMAC_VERIFIED_FULL_MAX 32769u", region)
        self.assertIn(
            "if (n > SR_DMAC_VERIFIED_FULL_MAX) sr_dmac_note_unverified_size(n);", region
        )

    def test_no_busy_result_is_fabricated(self) -> None:
        """No capture in any session observed 0x80000021; the probe is
        single-threaded and cannot create a concurrent-DMA condition."""
        region = _dmac_region()
        self.assertNotIn("0x80000021", re.sub(r"/\*.*?\*/", "", region, flags=re.S))

    def test_the_unresolved_gap_stays_linked_to_its_issue(self) -> None:
        region = _dmac_region()
        self.assertIn("#328", region)
        self.assertIn("#87", region)


class TestDmacExecutableCoverage(unittest.TestCase):
    def test_regression_covers_both_nids_through_production_dispatch(self) -> None:
        text = (ROOT / "src" / "rt" / "hle_thread_selftest.c").read_text(encoding="utf-8")
        self.assertIn("test_dmac_semantics();", text)
        self.assertIn("test_dmac_hardware_semantics(NID_SCE_DMAC_MEMCPY", text)
        self.assertIn("test_dmac_hardware_semantics(NID_SCE_DMAC_TRY_MEMCPY", text)

    def test_regression_asserts_the_measured_hardware_cases(self) -> None:
        text = (ROOT / "src" / "rt" / "hle_thread_selftest.c").read_text(encoding="utf-8")
        for needle in (
            "PSP: zero size returns the illegal-size error",
            "PSP: a NULL destination returns the illegal-address error",
            "PSP: a NULL source returns the illegal-address error",
            "PSP: a rejected span leaves both buffers byte-for-byte unchanged",
            "PSP: a same-pointer self copy leaves the buffer unchanged",
            "PSP: a forward-overlapping copy is memmove-correct across the span",
            "PSP: a backward-overlapping copy is memmove-correct across the span",
            "an unmeasured oversize transfer is copied in full, not clamped to a guess",
        ):
            self.assertIn(needle, text)


if __name__ == "__main__":
    unittest.main()
