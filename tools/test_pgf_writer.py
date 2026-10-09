#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Round-trip and refusal coverage for the deterministic PGF writer (issue #313).

Layer 1 (behavioural): write a project-authored synthetic glyph set, then open
the result through the public C reader ``src/rt/pgf_public.c`` with the harness
``tests/native/test_pgf_writer_roundtrip.c`` and compare every guest record and
every drawn sample with the glyph set the writer was given.  The fixtures are a
box, a bar, a diagonal, and a ring -- project-authored shapes, never a font file.

Layer 2 (structural): cross-check the emitted container with the independent
public validator ``tools/nk_core/fonts.py`` (PGF_SPEC.md section 8, vector 5) and
decode the packed character map, pointer table, and glyph records in the test
itself, so the packed widths, the whole-word rounding, and the record bit layout
are pinned without the C reader.

Layer 3 (refusals): malformed glyph sets are refused by name, never clamped.
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import pgf_writer  # noqa: E402
from nk_core.fonts import validate_pgf_data  # noqa: E402
from pgf_writer import Glyph, PgfWriteError, build_pgf, encode_bitmap  # noqa: E402

RT = ROOT / "src" / "rt"
HELPER_C = ROOT / "tests" / "native" / "test_pgf_writer_roundtrip.c"
READER_C = RT / "pgf_public.c"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")

POINTS = pgf_writer.POINTS
FONT_NAME = pgf_writer.DEFAULT_FONT_NAME

#: Project-authored synthetic shapes. ``samples`` is in the order the glyph's own
#: row order selects, so a reader that honours that order reproduces the raster.
BOX_A = Glyph(code=0x41, width=4, height=3, samples=(1,) * 12)
BAR_B = Glyph(code=0x42, width=2, height=5, samples=(0, 0, 15, 15, 7, 7, 7, 7, 0, 0))
DIAGONAL_C = Glyph(code=0x43, width=3, height=2, samples=(2, 3, 4, 5, 6, 7), row_order=2)
RING_D = Glyph(
    code=0x44,
    width=5,
    height=5,
    samples=(
        15, 15, 15, 15, 15,
        15, 0, 0, 0, 15,
        15, 0, 0, 0, 15,
        15, 0, 0, 0, 15,
        15, 15, 15, 15, 15,
    ),
)
GLYPHS = (BOX_A, BAR_B, DIAGONAL_C, RING_D)
ABSENT_CODE = 0x5A


