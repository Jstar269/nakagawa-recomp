# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Synthetic tests for tools/pgf_build.py.

All glyph tables here are synthetic (hand-written 8x8 patterns). No retail,
PPSSPP, OFL, or Ume bytes are used or fetched. Network-independent.
"""

import json
import unittest

from pgf_build import (
    build_manifest,
    build_pgf,
    parse_header,
    read_glyph,
    required_notices,
    sha256_hex,
)


def _pattern(seed: int, width: int = 8, height: int = 8) -> list[bytes]:
    rows = []
    for y in range(height):
        row = bytearray((width + 1) // 2)
        for x in range(width):
            if (x * 31 + y * 17 + seed) % 3:
                if x % 2 == 0:
                    row[x // 2] |= 0x0F
                else:
                    row[x // 2] |= 0xF0
        rows.append(bytes(row))
    return rows


def _sample_glyphs() -> dict:
    return {
        0x41: (8, 8, _pattern(1)),  # A
        0x42: (8, 8, _pattern(2)),  # B
        0x3042: (8, 8, _pattern(3)),  # hiragana A (synthetic bitmap)
    }


class PgfBuildTest(unittest.TestCase):
    def test_header_round_trip(self) -> None:
        blob = build_pgf(_sample_glyphs(), font_name="Nakagawa Test Sans")
        info = parse_header(blob)
        self.assertEqual(info["font_name"], "Nakagawa Test Sans")
        self.assertEqual(info["font_type"], "Regular")
        self.assertEqual(info["first_glyph"], 32)
        self.assertEqual(info["last_glyph"], 0x3042)
        self.assertEqual(info["charptr_len"], 3)

    def test_glyph_round_trip(self) -> None:
        glyphs = _sample_glyphs()
        blob = build_pgf(glyphs, font_name="Nakagawa Test Sans")
        for code, (width, height, rows) in glyphs.items():
            got_w, got_h, body = read_glyph(blob, code)
            self.assertEqual((got_w, got_h), (width, height))
            self.assertEqual(body, b"".join(rows))

    def test_unmapped_codepoint_falls_back_to_slot_zero(self) -> None:
        blob = build_pgf(_sample_glyphs(), font_name="Nakagawa Test Sans")
        # U+0043 'C' is inside the charmap but has no glyph: resolves to slot 0.
        got_w, _, _ = read_glyph(blob, 0x43)
        self.assertEqual(got_w, 8)

    def test_deterministic_bytes(self) -> None:
        first = build_pgf(_sample_glyphs(), font_name="Nakagawa Test Sans")
        second = build_pgf(_sample_glyphs(), font_name="Nakagawa Test Sans")
        self.assertEqual(sha256_hex(first), sha256_hex(second))

    def test_camouflage_names_refused(self) -> None:
        for bad in (
            "FTT-NewRodin Pro DB",
            "FTT-NewRodin Pro Latin",
            "AsiaKNHH-SONY-uni",
            "Source Han Sans",
        ):
            with self.assertRaises(ValueError, msg=bad):
                build_pgf(_sample_glyphs(), font_name=bad)

    def test_manifest_pins_inputs_and_notices(self) -> None:
        blob = build_pgf(_sample_glyphs(), font_name="Nakagawa Test Sans")
        manifest = build_manifest(
            {"glyph-table": sha256_hex(b"synthetic")},
            font_name="Nakagawa Test Sans",
            source_family="source-han-sans",
            codepoints=sorted(_sample_glyphs()),
            output_sha256=sha256_hex(blob),
        )
        doc = json.loads(manifest)
        self.assertEqual(doc["output_sha256"], sha256_hex(blob))
        self.assertIn("SIL Open Font License 1.1 text", doc["required_notices"])
        self.assertIn("scaffold", doc["conformance"])
        with self.assertRaises(KeyError):
            build_manifest({}, font_name="x", source_family="sony-cdn",
                           codepoints=[0x41], output_sha256="0" * 64)

    def test_required_notices_families(self) -> None:
        self.assertTrue(required_notices("ume"))
        self.assertTrue(required_notices("source-han-sans"))

    def test_rejects_garbage(self) -> None:
        with self.assertRaises(ValueError):
            parse_header(b"too short")
        with self.assertRaises(ValueError):
            build_pgf({}, font_name="Nakagawa Test Sans")


if __name__ == "__main__":
    unittest.main()
