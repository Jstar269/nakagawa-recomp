# SPDX-License-Identifier: GPL-2.0-or-later

"""Regression tests for Prx._apply's R_MIPS_HI16/LO16 pairing.

2026-07-17: a source review found that the HI16 handler's forward scan for its
paired low half accepted the FIRST relocation of any type after it, rather than
specifically an R_MIPS_LO16 (or the PSP's R_MIPS_16 alternative). An unrelated
relocation record (R_MIPS_32, R_MIPS_26, ...) sitting between an HI16 and its
real LO16 in table order was misread as the low addend, corrupting the
reconstructed upper-16 immediate whenever the two words' sign bits disagreed.
These tests exercise Prx._apply directly (not the full ELF/section parser) so
each scenario is a small, explicit table of relocation records.
"""

import struct
import unittest

import prxload
from prxload import (
    R_MIPS_16,
    R_MIPS_26,
    R_MIPS_32,
    R_MIPS_GPREL16,
    R_MIPS_HI16,
    R_MIPS_LITERAL,
    R_MIPS_LO16,
    R_MIPS_NONE,
    R_MIPS_REL32,
)


def make_prx(words, seg_vaddr):
    """A Prx instance with just enough state for _apply: a flat little-endian
    word array at guest base 0, and a caller-supplied segment vaddr table."""
    prx = object.__new__(prxload.Prx)
    prx.lo = 0
    prx.seg_vaddr = list(seg_vaddr)
    mem = bytearray(len(words) * 4)
    for i, w in enumerate(words):
        struct.pack_into("<I", mem, i * 4, w & 0xFFFFFFFF)
    prx.mem = mem
    return prx


def info(rtype, ofs_seg=0, addr_seg=0):
    return (rtype & 0xFF) | ((ofs_seg & 0xFF) << 8) | ((addr_seg & 0xFF) << 16)


