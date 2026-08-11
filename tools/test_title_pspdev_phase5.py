# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regression tests for the second wholly source-owned PSPDEV fixture.

`assets/titles/pspdev-phase5.json` must describe a configuration that is
*meaningfully different* from both the HST retail manifest and the first
synthetic fixture -- a different executable policy, a different feature
surface, and a different private-binding shape -- proving the manifest-driven
planning design is genuinely multi-title rather than parameterized HST.
"""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import title_codegen_plan
import title_manifest

PSPDEV_BASE = 0x08804000


class PspdevPhase5TitleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.path = ROOT / "assets" / "titles" / "pspdev-phase5.json"
        self.manifest = title_manifest.validate_manifest(
            title_manifest.load_manifest(self.path)
        )
        self.hst = title_manifest.load_manifest(ROOT / "assets" / "titles" / "hst-ucus98701.json")
        self.synthetic = title_manifest.load_manifest(ROOT / "assets" / "titles" / "synthetic.json")

    def plan(self, **overrides):
        values = {
            "game_name": "pspdev_phase5",
            "game_elf": Path("build/fixtures/pspdev_phase5.elf"),
            "build_dir": Path("build/pspdev_phase5"),
            "module_dir": None,
            "psp_header": None,
            "codegen_profile": None,
            "include_optional_modules": set(),
            "funcs_per_chunk": 64,
        }
        values.update(overrides)
        return title_codegen_plan.build_plan(self.manifest, **values)

    def test_fixture_is_source_owned_and_declared_in_the_manifest(self) -> None:
        # The manifest's data root must point at the committed, source-owned
        # PSPDEV/PSPSDK fixture sources -- never a private workspace path.
        fixture_dir = ROOT / self.manifest["filesystem"]["data_root"]
        self.assertTrue(fixture_dir.is_dir())
        self.assertTrue((fixture_dir / "main.c").is_file())
        self.assertTrue((fixture_dir / "Makefile").is_file())
        self.assertEqual(self.manifest["id"], "pspdev-phase5-v1")
        self.assertEqual(self.manifest["kind"], "synthetic")

    def test_fixture_is_not_a_renamed_copy_of_the_first_synthetic(self) -> None:
        synthetic = title_manifest.validate_manifest(self.synthetic)
        # Meaningfully different executable policy: canonical user-module load base
        # versus the user-memory region start used by synthetic-allegrex-v1.
        self.assertEqual(self.manifest["executable"]["base"], PSPDEV_BASE)
        self.assertEqual(self.manifest["executable"]["entry"], PSPDEV_BASE)
        self.assertNotEqual(
            self.manifest["executable"]["base"],
            synthetic["executable"]["base"],
        )
        # Different feature surface (the fixture performs PSP HLE system calls).
        self.assertIn("psp-hle", self.manifest["feature_requirements"])
        self.assertNotIn("psp-hle", synthetic["feature_requirements"])
        # Different module shape: no optional guest PRX here.
        self.assertEqual(self.manifest["modules"], [])
        self.assertTrue(synthetic["modules"])
        # Different filesystem roots and identities.
        self.assertNotEqual(self.manifest["id"], synthetic["id"])
        self.assertNotEqual(
            self.manifest["filesystem"]["data_root"],
            synthetic["filesystem"]["data_root"],
        )
        # The protected digests must be pairwise distinct across all three titles.
        digests = {
            title_codegen_plan.compute_protected_digest(raw)
            for raw in (self.manifest, self.synthetic, self.hst)
        }
        self.assertEqual(len(digests), 3)

    def test_generic_plan_differs_from_hst_and_first_synthetic(self) -> None:
        plan = self.plan()
        self.assertEqual(plan["title_manifest_id"], "pspdev-phase5-v1")
        self.assertEqual(plan["game_base"], PSPDEV_BASE)
        self.assertEqual(plan["game_entry"], PSPDEV_BASE)
        self.assertEqual(plan["codegen_profile"], "none")
        self.assertEqual(plan["bss_metadata_source"], "elf")
        self.assertEqual(plan["environment"], {
            "GAME_BASE": "0x08804000",
            "GAME_ENTRY": "0x08804000",
            "HST_EXTRA_SPANS": "",
        })
        # No HST profile, no guest modules, no PSP-header binding.
        self.assertNotIn("--profile=hst", plan["commands"]["codegen"])
        self.assertFalse(
            any(arg.startswith("--extra-elf=") for arg in plan["commands"]["codegen"])
        )
        self.assertNotIn("--psp-header=", plan["commands"]["prxload"])
        # And it is not equal to the first synthetic fixture's plan: base, features,
        # and filesystem roots differ, so the generated commands must differ.
        synthetic_plan = title_codegen_plan.build_plan(
            title_manifest.validate_manifest(self.synthetic),
            game_name="synthetic",
            game_elf=Path("build/fixtures/synthetic.elf"),
            build_dir=Path("build/synthetic"),
            module_dir=None,
            psp_header=None,
            codegen_profile="none",
            funcs_per_chunk=64,
        )
        self.assertNotEqual(plan["environment"], synthetic_plan["environment"])
        self.assertNotEqual(plan["commands"], synthetic_plan["commands"])

    def test_manager_plan_has_reduced_private_binding_requirements(self) -> None:
        plan = title_codegen_plan.build_manager_plan(
            self.manifest,
            game_name="pspdev_phase5",
            game_elf=Path("build/fixtures/pspdev_phase5.elf"),
            build_dir=Path("build/pspdev_phase5"),
            module_dir=None,
            psp_header=None,
            codegen_profile=None,
            funcs_per_chunk=64,
        )
        self.assertEqual(plan["title_kind"], "synthetic")
        self.assertEqual(plan["codegen_profile"], "none")
        self.assertEqual(plan["required_guest_modules"], [])
        self.assertEqual(plan["optional_guest_modules"], [])
        self.assertEqual(
            plan["private_binding_requirements"],
            {"game_elf": True, "module_dir": False, "psp_header": False},
        )
        self.assertEqual(plan["make"]["game_base"], "0x08804000")
        self.assertEqual(plan["make"]["codegen_profile_arg"], "")
        self.assertEqual(plan["environment"]["HST_EXTRA_SPANS"], "")

    def test_cli_is_deterministic_and_emits_no_private_bindings(self) -> None:
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
        self.assertEqual(parsed["environment"]["HST_EXTRA_SPANS"], "")


if __name__ == "__main__":
    unittest.main()
