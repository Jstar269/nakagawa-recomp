# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for tools/package_repro_check.py: every kind of package difference is named (#324)."""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import package_repro_check  # noqa: E402

_BODY = (b"nakagawa " * 20000) + bytes(range(256)) * 40


def _write_zip(path: Path, entries, *, level=9, date=(2026, 10, 1, 0, 0, 0), attr=0o100644 << 16, comment=b""):
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in entries:
            info = zipfile.ZipInfo(name, date)
            info.create_system = 3
            info.external_attr = attr
            info.compress_type = zipfile.ZIP_DEFLATED
            info.compress_level = level
            archive.writestr(info, data)
        archive.comment = comment


class CompareZipTests(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        self.entries = [("bin/a.exe", _BODY), ("docs/b.md", b"# b\n"), ("empty.txt", b"")]

    def tearDown(self):
        self._temp.cleanup()

    def _pair(self, **second):
        one, two = self.root / "one.zip", self.root / "two.zip"
        _write_zip(one, self.entries)
        _write_zip(two, second.pop("entries", self.entries), **second)
        return one, two

    def _kinds(self, first, second):
        report = package_repro_check.compare(first, second)
        return report, sorted({item["kind"] for item in report["differences"]})

    def test_identical_zips_are_identical(self):
        report, kinds = self._kinds(*self._pair())
        self.assertTrue(report["identical"])
        self.assertEqual(kinds, [])
        self.assertEqual(report["sha256"]["first"], report["sha256"]["second"])

    def test_entry_time_permissions_and_comment_are_named(self):
        _, kinds = self._kinds(*self._pair(date=(2026, 10, 2, 0, 0, 0)))
        self.assertEqual(kinds, ["date_time"])
        _, kinds = self._kinds(*self._pair(attr=0o100755 << 16))
        self.assertEqual(kinds, ["external_attr"])
        _, kinds = self._kinds(*self._pair(comment=b"built here"))
        self.assertEqual(kinds, ["archive-comment"])

    def test_changed_content_and_missing_entries_are_named(self):
        changed = [("bin/a.exe", _BODY + b"!"), ("docs/b.md", b"# b\n"), ("empty.txt", b"")]
        report, kinds = self._kinds(*self._pair(entries=changed))
        self.assertIn("content", kinds)
        self.assertEqual([item["entry"] for item in report["differences"] if item["kind"] == "content"],
                         ["bin/a.exe"])
        fewer = self.entries[:2]
        report, kinds = self._kinds(*self._pair(entries=fewer))
        self.assertEqual(kinds, ["only-in-first"])
        self.assertEqual(report["differences"][0]["entry"], "empty.txt")

    def test_entry_order_is_named(self):
        _, kinds = self._kinds(*self._pair(entries=list(reversed(self.entries))))
        self.assertIn("entry-order", kinds)

    def test_same_content_deflated_differently_is_a_compression_difference_only(self):
        report, kinds = self._kinds(*self._pair(level=1))
        self.assertFalse(report["identical"])
        self.assertIn("compressed-bytes", kinds)
        self.assertNotIn("content", kinds)

    def test_an_entry_that_only_moved_is_not_a_difference(self):
        # An earlier entry changes size, so every later entry sits at another offset: only the first differs.
        shifted = [("a.txt", b"short")] + self.entries
        base = [("a.txt", b"a much longer first entry")] + self.entries
        one, two = self.root / "base.zip", self.root / "shifted.zip"
        _write_zip(one, base)
        _write_zip(two, shifted)
        report = package_repro_check.compare(one, two)
        self.assertEqual([(item["kind"], item["entry"]) for item in report["differences"]],
                         [("content", "a.txt")])

    def test_listing_is_limited_per_kind_and_counts_stay_complete(self):
        many = [(f"f{i}.txt", b"x") for i in range(10)]
        one, two = self.root / "m1.zip", self.root / "m2.zip"
        _write_zip(one, many)
        _write_zip(two, many, date=(2026, 10, 2, 0, 0, 0))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(package_repro_check.main([str(one), str(two), "--limit", "3"]), 1)
        text = out.getvalue()
        self.assertIn("DIFFERENT: 10 difference(s)", text)
        self.assertEqual(text.count("[date_time] f"), 3)
        self.assertIn("7 more (10 in all)", text)

    def test_cli_exit_codes_and_json(self):
        one, two = self._pair()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(package_repro_check.main([str(one), str(two)]), 0)
        self.assertTrue(out.getvalue().startswith("IDENTICAL "))
        other = self.root / "three.zip"
        _write_zip(other, self.entries, date=(2026, 10, 3, 0, 0, 0))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(package_repro_check.main([str(one), str(other), "--json"]), 1)
        self.assertFalse(json.loads(out.getvalue())["identical"])
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(package_repro_check.main([str(one), str(self.root)]), 2)
        self.assertIn("FAIL", err.getvalue())


class CompareFolderTests(unittest.TestCase):
    def test_folder_differences_are_named_by_path(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for side in ("a", "b"):
                (root / side / "bin").mkdir(parents=True)
                (root / side / "bin" / "x.dll").write_bytes(b"same")
                (root / side / "SBOM.json").write_bytes(b"one")
            self.assertTrue(package_repro_check.compare(root / "a", root / "b")["identical"])
            (root / "b" / "SBOM.json").write_bytes(b"two")
            (root / "b" / "extra.txt").write_bytes(b"x")
            report = package_repro_check.compare(root / "a", root / "b")
            self.assertEqual([(item["kind"], item["entry"]) for item in report["differences"]],
                             [("only-in-second", "extra.txt"), ("content", "SBOM.json")])


if __name__ == "__main__":
    unittest.main()
