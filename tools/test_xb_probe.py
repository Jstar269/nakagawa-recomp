# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Synthetic, retail-free coverage for the direct XB probe."""

from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import io
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from xb_probe import (
    XBArchiveReader,
    XBCompression,
    XBProbeError,
    XBLimits,
    _sjis_hash,
    main,
    variant_from_path,
)


def _pad4(data: bytes) -> bytes:
    return data + b"\0" * ((-len(data)) % 4)


def _lzs_body(data: bytes) -> bytes:
    result = bytearray()
    for start in range(0, len(data), 64):
        chunk = data[start : start + 64]
        result.append((len(chunk) - 1) << 2)
        result.extend(chunk)
    return bytes(result)


def _lzs_stream(data: bytes, endian: str = "<") -> bytes:
    body = _lzs_body(data)
    return struct.pack(endian + "II", len(data), len(body)) + body


def _reverse_bits(value: int, width: int) -> int:
    result = 0
    for _ in range(width):
        result = (result << 1) | (value & 1)
        value >>= 1
    return result


def _huffman_codes() -> dict[int, tuple[int, int]]:
    """A complete synthetic table with 254 eight-bit + 4 nine-bit codes."""

    result: dict[int, tuple[int, int]] = {}
    code = 0
    for width, count in ((8, 254), (9, 4)):
        for symbol in range(256) if width == 8 else range(254, 258):
            if len(result) >= 254 and width == 8:
                break
            result.setdefault(symbol, (code, width))
            code += 1
            if width == 8 and len(result) == 254:
                break
        code <<= 1
    return result


def _huffman_body(data: bytes, endian: str = "<") -> bytes:
    codes = _huffman_codes()
    table = bytearray([9] + [0] * 7)
    table.extend((254,))
    table.extend(range(254))
    table.extend((4, 254, 255, 254, 255))
    if len(table) % 2:
        table.append(0)

    bits = 0
    bit_count = 0
    for value in data:
        code, width = codes[value]
        bits |= _reverse_bits(code, width) << bit_count
        bit_count += width
    stream = bytearray()
    for offset in range(0, bit_count, 16):
        word = (bits >> offset) & 0xFFFF
        stream.extend(word.to_bytes(2, "little" if endian == "<" else "big"))
    return bytes(table) + bytes(stream)


