#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Determinism, refusal, and reader round-trip coverage for tools/ttf2pgf.py (issue #313).

Layer 1 (behavioural): convert the pinned public fixture font
``fixtures/fonts/gudea/Gudea-Regular.ttf`` and open the result through the
public C reader ``src/rt/pgf_public.c`` with the harness
``tests/native/test_pgf_writer_roundtrip.c``.  Every guest record and every
drawn sample must equal the converter's own raster, the source-owned ``H``
shape must render as two stems plus a crossbar, the space must stay a
zero-area glyph, and a composite-glyph range must round-trip as well.

Layer 2 (reproducibility): the same pinned input, parameters, and converter
revision must produce byte-identical PGF and manifest output across repeated
runs, and the manifest must carry the digests, coverage, metrics, converter
identity, and the pin's complete licence material.

Layer 3 (refusals): malformed sfnt structures, unsupported outline and font
forms, out-of-range parameters, and policy-violating output names are refused
by name, and the CLI writes nothing on any refusal.

The fixture bytes are pinned by ``fixtures/fonts/gudea/PIN.json``; no test
downloads anything, and no firmware, retail, or third-party PGF bytes are
read anywhere.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from fractions import Fraction
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import ttf2pgf  # noqa: E402
from nk_core.fonts import validate_pgf_data  # noqa: E402
from ttf2pgf import DEFAULT_FONT_NAME, TtfConvertError, convert, load_pin, manifest_bytes  # noqa: E402

RT = ROOT / "src" / "rt"
HELPER_C = ROOT / "tests" / "native" / "test_pgf_writer_roundtrip.c"
READER_C = RT / "pgf_public.c"
TOOL = ROOT / "tools" / "ttf2pgf.py"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")

FONT_DIR = ROOT / "fixtures" / "fonts" / "gudea"
FONT = FONT_DIR / "Gudea-Regular.ttf"
PIN = FONT_DIR / "PIN.json"
LICENSE_TEXT = FONT_DIR / "OFL.txt"

DEFAULT_CODES = (0x20, 0x7E)
PPEM_SHAPE = 16  # large enough that the source-owned H shape is unambiguous


def _directory(data: bytes) -> dict[str, int]:
    """Map table tag -> byte offset of its directory entry (for crafting mutations)."""
    count = struct.unpack_from(">H", data, 4)[0]
    return {
        data[12 + 16 * index : 16 + 16 * index].decode("ascii"): 12 + 16 * index
        for index in range(count)
    }


def _patch(data: bytes, offset: int, blob: bytes) -> bytes:
    mutated = bytearray(data)
    mutated[offset : offset + len(blob)] = blob
    return bytes(mutated)


def _cli(*args: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(TOOL), *[str(arg) for arg in args]],
        capture_output=True,
        text=True,
    )


def _glyph_map(glyphs: tuple) -> dict[int, object]:
    return {glyph.code: glyph for glyph in glyphs}


def _clusters(samples: list[int], row: int, width: int) -> list[int]:
    """Lengths of maximal non-zero runs in one raster row."""
    runs: list[int] = []
    current = 0
    for value in samples[row * width : (row + 1) * width]:
        if value:
            current += 1
        elif current:
            runs.append(current)
            current = 0
    if current:
        runs.append(current)
    return runs


class CliRefusalMixin:
    """One assertion per named refusal: exit 2, stable reason, no output file."""

    def assert_cli_refused(self, reason: str, output: Path, *args: object) -> None:
        result = _cli(output, *args)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn(f"refused: {reason}:", result.stderr)
        self.assertFalse(output.exists(), "a refusal must write no output")


