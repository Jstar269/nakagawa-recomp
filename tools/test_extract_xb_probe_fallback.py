# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Verify that extract_xb.py can extract XB archives via xb_probe without libxb."""

from __future__ import annotations

from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from extract_xb import extract_one
from test_xb_probe import _make_archive
from xb_probe import XBCompression


class ExtractXbProbeFallbackTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.mkdtemp(prefix="test_extract_xb_")

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_extract_one_using_xb_probe(self) -> None:
        entries = [
            ("data/test/hello.txt", b"Hello from synthetic XB!", XBCompression.NONE),
            ("data/test/compressed.bin", b"A" * 120 + b"B" * 60, XBCompression.LZS),
        ]
        archive_bytes = _make_archive(entries, name="test.xb")
        archive_path = Path(self.temp_dir) / "test.xb"
        archive_path.write_bytes(archive_bytes)

        out_dir = Path(self.temp_dir) / "output"
        arc_ret, ok, err = extract_one(str(archive_path), str(out_dir))

        self.assertTrue(ok, f"extract_one failed with: {err}")
        self.assertIsNone(err)

        extracted_hello = out_dir / "data" / "test" / "hello.txt"
        extracted_comp = out_dir / "data" / "test" / "compressed.bin"

        self.assertTrue(extracted_hello.is_file())
        self.assertTrue(extracted_comp.is_file())

        self.assertEqual(extracted_hello.read_bytes(), b"Hello from synthetic XB!")
        self.assertEqual(extracted_comp.read_bytes(), b"A" * 120 + b"B" * 60)


if __name__ == "__main__":
    unittest.main()
