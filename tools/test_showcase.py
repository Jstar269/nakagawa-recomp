# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Source-owned showcase art and disc-image regression checks."""

from __future__ import annotations

from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "fixtures" / "showcase"))
sys.path.insert(0, str(ROOT / "tools"))

import showcase
from nk_core.iso_inspect import inspect_iso


def minimal_elf() -> bytes:
    header = bytearray(52)
    header[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHIIIIIHHHHHH", header, 16,
                     2, 8, 1, 0x08804000, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    program = struct.pack("<IIIIIIII", 1, 0x100, 0x08804000, 0x08804000,
                          4, 0x100, 5, 0x1000)
    return bytes(header) + program + bytes(0x100 - 84) + b"\x13\x37\x00\x00"


class ShowcaseImageTests(unittest.TestCase):
    def test_framebuffer_metrics_require_visible_pixels(self) -> None:
        background = bytes((16, 24, 32))
        pixels = background + bytes((255, 0, 0)) + bytes((0, 255, 0)) + bytes((0, 0, 255))
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "frame.ppm"
            path.write_bytes(b"P6\n2 2\n255\n" + pixels)
            self.assertEqual(showcase._ppm_metrics(path, background), (4, 3))

    def test_icon_png_is_deterministic_144_by_80_rgba(self) -> None:
        palette = ((20, 40, 60), (80, 160, 200))
        first = showcase.make_icon_png(palette)
        second = showcase.make_icon_png(palette)
        self.assertEqual(first, second)
        self.assertTrue(first.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(struct.unpack_from(">II", first, 16), (144, 80))
        offset = 8
        raw = None
        while offset < len(first):
            size = struct.unpack_from(">I", first, offset)[0]
            kind = first[offset + 4:offset + 8]
            data = first[offset + 8:offset + 8 + size]
            if kind == b"IDAT":
                raw = zlib.decompress(data)
            offset += size + 12
        self.assertIsNotNone(raw)
        self.assertEqual(len(raw), 80 * (1 + 144 * 4))

    def test_generated_iso_has_psp_tree_and_stable_synthetic_identity(self) -> None:
        elf = minimal_elf()
        icon = showcase.make_icon_png(((10, 30, 50), (70, 150, 210)))
        notice = b"PSPSDK three-clause BSD license test notice\n"
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "showcase.iso"
            showcase.write_iso(path, "TEST00007", "Nakagawa 3D Showcase", icon, elf, notice)
            first = path.read_bytes()
            showcase.write_iso(path, "TEST00007", "Nakagawa 3D Showcase", icon, elf, notice)
            self.assertEqual(path.read_bytes(), first)
            metadata = inspect_iso(path)
        self.assertEqual(metadata.disc_id, "TEST00007")
        self.assertEqual(metadata.title, "Nakagawa 3D Showcase")

    def test_iso_builder_rejects_non_elf_eboot(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(showcase.ShowcaseError, "plaintext EBOOT"):
                showcase.write_iso(Path(temp) / "bad.iso", "TEST00007", "Bad",
                                   b"icon", b"not an ELF", b"notice")

    def test_already_based_elf_is_rejected_instead_of_packaged_as_prx(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "showcase_scene_app.prx"
            path.write_bytes(minimal_elf())
            with self.assertRaisesRegex(showcase.ShowcaseError,
                                        "not a final relocatable PSP PRX"):
                showcase.validate_psp_prx(path, 0x08804000)


if __name__ == "__main__":
    unittest.main()
