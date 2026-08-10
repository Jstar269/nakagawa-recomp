# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Bounded, read-only access to one explicitly supplied ClapHanz XB archive.

This module is deliberately independent of libxb.  It implements the small XB
reader needed for an investigation probe and never extracts files unless a
caller explicitly asks for bytes through :meth:`XBArchiveReader.read_entry`.
The command-line interface prints metadata only, so pointing it at a private
archive does not dump private contents into a terminal or log by default.

The format constants and compression algorithms follow the public libxb source
audit recorded in ``docs/ISSUE196_DIRECT_XB.md``.  The implementation here is
source-owned and adds bounds, path containment, duplicate detection, and
per-entry span checks that are intentionally stricter than the reference
extractor.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from enum import IntEnum
import json
from pathlib import Path
import re
import struct
import sys
from typing import Sequence


SIGNATURE = b"\x78\x65\x00\x01"
_HUF_TABLE_SIZE = 1 << 10
_HUF_MAX_DEPTH = 10
_KNOWN_VARIANT_LOCALES = {0: "USE", 2: "FRE", 3: "SPA"}
_VARIANT_RE = re.compile(r"\.xb([0-9]*)$", re.IGNORECASE)


class XBProbeError(ValueError):
    """Raised when an XB archive is not safe to inspect."""


class XBCompression(IntEnum):
    """Four compression tags stored in the high nibble of an FST offset."""

    DEFLATE = 0
    HUFFMAN = 1
    LZS = 2
    NONE = 3


@dataclass(frozen=True)
class XBLimits:
    """Resource limits applied before and during archive inspection."""

    max_archive_bytes: int = 512 * 1024 * 1024
    max_files: int = 100_000
    max_name_bytes: int = 255
    max_string_table_bytes: int = 16 * 1024 * 1024
    max_entry_bytes: int = 256 * 1024 * 1024
    max_total_expanded_bytes: int = 512 * 1024 * 1024
    max_huffman_depth: int = 16
    max_huffman_codes: int = 4096


@dataclass(frozen=True)
class XBVariant:
    """Variant inferred from the explicitly supplied filename, if present."""

    suffix: str | None
    number: int | None
    locale_hint: str | None

    @property
    def label(self) -> str:
        return "base" if self.suffix is None else f"xb{self.suffix}"


def variant_from_path(path: str | Path) -> XBVariant:
    """Return a stable label for ``.xb``/``.xbN`` without guessing semantics."""

    match = _VARIANT_RE.search(Path(path).name)
    if not match:
        return XBVariant(None, None, None)
    suffix = match.group(1)
    if not suffix:
        return XBVariant(None, None, None)
    number = int(suffix, 10)
    return XBVariant(suffix, number, _KNOWN_VARIANT_LOCALES.get(number))


@dataclass(frozen=True)
class XBEntry:
    """Metadata for one validated inner file."""

    index: int
    path: str
    offset: int
    expanded_size: int
    compression: XBCompression
    stored_size: int
    span_size: int

    @property
    def compressed_size(self) -> int | None:
        """Return the compressed payload size, or ``None`` for raw data."""

        if self.compression is XBCompression.NONE:
            return None
        return self.stored_size - 8

    def metadata(self) -> dict[str, object]:
        """Return metadata only; no archive bytes are included."""

        return {
            "index": self.index,
            "path": self.path,
            "offset": self.offset,
            "expanded_size": self.expanded_size,
            "compression": self.compression.name.lower(),
            "stored_size": self.stored_size,
            "span_size": self.span_size,
        }


class _Reader:
    """A bounded cursor that never permits a short primitive read."""

    def __init__(self, data: bytes | bytearray | memoryview, endian: str):
        self.data = memoryview(data)
        self.endian = endian
        self.pos = 0

    def require(self, size: int, what: str) -> None:
        if size < 0 or self.pos > len(self.data) - size:
            raise XBProbeError(f"truncated {what} at offset 0x{self.pos:x}")

    def read(self, size: int, what: str = "data") -> bytes:
        self.require(size, what)
        result = self.data[self.pos : self.pos + size].tobytes()
        self.pos += size
        return result

    def u8(self, what: str = "u8") -> int:
        return self.read(1, what)[0]

    def u16(self, what: str = "u16") -> int:
        return struct.unpack(self.endian + "H", self.read(2, what))[0]

    def u32(self, what: str = "u32") -> int:
        return struct.unpack(self.endian + "I", self.read(4, what))[0]

    def align(self, alignment: int, what: str = "alignment") -> None:
        if alignment <= 0 or alignment & (alignment - 1):
            raise XBProbeError(f"invalid internal alignment {alignment}")
        padding = (-self.pos) % alignment
        if padding:
            self.read(padding, what)

    def remaining(self) -> int:
        return len(self.data) - self.pos

    def eof(self) -> bool:
        return self.pos >= len(self.data)


