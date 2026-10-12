# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Python/C parity for the PGF validator and the font-slot layout.

The native reader (src/rt/pgf_public.c, pgf_validate_memory) is built with a small harness
(tests/native/pgf_verdict_harness.c). Every image in a corpus, built with tools/pgf_writer.py
and mutated by named byte patches and by a seeded fuzz pass, is checked by both the native
reader and the Python mirror in tools/nk_core/fonts.py. The refusal and every header fact
and coverage flag must agree. The shared layout constants are pinned to their C headers too.
"""

from __future__ import annotations

import random
import re
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

from nk_core import fonts  # noqa: E402
from pgf_writer import Glyph, build_pgf  # noqa: E402

HARNESS_C = ROOT / "tests" / "native" / "pgf_verdict_harness.c"
READER_C = ROOT / "src" / "rt" / "pgf_public.c"
HOST_C = ROOT / "src" / "core" / "nk_pgf_host.c"
PGF_API_H = ROOT / "src" / "rt" / "pgf_api.h"
SLOTS_H = ROOT / "src" / "core" / "nk_font_slots.h"
CC = shutil.which("gcc") or shutil.which("cc")


def _glyph(code: int, width: int = 2, height: int = 2, row_order: int = 1) -> Glyph:
    samples = tuple((index * 3 + code) % 16 for index in range(width * height))
    return Glyph(code=code, width=width, height=height, samples=samples, row_order=row_order)


def _latin_dense() -> bytes:
    return build_pgf([_glyph(code) for code in range(0x41, 0x5B)], font_name="Nakagawa Test Latin")


def _latin_sparse() -> bytes:
    return build_pgf([_glyph(0x41), _glyph(0x5A)], font_name="Nakagawa Test Latin")


def _japanese() -> bytes:
    return build_pgf([_glyph(0x3042), _glyph(0x30A2), _glyph(0x4E00)], font_name="Nakagawa Test Japanese")


def _korean() -> bytes:
    return build_pgf([_glyph(0xAC00), _glyph(0xD55C), _glyph(0x41)], font_name="Nakagawa Test Korean")


def _mixed_kana_hangul() -> bytes:
    return build_pgf([_glyph(0x3042), _glyph(0xAC00)], font_name="Nakagawa Test Mixed")


def _digits_only() -> bytes:
    return build_pgf([_glyph(0x30 + index) for index in range(10)], font_name="Nakagawa Test Digits")


def _patched(image: bytes, offset: int, value: bytes) -> bytes:
    out = bytearray(image)
    out[offset:offset + len(value)] = value
    return bytes(out)


def _corpus(rng: random.Random) -> dict:
    """Named images: valid, sparse, slot-coverage variants, and the refusals the task lists."""
    dense = _latin_dense()
    sparse = _latin_sparse()
    images: dict = {
        "dense_latin": dense,
        "sparse_latin": sparse,
        "japanese": _japanese(),
        "korean": _korean(),
        "mixed_kana_hangul": _mixed_kana_hangul(),
        "digits_only": _digits_only(),
        "truncated_0": dense[:0],
        "truncated_100": dense[:100],
        "truncated_391": dense[:391],
        "truncated_392_header_declares_more": dense[:392] + b"",
        "truncated_half": dense[: len(dense) // 2],
        "bad_magic": _patched(dense, 4, b"BAD0"),
        "header_offset_nonzero": _patched(dense, 0, struct.pack("<H", 1)),
        "revision_4": _patched(dense, 8, struct.pack("<i", 4)),
        "revision_negative": _patched(dense, 8, struct.pack("<i", -1)),
        "version_negative": _patched(dense, 12, struct.pack("<i", -1)),
        "header_412_revision_2": _patched(dense, 2, struct.pack("<H", 412)),
        "header_100": _patched(dense, 2, struct.pack("<H", 100)),
        "first_after_last": _patched(dense, 0xB6, struct.pack("<H", 0x5B)),
        "char_map_bits_zero": _patched(dense, 0x18, struct.pack("<I", 0)),
        "char_map_bits_33": _patched(dense, 0x18, struct.pack("<I", 33)),
        "pointer_bits_zero": _patched(dense, 0x1C, struct.pack("<I", 0)),
        "pointer_count_zero": _patched(dense, 0x14, struct.pack("<I", 0)),
        "pointer_count_over_limit": _patched(dense, 0x14, struct.pack("<I", (1 << 20) + 1)),
        "shadow_bits_8_without_shadows": _patched(dense, 0x170, struct.pack("<I", 8)),
        "shadow_count_without_16_bit_map": _patched(
            _patched(dense, 0x16C, struct.pack("<I", 1)), 0x170, struct.pack("<I", 8)
        ),
        "metric_count_overruns": _patched(dense, 0x102, bytes([255])),
        "too_large_one_over": dense + bytes(16 * 1024 * 1024 + 1 - len(dense)),
        "exactly_16_mib": dense + bytes(16 * 1024 * 1024 - len(dense)),
        "revision_3_header_no_extension": _patched(
            _patched(dense[:392] + bytes(20) + dense[392:], 2, struct.pack("<H", 412)),
            8,
            struct.pack("<i", 3),
        ),
        "revision_3_extension_count_set": _patched(
            _patched(dense[:392] + bytes(20) + dense[392:], 2, struct.pack("<H", 412)),
            0x18C,
            struct.pack("<H", 1),
        ),
    }
    # Glyph-level refusal: the first pointer entry points past the end of the image.
    pointer_offset = fonts._Image(dense)
    pointer_offset.parse_directory()
    at = pointer_offset.char_pointer_offset
    images["glyph_pointer_past_end"] = _patched(dense, at, b"\xff\xff\xff")
    # Fuzz: seeded byte flips and cuts over the valid images, so the parity pass reaches
    # the glyph-level rules and the sparse path, not only the named refusals.
    bases = [dense, sparse, images["japanese"], images["korean"]]
    for index in range(160):
        image = bytearray(rng.choice(bases))
        for _ in range(rng.randint(1, 4)):
            image[rng.randrange(len(image))] = rng.randrange(256)
        if rng.random() < 0.25:
            image = image[: rng.randrange(len(image) + 1)]
        images[f"fuzz_{index:03d}"] = bytes(image)
    return images


def _native_verdicts(executable: Path, paths: list) -> list:
    result = subprocess.run(
        [str(executable)],
        input="\n".join(str(path) for path in paths) + "\n",
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"verdict harness failed: {result.stderr}")
    return result.stdout.splitlines()


@unittest.skipUnless(CC, "no C compiler on PATH")
class PgfValidatorParityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="pgf_parity_")
        exe_name = "pgf_verdict_harness.exe" if sys.platform == "win32" else "pgf_verdict_harness"
        cls.exe = Path(cls.tmp.name) / exe_name
        build = subprocess.run(
            [
                CC, "-std=c99", "-Wall", "-Wextra", f"-I{ROOT / 'src' / 'rt'}", f"-I{ROOT / 'src' / 'core'}",
                "-o", str(cls.exe), str(HARNESS_C), str(READER_C), str(HOST_C),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if build.returncode != 0:
            raise AssertionError(f"building the verdict harness failed:\n{build.stderr}")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_python_mirror_agrees_with_native_reader_on_corpus(self) -> None:
        """Refusal, header facts, and coverage flags match for every corpus image."""
        rng = random.Random(20261009)
        images = _corpus(rng)
        folder = Path(self.tmp.name) / "corpus"
        folder.mkdir()
        paths = []
        for name, data in images.items():
            path = folder / f"{name}.pgf"
            path.write_bytes(data)
            paths.append(path)
        lines = _native_verdicts(self.exe, paths)
        self.assertEqual(len(lines), len(paths))

        mismatches = []
        accepted = 0
        refused_by = {}
        for path, line in zip(paths, lines, strict=True):
            tokens = line.split()
            native = {"refusal": fonts.REFUSAL_NAMES[int(tokens[0])], "ok": tokens[1] == "1"}
            data = path.read_bytes()
            mirror = fonts.pgf_verdict(data)
            if native["ok"]:
                accepted += 1
            else:
                refused_by[native["refusal"]] = refused_by.get(native["refusal"], 0) + 1
            if mirror["refusal"] != native["refusal"] or mirror["ok"] != native["ok"]:
                mismatches.append((path.name, mirror["refusal"], native["refusal"]))
                continue
            if native["ok"]:
                values = [int(token) for token in tokens[2:]]
                expected = [
                    mirror["revision"], mirror["header_size"], mirror["first_glyph"], mirror["last_glyph"],
                    mirror["glyph_count"], mirror["char_map_count"], mirror["nominal_h"], mirror["nominal_v"],
                    mirror["has_latin"], mirror["has_kana"], mirror["has_hangul"],
                ]
                if values != expected:
                    mismatches.append((path.name, values, expected))
        self.assertEqual(mismatches, [], f"Python and C disagree on {len(mismatches)} images: {mismatches[:5]}")
        # The corpus must reach every named refusal and both accepted shapes, or it proves little.
        for refusal in ("TOO_LARGE", "TRUNCATED", "HEADER_OFFSET", "MAGIC", "REVISION", "HEADER_SIZE",
                        "GLYPH_RANGE", "COUNTS", "NO_GLYPHS", "GLYPH"):
            self.assertIn(refusal, refused_by, f"corpus never reaches {refusal}")
        self.assertGreater(accepted, 20)

    def test_named_images_have_the_expected_verdicts(self) -> None:
        """The task's named cases refuse by name, and dense and sparse images are accepted."""
        dense = _latin_dense()
        sparse = _latin_sparse()
        self.assertTrue(fonts.pgf_verdict(dense)["ok"])
        sparse_verdict = fonts.pgf_verdict(sparse)
        self.assertTrue(sparse_verdict["ok"])
        self.assertGreater(sparse_verdict["char_map_count"], sparse_verdict["glyph_count"])
        self.assertEqual(fonts.pgf_verdict(dense[:100])["refusal"], "TRUNCATED")
        self.assertEqual(fonts.pgf_verdict(_patched(dense, 4, b"BAD0"))["refusal"], "MAGIC")
        self.assertEqual(fonts.pgf_verdict(_patched(dense, 0xB6, struct.pack("<H", 0x5B)))["refusal"], "GLYPH_RANGE")
        self.assertEqual(fonts.pgf_verdict(_patched(dense, 8, struct.pack("<i", 4)))["refusal"], "REVISION")
        self.assertEqual(fonts.pgf_verdict(_patched(dense, 2, struct.pack("<H", 412)))["refusal"], "HEADER_SIZE")
        self.assertEqual(fonts.pgf_verdict(bytes(16 * 1024 * 1024 + 1))["refusal"], "TOO_LARGE")

    def test_slot_layout_constants_match_the_c_headers(self) -> None:
        """The Python layout constants are the values in nk_font_slots.h, and the refusal order is pgf_api.h's."""
        header = SLOTS_H.read_text(encoding="utf-8")

        def define(name: str) -> str:
            match = re.search(rf'^#define {name} "([^"]*)"', header, re.M)
            self.assertIsNotNone(match, f"{name} missing from nk_font_slots.h")
            return match.group(1)

        def number(name: str) -> int:
            match = re.search(rf"^#define {name} (\d+)", header, re.M)
            self.assertIsNotNone(match, f"{name} missing from nk_font_slots.h")
            return int(match.group(1))

        self.assertEqual(define("NK_FONT_CACHE_PARENT"), fonts.CACHE_PARENT)
        self.assertEqual(define("NK_FONT_CACHE_SUBDIR"), fonts.CACHE_SUBDIR)
        self.assertEqual(define("NK_FONT_MANIFEST_NAME"), fonts.MANIFEST_NAME)
        self.assertEqual(define("NK_FONT_MANIFEST_SOURCE_USER"), fonts.MANIFEST_SOURCE_USER)
        self.assertEqual(define("NK_FONT_PROJECT_SUBDIR"), fonts.PROJECT_SUBDIR)
        self.assertEqual(number("NK_FONT_CACHE_SCHEMA_VERSION"), fonts.MANIFEST_SCHEMA_VERSION)
        self.assertEqual(number("NK_FONT_READER_VERSION"), fonts.READER_VERSION)
        self.assertEqual(number("NK_FONT_PGF_MAX_BYTES"), fonts.PGF_MAX_BYTES)
        self.assertEqual(define("NK_FONT_SLOT_JAPANESE_FILE"), fonts.SLOT_FILES["japanese"])
        self.assertEqual(define("NK_FONT_SLOT_LATIN_FILE"), fonts.SLOT_FILES["latin"])
        self.assertEqual(define("NK_FONT_SLOT_KOREAN_FILE"), fonts.SLOT_FILES["korean"])
        enum_match = re.search(r"NK_FONT_SLOT_JAPANESE = 0,\s*NK_FONT_SLOT_LATIN = 1,\s*NK_FONT_SLOT_KOREAN = 2", header)
        self.assertIsNotNone(enum_match, "NkFontSlot order changed; update fonts.SLOTS to match")

        api = PGF_API_H.read_text(encoding="utf-8")
        enum_body = re.search(r"typedef enum PgfRefusal \{(.*?)\} PgfRefusal;", api, re.S)
        self.assertIsNotNone(enum_body)
        names = re.findall(r"PGF_REFUSE_([A-Z_]+)", enum_body.group(1))
        self.assertEqual(names, list(fonts.REFUSAL_NAMES))


