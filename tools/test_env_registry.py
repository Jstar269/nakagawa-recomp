#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for the runtime's environment-variable registry (src/rt/env_registry.h).

The header is one table naming every variable the runtime reads, with its kind, default,
reading files and purpose. These tests keep it honest:

1. Every row is well formed and the names are unique and sorted.
2. Every `SR_*` string literal in src/rt, and every name passed to getenv or to a
   switch-reading helper, has a row (a name ending in _N covers its numbered family).
3. Every row is read or named somewhere in src/rt (no stale rows), and its reading files exist.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
RT = ROOT / "src" / "rt"
REGISTRY = RT / "env_registry.h"

KINDS = {"flag-presence", "flag-nonzero", "number", "path", "string", "list", "system"}
VENDORED = ("h264_ffmpeg_sub",)
SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".inc")

ROW_RE = re.compile(
    r'^\s*X\("(?P<name>[A-Za-z0-9_]+)", "(?P<kind>[a-z-]+)", "(?P<default>(?:[^"\\]|\\.)*)", '
    r'"(?P<readers>(?:[^"\\]|\\.)*)", "(?P<purpose>(?:[^"\\]|\\.)*)"\)(?: \\)?$'
)
GETENV_RE = re.compile(r'\b_?w?getenv\(\s*L?"([A-Za-z][A-Za-z0-9_]*)"\s*\)')
SR_LITERAL_RE = re.compile(r'"(SR_[A-Z0-9_]+)"')


def registry_rows():
    rows = []
    in_table = False
    for number, line in enumerate(REGISTRY.read_text(encoding="utf-8").split("\n"), 1):
        if line.startswith("#define NK_ENV_REGISTRY(X)"):
            in_table = True
            continue
        if in_table:
            if not line.strip():
                break
            match = ROW_RE.match(line)
            rows.append((number, line, match))
    return rows


def source_files():
    for path in sorted(RT.rglob("*")):
        if path.suffix in SOURCE_SUFFIXES and not any(part in VENDORED for part in path.parts):
            if path != REGISTRY:
                yield path


def covered(name, names, families):
    if name in names:
        return True
    return any(re.fullmatch(prefix + r"\d+", name) for prefix in families)


class EnvRegistryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = registry_rows()
        cls.entries = [m.groupdict() for _, _, m in cls.rows if m]
        cls.names = {e["name"] for e in cls.entries}
        cls.families = {e["name"][:-1] for e in cls.entries if e["name"].endswith("_N")}
        cls.used = {}
        for path in source_files():
            text = path.read_text(encoding="utf-8", errors="replace")
            rel = path.relative_to(RT).as_posix()
            for regex in (GETENV_RE, SR_LITERAL_RE):
                for match in regex.finditer(text):
                    cls.used.setdefault(match.group(1), set()).add(rel)

    def test_every_row_is_well_formed(self):
        self.assertTrue(self.rows, "no rows found after #define NK_ENV_REGISTRY(X)")
        for number, line, match in self.rows:
            self.assertIsNotNone(match, f"env_registry.h:{number} is not a well-formed row: {line!r}")
        for entry in self.entries:
            name = entry["name"]
            self.assertIn(entry["kind"], KINDS, f"{name}: unknown kind {entry['kind']!r}")
            for field in ("default", "readers", "purpose"):
                self.assertTrue(entry[field].strip(), f"{name}: empty {field}")
            self.assertLessEqual(len(entry["purpose"]), 160, f"{name}: purpose is more than one line")

    def test_names_are_unique_and_sorted(self):
        names = [e["name"] for e in self.entries]
        self.assertEqual(len(names), len(set(names)), "duplicate row")
        self.assertEqual(names, sorted(names), "rows must stay sorted by name")

    def test_every_name_the_runtime_reads_has_a_row(self):
        missing = sorted(
            name for name in self.used
            if (name.startswith("SR_") or name in self._getenv_names())
            and not covered(name, self.names, self.families)
        )
        self.assertEqual(missing, [], "read in src/rt but missing from src/rt/env_registry.h: %s" % ", ".join(missing))

    def _getenv_names(self):
        names = set()
        for path in source_files():
            names.update(GETENV_RE.findall(path.read_text(encoding="utf-8", errors="replace")))
        return names

    def test_every_row_is_read_somewhere(self):
        stale = []
        for entry in self.entries:
            name = entry["name"]
            if name.endswith("_N"):
                prefix = name[:-1]
                found = any(re.fullmatch(prefix + r"\d+", used) for used in self.used) or any(
                    prefix in text
                    for text in (p.read_text(encoding="utf-8", errors="replace") for p in source_files())
                )
            else:
                found = name in self.used or any(
                    name in text for text in (p.read_text(encoding="utf-8", errors="replace") for p in source_files())
                )
            if not found:
                stale.append(name)
        self.assertEqual(stale, [], "rows nothing in src/rt names: %s" % ", ".join(stale))

    def test_reading_files_exist(self):
        for entry in self.entries:
            for rel in entry["readers"].split(","):
                self.assertTrue((RT / rel).is_file(), f"{entry['name']}: reader {rel} does not exist")


if __name__ == "__main__":
    unittest.main()