def _sjis_hash(raw: bytes) -> int:
    value = 0
    for byte in raw:
        value = (((value & 0x7F) << 1) | ((value & 0x80) >> 7)) ^ byte
    return value & 0xFF


def normalize_inner_path(path: str) -> str:
    """Normalize separators while rejecting every escaping/ambiguous form."""

    if not path or "\x00" in path:
        raise XBProbeError("empty or NUL-containing inner path")
    normalized = path.replace("\\", "/")
    if normalized.startswith("/") or re.match(r"^[A-Za-z]:", normalized):
        raise XBProbeError(f"absolute inner path is not allowed: {path!r}")
    parts = normalized.split("/")
    if any(not part or part in (".", "..") for part in parts):
        raise XBProbeError(f"traversal or empty component in inner path: {path!r}")
    if any(ord(char) < 0x20 or ord(char) == 0x7F for char in normalized):
        raise XBProbeError(f"control character in inner path: {path!r}")
    return "/".join(parts)


def _check_zero_padding(data: bytes, label: str) -> None:
    """Allow zero bytes used for alignment; reject non-zero padding."""

    if any(data):
        raise XBProbeError(f"unexpected non-zero trailing bytes in {label}")


def _decode_lzs_body(payload: bytes, expanded_size: int, endian: str, limits: XBLimits) -> bytes:
    """Decode a bounded ClapHanz LZS body (without its 8-byte size header)."""

    if expanded_size <= 0 or expanded_size > limits.max_entry_bytes:
        raise XBProbeError("invalid or oversized LZS expanded size")
    reader = _Reader(payload, endian)
    output = bytearray()
    try:
        while len(output) < expanded_size:
            code = reader.u8("LZS control byte")
            if (code & 0x03) == 0:
                copy_len = (code >> 2) + 1
                if len(output) + copy_len > expanded_size:
                    raise XBProbeError("LZS literal exceeds expanded size")
                output.extend(reader.read(copy_len, "LZS literal"))
                continue

            if code & 0x01:
                b0 = reader.u8("LZS short-run distance")
                value = (b0 << 8) | code
                run_len = ((value & 0x0E) >> 1) + 3
                run_offset = value >> 4
            else:
                b0 = reader.u8("LZS long-run distance")
                b1 = reader.u8("LZS long-run distance")
                value = (b1 << 16) | (b0 << 8) | code
                run_len = ((value & 0x3FC) >> 2) + 3
                run_offset = value >> 12

            if run_offset <= 0 or run_offset > len(output):
                raise XBProbeError("LZS back-reference leaves output")
            if len(output) + run_len > expanded_size:
                raise XBProbeError("LZS run exceeds expanded size")
            source = len(output) - run_offset
            for _ in range(run_len):
                output.append(output[source])
                source += 1
    except IndexError as exc:
        raise XBProbeError("LZS back-reference is malformed") from exc
    _check_zero_padding(reader.data[reader.pos :].tobytes(), "LZS body")
    return bytes(output)


@dataclass(frozen=True)
class _HuffmanSymbol:
    length: int
    symbol: int


