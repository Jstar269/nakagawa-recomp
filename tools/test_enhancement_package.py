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
import hashlib
from io import StringIO
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from typing import Any
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import codegen
from enhancement_package import (
    AOT_ENHANCEMENT_PACKAGE_EPOCH,
    HOOK_KEYS,
    HOOK_TYPES,
    REQUIRED_ROOT_KEYS,
    RNG_POLICY_MODES,
    ROOT_KEYS,
    SITE_TRANSFORMS,
    SUPPORTED_SCHEMA_VERSION,
    VALID_CATEGORIES,
    EnhancementPackageError,
    compute_stock_words_digest,
    load_package,
    validate_package,
)
from nk_core import package_cache

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

        # Site descriptors: insn_offset is a byte offset the validator requires to be 4-aligned.
        site_props = schema["properties"]["sites"]["items"]["properties"]
        self.assertEqual(site_props["insn_offset"].get("multipleOf"), 4)
        self.assertIn("Byte offset", site_props["insn_offset"]["description"])
        self.assertEqual(set(site_props["transform"]["enum"]), SITE_TRANSFORMS)

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

    def test_timing_rescale_package_schema_and_fail_closed_validation(self) -> None:
        """Declarative 60 Hz frame-rate rescale packages must validate all fields and fail closed."""
        stock_word = 0x24020002  # li $v0, 2
        valid_60hz_pkg: dict[str, Any] = {
            "schema_version": 1,
            "id": "synth.timing.60hz",
            "display_name": "Synthetic 60 Hz Logic Package",
            "version": "1.0.0",
            "category": "timing_frame_presentation",
            "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.01",
            "executable_sha256": "a" * 64,
            "capabilities": ["logic_60hz_rescale", "vblank_wait_2_to_1"],
            "rng_policy": {
                "mode": "rescale_probability",
                "reason": "Halve stateless per-update probability thresholds and gate stateful multi-roll routines at 30 Hz.",
            },
            "sites": [
                {
                    "id": "S001",
                    "function": "synth_vblank_sync_loop",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([stock_word]),
                    "transform": "vblank_wait_2_to_1",
                }
            ],
        }
        validated = validate_package(valid_60hz_pkg)
        self.assertEqual(validated["game_version"], "1.01")
        self.assertEqual(validated["sites"][0]["word_count"], 1)
        self.assertEqual(validated["rng_policy"]["mode"], "rescale_probability")

        # Schema must match SITE_TRANSFORMS and RNG_POLICY_MODES
        with SCHEMA_PATH.open("r", encoding="utf-8") as f:
            schema = json.load(f)
        site_schema = schema["properties"]["sites"]["items"]
        self.assertEqual(set(site_schema["properties"]["transform"]["enum"]), SITE_TRANSFORMS)
        rng_schema = schema["properties"]["rng_policy"]
        self.assertEqual(set(rng_schema["properties"]["mode"]["enum"]), RNG_POLICY_MODES)

        # Missing cross-field timing properties must fail closed
        for missing_key in ("game_version", "executable_sha256", "rng_policy", "sites"):
            with self.subTest(missing_key=missing_key):
                incomplete = copy.deepcopy(valid_60hz_pkg)
                del incomplete[missing_key]
                with self.assertRaises(EnhancementPackageError):
                    validate_package(incomplete)

        # Wildcard target_title_id '*' is forbidden when sites are present
        wildcard_pkg = copy.deepcopy(valid_60hz_pkg)
        wildcard_pkg["target_title_id"] = "*"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(wildcard_pkg)
        self.assertIn("Timing rescale package must target a specific title id, not '*'", str(ctx.exception))

        # Address-literal and FUN_/sub_/loc_/func_ rejection in site function names
        bad_functions = [
            "0x08804000",
            "FUN_12345678",
            "fun_1234abcd",
            "sub_08804000",
            "loc_08804000",
            "func_08804000",
            "12345678",
            "deadbeef",
        ]
        for bad_fn in bad_functions:
            with self.subTest(bad_function=bad_fn):
                bad_pkg = copy.deepcopy(valid_60hz_pkg)
                bad_pkg["sites"][0]["function"] = bad_fn
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(bad_pkg)
                self.assertIn("Address-literal", str(ctx.exception))

        # Non-4-byte-aligned insn_offset rejected
        misaligned_pkg = copy.deepcopy(valid_60hz_pkg)
        misaligned_pkg["sites"][0]["insn_offset"] = 2
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(misaligned_pkg)
        self.assertIn("4-byte-aligned", str(ctx.exception))

        # Unknown transform rejected
        bad_transform_pkg = copy.deepcopy(valid_60hz_pkg)
        bad_transform_pkg["sites"][0]["transform"] = "accelerate_clock_x2"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(bad_transform_pkg)
        self.assertIn("Unknown site transform", str(ctx.exception))


