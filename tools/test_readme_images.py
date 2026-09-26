# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Deterministic checks on the README's published showcase screenshots.

The README embeds committed framebuffer snapshots. A missing file, a wrong
pixel format, an oversized capture or a path the publication policy does not
admit must fail here rather than ship a broken or unpublishable image.
"""

from __future__ import annotations

import json
import pathlib
import re
import struct
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
README = ROOT / "README.md"
IMAGES = ROOT / "docs" / "images"
PROFILE = ROOT / "assets" / "public_source_profile.json"

# Native PSP framebuffer resolution of the showcase demos (sceGuDispBuffer).
EXPECTED_WIDTH = 480
EXPECTED_HEIGHT = 272
# A published screenshot is documentation, not a gallery: keep it small enough
# that a repository clone stays cheap.
MAX_PNG_BYTES = 100 * 1024

MARKDOWN_IMAGE_RE = re.compile(r"!\[[^\]]*\]\(([^)\s]+)\)")


def read_png_size(data: bytes) -> tuple[int, int, int, int]:
    """Return (width, height, bit_depth, colour_type) from a PNG IHDR."""
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("not a PNG file")
    width, height = struct.unpack(">2I", data[16:24])
    return width, height, data[24], data[25]


class ReadmeImageTests(unittest.TestCase):
    def test_every_referenced_image_exists(self) -> None:
        references = MARKDOWN_IMAGE_RE.findall(README.read_text(encoding="utf-8"))
        self.assertTrue(references, "README embeds no images")
        for reference in references:
            with self.subTest(image=reference):
                target = (README.parent / reference).resolve()
                self.assertTrue(
                    target.is_file() and ROOT.resolve() in target.parents,
                    f"README references a missing or escaping image: {reference}",
                )

    def test_published_images_are_native_sized_and_small(self) -> None:
        images = sorted(IMAGES.glob("*.png"))
        self.assertTrue(images, "docs/images holds no published screenshots")
        for image in images:
            with self.subTest(image=image.name):
                data = image.read_bytes()
                width, height, bit_depth, colour_type = read_png_size(data)
                self.assertEqual(
                    (width, height), (EXPECTED_WIDTH, EXPECTED_HEIGHT),
                    f"{image.name} is not the native {EXPECTED_WIDTH}x{EXPECTED_HEIGHT} framebuffer",
                )
                self.assertEqual((bit_depth, colour_type), (8, 2),
                                 f"{image.name} is not 8-bit truecolour RGB")
                self.assertLess(len(data), MAX_PNG_BYTES,
                                f"{image.name} is larger than {MAX_PNG_BYTES} bytes")

    def test_publication_policy_admits_exactly_the_published_images(self) -> None:
        include_paths = json.loads(PROFILE.read_text(encoding="utf-8"))["include_paths"]
        published = {path.relative_to(ROOT).as_posix() for path in IMAGES.glob("*.png")}
        self.assertEqual(
            sorted(path for path in include_paths if path.startswith("docs/images/")),
            sorted(published),
            "docs/images and the public source profile disagree about the published screenshots",
        )


if __name__ == "__main__":
    unittest.main()
