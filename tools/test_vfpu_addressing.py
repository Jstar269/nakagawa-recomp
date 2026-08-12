#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""VFPU vector-register addressing: hardware agreement and cross-implementation identity.

Two distinct kinds of assertion live here and must not be conflated.

**Hardware-measured (tier H).**  ``fixtures/vfpu_addressing/hardware_vfpu_addr_001.json``
records what a PSP-3001 actually did: all 128 scalar encodings, and 14 selected
wide encodings.  Tests against those cases are evidence about silicon.

**Derived (tier S).**  The remaining 498 wide encodings were *not* observed.
Tests over the full 512-entry domain check that the two independent
implementations -- ``tools/codegen.py`` ``vreg_indices`` and the production C
``vreg_idx`` in ``src/rt/vfpu_interp.c`` -- agree with each other and with the
closed form the hardware confirmed.  That is a consistency and regression
property, **not** a claim that silicon was observed for those encodings.

Every test name below says which kind it is.
"""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import codegen

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "fixtures" / "vfpu_addressing" / "hardware_vfpu_addr_001.json"
SELFTEST = ROOT / "src" / "rt" / "vfpu_addr_selftest.c"

WIDTHS = (1, 2, 3, 4)
ENCODINGS = range(128)


def scalar_rule(enc: int) -> int:
    """The closed form the hardware confirmed for all 128 scalar encodings."""
    return ((enc >> 2) & 7) * 16 + (enc & 3) * 4 + ((enc >> 5) & 3)


def load_fixture() -> dict:
    return json.loads(FIXTURE.read_text(encoding="utf-8"))


def build_and_run_c() -> dict[tuple[int, int], list[int]]:
    """Run the production C decoder over its whole domain."""
    cc = shutil.which("gcc") or shutil.which("cc")
    if cc is None:
        raise unittest.SkipTest("no C compiler on PATH")
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "vfpu_addr_selftest"
        build = subprocess.run(
            [cc, "-std=c11", "-I", str(ROOT / "src" / "rt"), "-o", str(exe), str(SELFTEST), "-lm"],
            capture_output=True,
            text=True,
        )
        if build.returncode != 0:
            raise AssertionError(f"selftest failed to build:\n{build.stderr}")
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        if run.returncode != 0:
            raise AssertionError(f"selftest failed to run:\n{run.stderr}")

    table: dict[tuple[int, int], list[int]] = {}
    for line in run.stdout.splitlines():
        if not line.strip():
            continue
        head, *rest = line.split()
        width = int(head[1:])
        enc = int(rest[0][1:], 16)
        lanes = int(rest[1][1:])
        idx = [int(x) for x in rest[2:]]
        assert len(idx) == lanes, f"lane count mismatch on {line!r}"
        table[(width, enc)] = idx
    return table


class FixtureIntegrityTest(unittest.TestCase):
    """The fixture must state its own limits honestly."""

    def setUp(self) -> None:
        self.fx = load_fixture()

    def test_boundary_is_declared(self) -> None:
        b = self.fx["measurement_boundary"]
        self.assertEqual(b["scalar_encodings_measured"], 128)
        self.assertEqual(b["scalar_encodings_possible"], 128)
        self.assertEqual(b["wide_encodings_measured"], 14)
        self.assertEqual(b["wide_encodings_possible"], 512)
        self.assertLess(b["wide_encodings_measured"], b["wide_encodings_possible"])

    def test_wide_case_count_matches_the_declared_boundary(self) -> None:
        self.assertEqual(
            len(self.fx["wide_cases"]),
            self.fx["measurement_boundary"]["wide_encodings_measured"],
        )

    def test_unmeasured_areas_are_named(self) -> None:
        text = " ".join(self.fx["measurement_boundary"]["does_not_cover"])
        for name in ("mreg_index", "vreg_names", "oz_n"):
            self.assertIn(name, text, f"{name} must be named as NOT covered")

    def test_no_private_capture_path_is_committed(self) -> None:
        raw = self.fx["provenance"]["raw_captures"]
        self.assertIn("NOT committed", raw)
        blob = json.dumps(self.fx)
        self.assertNotIn("oracle/hardware-results/vfpu-addr-run", blob)

    def test_provenance_is_measured_not_placeholder(self) -> None:
        p = self.fx["provenance"]
        self.assertTrue(p["acceptance_eligible"])
        self.assertEqual(len(p["prx_sha256"]), 64)
        self.assertNotEqual(set(p["prx_sha256"]), {"0"})
        self.assertNotIn("unknown", (p["model"] + p["firmware"]).lower())


class HardwareAgreementTest(unittest.TestCase):
    """TIER H. Assertions about what real silicon did."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.fx = load_fixture()

    def test_python_decoder_matches_measured_wide_cases(self) -> None:
        for case in self.fx["wide_cases"]:
            enc = int(case["encoding"], 16)
            got = codegen.vreg_indices(enc, case["width"])
            self.assertEqual(
                got,
                case["physical"],
                f"width {case['width']} encoding {case['encoding']}: "
                f"codegen says {got}, hardware measured {case['physical']}",
            )

    def test_python_decoder_matches_measured_scalar_rule(self) -> None:
        """All 128 scalar encodings were measured and agreed with this form."""
        for enc in ENCODINGS:
            self.assertEqual(codegen.vreg_indices(enc, 1), [scalar_rule(enc)])

    def test_published_scalar_spot_values(self) -> None:
        for enc_hex, phys in self.fx["scalar_rule"]["published_spot_values"].items():
            enc = int(enc_hex, 16)
            self.assertEqual(scalar_rule(enc), phys)
            self.assertEqual(codegen.vreg_indices(enc, 1), [phys])

    def test_triple_row_selector_is_bit_six(self) -> None:
        """The discriminating case: a bit-5 selector would give a different answer."""
        self.assertEqual(codegen.vreg_indices(0x20, 3), [0, 4, 8])
        self.assertEqual(codegen.vreg_indices(0x40, 3), [1, 2, 3])

    def test_transpose_wraps_rather_than_saturating(self) -> None:
        self.assertEqual(codegen.vreg_indices(0x60, 4), [8, 12, 0, 4])
        self.assertEqual(codegen.vreg_indices(0x40, 4), [2, 3, 0, 1])

    def test_c_decoder_matches_measured_cases(self) -> None:
        table = build_and_run_c()
        for case in self.fx["wide_cases"]:
            enc = int(case["encoding"], 16)
            self.assertEqual(
                table[(case["width"], enc)],
                case["physical"],
                f"production C vreg_idx disagrees with hardware at "
                f"width {case['width']} encoding {case['encoding']}",
            )


