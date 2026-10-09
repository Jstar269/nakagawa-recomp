# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Pin tools/prx_reloc_model.py to the runtime PRX loader.

Position-independent module translation is only correct if the value the model
predicts for every relocation site at a base is the value the runtime loader
(src/rt/prx_loader.c) writes there.  These tests build synthetic modules with
the clean-room loader suite's fixture builder, load each one through the real C
loader at several bases, and compare every loaded segment byte for byte with
``RelocationModel.segment_image``.  The refusal cases pin the forms the model
deliberately declines to express.
"""

import random
import struct
import unittest

import prx_reloc_model as model
from test_prx_loader_cleanroom import (
    ET_PSP,
    PT_REL_A,
    PT_REL_B,
    Module,
    PrxTestBase,
    b_cmd,
    b_stream,
    basic_seg_with_words,
    reloc_a,
)

# Bases chosen to move every relocation-sensitive bit the arena allows: zero,
# small, both sides of the 0x8000 carry boundary, and odd multiples of four.
BASES = (0x0, 0x40, 0x7FC0, 0x8000, 0x8004, 0x10000, 0x1C3A4, 0x24000)
SEG0 = 0x1000

B_FLAGS = [0x00, 0x04, 0x01, 0x03, 0x05, 0x09, 0x11]
SET32, REL00, REL01, REL10, REL00_KEEP, REL00_EXT = 2, 3, 4, 5, 6, 7


class ModelMatchesLoader(PrxTestBase):
    def assert_model_matches_loader(self, blob, bases=BASES):
        relocations = model.model_relocations(blob, "fixture")
        for base in bases:
            info = self.run_load(blob, base)
            self.assertEqual(info.get("result"), "ok", info.get("err"))
            self.assertEqual(len(info["mems"]), len(relocations.segments))
            for index, loaded in enumerate(info["mems"]):
                predicted = relocations.segment_image(index, base)
                if predicted != loaded:
                    for offset in range(0, len(loaded), 4):
                        got = loaded[offset:offset + 4]
                        want = predicted[offset:offset + 4]
                        if got != want:
                            self.fail(
                                f"base 0x{base:x} segment {index} offset 0x{offset:x}: "
                                f"loader {got.hex()} model {want.hex()}"
                            )
                    self.fail(f"base 0x{base:x} segment {index} differs")
        return relocations

    def test_format_a_every_kind(self):
        words = [
            0x11111111,  # kind 0
            0x24640005,  # kind 1 (16)
            0x00001000,  # kind 2 (32)
            0x0C000010,  # kind 4 (26, jal)
            0x3C010000,  # HI16 run member
            0x3C02FFFF,  # HI16 run member
            0x24218000,  # LO16 partner (carry case)
            0x3C030012,  # HI16 with a 32-bit partner
            0x00000040,  # 32-bit partner
            0x8C440010,  # kind 6 (LO16) on a load
            0x22222222,  # kind 8 (LITERAL)
        ]
        data, offs, memsz = basic_seg_with_words(words, vaddr=SEG0, bss=0x40)
        table = b"".join([
            reloc_a(offs[0], 0),
            reloc_a(offs[1], 1),
            reloc_a(offs[2], 2),
            reloc_a(offs[3], 4),
            reloc_a(offs[4], 5),
            reloc_a(offs[5], 5),
            reloc_a(offs[6], 6),
            reloc_a(offs[7], 5),
            reloc_a(offs[8], 2),
            reloc_a(offs[9], 6),
            reloc_a(offs[10], 8),
        ])
        module = Module()
        module.add_seg(SEG0, data, memsz)
        module.add_reloc(PT_REL_A, table)
        relocations = self.assert_model_matches_loader(module.build())
        kinds = {address - SEG0: site.kind for address, site in relocations.sites.items()}
        self.assertEqual(kinds[offs[1]], model.KIND_LO16)
        self.assertEqual(kinds[offs[2]], model.KIND_WORD32)
        self.assertEqual(kinds[offs[3]], model.KIND_JUMP26)
        self.assertEqual(kinds[offs[4]], model.KIND_HI16)
        self.assertEqual(kinds[offs[7]], model.KIND_HI16)
        self.assertNotIn(offs[0], kinds, "a NONE record makes no site")
        self.assertNotIn(offs[10], kinds, "a LITERAL record makes no site")

    def test_format_a_section_source_and_two_segments(self):
        data0, offs, mem0 = basic_seg_with_words(
            [0x3C050000, 0x24A50120, 0x0C000004, 0x00000010], vaddr=0)
        data1 = struct.pack("<4I", 0x00000008, 0, 0, 0)
        table = b"".join([
            reloc_a(offs[0], 5, oseg=0, aseg=1),
            reloc_a(offs[1], 6, oseg=0, aseg=1),
            reloc_a(offs[2], 4, oseg=0, aseg=0),
            reloc_a(offs[3], 2, oseg=0, aseg=1),
            reloc_a(0, 2, oseg=1, aseg=0),
        ])
        module = Module()
        module.add_seg(0, data0, mem0)
        module.add_seg(0x800, data1, 0x40)
        module.add_section(".rel.sceModule", PT_REL_A, 0, table)
        self.assert_model_matches_loader(module.build())

    def test_format_b_every_kind(self):
        words = [0x24640002, 0x00000009, 0x0C000020, 0x3C010000, 0x3C020000,
                 0x00000001, 0x3C030000, 0x24650044, 0x00000000, 0xFFFFFFFF]
        data0, offs, mem0 = basic_seg_with_words(words + [0] * 16, vaddr=SEG0)
        f, t, sw = 3, 3, 2
        kinds = [1, 2, 3, 4, 5, 6, 7]
        cmds = b""

        def at(offset):
            return b_cmd(SET32, 0, 0, 0, f, t, sw) + struct.pack("<I", offset)

        cmds += at(offs[0]) + b_cmd(REL00, 1, 1, 0, f, t, sw)           # kind 1, seg 1
        cmds += at(offs[1]) + b_cmd(REL00, 2, 2, 0, f, t, sw)           # kind 2, seg 2
        cmds += at(offs[2]) + b_cmd(REL00, 0, 3, 0, f, t, sw)           # kind 3
        cmds += at(offs[3]) + b_cmd(REL00_EXT, 1, 4, 0, f, t, sw) + struct.pack("<H", 0x8020)
        cmds += at(offs[4]) + b_cmd(REL00_KEEP, 1, 4, 0, f, t, sw)      # keeps 0x8020
        cmds += at(offs[5]) + b_cmd(REL00, 0, 2, 0, f, t, sw)           # breaks the chain
        cmds += at(offs[6]) + b_cmd(REL00_KEEP, 0, 4, 0, f, t, sw)      # addend 0
        cmds += at(offs[7]) + b_cmd(REL00, 2, 5, 0, f, t, sw)           # kind 5
        cmds += at(offs[8]) + b_cmd(REL00, 0, 6, 0, f, t, sw)           # kind 6 -> j
        cmds += at(offs[9]) + b_cmd(REL00, 0, 7, 0, f, t, sw)           # kind 7 -> jal
        blob_b = b_stream(f, t, B_FLAGS, kinds, cmds)
        module = Module()
        module.add_seg(SEG0, data0, mem0)
        module.add_seg(0x3000, bytes(64))
        module.add_seg(0x5000, bytes(64))
        module.add_reloc(PT_REL_B, blob_b)
        relocations = self.assert_model_matches_loader(module.build())
        forced = {address - SEG0: site.opcode for address, site in relocations.sites.items()}
        self.assertEqual(forced[offs[8]], model.OPCODE_J)
        self.assertEqual(forced[offs[9]], model.OPCODE_JAL)

    def test_format_b_offset_forms(self):
        data0, offs, mem0 = basic_seg_with_words([0] * 64, vaddr=SEG0)
        raw = bytearray(data0)
        first = offs[0]
        targets = (first + 0x10, first + 0x20, first + 0x30)
        for index, target in enumerate(targets):
            struct.pack_into("<I", raw, target, 0x3C000000 | (0x10 * index))
        f, t, sw = 3, 2, 1
        dw = 16 - f - sw - t
        cmds = b_cmd(SET32, 0, 0, 0, f, t, sw) + struct.pack("<I", targets[0] - 0x10)
        cmds += b_cmd(REL00, 0, 1, 0x10, f, t, sw)
        cmds += b_cmd(SET32, 0, 0, 0, f, t, sw) + struct.pack("<I", targets[1] + 0x10)
        cmds += b_cmd(REL01, 0, 1, (1 << dw) - 1, f, t, sw) + struct.pack("<H", 0xFFF0)
        cmds += b_cmd(REL10, 0, 1, 0, f, t, sw) + struct.pack("<I", targets[2])
        module = Module()
        module.add_seg(SEG0, bytes(raw), mem0)
        module.add_reloc(PT_REL_B, b_stream(f, t, B_FLAGS, [1], cmds))
        self.assert_model_matches_loader(module.build())

    def test_seeded_random_format_a_streams(self):
        generator = random.Random(0x704)
        for _round in range(12):
            count = 48
            words = [generator.getrandbits(32) for _ in range(count)]
            data, offs, memsz = basic_seg_with_words(words, vaddr=SEG0)
            records = []
            index = 0
            while index < count:
                choice = generator.randrange(6)
                if choice == 0 and index + 2 < count:
                    run = generator.randrange(1, 3)
                    run = min(run, count - index - 1)
                    for member in range(run):
                        records.append(reloc_a(offs[index + member], 5))
                    records.append(reloc_a(offs[index + run], generator.choice((1, 2, 6))))
                    index += run + 1
                    continue
                kind = (0, 1, 2, 4, 6, 8)[choice]
                records.append(reloc_a(offs[index], kind))
                index += 1
            module = Module()
            module.add_seg(SEG0, data, memsz)
            module.add_reloc(PT_REL_A, b"".join(records))
            self.assert_model_matches_loader(
                module.build(), bases=tuple(generator.randrange(0, 0x20000) & ~3 for _ in range(4)))


class ModelRefusals(PrxTestBase):
    def test_gprel16_is_refused_as_the_loader_refuses_it(self):
        data, offs, memsz = basic_seg_with_words([0x00621821], vaddr=SEG0)
        module = Module()
        module.add_seg(SEG0, data, memsz)
        module.add_reloc(PT_REL_A, reloc_a(offs[0], 7))
        blob = module.build()
        self.assert_fail(blob, needle="GPREL16")
        with self.assertRaisesRegex(model.RelocationModelError, "GPREL16"):
            model.model_relocations(blob)

    def test_twice_relocated_site_is_not_a_single_base_function(self):
        data, offs, memsz = basic_seg_with_words([0x00000010], vaddr=SEG0)
        module = Module()
        module.add_seg(SEG0, data, memsz)
        module.add_reloc(PT_REL_A, reloc_a(offs[0], 2) + reloc_a(offs[0], 2))
        blob = module.build()
        self.assertEqual(self.run_load(blob).get("result"), "ok",
                         "the loader itself accepts a stacked relocation")
        with self.assertRaisesRegex(model.RelocationModelError, "earlier record"):
            model.model_relocations(blob)

    def test_overlapping_unaligned_site_is_refused(self):
        data, offs, memsz = basic_seg_with_words([0x00000010, 0x00000020], vaddr=SEG0)
        module = Module()
        module.add_seg(SEG0, data, memsz)
        module.add_reloc(PT_REL_A, reloc_a(offs[0], 2) + reloc_a(offs[0] + 2, 2))
        with self.assertRaisesRegex(model.RelocationModelError, "earlier record"):
            model.model_relocations(module.build())

    def test_hi16_partner_relocated_earlier_is_refused(self):
        data, offs, memsz = basic_seg_with_words([0x24210010, 0x3C010000], vaddr=SEG0)
        module = Module()
        module.add_seg(SEG0, data, memsz)
        module.add_reloc(PT_REL_A, reloc_a(offs[0], 6) + reloc_a(offs[1], 5) + reloc_a(offs[0], 0))
        with self.assertRaisesRegex(model.RelocationModelError, "earlier record"):
            model.model_relocations(module.build())

    def test_hi16_run_without_partner_is_refused(self):
        data, offs, memsz = basic_seg_with_words([0x3C010000], vaddr=SEG0)
        module = Module()
        module.add_seg(SEG0, data, memsz)
        module.add_reloc(PT_REL_A, reloc_a(offs[0], 5))
        with self.assertRaisesRegex(model.RelocationModelError, "partner"):
            model.model_relocations(module.build())

    def test_fixed_address_module_without_relocations_models_no_sites(self):
        data, _offs, memsz = basic_seg_with_words([0x03E00008, 0], vaddr=SEG0)
        module = Module(e_type=2)
        module.add_seg(SEG0, data, memsz)
        relocations = model.model_relocations(module.build())
        self.assertFalse(relocations.relocatable)
        self.assertEqual(relocations.sites, {})
        self.assertEqual(relocations.load_low, SEG0)

    def test_fixed_address_module_with_relocations_is_refused(self):
        data, offs, memsz = basic_seg_with_words([0x00000010], vaddr=SEG0)
        module = Module(e_type=2)
        module.add_seg(SEG0, data, memsz)
        module.add_reloc(PT_REL_A, reloc_a(offs[0], 2))
        with self.assertRaisesRegex(model.RelocationModelError, "fixed-address"):
            model.model_relocations(module.build())


class SiteArithmetic(unittest.TestCase):
    def test_hi16_carries_at_the_signed_low_boundary(self):
        site = model.RelocationSite(0x10, model.KIND_HI16, 0x00017FF0)
        self.assertEqual(site.field(0x0), 0x0001)
        self.assertEqual(site.field(0x10), 0x0002, "0x18000 rounds up past the sign boundary")
        self.assertEqual(site.word(0x3C080000, 0x10), 0x3C080002)

    def test_lo16_wraps_and_keeps_the_opcode(self):
        site = model.RelocationSite(0x10, model.KIND_LO16, 0xFFF8)
        self.assertEqual(site.field(0x10), 0x0008)
        self.assertEqual(site.word(0x25080000 | 0xFFF8, 0x10), 0x25080008)

    def test_jump26_target_follows_the_base_and_forced_opcode(self):
        site = model.RelocationSite(0x40, model.KIND_JUMP26, 0x120, model.OPCODE_JAL)
        self.assertEqual(site.target(0x08A00000), 0x08A00120)
        self.assertEqual(site.word(0x08000000, 0x08A00000) >> 26, model.OPCODE_JAL)

    def test_lo16_and_hi16_pair_reconstructs_the_relocated_address(self):
        for address in (0x0, 0x7FFC, 0x8000, 0x12348, 0x1FFFC):
            hi = model.RelocationSite(0, model.KIND_HI16, address)
            lo = model.RelocationSite(4, model.KIND_LO16, address)
            for base in (0x08804000, 0x08A57100, 0x09FF8000):
                low = lo.field(base)
                signed = low - 0x10000 if low & 0x8000 else low
                self.assertEqual(((hi.field(base) << 16) + signed) & 0xFFFFFFFF,
                                 (address + base) & 0xFFFFFFFF)


if __name__ == "__main__":
    unittest.main()
