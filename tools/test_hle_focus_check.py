#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for tools/hle_focus_check.py (issue #307), on small compiled objects."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_focus_check  # noqa: E402

CC = shutil.which("gcc") or shutil.which("cc")
NM = shutil.which("nm")
SOURCES = ["fake/focus_a.c", "fake/focus_b.c", "fake/focus_c.c"]


@unittest.skipUnless(CC and NM, "needs a C compiler and nm")
class FocusCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def compile(self, name: str, body: str) -> Path:
        source = self.dir / f"{name}.c"
        source.write_text(body, encoding="utf-8")
        obj = self.dir / f"{name}.o"
        subprocess.run([str(CC), "-c", str(source), "-o", str(obj)], check=True)
        return obj

    def link(self, *objects: Path) -> Path:
        exe = self.dir / "t.exe"
        subprocess.run([str(CC), "-o", str(exe), *map(str, objects)], check=True)
        return exe

    def build(self) -> tuple[Path, Path, Path, Path]:
        a = self.compile("focus_a", "int a_only(void) { return 1; }\nint shared(void) { return 2; }\n")
        b = self.compile("focus_b", "int b_only(void) { return 3; }\n")
        c = self.compile("focus_c", "int c_only(void) { return 4; }\n")
        main = self.compile("main", "extern int a_only(void);\nint main(void) { return a_only() - 1; }\n")
        return a, b, c, main

    def run_check(self, exe: Path, include: list[str], required: list[str] | None = None) -> list[str]:
        problems: list[str] = hle_focus_check.check(exe, self.dir, include, [], required or [], NM, SOURCES)
        return problems

    def test_focused_executable_passes(self) -> None:
        a, _, _, main = self.build()
        self.assertEqual(self.run_check(self.link(main, a), ["fake/focus_a.c"], ["a_only"]), [])

    def test_linked_excluded_file_is_reported(self) -> None:
        a, b, _, main = self.build()
        problems = self.run_check(self.link(main, a, b), ["fake/focus_a.c"])
        self.assertEqual(len(problems), 1)
        self.assertIn("focus_b.c is linked", problems[0])
        self.assertIn("b_only", problems[0])

    def test_stub_redefining_included_symbol_is_reported(self) -> None:
        a, _, _, main = self.build()
        stub = self.compile("stub", "int shared(void) { return 9; }\n")
        problems = hle_focus_check.check(self.link(main, a), self.dir, ["fake/focus_a.c"], [stub], [], NM, SOURCES)
        self.assertEqual(len(problems), 1)
        self.assertIn("stub stub.o redefines", problems[0])
        self.assertIn("shared", problems[0])

    def test_missing_required_symbol_is_reported(self) -> None:
        a, _, _, main = self.build()
        problems = self.run_check(self.link(main, a), ["fake/focus_a.c"], ["not_there"])
        self.assertEqual(problems, ["required symbol not_there is not defined in t.exe"])

    def test_missing_excluded_object_fails_closed(self) -> None:
        a, b, _, main = self.build()
        exe = self.link(main, a)
        b.unlink()
        with self.assertRaises(hle_focus_check.FocusError):
            self.run_check(exe, ["fake/focus_a.c"])

    def test_unknown_included_file_fails_closed(self) -> None:
        a, _, _, main = self.build()
        with self.assertRaises(hle_focus_check.FocusError):
            self.run_check(self.link(main, a), ["fake/focus_zzz.c"])


if __name__ == "__main__":
    unittest.main()
