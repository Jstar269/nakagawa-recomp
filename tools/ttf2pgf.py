#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Deterministic OpenType/TTF -> PSP PGF converter for public fixtures (issue #313).

The converter reads a pinned TrueType-outline font, rasterizes the requested
code points to 4-bit alpha samples, and emits the PGF container written by
``pgf_writer.build_pgf`` so the public reader ``src/rt/pgf_public.c`` can open
the result through its published ABI.  The behaviour contract lives in
``docs/cleanroom/PGF_CONVERTER_SPEC.md``; this module is the project-authored
implementation of that specification.

Determinism is a hard property, mirroring ``pgf_writer``.  Every value --
scaling, curve flattening, the supersample grid, coverage rounding, iteration
order, and the manifest JSON -- is a pure function of the input bytes, the
pinned parameters, and documented integer/rational arithmetic.  No clock,
locale, environment variable, hash-order iteration, or random source is
consulted, so the same pinned input and converter revision produce
byte-identical PGF and manifest output on every supported host.

Malformed or unsupported input is refused by name through
:class:`TtfConvertError`; nothing is clamped, guessed, or silently dropped.
The CLI writes no file until every stage has succeeded.

Scope boundaries (see the specification): TrueType ``glyf`` outlines only (no
CFF, no variable fonts), no hint execution, no kerning pairs (PGF carries
per-glyph advances only), and honest project-owned output names -- firmware
font names, vendor camouflage names, and reserved or trademark font names are
refused rather than reproduced (the denylist lives in ``pgf_writer``).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import sys
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path

try:  # pragma: no cover - the tests import both styles
    from . import pgf_writer
except ImportError:  # direct script execution: tools/ is the script directory
    import pgf_writer  # type: ignore[no-redef]

CONVERTER_NAME = "ttf2pgf"
CONVERTER_VERSION = "0.1.0"
SPEC_PATH = "docs/cleanroom/PGF_CONVERTER_SPEC.md"
PIN_SCHEMA = "font-pin/v1"

#: Output supersampling grid: 8x8 sub-samples per pixel before the 4-bit round.
SUPERSAMPLE = 8
SUB_PIXEL = 64 // SUPERSAMPLE  #: sub-sample step in 26.6 units (8 units)
#: Quadratic flatness threshold: second-difference manhattan norm in 26.6 units.
FLATNESS = Fraction(2, 1)
#: Hard bound for the recursive flattener; the second difference shrinks 4x per
#: level, so a well-formed outline converges long before this depth.
MAX_CURVE_DEPTH = 24
MAX_COMPOSITE_DEPTH = 8
MAX_COMPOSITE_COMPONENTS = 64

MIN_PPEM = 1
MAX_PPEM = 128
DEFAULT_PPEM = 12
DEFAULT_CODES = "0x20-0x7E"
DEFAULT_FONT_NAME = "Nakagawa Open Latin"
DEFAULT_FONT_TYPE = "Regular"

SCALER_TRUE = 0x00010000
SCALER_APPLE_TRUE = 0x74727565  # 'true'
SCALER_OTTO = 0x4F54544F  # 'OTTO': PostScript outlines, refused by name
REQUIRED_TABLES = ("cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp")
OUTLINE_TABLE_REFUSALS = ("CFF ", "CFF2")
VARIABLE_FONT_TABLES = ("fvar", "gvar", "cvar", "HVAR", "MVAR", "STAT")

SUPPORTED_CMAP_FORMATS = (0, 4, 6, 12)
#: Deterministic subtable preference: Unicode platforms first, then Microsoft
#: full-repertoire, Microsoft BMP, Microsoft symbol, then legacy records.
CMAP_PRIORITY = ((0, 3), (0, 4), (0, 2), (0, 1), (0, 0), (3, 10), (3, 1), (3, 6), (3, 0), (1, 0))

MAX_TABLE_COUNT = 512
MAX_CODE = 0xFFFF

# Composite glyf flags (OpenType specification).
_ARG_WORDS = 0x0001
_ARGS_XY = 0x0002
_A_SCALE = 0x0008
_MORE = 0x0020
_XY_SCALE = 0x0040
_TWO_BY_TWO = 0x0080
_INSTRUCTIONS = 0x0100
_SCALED_OFFSET = 0x0800

# Simple glyf point flags.
_ON_CURVE = 0x01
_X_SHORT = 0x02
_Y_SHORT = 0x04
_REPEAT = 0x08
_X_SAME = 0x10
_Y_SAME = 0x20


class TtfConvertError(ValueError):
    """A named refusal from the converter.

    :attr:`reason` is the stable reason name asserted by the tests and printed
    by the CLI; the rest of the message is human context.
    """

    def __init__(self, reason: str, detail: str) -> None:
        super().__init__(f"{reason}: {detail}")
        self.reason = reason


def _refuse(reason: str, detail: str) -> TtfConvertError:
    return TtfConvertError(reason, detail)


# ---------------------------------------------------------------------------
# Source pin: input attribution and licence material
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class SourcePin:
    """Verified provenance for one pinned input font (schema ``font-pin/v1``)."""

    path: Path
    sha256: str
    size_bytes: int
    spdx: str
    copyright: str
    license_text: str
    license_text_sha256: str
    license_text_file: str
    reserved_font_names: tuple[str, ...]
    source: dict[str, str]
    family: str | None


def _hex64(value: object, label: str) -> str:
    if not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
        raise _refuse("pin-invalid", f"{label} must be a lowercase 64-character SHA-256 hex string")
    return value


