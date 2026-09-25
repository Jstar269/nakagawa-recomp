# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Unit tests for enhancement package declaration schema and validator.

Tests fail-closed boundaries:
- Valid synthetic package passes.
- Unknown category is rejected (including obsolete duplicate guest_hook).
- Incompatible schema version is rejected.
- Address-literal rejection across hook symbols and capability names (0x-hex,
  decompiler-style sub_/loc_/func_ stems, bare 6-8 digit hex strings including
  all-letter ones like 'deadbeef', and numeric addresses).
- Speculative category payload objects and undeclared properties fail closed.
- Zero jsonschema dependency: normative validator runs in pure Python.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from enhancement_package import (
    HOOK_KEYS,
    HOOK_TYPES,
    REQUIRED_ROOT_KEYS,
    ROOT_KEYS,
    SUPPORTED_SCHEMA_VERSION,
    VALID_CATEGORIES,
    EnhancementPackageError,
    load_package,
    validate_package,
)

SCHEMA_PATH = ROOT / "assets" / "enhancement_package.schema.json"


class TestEnhancementPackageSchema(unittest.TestCase):
    """Test enhancement package schema validation and fail-closed rules."""

    def setUp(self) -> None:
        self.valid_presentation_pkg: dict[str, Any] = {
            "schema_version": 1,
            "id": "synth.overlay.fps",
            "display_name": "Synthetic Frame Rate Overlay",
            "version": "1.0.0",
            "category": "presentation_only",
            "target_title_id": "*",
            "capabilities": ["hud_overlay", "fps_counter"],
        }

        self.valid_asset_pkg: dict[str, Any] = {
            "schema_version": 1,
            "id": "synth.textures.hd",
            "display_name": "Synthetic HD Texture Pack",
            "version": "0.1.0",
            "category": "asset_replacement",
            "target_title_id": "synthetic-allegrex-v1",
            "capabilities": ["texture_override"],
        }

        self.valid_hook_pkg: dict[str, Any] = {
            "schema_version": 1,
            "id": "synth.hook.intro_skip",
            "display_name": "Synthetic Intro Skip Hook",
            "version": "1.0.0",
            "category": "guest_memory_code_hook",
            "target_title_id": "synthetic-allegrex-v1",
            "capabilities": ["milestone_skip"],
            "hooks": [
                {
                    "symbol": "title_intro_movie_start",
                    "type": "pre_call",
                }
            ],
        }

    def test_schema_file_matches_python_contract(self) -> None:
        """The checked-in schema must be valid JSON Draft 2020-12 matching Python contract."""
        self.assertEqual(SUPPORTED_SCHEMA_VERSION, 1)
        self.assertTrue(SCHEMA_PATH.is_file(), f"Schema file missing: {SCHEMA_PATH}")
        with SCHEMA_PATH.open("r", encoding="utf-8") as f:
            schema = json.load(f)

        self.assertEqual(schema.get("$schema"), "https://json-schema.org/draft/2020-12/schema")
        self.assertFalse(schema.get("additionalProperties"))
        self.assertEqual(schema.get("properties", {}).get("schema_version", {}).get("const"), 1)
        self.assertEqual(set(schema.get("required", [])), REQUIRED_ROOT_KEYS)
        self.assertEqual(set(schema.get("properties", {}).keys()), ROOT_KEYS)

        # Category enum must exactly match VALID_CATEGORIES
        schema_categories = set(schema["properties"]["category"]["enum"])
        self.assertEqual(schema_categories, VALID_CATEGORIES)
        self.assertNotIn("guest_hook", schema_categories)

        # Hooks definition must be symbol + type only
        hook_item = schema["properties"]["hooks"]["items"]
        self.assertFalse(hook_item.get("additionalProperties"))
        self.assertEqual(set(hook_item.get("required", [])), HOOK_KEYS)
        self.assertEqual(set(hook_item.get("properties", {}).keys()), HOOK_KEYS)
        self.assertEqual(set(hook_item["properties"]["type"]["enum"]), HOOK_TYPES)

        # Speculative category payload objects must NOT exist in schema
        for speculative in ("assets", "presentation", "timing", "camera", "save_data"):
            self.assertNotIn(speculative, schema.get("properties", {}))

    def test_load_package_from_disk(self) -> None:
        """load_package must correctly read and parse JSON packages."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            file_path = Path(tmp_dir) / "test_pkg.json"
            file_path.write_text(json.dumps(self.valid_presentation_pkg), encoding="utf-8")
            loaded = load_package(file_path)
            self.assertEqual(loaded["id"], "synth.overlay.fps")

            # Missing file raises EnhancementPackageError
            missing_path = Path(tmp_dir) / "nonexistent.json"
            with self.assertRaises(EnhancementPackageError):
                load_package(missing_path)

            # Malformed JSON raises EnhancementPackageError
            bad_json_path = Path(tmp_dir) / "bad.json"
            bad_json_path.write_text("{invalid_json", encoding="utf-8")
            with self.assertRaises(EnhancementPackageError):
                load_package(bad_json_path)

            # Non-dict root raises EnhancementPackageError
            array_json_path = Path(tmp_dir) / "array.json"
            array_json_path.write_text("[]", encoding="utf-8")
            with self.assertRaises(EnhancementPackageError):
                load_package(array_json_path)

    def test_valid_synthetic_packages_pass_validation(self) -> None:
        """Standard synthetic packages across categories must validate cleanly."""
        validated_pres = validate_package(self.valid_presentation_pkg)
        self.assertEqual(validated_pres["category"], "presentation_only")

        validated_asset = validate_package(self.valid_asset_pkg)
        self.assertEqual(validated_asset["category"], "asset_replacement")

        validated_hook = validate_package(self.valid_hook_pkg)
        self.assertEqual(validated_hook["category"], "guest_memory_code_hook")

        # Package with optional min_runtime_version passes
        pkg_with_min_rt = copy.deepcopy(self.valid_presentation_pkg)
        pkg_with_min_rt["min_runtime_version"] = "1.0.0"
        self.assertEqual(validate_package(pkg_with_min_rt)["min_runtime_version"], "1.0.0")

    def test_unknown_category_rejected(self) -> None:
        """Unknown or arbitrary category strings must fail closed."""
        pkg = copy.deepcopy(self.valid_presentation_pkg)
        pkg["category"] = "unsupported_magic_accelerator"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg)
        self.assertIn("Unknown enhancement category", str(ctx.exception))

        # Obsolete duplicate 'guest_hook' is explicitly rejected
        pkg_guest_hook = copy.deepcopy(self.valid_presentation_pkg)
        pkg_guest_hook["category"] = "guest_hook"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_guest_hook)
        self.assertIn("Unknown enhancement category", str(ctx.exception))

    def test_incompatible_version_rejected(self) -> None:
        """Schema versions other than 1 must be rejected."""
        # Future version
        pkg_future = copy.deepcopy(self.valid_presentation_pkg)
        pkg_future["schema_version"] = 2
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_future)
        self.assertIn("Incompatible schema version", str(ctx.exception))

        # Obsolete / zero version
        pkg_zero = copy.deepcopy(self.valid_presentation_pkg)
        pkg_zero["schema_version"] = 0
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_zero)
        self.assertIn("Incompatible schema version", str(ctx.exception))

        # Boolean schema version
        pkg_bool = copy.deepcopy(self.valid_presentation_pkg)
        pkg_bool["schema_version"] = True
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_bool)
        self.assertIn("Incompatible schema version", str(ctx.exception))

        # Missing schema version
        pkg_missing = copy.deepcopy(self.valid_presentation_pkg)
        del pkg_missing["schema_version"]
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_missing)
        self.assertIn("Missing required property 'schema_version'", str(ctx.exception))

    def test_address_literal_rejection_in_hook_symbols(self) -> None:
        """Hook symbols must reject 0x-hex, decompiler stems, bare hex, and numeric addresses."""
        cases = [
            ("0x08804000", "0x-prefixed hex address"),
            ("0xdeadbeef", "0x-prefixed all-letter hex"),
            ("0X08804000", "0X-prefixed uppercase hex"),
            ("sub_08804000", "decompiler sub_ stem with hex"),
            ("sub_deadbeef", "decompiler sub_ stem with all-letter hex"),
            ("loc_08804000", "decompiler loc_ stem with hex"),
            ("loc_deadbeef", "decompiler loc_ stem with all-letter hex"),
            ("func_08804000", "decompiler func_ stem with hex"),
            ("func_deadbeef", "decompiler func_ stem with all-letter hex"),
            ("08804000", "bare 8-digit hex string"),
            ("123456", "bare 6-digit hex string"),
            ("12345678", "bare 8-digit numeric hex string"),
            ("deadbeef", "bare 8-digit all-letter hex string"),
            ("cafebabe", "bare 8-digit all-letter hex string cafebabe"),
            ("abcdef", "bare 6-digit all-letter hex string abcdef"),
            (142622720, "numeric integer address literal"),
        ]

        for symbol_val, label in cases:
            with self.subTest(symbol=symbol_val, label=label):
                pkg = copy.deepcopy(self.valid_hook_pkg)
                pkg["hooks"][0]["symbol"] = symbol_val
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(pkg)
                self.assertIn("Address-literal hook symbol rejected", str(ctx.exception))

    def test_address_literal_rejection_in_capabilities(self) -> None:
        """Capability names must reject 0x-hex, decompiler stems, bare hex, and numeric addresses."""
        cases = [
            ("0x08804000", "0x-prefixed hex address"),
            ("0xdeadbeef", "0x-prefixed all-letter hex"),
            ("sub_08804000", "decompiler sub_ stem with hex"),
            ("sub_deadbeef", "decompiler sub_ stem with all-letter hex"),
            ("loc_08804000", "decompiler loc_ stem with hex"),
            ("loc_deadbeef", "decompiler loc_ stem with all-letter hex"),
            ("func_08804000", "decompiler func_ stem with hex"),
            ("func_deadbeef", "decompiler func_ stem with all-letter hex"),
            ("08804000", "bare 8-digit hex string"),
            ("123456", "bare 6-digit hex string"),
            ("deadbeef", "bare 8-digit all-letter hex string"),
            ("cafebabe", "bare 8-digit all-letter hex string cafebabe"),
            ("abcdef", "bare 6-digit all-letter hex string abcdef"),
            (142622720, "numeric integer address literal"),
        ]

        for cap_val, label in cases:
            with self.subTest(capability=cap_val, label=label):
                pkg = copy.deepcopy(self.valid_presentation_pkg)
                pkg["capabilities"] = [cap_val]
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(pkg)
                self.assertIn("Address-literal capability name rejected", str(ctx.exception))

    def test_speculative_payload_objects_rejected(self) -> None:
        """Speculative category payload objects and extra properties must fail closed."""
        speculative_keys = [
            ("assets", [{"asset_id": "tex1", "replacement_path": "a.png", "kind": "texture"}]),
            ("presentation", {"overlay_enabled": True}),
            ("timing", {"target_fps": 60}),
            ("camera", {"fov_multiplier": 1.5}),
            ("save_data", {"format_version": 1}),
            ("description", "A description string"),
            ("author", "Author Name"),
            ("untrusted_script", "print('hello')"),
        ]

        for key, value in speculative_keys:
            with self.subTest(key=key):
                pkg = copy.deepcopy(self.valid_presentation_pkg)
                pkg[key] = value
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(pkg)
                self.assertIn("Unknown properties forbidden", str(ctx.exception))

        # Extraneous properties in hook item must also fail closed
        hook_pkg_with_desc = copy.deepcopy(self.valid_hook_pkg)
        hook_pkg_with_desc["hooks"][0]["description"] = "A description in hook"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(hook_pkg_with_desc)
        self.assertIn("Unknown properties forbidden in hook", str(ctx.exception))

        hook_pkg_with_addr = copy.deepcopy(self.valid_hook_pkg)
        hook_pkg_with_addr["hooks"][0]["guest_addr"] = "0x08804000"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(hook_pkg_with_addr)
        self.assertIn("Unknown properties forbidden in hook", str(ctx.exception))

    def test_missing_required_fields_rejected(self) -> None:
        """Packages missing required metadata must fail validation."""
        for field in ("id", "display_name", "version", "category", "target_title_id", "capabilities"):
            with self.subTest(field=field):
                pkg = copy.deepcopy(self.valid_presentation_pkg)
                del pkg[field]
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(pkg)
                self.assertIn(f"Missing required property '{field}'", str(ctx.exception))

    def test_capabilities_validation(self) -> None:
        """Capabilities list must be non-empty array of unique valid identifiers."""
        # Empty array
        pkg_empty = copy.deepcopy(self.valid_presentation_pkg)
        pkg_empty["capabilities"] = []
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_empty)
        self.assertIn("capabilities array must contain at least 1 item", str(ctx.exception))

        # Duplicates
        pkg_dup = copy.deepcopy(self.valid_presentation_pkg)
        pkg_dup["capabilities"] = ["hud_overlay", "hud_overlay"]
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_dup)
        self.assertIn("duplicate capability", str(ctx.exception))

        # Non-array
        pkg_str = copy.deepcopy(self.valid_presentation_pkg)
        pkg_str["capabilities"] = "hud_overlay"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_str)
        self.assertIn("capabilities must be an array", str(ctx.exception))

    def test_hooks_validation(self) -> None:
        """Hooks must be array of objects with valid symbol and type."""
        # Non-array
        pkg_not_list = copy.deepcopy(self.valid_hook_pkg)
        pkg_not_list["hooks"] = "not_a_list"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_not_list)
        self.assertIn("hooks must be an array", str(ctx.exception))

        # Missing symbol
        pkg_no_sym = copy.deepcopy(self.valid_hook_pkg)
        del pkg_no_sym["hooks"][0]["symbol"]
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_no_sym)
        self.assertIn("Missing required property 'symbol' in hook", str(ctx.exception))

        # Missing type
        pkg_no_type = copy.deepcopy(self.valid_hook_pkg)
        del pkg_no_type["hooks"][0]["type"]
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_no_type)
        self.assertIn("Missing required property 'type' in hook", str(ctx.exception))

        # Unknown type
        pkg_bad_type = copy.deepcopy(self.valid_hook_pkg)
        pkg_bad_type["hooks"][0]["type"] = "invalid_intercept"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg_bad_type)
        self.assertIn("Unknown hook type", str(ctx.exception))

    def test_cli_runner_success_and_failure(self) -> None:
        """CLI tool must report OK for valid packages and exit non-zero for invalid ones."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            valid_path = Path(tmp_dir) / "valid.json"
            valid_path.write_text(json.dumps(self.valid_presentation_pkg), encoding="utf-8")

            invalid_path = Path(tmp_dir) / "invalid.json"
            bad_pkg = copy.deepcopy(self.valid_hook_pkg)
            bad_pkg["hooks"][0]["symbol"] = "0x08900000"
            invalid_path.write_text(json.dumps(bad_pkg), encoding="utf-8")

            # Valid file CLI run
            res_ok = subprocess.run(
                [sys.executable, "-m", "enhancement_package", str(valid_path)],
                cwd=ROOT / "tools",
                capture_output=True,
                text=True,
            )
            self.assertEqual(res_ok.returncode, 0, f"CLI stdout: {res_ok.stdout}, stderr: {res_ok.stderr}")
            self.assertIn("OK", res_ok.stdout)

            # Invalid file CLI run
            res_bad = subprocess.run(
                [sys.executable, "-m", "enhancement_package", str(invalid_path)],
                cwd=ROOT / "tools",
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(res_bad.returncode, 0)
            self.assertIn("Address-literal hook symbol rejected", res_bad.stderr)


if __name__ == "__main__":
    unittest.main()
