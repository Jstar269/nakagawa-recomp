# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Synthetic malformed-ELF, ProgramImage, and CFG coverage."""

from __future__ import annotations

import hashlib
import json
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
from test_import_name_safety import build_synthetic_import_prx  # noqa: E402


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


def make_elf(loads, *, entry=0):
    """Build a tiny little-endian ET_EXEC with only program headers."""
    phoff = 52
    payload_offset = phoff + 32 * len(loads)
    offsets = []
    cursor = payload_offset
    for _vaddr, words, _memsz, _flags, _align in loads:
        offsets.append(cursor)
        cursor += len(words) * 4
    blob = bytearray(cursor)
    struct.pack_into(
        "<HHIIIIIHHHHHH", blob, 16,
        2, 8, 1, entry, phoff, 0, 0, 52, 32, len(loads), 0, 0, 0,
    )
    for index, ((vaddr, words, memsz, flags, align), offset) in enumerate(zip(loads, offsets)):
        struct.pack_into(
            "<8I", blob, phoff + index * 32,
            1, offset, vaddr, vaddr, len(words) * 4, memsz, flags, align,
        )
        for word_index, word in enumerate(words):
            struct.pack_into("<I", blob, offset + word_index * 4, word & 0xFFFFFFFF)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    return blob


def make_relocation_elf(*, info=0, section_type=0x700000A0):
    """Build a valid section table with one out-of-range PSP type-A relocation."""
    phoff = 52
    payload_offset = phoff + 32
    shstr = b"\x00.shstrtab\x00.reloc\x00"
    shstr_offset = payload_offset + 0x40
    reloc_offset = payload_offset + 0x60
    shoff = payload_offset + 0x70
    blob = bytearray(max(shoff + 3 * 40, payload_offset + 0x100))
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into(
        "<HHIIIIIHHHHHH", blob, 16,
        2, 8, 1, 0, phoff, shoff, 0, 52, 32, 1, 40, 3, 1,
    )
    struct.pack_into("<8I", blob, phoff, 1, payload_offset, 0, 0, 0x100, 0x100, 5, 4)
    blob[shstr_offset:shstr_offset + len(shstr)] = shstr
    struct.pack_into("<II", blob, reloc_offset, 0x200, info)
    # Null section, section-string table, and PSP type-A relocation section.
    struct.pack_into("<10I", blob, shoff + 40, 1, 3, 0, 0, shstr_offset, len(shstr), 0, 0, 1, 0)
    struct.pack_into(
        "<10I", blob, shoff + 80,
        shstr.index(b".reloc"), section_type, 0, 0, reloc_offset, 8, 0, 0, 4, 8,

    )
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

    def test_program_image_matches_legacy_flat_loader_for_a_valid_fixture(self) -> None:
        blob = make_elf([(0, [0x03E00008], 4, 5, 4)])
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "same.elf"
            path.write_bytes(blob)
            image = prxload.load_program_image(path, base=0)
            legacy = prxload.Prx(str(path), 0)
        self.assertEqual(image.flat_bytes, bytes(legacy.mem))
        self.assertEqual(image.image_start, legacy.lo)
        self.assertEqual(image.entry_point, legacy.entry)

    def test_cfg_entry_set_compares_with_the_legacy_analyzer(self) -> None:
        blob = make_elf([
            (0, [0x0C000040, 0, 0x03E00008, 0], 16, 5, 4),
        ], entry=0)
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "legacy-compare.elf"
            path.write_bytes(blob)
            image = prxload.load_program_image(path, base=0)
            legacy = analyze.Elf(str(path))
            legacy_entries, legacy_ranges = analyze.analyze(legacy)
        report = analyze.canonical_cfg_report(image, ranges=legacy_ranges, entries=legacy_entries)
        self.assertEqual(analyze.cfg_compatibility_findings(report, legacy_entries), [])

    def test_legacy_readers_cap_hostile_input_before_parsing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "too-large-for-test.elf"
            path.write_bytes(b"0123456789")
            old_prx_limit = prxload.MAX_ELF_FILE_BYTES
            old_analyze_limit = analyze.MAX_ELF_FILE_BYTES
            prxload.MAX_ELF_FILE_BYTES = 4
            analyze.MAX_ELF_FILE_BYTES = 4
            try:
                with self.assertRaisesRegex(ValueError, "256 MiB"):
                    prxload.Prx(str(path), 0)
                with self.assertRaisesRegex(ValueError, "256 MiB"):
                    analyze.Elf(str(path))
            finally:
                prxload.MAX_ELF_FILE_BYTES = old_prx_limit
                analyze.MAX_ELF_FILE_BYTES = old_analyze_limit

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

    def test_empty_segment_still_requires_an_in_file_source_offset(self) -> None:
        blob = elf32(file_size=0, mem_size=0)
        struct.pack_into("<I", blob, 52 + 4, len(blob) + 1)
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "empty-segment-oob.elf"
            path.write_bytes(blob)
            with self.assertRaisesRegex(ValueError, "program segment"):
                analyze.Elf(str(path))
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
class ProgramImageTests(unittest.TestCase):
    def write_blob(self, root, name, blob):
        path = root / name
        path.write_bytes(blob)
        return path

    def test_valid_image_is_immutable_and_contains_zero_fill(self):
        blob = make_elf([(0, [0x03E00008], 8, 5, 4)])
        with tempfile.TemporaryDirectory() as td:
            path = self.write_blob(Path(td), "valid.elf", blob)
            image = prxload.load_program_image(path, base=0x1000)

        self.assertEqual(image.schema_version, 1)
        self.assertEqual(image.source_size, len(blob))
        self.assertEqual(image.source_sha256, hashlib.sha256(blob).hexdigest())
        self.assertEqual(image.entry_point, 0x1000)
        self.assertEqual(image.segments[0].permissions, "rx")
        self.assertEqual(image.segments[0].zero_fill.as_dict(), {"start": 0x1004, "end": 0x1008})
        self.assertEqual(image.read_at_vaddr(0x1004, 4), b"\0\0\0\0")
        self.assertIsNone(image.read_at_vaddr(0x1008, 1))
        self.assertIsNone(image.read_at_vaddr(None, 0))
        self.assertIsNone(image.read_at_vaddr(0, 0))
        cfg = analyze.canonical_cfg_report(image, entries=[image.entry_point])
        self.assertEqual(cfg["executable_intervals"], [{"start": 0x1000, "end": 0x1008}])
        self.assertEqual(cfg["instructions"][0]["address"], 0x1000)
        with self.assertRaises((AttributeError, TypeError)):
            image.flat_bytes[0] = 1
        with self.assertRaises((AttributeError, TypeError)):
            image.load_base = 0
        rendered = prxload.canonical_program_image_json(image)
        self.assertEqual(rendered, prxload.canonical_program_image_json(image))
        self.assertEqual(rendered, json.dumps(json.loads(rendered), ensure_ascii=True, sort_keys=True, separators=(",", ":")) + "\n")

    def assert_invalid(self, blob, expected_code, *, base=0, name="invalid.elf"):
        with tempfile.TemporaryDirectory() as td:
            path = self.write_blob(Path(td), name, blob)
            with self.assertRaises(prxload.ProgramImageValidationError) as context:
                prxload.load_program_image(path, base=base)
        self.assertIn(expected_code, {finding.code for finding in context.exception.findings})

    def test_security_negative_fixtures_are_structured_and_fail_closed(self):
        self.assert_invalid(b"\x7fELF", "elf-envelope", name="truncated.elf")

        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "too-large.elf"
            path.write_bytes(b"0123456789")
            original_limit = prxload.MAX_ELF_FILE_BYTES
            prxload.MAX_ELF_FILE_BYTES = 4
            try:
                with self.assertRaises(prxload.ProgramImageValidationError) as context:
                    prxload.load_program_image(path)
            finally:
                prxload.MAX_ELF_FILE_BYTES = original_limit
        self.assertIn("source-too-large", {finding.code for finding in context.exception.findings})

        with self.assertRaises(prxload.ProgramImageValidationError) as context:
            prxload.load_program_image(None)
        self.assertIn("source-read", {finding.code for finding in context.exception.findings})

        filesz_gt_memsz = make_elf([(0, [0x12345678], 2, 5, 4)])
        self.assert_invalid(filesz_gt_memsz, "elf-envelope", name="filesz-gt-memsz.elf")

        source_oob = make_elf([(0, [0x12345678], 4, 5, 4)])
        struct.pack_into("<I", source_oob, 52 + 4, len(source_oob) + 8)
        self.assert_invalid(source_oob, "elf-envelope", name="source-oob.elf")

        overflow_table = elf32()
        struct.pack_into("<HH", overflow_table, 42, 0xFFFF, 0xFFFF)
        self.assert_invalid(overflow_table, "elf-envelope", name="overflow-table.elf")

        invalid_alignment = make_elf([(0, [0x12345678], 4, 5, 3)])
        self.assert_invalid(invalid_alignment, "alignment-invalid", name="alignment.elf")

        self.assert_invalid(
            make_elf([(0, [0x12345678], 4, 5, 4)]),
            "guest-destination-oob", base=0xFFFFFFFE, name="guest-oob.elf",
        )

        overlap = make_elf([
            (0, [0x11111111, 0x22222222], 8, 5, 1),
            (4, [0x33333333, 0x44444444], 8, 5, 1),
        ])
        self.assert_invalid(overlap, "segment-overlap", name="overlap.elf")

        self.assert_invalid(make_relocation_elf(), "relocation-target-oob", name="reloc-oob.elf")
        self.assert_invalid(
            make_relocation_elf(info=0x00010000),
            "relocation-segment-oob", name="reloc-segment-oob.elf",
        )
        self.assert_invalid(
            make_relocation_elf(section_type=0x700000A1),
            "relocation-packed-invalid", name="reloc-packed-invalid.elf",
        )

        section_span = make_relocation_elf(section_type=1)
        # The section is file-backed and structurally present, but its virtual
        # span wraps the guest address space.
        section_header = 52 + 32 + 0x70 + 80
        struct.pack_into("<I", section_span, section_header + 12, 0xFFFFFFFC)
        self.assert_invalid(section_span, "section-address-span-oob", name="section-span-oob.elf")

    def test_rebasing_is_consistent_for_nonzero_link_time_addresses(self):
        blob = make_elf([(0x100, [0x03E00008], 4, 5, 4)], entry=0x100)
        with tempfile.TemporaryDirectory() as td:
            path = self.write_blob(Path(td), "rebased.elf", blob)
            image = prxload.load_program_image(path, base=0x1000)
        self.assertEqual(image.entry_point, 0x1100)
        self.assertEqual(image.segments[0].guest_start, 0x1100)
        self.assertEqual(image.read_at_vaddr(0x1100, 4), struct.pack("<I", 0x03E00008))

    def test_rebased_psp_metadata_pointers_are_not_double_rebased(self):
        base = 0x08804000
        blob, _ = build_synthetic_import_prx(b"sceDisplay", base)
        blob = bytearray(blob)
        # The fixture models an already-rebased metadata table.  Keep its
        # intentionally minimal segment alignment permissive for this adapter
        # regression; alignment rejection itself has a separate negative case.
        struct.pack_into("<I", blob, 52 + 28, 0)
        with tempfile.TemporaryDirectory() as td:
            path = self.write_blob(Path(td), "rebased-metadata.prx", blob)
            image = prxload.load_program_image(path, base=base)
        self.assertEqual(image.imports[0].library, "sceDisplay")
        self.assertEqual(image.imports[0].nids, (0x12345678,))
        self.assertEqual(image.imports[0].stub_table, base + 0x80)

    def test_export_pointer_must_resolve_inside_the_validated_image(self):
        base = 0x08804000
        name = b"sceSynthetic"
        blob, _ = build_synthetic_import_prx(name, base)
        blob = bytearray(blob)
        data_off = 0x100
        module_off = 0x40
        name_off = 0x90
        nid_off = (name_off + len(name) + 1 + 3) & ~3
        entry_off = nid_off + 4
        # Turn the fixture's library entry into an export and make its function
        # pointer leave the only validated PT_LOAD segment.
        struct.pack_into("<2I", blob, data_off + module_off + 36, base + entry_off, base + entry_off + 20)
        struct.pack_into("<2I", blob, data_off + module_off + 44, 0, 0)
        struct.pack_into("<I", blob, data_off + nid_off, base + 0x5000)
        self.assert_invalid(blob, "pointer-target-oob", base=base, name="export-pointer-oob.prx")

    def test_string_bounds_cover_missing_nul_and_overlong_import_names(self):
        loads = [{"guest_start": 0, "guest_end": 16, "filesz": 16, "off": 0}]
        short_findings = []
        self.assertEqual(
            prxload._program_image_cstr(b"A" * 16, loads, 1, "import.name", short_findings), ""
        )
        self.assertIn("string-oob", {finding.code for finding in short_findings})

        long_findings = []
        self.assertEqual(
            prxload._program_image_cstr(
                b"B" * 2048,
                [{"guest_start": 0, "guest_end": 2048, "filesz": 2048, "off": 0}],
                1, "import.name", long_findings,
            ), ""
        )
        self.assertIn("string-overlong", {finding.code for finding in long_findings})