GOLDEN_HEADER = ROOT / "tests" / "native" / "pgf_golden_vectors.h"


def golden_vectors() -> dict:
    """Synthetic two-glyph fonts for the native font tests, one per slot. Built by pgf_writer."""
    def glyph(code: int) -> Glyph:
        return Glyph(code=code, width=2, height=2, samples=(1, 2, 3, 4), row_order=1)

    return {
        "Latin": build_pgf([glyph(0x41), glyph(0x61)], font_name="Nakagawa Test Latin"),
        "Japanese": build_pgf([glyph(0x41), glyph(0x3042)], font_name="Nakagawa Test Japanese"),
        "Korean": build_pgf([glyph(0x41), glyph(0xAC00)], font_name="Nakagawa Test Korean"),
    }


def golden_header_text() -> str:
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Copyright (C) 2026 the Nakagawa Recomp authors",
        "",
        "/* Generated by: python tools/test_pgf_validate_parity.py --regenerate",
        " * from tools/pgf_writer.py. Synthetic two-glyph fonts for the native font tests; they",
        " * hold no font data from any console or font vendor. */",
        "",
    ]
    for name, image in golden_vectors().items():
        lines.append(f"static const unsigned char kGolden{name}Pgf[{len(image)}] = {{")
        for start in range(0, len(image), 12):
            chunk = ", ".join(f"0x{byte:02x}" for byte in image[start:start + 12])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")
    return "\n".join(lines).rstrip("\n") + "\n"