def _decode_huffman_body(payload: bytes, expanded_size: int, endian: str, limits: XBLimits) -> bytes:
    """Decode a bounded ClapHanz Huffman body."""

    if expanded_size <= 0 or expanded_size > limits.max_entry_bytes:
        raise XBProbeError("invalid or oversized Huffman expanded size")
    reader = _Reader(payload, endian)
    max_length = reader.u8("Huffman maximum depth")
    if max_length == 0 or max_length > limits.max_huffman_depth:
        raise XBProbeError("unsupported or invalid Huffman depth")

    table: list[_HuffmanSymbol | None] = [None] * _HUF_TABLE_SIZE
    code = 0
    length = 1
    code_count = 0
    while length <= max_length:
        code_num = reader.u8("Huffman code count")
        code_count += code_num
        if code_count > limits.max_huffman_codes:
            raise XBProbeError("Huffman code table is too large")
        for _ in range(code_num):
            if code >= (1 << length):
                raise XBProbeError("oversubscribed Huffman table")
            code_bits = code
            index = 0
            for _ in range(length):
                index = (index << 1) | (code_bits & 1)
                code_bits >>= 1
            symbol = reader.u8("Huffman symbol")
            while index < _HUF_TABLE_SIZE:
                previous = table[index]
                current = _HuffmanSymbol(length, symbol)
                if previous is not None and previous != current:
                    raise XBProbeError("conflicting Huffman table entries")
                table[index] = current
                index += 1 << length
            code += 1
        length += 1
        code <<= 1

    if any(entry is None for entry in table):
        raise XBProbeError("Huffman table does not cover all codes")
    if reader.pos % 2:
        reader.read(1, "Huffman table alignment")

    output = bytearray()
    bit_buffer = 0
    bit_count = 0
    try:
        while len(output) < expanded_size:
            while bit_count < _HUF_MAX_DEPTH:
                word = reader.u16("Huffman bitstream")
                bit_buffer |= word << bit_count
                bit_count += 16
            entry = table[bit_buffer & (_HUF_TABLE_SIZE - 1)]
            if entry is None:  # Defensive; the coverage check above proves this.
                raise XBProbeError("Huffman code has no table entry")
            if entry.length <= _HUF_MAX_DEPTH:
                output.append(entry.symbol & 0xFF)
                bit_buffer >>= entry.length
                bit_count -= entry.length
            else:
                bit_buffer >>= _HUF_MAX_DEPTH
                bit_count -= _HUF_MAX_DEPTH
                while bit_count < 8:
                    word = reader.u16("Huffman literal")
                    bit_buffer |= word << bit_count
                    bit_count += 16
                output.append(bit_buffer & 0xFF)
                bit_buffer >>= 8
                bit_count -= 8
    except (IndexError, struct.error) as exc:
        raise XBProbeError("Huffman bitstream is malformed") from exc
    _check_zero_padding(reader.data[reader.pos :].tobytes(), "Huffman body")
    return bytes(output)


def _decode_prefixed(
    payload: bytes,
    compression: XBCompression,
    expected_size: int,
    endian: str,
    limits: XBLimits,
) -> tuple[bytes, int]:
    """Decode one file payload and return bytes plus its stored size."""

    if len(payload) < 8:
        raise XBProbeError("compressed entry is missing its size header")
    reader = _Reader(payload, endian)
    expanded_size = reader.u32("compressed expanded size")
    compressed_size = reader.u32("compressed payload size")
    if expanded_size <= 0 or expanded_size > limits.max_entry_bytes:
        raise XBProbeError("invalid or oversized compressed expanded size")
    if compressed_size > reader.remaining():
        raise XBProbeError("compressed payload is truncated")
    if compressed_size == 0:
        body = reader.read(expanded_size, "uncompressed fallback payload")
        _check_zero_padding(reader.data[reader.pos :].tobytes(), "compressed entry")
        stored_size = 8 + expanded_size
    else:
        body = reader.read(compressed_size, "compressed payload")
        _check_zero_padding(reader.data[reader.pos :].tobytes(), "compressed entry")
        stored_size = 8 + compressed_size

    if compression is XBCompression.LZS:
        if expanded_size != expected_size:
            raise XBProbeError("LZS expanded size disagrees with FST")
        if compressed_size == 0:
            if len(body) != expected_size:
                raise XBProbeError("uncompressed LZS fallback has wrong size")
            return body, stored_size
        return _decode_lzs_body(body, expected_size, endian, limits), stored_size

    if compression is XBCompression.HUFFMAN:
        if expanded_size != expected_size:
            raise XBProbeError("Huffman expanded size disagrees with FST")
        if compressed_size == 0:
            if len(body) != expected_size:
                raise XBProbeError("uncompressed Huffman fallback has wrong size")
            return body, stored_size
        return _decode_huffman_body(body, expected_size, endian, limits), stored_size

    if compression is XBCompression.DEFLATE:
        intermediate = body if compressed_size == 0 else _decode_huffman_body(body, expanded_size, endian, limits)
        result, _ = _decode_prefixed(intermediate, XBCompression.LZS, expected_size, endian, limits)
        return result, stored_size

    raise XBProbeError(f"unsupported compression tag {compression!r}")


