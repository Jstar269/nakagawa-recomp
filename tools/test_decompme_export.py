#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for tools/decompme_export.py (decompilation track, read-only exporter).

Hermetic tests cover the pure pieces - the minimal MIPS ELF object (built then
parsed back), the base context, and the function-extent bounding. An optional
integration test runs the exporter against the private place_game_here/EBOOT.elf
if present, checking only structural invariants (it never prints game bytes) and
skipping cleanly when the private input is absent.
"""

from __future__ import annotations

import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decompme_export as dx  # noqa: E402

REPO = dx.REPO
EBOOT = REPO / "place_game_here" / "EBOOT.elf"


def _parse_min_obj(obj: bytes) -> dict:
    """Parse the subset of ELF that min_mips_elf_object emits, for verification."""
    assert obj[:4] == b"\x7fELF", "bad ELF magic"
    (e_type, e_machine, _ver, _entry, _phoff, e_shoff, _flags,
     _eh, _phe, _phn, _she, e_shnum, e_shstrndx) = struct.unpack("<HHIIIIIHHHHHH", obj[16:52])

    def shdr(i):
        o = e_shoff + i * 40
        return struct.unpack("<10I", obj[o:o + 40])

    sections = [shdr(i) for i in range(e_shnum)]
    shstr_off = sections[e_shstrndx][4]
    shstr_size = sections[e_shstrndx][5]
    shstr = obj[shstr_off:shstr_off + shstr_size]

    def name_of(nameoff):
        return shstr[nameoff:shstr.index(b"\x00", nameoff)].decode("ascii")

    names = [name_of(s[0]) for s in sections]
    ti = names.index(".text")
    sti = names.index(".symtab")
    stri = names.index(".strtab")
    text = obj[sections[ti][4]:sections[ti][4] + sections[ti][5]]
    symtab = obj[sections[sti][4]:sections[sti][4] + sections[sti][5]]
    strtab = obj[sections[stri][4]:sections[stri][4] + sections[stri][5]]
    st_name, _val, st_size, _info, _other, st_shndx = struct.unpack("<IIIBBH", symtab[16:32])
    symname = strtab[st_name:strtab.index(b"\x00", st_name)].decode("ascii")
    return {"e_type": e_type, "e_machine": e_machine, "little_endian": obj[5] == 1,
            "class32": obj[4] == 1, "text": text, "symname": symname,
            "st_size": st_size, "st_shndx": st_shndx, "text_index": ti}


class TestMinMipsElfObject(unittest.TestCase):
    def test_roundtrip(self):
        body = bytes(range(64)) * 3  # 192 bytes, not 4-aligned count-wise is fine
        obj = dx.min_mips_elf_object(body, name="f_deadbeef")
        p = _parse_min_obj(obj)
        self.assertTrue(p["class32"] and p["little_endian"], "must be 32-bit little-endian")
        self.assertEqual(p["e_type"], 1, "ET_REL")
        self.assertEqual(p["e_machine"], 8, "EM_MIPS")
        self.assertEqual(p["text"], body, ".text must hold the exact function bytes")
        self.assertEqual(p["symname"], "f_deadbeef")
        self.assertEqual(p["st_size"], len(body), "symbol size must equal the function size")
        self.assertEqual(p["st_shndx"], p["text_index"], "symbol must point at .text")

    def test_odd_length_body(self):
        body = b"\x01\x02\x03"  # exercises section-offset alignment
        obj = dx.min_mips_elf_object(body, name="x")
        p = _parse_min_obj(obj)
        self.assertEqual(p["text"], body)
        self.assertEqual(p["st_size"], 3)


class TestContextAndExtent(unittest.TestCase):
    def test_context_has_base_typedefs(self):
        ctx = dx.gen_context_c()
        for t in ("u8", "u16", "u32", "s32", "f32", "f64"):
            self.assertIn(f" {t};", ctx)

    def test_func_extent_bounds_to_next_start(self):
        starts = {0x1000, 0x1100, 0x1200}
        ranges = [(0x1000, 0x2000)]
        self.assertEqual(dx.func_extent(0x1000, starts, ranges), (0x1000, 0x1100))

    def test_func_extent_last_uses_range_end(self):
        starts = {0x1000, 0x1200}
        ranges = [(0x1000, 0x2000)]
        self.assertEqual(dx.func_extent(0x1200, starts, ranges), (0x1200, 0x2000))

    def test_func_extent_out_of_range(self):
        self.assertIsNone(dx.func_extent(0x500, {0x1000}, [(0x1000, 0x2000)]))


@unittest.skipUnless(EBOOT.exists(), "private place_game_here/EBOOT.elf not present")
class TestExportAgainstRealEboot(unittest.TestCase):
    """Structure-only end-to-end check on the real game input. Never prints bytes."""

    def test_bundle_structure(self):
        sys.path.insert(0, dx.TOOLS_DIR)
        import analyze
        elf = analyze.Elf(str(EBOOT), base=0)
        starts, ranges = analyze.analyze(elf)
        # Pick a real function start that has readable bytes.
        addr = None
        for s in sorted(starts):
            ext = dx.func_extent(s, starts, ranges)
            if ext and elf.read_at_vaddr(s, min(64, ext[1] - ext[0])):
                addr = s
                break
        self.assertIsNotNone(addr, "no readable function start found")

        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "f"
            meta = dx.export_function(str(EBOOT), addr, 0, out, name=f"f_{addr:08x}")
            # Files exist.
            for fn in ("metadata.json", "context.c", "function.bin", "target.o", "starter.c"):
                self.assertTrue((out / fn).exists(), f"missing {fn}")
            # Metadata is internally consistent with the emitted bytes.
            fb = (out / "function.bin").read_bytes()
            self.assertEqual(len(fb), meta["size"])
            self.assertEqual(dx.sha256_hex(fb), meta["sha256"])
            # target.o embeds exactly those bytes.
            p = _parse_min_obj((out / "target.o").read_bytes())
            self.assertEqual(p["text"], fb)
            self.assertEqual(p["e_machine"], 8)
            # metadata.json is valid JSON with a commit field.
            m = json.loads((out / "metadata.json").read_text())
            self.assertIn("nakagawa_commit", m)


if __name__ == "__main__":
    unittest.main()
