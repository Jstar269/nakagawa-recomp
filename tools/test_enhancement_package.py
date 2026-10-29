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
import shutil
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
    UPDATE_BOUNDARY_KEYS,
    VALID_CATEGORIES,
    EnhancementPackageError,
    compute_stock_words_digest,
    find_data_file,
    load_package,
    plan_data_value,
    plan_float_pair_rewrite,
    plan_site_rewrite,
    transform_data_file,
    validate_package,
    verify_and_resolve_data_sites,
    verify_and_resolve_sites,
)
from nk_core import package_cache

SCHEMA_PATH = ROOT / "assets" / "enhancement_package.schema.json"
GCC = shutil.which("gcc") or shutil.which("cc")


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

        # The update boundary is one pinned instruction: function, offset, digest.
        boundary = schema["properties"]["update_boundary"]
        self.assertFalse(boundary.get("additionalProperties"))
        self.assertEqual(set(boundary["required"]), UPDATE_BOUNDARY_KEYS)
        self.assertEqual(set(boundary["properties"]), UPDATE_BOUNDARY_KEYS)
        self.assertEqual(boundary["properties"]["insn_offset"].get("multipleOf"), 4)

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

        # A package that gates must declare where each logic update begins.
        gated_pkg = copy.deepcopy(valid_60hz_pkg)
        gated_pkg["sites"].append(dict(gated_pkg["sites"][0], id="S002", insn_offset=8,
                                       transform="gate_30hz"))
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(copy.deepcopy(gated_pkg))
        self.assertIn("must declare update_boundary", str(ctx.exception))
        boundary = {
            "function": "synth_frame_body",
            "insn_offset": 0,
            "stock_digest": compute_stock_words_digest([0x27BDFFF0]),  # addiu sp, sp, -16
        }
        validate_package(dict(copy.deepcopy(gated_pkg), update_boundary=dict(boundary)))
        bad_boundaries = (
            ({**boundary, "insn_offset": 2}, "4-byte-aligned"),
            ({**boundary, "function": "FUN_00047920"}, "Address-literal"),
            ({**boundary, "stock_digest": "x"}, "SHA-256"),
            ({k: v for k, v in boundary.items() if k != "stock_digest"}, "Missing required property"),
            ({**boundary, "word_count": 1}, "Unknown properties"),
            ({"function": "synth_vblank_sync_loop", "insn_offset": 0,
              "stock_digest": boundary["stock_digest"]}, "must not be a rewritten site"),
            ("synth_frame_body", "must be an object"),
        )
        for bad, why in bad_boundaries:
            with self.subTest(boundary=bad):
                with self.assertRaises(EnhancementPackageError) as ctx:
                    validate_package(dict(copy.deepcopy(valid_60hz_pkg), update_boundary=bad))
                self.assertIn(why, str(ctx.exception))
        # Only a timing package may name a boundary.
        presentation_pkg = {k: v for k, v in valid_60hz_pkg.items()
                            if k not in ("game_version", "executable_sha256", "rng_policy", "sites")}
        presentation_pkg = dict(presentation_pkg, category="presentation_only", update_boundary=boundary)
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(presentation_pkg)
        self.assertIn("only valid for category 'timing_frame_presentation'", str(ctx.exception))

        # Unknown transform rejected
        bad_transform_pkg = copy.deepcopy(valid_60hz_pkg)
        bad_transform_pkg["sites"][0]["transform"] = "accelerate_clock_x2"
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(bad_transform_pkg)
        self.assertIn("Unknown site transform", str(ctx.exception))