class XBArchiveReader:
    """Open and validate one XB archive without changing production lookup."""

    def __init__(
        self,
        path: str | Path,
        *,
        endian: str = "<",
        limits: XBLimits | None = None,
    ):
        if endian not in ("<", ">"):
            raise XBProbeError("endian must be '<' or '>'")
        self.path = Path(path)
        self.endian = endian
        self.limits = limits or XBLimits()
        try:
            size = self.path.stat().st_size
        except OSError as exc:
            raise XBProbeError(f"cannot stat archive: {self.path}") from exc
        if size > self.limits.max_archive_bytes:
            raise XBProbeError("archive exceeds the configured byte limit")
        try:
            self._data = self.path.read_bytes()
        except OSError as exc:
            raise XBProbeError(f"cannot read archive: {self.path}") from exc
        if len(self._data) != size:
            raise XBProbeError("archive changed while it was being read")

        self.variant = variant_from_path(self.path)
        self._entries, self._data_start = self._parse()
        self._by_path = {entry.path: entry for entry in self._entries}

    @classmethod
    def from_bytes(
        cls,
        data: bytes,
        *,
        name: str = "fixture.xb",
        endian: str = "<",
        limits: XBLimits | None = None,
    ) -> "XBArchiveReader":
        """Build a reader for synthetic tests without creating a private file."""

        obj = cls.__new__(cls)
        if endian not in ("<", ">"):
            raise XBProbeError("endian must be '<' or '>'")
        obj.path = Path(name)
        obj.endian = endian
        obj.limits = limits or XBLimits()
        if len(data) > obj.limits.max_archive_bytes:
            raise XBProbeError("archive exceeds the configured byte limit")
        obj._data = bytes(data)
        obj.variant = variant_from_path(name)
        obj._entries, obj._data_start = obj._parse()
        obj._by_path = {entry.path: entry for entry in obj._entries}
        return obj

    @property
    def entries(self) -> tuple[XBEntry, ...]:
        return tuple(self._entries)

    @property
    def data_start(self) -> int:
        return self._data_start

    def lookup(self, path: str) -> XBEntry | None:
        """Return one exact normalized key, or ``None`` if it is absent."""

        return self._by_path.get(normalize_inner_path(path))

    def read_entry(self, entry_or_path: XBEntry | str) -> bytes:
        """Read one validated entry on explicit request."""

        entry = entry_or_path if isinstance(entry_or_path, XBEntry) else self.lookup(entry_or_path)
        if entry is None:
            raise KeyError(entry_or_path)
        payload = self._entry_span(entry)
        if entry.compression is XBCompression.NONE:
            data = payload[: entry.expanded_size]
        else:
            data, _ = _decode_prefixed(
                payload,
                entry.compression,
                entry.expanded_size,
                self.endian,
                self.limits,
            )
        if len(data) != entry.expanded_size:
            raise XBProbeError("decoded entry size disagrees with FST")
        return data

    def metadata(self, *, limit: int = 100) -> dict[str, object]:
        """Return bounded metadata suitable for JSON/logging."""

        if limit < 0:
            raise XBProbeError("metadata limit must be non-negative")
        visible = self._entries[:limit]
        return {
            "path": str(self.path),
            "variant": self.variant.label,
            "variant_number": self.variant.number,
            "locale_hint": self.variant.locale_hint,
            "endian": "little" if self.endian == "<" else "big",
            "data_start": self.data_start,
            "entry_count": len(self._entries),
            "entries": [entry.metadata() for entry in visible],
            "entries_truncated": len(visible) != len(self._entries),
        }

    def _entry_span(self, entry: XBEntry) -> bytes:
        start = entry.offset
        end = start + entry.span_size
        if end > len(self._data):
            raise XBProbeError("entry span exceeds archive")
        return self._data[start:end]

    def _parse(self) -> tuple[list[XBEntry], int]:
        if len(self._data) < len(SIGNATURE) + 4:
            raise XBProbeError("archive is shorter than its header")
        reader = _Reader(self._data, self.endian)
        if reader.read(4, "XB signature") != SIGNATURE:
            raise XBProbeError("not an XB archive")
        file_count = reader.u32("XB file count")
        if file_count == 0 or file_count > self.limits.max_files:
            raise XBProbeError("invalid or excessive XB file count")
        fst_bytes = file_count * 8
        reader.require(fst_bytes, "filesystem table")
        fst: list[tuple[int, int, XBCompression]] = []
        for _ in range(file_count):
            expanded_size = reader.u32("FST expanded size")
            packed = reader.u32("FST compression/offset")
            compression_code = packed >> 28
            if compression_code not in range(4):
                raise XBProbeError("unknown XB compression tag")
            offset = (packed & 0x0FFFFFFF) * 4
            fst.append((expanded_size, offset, XBCompression(compression_code)))

        reader.align(4, "FST alignment")
        string_expanded = reader.u32("string-table expanded size")
        string_compressed = reader.u32("string-table compressed size")
        if string_expanded <= 0 or string_expanded > self.limits.max_string_table_bytes:
            raise XBProbeError("invalid or oversized string-table expanded size")
        if string_compressed == 0:
            table_data = reader.read(string_expanded, "string table")
        else:
            if string_compressed > self.limits.max_string_table_bytes:
                raise XBProbeError("oversized string-table compressed size")
            compressed = reader.read(string_compressed, "compressed string table")
            table_data = _decode_lzs_body(compressed, string_expanded, self.endian, self.limits)

        table_reader = _Reader(table_data, self.endian)
        names: list[str] = []
        seen: set[str] = set()
        for index in range(file_count):
            name_length = table_reader.u8("string-table name length")
            if name_length > self.limits.max_name_bytes:
                raise XBProbeError("inner path exceeds the name limit")
            expected_hash = table_reader.u8("string-table name hash")
            start = table_reader.pos
            while table_reader.pos < len(table_reader.data) and table_reader.data[table_reader.pos] != 0:
                table_reader.pos += 1
            if table_reader.pos >= len(table_reader.data):
                raise XBProbeError("unterminated string-table path")
            raw = table_reader.data[start : table_reader.pos].tobytes()
            table_reader.pos += 1
            if len(raw) != name_length:
                raise XBProbeError("string-table path length mismatch")
            try:
                value = raw.decode("shift_jis", errors="strict")
            except UnicodeDecodeError as exc:
                raise XBProbeError("string-table path is not valid Shift-JIS") from exc
            if expected_hash != _sjis_hash(raw):
                raise XBProbeError("string-table path hash mismatch")
            canonical = normalize_inner_path(value)
            if canonical in seen:
                raise XBProbeError(f"duplicate inner path: {canonical}")
            seen.add(canonical)
            names.append(canonical)
        if not table_reader.eof():
            raise XBProbeError("string table has trailing bytes")

        reader.align(4, "string-table alignment")
        data_start = reader.pos
        if data_start >= len(self._data):
            raise XBProbeError("archive has no file-data section")

        offsets = sorted((offset, index) for index, (_, offset, _) in enumerate(fst))
        entries: list[XBEntry | None] = [None] * file_count
        total_expanded = 0
        previous_offset = None
        for position, (offset, index) in enumerate(offsets):
            expanded_size, _, compression = fst[index]
            if expanded_size <= 0 or expanded_size > self.limits.max_entry_bytes:
                raise XBProbeError("invalid or oversized FST entry size")
            if offset < data_start or offset >= len(self._data) or offset % 4:
                raise XBProbeError("FST entry offset is outside the file-data section")
            if previous_offset == offset:
                raise XBProbeError("duplicate FST data offset")
            previous_offset = offset
            next_offset = offsets[position + 1][0] if position + 1 < len(offsets) else len(self._data)
            if next_offset <= offset:
                raise XBProbeError("FST data offsets are not increasing")
            span_size = next_offset - offset
            payload = self._data[offset:next_offset]
            if compression is XBCompression.NONE:
                stored_size = expanded_size
                if expanded_size > span_size:
                    raise XBProbeError("raw FST entry is truncated")
                _check_zero_padding(payload[expanded_size:], "raw entry")
            else:
                if len(payload) < 8:
                    raise XBProbeError("compressed entry is missing its size header")
                hdr_reader = _Reader(payload, self.endian)
                hdr_expanded = hdr_reader.u32("compressed expanded size")
                hdr_compressed = hdr_reader.u32("compressed payload size")
                if hdr_expanded <= 0 or hdr_expanded > self.limits.max_entry_bytes:
                    raise XBProbeError("invalid or oversized compressed expanded size")
                if compression in (XBCompression.LZS, XBCompression.HUFFMAN) and hdr_expanded != expanded_size:
                    raise XBProbeError("compressed entry expanded size mismatch")
                if hdr_compressed == 0:
                    stored_size = 8 + hdr_expanded
                else:
                    stored_size = 8 + hdr_compressed
                if stored_size > span_size:
                    raise XBProbeError("compressed entry exceeds its FST span")
                _check_zero_padding(payload[stored_size:], "compressed entry")
            total_expanded += expanded_size
            if total_expanded > self.limits.max_total_expanded_bytes:
                raise XBProbeError("total expanded size exceeds the configured limit")
            entries[index] = XBEntry(
                index=index,
                path=names[index],
                offset=offset,
                expanded_size=expanded_size,
                compression=compression,
                stored_size=stored_size,
                span_size=span_size,
            )

        if any(entry is None for entry in entries):
            raise XBProbeError("internal FST/name cardinality mismatch")
        return [entry for entry in entries if entry is not None], data_start