def _make_archive(
    entries: list[tuple[str, bytes, XBCompression]],
    *,
    endian: str = "<",
    string_table_compressed: bool = False,
    name: str = "fixture.xb",
) -> bytes:
    names = []
    for path, _, _ in entries:
        raw = path.encode("shift_jis")
        names.append(bytes((len(raw), _sjis_hash(raw))) + raw + b"\0")
    string_table = b"".join(names)
    if string_table_compressed:
        body = _lzs_body(string_table)
        string_section = struct.pack(endian + "II", len(string_table), len(body)) + body
    else:
        string_section = struct.pack(endian + "II", len(string_table), 0) + string_table
    string_section = _pad4(string_section)

    header = b"xe\0\1" + struct.pack(endian + "I", len(entries))
    data_start = len(header) + len(entries) * 8 + len(string_section)
    fst = bytearray()
    payloads = []
    offset = data_start
    for _, raw_data, compression in entries:
        if compression is XBCompression.NONE:
            payload = raw_data
        elif compression is XBCompression.LZS:
            body = _lzs_body(raw_data)
            payload = struct.pack(endian + "II", len(raw_data), len(body)) + body
        elif compression is XBCompression.HUFFMAN:
            body = _huffman_body(raw_data, endian)
            payload = struct.pack(endian + "II", len(raw_data), len(body)) + body
        elif compression is XBCompression.DEFLATE:
            inner = _lzs_stream(raw_data, endian)
            body = _huffman_body(inner, endian)
            payload = struct.pack(endian + "II", len(inner), len(body)) + body
        else:  # pragma: no cover - enum exhaustiveness guard
            raise AssertionError(compression)
        payload = _pad4(payload)
        fst.extend(struct.pack(endian + "II", len(raw_data), (int(compression) << 28) | (offset // 4)))
        payloads.append(payload)
        offset += len(payload)
    return header + bytes(fst) + string_section + b"".join(payloads)


class XBProbeTests(unittest.TestCase):
    def test_variant_and_exact_lookup_do_not_dump_data(self) -> None:
        archive = XBArchiveReader.from_bytes(
            _make_archive(
                [
                    ("data/menu/common.to", b"synthetic text", XBCompression.NONE),
                    ("data/chara/test.bin", b"synthetic bytes", XBCompression.NONE),
                ],
                name="face.xb0",
            ),
            name="face.xb0",
        )
        self.assertEqual(archive.variant.label, "xb0")
        self.assertEqual(archive.variant.locale_hint, "USE")
        entry = archive.lookup(r"data\menu\common.to")
        self.assertIsNotNone(entry)
        assert entry is not None
        self.assertEqual(archive.read_entry(entry), b"synthetic text")
        self.assertIsNone(archive.lookup("data/menu/missing.to"))
        metadata = archive.metadata()
        self.assertNotIn("synthetic text", repr(metadata))

    def test_big_endian_and_variant_are_explicit(self) -> None:
        archive = XBArchiveReader.from_bytes(
            _make_archive(
                [
                    ("data/test.bin", b"big-endian", XBCompression.NONE),
                    ("data/huf.bin", b"ABBAA", XBCompression.HUFFMAN),
                    ("data/deflate.bin", b"ABBAAB", XBCompression.DEFLATE),
                ],
                endian=">",
                name="face.xb3",
            ),
            endian=">",
            name="face.xb3",
        )
        self.assertEqual(archive.endian, ">")
        self.assertEqual(archive.variant.locale_hint, "SPA")
        self.assertEqual(archive.read_entry("data/test.bin"), b"big-endian")
        self.assertEqual(archive.read_entry("data/huf.bin"), b"ABBAA")
        self.assertEqual(archive.read_entry("data/deflate.bin"), b"ABBAAB")
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(
                _make_archive(
                    [("data/test.bin", b"big-endian", XBCompression.NONE)],
                    endian=">",
                )
            )

    def test_lzs_huffman_deflate_and_compressed_string_table(self) -> None:
        entries = [
            ("data/lzs.bin", b"lzs payload", XBCompression.LZS),
            ("data/huf.bin", b"ABBAA", XBCompression.HUFFMAN),
            ("data/deflate.bin", b"ABBAAB", XBCompression.DEFLATE),
        ]
        archive = XBArchiveReader.from_bytes(
            _make_archive(entries, string_table_compressed=True),
        )
        for path, data, _ in entries:
            self.assertEqual(archive.read_entry(path), data)

    def test_uncompressed_fallback_for_compression_tag(self) -> None:
        raw = b"fallback"
        archive_bytes = _make_archive(
            [("data/raw.bin", raw, XBCompression.NONE)],
        )
        # Change the FST tag to LZS and replace the raw data with its legal
        # compression-header fallback (compress_size == 0).
        data_start = archive_bytes.find(raw)
        payload = struct.pack("<II", len(raw), 0) + raw
        mutated = bytearray(archive_bytes[:data_start] + _pad4(payload))
        fst = 8
        original = struct.unpack_from("<I", mutated, fst + 4)[0]
        struct.pack_into("<I", mutated, fst + 4, (2 << 28) | (original & 0x0FFFFFFF))
        archive = XBArchiveReader.from_bytes(bytes(mutated))
        self.assertEqual(archive.read_entry("data/raw.bin"), raw)

    def test_rejects_traversal_absolute_and_duplicate_paths(self) -> None:
        for bad_name in ("../escape.bin", "/absolute.bin", "C:/drive.bin", "data/../x.bin"):
            with self.subTest(bad_name=bad_name), self.assertRaises(XBProbeError):
                XBArchiveReader.from_bytes(_make_archive([(bad_name, b"x", XBCompression.NONE)]))
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(
                _make_archive(
                    [
                        ("data/same.bin", b"a", XBCompression.NONE),
                        (r"data\same.bin", b"b", XBCompression.NONE),
                    ]
                )
            )

    def test_rejects_truncation_offsets_counts_sizes_and_compression(self) -> None:
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(b"xe\0\1")
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(b"xe\0\1" + struct.pack("<I", 0xFFFFFFFF))

        valid = bytearray(_make_archive([("data/test.bin", b"payload", XBCompression.NONE)]))
        # Offset points into the header/table rather than the data section.
        struct.pack_into("<I", valid, 12, 3 << 28)
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(bytes(valid))

        valid = bytearray(_make_archive([("data/test.bin", b"payload", XBCompression.NONE)]))
        struct.pack_into("<I", valid, 8, 0x100)
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(bytes(valid))

        valid = bytearray(_make_archive([("data/test.bin", b"payload", XBCompression.NONE)]))
        struct.pack_into("<I", valid, 12, (4 << 28) | (struct.unpack_from("<I", valid, 12)[0] & 0x0FFFFFFF))
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(bytes(valid))

        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(_make_archive([("data/test.bin", b"x", XBCompression.LZS)])[:-3])

        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(
                _make_archive([("data/test.bin", b"x", XBCompression.NONE)]),
                limits=XBLimits(max_entry_bytes=0),
            )

    def test_cli_metadata_and_lookup_are_bounded(self) -> None:
        data = _make_archive([("data/test.bin", b"private synthetic", XBCompression.NONE)])
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.xb2"
            path.write_bytes(data)
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                self.assertEqual(main([str(path), "--json", "--lookup", "data/test.bin"]), 0)
            self.assertIn('"compression": "none"', stdout.getvalue())
            self.assertNotIn("private synthetic", stdout.getvalue())
            stderr = io.StringIO()
            with redirect_stderr(stderr):
                self.assertEqual(main([str(path), "--lookup", "data/missing.bin"]), 2)
            self.assertIn("inner key not found", stderr.getvalue())

    def test_shortest_valid_archive(self) -> None:
        raw = _make_archive([("a.bin", b"x", XBCompression.NONE)])
        reader = XBArchiveReader.from_bytes(raw)
        self.assertEqual(len(reader.entries), 1)
        self.assertEqual(reader.read_entry("a.bin"), b"x")

    def test_limits_and_table_bounds(self) -> None:
        valid = _make_archive([("a.bin", b"x", XBCompression.NONE)])
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(valid, limits=XBLimits(max_archive_bytes=10))
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(valid, limits=XBLimits(max_files=0))
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(valid, limits=XBLimits(max_string_table_bytes=1))

    def test_total_expansion_limit(self) -> None:
        raw = _make_archive([("a.bin", b"x" * 100, XBCompression.NONE), ("b.bin", b"y" * 100, XBCompression.NONE)])
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(raw, limits=XBLimits(max_total_expanded_bytes=150))

    def test_string_table_corruptions(self) -> None:
        # Unterminated string table
        raw = _make_archive([("a.bin", b"x", XBCompression.NONE)])
        st_start = raw.find(b"\x01")
        mutated = bytearray(raw)
        mutated[st_start + 2] = ord("a")
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(bytes(mutated))

        # Name length mismatch
        mutated = bytearray(raw)
        mutated[st_start] = 5
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(bytes(mutated))

        # Bad name hash
        mutated = bytearray(raw)
        mutated[st_start + 1] ^= 0xFF
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(bytes(mutated))

    def test_path_validation_edge_cases(self) -> None:
        for bad_path in (
            "a//b.bin",
            "/a/b.bin",
            r"\a\b.bin",
            "C:/a/b.bin",
            "d:\\a\\b.bin",
            "a/./b.bin",
            "a/../b.bin",
            "a/b\x07.bin",
            "a/b\x7f.bin",
        ):
            with self.subTest(bad_path=bad_path), self.assertRaises(XBProbeError):
                XBArchiveReader.from_bytes(_make_archive([(bad_path, b"x", XBCompression.NONE)]))

    def test_zero_and_nonzero_padding(self) -> None:
        # Valid zero padding (4 bytes)
        raw = _make_archive([("a.bin", b"x", XBCompression.NONE)])
        padded = raw + b"\0\0\0\0"
        reader = XBArchiveReader.from_bytes(padded)
        self.assertEqual(reader.read_entry("a.bin"), b"x")

        # Non-zero invalid padding at end
        bad_padded = raw + b"\0\0\0\x01"
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(bad_padded)

    def test_lzs_malformed_inputs(self) -> None:
        body = b"\x01\x00"
        payload = struct.pack("<II", 10, len(body)) + body
        raw = _make_archive([("a.bin", b"0123456789", XBCompression.NONE)])
        data_start = raw.find(b"0123456789")
        mutated = bytearray(raw[:data_start] + _pad4(payload))
        struct.pack_into("<I", mutated, 12, (2 << 28) | (data_start // 4))
        reader = XBArchiveReader.from_bytes(bytes(mutated))
        with self.assertRaises(XBProbeError):
            reader.read_entry("a.bin")

        body = _lzs_body(b"x" * 20)
        payload = struct.pack("<II", 10, len(body)) + body
        mutated = bytearray(raw[:data_start] + _pad4(payload))
        struct.pack_into("<I", mutated, 8, 10)
        struct.pack_into("<I", mutated, 12, (2 << 28) | (data_start // 4))
        reader = XBArchiveReader.from_bytes(bytes(mutated))
        with self.assertRaises(XBProbeError):
            reader.read_entry("a.bin")

    def test_huffman_malformed_inputs(self) -> None:
        huf_table = bytes([1, 3, 65, 66, 67]) + b"\0"
        payload = struct.pack("<II", 3, len(huf_table)) + huf_table
        raw = _make_archive([("a.bin", b"ABC", XBCompression.NONE)])
        data_start = raw.find(b"ABC")
        mutated = bytearray(raw[:data_start] + _pad4(payload))
        struct.pack_into("<I", mutated, 8, 3)
        struct.pack_into("<I", mutated, 12, (1 << 28) | (data_start // 4))
        reader = XBArchiveReader.from_bytes(bytes(mutated))
        with self.assertRaises(XBProbeError):
            reader.read_entry("a.bin")

    def test_variant_labeling(self) -> None:
        self.assertEqual(variant_from_path("data.xb").label, "base")
        self.assertEqual(variant_from_path("data.xb0").label, "xb0")
        self.assertEqual(variant_from_path("data.xb0").locale_hint, "USE")
        self.assertEqual(variant_from_path("data.xb2").locale_hint, "FRE")
        self.assertEqual(variant_from_path("data.xb3").locale_hint, "SPA")
        self.assertEqual(variant_from_path("data.xb99").label, "xb99")
        self.assertIsNone(variant_from_path("data.xb99").locale_hint)

    def test_cli_error_exits_nonzero(self) -> None:
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            self.assertEqual(main(["nonexistent.xb"]), 2)
        self.assertIn("error:", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