class Hi16Lo16PairingTests(unittest.TestCase):
    def test_unknown_relocation_type_fails_loudly(self):
        # Type 0xF is outside the recognized Type-A PSP relocation set.  Keep
        # the fixture minimal so the test reaches the central relocation
        # dispatcher with a valid segment and target word.
        prx = make_prx([0xDEADBEEF], seg_vaddr=[0])

        with self.assertRaisesRegex(
            ValueError,
            r"unsupported Type-A relocation type 0xf at offset 0x00000000"
            r" \(offset segment 0, target segment 0\)",
        ):
            prx._apply([(0, info(0xF))])

    def test_known_noop_relocation_types_remain_nonfatal(self):
        # These values are recognized Type-A categories, not unknown types:
        # NONE/GPREL16 are deliberate loader no-ops and LITERAL is firmware's
        # diagnostic-only relocation category.
        for rtype in (R_MIPS_NONE, R_MIPS_GPREL16, R_MIPS_LITERAL):
            with self.subTest(rtype=rtype):
                prx = make_prx([0xDEADBEEF], seg_vaddr=[0])
                prx._apply([(0, info(rtype))])
                self.assertEqual(prx.r32(0), 0xDEADBEEF)

    def test_known_but_unsupported_type_a_relocation_fails_loudly(self):
        # R_MIPS_REL32 is a known MIPS/PSP relocation vocabulary value, but it
        # is not supported by the Type-A section path (packed streams have a
        # separate dispatcher).
        prx = make_prx([0xDEADBEEF], seg_vaddr=[0])

        with self.assertRaisesRegex(ValueError, r"unsupported Type-A relocation type 0x3"):
            prx._apply([(0, info(R_MIPS_REL32))])

    def test_normal_hi16_lo16_pair(self):
        # lui $2,0x0010 ; addiu $2,$2,0x2000 relocated by +0x1000: the classic
        # case, no interference, sanity-checks the carry arithmetic itself.
        prx = make_prx([0x3C020010, 0x24422000], seg_vaddr=[0, 0x1000])
        rels = [(0, info(R_MIPS_HI16, addr_seg=1)), (4, info(R_MIPS_LO16, addr_seg=1))]
        prx._apply(rels)
        # True target = 0x00102000 + 0x1000 = 0x00103000 -> hi=0x0010, lo=0x3000.
        self.assertEqual(prx.r32(0) & 0xFFFF, 0x0010)
        self.assertEqual(prx.r32(4) & 0xFFFF, 0x3000)

    def test_unrelated_relocation_between_hi16_and_lo16_is_skipped(self):
        # HI16(field=0) ... unrelated R_MIPS_32 word (low16=0x7fff, no sign bit) ...
        # real LO16(raw=0) relocated by +0x10. The correct low addend (0) does not
        # cross the 0x8000 carry boundary, so hi must stay 0x0000. Mistaking the
        # unrelated word's low16=0x7fff for the addend crosses the boundary
        # (0x7fff + 0x10 = 0x800f, bit15 set) and wrongly bumps hi to 0x0001 --
        # this is the exact bug the fix removes.
        prx = make_prx(
            [0x3C020000, 0x12347FFF, 0x24420000],
            seg_vaddr=[0, 0x10],
        )
        rels = [
            (0, info(R_MIPS_HI16, addr_seg=1)),
            (4, info(R_MIPS_32, addr_seg=0)),
            (8, info(R_MIPS_LO16, addr_seg=1)),
        ]
        prx._apply(rels)
        self.assertEqual(prx.r32(0), 0x3C020000, "HI16 must skip the unrelated R_MIPS_32 record")

    def test_unrelated_relocation_types_are_all_skipped(self):
        # Same shape as above but the intervening record is R_MIPS_26 (a jal
        # target) rather than R_MIPS_32 -- any non-LO16/16 type must be skipped.
        prx = make_prx(
            [0x3C020000, 0x0BFFFFFF, 0x24420000],
            seg_vaddr=[0, 0x10],
        )
        rels = [
            (0, info(R_MIPS_HI16, addr_seg=1)),
            (4, info(R_MIPS_26, addr_seg=0)),
            (8, info(R_MIPS_LO16, addr_seg=1)),
        ]
        prx._apply(rels)
        self.assertEqual(prx.r32(0), 0x3C020000, "HI16 must skip the unrelated R_MIPS_26 record")

    def test_multiple_hi16_share_one_lo16(self):
        # Two HI16 records (e.g. two `lui`s addressing the same symbol) both
        # pair with the single LO16 that follows both of them.
        prx = make_prx(
            [0x3C020000, 0x3C030000, 0x24428100],
            seg_vaddr=[0, 0x10],
        )
        rels = [
            (0, info(R_MIPS_HI16, addr_seg=1)),
            (4, info(R_MIPS_HI16, addr_seg=1)),
            (8, info(R_MIPS_LO16, addr_seg=1)),
        ]
        prx._apply(rels)
        # raw lo16 = 0x8100 -> s16 = 0x8100-0x10000 = -0x7f00 (bit15 set, carry).
        # cur = 0 + (-0x7f00) + 0x10 = 0xffff8110 -> hi = 0xffff+1 = 0x0000.
        self.assertEqual(prx.r32(0) & 0xFFFF, 0x0000)
        self.assertEqual(prx.r32(4) & 0xFFFF, 0x0000)

    def test_psp_r_mips_16_is_a_valid_lo16_partner(self):
        # The PSP toolchain's alternative low-half relocation type (1) must be
        # accepted as a pairing target exactly like R_MIPS_LO16.
        prx = make_prx([0x3C020000, 0x24420000], seg_vaddr=[0, 0x8100])
        rels = [(0, info(R_MIPS_HI16, addr_seg=1)), (4, info(R_MIPS_16, addr_seg=1))]
        prx._apply(rels)
        # relocate_to alone (0x8100) crosses the carry boundary: cur=0x8100 -> hi=1.
        self.assertEqual(prx.r32(0) & 0xFFFF, 0x0001)

    def test_hi16_with_no_paired_lo16_applies_delta_alone(self):
        # A HI16 with nothing but more HI16es (or nothing at all) after it is
        # malformed/unpaired input; it must not silently borrow a stray word's
        # bits, and must not raise.
        prx = make_prx([0x3C020000], seg_vaddr=[0, 0x10000])
        rels = [(0, info(R_MIPS_HI16, addr_seg=1))]
        prx._apply(rels)
        self.assertEqual(prx.r32(0) & 0xFFFF, 0x0001)  # relocate_to=0x10000 alone -> hi=1


if __name__ == "__main__":
    unittest.main()
