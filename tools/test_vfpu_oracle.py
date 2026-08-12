# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Contract tests for the VFPU transcendental hardware oracle (Loop A).

These guard the properties that make the oracle meaningful.  They deliberately do
not assert any numeric result: the correct values are exactly what this oracle
exists to discover, and baking an expectation in would recreate the fabricated
evidence this whole surface was built to prevent.
"""

from __future__ import annotations

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ROOT / "fixtures" / "vfpu_oracle" / "vfpu_oracle_cases.h"
PROBE = ROOT / "fixtures" / "vfpu_oracle" / "vfpu_probe.c"
HOST = ROOT / "src" / "rt" / "vfpu_oracle_host.c"
MAKEFILE = (ROOT / "Makefile").read_text(encoding="utf-8", errors="replace")


class SharedInputVectorTests(unittest.TestCase):
    """Both sides must ask the same question."""

    def setUp(self) -> None:
        self.cases = CASES.read_text(encoding="utf-8")

    def test_probe_and_host_include_the_same_vector(self) -> None:
        for path in (PROBE, HOST):
            self.assertIn("vfpu_oracle_cases.h", path.read_text(encoding="utf-8"),
                          f"{path.name} must include the shared vector, not define its own")

    def test_declared_count_matches_the_literal_list(self) -> None:
        declared = int(re.search(r"#define VFPU_ORACLE_INPUT_COUNT (\d+)", self.cases).group(1))
        body = self.cases.split("VFPU_ORACLE_INPUTS[VFPU_ORACLE_INPUT_COUNT] = {", 1)[1].split("};", 1)[0]
        actual = len(re.findall(r"0x[0-9A-Fa-f]{8}u", body))
        self.assertEqual(declared, actual,
                         "a count/list mismatch would silently compare different-length runs")

    def test_inputs_are_raw_bit_patterns(self) -> None:
        body = self.cases.split("VFPU_ORACLE_INPUTS[VFPU_ORACLE_INPUT_COUNT] = {", 1)[1].split("};", 1)[0]
        stripped = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        self.assertNotRegex(stripped, r"\d+\.\d+[fF]?",
                            "decimal literals are re-rounded per compiler; use raw IEEE-754 bits")

    def test_large_argument_boundary_is_covered(self) -> None:
        first = int(re.search(r"#define VFPU_ORACLE_LARGE_ARG_FIRST (\d+)", self.cases).group(1))
        self.assertGreater(first, 0)
        # 2^32 and 2^33 bracket the region upstream (ppsspp#21070) flags as untested.
        for bits in ("0x4F800000u", "0x50000000u"):
            self.assertIn(bits, self.cases, "the upstream-flagged region must stay covered")


class ProbeContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.probe = PROBE.read_text(encoding="utf-8")

    def test_vfpu_thread_attribute_is_declared(self) -> None:
        """Without THREAD_ATTR_VFPU the first VFPU access traps, and the probe
        would report a fault that reads like a divergence."""
        self.assertIn("THREAD_ATTR_VFPU", self.probe)

    def test_probe_distinguishes_emulator_from_hardware(self) -> None:
        self.assertIn('emulated ? "ppsspp" : "psp"', self.probe)
        self.assertIn("EMULATOR_DEVCTL_IS_EMULATOR", self.probe)

    def test_bits_move_without_an_fpu_round_trip(self) -> None:
        """mtv/mfv preserve NaN payloads and denormals exactly."""
        self.assertIn("mtv", self.probe)
        self.assertIn("mfv", self.probe)


class HostHarnessContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.host = HOST.read_text(encoding="utf-8")

    def test_host_uses_production_implementations(self) -> None:
        for fn in ("sr_vfpu_rcp", "sr_vfpu_rsqrt", "sr_vfpu_sqrt", "sr_vfpu_asin",
                   "sr_vfpu_log2", "sr_vfpu_sin", "sr_vfpu_cos", "sr_vfpu_exp2"):
            self.assertIn(fn, self.host,
                          "the Nakagawa side must call production code, not reimplement it")

    def test_provenance_is_required_not_defaulted(self) -> None:
        self.assertIn("--model", self.host)
        self.assertIn("--firmware", self.host)
        self.assertIn("--source-commit", self.host)
        self.assertIn("--artifact-sha256", self.host)
        self.assertIn("return 2", self.host)

    def test_make_target_is_game_independent(self) -> None:
        """The recipe must not depend on the private game image or its generated
        chunks. Comment prose is stripped first: the surrounding comment names
        GAME_ELF precisely to say it is *not* required."""
        block = MAKEFILE.split("psp-oracle-vfpu -- Nakagawa side", 1)[1].split("psp-oracle-vfpu-build:", 1)[0]
        recipe = "\n".join(l for l in block.splitlines() if not l.lstrip().startswith("#"))
        self.assertNotIn("GAME_ELF", recipe)
        self.assertNotIn("CHUNK_OBJS", recipe)
        self.assertIn("sr_vfpu", MAKEFILE.split("psp-oracle-vfpu -- Nakagawa side", 1)[1][:600])


if __name__ == "__main__":
    unittest.main()
