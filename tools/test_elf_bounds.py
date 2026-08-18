# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Synthetic malformed-ELF coverage for the shared offline parser envelope."""

from __future__ import annotations

import struct
import sys
import tempfile
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import analyze  # noqa: E402
import prxload  # noqa: E402
from elf_bounds import checked_mul, checked_span, image_extent  # noqa: E402


def elf32(*, file_size: int = 16, mem_size: int | None = None) -> bytearray:
    mem_size = file_size if mem_size is None else mem_size
    phoff = 52
    payload = phoff + 32
    blob = bytearray(payload + file_size)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into("<H", blob, 16, 2)  # ET_EXEC
    struct.pack_into("<III", blob, 24, 0, phoff, 0)
    struct.pack_into("<HHHHH", blob, 42, 32, 1, 0, 0, 0)
    struct.pack_into(
        "<8I", blob, phoff, 1, payload, 0, 0, file_size, mem_size, 5, 4
    )
    for i in range(file_size):
        blob[payload + i] = (i * 17 + 3) & 0xFF
    return blob


class TestElfBounds(unittest.TestCase):
    def test_valid_minimal_elf_is_still_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "valid.elf"
            path.write_bytes(elf32())
            loaded = prxload.Prx(str(path), 0)
            self.assertEqual(len(loaded.mem), 16)
            analyzed = analyze.Elf(str(path))
            self.assertEqual(analyzed.entry, 0)

    def test_truncated_header_is_controlled(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "short.elf"
            path.write_bytes(b"\x7fELF")
            with self.assertRaisesRegex(ValueError, "truncated ELF32 header"):
                prxload.Prx(str(path), 0)
            with self.assertRaisesRegex(ValueError, "truncated ELF32 header"):
                analyze.Elf(str(path))

    def test_program_table_and_file_extent_are_checked_before_slicing(self) -> None:
        blob = elf32(file_size=16)
        struct.pack_into("<I", blob, 28, len(blob) + 4)
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "bad-table.elf"
            path.write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "program-header table"):
                prxload.Prx(str(path), 0)

        blob = elf32(file_size=32)
        struct.pack_into("<I", blob, 52 + 4, len(blob) - 8)
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "bad-segment.elf"
            path.write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "program segment"):
                prxload.Prx(str(path), 0)

    def test_forged_bss_cannot_request_a_huge_flat_image(self) -> None:
        blob = elf32(file_size=16, mem_size=0xFFFFFFFF)
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "huge-bss.elf"
            path.write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "256 MiB"):
                prxload.Prx(str(path), 0)


class TestCheckedPrimitives(unittest.TestCase):
    """Edge matrix for the shared span/mul/extent primitives: UINT32_MAX and
    UINT64_MAX-scale values, zero sizes, and one-past-end boundaries."""

    def test_checked_span_boundaries(self) -> None:
        checked_span(100, 0, 100, "whole file")           # exact fit
        checked_span(100, 0, 0, "empty span at start")    # zero size anywhere is legal
        checked_span(100, 100, 0, "empty span at EOF")    # offset == total, size 0
        checked_span(100, 99, 1, "last byte")             # one-past-end exclusive
        checked_span(0, 0, 0, "empty file")               # degenerate but consistent
        for label, offset, size in (
            ("one past EOF", 100, 1),
            ("two past EOF", 101, 0),
            ("negative offset", -1, 1),
            ("negative size", 0, -1),
            ("size beyond tail", 50, 51),
            ("offset beyond total", 0xFFFFFFFF, 1),
            ("UINT32_MAX offset", 0xFFFFFFFF, 0xFFFFFFFF),
        ):
            with self.assertRaisesRegex(ValueError, "out of range", msg=label):
                checked_span(100, offset, size, label)

    def test_checked_mul_overflow_and_limits(self) -> None:
        self.assertEqual(checked_mul(0, 0xFFFFFFFF, "zero times huge"), 0)
        self.assertEqual(checked_mul(8, 1024, "ordinary"), 8192)
        self.assertEqual(checked_mul(0xFFFFFFFF, 0xFFFFFFFF, "huge pair"), 0xFFFFFFFE00000001)
        with self.assertRaisesRegex(ValueError, "negative extent"):
            checked_mul(-1, 8, "negative")
        with self.assertRaisesRegex(ValueError, "negative extent"):
            checked_mul(8, -1, "negative")
        self.assertEqual(checked_mul(16, 1024, "at limit", 16384), 16384)
        with self.assertRaisesRegex(ValueError, "exceeds"):
            checked_mul(16, 1025, "past limit", 16384)

    def test_image_extent_bounds(self) -> None:
        self.assertEqual(image_extent(0, 0, 0, "empty"), (0, 0))
        self.assertEqual(image_extent(0x08000000, 0x1000, 0x2000, "normal"), (0x08001000, 0x08003000))
        # An image ending exactly at the top of the 32-bit space is valid...
        self.assertEqual(
            image_extent(0xFF000000, 0x00FF0000, 1, "top edge"), (0xFFFF0000, 0xFFFF0001)
        )
        # ...one byte past it is not.
        with self.assertRaisesRegex(ValueError, "32-bit"):
            image_extent(0xFF000000, 0x00FF0000, 0x10001, "past top")
        with self.assertRaisesRegex(ValueError, "32-bit"):
            image_extent(0xFFFFFFFF, 1, 1, "base plus vaddr wraps")
        with self.assertRaisesRegex(ValueError, "negative"):
            image_extent(-1, 0, 0, "negative base")
        with self.assertRaisesRegex(ValueError, "negative"):
            image_extent(0, 0, -1, "negative size")
        with self.assertRaisesRegex(ValueError, "256 MiB"):
            image_extent(0, 0, 0x10000001, "one past image bound")


