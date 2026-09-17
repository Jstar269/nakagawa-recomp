# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later:
# PGF container layout knowledge comes from the public struct PGFHeader in
# PPSSPP Core/Font/PGF.h. No PPSSPP implementation code is copied here.
# Modified by Nakagawa Recomp contributors, 2026-09-17.
# See NOTICE.md for upstream lineage and modification provenance.

"""Project-owned deterministic PGF builder (scaffold).

Scope, stated plainly: this emits a structurally valid PGF *header*
(revision 2 / version 6 layout, exactly the ``struct PGFHeader`` field order
in PPSSPP ``Core/Font/PGF.h``) plus complete charmap / char-pointer tables,
followed by glyph records in a project-defined simple 4bpp row-major
encoding. The Sony record-level RLE/shadow encoding parity with PPSSPP's
``PGF.cpp`` reader is explicitly UNVERIFIED open work -- files built here are
for converter-pipeline development and must not be shipped as user fonts.

What this module does guarantee:

* Determinism: codepoints are sorted, field order is fixed, no timestamps.
  The same inputs always produce byte-identical output.
* Honest names: Sony/Fontworks camouflage names (``FTT-NewRodin``,
  ``AsiaKNHH``, ``SONY``) are refused, as is any primary font name using
  Adobe's Reserved Font Name ``Source`` (SIL OFL 1.1 section 3) -- our
  payloads are Modified Versions and must carry a project-owned name.
* Manifest: :func:`build_manifest` records input SHA-256, codepoint
  coverage, and output SHA-256 so a future payload can be pinned like
  ``src/rt/atrac3p/PROVENANCE.md`` and ``assets/vfpu/PROVENANCE.json`` do.
* Licence accounting: :func:`required_notices` states which licence texts
  must accompany a shipped payload derived from a given source family
  (OFL 1.1 for Source Han Sans; modified BSD for Ume). Licence texts live
  under ``THIRD_PARTY_LICENSES/`` only once a payload actually ships.

Glyph coverage needed by sceFont users (from ``docs/provenance/FONT_ORIGINS.md``
and contributor notes in PPSSPP #13718): JIS X 0208 level-1 kanji (2,946) for
jpn0-class payloads, KS X 1001 coverage for kr0-class, full Latin + metrics
(ascender 9.0-10.0, descender -2.0, max glyph 16x14) for ltn-class. Reaching
that coverage with raster quality is open engineering work; this scaffold
proves the container, naming, and pinning mechanics only.
"""

from __future__ import annotations

import hashlib
import json
import struct

PGF_MAGIC = b"PGF0"
PGF_REVISION = 2
PGF_VERSION = 6
HEADER_SIZE = 392

# Offset map for struct PGFHeader (packed), per PPSSPP Core/Font/PGF.h.
_OFF_HEADER_OFFSET = 0
_OFF_HEADER_SIZE = 2
_OFF_MAGIC = 4
_OFF_REVISION = 8
_OFF_VERSION = 12
_OFF_CHARMAP_LEN = 16
_OFF_CHARPTR_LEN = 20
_OFF_CHARMAP_BPE = 24
_OFF_CHARPTR_BPE = 28
_OFF_BPP = 34
_OFF_FONT_NAME = 53
_OFF_FONT_TYPE = 117
_OFF_FIRST_GLYPH = 182
_OFF_LAST_GLYPH = 184

FORBIDDEN_NAME_FRAGMENTS = ("ftt-newrodin", "asiaknhh", "sony", "sce-")


def _check_font_name(name: str) -> str:
    """Validate a project-owned PGF fontName. Raises ValueError."""
    if not name or len(name.encode("ascii", "replace")) > 63:
        raise ValueError("fontName must be 1..63 ASCII chars")
    try:
        name.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ValueError("fontName must be ASCII (PGF header field)") from exc
    lowered = name.lower()
    for frag in FORBIDDEN_NAME_FRAGMENTS:
        if frag in lowered:
            raise ValueError(
                "refusing Sony/Fontworks camouflage name %r "
                "(see docs/provenance/FONT_ORIGINS.md section 5.3)" % name
            )
    primary = lowered.split()[0] if lowered.split() else ""
    if primary in ("source", "sourcehan", "sourcehansans"):
        raise ValueError(
            "refusing Reserved Font Name 'Source' for a Modified Version "
            "(SIL OFL 1.1 section 3); pick a project-owned name"
        )
    return name


def _bits_needed(n: int) -> int:
    width = 1
    while (1 << width) <= n:
        width += 1
    return width


