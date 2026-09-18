# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Synthetic, retail-free coverage for XB extraction containment and limits.

Every fixture here is generated in-process.  No archive, texture, name, or hash
from a retail title appears in this file.
"""

from __future__ import annotations

from concurrent.futures import Future
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import extract_xb  # noqa: E402
from extract_xb import (  # noqa: E402
    DEFAULT_MAX_WORKERS,
    EXPLICIT_MAX_WORKERS,
    OversizedInputError,
    UnsafeArchivePathError,
    canonical_root,
    normalize_member_path,
    prepare_destination,
    preflight_archive_members,
    process_extracted_directory,
    read_bounded,
    resolve_workers,
    safe_join,
    save_png,
    verify_extracted_tree,
)
from test_xb_probe import (  # noqa: E402
    _huffman_codes,
    _lzs_body,
    _make_archive,
    _pad4,
    _reverse_bits,
)
from test_extract_xb_gim import gim, image_block, palette_block  # noqa: E402
from xb_probe import XBArchiveReader, XBCompression, XBLimits, _sjis_hash  # noqa: E402


BACKSLASH = chr(92)
ONE_MIB = 1024 * 1024


def _write_archive(directory: str, entries, name: str = "fixture.xb") -> str:
    """Materialize a synthetic XB archive so the extractor can open it."""

    path = os.path.join(directory, name)
    with open(path, "wb") as handle:
        handle.write(_make_archive(entries))
    return path


def _make_raw_archive(entries) -> bytes:
    """Build an archive where the FST size is chosen independently of the payload.

    ``_make_archive`` derives every field from the member bytes, which is right
    for well-formed fixtures but cannot express the hostile family: an entry
    whose declared FST expanded size disagrees with what its nested compression
    layer claims.  Each entry here is ``(path, fst_expanded_size, compression,
    payload)`` and the payload is stored verbatim.
    """

    names = []
    for path, _, _, _ in entries:
        raw = path.encode("shift_jis")
        names.append(bytes((len(raw), _sjis_hash(raw))) + raw + b"\0")
    string_table = b"".join(names)
    string_section = _pad4(struct.pack("<II", len(string_table), 0) + string_table)

    header = b"xe\0\1" + struct.pack("<I", len(entries))
    data_start = len(header) + len(entries) * 8 + len(string_section)
    fst = bytearray()
    payloads = []
    offset = data_start
    for _, fst_size, compression, payload in entries:
        payload = _pad4(payload)
        fst.extend(
            struct.pack("<II", fst_size, (int(compression) << 28) | (offset // 4))
        )
        payloads.append(payload)
        offset += len(payload)
    return header + bytes(fst) + string_section + b"".join(payloads)


def _huffman_stream(data: bytes) -> bytes:
    """Little-endian Huffman body with the same table as ``_huffman_body``.

    ``test_xb_probe._huffman_body`` accumulates the whole bitstream into one
    Python integer, which is quadratic and unusable at the megabyte sizes the
    hostile fixtures need.  This emits 16-bit words from a rolling accumulator
    instead; the table header and bit order are identical, so the decoder
    cannot tell the two apart.
    """

    codes = _huffman_codes()
    table = bytearray([9] + [0] * 7)
    table.extend((254,))
    table.extend(range(254))
    table.extend((4, 254, 255, 254, 255))
    if len(table) % 2:
        table.append(0)

    reversed_codes = {
        symbol: (_reverse_bits(code, width), width)
        for symbol, (code, width) in codes.items()
    }
    out = bytearray()
    acc = 0
    nbits = 0
    for value in data:
        bits, width = reversed_codes[value]
        acc |= bits << nbits
        nbits += width
        while nbits >= 16:
            out += (acc & 0xFFFF).to_bytes(2, "little")
            acc >>= 16
            nbits -= 16
    if nbits:
        out += (acc & 0xFFFF).to_bytes(2, "little")
    return bytes(table) + bytes(out)


def _deflate_payload(expanded: bytes) -> bytes:
    """A well-formed DEFLATE payload: Huffman wrapping an inner LZS stream."""

    body = _lzs_body(expanded)
    inner = struct.pack("<II", len(expanded), len(body)) + body
    # The Huffman decoder always reads ahead, so the stream needs trailing
    # lookahead words; they are zero and pass the decoder's padding check.
    huff = _huffman_stream(inner) + bytes(8)
    return struct.pack("<II", len(inner), len(huff)) + huff


def _write_raw_archive(directory: str, entries, name: str = "fixture.xb") -> str:
    path = os.path.join(directory, name)
    with open(path, "wb") as handle:
        handle.write(_make_raw_archive(entries))
    return path


class _ImportBlocker:
    """A meta-path finder that records and refuses one module name."""

    def __init__(self, name: str) -> None:
        self.name = name
        self.attempts: list[str] = []

    def find_module(self, fullname, path=None):  # pragma: no cover - legacy API
        return None

    def find_spec(self, fullname, path=None, target=None):
        if fullname == self.name or fullname.startswith(self.name + "."):
            self.attempts.append(fullname)
            raise ImportError(f"{fullname} must not be imported by production code")
        return None


def _gim_bytes(payload_len: int = 32) -> bytes:
    """A GIM-magic blob whose contents are irrelevant to the size gate."""

    return b"MIG.00.1PSP" + b"\0" * 5 + b"\0" * payload_len


def _has_output(dest: str) -> bool:
    """True if a refused archive left anything behind, staging included.

    A refused archive should not create its destination at all: the tree is
    built in staging and only promoted on full success.
    """

    if os.path.lexists(dest + ".staging"):
        return True
    if not os.path.exists(dest):
        return False
    return bool(os.listdir(dest))


class NormalizeMemberPathTests(unittest.TestCase):
    def test_dot_dot_traversal_is_rejected_in_both_separator_forms(self) -> None:
        for name in (
            "../escape",
            ".." + BACKSLASH + "escape",
            "data/../../escape",
            "data" + BACKSLASH + ".." + BACKSLASH + ".." + BACKSLASH + "escape",
            "..",
            ".",
            "data/./menu",
            "data/..",
        ):
            with self.subTest(name=name):
                with self.assertRaises(UnsafeArchivePathError):
                    normalize_member_path(name)

    def test_rooted_and_drive_qualified_names_are_rejected(self) -> None:
        for name in (
            "/etc/passwd",
            BACKSLASH + "Windows" + BACKSLASH + "System32",
            "C:" + BACKSLASH + "Windows" + BACKSLASH + "System32",
            "c:relative",
            "Z:/data",
        ):
            with self.subTest(name=name):
                with self.assertRaises(UnsafeArchivePathError):
                    normalize_member_path(name)

    def test_unc_and_device_forms_are_rejected(self) -> None:
        four = BACKSLASH * 2
        for name in (
            four + "server" + BACKSLASH + "share" + BACKSLASH + "x",
            "//server/share/x",
            four + "?" + BACKSLASH + "C:" + BACKSLASH + "x",
            four + "." + BACKSLASH + "PhysicalDrive0",
            "//?/UNC/server/share/x",
        ):
            with self.subTest(name=name):
                with self.assertRaises(UnsafeArchivePathError):
                    normalize_member_path(name)

    def test_empty_control_and_oversize_components_are_rejected(self) -> None:
        for name in (
            "",
            "data//menu",
            "data/" + BACKSLASH + "menu",
            "data/menu" + chr(0) + ".to",
            "data/menu" + chr(1) + ".to",
            "data/menu" + chr(127) + ".to",
            "x" * (extract_xb.MAX_MEMBER_COMPONENT_CHARS + 1),
            "y" * (extract_xb.MAX_MEMBER_PATH_CHARS + 1),
        ):
            with self.subTest(name=name):
                with self.assertRaises(UnsafeArchivePathError):
                    normalize_member_path(name)
        with self.assertRaises(UnsafeArchivePathError):
            normalize_member_path(None)  # type: ignore[arg-type]

    def test_reserved_device_names_are_rejected(self) -> None:
        for name in ("NUL", "nul", "data/CON", "COM1.txt", "lpt9.bin", "CONIN$", "aux"):
            with self.subTest(name=name):
                with self.assertRaises(UnsafeArchivePathError):
                    normalize_member_path(name)

    def test_trailing_dot_or_space_and_forbidden_characters_are_rejected(self) -> None:
        for name in (
            "data/menu.",
            "data/menu ",
            "data./menu",
            "data /menu",
            "data/a<b",
            "data/a>b",
            'data/a"b',
            "data/a|b",
            "data/a?b",
            "data/a*b",
            "data/a:b",
        ):
            with self.subTest(name=name):
                with self.assertRaises(UnsafeArchivePathError):
                    normalize_member_path(name)

    def test_valid_nested_and_mixed_separator_paths_are_accepted(self) -> None:
        self.assertEqual(
            normalize_member_path("data/menu/text/common.to"),
            "data/menu/text/common.to",
        )
        self.assertEqual(
            normalize_member_path("data" + BACKSLASH + "menu/text" + BACKSLASH + "c.to"),
            "data/menu/text/c.to",
        )
        # A dot inside a component, and a name that merely starts with dots, are
        # ordinary names -- containment must not over-reject them.
        self.assertEqual(normalize_member_path("a/..b/c.bin"), "a/..b/c.bin")
        self.assertEqual(normalize_member_path("single.bin"), "single.bin")


class SafeJoinTests(unittest.TestCase):
    def test_valid_member_lands_under_the_canonical_root(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            joined = safe_join(root, "data" + BACKSLASH + "menu/x.to")
            root_real = canonical_root(root)
            self.assertTrue(joined.startswith(root_real + os.sep))
            self.assertEqual(
                os.path.relpath(joined, root_real).replace(os.sep, "/"),
                "data/menu/x.to",
            )

    def test_escaping_members_never_produce_a_path(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            for name in (
                ".." + BACKSLASH + "escape",
                "../escape",
                "C:" + BACKSLASH + "escape",
                "/escape",
            ):
                with self.subTest(name=name):
                    with self.assertRaises(UnsafeArchivePathError):
                        safe_join(root, name)

    def test_root_resolution_rejects_an_escaping_relative_path(self) -> None:
        # contained_path is the layer below the lexical normalizer, and it is
        # what makes containment a property of the resolved location rather
        # than of the string.  Exercise it directly with input the normalizer
        # would already have refused.
        with tempfile.TemporaryDirectory() as base:
            root = os.path.join(base, "root")
            os.makedirs(root)
            root_real = canonical_root(root)
            for rel in ("../escape.bin", "data/../../escape.bin"):
                with self.subTest(rel=rel):
                    with self.assertRaises(UnsafeArchivePathError) as caught:
                        extract_xb.contained_path(root_real, rel)
                    self.assertIn("escapes the extraction root", str(caught.exception))
            self.assertEqual(
                extract_xb.contained_path(root_real, "data/menu/x.to"),
                os.path.join(root_real, "data", "menu", "x.to"),
            )

    def test_a_symlinked_destination_component_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            root = os.path.join(base, "root")
            outside = os.path.join(base, "outside")
            os.makedirs(os.path.join(root))
            os.makedirs(outside)
            try:
                os.symlink(outside, os.path.join(root, "link"), target_is_directory=True)
            except (OSError, NotImplementedError, AttributeError) as exc:
                self.skipTest(f"symlink creation unavailable: {exc}")
            with self.assertRaises(UnsafeArchivePathError):
                safe_join(root, "link/payload.bin")


class VerifyExtractedTreeTests(unittest.TestCase):
    def test_a_clean_tree_passes(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "data", "menu"))
            with open(os.path.join(root, "data", "menu", "x.to"), "wb") as handle:
                handle.write(b"synthetic")
            verify_extracted_tree(root)

    def test_a_symlink_inside_the_tree_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            root = os.path.join(base, "root")
            outside = os.path.join(base, "outside")
            os.makedirs(root)
            os.makedirs(outside)
            try:
                os.symlink(outside, os.path.join(root, "link"), target_is_directory=True)
            except (OSError, NotImplementedError, AttributeError) as exc:
                self.skipTest(f"symlink creation unavailable: {exc}")
            with self.assertRaises(UnsafeArchivePathError):
                verify_extracted_tree(root)


class PreflightTests(unittest.TestCase):
    def test_a_safe_archive_preflights_to_its_member_list(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base,
                [
                    ("data/menu/common.to", b"synthetic text", XBCompression.NONE),
                    (
                        "data" + BACKSLASH + "chara" + BACKSLASH + "t.bin",
                        b"synthetic bytes",
                        XBCompression.NONE,
                    ),
                ],
            )
            dest = os.path.join(base, "out")
            os.makedirs(dest)
            self.assertEqual(
                preflight_archive_members(archive, dest),
                ["data/menu/common.to", "data/chara/t.bin"],
            )

    def test_a_reserved_device_member_refuses_the_whole_archive(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base,
                [
                    ("data/menu/common.to", b"synthetic text", XBCompression.NONE),
                    ("data/NUL", b"synthetic bytes", XBCompression.NONE),
                ],
            )
            dest = os.path.join(base, "out")
            os.makedirs(dest)
            with self.assertRaises(UnsafeArchivePathError):
                preflight_archive_members(archive, dest)

    def test_colliding_member_names_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base,
                [
                    ("data/menu/common.to", b"first", XBCompression.NONE),
                    ("data" + BACKSLASH + "menu" + BACKSLASH + "Common.to",
                     b"second", XBCompression.NONE),
                ],
            )
            dest = os.path.join(base, "out")
            os.makedirs(dest)
            with self.assertRaises(UnsafeArchivePathError) as caught:
                preflight_archive_members(archive, dest)
            self.assertIn("colliding", str(caught.exception))

    def test_case_colliding_directory_components_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base,
                [
                    ("Foo/a.bin", b"first", XBCompression.NONE),
                    ("foo/b.bin", b"second", XBCompression.NONE),
                ],
            )
            dest = os.path.join(base, "out")
            os.makedirs(dest)
            with self.assertRaises(UnsafeArchivePathError) as caught:
                preflight_archive_members(archive, dest)
            self.assertIn("path components", str(caught.exception))

    def test_file_directory_prefix_collisions_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base,
                [
                    ("data/menu", b"file", XBCompression.NONE),
                    ("data/menu/item.bin", b"nested", XBCompression.NONE),
                ],
            )
            dest = os.path.join(base, "out")
            os.makedirs(dest)
            with self.assertRaises(UnsafeArchivePathError) as caught:
                preflight_archive_members(archive, dest)
            self.assertIn("existing file", str(caught.exception))

    def test_an_unparsable_archive_is_refused_rather_than_extracted(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = os.path.join(base, "broken.xb")
            with open(archive, "wb") as handle:
                handle.write(b"not an xb archive at all")
            dest = os.path.join(base, "out")
            os.makedirs(dest)
            with self.assertRaises(UnsafeArchivePathError) as caught:
                preflight_archive_members(archive, dest)
            self.assertIn("cannot be validated", str(caught.exception))


class ContainmentMutantTests(unittest.TestCase):
    """Removing the containment check must make these fixtures escape."""

    def test_mutant_lexical_only_normalizer_admits_a_reserved_device_name(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base, [("data/NUL", b"synthetic bytes", XBCompression.NONE)]
            )
            dest = os.path.join(base, "out")
            os.makedirs(dest)

            # Control: the real boundary refuses the archive.
            with self.assertRaises(UnsafeArchivePathError):
                preflight_archive_members(archive, dest)

            # Mutant: replace the repository-owned normalizer with the purely
            # lexical behavior recorded for the reference extractor.  The unsafe
            # member is now accepted, which is what makes the check load-bearing.
            original = extract_xb.normalize_member_path
            extract_xb.normalize_member_path = lambda name: name.replace(BACKSLASH, "/")
            try:
                self.assertEqual(
                    preflight_archive_members(archive, dest), ["data/NUL"]
                )
            finally:
                extract_xb.normalize_member_path = original

            # And the boundary is restored afterwards.
            with self.assertRaises(UnsafeArchivePathError):
                preflight_archive_members(archive, dest)

    def test_mutant_without_root_resolution_lets_traversal_escape(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            root = os.path.join(base, "root")
            os.makedirs(root)
            root_real = canonical_root(root)
            escaping = "data/../../escape.bin"

            # Control: the real boundary refuses to produce a path at all.
            with self.assertRaises(UnsafeArchivePathError):
                safe_join(root, escaping)

            original_normalize = extract_xb.normalize_member_path
            original_contained = extract_xb.contained_path
            extract_xb.normalize_member_path = lambda name: name.replace(BACKSLASH, "/")
            extract_xb.contained_path = lambda root_arg, rel: os.path.join(
                root_arg, *rel.split("/")
            )
            try:
                mutated = extract_xb.safe_join(root, escaping)
            finally:
                extract_xb.normalize_member_path = original_normalize
                extract_xb.contained_path = original_contained

            # The mutant writes outside the extraction root.
            self.assertFalse(
                os.path.realpath(mutated).startswith(root_real + os.sep)
            )


class _FakeLibxbModule:
    """A libxb that explodes if production ever reaches for it.

    Production extraction is repository-owned, so this module must stay
    untouched: ``calls`` records any use, and importing it at all is enough to
    fail the no-fallback test below.
    """

    class XBOpenMode:
        READ = "read"

    class XBEndian:
        LITTLE = "little"

    def __init__(self) -> None:
        self.calls = []
        module = self

        class XBArchive:
            def __init__(self, path, mode, endian, verbose=False):
                self.path = path

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

            def extract_all(self, path):
                module.calls.append((self.path, path))
                target = os.path.join(path, "data", "menu")
                os.makedirs(target, exist_ok=True)
                with open(os.path.join(target, "common.to"), "wb") as handle:
                    handle.write(b"synthetic text")

        self.XBArchive = XBArchive


class ExtractOneTests(unittest.TestCase):
    """End-to-end behavior of the repository-owned extractor."""

    def setUp(self) -> None:
        self.fake = _FakeLibxbModule()
        self._saved = sys.modules.get("libxb")
        sys.modules["libxb"] = self.fake  # type: ignore[assignment]

    def tearDown(self) -> None:
        if self._saved is None:
            sys.modules.pop("libxb", None)
        else:
            sys.modules["libxb"] = self._saved

    def test_a_safe_archive_is_extracted_without_the_optional_dependency(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base, [("data/menu/common.to", b"synthetic text", XBCompression.NONE)]
            )
            dest = os.path.join(base, "out")
            _, status, err = extract_xb.extract_one(archive, dest)
            self.assertEqual((status, err), ("ok", None))
            self.assertEqual(self.fake.calls, [])
            member = os.path.join(dest, "data", "menu", "common.to")
            self.assertTrue(os.path.isfile(member))
            self.assertEqual(Path(member).read_bytes(), b"synthetic text")
            self.assertTrue(os.path.isfile(os.path.join(dest, "inventory_map.json")))
            self.assertFalse(os.path.lexists(dest + ".staging"))

    def test_production_extraction_never_imports_libxb(self) -> None:
        # The strongest form of "no fallback": remove the module entirely and
        # make any import of it fail, then extract successfully anyway.
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base, [("data/menu/common.to", b"synthetic text", XBCompression.NONE)]
            )
            blocker = _ImportBlocker("libxb")
            sys.modules.pop("libxb", None)
            sys.meta_path.insert(0, blocker)
            try:
                _, status, err = extract_xb.extract_one(
                    archive, os.path.join(base, "out")
                )
            finally:
                sys.meta_path.remove(blocker)
            self.assertEqual((status, err), ("ok", None))
            self.assertEqual(blocker.attempts, [])

    def test_an_unsafe_archive_writes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base,
                [
                    ("data/menu/common.to", b"synthetic text", XBCompression.NONE),
                    ("data/COM1.bin", b"synthetic bytes", XBCompression.NONE),
                ],
            )
            dest = os.path.join(base, "out")
            _, status, err = extract_xb.extract_one(archive, dest)
            self.assertEqual(status, "failed")
            self.assertIn("reserved device name", err)
            self.assertEqual(self.fake.calls, [])
            self.assertFalse(_has_output(dest))

    def test_an_unparsable_archive_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = os.path.join(base, "broken.xb")
            with open(archive, "wb") as handle:
                handle.write(b"not an xb archive at all")
            _, status, err = extract_xb.extract_one(archive, os.path.join(base, "out"))
            self.assertEqual(status, "failed")
            self.assertIn("cannot be validated", err)
            self.assertEqual(self.fake.calls, [])

    def test_a_populated_destination_is_skipped_without_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base, [("data/menu/common.to", b"synthetic text", XBCompression.NONE)]
            )
            dest = os.path.join(base, "out")
            os.makedirs(dest)
            with open(os.path.join(dest, "stale.bin"), "wb") as handle:
                handle.write(b"previous run")
            _, status, _ = extract_xb.extract_one(archive, dest)
            self.assertEqual(status, "skipped")
            self.assertFalse(os.path.exists(os.path.join(dest, "data")))

            _, status, err = extract_xb.extract_one(archive, dest, overwrite=True)
            self.assertEqual((status, err), ("ok", None))
            self.assertTrue(
                os.path.isfile(os.path.join(dest, "data", "menu", "common.to"))
            )
            # Merging must not remove what the caller did not replace.
            self.assertTrue(os.path.isfile(os.path.join(dest, "stale.bin")))
            self.assertFalse(os.path.lexists(dest + ".staging"))


class NestedCompressionBudgetTests(unittest.TestCase):
    """The hostile family: a nested layer that produces more than it declares.

    Validating names and outer archive structure says nothing about how many
    bytes a nested DEFLATE -> LZS layer will produce.  These fixtures are the
    exact shape that defeated the earlier design, at a bounded 1 MiB; no test
    here tries to exhaust memory.
    """

    def test_hostile_nested_lzs_expansion_is_rejected_before_any_write(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            # FST says 1 byte; the inner LZS stream declares (and can produce)
            # 1 MiB.  The earlier design accepted this and wrote ~1,049,599
            # bytes because the decode happened outside the repository.
            archive = _write_raw_archive(
                base,
                [
                    (
                        "data/menu/common.to",
                        1,
                        XBCompression.DEFLATE,
                        _deflate_payload(bytes(ONE_MIB)),
                    )
                ],
            )
            dest = os.path.join(base, "out")
            _, status, err = extract_xb.extract_one(archive, dest)
            self.assertEqual(status, "failed")
            self.assertIn("disagrees with FST", err)
            # Nothing reached the filesystem, in the destination or in staging.
            self.assertFalse(_has_output(dest))

    def test_hostile_nested_expansion_is_rejected_by_the_preflight_too(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = _write_raw_archive(
                base,
                [
                    (
                        "data/menu/common.to",
                        1,
                        XBCompression.DEFLATE,
                        _deflate_payload(bytes(ONE_MIB)),
                    )
                ],
            )
            dest = os.path.join(base, "out")
            os.makedirs(dest)
            with self.assertRaises(UnsafeArchivePathError) as caught:
                extract_xb.preflight_archive_members(archive, dest)
            self.assertIn("disagrees with FST", str(caught.exception))
            self.assertFalse(_has_output(dest))

    def test_valid_nested_deflate_lzs_still_extracts_byte_for_byte(self) -> None:
        payload = bytes(ONE_MIB)
        with tempfile.TemporaryDirectory() as base:
            archive = _write_raw_archive(
                base,
                [
                    (
                        "data/menu/common.to",
                        len(payload),
                        XBCompression.DEFLATE,
                        _deflate_payload(payload),
                    )
                ],
            )
            dest = os.path.join(base, "out")
            _, status, err = extract_xb.extract_one(archive, dest)
            self.assertEqual((status, err), ("ok", None))
            written = Path(dest, "data", "menu", "common.to").read_bytes()
            self.assertEqual(len(written), ONE_MIB)
            self.assertEqual(written, payload)

    def test_nested_expansion_over_the_per_entry_budget_is_rejected(self) -> None:
        payload = bytes(ONE_MIB)
        with tempfile.TemporaryDirectory() as base:
            archive = _write_raw_archive(
                base,
                [
                    (
                        "data/menu/common.to",
                        len(payload),
                        XBCompression.DEFLATE,
                        _deflate_payload(payload),
                    )
                ],
            )
            dest = os.path.join(base, "out")
            original = extract_xb.MAX_ENTRY_DECODED_BYTES
            extract_xb.MAX_ENTRY_DECODED_BYTES = ONE_MIB // 2
            try:
                _, status, err = extract_xb.extract_one(archive, dest)
            finally:
                extract_xb.MAX_ENTRY_DECODED_BYTES = original
            self.assertEqual(status, "failed")
            self.assertFalse(_has_output(dest))

    def test_aggregate_decoded_budget_is_enforced_across_members(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base,
                [
                    (f"data/part{i}.bin", bytes(64), XBCompression.NONE)
                    for i in range(4)
                ],
            )
            dest = os.path.join(base, "out")
            original = extract_xb.MAX_ARCHIVE_DECODED_BYTES
            extract_xb.MAX_ARCHIVE_DECODED_BYTES = 100
            try:
                _, status, err = extract_xb.extract_one(archive, dest)
            finally:
                extract_xb.MAX_ARCHIVE_DECODED_BYTES = original
            self.assertEqual(status, "failed")
            self.assertFalse(_has_output(dest))

    def test_extractor_aggregate_accounting_does_not_rely_on_the_reader(self) -> None:
        # Give the reader deliberately permissive limits so only this module's
        # own running total can reject the archive.
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base,
                [
                    (f"data/part{i}.bin", bytes(64), XBCompression.NONE)
                    for i in range(4)
                ],
            )
            dest = os.path.join(base, "out")
            os.makedirs(dest)
            reader = XBArchiveReader(archive, limits=XBLimits())
            plan = extract_xb.plan_archive_members(reader, dest)
            self.assertEqual(len(plan), 4)

            original = extract_xb.MAX_ARCHIVE_DECODED_BYTES
            extract_xb.MAX_ARCHIVE_DECODED_BYTES = 100
            try:
                with self.assertRaises(UnsafeArchivePathError) as caught:
                    extract_xb._decode_within_budget(reader, plan, sink=None)
                self.assertIn("aggregate decode budget", str(caught.exception))
                # And the same plan succeeds once the budget allows it.
                extract_xb.MAX_ARCHIVE_DECODED_BYTES = 1024
                self.assertEqual(
                    extract_xb._decode_within_budget(reader, plan, sink=None), 256
                )
            finally:
                extract_xb.MAX_ARCHIVE_DECODED_BYTES = original


class PathIdentityTests(unittest.TestCase):
    """VALIDATED_MEMBER_PATH == WRITTEN_MEMBER_PATH, on every host."""

    def test_the_writer_receives_exactly_the_validated_identity(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base,
                [
                    (
                        "data" + BACKSLASH + "menu" + BACKSLASH + "common.to",
                        b"synthetic text",
                        XBCompression.NONE,
                    ),
                    ("data/chara/t.bin", b"synthetic bytes", XBCompression.NONE),
                ],
            )
            dest = os.path.join(base, "out")
            os.makedirs(dest)
            reader = XBArchiveReader(archive, limits=extract_xb.extractor_limits())
            validated = [rel for rel, _ in extract_xb.plan_archive_members(reader, dest)]

            handed_to_writer = []
            original = extract_xb.write_member

            def recording(root_real, rel_posix, data, overwrite=False):
                handed_to_writer.append(rel_posix)
                return original(root_real, rel_posix, data, overwrite=overwrite)

            extract_xb.write_member = recording
            try:
                extract_xb.extract_archive(archive, dest)
            finally:
                extract_xb.write_member = original

            self.assertEqual(handed_to_writer, validated)
            self.assertEqual(
                validated, ["data/menu/common.to", "data/chara/t.bin"]
            )

    def test_backslash_members_become_directories_on_every_host(self) -> None:
        # The regression that motivated this: a dependency that rewrote '/' to
        # '\' before writing produced a separator on Windows and an ordinary
        # filename character on POSIX.  The archive grammar decides, so both
        # separators always produce the same tree.
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base,
                [
                    (
                        "data" + BACKSLASH + "menu" + BACKSLASH + "common.to",
                        b"synthetic text",
                        XBCompression.NONE,
                    )
                ],
            )
            dest = os.path.join(base, "out")
            _, status, err = extract_xb.extract_one(archive, dest)
            self.assertEqual((status, err), ("ok", None))
            self.assertTrue(os.path.isdir(os.path.join(dest, "data", "menu")))
            self.assertTrue(
                os.path.isfile(os.path.join(dest, "data", "menu", "common.to"))
            )
            # No component anywhere in the produced tree carries a literal
            # backslash, which is what a POSIX host would otherwise create.
            for _dirpath, dirnames, filenames in os.walk(dest):
                for name in dirnames + filenames:
                    self.assertNotIn(BACKSLASH, name)

    def test_member_components_are_the_split_used_for_containment(self) -> None:
        self.assertEqual(
            extract_xb.member_components(
                normalize_member_path("a" + BACKSLASH + "b/c.bin")
            ),
            ["a", "b", "c.bin"],
        )


class _RecordingExecutor:
    """A synchronous executor that records peak outstanding futures.

    Real ``Future`` objects are handed back so ``concurrent.futures.wait`` is
    exercised for real; they are simply already resolved.
    """

    def __init__(self, owner):
        self.owner = owner

    def submit(self, fn, *args):
        self.owner.outstanding += 1
        self.owner.peak = max(self.owner.peak, self.owner.outstanding)
        self.owner.submitted += 1
        future = Future()
        future.set_result(fn(*args))
        return future


def _identity(value):
    """Worker body for the submission tests; deliberately does no accounting."""

    return value


class BoundedSubmissionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.outstanding = 0
        self.peak = 0
        self.submitted = 0

    def test_a_large_work_list_never_exceeds_the_in_flight_bound(self) -> None:
        executor = _RecordingExecutor(self)
        work = [(i,) for i in range(5000)]
        bound = extract_xb.max_in_flight_tasks(4)

        results = []
        for value in extract_xb.submit_bounded(executor, _identity, work, bound):
            # A future is only retired once its result has been handed back, so
            # `outstanding` is the count of live futures and `peak` is the high
            # water mark across the whole run.
            self.outstanding -= 1
            results.append(value)

        self.assertEqual(self.submitted, 5000)
        self.assertEqual(sorted(results), list(range(5000)))
        self.assertLessEqual(self.peak, bound)
        self.assertGreater(self.peak, 0)
        self.assertEqual(self.outstanding, 0)

    def test_the_bound_scales_with_workers_and_is_never_zero(self) -> None:
        self.assertEqual(
            extract_xb.max_in_flight_tasks(4),
            4 * extract_xb.IN_FLIGHT_TASKS_PER_WORKER,
        )
        self.assertGreaterEqual(extract_xb.max_in_flight_tasks(0), 1)
        with self.assertRaises(ValueError):
            list(extract_xb.submit_bounded(_RecordingExecutor(self), int, [], 0))


class ResourceLimitTests(unittest.TestCase):
    def test_read_bounded_rejects_oversize_and_accepts_within_budget(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            path = os.path.join(base, "blob.bin")
            with open(path, "wb") as handle:
                handle.write(b"a" * 64)
            self.assertEqual(len(read_bounded(path, 64)), 64)
            with self.assertRaises(OversizedInputError):
                read_bounded(path, 63)

    def test_oversized_gim_is_rejected_before_any_data_is_read(self) -> None:
        with tempfile.TemporaryDirectory() as dest:
            path = os.path.join(dest, "big.gim")
            with open(path, "wb") as handle:
                handle.write(_gim_bytes(256))

            # The candidate must be opened for fstat(), but an oversized file
            # must be rejected before any bytes are read from that handle.
            original_read = extract_xb.os.read

            def exploding_read(*args, **kwargs):
                raise AssertionError("candidate bytes were read before the size gate")

            extract_xb.os.read = exploding_read
            try:
                inventory = process_extracted_directory(dest, max_gim_bytes=16)
            finally:
                extract_xb.os.read = original_read

            self.assertEqual(inventory["textures"], [])
            self.assertEqual(len(inventory["rejected"]), 1)
            self.assertEqual(inventory["rejected"][0]["name"], "big.gim")
            self.assertIn("budget", inventory["rejected"][0]["reason"])

    def test_read_bounded_uses_the_open_handle_for_size_and_data(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            path = os.path.join(base, "blob.bin")
            payload = b"synthetic bounded bytes"
            with open(path, "wb") as handle:
                handle.write(payload)

            original_getsize = extract_xb.os.path.getsize

            def exploding_getsize(*args, **kwargs):
                raise AssertionError("path-based stat bypassed the open handle")

            extract_xb.os.path.getsize = exploding_getsize
            try:
                self.assertEqual(read_bounded(path, len(payload)), payload)
            finally:
                extract_xb.os.path.getsize = original_getsize

    def test_gim_within_budget_is_still_decoded(self) -> None:
        with tempfile.TemporaryDirectory() as dest:
            # A minimal valid T8 image block plus palette, built the same way as
            # the malformed-GIM fixtures in test_extract_xb_gim.py.
            content = bytearray(36)
            struct.pack_into("<H", content, 4, 5)
            struct.pack_into("<H", content, 8, 2)
            struct.pack_into("<H", content, 10, 2)
            struct.pack_into("<I", content, 28, 36)
            struct.pack_into("<I", content, 32, 40)
            image = bytes(content) + b"\0\0\0\0"
            pal = bytearray(36)
            struct.pack_into("<I", pal, 28, 36)
            struct.pack_into("<I", pal, 32, 40)
            palette = bytes(pal) + b"\x00\x00\xff\xff"

            def block(block_id, body):
                header = bytearray(16)
                struct.pack_into("<H", header, 0, block_id)
                struct.pack_into("<I", header, 4, 16 + len(body))
                struct.pack_into("<I", header, 12, 16)
                return bytes(header) + body

            blob = b"MIG.00.1PSP" + b"\0" * 5 + block(0x0004, image) + block(0x0005, palette)
            with open(os.path.join(dest, "small.gim"), "wb") as handle:
                handle.write(blob)

            inventory = process_extracted_directory(dest)
            self.assertEqual(len(inventory["textures"]), 1)
            self.assertEqual(inventory["rejected"], [])
            self.assertTrue(os.path.isfile(os.path.join(dest, "small.png")))

    def test_default_worker_count_is_capped_below_the_cpu_count(self) -> None:
        default = resolve_workers(None)
        self.assertGreaterEqual(default, 1)
        self.assertLessEqual(default, DEFAULT_MAX_WORKERS)
        self.assertLessEqual(default, max(1, os.cpu_count() or 1))

    def test_explicit_worker_override_is_bounded(self) -> None:
        self.assertEqual(resolve_workers(1), 1)
        self.assertEqual(resolve_workers(EXPLICIT_MAX_WORKERS), EXPLICIT_MAX_WORKERS)
        for bad in (0, -1, EXPLICIT_MAX_WORKERS + 1, "8", 2.0, True):
            with self.subTest(bad=bad):
                self.assertIsNone(resolve_workers(bad))


class OverwriteTests(unittest.TestCase):
    def test_a_populated_destination_is_not_reused_by_default(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            dest = os.path.join(base, "out")
            os.makedirs(dest)
            with open(os.path.join(dest, "existing.bin"), "wb") as handle:
                handle.write(b"synthetic")
            with self.assertRaises(FileExistsError):
                prepare_destination(dest)
            self.assertEqual(prepare_destination(dest, overwrite=True), canonical_root(dest))

    def test_an_empty_or_missing_destination_is_created(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            dest = os.path.join(base, "out", "nested")
            # Only the parent is created; the destination itself appears when
            # a staging tree is promoted onto it.
            self.assertEqual(prepare_destination(dest), canonical_root(dest))
            self.assertTrue(os.path.isdir(os.path.dirname(dest)))
            self.assertFalse(os.path.exists(dest))

    def test_save_png_does_not_clobber_an_existing_file_by_default(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            target = os.path.join(base, "x.png")
            with open(target, "wb") as handle:
                handle.write(b"pre-existing")
            with self.assertRaises(FileExistsError):
                save_png(1, 1, bytearray(4), target)
            self.assertEqual(Path(target).read_bytes(), b"pre-existing")
            save_png(1, 1, bytearray(4), target, overwrite=True)
            self.assertTrue(Path(target).read_bytes().startswith(b"\x89PNG"))

    def test_a_png_name_collision_is_recorded_instead_of_overwritten(self) -> None:
        with tempfile.TemporaryDirectory() as dest:
            with open(os.path.join(dest, "tex.gim"), "wb") as handle:
                handle.write(_gim_bytes(16))
            with open(os.path.join(dest, "tex.png"), "wb") as handle:
                handle.write(b"member that already owns this name")
            inventory = process_extracted_directory(dest)
            self.assertEqual(inventory["textures"], [])
            self.assertEqual(
                Path(dest, "tex.png").read_bytes(),
                b"member that already owns this name",
            )


    def test_extraction_keeps_archive_member_when_png_name_collides(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            gim_data = gim(
                image_block(raw=b"\0\0\0\0"),
                palette_block(raw=b"\x00\x00\xff\xff"),
            )
            archive = _write_archive(
                base,
                [
                    ("tex.gim", gim_data, XBCompression.NONE),
                    ("tex.png", b"archive-owned member", XBCompression.NONE),
                ],
            )
            dest = os.path.join(base, "out")
            _, status, err = extract_xb.extract_one(archive, dest, overwrite=True)
            self.assertEqual((status, err), ("ok", None))
            self.assertEqual(
                Path(dest, "tex.png").read_bytes(), b"archive-owned member"
            )


class PromotionTests(unittest.TestCase):
    def test_promotion_preflights_type_conflicts_before_replacing(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            staging = os.path.join(base, "staging")
            dest = os.path.join(base, "dest")
            os.makedirs(staging)
            os.makedirs(dest)
            Path(staging, "a.bin").write_bytes(b"new-a")
            Path(staging, "b.bin").write_bytes(b"new-b")
            Path(dest, "a.bin").write_bytes(b"old-a")
            os.makedirs(Path(dest, "b.bin"))

            with self.assertRaises(UnsafeArchivePathError):
                extract_xb._promote_staging(staging, dest)
            self.assertEqual(Path(dest, "a.bin").read_bytes(), b"old-a")
            self.assertTrue(Path(dest, "b.bin").is_dir())

    def test_promotion_rolls_back_when_a_later_replace_fails(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            staging = os.path.join(base, "staging")
            dest = os.path.join(base, "dest")
            os.makedirs(staging)
            os.makedirs(dest)
            Path(staging, "a.bin").write_bytes(b"new-a")
            Path(staging, "b.bin").write_bytes(b"new-b")
            Path(dest, "a.bin").write_bytes(b"old-a")
            Path(dest, "b.bin").write_bytes(b"old-b")

            original_replace = extract_xb.os.replace
            failing_source = os.path.join(staging, "b.bin")
            failing_target = os.path.join(dest, "b.bin")

            def fail_second_file(source, target):
                if source == failing_source and target == failing_target:
                    raise OSError("synthetic promotion failure")
                return original_replace(source, target)

            extract_xb.os.replace = fail_second_file
            try:
                with self.assertRaisesRegex(OSError, "synthetic promotion failure"):
                    extract_xb._promote_staging(staging, dest)
            finally:
                extract_xb.os.replace = original_replace

            self.assertEqual(Path(dest, "a.bin").read_bytes(), b"old-a")
            self.assertEqual(Path(dest, "b.bin").read_bytes(), b"old-b")

    def test_post_commit_cleanup_failure_does_not_rollback_replacements(self) -> None:
        with tempfile.TemporaryDirectory() as base:
            staging = os.path.join(base, "staging")
            dest = os.path.join(base, "dest")
            os.makedirs(staging)
            os.makedirs(dest)
            Path(staging, "a.bin").write_bytes(b"new-a")
            Path(staging, "b.bin").write_bytes(b"new-b")
            Path(dest, "a.bin").write_bytes(b"old-a")
            Path(dest, "b.bin").write_bytes(b"old-b")

            original_discard = extract_xb._discard_staging

            def fail_cleanup(_staging):
                raise OSError("synthetic cleanup failure")

            extract_xb._discard_staging = fail_cleanup
            try:
                self.assertFalse(extract_xb._promote_staging(staging, dest))
            finally:
                extract_xb._discard_staging = original_discard

            self.assertEqual(Path(dest, "a.bin").read_bytes(), b"new-a")
            self.assertEqual(Path(dest, "b.bin").read_bytes(), b"new-b")
            self.assertTrue(os.path.isdir(staging))


class WriteExclusiveTests(unittest.TestCase):
    """Write loop progress invariants and exclusive file creation."""

    def test_positive_short_writes_continue_until_complete(self) -> None:
        payload = bytes(range(256)) * 4  # 1024 bytes
        original_write = extract_xb.os.write
        chunks_written = []

        def short_write(fd, buf):
            step = min(37, len(buf))
            chunk = memoryview(buf)[:step]
            written = original_write(fd, chunk)
            chunks_written.append(written)
            return written

        extract_xb.os.write = short_write
        try:
            with tempfile.TemporaryDirectory() as base:
                target = os.path.join(base, "short.bin")
                extract_xb._write_exclusive(target, payload)
                self.assertEqual(Path(target).read_bytes(), payload)
                self.assertGreater(len(chunks_written), 1)
                self.assertEqual(sum(chunks_written), len(payload))
        finally:
            extract_xb.os.write = original_write

    def test_positive_short_writes_succeed_in_extraction(self) -> None:
        payload = b"synthetic text for multi-chunk write testing " * 10
        original_write = extract_xb.os.write

        def short_write(fd, buf):
            step = min(17, len(buf))
            return original_write(fd, memoryview(buf)[:step])

        extract_xb.os.write = short_write
        try:
            with tempfile.TemporaryDirectory() as base:
                archive = _write_archive(
                    base, [("data/menu/common.to", payload, XBCompression.NONE)]
                )
                dest = os.path.join(base, "out")
                _, status, err = extract_xb.extract_one(archive, dest)
                self.assertEqual((status, err), ("ok", None))
                written = Path(dest, "data", "menu", "common.to").read_bytes()
                self.assertEqual(written, payload)
                self.assertFalse(os.path.lexists(dest + ".staging"))
        finally:
            extract_xb.os.write = original_write

    def test_zero_progress_write_raises_deterministic_oserror_without_looping(
        self,
    ) -> None:
        original_write = extract_xb.os.write
        call_count = 0

        def zero_write(fd, buf):
            nonlocal call_count
            call_count += 1
            if call_count > 5:
                raise AssertionError(
                    "infinite write loop detected in _write_exclusive: "
                    "zero return was not rejected"
                )
            return 0

        extract_xb.os.write = zero_write
        try:
            with tempfile.TemporaryDirectory() as base:
                target = os.path.join(base, "out.bin")
                with self.assertRaises(OSError) as caught:
                    extract_xb._write_exclusive(target, b"test payload")
                self.assertEqual(call_count, 1)
                self.assertIn("no progress", str(caught.exception).lower())
        finally:
            extract_xb.os.write = original_write

    def test_zero_progress_during_extraction_fails_closed_and_cleans_staging(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as base:
            archive = _write_archive(
                base, [("data/menu/common.to", b"synthetic text", XBCompression.NONE)]
            )
            dest = os.path.join(base, "out")
            original_write = extract_xb.os.write
            call_count = 0

            def failing_zero_write(fd, buf):
                nonlocal call_count
                call_count += 1
                if call_count > 5:
                    raise AssertionError("infinite loop in _write_exclusive")
                return 0

            extract_xb.os.write = failing_zero_write
            try:
                _, status, err = extract_xb.extract_one(archive, dest)
            finally:
                extract_xb.os.write = original_write

            self.assertEqual(status, "failed")
            self.assertIn("no progress", str(err).lower())
            self.assertFalse(_has_output(dest))

    def test_negative_write_count_raises_deterministic_oserror(self) -> None:
        original_write = extract_xb.os.write

        def negative_write(fd, buf):
            return -1

        extract_xb.os.write = negative_write
        try:
            with tempfile.TemporaryDirectory() as base:
                target = os.path.join(base, "out.bin")
                with self.assertRaises(OSError) as caught:
                    extract_xb._write_exclusive(target, b"test payload")
                self.assertIn("no progress", str(caught.exception).lower())
        finally:
            extract_xb.os.write = original_write

    def test_excessive_write_count_raises_deterministic_oserror(self) -> None:
        original_write = extract_xb.os.write

        def overflow_write(fd, buf):
            return len(buf) + 1

        extract_xb.os.write = overflow_write
        try:
            with tempfile.TemporaryDirectory() as base:
                target = os.path.join(base, "out.bin")
                with self.assertRaises(OSError) as caught:
                    extract_xb._write_exclusive(target, b"test payload")
                self.assertIn("larger than remaining", str(caught.exception).lower())
        finally:
            extract_xb.os.write = original_write

    def test_empty_payload_creates_empty_file_without_calling_write(self) -> None:
        original_write = extract_xb.os.write
        calls = 0

        def counting_write(fd, buf):
            nonlocal calls
            calls += 1
            return original_write(fd, buf)

        extract_xb.os.write = counting_write
        try:
            with tempfile.TemporaryDirectory() as base:
                target = os.path.join(base, "empty.bin")
                extract_xb._write_exclusive(target, b"")
                self.assertEqual(calls, 0)
                self.assertEqual(Path(target).read_bytes(), b"")
        finally:
            extract_xb.os.write = original_write


if __name__ == "__main__":
    unittest.main()
