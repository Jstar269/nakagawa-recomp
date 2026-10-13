# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Position-independent translation of runtime-placed guest modules (#704).

A module the game loads at run time lands wherever the guest allocator puts it,
so ``tools/codegen.py --extra-elf=<module>@runtime`` must translate it once, in
its own link space, with nothing in the generated code depending on a load base.
These tests drive the real generator over the source-owned overlay modules of
the platform ladder and pin the generated contract the runtime binds:

* every guest address of the module's own image is ``(sr_m<k>_base + link)``;
* an instruction whose immediate the loader relocates reads that immediate from
  the loaded image (``MEM_R16``), never from the link-time word;
* the module's functions are reached only through its ``SrModuleCode``
  descriptor, never registered at a fixed address;
* exported functions (module_stop, library exports) are translation entries
  even where no call or prologue heuristic would find them;
* a module the relocation model refuses still gets a descriptor naming why.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import codegen
import prx_reloc_model
from analyze import Elf, analyze, exec_ranges, in_ranges

_SPEC = importlib.util.spec_from_file_location(
    "platform_ladder_generator_runtime_modules",
    ROOT / "fixtures" / "platform_ladder" / "generate.py",
)
ladder = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(ladder)


def run_codegen(argv):
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        status = codegen.main(["codegen.py", *argv])
    return status, stdout.getvalue(), stderr.getvalue()


