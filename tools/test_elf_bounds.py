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


if __name__ == "__main__":
    unittest.main()
