# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import title_codegen_plan
import title_manifest


class TitleManagerPlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.hst_path = ROOT / "assets" / "titles" / "hst-ucus98701.json"
        self.hst = (
            title_manifest.load_manifest(self.hst_path)
            if self.hst_path.is_file()
            else None
        )
        self.synthetic = title_manifest.load_manifest(ROOT / "assets" / "titles" / "synthetic.json")

    def hst_plan(self, manifest=None, **overrides):
        values = {
            "game_name": "hst",
            "game_elf": Path("place_game_here/EBOOT.elf"),
            "build_dir": Path("build/hst"),
            "module_dir": Path("place_game_here/EXTRACTED/decrypted"),
            "psp_header": Path("place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN"),
            "codegen_profile": None,
            "funcs_per_chunk": 2000,
        }
        values.update(overrides)
        return title_codegen_plan.build_manager_plan(
            self.hst if manifest is None else manifest,
            **values,
        )

    def synthetic_plan(self, manifest=None, **overrides):
        values = {
            "game_name": "synthetic",
            "game_elf": Path("fixtures/synthetic.elf"),
            "build_dir": Path("build/synthetic"),
            "module_dir": None,
            "psp_header": None,
            "codegen_profile": None,
            "funcs_per_chunk": 64,
        }
        values.update(overrides)
        return title_codegen_plan.build_manager_plan(
            self.synthetic if manifest is None else manifest,
            **values,
        )

    def test_hst_plan_is_bounded_and_contains_only_manager_fields(self) -> None:
        if self.hst is None:
            self.skipTest("private HST title manifest is unavailable in the sanitized public tree")
        plan = self.hst_plan()
        self.assertEqual(plan["plan_version"], 1)
        self.assertEqual(plan["plan_kind"], "title-manager-build")
        self.assertEqual(plan["title_manifest_id"], "hst-ucus98701-v1")
        self.assertEqual(plan["game_name"], "hst")
        self.assertEqual(plan["game_base"], 0)
        self.assertEqual(plan["game_entry"], 0)
        self.assertEqual(plan["codegen_profile"], "hst")
        self.assertEqual(plan["bss_metadata_source"], "psp-header")
        self.assertEqual(plan["extra_executable_spans"], [{"start": 3158420, "end": 3173924}])
        self.assertEqual(
            plan["required_guest_modules"],
            [
                {"name": "libfont.prx", "load_address": 840957952},
                {"name": "scePsmf_library.prx", "load_address": 841482240},
                {"name": "scePsmfP_library.prx", "load_address": 841975912},
            ],
        )
        self.assertEqual(plan["optional_guest_modules"], [])
        self.assertEqual(
            plan["private_binding_requirements"],
            {"game_elf": True, "module_dir": True, "psp_header": True},
        )
        self.assertEqual(plan["make"], {
            "game_name": "hst",
            "game_base": "0",
            "game_entry": "0",
            "codegen_profile_arg": "--profile=hst",
            "build_dir": "build/hst",
            "funcs_per_chunk": 2000,
        })
        rendered = json.dumps(plan, sort_keys=True)
        self.assertNotIn("place_game_here", rendered)
        self.assertNotIn("EBOOT", rendered)
        self.assertNotIn("commands", plan)

    def test_synthetic_plan_uses_generic_profile_and_clears_hst_span(self) -> None:
        plan = self.synthetic_plan(game_elf=Path("build/fixtures/synthetic.elf"))
        self.assertEqual(plan["title_kind"], "synthetic")
        self.assertEqual(plan["game_base"], 0x08800000)
        self.assertEqual(plan["game_entry"], 0x08800000)
        self.assertEqual(plan["codegen_profile"], "none")
        self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], "")
        self.assertNotIn("HST_EXTRA_SPANS", plan["environment"])
        self.assertEqual(plan["required_guest_modules"], [])
        self.assertEqual(plan["optional_guest_modules"], [])
        self.assertEqual(plan["private_binding_requirements"]["module_dir"], False)
        self.assertEqual(plan["make"]["codegen_profile_arg"], "")

    def test_profile_conflict_and_bad_span_fail_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "conflicts with the manifest"):
            self.synthetic_plan(codegen_profile="hst")
        bad = copy.deepcopy(self.synthetic)
        bad["executable"] = copy.deepcopy(bad["executable"])
        bad["executable"]["extra_executable_spans"] = [
            {"start": 1, "end": 2},
            {"start": 3, "end": 4},
        ]
        with self.assertRaisesRegex(ValueError, "at most one"):
            self.synthetic_plan(manifest=bad)

    def test_cli_manager_output_is_deterministic(self) -> None:
        command = [
            sys.executable,
            str(ROOT / "tools" / "title_codegen_plan.py"),
            str(ROOT / "assets" / "titles" / "synthetic.json"),
            "--manager-plan",
            "--game-name=synthetic",
            "--game-elf=fixtures/synthetic.elf",
            "--build-dir=build/synthetic",
        ]
        first = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
        second = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(first.stdout, second.stdout)
        parsed = json.loads(first.stdout)
        self.assertEqual(parsed["plan_version"], 1)
        self.assertEqual(parsed["title_manifest_id"], "synthetic-allegrex-v1")
        self.assertNotIn("commands", parsed)


if __name__ == "__main__":
    unittest.main()