def _write_synthetic_mips_elf(path: Path, words: dict[int, int], entry: int,
                              sections: tuple[tuple[str, int, int, int], ...] = ()) -> bytes:
    """Write a minimal synthetic MIPS32 little-endian ET_EXEC ELF for testing.

    ``sections`` adds a section header table of (name, addr, size, flags) PROGBITS
    sections over the loaded image.
    """
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
    if sections:
        names = b"\0.shstrtab\0" + b"".join(name.encode() + b"\0" for name, *_ in sections)
        strtab_off = len(blob)
        blob += names
        while len(blob) % 4:
            blob.append(0)
        shoff = len(blob)
        headers = [bytes(40), struct.pack("<10I", 1, 3, 0, 0, strtab_off, len(names), 0, 0, 1, 0)]
        name_at = len(b"\0.shstrtab\0")
        for name, addr, size, flags in sections:
            headers.append(struct.pack("<10I", name_at, 1, flags, addr, payload_off + addr, size, 0, 0, 4, 0))
            name_at += len(name) + 1
        blob += b"".join(headers)
        struct.pack_into("<I", blob, 32, shoff)
        struct.pack_into("<HHH", blob, 46, 40, len(headers), 1)
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
            # One update runs synth_vblank_wait once; its return's delay slot counts it.
            "update_boundary": {
                "function": "synth_vblank_wait",
                "insn_offset": 8,
                "stock_digest": compute_stock_words_digest([self.words[0x00001108]]),
            },
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

    def _codegen_with(self, tmp: Path, name: str, pkg: dict[str, Any], symbols: dict[str, str]
                      ) -> tuple[int, str, dict[str, bytes]]:
        elf_path = tmp / "synth.elf"
        pkg_path = tmp / f"{name}.json"
        pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
        (tmp / f"{name}.symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
        out_dir = tmp / name
        out_dir.mkdir()
        return self._run_codegen(elf_path, out_dir / "out.c", [f"--enhancement-package={pkg_path}"])

    def test_gated_sites_count_logic_updates_at_the_update_boundary(self) -> None:
        """The boundary statement counts each update once; gates read that count, not the display."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", self.words, self.entry)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)
            rc, output, files = self._codegen_with(tmp, "boundary", pkg, symbols)
            self.assertEqual(rc, 0, output)
            combined = b"\n".join(files.values()).decode("utf-8")
            self.assertEqual(combined.count("nk_enhancement_updates++"), 1)
            self.assertIn("uint32_t nk_enhancement_updates;", combined)
            self.assertIn("(nk_enhancement_updates & 1u) == 0u", combined)
            self.assertNotIn("sr_display_vcount", combined)
            # The count runs before the boundary's own stock statement.
            count_at = combined.index("nk_enhancement_updates++")
            self.assertLess(count_at, combined.index("sr_begin(s, 0x00001108u", count_at))

    def test_update_boundary_refusals(self) -> None:
        """A gate without a boundary, or a boundary that does not verify, refuses the package."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", self.words, self.entry)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)

            # A unit step under step_x0.5 plans a gate, so it needs a boundary too.
            words = dict(self.words)
            words[0x00001300] = 0x24A50001  # addiu a1, a1, 1
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", words, self.entry)
            step_pkg = copy.deepcopy(pkg)
            step_pkg["executable_sha256"] = hashlib.sha256(elf_bytes).hexdigest()
            step_pkg["sites"] = [dict(step_pkg["sites"][2],
                                      stock_digest=compute_stock_words_digest([0x24A50001]))]
            del step_pkg["update_boundary"]
            rc, output, files = self._codegen_with(tmp, "step-no-boundary", step_pkg, symbols)
            self.assertEqual(rc, 1, output)
            self.assertIn("declares no update_boundary", output)
            self.assertEqual(files, {})
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", self.words, self.entry)

            cases = (
                ("boundary-digest", {"stock_digest": compute_stock_words_digest([0x24020099])},
                 "Stock instruction digest mismatch"),
                ("boundary-control", {"insn_offset": 4,
                                      "stock_digest": compute_stock_words_digest([0x03E00008])},
                 "control transfer cannot begin a logic update"),
            )
            for name, change, why in cases:
                with self.subTest(name):
                    bad = copy.deepcopy(pkg)
                    bad["update_boundary"].update(change)
                    rc, output, files = self._codegen_with(tmp, name, bad, symbols)
                    self.assertEqual(rc, 1, output)
                    self.assertIn(why, output)
                    self.assertEqual(files, {})

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_gates_alternate_on_updates_when_frames_drop(self) -> None:
        """Compile the generated gate and boundary count and run updates of 1 to 3 vblanks.

        A gated statement must run on every other logic update however many display
        vblanks each update takes (a frame drop, or a stock two-vblank wait).
        """
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", self.words, self.entry)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)
            rc, output, files = self._codegen_with(tmp, "drops", pkg, symbols)
            self.assertEqual(rc, 0, output)
            header = files["out_funcs.h"].decode("utf-8")
            gate_def = next(line for line in header.splitlines()
                            if "int nk_enhancement_gate_30hz(void)" in line)
            combined = b"\n".join(files.values()).decode("utf-8")
            count_stmt = next(line.strip() for line in combined.splitlines()
                              if "nk_enhancement_updates++" in line)
            harness = tmp / "drops.c"
            harness.write_text(
                "#include <stdint.h>\n#include <stdio.h>\n"
                "static uint32_t vcount;\n"
                "uint32_t sr_display_vcount(void) { return vcount; }\n"
                "int nk_enhancement_on(void) { return 1; }\n"
                "uint32_t nk_enhancement_updates;\n"
                f"{gate_def}\n"
                "int main(void) {\n"
                "    static const unsigned vblanks[] = {1, 1, 2, 2, 2, 1, 3, 2, 1, 1, 2, 2};\n"
                "    unsigned runs = 0, alternations = 0, last = 2;\n"
                "    for (unsigned u = 0; u < 1200; u++) {\n"
                f"        {count_stmt}\n"
                "        unsigned ran = 0;\n"
                "        if (nk_enhancement_gate_30hz()) { ran = 1; runs++; }\n"
                "        if (ran != last) alternations++;\n"
                "        last = ran;\n"
                "        vcount += vblanks[u % 12];\n"
                "    }\n"
                "    printf(\"%u %u\\n\", runs, alternations);\n"
                "    return 0;\n"
                "}\n",
                encoding="utf-8",
            )
            exe = tmp / "drops.exe"
            subprocess.run([GCC, "-std=c11", "-Wall", "-Werror", "-Wno-unused-function",
                            "-o", str(exe), str(harness)], check=True, capture_output=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
            runs, alternations = (int(v) for v in out.split())
            self.assertEqual(runs, 600)
            self.assertEqual(alternations, 1200)

    def test_data_sites_and_files_reach_the_generated_start_up(self) -> None:
        """Data sites become a verified load-time patch; data files a verified registration."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            words = dict(self.words)
            words[0x00001800] = 21           # s32 count in .data
            words[0x00001804] = 0x3F800000   # f32 1.0 in .data
            sections = (
                (".text", 0x00001000, 0x700, 6),  # alloc + exec
                (".data", 0x00001800, 0x10, 3),   # alloc + write
            )
            elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", words, self.entry, sections)
            pkg, symbols = self._make_package_and_symbols(elf_bytes)
            symbols["synth_wait_table"] = "0x00001800"
            symbols["synth_code_as_data"] = "0x00001200"
            pkg["data_sites"] = [
                {"id": "D_COUNT", "symbol": "synth_wait_table", "offset": 0, "type": "s32",
                 "stock_digest": hashlib.sha256(struct.pack("<i", 21)).hexdigest(),
                 "transform": "count_x2"},
                {"id": "D_STEP", "symbol": "synth_wait_table", "offset": 4, "type": "f32",
                 "stock_digest": hashlib.sha256(struct.pack("<f", 1.0)).hexdigest(),
                 "transform": "step_x0.5"},
            ]
            csv = b"Name,Wait,Speed\r\nlabel,frames,rate\r\na,30,0.25\r\nb,12,1.5\r\n"
            data_root = tmp / "game" / "xbdata" / "pack.xb.d" / "data" / "param"
            data_root.mkdir(parents=True)
            (data_root / "Timing.csv").write_bytes(csv)
            pkg["data_files"] = [{
                "id": "F_TIMING", "path": "data/param/timing.csv",
                "sha256": hashlib.sha256(csv).hexdigest(), "format": "csv",
                "header_row": 0, "first_data_row": 2,
                "cells": [{"column": "Wait", "transform": "count_x2"},
                          {"column": "Speed", "transform": "step_x0.5"}],
            }]
            elf_path = tmp / "synth.elf"
            pkg_path = tmp / "data.json"
            pkg_path.write_text(json.dumps(pkg), encoding="utf-8")
            (tmp / "data.symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
            out_dir = tmp / "data"
            out_dir.mkdir()
            rc, output, files = self._run_codegen(
                elf_path, out_dir / "out.c",
                [f"--enhancement-package={pkg_path}", f"--enhancement-data-root={tmp / 'game'}"])
            self.assertEqual(rc, 0, output)
            combined = b"\n".join(files.values()).decode("utf-8")
            self.assertIn("if (nk_enhancement_on()) nk_enhancement_apply_data();", combined)
            self.assertIn("sr_enhancement_register_updates(&nk_enhancement_updates);", combined)
            self.assertIn("nk_enh_data_new_0[] = {0x2a, 0x00, 0x00, 0x00}", combined)  # 21 -> 42
            self.assertIn("nk_enh_data_new_1[] = {0x00, 0x00, 0x00, 0x3f}", combined)  # 1.0 -> 0.5
            self.assertIn("ENHANCEMENT_DATA_MISMATCH: data site D_COUNT", combined)
            rewritten = b"Name,Wait,Speed\r\nlabel,frames,rate\r\na,60,0.125\r\nb,24,0.75\r\n"
            self.assertIn(f"nk_enh_file_data_0[] = {{{', '.join(f'0x{b:02x}' for b in rewritten)}}}",
                          combined)
            self.assertIn('sr_enhancement_file_register("data/param/timing.csv", nk_enh_file_sha_0, '
                          f"nk_enh_file_data_0, {len(rewritten)}u)", combined)

            refusals = (
                ("no-data-root", None, [], "need --enhancement-data-root"),
                ("code-as-data", {"symbol": "synth_code_as_data"}, None, "initialised data"),
                ("data-digest", {"stock_digest": "0" * 64}, None, "Stock data digest mismatch"),
            )
            for name, change, extra, why in refusals:
                with self.subTest(name):
                    bad = copy.deepcopy(pkg)
                    if change:
                        bad["data_sites"][0].update(change)
                    bad_path = tmp / f"{name}.json"
                    bad_path.write_text(json.dumps(bad), encoding="utf-8")
                    (tmp / f"{name}.symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
                    bad_dir = tmp / name
                    bad_dir.mkdir()
                    args = [f"--enhancement-package={bad_path}"]
                    args += extra if extra is not None else [f"--enhancement-data-root={tmp / 'game'}"]
                    rc, output, files = self._run_codegen(elf_path, bad_dir / "out.c", args)
                    self.assertEqual(rc, 1, output)
                    self.assertIn(why, output)
                    self.assertEqual(files, {})

    def test_float_pair_rewrite_scales_the_whole_constant(self) -> None:
        """lui+ori of one register names a float exactly; anything else refuses."""
        site = {"id": "PAIR", "transform": "decay_k_to_1-sqrt(1-k)"}
        lui, ori = 0x3C023F4C, 0x3442CCCD  # lui v0,0x3f4c; ori v0,v0,0xcccd = 0.8f
        hi, lo = plan_float_pair_rewrite(site, lui, ori)
        bits = ((hi["word"] & 0xFFFF) << 16) | (lo["word"] & 0xFFFF)
        expected = struct.unpack("<I", struct.pack("<f", 1.0 - (1.0 - struct.unpack(
            "<f", struct.pack("<I", 0x3F4CCCCD))[0]) ** 0.5))[0]
        self.assertEqual(bits, expected)
        self.assertEqual(hi["word"] >> 16, lui >> 16)
        self.assertEqual(lo["word"] >> 16, ori >> 16)
        step = {"id": "PAIR", "transform": "step_x0.5"}
        hi, lo = plan_float_pair_rewrite(step, lui, ori)
        self.assertEqual(((hi["word"] & 0xFFFF) << 16) | (lo["word"] & 0xFFFF), 0x3ECCCCCD)  # 0.4f
        for w0, w1, why in (
            (lui, 0x3463CCCD, "ori of the lui's register"),   # ori v1,v1 after lui v0
            (0x24020002, ori, "starts with lui"),
            (lui, 0x2442CCCD, "ori of the lui's register"),   # addiu, not ori
        ):
            with self.subTest(w0=hex(w0), w1=hex(w1)):
                with self.assertRaises(EnhancementPackageError) as ctx:
                    plan_float_pair_rewrite(step, w0, w1)
                self.assertIn(why, str(ctx.exception))
        with self.assertRaises(EnhancementPackageError):
            plan_float_pair_rewrite({"id": "PAIR", "transform": "gate_30hz"}, lui, ori)

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


class TestEnhancementDataTransforms(unittest.TestCase):
    """Data values and CSV cells rescale exactly or refuse."""

    def test_data_values(self) -> None:
        self.assertEqual(plan_data_value("count_x2", "s32", struct.pack("<i", -3)), struct.pack("<i", -6))
        self.assertEqual(plan_data_value("step_x0.5", "u16", struct.pack("<H", 40)), struct.pack("<H", 20))
        self.assertEqual(plan_data_value("retention_m_to_sqrt(m)", "f32", struct.pack("<f", 0.25)),
                         struct.pack("<f", 0.5))
        for transform, dtype, raw in (
            ("step_x0.5", "s32", struct.pack("<i", 7)),          # odd count
            ("count_x2", "u8", struct.pack("<B", 200)),          # overflow
            ("decay_k_to_1-sqrt(1-k)", "s32", struct.pack("<i", 1)),
            ("decay_k_to_1-sqrt(1-k)", "f32", struct.pack("<f", 1.5)),
        ):
            with self.subTest(transform=transform, dtype=dtype), self.assertRaises(ValueError):
                plan_data_value(transform, dtype, raw)

    def test_data_sites_resolve_fail_closed(self) -> None:
        stock = struct.pack("<2i", 15, 45)
        pkg = {
            "schema_version": 1, "id": "synth.data", "display_name": "Synthetic data", "version": "1.0.0",
            "category": "timing_frame_presentation", "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.00", "executable_sha256": "a" * 64, "capabilities": ["count_x2"],
            "rng_policy": {"mode": "subtick_30hz", "reason": "Every roll stays on alternate updates."},
            "sites": [{"id": "S1", "function": "f", "insn_offset": 0,
                       "stock_digest": "b" * 64, "transform": "count_x2"}],
            "data_sites": [{"id": "D1", "symbol": "table", "offset": 4, "type": "s32", "count": 2,
                            "stock_digest": hashlib.sha256(stock).hexdigest(), "transform": "count_x2"}],
        }
        image = {0x104: stock}
        resolved = verify_and_resolve_data_sites(
            pkg, symbol_to_addr={"table": 0x100}, read_data_fn=lambda a, n: image.get(a))
        self.assertEqual(resolved, [{"id": "D1", "addr": 0x104, "stock": stock,
                                     "data": struct.pack("<2i", 30, 90)}])
        with self.assertRaisesRegex(EnhancementPackageError, "Cannot resolve data symbol"):
            verify_and_resolve_data_sites(pkg, symbol_to_addr={}, read_data_fn=lambda a, n: image.get(a))
        with self.assertRaisesRegex(EnhancementPackageError, "initialised data"):
            verify_and_resolve_data_sites(pkg, symbol_to_addr={"table": 0x100}, read_data_fn=lambda a, n: None)
        overlap = copy.deepcopy(pkg)
        overlap["data_sites"].append(dict(overlap["data_sites"][0], id="D2", offset=8))
        with self.assertRaisesRegex(EnhancementPackageError, "overlaps"):
            validate_package(overlap)
        misaligned = copy.deepcopy(pkg)
        misaligned["data_sites"][0]["offset"] = 2
        with self.assertRaisesRegex(EnhancementPackageError, "not aligned"):
            validate_package(misaligned)
        gate = copy.deepcopy(pkg)
        gate["data_sites"][0]["transform"] = "gate_30hz"
        with self.assertRaisesRegex(EnhancementPackageError, "has no data rescale"):
            validate_package(gate)

    def _csv_entry(self, csv: bytes, cells: list[dict[str, Any]]) -> dict[str, Any]:
        return {"id": "F1", "path": "data/p/t.csv", "sha256": hashlib.sha256(csv).hexdigest(),
                "format": "csv", "header_row": 0, "first_data_row": 1, "cells": cells}

    def test_csv_cells(self) -> None:
        csv = b"Key,A,B\nx,3,0.1\ny,10,2.50\n\n"
        out = transform_data_file(self._csv_entry(csv, [
            {"column": "A", "transform": "count_x2", "rows": [1]},
            {"column": "B", "transform": "decay_k_to_1-sqrt(1-k)", "rows": [0]},
        ]), csv)
        lines = out.split(b"\n")
        self.assertEqual(lines[1].split(b",")[1], b"3")            # row 0 left alone
        self.assertEqual(lines[2].split(b",")[1], b"20")
        decayed = struct.unpack("<f", struct.pack("<f", 1.0 - (1.0 - struct.unpack(
            "<f", struct.pack("<f", 0.1))[0]) ** 0.5))[0]
        cell = float(lines[1].split(b",")[2])
        self.assertEqual(struct.unpack("<f", struct.pack("<f", cell))[0], decayed)
        self.assertEqual(lines[2].split(b",")[2], b"2.50")          # row 1 left alone
        self.assertEqual(lines[3:], [b"", b""])                     # trailing lines kept
        for cells, why in (
            ([{"column": "C", "transform": "count_x2"}], "not exactly one header"),
            ([{"column": "Key", "transform": "count_x2"}], "not a plain decimal"),
            ([{"column": "A", "transform": "step_x0.5"}], "no exact rescale"),
            ([{"column": "A", "transform": "count_x2", "rows": [5]}], "past the last data row"),
        ):
            with self.subTest(cells=cells), self.assertRaises(EnhancementPackageError) as ctx:
                transform_data_file(self._csv_entry(csv, cells), csv)
            self.assertIn(why, str(ctx.exception))
        with self.assertRaisesRegex(EnhancementPackageError, "does not match its sha256"):
            transform_data_file(self._csv_entry(csv, [{"column": "A", "transform": "count_x2"}]), csv + b"x")

    def test_find_data_file_matches_path_without_case_and_digest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            root = Path(tmp_dir)
            for i, body in enumerate((b"old", b"new")):
                folder = root / f"a{i}.xb.d" / "Data" / "P"
                folder.mkdir(parents=True)
                (folder / "T.CSV").write_bytes(body)
            entry = self._csv_entry(b"new", [{"column": "A", "transform": "count_x2"}])
            self.assertEqual(find_data_file(entry, root), b"new")
            entry["sha256"] = hashlib.sha256(b"other").hexdigest()
            with self.assertRaisesRegex(EnhancementPackageError, "No file data/p/t.csv"):
                find_data_file(entry, root)
            entry["path"] = "p/t.csv/../t.csv"
            with self.assertRaisesRegex(EnhancementPackageError, "relative disc path"):
                validate_package({
                    "schema_version": 1, "id": "synth.data", "display_name": "Synthetic data",
                    "version": "1.0.0", "category": "timing_frame_presentation",
                    "target_title_id": "synthetic-allegrex-v1", "game_version": "1.00",
                    "executable_sha256": "a" * 64, "capabilities": ["count_x2"],
                    "rng_policy": {"mode": "subtick_30hz", "reason": "Every roll stays on alternate updates."},
                    "sites": [{"id": "S1", "function": "f", "insn_offset": 0,
                               "stock_digest": "b" * 64, "transform": "count_x2"}],
                    "data_files": [entry],
                })


def _f32_of(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def _bits_of(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


class TestEnhancementPhysicsAndOffsetTransforms(unittest.TestCase):
    """offset_x2, window_start_x2, accel_x0.25, launch_velocity_x0.5 and f64_high."""

    def _plan(self, transform: str, word: int, **extra: Any) -> dict[str, Any]:
        return plan_site_rewrite(dict({"id": "S", "transform": transform}, **extra), word)

    def test_offsets_and_windows(self) -> None:
        # addiu v0,v0,2 (a lookahead D + 2): offset_x2 doubles the term even though rt == rs.
        self.assertEqual(self._plan("offset_x2", 0x24420002)["word"], 0x24420004)
        self.assertEqual(self._plan("offset_x2", 0x2442FFFD)["word"], 0x2442FFFA)  # -3 -> -6
        # addiu v1,a1,-4 (window head-4 .. head-1, 3 updates) -> -7 (6 updates).
        self.assertEqual(self._plan("window_start_x2", 0x24A3FFFC)["word"], 0x24A3FFF9)
        for transform, word, why in (
            ("offset_x2", 0x24420000, "nonzero"),
            ("offset_x2", 0x28420002, "addi/addiu"),          # slti
            ("window_start_x2", 0x24A3FFFF, "-2 or less"),     # -1 has no span
            ("window_start_x2", 0x24A30004, "-2 or less"),
        ):
            with self.subTest(transform=transform, word=hex(word)):
                with self.assertRaises(EnhancementPackageError) as ctx:
                    self._plan(transform, word)
                self.assertIn(why, str(ctx.exception))

    def test_accel_and_double_upper_words(self) -> None:
        # lui v0,0x3d05 (the upper half of 0.0326667): two exponent steps are exact.
        word = self._plan("accel_x0.25", 0x3C023D05)["word"]
        self.assertEqual(_f32_of((word & 0xFFFF) << 16), _f32_of(0x3D050000) / 4)
        with self.assertRaises(EnhancementPackageError):
            self._plan("accel_x0.25", 0x24020002)
        # lui a0,0x4011 is the upper word of the double 4.25: halved is 2.125 (0x4001...).
        hi = self._plan("step_x0.5", 0x3C044011, operand="f64_high")["word"] & 0xFFFF
        self.assertEqual(hi, 0x4001)
        hi = self._plan("count_x2", 0x3C04C011, operand="f64_high")["word"] & 0xFFFF  # -4.25 -> -8.5
        self.assertEqual(hi, 0xC021)
        self.assertEqual(struct.unpack(">d", bytes.fromhex("4021000000000000"))[0], 8.5)
        with self.assertRaises(EnhancementPackageError):
            self._plan("decay_k_to_1-sqrt(1-k)", 0x3C043FE0, operand="f64_high")

    def test_launch_velocity_matches_the_30hz_arc_every_second_update(self) -> None:
        """v0' = v0/2 + g/8 with g' = g/4 puts a semi-implicit Euler arc on the same points."""
        v0, g = _f32_of(0xBE99999A), _f32_of(0x3D05CD7C)   # -0.3 and 0.032666668
        words = {
            0x1000: 0x3C02BE99, 0x1004: 0x3442999A,       # lui/ori v0 = v0
            0x1100: 0x3C023D05, 0x1140: 0x3442CD7C,       # lui ... ori (not adjacent) = g
        }
        pkg = {
            "schema_version": 1, "id": "synth.jump", "display_name": "Synthetic jump", "version": "1.0.0",
            "category": "timing_frame_presentation", "target_title_id": "synthetic-allegrex-v1",
            "game_version": "1.00", "executable_sha256": "a" * 64, "capabilities": ["count_x2"],
            "rng_policy": {"mode": "subtick_30hz", "reason": "No random rolls in this fixture."},
            "sites": [
                {"id": "V0", "function": "jump", "insn_offset": 0, "word_count": 2,
                 "stock_digest": compute_stock_words_digest([words[0x1000], words[0x1004]]),
                 "transform": "launch_velocity_x0.5", "acceleration_site": "G"},
                {"id": "G", "function": "jump", "insn_offset": 0x100,
                 "stock_digest": compute_stock_words_digest([words[0x1100]]),
                 "low_insn_offset": 0x140, "low_stock_digest": compute_stock_words_digest([words[0x1140]]),
                 "transform": "accel_x0.25"},
            ],
        }
        resolved = verify_and_resolve_sites(
            pkg, executable_sha256="a" * 64, symbol_to_addr={"jump": 0x1000},
            read_words_fn=lambda a, n: [words[a + 4 * i] for i in range(n)] if a in words else None)
        new_v0 = _f32_of(((resolved[0x1000]["rewrite"]["word"] & 0xFFFF) << 16)
                         | (resolved[0x1004]["rewrite"]["word"] & 0xFFFF))
        new_g = _f32_of((resolved[0x1100]["rewrite"]["word"] & 0xFFFF) << 16 | 0xCD7C)
        self.assertEqual(new_v0, _f32_of(_bits_of(v0 / 2 + g / 8)))
        self.assertNotIn(0x1140, resolved)   # the pinned low word is read, never rewritten

        def arc(v: float, a: float, steps: int) -> list[float]:
            y, out = 0.0, []
            for _ in range(steps):
                v += a
                y += v
                out.append(y)
            return out
        stock, enhanced = arc(v0, g, 20), arc(new_v0, new_g, 40)
        for n in range(20):
            self.assertAlmostEqual(stock[n], enhanced[2 * n + 1], places=5)

        no_pin = copy.deepcopy(pkg)
        del no_pin["sites"][1]["low_insn_offset"], no_pin["sites"][1]["low_stock_digest"]
        with self.assertRaisesRegex(EnhancementPackageError, "does not pin its whole float"):
            verify_and_resolve_sites(
                no_pin, executable_sha256="a" * 64, symbol_to_addr={"jump": 0x1000},
                read_words_fn=lambda a, n: [words[a + 4 * i] for i in range(n)] if a in words else None)
        wrong = copy.deepcopy(pkg)
        wrong["sites"][1]["transform"] = "step_x0.5"
        with self.assertRaisesRegex(EnhancementPackageError, "must be an accel_x0.25 site"):
            validate_package(wrong)
        missing = copy.deepcopy(pkg)
        del missing["sites"][0]["acceleration_site"]
        with self.assertRaisesRegex(EnhancementPackageError, "needs acceleration_site"):
            validate_package(missing)


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
