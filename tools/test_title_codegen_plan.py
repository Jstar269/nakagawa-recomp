# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import title_codegen_plan
import title_manifest


class TitleCodegenPlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.hst_path = ROOT / "assets" / "titles" / "hst-ucus98701.json"
        self.synthetic_path = ROOT / "assets" / "titles" / "synthetic.json"
        self.synthetic = title_manifest.validate_manifest(
            title_manifest.load_manifest(self.synthetic_path)
        )
        self.hst = (
            title_manifest.validate_manifest(title_manifest.load_manifest(self.hst_path))
            if self.hst_path.is_file()
            else None
        )

    def plan(self, manifest=None, **overrides):
        values = {
            "game_name": "synthetic",
            "game_elf": Path("fixtures/synthetic.elf"),
            "build_dir": Path("build/synthetic"),
            "module_dir": None,
            "psp_header": None,
            "codegen_profile": None,
            "include_optional_modules": set(),
            "funcs_per_chunk": 2000,
        }
        values.update(overrides)
        selected = self.synthetic if manifest is None else manifest
        return title_codegen_plan.build_plan(selected, **values)

    def test_hst_plan_matches_current_makefile_contract(self) -> None:
        if self.hst is None:
            self.skipTest("private HST title manifest is unavailable in the sanitized public tree")
        plan = self.plan(
            self.hst,
            game_name="hst",
            game_elf=Path("place_game_here/EBOOT.elf"),
            build_dir=Path("build/hst"),
            module_dir=Path("place_game_here/EXTRACTED/decrypted"),
            psp_header=Path("place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN"),
            codegen_profile="hst",
        )
        self.assertEqual(plan["game_base"], 0)
        self.assertEqual(plan["game_entry"], 0)
        self.assertEqual(plan["codegen_profile"], "hst")
        # Generic contract now emits only TITLE_EXTRA_SPANS (host-portable).
        # HST_EXTRA_SPANS is legacy and lives only in the HST compatibility layer
        # (Makefile and PowerShell adapter), not in the generic planner. For HST,
        # the planner emits TITLE only; the adapter synthesizes HST for legacy consumers.
        self.assertEqual(plan["environment"]["GAME_BASE"], "0x00000000")
        self.assertEqual(plan["environment"]["GAME_ENTRY"], "0x00000000")
        self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], "0x00303194,0x00306e24")
        self.assertNotIn("HST_EXTRA_SPANS", plan["environment"])
        self.assertEqual(plan["commands"]["prxload"], [
            "python", "tools/prxload.py", "place_game_here/EBOOT.elf", "0x00000000",
            "--psp-header=place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN",
            "--out=build/hst/hst_image.bin",
        ])
        self.assertEqual(plan["commands"]["codegen"], [
            "python", "tools/codegen.py", "place_game_here/EBOOT.elf",
            "build/hst/hst_recomp.c", "--base=0x00000000", "--profile=hst",
            "--extra-elf=place_game_here/EXTRACTED/decrypted/libfont.prx@0x32200000",
            "--extra-elf=place_game_here/EXTRACTED/decrypted/scePsmf_library.prx@0x32280000",
            "--extra-elf=place_game_here/EXTRACTED/decrypted/scePsmfP_library.prx@0x322f8868",
            "--funcs-per-chunk=2000",
        ])
        self.assertEqual(plan["commands"]["imports"], [
            "python", "tools/imports.py", "place_game_here/EBOOT.elf", "0x00000000",
            "--toml=build/hst/hst_imports.toml",
        ])

    def test_generic_plan_explicitly_disables_hst_default_span(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["id"] = "synthetic-test-v1"
        manifest["executable"] = {
            "base": 0x08804000,
            "entry": 0x08804128,
            "bss_metadata_source": "elf",
            "extra_executable_spans": [],
        }
        manifest["modules"] = []
        manifest["codegen_profile"] = "none"
        plan = self.plan(
            manifest,
            game_name="synthetic",
            module_dir=None,
            psp_header=None,
            codegen_profile="none",
        )
        self.assertEqual(plan["environment"]["GAME_BASE"], "0x08804000")
        self.assertEqual(plan["environment"]["GAME_ENTRY"], "0x08804128")
        self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], "")
        self.assertNotIn("HST_EXTRA_SPANS", plan["environment"])
        self.assertEqual(plan["codegen_profile"], "none")
        self.assertNotIn("--profile=hst", plan["commands"]["codegen"])

    def test_psp_header_is_required_only_by_manifest_policy(self) -> None:
        psp_manifest = copy.deepcopy(self.synthetic)
        psp_manifest["executable"] = dict(psp_manifest["executable"])
        psp_manifest["executable"]["bss_metadata_source"] = "psp-header"
        with self.assertRaisesRegex(ValueError, "psp_header is required"):
            self.plan(psp_manifest, psp_header=None)
        generic = copy.deepcopy(self.synthetic)
        generic["executable"] = dict(generic["executable"])
        generic["executable"]["bss_metadata_source"] = "elf"
        with self.assertRaisesRegex(ValueError, "incompatible"):
            self.plan(generic, psp_header=Path("unexpected.bin"))

    def test_module_directory_is_required_for_guest_modules(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["modules"] = [{
            "name": "required.prx",
            "load_address": 1,
            "required": True,
            "role": "guest-prx",
        }]
        with self.assertRaisesRegex(ValueError, "module_dir is required"):
            self.plan(manifest, module_dir=None)

    def test_hle_capabilities_are_not_passed_as_guest_elf_files(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["modules"] = [
            {
                "name": "kernel-capability",
                "load_address": 1,
                "required": True,
                "role": "hle-capability",
            }
        ]
        plan = self.plan(manifest, module_dir=None)
        self.assertFalse(any(arg.startswith("--extra-elf=") for arg in plan["commands"]["codegen"]))

    def test_optional_guest_modules_require_explicit_selection(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["modules"] = [
            {
                "name": "required.prx",
                "load_address": 1,
                "required": True,
                "role": "guest-prx",
            },
            {
                "name": "optional.prx",
                "load_address": 2,
                "required": False,
                "role": "optional-guest-prx",
            },
        ]
        default = self.plan(manifest, module_dir=Path("modules"))
        self.assertTrue(any("required.prx@" in arg for arg in default["commands"]["codegen"]))
        self.assertFalse(any("optional.prx@" in arg for arg in default["commands"]["codegen"]))
        selected = self.plan(
            manifest,
            module_dir=Path("modules"),
            include_optional_modules={"optional.prx"},
        )
        self.assertTrue(any("optional.prx@" in arg for arg in selected["commands"]["codegen"]))
        with self.assertRaisesRegex(ValueError, "unknown optional"):
            self.plan(
                manifest,
                module_dir=Path("modules"),
                include_optional_modules={"missing.prx"},
            )

    def test_module_binding_rejects_ambiguous_separator(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["modules"] = [{
            "name": "required.prx",
            "load_address": 1,
            "required": True,
            "role": "guest-prx",
        }]
        with self.assertRaisesRegex(ValueError, "must not contain '@'"):
            self.plan(manifest, module_dir=Path("private@modules"))

    def test_current_analyzer_limit_fails_closed(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["executable"] = dict(manifest["executable"])
        manifest["executable"]["extra_executable_spans"] = [
            {"start": 1, "end": 2}, {"start": 3, "end": 4}
        ]
        with self.assertRaisesRegex(ValueError, "at most one"):
            self.plan(manifest)
        manifest["executable"]["extra_executable_spans"] = [{"start": 1, "end": 2}]
        manifest["executable"]["base"] = 0x08804000
        manifest["codegen_profile"] = "none"
        with self.assertRaisesRegex(ValueError, "nonzero base"):
            self.plan(manifest, codegen_profile="none")
        manifest["executable"]["extra_executable_spans"] = []
        manifest["codegen_profile"] = "hst"
        with self.assertRaisesRegex(ValueError, "hst codegen profile requires"):
            self.plan(manifest)

    def test_profiles_and_chunk_bounds_fail_closed(self) -> None:
        for profile in ("unknown", " HST", "hst/unsafe", ""):
            with self.subTest(profile=profile):
                with self.assertRaises(ValueError):
                    self.plan(codegen_profile=profile)
        for count in (0, -1, 100001, True):
            with self.subTest(count=count):
                with self.assertRaises(ValueError):
                    self.plan(funcs_per_chunk=count)

    def test_unused_module_binding_and_bad_paths_fail_closed(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["modules"] = []
        with self.assertRaisesRegex(ValueError, "no guest modules"):
            self.plan(manifest, module_dir=Path("unused"))
        with self.assertRaisesRegex(ValueError, "control characters"):
            self.plan(game_elf=Path("bad\nelf"))
        with self.assertRaisesRegex(ValueError, "identify a file"):
            self.plan(build_dir=Path("."))
        with self.assertRaisesRegex(ValueError, "must be strings"):
            self.plan(include_optional_modules={1})

    def test_windows_style_bindings_are_rendered_with_forward_slashes(self) -> None:
        manifest = copy.deepcopy(self.synthetic)
        manifest["modules"] = [{
            "name": "required.prx",
            "load_address": 1,
            "required": True,
            "role": "guest-prx",
        }]
        plan = self.plan(
            manifest,
            game_elf=Path(r"C:\private\EBOOT.elf"),
            build_dir=Path(r"C:\repo\build\synthetic"),
            module_dir=Path(r"C:\private\modules"),
        )
        rendered = json.dumps(plan)
        self.assertNotIn("\\\\", rendered)
        self.assertIn("C:/private/EBOOT.elf", rendered)
        self.assertIn("C:/repo/build/synthetic/synthetic_recomp.c", rendered)

    def test_plan_json_is_deterministic(self) -> None:
        first = title_codegen_plan.canonical_json(self.plan())
        second = title_codegen_plan.canonical_json(self.plan())
        self.assertEqual(first, second)
        self.assertEqual(json.loads(first)["title_manifest_id"], "synthetic-allegrex-v1")

    def test_manifest_profile_is_authoritative_when_cli_profile_is_omitted(self) -> None:
        plan = title_codegen_plan.build_plan(
            self.synthetic,
            game_name="synthetic",
            game_elf=Path("fixtures/synthetic.elf"),
            build_dir=Path("build/synthetic"),
            module_dir=None,
            psp_header=None,
            codegen_profile=None,
        )
        self.assertEqual(plan["codegen_profile"], "none")
        with self.assertRaisesRegex(ValueError, "conflicts with the manifest"):
            self.plan(codegen_profile="hst")

    def test_cli_emits_json_and_never_executes_codegen(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            out_c = Path(temp) / "build" / "synthetic" / "synthetic_recomp.c"
            proc = subprocess.run([
                sys.executable, str(ROOT / "tools" / "title_codegen_plan.py"),
                str(self.synthetic_path), "--game-name=synthetic",
                "--game-elf=fixtures/synthetic.elf", "--build-dir=build/synthetic",
                "--profile=none",
            ], cwd=ROOT, capture_output=True, text=True, check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(json.loads(proc.stdout)["game_name"], "synthetic")
            self.assertFalse(out_c.exists())


if __name__ == "__main__":
    unittest.main()