class RuntimeModuleTranslation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="codegen_runtime_modules_")
        root = Path(cls.tmp.name)
        cls.overlays = []
        for spec in ladder.OVERLAYS[:2]:
            path = root / spec.file_name
            path.write_bytes(ladder.build_overlay_prx(spec))
            cls.overlays.append((spec, path))
        main_prx, _psp = ladder.build_prx(ladder.PLANS["ladder-zero"])
        cls.main = root / "main.prx"
        cls.main.write_bytes(main_prx)
        cls.out = root / "with" / "g_recomp.c"
        cls.out.parent.mkdir()
        status, _out, cls.stderr = run_codegen([
            str(cls.main), str(cls.out), "--base=0x08940000", "--funcs-per-chunk=4",
            *[f"--extra-elf={path}@runtime" for _spec, path in cls.overlays],
        ])
        assert status == 0, cls.stderr
        cls.main_c = cls.out.read_text(encoding="ascii")
        cls.funcs_h = cls.out.with_name("g_recomp_funcs.h").read_text(encoding="ascii")
        cls.chunks = "\n".join(
            path.read_text(encoding="ascii")
            for path in sorted(cls.out.parent.glob("g_recomp_[0-9]*.c"))
        )
        cls.plain = root / "plain" / "g_recomp.c"
        cls.plain.parent.mkdir()
        status, _out, err = run_codegen([
            str(cls.main), str(cls.plain), "--base=0x08940000", "--funcs-per-chunk=4",
        ])
        assert status == 0, err

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def module_function(self, index, offset):
        match = re.search(
            rf"(?ms)^void m{index}_{offset:08x}\(CpuState \*s\) \{{.*?^\}}", self.chunks)
        self.assertIsNotNone(match, f"m{index}_{offset:08x} was not generated")
        return match.group(0)

    def test_exports_are_translation_entries(self):
        for spec, path in self.overlays:
            elf = Elf(path.read_bytes(), base=0)
            entries, _ranges = analyze(elf)
            for offset in (ladder.OVERLAY_START_OFF, ladder.OVERLAY_STOP_OFF,
                           ladder.OVERLAY_QUERY_OFF):
                self.assertIn(offset, entries,
                              f"{spec.file_name}: exported 0x{offset:x} is not an entry")

    def test_descriptor_carries_identity_extent_and_tables(self):
        for index, (spec, path) in enumerate(self.overlays):
            data = path.read_bytes()
            relocations = prx_reloc_model.model_relocations(data, spec.file_name)
            block = re.search(
                rf"(?s)static const SrModuleCode sr_m{index}_code = \{{(.*?)\}};", self.main_c)
            self.assertIsNotNone(block)
            text = block.group(1)
            self.assertIn(f'"{spec.file_name}", 0x{len(data):08x}u, '
                          f'0x{codegen.fnv1a64(data):016x}ull', text)
            self.assertIn(f"0x{relocations.load_low:08x}u, 0x{relocations.load_high:08x}u, "
                          f"0x{relocations.alignment:x}u, 1u", text)
            self.assertIn(f"NULL, &sr_m{index}_base", text)
            self.assertIn(f"    sr_module_code_register(&sr_m{index}_code);", self.main_c)
            self.assertIn(f"extern uint32_t sr_m{index}_base;", self.funcs_h)
            for offset in (ladder.OVERLAY_START_OFF, ladder.OVERLAY_STOP_OFF,
                           ladder.OVERLAY_QUERY_OFF, ladder.OVERLAY_STUB_OFF):
                self.assertRegex(self.main_c, rf"\{{ 0x{offset:08x}u, \d+u, m{index}_{offset:08x} \}}")
                self.assertIn(f"void m{index}_{offset:08x}(CpuState *s);", self.funcs_h)

    def test_module_functions_are_never_registered_at_fixed_addresses(self):
        registrations = re.findall(r"sr_register\(0x([0-9a-f]{8})u, ([fmr]\w+)\)", self.chunks)
        self.assertTrue(registrations, "the primary image still registers its functions")
        self.assertFalse([name for _addr, name in registrations if name.startswith("m")])
        plain_registrations = re.findall(
            r"sr_register\(0x([0-9a-f]{8})u, ([fr]\w+)\)",
            "\n".join(p.read_text(encoding="ascii")
                      for p in sorted(self.plain.parent.glob("g_recomp_[0-9]*.c"))))
        self.assertEqual(sorted(registrations), sorted(plain_registrations),
                         "adding runtime modules changes no primary-image registration")

    def test_relocated_immediates_read_the_loaded_image(self):
        for index, (spec, path) in enumerate(self.overlays):
            data = path.read_bytes()
            relocations = prx_reloc_model.model_relocations(data, spec.file_name)
            elf = Elf(data, base=0)
            ranges = exec_ranges(elf)
            immediate_sites = sorted(
                address for address, site in relocations.sites.items()
                if site.kind in (prx_reloc_model.KIND_LO16, prx_reloc_model.KIND_HI16)
                and in_ranges(address, ranges)
            )
            self.assertGreaterEqual(len(immediate_sites), 10)
            for address in immediate_sites:
                self.assertIn(f"MEM_R16((sr_m{index}_base + 0x{address:08x}u))", self.chunks,
                              f"{spec.file_name}: site 0x{address:x} does not read the image")
            start = self.module_function(index, ladder.OVERLAY_START_OFF)
            # lui t1, %hi(seed): the link-time immediate never reaches generated code.
            lui_site = immediate_sites[0]
            link_immediate = struct.unpack_from(
                "<I", data, ladder.OVERLAY_TEXT_FILE_OFFSET + lui_site)[0] & 0xFFFF
            self.assertIn(
                f"(((uint32_t)(uint16_t)MEM_R16((sr_m{index}_base + 0x{lui_site:08x}u))) << 16)",
                start)
            self.assertNotIn(f"(0x{link_immediate:x}u << 16)", start)

    def test_every_own_address_is_base_relative(self):
        for index, _overlay in enumerate(self.overlays):
            start = self.module_function(index, ladder.OVERLAY_START_OFF)
            self.assertIn(f"SR_YIELD(s, (sr_m{index}_base + 0x00000000u));", start)
            # jal mix: the link value and the direct call are both module-relative.
            self.assertRegex(start, rf"s->r\[31\] = \(sr_m{index}_base \+ 0x[0-9a-f]{{8}}u\);")
            self.assertIn(f"m{index}_{ladder.OVERLAY_MIX_OFF:08x}(s);", start)
            self.assertIn(f"m{index}_{ladder.OVERLAY_STUB_OFF:08x}(s);", start)
            # jalr through the relocated function pointer carries a module-relative resume.
            self.assertRegex(start, rf"dispatch_call\(s, _t, \(sr_m{index}_base \+ 0x[0-9a-f]{{8}}u\)\);")
            literals = re.findall(r"\b0x([0-9a-f]{8})u\b", start)
            self.assertFalse(
                [value for value in literals if 0x08000000 <= int(value, 16) < 0x0C000000],
                "a module body names no absolute user-RAM address")

    def test_same_bytes_translate_identically_at_any_index_order(self):
        # Two codegen runs that list the modules in the opposite order assign the
        # other index; the module bodies differ only by that index.
        reverse = Path(self.tmp.name) / "reverse" / "g_recomp.c"
        reverse.parent.mkdir()
        status, _out, err = run_codegen([
            str(self.main), str(reverse), "--base=0x08940000", "--funcs-per-chunk=4",
            *[f"--extra-elf={path}@runtime" for _spec, path in reversed(self.overlays)],
        ])
        self.assertEqual(status, 0, err)
        chunks = "\n".join(p.read_text(encoding="ascii")
                           for p in sorted(reverse.parent.glob("g_recomp_[0-9]*.c")))
        first = self.module_function(0, ladder.OVERLAY_START_OFF)
        swapped = re.search(r"(?ms)^void m1_00000000\(CpuState \*s\) \{.*?^\}", chunks).group(0)
        self.assertEqual(first.replace("m0_", "mX_").replace("sr_m0_", "sr_mX_"),
                         swapped.replace("m1_", "mX_").replace("sr_m1_", "sr_mX_"))


