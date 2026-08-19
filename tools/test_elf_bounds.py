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


if __name__ == "__main__":
    unittest.main()
