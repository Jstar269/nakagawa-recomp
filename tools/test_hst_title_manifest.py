# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The public HST title manifest: tracked, public-included, scrubbed, and on the manager contract.

The manifest is public by owner decision (2026-10-27): it ships so a new user's
player recognizes UCUS98701 as the built-in ``hst`` title. These tests hold it to
that decision -- tracked and included, never ignored or excluded -- and to the
scrub the decision required: addresses, disc identity, module names and load
addresses, profiles and runtime bindings, and no private paths or evidence.
"""

from __future__ import annotations

import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import publication_policy
import test_public_title_isolation
import title_manifest

REL = "assets/titles/hst-ucus98701.json"

#: The manager's standard repository-relative input layout under place_game_here/.
MANAGER_INPUT_LAYOUT = {
    "executable": "place_game_here/EBOOT.elf",
    "module_dir": "place_game_here/EXTRACTED/decrypted",
    "psp_header": "place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN",
}

#: Every top-level field the public manifest may carry. A new field is a new
#: publication decision, not a silent addition.
PUBLIC_FIELDS = {
    "schema_version", "id", "display_name", "kind", "disc", "executable", "modules",
    "filesystem", "hle_profile", "codegen_profile", "feature_requirements",
    "verification_profile", "runtime_bindings", "notes",
}


class HstTitleManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.path = ROOT / REL

    def setUp(self) -> None:
        self.raw = self.path.read_bytes()
        self.document = json.loads(self.raw.decode("utf-8"))
        self.manifest = title_manifest.validate_manifest(title_manifest.load_manifest(self.path))

    def test_manifest_is_tracked_and_public_included(self) -> None:
        self.assertTrue(test_public_title_isolation.is_tracked(REL), f"{REL} must be tracked")
        policy = publication_policy.load_policy(ROOT / "assets" / "public_source_profile.json")
        resolution = policy.resolve(REL)
        self.assertEqual(resolution.disposition, publication_policy.INCLUDED)
        self.assertFalse(resolution.is_excluded)
        self.assertNotIn(REL, policy.exclude_paths)
        gitignore = (ROOT / ".gitignore").read_text(encoding="utf-8").splitlines()
        self.assertFalse(
            any(line.strip().lstrip("/") == REL for line in gitignore),
            ".gitignore must not hide the public HST manifest",
        )
        release = json.loads((ROOT / "assets" / "release_manifest.json").read_text(encoding="utf-8"))
        excluded = {
            item.get("path") if isinstance(item, dict) else item
            for item in release.get("excluded_paths", [])
        }
        self.assertNotIn(REL, excluded)

    def test_public_hst_identity_and_zero_based_executable_policy(self) -> None:
        self.assertEqual(self.manifest["id"], "hst-ucus98701-v1")
        self.assertEqual(self.manifest["kind"], "retail")
        self.assertEqual(self.manifest["disc"], {
            "id": "UCUS98701",
            "region": "NA",
            "revision_policy": "exact-disc-id",
        })
        self.assertEqual(self.manifest["executable"]["base"], 0)
        self.assertEqual(self.manifest["executable"]["entry"], 0)
        self.assertEqual(self.manifest["executable"]["bss_metadata_source"], "psp-header")
        self.assertEqual(self.manifest["codegen_profile"], "hst")
        self.assertEqual(self.manifest["hle_profile"], "hst-current")
        self.assertEqual(self.manifest["verification_profile"], "hst-native-player-package")
        bindings = self.manifest["runtime_bindings"]
        for name in ("fallback_entry", "worker_thread_entry", "launcher_thread_entry"):
            self.assertIsInstance(bindings[name], int, name)
            self.assertGreater(bindings[name], 0, name)

    def test_module_names_and_load_addresses_match_private_adapter(self) -> None:
        adapter = (ROOT / "tools" / "title_manager_plan.ps1").read_text(encoding="utf-8")
        for module in self.manifest["modules"]:
            self.assertIn(module["name"], adapter)
        self.assertNotIn("CODEGEN_PROFILE_ARG := --profile=hst", (ROOT / "Makefile").read_text(encoding="utf-8"))

    def test_extra_executable_span_is_owned_by_the_manifest_plan(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertNotIn("HST_EXTRA_SPANS", makefile)
        self.assertNotIn("0x00303194,0x00306e24", makefile)
        expected = self.manifest["executable"]["extra_executable_spans"]
        self.assertEqual(expected, [{"start": 0x00303194, "end": 0x00306E24}])
        adapter = (ROOT / "tools" / "title_manager_plan.ps1").read_text(encoding="utf-8")
        self.assertIn("3158420; end = 3173924", adapter)

    def test_zero_base_and_entry_match_manager_contract(self) -> None:
        # The manager hands Make the planner's values, never a literal of its own.
        manager = (ROOT / "nk_manager.ps1").read_text(encoding="utf-8")
        self.assertIn('"GAME_BASE=$($script:TitleManagerPlan.make.game_base)"', manager)
        self.assertIn('"GAME_ENTRY=$($script:TitleManagerPlan.make.game_entry)"', manager)
        planner = subprocess.run(
            [
                sys.executable, str(ROOT / "tools" / "title_codegen_plan.py"), str(self.path),
                "--manager-plan", "--game-name=hst",
                f"--game-elf={MANAGER_INPUT_LAYOUT['executable']}",
                "--build-dir=build/hst",
                f"--module-dir={MANAGER_INPUT_LAYOUT['module_dir']}",
                f"--psp-header={MANAGER_INPUT_LAYOUT['psp_header']}",
            ],
            cwd=ROOT, capture_output=True, text=True, check=True,
        )
        plan = json.loads(planner.stdout)
        self.assertEqual(plan["make"]["game_base"], "0")
        self.assertEqual(plan["make"]["game_entry"], "0")
        self.assertEqual(plan["make"]["codegen_profile_arg"], "--profile=hst")
        self.assertEqual(plan["environment"]["GAME_BASE"], "0x00000000")
        self.assertEqual(plan["environment"]["GAME_ENTRY"], "0x00000000")
        self.assertEqual(plan["environment"]["TITLE_EXTRA_SPANS"], "0x00303194,0x00306e24")

    def test_manifest_contains_no_private_evidence_fields(self) -> None:
        rendered = title_manifest.canonical_json(self.manifest).lower()
        for forbidden in (
            "sha256",
            "private_key",
            "oracle",
            "decompiler_output",
            "savedata",
            "screenshot",
        ):
            self.assertNotIn(forbidden, rendered)
        self.assertIn("-TitleManifest", self.manifest["notes"])

    def test_manifest_is_scrubbed_of_private_paths(self) -> None:
        self.assertLessEqual(set(self.document), PUBLIC_FIELDS)
        text = self.raw.decode("utf-8")
        self.assertTrue(text.isascii(), "the public manifest is plain ASCII")
        lowered = text.lower()
        self.assertNotIn("\\", text, "no Windows paths or escapes")
        for marker in ("c:/", "/users/", "/home/", "appdata", "jstar", ".iso", "xbdata_extracted"):
            self.assertNotIn(marker, lowered, marker)
        policy = publication_policy.load_policy(ROOT / "assets" / "public_source_profile.json")
        for private_root in policy.private_roots:
            self.assertNotIn(private_root.lower(), lowered)

        filesystem = self.document["filesystem"]
        # disc_image would name the user's own dump; the player finds the disc itself.
        self.assertNotIn("disc_image", filesystem)
        self.assertEqual({key: filesystem[key] for key in MANAGER_INPUT_LAYOUT}, MANAGER_INPUT_LAYOUT)
        self.assertEqual(filesystem["data_root"], "xbdata")
        self.assertEqual(filesystem["memory_stick_root"], "memstick")
        paths = [filesystem["data_root"], filesystem["memory_stick_root"], *MANAGER_INPUT_LAYOUT.values()]
        paths += [root["root"] for root in filesystem.get("loose_content_roots", [])]
        for value in paths:
            pure = PurePosixPath(value)
            self.assertFalse(pure.is_absolute(), value)
            self.assertNotIn("..", pure.parts, value)
            self.assertNotIn("\\", value)
            self.assertNotIn(":", value)
        for module in self.document["modules"]:
            self.assertTrue(module["guest_path"].startswith("disc0:/PSP_GAME/USRDIR/module/"), module)
        # Addresses only: no bytes, no digests, no extra binding fields beyond the schema's.
        self.assertNotRegex(lowered, r"[0-9a-f]{32}")


if __name__ == "__main__":
    unittest.main()