class DerivedConsistencyTest(unittest.TestCase):
    """TIER S. Derived from the measured rule; NOT additional hardware evidence.

    498 of the 512 wide encodings were never observed on silicon. These tests
    assert that the two implementations agree across the whole finite domain,
    which catches divergence and regression but proves nothing about hardware
    for the unmeasured encodings.
    """

    def test_c_and_python_agree_over_the_entire_domain(self) -> None:
        table = build_and_run_c()
        self.assertEqual(len(table), len(WIDTHS) * len(ENCODINGS))
        mismatches = []
        for width in WIDTHS:
            for enc in ENCODINGS:
                py = codegen.vreg_indices(enc, width)
                c = table[(width, enc)]
                if py != c:
                    mismatches.append(f"w{width} e{enc:02x}: py={py} c={c}")
        self.assertEqual(mismatches, [], "C and Python decoders diverge: " + "; ".join(mismatches[:8]))

    def test_lane_counts_follow_the_width(self) -> None:
        for width in WIDTHS:
            for enc in ENCODINGS:
                self.assertEqual(len(codegen.vreg_indices(enc, width)), width)

    def test_every_index_is_inside_the_register_file(self) -> None:
        for width in WIDTHS:
            for enc in ENCODINGS:
                for idx in codegen.vreg_indices(enc, width):
                    self.assertTrue(0 <= idx < 128, f"w{width} e{enc:02x} -> {idx}")

    def test_lanes_within_one_access_are_distinct(self) -> None:
        """A vector access must never name the same physical register twice."""
        for width in WIDTHS:
            for enc in ENCODINGS:
                idx = codegen.vreg_indices(enc, width)
                self.assertEqual(len(set(idx)), len(idx), f"w{width} e{enc:02x} -> {idx}")

    def test_scalar_decode_is_a_bijection(self) -> None:
        """The 128 scalar encodings must cover all 128 registers exactly once."""
        seen = [codegen.vreg_indices(enc, 1)[0] for enc in ENCODINGS]
        self.assertEqual(sorted(seen), list(range(128)))


if __name__ == "__main__":
    unittest.main()