class RuntimeModuleBoundaries(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="codegen_runtime_boundaries_")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        main_prx, _psp = ladder.build_prx(ladder.PLANS["ladder-zero"])
        self.main = self.root / "main.prx"
        self.main.write_bytes(main_prx)

    def test_a_refused_relocation_stream_still_gets_a_named_descriptor(self):
        overlay = bytearray(ladder.build_overlay_prx(ladder.OVERLAYS[0]))
        # Rewrite the first relocation record's kind to GPREL16 (format A kind 7),
        # which the runtime loader refuses; the model refuses it the same way.
        envelope = prx_reloc_model.validate_elf32_envelope(bytes(overlay), "x")
        table = next(s for s in envelope["shdrs"] if s["typ"] == prx_reloc_model.PRX_RELOC_A)
        offset, info = struct.unpack_from("<II", overlay, table["off"])
        struct.pack_into("<II", overlay, table["off"], offset, (info & ~0xFF) | 7)
        path = self.root / "gprel.prx"
        path.write_bytes(bytes(overlay))
        out = self.root / "out" / "g_recomp.c"
        out.parent.mkdir()
        status, _out, err = run_codegen([
            str(self.main), str(out), "--base=0x08940000", f"--extra-elf={path}@runtime"])
        self.assertEqual(status, 0, err)
        main_c = out.read_text(encoding="ascii")
        self.assertRegex(main_c, r'"relocations cannot be translated: [^"]*GPREL16[^"]*", &sr_m0_base,')
        self.assertIn("NULL, 0u, NULL, 0u", main_c, "an untranslatable module carries no bodies")

    def test_runtime_module_names_must_be_distinct(self):
        first = self.root / "a" / "overlay.prx"
        second = self.root / "b" / "OVERLAY.PRX"
        for path in (first, second):
            path.parent.mkdir()
            path.write_bytes(ladder.build_overlay_prx(ladder.OVERLAYS[0]))
        out = self.root / "out" / "g_recomp.c"
        out.parent.mkdir()
        status, _out, err = run_codegen([
            str(self.main), str(out), "--base=0x08940000",
            f"--extra-elf={first}@runtime", f"--extra-elf={second}@runtime"])
        self.assertEqual(status, 2)
        self.assertIn("runtime-placed module names must be distinct", err)

    def test_fixed_address_image_binds_at_its_link_addresses(self):
        data, offs, memsz = basic_fixed_image()
        path = self.root / "fixed.prx"
        path.write_bytes(data)
        out = self.root / "out" / "g_recomp.c"
        out.parent.mkdir()
        status, _out, err = run_codegen([
            str(self.main), str(out), "--base=0x08940000", f"--extra-elf={path}@runtime"])
        self.assertEqual(status, 0, err)
        main_c = out.read_text(encoding="ascii")
        self.assertIn(f"0x{FIXED_LINK:08x}u, 0x{FIXED_LINK + memsz:08x}u, 0x4u, 0u", main_c)
        chunks = "\n".join(p.read_text(encoding="ascii")
                           for p in sorted(out.parent.glob("g_recomp_[0-9]*.c")))
        self.assertIn(f"void m0_{FIXED_LINK:08x}(CpuState *s)", chunks)


FIXED_LINK = 0x08B00000


def basic_fixed_image():
    """A fixed-address (ELF type 2) image: addiu v0, zero, 7 ; jr ra ; nop."""
    words = [0x24020007, 0x03E00008, 0x00000000]
    code = b"".join(struct.pack("<I", w) for w in words)
    phoff = 52
    body = phoff + 32
    header = (b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
              + struct.pack("<HHIIIIIHHHHHH", 2, 8, 1, FIXED_LINK, phoff, 0, 0,
                            52, 32, 1, 40, 0, 0))
    program = struct.pack("<8I", 1, body, FIXED_LINK, FIXED_LINK, len(code), len(code), 5, 4)
    return header + program + code, [0, 4, 8], len(code)


