# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Per-user PSP system-font import: PGF checks, slot classification, manifest v2, removal.

The checks in :func:`pgf_verdict` mirror the project-authored native reader
(``src/rt/pgf_public.c``, ``pgf_validate_memory``) rule for rule, in the same order.
``tests/native`` and ``tools/test_pgf_validate_parity.py`` compare the two on the same
files. The slot constants mirror ``src/core/nk_font_slots.h``, and a test pins them.

Nothing here reads keys, decrypts, or uploads. The user brings their own font files.
"""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import struct
from typing import Any, Dict, Iterable, List, Optional, Tuple

from .prereq_fetcher import default_data_root as default_user_data_root

# --- Layout, mirrored from src/core/nk_font_slots.h -------------------------------------
CACHE_PARENT = "fonts"
CACHE_SUBDIR = "v2"
MANIFEST_NAME = "manifest.json"
MANIFEST_SCHEMA_VERSION = 2
MANIFEST_SOURCE_USER = "user"
READER_VERSION = 1
PROJECT_SUBDIR = "font"
PGF_MAX_BYTES = 16 * 1024 * 1024
IMPORT_MAX_FILES = 64

#: Slot ids in NkFontSlot order. Each slot's cache file is its served project name.
SLOTS: Tuple[str, ...] = ("japanese", "latin", "korean")
SLOT_ROLES: Dict[str, str] = {"japanese": "Japanese", "latin": "Latin", "korean": "Korean"}
SLOT_FILES: Dict[str, str] = {
    "japanese": "nkjpn.pgf",
    "latin": "nkltn.pgf",
    "korean": "nkkr.pgf",
}

# --- Reader constants, mirrored from src/rt/pgf_public.c and docs/cleanroom/PGF_SPEC.md ----
BASE_HEADER_SIZE = 392
REV3_HEADER_SIZE = 412
MAX_COUNT = 1 << 20
TABLE_COUNT = 4
COMPOSITE_PARTS = 3
PROBE_LATIN = (0x41, 0x61)
PROBE_KANA = (0x3042, 0x30A2)
PROBE_HANGUL = (0xAC00, 0xD55C)

#: Refusal names in PgfRefusal order (src/rt/pgf_api.h). Index 0 is accepted.
REFUSAL_NAMES: Tuple[str, ...] = (
    "NONE",
    "TOO_LARGE",
    "TRUNCATED",
    "HEADER_OFFSET",
    "MAGIC",
    "REVISION",
    "HEADER_SIZE",
    "GLYPH_RANGE",
    "COUNTS",
    "NO_GLYPHS",
    "GLYPH",
)
REFUSAL_TEXT: Dict[str, str] = {
    "TOO_LARGE": "larger than the 16 MiB reader ceiling",
    "TRUNCATED": "truncated",
    "HEADER_OFFSET": "header offset is not zero",
    "MAGIC": "invalid PGF magic (expected PGF0)",
    "REVISION": "unsupported revision or version",
    "HEADER_SIZE": "header size does not match the revision",
    "GLYPH_RANGE": "corrupt glyph indices (first exceeds last)",
    "COUNTS": "count or bit width out of range",
    "NO_GLYPHS": "no glyph records",
    "GLYPH": "a glyph record fails the reader's checks",
}


class FontValidationError(ValueError):
    """Raised when a PGF font file fails the reader's checks."""


class FontImportError(RuntimeError):
    """Raised when font import cannot find, read, or stage font files."""


class _Refused(Exception):
    """Internal: the reader refuses the image by the named rule."""

    def __init__(self, refusal: str) -> None:
        super().__init__(refusal)
        self.refusal = refusal


# --- The reader's rules --------------------------------------------------------------------


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _i32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def _sign7(value: int) -> int:
    value &= 0x7F
    return value - 128 if value & 0x40 else value


def _packed_size(count: int, bits: int) -> int:
    words = (count * bits + 31) // 32
    return words * 4