def _string(value: object, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise _refuse("pin-invalid", f"{label} must be a non-empty string")
    return value


def load_pin(pin_path: Path, input_bytes: bytes) -> SourcePin:
    """Load and verify the source pin against the exact input bytes.

    The pin binds the input digest, the licence text bytes, and the reserved
    font names the output name must avoid.  Any mismatch is a named refusal.
    """
    try:
        document = json.loads(pin_path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise _refuse("pin-invalid", f"cannot read {pin_path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise _refuse("pin-invalid", f"{pin_path} is not valid JSON: {exc}") from exc
    if not isinstance(document, dict):
        raise _refuse("pin-invalid", f"{pin_path} must contain a JSON object")
    if document.get("schema") != PIN_SCHEMA:
        raise _refuse("pin-invalid", f"schema must be {PIN_SCHEMA!r}")

    digest = hashlib.sha256(input_bytes).hexdigest()
    pinned = _hex64(document.get("sha256"), "sha256")
    if digest != pinned:
        raise _refuse("pin-digest-mismatch", f"input SHA-256 {digest} does not match the pin {pinned}")
    size = document.get("size_bytes")
    if not isinstance(size, int) or isinstance(size, bool) or size != len(input_bytes):
        raise _refuse("pin-size-mismatch", f"input is {len(input_bytes)} bytes, pin records {size!r}")

    license_block = document.get("license")
    if not isinstance(license_block, dict):
        raise _refuse("pin-invalid", "license must be an object")
    spdx = _string(license_block.get("spdx"), "license.spdx")
    copyright_line = _string(license_block.get("copyright"), "license.copyright")
    text_file = _string(license_block.get("license_text"), "license.license_text")
    text_digest = _hex64(license_block.get("license_text_sha256"), "license.license_text_sha256")
    names = license_block.get("reserved_font_names")
    if not isinstance(names, list) or any(not isinstance(name, str) or not name for name in names):
        raise _refuse("pin-invalid", "license.reserved_font_names must be a list of non-empty strings")

    text_path = pin_path.parent / text_file
    try:
        license_text = text_path.read_bytes().decode("utf-8")
    except OSError as exc:
        raise _refuse("pin-invalid", f"cannot read licence text {text_path}: {exc}") from exc
    except UnicodeDecodeError as exc:
        raise _refuse("pin-invalid", f"licence text {text_path} is not UTF-8: {exc}") from exc
    actual_text_digest = hashlib.sha256(license_text.encode("utf-8")).hexdigest()
    if actual_text_digest != text_digest:
        raise _refuse(
            "pin-license-mismatch",
            f"licence text SHA-256 {actual_text_digest} does not match the pin {text_digest}",
        )

    source = document.get("source")
    if not isinstance(source, dict):
        raise _refuse("pin-invalid", "source must be an object")
    for field in ("url", "repository", "path", "retrieved"):
        _string(source.get(field), f"source.{field}")

    family = document.get("family")
    if family is not None and (not isinstance(family, str) or not family):
        raise _refuse("pin-invalid", "family must be a non-empty string when present")

    return SourcePin(
        path=pin_path,
        sha256=pinned,
        size_bytes=size,
        spdx=spdx,
        copyright=copyright_line,
        license_text=license_text,
        license_text_sha256=text_digest,
        license_text_file=text_file,
        reserved_font_names=tuple(names),
        source={key: str(source[key]) for key in ("url", "repository", "path", "retrieved")},
        family=family,
    )


def validate_output_name(name: str, pin: SourcePin | None) -> None:
    """Refuse firmware, camouflage, vendor, and reserved names before anything is written."""
    reason = pgf_writer.denied_font_name(name)
    if reason is not None:
        detail = (
            "reproduces a vendor camouflage marker"
            if reason == "camouflage-font-name"
            else "contains a reserved or trademark font name"
        )
        raise _refuse(reason, f"{name!r} {detail} (see docs/provenance/FONT_ORIGINS.md)")
    lowered = name.casefold()
    if pin is not None:
        for reserved in pin.reserved_font_names:
            if reserved.casefold() in lowered:
                raise _refuse(
                    "reserved-font-name",
                    f"{name!r} contains {reserved!r}, a Reserved Font Name listed by the source pin",
                )


# ---------------------------------------------------------------------------
# Bounds-checked sfnt readers
# ---------------------------------------------------------------------------


def _u16(data: bytes, offset: int, section: str) -> int:
    if offset < 0 or offset + 2 > len(data):
        raise _refuse("malformed-font", f"{section}: read of 2 bytes at {offset} is out of bounds")
    return struct.unpack_from(">H", data, offset)[0]


def _s16(data: bytes, offset: int, section: str) -> int:
    if offset < 0 or offset + 2 > len(data):
        raise _refuse("malformed-font", f"{section}: read of 2 bytes at {offset} is out of bounds")
    return struct.unpack_from(">h", data, offset)[0]


def _u32(data: bytes, offset: int, section: str) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise _refuse("malformed-font", f"{section}: read of 4 bytes at {offset} is out of bounds")
    return struct.unpack_from(">I", data, offset)[0]


def _s8(data: bytes, offset: int, section: str) -> int:
    if offset < 0 or offset + 1 > len(data):
        raise _refuse("malformed-font", f"{section}: read of 1 byte at {offset} is out of bounds")
    return struct.unpack_from(">b", data, offset)[0]


@dataclass(frozen=True)
class _Subtable:
    platform: int
    encoding: int
    offset: int
    format: int


class _Ttf:
    """Bounds-checked read view over one TrueType-outline font.

    The parsed state is immutable after construction except for the outline
    cache, which memoizes exact font-unit contours per glyph id.
    """

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.tables = _parse_directory(data)
        for tag in VARIABLE_FONT_TABLES:
            if tag in self.tables:
                raise _refuse(
                    "unsupported-variable-font",
                    f"table {tag!r} marks a variable font; only static default outlines are supported",
                )
        for tag in OUTLINE_TABLE_REFUSALS:
            if tag in self.tables:
                raise _refuse(
                    "unsupported-outline-format",
                    f"table {tag!r} carries PostScript outlines; only glyf outlines are supported",
                )
        for tag in REQUIRED_TABLES:
            if tag not in self.tables:
                raise _refuse("missing-table", f"required table {tag!r} is absent")

        head = self._section("head")
        if len(head) < 54:
            raise _refuse("malformed-head", f"head is {len(head)} bytes; the specification needs 54")
        if _u32(head, 12, "head") != 0x5F0F3CF5:
            raise _refuse("malformed-head", "magicNumber is not 0x5F0F3CF5")
        self.units_per_em = _u16(head, 18, "head")
        if not 16 <= self.units_per_em <= 16384:
            raise _refuse("malformed-head", f"unitsPerEm {self.units_per_em} is outside 16..16384")
        self.loca_format = _s16(head, 50, "head")
        if self.loca_format not in (0, 1):
            raise _refuse(
                "unsupported-loca-format",
                f"indexToLocFormat {self.loca_format} is neither 0 (short) nor 1 (long)",
            )

        maxp = self._section("maxp")
        if len(maxp) < 6:
            raise _refuse("malformed-maxp", f"maxp is {len(maxp)} bytes; the specification needs 6")
        self.num_glyphs = _u16(maxp, 4, "maxp")
        if self.num_glyphs == 0:
            raise _refuse("malformed-maxp", "numGlyphs is 0")

        hhea = self._section("hhea")
        if len(hhea) < 36:
            raise _refuse("malformed-hhea", f"hhea is {len(hhea)} bytes; the specification needs 36")
        self.num_h_metrics = _u16(hhea, 34, "hhea")
        if not 0 < self.num_h_metrics <= self.num_glyphs:
            raise _refuse(
                "malformed-hhea",
                f"numberOfHMetrics {self.num_h_metrics} must be within 1..{self.num_glyphs}",
            )

        hmtx = self._section("hmtx")
        expected_hmtx = self.num_h_metrics * 4 + (self.num_glyphs - self.num_h_metrics) * 2
        if len(hmtx) != expected_hmtx:
            raise _refuse(
                "malformed-hmtx",
                f"hmtx is {len(hmtx)} bytes; the specification needs exactly {expected_hmtx}",
            )
        self.hmtx = hmtx

        loca = self._section("loca")
        step = 2 if self.loca_format == 0 else 4
        expected_loca = (self.num_glyphs + 1) * step
        if len(loca) != expected_loca:
            raise _refuse(
                "malformed-loca",
                f"loca is {len(loca)} bytes; the specification needs exactly {expected_loca}",
            )
        glyf_size = self.tables["glyf"][1]
        unpack = ">H" if step == 2 else ">I"
        offsets: list[int] = []
        previous = -1
        for index in range(self.num_glyphs + 1):
            value = struct.unpack_from(unpack, loca, index * step)[0]
            if step == 2:
                value *= 2
            if value < previous:
                raise _refuse(
                    "malformed-loca",
                    f"entry {index} ({value}) precedes entry {index - 1} ({previous})",
                )
            if value > glyf_size:
                raise _refuse(
                    "malformed-loca",
                    f"entry {index} points {value} bytes into a {glyf_size}-byte glyf table",
                )
            previous = value
            offsets.append(value)
        self.loca = offsets
        self.glyf = self._section("glyf")
        self.cmap = self._parse_cmap(self._section("cmap"))
        self._outline_cache: dict[int, list[list[tuple]]] = {}

    def _section(self, tag: str) -> bytes:
        offset, length = self.tables[tag]
        return self.data[offset : offset + length]

    def _parse_cmap(self, cmap: bytes) -> dict[int, int]:
        if len(cmap) < 4:
            raise _refuse("malformed-cmap", f"cmap is {len(cmap)} bytes; the header needs 4")
        count = _u16(cmap, 2, "cmap")
        if 4 + 8 * count > len(cmap):
            raise _refuse(
                "malformed-cmap",
                f"encoding records extend to {4 + 8 * count} bytes of a {len(cmap)}-byte table",
            )
        records: list[tuple[int, int, _Subtable]] = []
        for index in range(count):
            platform, encoding, rel = struct.unpack_from(">HHI", cmap, 4 + 8 * index)
            if rel + 2 > len(cmap):
                raise _refuse(
                    "malformed-cmap",
                    f"encoding record {index} points to offset {rel}, beyond the table",
                )
            fmt = _u16(cmap, rel, "cmap")
            try:
                rank = CMAP_PRIORITY.index((platform, encoding))
            except ValueError:
                continue
            if fmt in SUPPORTED_CMAP_FORMATS:
                records.append((rank, index, _Subtable(platform, encoding, rel, fmt)))
        records.sort(key=lambda entry: (entry[0], entry[1]))
        unsupported = False
        for _, _, subtable in records:
            try:
                return _parse_cmap_subtable(cmap, subtable)
            except TtfConvertError as exc:
                if exc.reason == "unsupported-cmap-format":
                    unsupported = True
                    continue
                raise
        if unsupported:
            raise _refuse(
                "unsupported-cmap-format",
                "no character map uses a supported subtable format (0, 4, 6, or 12)",
            )
        raise _refuse("malformed-cmap", "the font declares no usable Unicode character map")

    def glyph_id(self, code: int) -> int | None:
        """Return the mapped glyph id; the .notdef mapping counts as absent."""
        gid = self.cmap.get(code)
        if not gid or gid >= self.num_glyphs:
            return None
        return gid

    def advance_units(self, gid: int) -> int:
        if gid < self.num_h_metrics:
            return _u16(self.hmtx, gid * 4, "hmtx")
        return _u16(self.hmtx, (self.num_h_metrics - 1) * 4, "hmtx")

    def outline(self, gid: int, depth: int = 0, budget: list[int] | None = None) -> list[list[tuple]]:
        """Return one glyph's contours in font units (ints, or Fractions for scaled components)."""
        if depth > MAX_COMPOSITE_DEPTH:
            raise _refuse("composite-too-deep", f"component nesting exceeds {MAX_COMPOSITE_DEPTH} levels")
        cached = self._outline_cache.get(gid)
        if cached is not None:
            return cached
        if budget is None:
            budget = [0]
        start, end = self.loca[gid], self.loca[gid + 1]
        if start == end:
            contours: list[list[tuple]] = []
        else:
            if end - start < 10:
                raise _refuse(
                    "malformed-glyph",
                    f"glyph {gid} has {end - start} bytes; the record header needs 10",
                )
            kind = _s16(self.glyf, start, "glyf")
            if kind >= 0:
                contours = _parse_simple_glyph(self.glyf, start, end, kind, gid)
            elif kind == -1:
                contours = self._parse_composite(start, end, gid, depth, budget)
            else:
                raise _refuse(
                    "malformed-glyph",
                    f"glyph {gid} declares numberOfContours {kind}; only 0..n and -1 are defined",
                )
        if depth == 0:
            self._outline_cache[gid] = contours
        return contours

    def _parse_composite(self, start: int, end: int, gid: int, depth: int, budget: list[int]) -> list:
        glyf = self.glyf
        pos = start + 10
        components: list[tuple[int, Fraction, Fraction, Fraction, Fraction]] = []
        flags = _MORE
        while flags & _MORE:
            if pos + 4 > end:
                raise _refuse("malformed-glyph", f"composite glyph {gid} is truncated at a component header")
            flags = _u16(glyf, pos, "glyf")
            component_gid = _u16(glyf, pos + 2, "glyf")
            pos += 4
            if component_gid >= self.num_glyphs:
                raise _refuse(
                    "malformed-glyph",
                    f"composite glyph {gid} references glyph {component_gid}, beyond {self.num_glyphs}",
                )
            if flags & _ARG_WORDS:
                if pos + 4 > end:
                    raise _refuse("malformed-glyph", f"composite glyph {gid} is truncated in its arguments")
                arg1, arg2 = _s16(glyf, pos, "glyf"), _s16(glyf, pos + 2, "glyf")
                pos += 4
            else:
                if pos + 2 > end:
                    raise _refuse("malformed-glyph", f"composite glyph {gid} is truncated in its arguments")
                arg1 = _s8(glyf, pos, "glyf")
                arg2 = _s8(glyf, pos + 1, "glyf")
                pos += 2
            if not flags & _ARGS_XY:
                raise _refuse(
                    "unsupported-composite-point-args",
                    f"composite glyph {gid} matches points instead of offsets; only XY offsets are supported",
                )
            scale_x = scale_y = Fraction(1, 1)
            if flags & _A_SCALE:
                if pos + 2 > end:
                    raise _refuse("malformed-glyph", f"composite glyph {gid} is truncated in its scale")
                scale_x = scale_y = _f2dot14(_s16(glyf, pos, "glyf"))
                pos += 2
            elif flags & _XY_SCALE:
                if pos + 4 > end:
                    raise _refuse("malformed-glyph", f"composite glyph {gid} is truncated in its scales")
                scale_x = _f2dot14(_s16(glyf, pos, "glyf"))
                scale_y = _f2dot14(_s16(glyf, pos + 2, "glyf"))
                pos += 4
            elif flags & _TWO_BY_TWO:
                raise _refuse(
                    "unsupported-composite-transform",
                    f"composite glyph {gid} uses a 2x2 transform; only uniform and axis scales are supported",
                )
            offset_x: Fraction = Fraction(arg1, 1)
            offset_y: Fraction = Fraction(arg2, 1)
            if flags & _SCALED_OFFSET:
                offset_x *= scale_x
                offset_y *= scale_y
            budget[0] += 1
            if budget[0] > MAX_COMPOSITE_COMPONENTS:
                raise _refuse(
                    "composite-too-many-components",
                    f"composite expansion exceeds {MAX_COMPOSITE_COMPONENTS} components",
                )
            components.append((component_gid, offset_x, offset_y, scale_x, scale_y))
            if flags & _MORE:
                continue
            if flags & _INSTRUCTIONS:
                # Hint programs are present in the file but never executed:
                # rasterization is unhinted by specification.
                if pos + 2 > end:
                    raise _refuse(
                        "malformed-glyph",
                        f"composite glyph {gid} is truncated at its instruction length",
                    )
                length = _u16(glyf, pos, "glyf")
                if pos + 2 + length > end:
                    raise _refuse(
                        "malformed-glyph",
                        f"composite glyph {gid} declares {length} hint bytes beyond the record",
                    )
            break
        contours: list = []
        for component_gid, offset_x, offset_y, scale_x, scale_y in components:
            for contour in self.outline(component_gid, depth + 1, budget):
                contours.append(
                    [
                        (x * scale_x + offset_x, y * scale_y + offset_y, on)
                        for x, y, on in contour
                    ]
                )
        return contours


def _f2dot14(raw: int) -> Fraction:
    return Fraction(raw, 16384)


def _parse_directory(data: bytes) -> dict[str, tuple[int, int]]:
    if len(data) < 12:
        raise _refuse("malformed-font", f"file is {len(data)} bytes; the sfnt header needs 12")
    scaler = struct.unpack_from(">I", data, 0)[0]
    if scaler == SCALER_OTTO:
        raise _refuse("unsupported-outline-format", "sfnt scaler 'OTTO' carries PostScript outlines")
    if scaler not in (SCALER_TRUE, SCALER_APPLE_TRUE):
        raise _refuse("unsupported-sfnt-version", f"scaler type 0x{scaler:08X} is not a TrueType sfnt")
    count = _u16(data, 4, "directory")
    if not 1 <= count <= MAX_TABLE_COUNT:
        raise _refuse("malformed-font", f"numTables {count} is outside 1..{MAX_TABLE_COUNT}")
    if 12 + 16 * count > len(data):
        raise _refuse("malformed-font", f"the {count} directory entries extend beyond the file")
    tables: dict[str, tuple[int, int]] = {}
    for index in range(count):
        tag_raw, _checksum, offset, length = struct.unpack_from(">4sIII", data, 12 + 16 * index)
        try:
            tag = tag_raw.decode("ascii")
        except UnicodeDecodeError as exc:
            raise _refuse("malformed-font", f"directory entry {index} has a non-ASCII tag {tag_raw!r}") from exc
        if tag in tables:
            raise _refuse("malformed-font", f"table tag {tag!r} appears twice in the directory")
        if offset + length > len(data):
            raise _refuse(
                "truncated-table",
                f"table {tag!r} claims bytes {offset}..{offset + length} of a {len(data)}-byte file",
            )
        tables[tag] = (offset, length)
    return tables


def _parse_cmap_subtable(cmap: bytes, subtable: _Subtable) -> dict[int, int]:
    rel, fmt = subtable.offset, subtable.format
    if fmt not in SUPPORTED_CMAP_FORMATS:
        raise _refuse("unsupported-cmap-format", f"cmap format {fmt} is not supported")
    if fmt == 0:
        if rel + 262 > len(cmap):
            raise _refuse("malformed-cmap", "format 0 subtable is truncated")
        return {
            code: glyph
            for code, glyph in enumerate(struct.unpack_from(">256B", cmap, rel + 6))
            if glyph
        }
    if fmt == 6:
        if rel + 10 > len(cmap):
            raise _refuse("malformed-cmap", "format 6 subtable header is truncated")
        first = _u16(cmap, rel + 6, "cmap")
        total = _u16(cmap, rel + 8, "cmap")
        if rel + 10 + 2 * total > len(cmap):
            raise _refuse("malformed-cmap", "format 6 glyph array is truncated")
        if total == 0:
            return {}
        return {
            first + index: glyph
            for index, glyph in enumerate(struct.unpack_from(f">{total}H", cmap, rel + 10))
            if glyph and first + index <= MAX_CODE
        }
    if fmt == 4:
        return _parse_cmap4(cmap, rel)
    return _parse_cmap12(cmap, rel)


def _parse_cmap4(cmap: bytes, rel: int) -> dict[int, int]:
    if rel + 14 > len(cmap):
        raise _refuse("malformed-cmap", "format 4 header is truncated")
    length = _u16(cmap, rel + 2, "cmap")
    if length < 16 or rel + length > len(cmap):
        raise _refuse("malformed-cmap", f"format 4 declares length {length} beyond the table")
    seg_x2 = _u16(cmap, rel + 6, "cmap")
    if seg_x2 == 0 or seg_x2 % 2:
        raise _refuse("malformed-cmap", f"format 4 segCountX2 {seg_x2} is zero or odd")
    segments = seg_x2 // 2
    if 16 + 4 * seg_x2 > length:
        raise _refuse("malformed-cmap", f"format 4 segment arrays need {16 + 4 * seg_x2} bytes of {length}")
    ends = struct.unpack_from(f">{segments}H", cmap, rel + 14)
    starts = struct.unpack_from(f">{segments}H", cmap, rel + 16 + seg_x2)
    deltas = struct.unpack_from(f">{segments}h", cmap, rel + 16 + 2 * seg_x2)
    ranges_base = rel + 16 + 3 * seg_x2
    ranges = struct.unpack_from(f">{segments}H", cmap, ranges_base)
    mapping: dict[int, int] = {}
    for index in range(segments):
        start, end = starts[index], ends[index]
        if start > end:
            continue
        # U+FFFF is a noncharacter and the format 4 terminator segment ends there, so it is never mapped.
        for code in range(start, min(end, MAX_CODE - 1) + 1):
            if ranges[index] == 0:
                glyph = (code + deltas[index]) & 0xFFFF
            else:
                address = ranges_base + 2 * index + ranges[index] + 2 * (code - start)
                if address + 2 > rel + length:
                    raise _refuse("malformed-cmap", "format 4 glyphIdArray index leaves the subtable")
                glyph = struct.unpack_from(">H", cmap, address)[0]
                if glyph:
                    glyph = (glyph + deltas[index]) & 0xFFFF
            if glyph:
                mapping[code] = glyph
    return mapping


def _parse_cmap12(cmap: bytes, rel: int) -> dict[int, int]:
    if rel + 16 > len(cmap):
        raise _refuse("malformed-cmap", "format 12 header is truncated")
    length = _u32(cmap, rel + 4, "cmap")
    if length < 16 or rel + length > len(cmap):
        raise _refuse("malformed-cmap", f"format 12 declares length {length} beyond the table")
    groups = _u32(cmap, rel + 12, "cmap")
    if 16 + 12 * groups > length:
        raise _refuse("malformed-cmap", "format 12 group array is truncated")
    mapping: dict[int, int] = {}
    for index in range(groups):
        offset = rel + 16 + 12 * index
        start, end, first = struct.unpack_from(">III", cmap, offset)
        if start > end:
            raise _refuse("malformed-cmap", f"format 12 group {index} starts after it ends")
        if end > 0x10FFFF:
            raise _refuse("malformed-cmap", f"format 12 group {index} ends beyond U+10FFFF")
        for code in range(start, min(end, MAX_CODE) + 1):
            glyph = first + (code - start)
            if glyph > MAX_CODE:
                raise _refuse("malformed-cmap", f"format 12 group {index} yields glyph {glyph} beyond 16 bits")
            if glyph:
                mapping[code] = glyph
    return mapping


def _parse_simple_glyph(glyf: bytes, start: int, end: int, contours: int, gid: int) -> list[list[tuple]]:
    if contours == 0:
        return []
    pos = start + 10
    if pos + 2 * contours + 2 > end:
        raise _refuse("malformed-glyph", f"simple glyph {gid} is truncated in its contour ends")
    ends = struct.unpack_from(f">{contours}H", glyf, pos)
    pos += 2 * contours
    # Every TrueType contour holds at least one point, so end indices strictly increase.
    previous = -1
    for last in ends:
        if last <= previous:
            raise _refuse(
                "malformed-glyph",
                f"simple glyph {gid} endPts are not strictly increasing ({previous} then {last})",
            )
        previous = last
    point_total = ends[-1] + 1
    instructions = _u16(glyf, pos, "glyf")
    pos += 2 + instructions
    if pos > end:
        raise _refuse("malformed-glyph", f"simple glyph {gid} is truncated in its instructions")

    flags: list[int] = []
    while len(flags) < point_total:
        if pos >= end:
            raise _refuse("malformed-glyph", f"simple glyph {gid} is truncated in its point flags")
        flag = glyf[pos]
        pos += 1
        flags.append(flag)
        if flag & _REPEAT:
            if pos >= end:
                raise _refuse("malformed-glyph", f"simple glyph {gid} is truncated in a flag repeat")
            repeat = glyf[pos]
            pos += 1
            if len(flags) + repeat > point_total:
                raise _refuse("malformed-glyph", f"simple glyph {gid} repeats flags past the point count")
            flags.extend([flag] * repeat)

    xs: list[int] = []
    for flag in flags:
        if flag & _X_SHORT:
            if pos >= end:
                raise _refuse("malformed-glyph", f"simple glyph {gid} is truncated in x coordinates")
            value = glyf[pos]
            pos += 1
            xs.append(value if flag & _X_SAME else -value)
        elif flag & _X_SAME:
            xs.append(0)
        else:
            if pos + 2 > end:
                raise _refuse("malformed-glyph", f"simple glyph {gid} is truncated in x coordinates")
            xs.append(_s16(glyf, pos, "glyf"))
            pos += 2

    ys: list[int] = []
    for flag in flags:
        if flag & _Y_SHORT:
            if pos >= end:
                raise _refuse("malformed-glyph", f"simple glyph {gid} is truncated in y coordinates")
            value = glyf[pos]
            pos += 1
            ys.append(value if flag & _Y_SAME else -value)
        elif flag & _Y_SAME:
            ys.append(0)
        else:
            if pos + 2 > end:
                raise _refuse("malformed-glyph", f"simple glyph {gid} is truncated in y coordinates")
            ys.append(_s16(glyf, pos, "glyf"))
            pos += 2

    points: list[tuple[int, int, bool]] = []
    x = y = 0
    for dx, dy, flag in zip(xs, ys, flags, strict=True):
        x += dx
        y += dy
        points.append((x, y, bool(flag & _ON_CURVE)))

    out: list[list[tuple]] = []
    first = 0
    for last in ends:
        if last >= len(points):
            raise _refuse("malformed-glyph", f"simple glyph {gid} endPts entry {last} exceeds the point count")
        out.append(points[first : last + 1])
        first = last + 1
    # The final end index must name the final point.  The flag loop already sizes
    # points to ends[-1] + 1, so this is a fail-closed invariant, not a new input rule.
    if first != len(points):
        raise _refuse("malformed-glyph", f"simple glyph {gid} contours cover {first} of {len(points)} points")
    return out


# ---------------------------------------------------------------------------
# Outlines: font units -> 26.6, exact rational flattening -> integer edges
# ---------------------------------------------------------------------------


def _to26_6(value: int | Fraction, ppem: int, units_per_em: int) -> int:
    """Scale font units to 26.6 pixels: floor(value * ppem * 64 / upem + 1/2).

    Ties round toward positive infinity.  The arithmetic is exact rational
    arithmetic rounded exactly once, so every host computes the same integer.
    """
    rational = value if isinstance(value, Fraction) else Fraction(value, 1)
    return math.floor(rational * ppem * 64 / units_per_em + Fraction(1, 2))


def _midpoint(a: tuple, b: tuple) -> tuple[Fraction, Fraction]:
    """Exact midpoint of two points: ints or Fractions in, Fractions out, never floats."""
    return Fraction(a[0] + b[0], 2), Fraction(a[1] + b[1], 2)


def _round26_6(value: int | Fraction) -> int:
    """Round one exact 26.6 coordinate to an integer: floor(value + 1/2), ties toward +infinity.

    Applied once to each flattened vertex, after all rational arithmetic.  A float
    here is a programming error, so it is rejected rather than rounded.
    """
    if isinstance(value, int):
        return value
    if not isinstance(value, Fraction):
        raise TypeError(f"26.6 coordinate must be an int or Fraction, not {type(value).__name__}")
    return math.floor(value + Fraction(1, 2))


def _expand_contour(points: list[tuple]) -> list[tuple]:
    """Expand one contour into line/quad segments, adding implied midpoints.

    Segments are ``("L", a, b)`` or ``("Q", a, control, b)`` in exact 26.6
    coordinates (ints or Fractions).  The first on-curve point anchors the
    contour; consecutive off-curve points imply an on-curve midpoint; a fully
    off-curve contour starts at the midpoint of its last and first points.
    """
    if len(points) < 2:
        return []
    on_curve = [index for index, point in enumerate(points) if point[2]]
    if on_curve:
        pivot = on_curve[0]
        points = points[pivot:] + points[:pivot]
    else:
        first, last = points[0], points[-1]
        mid = _midpoint(first, last)
        points = [(mid[0], mid[1], True), *points]

    segments: list[tuple] = []
    cursor = (points[0][0], points[0][1])
    start = cursor
    pending: tuple | None = None
    for x, y, on in points[1:] + [points[0]]:
        if on:
            if pending is None:
                if (x, y) != cursor:
                    segments.append(("L", cursor, (x, y)))
            else:
                segments.append(("Q", cursor, pending, (x, y)))
            cursor = (x, y)
            pending = None
        else:
            control = (x, y)
            if pending is not None:
                implied = _midpoint(pending, control)
                segments.append(("Q", cursor, pending, implied))
                cursor = implied
            pending = control
    if pending is not None:
        segments.append(("Q", cursor, pending, start))
    elif cursor != start:
        segments.append(("L", cursor, start))
    return segments


def _flatten_quad(a: tuple, control: tuple, b: tuple, depth: int, out: list[tuple]) -> None:
    """Append the flattened points of one quadratic (excluding ``a``) to ``out``.

    Exact rational de Casteljau halving: the second difference shrinks 4x per
    level, so recursion terminates at the flatness threshold or the hard depth
    bound, whichever comes first.  No floating point participates.
    """
    second_x = a[0] - 2 * control[0] + b[0]
    second_y = a[1] - 2 * control[1] + b[1]
    if depth >= MAX_CURVE_DEPTH or abs(second_x) + abs(second_y) <= FLATNESS:
        out.append(b)
        return
    left_control = _midpoint(a, control)
    right_control = _midpoint(control, b)
    middle = _midpoint(left_control, right_control)
    _flatten_quad(a, left_control, middle, depth + 1, out)
    _flatten_quad(middle, right_control, b, depth + 1, out)


@dataclass(frozen=True)
class _Raster:
    """One planned glyph: pixel box ``(left, bottom, right, top)`` and edges."""

    edges: tuple[tuple[int, int, int, int], ...]
    left: int
    bottom: int
    right: int
    top: int

    @property
    def width(self) -> int:
        return self.right - self.left

    @property
    def height(self) -> int:
        return self.top - self.bottom


_EMPTY_RASTER = _Raster(edges=(), left=0, bottom=0, right=0, top=0)


def _plan_raster(contours: list[list[tuple]], ppem: int, units_per_em: int) -> _Raster:
    """Scale, flatten, and bound one outline at the given size.

    Scaling and flattening stay exact; each flattened vertex is then rounded once
    by ``_round26_6`` (floor(v + 1/2), ties toward +infinity) before edges form.
    The bounding box uses the flattened-and-rounded outline, which stays within
    ``FLATNESS/8 + 1/2`` 26.6 units of the true curve -- far below the 4-unit
    offset of the first sub-sample centre -- so the grid can never clip a
    sub-sample that would have been covered (see the specification).
    """
    edges: list[tuple[int, int, int, int]] = []
    min_x = min_y = None
    max_x = max_y = None
    for contour in contours:
        scaled = [
            (_to26_6(x, ppem, units_per_em), _to26_6(y, ppem, units_per_em), on)
            for x, y, on in contour
        ]
        segments = _expand_contour(scaled)
        chain: list[tuple[int, int]] = []
        for segment in segments:
            if segment[0] == "L":
                raw_points = [segment[1], segment[2]]
            else:
                flat: list[tuple] = []
                _flatten_quad(segment[1], segment[2], segment[3], 0, flat)
                raw_points = [segment[1], *flat]
            points = [(_round26_6(x), _round26_6(y)) for x, y in raw_points]
            if chain and points and points[0] == chain[-1]:
                chain.extend(points[1:])
            else:
                chain.extend(points)
        if not chain:
            continue
        for point in chain:
            x, y = point
            min_x = x if min_x is None else min(min_x, x)
            max_x = x if max_x is None else max(max_x, x)
            min_y = y if min_y is None else min(min_y, y)
            max_y = y if max_y is None else max(max_y, y)
        for first, second in zip(chain, chain[1:], strict=False):
            if first != second:
                edges.append((first[0], first[1], second[0], second[1]))
    if min_x is None:
        return _EMPTY_RASTER
    return _Raster(
        edges=tuple(edges),
        left=min_x // 64,
        right=-((-max_x) // 64),
        bottom=min_y // 64,
        top=-((-max_y) // 64),
    )


# ---------------------------------------------------------------------------
# Rasterization: 8x8 supersampled non-zero winding, 4-bit output
# ---------------------------------------------------------------------------


def _rasterize(raster: _Raster) -> tuple[int, int, tuple[int, ...]]:
    """Rasterize one planned glyph into row-major 4-bit alpha samples.

    Returns ``(width, height, samples)``; a degenerate box or edge-free outline
    yields a zero-area glyph (empty samples), which the public reader reports
    as present-but-unmapped exactly like a space.
    """
    width, height = raster.width, raster.height
    if width <= 0 or height <= 0 or not raster.edges:
        return 0, 0, ()
    left26 = raster.left * 64
    top26 = raster.top * 64
    sub_columns = width * SUPERSAMPLE
    counts = [0] * (width * height)
    for row in range(height):
        for step in range(SUPERSAMPLE):
            # Sub-scanline centre, mirrored from the y-up outline into the
            # top-down bitmap grid: rows are 64 units, centres at +4 of each
            # SUPERSAMPLE-th slice.
            y = top26 - (row * SUPERSAMPLE + step) * SUB_PIXEL - SUB_PIXEL // 2
            crossings: list[tuple[Fraction, int]] = []
            for x0, y0, x1, y1 in raster.edges:
                if y0 == y1:
                    continue
                if y0 <= y < y1:
                    direction = 1
                elif y1 <= y < y0:
                    direction = -1
                else:
                    continue
                numer = x0 * (y1 - y0) + (y - y0) * (x1 - x0)
                den = y1 - y0
                if den < 0:
                    numer, den = -numer, -den
                crossings.append((Fraction(numer, den) - left26, direction))
            if not crossings:
                continue
            crossings.sort(key=lambda entry: (entry[0], entry[1]))
            bits = 0
            winding = 0
            for index, (position, direction) in enumerate(crossings):
                winding += direction
                if winding == 0 or index + 1 >= len(crossings):
                    continue
                nxt = crossings[index + 1][0]
                # Sub-column centres are 8*j + 4 (local 26.6); keep strictly
                # inside the active span so shared edges never double-count.
                j_min = (position - (SUB_PIXEL // 2)) // SUB_PIXEL + 1
                j_max = -((SUB_PIXEL // 2 - nxt) // SUB_PIXEL) - 1
                if j_max < j_min:
                    continue
                j_min = max(j_min, 0)
                j_max = min(j_max, sub_columns - 1)
                if j_max >= j_min:
                    bits |= ((1 << (j_max - j_min + 1)) - 1) << j_min
            for column in range(width):
                active = (bits >> (column * SUPERSAMPLE)) & 0xFF
                if active:
                    counts[row * width + column] += active.bit_count()
    scale = SUPERSAMPLE * SUPERSAMPLE
    samples = tuple((count * 15 + 32) // scale for count in counts)
    return width, height, samples


# ---------------------------------------------------------------------------
# Conversion pipeline
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Conversion:
    """Everything one successful conversion produced."""

    image: bytes
    glyphs: tuple[pgf_writer.Glyph, ...]
    manifest: dict
    ppem: int
    codes: tuple[int, int]


def parse_code_range(spec: str) -> tuple[int, int]:
    """Parse ``CODE`` or ``START-END`` (hex with ``0x`` or decimal) into a range."""
    text = spec.strip()
    if "-" in text:
        start_text, _, end_text = text.partition("-")
    else:
        start_text = end_text = text
    try:
        start = int(start_text.strip(), 0)
        end = int(end_text.strip(), 0)
    except ValueError as exc:
        raise _refuse("bad-code-range", f"{spec!r} is not a code or START-END range") from exc
    if not 0 <= start <= MAX_CODE or not 0 <= end <= MAX_CODE:
        raise _refuse("bad-code-range", f"{spec!r} leaves the 16-bit character space")
    if start > end:
        raise _refuse("bad-code-range", f"{spec!r} starts after it ends")
    return start, end


@dataclass(frozen=True)
class CodePointSet:
    """A strictly ascending code-point list read from a ``--codepoints`` file."""

    codes: tuple[int, ...]
    sha256: str
    #: The file's leaf name only; the manifest never records a host path.
    source: str


def load_code_points(path: Path) -> CodePointSet:
    """Read one code point per line (``0x41``, ``U+0041`` or decimal); ``#`` starts a comment.

    The ``0x`` and ``U+`` prefixes and the hex digits are accepted in either case.

    The list must be strictly ascending, so a set has exactly one reading and the
    output is independent of how the file was written.
    """
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise _refuse("bad-codepoint-list", f"cannot read {path.name}: {exc}") from exc
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError as exc:
        raise _refuse("bad-codepoint-list", f"{path.name} is not plain ASCII text") from exc
    codes: list[int] = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        token = line.upper()
        try:
            if token.startswith("U+") or token.startswith("0X"):
                value = int(token[2:], 16)
            else:
                value = int(token, 10)
        except ValueError as exc:
            raise _refuse(
                "bad-codepoint-list", f"{path.name} line {number}: {line!r} is not a code point"
            ) from exc
        if not 0 <= value <= MAX_CODE:
            raise _refuse(
                "bad-codepoint-list", f"{path.name} line {number}: {line!r} leaves the 16-bit character space"
            )
        if codes and value <= codes[-1]:
            raise _refuse(
                "codepoints-unsorted",
                f"{path.name} line {number}: U+{value:04X} does not follow U+{codes[-1]:04X}; "
                "list code points in ascending order without repeats",
            )
        codes.append(value)
    if not codes:
        raise _refuse("empty-codepoint-set", f"{path.name} lists no code points")
    return CodePointSet(codes=tuple(codes), sha256=hashlib.sha256(data).hexdigest(), source=path.name)


@dataclass(frozen=True)
class _MetricTargets:
    ascender_px: float
    descender_px: float
    tolerance_px: float
    ppem_min: int
    ppem_max: int


def load_metric_targets(path: Path) -> _MetricTargets:
    """Load the metric-target policy document (schema documented in the spec)."""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise _refuse("metric-targets-invalid", f"cannot read {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise _refuse("metric-targets-invalid", f"{path} is not valid JSON: {exc}") from exc
    if not isinstance(document, dict):
        raise _refuse("metric-targets-invalid", "metric targets must be a JSON object")

    def number(key: str, default: float | None = None) -> float:
        value = document.get(key, default)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise _refuse("metric-targets-invalid", f"{key} must be a number")
        return float(value)

    def integer(key: str, default: int) -> int:
        value = document.get(key, default)
        if isinstance(value, bool) or not isinstance(value, int):
            raise _refuse("metric-targets-invalid", f"{key} must be an integer")
        return value

    ascender = number("ascender_px")
    descender = number("descender_px")
    tolerance = number("tolerance_px", 1.0)
    ppem_min = integer("ppem_min", 4)
    ppem_max = integer("ppem_max", 64)
    if tolerance <= 0:
        raise _refuse("metric-targets-invalid", "tolerance_px must be positive")
    if ascender <= 0:
        raise _refuse("metric-targets-invalid", "ascender_px must be positive")
    if not 1 <= ppem_min <= ppem_max <= MAX_PPEM:
        raise _refuse(
            "metric-targets-invalid",
            f"ppem range {ppem_min}..{ppem_max} must sit inside 1..{MAX_PPEM}",
        )
    return _MetricTargets(ascender, descender, tolerance, ppem_min, ppem_max)


class _Planner:
    """Builds glyph plans and metric-target searches for one font and code list.

    The code list is ascending and may be sparse: its span can be far wider than the
    number of glyphs it names.
    """

    def __init__(self, font: _Ttf, codes: list[int] | tuple[int, ...]) -> None:
        self.font = font
        self.codes = tuple(codes)
        gids: dict[int, int] = {}
        for code in self.codes:
            gid = font.glyph_id(code)
            if gid is None:
                raise _refuse(
                    "missing-code-point",
                    f"U+{code:04X} has no glyph in the font's character map",
                )
            gids[code] = gid
        self.gids = gids
        self._rasters: dict[int, dict[int, _Raster]] = {}

    def raster_at(self, code: int, ppem: int) -> _Raster:
        per_size = self._rasters.setdefault(ppem, {})
        cached = per_size.get(code)
        if cached is None:
            contours = self.font.outline(self.gids[code])
            cached = _plan_raster(contours, ppem, self.font.units_per_em)
            per_size[code] = cached
        return cached

    def extents_at(self, ppem: int) -> tuple[int, int]:
        """Header ascender/descender in 26.6 as ``build_pgf`` would derive them.

        Mirrors ``pgf_writer._extrema``: every glyph contributes
        ``y_base = top*64`` and ``y_base - height*64``; zero-area glyphs
        contribute ``(0, 0)``.
        """
        ascender = 0
        descender = 0
        for code in self.codes:
            raster = self.raster_at(code, ppem)
            if raster.width <= 0 or raster.height <= 0:
                continue
            ascender = max(ascender, raster.top * 64)
            descender = min(descender, (raster.top - raster.height) * 64)
        return ascender, descender

    def extents_cheap(self, ppem: int) -> tuple[int, int]:
        """Predicted extents from scaled control points (an upper bound box)."""
        ascender = 0
        descender = 0
        for code in self.codes:
            contours = self.font.outline(self.gids[code])
            if not contours:
                continue
            top = 0
            bottom = 0
            seen = False
            for contour in contours:
                for _x, y, _on in contour:
                    y26 = _to26_6(y, ppem, self.font.units_per_em)
                    if not seen or y26 > top:
                        top = y26
                    if not seen or y26 < bottom:
                        bottom = y26
                    seen = True
            if not seen:
                continue
            ascender = max(ascender, -(-top // 64) * 64)
            descender = min(descender, (bottom // 64) * 64)
        return ascender, descender

    def search_ppem(self, targets: _MetricTargets) -> tuple[int, tuple[int, int]]:
        """Smallest ppem whose exact header extrema meet both targets.

        The cheap control-point bound prunes candidate sizes; only candidates
        within tolerance plus two pixels are measured exactly, so the search
        stays fast while the acceptance decision uses the same extents the
        writer will emit.
        """
        for ppem in range(targets.ppem_min, targets.ppem_max + 1):
            predicted_asc, predicted_desc = self.extents_cheap(ppem)
            slack = targets.tolerance_px + 2.0
            if abs(predicted_asc / 64 - targets.ascender_px) > slack:
                continue
            if abs(predicted_desc / 64 - targets.descender_px) > slack:
                continue
            ascender, descender = self.extents_at(ppem)
            if (
                abs(ascender / 64 - targets.ascender_px) <= targets.tolerance_px
                and abs(descender / 64 - targets.descender_px) <= targets.tolerance_px
            ):
                return ppem, (ascender, descender)
        raise _refuse(
            "metric-target-unreachable",
            f"no ppem in {targets.ppem_min}..{targets.ppem_max} lands the header within "
            f"{targets.tolerance_px}px of ascender {targets.ascender_px}px / "
            f"descender {targets.descender_px}px for this font",
        )


def convert(
    data: bytes,
    *,
    input_path: str,
    output_path: str,
    codes: tuple[int, int] | None = None,
    code_points: CodePointSet | None = None,
    ppem: int | None = None,
    metric_targets: _MetricTargets | None = None,
    font_name: str = DEFAULT_FONT_NAME,
    font_type: str = DEFAULT_FONT_TYPE,
    pin: SourcePin | None = None,
) -> Conversion:
    """Convert one pinned TrueType font into a PGF image plus its manifest.

    The code points come either from an inclusive ``codes`` range or from an explicit
    ``code_points`` set (a sparse set is written as a sparse character map). Every stage
    validates before the next runs; the returned bytes are final. Raises
    :class:`TtfConvertError` naming the refusal on any malformed, unsupported, or
    policy-violating input.
    """
    if codes is not None and code_points is not None:
        raise _refuse("conflicting-parameters", "--codes and --codepoints are mutually exclusive")
    if code_points is not None:
        code_list = list(code_points.codes)
        if not code_list:
            raise _refuse("empty-codepoint-set", "the code-point set lists no code points")
    else:
        first, last = codes if codes is not None else parse_code_range(DEFAULT_CODES)
        if first > last:
            raise _refuse("bad-code-range", f"U+{first:04X}..U+{last:04X} starts after it ends")
        code_list = list(range(first, last + 1))
    first, last = code_list[0], code_list[-1]
    if metric_targets is not None and ppem is not None:
        raise _refuse("conflicting-parameters", "--ppem and --metric-targets are mutually exclusive")
    if ppem is None and metric_targets is None:
        ppem = DEFAULT_PPEM
    if ppem is not None and not MIN_PPEM <= ppem <= MAX_PPEM:
        raise _refuse("ppem-out-of-range", f"ppem {ppem} is outside {MIN_PPEM}..{MAX_PPEM}")

    validate_output_name(font_name, pin)

    font = _Ttf(data)
    planner = _Planner(font, code_list)

    chosen_targets: dict | None = None
    if metric_targets is not None:
        ppem, extents = planner.search_ppem(metric_targets)
        chosen_targets = {
            "ascender_px": metric_targets.ascender_px,
            "descender_px": metric_targets.descender_px,
            "tolerance_px": metric_targets.tolerance_px,
            "ppem_min": metric_targets.ppem_min,
            "ppem_max": metric_targets.ppem_max,
            "achieved_ascender_26_6": extents[0],
            "achieved_descender_26_6": extents[1],
        }
    assert ppem is not None

    glyphs: list[pgf_writer.Glyph] = []
    zero_area: list[int] = []
    max_width = max_height = 0
    for code in planner.codes:
        raster = planner.raster_at(code, ppem)
        if raster.width > pgf_writer.MAX_EDGE or raster.height > pgf_writer.MAX_EDGE:
            raise _refuse(
                "glyph-too-large",
                f"U+{code:04X} rasterizes to {raster.width}x{raster.height} at ppem {ppem}; "
                f"the record field holds at most {pgf_writer.MAX_EDGE}",
            )
        advance_x = _to26_6(font.advance_units(planner.gids[code]), ppem, font.units_per_em)
        if raster.width <= 0 or raster.height <= 0:
            zero_area.append(code)
            glyphs.append(pgf_writer.Glyph(code=code, width=0, height=0, samples=(), advance_x=advance_x))
            continue
        width, height, samples = _rasterize(raster)
        max_width = max(max_width, width)
        max_height = max(max_height, height)
        glyphs.append(
            pgf_writer.Glyph(
                code=code,
                width=width,
                height=height,
                samples=samples,
                adjust_x=0,
                adjust_y=0,
                advance_x=advance_x,
                advance_y=0,
                dimension_width=width * pgf_writer.POINTS,
                dimension_height=height * pgf_writer.POINTS,
                x_left=raster.left * pgf_writer.POINTS,
                y_base=raster.top * pgf_writer.POINTS,
            )
        )

    try:
        image = pgf_writer.build_pgf(
            glyphs,
            font_name=font_name,
            font_type=font_type,
            nominal_em_26_6=ppem * 64,
        )
    except pgf_writer.PgfWriteError as exc:
        prefix = f"{exc.reason}: "
        detail = str(exc)[len(prefix) :] if str(exc).startswith(prefix) else str(exc)
        raise TtfConvertError(exc.reason, detail) from exc

    manifest = _build_manifest(
        data=data,
        image=image,
        font=font,
        planner=planner,
        codes=(first, last),
        code_points=code_points,
        ppem=ppem,
        glyphs=glyphs,
        zero_area=zero_area,
        max_width=max_width,
        max_height=max_height,
        font_name=font_name,
        font_type=font_type,
        input_path=input_path,
        output_path=output_path,
        pin=pin,
        metric_targets=chosen_targets,
    )
    return Conversion(
        image=image,
        glyphs=tuple(glyphs),
        manifest=manifest,
        ppem=ppem,
        codes=(first, last),
    )


def _read_header_fields(image: bytes) -> dict:
    """Read back the identity and extrema the writer actually emitted."""
    return {
        "revision": struct.unpack_from("<i", image, 0x08)[0],
        "first_glyph": struct.unpack_from("<H", image, 0xB6)[0],
        "last_glyph": struct.unpack_from("<H", image, 0xB8)[0],
        "horizontal_size_26_6": struct.unpack_from("<i", image, 0x24)[0],
        "vertical_size_26_6": struct.unpack_from("<i", image, 0x28)[0],
        "ascender_26_6": struct.unpack_from("<i", image, 0xD4)[0],
        "descender_26_6": struct.unpack_from("<i", image, 0xD8)[0],
        "max_width_26_6": struct.unpack_from("<i", image, 0xF4)[0],
        "max_height_26_6": struct.unpack_from("<i", image, 0xF8)[0],
        "max_advance_26_6": struct.unpack_from("<i", image, 0xEC)[0],
    }


def _build_manifest(
    *,
    data: bytes,
    image: bytes,
    font: _Ttf,
    planner: _Planner,
    codes: tuple[int, int],
    code_points: CodePointSet | None,
    ppem: int,
    glyphs: list[pgf_writer.Glyph],
    zero_area: list[int],
    max_width: int,
    max_height: int,
    font_name: str,
    font_type: str,
    input_path: str,
    output_path: str,
    pin: SourcePin | None,
    metric_targets: dict | None,
) -> dict:
    first, last = codes
    header = _read_header_fields(image)
    license_block = None
    if pin is not None:
        license_block = {
            "spdx": pin.spdx,
            "copyright": pin.copyright,
            "reserved_font_names": list(pin.reserved_font_names),
            "license_text_file": pin.license_text_file,
            "license_text_sha256": pin.license_text_sha256,
            "license_text": pin.license_text,
            "source": dict(pin.source),
            "family": pin.family,
        }
    return {
        "converter": {
            "name": CONVERTER_NAME,
            "version": CONVERTER_VERSION,
            "spec": SPEC_PATH,
        },
        "coverage": {
            "first": first,
            "last": last,
            "count": len(glyphs),
            "span": last - first + 1,
            "range": f"U+{first:04X}..U+{last:04X}",
        },
        "code_points": None
        if code_points is None
        else {
            "count": len(code_points.codes),
            "path": code_points.source,
            "sha256": code_points.sha256,
        },
        "glyphs": {
            "count": len(glyphs),
            "zero_area_codes": zero_area,
            "max_width_px": max_width,
            "max_height_px": max_height,
        },
        "input": {
            "path": _leaf_name(input_path),
            "sha256": hashlib.sha256(data).hexdigest(),
            "size_bytes": len(data),
            "units_per_em": font.units_per_em,
            "num_glyphs": font.num_glyphs,
            "outline_format": "glyf",
            "tables_used": list(REQUIRED_TABLES),
        },
        "license": license_block,
        "metric_targets": metric_targets,
        "output": {
            "path": _leaf_name(output_path),
            "sha256": hashlib.sha256(image).hexdigest(),
            "size_bytes": len(image),
            "font_name": font_name,
            "font_type": font_type,
            **header,
        },
        "parameters": {
            "ppem": ppem,
            "nominal_em_26_6": ppem * 64,
            "supersample": SUPERSAMPLE,
            "coverage_bits": 4,
            "rounding": "scale: floor(x * ppem * 64 / upem + 1/2); coverage: floor((n * 15 + 32) / 64)",
            "hinting": "none",
            "advance_rounding": "26.6, half-up from font units",
        },
    }


def _leaf_name(path: str) -> str:
    """The final path component under either host separator.

    The manifest records file names only, so a build run from another directory, or on another
    host, writes the same bytes (plan 6.2).
    """
    return re.split(r"[\\/]", path)[-1]


def manifest_bytes(manifest: dict) -> bytes:
    """Canonical manifest JSON: sorted keys, 2-space indent, trailing newline."""
    return (json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=True) + "\n").encode("utf-8")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Convert a pinned TrueType-outline font into a deterministic PGF fixture (issue #313)."
    )
    parser.add_argument("output", type=Path, help="destination .pgf path")
    parser.add_argument("input", type=Path, help="source .ttf path")
    parser.add_argument("--pin", type=Path, help="font-pin/v1 JSON binding the input digest and licence")
    parser.add_argument("--manifest", type=Path, help="conversion manifest output (requires --pin)")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--codes", default=None, help="code or START-END range (default 0x20-0x7E)")
    source.add_argument(
        "--codepoints",
        type=Path,
        help="file listing code points, one per line, strictly ascending (sparse output)",
    )
    size = parser.add_mutually_exclusive_group()
    size.add_argument("--ppem", type=int, help=f"pixel size of the em (default {DEFAULT_PPEM})")
    size.add_argument("--metric-targets", type=Path, help="metric-target policy JSON; selects the ppem")
    parser.add_argument("--font-name", default=DEFAULT_FONT_NAME, help="honest PGF fontName")
    parser.add_argument("--font-type", default=DEFAULT_FONT_TYPE, help="PGF fontType (style)")
    args = parser.parse_args(argv)

    if args.manifest is not None and args.pin is None:
        parser.error("--manifest requires --pin so the manifest can carry attribution and licence material")

    try:
        code_points = load_code_points(args.codepoints) if args.codepoints is not None else None
        codes = None if code_points is not None else parse_code_range(args.codes or DEFAULT_CODES)
        data = args.input.read_bytes()
        pin = load_pin(args.pin, data) if args.pin is not None else None
        targets = load_metric_targets(args.metric_targets) if args.metric_targets is not None else None
        conversion = convert(
            data,
            input_path=str(args.input),
            output_path=str(args.output),
            codes=codes,
            code_points=code_points,
            ppem=args.ppem,
            metric_targets=targets,
            font_name=args.font_name,
            font_type=args.font_type,
            pin=pin,
        )
        manifest = manifest_bytes(conversion.manifest)
    except TtfConvertError as exc:
        print(f"{CONVERTER_NAME}: refused: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"{CONVERTER_NAME}: unreadable input: {exc}", file=sys.stderr)
        return 2

    try:
        args.output.write_bytes(conversion.image)
        if args.manifest is not None:
            try:
                args.manifest.write_bytes(manifest)
            except OSError:
                args.output.unlink(missing_ok=True)
                raise
    except OSError as exc:
        print(f"{CONVERTER_NAME}: cannot write output: {exc}", file=sys.stderr)
        return 1
    digest = hashlib.sha256(conversion.image).hexdigest()
    print(f"{args.output} {len(conversion.image)} bytes sha256={digest}")
    if args.manifest is not None:
        print(f"{args.manifest} {len(manifest)} bytes sha256={hashlib.sha256(manifest).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
