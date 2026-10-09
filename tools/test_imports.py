# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for the trusted code-generation import-map compatibility path.

The security/audit parser in :mod:`psp_import_table` remains strict about the
full named sections.  ``tools/imports.py`` also has to consume legacy retail
ET_EXEC inputs whose named NID section contains unreferenced trailing words;
these tests keep that compatibility bounded to a consistent window-paired
prefix and ensure the condition is visible in diagnostics. Section-less inputs
locate SceModuleInfo through the first loadable segment's p_paddr.
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
    build_interleaved_import_elf,
    build_stripped_module_elf,
)

PRIMARY = [
    ("SynthAlpha", [0x22000001, 0x22000002, 0x22000003]),
    ("SynthBeta", [0x22000004]),
    ("SynthGamma", [0x22000005, 0x22000006]),
]


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
