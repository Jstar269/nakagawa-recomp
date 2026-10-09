# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for the trusted code-generation import-map compatibility path.

The security/audit parser in :mod:`psp_import_table` remains strict about the
full named sections.  ``tools/imports.py`` also has to consume the retail
layouts the PSP loader accepts: named NID sections with unreferenced words
before or after the window-paired run, per-library stub sections, windows
detached from the named sections, and section-less inputs whose SceModuleInfo
only the first loadable segment's p_paddr locates. These tests keep each
compatibility bounded, visible in diagnostics, and failing closed for the
malformed variants.
"""

from __future__ import annotations

import contextlib
import io
from pathlib import Path
import sys
import struct
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import analyze
from analyze import Elf
import imports
from import_fixtures import (
    BASE_VADDR,
    DATA_FILE_OFF,
    INTERLEAVED_NIDS,
    INTERLEAVED_SHAPE,
    build_import_layout_elf,
    build_interleaved_import_elf,
    build_stripped_module_elf,
)

PRIMARY = [
    ("SynthAlpha", [0x22000001, 0x22000002, 0x22000003]),
    ("SynthBeta", [0x22000004]),
    ("SynthGamma", [0x22000005, 0x22000006]),
]
DETACHED = [
    ("SynthModuleOne", [0x33000001, 0x33000002]),
    ("SynthModuleTwo", [0x33000003]),
]


def _entry_offset(blob: bytes, index: int) -> int:
    """File offset of PspLibStubEntry ``index`` in a base-0 fixture."""
    elf = Elf(blob, base=0)
    header = elf.read_at_vaddr(elf.sec(".rodata.sceModuleInfo")["addr"], 52)
    libstub = struct.unpack_from("<I", header, 44)[0]
    return DATA_FILE_OFF + (libstub - BASE_VADDR) + 20 * index


class CodegenImportCompatibilityTests(unittest.TestCase):
    def _parse(self, blob: bytes):
        with tempfile.TemporaryDirectory(prefix="imports-codegen-") as tmp:
            path = Path(tmp) / "fixture.elf"
            path.write_bytes(blob)
            return imports._import_model(Elf(str(path), base=0))

    def test_section_tail_uses_consistent_window_prefix(self) -> None:
        windows = [("SynthAlpha", 0, 2), ("SynthBeta", 2, 1)]
        nids = [0x11000001, 0x11000002, 0x11000003]
        stubs, findings = self._parse(
            build_interleaved_import_elf(
                windows, nids, corrupt="nid_region_mismatch"
            )
        )
        self.assertEqual(len(stubs), len(nids))
        self.assertEqual([nid for _lib, nid in stubs.values()], nids)
        self.assertTrue(any("unreferenced tail" in finding for finding in findings))

    def test_exactly_paired_sections_have_no_tail_diagnostic(self) -> None:
        _stubs, findings = self._parse(
            build_interleaved_import_elf(INTERLEAVED_SHAPE, INTERLEAVED_NIDS)
        )
        self.assertFalse(any("unreferenced tail" in finding for finding in findings))

    def test_zero_function_window_does_not_read_unused_library_name(self) -> None:
        nid = 0x11000001
        blob = bytearray(build_interleaved_import_elf(
            [("UnusedLibrary", 0, 0), ("SynthAlpha", 0, 1)], [nid]
        ))
        elf = Elf(bytes(blob), base=0)
        module_info = elf.sec(".rodata.sceModuleInfo")
        header = elf.read_at_vaddr(module_info["addr"], 52)
        libstub = struct.unpack("<I", header[44:48])[0]
        import_entry_offset = DATA_FILE_OFF + (libstub - BASE_VADDR)
        # The fixture helper treats a zero function count as a variable-only
        # window. Make this an empty window so the test isolates the guarantee
        # that unused library names are not dereferenced.
        struct.pack_into("<B", blob, import_entry_offset + 9, 0)
        name_pointer_offset = import_entry_offset
        struct.pack_into("<I", blob, name_pointer_offset, 0x00100000)

        stubs, _findings = self._parse(bytes(blob))

        self.assertEqual(list(stubs.values()), [("SynthAlpha", nid)])


class RetailImportLayoutTests(unittest.TestCase):
    """Real PSP import-table layouts the analyzer models (synthetic bytes)."""

    def _model(self, blob: bytes):
        return imports._import_model(Elf(blob, base=0))

    def _boundary(self, blob: bytes) -> imports.ImportTableError:
        with self.assertRaises(imports.ImportTableError) as raised:
            self._model(blob)
        return raised.exception

    def test_nid_section_head_with_per_library_stub_sections(self) -> None:
        # A linker that emits one .sceStub.text.<library> section per library
        # (and no .sceStub.text) and reserves zero words at the start of
        # .rodata.sceNid, with the libraries laid out in reverse table order.
        blob, expected = build_import_layout_elf(
            PRIMARY, nid_head_words=6, per_library_stub_sections=True,
            reverse_primary_layout=True,
        )
        stubs, findings = self._model(blob)
        self.assertEqual(stubs, expected)
        self.assertEqual(findings, [
            ".rodata.sceNid has an unreferenced head of 6 NID words; "
            "using the window-paired run (6 slots)"
        ])

    def test_nid_section_head_and_tail_use_the_window_run(self) -> None:
        blob, expected = build_import_layout_elf(PRIMARY, nid_head_words=2, nid_tail_words=3)
        stubs, findings = self._model(blob)
        self.assertEqual(stubs, expected)
        self.assertIn("unreferenced head of 2 NID words and a tail of 3", findings[0])

    def test_nid_section_tail_names_the_nid_section(self) -> None:
        blob, expected = build_import_layout_elf(PRIMARY, nid_tail_words=1)
        stubs, findings = self._model(blob)
        self.assertEqual(stubs, expected)
        self.assertEqual(findings, [
            ".rodata.sceNid has an unreferenced tail of 1 NID word; "
            "using the window-paired run (6 slots)"
        ])

    def test_unreferenced_stub_slots_without_nid_pairing_fail_closed(self) -> None:
        # Stub slots are code: a slot no window claims has no NID when the
        # sections do not pair 1:1, on either side of the window run.
        for head, tail, words in ((0, 1, "0 stub slots before and 1 stub slot after"),
                                  (2, 0, "2 stub slots before and 0 stub slots after")):
            with self.subTest(head=head, tail=tail):
                blob, _expected = build_import_layout_elf(
                    PRIMARY, stub_head_slots=head, stub_tail_slots=tail)
                exc = self._boundary(blob)
                self.assertEqual(exc.code, "ANALYZER_IMPORT_REGIONS_MISMATCH")
                self.assertIn(f".sceStub.text has {words} the import windows", str(exc))

    def test_unclaimed_stub_slots_with_paired_nid_words_use_the_section_pairing(self) -> None:
        # One extra stub slot and one extra NID word keep the sections 1:1:
        # the psp-fixup-imports global pairing names the unclaimed slot.
        blob, expected = build_import_layout_elf(PRIMARY, stub_tail_slots=1, nid_tail_words=1)
        stubs, findings = self._model(blob)
        extra = max(expected) + 8
        self.assertEqual(stubs, {**expected, extra: (imports.UNATTRIBUTED_LIBRARY, 0)})
        self.assertEqual(findings, ["stub slots not covered by any library window: 1 positions [6]"])

    def test_detached_windows_pair_by_their_own_runs(self) -> None:
        # Imports from a game-supplied module: stubs inside .text, NIDs after
        # the library name, both outside the named import sections.
        blob, expected = build_import_layout_elf(PRIMARY, detached=DETACHED)
        stubs, findings = self._model(blob)
        self.assertEqual(stubs, expected)
        self.assertEqual(findings, [
            "import windows outside the named import sections: 2 windows, "
            "3 slots paired by their own runs"
        ])

    def test_detached_runs_do_not_fill_the_gap_between_them(self) -> None:
        # Same pairing offset, one unused slot apart: no window claims the gap,
        # and there is no fixup region outside the named sections to recover it
        # from, so it must not become an import stub.
        blob, expected = build_import_layout_elf(
            PRIMARY, detached=DETACHED, detached_gap_slots=1)
        stubs, _findings = self._model(blob)
        self.assertEqual(stubs, expected)
        first_run = [a for a, (lib, _nid) in expected.items() if lib == "SynthModuleOne"]
        self.assertNotIn(max(first_run) + 8, stubs)

    def test_overlapping_detached_runs_fail_closed(self) -> None:
        blob, _expected = build_import_layout_elf(PRIMARY, detached=DETACHED)
        image = bytearray(blob)
        first = _entry_offset(blob, len(PRIMARY))
        second = _entry_offset(blob, len(PRIMARY) + 1)
        first_sym = struct.unpack_from("<I", image, first + 16)[0]
        struct.pack_into("<I", image, second + 16, first_sym + 8)
        exc = self._boundary(bytes(image))
        self.assertEqual(exc.code, "ANALYZER_IMPORT_REGIONS_MISMATCH")
        self.assertIn("overlap", str(exc))

    def test_window_half_inside_the_named_sections_fails_closed(self) -> None:
        # NID run inside .rodata.sceNid but stub run moved into .text: the
        # window belongs to the named region and is inconsistent with it.
        blob, _expected = build_import_layout_elf(PRIMARY, detached=DETACHED)
        image = bytearray(blob)
        detached_sym = struct.unpack_from(
            "<I", image, _entry_offset(blob, len(PRIMARY)) + 16)[0]
        struct.pack_into("<I", image, _entry_offset(blob, 1) + 16, detached_sym)
        exc = self._boundary(bytes(image))
        self.assertEqual(exc.code, "ANALYZER_IMPORT_TABLE_INVALID")
        self.assertIn("is not an 8-byte slot of the stub region", str(exc))

    def test_no_window_in_the_named_sections_fails_closed(self) -> None:
        blob, _expected = build_import_layout_elf([], detached=DETACHED)
        exc = self._boundary(blob)
        self.assertEqual(exc.code, "ANALYZER_IMPORT_TABLE_INVALID")
        self.assertIn("no import window lies in the named import sections", str(exc))

    def test_sectionless_windows_with_different_offsets_fail_closed(self) -> None:
        # Without section bounds a detached run cannot be told from a corrupted
        # window, so mixed pairing offsets stay a named boundary.
        blob, _expected, _modinfo = build_stripped_module_elf(PRIMARY, gp=0x00008000)
        image = bytearray(blob)
        first_nid = struct.unpack_from("<I", image, _entry_offset(blob, 0) + 12)[0]
        struct.pack_into("<I", image, _entry_offset(blob, 2) + 12, first_nid)
        exc = self._boundary(bytes(image))
        self.assertEqual(exc.code, "ANALYZER_IMPORT_REGIONS_MISMATCH")


class StrippedModuleInfoTests(unittest.TestCase):
    """SceModuleInfo of section-less inputs comes from the first loadable p_paddr."""

    def test_module_info_with_zero_gp_is_located_through_p_paddr(self) -> None:
        blob, expected, modinfo = build_stripped_module_elf(PRIMARY)
        elf = Elf(blob, base=0)
        self.assertEqual(elf.sec(".rodata.sceModuleInfo")["addr"], modinfo)
        self.assertEqual(imports.parse_imports(elf), expected)
        # Code (with the import stubs) ends where the module metadata starts.
        text = elf.sec(".text")
        self.assertTrue(all(text["addr"] <= a < text["addr"] + text["size"] for a in expected))
        self.assertLessEqual(text["addr"] + text["size"], modinfo)

    def test_p_paddr_wins_over_a_later_false_module_info(self) -> None:
        # The heuristic scan skips the real record (gp == 0) and would accept
        # the later decoy, whose import window has no NID table.
        blob, expected, modinfo = build_stripped_module_elf(PRIMARY, decoy=True)
        elf = Elf(blob, base=0)
        self.assertEqual(elf.sec(".rodata.sceModuleInfo")["addr"], modinfo)
        with contextlib.redirect_stderr(io.StringIO()):
            starts, _ranges = analyze.analyze(elf)
        self.assertTrue(set(expected) <= starts)
        self.assertEqual(imports.parse_imports(elf), expected)

    def test_kernel_mode_bit_of_p_paddr_is_masked(self) -> None:
        blob, expected, modinfo = build_stripped_module_elf(PRIMARY, kernel_bit=True)
        elf = Elf(blob, base=0)
        self.assertEqual(elf.sec(".rodata.sceModuleInfo")["addr"], modinfo)
        self.assertEqual(imports.parse_imports(elf), expected)

    def test_p_paddr_repeating_p_vaddr_falls_back_to_the_scan(self) -> None:
        # A toolchain ELF whose p_paddr is just its load address names no file
        # offset; the scan still finds a record with a non-zero gp.
        blob, expected, modinfo = build_stripped_module_elf(
            PRIMARY, gp=0x00008000, paddr=BASE_VADDR)
        elf = Elf(blob, base=0)
        self.assertEqual(elf.sec(".rodata.sceModuleInfo")["addr"], modinfo)
        self.assertEqual(imports.parse_imports(elf), expected)

    def test_p_paddr_naming_code_is_not_a_module_info(self) -> None:
        # p_paddr points at instruction words, which do not decode as a
        # record; the scan then finds the real one (non-zero gp here).
        blob, expected, modinfo = build_stripped_module_elf(
            PRIMARY, gp=0x00008000, paddr=DATA_FILE_OFF)
        elf = Elf(blob, base=0)
        self.assertEqual(elf.sec(".rodata.sceModuleInfo")["addr"], modinfo)
        self.assertEqual(imports.parse_imports(elf), expected)


if __name__ == "__main__":
    unittest.main()
