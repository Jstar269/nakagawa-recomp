#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Deterministic PGF writer for project-generated glyph bitmaps (issue #313).

The writer takes an in-memory glyph set -- code point, 4-bit alpha samples, and
26.6 metrics -- and emits the PSP PGF container described in
``docs/cleanroom/PGF_SPEC.md``, so the public reader ``src/rt/pgf_public.c``
can open the result through its published ABI.  It reads no font file: the
shapes come from the caller, so its output is a fixture and never an
authenticity claim (issue #313, and the "Generated PGF fixtures" boundary in
PGF_SPEC.md section 4).

Determinism is a hard property.  Glyph order, metric-table order, packed
section widths, RLE choices, and every reserved byte are functions of the input
values alone; nothing here reads the clock, the host environment, the locale, or
a random source.  Two runs of the same glyph set therefore produce identical
bytes, which is what makes the generated fixtures reproducible.

Malformed input is refused by name through :class:`PgfWriteError`.  The writer
never clamps a value, truncates a bitmap, drops a duplicate, or substitutes a
default in place of an out-of-range metric.

The emitted revision is 2 with a 392-byte base header: the documented smallest
section directory.  Revision 3 and its 412-byte extension exist to describe
subcharmap tables whose reference semantics are still unnamed (PGF_SPEC.md
sections 3.3 and 3.7, open question O-20), so a writer that does not know those
references must not claim them.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from dataclasses import dataclass, fields
from pathlib import Path

MAGIC = b"PGF0"
HEADER_SIZE = 392
REVISION = 2
VERSION = 0
SOURCE_BITS_PER_PIXEL = 4
DEFAULT_RESOLUTION = 1920
DEFAULT_FONT_NAME = "Nakagawa Synthetic"
DEFAULT_FONT_TYPE = "SYNTHETIC-FIXTURE"

MAX_IMAGE_SIZE = 16 * 1024 * 1024
MAX_COUNT = 1 << 20
MAX_EDGE = 127
#: The header stores each deduplicated metric table's entry count in one byte
#: (0x0102..0x0105), so a table holds at most 255 entries.
MAX_TABLE_ENTRIES = 255
MAX_REPEAT_RUN = 8
MAX_LITERAL_RUN = 8
POINTS = 64
INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1
ADJUST_MIN = -64
ADJUST_MAX = 63
SUPPORTED_ROW_ORDERS = (1, 2)

#: PSP firmware family names. A project-generated font must never claim one.
RESERVED_FONT_NAMES = frozenset({"jpn0", "kr0", *(f"ltn{index}" for index in range(16))})

TABLE_COUNT = 4
#: Metric groups in record order: dimension, X adjustment, Y adjustment, advance.
TABLE_COLUMNS = (
    ("dimension_width", "dimension_height"),
    ("x_left", "x_center"),
    ("y_base", "y_top"),
    ("advance_x", "advance_y"),
)


class PgfWriteError(ValueError):
    """A named refusal from the deterministic PGF writer.

    :attr:`reason` is the stable reason name asserted by the writer's tests and
    reported by any caller that has to say *why* a glyph set was rejected; the
    rest of the message is human context.
    """

    def __init__(self, reason: str, detail: str) -> None:
        super().__init__(f"{reason}: {detail}")
        self.reason = reason


def _refuse(reason: str, detail: str) -> PgfWriteError:
    return PgfWriteError(reason, detail)


@dataclass(frozen=True)
class Glyph:
    """One raster glyph: a code point, its samples, and its 26.6 metrics.

    ``samples`` holds ``width * height`` alpha nibbles in the order
    ``row_order`` selects: raster order for row order 1, column-major for row
    order 2.  Every metric is a signed 26.6 fixed-point value except
    ``adjust_x`` and ``adjust_y``, which are the record's signed 7-bit pixel
    adjustments.  A ``None`` metric is derived from the shape itself, never
    invented; see :func:`_resolve`.
    """

    code: int
    width: int
    height: int
    samples: tuple[int, ...]
    row_order: int = 1
    adjust_x: int = 0
    adjust_y: int = 0
    advance_x: int | None = None
    advance_y: int | None = None
    dimension_width: int | None = None
    dimension_height: int | None = None
    x_left: int | None = None
    x_center: int | None = None
    y_base: int | None = None
    y_top: int | None = None