def _raster(glyph: Glyph) -> list[int]:
    """The samples in raster order, undoing a column-major row order 2."""
    if glyph.row_order == 1:
        return list(glyph.samples)
    raster = [0] * (glyph.width * glyph.height)
    for index, sample in enumerate(glyph.samples):
        raster[(index % glyph.height) * glyph.width + index // glyph.height] = sample
    return raster


def _metrics(glyph: Glyph) -> dict[str, int]:
    """The metrics the writer emits for a glyph that derives them from its shape."""
    dimension_width = glyph.width * POINTS
    dimension_height = glyph.height * POINTS
    advance_x = glyph.width * POINTS
    x_left = glyph.adjust_x * POINTS
    return {
        "width": glyph.width,
        "height": glyph.height,
        "adjust_x": glyph.adjust_x,
        "adjust_y": glyph.adjust_y,
        "dimension": (dimension_width, dimension_height),
        "x_adjustment": (x_left, x_left + advance_x // 2),
        "y_adjustment": (0, dimension_height),
        "advance": (advance_x, 0),
        "y_base": 0,
        "descender": -dimension_height,
        "x_left": x_left,
        "x_center": x_left + advance_x // 2,
        "y_top": dimension_height,
        "advance_x": advance_x,
        "advance_y": 0,
        "shadow_row": 0,
        "shadow_id": 0,
    }


def _signed(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def _bits(data: bytes, offset: int, bit_offset: int, width: int) -> int:
    """Read ``width`` bits least-significant bit first, as the format packs them."""
    value = 0
    for step in range(width):
        absolute = bit_offset + step
        value |= ((data[offset + absolute // 8] >> (absolute % 8)) & 1) << step
    return value


class PgfWriterStructureTests(unittest.TestCase):
    """The emitted container is structurally valid and self-consistent."""

    def setUp(self) -> None:
        self.image = build_pgf(GLYPHS)

    def test_public_structural_validator_accepts_the_image(self):
        report = validate_pgf_data(self.image, "synthetic.pgf")
        self.assertEqual(report["size"], len(self.image))

    def test_header_declares_the_documented_container(self):
        self.assertEqual(struct.unpack_from("<H", self.image, 0x00)[0], 0)
        self.assertEqual(struct.unpack_from("<H", self.image, 0x02)[0], pgf_writer.HEADER_SIZE)
        self.assertEqual(self.image[0x04:0x08], pgf_writer.MAGIC)
        self.assertEqual(_signed(self.image, 0x08), pgf_writer.REVISION)
        self.assertEqual(_signed(self.image, 0x0C), pgf_writer.VERSION)
        self.assertEqual(self.image[0x22], pgf_writer.SOURCE_BITS_PER_PIXEL)
        first, last = struct.unpack_from("<HH", self.image, 0xB6)
        self.assertEqual((first, last), (0x41, 0x44))
        self.assertEqual(struct.unpack_from("<I", self.image, 0x10)[0], last - first + 1)
        self.assertEqual(struct.unpack_from("<I", self.image, 0x14)[0], len(GLYPHS))
        self.assertEqual(struct.unpack_from("<I", self.image, 0x16C)[0], 0)
        self.assertEqual(self.image[0x35 : 0x35 + len(FONT_NAME)], FONT_NAME.encode("ascii"))
        self.assertEqual(self.image[0x35 + len(FONT_NAME)], 0)

    def test_metric_tables_are_deduplicated(self):
        metrics = [_metrics(glyph) for glyph in GLYPHS]
        expected = [
            len({item["dimension"] for item in metrics}),
            len({item["x_adjustment"] for item in metrics}),
            len({item["y_adjustment"] for item in metrics}),
            len({item["advance"] for item in metrics}),
        ]
        self.assertEqual(list(self.image[0x102:0x106]), expected)
        # BAR_B and RING_D share a vertical size, so the Y-adjustment table is
        # strictly smaller than the glyph count: dedup is real, not per-glyph.
        self.assertLess(expected[2], len(GLYPHS))

    def test_packed_map_and_pointers_decode_to_the_glyph_set(self):
        image = self.image
        first = struct.unpack_from("<H", image, 0xB6)[0]
        char_map_count = struct.unpack_from("<I", image, 0x10)[0]
        char_map_bits = struct.unpack_from("<I", image, 0x18)[0]
        pointer_bits = struct.unpack_from("<I", image, 0x1C)[0]
        cursor = pgf_writer.HEADER_SIZE + sum(image[0x102 + table] * 8 for table in range(4))
        map_offset = cursor
        map_size = ((char_map_count * char_map_bits + 31) // 32) * 4
        pointer_offset = map_offset + map_size
        pointer_size = ((len(GLYPHS) * pointer_bits + 31) // 32) * 4
        glyph_data = pointer_offset + pointer_size
        self.assertEqual(map_size % 4, 0)
        self.assertLessEqual(glyph_data, len(image))

        sentinel = (1 << char_map_bits) - 1
        resolved = {}
        for index in range(char_map_count):
            value = _bits(image, map_offset, index * char_map_bits, char_map_bits)
            if value != sentinel:
                resolved[first + index] = value
        self.assertEqual(sorted(resolved), [glyph.code for glyph in GLYPHS])
        self.assertEqual([resolved[glyph.code] for glyph in GLYPHS], list(range(len(GLYPHS))))

        offsets = [_bits(image, pointer_offset, index * pointer_bits, pointer_bits) * 4
                   for index in range(len(GLYPHS))]
        self.assertEqual(offsets[0], 0)
        self.assertEqual(offsets, sorted(offsets))
        for glyph, offset in zip(GLYPHS, offsets, strict=True):
            self.assertEqual(offset % 4, 0, "a pointer unit is a quarter of a record offset")
            record = glyph_data + offset
            self.assertEqual(_bits(image, record, 14, 7), glyph.width)
            self.assertEqual(_bits(image, record, 21, 7), glyph.height)
            self.assertEqual(_bits(image, record, 42, 2), glyph.row_order)
            self.assertEqual(_bits(image, record, 55, 9), 0, "this writer emits no shadow")
            bitmap = encode_bitmap(tuple(glyph.samples))
            self.assertEqual(image[record + 12 : record + 12 + len(bitmap)], bitmap)

    def test_identical_input_produces_identical_bytes(self):
        self.assertEqual(build_pgf(GLYPHS), build_pgf(list(GLYPHS)))
        self.assertEqual(build_pgf(list(reversed(GLYPHS))), self.image)

    def test_bitmap_encoder_is_a_pure_function_of_the_samples(self):
        self.assertEqual(encode_bitmap((1,) * 12), encode_bitmap((1,) * 12))
        # Eight equal samples use one repeat control plus one sample nibble.
        self.assertEqual(encode_bitmap((5,) * 8).hex(), "57")
        # Eight distinct samples use one literal control plus eight sample nibbles.
        self.assertEqual(encode_bitmap(tuple(range(8))).hex(), "0821436507")
        # A nine-sample run splits at the eight-sample ceiling of one control.
        self.assertEqual(encode_bitmap((0,) * 9).hex(), "070f")


class PgfWriterRefusalTests(unittest.TestCase):
    """Malformed input is refused by name, never clamped or repaired."""

    def assert_refused(self, reason: str, **kwargs) -> None:
        glyphs = kwargs.pop("glyphs", None)
        if glyphs is None:
            glyphs = [Glyph(code=0x41, width=1, height=1, samples=(3,))]
        with self.assertRaises(PgfWriteError) as caught:
            build_pgf(glyphs, **kwargs)
        self.assertEqual(caught.exception.reason, reason)

    def test_oversized_glyph_is_refused(self):
        self.assert_refused("glyph-too-large", glyphs=[Glyph(code=0x41, width=128, height=1, samples=())])
        self.assert_refused("glyph-too-large", glyphs=[Glyph(code=0x41, width=1, height=200, samples=())])

    def test_duplicate_code_point_is_refused(self):
        self.assert_refused("duplicate-code-point", glyphs=[BOX_A, BOX_A])

    def test_out_of_range_metrics_are_refused(self):
        self.assert_refused("metric-out-of-range", glyphs=[Glyph(0x41, 2, 2, (0,) * 4, advance_x=1 << 31)])
        self.assert_refused("metric-out-of-range", glyphs=[Glyph(0x41, 2, 2, (0,) * 4, advance_y=-(1 << 31) - 1)])
        self.assert_refused("metric-out-of-range", glyphs=[Glyph(0x41, 2, 2, (0,) * 4, adjust_x=64)])
        self.assert_refused("metric-out-of-range", glyphs=[Glyph(0x41, 2, 2, (0,) * 4, adjust_y=-65)])
        self.assert_refused(
            "metric-out-of-range",
            glyphs=[Glyph(0x41, 1, 1, (0,), y_base=1 << 30, dimension_height=1 << 30)],
        )

    def test_sample_and_shape_refusals(self):
        self.assert_refused("sample-out-of-range", glyphs=[Glyph(0x41, 2, 1, (0, 16))])
        self.assert_refused("sample-count-mismatch", glyphs=[Glyph(0x41, 2, 2, (0,) * 3)])
        self.assert_refused("code-point-out-of-range", glyphs=[Glyph(0x10000, 1, 1, (0,))])
        self.assert_refused("unsupported-row-order", glyphs=[Glyph(0x41, 1, 1, (0,), row_order=3)])
        self.assert_refused("empty-glyph-set", glyphs=[])

    def test_reserved_firmware_font_name_is_refused(self):
        self.assert_refused("font-field-invalid", font_name="jpn0")
        self.assert_refused("font-field-invalid", font_name="")
        self.assert_refused("font-field-invalid", font_name="Nakagawa \u00e9")
        self.assert_refused("font-field-invalid", font_type="")

    def test_metric_table_overflow_is_refused(self):
        glyphs = [
            Glyph(code=0x41 + index, width=1, height=1, samples=(0,), advance_x=index)
            for index in range(pgf_writer.MAX_TABLE_ENTRIES + 1)
        ]
        self.assert_refused("metric-table-overflow", glyphs=glyphs)

    def test_metric_table_at_the_limit_is_accepted(self):
        # Exactly MAX_TABLE_ENTRIES distinct advances still fit the one-byte header count.
        self.assertEqual(pgf_writer.MAX_TABLE_ENTRIES, 255)
        glyphs = [
            Glyph(code=0x41 + index, width=1, height=1, samples=(0,), advance_x=index)
            for index in range(pgf_writer.MAX_TABLE_ENTRIES)
        ]
        self.assertTrue(build_pgf(glyphs))

    def test_non_contiguous_code_points_are_refused_by_name(self):
        glyphs = [
            Glyph(code=0x41, width=1, height=1, samples=(0,)),
            Glyph(code=0x50, width=1, height=1, samples=(0,)),
        ]
        self.assert_refused("non-contiguous-code-points", glyphs=glyphs)

    def test_firmware_font_names_are_refused_with_their_extension(self):
        for name in ("jpn0.pgf", "LTN15.PGF", "kr0"):
            with self.subTest(name=name):
                self.assert_refused("font-field-invalid", font_name=name)

    def test_vendor_and_trademark_font_names_are_refused(self):
        # Vendor names are split across literals so this file never holds them whole.
        names = (
            "F" "TT-" "New" "Rodin Pro DB", "Asia" "KNHH-" "SO" "NY-uni", "Asia" "NHH(512Johab)",
            "Font" "works Latin", "Noto Sans JP", "Lato Display", "Atkinson Hyperlegible Next",
            "Nanum Gothic", "Sawarabi Gothic", "Gowun Dodum", "M PLUS 1p", "Ume Hy Gothic",
            "Source Han Sans",
        )
        for name in names:
            with self.subTest(name=name):
                self.assert_refused("font-field-invalid", font_name=name)

    def test_names_that_only_contain_a_short_mark_are_accepted(self):
        # A whole-word mark does not refuse an ordinary word that happens to contain it.
        glyphs = [Glyph(code=0x41, width=1, height=1, samples=(3,))]
        for name in ("Platonic Serif", "Volume Mono"):
            with self.subTest(name=name):
                self.assertTrue(build_pgf(glyphs, font_name=name))

    def test_cli_refusal_names_the_reason_and_writes_nothing(self):
        with tempfile.TemporaryDirectory(prefix="pgf_writer_") as tmp:
            glyph_set = Path(tmp) / "glyphs.json"
            output = Path(tmp) / "out.pgf"
            glyph_set.write_text(
                '{"glyphs": [{"code": 65, "width": 4, "height": 1, "samples": [0]}]}',
                encoding="utf-8",
            )
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools" / "pgf_writer.py"), str(output), str(glyph_set)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn("sample-count-mismatch", result.stderr)
            self.assertFalse(output.exists())


@unittest.skipUnless(CC, "no C compiler on PATH")
class PgfWriterReaderRoundTripTests(unittest.TestCase):
    """The generated font opens through the public reader and matches bit-exactly."""

    @classmethod
    def setUpClass(cls) -> None:
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="pgf_roundtrip_")
        cls.exe = os.path.join(cls.tmp, "pgf_roundtrip.exe")
        result = subprocess.run(
            [CC, "-std=c99", "-Wall", "-Wextra", "-Werror", f"-I{RT}", "-o", cls.exe,
             str(READER_C), str(HELPER_C)],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise AssertionError("the PGF round-trip harness did not build:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _read_back(self, glyphs: tuple[Glyph, ...], codes: list[int]) -> list[str]:
        path = Path(self.tmp) / "generated.pgf"
        path.write_bytes(build_pgf(glyphs))
        result = subprocess.run(
            [self.exe, str(path), *[str(code) for code in codes]],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.splitlines()

    def test_reader_opens_the_generated_font_and_reproduces_every_glyph(self):
        codes = [glyph.code for glyph in GLYPHS] + [ABSENT_CODE]
        lines = self._read_back(GLYPHS, codes)
        self.assertEqual(lines[0], "open ok")
        font = bytes.fromhex(lines[1].removeprefix("font "))
        self.assertEqual(len(font), 0x108)
        self.assertEqual(font[0x7C : 0x7C + len(FONT_NAME)], FONT_NAME.encode("ascii"))
        self.assertEqual(struct.unpack_from("<I", font, 0x54)[0], 0x44 - 0x41 + 1)
        self.assertEqual(struct.unpack_from("<I", font, 0x58)[0], 0)
        self.assertEqual(struct.unpack_from("<H", font, 0x76)[0], 2, "Latin: U+3042 is absent")
        self.assertEqual(font[0x104], pgf_writer.SOURCE_BITS_PER_PIXEL)

        records = lines[2 : 2 + len(codes) * 2]
        present = slice(0, 2 * len(GLYPHS), 2)
        drawn = slice(1, 2 * len(GLYPHS), 2)
        for glyph, line in zip(GLYPHS, records[present], strict=True):
            fields = line.split(" ")
            self.assertEqual(fields[:2], ["char", str(glyph.code)])
            self.assertEqual(fields[2], "1", f"U+{glyph.code:04X} presence")
            info = bytes.fromhex(fields[3])
            self.assertEqual(len(info), 0x3C)
            metrics = _metrics(glyph)
            self.assertEqual(struct.unpack_from("<I", info, 0x00)[0], metrics["width"])
            self.assertEqual(struct.unpack_from("<I", info, 0x04)[0], metrics["height"])
            self.assertEqual(_signed(info, 0x08), metrics["adjust_x"])
            self.assertEqual(_signed(info, 0x0C), metrics["adjust_y"])
            self.assertEqual(_signed(info, 0x10), metrics["dimension"][0])
            self.assertEqual(_signed(info, 0x14), metrics["dimension"][1])
            self.assertEqual(_signed(info, 0x18), metrics["y_base"])
            self.assertEqual(_signed(info, 0x1C), metrics["descender"])
            self.assertEqual(_signed(info, 0x20), metrics["x_left"])
            self.assertEqual(_signed(info, 0x24), metrics["y_base"])
            self.assertEqual(_signed(info, 0x28), metrics["x_center"])
            self.assertEqual(_signed(info, 0x2C), metrics["y_top"])
            self.assertEqual(_signed(info, 0x30), metrics["advance_x"])
            self.assertEqual(_signed(info, 0x34), metrics["advance_y"])
            self.assertEqual(struct.unpack_from("<H", info, 0x38)[0], metrics["shadow_row"])
            self.assertEqual(struct.unpack_from("<H", info, 0x3A)[0], metrics["shadow_id"])
        for glyph, line in zip(GLYPHS, records[drawn], strict=True):
            fields = line.split(" ")
            self.assertEqual(fields[:2], ["draw", str(glyph.code)])
            self.assertEqual(int(fields[2]), glyph.width)
            self.assertEqual(int(fields[3]), glyph.height)
            self.assertEqual(list(bytes.fromhex(fields[4])), _raster(glyph))

        absent = records[-2].split(" ")
        self.assertEqual(absent[:3], ["char", str(ABSENT_CODE), "0"])
        self.assertEqual(bytes.fromhex(absent[3]), bytes(0x3C))
        self.assertEqual(records[-1], f"draw {ABSENT_CODE} none")

    def test_reader_drawn_samples_match_the_writer_input_bit_for_bit(self):
        glyph = Glyph(code=0x4E, width=4, height=4, samples=(15, 15, 0, 0, 15, 7, 7, 0, 0, 15, 0, 0, 1, 1, 1, 1))
        lines = self._read_back((glyph,), [0x4E])
        self.assertEqual(list(bytes.fromhex(lines[3].split(" ")[4])), _raster(glyph))

    def test_a_second_run_reads_back_identically(self):
        codes = [glyph.code for glyph in GLYPHS]
        self.assertEqual(self._read_back(GLYPHS, codes), self._read_back(GLYPHS, codes))


if __name__ == "__main__":
    unittest.main()
