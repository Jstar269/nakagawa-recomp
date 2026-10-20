# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Deterministic checks on the README's published showcase screenshots.

The README embeds committed framebuffer snapshots. A missing file, a wrong
pixel format, an oversized capture or a path the publication policy does not
admit must fail here rather than ship a broken or unpublishable image.

Status badges are the one kind of remote image allowed: HTTPS only, and only
from the badge services listed in ``BADGE_URL_RES``, so the README cannot
hot-link arbitrary third-party images or tracking pixels.
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
HTML_IMAGE_RE = re.compile(r"<img[^>]*src=\"([^\"]+)\"", re.IGNORECASE)
# Remote images the README may show: status badges from these services only.
BADGE_URL_RES = (
    re.compile(r"https://github\.com/Jstar269/nakagawa-recomp/actions/workflows/[\w.-]+\.ya?ml/badge\.svg(\?[\w=&%.-]*)?"),
    re.compile(r"https://img\.shields\.io/[\w./%+-]+(\?[\w=&%.:/-]*)?"),
    re.compile(r"https://api\.scorecard\.dev/projects/github\.com/Jstar269/nakagawa-recomp/badge"),
    re.compile(r"https://www\.bestpractices\.dev/projects/\d+/badge"),
    re.compile(r"https://api\.reuse\.software/badge/github\.com/Jstar269/[\w.-]+"),
)


def read_png_size(data: bytes) -> tuple[int, int, int, int]:
    """Return (width, height, bit_depth, colour_type) from a PNG IHDR."""
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("not a PNG file")
    width, height = struct.unpack(">2I", data[16:24])
    return width, height, data[24], data[25]


class ReadmeImageTests(unittest.TestCase):
    def test_every_referenced_image_exists(self) -> None:
        text = README.read_text(encoding="utf-8")
        references = MARKDOWN_IMAGE_RE.findall(text) + HTML_IMAGE_RE.findall(text)
        self.assertTrue(references, "README embeds no images")
        for reference in references:
            with self.subTest(image=reference):
                if "://" in reference or reference.startswith("//"):
                    self.assertTrue(
                        any(pattern.fullmatch(reference) for pattern in BADGE_URL_RES),
                        f"README shows a remote image that is not an allowed status badge: {reference}",
                    )
                    continue
                target = (README.parent / reference).resolve()
                self.assertTrue(
                    target.is_file() and ROOT.resolve() in target.parents,
                    f"README references a missing or escaping image: {reference}",
                )

    def test_remote_images_are_limited_to_status_badges(self) -> None:
        allowed = "https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg"
        refused = (
            "http://img.shields.io/badge/x-y-blue.svg",
            "https://example.com/pixel.png",
            "https://github.com/someone-else/repo/actions/workflows/ci.yml/badge.svg",
        )
        self.assertTrue(any(p.fullmatch(allowed) for p in BADGE_URL_RES))
        for url in refused:
            with self.subTest(url=url):
                self.assertFalse(any(p.fullmatch(url) for p in BADGE_URL_RES))

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