@dataclass(frozen=True)
class _Planned:
    """A validated glyph with every derived value resolved."""

    code: int
    width: int
    height: int
    row_order: int
    adjust_x: int
    adjust_y: int
    advance_x: int
    advance_y: int
    dimension_width: int
    dimension_height: int
    x_left: int
    x_center: int
    y_base: int
    y_top: int
    bitmap: bytes


def _run_length(samples: tuple[int, ...], index: int, limit: int) -> int:
    length = 1
    while length < limit and index + length < len(samples) and samples[index + length] == samples[index]:
        length += 1
    return length


def encode_bitmap(samples: tuple[int, ...]) -> bytes:
    """Encode alpha samples as the format's 4-bit RLE stream, low nibble first.

    A control nibble of 0-7 repeats the following sample ``control + 1`` times; a
    control nibble of 8-15 emits the following ``16 - control`` samples
    literally.  Both forms are chosen greedily from the remaining samples only,
    so the result is a pure function of the input and never depends on how the
    glyph was assembled.
    """
    nibbles: list[int] = []
    index = 0
    while index < len(samples):
        run = _run_length(samples, index, MAX_REPEAT_RUN)
        if run >= 3:
            nibbles.append(run - 1)
            nibbles.append(samples[index])
            index += run
            continue
        literal: list[int] = []
        while index < len(samples) and len(literal) < MAX_LITERAL_RUN:
            if _run_length(samples, index, MAX_REPEAT_RUN) >= 3:
                break
            literal.append(samples[index])
            index += 1
        nibbles.append(8 + (MAX_LITERAL_RUN - len(literal)))
        nibbles.extend(literal)
    packed = bytearray((len(nibbles) + 1) // 2)
    for position, nibble in enumerate(nibbles):
        if position % 2 == 0:
            packed[position // 2] |= nibble & 0x0F
        else:
            packed[position // 2] |= (nibble & 0x0F) << 4
    return bytes(packed)


def _pack_fields(entries: list[tuple[int, int]]) -> bytes:
    """Pack ``(value, width)`` pairs least-significant bit first."""
    out = bytearray((sum(width for _, width in entries) + 7) // 8)
    bit = 0
    for value, width in entries:
        for offset in range(width):
            if (value >> offset) & 1:
                index = (bit + offset) // 8
                out[index] |= 1 << ((bit + offset) % 8)
        bit += width
    return bytes(out)


def _packed_section(values: list[int], bits: int) -> bytes:
    """Write ``values`` as a low-bit-first packed array rounded to whole 32-bit words."""
    entries = [(value, bits) for value in values]
    data = _pack_fields(entries)
    words = (len(data) + 3) // 4
    return data + bytes(words * 4 - len(data))


def _entry_width(max_value: int, *, sentinel_reserved: bool) -> int:
    """Smallest packed entry width holding ``max_value``, plus a free value when asked.

    The character map reserves an all-ones absence sentinel, so its width must
    also exceed every glyph identifier it carries; the pointer table reserves
    nothing, because an all-ones pointer is an ordinary offset.
    """
    width = 1
    while (1 << width) - 1 < max_value + (1 if sentinel_reserved else 0):
        width += 1
    return width


def _resolve(glyph: Glyph) -> _Planned:
    """Validate one glyph and resolve its ``None`` metrics from its own shape."""
    code = glyph.code
    if not isinstance(code, int) or isinstance(code, bool) or not 0 <= code <= 0xFFFF:
        raise _refuse("code-point-out-of-range", f"{code!r} is not a 16-bit character code")
    if not isinstance(glyph.width, int) or not isinstance(glyph.height, int):
        raise _refuse("glyph-too-large", f"U+{code:04X} has non-integer dimensions")
    if not 0 <= glyph.width <= MAX_EDGE or not 0 <= glyph.height <= MAX_EDGE:
        raise _refuse(
            "glyph-too-large",
            f"U+{code:04X} is {glyph.width}x{glyph.height}; the record field is 7 bits",
        )
    if glyph.row_order not in SUPPORTED_ROW_ORDERS:
        raise _refuse(
            "unsupported-row-order",
            f"U+{code:04X} declares row order {glyph.row_order}; only 1 and 2 are writable",
        )
    samples = tuple(glyph.samples)
    if len(samples) != glyph.width * glyph.height:
        raise _refuse(
            "sample-count-mismatch",
            f"U+{code:04X} declares {glyph.width}x{glyph.height} but carries {len(samples)} samples",
        )
    for sample in samples:
        if not isinstance(sample, int) or not 0 <= sample <= 15:
            raise _refuse("sample-out-of-range", f"U+{code:04X} carries sample {sample!r}")
    if not ADJUST_MIN <= glyph.adjust_x <= ADJUST_MAX or not ADJUST_MIN <= glyph.adjust_y <= ADJUST_MAX:
        raise _refuse(
            "metric-out-of-range",
            f"U+{code:04X} adjustment ({glyph.adjust_x},{glyph.adjust_y}) leaves the signed 7-bit range",
        )
    dimension_width = glyph.width * POINTS if glyph.dimension_width is None else glyph.dimension_width
    dimension_height = glyph.height * POINTS if glyph.dimension_height is None else glyph.dimension_height
    advance_x = glyph.width * POINTS if glyph.advance_x is None else glyph.advance_x
    advance_y = 0 if glyph.advance_y is None else glyph.advance_y
    x_left = glyph.adjust_x * POINTS if glyph.x_left is None else glyph.x_left
    x_center = x_left + advance_x // 2 if glyph.x_center is None else glyph.x_center
    y_base = 0 if glyph.y_base is None else glyph.y_base
    y_top = y_base + dimension_height if glyph.y_top is None else glyph.y_top
    metrics = {
        "dimension_width": dimension_width,
        "dimension_height": dimension_height,
        "advance_x": advance_x,
        "advance_y": advance_y,
        "x_left": x_left,
        "x_center": x_center,
        "y_base": y_base,
        "y_top": y_top,
    }
    for name, value in metrics.items():
        if not INT32_MIN <= value <= INT32_MAX:
            raise _refuse("metric-out-of-range", f"U+{code:04X} {name} {value} is not a signed 32-bit value")
    descender = y_base - dimension_height
    if not INT32_MIN <= descender <= INT32_MAX:
        raise _refuse("metric-out-of-range", f"U+{code:04X} derived descender {descender} is not representable")
    return _Planned(
        code=code,
        width=glyph.width,
        height=glyph.height,
        row_order=glyph.row_order,
        adjust_x=glyph.adjust_x,
        adjust_y=glyph.adjust_y,
        advance_x=advance_x,
        advance_y=advance_y,
        dimension_width=dimension_width,
        dimension_height=dimension_height,
        x_left=x_left,
        x_center=x_center,
        y_base=y_base,
        y_top=y_top,
        bitmap=encode_bitmap(samples),
    )


def _font_field(value: object, label: str) -> bytes:
    if not isinstance(value, str):
        raise _refuse("font-field-invalid", f"{label} must be a string")
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as exc:
        raise _refuse("font-field-invalid", f"{label} must be ASCII: {exc}") from exc
    if not 1 <= len(encoded) <= 64:
        raise _refuse("font-field-invalid", f"{label} must be 1..64 ASCII bytes")
    return encoded


def _validate_font_name(value: object) -> bytes:
    encoded = _font_field(value, "font name")
    stem = value.lower() if isinstance(value, str) else ""
    if stem.endswith(".pgf"):
        stem = stem[: -len(".pgf")]
    if stem in RESERVED_FONT_NAMES:
        raise _refuse("font-field-invalid", f"{value!r} is a PSP firmware font name and must not be claimed")
    return encoded


def _build_metric_tables(plans: list[_Planned]) -> tuple[list[bytes], list[list[int]]]:
    """Deduplicate the four metric tables; return their bytes and per-glyph indexes."""
    names = ("dimension", "x_adjustment", "y_adjustment", "advance")
    tables: list[bytes] = []
    indexes: list[list[int]] = []
    for table, columns in enumerate(TABLE_COLUMNS):
        pairs = sorted({tuple(getattr(plan, column) for column in columns) for plan in plans})
        if len(pairs) > MAX_TABLE_ENTRIES:
            raise _refuse(
                "metric-table-overflow",
                f"{names[table]} table needs {len(pairs)} entries; the header count is one byte "
            f"(at most {MAX_TABLE_ENTRIES})",
            )
        position = {pair: index for index, pair in enumerate(pairs)}
        tables.append(b"".join(struct.pack("<ii", first, second) for first, second in pairs))
        indexes.append([position[tuple(getattr(plan, column) for column in columns)] for plan in plans])
    return tables, indexes


def _record_bytes(plan: _Planned, indexes: list[int]) -> bytes:
    """Build one glyph metric record: the fixed 8-byte prefix plus four table indexes."""
    prefix = _pack_fields(
        [
            (0, 14),  # shadow record offset: this writer emits no shadow record
            (plan.width, 7),
            (plan.height, 7),
            (plan.adjust_x & 0x7F, 7),
            (plan.adjust_y & 0x7F, 7),
            (plan.row_order, 2),
            (0, 1),  # role bit: reachability, not layout (PGF_SPEC.md section 3.5)
            (0b111, 3),  # all three metric groups are indexed
            (0, 7),  # unassigned bits
            (0, 9),  # shadow identifier: none
        ]
    )
    if len(prefix) != 8 or len(indexes) != TABLE_COUNT:
        raise _refuse("internal-layout", "a metric record is not the documented 8 prefix + 4 index bytes")
    return prefix + bytes(indexes)


def _extrema(plans: list[_Planned]) -> dict[str, int]:
    """Header maxima and the derived ascender/descender for the whole set."""
    return {
        "horizontal_size": max(plan.advance_x for plan in plans),
        "vertical_size": max(plan.dimension_height for plan in plans),
        "ascender": max(plan.y_base for plan in plans),
        "descender": min(plan.y_base - plan.dimension_height for plan in plans),
        "left_x": min(plan.x_left for plan in plans),
        "base_y": max(plan.y_base for plan in plans),
        "centre_x": min(plan.x_center for plan in plans),
        "top_y": max(plan.y_top for plan in plans),
        "advance_x": max(plan.advance_x for plan in plans),
        "advance_y": max(plan.advance_y for plan in plans),
        "max_width": max(plan.dimension_width for plan in plans),
        "max_height": max(plan.dimension_height for plan in plans),
        "glyph_width": max(plan.width for plan in plans),
        "glyph_height": max(plan.height for plan in plans),
    }


def build_pgf(
    glyphs: list[Glyph] | tuple[Glyph, ...],
    *,
    font_name: str = DEFAULT_FONT_NAME,
    font_type: str = DEFAULT_FONT_TYPE,
) -> bytes:
    """Write one deterministic PGF image from an in-memory glyph set.

    Glyphs are ordered by code point, so the caller's collection order cannot
    change the output.  Every rejection is a :class:`PgfWriteError` naming its
    reason.
    """
    name_bytes = _validate_font_name(font_name)
    type_bytes = _font_field(font_type, "font type")
    glyphs = list(glyphs)
    if not glyphs:
        raise _refuse("empty-glyph-set", "a PGF needs at least one glyph record")
    plans: list[_Planned] = []
    seen: set[int] = set()
    for glyph in glyphs:
        if not isinstance(glyph, Glyph):
            raise _refuse("unsupported-input", f"{type(glyph).__name__} is not a Glyph")
        if glyph.code in seen:
            raise _refuse("duplicate-code-point", f"U+{glyph.code:04X} appears more than once")
        seen.add(glyph.code)
        plans.append(_resolve(glyph))
    plans.sort(key=lambda plan: plan.code)

    first_glyph = plans[0].code
    last_glyph = plans[-1].code
    glyph_count = len(plans)
    char_map_count = last_glyph - first_glyph + 1
    if char_map_count > MAX_COUNT or glyph_count > MAX_COUNT:
        raise _refuse(
            "character-map-too-large",
            f"codes U+{first_glyph:04X}..U+{last_glyph:04X} need {char_map_count} map entries",
        )

    if glyph_count != char_map_count:
        # PGF_SPEC section 3.1: the public reader requires one glyph record per code point
        # in first..last, so a gap would produce a file that reader refuses.
        missing = next(code for code in range(first_glyph, last_glyph + 1) if code not in seen)
        raise _refuse(
            "non-contiguous-code-points",
            f"codes U+{first_glyph:04X}..U+{last_glyph:04X} need a glyph for every code point; "
            f"U+{missing:04X} is missing",
        )

    tables, table_indexes = _build_metric_tables(plans)

    glyph_data = bytearray()
    pointers: list[int] = []
    for index, plan in enumerate(plans):
        # The pointer table stores record offsets divided by four, so every record
        # must start on a four-byte boundary inside the glyph-data section.
        while len(glyph_data) % 4:
            glyph_data.append(0)
        pointers.append(len(glyph_data) // 4)
        glyph_data += _record_bytes(plan, [table_indexes[table][index] for table in range(TABLE_COUNT)])
        glyph_data += plan.bitmap

    char_map_bits = _entry_width(glyph_count - 1, sentinel_reserved=True)
    pointer_bits = _entry_width(max(pointers), sentinel_reserved=False)
    glyph_of_code = {plan.code: index for index, plan in enumerate(plans)}
    char_map = [glyph_of_code.get(first_glyph + index, (1 << char_map_bits) - 1) for index in range(char_map_count)]

    header = bytearray(HEADER_SIZE)
    struct.pack_into("<H", header, 0x00, 0)
    struct.pack_into("<H", header, 0x02, HEADER_SIZE)
    header[0x04:0x08] = MAGIC
    struct.pack_into("<i", header, 0x08, REVISION)
    struct.pack_into("<i", header, 0x0C, VERSION)
    struct.pack_into("<I", header, 0x10, char_map_count)
    struct.pack_into("<I", header, 0x14, glyph_count)
    struct.pack_into("<I", header, 0x18, char_map_bits)
    struct.pack_into("<I", header, 0x1C, pointer_bits)
    header[0x22] = SOURCE_BITS_PER_PIXEL
    header[0x35 : 0x35 + len(name_bytes)] = name_bytes
    header[0x75 : 0x75 + len(type_bytes)] = type_bytes
    struct.pack_into("<H", header, 0xB6, first_glyph)
    struct.pack_into("<H", header, 0xB8, last_glyph)

    extrema = _extrema(plans)
    for index, key in enumerate(
        ("ascender", "descender", "left_x", "base_y", "centre_x", "top_y", "advance_x", "advance_y")
    ):
        struct.pack_into("<i", header, 0xD4 + index * 4, extrema[key])
    struct.pack_into("<i", header, 0xF4, extrema["max_width"])
    struct.pack_into("<i", header, 0xF8, extrema["max_height"])
    struct.pack_into("<H", header, 0xFC, extrema["glyph_width"])
    struct.pack_into("<H", header, 0xFE, extrema["glyph_height"])
    for table in range(TABLE_COUNT):
        header[0x102 + table] = len(tables[table]) // 8
    struct.pack_into("<I", header, 0x16C, 0)  # empty shadow character map
    struct.pack_into("<I", header, 0x170, 0)
    struct.pack_into("<i", header, 0x24, extrema["horizontal_size"])
    struct.pack_into("<i", header, 0x28, extrema["vertical_size"])
    struct.pack_into("<i", header, 0x2C, DEFAULT_RESOLUTION)
    struct.pack_into("<i", header, 0x30, DEFAULT_RESOLUTION)

    image = bytes(header) + b"".join(tables) + _packed_section(char_map, char_map_bits)
    image += _packed_section(pointers, pointer_bits) + bytes(glyph_data)
    if len(image) > MAX_IMAGE_SIZE:
        raise _refuse("image-too-large", f"{len(image)} bytes exceeds the 16 MiB container ceiling")
    return image


def _optional_int(entry: dict[str, object], key: str) -> int | None:
    value = entry.get(key)
    return None if value is None else int(value)  # type: ignore[arg-type]


def _glyph_from_dict(entry: object) -> Glyph:
    """Build one glyph from a JSON glyph-set entry, naming every malformed field."""
    if not isinstance(entry, dict):
        raise _refuse("unsupported-input", "each glyph must be a JSON object")
    unknown = sorted(set(entry) - set(field.name for field in fields(Glyph)))
    if unknown:
        raise _refuse("unsupported-input", f"unknown glyph fields: {', '.join(unknown)}")
    try:
        return Glyph(
            code=int(entry["code"]),  # type: ignore[arg-type]
            width=int(entry["width"]),  # type: ignore[arg-type]
            height=int(entry["height"]),  # type: ignore[arg-type]
            samples=tuple(int(sample) for sample in entry["samples"]),  # type: ignore[arg-type]
            row_order=int(entry.get("row_order", 1)),  # type: ignore[arg-type]
            adjust_x=int(entry.get("adjust_x", 0)),  # type: ignore[arg-type]
            adjust_y=int(entry.get("adjust_y", 0)),  # type: ignore[arg-type]
            advance_x=_optional_int(entry, "advance_x"),
            advance_y=_optional_int(entry, "advance_y"),
            dimension_width=_optional_int(entry, "dimension_width"),
            dimension_height=_optional_int(entry, "dimension_height"),
            x_left=_optional_int(entry, "x_left"),
            x_center=_optional_int(entry, "x_center"),
            y_base=_optional_int(entry, "y_base"),
            y_top=_optional_int(entry, "y_top"),
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise _refuse("unsupported-input", f"glyph entry is malformed: {exc}") from exc


def _font_option(document: dict[str, object], key: str, default: str) -> str:
    if key not in document:
        return default
    value = document[key]
    if not isinstance(value, str):
        raise _refuse("unsupported-input", f"{key} must be a string")
    return value


def _main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Write a deterministic PGF from a JSON glyph set.")
    parser.add_argument("output", type=Path, help="destination .pgf path")
    parser.add_argument("glyph_set", type=Path, help="JSON object with a 'glyphs' array")
    args = parser.parse_args(argv)
    try:
        document = json.loads(args.glyph_set.read_text(encoding="utf-8"))
        if not isinstance(document, dict) or not isinstance(document.get("glyphs"), list):
            raise _refuse("unsupported-input", "glyph set must be a JSON object with a 'glyphs' array")
        image = build_pgf(
            [_glyph_from_dict(entry) for entry in document["glyphs"]],
            font_name=_font_option(document, "font_name", DEFAULT_FONT_NAME),
            font_type=_font_option(document, "font_type", DEFAULT_FONT_TYPE),
        )
    except PgfWriteError as exc:
        print(f"pgf_writer: refused: {exc}", file=sys.stderr)
        return 2
    except (OSError, json.JSONDecodeError) as exc:
        print(f"pgf_writer: unreadable glyph set: {exc}", file=sys.stderr)
        return 2
    args.output.write_bytes(image)
    print(f"{args.output} {len(image)} bytes sha256={hashlib.sha256(image).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
