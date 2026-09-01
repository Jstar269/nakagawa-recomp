# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The second wholly source-owned title fixture must prove multi-title planning.

`assets/titles/pspdev-phase5.json` describes a standard PSPDEV/PSPSDK module whose
sources live in `fixtures/pspdev_phase5`. Its value is only realized if its
configuration is *materially different* from the first synthetic fixture, so this
suite asserts the difference and then walks the same manifest -> validated plan ->
codegen/analyzer route with it. Nothing here reads a private manifest or any
game-derived input.
"""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import title_codegen_plan  # noqa: E402
import title_manifest  # noqa: E402

#: The canonical PSP user-module load base, one 16 KiB page above the start of the
#: user memory region that `synthetic.json` uses.
PSPDEV_BASE = 0x08804000
TITLES = ROOT / "assets" / "titles"


class PspdevPhase5FixtureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.path = TITLES / "pspdev-phase5.json"
        self.manifest = title_manifest.validate_manifest(
            title_manifest.load_manifest(self.path)
        )
        self.synthetic = title_manifest.validate_manifest(
            title_manifest.load_manifest(TITLES / "synthetic.json")
        )

    def build_plan(self, **overrides):
        values = {
            "game_name": "pspdev_phase5",
            "game_elf": Path("build/fixtures/pspdev_phase5.elf"),
            "build_dir": Path("build/pspdev_phase5"),
            "module_dir": None,
            "psp_header": None,
            "codegen_profile": None,
            "funcs_per_chunk": 64,
        }
        values.update(overrides)
        return title_codegen_plan.build_plan(self.manifest, **values)

    def test_manifest_points_at_committed_source_owned_material(self) -> None:
        fixture_dir = ROOT / self.manifest["filesystem"]["data_root"]
        self.assertTrue(fixture_dir.is_dir())
        self.assertTrue((fixture_dir / "main.c").is_file())
        self.assertTrue((fixture_dir / "Makefile").is_file())
        # A standard PSPDEV/PSPSDK module: ordinary toolchain, no vendored blobs.
        makefile = (fixture_dir / "Makefile").read_text(encoding="utf-8")
        self.assertIn("psp-config --pspsdk-path", makefile)
        self.assertIn("build.mak", makefile)
        self.assertEqual(self.manifest["kind"], "synthetic")
        self.assertEqual(self.manifest["id"], "pspdev-phase5-v1")

    def test_configuration_differs_materially_from_the_first_fixture(self) -> None:
        # Different executable policy: canonical user-module load base rather than the
        # user-memory region start.
        self.assertEqual(self.manifest["executable"]["base"], PSPDEV_BASE)
        self.assertEqual(self.manifest["executable"]["entry"], PSPDEV_BASE)
        self.assertNotEqual(
            self.manifest["executable"]["base"], self.synthetic["executable"]["base"]
        )
        # Different feature surface: this fixture makes PSP HLE system calls.
        self.assertIn("psp-hle", self.manifest["feature_requirements"])
        self.assertNotIn("psp-hle", self.synthetic["feature_requirements"])
        # Different module shape: no guest PRX of any kind, optional or required.
        self.assertEqual(self.manifest["modules"], [])
        self.assertTrue(self.synthetic["modules"])
        # Neither fixture asks for an extra executable span.
        self.assertEqual(self.manifest["executable"]["extra_executable_spans"], [])
        # Distinct identity and roots.
        self.assertNotEqual(self.manifest["id"], self.synthetic["id"])
        self.assertNotEqual(
            self.manifest["filesystem"]["data_root"],
            self.synthetic["filesystem"]["data_root"],
        )

    def test_plan_carries_no_hst_specific_configuration(self) -> None:
        plan = self.build_plan()
        self.assertEqual(plan["title_manifest_id"], "pspdev-phase5-v1")
        self.assertEqual(plan["game_base"], PSPDEV_BASE)
        self.assertEqual(plan["game_entry"], PSPDEV_BASE)
        self.assertEqual(plan["codegen_profile"], "none")
        self.assertEqual(plan["bss_metadata_source"], "elf")
        self.assertEqual(plan["environment"]["GAME_BASE"], "0x08804000")
        self.assertEqual(plan["environment"]["GAME_ENTRY"], "0x08804000")
        self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], "")
        self.assertNotIn("HST_EXTRA_SPANS", plan["environment"])
        codegen = plan["commands"]["codegen"]
        self.assertNotIn("--profile=hst", codegen)
        self.assertFalse([arg for arg in codegen if arg.startswith("--extra-elf=")])
        self.assertFalse(
            [arg for arg in plan["commands"]["prxload"] if arg.startswith("--psp-header=")]
        )

    def test_plan_differs_from_the_first_fixtures_plan(self) -> None:
        plan = self.build_plan()
        synthetic_plan = title_codegen_plan.build_plan(
            self.synthetic,
            game_name="synthetic",
            game_elf=Path("build/fixtures/synthetic.elf"),
            build_dir=Path("build/synthetic"),
            codegen_profile="none",
            funcs_per_chunk=64,
        )
        self.assertNotEqual(plan["environment"], synthetic_plan["environment"])
        self.assertNotEqual(plan["commands"], synthetic_plan["commands"])
        self.assertNotEqual(
            title_codegen_plan.compute_protected_digest(self.manifest),
            title_codegen_plan.compute_protected_digest(self.synthetic),
        )

    def test_manager_plan_requires_fewer_private_bindings(self) -> None:
        plan = title_codegen_plan.build_manager_plan(
            self.manifest,
            game_name="pspdev_phase5",
            game_elf=Path("build/fixtures/pspdev_phase5.elf"),
            build_dir=Path("build/pspdev_phase5"),
            funcs_per_chunk=64,
        )
        self.assertEqual(plan["title_kind"], "synthetic")
        self.assertEqual(plan["required_guest_modules"], [])
        self.assertEqual(plan["optional_guest_modules"], [])
        self.assertEqual(
            plan["private_binding_requirements"],
            {"game_elf": True, "module_dir": False, "psp_header": False},
        )
        self.assertEqual(plan["make"]["game_base"], "0x08804000")
        self.assertEqual(plan["make"]["codegen_profile_arg"], "")
        self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], "")
        self.assertNotIn("HST_EXTRA_SPANS", plan["environment"])

    def test_planner_cli_is_byte_deterministic(self) -> None:
        command = [
            sys.executable,
            str(ROOT / "tools" / "title_codegen_plan.py"),
            str(self.path),
            "--game-name=pspdev_phase5",
            "--game-elf=build/fixtures/pspdev_phase5.elf",
            "--build-dir=build/pspdev_phase5",
            "--funcs-per-chunk=64",
        ]
        first = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
        second = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(first.stdout, second.stdout)
        parsed = json.loads(first.stdout)
        self.assertEqual(parsed["game_base"], PSPDEV_BASE)
        self.assertEqual(parsed["environment"]["TITLE_EXTRA_SPANS"], "")
        self.assertNotIn("HST_EXTRA_SPANS", parsed["environment"])

    def test_manifest_declares_no_private_or_derived_material(self) -> None:
        rendered = title_manifest.canonical_json(self.manifest).lower()
        for forbidden in (
            "sha256", "private", "oracle", "decompiler", "savedata",
            "screenshot", "place_game_here", "ucus",
        ):
            self.assertNotIn(forbidden, rendered)


if __name__ == "__main__":
    unittest.main()
