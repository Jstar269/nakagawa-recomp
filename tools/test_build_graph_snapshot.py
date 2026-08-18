# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for the read-only build graph snapshot (tools/build_graph_snapshot.py).

Guards the dependency-file parsing (including gcc -MP phony header rules and
continuation lines), unit deduplication, and the prereq categorization that
the compiler-neutral build manifest baseline relies on.
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import build_graph_snapshot as bgs

ROOT = Path(__file__).resolve().parents[1]

# A realistic gcc -MMD file: backslash-continued prereq lines plus -MP phony
# header rules. Each continuation is one literal backslash at end of line.
SAMPLE_D = (
    "build/hst/hle.o: src/rt/hle.c \\\n"
    " src/rt/atrac3p_bridge.h \\\n"
    " src/rt/fp_convert.h\n"
    "build/hst/hle.o: src/rt/atrac3p_bridge.h\n"
    "src/rt/hle.c:\n"
    "src/rt/atrac3p_bridge.h:\n"
    "src/rt/fp_convert.h:\n"
)


class DepFileParsingTests(unittest.TestCase):
    def test_phony_header_rules_are_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td) / "hle.d"
            d.write_text(SAMPLE_D, encoding="utf-8")
            parsed = bgs._parse_d(d)
        self.assertEqual(set(parsed), {"build/hst/hle.o"})
        self.assertEqual(
            sorted(parsed["build/hst/hle.o"]),
            sorted(["src/rt/hle.c", "src/rt/atrac3p_bridge.h", "src/rt/fp_convert.h"]),
        )

    def test_duplicate_object_rules_are_merged(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td) / "x.d"
            d.write_text("o.o: a.h\n" "o.o: b.h c.h\n" "a.h:\n" "b.h:\n" "c.h:\n", encoding="utf-8")
            parsed = bgs._parse_d(d)
        self.assertEqual(sorted(parsed["o.o"]), ["a.h", "b.h", "c.h"])

    def test_categorize(self) -> None:
        bd = "build/hst"
        self.assertEqual(bgs._categorize("src/rt/hle.c", bd, "hst"), "runtime_source")
        self.assertEqual(bgs._categorize("src/rt/recomp.h", bd, "hst"), "runtime_header")
        self.assertEqual(bgs._categorize("build/hst/hst_recomp_0.c", bd, "hst"), "generated_source_or_header")
        # generated headers (hst_recomp_funcs.h) belong to the generated unit family
        self.assertEqual(bgs._categorize("build/hst/hst_recomp_funcs.h", bd, "hst"), "generated_source_or_header")
        self.assertEqual(bgs._categorize(".recomp-profile-x", bd, "hst"), "profile_stamp")


class SnapshotTests(unittest.TestCase):
    def test_snapshot_dedupes_and_counts_units(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            bd = Path(td)
            (bd / "hle.d").write_text(SAMPLE_D, encoding="utf-8")
            (bd / "hst_recomp_0.d").write_text(
                "build/hst/hst_recomp_0.o: build/hst/hst_recomp_0.c src/rt/recomp.h\n",
                encoding="utf-8",
            )
            (bd / "runtime_profile.json").write_text(
                json.dumps({"sections": {"runtime": {"profile_hash": "abc123"}}}), encoding="utf-8"
            )
            (bd / "hst_recomp.c").write_text("x", encoding="utf-8")
            (bd / "hst_recomp_0.c").write_text("x", encoding="utf-8")
            (bd / "hle.o").write_bytes(b"\x00" * 8)
            snap = bgs.snapshot(bd, "hst")
        self.assertEqual(len(snap["units"]), 2)
        self.assertEqual(snap["generated_units"]["chunk_count"], 1)
        self.assertEqual(snap["profile_hashes"]["runtime"], "abc123")
        self.assertEqual(snap["outputs"]["object_count"], 1)

    def test_missing_build_dir_fails(self) -> None:
        with self.assertRaises(SystemExit):
            bgs.snapshot(Path("definitely/not/here"), "hst")


if __name__ == "__main__":
    unittest.main()
