# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Locks the analyze.py TOML emitter's fail-closed string contract.

emit_toml is the one place analyzer output interpolates arbitrary strings into
a host-syntax document. Guest bytes never reach it today (model fields are
host constants or round-tripped TOML), but the emitter must never silently
produce a malformed document: strings are escaped for basic strings and
control characters fail closed instead of being emitted raw.
"""

from __future__ import annotations

import sys
import tomllib
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

from analyze import _fmt_value  # noqa: E402


class TestTomlEmit(unittest.TestCase):
    def test_string_escape_contract(self):
        self.assertEqual(_fmt_value("name", 'a"b\\c'), r'"a\"b\\c"')

    def test_plain_strings_pass_through(self):
        self.assertEqual(_fmt_value("kind", "function"), '"function"')
        self.assertEqual(_fmt_value("note", "bal tail"), '"bal tail"')

    def test_non_string_values(self):
        self.assertEqual(_fmt_value("addr", 0x1234), "0x00001234")
        self.assertEqual(_fmt_value("entry", 0x8000), "0x00008000")
        self.assertEqual(_fmt_value("skip", True), "true")
        self.assertEqual(_fmt_value("size", 42), "42")

    def test_control_characters_are_rejected_fail_closed(self):
        for hostile in ("a\nb", "a\rb", "a\tb", "a\x01b", "\x00", "a\x1fb", "a\x7fb"):
            with self.assertRaisesRegex(ValueError, "control"):
                _fmt_value("note", hostile)

    def test_printable_unicode_output_is_toml_parseable(self):
        value = "café π 🙂"
        rendered = _fmt_value("note", value)
        parsed = tomllib.loads(f"note = {rendered}\n")
        self.assertEqual(parsed["note"], value)


if __name__ == "__main__":
    unittest.main()