def _write_synthetic_mips_elf(path: Path, words: dict[int, int], entry: int) -> bytes:
    """Write a minimal synthetic MIPS32 little-endian ET_EXEC ELF for testing."""
    filesz = max(max(words) + 8, 0x1000)
    payload_off = 52 + 32
    blob = bytearray(payload_off + filesz)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into(
        "<HHIIIIIHHHHHH",
        blob,
        16,
        2,  # e_type = ET_EXEC
        8,  # e_machine = EM_MIPS
        1,
        entry,
        52,
        0,
        0,
        52,
        32,
        1,
        0,
        0,
        0,
    )
    struct.pack_into("<8I", blob, 52, 1, payload_off, 0, 0, filesz, filesz, 5, 4)
    for addr, word in words.items():
        struct.pack_into("<I", blob, payload_off + addr, word)
    raw = bytes(blob)
    path.write_bytes(raw)
    return raw


class TestEnhancementAotCodegenAndCache(unittest.TestCase):
    """Verify authentic byte-identity, all 6 transform classes, fail-closed digest rejection, and AOT cache epoch."""

    def setUp(self) -> None:
        # Build a synthetic MIPS ELF with 6 functions exercising each of the 6 transform classes:
        #   0x00001000: synth_main_entry (calls the 6 helper functions)
        #   0x00001100: synth_vblank_wait     -> vblank_wait_2_to_1: addiu $v0, $zero, 2 (0x24020002)
        #   0x00001200: synth_timer_duration  -> count_x2:           addiu $a0, $zero, 30 (0x2404001e)
        #   0x00001300: synth_velocity_step   -> step_x0.5:          lui $a1, 0x3f80 (1.0f -> 0.5f 0x3f00)
        #   0x00001400: synth_damping_decay   -> decay_k_to_1-sqrt(1-k): lui $a2, 0x3f40 (0.75f -> sqrt(0.75)=0.8660254f 0x3f5db3d7)
        #   0x00001500: synth_rng_prob_check  -> probability_per_update_x0.5: slti $v1, $a0, 100 (0x28830064 -> 50)
        #   0x00001600: synth_subtick_gate    -> gate_30hz:          jal 0x00001200 (0x0c000480)
        self.entry = 0x00001000
        self.fn_addrs = {
            "synth_main_entry": 0x00001000,
            "synth_vblank_wait": 0x00001100,
            "synth_timer_duration": 0x00001200,
            "synth_velocity_step": 0x00001300,
            "synth_damping_decay": 0x00001400,
            "synth_rng_prob_check": 0x00001500,
            "synth_subtick_gate": 0x00001600,
        }
        jr_ra = 0x03E00008
        nop = 0x00000000

        def jal(target: int) -> int:
            return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)

        self.words: dict[int, int] = {
            # synth_main_entry
            0x00001000: jal(0x00001100),
            0x00001004: nop,
            0x00001008: jal(0x00001200),
            0x0000100C: nop,
            0x00001010: jal(0x00001300),
            0x00001014: nop,
            0x00001018: jal(0x00001400),
            0x0000101C: nop,
            0x00001020: jal(0x00001500),
            0x00001024: nop,
            0x00001028: jal(0x00001600),
            0x0000102C: nop,
            0x00001030: jr_ra,
            0x00001034: nop,
            # synth_vblank_wait: addiu $v0, $zero, 2
            0x00001100: 0x24020002,
            0x00001104: jr_ra,
            0x00001108: nop,
            # synth_timer_duration: addiu $a0, $zero, 30
            0x00001200: 0x2404001E,
            0x00001204: jr_ra,
            0x00001208: nop,
            # synth_velocity_step: lui $a1, 0x3f80 (1.0f)
            0x00001300: 0x3C053F80,
            0x00001304: jr_ra,
            0x00001308: nop,
            # synth_damping_decay: lui $a2, 0x3f40 (0.75f)
            0x00001400: 0x3C063F40,
            0x00001404: jr_ra,
            0x00001408: nop,
            # synth_rng_prob_check: slti $v1, $a0, 100
            0x00001500: 0x28830064,
            0x00001504: jr_ra,
            0x00001508: nop,
            # synth_subtick_gate: jal synth_timer_duration
            0x00001600: jal(0x00001200),
            0x00001604: nop,
            0x00001608: jr_ra,
            0x0000160C: nop,
        }

    def _run_codegen(self, elf_path: Path, out_c: Path, extra_args: list[str] | None = None) -> tuple[int, str, dict[str, bytes]]:
        argv = [
            "codegen.py",
            str(elf_path),
            str(out_c),
            "--base=0",
            "--profile=none",
            "--funcs-per-chunk=4000",
        ]
        if extra_args:
            argv.extend(extra_args)
        orig_out, orig_err = sys.stdout, sys.stderr
        try:
            sys.stdout, sys.stderr = StringIO(), StringIO()
            rc = codegen.main(argv)
            output = sys.stdout.getvalue() + sys.stderr.getvalue()
        finally:
            sys.stdout, sys.stderr = orig_out, orig_err
        files: dict[str, bytes] = {}
        if rc == 0:
            for p in sorted(out_c.parent.glob("out*")):
                if p.is_file():
                    files[p.name] = p.read_bytes()
        return rc, output, files

    def _make_package_and_symbols(self, elf_bytes: bytes) -> tuple[dict[str, Any], dict[str, str]]:
        elf_sha = hashlib.sha256(elf_bytes).hexdigest()
        symbols = {name: f"0x{addr:08x}" for name, addr in self.fn_addrs.items()}
        pkg: dict[str, Any] = {
            "schema_version": 1,
            "id": "synth.timing.60hz",
            "display_name": "Synthetic 60 Hz Logic Rescale",
            "version": "1.0.0",
            "category": "timing_frame_presentation",
            "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.00",
            "executable_sha256": elf_sha,
            "capabilities": [
                "vblank_wait_2_to_1",
                "count_x2",
                "step_x0_5",
                "decay_k_to_1_sqrt_1_k",
                "probability_per_update_x0_5",
                "gate_30hz",
            ],
            "rng_policy": {
                "mode": "rescale_probability",
                "reason": "Halve single-roll per-frame thresholds and gate stateful multi-roll routines at 30 Hz.",
            },
            "sites": [
                {
                    "id": "SYNTH_VBLANK",
                    "function": "synth_vblank_wait",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([self.words[0x00001100]]),
                    "transform": "vblank_wait_2_to_1",
                },
                {
                    "id": "SYNTH_COUNT",
                    "function": "synth_timer_duration",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([self.words[0x00001200]]),
                    "transform": "count_x2",
                },
                {
                    "id": "SYNTH_STEP",
                    "function": "synth_velocity_step",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([self.words[0x00001300]]),
                    "transform": "step_x0.5",
                },
                {
                    "id": "SYNTH_DECAY",
                    "function": "synth_damping_decay",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([self.words[0x00001400]]),
                    "transform": "decay_k_to_1-sqrt(1-k)",
                },
                {
                    "id": "SYNTH_PROB",
                    "function": "synth_rng_prob_check",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([self.words[0x00001500]]),
                    "transform": "probability_per_update_x0.5",
                },
                {
                    "id": "SYNTH_GATE",
                    "function": "synth_subtick_gate",
                    "insn_offset": 0,
                    "stock_digest": compute_stock_words_digest([self.words[0x00001600]]),
                    "transform": "gate_30hz",
                },
            ],
        }
        return pkg, symbols

    def test_authentic_mode_output_and_transforms_and_fail_closed_digest(self) -> None:
        """Verify authentic output stays untouched, all 6 transforms apply when enabled, and 1 wrong digest fails closed."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_path = tmp / "synth.elf"
            elf_bytes = _write_synthetic_mips_elf(elf_path, self.words, self.entry)

            # 1. Authentic baseline run (package off)
            auth_dir = tmp / "authentic"
            auth_dir.mkdir()
            rc_auth, out_auth, files_auth = self._run_codegen(elf_path, auth_dir / "out.c")
            self.assertEqual(rc_auth, 0, f"Authentic codegen failed: {out_auth}")
            self.assertTrue(files_auth)
            auth_combined = b"\n".join(files_auth.values()).decode("utf-8")
            self.assertNotIn("AOT Enhancement Epoch", auth_combined)
            self.assertIn("s->r[2] = (0u + 0x00000002u);", auth_combined)    # vblank wait 2
            self.assertIn("s->r[4] = (0u + 0x0000001eu);", auth_combined)    # count 30
            self.assertIn("s->r[5] = (0x3f80u << 16);", auth_combined)       # step 1.0f

            # 2. Enhanced run (package on)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)
            pkg_path = tmp / "synth-60hz.json"
            sym_path = tmp / "synth-60hz.symbols.json"
            pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
            sym_path.write_text(json.dumps(symbols), encoding="utf-8")

            enh_dir = tmp / "enhanced"
            enh_dir.mkdir()
            rc_enh, out_enh, files_enh = self._run_codegen(
                elf_path,
                enh_dir / "out.c",
                [f"--enhancement-package={pkg_path}"],
            )
            self.assertEqual(rc_enh, 0, f"Enhanced codegen failed: {out_enh}")
            enh_combined = b"\n".join(files_enh.values()).decode("utf-8")

            # Verify epoch stamp and all 6 transforms in generated C
            self.assertIn(f"AOT Enhancement Epoch: {AOT_ENHANCEMENT_PACKAGE_EPOCH}", enh_combined)
            self.assertIn("s->r[2] = (0u + 0x00000001u);", enh_combined)          # vblank_wait_2_to_1: 2 -> 1
            self.assertIn("s->r[4] = (0u + 0x0000003cu);", enh_combined)          # count_x2: 30 (0x1e) -> 60 (0x3c)
            self.assertIn("s->r[5] = (0x3f00u << 16);", enh_combined)             # step_x0.5: 1.0f (0x3f80) -> 0.5f (0x3f00)
            self.assertIn("s->r[6] = (0x3f00u << 16);", enh_combined)             # decay: k=0.75 -> 1-sqrt(1-0.75)=0.5f (0x3f00)
            self.assertIn("(int32_t)0x00000032u ? 1u : 0u", enh_combined)         # probability_per_update_x0.5: 100 (0x64) -> 50 (0x32)
            self.assertIn("if (!nk_enhancement_on() || nk_enhancement_gate_30hz())", enh_combined)  # gated call
            self.assertIn("int nk_enhancement_on(void) {", enh_combined)  # run-time opt-in, off by default
            self.assertIn("getenv(\"SR_EXPERIMENTAL_60HZ\")", enh_combined)
            self.assertNotIn("nk_enhancement", auth_combined)

            # 2b. A (transform, instruction) pair without a defined rewrite refuses the whole package
            nodef_pkg = copy.deepcopy(pkg)
            nodef_pkg["sites"][0] = dict(nodef_pkg["sites"][0], transform="gate_30hz")  # gate on addiu v0,zero,2
            nodef_path = tmp / "no-rewrite.json"
            nodef_path.write_text(json.dumps(nodef_pkg), encoding="utf-8")
            (tmp / "no-rewrite.symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
            nodef_dir = tmp / "no_rewrite"
            nodef_dir.mkdir()
            rc_nd, out_nd, files_nd = self._run_codegen(
                elf_path, nodef_dir / "out.c", [f"--enhancement-package={nodef_path}"]
            )
            self.assertNotEqual(rc_nd, 0)
            self.assertIn("has no defined rewrite", out_nd)
            self.assertEqual(files_nd, {})

            # 3. Re-running authentic mode after an enhanced run in the same process must stay byte-identical
            auth2_dir = tmp / "authentic2"
            auth2_dir.mkdir()
            rc_auth2, _, files_auth2 = self._run_codegen(elf_path, auth2_dir / "out.c")
            self.assertEqual(rc_auth2, 0)
            self.assertEqual(files_auth, files_auth2, "Authentic output changed after enhanced run")

            # 4. Fail-closed check: 1 wrong stock_digest must refuse the whole package
            bad_pkg = copy.deepcopy(pkg)
            bad_pkg["sites"][2]["stock_digest"] = compute_stock_words_digest([0x24020099])
            bad_pkg_path = tmp / "bad-digest.json"
            bad_sym_path = tmp / "bad-digest.symbols.json"
            bad_pkg_path.write_text(json.dumps(bad_pkg), encoding="utf-8")
            bad_sym_path.write_text(json.dumps(symbols), encoding="utf-8")

            bad_dir = tmp / "bad_run"
            bad_dir.mkdir()
            rc_bad, out_bad, files_bad = self._run_codegen(
                elf_path,
                bad_dir / "out.c",
                [f"--enhancement-package={bad_pkg_path}"],
            )
            self.assertNotEqual(rc_bad, 0, "Codegen must fail closed when a stock_digest mismatches")
            self.assertIn("Stock instruction digest mismatch", out_bad)
            self.assertEqual(files_bad, {}, "No partial enhanced output may be emitted on digest failure")

    def test_rewrite_planner_is_defined_or_refuses(self) -> None:
        """Each (transform, instruction) pair has one defined rewrite; anything else refuses."""
        from enhancement_package import EnhancementPackageError, plan_site_rewrite

        def plan(transform: str, word: int) -> dict:
            return plan_site_rewrite({"id": "S", "transform": transform}, word)

        self.assertEqual(plan("count_x2", 0x2404001E), {"kind": "word", "word": 0x2404003C})
        self.assertEqual(plan("step_x0.5", 0x3C053F80), {"kind": "word", "word": 0x3C053F00})
        self.assertEqual(plan("step_x0.5", 0x24840001), {"kind": "gate"})  # addiu a0,a0,1
        self.assertEqual(plan("count_x2", 0x2484FFFF), {"kind": "gate"})  # addiu a0,a0,-1
        self.assertEqual(plan("retention_m_to_sqrt(m)", 0x3C023F40)["word"], 0x3C023F5E)
        self.assertEqual(plan("gate_30hz", 0x0C000480), {"kind": "gate"})  # jal
        self.assertEqual(plan("count_x2", 0xC4430010), {"kind": "scale_f_reg", "reg": 3, "factor": 2.0})
        self.assertEqual(
            plan("step_x0.5", 0x46041080),  # add.s f2,f2,f4 accumulates: the addend is scaled
            {"kind": "scale_f_addend", "reg": 4, "factor": 0.5},
        )
        for transform, word in (
            ("step_x0.5", 0xAFA40008),  # sw
            ("count_x2", 0x3C040010),  # lui of a non-float value
            ("count_x2", 0x0C000480),  # jal
            ("count_x2", 0x2485FFFA),  # negative immediate
            ("gate_30hz", 0xC4820008),  # gate on a load
            ("step_x0.5", 0x4604103C),  # FPU compare
            ("step_x0.5", 0x34841234),  # ori low half
            ("count_x2", 0x44841000),  # mtc1
            ("vblank_wait_2_to_1", 0x24020003),
            ("decay_k_to_1-sqrt(1-k)", 0x3C023F80),  # 1.0 is outside (0, 1)
        ):
            with self.subTest(transform=transform, word=f"{word:08x}"):
                with self.assertRaises(EnhancementPackageError):
                    plan(transform, word)

    def test_aot_cache_epoch_separation_and_authentic_invariance(self) -> None:
        """With package off, AOT cache digest is untouched; with package on, it keys a distinct epoch."""
        base_opts = {
            "base": "0x08804000",
            "codegen_profile": "none",
            "entry": "0x08804000",
            "extra_modules": [],
            "extra_spans": [],
            "funcs_per_chunk": 500,
            "game_name": "synthetic",
            "psp_header_mode": "none",
            "static_verify": False,
        }
        # Authentic options with empty environment must add zero keys
        enh_off = package_cache.enhancement_package_options({})
        self.assertEqual(enh_off, {})

        with tempfile.TemporaryDirectory() as tmp_dir:
            pkg_path = Path(tmp_dir) / "synth-60hz.json"
            pkg, _ = self._make_package_and_symbols(b"synthetic_elf_bytes")
            pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
            enh_on = package_cache.enhancement_package_options({"ENHANCEMENT_PACKAGE": str(pkg_path)})
            self.assertEqual(enh_on["enhancement_package_epoch"], AOT_ENHANCEMENT_PACKAGE_EPOCH)
            self.assertEqual(enh_on["enhancement_package_id"], "synth.timing.60hz")
            self.assertEqual(len(enh_on["enhancement_package_sha256"]), 64)

            opts_authentic = dict(base_opts)
            opts_authentic.update(enh_off)
            opts_enhanced = dict(base_opts)
            opts_enhanced.update(enh_on)

            self.assertEqual(opts_authentic, base_opts)
            self.assertNotEqual(
                hashlib.sha256(package_cache.canonical_json(opts_authentic).encode("utf-8")).hexdigest(),
                hashlib.sha256(package_cache.canonical_json(opts_enhanced).encode("utf-8")).hexdigest(),
            )

    def test_a_site_that_is_never_emitted_refuses_the_package(self) -> None:
        """Every verified site must be emitted; a site whose function is stubbed refuses."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_path = tmp / "synth.elf"
            elf_bytes = _write_synthetic_mips_elf(elf_path, self.words, self.entry)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)
            pkg_path = tmp / "synth-60hz.json"
            pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
            (tmp / "synth-60hz.symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
            real_emit = codegen.emit_function

            def emit_or_stub(elf, start, *args, **kwargs):
                lines = real_emit(elf, start, *args, **kwargs)
                if start == self.fn_addrs["synth_velocity_step"]:
                    # The site was already rewritten when the function failed late.
                    raise codegen.Unsupported("synthetic late failure")
                return lines

            out_dir = tmp / "stubbed"
            out_dir.mkdir()
            with mock.patch.object(codegen, "emit_function", side_effect=emit_or_stub):
                rc, output, files = self._run_codegen(
                    elf_path, out_dir / "out.c", [f"--enhancement-package={pkg_path}"]
                )
            self.assertEqual(rc, 1, output)
            self.assertIn("SYNTH_STEP", output)
            self.assertIn("not emitted", output)
            self.assertEqual(files, {})

    def test_guest_symbol_map_names_functions_and_package_sites(self) -> None:
        """--symbol-map names guest functions and resolves the package's sites, over the ELF symbols."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_path = tmp / "synth.elf"
            elf_bytes = _write_synthetic_mips_elf(elf_path, self.words, self.entry)
            pkg, _ = self._make_package_and_symbols(elf_bytes)
            pkg_path = tmp / "synth-60hz.json"  # no synth-60hz.symbols.json sidecar
            pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
            package = f"--enhancement-package={pkg_path}"

            # The map wins over the ELF symbol table: pointing a site's function at another
            # function's entry refuses the package on its stock digest.
            wrong_map = tmp / "wrong-symbols.txt"
            wrong_map.write_text("synth_vblank_wait = 0x00001200; // type:func\n",
                                 encoding="utf-8")
            wrong_dir = tmp / "wrong"
            wrong_dir.mkdir()
            rc, output, files = self._run_codegen(
                elf_path, wrong_dir / "out.c", [package, f"--symbol-map={wrong_map}"])
            self.assertEqual(rc, 1, output)
            self.assertIn("Stock instruction digest mismatch", output)
            self.assertEqual(files, {})

            # A map that agrees with the ELF resolves the same sites and names the function.
            guest_map = tmp / "guest-symbols.txt"
            guest_map.write_text("synth_vblank_wait = 0x00001100; // type:func size:0xC\n",
                                 encoding="utf-8")
            for name, extra in (("elf-symbols", []), ("named", [f"--symbol-map={guest_map}"])):
                with self.subTest(name):
                    out_dir = tmp / name
                    out_dir.mkdir()
                    rc, output, files = self._run_codegen(
                        elf_path, out_dir / "out.c", [package, *extra])
                    self.assertEqual(rc, 0, output)
                    combined = b"\n".join(files.values()).decode("utf-8")
                    self.assertIn("sites=6", combined)
                    self.assertIn("s->r[2] = (0u + 0x00000001u);", combined)
                    self.assertEqual("f_00001100_synth_vblank_wait" in combined, bool(extra))

    def test_site_names_leave_out_a_name_given_to_two_addresses(self) -> None:
        """A name the guest map gives to two functions resolves no site at all."""
        names = codegen.site_names_from_symbol_map({
            0x1100: ("synth_vblank_wait", 0xC),
            0x1200: ("helper", 0),
            0x1300: ("helper", 0),
        })
        self.assertEqual(names, {"synth_vblank_wait": 0x1100})

    def test_lle_scaled_store_never_writes_the_fpr(self) -> None:
        """Under --lle-cpu a faulting swc1 returns early, so the rewrite must not touch s->f."""
        swc1 = 0xE4A40010  # swc1 $f4, 0x10($a1)
        saved = (codegen.ENHANCEMENT_SITES, codegen.LLE_CPU)
        try:
            codegen.ENHANCEMENT_SITES = {
                0x1300: {"rewrite": {"kind": "scale_f_store", "reg": 4, "factor": 0.5}}
            }
            codegen.ENHANCEMENT_APPLIED.clear()
            for lle in (False, True):
                with self.subTest(lle_cpu=lle):
                    line = codegen.normal_line(0x1300, swc1, lle_cpu=lle)
                    enhanced = line.split("else", 1)[0]
                    self.assertNotIn("s->f[4] =", enhanced)
                    self.assertNotIn("s->f[4] *=", enhanced)
                    self.assertNotIn("s->fi[4]", enhanced)
                    self.assertIn("s->f[4] * 0.5f", enhanced)
                    if lle:
                        self.assertIn("return;", enhanced)
        finally:
            codegen.ENHANCEMENT_SITES, codegen.LLE_CPU = saved
            codegen.ENHANCEMENT_APPLIED.clear()


class TestEnhancementBuildPackageWiring(unittest.TestCase):
    """White-box tests of the build-package refusals and the package directory choice."""

    DISC = "ULUS99999"
    TITLE = "synthetic-title"
    EXE_SHA = "ab" * 32

    def setUp(self) -> None:
        import nk_cli
        import title_codegen_plan

        self.nk_cli = nk_cli
        self.plan = title_codegen_plan
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.pkg = {
            "schema_version": 1,
            "id": "synth.timing.60hz",
            "display_name": "Synthetic 60 Hz Logic Rescale",
            "version": "1.0.0",
            "category": "timing_frame_presentation",
            "target_title_id": self.TITLE,
            "game_version": "1.00",
            "executable_sha256": self.EXE_SHA,
            "capabilities": ["count_x2"],
            "rng_policy": {"mode": "rescale_probability", "reason": "Synthetic."},
            "sites": [{
                "id": "SYNTH_COUNT",
                "function": "synth_timer_duration",
                "insn_offset": 0,
                "stock_digest": compute_stock_words_digest([0x2404001E]),
                "transform": "count_x2",
            }],
        }

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write_pkg(self, name: str, **changes: Any) -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(dict(self.pkg, **changes)), encoding="utf-8")
        return path

    def _select(self, package: Path | None = None, experimental_60hz: bool = False,
                environment: dict[str, str] | None = None):
        return self.nk_cli._select_enhancement_package(
            self.root,
            self.DISC,
            title_id=self.TITLE,
            executable_sha256=self.EXE_SHA,
            enhancement_package=package,
            experimental_60hz=experimental_60hz,
            environment={} if environment is None else environment,
        )

    def test_no_package_builds_the_authentic_directory(self) -> None:
        self.assertEqual(self._select(), (None, self.root / "packages" / self.DISC))

    def test_explicit_package_builds_its_own_directory(self) -> None:
        path = self._write_pkg("pkg.json")
        selected, target = self._select(path)
        self.assertEqual(selected, path.resolve())
        self.assertEqual(target, self.root / "packages" / f"{self.DISC}-enhanced-synth.timing.60hz")

    def test_target_title_mismatch_refuses(self) -> None:
        path = self._write_pkg("pkg.json", target_title_id="other-title")
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "target_title_id"):
            self._select(path)

    def test_executable_digest_mismatch_refuses(self) -> None:
        path = self._write_pkg("pkg.json", executable_sha256="cd" * 32)
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "executable_sha256"):
            self._select(path)

    def test_experimental_60hz_without_a_package_refuses(self) -> None:
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "requires an enhancement package"):
            self._select(experimental_60hz=True)

    def test_experimental_60hz_finds_the_lowercase_package(self) -> None:
        path = self._write_pkg(f"enhancements/{self.DISC.lower()}-60hz.json")
        selected, target = self._select(experimental_60hz=True)
        self.assertEqual(selected, path.resolve())
        self.assertEqual(target.name, f"{self.DISC}-enhanced-synth.timing.60hz")

    def test_ambient_package_never_enhances_a_build(self) -> None:
        path = self._write_pkg("pkg.json")
        for environment in (
            {"ENHANCEMENT_PACKAGE": str(path)},
            {"CODEGEN_USER_ARGS": f"--static-verify --enhancement-package={path}"},
            {"MAKEFLAGS": f"-j3 ENHANCEMENT_PACKAGE={path}"},
        ):
            for explicit in (None, path):
                with self.subTest(environment=environment, explicit=explicit):
                    with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "environment"):
                        self._select(explicit, environment=environment)
        # Empty values name no package.
        self.assertEqual(
            self._select(environment={"ENHANCEMENT_PACKAGE": " ", "CODEGEN_USER_ARGS": "--static-verify"}),
            (None, self.root / "packages" / self.DISC),
        )

    def test_planner_refuses_an_ambient_package(self) -> None:
        path = self._write_pkg("pkg.json")
        for environment in (
            {"ENHANCEMENT_PACKAGE": str(path)},
            {"CODEGEN_USER_ARGS": f"--enhancement-package={path}"},
        ):
            for explicit in (None, path):
                with self.subTest(environment=environment, explicit=explicit):
                    with mock.patch.dict(os.environ, environment):
                        with self.assertRaises(self.plan.PackageRouteError) as ctx:
                            self.plan.build_package(
                                {},
                                manifest_path=self.root / "manifest.json",
                                game_elf=self.root / "game.elf",
                                output_dir=self.root / "out",
                                enhancement_package_path=explicit,
                            )
                    self.assertEqual(ctx.exception.code, "PACKAGE_AMBIENT_ENHANCEMENT")

    def test_invalid_explicit_package_refuses_the_cache_key(self) -> None:
        path = self._write_pkg("pkg.json", sites=[])
        with self.assertRaises(package_cache.PackageCacheError):
            package_cache.enhancement_package_options({"ENHANCEMENT_PACKAGE": str(path)})


if __name__ == "__main__":
    unittest.main()