def _endian_arg(value: str) -> str:
    return {"little": "<", "big": ">"}[value]


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="bounded read-only metadata/lookup probe for one XB archive")
    parser.add_argument("archive", type=Path, help="explicit local .xb/.xbN archive")
    parser.add_argument(
        "--endian",
        choices=("little", "big"),
        default="little",
        help="archive byte order (MNTP/libxb preset is little-endian)",
    )
    parser.add_argument("--lookup", help="exact inner key to look up; prints metadata only")
    parser.add_argument(
        "--limit",
        type=int,
        default=100,
        help="maximum metadata entries to print (default: 100)",
    )
    parser.add_argument("--json", action="store_true", help="emit JSON metadata")
    parser.add_argument("--max-archive-bytes", type=int, default=XBLimits.max_archive_bytes)
    parser.add_argument("--max-files", type=int, default=XBLimits.max_files)
    parser.add_argument("--max-entry-bytes", type=int, default=XBLimits.max_entry_bytes)
    parser.add_argument("--max-total-bytes", type=int, default=XBLimits.max_total_expanded_bytes)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_arg_parser().parse_args(argv)
    try:
        limits = XBLimits(
            max_archive_bytes=args.max_archive_bytes,
            max_files=args.max_files,
            max_entry_bytes=args.max_entry_bytes,
            max_total_expanded_bytes=args.max_total_bytes,
        )
        archive = XBArchiveReader(args.archive, endian=_endian_arg(args.endian), limits=limits)
        if args.lookup is not None:
            entry = archive.lookup(args.lookup)
            if entry is None:
                raise XBProbeError(f"inner key not found: {args.lookup}")
            payload: object = entry.metadata()
        else:
            payload = archive.metadata(limit=args.limit)
        if args.json:
            print(json.dumps(payload, ensure_ascii=False, sort_keys=True))
        elif isinstance(payload, dict) and "path" in payload and "expanded_size" in payload:
            print(
                f"{payload['path']} offset=0x{payload['offset']:x} "
                f"size={payload['expanded_size']} compression={payload['compression']}"
            )
        else:
            print(
                f"archive={archive.path} variant={archive.variant.label} "
                f"endian={args.endian} entries={len(archive.entries)}"
            )
            for entry in archive.entries[: args.limit]:
                print(
                    f"{entry.path} offset=0x{entry.offset:x} "
                    f"size={entry.expanded_size} compression={entry.compression.name.lower()}"
                )
    except (OSError, XBProbeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