def _get_bits(data: bytes, base: int, span: int, bit_offset: int, bit_count: int) -> Optional[int]:
    """Read bit_count bits at bit_offset inside the region data[base:base+span]; None if invalid."""
    end_bit = bit_offset + bit_count
    if bit_count == 0 or bit_count > 32 or end_bit > span * 8:
        return None
    first_byte = bit_offset // 8
    shift = bit_offset % 8
    bytes_needed = (shift + bit_count + 7) // 8
    packed = 0
    for i in range(bytes_needed):
        packed |= data[base + first_byte + i] << (8 * i)
    packed >>= shift
    if bit_count == 32:
        return packed & 0xFFFFFFFF
    return packed & ((1 << bit_count) - 1)


class _Image:
    """The reader's state for one image: directory fields, sections, and glyph checks."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.size = len(data)
        self.first_glyph = 0
        self.glyph_count = 0
        self.char_map_count = 0
        self.char_map_bits = 0
        self.char_pointer_bits = 0
        self.shadow_count = 0
        self.metric_counts = [0] * TABLE_COUNT
        self.metric_offsets = [0] * TABLE_COUNT
        self.shadow_map_offset = 0
        self.char_map_offset = 0
        self.char_pointer_offset = 0
        self.glyph_data_offset = 0
        self.revision = 0

    # Directory -------------------------------------------------------------------------
    def parse_directory(self) -> None:
        data = self.data
        size = self.size
        if size > 16 * 1024 * 1024:
            raise _Refused("TOO_LARGE")
        if size < BASE_HEADER_SIZE:
            raise _Refused("TRUNCATED")
        if _u16(data, 0) != 0:
            raise _Refused("HEADER_OFFSET")
        if data[4:8] != b"PGF0":
            raise _Refused("MAGIC")
        header_size = _u16(data, 2)
        revision = _i32(data, 8)
        version = _i32(data, 12)
        if revision < 0 or revision > 3 or version < 0:
            raise _Refused("REVISION")
        if (header_size not in (BASE_HEADER_SIZE, REV3_HEADER_SIZE)) or (
            header_size == REV3_HEADER_SIZE and revision != 3
        ):
            raise _Refused("HEADER_SIZE")
        if size < header_size:
            raise _Refused("TRUNCATED")

        self.revision = revision
        self.char_map_count = _u32(data, 0x10)
        pointer_count = _u32(data, 0x14)
        char_map_bits = _u32(data, 0x18)
        pointer_bits = _u32(data, 0x1C)
        first = _u16(data, 0xB6)
        last = _u16(data, 0xB8)
        shadow_count = _u32(data, 0x16C)
        shadow_bits = _u32(data, 0x170)
        if first > last:
            raise _Refused("GLYPH_RANGE")
        if (
            char_map_bits == 0
            or char_map_bits > 32
            or pointer_bits == 0
            or pointer_bits > 32
            or self.char_map_count > MAX_COUNT
            or pointer_count > MAX_COUNT
            or shadow_count > MAX_COUNT
        ):
            raise _Refused("COUNTS")
        glyph_count = pointer_count
        if glyph_count == 0:
            raise _Refused("NO_GLYPHS")
        if glyph_count > MAX_COUNT:
            raise _Refused("COUNTS")
        if shadow_count == 0:
            shadow_ok = shadow_bits == 0 or shadow_bits == 16
        else:
            shadow_ok = shadow_bits == 16
        if not shadow_ok:
            raise _Refused("COUNTS")

        self.first_glyph = first
        self.glyph_count = glyph_count
        self.char_map_bits = char_map_bits
        self.char_pointer_bits = pointer_bits
        self.shadow_count = shadow_count
        self.metric_counts = [data[0x102 + i] for i in range(TABLE_COUNT)]

        rev3_a = _u16(data, 0x18C) if header_size == REV3_HEADER_SIZE else 0
        rev3_b = _u16(data, 0x194) if header_size == REV3_HEADER_SIZE else 0

        cursor = header_size

        def add_section(length: int) -> int:
            nonlocal cursor
            if length > size or cursor > size or length > size - cursor:
                raise _Refused("TRUNCATED")
            offset = cursor
            cursor += length
            return offset

        for i in range(TABLE_COUNT):
            self.metric_offsets[i] = add_section(self.metric_counts[i] * 8)
        self.shadow_map_offset = add_section(_packed_size(shadow_count, shadow_bits))
        if header_size == REV3_HEADER_SIZE:
            add_section(rev3_a * 4)
            add_section(rev3_b * 4)
        self.char_map_offset = add_section(_packed_size(self.char_map_count, char_map_bits))
        self.char_pointer_offset = add_section(_packed_size(pointer_count, pointer_bits))
        self.glyph_data_offset = cursor

    # Map, pointers, metrics -----------------------------------------------------------
    def map_entry(self, code: int) -> Optional[int]:
        index = code - self.first_glyph
        if index < 0 or index >= self.char_map_count:
            return None
        span = self.size - self.char_map_offset
        value = _get_bits(self.data, self.char_map_offset, span, index * self.char_map_bits, self.char_map_bits)
        if value is None:
            return None
        if value == (1 << self.char_map_bits) - 1 or value >= self.glyph_count:
            return None
        return value

    def pointer_entry(self, glyph_id: int) -> Optional[int]:
        if glyph_id >= self.glyph_count:
            return None
        span = self.size - self.char_pointer_offset
        units = _get_bits(
            self.data, self.char_pointer_offset, span, glyph_id * self.char_pointer_bits, self.char_pointer_bits
        )
        if units is None:
            return None
        return units * 4

    def table_pair(self, table: int, index: int) -> Optional[Tuple[int, int]]:
        if table >= TABLE_COUNT or index >= self.metric_counts[table]:
            return None
        entry = self.metric_offsets[table] + index * 8
        return _i32(self.data, entry), _i32(self.data, entry + 4)

    # Glyphs ---------------------------------------------------------------------------
    def primary_glyph(self, glyph_id: int) -> Optional[Dict[str, Any]]:
        relative = self.pointer_entry(glyph_id)
        if relative is None or relative > self.size - self.glyph_data_offset:
            return None
        absolute64 = self.glyph_data_offset + relative
        if absolute64 > self.size:
            return None
        absolute = absolute64
        available = self.size - absolute
        if available < 12:
            return None
        base = absolute

        def bits(bit_offset: int, bit_count: int) -> Optional[int]:
            return _get_bits(self.data, base, available, bit_offset, bit_count)

        shadow_offset = bits(0, 14)
        width = bits(14, 7)
        height = bits(21, 7)
        adjust_x = bits(28, 7)
        adjust_y = bits(35, 7)
        row_order = bits(42, 2)
        flags = bits(45, 3)
        shadow_id = bits(55, 9)
        if None in (shadow_offset, width, height, adjust_x, adjust_y, row_order, flags, shadow_id):
            return None

        cursor = absolute + 8
        dimension = y_metrics = None
        for group in range(3):
            if flags & (1 << group):
                if cursor >= self.size:
                    return None
                field = self.data[cursor]
                cursor += 1
                pair = self.table_pair(group, field)
                if pair is None:
                    return None
            else:
                if cursor > self.size or self.size - cursor < 8:
                    return None
                pair = (_i32(self.data, cursor), _i32(self.data, cursor + 4))
                cursor += 8
            # The X-adjustment pair is read and bounds-checked above; only the other two are kept.
            if group == 0:
                dimension = pair
            elif group == 2:
                y_metrics = pair
        if cursor >= self.size:
            return None
        field = self.data[cursor]
        cursor += 1
        advance = self.table_pair(3, field)
        if advance is None:
            return None
        record_size = cursor - absolute
        descender = y_metrics[0] - dimension[1]
        if descender < -(1 << 31) or descender > (1 << 31) - 1:
            return None
        return {
            "record_offset": absolute,
            "record_size": record_size,
            "bitmap_offset": cursor,
            "shadow_offset": shadow_offset,
            "width": width,
            "height": height,
            "row_order": row_order,
            "shadow_id": shadow_id,
            "composite": 0,
            "shadow_row_order": 0,
        }

    def checked_glyph(self, glyph_id: int, allow_composite: bool) -> Optional[Dict[str, Any]]:
        glyph = self.primary_glyph(glyph_id)
        if glyph is None:
            return None
        if glyph["shadow_id"] != 0:
            if glyph["shadow_id"] > self.shadow_count:
                return None
            pos = self.shadow_map_offset + (glyph["shadow_id"] - 1) * 2
            shadow_code = _u16(self.data, pos)
            if self.map_entry(shadow_code) is None:
                return None
            if glyph["shadow_offset"] < glyph["record_size"]:
                return None
            target = glyph["record_offset"] + glyph["shadow_offset"]
            if target > self.size or self.size - target < 6:
                return None
            shadow_row = _get_bits(self.data, target, self.size - target, 42, 2)
            if shadow_row is None:
                return None
            glyph["shadow_row_order"] = shadow_row
        if glyph["row_order"] != 3:
            return glyph
        if not allow_composite:
            return None
        return self.resolve_composite(glyph)

    def resolve_composite(self, glyph: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        if glyph["bitmap_offset"] > self.size or self.size - glyph["bitmap_offset"] < COMPOSITE_PARTS * 2:
            return None
        for part in range(COMPOSITE_PARTS):
            code = _u16(self.data, glyph["bitmap_offset"] + part * 2)
            component_id = self.map_entry(code)
            if component_id is None:
                return None
            component = self.checked_glyph(component_id, False)
            if component is None:
                return None
            if component["row_order"] not in (1, 2) or component["width"] == 0 or component["height"] == 0:
                return None
        glyph["composite"] = 1
        return glyph

    def has_char(self, code: int) -> bool:
        glyph_id = self.map_entry(code)
        if glyph_id is None:
            return False
        glyph = self.checked_glyph(glyph_id, True)
        if glyph is None:
            return False
        return glyph["width"] != 0 and glyph["height"] != 0


def pgf_verdict(data: bytes) -> Dict[str, Any]:
    """Run the reader's checks on an in-memory image, mirroring pgf_validate_memory.

    Returns a dict with ``refusal`` (a name from REFUSAL_NAMES; "NONE" when accepted), and,
    when accepted, the header facts and coverage flags. Never raises for a bad image.
    """
    image = _Image(bytes(data))
    try:
        image.parse_directory()
        for glyph_id in range(image.glyph_count):
            if image.checked_glyph(glyph_id, True) is None:
                raise _Refused("GLYPH")
    except _Refused as refused:
        return {"refusal": refused.refusal, "ok": False}
    raw = image.data
    return {
        "refusal": "NONE",
        "ok": True,
        "revision": image.revision,
        "header_size": _u16(raw, 2),
        "first_glyph": image.first_glyph,
        "last_glyph": _u16(raw, 0xB8),
        "glyph_count": image.glyph_count,
        "char_map_count": image.char_map_count,
        "nominal_h": _i32(raw, 0x24),
        "nominal_v": _i32(raw, 0x28),
        "has_latin": int(all(image.has_char(code) for code in PROBE_LATIN)),
        "has_kana": int(any(image.has_char(code) for code in PROBE_KANA)),
        "has_hangul": int(any(image.has_char(code) for code in PROBE_HANGUL)),
    }


def classify_verdict(verdict: Dict[str, Any]) -> Tuple[Optional[str], str]:
    """Name the slot for an accepted verdict, as nk_font_classify_verdict does."""
    if not verdict.get("ok"):
        return None, "the reader refused the file"
    if verdict["has_hangul"] and verdict["has_kana"]:
        return None, "covers both Hangul and kana, so it names no slot"
    if verdict["has_hangul"]:
        return "korean", ""
    if verdict["has_kana"]:
        return "japanese", ""
    if verdict["has_latin"]:
        return "latin", ""
    return None, "covers none of the slot probe characters"


def _inspect(data: bytes) -> Dict[str, Any]:
    verdict = pgf_verdict(data)
    result: Dict[str, Any] = {
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "accepted": bool(verdict["ok"]),
        "refusal": "" if verdict["ok"] else REFUSAL_TEXT[verdict["refusal"]],
        "verdict": verdict,
        "slot": None,
        "slot_reason": "",
    }
    if verdict["ok"]:
        result["slot"], result["slot_reason"] = classify_verdict(verdict)
    else:
        result["slot_reason"] = result["refusal"]
    return result


# --- Public validation -----------------------------------------------------------------


def validate_pgf_data(data: bytes, filename: str = "PGF") -> Dict[str, Any]:
    """Validate PGF bytes with the reader's rules and report size, SHA-256, and slot.

    Raises FontValidationError naming the refusal. Accepted images return the header
    facts, the coverage flags, and the slot the image names (None when it names none).
    """
    info = _inspect(data)
    if not info["accepted"]:
        raise FontValidationError(f"Font file '{filename}' is refused by the PGF reader: {info['refusal']}.")
    verdict = info["verdict"]
    return {
        "size": info["size"],
        "sha256": info["sha256"],
        "revision": verdict["revision"],
        "header_size": verdict["header_size"],
        "first_glyph": verdict["first_glyph"],
        "last_glyph": verdict["last_glyph"],
        "glyph_count": verdict["glyph_count"],
        "char_map_count": verdict["char_map_count"],
        "sparse": verdict["char_map_count"] > verdict["glyph_count"],
        "nominal_h": verdict["nominal_h"],
        "nominal_v": verdict["nominal_v"],
        "has_latin": bool(verdict["has_latin"]),
        "has_kana": bool(verdict["has_kana"]),
        "has_hangul": bool(verdict["has_hangul"]),
        "slot": info["slot"],
        "slot_reason": info["slot_reason"],
    }


def validate_pgf_file(file_path: Path) -> Dict[str, Any]:
    """Read and validate a PGF font file from disk, refusing a file above the ceiling unread."""
    if not file_path.is_file():
        raise FontValidationError(f"Font file '{file_path.name}' does not exist or is not a file.")
    try:
        if file_path.stat().st_size > PGF_MAX_BYTES:
            raise FontValidationError(
                f"Font file '{file_path.name}' is refused by the PGF reader: {REFUSAL_TEXT['TOO_LARGE']}."
            )
        data = file_path.read_bytes()
    except OSError as exc:
        raise FontValidationError(f"Font file '{file_path.name}' could not be read: {exc}") from exc
    return validate_pgf_data(data, filename=file_path.name)


# --- Cache layout and manifest v2 -----------------------------------------------------


def get_font_cache_dir(user_data_root: Optional[Path] = None) -> Path:
    """Return the versioned per-user font cache directory: <user data>/fonts/v2."""
    root = (user_data_root or default_user_data_root()).expanduser().resolve(strict=False)
    return root / CACHE_PARENT / CACHE_SUBDIR


def _load_manifest(cache_dir: Path) -> Tuple[str, str, Dict[str, Dict[str, Any]]]:
    """Return (status, message, entries) with entries keyed by slot id.

    Status is MISSING (no manifest), INVALID (malformed, or an entry this build does not
    accept), or OK. The entries are the manifest's own values; file checks are separate.
    """
    path = cache_dir / MANIFEST_NAME
    if not path.is_file():
        return "MISSING", "No imported PSP font.", {}
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return "INVALID", "PSP font manifest is malformed; run fonts import <folder>.", {}
    if not isinstance(manifest, dict):
        return "INVALID", "PSP font manifest is malformed; run fonts import <folder>.", {}
    schema = manifest.get("schema_version")
    if type(schema) is not int or schema != MANIFEST_SCHEMA_VERSION:
        return "INVALID", "PSP font manifest schema is not version 2; run fonts import <folder>.", {}
    files = manifest.get("files")
    if not isinstance(files, dict) or not files:
        return "INVALID", "PSP font manifest lists no files; run fonts import <folder>.", {}
    entries: Dict[str, Dict[str, Any]] = {}
    for name, entry in files.items():
        slot = next((s for s in SLOTS if SLOT_FILES[s] == name), None)
        size = entry.get("size") if isinstance(entry, dict) else None
        sha = entry.get("sha256") if isinstance(entry, dict) else None
        ok = (
            slot is not None
            and slot not in entries
            and isinstance(entry, dict)
            and entry.get("slot") == slot
            and isinstance(size, int)
            and not isinstance(size, bool)
            and size > 0
            and isinstance(sha, str)
            and len(sha) == 64
            and entry.get("source") == MANIFEST_SOURCE_USER
        )
        if not ok:
            return "INVALID", (
                "PSP font manifest has an entry this build does not accept; run fonts import <folder>."
            ), {}
        entries[slot] = {"size": size, "sha256": sha}
    return "OK", "Imported PSP fonts are listed in the manifest.", entries


def _check_slot_file(cache_dir: Path, slot: str, entry: Dict[str, Any]) -> Tuple[bool, str]:
    """Check a manifest entry against its file: present, size and SHA-256, accepted, right slot."""
    path = cache_dir / SLOT_FILES[slot]
    if not path.is_file():
        return False, "its file is missing"
    try:
        data = path.read_bytes()
    except OSError as exc:
        return False, f"its file cannot be read ({exc.strerror or exc})"
    info = _inspect(data)
    if not info["accepted"]:
        return False, f"the reader refused it: {info['refusal']}"
    if info["slot"] != slot:
        return False, f"it does not name the {SLOT_ROLES[slot]} slot"
    if info["size"] != entry["size"] or info["sha256"] != entry["sha256"]:
        return False, "its size or SHA-256 differs from the manifest"
    return True, ""


def inspect_font_cache(
    user_data_root: Optional[Path] = None,
    fallback_root: Optional[Path] = None,
    cache_dir: Optional[Path] = None,
) -> Tuple[str, str]:
    """Check the imported-font manifest and each file: ('OK', 'MISSING', or 'INVALID').

    ``fallback_root`` is accepted for the existing call sites and is not a font source.
    """
    del fallback_root
    cdir = cache_dir or get_font_cache_dir(user_data_root)
    status, message, entries = _load_manifest(cdir)
    if status != "OK":
        return status, message
    for slot in SLOTS:
        if slot not in entries:
            continue
        ok, reason = _check_slot_file(cdir, slot, entries[slot])
        if not ok:
            return "INVALID", (
                f"Imported {SLOT_ROLES[slot]} font is invalid: {reason}; run fonts import <folder>."
            )
    return "OK", "Imported PSP fonts are listed in the manifest."


def slot_states(
    user_data_root: Optional[Path] = None,
    project_root: Optional[Path] = None,
) -> Dict[str, Dict[str, str]]:
    """Per-slot preflight: the imported font when it checks, else the project font, else none."""
    cdir = get_font_cache_dir(user_data_root)
    status, message, entries = _load_manifest(cdir)
    states: Dict[str, Dict[str, str]] = {}
    for slot in SLOTS:
        role = SLOT_ROLES[slot]
        reason = ""
        if status == "OK" and slot in entries:
            ok, why = _check_slot_file(cdir, slot, entries[slot])
            if ok:
                states[slot] = {"source": "user", "detail": f"{role}: imported from your PSP."}
                continue
            reason = why
        elif status == "INVALID":
            reason = message
        if project_root is not None:
            project_path = Path(project_root) / PROJECT_SUBDIR / SLOT_FILES[slot]
            if project_path.is_file():
                try:
                    info = _inspect(project_path.read_bytes())
                except OSError:
                    info = {"accepted": False}
                if info.get("accepted") and info.get("slot") == slot:
                    states[slot] = {"source": "project", "detail": f"{role}: project font."}
                    continue
        if reason:
            detail = f"{role}: missing ({reason})."
        else:
            detail = f"{role}: missing. Import a font from your PSP or install the project font."
        states[slot] = {"source": "none", "detail": detail}
    return states


def resolve_font_directory(
    user_data_root: Optional[Path] = None,
    fallback_root: Optional[Path] = None,
) -> Optional[Path]:
    """Resolve the font directory for runtime execution: the cache when its manifest checks."""
    cdir = get_font_cache_dir(user_data_root)
    status, _ = inspect_font_cache(cache_dir=cdir)
    if status == "OK":
        return cdir
    if fallback_root:
        fallback = fallback_root / PROJECT_SUBDIR
        if fallback.is_dir():
            return fallback
    return None


# --- Import and removal ---------------------------------------------------------------


def scan_source_folder(folder: Path) -> List[Path]:
    """List the .pgf files directly inside folder (not subfolders), whatever their names."""
    if not folder.is_dir():
        raise FontImportError(f"Font source path is not a directory: '{folder}'")
    try:
        names = sorted(
            entry.name for entry in folder.iterdir() if entry.is_file() and entry.name.lower().endswith(".pgf")
        )
    except OSError as exc:
        raise FontImportError(f"Error reading directory '{folder}': {exc}") from exc
    return [folder / name for name in names]


def _write_manifest(cache_dir: Path, entries: Dict[str, Dict[str, Any]]) -> None:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    files = {}
    for slot in SLOTS:
        if slot not in entries:
            continue
        files[SLOT_FILES[slot]] = {
            "slot": slot,
            "size": entries[slot]["size"],
            "sha256": entries[slot]["sha256"],
            "source": MANIFEST_SOURCE_USER,
            "reader_version": READER_VERSION,
        }
    manifest = {"schema_version": MANIFEST_SCHEMA_VERSION, "import_time": now, "files": files}
    target = cache_dir / MANIFEST_NAME
    staged = cache_dir / (MANIFEST_NAME + ".tmp")
    staged.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    os.replace(staged, target)


def import_fonts(
    source_folder: Path,
    user_data_root: Optional[Path] = None,
    choose: Optional[Dict[str, str]] = None,
) -> Dict[str, Any]:
    """Import user fonts from one folder: check each .pgf, name its slot, copy, and write manifest v2.

    Each file is read once and checked by the reader. A file the reader refuses, or that
    names no slot, is reported and skipped. A slot that several files name is imported only
    when ``choose`` names one of them for that slot; otherwise it is left alone. Originals are
    not touched. Slots this run does not change keep their entries.
    """
    source_folder = Path(source_folder)
    paths = scan_source_folder(source_folder)
    if not paths:
        raise FontImportError(f"No .pgf files are in '{source_folder}'.")
    if len(paths) > IMPORT_MAX_FILES:
        raise FontImportError(f"The folder holds more than {IMPORT_MAX_FILES} .pgf files.")
    choose = dict(choose or {})
    for slot in choose:
        if slot not in SLOTS:
            raise FontImportError(f"Unknown font slot '{slot}'; use one of {', '.join(SLOTS)}.")

    files: List[Dict[str, Any]] = []
    checked: Dict[str, Dict[str, Any]] = {}
    candidates: Dict[str, List[int]] = {slot: [] for slot in SLOTS}
    data_by_index: Dict[int, bytes] = {}
    for path in paths:
        outcome: Dict[str, Any] = {"name": path.name, "slot": None, "imported": False, "detail": ""}
        try:
            size_on_disk = path.stat().st_size
            if size_on_disk > PGF_MAX_BYTES:
                outcome["detail"] = f"refused: {REFUSAL_TEXT['TOO_LARGE']}"
                files.append(outcome)
                continue
            data = path.read_bytes()
        except OSError as exc:
            outcome["detail"] = f"refused: cannot be read ({exc.strerror or exc})"
            files.append(outcome)
            continue
        info = _inspect(data)
        index = len(files)
        checked[path.name] = info
        if not info["accepted"]:
            outcome["detail"] = f"refused: {info['refusal']}"
        elif info["slot"] is None:
            outcome["detail"] = f"refused: {info['slot_reason']}"
        else:
            outcome["slot"] = info["slot"]
            outcome["detail"] = f"names the {SLOT_ROLES[info['slot']]} slot"
            candidates[info["slot"]].append(index)
            data_by_index[index] = data
        files.append(outcome)

    selected: Dict[str, int] = {}
    for slot in SLOTS:
        options = candidates[slot]
        want = choose.get(slot)
        if want:
            match = [i for i in options if files[i]["name"] == want]
            if not match:
                raise FontImportError(f"The chosen {SLOT_ROLES[slot]} file does not name that slot.")
            selected[slot] = match[0]
        elif len(options) == 1:
            selected[slot] = options[0]
        elif len(options) > 1:
            for i in options:
                files[i]["detail"] = (
                    f"not imported: {len(options)} files name the {SLOT_ROLES[slot]} slot; choose one"
                )
        for i in options:
            if slot in selected and i != selected[slot]:
                files[i]["detail"] = f"not chosen for the {SLOT_ROLES[slot]} slot"

    cache_dir = get_font_cache_dir(user_data_root)
    cache_dir.mkdir(parents=True, exist_ok=True)
    _, _, previous = _load_manifest(cache_dir)

    staged: List[Tuple[Path, Path]] = []
    try:
        for slot, index in selected.items():
            source_path = source_folder / files[index]["name"]
            data = source_path.read_bytes()
            info = _inspect(data)
            if not info["accepted"] or info["slot"] != slot or info["sha256"] != checked[files[index]["name"]]["sha256"]:
                raise FontImportError(
                    f"The chosen {SLOT_ROLES[slot]} file changed or failed its check; try again."
                )
            target = cache_dir / SLOT_FILES[slot]
            tmp = cache_dir / (SLOT_FILES[slot] + ".tmp")
            tmp.write_bytes(data)
            staged.append((tmp, target))
    except OSError as exc:
        for tmp, _ in staged:
            tmp.unlink(missing_ok=True)
        raise FontImportError(f"Failed to stage imported fonts into cache: {exc}") from exc
    except FontImportError:
        for tmp, _ in staged:
            tmp.unlink(missing_ok=True)
        raise

    entries = {slot: dict(entry) for slot, entry in previous.items()}
    try:
        for tmp, target in staged:
            os.replace(tmp, target)
        for slot, index in selected.items():
            info = checked[files[index]["name"]]
            entries[slot] = {"size": info["size"], "sha256": info["sha256"]}
            files[index]["imported"] = True
            files[index]["detail"] = f"imported as the {SLOT_ROLES[slot]} font"
        if selected:
            _write_manifest(cache_dir, entries)
    except OSError as exc:
        raise FontImportError(f"Failed to stage imported fonts into cache: {exc}") from exc

    return {
        "status": "OK" if selected else "NONE",
        "cache_dir": str(cache_dir),
        "imported_count": len(selected),
        "slots": {slot: SLOT_FILES[slot] for slot in selected},
        "files": {SLOT_FILES[slot]: {"size": entries[slot]["size"], "sha256": entries[slot]["sha256"]}
                  for slot in entries},
        "outcomes": files,
    }


def remove_imports(user_data_root: Optional[Path] = None, slots: Optional[Iterable[str]] = None) -> int:
    """Remove the imported font for each named slot (every slot when None).

    Deletes only the cache's slot files. A slot's manifest entry is dropped once its file
    is gone: deleted here, or already absent. A delete that fails for any other reason,
    such as a directory at the slot's file name, keeps that slot's entry, and the other
    named slots are still processed. The manifest is rewritten only when an entry was
    dropped, and removed when no entry is left or when it is not usable. Returns the number
    of slot files removed; raises FontImportError naming each slot whose file could not be
    removed, after the manifest is updated.
    """
    cache_dir = get_font_cache_dir(user_data_root)
    wanted = list(SLOTS) if slots is None else list(slots)
    for slot in wanted:
        if slot not in SLOTS:
            raise FontImportError(f"Unknown font slot '{slot}'; use one of {', '.join(SLOTS)}.")
    status, _, entries = _load_manifest(cache_dir)
    removed = 0
    dropped = False
    failures: List[str] = []
    for slot in wanted:
        path = cache_dir / SLOT_FILES[slot]
        try:
            path.unlink()
            removed += 1
        except (FileNotFoundError, NotADirectoryError):
            # Already absent. A cache path through a non-directory is ENOTDIR on POSIX and
            # not-found on Windows; either way no slot file is there.
            pass
        except OSError as exc:
            kept = "; its manifest entry is kept" if slot in entries else ""
            failures.append(
                f"The {SLOT_ROLES[slot]} font file {SLOT_FILES[slot]} could not be removed "
                f"({exc.strerror or exc}){kept}."
            )
            continue
        if entries.pop(slot, None) is not None:
            dropped = True
    manifest_path = cache_dir / MANIFEST_NAME
    if status != "OK":
        if manifest_path.is_file():
            manifest_path.unlink()
    elif dropped:
        if entries:
            _write_manifest(cache_dir, entries)
        else:
            manifest_path.unlink(missing_ok=True)
    if failures:
        if removed:
            failures.append(f"Removed {removed} other imported font file(s).")
        raise FontImportError(" ".join(failures))
    return removed
