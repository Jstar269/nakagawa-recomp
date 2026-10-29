# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Compressed disc images reach the Python tools through the C wrapper.

``tools/nk_core/disc_expand_tool.py`` builds ``nk_disc_expand`` from the same C
code the player runs (``src/core/nk_disc_expand.c``) and ``iso_inspect`` and
``nk_cli.py bringup`` expand a .cso, .zso or .chd through it before reading
the plain .iso. These tests build every image here from a synthetic ISO (no
game data): a CSO with real DEFLATE blocks from ``zlib``, a ZSO with LZ4 run
blocks, and a CHD v5 DVD image with an uncompressed map. The block decoders
themselves are covered in depth by ``tests/native/test_disc_expand.c``.

The writers are importable (``build_cso``, ``build_zso``, ``build_chd``) so
the player UI test makes its compressed image the same way.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zlib

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from nk_core import disc_expand_tool  # noqa: E402
from nk_core.iso_inspect import IsoInspectionError, inspect_iso  # noqa: E402
from test_iso_parity import create_test_iso_with_executables  # noqa: E402

SECTOR = 2048
PLAIN_FLAG = 0x80000000


def build_cso(iso: bytes, *, version: int = 1, align: int = 0) -> bytes:
    """A CISO image: raw DEFLATE blocks, stored blocks where DEFLATE does not shrink."""
    blocks = len(iso) // SECTOR
    out = bytearray(b"CISO" + struct.pack("<IQIBB2x", 24, len(iso), SECTOR, version, align))
    index_at = len(out)
    out += bytes((blocks + 1) * 4)
    entries = []
    for i in range(blocks + 1):
        while len(out) % (1 << align):
            out.append(0)
        entry = len(out) >> align
        if i < blocks:
            block = iso[i * SECTOR:(i + 1) * SECTOR]
            packer = zlib.compressobj(9, zlib.DEFLATED, -15)
            packed = packer.compress(block) + packer.flush()
            if len(packed) < SECTOR:
                out += packed
            else:
                out += block
                entry |= PLAIN_FLAG if version < 2 else 0
        entries.append(entry)
    struct.pack_into(f"<{blocks + 1}I", out, index_at, *entries)
    return bytes(out)