def elf32_with_shdrs(*, sec_type: int = 1, sec_off: int = 0, sec_size: int = 0,
                     name_off: int = 1) -> tuple[bytearray, int]:
    """Minimal ELF with two sections: a test section (index 0) and the
    section-string table (index 1). Returns the blob and the shstr offset."""
    blob = elf32(file_size=16)
    shoff = len(blob)
    blob.extend(bytes(40 * 2))
    struct.pack_into("<10I", blob, shoff, name_off, sec_type, 0, 0, sec_off, sec_size, 0, 0, 0, 0)
    struct.pack_into("<10I", blob, shoff + 40, 0, 3, 0, 0, shoff + 80, 2, 0, 0, 0, 0)
    blob.extend(b"\x00\x00")
    struct.pack_into("<I", blob, 32, shoff)
    struct.pack_into("<H", blob, 46, 40)
    struct.pack_into("<H", blob, 48, 2)
    struct.pack_into("<H", blob, 50, 1)
    return blob, shoff + 80


class TestElfEnvelopeEdges(unittest.TestCase):
    """Remaining fixed-table edge cases: multiply overflow, string-table
    bounds, NUL termination, filesz/memsz mismatch, table-at-EOF."""

    def test_huge_phentsize_times_phnum_is_rejected_before_slicing(self) -> None:
        blob = elf32(file_size=16)
        struct.pack_into("<HHHHH", blob, 42, 0xFFFF, 0xFFFF, 0, 0, 0)
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "huge-table.elf"
            path.write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "program-header table"):
                prxload.Prx(str(path), 0)

    def test_filesz_exceeding_memsz_is_rejected(self) -> None:
        blob = elf32(file_size=16, mem_size=8)
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "filesz-memsz.elf"
            path.write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "filesz exceeds memsz"):
                prxload.Prx(str(path), 0)

    def test_elf_without_load_segments_fails_closed(self) -> None:
        blob = elf32(file_size=16)
        struct.pack_into("<I", blob, 28, len(blob))  # phoff = EOF
        struct.pack_into("<H", blob, 44, 0)          # phnum = 0 -> no table, no PT_LOAD
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "no-loads.elf"
            path.write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "no PT_LOAD segments"):
                prxload.Prx(str(path), 0)

    def test_section_table_edges_are_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)

            blob, shstr_off = elf32_with_shdrs()
            struct.pack_into("<H", blob, 46, 0)  # e_shentsize = 0
            (root / "no-shentsize.elf").write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "section-header entry is too small"):
                prxload.Prx(str(root / "no-shentsize.elf"), 0)

            blob, shstr_off = elf32_with_shdrs()
            blob[shstr_off] = 1
            blob[shstr_off + 1] = 2  # string table with no NUL byte
            (root / "no-nul.elf").write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "unterminated section name"):
                prxload.Prx(str(root / "no-nul.elf"), 0)

            blob, shstr_off = elf32_with_shdrs()
            struct.pack_into("<I", blob, shstr_off - 80, 2)  # shdr[0].sh_name == shstr size
            (root / "name-ooo.elf").write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "section name offset is out of range"):
                prxload.Prx(str(root / "name-ooo.elf"), 0)

            blob, shstr_off = elf32_with_shdrs()
            struct.pack_into("<I", blob, shstr_off - 36, 8)  # shdr[1].sh_type = SHT_NOBITS
            (root / "strtab-nobits.elf").write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "section-string table cannot be SHT_NOBITS"):
                prxload.Prx(str(root / "strtab-nobits.elf"), 0)

    def test_section_bytes_must_be_file_backed(self) -> None:
        blob, _ = elf32_with_shdrs(sec_off=0, sec_size=0x100000)
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "huge-section.elf"
            path.write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "section"):
                prxload.Prx(str(path), 0)

    def test_shstrndx_out_of_range_is_rejected(self) -> None:
        blob, _ = elf32_with_shdrs()
        struct.pack_into("<H", blob, 50, 7)  # e_shstrndx = 7 >= shnum
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "bad-strndx.elf"
            path.write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "section-string-table index"):
                prxload.Prx(str(path), 0)

    def test_relocation_section_out_of_file_is_rejected(self) -> None:
        blob, _ = elf32_with_shdrs(sec_type=0x700000A0, sec_off=0, sec_size=0x100000)
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "reloc-ooo.elf"
            path.write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "section"):
                prxload.Prx(str(path), 0)


if __name__ == "__main__":
    unittest.main()
