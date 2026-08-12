# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Synthetic, retail-free malformed-GIM coverage for issue #171."""

from __future__ import annotations

import struct
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from extract_xb import decode_gim_data  # noqa: E402


def block(block_id: int, content: bytes, *, header_size: int = 16, size: int | None = None) -> bytes:
    actual_size = 16 + len(content) if size is None else size
    header = bytearray(16)
    struct.pack_into("<H", header, 0, block_id)
    struct.pack_into("<I", header, 4, actual_size)
    struct.pack_into("<I", header, 12, header_size)
    return bytes(header) + content


def image_block(*, fmt: int = 5, width: int = 2, height: int = 2,
                raw: bytes = b"\x00\x01\x02\x03", data_end: int | None = None) -> bytes:
    content = bytearray(36)
    struct.pack_into("<H", content, 4, fmt)
    struct.pack_into("<H", content, 6, 0)
    struct.pack_into("<H", content, 8, width)
    struct.pack_into("<H", content, 10, height)
    end = 36 + len(raw) if data_end is None else data_end
    struct.pack_into("<I", content, 28, 36)
    struct.pack_into("<I", content, 32, end)
    return block(0x0004, bytes(content) + raw)


def palette_block(*, raw: bytes = b"\x00\x00\xff\xff") -> bytes:
    content = bytearray(36)
    struct.pack_into("<I", content, 28, 36)
    struct.pack_into("<I", content, 32, 36 + len(raw))
    return block(0x0005, bytes(content) + raw)


def gim(*blocks: bytes) -> bytes:
    return b"MIG.00.1PSP" + b"\0" * 5 + b"".join(blocks)


class TestGimBounds(unittest.TestCase):
    def test_valid_t8_fixture_decodes(self) -> None:
        raw = image_block(raw=b"\0\0\0\0")
        decoded = decode_gim_data(gim(raw, palette_block(raw=b"\x00\x00\xff\xff")))
        self.assertIsNotNone(decoded)
        assert decoded is not None
        self.assertEqual(decoded[:2], (2, 2))
        self.assertEqual(len(decoded[2]), 16)

    def test_truncated_or_inconsistent_block_is_rejected(self) -> None:
        malformed = bytearray(gim(image_block()))
        struct.pack_into("<I", malformed, 16 + 4, 0)
        self.assertIsNone(decode_gim_data(bytes(malformed)))

        malformed = bytearray(gim(image_block()))
        struct.pack_into("<I", malformed, 16 + 12, 64)
        self.assertIsNone(decode_gim_data(bytes(malformed)))

        self.assertIsNone(decode_gim_data(gim(block(0x0004, b"\0" * 8))))
        self.assertIsNone(decode_gim_data(gim(block(0x0005, b"\0" * 8))))

    def test_dimensions_are_rejected_before_pixel_allocation(self) -> None:
        self.assertIsNone(decode_gim_data(gim(image_block(width=5000, raw=b"x"))))
        self.assertIsNone(decode_gim_data(gim(image_block(width=0, raw=b"x"))))
        self.assertIsNone(decode_gim_data(gim(image_block(width=2, height=2, raw=b"x"))))

    def test_image_and_palette_offsets_stay_inside_their_blocks(self) -> None:
        self.assertIsNone(decode_gim_data(gim(image_block(data_end=0x1000))))

        reversed_image = bytearray(image_block())
        struct.pack_into("<I", reversed_image, 16 + 28, 40)
        struct.pack_into("<I", reversed_image, 16 + 32, 36)
        self.assertIsNone(decode_gim_data(gim(bytes(reversed_image))))

        bad_palette = bytearray(palette_block())
        struct.pack_into("<I", bad_palette, 16 + 28, 0x1000)
        self.assertIsNone(decode_gim_data(gim(image_block(), bytes(bad_palette))))

        reversed_palette = bytearray(palette_block())
        struct.pack_into("<I", reversed_palette, 16 + 28, 40)
        struct.pack_into("<I", reversed_palette, 16 + 32, 36)
        self.assertIsNone(decode_gim_data(gim(image_block(), bytes(reversed_palette))))

    def test_nested_block_depth_is_bounded(self) -> None:
        nested = image_block()
        for _ in range(40):
            nested = block(0x0002, nested)
        self.assertIsNone(decode_gim_data(gim(nested)))


if __name__ == "__main__":
    unittest.main()
