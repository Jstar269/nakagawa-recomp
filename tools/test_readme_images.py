# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Deterministic checks on the README's published screenshots and brand marks.

The README embeds committed framebuffer snapshots. A missing file, a wrong
pixel format, an oversized capture or a path the publication policy does not
admit must fail here rather than ship a broken or unpublishable image.

Status badges are the one kind of remote image allowed: HTTPS only, and only
from the badge services listed in ``BADGE_URL_RES``, so the README cannot
hot-link arbitrary third-party images or tracking pixels.
"""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import re
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import make_showcase_animation as animation  # noqa: E402
README = ROOT / "README.md"
IMAGES = ROOT / "docs" / "images"
PROFILE = ROOT / "assets" / "public_source_profile.json"

# Native PSP framebuffer resolution of the showcase demos (sceGuDispBuffer).
EXPECTED_WIDTH = 480
EXPECTED_HEIGHT = 272
# A published screenshot is documentation, not a gallery: keep it small enough
# that a repository clone stays cheap.
MAX_PNG_BYTES = 100 * 1024
MAX_GIF_BYTES = 1_000_000

MARKDOWN_IMAGE_RE = re.compile(r"!\[[^\]]*\]\(([^)\s]+)\)")
HTML_IMAGE_RE = re.compile(r"<img[^>]*src=\"([^\"]+)\"", re.IGNORECASE)
# Remote images the README may show: status badges from these services only.
HTML_SOURCE_RE = re.compile(r'<source[^>]*srcset="([^"]+)"', re.IGNORECASE)
BRAND_MARKS = {
    "docs/images/brand/nakagawa-mark-dark.svg",
    "docs/images/brand/nakagawa-mark-light.svg",
}
# The website's link-preview card: brand art, sized for Open Graph, not a framebuffer.
SHARE_CARD = "docs/images/brand/og-card.png"
SHARE_CARD_SIZE = (1200, 630)
MAX_SHARE_CARD_BYTES = 300 * 1024
BADGE_URL_RES = (
    re.compile(r"https://github\.com/Jstar269/nakagawa-recomp/actions/workflows/[\w.-]+\.ya?ml/badge\.svg(\?[\w=&%.-]*)?"),
    re.compile(r"https://img\.shields\.io/[\w./%+-]+(\?[\w=&%.:/-]*)?"),
    re.compile(r"https://api\.scorecard\.dev/projects/github\.com/Jstar269/nakagawa-recomp/badge"),
    re.compile(r"https://www\.bestpractices\.dev/projects/\d+/(badge|baseline)"),
    re.compile(r"https://api\.reuse\.software/badge/github\.com/Jstar269/[\w.-]+"),
    re.compile(r"https://codecov\.io/gh/Jstar269/nakagawa-recomp/graph/badge\.svg(\?branch=main)?"),
)


def read_png_size(data: bytes) -> tuple[int, int, int, int]:
    """Return (width, height, bit_depth, colour_type) from a PNG IHDR."""
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("not a PNG file")
    width, height = struct.unpack(">2I", data[16:24])
    return width, height, data[24], data[25]


def read_gif_metadata(data: bytes) -> tuple[int, int, int, bool]:
    """Return width, height, image-frame count, and Netscape loop presence."""
    if len(data) < 14 or data[:6] != b"GIF89a":
        raise ValueError("not a GIF89a file")
    width, height = struct.unpack_from("<HH", data, 6)
    packed = data[10]
    offset = 13
    if packed & 0x80:
        offset += 3 * (1 << ((packed & 0x07) + 1))

    frames = 0
    looping = False
    trailer = False

    def skip_subblocks(position: int) -> int:
        while True:
            if position >= len(data):
                raise ValueError("truncated GIF data sub-blocks")
            size = data[position]
            position += 1
            if size == 0:
                return position
            if position + size > len(data):
                raise ValueError("truncated GIF data sub-block")
            position += size

    while offset < len(data):
        marker = data[offset]
        offset += 1
        if marker == 0x3B:
            if offset != len(data):
                raise ValueError("trailing bytes after GIF trailer")
            trailer = True
            break
        if marker == 0x21:
            if offset >= len(data):
                raise ValueError("truncated GIF extension")
            label = data[offset]
            offset += 1
            if label == 0xFF and offset < len(data):
                block_size = data[offset]
                app_start = offset + 1
                app_end = app_start + block_size
                if app_end > len(data):
                    raise ValueError("truncated GIF application extension")
                looping = looping or data[app_start:app_end] == b"NETSCAPE2.0"
            offset = skip_subblocks(offset)
            continue
        if marker != 0x2C or offset + 9 > len(data):
            raise ValueError("invalid or truncated GIF block")
        image_packed = data[offset + 8]
        offset += 9
        if image_packed & 0x80:
            offset += 3 * (1 << ((image_packed & 0x07) + 1))
        if offset >= len(data) or not 2 <= data[offset] <= 8:
            raise ValueError("invalid GIF LZW minimum code size")
        offset += 1
        offset = skip_subblocks(offset)
        frames += 1

    if not trailer:
        raise ValueError("GIF trailer is missing")
    return width, height, frames, looping


class ReadmeImageTests(unittest.TestCase):
    def test_every_referenced_image_exists(self) -> None:
        text = README.read_text(encoding="utf-8")
        references = (
            MARKDOWN_IMAGE_RE.findall(text)
            + HTML_IMAGE_RE.findall(text)
            + HTML_SOURCE_RE.findall(text)
        )
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
            "https://codecov.io/gh/someone-else/repo/graph/badge.svg",
            "https://codecov.io/gh/Jstar269/nakagawa-recomp/graph/badge.svg?token=deadbeef",
        )
        self.assertTrue(any(p.fullmatch(allowed) for p in BADGE_URL_RES))
        for url in refused:
            with self.subTest(url=url):
                self.assertFalse(any(p.fullmatch(url) for p in BADGE_URL_RES))

    def test_published_images_are_native_sized_and_small(self) -> None:
        # The README shows the showcase demos as GIFs (checked below); its last two
        # PNG stills were removed in #1531. A PNG screenshot added here later is
        # still held to the native framebuffer size and the byte budget.
        images = sorted(IMAGES.glob("*.png"))
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

    def test_published_animations_are_native_sized_small_and_looping(self) -> None:
        images = sorted(IMAGES.glob("*.gif"))
        self.assertEqual(
            {image.name for image in images},
            {"showcase-ge-scene.gif", "showcase-breakout.gif"},
            "the README must publish both animated showcase demos",
        )
        for image in images:
            with self.subTest(image=image.name):
                data = image.read_bytes()
                width, height, frames, looping = read_gif_metadata(data)
                self.assertEqual(
                    (width, height), (EXPECTED_WIDTH, EXPECTED_HEIGHT),
                    f"{image.name} is not the native {EXPECTED_WIDTH}x{EXPECTED_HEIGHT} framebuffer",
                )
                self.assertGreaterEqual(frames, 2, f"{image.name} is not animated")
                self.assertTrue(looping, f"{image.name} does not loop")
                self.assertLess(len(data), MAX_GIF_BYTES,
                                f"{image.name} is larger than {MAX_GIF_BYTES} bytes")

    def test_share_card_is_open_graph_sized_and_small(self) -> None:
        data = (ROOT / SHARE_CARD).read_bytes()
        width, height, bit_depth, _ = read_png_size(data)
        self.assertEqual((width, height), SHARE_CARD_SIZE)
        self.assertEqual(bit_depth, 8)
        self.assertLess(len(data), MAX_SHARE_CARD_BYTES)

    def test_publication_policy_admits_exactly_the_published_images(self) -> None:
        include_paths = json.loads(PROFILE.read_text(encoding="utf-8"))["include_paths"]
        published = {
            path.relative_to(ROOT).as_posix()
            for pattern in ("*.png", "*.gif")
            for path in IMAGES.glob(pattern)
        }
        published.update(BRAND_MARKS)
        published.add(SHARE_CARD)
        self.assertEqual(
            sorted(path for path in include_paths if path.startswith("docs/images/")),
            sorted(published),
            "docs/images and the public source profile disagree about the published images",
        )


class ShowcaseAnimationToolTests(unittest.TestCase):
    """tools/make_showcase_animation.py on small synthetic frames (no game, no player run)."""

    W = animation.WIDTH
    H = animation.HEIGHT

    def ppm(self, pixels: bytes | None = None, header: bytes | None = None) -> bytes:
        if pixels is None:
            pixels = bytes(self.W * self.H * 3)
        if header is None:
            header = b"P6\n%d %d\n255\n" % (self.W, self.H)
        return header + pixels

    def write(self, directory: pathlib.Path, name: str, data: bytes) -> pathlib.Path:
        path = directory / name
        path.write_bytes(data)
        return path

    def tempdir(self) -> pathlib.Path:
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        return pathlib.Path(holder.name)

    @staticmethod
    def noise(length: int, seed: int) -> bytes:
        """Deterministic pseudo-random bytes (a 32-bit LCG), so the test needs no random module."""
        state = seed & 0xFFFFFFFF
        out = bytearray()
        for _ in range(length):
            state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
            out.append(state >> 24)
        return bytes(out)

    @staticmethod
    def lzw_decode(stream: bytes, min_code_size: int = 8) -> tuple[bytes, int]:
        """A textbook GIF LZW decoder. Returns the index stream and the number of clear codes seen."""
        clear = 1 << min_code_size
        end = clear + 1
        width = min_code_size + 1
        table: list[bytes] = []
        out = bytearray()
        clears = 0
        bits = 0
        count = 0
        previous: bytes | None = None
        position = 0
        saw_end = False
        while not saw_end:
            while count < width:
                if position >= len(stream):
                    raise AssertionError("LZW stream ended before the end code")
                bits |= stream[position] << count
                position += 1
                count += 8
            code = bits & ((1 << width) - 1)
            bits >>= width
            count -= width
            if code == clear:
                clears += 1
                table = [bytes([i]) for i in range(clear)] + [b"", b""]
                width = min_code_size + 1
                previous = None
                continue
            if code == end:
                saw_end = True
                continue
            if previous is None:
                entry = table[code]
            elif code < len(table):
                entry = table[code]
                table.append(previous + entry[:1])
            elif code == len(table):
                entry = previous + previous[:1]
                table.append(entry)
            else:
                raise AssertionError(f"LZW code {code} is past the table ({len(table)} entries)")
            out.extend(entry)
            previous = entry
            if len(table) == (1 << width) and width < 12:
                width += 1
        return bytes(out), clears

    @staticmethod
    def gif_image_streams(gif: bytes) -> list[bytes]:
        """The concatenated LZW data of every image block in a GIF written by encode_gif."""
        offset = 13 + 3 * (1 << ((gif[10] & 7) + 1))
        streams = []
        while gif[offset] != 0x3B:
            if gif[offset] == 0x21:
                offset += 2
                while gif[offset]:
                    offset += gif[offset] + 1
                offset += 1
                continue
            assert gif[offset] == 0x2C
            offset += 10          # separator, rectangle, packed flags
            assert gif[offset] == animation.MIN_CODE_SIZE
            offset += 1
            data = bytearray()
            while gif[offset]:
                data.extend(gif[offset + 1:offset + 1 + gif[offset]])
                offset += gif[offset] + 1
            offset += 1
            streams.append(bytes(data))
        return streams

    # read_ppm

    def test_read_ppm_accepts_comments_and_crlf(self) -> None:
        pixels = self.noise(self.W * self.H * 3, 1)
        directory = self.tempdir()
        headers = {
            "plain": b"P6\n%d %d\n255\n" % (self.W, self.H),
            "comment lines": b"P6\n# captured by the player\n%d %d\n# maxval\n255\n" % (self.W, self.H),
            "crlf": b"P6\r\n%d %d\r\n255\r\n" % (self.W, self.H),
            "crlf and comment": b"P6\r\n# note\r\n%d %d\r\n255\r\n" % (self.W, self.H),
        }
        for label, header in headers.items():
            with self.subTest(header=label):
                path = self.write(directory, "ok.ppm", self.ppm(pixels, header))
                self.assertEqual(animation.read_ppm(path), pixels)

    def test_read_ppm_keeps_pixel_bytes_that_look_like_whitespace_or_comments(self) -> None:
        # The byte after maxval's single delimiter is pixel data, even when it is 0x0A or '#'.
        total = self.W * self.H * 3
        pixels = (b"\x0a#\x0d\x0a" * total)[:total]
        path = self.write(self.tempdir(), "tricky.ppm", self.ppm(pixels))
        self.assertEqual(animation.read_ppm(path), pixels)

    def test_read_ppm_refuses_anything_but_a_full_native_p6(self) -> None:
        directory = self.tempdir()
        full = bytes(self.W * self.H * 3)
        cases = {
            "P3 magic": self.ppm(full, b"P3\n%d %d\n255\n" % (self.W, self.H)),
            "other size": self.ppm(full, b"P6\n%d %d\n255\n" % (self.W + 1, self.H)),
            "maxval 65535": self.ppm(full, b"P6\n%d %d\n65535\n" % (self.W, self.H)),
            "maxval 15": self.ppm(full, b"P6\n%d %d\n15\n" % (self.W, self.H)),
            "short pixels": self.ppm(full[:-1]),
            "long pixels": self.ppm(full + b"\x00"),
            "no pixels": self.ppm(b""),
            "non-numeric size": self.ppm(full, b"P6\nwide %d\n255\n" % self.H),
            "truncated header": b"P6\n%d" % self.W,
            "empty file": b"",
        }
        for label, data in cases.items():
            with self.subTest(case=label):
                path = self.write(directory, "bad.ppm", data)
                with self.assertRaises(ValueError):
                    animation.read_ppm(path)

    # _frames

    def test_frames_sort_numerically_and_ignore_other_names(self) -> None:
        directory = self.tempdir()
        for name in ("frame_v10.ppm", "frame_v2.ppm", "frame_v100.ppm", "frame_v3.txt",
                     "xframe_v4.ppm", "frame_v5.ppm.bak", "frame_v.ppm"):
            self.write(directory, name, b"")
        (directory / "frame_v7.ppm").mkdir()      # a directory with a matching name is not a frame
        found = animation._frames(directory)
        self.assertEqual([vcount for vcount, _path in found], [2, 10, 100])
        self.assertEqual([path.name for _vcount, path in found],
                         ["frame_v2.ppm", "frame_v10.ppm", "frame_v100.ppm"])

    def test_frames_need_a_directory_with_two_snapshots(self) -> None:
        directory = self.tempdir()
        with self.assertRaises(ValueError):
            animation._frames(directory / "missing")
        with self.assertRaises(ValueError):
            animation._frames(directory)
        self.write(directory, "frame_v1.ppm", b"")
        with self.assertRaises(ValueError):
            animation._frames(directory)
        self.write(directory, "frame_v2.ppm", b"")
        self.assertEqual(len(animation._frames(directory)), 2)

    # _lzw_encode

    def test_lzw_round_trip_of_a_flat_frame(self) -> None:
        indices = bytes([0x92]) * (self.W * self.H)
        decoded, clears = self.lzw_decode(animation._lzw_encode(indices))
        self.assertEqual(decoded, indices)
        self.assertGreaterEqual(clears, 1)

    def test_lzw_round_trip_of_short_and_empty_streams(self) -> None:
        for indices in (b"", b"\x00", b"\xff", b"\x01\x01", b"\x00\x01\x02\x03" * 3):
            with self.subTest(length=len(indices)):
                decoded, _clears = self.lzw_decode(animation._lzw_encode(indices))
                self.assertEqual(decoded, indices)

    def test_lzw_round_trip_grows_code_width_through_every_boundary(self) -> None:
        # Distinct pairs fill the dictionary, so the code width walks 9 -> 10 -> 11 -> 12.
        indices = bytes(i & 0xFF for i in range(3000)) + bytes((i * 7) & 0xFF for i in range(3000))
        decoded, _clears = self.lzw_decode(animation._lzw_encode(indices))
        self.assertEqual(decoded, indices)

    def test_lzw_round_trip_of_a_noisy_frame_crosses_the_dictionary_reset(self) -> None:
        indices = self.noise(self.W * self.H, 12345)
        decoded, clears = self.lzw_decode(animation._lzw_encode(indices))
        self.assertGreater(clears, 1, "a noisy frame must fill the 4096-entry dictionary and reset it")
        self.assertEqual(decoded, indices)

    # encode_gif

    def two_frames(self) -> list[bytes]:
        first = bytes(self.W * self.H * 3)
        second = self.noise(self.W * self.H * 3, 99)
        return [first, second]

    def test_encode_gif_is_deterministic_and_describes_a_looping_animation(self) -> None:
        frames = self.two_frames()
        first = animation.encode_gif(frames, 7)
        self.assertEqual(first, animation.encode_gif(frames, 7))
        width, height, count, looping = read_gif_metadata(first)
        self.assertEqual((width, height, count, looping), (self.W, self.H, 2, True))
        self.assertLess(len(first), MAX_GIF_BYTES)

    def test_encode_gif_writes_the_requested_delay_and_the_332_indices(self) -> None:
        frames = self.two_frames()
        gif = animation.encode_gif(frames, 1234)
        self.assertEqual(gif.count(b"!\xf9\x04\x04" + struct.pack("<H", 1234) + b"\x00\x00"), 2)
        streams = self.gif_image_streams(gif)
        self.assertEqual(len(streams), 2)
        for rgb, stream in zip(frames, streams):
            decoded, _clears = self.lzw_decode(stream)
            self.assertEqual(decoded, animation._indices_332(rgb))

    def test_encode_gif_quantises_to_the_3_3_2_palette(self) -> None:
        rgb = bytes([255, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0x20, 0x40, 0x40])
        indices = animation._indices_332(rgb + bytes(self.W * self.H * 3 - len(rgb)))
        self.assertEqual(list(indices[:6]), [0xFF, 0x00, 0xE0, 0x1C, 0x03, 0x29])
        palette = animation._palette_332()
        self.assertEqual(len(palette), 768)
        self.assertEqual(palette[0xFF * 3:0xFF * 3 + 3], b"\xff\xff\xff")
        self.assertEqual(palette[0xE0 * 3:0xE0 * 3 + 3], b"\xff\x00\x00")

    def test_encode_gif_refuses_bad_delays_and_empty_input(self) -> None:
        frames = self.two_frames()
        for delay in (0, -1, 65536):
            with self.subTest(delay=delay):
                with self.assertRaises(ValueError):
                    animation.encode_gif(frames, delay)
        for delay in (1, 65535):
            with self.subTest(delay=delay):
                self.assertEqual(read_gif_metadata(animation.encode_gif(frames, delay))[2], 2)
        with self.assertRaises(ValueError):
            animation.encode_gif([], 10)

    def test_encode_gif_refuses_an_animation_over_the_size_limit(self) -> None:
        frames = [self.noise(self.W * self.H * 3, seed) for seed in range(1, 9)]
        with self.assertRaises(ValueError) as caught:
            animation.encode_gif(frames, 10)
        self.assertIn(str(animation.MAX_GIF_BYTES), str(caught.exception))

    # assemble and main

    def make_scene(self, directory: pathlib.Path) -> None:
        directory.mkdir(parents=True, exist_ok=True)
        self.write(directory, "frame_v10.ppm", self.ppm(self.noise(self.W * self.H * 3, 3)))
        self.write(directory, "frame_v2.ppm", self.ppm(bytes(self.W * self.H * 3)))

    def test_assemble_is_byte_identical_across_runs(self) -> None:
        root = self.tempdir()
        self.make_scene(root / "scene")
        count, size = animation.assemble(root / "scene", root / "out" / "a.gif", 10)
        animation.assemble(root / "scene", root / "out" / "b.gif", 10)
        first = (root / "out" / "a.gif").read_bytes()
        self.assertEqual((count, size), (2, len(first)))
        self.assertEqual(first, (root / "out" / "b.gif").read_bytes())
        self.assertEqual(read_gif_metadata(first), (self.W, self.H, 2, True))

    def run_main(self, *args: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", ["make_showcase_animation.py", *args]), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = animation.main()
        return code, out.getvalue(), err.getvalue()

    def test_main_writes_both_animations(self) -> None:
        root = self.tempdir()
        self.make_scene(root / "scene")
        self.make_scene(root / "breakout")
        code, out, err = self.run_main("--scene-dir", str(root / "scene"),
                                       "--breakout-dir", str(root / "breakout"),
                                       "--output-dir", str(root / "out"), "--delay-cs", "5")
        self.assertEqual((code, err), (0, ""))
        self.assertIn("3D showcase: 2 frames", out)
        self.assertIn("Breakout showcase: 2 frames", out)
        for name in ("showcase-ge-scene.gif", "showcase-breakout.gif"):
            self.assertEqual(read_gif_metadata((root / "out" / name).read_bytes())[2], 2)

    def test_main_reports_a_bad_frame_directory_and_exits_1(self) -> None:
        root = self.tempdir()
        self.make_scene(root / "breakout")
        code, out, err = self.run_main("--scene-dir", str(root / "missing"),
                                       "--breakout-dir", str(root / "breakout"),
                                       "--output-dir", str(root / "out"))
        self.assertEqual(code, 1)
        self.assertEqual(out, "")
        self.assertTrue(err.startswith("ERROR:"), err)
        self.assertIn("missing", err)
        self.assertFalse((root / "out").exists(), "nothing is written when the first scene fails")

    def test_main_reports_a_malformed_frame_and_exits_1(self) -> None:
        root = self.tempdir()
        self.make_scene(root / "scene")
        self.write(root / "scene", "frame_v20.ppm", b"P3\n1 1\n255\n0 0 0\n")
        self.make_scene(root / "breakout")
        code, _out, err = self.run_main("--scene-dir", str(root / "scene"),
                                        "--breakout-dir", str(root / "breakout"),
                                        "--output-dir", str(root / "out"))
        self.assertEqual(code, 1)
        self.assertIn("ERROR:", err)
        self.assertIn("frame_v20.ppm", err)


if __name__ == "__main__":
    unittest.main()