class RelocatedImmediateEmission(unittest.TestCase):
    def setUp(self):
        self.previous = codegen.ADDRESS_SPACE
        self.addCleanup(setattr, codegen, "ADDRESS_SPACE", self.previous)

    def space(self, sites, relocatable=True):
        class Model:
            def __init__(self):
                self.relocatable = relocatable

            def site(self, address):
                return sites.get(address)
        codegen.ADDRESS_SPACE = codegen.AddressSpace(
            base_symbol="sr_m7_base", prefix="m7", relocations=Model())
        return codegen.ADDRESS_SPACE

    def test_lo16_and_hi16_sites_read_their_immediate(self):
        space = self.space({
            0x10: prx_reloc_model.RelocationSite(0x10, "hi16", 0x2040),
            0x14: prx_reloc_model.RelocationSite(0x14, "lo16", 0x2040),
        })
        lui = space.instruction_word(0x10, 0x3C080000)           # lui t0, 0
        addiu = space.instruction_word(0x14, 0x25082040)         # addiu t0, t0, 0x2040
        lui_c, _, _ = codegen.effect(0x10, lui)
        addiu_c, _, _ = codegen.effect(0x14, addiu)
        self.assertEqual(lui_c,
                         "s->r[8] = (((uint32_t)(uint16_t)MEM_R16((sr_m7_base + 0x00000010u))) << 16);")
        self.assertEqual(addiu_c,
                         "s->r[8] = (s->r[8] + ((uint32_t)(int32_t)(int16_t)(uint16_t)"
                         "MEM_R16((sr_m7_base + 0x00000014u))));")

    def test_lo16_site_on_ll_and_sc_reads_its_immediate(self):
        # ll/sc address through simm() exactly like lw/sw, so a %lo() operand on a
        # lock word in a relocatable module is expressible, not a refusal.
        space = self.space({
            0x30: prx_reloc_model.RelocationSite(0x30, "lo16", 0x2040),
            0x34: prx_reloc_model.RelocationSite(0x34, "lo16", 0x2040),
        })
        ll = space.instruction_word(0x30, 0xC1092040)            # ll t1, 0x2040(t0)
        sc = space.instruction_word(0x34, 0xE1092040)            # sc t1, 0x2040(t0)
        relocated = ("((uint32_t)(int32_t)(int16_t)(uint16_t)"
                     "MEM_R16((sr_m7_base + 0x000000{:02x}u)))")
        ll_c, _, _ = codegen.effect(0x30, ll)
        sc_c, _, _ = codegen.effect(0x34, sc)
        self.assertIn("uint32_t _ea = s->r[8] + " + relocated.format(0x30) + ";", ll_c)
        self.assertIn("uint32_t _ea = s->r[8] + " + relocated.format(0x34) + ";", sc_c)

    def test_relocations_the_translation_cannot_express_fail_closed(self):
        space = self.space({
            0x20: prx_reloc_model.RelocationSite(0x20, "lo16", 0x10),
            0x24: prx_reloc_model.RelocationSite(0x24, "word32", 0x10),
            0x2a: prx_reloc_model.RelocationSite(0x2a, "lo16", 0x10),
        })
        with self.assertRaisesRegex(codegen.Unsupported, "lo16 relocation on opcode 0x04"):
            space.instruction_word(0x20, 0x10000004)              # beq: no immediate operand
        with self.assertRaisesRegex(codegen.Unsupported, "word32 relocation on an executed word"):
            space.instruction_word(0x24, 0x24020001)
        with self.assertRaisesRegex(codegen.Unsupported, "straddles"):
            space.instruction_word(0x28, 0x24020001)
        with self.assertRaisesRegex(codegen.Unsupported, "unrelocated absolute jump"):
            space.check_jump(0x40, 0x0C000010)

    def test_a_fixed_address_image_keeps_its_absolute_jumps(self):
        space = self.space({}, relocatable=False)
        space.check_jump(0x40, 0x0C000010)                       # no refusal
        self.assertEqual(codegen.G(0x08B00000), "(sr_m7_base + 0x08b00000u)")

    def test_relocated_immediate_is_not_a_translation_time_constant(self):
        word = codegen.RelocatedWord(0x24020005, "MEM_R16(x)")
        with self.assertRaises(codegen.Unsupported):
            codegen.s16(word)

    def test_module_identity_digest_matches_the_runtime_vectors(self):
        self.assertEqual(codegen.fnv1a64(b""), 0xCBF29CE484222325)
        self.assertEqual(codegen.fnv1a64(b"a"), 0xAF63DC4C8601EC8C)


if __name__ == "__main__":
    unittest.main()
