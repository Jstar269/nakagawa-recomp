# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for the trusted code-generation import-map compatibility path.

The security/audit parser in :mod:`psp_import_table` remains strict about the
full named sections.  ``tools/imports.py`` also has to consume legacy retail
ET_EXEC inputs whose named NID section contains unreferenced trailing words;
these tests keep that compatibility bounded to a consistent window-paired
prefix and ensure the condition is visible in diagnostics.
"""

from __future__ import annotations

from pathlib import Path
import sys
import struct
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from analyze import Elf
import imports
from import_fixtures import (
    BASE_VADDR,
    DATA_FILE_OFF,
    INTERLEAVED_NIDS,
    INTERLEAVED_SHAPE,
    build_interleaved_import_elf,
)


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
        name_pointer_offset = DATA_FILE_OFF + (libstub - BASE_VADDR)
        struct.pack_into("<I", blob, name_pointer_offset, 0x00100000)

        stubs, _findings = self._parse(bytes(blob))

        self.assertEqual(list(stubs.values()), [("SynthAlpha", nid)])


if __name__ == "__main__":
    unittest.main()