def _pack_bits(values: list[int], bpe: int) -> bytes:
    """Pack integers LSB-first, matching PPSSPP/pgftool get_value ordering."""
    total = len(values) * bpe
    raw = bytearray((total + 31) // 32 * 4)
    pos = 0
    for value in values:
        if value < 0 or value >= (1 << bpe):
            raise ValueError("value %d exceeds %d bits" % (value, bpe))
        for i in range(bpe):
            if (value >> i) & 1:
                raw[pos // 8] |= 1 << (pos % 8)
            pos += 1
    return bytes(raw)


def _unpack_bits(raw: bytes, count: int, bpe: int) -> list[int]:
    out: list[int] = []
    pos = 0
    for _ in range(count):
        value = 0
        for i in range(bpe):
            if (raw[pos // 8] >> (pos % 8)) & 1:
                value |= 1 << i
            pos += 1
        out.append(value)
    return out


def required_notices(source_family: str) -> list[str]:
    """Licence texts that must accompany a shipped payload. Raises KeyError."""
    table = {
        "source-han-sans": [
            "SIL Open Font License 1.1 text",
            "Adobe copyright notice with Reserved Font Name 'Source'",
        ],
        "ume": [
            "Ume-family modified-BSD licence text",
            "Ume copyright notice",
        ],
    }
    return list(table[source_family])


def build_pgf(
    glyphs: dict[int, tuple[int, int, list[bytes]]],
    *,
    font_name: str,
    font_type: str = "Regular",
    first_glyph: int = 32,
) -> bytes:
    """Build a scaffold PGF.

    ``glyphs`` maps codepoint -> (width, height, rows) where each row is
    ``bytes`` of packed 4bpp pixels (2 pixels per byte, natural order),
    ``len(rows) == height`` and each row holds ``ceil(width / 2)`` bytes.
    Returns the complete file bytes (header + tables + records).
    """
    name = _check_font_name(font_name)
    if not glyphs:
        raise ValueError("need at least one glyph")
    codes = sorted(glyphs)
    last_glyph = max(codes)
    if min(codes) < 0 or last_glyph > 0xFFFF:
        raise ValueError("codepoints must fit in 16 bits")

    glyph_ids = {code: i for i, code in enumerate(codes)}
    charmap_len = last_glyph + 1
    # Unmapped slots point at glyph id 0 (the .notdef-equivalent slot).
    charmap = [glyph_ids.get(c, 0) for c in range(charmap_len)]
    charmap_bpe = _bits_needed(len(codes) - 1) if len(codes) > 1 else 1

    records: list[bytes] = []
    offsets: list[int] = []
    pos = 0
    for code in codes:
        width, height, rows = glyphs[code]
        if height != len(rows):
            raise ValueError("glyph U+%04X: height/row count mismatch" % code)
        stride = (width + 1) // 2
        for row in rows:
            if len(row) != stride:
                raise ValueError("glyph U+%04X: bad row stride" % code)
        body = b"".join(rows)
        # Simple record: u16 width, u16 height, then 4bpp row-major bytes.
        # NOT the Sony RLE record layout -- see module docstring.
        records.append(struct.pack("<HH", width, height) + body)
        offsets.append(pos)
        pos += 4 + len(body)
    charptr_bpe = _bits_needed(pos) if pos else 1

    charmap_raw = _pack_bits(charmap, charmap_bpe)
    charptr_raw = _pack_bits(offsets, charptr_bpe)
    glyph_section = b"".join(records)

    header = bytearray(HEADER_SIZE)
    struct.pack_into("<H", header, _OFF_HEADER_OFFSET, 0)
    struct.pack_into("<H", header, _OFF_HEADER_SIZE, HEADER_SIZE)
    header[_OFF_MAGIC:_OFF_MAGIC + 4] = PGF_MAGIC
    struct.pack_into("<i", header, _OFF_REVISION, PGF_REVISION)
    struct.pack_into("<i", header, _OFF_VERSION, PGF_VERSION)
    struct.pack_into("<i", header, _OFF_CHARMAP_LEN, charmap_len)
    struct.pack_into("<i", header, _OFF_CHARPTR_LEN, len(codes))
    struct.pack_into("<i", header, _OFF_CHARMAP_BPE, charmap_bpe)
    struct.pack_into("<i", header, _OFF_CHARPTR_BPE, charptr_bpe)
    header[_OFF_BPP] = 4
    header[_OFF_FONT_NAME:_OFF_FONT_NAME + 64] = name.encode("ascii") + b"\x00" * (
        64 - len(name)
    )
    ftype = font_type.encode("ascii")
    if len(ftype) > 63:
        raise ValueError("fontType must fit in 63 ASCII chars")
    header[_OFF_FONT_TYPE:_OFF_FONT_TYPE + 64] = ftype + b"\x00" * (64 - len(ftype))
    struct.pack_into("<H", header, _OFF_FIRST_GLYPH, first_glyph)
    struct.pack_into("<H", header, _OFF_LAST_GLYPH, last_glyph)
    return bytes(header) + charmap_raw + charptr_raw + glyph_section


def parse_header(data: bytes) -> dict:
    """Parse and validate a scaffold PGF header. Raises ValueError."""
    if len(data) < HEADER_SIZE:
        raise ValueError("too short for a PGF header")
    (header_size,) = struct.unpack_from("<H", data, _OFF_HEADER_SIZE)
    magic = data[_OFF_MAGIC:_OFF_MAGIC + 4]
    revision, version = struct.unpack_from("<ii", data, _OFF_REVISION)
    if magic != PGF_MAGIC:
        raise ValueError("bad magic %r" % magic)
    if header_size != HEADER_SIZE:
        raise ValueError("unexpected header size %d" % header_size)
    if (revision, version) != (PGF_REVISION, PGF_VERSION):
        raise ValueError("unexpected revision/version %d/%d" % (revision, version))
    (charmap_len, charptr_len, charmap_bpe, charptr_bpe) = struct.unpack_from(
        "<iiii", data, _OFF_CHARMAP_LEN
    )
    font_name = data[_OFF_FONT_NAME:_OFF_FONT_NAME + 64].split(b"\x00")[0].decode("ascii")
    font_type = data[_OFF_FONT_TYPE:_OFF_FONT_TYPE + 64].split(b"\x00")[0].decode("ascii")
    first_glyph, last_glyph = struct.unpack_from("<HH", data, _OFF_FIRST_GLYPH)
    return {
        "font_name": font_name,
        "font_type": font_type,
        "charmap_len": charmap_len,
        "charptr_len": charptr_len,
        "charmap_bpe": charmap_bpe,
        "charptr_bpe": charptr_bpe,
        "first_glyph": first_glyph,
        "last_glyph": last_glyph,
    }


def read_glyph(data: bytes, codepoint: int) -> tuple[int, int, bytes]:
    """Resolve one glyph through the charmap. Returns (width, height, body)."""
    info = parse_header(data)
    if codepoint > info["last_glyph"]:
        raise ValueError("codepoint U+%04X outside charmap" % codepoint)
    charmap_bytes = (info["charmap_len"] * info["charmap_bpe"] + 31) // 32 * 4
    charptr_bytes = (info["charptr_len"] * info["charptr_bpe"] + 31) // 32 * 4
    base = HEADER_SIZE
    glyph_base = base + charmap_bytes + charptr_bytes
    glyph_id = _unpack_bits(
        data[base:base + charmap_bytes], info["charmap_len"], info["charmap_bpe"]
    )[codepoint]
    offset = _unpack_bits(
        data[base + charmap_bytes:base + charmap_bytes + charptr_bytes],
        info["charptr_len"],
        info["charptr_bpe"],
    )[glyph_id]
    width, height = struct.unpack_from("<HH", data, glyph_base + offset)
    stride = (width + 1) // 2
    body = data[glyph_base + offset + 4:glyph_base + offset + 4 + stride * height]
    if len(body) != stride * height:
        raise ValueError("truncated glyph record")
    return width, height, body


def build_manifest(
    inputs_sha256: dict[str, str],
    *,
    font_name: str,
    source_family: str,
    codepoints: list[int],
    output_sha256: str,
) -> str:
    """Deterministic JSON manifest binding inputs to one output payload."""
    notices = required_notices(source_family)  # raises KeyError when unknown
    doc = {
        "schema_version": 1,
        "generator": "tools/pgf_build.py",
        "font_name": font_name,
        "source_family": source_family,
        "inputs_sha256": dict(sorted(inputs_sha256.items())),
        "codepoint_count": len(codepoints),
        "codepoint_first": min(codepoints),
        "codepoint_last": max(codepoints),
        "output_sha256": output_sha256,
        "required_notices": notices,
        "conformance": "scaffold: header + tables per PGF.h rev2/ver6; "
        "glyph-record RLE parity unverified (see module docstring)",
    }
    return json.dumps(doc, sort_keys=True, indent=2, ensure_ascii=True) + "\n"


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()