class GoldenVectorTests(unittest.TestCase):
    def test_golden_header_matches_the_writer(self) -> None:
        """tests/native/pgf_golden_vectors.h is the writer's output; regenerate it when the writer changes."""
        self.assertTrue(GOLDEN_HEADER.is_file(), "run: python tools/test_pgf_validate_parity.py --regenerate")
        self.assertEqual(
            GOLDEN_HEADER.read_text(encoding="utf-8").replace("\r\n", "\n"),
            golden_header_text(),
            "pgf_golden_vectors.h is stale; run: python tools/test_pgf_validate_parity.py --regenerate",
        )

    def test_golden_vectors_are_accepted_and_classified(self) -> None:
        slots = {"Latin": "latin", "Japanese": "japanese", "Korean": "korean"}
        for name, image in golden_vectors().items():
            info = fonts.validate_pgf_data(image, f"{name}.pgf")
            self.assertEqual(info["slot"], slots[name], name)
            # Each golden font names U+0041 and one more code far from it: a sparse character map.
            self.assertTrue(info["sparse"], name)


if __name__ == "__main__":
    if "--regenerate" in sys.argv:
        GOLDEN_HEADER.write_text(golden_header_text(), encoding="utf-8", newline="\n")
        print(f"wrote {GOLDEN_HEADER}")
        sys.exit(0)
    unittest.main()
