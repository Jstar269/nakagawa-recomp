# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for the relocatable compilation database tool (issue #368)."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compdb_relocate  # noqa: E402

OLD = "/old/checkout"
BS = chr(92)


def _entry() -> dict:
    return {
        "directory": OLD,
        "file": OLD + "/src/rt/a.c",
        "arguments": ["gcc", "-I" + OLD + "/src", "-c", OLD + "/src/rt/a.c",
                      "-o", OLD + "/build/a.o", "-I/usr/include"],
    }


class RelocateTests(unittest.TestCase):
    def test_paths_under_root_become_relative(self) -> None:
        (out,) = compdb_relocate.relocate([_entry()], OLD, "/new/place")
        self.assertEqual(out["directory"], "/new/place")
        self.assertEqual(out["file"], "src/rt/a.c")
        self.assertEqual(out["arguments"], ["gcc", "-Isrc", "-c", "src/rt/a.c",
                                            "-o", "build/a.o", "-I/usr/include"])

    def test_no_old_root_survives_anywhere(self) -> None:
        (out,) = compdb_relocate.relocate([_entry()], OLD, "/new/place")
        self.assertNotIn(OLD, json.dumps(out))

    def test_backslash_roots_and_command_strings(self) -> None:
        root = "C:" + BS + "w" + BS + "nk"
        entry = {"directory": root, "file": root + BS + "x.c",
                 "command": "gcc -IC:/w/nk/src -c C:/w/nk/x.c"}
        (out,) = compdb_relocate.relocate([entry], root, "D:/moved")
        self.assertEqual(out["file"], "x.c")
        self.assertEqual(out["command"], "gcc -Isrc -c x.c")

    def test_sibling_prefix_is_not_rewritten(self) -> None:
        entry = {"directory": OLD, "file": OLD + "-other/x.c", "arguments": []}
        (out,) = compdb_relocate.relocate([entry], OLD, "/n")
        self.assertEqual(out["file"], OLD + "-other/x.c")

    def test_main_rewrites_file_in_place(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "compile_commands.json"
            db.write_text(json.dumps([_entry()]), encoding="utf-8")
            self.assertEqual(compdb_relocate.main([str(db), "--root", tmp]), 0)
            (out,) = json.loads(db.read_text(encoding="utf-8"))
            self.assertEqual(out["directory"], Path(tmp).resolve().as_posix())
            self.assertEqual(out["file"], "src/rt/a.c")


if __name__ == "__main__":
    unittest.main()
