# SPDX-License-Identifier: GPL-3.0-or-later
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
    """A framed LZS stream; its size field counts the 8-byte header too."""

    body = _lzs_body(data)
    return struct.pack(endian + "II", len(data), 8 + len(body)) + body


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
    for width, _count in ((8, 254), (9, 4)):
        for symbol in range(256) if width == 8 else range(254, 258):
            if len(result) >= 254 and width == 8:
                break
            result.setdefault(symbol, (code, width))
            code += 1
            if width == 8 and len(result) == 254:
                break
        code <<= 1
    return result


def _huffman_table() -> bytes:
    """The code table matching :func:`_huffman_codes`, padded to an even length."""

    table = bytearray([9] + [0] * 7)
    table.extend((254,))
    table.extend(range(254))
    table.extend((4, 254, 255, 254, 255))
    if len(table) % 2:
        table.append(0)
    return bytes(table)


HUFFMAN_TABLE_LEN = len(_huffman_table())


def _huffman_body(data: bytes, endian: str = "<") -> bytes:
    """Code table plus bitstream.  A framed stream's size field counts only the
    bitstream, i.e. ``len(body) - HUFFMAN_TABLE_LEN``."""

    codes = _huffman_codes()
    table = _huffman_table()

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
    return table + bytes(stream)


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
        string_section = struct.pack(endian + "II", len(string_table), 8 + len(body)) + body
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
            payload = _lzs_stream(raw_data, endian)
        elif compression is XBCompression.HUFFMAN:
            body = _huffman_body(raw_data, endian)
            payload = struct.pack(endian + "II", len(raw_data), len(body) - HUFFMAN_TABLE_LEN) + body
        elif compression is XBCompression.DEFLATE:
            inner = _lzs_stream(raw_data, endian)
            body = _huffman_body(inner, endian)
            payload = struct.pack(endian + "II", len(inner), len(body) - HUFFMAN_TABLE_LEN) + body
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
        payload = struct.pack("<II", 10, 8 + len(body)) + body
        raw = _make_archive([("a.bin", b"0123456789", XBCompression.NONE)])
        data_start = raw.find(b"0123456789")
        mutated = bytearray(raw[:data_start] + _pad4(payload))
        struct.pack_into("<I", mutated, 12, (2 << 28) | (data_start // 4))
        reader = XBArchiveReader.from_bytes(bytes(mutated))
        with self.assertRaises(XBProbeError):
            reader.read_entry("a.bin")

        body = _lzs_body(b"x" * 20)
        payload = struct.pack("<II", 10, 8 + len(body)) + body
        mutated = bytearray(raw[:data_start] + _pad4(payload))
        struct.pack_into("<I", mutated, 8, 10)
        struct.pack_into("<I", mutated, 12, (2 << 28) | (data_start // 4))
        reader = XBArchiveReader.from_bytes(bytes(mutated))
        with self.assertRaises(XBProbeError):
            reader.read_entry("a.bin")

    def test_huffman_malformed_inputs(self) -> None:
        # Three codes cannot share a one-bit length, so the table is
        # oversubscribed.  The code table is walked to find the stream's extent,
        # so the archive is refused when it is opened.
        huf_table = bytes([1, 3, 65, 66, 67]) + b"\0"
        payload = struct.pack("<II", 3, 2) + huf_table + b"\0\0"
        raw = _make_archive([("a.bin", b"ABC", XBCompression.NONE)])
        data_start = raw.find(b"ABC")
        mutated = bytearray(raw[:data_start] + _pad4(payload))
        struct.pack_into("<I", mutated, 8, 3)
        struct.pack_into("<I", mutated, 12, (1 << 28) | (data_start // 4))
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(bytes(mutated))

    def test_huffman_truncated_bitstream(self) -> None:
        # A one-symbol table is valid, but 33 one-bit symbols need three
        # bitstream words and only one is stored.  The old reader substituted
        # zero words and fabricated repeated symbols instead.
        huf_table = bytes([1, 1, 65, 0])
        payload = struct.pack("<II", 33, 2) + huf_table + b"\0\0"
        raw = _make_archive([("a.bin", b"AAA", XBCompression.NONE)])
        data_start = raw.find(b"AAA")
        mutated = bytearray(raw[:data_start] + _pad4(payload))
        struct.pack_into("<I", mutated, 8, 33)
        struct.pack_into("<I", mutated, 12, (1 << 28) | (data_start // 4))
        reader = XBArchiveReader.from_bytes(bytes(mutated))
        with self.assertRaisesRegex(XBProbeError, "Huffman bitstream is truncated"):
            reader.read_entry("a.bin")

    def test_huffman_uses_buffered_symbols_before_refill(self) -> None:
        # A one-symbol table can decode sixteen one-bit symbols from one
        # buffered word.  Requiring another word after the seventh symbol
        # falsely rejected a valid stream with nine bits still buffered.
        huf_table = bytes([1, 1, 65, 0])
        payload = struct.pack("<II", 16, 2) + huf_table + b"\0\0"
        raw = _make_archive([("a.bin", b"AAA", XBCompression.NONE)])
        data_start = raw.find(b"AAA")
        mutated = bytearray(raw[:data_start] + _pad4(payload))
        struct.pack_into("<I", mutated, 8, 16)
        struct.pack_into("<I", mutated, 12, (1 << 28) | (data_start // 4))
        reader = XBArchiveReader.from_bytes(bytes(mutated))
        self.assertEqual(reader.read_entry("a.bin"), b"A" * 16)

    def test_archive_loading_uses_one_bounded_handle(self) -> None:
        raw = _make_archive([("a.bin", b"payload", XBCompression.NONE)])
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.xb"
            path.write_bytes(raw)
            original_read_bytes = Path.read_bytes

            def forbidden_read_bytes(self):
                raise AssertionError("archive must not use an unbounded path read")

            Path.read_bytes = forbidden_read_bytes
            try:
                reader = XBArchiveReader(path)
            finally:
                Path.read_bytes = original_read_bytes
            self.assertEqual(reader.read_entry("a.bin"), b"payload")

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

    def test_accepts_unpadded_final_entry_and_rejects_actual_truncation(self) -> None:
        raw = _make_archive([("a.bin", b"x", XBCompression.NONE)])
        self.assertEqual(len(raw), 36)
        # Entry starts at offset 32 with 1 byte of payload.
        # Lengths 33..36 are complete (33 is unpadded, 34..36 have 1..3 zero padding bytes).
        for complete_len in (33, 34, 35, 36):
            with self.subTest(complete_len=complete_len):
                reader = XBArchiveReader.from_bytes(raw[:complete_len])
                self.assertEqual(reader.read_entry("a.bin"), b"x")

        # An archive shorter than stored payload is genuinely truncated.
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(raw[:32])

        raw_multi = _make_archive([("a.bin", b"abcdef", XBCompression.NONE)])
        # Offset 32 + 6 bytes payload = 38 bytes unpadded.
        reader = XBArchiveReader.from_bytes(raw_multi[:38])
        self.assertEqual(reader.read_entry("a.bin"), b"abcdef")
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(raw_multi[:37])

        # Unpadded compressed entry is also valid.
        raw_lzs = _make_archive([("a.bin", b"x", XBCompression.LZS)])
        # Offset 32 + 8-byte header + 2-byte LZS body = 42 bytes unpadded.
        reader = XBArchiveReader.from_bytes(raw_lzs[:42])
        self.assertEqual(reader.read_entry("a.bin"), b"x")
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(raw_lzs[:41])

        # Non-zero trailing bytes in padding must still be rejected.
        corrupt_padding = raw[:34] + b"\x01"
        with self.assertRaises(XBProbeError):
            XBArchiveReader.from_bytes(corrupt_padding)

    def test_handles_incremental_file_reads(self) -> None:
        raw = _make_archive([("a.bin", b"payload", XBCompression.NONE)])
        import tempfile

        class ShortReadHandle:
            def __init__(self, data: bytes, real_file):
                self._data = data
                self._pos = 0
                self._real_file = real_file
            def __enter__(self):
                return self
            def __exit__(self, *args):
                self._real_file.close()

            def fileno(self) -> int:
                return self._real_file.fileno()

            def read(self, size: int = -1) -> bytes:
                if self._pos >= len(self._data):
                    return b""
                chunk_len = min(5, len(self._data) - self._pos)
                if size >= 0:
                    chunk_len = min(chunk_len, size)
                chunk = self._data[self._pos : self._pos + chunk_len]
                self._pos += chunk_len
                return chunk

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.xb"
            path.write_bytes(raw)

            real_open = Path.open

            def custom_open(self_path, *args, **kwargs):
                if kwargs.get("mode") == "rb" or (args and args[0] == "rb"):
                    f = real_open(self_path, *args, **kwargs)
                    return ShortReadHandle(raw, f)
                return real_open(self_path, *args, **kwargs)

            Path.open = custom_open
            try:
                reader = XBArchiveReader(path)
                self.assertEqual(reader.read_entry("a.bin"), b"payload")
            finally:
                Path.open = real_open


def _raw_archive(
    entries: list[tuple[str, int, XBCompression, bytes]],
    *,
    string_section: bytes | None = None,
) -> bytes:
    """Build an archive from ``(path, fst_size, compression, payload)`` rows.

    Each payload is stored verbatim (padded to four bytes), so a test decides
    exactly what the stream's own header claims.  ``string_section`` replaces the
    default raw string table, header included, to exercise the compressed form.
    """

    names = []
    for path, _, _, _ in entries:
        raw = path.encode("shift_jis")
        names.append(bytes((len(raw), _sjis_hash(raw))) + raw + b"\0")
    string_table = b"".join(names)
    if string_section is None:
        string_section = struct.pack("<II", len(string_table), 0) + string_table
    string_section = _pad4(string_section)
    header = b"xe\0\1" + struct.pack("<I", len(entries))
    offset = len(header) + len(entries) * 8 + len(string_section)
    fst = bytearray()
    payloads = []
    for _, fst_size, compression, payload in entries:
        payload = _pad4(payload)
        fst.extend(struct.pack("<II", fst_size, (int(compression) << 28) | (offset // 4)))
        payloads.append(payload)
        offset += len(payload)
    return header + bytes(fst) + string_section + b"".join(payloads)


def _names_table(paths: list[str]) -> bytes:
    names = []
    for path in paths:
        raw = path.encode("shift_jis")
        names.append(bytes((len(raw), _sjis_hash(raw))) + raw + b"\0")
    return b"".join(names)


class RetailFramingTests(unittest.TestCase):
    """The size fields of retail archives, reproduced with synthetic bytes.

    Each accepted shape below was a refusal before: the reader took the LZS and
    string-table size field for the body length and the Huffman size field for
    the whole stream, so it read the next section's bytes as "trailing data".
    The rejected shapes are the ones that must keep failing closed.
    """

    def _open(self, entries, **kwargs) -> XBArchiveReader:
        return XBArchiveReader.from_bytes(_raw_archive(entries, **kwargs))

    # -- accepted -----------------------------------------------------------

    def test_lzs_size_field_counts_its_own_header_at_every_alignment(self) -> None:
        # Stream lengths 9+n for n in 1..9 cover every padding amount, 0..3.
        members = [(f"data/m{n}.bin", bytes(range(65, 65 + n))) for n in range(1, 10)]
        rows = [(path, len(data), XBCompression.LZS, _lzs_stream(data)) for path, data in members]
        archive = self._open(rows)
        paddings = set()
        for (path, data), entry in zip(members, archive.entries, strict=True):
            self.assertEqual(archive.read_entry(path), data)
            self.assertEqual(entry.stored_size, 9 + len(data))
            paddings.add(entry.span_size - entry.stored_size)
        self.assertEqual(paddings, {0, 1, 2, 3})

    def test_compressed_string_table_size_field_counts_its_own_header(self) -> None:
        paths = [f"data/menu/entry_number_{n}.bin" for n in range(3)]
        table = _names_table(paths)
        body = _lzs_body(table)
        self.assertGreater(len(body), 8)
        section = struct.pack("<II", len(table), 8 + len(body)) + body
        rows = [(path, 1, XBCompression.NONE, b"x") for path in paths]
        archive = self._open(rows, string_section=section)
        self.assertEqual([entry.path for entry in archive.entries], paths)
        self.assertEqual(archive.read_entry(paths[1]), b"x")

    def test_huffman_size_field_counts_the_bitstream_not_the_code_table(self) -> None:
        data = b"ABBAAB" * 5
        body = _huffman_body(data)
        stream = body[HUFFMAN_TABLE_LEN:]
        payload = struct.pack("<II", len(data), len(stream)) + body
        archive = self._open([("data/h.bin", len(data), XBCompression.HUFFMAN, payload)])
        entry = archive.entries[0]
        self.assertEqual(entry.stored_size, 8 + HUFFMAN_TABLE_LEN + len(stream))
        self.assertEqual(archive.read_entry("data/h.bin"), data)

    def test_deflate_nests_a_self_counting_lzs_stream_in_a_huffman_stream(self) -> None:
        data = b"nested payload" * 3
        inner = _lzs_stream(data)
        self.assertEqual(struct.unpack_from("<I", inner, 4)[0], len(inner))
        body = _huffman_body(inner)
        stream = body[HUFFMAN_TABLE_LEN:]
        payload = struct.pack("<II", len(inner), len(stream)) + body
        archive = self._open([("data/d.bin", len(data), XBCompression.DEFLATE, payload)])
        self.assertEqual(archive.entries[0].stored_size, 8 + HUFFMAN_TABLE_LEN + len(stream))
        self.assertEqual(archive.read_entry("data/d.bin"), data)

    def test_zero_alignment_after_a_stream_is_accepted_but_not_required(self) -> None:
        data = b"abc"
        stream = _lzs_stream(data)
        for tail in (b"", b"\0", b"\0\0\0"):
            with self.subTest(tail=tail):
                archive = self._open(
                    [("data/a.bin", len(data), XBCompression.LZS, stream + tail)]
                )
                self.assertEqual(archive.read_entry("data/a.bin"), data)

    def test_zero_lookahead_words_after_the_bitstream_are_accepted(self) -> None:
        data = b"ABBAAB"
        body = _huffman_body(data)
        stream = body[HUFFMAN_TABLE_LEN:] + bytes(4)
        payload = struct.pack("<II", len(data), len(stream)) + body + bytes(4)
        archive = self._open([("data/h.bin", len(data), XBCompression.HUFFMAN, payload)])
        self.assertEqual(archive.read_entry("data/h.bin"), data)

    # -- still refused ------------------------------------------------------

    def test_lzs_size_field_that_cannot_cover_its_header_is_refused(self) -> None:
        data = b"abcdef"
        body = _lzs_body(data)
        for field in (1, 8):
            with self.subTest(field=field):
                payload = struct.pack("<II", len(data), field) + body
                with self.assertRaisesRegex(XBProbeError, "does not fit its FST span"):
                    self._open([("data/a.bin", len(data), XBCompression.LZS, payload)])

    def test_lzs_stream_that_under_reports_its_size_is_refused(self) -> None:
        # Writing the body length (the old reading of the field) leaves body bytes
        # outside the stream; they are not zero, so they cannot be padding.
        data = bytes(range(65, 85))
        body = _lzs_body(data)
        payload = struct.pack("<II", len(data), len(body)) + body
        with self.assertRaisesRegex(XBProbeError, "non-zero trailing bytes"):
            self._open([("data/a.bin", len(data), XBCompression.LZS, payload)])

    def test_stream_that_runs_past_the_end_of_the_archive_is_refused(self) -> None:
        data = b"abcdef"
        body = _lzs_body(data)
        lzs = struct.pack("<II", len(data), 8 + len(body) + 16) + body
        with self.assertRaisesRegex(XBProbeError, "does not fit its FST span"):
            self._open([("data/a.bin", len(data), XBCompression.LZS, lzs)])
        huffman_body = _huffman_body(b"ABBAAB")
        huffman = struct.pack("<II", 6, len(huffman_body) - HUFFMAN_TABLE_LEN + 16) + huffman_body
        with self.assertRaisesRegex(XBProbeError, "does not fit its FST span"):
            self._open([("data/h.bin", 6, XBCompression.HUFFMAN, huffman)])

    def test_non_zero_bytes_after_a_stream_are_not_padding(self) -> None:
        data = b"abc"
        stream = _lzs_stream(data)
        for tail in (b"\x01", b"\0\0\0\x01", b"\0" * 8 + b"\x7f"):
            with self.subTest(tail=tail):
                with self.assertRaisesRegex(XBProbeError, "non-zero trailing bytes"):
                    self._open([("data/a.bin", len(data), XBCompression.LZS, stream + tail)])

    def test_lzs_size_field_that_overstates_the_body_must_cover_only_zeros(self) -> None:
        data = b"abc"
        body = _lzs_body(data)
        zero = struct.pack("<II", len(data), 8 + len(body) + 4) + body + bytes(4)
        archive = self._open([("data/a.bin", len(data), XBCompression.LZS, zero)])
        self.assertEqual(archive.read_entry("data/a.bin"), data)
        dirty = struct.pack("<II", len(data), 8 + len(body) + 4) + body + b"\0\0\0\x01"
        archive = self._open([("data/a.bin", len(data), XBCompression.LZS, dirty)])
        with self.assertRaisesRegex(XBProbeError, "non-zero trailing bytes in LZS body"):
            archive.read_entry("data/a.bin")

    def test_huffman_size_field_that_overstates_the_bitstream_must_cover_only_zeros(self) -> None:
        data = b"ABBAAB"
        body = _huffman_body(data)
        field = len(body) - HUFFMAN_TABLE_LEN + 2
        dirty = struct.pack("<II", len(data), field) + body + b"\x01\0"
        archive = self._open([("data/h.bin", len(data), XBCompression.HUFFMAN, dirty)])
        with self.assertRaisesRegex(XBProbeError, "non-zero trailing bytes in Huffman"):
            archive.read_entry("data/h.bin")

    def test_deflate_inner_stream_must_account_for_its_whole_intermediate(self) -> None:
        data = b"nested payload"
        inner = _lzs_stream(data)
        for tail, ok in ((bytes(2), True), (b"\x01\0", False)):
            with self.subTest(tail=tail):
                body = _huffman_body(inner + tail)
                payload = struct.pack("<II", len(inner) + len(tail), len(body) - HUFFMAN_TABLE_LEN) + body
                archive = self._open([("data/d.bin", len(data), XBCompression.DEFLATE, payload)])
                if ok:
                    self.assertEqual(archive.read_entry("data/d.bin"), data)
                else:
                    with self.assertRaisesRegex(XBProbeError, "non-zero trailing bytes in nested LZS"):
                        archive.read_entry("data/d.bin")

    def test_string_table_size_field_is_checked(self) -> None:
        paths = [f"data/menu/entry_number_{n}.bin" for n in range(3)]
        table = _names_table(paths)
        body = _lzs_body(table)
        rows = [(path, 1, XBCompression.NONE, b"x") for path in paths]
        # A field that cannot cover its own header.
        for field in (1, 8):
            with self.subTest(field=field):
                section = struct.pack("<II", len(table), field) + body
                with self.assertRaisesRegex(XBProbeError, "does not cover its header"):
                    self._open(rows, string_section=section)
        # The body length written as the field (the old reading) cuts the stream
        # short, and a short LZS stream cannot fill the declared table.
        section = struct.pack("<II", len(table), len(body)) + body
        with self.assertRaises(XBProbeError):
            self._open(rows, string_section=section)


if __name__ == "__main__":
    unittest.main()