class ConversionDeterminismTests(unittest.TestCase):
    """Layer 2: byte-identical output and a complete conversion manifest."""

    def setUp(self) -> None:
        self.data = FONT.read_bytes()
        self.pin = load_pin(PIN, self.data)

    def _convert(self, **overrides: object):
        arguments: dict = {
            "input_path": str(FONT),
            "output_path": "gudea_latin.pgf",
            "codes": DEFAULT_CODES,
            "pin": self.pin,
        }
        arguments.update(overrides)
        return convert(self.data, **arguments)

    def test_same_pinned_input_produces_identical_bytes_and_manifest(self) -> None:
        first = self._convert()
        second = self._convert()
        self.assertEqual(first.image, second.image)
        self.assertEqual(manifest_bytes(first.manifest), manifest_bytes(second.manifest))
        self.assertEqual(first.glyphs, second.glyphs)

    def test_repeated_cli_runs_are_byte_identical(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ttf2pgf_det_") as tmp:
            image = Path(tmp) / "out.pgf"
            manifest = Path(tmp) / "out.json"
            args = (image, FONT, "--pin", PIN, "--manifest", manifest, "--ppem", PPEM_SHAPE)
            first = _cli(*args)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            image_bytes, manifest_bytes_first = image.read_bytes(), manifest.read_bytes()
            second = _cli(*args)
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            self.assertEqual(image.read_bytes(), image_bytes)
            self.assertEqual(manifest.read_bytes(), manifest_bytes_first)

    def test_cli_manifest_is_byte_identical_across_output_directories(self) -> None:
        """The same build run from two host directories records the same manifest (plan 6.2)."""
        manifests = []
        with tempfile.TemporaryDirectory(prefix="ttf2pgf_host_a_") as first_dir, \
                tempfile.TemporaryDirectory(prefix="ttf2pgf_host_b_") as second_dir:
            for directory in (first_dir, second_dir):
                image = Path(directory) / "gudea.pgf"
                manifest = Path(directory) / "gudea.json"
                result = _cli(image, FONT, "--pin", PIN, "--manifest", manifest, "--ppem", PPEM_SHAPE)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                manifests.append(manifest.read_bytes())
        self.assertEqual(manifests[0], manifests[1], "the manifest must not embed the output directory")

    def test_manifest_paths_are_basenames_under_either_host_separator(self) -> None:
        posix = self._convert(
            input_path="/srv/build/ofl/Gudea-Regular.ttf", output_path="/srv/build/out/gudea.pgf",
        )
        windows = self._convert(
            input_path="C:\\Work\\ofl\\Gudea-Regular.ttf", output_path="D:\\out\\gudea.pgf",
        )
        self.assertEqual(manifest_bytes(posix.manifest), manifest_bytes(windows.manifest))
        self.assertEqual(posix.manifest["input"]["path"], "Gudea-Regular.ttf")
        self.assertEqual(posix.manifest["output"]["path"], "gudea.pgf")

    def test_manifest_carries_digests_coverage_and_full_licence_material(self) -> None:
        conversion = self._convert()
        manifest = conversion.manifest
        self.assertEqual(
            manifest_bytes(manifest),
            manifest_bytes(json.loads(manifest_bytes(manifest))),
            "manifest JSON must round-trip byte-identically",
        )

        self.assertEqual(manifest["converter"]["name"], "ttf2pgf")
        self.assertEqual(manifest["converter"]["spec"], ttf2pgf.SPEC_PATH)
        self.assertEqual(manifest["converter"]["version"], ttf2pgf.CONVERTER_VERSION)

        self.assertEqual(manifest["input"]["sha256"], hashlib.sha256(self.data).hexdigest())
        self.assertEqual(manifest["input"]["sha256"], self.pin.sha256)
        self.assertEqual(manifest["input"]["size_bytes"], len(self.data))
        self.assertEqual(manifest["input"]["outline_format"], "glyf")
        self.assertEqual(
            sorted(manifest["input"]["tables_used"]), sorted(ttf2pgf.REQUIRED_TABLES)
        )

        self.assertEqual(manifest["output"]["sha256"], hashlib.sha256(conversion.image).hexdigest())
        self.assertEqual(manifest["output"]["size_bytes"], len(conversion.image))
        self.assertEqual(manifest["output"]["revision"], 2)
        self.assertEqual(manifest["output"]["font_name"], DEFAULT_FONT_NAME)

        coverage = manifest["coverage"]
        self.assertEqual((coverage["first"], coverage["last"]), DEFAULT_CODES)
        self.assertEqual(coverage["count"], DEFAULT_CODES[1] - DEFAULT_CODES[0] + 1)
        self.assertEqual(coverage["range"], "U+0020..U+007E")
        self.assertEqual(manifest["glyphs"]["count"], coverage["count"])
        self.assertEqual(manifest["glyphs"]["zero_area_codes"], [0x20])

        license_block = manifest["license"]
        self.assertEqual(license_block["spdx"], "OFL-1.1")
        self.assertEqual(license_block["reserved_font_names"], ["Gudea"])
        self.assertEqual(license_block["license_text"], LICENSE_TEXT.read_text(encoding="utf-8"))
        self.assertEqual(
            license_block["license_text_sha256"],
            hashlib.sha256(LICENSE_TEXT.read_bytes()).hexdigest(),
        )
        self.assertTrue(license_block["copyright"].startswith("Copyright (c)"))
        self.assertIn("google/fonts", license_block["source"]["repository"])
        self.assertEqual(license_block["source"]["path"], "ofl/gudea/Gudea-Regular.ttf")

        forbidden = {"timestamp", "generated_at", "created_at", "now"}

        def walk(value: object) -> None:
            if isinstance(value, dict):
                for key, child in value.items():
                    self.assertNotIn(str(key).lower(), forbidden, "manifest must not carry run-time fields")
                    walk(child)
            elif isinstance(value, list):
                for child in value:
                    walk(child)

        walk(manifest)

    def test_manifest_header_fields_match_the_emitted_bytes(self) -> None:
        conversion = self._convert(ppem=PPEM_SHAPE)
        image = conversion.image
        manifest = manifest_bytes(conversion.manifest)
        parsed = json.loads(manifest)
        self.assertEqual(
            struct.unpack_from("<i", image, 0x08)[0], parsed["output"]["revision"]
        )
        self.assertEqual(struct.unpack_from("<H", image, 0xB6)[0], parsed["output"]["first_glyph"])
        self.assertEqual(struct.unpack_from("<H", image, 0xB8)[0], parsed["output"]["last_glyph"])
        for offset, key in (
            (0xD4, "ascender_26_6"),
            (0xD8, "descender_26_6"),
            (0xF4, "max_width_26_6"),
            (0xF8, "max_height_26_6"),
            (0xEC, "max_advance_26_6"),
        ):
            self.assertEqual(struct.unpack_from("<i", image, offset)[0], parsed["output"][key], key)
        name = DEFAULT_FONT_NAME.encode("ascii")
        self.assertEqual(image[0x35 : 0x35 + len(name)], name)
        self.assertEqual(parsed["parameters"]["supersample"], ttf2pgf.SUPERSAMPLE)
        self.assertEqual(parsed["parameters"]["ppem"], conversion.ppem)

    def test_independent_structural_validator_accepts_the_output(self) -> None:
        conversion = self._convert()
        report = validate_pgf_data(conversion.image)
        self.assertIsInstance(report, dict)


class SourcePinTests(unittest.TestCase):
    """The pin binds the exact input bytes and licence text."""

    def test_pin_binds_the_exact_fixture_bytes(self) -> None:
        pin = load_pin(PIN, FONT.read_bytes())
        self.assertEqual(pin.sha256, hashlib.sha256(FONT.read_bytes()).hexdigest())
        self.assertEqual(pin.spdx, "OFL-1.1")
        self.assertEqual(pin.reserved_font_names, ("Gudea",))

    def test_a_single_flipped_byte_fails_the_pin(self) -> None:
        data = bytearray(FONT.read_bytes())
        data[5000] ^= 0xFF
        with self.assertRaises(TtfConvertError) as caught:
            load_pin(PIN, bytes(data))
        self.assertEqual(caught.exception.reason, "pin-digest-mismatch")

    def test_a_corrupt_pin_is_refused_by_name(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ttf2pgf_pin_") as tmp:
            bad = Path(tmp) / "PIN.json"
            bad.write_text('{"schema": "something-else"}', encoding="utf-8")
            with self.assertRaises(TtfConvertError) as caught:
                load_pin(bad, FONT.read_bytes())
            self.assertEqual(caught.exception.reason, "pin-invalid")


class RefusalTests(CliRefusalMixin, unittest.TestCase):
    """Layer 3: every malformed, unsupported, or policy-violating input is named."""

    def setUp(self) -> None:
        self.data = FONT.read_bytes()
        self.entries = _directory(self.data)
        self._tmp = tempfile.TemporaryDirectory(prefix="ttf2pgf_refuse_")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.output = self.tmp / "out.pgf"

    def _write(self, name: str, data: bytes) -> Path:
        path = self.tmp / name
        path.write_bytes(data)
        return path

    def test_otto_scaler_is_refused_as_postscript_outlines(self) -> None:
        path = self._write("otto.ttf", _patch(self.data, 0, b"OTTO"))
        self.assert_cli_refused("unsupported-outline-format", self.output, path)

    def test_collection_scaler_is_refused_by_version(self) -> None:
        path = self._write("ttcf.ttf", _patch(self.data, 0, b"ttcf"))
        self.assert_cli_refused("unsupported-sfnt-version", self.output, path)

    def test_missing_required_table_is_named(self) -> None:
        offset = self.entries["glyf"]
        path = self._write("noglyf.ttf", _patch(self.data, offset, b"XXXX"))
        self.assert_cli_refused("missing-table", self.output, path)

    def test_injected_variable_font_table_is_refused(self) -> None:
        offset = self.entries["GDEF"]
        path = self._write("fvar.ttf", _patch(self.data, offset, b"fvar"))
        self.assert_cli_refused("unsupported-variable-font", self.output, path)

    def test_injected_cff_table_is_refused(self) -> None:
        offset = self.entries["OS/2"]
        path = self._write("cff.ttf", _patch(self.data, offset, b"CFF "))
        self.assert_cli_refused("unsupported-outline-format", self.output, path)

    def test_table_extending_past_end_of_file_is_refused(self) -> None:
        offset = self.entries["name"] + 12
        path = self._write("trunc.ttf", _patch(self.data, offset, (0xFFFFFF).to_bytes(4, "big")))
        self.assert_cli_refused("truncated-table", self.output, path)

    def test_duplicate_directory_tag_is_refused(self) -> None:
        offset = self.entries["post"]
        path = self._write("dup.ttf", _patch(self.data, offset, b"name"))
        self.assert_cli_refused("malformed-font", self.output, path)

    def test_non_monotonic_loca_is_refused(self) -> None:
        entry = self.entries["loca"]
        table_offset = struct.unpack_from(">I", self.data, entry + 8)[0]
        path = self._write("loca.ttf", _patch(self.data, table_offset, b"\x00\x05\x00\x03"))
        self.assert_cli_refused("malformed-loca", self.output, path)

    def test_unmapped_code_point_is_refused(self) -> None:
        self.assert_cli_refused(
            "missing-code-point", self.output, FONT, "--codes", "0x00-0x20"
        )

    def test_inverted_code_range_is_refused(self) -> None:
        self.assert_cli_refused(
            "bad-code-range", self.output, FONT, "--codes", "0x50-0x40"
        )

    def test_out_of_range_ppem_is_refused(self) -> None:
        self.assert_cli_refused("ppem-out-of-range", self.output, FONT, "--ppem", "0")

    def test_oversized_glyph_is_refused_by_name(self) -> None:
        # A tiny unitsPerEm makes every outline huge at a legal ppem, which
        # reaches the writer's 7-bit record limit instead of any clamp.
        head_entry = self.entries["head"]
        table_offset = struct.unpack_from(">I", self.data, head_entry + 8)[0]
        mutated = _patch(self.data, table_offset + 18, struct.pack(">H", 16))
        with self.assertRaises(TtfConvertError) as caught:
            convert(
                mutated,
                input_path="crafted.ttf",
                output_path="out.pgf",
                codes=DEFAULT_CODES,
                ppem=48,
            )
        self.assertEqual(caught.exception.reason, "glyph-too-large")

    def test_output_name_using_a_pin_reserved_name_is_refused(self) -> None:
        result = _cli(self.output, FONT, "--pin", PIN, "--font-name", "Gudea Display")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("refused: reserved-font-name:", result.stderr)
        self.assertFalse(self.output.exists())

    def test_camouflage_font_name_is_refused(self) -> None:
        result = _cli(self.output, FONT, "--font-name", _joined("F", "TT-", "New", "Rodin Pro Latin"))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("refused: camouflage-font-name:", result.stderr)
        self.assertFalse(self.output.exists())

    def test_adobe_reserved_font_name_source_is_refused(self) -> None:
        result = _cli(self.output, FONT, "--font-name", "Source Compatible")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("refused: reserved-font-name:", result.stderr)
        self.assertFalse(self.output.exists())

    def test_codepoints_and_codes_are_mutually_exclusive(self) -> None:
        points = self._write("one.txt", b"0x41\n")
        result = _cli(self.output, FONT, "--codes", "0x41", "--codepoints", points)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("not allowed with", result.stderr)
        self.assertFalse(self.output.exists())

    def test_unsorted_or_repeated_codepoints_are_refused(self) -> None:
        for name, text in (("unsorted.txt", "0x48\n0x41\n"), ("repeated.txt", "0x41\n0x41\n")):
            with self.subTest(name=name):
                path = self._write(name, text.encode("ascii"))
                self.assert_cli_refused("codepoints-unsorted", self.output, FONT, "--codepoints", path)

    def test_malformed_or_empty_codepoint_list_is_refused(self) -> None:
        path = self._write("garbage.txt", b"0x41\nnot-a-code\n")
        self.assert_cli_refused("bad-codepoint-list", self.output, FONT, "--codepoints", path)
        path = self._write("empty.txt", b"# no code points\n\n")
        self.assert_cli_refused("empty-codepoint-set", self.output, FONT, "--codepoints", path)

    def test_listed_code_point_without_a_glyph_is_refused(self) -> None:
        path = self._write("unmapped.txt", b"0x01\n0x41\n")
        self.assert_cli_refused("missing-code-point", self.output, FONT, "--codepoints", path)

    def test_vendor_marker_in_the_font_name_is_refused_before_writing(self) -> None:
        result = _cli(self.output, FONT, "--font-name", "No" "to Sans")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("refused: reserved-font-name:", result.stderr)
        self.assertFalse(self.output.exists())

    def test_firmware_font_name_is_refused_by_the_writer(self) -> None:
        result = _cli(self.output, FONT, "--font-name", "ltn0")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("refused: font-field-invalid:", result.stderr)
        self.assertFalse(self.output.exists())

    def test_pin_mismatch_writes_nothing(self) -> None:
        data = bytearray(self.data)
        data[5000] ^= 0xFF
        path = self._write("flipped.ttf", bytes(data))
        self.assert_cli_refused("pin-digest-mismatch", self.output, path, "--pin", PIN)

    def test_manifest_without_pin_is_a_usage_error(self) -> None:
        result = _cli(self.output, FONT, "--manifest", self.tmp / "out.json")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("--manifest requires --pin", result.stderr)
        self.assertFalse(self.output.exists())

    def test_unreachable_metric_target_is_refused(self) -> None:
        targets = self.tmp / "targets.json"
        targets.write_text(
            json.dumps(
                {"ascender_px": 9, "descender_px": -9, "tolerance_px": 0.25, "ppem_min": 4, "ppem_max": 24}
            ),
            encoding="utf-8",
        )
        self.assert_cli_refused(
            "metric-target-unreachable", self.output, FONT, "--metric-targets", targets
        )

    def test_invalid_metric_target_document_is_refused(self) -> None:
        targets = self.tmp / "targets.json"
        targets.write_text('{"ascender_px": 9, "descender_px": -2, "tolerance_px": 0}', encoding="utf-8")
        self.assert_cli_refused(
            "metric-targets-invalid", self.output, FONT, "--metric-targets", targets
        )

    def test_explicit_ppem_and_metric_targets_conflict(self) -> None:
        targets = self.tmp / "targets.json"
        targets.write_text('{"ascender_px": 9, "descender_px": -2}', encoding="utf-8")
        with self.assertRaises(TtfConvertError) as caught:
            convert(
                self.data,
                input_path=str(FONT),
                output_path="out.pgf",
                codes=DEFAULT_CODES,
                ppem=12,
                metric_targets=ttf2pgf.load_metric_targets(targets),
            )
        self.assertEqual(caught.exception.reason, "conflicting-parameters")


class CompositeRefusalTests(unittest.TestCase):
    """Crafted composite mutations exercise the named unsupported paths."""

    def setUp(self) -> None:
        self.data = FONT.read_bytes()
        self.font = ttf2pgf._Ttf(self.data)
        self._tmp = tempfile.TemporaryDirectory(prefix="ttf2pgf_comp_")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)

    def _first_composite_code(self) -> int:
        for code in range(0xC0, 0x100):
            gid = self.font.glyph_id(code)
            if gid is None:
                continue
            start = self.font.loca[gid]
            if self.font.loca[gid + 1] > start and struct.unpack_from(">h", self.font.glyf, start)[0] == -1:
                return code
        raise AssertionError("the pinned fixture has no composite glyph in U+00C0..U+00FF")

    def _mutate_component(self, code: int, offset_delta: int, blob: bytes) -> bytes:
        gid = self.font.glyph_id(code)
        assert gid is not None
        start = self.font.loca[gid]
        glyf_entry = _directory(self.data)["glyf"]
        glyf_offset = struct.unpack_from(">I", self.data, glyf_entry + 8)[0]
        return _patch(self.data, glyf_offset + start + offset_delta, blob)

    def test_point_matching_component_arguments_are_refused(self) -> None:
        code = self._first_composite_code()
        gid = self.font.glyph_id(code)
        assert gid is not None
        start = self.font.loca[gid]
        flags = struct.unpack_from(">H", self.font.glyf, start + 10)[0]
        patched = flags & ~ttf2pgf._ARGS_XY
        data = self._mutate_component(code, 10, struct.pack(">H", patched))
        with self.assertRaises(TtfConvertError) as caught:
            convert(data, input_path="crafted.ttf", output_path="out.pgf", codes=(code, code))
        self.assertEqual(caught.exception.reason, "unsupported-composite-point-args")

    def test_two_by_two_component_transform_is_refused(self) -> None:
        code = self._first_composite_code()
        gid = self.font.glyph_id(code)
        assert gid is not None
        start = self.font.loca[gid]
        flags = struct.unpack_from(">H", self.font.glyf, start + 10)[0]
        self.assertEqual(flags & (ttf2pgf._A_SCALE | ttf2pgf._XY_SCALE), 0, "fixture assumption")
        patched = flags | ttf2pgf._TWO_BY_TWO
        data = self._mutate_component(code, 10, struct.pack(">H", patched))
        with self.assertRaises(TtfConvertError) as caught:
            convert(data, input_path="crafted.ttf", output_path="out.pgf", codes=(code, code))
        self.assertEqual(caught.exception.reason, "unsupported-composite-transform")

    def test_self_referential_component_is_refused_at_the_depth_bound(self) -> None:
        code = self._first_composite_code()
        gid = self.font.glyph_id(code)
        assert gid is not None
        data = self._mutate_component(code, 12, struct.pack(">H", gid))
        with self.assertRaises(TtfConvertError) as caught:
            convert(data, input_path="crafted.ttf", output_path="out.pgf", codes=(code, code))
        self.assertEqual(caught.exception.reason, "composite-too-deep")


class MalformedGlyphTests(unittest.TestCase):
    """Crafted endPtsOfContours values in the pinned glyf table are refused by name."""

    def setUp(self) -> None:
        self.data = FONT.read_bytes()
        self.font = ttf2pgf._Ttf(self.data)

    def _with_end_points(self, code: int, ends: tuple[int, ...]) -> bytes:
        gid = self.font.glyph_id(code)
        assert gid is not None
        start = self.font.loca[gid]
        self.assertEqual(struct.unpack_from(">h", self.font.glyf, start)[0], len(ends), "fixture assumption")
        glyf_offset = struct.unpack_from(">I", self.data, _directory(self.data)["glyf"] + 8)[0]
        return _patch(self.data, glyf_offset + start + 10, struct.pack(f">{len(ends)}H", *ends))

    def _assert_glyph_refused(self, code: int, ends: tuple[int, ...]) -> None:
        data = self._with_end_points(code, ends)
        with self.assertRaises(TtfConvertError) as caught:
            convert(data, input_path="crafted.ttf", output_path="out.pgf", codes=(code, code))
        self.assertEqual(caught.exception.reason, "malformed-glyph")

    def test_duplicate_end_point_index_is_refused(self) -> None:
        # 'O' has endPts (10, 21); a repeated index would silently drop the second contour.
        self._assert_glyph_refused(0x4F, (10, 10))

    def test_decreasing_middle_end_point_index_is_refused(self) -> None:
        # 'B' has endPts (14, 22, 30); a decreasing middle index would overlap contours.
        self._assert_glyph_refused(0x42, (14, 10, 30))

    def test_last_end_point_beyond_the_glyph_data_is_refused(self) -> None:
        # The last index implies 41 points, but 'O' carries only 22.
        self._assert_glyph_refused(0x4F, (10, 40))


class ExactArithmeticTests(unittest.TestCase):
    """Implied midpoints and de Casteljau halvings are exact rationals; no float reaches the rasterizer."""

    def test_implied_midpoint_of_large_odd_coordinates_is_exact(self) -> None:
        # 2**60 + 1 is not representable as a float; a float midpoint would round it to 2**59.
        big = 2**60 + 1
        segments = ttf2pgf._expand_contour([(big, 0, False), (0, 0, False)])
        implied_x = segments[0][3][0]
        self.assertEqual(implied_x, Fraction(big, 2))
        self.assertIsInstance(implied_x, Fraction)

    def test_expanded_contour_contains_no_float(self) -> None:
        segments = ttf2pgf._expand_contour([(0, 0, False), (3, 0, False), (3, 3, False), (0, 3, False)])
        self.assertTrue(segments)
        for segment in segments:
            for point in segment[1:]:
                for coordinate in point:
                    self.assertNotIsInstance(coordinate, float)

    def test_flattened_quadratic_contains_only_exact_coordinates(self) -> None:
        out: list[tuple] = []
        ttf2pgf._flatten_quad((0, 0), (1, 2), (2, 0), 0, out)
        self.assertGreater(len(out), 1, "the curve must subdivide for this check to mean anything")
        for point in out:
            for coordinate in point:
                self.assertIsInstance(coordinate, (int, Fraction))
                self.assertNotIsInstance(coordinate, float)

    def test_flattened_vertices_round_half_toward_positive_infinity(self) -> None:
        self.assertEqual(ttf2pgf._round26_6(Fraction(5, 2)), 3)
        self.assertEqual(ttf2pgf._round26_6(Fraction(-5, 2)), -2)
        self.assertEqual(ttf2pgf._round26_6(7), 7)

    def test_float_coordinates_are_rejected_not_rounded(self) -> None:
        with self.assertRaises(TypeError):
            ttf2pgf._round26_6(2.5)


def _format4_subtable(segments: list[tuple[int, int, int]]) -> bytes:
    """A format 4 cmap subtable of (start, end, idDelta) segments with no glyphIdArray."""
    count = len(segments)
    header = struct.pack(">HHHHHHH", 4, 16 + 8 * count, 0, 2 * count, 0, 0, 0)
    ends = struct.pack(f">{count}H", *(end for _, end, _ in segments))
    starts = struct.pack(f">{count}H", *(start for start, _, _ in segments))
    deltas = struct.pack(f">{count}h", *(delta for _, _, delta in segments))
    return header + ends + struct.pack(">H", 0) + starts + deltas + struct.pack(f">{count}H", *([0] * count))


class CmapTerminatorTests(unittest.TestCase):
    """U+FFFF is a noncharacter; the format 4 terminator segment must never map it."""

    def test_terminator_segment_does_not_map_u_ffff(self) -> None:
        # If the terminator were mapped, idDelta 5 would send U+FFFF to glyph 4.
        subtable = _format4_subtable([(0x41, 0x43, 0), (0xFFFF, 0xFFFF, 5)])
        self.assertEqual(ttf2pgf._parse_cmap4(subtable, 0), {0x41: 0x41, 0x42: 0x42, 0x43: 0x43})

    def test_range_ending_at_u_ffff_stops_before_u_ffff(self) -> None:
        mapping = ttf2pgf._parse_cmap4(_format4_subtable([(0xFFF0, 0xFFFF, 0)]), 0)
        self.assertEqual(sorted(mapping), list(range(0xFFF0, 0xFFFF)))
        self.assertNotIn(0xFFFF, mapping)


class MetricTargetTests(unittest.TestCase):
    """Metric targets are a separate policy input that selects the size."""

    def test_targets_select_a_ppem_whose_header_lands_in_tolerance(self) -> None:
        data = FONT.read_bytes()
        with tempfile.TemporaryDirectory(prefix="ttf2pgf_target_") as tmp:
            targets = Path(tmp) / "targets.json"
            targets.write_text(
                json.dumps(
                    {"ascender_px": 9, "descender_px": -2, "tolerance_px": 1, "ppem_min": 6, "ppem_max": 24}
                ),
                encoding="utf-8",
            )
            policy = ttf2pgf.load_metric_targets(targets)
            conversion = convert(
                data,
                input_path=str(FONT),
                output_path="target.pgf",
                codes=DEFAULT_CODES,
                metric_targets=policy,
                pin=load_pin(PIN, data),
            )
        header = ttf2pgf._read_header_fields(conversion.image)
        self.assertLessEqual(abs(header["ascender_26_6"] / 64 - 9.0), 1.0)
        self.assertLessEqual(abs(header["descender_26_6"] / 64 - (-2.0)), 1.0)
        recorded = conversion.manifest["metric_targets"]
        self.assertIsNotNone(recorded)
        assert recorded is not None
        self.assertEqual(recorded["achieved_ascender_26_6"], header["ascender_26_6"])
        self.assertEqual(recorded["achieved_descender_26_6"], header["descender_26_6"])
        self.assertEqual(conversion.manifest["parameters"]["ppem"], conversion.ppem)
        self.assertGreaterEqual(conversion.ppem, 6)
        self.assertLessEqual(conversion.ppem, 24)


@unittest.skipUnless(CC, "no C compiler on PATH")
class CodePointSetTests(unittest.TestCase):
    """--codepoints converts exactly the listed code points, and a sparse set is written sparse."""

    SET = (0x41, 0x48, 0x67, 0x7E)

    def setUp(self) -> None:
        self.data = FONT.read_bytes()
        self.pin = load_pin(PIN, self.data)
        self._tmp = tempfile.TemporaryDirectory(prefix="ttf2pgf_codepoints_")
        self.addCleanup(self._tmp.cleanup)
        self.points = Path(self._tmp.name) / "points.txt"
        self.points.write_text("".join(f"0x{code:04X}\n" for code in self.SET), encoding="ascii")

    def _convert(self, ppem: int = PPEM_SHAPE):
        code_points = ttf2pgf.load_code_points(self.points)
        return convert(
            self.data,
            input_path=str(FONT),
            output_path="sparse.pgf",
            code_points=code_points,
            ppem=ppem,
            pin=self.pin,
        )

    def test_listed_codes_only_are_converted_and_the_map_is_sparse(self) -> None:
        conversion = self._convert()
        image = conversion.image
        self.assertEqual(sorted(glyph.code for glyph in conversion.glyphs), list(self.SET))
        self.assertEqual(struct.unpack_from("<HH", image, 0xB6), (0x41, 0x7E))
        self.assertEqual(struct.unpack_from("<I", image, 0x10)[0], 0x7E - 0x41 + 1)
        self.assertEqual(struct.unpack_from("<I", image, 0x14)[0], len(self.SET))
        validate_pgf_data(image, "sparse.pgf")

    def test_manifest_records_the_point_list_by_digest_and_leaf_name(self) -> None:
        conversion = self._convert()
        points = conversion.manifest["code_points"]
        self.assertEqual(points["sha256"], hashlib.sha256(self.points.read_bytes()).hexdigest())
        self.assertEqual(points["count"], len(self.SET))
        self.assertEqual(points["path"], "points.txt")
        self.assertEqual(conversion.manifest["coverage"]["count"], len(self.SET))
        self.assertEqual(conversion.manifest["coverage"]["span"], 0x7E - 0x41 + 1)
        self.assertEqual(manifest_bytes(conversion.manifest), manifest_bytes(self._convert().manifest))

    def test_nominal_size_is_the_em_size_at_the_chosen_ppem_not_the_extrema(self) -> None:
        conversion = self._convert(ppem=16)
        image = conversion.image
        self.assertEqual(struct.unpack_from("<i", image, 0x24)[0], 16 * 64)
        self.assertEqual(struct.unpack_from("<i", image, 0x28)[0], 16 * 64)
        self.assertEqual(conversion.manifest["parameters"]["nominal_em_26_6"], 16 * 64)
        self.assertEqual(conversion.manifest["output"]["horizontal_size_26_6"], 16 * 64)
        self.assertNotEqual(
            conversion.manifest["output"]["max_advance_26_6"],
            16 * 64,
            "the fixture's advance extremum must differ from the nominal size for this check to mean anything",
        )


class ReaderRoundTripTests(unittest.TestCase):
    """Layer 1: the generated font opens through the production reader path."""

    @classmethod
    def setUpClass(cls) -> None:
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="ttf2pgf_roundtrip_")
        cls.exe = os.path.join(cls.tmp, "ttf2pgf_roundtrip.exe")
        result = subprocess.run(
            [
                CC,
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{RT}",
                "-o",
                cls.exe,
                str(READER_C),
                str(HELPER_C),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise AssertionError("the PGF round-trip harness did not build:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _read_back(self, image: bytes, codes: list[int]) -> list[str]:
        path = Path(self.tmp) / "converted.pgf"
        path.write_bytes(image)
        result = subprocess.run(
            [self.exe, str(path), *[str(code) for code in codes]],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.splitlines()

    def _convert(self, codes: tuple[int, int] = DEFAULT_CODES, ppem: int = PPEM_SHAPE):
        data = FONT.read_bytes()
        return convert(
            data,
            input_path=str(FONT),
            output_path="converted.pgf",
            codes=codes,
            ppem=ppem,
            pin=load_pin(PIN, data),
        )

    def test_every_drawn_sample_equals_the_converter_raster(self) -> None:
        conversion = self._convert()
        glyphs = _glyph_map(conversion.glyphs)
        probe = [0x48, 0x67, 0x32, 0x20, 0xE9]  # H, g, 2, space, absent U+00E9
        lines = self._read_back(conversion.image, probe)
        self.assertEqual(lines[0], "open ok")
        font = bytes.fromhex(lines[1].removeprefix("font "))
        self.assertEqual(len(font), 0x108)
        name = DEFAULT_FONT_NAME.encode("ascii")
        self.assertEqual(font[0x7C : 0x7C + len(name)], name)
        self.assertEqual(struct.unpack_from("<I", font, 0x54)[0], 0x7E - 0x20 + 1)

        records = lines[2:]
        self.assertEqual(len(records), len(probe) * 2)
        for index, code in enumerate(probe):
            char_line = records[index * 2].split(" ")
            draw_line = records[index * 2 + 1].split(" ")
            self.assertEqual(char_line[:2], ["char", str(code)])
            if code == 0xE9:
                self.assertEqual(char_line[2], "0")
                self.assertEqual(bytes.fromhex(char_line[3]), bytes(0x3C))
                self.assertEqual(draw_line[:3], ["draw", str(code), "none"])
                continue
            glyph = glyphs[code]
            if code == 0x20:
                self.assertEqual(char_line[2], "0", "a zero-area space is not present")
                self.assertEqual(draw_line[2:4], ["0", "0"])
                self.assertEqual(draw_line[4], "skipped")
                continue
            self.assertEqual(char_line[2], "1", f"U+{code:04X} presence")
            info = bytes.fromhex(char_line[3])
            self.assertEqual(len(info), 0x3C)
            self.assertEqual(struct.unpack_from("<I", info, 0x00)[0], glyph.width)
            self.assertEqual(struct.unpack_from("<I", info, 0x04)[0], glyph.height)
            self.assertEqual(struct.unpack_from("<i", info, 0x18)[0], glyph.y_base)
            self.assertEqual(struct.unpack_from("<i", info, 0x30)[0], glyph.advance_x)
            self.assertEqual(draw_line[2:4], [str(glyph.width), str(glyph.height)])
            drawn = list(bytes.fromhex(draw_line[4]))
            self.assertEqual(len(drawn), glyph.width * glyph.height)
            self.assertEqual(drawn, list(glyph.samples), f"U+{code:04X} drawn samples")

    def test_source_owned_h_renders_as_two_stems_and_a_crossbar(self) -> None:
        conversion = self._convert()
        glyph = _glyph_map(conversion.glyphs)[0x48]
        samples = list(glyph.samples)
        width, height = glyph.width, glyph.height
        self.assertGreaterEqual(width, 7)
        self.assertGreaterEqual(height, 8)
        for row in range(height):
            self.assertTrue(_clusters(samples, row, width), f"H row {row} must carry ink")
        self.assertGreaterEqual(len(_clusters(samples, 0, width)), 2, "H opens with two stems")
        self.assertGreaterEqual(len(_clusters(samples, height - 1, width)), 2, "H closes with two stems")
        middle = height // 2
        self.assertEqual(len(_clusters(samples, middle, width)), 1, "the crossbar joins the stems")
        self.assertGreaterEqual(_clusters(samples, middle, width)[0], width - 2, "the crossbar spans the H")
        self.assertEqual(max(samples), 15, "the stems reach full coverage")
        self.assertEqual(min(samples), 0, "the counters stay fully transparent")

    def test_latin1_composite_range_round_trips_through_the_reader(self) -> None:
        codes = (0xC0, 0xFF)
        conversion = self._convert(codes=codes)
        glyph = _glyph_map(conversion.glyphs)[0xE9]  # e-acute: a composite glyph
        lines = self._read_back(conversion.image, [0xE9, 0xC0])
        self.assertEqual(lines[0], "open ok")
        char_line = lines[2].split(" ")
        draw_line = lines[3].split(" ")
        self.assertEqual(char_line[2], "1")
        self.assertEqual(
            list(bytes.fromhex(draw_line[4])),
            list(glyph.samples),
            "composited outlines survive the writer and the reader bit-exactly",
        )
        self.assertTrue(any(glyph.samples), "the composite glyph has ink")

    def test_a_second_read_is_identical(self) -> None:
        conversion = self._convert()
        codes = [0x41, 0x67, 0x20]
        first = self._read_back(conversion.image, codes)
        second = self._read_back(conversion.image, codes)
        self.assertEqual(first, second)

    def test_sparse_codepoint_set_round_trips_and_absent_codes_are_misses(self) -> None:
        codes = CodePointSetTests.SET
        points = Path(self.tmp) / "set.txt"
        points.write_text("".join(f"0x{code:04X}\n" for code in codes), encoding="ascii")
        data = FONT.read_bytes()
        conversion = convert(
            data,
            input_path=str(FONT),
            output_path="converted.pgf",
            code_points=ttf2pgf.load_code_points(points),
            ppem=PPEM_SHAPE,
            pin=load_pin(PIN, data),
        )
        glyphs = _glyph_map(conversion.glyphs)
        probe = [0x41, 0x42, 0x48, 0x7E]  # U+0042 is outside the set
        lines = self._read_back(conversion.image, probe)
        self.assertEqual(lines[0], "open ok")
        font = bytes.fromhex(lines[1].removeprefix("font "))
        self.assertEqual(struct.unpack_from("<I", font, 0x54)[0], 0x7E - 0x41 + 1)
        records = lines[2:]
        for index, code in enumerate(probe):
            char_line = records[index * 2].split(" ")
            draw_line = records[index * 2 + 1].split(" ")
            self.assertEqual(char_line[:2], ["char", str(code)])
            if code not in glyphs:
                self.assertEqual(char_line[2], "0", f"U+{code:04X} is outside the set")
                self.assertEqual(draw_line[:3], ["draw", str(code), "none"])
                continue
            self.assertEqual(char_line[2], "1", f"U+{code:04X} presence")
            self.assertEqual(list(bytes.fromhex(draw_line[4])), list(glyphs[code].samples))


def _joined(*parts: str) -> str:
    """A name assembled from literal pieces, so this file never holds a vendor name whole."""
    return "".join(parts)


class NameDenylistTests(unittest.TestCase):
    """Output names: vendor camouflage and reserved or trademark names are refused by name."""

    def test_vendor_camouflage_names_are_refused(self) -> None:
        names = (
            _joined("F", "TT-", "New", "Rodin Pro Latin"),
            _joined("f", "tt-", "new", "rodin"),
            _joined("Asia", "NHH", "(512Johab)"),
            _joined("Asia", "KNHH", "-", "SO", "NY", "-uni"),
            _joined("Font", "works", " Latin"),
            _joined("SO", "NY", " Latin"),
        )
        for name in names:
            with self.subTest(name=name):
                with self.assertRaises(TtfConvertError) as caught:
                    ttf2pgf.validate_output_name(name, None)
                self.assertEqual(caught.exception.reason, "camouflage-font-name")

    def test_reserved_and_trademark_names_are_refused(self) -> None:
        names = (
            "Noto Sans JP", "Lato Display", "Atkinson Hyperlegible Next", "Nanum Gothic",
            "Sawarabi Gothic", "Gowun Dodum", "M PLUS 1p", "Ume Hy Gothic", "Source Han Sans",
        )
        for name in names:
            with self.subTest(name=name):
                with self.assertRaises(TtfConvertError) as caught:
                    ttf2pgf.validate_output_name(name, None)
                self.assertEqual(caught.exception.reason, "reserved-font-name")

    def test_project_names_and_ordinary_words_are_accepted(self) -> None:
        for name in ("Nakagawa Open Japanese", "Nakagawa Open Latin", "Nakagawa Open Korean",
                     "Platonic Serif", "Volume Mono", "Sonyx Latin"):
            with self.subTest(name=name):
                ttf2pgf.validate_output_name(name, None)  # must not raise


class VendorFontNameScanTests(unittest.TestCase):
    """Publication scan: no tracked source file names a vendor font.

    The scan covers every tracked file except Markdown and binary files. Markdown is the
    provenance record (docs/provenance/FONT_ORIGINS.md) that documents where each name was
    found, so it names the vendor fonts on purpose.

    The marker set is this class's own literal list, not read from the converter's
    denylist, so weakening that denylist cannot hide a name from this scan. It is narrower
    than the denylist on one point, deliberately: the console maker's name is not a
    whole-word marker here. That name is the platform's own identity and appears
    legitimately across tracked source (the PSP's device identifiers, the non-affiliation
    notice, third-party attribution), so a whole-word match would fail on those lines.
    Consequently a maker-branded font name in tracked source passes this scan; the
    converter's denylist refuses it at output instead.
    Each name is split across literals so this file does not contain it whole.
    """

    PATTERN = re.compile(
        "|".join(
            re.escape(part)
            for part in (_joined("New", "Rodin"), _joined("Asia", "KNHH"),
                         _joined("Asia", "NHH"), _joined("Font", "works"))
        )
        + r"|(?<![0-9A-Za-z])" + re.escape(_joined("F", "TT")) + r"(?![0-9A-Za-z])",
        re.IGNORECASE,
    )

    @classmethod
    def _hit_lines(cls, text: str) -> list[int]:
        """1-based numbers of the lines in ``text`` that the scan flags."""
        return [number for number, line in enumerate(text.splitlines(), start=1)
                if cls.PATTERN.search(line)]

    def test_scan_catches_each_listed_marker(self) -> None:
        # The tree test passes vacuously if the pattern is broken, so prove it fires.
        markers = (
            _joined("New", "Rodin"), _joined("Asia", "KNHH"), _joined("Asia", "NHH"),
            _joined("Font", "works"), _joined("F", "TT"),
        )
        for marker in markers:
            with self.subTest(marker=marker):
                self.assertEqual(self._hit_lines("font = '" + marker + "'\n"), [1])

    def test_whole_word_marker_needs_a_word_boundary(self) -> None:
        self.assertEqual(self._hit_lines("x = 'X" + _joined("F", "TT") + "Y'\n"), [])

    def test_platform_prose_naming_the_console_maker_is_not_flagged(self) -> None:
        # The maker's name is deliberately not a marker here (see the class docstring).
        prose = "# Not affiliated with " + _joined("SO", "NY") + " Interactive Entertainment.\n"
        self.assertEqual(self._hit_lines(prose), [])

    def test_tracked_source_names_no_vendor_font(self) -> None:
        if shutil.which("git") is None:
            self.skipTest("git is required to enumerate the tracked tree")
        listing = subprocess.run(
            ["git", "-C", str(ROOT), "ls-files", "-z"], capture_output=True, text=True, check=True,
        ).stdout
        offenders = []
        for entry in sorted(filter(None, listing.split("\0"))):
            if entry.lower().endswith(".md"):
                continue
            try:
                data = (ROOT / entry).read_bytes()
            except OSError:
                continue
            if b"\0" in data:
                continue
            try:
                text = data.decode("utf-8")
            except UnicodeDecodeError:
                continue
            offenders.extend(f"{entry}:{number}" for number in self._hit_lines(text))
        self.assertEqual(offenders, [], "tracked source names a vendor font; use a project-owned name")


if __name__ == "__main__":
    unittest.main()