def _lz4_runs(block: bytes) -> bytes:
    """One LZ4 block: runs of a repeated byte become offset-1 matches."""
    out = bytearray()
    anchor = 0
    pos = 0
    end = len(block)

    def length_bytes(value: int) -> bytes:
        return b"\xff" * (value // 255) + bytes([value % 255])

    def sequence(literals: bytes, match: int) -> None:
        token = (min(len(literals), 15) << 4) | (min(match - 4, 15) if match else 0)
        out.append(token)
        if len(literals) >= 15:
            out.extend(length_bytes(len(literals) - 15))
        out.extend(literals)
        if match:
            out.extend(struct.pack("<H", 1))
            if match - 4 >= 15:
                out.extend(length_bytes(match - 19))

    while pos + 12 <= end:
        run = 0
        limit = end - 5 - (pos + 1)
        while run < limit and block[pos + 1 + run] == block[pos]:
            run += 1
        if run >= 4:
            sequence(block[anchor:pos + 1], run)
            pos += 1 + run
            anchor = pos
        else:
            pos += 1
    sequence(block[anchor:], 0)
    return bytes(out)


def build_zso(iso: bytes) -> bytes:
    """A ZISO image: LZ4 blocks, stored blocks where LZ4 does not shrink."""
    blocks = len(iso) // SECTOR
    out = bytearray(b"ZISO" + struct.pack("<IQIBB2x", 24, len(iso), SECTOR, 1, 0))
    index_at = len(out)
    out += bytes((blocks + 1) * 4)
    entries = []
    for i in range(blocks + 1):
        entry = len(out)
        if i < blocks:
            block = iso[i * SECTOR:(i + 1) * SECTOR]
            packed = _lz4_runs(block)
            if len(packed) < SECTOR:
                out += packed
            else:
                out += block
                entry |= PLAIN_FLAG
        entries.append(entry)
    struct.pack_into(f"<{blocks + 1}I", out, index_at, *entries)
    return bytes(out)


def build_chd(iso: bytes, *, metadata_tag: bytes = b"DVD ") -> bytes:
    """A CHD v5 image with an uncompressed map; all-zero hunks are left unstored."""
    hunk = 4096
    hunks = (len(iso) + hunk - 1) // hunk
    header = bytearray(124)
    header[0:8] = b"MComprHD"
    struct.pack_into(">II", header, 8, 124, 5)
    struct.pack_into(">QQQII", header, 32, len(iso), 0, 124, hunk, SECTOR)
    meta_text = b"synthetic\x00"
    meta = metadata_tag + struct.pack(">IQ", len(meta_text), 0) + meta_text
    map_offset = 124 + len(meta)
    struct.pack_into(">Q", header, 40, map_offset)
    out = bytearray(header + meta + bytes(hunks * 4))
    out += bytes(-len(out) % hunk)
    for index in range(hunks):
        data = iso[index * hunk:(index + 1) * hunk].ljust(hunk, b"\x00")
        if index and not any(data):
            continue
        struct.pack_into(">I", out, map_offset + index * 4, len(out) // hunk)
        out += data
    return bytes(out)


BUILDERS = {"cso": build_cso, "zso": build_zso, "chd": build_chd}


def _have_compiler() -> bool:
    return bool(os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc"))


@unittest.skipUnless(_have_compiler(), "no C compiler to build nk_disc_expand (SKIP, not a pass)")
class DiscExpandToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory(prefix="nk-disc-expand-")
        cls.root = Path(cls._tmp.name)
        cls.iso_path = cls.root / "synthetic.iso"
        create_test_iso_with_executables(
            cls.iso_path, b"synthetic input executable\n", disc_id="TEST00042",
            title="Synthetic Compressed Disc",
        )
        cls.iso = cls.iso_path.read_bytes()
        cls.images = {}
        for name, build in BUILDERS.items():
            image = cls.root / f"synthetic.{name}"
            image.write_bytes(build(cls.iso))
            cls.images[name] = image
        disc_expand_tool.find_or_build_tool()

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def test_magic_names_each_format_and_nothing_else(self) -> None:
        for name, image in self.images.items():
            with self.subTest(name=name):
                self.assertEqual(disc_expand_tool.compressed_format(image), name)
        self.assertIsNone(disc_expand_tool.compressed_format(self.iso_path))
        self.assertIsNone(disc_expand_tool.compressed_format(self.root / "missing.cso"))

    def test_probe_reports_the_expanded_size(self) -> None:
        for name, image in self.images.items():
            with self.subTest(name=name):
                result = disc_expand_tool.run_tool("probe", str(image))
                self.assertEqual(result, {"status": "OK", "format": name,
                                          "expanded_bytes": len(self.iso)})

    def test_import_expands_once_into_the_per_title_folder(self) -> None:
        for name, image in self.images.items():
            with self.subTest(name=name):
                data_root = self.root / f"data-{name}"
                first = disc_expand_tool.import_image(image, data_root)
                expected = data_root / "discs" / "TEST00042" / f"TEST00042.{name}.iso"
                self.assertEqual(Path(first["iso_path"]), expected)
                self.assertEqual(first["disc_id"], "TEST00042")
                self.assertFalse(first["reused"])
                self.assertEqual(expected.read_bytes(), self.iso)
                again = disc_expand_tool.import_image(image, data_root)
                self.assertTrue(again["reused"])
                self.assertEqual(again["iso_path"], first["iso_path"])

    def test_iso_inspect_reads_each_format_as_its_iso(self) -> None:
        plain = inspect_iso(self.iso_path)
        for name, image in self.images.items():
            with self.subTest(name=name):
                data_root = self.root / f"inspect-{name}"
                info = inspect_iso(image, user_data_root=data_root)
                self.assertEqual((info.disc_id, info.title, info.version, info.size_bytes),
                                 (plain.disc_id, plain.title, plain.version, plain.size_bytes))
                self.assertEqual(info.container_metadata["format"], "iso9660")
                self.assertEqual(info.container_metadata["source_format"], name)
                expanded = data_root / "discs" / "TEST00042" / f"TEST00042.{name}.iso"
                self.assertEqual(expanded.read_bytes(), self.iso)

    def test_a_refused_image_keeps_its_named_boundary(self) -> None:
        bad = bytearray(self.images["cso"].read_bytes())
        bad[20] = 3  # CSO format version 3
        path = self.root / "bad-version.cso"
        path.write_bytes(bytes(bad))
        with self.assertRaises(IsoInspectionError) as caught:
            inspect_iso(path, user_data_root=self.root / "data-bad")
        self.assertEqual(caught.exception.boundary_code, "DISC_IMAGE_VERSION_UNSUPPORTED")
        self.assertFalse((self.root / "data-bad" / "discs" / "TEST00042").exists())

    def test_a_cd_type_chd_is_refused_by_name(self) -> None:
        path = self.root / "cd-layout.chd"
        path.write_bytes(build_chd(self.iso, metadata_tag=b"CHT2"))
        with self.assertRaises(disc_expand_tool.DiscImageRefused) as caught:
            disc_expand_tool.import_image(path, self.root / "data-cd")
        self.assertEqual(caught.exception.code, "DISC_IMAGE_CHD_CD_UNSUPPORTED")

    def test_a_plain_iso_passes_through_unchanged(self) -> None:
        self.assertEqual(disc_expand_tool.ensure_plain_iso(self.iso_path, self.root / "unused"),
                         self.iso_path)
        self.assertFalse((self.root / "unused").exists())


@unittest.skipUnless(_have_compiler(), "no C compiler to build nk_disc_expand (SKIP, not a pass)")
class BringupAcceptsCompressedImagesTests(unittest.TestCase):
    def setUp(self) -> None:
        import nk_cli

        self.nk_cli = nk_cli
        tmp = tempfile.TemporaryDirectory(prefix="nk-disc-bringup-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.iso_path = self.root / "synthetic.iso"
        create_test_iso_with_executables(
            self.iso_path, b"synthetic input executable\n", disc_id="TEST00042",
            title="Synthetic Compressed Disc",
        )
        self.iso = self.iso_path.read_bytes()

    def _bringup(self, image: Path, case: str):
        report_path = self.root / case / "bringup.json"
        args = argparse.Namespace(
            iso=str(image), work_dir=str(self.root / case / "work"), report=str(report_path),
            launch_timeout=1, instruction_trace=False,
        )
        seen = []
        # The inspect stage passes the compressed handling, then stops at a
        # refused executable, so no package is built.
        preflight = {
            "checks": [
                {"code": "DISC_SFO", "status": "OK", "issues": []},
                {"code": "EXECUTABLE", "status": "UNSUPPORTED", "issues": [308]},
            ],
            "selected_executable": None,
            "modified_dump_cfw_loader": False,
        }

        def fake_preflight(iso_path, **_kwargs):
            seen.append(Path(iso_path))
            return preflight

        with (
            mock.patch.object(self.nk_cli, "inspect_compatibility_preflight",
                              side_effect=fake_preflight),
            mock.patch("builtins.print"),
        ):
            status = self.nk_cli.cmd_bringup(args)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.nk_cli.validate_bringup_report(report)
        return status, report, seen

    def test_bringup_reads_the_expanded_iso_of_each_format(self) -> None:
        for name, build in BUILDERS.items():
            with self.subTest(name=name):
                image = self.root / f"synthetic.{name}"
                image.write_bytes(build(self.iso))
                status, report, seen = self._bringup(image, f"case-{name}")
                self.assertEqual(status, 1)
                self.assertEqual(report["failure_class"], "EXECUTABLE_UNSUPPORTED")
                self.assertEqual(len(seen), 1)
                self.assertEqual(seen[0].name, f"TEST00042.{name}.iso")
                self.assertEqual(seen[0].read_bytes(), self.iso)
                user_data = (self.root / f"case-{name}" / "work" / "user-data").resolve()
                self.assertTrue(seen[0].resolve().is_relative_to(user_data))

    def test_bringup_names_a_refused_image(self) -> None:
        bad = bytearray(build_cso(self.iso))
        struct.pack_into("<Q", bad, 8, 1 << 62)  # an expanded size no disc has
        image = self.root / "bad-size.cso"
        image.write_bytes(bytes(bad))
        status, report, seen = self._bringup(image, "case-bad")
        self.assertEqual(status, 1)
        self.assertEqual(report["reached_stage"], "inspect")
        self.assertEqual(report["failure_class"], "DISC_IMAGE_REFUSED")
        self.assertEqual(seen, [])


if __name__ == "__main__":
    unittest.main()
