# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

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


class SyntheticTitleCodegenPlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.path = ROOT / "assets" / "titles" / "synthetic.json"
        self.manifest = title_manifest.validate_manifest(
            title_manifest.load_manifest(self.path)
        )

    def plan(self, **overrides):
        values = {
            "game_name": "synthetic",
            "game_elf": Path("build/fixtures/synthetic.elf"),
            "build_dir": Path("build/synthetic"),
            "module_dir": None,
            "psp_header": None,
            "codegen_profile": "none",
            "include_optional_modules": set(),
            "funcs_per_chunk": 64,
        }
        values.update(overrides)
        return title_codegen_plan.build_plan(self.manifest, **values)

    def test_source_owned_manifest_uses_generic_planning_path(self) -> None:
        plan = self.plan()
        self.assertEqual(plan["title_manifest_id"], "synthetic-allegrex-v1")
        self.assertEqual(plan["game_base"], 0x08800000)
        self.assertEqual(plan["game_entry"], 0x08800000)
        self.assertEqual(plan["codegen_profile"], "none")
        self.assertEqual(plan["bss_metadata_source"], "elf")
        self.assertEqual(plan["environment"], {
            "GAME_BASE": "0x08800000",
            "GAME_ENTRY": "0x08800000",
            "HST_EXTRA_SPANS": "",
        })
        self.assertEqual(plan["commands"]["codegen"], [
            "python",
            "tools/codegen.py",
            "build/fixtures/synthetic.elf",
            "build/synthetic/synthetic_recomp.c",
            "--base=0x08800000",
            "--funcs-per-chunk=64",
        ])
        self.assertFalse(
            any(arg.startswith("--extra-elf=") for arg in plan["commands"]["codegen"])
        )

    def test_optional_prx_requires_explicit_private_binding(self) -> None:
        with self.assertRaisesRegex(ValueError, "module_dir is required"):
            self.plan(include_optional_modules={"synthetic.prx"})
        plan = self.plan(
            module_dir=Path("build/fixtures/modules"),
            include_optional_modules={"synthetic.prx"},
        )
        self.assertIn(
            "--extra-elf=build/fixtures/modules/synthetic.prx@0x08c00000",
            plan["commands"]["codegen"],
        )

    def test_cli_is_deterministic_and_contains_no_hst_profile(self) -> None:
        command = [
            sys.executable,
            str(ROOT / "tools" / "title_codegen_plan.py"),
            str(self.path),
            "--game-name=synthetic",
            "--game-elf=build/fixtures/synthetic.elf",
            "--build-dir=build/synthetic",
            "--profile=none",
            "--funcs-per-chunk=64",
        ]
        first = subprocess.run(
            command, cwd=ROOT, capture_output=True, text=True, check=False
        )
        second = subprocess.run(
            command, cwd=ROOT, capture_output=True, text=True, check=False
        )
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(first.stdout, second.stdout)
        parsed = json.loads(first.stdout)
        self.assertEqual(parsed["environment"]["HST_EXTRA_SPANS"], "")
        self.assertNotIn("--profile=hst", parsed["commands"]["codegen"])


if __name__ == "__main__":
    unittest.main()