class FakeCodeImage:
    def __init__(self, words, data=b""):
        self.words = dict(words)
        self.data = data
        self.reloc = None
        self.sections = []
        self.executable_intervals = ()

    def read_at_vaddr(self, address, size):
        if size == 0:
            return b""
        raw = bytearray()
        for offset in range(size):
            byte_address = address + offset
            word_address = byte_address & ~3
            if word_address not in self.words:
                return None
            raw.append((self.words[word_address] >> ((byte_address & 3) * 8)) & 0xFF)
        return bytes(raw)


class DenseCfgScaleImage:
    """Deterministic source-owned CFG scale fixture with no retail data."""

    def __init__(self, *, node_count=200_000, owner_count=5_000, base=0x00100000):
        if node_count % owner_count:
            raise ValueError("node_count must divide evenly by owner_count")
        block_size = node_count // owner_count
        if block_size < 24:
            raise ValueError("fixture blocks need room for control-flow cases")
        self.base = base
        self.node_count = node_count
        self.owner_count = owner_count
        self.shared_target = base + 10 * 4
        words = [0] * node_count
        self.entries = [base + owner * block_size * 4 for owner in range(owner_count)]
        for owner in range(owner_count):
            start = owner * block_size
            address = base + start * 4
            # Every block has a branch-likely edge and delay slot.  The target
            # and not-taken paths both remain inside the block.
            words[start + 2] = branch(20, 1, 2, address + 8, address + 32)
            # Two direct jumps converge on a non-entry node to force owner
            # conflicts without making the shared node a new callable owner.
            if owner in (1, owner_count // 2):
                words[start + 6] = j(self.shared_target)
            elif owner == 2:
                words[start + 6] = j(self.entries[-1])
            elif owner == 3:
                words[start + 6] = branch(4, 0, 0, address + 24, self.shared_target)
            else:
                words[start + 12] = jal(self.entries[(owner + 1) % owner_count])
            # A computed jump leaves the remainder unowned in selected blocks.
            if owner % 400 == 0:
                words[start + 16] = jr(8)
                words[start + 18] = 0x012A4020  # nonzero unowned executable word
            # Every 250th block intentionally falls through into the next
            # callable entry, producing continuation/interior-entry evidence.
            elif owner % 250 != 0:
                words[start + 18] = jr(31)
            # All other blocks terminate at index 18 with a delay-slot NOP.
            words[start + 19] = 0

        self._blob = bytearray(node_count * 4 + 2)
        for index, word in enumerate(words):
            struct.pack_into("<I", self._blob, index * 4, word & 0xFFFFFFFF)
        self.data = struct.pack("<I", self.shared_target)
        self.reloc = None
        self.sections = [
            {
                "nm": ".rodata",
                "typ": 1,
                "flags": 0,
                "addr": 0x90000000,
                "off": 0,
                "size": 4,
            }
        ]
        self.executable_intervals = (
            (base, base + node_count * 4 + 2),
        )

    def read_at_vaddr(self, address, size):
        if not isinstance(address, int) or not isinstance(size, int) or size < 0:
            return None
        offset = address - self.base
        if offset < 0 or offset > len(self._blob):
            return None
        if size == 0:
            return b""
        if offset + size > len(self._blob):
            return None
        return bytes(self._blob[offset:offset + size])


def j(target):
    return 0x08000000 | ((target >> 2) & 0x03FFFFFF)


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def branch(op, rs, rt, source, target):
    displacement = ((target - (source + 4)) >> 2) & 0xFFFF
    return (op << 26) | (rs << 21) | (rt << 16) | displacement


def jr(register):
    return (register << 21) | 8


class CanonicalCfgTests(unittest.TestCase):
    def test_cfg_requires_a_mapped_image_reader(self):
        with self.assertRaisesRegex(ValueError, "read_at_vaddr"):
            analyze.canonical_cfg_report(None)

    def test_cfg_preserves_calls_tail_likely_delay_and_unresolved_edges(self):
        words = {
            0x1000: jal(0x1100), 0x1004: 0,
            0x1008: 0, 0x100C: j(0x1200), 0x1010: 0,
            0x1014: jr(31), 0x1018: 0,
            0x1100: jr(31), 0x1104: 0,
            0x1200: jr(31), 0x1204: 0,
            0x1250: jr(8), 0x1254: 0,
            0x1300: branch(20, 1, 2, 0x1300, 0x130C),
            0x1304: 0, 0x1308: 0, 0x130C: jr(31), 0x1310: 0,
            0x1400: j(0x1600), 0x1404: 0,
            0x1500: j(0x1600), 0x1504: 0,
            0x1600: jr(31), 0x1604: 0,
            0x1700: 0, 0x1704: jr(31), 0x1708: 0,
            0x1800: 0,
            0x1900: branch(4, 0, 0, 0x1900, 0x1A00), 0x1904: 0,
            0x1A00: jr(31), 0x1A04: 0,
        }
        image = FakeCodeImage(words, struct.pack("<I", 0x1200))
        image.sections = [
            {"nm": ".rodata", "typ": 1, "flags": 0, "addr": 0x2000, "off": 0, "size": 4},
            {"nm": ".init", "typ": 1, "flags": 4, "addr": 0x2100, "off": 0, "size": 4},
        ]
        report = analyze.canonical_cfg_report(
            image,
            ranges=[(0x1000, 0x1A08)],
            entries=[0x1000, 0x1100, 0x1200, 0x1300, 0x1400, 0x1500, 0x1700, 0x1704, 0x1900, 0x1A00],
        )
        self.assertEqual(
            report,
            analyze._canonical_cfg_report_reference(
                image,
                ranges=[(0x1000, 0x1A08)],
                entries=[0x1000, 0x1100, 0x1200, 0x1300, 0x1400, 0x1500, 0x1700, 0x1704, 0x1900, 0x1A00],
            ),
        )
        by_address = {node["address"]: node for node in report["instructions"]}
        self.assertEqual(report["schema_version"], 1)
        self.assertTrue(any(edge["kind"] == "call" and edge["target"] == 0x1100 for edge in report["call_edges"]))
        self.assertTrue(any(edge["kind"] == "tail-call" and edge["target"] == 0x1200 for edge in report["tail_call_edges"]))
        self.assertTrue(any(edge["kind"] == "tail-call" and edge["target"] == 0x1A00 for edge in report["tail_call_edges"]))
        self.assertEqual(by_address[0x1300]["branch_likely"], True)
        self.assertEqual(by_address[0x1304]["delay_slot_of"], 0x1300)
        self.assertEqual(by_address[0x1304]["delay_slot_annulled_when_not_taken"], True)
        self.assertTrue(any(edge["source"] == 0x1250 and edge["target"] is None for edge in report["unresolved_indirect_edges"]))
        self.assertTrue(any(edge["source"] == 0x1700 and edge["target"] == 0x1704 for edge in report["continuation_edges"]))
        self.assertTrue(any(conflict["address"] == 0x1600 for conflict in report["ownership_conflicts"]))
        self.assertIn(0x1800, [item["start"] for item in report["byte_classification"] if item["classification"] == "padding"])
        self.assertEqual(report["jump_table_ownership"][0]["target"], 0x1200)
        self.assertEqual([span["section"] for span in report["data_spans"]], [".rodata"])
        self.assertEqual(analyze.verify_canonical_cfg_report(report), [])
        self.assertEqual(
            analyze.cfg_compatibility_findings(report, report["entries"]), []
        )
        self.assertEqual(analyze.canonical_cfg_json(report), analyze.canonical_cfg_json(json.loads(analyze.canonical_cfg_json(report))))

    def test_cfg_compact_scale_fixture_is_deterministic_and_verified(self):
        image = DenseCfgScaleImage()
        ranges = image.executable_intervals
        first = analyze.canonical_cfg_state(image, ranges=ranges, entries=image.entries)
        first_summary = first.summary()
        self.assertEqual(analyze.verify_canonical_cfg_state(first), [])
        self.assertEqual(first_summary["instruction_count"], 200_000)
        self.assertEqual(first_summary["node_count"], 200_000)
        self.assertEqual(first_summary["owner_count"], 5_000)
        self.assertGreater(first_summary["edge_count"], 200_000)
        self.assertGreater(first_summary["edges_by_class"]["delay-slot"], 5_000)
        self.assertGreaterEqual(first_summary["edges_by_class"]["branch"], 5_000)
        self.assertGreaterEqual(first_summary["edges_by_class"]["call"], 4_000)
        self.assertGreaterEqual(first_summary["edges_by_class"]["direct-jump"], 1)
        self.assertGreaterEqual(first_summary["edges_by_class"]["direct-branch"], 1)
        self.assertGreaterEqual(first_summary["edges_by_class"]["tail-call"], 1)
        self.assertGreater(first_summary["edges_by_class"]["unresolved-indirect"], 1)
        self.assertGreater(first_summary["continuation_count"], 1)
        self.assertGreater(first_summary["conflict_count"], 1)
        self.assertEqual(first_summary["unreadable_span_count"], 1)
        self.assertEqual(first_summary["data_span_count"], 1)
        self.assertGreaterEqual(first_summary["jump_table_count"], 1)
        self.assertGreater(
            sum(first._classification(index) == "unowned-executable" for index in range(first.node_count)),
            1,
        )

        second_summary = analyze.canonical_cfg_summary(
            image, ranges=ranges, entries=image.entries
        )
        self.assertEqual(second_summary, first_summary)

    def test_cfg_verifier_rejects_malformed_and_out_of_sync_reports(self):
        malformed = [
            {"schema_version": 1, "executable_intervals": None},
            {"schema_version": 1, "executable_intervals": [None]},
            {"schema_version": 1, "executable_intervals": [], "instructions": [None]},
            {"schema_version": 1, "executable_intervals": [], "byte_classification": None},
            {"schema_version": 1, "executable_intervals": [], "edges": [None]},
        ]
        for candidate in malformed:
            with self.subTest(candidate=candidate):
                findings = analyze.verify_canonical_cfg_report(candidate)
                self.assertIsInstance(findings, list)
                self.assertTrue(findings)

        image = FakeCodeImage({0x2000: jr(31), 0x2004: 0})
        report = analyze.canonical_cfg_report(image, ranges=[(0x2000, 0x2008)], entries=[0x2000])
        report["byte_classification"][0]["end"] -= 1
        codes = {finding["code"] for finding in analyze.verify_canonical_cfg_report(report)}
        self.assertIn("byte-classification-span-mismatch", codes)

        clean = analyze.canonical_cfg_report(
            image, ranges=[(0x2000, 0x2008)], entries=[0x2000]
        )
        clean["call_edges"].append({
            "source": 0x2000, "target": 0xDEAD, "kind": "call", "detail": "tampered"
        })
        self.assertIn(
            "call_edges-projection-mismatch",
            {finding["code"] for finding in analyze.verify_canonical_cfg_report(clean)},
        )


if __name__ == "__main__":
    unittest.main()
