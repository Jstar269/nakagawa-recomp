# SPDX-License-Identifier: GPL-2.0-or-later
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
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from analyze import Elf
import imports
from import_fixtures import INTERLEAVED_NIDS, INTERLEAVED_SHAPE, build_interleaved_import_elf


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


if __name__ == "__main__":
    unittest.main()
