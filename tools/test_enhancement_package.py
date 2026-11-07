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
import decimal
import hashlib
import itertools
from io import StringIO
import json
import os
from pathlib import Path
import random
import re
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
import enhancement_package
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
    _gpr_written,
    _scale_value,
    _validate_site_extras,
    compute_stock_words_digest,
    draft_package_declaration,
    draft_timing_package,
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


def _f32_decay_reference(k: float) -> int:
    """The f32 bits nearest 1 - sqrt(1 - k), evaluated to 60 decimal digits."""
    with decimal.localcontext() as ctx:
        ctx.prec = 60
        exact = 1 - (1 - decimal.Decimal(k)).sqrt()
    return _bits_of(float(exact))



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

    @staticmethod
    def _run_codegen(elf_path: Path, out_c: Path, extra_args: list[str] | None = None) -> tuple[int, str, dict[str, bytes]]:
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
            assert GCC is not None
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
            if GCC:
                # The start-up block is C the runtime build compiles; check it is valid C.
                checked = subprocess.run(
                    [GCC, "-std=gnu11", "-fsyntax-only", "-Wall", "-Werror", "-Wno-unused-function",
                     f"-I{ROOT / 'src' / 'rt'}", f"-I{out_dir}", str(out_dir / "out.c")],
                    capture_output=True, text=True)
                self.assertEqual(checked.returncode, 0, checked.stderr[-2000:])

            refusals: tuple[tuple[str, dict[str, str] | None, list[str] | None, str], ...] = (
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

        def plan(transform: str, word: int) -> dict[str, Any]:
            planned: dict[str, Any] = plan_site_rewrite({"id": "S", "transform": transform}, word)
            return planned

        self.assertEqual(plan("count_x2", 0x2404001E), {"kind": "word", "word": 0x2404003C})
        self.assertEqual(plan("step_x0.5", 0x3C053F80), {"kind": "word", "word": 0x3C053F00})
        self.assertEqual(plan("step_x0.5", 0x24840001), {"kind": "gate"})  # addiu a0,a0,1
        self.assertEqual(plan("count_x2", 0x2484FFFF), {"kind": "gate"})  # addiu a0,a0,-1
        self.assertEqual(plan("retention_m_to_sqrt(m)", 0x3C023F40)["word"], 0x3C023F5E)
        # k = 2^-126: 1 - sqrt(1 - k) cancels to 0 in double; the result is k / 2 = 2^-127
        self.assertEqual(plan("decay_k_to_1-sqrt(1-k)", 0x3C020080)["word"], 0x3C020040)
        self.assertEqual(plan("decay_k_to_1-sqrt(1-k)", 0xC4430010),  # lwc1 f3: map the loaded factor
                         {"kind": "map_f_reg", "reg": 3, "map": "decay"})
        self.assertEqual(plan("retention_m_to_sqrt(m)", 0xC4430010),
                         {"kind": "map_f_reg", "reg": 3, "map": "retention"})
        self.assertEqual(plan("count_x2", 0xC8404F84),  # lv.s S000.s, 0x4f84(v0)
                         {"kind": "scale_v_reg", "vreg": 0, "factor": 2.0})
        self.assertEqual(plan("step_x0.5", 0xC8470011),  # lv.s S131.s (vt 7 | 1 << 5)
                         {"kind": "scale_v_reg", "vreg": 39, "factor": 0.5})
        self.assertEqual(plan("decay_k_to_1-sqrt(1-k)", 0xC844C410),  # lv.s S100.s, -0x3bf0(v0)
                         {"kind": "map_v_reg", "vreg": 4, "map": "decay"})
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
            ("decay_k_to_1-sqrt(1-k)", 0xE4430010),  # swc1 stores, it does not load the factor
            ("retention_m_to_sqrt(m)", 0x24020003),
        ):
            with self.subTest(transform=transform, word=f"{word:08x}"):
                with self.assertRaises(EnhancementPackageError):
                    plan(transform, word)

    def test_count_x2_even_and_gate_30hz_hold_plans(self) -> None:
        """count_x2_even doubles a slti/sltiu bound and keeps even counts; gate_30hz_hold holds f12."""
        from enhancement_package import EnhancementPackageError, plan_site_rewrite

        def plan(transform: str, word: int) -> dict:
            rewrite: dict = plan_site_rewrite({"id": "S", "transform": transform}, word)
            return rewrite

        self.assertEqual(plan("count_x2_even", 0x28410008),  # slti at, v0, 8
                         {"kind": "word_even", "word": 0x28410010, "reg": 2, "rt": 1})
        self.assertEqual(plan("count_x2_even", 0x2C410008),  # sltiu at, v0, 8
                         {"kind": "word_even", "word": 0x2C410010, "reg": 2, "rt": 1})
        self.assertEqual(plan("count_x2_even", 0x28413FFF),  # the largest bound that doubles in 16 bits
                         {"kind": "word_even", "word": 0x28417FFE, "reg": 2, "rt": 1})
        self.assertEqual(plan("gate_30hz_hold", 0x0C000480), {"kind": "gate", "hold_f": 12})  # jal
        for transform, word in (
            ("count_x2_even", 0x28420008),  # slti v0, v0, 8: the counter is overwritten
            ("count_x2_even", 0x28400008),  # slti zero, v0, 8
            ("count_x2_even", 0x2841FFFF),  # negative bound
            ("count_x2_even", 0x28410000),  # a bound of 0 is never true for a count
            ("count_x2_even", 0x2C410000),  # sltiu with a bound of 0
            ("count_x2_even", 0x28414000),  # 2 * 0x4000 does not fit
            ("count_x2_even", 0x24410008),  # addiu: not a compare
            ("gate_30hz_hold", 0x24840001),  # an accumulating update is not a call
            ("gate_30hz_hold", 0x0320F809),  # jalr: codegen gates only a jal
        ):
            with self.subTest(transform=transform, word=f"{word:08x}"):
                with self.assertRaises(EnhancementPackageError) as ctx:
                    plan(transform, word)
                self.assertIn("has no defined rewrite", str(ctx.exception))

        pkg, _ = self._make_package_and_symbols(b"synthetic_elf_bytes")
        pkg["sites"] = [dict(pkg["sites"][-1], transform="gate_30hz_hold")]
        del pkg["update_boundary"]
        with self.assertRaises(EnhancementPackageError) as ctx:
            validate_package(pkg)
        self.assertIn("gate_30hz_hold or gate_30hz_alt sites must declare update_boundary", str(ctx.exception))

    def _even_and_hold_build(self, tmp: Path) -> str:
        # The resolver's static checks: the held call goes to an ease, a leaf that reads f12
        # and writes f0 (f0 = f12 + f14 * (f13 - f12)), and the even bound compares a count
        # loaded just before it.
        self.words.update({
            0x00001500: 0x8CA40000,  # lw a0, 0(a1)
            0x00001504: 0x28830064,  # slti v1, a0, 100
            0x00001508: 0x03E00008, 0x0000150C: 0,
            0x00001600: 0x0C000000 | (0x00001700 >> 2),  # jal synth_ease
            0x00001700: 0x460C6841,  # sub.s f1, f13, f12
            0x00001704: 0x460E0842,  # mul.s f1, f1, f14
            0x00001708: 0x46016000,  # add.s f0, f12, f1
            0x0000170C: 0x03E00008, 0x00001710: 0,
        })
        self.fn_addrs["synth_ease"] = 0x00001700
        elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", self.words, self.entry)
        pkg, symbols = self._make_package_and_symbols(elf_bytes)
        for site in pkg["sites"]:
            if site["id"] == "SYNTH_PROB":  # slti v1, a0, 100, after the count's load
                site.update(transform="count_x2_even", insn_offset=4,
                            stock_digest=compute_stock_words_digest([0x28830064]))
            elif site["id"] == "SYNTH_GATE":  # jal synth_ease
                site["transform"] = "gate_30hz_hold"
        rc, output, files = self._codegen_with(tmp, "even-hold", pkg, symbols)
        self.assertEqual(rc, 0, output)
        return b"\n".join(files.values()).decode("utf-8")

    def test_count_x2_even_and_gate_30hz_hold_emit_both_forms(self) -> None:
        """The enhanced slti doubles its bound and drops odd counts; the held call copies f12 to f0."""
        with tempfile.TemporaryDirectory() as tmp_dir:
            combined = self._even_and_hold_build(Path(tmp_dir))
            self.assertIn("(int32_t)0x000000c8u ? 1u : 0u", combined)  # 100 -> 200
            self.assertIn("(int32_t)0x00000064u ? 1u : 0u", combined)  # the stock form is kept
            self.assertIn("uint32_t _nk_count = s->r[4];", combined)
            # slti: a negative count stays true, as in stock.
            self.assertIn("if ((_nk_count & 1u) && (int32_t)_nk_count >= 0) { s->r[3] = 0u; }", combined)
            self.assertIn("if (!nk_enhancement_on() || nk_enhancement_gate_30hz())", combined)
            self.assertIn("} else { s->fi[0] = s->fi[12]; }", combined)

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_count_x2_even_and_gate_30hz_hold_match_30hz_when_run(self) -> None:
        """Compile the generated statements and run 30 Hz against 60 Hz updates.

        count_x2_even: a counter of updates since an event meets the bound at the stock
        instants only (60 Hz count 2k where the stock count is k), the same number of times.
        gate_30hz_hold: three values advanced by an ease call each update, as a menu pane
        does, take the 30 Hz values, each held for two updates; none takes another's result.
        """
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            combined = self._even_and_hold_build(tmp)
            lines = combined.splitlines()
            at = next(i for i, line in enumerate(lines) if "uint32_t _nk_count" in line)
            compare = "\n".join(lines[at:at + 2])
            # The call statement: from the jal's trace hook to the line that closes its gate,
            # whatever that gate's skipped branch holds.
            at = next(i for i, line in enumerate(lines) if "sr_begin(s, 0x00001600u" in line)
            gate = next(i for i in range(at, len(lines)) if "nk_enhancement_gate_30hz()) {" in lines[i])
            end = next(i for i in range(gate + 1, len(lines)) if lines[i].lstrip().startswith("}"))
            call = "\n".join(lines[at:end + 1])
            authentic_dir = tmp / "authentic"
            authentic_dir.mkdir()
            rc, output, files = self._run_codegen(tmp / "synth.elf", authentic_dir / "out.c", [])
            self.assertEqual(rc, 0, output)
            stock_lines = b"\n".join(files.values()).decode("utf-8").splitlines()
            at = next(i for i, line in enumerate(stock_lines) if "sr_begin(s, 0x00001504u" in line)
            stock_compare = stock_lines[at]
            at = next(i for i, line in enumerate(stock_lines) if "sr_begin(s, 0x00001600u" in line)
            end = next(i for i in range(at, len(stock_lines))
                       if "sr_aot_return_boundary_end" in stock_lines[i])
            stock_call = "\n".join(stock_lines[at:end + 1])
            # Toggle off, each site runs the Authentic statement, byte for byte.
            self.assertIn(f"else {{ {stock_compare.strip()} }}", compare)
            self.assertIn(stock_lines[end], call)  # the checked call itself

            def program(name: str, compare: str, call: str) -> list[str]:
                callee_match = re.search(r"return; (\w+)\(s\);", call)
                self.assertIsNotNone(callee_match, call)
                assert callee_match is not None
                callee = callee_match.group(1)
                harness = tmp / f"{name}.c"
                harness.write_text(
                    "#include \"recomp.h\"\n"
                    "/* The real CpuState and trace hooks; the statement runs alone, with no caller\n"
                    " * frame to return through, so only the return boundary is stubbed. */\n"
                    "#undef SR_TAIL_DRAIN\n#define SR_TAIL_DRAIN(s) ((void)0)\n"
                    "#define sr_aot_return_boundary_begin(s, r, c, k) 0\n"
                    "#define sr_aot_return_boundary_end(s) 0\n"
                    "static int on;\nint nk_enhancement_on(void) { return on; }\n"
                    "uint32_t nk_enhancement_updates;\n"
                    "static inline int nk_enhancement_gate_30hz(void) "
                    "{ return (nk_enhancement_updates & 1u) == 0u; }\n"
                    "/* The ease: f0 = f12 + f14 * (f13 - f12), as the consumer's helper computes. */\n"
                    f"static void {callee}(CpuState *s) "
                    "{ s->f[0] = s->f[12] + s->f[14] * (s->f[13] - s->f[12]); }\n"
                    f"static void compare(CpuState *s) {{\n{compare}\n}}\n"
                    f"static void call(CpuState *s) {{\n{call}\n}}\n"
                    "static float ease(CpuState *s, float cur, float target) {\n"
                    "    s->f[12] = cur; s->f[13] = target; s->f[14] = 0.25f; call(s); return s->f[0];\n"
                    "}\n"
                    "int main(void) {\n"
                    "    CpuState st; CpuState *s = &st; memset(&st, 0, sizeof st);\n"
                    "    for (on = 0; on < 2; on++) {\n"
                    "        for (int32_t n = -4; n < 300; n++) {\n"
                    "            s->r[4] = (uint32_t)n; compare(s);\n"
                    "            if (s->r[3]) printf(\"%s %d\\n\", on ? \"hit60\" : \"hit30\", (int)n);\n"
                    "        }\n"
                    "    }\n"
                    "    for (on = 0; on < 2; on++) {\n"
                    "        float x = 0.0f, y = 100.0f, a = 1.0f;\n"
                    "        nk_enhancement_updates = 0;\n"
                    "        for (int u = 0; u < (on ? 40 : 20); u++) {\n"
                    "            nk_enhancement_updates++;\n"
                    "            x = ease(s, x, 64.0f); y = ease(s, y, 0.0f); a = ease(s, a, 0.0f);\n"
                    "            printf(\"%s %d %.6f %.6f %.6f\\n\", on ? \"ease60\" : \"ease30\", u, x, y, a);\n"
                    "        }\n"
                    "    }\n"
                    "    return 0;\n"
                    "}\n",
                    encoding="utf-8",
                )
                exe = tmp / f"{name}.exe"
                # The generated call statement keeps its guards on one line, as codegen emits it.
                assert GCC is not None, "skipUnless(GCC) runs this test only with a compiler"
                built = subprocess.run([GCC, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                                        "-Wno-misleading-indentation", "-Wno-unused-variable",
                                        f"-I{ROOT / 'src' / 'rt'}", "-o", str(exe), str(harness)],
                                       capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                run = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
                lines: list[str] = run.stdout.split("\n")
                return lines

            out = program("even_hold", compare, call)
            # Section 11.2, item 1: the package build with the toggle off prints what the
            # Authentic build prints, byte for byte.
            authentic = program("authentic", stock_compare, stock_call)
            off = [line for line in out if line.startswith(("hit30 ", "ease30 "))]
            self.assertEqual(off, [line for line in authentic if line.startswith(("hit30 ", "ease30 "))])
            self.assertEqual(len(off), 104 + 20)
            hit30 = [int(line.split()[1]) for line in out if line.startswith("hit30 ")]
            hit60 = [int(line.split()[1]) for line in out if line.startswith("hit60 ")]
            # A negative count is below the slti bound in stock and stays so (an "inactive"
            # sentinel such as -1 keeps its meaning); 0..99 at 30 Hz meet it at 0..198 at 60 Hz.
            self.assertEqual(hit30, [-4, -3, -2, -1] + list(range(100)))
            self.assertEqual(hit60, [-4, -3, -2, -1] + [2 * k for k in range(100)])
            ease30 = [tuple(line.split()[2:]) for line in out if line.startswith("ease30 ")]
            ease60 = [tuple(line.split()[2:]) for line in out if line.startswith("ease60 ")]
            # Update 1 of 60 is odd and holds the start; then each 30 Hz value shows for two updates.
            self.assertEqual(ease60[0], ("0.000000", "100.000000", "1.000000"))
            for k, values in enumerate(ease30[:19]):
                with self.subTest(update=k):
                    self.assertEqual(ease60[2 * k + 1], values)
                    self.assertEqual(ease60[2 * k + 2], values)

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

    def _argument_package(self, tmp: Path, helper_words: dict[int, int]) -> tuple[dict[str, Any], dict[str, str]]:
        """A package with one argument_x2 site on a helper the entry calls."""
        words = dict(self.words)
        words[0x00001030] = 0x0C000000 | (0x00001700 >> 2)  # jal helper, then the stock return
        words[0x00001034] = 0x00000000
        words[0x00001038] = 0x03E00008
        words[0x0000103C] = 0x00000000
        words.update(helper_words)
        elf_bytes = _write_synthetic_mips_elf(tmp / "synth.elf", words, self.entry)
        pkg, symbols = self._make_package_and_symbols(elf_bytes)
        symbols["synth_repeat_helper"] = "0x00001700"
        pkg["sites"].append({
            "id": "SYNTH_ARGS",
            "function": "synth_repeat_helper",
            "insn_offset": 0,
            "stock_digest": compute_stock_words_digest([helper_words[0x00001700]]),
            "transform": "argument_x2",
            "arguments": ["a3", "t0"],
        })
        return pkg, symbols

    def test_argument_x2_doubles_the_named_arguments_only_when_enabled(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            pkg, symbols = self._argument_package(tmp, {
                0x00001700: 0x8C820080,  # lw v0,128(a0)
                0x00001704: 0x03E00008,
                0x00001708: 0x00000000,
            })
            rc, output, files = self._codegen_with(tmp, "args", pkg, symbols)
            self.assertEqual(rc, 0, output)
            combined = b"\n".join(files.values()).decode("utf-8")
            lines = combined.splitlines()
            at = next(i for i, line in enumerate(lines) if "s->r[7] <<= 1; s->r[8] <<= 1;" in line)
            self.assertIn("if (nk_enhancement_on()) {", lines[at])
            # The doubling follows the stock load, and the other arm is the stock load alone.
            self.assertLess(lines[at].index("0x8c820080"), lines[at].index("s->r[7] <<= 1"))
            self.assertIn("else {", lines[at + 1])
            self.assertIn("0x8c820080", lines[at + 1])
            self.assertNotIn("<<= 1", lines[at + 1])

    def test_argument_x2_refuses_a_function_that_branches_back_to_its_entry(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            pkg, symbols = self._argument_package(tmp, {
                0x00001700: 0x8C820080,  # lw v0,128(a0)
                0x00001704: 0x1040FFFE,  # beqz v0, entry: a second pass would double again
                0x00001708: 0x00000000,
                0x0000170C: 0x03E00008,
                0x00001710: 0x00000000,
            })
            with self.assertRaises(EnhancementPackageError) as ctx:
                self._codegen_with(tmp, "loop", pkg, symbols)
            self.assertIn("branches back to its entry", str(ctx.exception))

    def test_argument_x2_refuses_a_function_that_jumps_back_to_its_entry(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            pkg, symbols = self._argument_package(tmp, {
                0x00001700: 0x8C820080,  # lw v0,128(a0)
                0x00001704: 0x08000000 | (0x00001700 >> 2),  # j entry: a second pass would double again
                0x00001708: 0x00000000,
            })
            with self.assertRaises(EnhancementPackageError) as ctx:
                self._codegen_with(tmp, "selfj", pkg, symbols)
            self.assertIn("branches back to its entry", str(ctx.exception))

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

    def test_mapped_factor_runs_only_when_enabled_and_inside_the_unit_interval(self) -> None:
        """An lwc1 factor is mapped after the stock load, behind the toggle, and only inside (0, 1);
        outside it keeps its stock value and the site reports it."""
        lwc1 = 0xC4430010  # lwc1 $f3, 0x10($v0)
        saved = codegen.ENHANCEMENT_SITES
        try:
            for kind, expr in (("decay", "(float)((double)s->f[3] / (1.0 + sqrt(1.0 - (double)s->f[3])))"),
                               ("retention", "sqrtf(s->f[3])")):
                with self.subTest(map=kind):
                    codegen.ENHANCEMENT_SITES = {
                        0x1300: {"id": "F3", "rewrite": {"kind": "map_f_reg", "reg": 3, "map": kind}}
                    }
                    codegen.ENHANCEMENT_APPLIED.clear()
                    line = codegen.normal_line(0x1300, lwc1, lle_cpu=False)
                    enhanced, stock = line.rsplit("else", 1)
                    self.assertIn("nk_enhancement_on()", enhanced)
                    self.assertIn(f"if (s->f[3] > 0.0f && s->f[3] < 1.0f) {{ s->f[3] = {expr}; }} "
                                  'else { static int nk_kept; nk_enhancement_factor_kept(&nk_kept, "F3", '
                                  "0x00001300u, s->f[3]); }", enhanced)
                    self.assertLess(enhanced.index("s->fi[3]"), enhanced.index(expr))
                    self.assertNotIn("sqrt", stock)
                    self.assertNotIn("nk_enhancement_factor_kept", stock)
        finally:
            codegen.ENHANCEMENT_SITES = saved
            codegen.ENHANCEMENT_APPLIED.clear()

    def test_vfpu_single_load_is_scaled_or_mapped_after_the_load(self) -> None:
        """lv.s loads the VFPU register first; the rewrite then scales or maps that register."""
        lvs = 0xC8470011  # lv.s S131.s, 0x10($v0): vt 7 | 1 << 5
        v = codegen.vreg_indices(39, 1)[0]
        saved = codegen.ENHANCEMENT_SITES
        try:
            for rewrite, expr in (({"kind": "scale_v_reg", "vreg": 39, "factor": 2.0}, f"s->v[{v}] *= 2.0f;"),
                                  ({"kind": "map_v_reg", "vreg": 39, "map": "decay"},
                                   f"if (s->v[{v}] > 0.0f && s->v[{v}] < 1.0f) "
                                   f"{{ s->v[{v}] = (float)((double)s->v[{v}] / "
                                   f"(1.0 + sqrt(1.0 - (double)s->v[{v}]))); }}")):
                with self.subTest(kind=rewrite["kind"]):
                    codegen.ENHANCEMENT_SITES = {0x1300: {"id": "S131", "rewrite": rewrite}}
                    codegen.ENHANCEMENT_APPLIED.clear()
                    enhanced, stock = codegen.normal_line(0x1300, lvs, lle_cpu=False).rsplit("else", 1)
                    self.assertIn(expr, enhanced)
                    self.assertLess(enhanced.index(f"s->vi[{v}] = MEM_R32("), enhanced.index(expr))
                    self.assertNotIn(f"s->v[{v}]", stock)
        finally:
            codegen.ENHANCEMENT_SITES = saved
            codegen.ENHANCEMENT_APPLIED.clear()

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_mapped_factor_values_match_the_lui_path(self) -> None:
        """Compile the emitted map and run it. Inside (0, 1) each result is bit for bit the f32 the
        lui path's formula gives, and that is the f32 nearest the exact map down to the smallest
        normal result; 1 - sqrt(1 - k) cancels to 0 there even in double. A subnormal result may
        round once more, so it is within one step of the nearest. Outside (0, 1) the value keeps
        its stock bits and the site reports once."""
        lwc1 = 0xC4430010  # lwc1 $f3, 0x10($v0)
        rng = random.Random(1932)
        ks = [2.0 ** -25, 1e-7, 1e-4, 0.2, 0.5, 0.75, 0.999, _f32_of(0x3F7FFFFF),
              1e-17, 1e-30, 2.0 ** -126, _f32_of(0x00000001), _f32_of(0x007FFFFF)]
        ks += [rng.random() for _ in range(2000)]
        ks += [2.0 ** rng.uniform(-149, -20) for _ in range(500)]
        ks = [struct.unpack("<f", struct.pack("<f", k))[0] for k in ks]
        outside = [0.0, 1.0, 1.5, -0.25, -0.0, _f32_of(0x7FC00000)]  # the last is a quiet NaN
        lvs = 0xC844C410  # lv.s S100.s, -0x3bf0($v0)
        v4 = codegen.vreg_indices(4, 1)[0]
        forms = (("map_f_reg", {"reg": 3}, lwc1, "s->f[3]"), ("map_v_reg", {"vreg": 4}, lvs, f"s->v[{v4}]"))
        saved = codegen.ENHANCEMENT_SITES
        try:
            for (rkind, operand, word, cell), kind in itertools.product(forms, ("decay", "retention")):
                with self.subTest(rewrite=rkind, map=kind):
                    codegen.ENHANCEMENT_SITES = {
                        0x1300: {"id": "K", "rewrite": {"kind": rkind, **operand, "map": kind}}
                    }
                    codegen.ENHANCEMENT_APPLIED.clear()
                    enhanced = codegen.normal_line(0x1300, word, lle_cpu=False).rsplit("else", 1)[0]
                    mapped = enhanced[enhanced.index(f"if ({cell} >"):].rsplit("}", 1)[0]
                    values = ks + outside
                    with tempfile.TemporaryDirectory() as tmp_dir:
                        tmp = Path(tmp_dir)
                        (tmp / "k.txt").write_text("\n".join(struct.pack("<f", k).hex() for k in values),
                                                   encoding="utf-8")
                        harness = tmp / "map.c"
                        harness.write_text(
                            "#include <math.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n"
                            "typedef struct { float f[32]; float v[128]; } S;\n"
                            "static int reports;\n"
                            "static void nk_enhancement_factor_kept(int *reported, const char *site, "
                            "uint32_t pc, float value) {\n"
                            "    (void)site; (void)pc; (void)value;\n"
                            "    if (!*reported) { *reported = 1; reports++; }\n"
                            "}\n"
                            "int main(int argc, char **argv) {\n"
                            "    FILE *in = fopen(argv[1], \"r\"); char hex[16]; S st, *s = &st;\n"
                            "    if (argc < 2 || !in) return 1;\n"
                            "    while (fscanf(in, \"%15s\", hex) == 1) {\n"
                            "        uint8_t b[4]; uint32_t bits;\n"
                            "        for (int i = 0; i < 4; i++) sscanf(hex + 2 * i, \"%2hhx\", &b[i]);\n"
                            f"        memcpy(&{cell}, b, 4);\n"
                            f"        {mapped}\n"
                            f"        memcpy(&bits, &{cell}, 4); printf(\"%08x\\n\", (unsigned)bits);\n"
                            "    }\n"
                            "    printf(\"reports=%d\\n\", reports);\n"
                            "    fclose(in); return 0;\n"
                            "}\n",
                            encoding="utf-8",
                        )
                        exe = tmp / "map.exe"
                        subprocess.run([str(GCC), "-std=c11", "-O2", "-Wall", "-Werror", "-o", str(exe),
                                        str(harness), "-lm"], check=True, capture_output=True, text=True)
                        out = subprocess.run([str(exe), str(tmp / "k.txt")], check=True, capture_output=True,
                                             text=True).stdout.split()
                    self.assertEqual(out.pop(), "reports=1")
                    transform = {"decay": "decay_k_to_1-sqrt(1-k)", "retention": "retention_m_to_sqrt(m)"}[kind]
                    for k, got in zip(values, out, strict=True):
                        if not 0.0 < k < 1.0:
                            self.assertEqual(int(got, 16), struct.unpack("<I", struct.pack("<f", k))[0])
                            continue
                        want = struct.unpack("<I", struct.pack("<f", _scale_value(transform, k)))[0]
                        self.assertEqual(int(got, 16), want, f"k={k!r}")
                        if kind == "decay":
                            nearest = _f32_decay_reference(k)
                            self.assertLessEqual(abs(want - nearest), 0 if nearest >= 0x00800000 else 1,
                                                 f"k={k!r}")
        finally:
            codegen.ENHANCEMENT_SITES = saved
            codegen.ENHANCEMENT_APPLIED.clear()

    def test_lle_scaled_store_never_writes_the_fpr(self) -> None:
        """Under --lle-cpu a faulting swc1 returns early, so the rewrite must not touch s->f."""
        swc1 = 0xE4A40010  # swc1 $f4, 0x10($a1)
        saved = (codegen.ENHANCEMENT_SITES, codegen.OPTIONS.lle_cpu)
        try:
            codegen.ENHANCEMENT_SITES = {
                0x1300: {"id": "F4", "rewrite": {"kind": "scale_f_store", "reg": 4, "factor": 0.5}}
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
            codegen.ENHANCEMENT_SITES, codegen.OPTIONS.lle_cpu = saved
            codegen.ENHANCEMENT_APPLIED.clear()


class TestEnhancementDataTransforms(unittest.TestCase):
    """Data values and CSV cells rescale exactly or refuse."""

    def test_data_values(self) -> None:
        self.assertEqual(plan_data_value("count_x2", "s32", struct.pack("<i", -3)), struct.pack("<i", -6))
        self.assertEqual(plan_data_value("step_x0.5", "u16", struct.pack("<H", 40)), struct.pack("<H", 20))
        self.assertEqual(plan_data_value("retention_m_to_sqrt(m)", "f32", struct.pack("<f", 0.25)),
                         struct.pack("<f", 0.5))
        tiny = struct.unpack("<f", struct.pack("<f", 1e-30))[0]
        self.assertEqual(plan_data_value("decay_k_to_1-sqrt(1-k)", "f32", struct.pack("<f", tiny)),
                         struct.pack("<I", _f32_decay_reference(tiny)))
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
        pkg: dict[str, Any] = {
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
    return float(struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0])


def _bits_of(value: float) -> int:
    return int(struct.unpack("<I", struct.pack("<f", value))[0])


class TestEnhancementPhysicsAndOffsetTransforms(unittest.TestCase):
    """offset_x2, window_start_x2, accel_x0.25, launch_velocity_x0.5 and f64_high."""

    def _plan(self, transform: str, word: int, **extra: Any) -> dict[str, Any]:
        planned: dict[str, Any] = plan_site_rewrite(dict({"id": "S", "transform": transform}, **extra), word)
        return planned

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

    def test_argument_x2_plans_and_refuses(self) -> None:
        site = {"arguments": ["a3", "t0"], "insn_offset": 0, "word_count": 1}
        # lw v0,128(a0) leaves the arguments alone.
        self.assertEqual(self._plan("argument_x2", 0x8C820080, **site),
                         {"kind": "scale_int_args", "regs": [7, 8]})
        # lw a3,128(a0) writes one: doubling after it would double the loaded value.
        with self.assertRaises(EnhancementPackageError) as ctx:
            self._plan("argument_x2", 0x8C870080, **site)
        self.assertIn("writes an argument", str(ctx.exception))
        # Every other form that writes a3 refuses too, not only the plain loads.
        for name, word in self._A3_WRITERS:
            with self.subTest(form=name):
                with self.assertRaises(EnhancementPackageError) as ctx:
                    self._plan("argument_x2", word, **site)
                self.assertIn("writes an argument", str(ctx.exception))

    # (form, encoding) of instructions that write a3 (r7).
    _A3_WRITERS = (
        ("lwl", 0x88870000),
        ("lwr", 0x98870000),
        ("ll", 0xC0870000),
        ("sc", 0xE0870000),
        ("mfc1", 0x44071000),
        ("cfc1", 0x4447F800),
        ("mfc0", 0x40076000),
        ("ext", 0x7C873800),
        ("ins", 0x7C873804),
        ("seb", 0x7C043C20),
        ("seh", 0x7C043E20),
        ("clz", 0x00803816),
        ("clo", 0x00803817),
        ("mfv", 0x48670000),
        ("mfvc", 0x48670080),
    )

    def test_gpr_written_table(self) -> None:
        for name, word in self._A3_WRITERS:
            with self.subTest(form=name):
                self.assertEqual(_gpr_written(word), 7)
        for name, word in (
            ("sw", 0xAFA70000),
            ("mtc1", 0x44843800),
            ("mtv", 0x48E70000),
            ("mtvc", 0x48E70080),
            ("mthi", 0x00800011),
            ("mtlo", 0x00800013),
            ("mult", 0x00870018),
            ("jr", 0x03E00008),
            ("syscall", 0x0000000C),
            ("sync", 0x0000000F),
            ("beq", 0x10870002),
        ):
            with self.subTest(form=name):
                self.assertIsNone(_gpr_written(word))

    def test_argument_x2_validation(self) -> None:
        for extra, why in (
            ({}, "needs a list"),
            ({"arguments": []}, "needs a list"),
            ({"arguments": ["s0"]}, "needs a list"),
            ({"arguments": ["a3", "a3"]}, "needs a list"),
            ({"arguments": "a3"}, "needs a list"),
            ({"arguments": ["a3"], "insn_offset": 4}, "function entry"),
            ({"arguments": ["a3"], "word_count": 2}, "function entry"),
        ):
            with self.subTest(extra=extra):
                site = dict({"transform": "argument_x2", "insn_offset": 0, "word_count": 1}, **extra)
                with self.assertRaises(EnhancementPackageError) as ctx:
                    _validate_site_extras(site, "sites[0]")
                self.assertIn(why, str(ctx.exception))
        with self.assertRaises(EnhancementPackageError) as ctx:
            _validate_site_extras({"transform": "count_x2", "insn_offset": 0, "word_count": 1,
                                   "arguments": ["a3"]}, "sites[0]")
        self.assertIn("belongs only to argument_x2", str(ctx.exception))

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
        pkg: dict[str, Any] = {
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


def _i(op: int, rs: int, rt: int, imm: int) -> int:
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def _jal(target: int) -> int:
    return (0x03 << 26) | ((target >> 2) & 0x3FFFFFF)


def _move(rd: int, rs: int) -> int:
    return (rs << 21) | (rd << 11) | 0x21     # addu rd, rs, zero


_NOP, _JR_RA = 0, 0x03E00008
_ZERO, _AT, _V0, _V1, _A0, _A1, _A2, _SP = 0, 1, 2, 3, 4, 5, 6, 29


def _program(blocks: dict[int, list[int]]) -> dict[int, int]:
    return {base + 4 * i: w for base, ws in blocks.items() for i, w in enumerate(ws)}


class TestEnhancementDraft(unittest.TestCase):
    """The 60 Hz draft finds the frame-pacing waits of a synthetic title."""

    STUB_REG, STUB_WAIT, STUB_MULTI = 0x9000, 0x9008, 0x9010
    IMPORTS = {STUB_REG: 0xCA04A2B9, STUB_WAIT: 0x984C27E7, STUB_MULTI: 0x40F1469C}

    def _counter_title(self, *, increment: bool = True, wrapper: bool = True) -> dict[int, int]:
        handler = [
            _i(0x0F, 0, _V1, 0x0031), _i(0x23, _V1, _A1, 0x105C),          # lw a1, counter
            _i(0x0F, 0, _A0, 0x0031),
            _i(0x09, _A1, _A1, 1 if increment else 0),                      # addiu a1, a1, 1
            _i(0x2B, _A0, _A1, 0x105C), _JR_RA, _NOP,
        ]
        if wrapper:   # register_vblank(handler) -> sceKernelRegisterSubIntrHandler(30, 0, handler, 0)
            registration = {
                0x1100: [_i(0x0F, 0, _A0, 0), _i(0x09, _A0, _A0, 0x1000), _jal(0x1180), _NOP, _JR_RA, _NOP],
                0x1180: [_i(0x09, _SP, _SP, -16), _move(_V1, _A0), _i(0x09, _ZERO, _A0, 30),
                         _move(_A2, _V1), _jal(self.STUB_REG), _NOP, _JR_RA, _NOP],
            }
        else:
            registration = {
                0x1100: [_i(0x0F, 0, _A2, 0), _i(0x09, _A2, _A2, 0x1000), _i(0x09, _ZERO, _A0, 30),
                         _jal(self.STUB_REG), _NOP, _JR_RA, _NOP],
            }
        wait = [
            _jal(self.STUB_WAIT), _NOP,                                       # wait one VBLANK
            _i(0x0F, 0, _V0, 0x0031), _i(0x23, _V0, _V0, 0x105C),
            _i(0x0B, _V0, _AT, 2),                                            # sltiu at, v0, 2
            _i(0x05, _AT, 0, -6 & 0xFFFF), _NOP,                              # bnez at, back
            _i(0x0F, 0, _V0, 0x0031), _i(0x2B, _V0, _ZERO, 0x105C),           # sw zero, counter
            _JR_RA, _NOP,
        ]
        return _program({0x1000: handler, **registration, 0x1200: wait,
                         0x1300: [_jal(0x1200), _NOP, _JR_RA, _NOP],          # frame body
                         0x1400: [_jal(0x1300), _NOP, _JR_RA, _NOP]})         # main loop

    def test_counter_poll_through_a_registration_wrapper(self) -> None:
        for wrapper in (True, False):
            with self.subTest(wrapper=wrapper):
                words = self._counter_title(wrapper=wrapper)
                draft = draft_timing_package(words, self.IMPORTS)
                self.assertEqual(len(draft["pacing_sites"]), 1)
                site = draft["pacing_sites"][0]
                self.assertEqual((site["kind"], site["address"], site["function"], site["insn_offset"]),
                                 ("vblank_counter_poll", 0x1210, 0x1200, 0x10))
                self.assertEqual(site["counter"], 0x0031105C)
                self.assertTrue(site["handler_registered"])
                self.assertEqual(draft["boundary_candidates"], [{"function": 0x1300, "calls": [0x1300]}])
                self.assertEqual(draft["unexplained_waits"], [])   # the poll's own one-VBLANK wait

    def test_package_validates_and_rewrites_the_poll(self) -> None:
        words = self._counter_title()
        package, symbols = draft_package_declaration(
            draft_timing_package(words, self.IMPORTS), words, package_id="synth-60hz",
            title_id="synthetic-allegrex-v1", executable_sha256="a" * 64, game_version="1.00")
        validate_package(package)
        self.assertEqual(symbols, {"draft_fn_00001200": "0x00001200", "draft_fn_00001300": "0x00001300"})
        self.assertEqual(package["update_boundary"]["function"], "draft_fn_00001300")
        site = package["sites"][0]
        self.assertEqual(site["transform"], "vblank_wait_2_to_1")
        self.assertEqual(plan_site_rewrite(site, words[0x1210])["word"], _i(0x0B, _V0, _AT, 1))
        resolved = verify_and_resolve_sites(
            package, executable_sha256="a" * 64, symbol_to_addr={k: int(v, 16) for k, v in symbols.items()},
            read_words_fn=lambda a, n: [words[a + 4 * i] for i in range(n)] if a in words else None)
        self.assertEqual(sorted(resolved), [0x1210])

    def test_wait_multi_and_unexplained_waits(self) -> None:
        words = _program({
            0x2000: [_i(0x09, _ZERO, _A0, 2), _jal(self.STUB_MULTI), _NOP, _JR_RA, _NOP],
            0x2100: [_jal(self.STUB_WAIT), _NOP, _jal(self.STUB_WAIT), _NOP, _JR_RA, _NOP],
            0x2200: [_jal(0x2000), _NOP, _jal(0x2000), _NOP, _JR_RA, _NOP],
            0x2300: [_jal(0x2000), _NOP, _jal(0x2100), _NOP, _JR_RA, _NOP],
        })
        draft = draft_timing_package(words, self.IMPORTS, function_starts=[0x2200, 0x2300])  # symbol entries
        self.assertEqual([(s["kind"], s["address"], s["api"]) for s in draft["pacing_sites"]],
                         [("wait_multi_2", 0x2000, "sceDisplayWaitVblankStartMulti")])
        self.assertEqual(draft["boundary_candidates"], [{"function": 0x2200, "calls": [0x2200, 0x2208]},
                                                        {"function": 0x2300, "calls": [0x2300]}])
        self.assertEqual([(w["call"], w["function"]) for w in draft["unexplained_waits"]],
                         [(0x2100, 0x2100), (0x2108, 0x2100)])
        package, _ = draft_package_declaration(draft, words, package_id="synth-60hz",
                                               title_id="synthetic-allegrex-v1", executable_sha256="a" * 64,
                                               game_version="1.00")
        self.assertNotIn("update_boundary", package)   # two candidates: a person chooses
        self.assertEqual(plan_site_rewrite(package["sites"][0], words[0x2000])["word"], _i(0x09, _ZERO, _A0, 1))

    def test_no_pacing_without_an_increment_or_a_clear(self) -> None:
        words = self._counter_title(increment=False)
        draft = draft_timing_package(words, self.IMPORTS)
        self.assertEqual(draft["pacing_sites"], [])
        self.assertEqual([w["call"] for w in draft["unexplained_waits"]], [0x1200])
        with self.assertRaisesRegex(EnhancementPackageError, "No frame-pacing wait"):
            draft_package_declaration(draft, words, package_id="synth-60hz", title_id="synthetic-allegrex-v1",
                                      executable_sha256="a" * 64, game_version="1.00")
        cleared_elsewhere = self._counter_title()
        cleared_elsewhere[0x1220] = _NOP     # drop the `sw zero, counter`
        self.assertEqual(draft_timing_package(cleared_elsewhere, self.IMPORTS)["pacing_sites"], [])


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


def _sample_csv(path: Path, *, hz: int, seconds: int, columns: dict[str, Any]) -> None:
    """Write a synthetic SR_SAMPLE_OUT file: one row per vblank, logic at `hz` updates a second.

    `columns` maps a label to a function of (update count, vblank) giving the cell text.
    """
    lines = ["vblank,updates," + ",".join(columns)]
    vblanks = round(seconds * 60000 / 1001)
    for v in range(1, vblanks + 1):
        n = (v * hz * 1001) // 60000
        lines.append(f"{v},{n & 0xFFFFFFFF}," + ",".join(fn(n, v) for fn in columns.values()))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


class TestEnhancementRateComparison(unittest.TestCase):
    """Section 9.7: the side-by-side timing check of a stock and an enhanced run."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)

    def _run(self, stock: dict[str, Any], enhanced: dict[str, Any], *extra: str,
             enhanced_hz: int = 60, seconds: int = 60) -> tuple[int, str, dict[str, Any]]:
        _sample_csv(self.tmp / "stock.csv", hz=30, seconds=seconds, columns=stock)
        _sample_csv(self.tmp / "enh.csv", hz=enhanced_hz, seconds=seconds, columns=enhanced)
        out = StringIO()
        with mock.patch("sys.stdout", out):
            rc = enhancement_package.main(["--compare-rates", str(self.tmp / "stock.csv"), str(self.tmp / "enh.csv"),
                                           "--json", str(self.tmp / "r.json"), *extra])
        return rc, out.getvalue(), json.loads((self.tmp / "r.json").read_text(encoding="utf-8"))

    def test_rescaled_events_match_and_pass(self) -> None:
        # One serve every 45 stock updates; the package doubles the count, so every 90 enhanced updates.
        rc, text, data = self._run({"serves": lambda n, v: str(n // 45)}, {"serves": lambda n, v: str(n // 90)},
                                   "--event", "count:serves")
        self.assertEqual(rc, 0, text)
        self.assertIn("rates: PASS", text)
        whole = data["report"]["phases"][0]
        self.assertEqual(whole["logic"], "OK")
        self.assertEqual(whole["events"][0]["verdict"], "PASS")
        self.assertAlmostEqual(whole["events"][0]["ratio"], 1.0, delta=0.05)

    def test_an_unrescaled_count_fails(self) -> None:
        # A frame count the package missed: the event comes twice as often per second.
        rc, text, data = self._run({"serves": lambda n, v: str(n // 45)}, {"serves": lambda n, v: str(n // 45)},
                                   "--event", "count:serves")
        self.assertEqual(rc, 1, text)
        self.assertIn("rates: FAIL", text)
        self.assertAlmostEqual(data["report"]["phases"][0]["events"][0]["ratio"], 2.0, delta=0.1)

    def test_lcg_steps_are_counted_and_compared_with_the_declared_ratio(self) -> None:
        a, c = 1103515245, 12345

        def lcg(steps: int) -> str:
            x = 7
            for _ in range(steps):
                x = (a * x + c) & 0xFFFFFFFF
            return str(x)

        cache: dict[int, str] = {}

        def rolls(n: int, v: int) -> str:  # one roll per update
            if n not in cache:
                cache[n] = lcg(n)
            return cache[n]

        rc, text, data = self._run({"seed": rolls}, {"seed": rolls}, "--event", f"lcg:seed:{a}:{c}=2",
                                   seconds=10)
        self.assertEqual(rc, 0, text)
        ev = data["report"]["phases"][0]["events"][0]
        self.assertEqual((ev["stock"], ev["verdict"], ev["unresolved"]), (data["stock"]["phases"]["all"]["updates"],
                                                                           "PASS", 0))
        rc, text, _ = self._run({"seed": rolls}, {"seed": rolls}, "--event", f"lcg:seed:{a}:{c}", seconds=10)
        self.assertEqual(rc, 1, text)

    def test_an_lcg_jump_past_the_limit_is_unresolved_not_zero(self) -> None:
        rc, text, data = self._run({"seed": lambda n, v: str(v * 977)}, {"seed": lambda n, v: str(v * 977)},
                                   "--event", "lcg:seed:1103515245:12345", "--lcg-limit", "8", seconds=2)
        self.assertEqual(rc, 1, text)
        self.assertGreater(data["report"]["unresolved"], 0)

    def test_slowed_logic_and_rare_events_are_incomplete_not_pass(self) -> None:
        rc, text, data = self._run({"serves": lambda n, v: str(n // 45)}, {"serves": lambda n, v: str(n // 90)},
                                   "--event", "count:serves", enhanced_hz=45)
        self.assertEqual(rc, 2, text)
        self.assertIn("rates: INCOMPLETE", text)
        self.assertEqual(data["report"]["phases"][0]["logic"], "SLOWED")
        rc, text, data = self._run({"rare": lambda n, v: str(n // 3000)}, {"rare": lambda n, v: str(n // 6000)},
                                   "--event", "count:rare")
        self.assertEqual(rc, 2, text)
        self.assertEqual(data["report"]["phases"][0]["events"][0]["verdict"], "NOT_JUDGED")

    def test_phases_split_by_column_and_the_counter_may_wrap(self) -> None:
        base = 0xFFFFFFC0  # wraps after 64 of the 120 updates
        _sample_csv(self.tmp / "s.csv", hz=30, seconds=4, columns={"mode": lambda n, v: "1" if v < 120 else "2"})
        lines = (self.tmp / "s.csv").read_text(encoding="utf-8").splitlines()
        wrapped = [lines[0]] + [",".join([c[0], str((int(c[1]) + base) & 0xFFFFFFFF), c[2]])
                                for c in (row.split(",") for row in lines[1:])]
        (self.tmp / "w.csv").write_text("\n".join(wrapped) + "\n", encoding="utf-8")
        rows = enhancement_package.load_sample_rows(self.tmp / "w.csv", ["updates", "mode"])
        m = enhancement_package.measure_rates(rows, updates="updates", nominal_hz=30, phase="mode")
        self.assertEqual(set(m["phases"]), {"1", "2"})
        total = sum(b["updates"] for b in m["phases"].values())
        self.assertEqual(total, int(lines[-1].split(",")[1]) - int(lines[1].split(",")[1]))
        self.assertAlmostEqual(m["phases"]["1"]["updates_per_second"], 30.0, delta=0.5)

    def test_change_events_and_a_loading_phase(self) -> None:
        # A flag that flips every 45 stock updates; the package doubles the period. The enhanced
        # run opens with a phase where no logic runs, which must not mark the run as slowed.
        flip = {"mode": lambda n, v: "1", "flag": lambda n, v: str((n // 45) % 2)}
        _sample_csv(self.tmp / "stock.csv", hz=30, seconds=60, columns=flip)
        _sample_csv(self.tmp / "enh.csv", hz=60, seconds=60,
                    columns={"mode": lambda n, v: "1", "flag": lambda n, v: str((n // 90) % 2)})
        lines = (self.tmp / "enh.csv").read_text(encoding="utf-8").splitlines()
        first_updates = lines[1].split(",")[1]
        loading = [f"{v},{first_updates},0,0" for v in range(1, 601)]
        shifted = [",".join([str(int(c[0]) + 600), *c[1:]]) for c in (row.split(",") for row in lines[1:])]
        (self.tmp / "enh.csv").write_text("\n".join([lines[0], *loading, *shifted]) + "\n", encoding="utf-8")
        out = StringIO()
        with mock.patch("sys.stdout", out):
            rc = enhancement_package.main(["--compare-rates", str(self.tmp / "stock.csv"), str(self.tmp / "enh.csv"),
                                           "--phase", "mode", "--event", "change:flag",
                                           "--json", str(self.tmp / "r.json")])
        data = json.loads((self.tmp / "r.json").read_text(encoding="utf-8"))
        self.assertEqual(rc, 0, out.getvalue())
        whole, loading_phase = data["report"]["phases"][0], data["report"]["phases"][1]
        self.assertEqual((loading_phase["phase"], loading_phase["logic"]), ("0", "ABSENT"))
        self.assertLess(whole["enhanced"]["updates_per_second"], 57.0)
        self.assertEqual(whole["events"][0]["verdict"], "PASS")
        self.assertGreater(whole["events"][0]["stock"], 30)

    def test_a_failing_phase_fails_a_run_whose_totals_pass(self) -> None:
        # Phase 1 (120 s) is rescaled right; phase 2 (20 s) runs its event at the stock period, so
        # twice as often per game second. The totals dilute it; the phase row must still fail.
        boundary = round(120 * 60000 / 1001)
        n_b = (boundary * 60 * 1001) // 60000

        def mode(n: int, v: int) -> str:
            return "1" if v < boundary else "2"

        def enhanced_count(n: int, v: int) -> str:
            return str(n // 18 if v < boundary else n_b // 18 + (n - n_b) // 9)

        rc, text, data = self._run({"mode": mode, "hits": lambda n, v: str(n // 9)},
                                   {"mode": mode, "hits": enhanced_count},
                                   "--phase", "mode", "--event", "count:hits", seconds=140)
        whole, first, second = data["report"]["phases"]
        self.assertEqual(whole["events"][0]["verdict"], "PASS", text)
        self.assertEqual(first["events"][0]["verdict"], "PASS", text)
        self.assertEqual(second["events"][0]["verdict"], "FAIL", text)
        self.assertEqual(rc, 1, text)
        self.assertIn("rates: FAIL (1 event(s): 1 result(s) differ [2 count:hits]", text)

    def test_a_truncated_or_one_sided_run_is_incomplete(self) -> None:
        serves = {"serves": lambda n, v: str(n // 45)}
        rc, text, data = self._run(serves, {"serves": lambda n, v: str(n // 90)}, "--event", "count:serves")
        self.assertEqual(rc, 0, text)
        # The enhanced capture stops halfway: its rates still agree, but it covers half the route.
        _sample_csv(self.tmp / "stock.csv", hz=30, seconds=60, columns=serves)
        _sample_csv(self.tmp / "enh.csv", hz=60, seconds=30, columns={"serves": lambda n, v: str(n // 90)})
        out = StringIO()
        with mock.patch("sys.stdout", out):
            rc = enhancement_package.main(["--compare-rates", str(self.tmp / "stock.csv"),
                                           str(self.tmp / "enh.csv"), "--event", "count:serves"])
        self.assertEqual(rc, 2, out.getvalue())
        self.assertIn("the shorter run covers 50% of the longer one's game time (below 90%)", out.getvalue())
        # A phase that runs logic in one run only means the runs did not follow the same route.
        rc, text, data = self._run({"mode": lambda n, v: "1", **serves},
                                   {"mode": lambda n, v: "1" if v < 3000 else "2",
                                    "serves": lambda n, v: str(n // 90)},
                                   "--phase", "mode", "--event", "count:serves")
        self.assertEqual(rc, 2, text)
        self.assertEqual(data["report"]["one_sided_phases"], ["2"])

    def test_repeated_rows_are_skipped_and_empty_cells_scope_the_event(self) -> None:
        # The sampler can write one vblank twice: an exact repeat is skipped and counted. An empty
        # event cell leaves that step out of the event's count and out of its game time.
        (self.tmp / "s.csv").write_text("vblank,updates,flag\n1,0,0\n2,1,0\n2,1,0\n3,2,1\n4,3,\n5,4,1\n",
                                        encoding="utf-8")
        rows = enhancement_package.load_sample_rows(self.tmp / "s.csv", ["updates", "flag"])
        m = enhancement_package.measure_rates(rows, updates="updates", nominal_hz=30,
                                              events=[enhancement_package.parse_rate_event("change:flag")])
        b = m["phases"]["all"]
        self.assertEqual((m["duplicate_rows"], m["empty_steps"]["change:flag"]), (1, 2))
        self.assertEqual((b["vblanks"], b["updates"], b["events"]["change:flag"]), (4, 4, 1))
        self.assertEqual(b["event_updates"]["change:flag"], 2)

    def test_a_capture_that_loses_time_refuses(self) -> None:
        cases = (
            ("vblank,updates\n1,0\nx,1\n", r"s.csv:3: column vblank: 'x' is not an integer"),
            ("vblank,updates\n1,0\n1,1\n", r"s.csv:3: vblank 1 does not follow vblank 1 with new values"),
            ("vblank,updates\n2,0\n1,1\n", r"s.csv:3: vblank 1 does not follow vblank 2"),
            ("vblank,updates\n1,0\n2,\n", r"s.csv:3: no updates value"),
            ("vblank,updates,serves\n1,0,0\n2,1,two\n", r"s.csv:3: column serves: 'two' is not an integer"),
        )
        for body, why in cases:
            (self.tmp / "s.csv").write_text(body, encoding="utf-8")
            columns = ["updates"] + (["serves"] if "serves" in body else [])
            rows = enhancement_package.load_sample_rows(self.tmp / "s.csv", columns)
            events = [enhancement_package.parse_rate_event("count:serves")] if "serves" in body else []
            with self.subTest(why=why), self.assertRaisesRegex(EnhancementPackageError, why):
                enhancement_package.measure_rates(rows, updates="updates", nominal_hz=30, events=events,
                                                  source=str(self.tmp / "s.csv"))

    def test_bad_inputs_and_options_give_a_rates_fail_line(self) -> None:
        _sample_csv(self.tmp / "good.csv", hz=30, seconds=1, columns={"serves": lambda n, v: str(n // 9)})
        (self.tmp / "latin1.csv").write_bytes(b"vblank,updates\n1,\xe9\n")
        good, event = str(self.tmp / "good.csv"), ["--event", "count:serves"]
        cases = (
            ([str(self.tmp / "missing.csv"), good, *event], "cannot read the sample file"),
            ([str(self.tmp / "latin1.csv"), good], "cannot read the sample file"),
            ([good, good, *event, "--stock-hz", "0"], "the nominal rate must be a positive number"),
            ([good, good, *event, "--enhanced-hz", "nan"], "the nominal rate must be a positive number"),
            ([good, good, *event, "--lcg-limit", "0"], "--lcg-limit must be at least 1"),
            ([good, good, *event, "--min-coverage", "2"], r"--min-coverage must lie in \[0, 1\]"),
        )
        for argv, why in cases:
            err = StringIO()
            with self.subTest(why=why), mock.patch("sys.stderr", err), mock.patch("sys.stdout", StringIO()):
                rc = enhancement_package.main(["--compare-rates", *argv])
                self.assertEqual(rc, 1)
                self.assertRegex(err.getvalue(), "rates: FAIL: .*" + why)

    def test_bad_sample_files_refuse(self) -> None:
        for body, why in (("", "empty sample file"), ("frame,updates\n1,0\n", "first column must be vblank"),
                          ("vblank,updates\n1,0\n2\n", r"s.csv:3: 1 cells, the header has 2")):
            (self.tmp / "s.csv").write_text(body, encoding="utf-8")
            with self.subTest(why=why), self.assertRaisesRegex(EnhancementPackageError, why):
                enhancement_package.load_sample_rows(self.tmp / "s.csv", ["updates"])
        _sample_csv(self.tmp / "s.csv", hz=30, seconds=1, columns={})
        err = StringIO()
        with mock.patch("sys.stderr", err):
            rc = enhancement_package.main(["--compare-rates", str(self.tmp / "s.csv"), str(self.tmp / "s.csv"),
                                           "--event", "count:serves"])
        self.assertEqual(rc, 1)
        self.assertIn("rates: FAIL:", err.getvalue())
        self.assertIn("no column serves", err.getvalue())

    def test_bad_event_specs_and_missing_columns_refuse(self) -> None:
        for spec in ("count", "sum:x", "lcg:x:1", "count:x=0", "change:x:y", "lcg:x:1:1:65"):
            with self.subTest(spec=spec), self.assertRaises(EnhancementPackageError):
                enhancement_package.parse_rate_event(spec)
        _sample_csv(self.tmp / "s.csv", hz=30, seconds=1, columns={})
        with self.assertRaisesRegex(EnhancementPackageError, "no column serves"):
            enhancement_package.load_sample_rows(self.tmp / "s.csv", ["updates", "serves"])


def _f32_bits_of(value: float) -> int:
    return int(struct.unpack("<I", struct.pack("<f", value))[0])


class TestEnhancementStoreSites(unittest.TestCase):
    """Load-time store sites: stock_values, the applied and mismatch lines, and requires."""

    SWC1 = 0xE4820000   # swc1 f2, 0(a0)
    SW = 0xAC850000     # sw a1, 0(a0)
    SW_SELF = 0xAC840000  # sw a0, 0(a0): the value register is also the base
    SW_ZERO = 0xAC800000  # sw zero, 0(a0)
    ADDIU = 0x2406001E  # addiu a2, zero, 30

    def _site(self, sid: str, word: int, **extra: Any) -> dict[str, Any]:
        return dict({"id": sid, "function": "synth_fn", "insn_offset": 0,
                     "stock_digest": compute_stock_words_digest([word]), "transform": "count_x2"},
                    **extra)

    def _package(self, sites: list[dict[str, Any]], **extra: Any) -> dict[str, Any]:
        return dict({
            "schema_version": 1, "id": "synth.store", "display_name": "Synthetic store sites",
            "version": "1.0.0", "category": "timing_frame_presentation",
            "target_title_id": "synthetic-allegrex-v1", "game_version": "1.00",
            "executable_sha256": "a" * 64, "capabilities": ["count_x2"],
            "rng_policy": {"mode": "subtick_30hz", "reason": "No random rolls in this fixture."},
            "sites": sites,
        }, **extra)

    def _plan(self, word: int, **extra: Any) -> dict[str, Any]:
        plan: dict[str, Any] = plan_site_rewrite(dict({"id": "S", "transform": "count_x2"}, **extra), word)
        return plan

    def _refused(self, word: int, why: str, **extra: Any) -> None:
        with self.subTest(word=hex(word), extra=extra):
            with self.assertRaisesRegex(EnhancementPackageError, why):
                self._plan(word, **extra)

    def test_validator_accepts_store_sites_with_stock_values(self) -> None:
        pkg = self._package([
            self._site("F_STORE", self.SWC1, stock_values=[30, 60.5]),
            self._site("I_STORE", self.SW, function="synth_other", stock_values=[9, -9]),
        ])
        validate_package(pkg)
        rw = self._plan(self.SWC1, stock_values=[30, 60.5])
        self.assertEqual(rw["kind"], "scale_f_store")
        self.assertEqual(rw["reg"], 2)
        self.assertEqual(rw["stock_values"], [[_f32_bits_of(30.0), _f32_bits_of(60.0)],
                                              [_f32_bits_of(60.5), _f32_bits_of(121.0)]])
        rw = self._plan(self.SW, stock_values=[9, -9])
        self.assertEqual(rw["kind"], "scale_int_store")
        self.assertEqual(rw["reg"], 5)
        self.assertEqual(rw["stock_values"], [[9, 18], [0xFFFFFFF7, 0xFFFFFFEE]])
        # A store without stock_values keeps its plain rewrite.
        self.assertEqual(self._plan(self.SWC1), {"kind": "scale_f_store", "reg": 2, "factor": 2.0})

    def test_stock_values_refusals(self) -> None:
        self._refused(self.ADDIU, "stock_values belongs only to", stock_values=[30])
        self._refused(self.SW, "needs stock_values")
        self._refused(self.SW, "no defined rewrite", transform="step_x0.5", stock_values=[8])
        self._refused(self.SW_SELF, "base register", stock_values=[8])
        self._refused(self.SW_ZERO, r"must not be \$zero", stock_values=[0])
        self._refused(self.SWC1, "duplicate", stock_values=[1.0, 1.0])
        self._refused(self.SWC1, "duplicate", stock_values=[0.1, 0.10000000149011612])
        self._refused(self.SWC1, "not finite", stock_values=[float("nan")])
        self._refused(self.SWC1, "not finite", stock_values=[float("inf")])
        self._refused(self.SWC1, "not finite", stock_values=[1e39])
        self._refused(self.SWC1, "not finite", stock_values=[3.0e38])   # doubled, past f32
        self._refused(self.SW, "duplicate", stock_values=[9, 9])
        self._refused(self.SW, "s32", stock_values=[2 ** 31])
        self._refused(self.SW, "s32", stock_values=[2 ** 30])           # doubled, 2^31 does not fit
        self._refused(self.SW, "s32", stock_values=[-2 ** 30 - 1])
        self._refused(self.SW, "integer", stock_values=[1.5])
        # The shape is checked before any instruction is read.
        for bad in ([], "30", [True], ["30"], [None], 30):
            with self.subTest(stock_values=bad):
                with self.assertRaisesRegex(EnhancementPackageError, "stock_values must be a non-empty list of numbers"):
                    validate_package(self._package([self._site("F_STORE", self.SWC1, stock_values=bad)]))

    def test_requires_validation(self) -> None:
        store = self._site("F_STORE", self.SWC1, stock_values=[30])
        other = self._site("G_COUNT", self.ADDIU, function="synth_gate", requires=["F_STORE"])
        validate_package(self._package([store, other]))
        cases = (
            ("unknown", [store, dict(other, requires=["NOPE"])], "names sites that declare stock_values"),
            ("self", [dict(store, requires=["F_STORE"])], "its own"),
            ("duplicate", [store, dict(other, requires=["F_STORE", "F_STORE"])], "duplicate"),
            ("no stock_values", [self._site("PLAIN", self.ADDIU, function="synth_plain"),
                                 dict(other, requires=["PLAIN"])],
             "names sites that declare stock_values"),
            ("empty", [store, dict(other, requires=[])], "requires must be a non-empty list"),
            ("not a list", [store, dict(other, requires="F_STORE")], "requires must be a non-empty list"),
        )
        for name, sites, why in cases:
            with self.subTest(name):
                with self.assertRaisesRegex(EnhancementPackageError, why):
                    validate_package(self._package(sites))
        # A required site may not itself wait on another site.
        first = self._site("A_STORE", self.SW, function="synth_a", stock_values=[9])
        chained = self._site("B_STORE", self.SWC1, function="synth_b", stock_values=[30],
                             requires=["A_STORE"])
        with self.assertRaisesRegex(EnhancementPackageError, "itself declares requires"):
            validate_package(self._package([first, chained, dict(other, requires=["B_STORE"])]))
        # A data entry is not a site.
        data_site = {"id": "D_COUNT", "symbol": "synth_table", "offset": 0, "type": "s32",
                     "stock_digest": hashlib.sha256(struct.pack("<i", 21)).hexdigest(),
                     "transform": "count_x2"}
        with self.assertRaisesRegex(EnhancementPackageError, "names sites that declare stock_values"):
            validate_package(self._package([store, dict(other, requires=["D_COUNT"])],
                                           data_sites=[data_site]))
        data_file = {"id": "F_TABLE", "path": "data/param/table.csv", "sha256": "c" * 64,
                     "format": "csv", "header_row": 0, "first_data_row": 1,
                     "cells": [{"column": "Wait", "transform": "count_x2"}]}
        with self.assertRaisesRegex(EnhancementPackageError, "names sites that declare stock_values"):
            validate_package(self._package([store, dict(other, requires=["F_TABLE"])],
                                           data_files=[data_file]))

    def test_stock_values_refused_on_a_two_word_site(self) -> None:
        lui, ori = 0x3C0141F0, 0x34210000   # lui at, 0x41f0; ori at, at, 0: 30.0f
        words = {0x1000: lui, 0x1004: ori}
        pair = self._site("P_PAIR", lui, function="pair", word_count=2, transform="step_x0.5",
                          stock_digest=compute_stock_words_digest([lui, ori]))
        pkg = self._package([pair])

        def resolve(package: dict[str, Any]) -> dict[int, Any]:
            resolved: dict[int, Any] = verify_and_resolve_sites(
                package, executable_sha256="a" * 64, symbol_to_addr={"pair": 0x1000},
                read_words_fn=lambda a, n: [words[a + 4 * i] for i in range(n)] if a in words else None)
            return resolved

        self.assertEqual(sorted(resolve(pkg)), [0x1000, 0x1004])
        pkg["sites"][0]["stock_values"] = [30]
        with self.assertRaisesRegex(EnhancementPackageError, "stock_values belongs only to swc1 and sw"):
            resolve(pkg)

    # -- compile and run ---------------------------------------------------------------

    def _generate(self, tmp: Path, gate_transform: str | None = None) -> dict[str, str]:
        """Generate the store-site fixture. With ``gate_transform``, the entry also calls
        0x1400, whose first word begins each logic update and whose ``jal 0x1500`` at 0x1404 (an ease
        leaf, as gate_30hz_hold needs) is a gate site of that transform that requires F_STORE."""
        def jal(target: int) -> int:
            return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)

        words = {
            0x1000: jal(0x1100), 0x1004: 0, 0x1008: jal(0x1200), 0x100C: 0,
            0x1010: jal(0x1300), 0x1014: 0, 0x1018: 0x03E00008, 0x101C: 0,
            0x1100: self.SWC1, 0x1104: 0x03E00008, 0x1108: 0,
            0x1200: self.SW, 0x1204: 0x03E00008, 0x1208: 0,
            0x1300: self.ADDIU, 0x1304: 0x03E00008, 0x1308: 0,
        }
        symbols = {"fstore": "0x1100", "istore": "0x1200", "gcount": "0x1300"}
        if gate_transform is not None:
            words.update({0x1018: jal(0x1400), 0x101C: 0, 0x1020: 0x03E00008, 0x1024: 0,
                          0x1400: 0x24070001, 0x1404: jal(0x1500), 0x1408: 0,  # addiu a3, zero, 1
                          0x140C: 0x03E00008, 0x1410: 0,
                          # sub.s f1, f13, f12; add.s f0, f12, f1; jr ra; nop
                          0x1500: 0x460C6841, 0x1504: 0x46016000, 0x1508: 0x03E00008, 0x150C: 0})
            symbols["hgate"] = "0x1400"
        elf = _write_synthetic_mips_elf(tmp / "synth.elf", words, 0x1000)
        sites = [
            dict(self._site("F_STORE", self.SWC1, stock_values=[30, 60]), function="fstore"),
            dict(self._site("I_STORE", self.SW, stock_values=[9, -9]), function="istore"),
            dict(self._site("G_COUNT", self.ADDIU, requires=["F_STORE"]), function="gcount"),
        ]
        extra: dict[str, Any] = {}
        if gate_transform is not None:
            sites.append(dict(self._site("H_GATE", jal(0x1500), requires=["F_STORE"]),
                              function="hgate", insn_offset=4, transform=gate_transform))
            extra["update_boundary"] = {"function": "hgate", "insn_offset": 0,
                                        "stock_digest": compute_stock_words_digest([0x24070001])}
        pkg = self._package(sites, executable_sha256=hashlib.sha256(elf).hexdigest(), **extra)
        (tmp / "store.json").write_text(json.dumps(pkg), encoding="utf-8")
        (tmp / "store.symbols.json").write_text(json.dumps(symbols), encoding="utf-8")
        out_dir = tmp / "out"
        out_dir.mkdir()
        rc, output, files = TestEnhancementAotCodegenAndCache._run_codegen(
            tmp / "synth.elf", out_dir / "out.c", [f"--enhancement-package={tmp / 'store.json'}"])
        self.assertEqual(rc, 0, output)
        # 30 scales to 60, which is also a stock value: a build-time warning, not a refusal.
        self.assertIn("warning: enhancement site F_STORE: stock value 30 scales to 60, which is also a "
                      "stock value; a path that stores an already scaled value again would scale it twice",
                      output)
        self.assertEqual(output.count("warning: enhancement site"), 1, output)
        return {name: data.decode("utf-8") for name, data in files.items()}

    @staticmethod
    def _statement(chunk: str, pc: int) -> str:
        """The toggle-on and stock lines of the rewritten site at ``pc``."""
        lines = chunk.splitlines()
        marker = f"sr_begin(s, 0x{pc:08x}u"
        at = next(i for i, line in enumerate(lines)
                  if line.lstrip().startswith("else {") and marker in line)
        if not lines[at - 1].lstrip().startswith("if (nk_enhancement_on())"):
            raise AssertionError(lines[at - 1])
        return lines[at - 1] + "\n" + lines[at]

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_store_sites_check_report_and_gate_when_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            files = self._generate(tmp)
            main, chunk = files["out.c"], files["out_0.c"]
            register_all = main[main.index("void sr_register_all(void)"):]
            self.assertIn("    nk_enh_sites_start();\n", register_all[:register_all.index("\n}")])
            begin = main.index("/* Enhancement store sites. */")
            end = main.index("/* End of enhancement store sites. */")
            block = main[begin:end]
            harness = (
                "#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
                "typedef struct { uint32_t r[32]; union { float f[32]; uint32_t fi[32]; }; } CpuState;\n"
                "static uint32_t mem[4];\n"
                "#define sr_begin(s, pc, op) ((void)0)\n#define sr_end(s, a, n) ((void)0)\n"
                "#define MEM_W32_PC(a, v, pc) (mem[((a) >> 2) & 3u] = (uint32_t)(v))\n"
                "void sr_perf_shutdown(void);\n"
                f"int nk_enhancement_on(void) {{ return NK_ON; }}\n{block}\n"
                f"static void run_f(CpuState *s) {{\n{self._statement(chunk, 0x1100)}\n}}\n"
                f"static void run_i(CpuState *s) {{\n{self._statement(chunk, 0x1200)}\n}}\n"
                f"static void run_g(CpuState *s) {{\n{self._statement(chunk, 0x1300)}\n}}\n"
                "int main(int argc, char **argv) {\n"
                "    CpuState s; memset(&s, 0, sizeof s); s.r[4] = 4;\n"
                "    nk_enh_sites_start();\n"
                "    for (int i = 1; i < argc; i++) {\n"
                "        char k = argv[i][0]; long v = k == 'G' || k == 'X' ? 0 : strtol(argv[i] + 2, NULL, 0);\n"
                "        mem[1] = 0xdeadbeefu;\n"
                "        if (k == 'F') { s.fi[2] = (uint32_t)v; run_f(&s); "
                "printf(\"F %08x %08x\\n\", mem[1], s.fi[2]); }\n"
                "        if (k == 'I') { s.r[5] = (uint32_t)v; run_i(&s); "
                "printf(\"I %08x %08x\\n\", mem[1], s.r[5]); }\n"
                "        if (k == 'G') { s.r[6] = 0; run_g(&s); printf(\"G %u\\n\", s.r[6]); }\n"
                "        /* X ends as hle_display.c's headless end of run does: reports, then _Exit. */\n"
                "        if (k == 'X') { fflush(stdout); sr_perf_shutdown(); _Exit(0); }\n"
                "        fflush(stdout);\n"
                "    }\n    return 0;\n}\n")
            src = tmp / "store_harness.c"
            src.write_text(harness, encoding="utf-8")
            exes = {}
            for on in (0, 1):
                exes[on] = tmp / f"store{on}.exe"
                subprocess.run([str(GCC), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                                f"-DNK_ON={on}", f"-I{ROOT / 'src' / 'rt'}", "-o", str(exes[on]), str(src),
                                str(ROOT / "src" / "rt" / "perf.c"), "-lm"],
                               check=True, capture_output=True, text=True)

            def run(on: int, *args: str) -> subprocess.CompletedProcess[str]:
                return subprocess.run([str(exes[on]), *args], capture_output=True, text=True)

            f30, f60, f120 = (f"0x{_f32_bits_of(v):08x}" for v in (30.0, 60.0, 120.0))
            f_unreached, i_unreached = (
                f"ENHANCEMENT_DATA_SITE_NOT_REACHED: site {sid} at 0x{pc} stored no value in this run; "
                "whatever it should have rescaled stayed stock"
                for sid, pc in (("F_STORE", "00001100"), ("I_STORE", "00001200")))
            # Toggle on, a stock value: the doubled value is stored, the register is not changed.
            res = run(1, f"F:{f30}", "I:9", "I:-9")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(res.stdout.splitlines(), [
                f"F {f60[2:]} {f30[2:]}", "I 00000012 00000009", "I ffffffee fffffff7"])
            self.assertEqual(res.stderr.splitlines(), [
                "ENHANCEMENT_DATA_APPLIED: site F_STORE at 0x00001100: 30 -> 60",
                "ENHANCEMENT_DATA_APPLIED: site I_STORE at 0x00001200: 9 -> 18",
                "ENHANCEMENT_DATA_APPLIED: site I_STORE at 0x00001200: -9 -> -18",
            ])
            # A value that is not a stock value stops the run before it is stored.
            res = run(1, f"F:{f60}", f"F:0x{_f32_bits_of(1.0):08x}", f"F:{f120}")
            self.assertEqual(res.returncode, 1)
            self.assertEqual(res.stdout.splitlines(), [f"F {f120[2:]} {f60[2:]}"])
            self.assertIn("ENHANCEMENT_DATA_MISMATCH: site F_STORE at 0x00001100 stores 1 (0x3f800000), "
                          "not one of the package's stock values; the run stops here", res.stderr)
            self.assertNotIn("NOT_REACHED", res.stderr)   # the stop is the report
            res = run(1, "I:10")
            self.assertEqual(res.returncode, 1)
            self.assertIn("ENHANCEMENT_DATA_MISMATCH: site I_STORE at 0x00001200 stores 10 (0x0000000a), "
                          "not one of the package's stock values; the run stops here", res.stderr)
            self.assertEqual(res.stdout, "")
            # Toggle off: the stock statement, no check, no output.
            res = run(0, f"F:{f30}", "F:0x3f800000", "I:9", "I:10", "G")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(res.stdout.splitlines(), [
                f"F {f30[2:]} {f30[2:]}", "F 3f800000 3f800000", "I 00000009 00000009",
                "I 0000000a 0000000a", "G 30"])
            self.assertEqual(res.stderr, "")
            # Only the first 16 applications of a site are logged.
            res = run(1, *(f"F:{f30}" for _ in range(20)))
            self.assertEqual(res.returncode, 0, res.stderr)
            lines = res.stderr.splitlines()
            self.assertEqual(len(lines), 18, res.stderr)
            self.assertTrue(all(line == "ENHANCEMENT_DATA_APPLIED: site F_STORE at 0x00001100: 30 -> 60"
                                for line in lines[:16]))
            self.assertEqual(lines[16],
                             "ENHANCEMENT_DATA_APPLIED: site F_STORE: later applications are not logged")
            self.assertEqual(lines[17], i_unreached)
            # requires: stock until the required site has applied, one line per run.
            res = run(1, "G", "G", f"F:{f30}", "G")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(res.stdout.splitlines(), ["G 30", "G 30", f"F {f60[2:]} {f30[2:]}", "G 60"])
            self.assertEqual(res.stderr.splitlines(), [
                "ENHANCEMENT_SITE_GATED: site G_COUNT at 0x00001300 runs stock: F_STORE has not applied",
                "ENHANCEMENT_DATA_APPLIED: site F_STORE at 0x00001100: 30 -> 60",
                i_unreached,
            ])
            # A store site that never runs is reported at exit, one line per site; a site
            # without stock_values (G_COUNT) is not a store site and has nothing to report.
            res = run(1, "G")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(res.stderr.splitlines(), [
                "ENHANCEMENT_SITE_GATED: site G_COUNT at 0x00001300 runs stock: F_STORE has not applied",
                f_unreached, i_unreached,
            ])
            res = run(1)
            self.assertEqual(res.stderr.splitlines(), [f_unreached, i_unreached])
            self.assertEqual(run(0).stderr, "")
            # The headless end of run leaves through _Exit, which skips atexit: the runtime's
            # end-of-process list prints the report, once, and the exit status is unchanged.
            res = run(1, f"F:{f30}", "X")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(res.stdout.splitlines(), [f"F {f60[2:]} {f30[2:]}"])
            self.assertEqual(res.stderr.splitlines(), [
                "ENHANCEMENT_DATA_APPLIED: site F_STORE at 0x00001100: 30 -> 60", i_unreached])
            res = run(0, "X")
            self.assertEqual((res.returncode, res.stderr), (0, ""))

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_requires_holds_a_gated_call_stock_until_ready(self) -> None:
        """A gate_30hz or gate_30hz_hold call site with ``requires`` makes the call every
        update, as stock, until the required site has applied; then it gates."""
        for transform in ("gate_30hz", "gate_30hz_hold"):
            with self.subTest(transform=transform), tempfile.TemporaryDirectory() as tmp_dir:
                tmp = Path(tmp_dir)
                files = self._generate(tmp, gate_transform=transform)
                main, chunk = files["out.c"], files["out_0.c"]
                begin = main.index("/* Enhancement store sites. */")
                block = main[begin:main.index("/* End of enhancement store sites. */")]
                lines = chunk.splitlines()
                at = next(i for i, line in enumerate(lines) if "sr_begin(s, 0x00001404u" in line)
                gate = next(i for i in range(at, len(lines)) if "nk_enhancement_gate_30hz()) {" in lines[i])
                self.assertIn("|| !nk_enh_ready(", lines[gate])
                end = next(i for i in range(gate + 1, len(lines)) if lines[i].lstrip().startswith("}"))
                call = "\n".join(lines[at:end + 1])
                callee = re.search(r"return; (\w+)\(s\);", call)
                self.assertIsNotNone(callee, call)
                assert callee is not None
                harness = (
                    "#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
                    "typedef struct { uint32_t r[32]; union { float f[32]; uint32_t fi[32]; }; } CpuState;\n"
                    "#define sr_begin(s, pc, op) ((void)0)\n#define sr_end(s, a, n) ((void)0)\n"
                    "#define MEM_W32_PC(a, v, pc) ((void)(v))\n"
                    "#define sr_aot_return_boundary_begin(s, r, c, k) 0\n"
                    "#define sr_aot_return_boundary_end(s) 0\n#define SR_TAIL_DRAIN(s) ((void)0)\n"
                    "void sr_perf_shutdown(void);\n"
                    "int nk_enhancement_on(void) { return 1; }\n"
                    "uint32_t nk_enhancement_updates;\n"
                    "static inline int nk_enhancement_gate_30hz(void) "
                    "{ return (nk_enhancement_updates & 1u) == 0u; }\n"
                    f"{block}\n"
                    "static unsigned calls;\n"
                    f"static void {callee.group(1)}(CpuState *s) {{ (void)s; calls++; }}\n"
                    f"static void run_f(CpuState *s) {{\n{self._statement(chunk, 0x1100)}\n}}\n"
                    f"static void run_h(CpuState *s) {{\n{call}\n}}\n"
                    "int main(void) {\n"
                    "    CpuState s; memset(&s, 0, sizeof s); s.r[4] = 4;\n"
                    "    nk_enh_sites_start();\n"
                    "    for (nk_enhancement_updates = 1; nk_enhancement_updates <= 4; nk_enhancement_updates++)\n"
                    "        run_h(&s);\n"
                    "    printf(\"before %u\\n\", calls); calls = 0;\n"
                    "    s.fi[2] = 0x41f00000u; run_f(&s);\n"
                    "    for (nk_enhancement_updates = 5; nk_enhancement_updates <= 8; nk_enhancement_updates++)\n"
                    "        run_h(&s);\n"
                    "    printf(\"after %u\\n\", calls); fflush(stdout);\n"
                    "    return 0;\n}\n")
                src = tmp / "gate_harness.c"
                src.write_text(harness, encoding="utf-8")
                exe = tmp / "gate.exe"
                assert GCC is not None
                built = subprocess.run([str(GCC), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                                        "-Wno-misleading-indentation", f"-I{ROOT / 'src' / 'rt'}", "-o",
                                        str(exe), str(src), str(ROOT / "src" / "rt" / "perf.c"), "-lm"],
                                       capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                res = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertEqual(res.returncode, 0, res.stderr)
                # Stock, every update, while F_STORE has not applied; then every second update.
                self.assertEqual(res.stdout.splitlines(), ["before 4", "after 2"])
                self.assertIn("ENHANCEMENT_SITE_GATED: site H_GATE at 0x00001404 runs stock: "
                              "F_STORE has not applied", res.stderr)

    def test_every_runtime_exit_path_reaches_the_end_of_process_reports(self) -> None:
        """A store site's NOT_REACHED report runs from sr_perf_shutdown(); every _Exit/_exit
        in the runtime calls it first. HLE self-tests that do not link perf.c stub it."""
        exit_call = re.compile(r"\b_(?:Exit|exit)\(")
        report = re.compile(r"\bsr_perf_shutdown\(\);")
        # verify_harness.c is its own program (#1843): it runs a synthetic module with no
        # enhancement package and does not link perf.c, so it has no store sites to report.
        not_the_runtime = {"verify_harness.c"}
        paths = 0
        for path in sorted((ROOT / "src" / "rt").glob("*.c")):
            if path.name in not_the_runtime:
                continue
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            for n, line in enumerate(lines):
                code = line.split("//", 1)[0]
                if not exit_call.search(code) or code.lstrip().startswith(("*", "/*")):
                    continue
                paths += 1
                window = "\n".join(lines[max(0, n - 10):n + 1])
                with self.subTest(path=f"{path.name}:{n + 1}"):
                    self.assertRegex(window, report)
        self.assertGreaterEqual(paths, 12)

    def test_package_without_store_keys_emits_no_store_table(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            case = TestEnhancementAotCodegenAndCache(
                "test_gated_sites_count_logic_updates_at_the_update_boundary")
            case.setUp()
            elf = _write_synthetic_mips_elf(tmp / "synth.elf", case.words, case.entry)
            pkg, symbols = case._make_package_and_symbols(elf)
            rc, output, files = case._codegen_with(tmp, "plain", pkg, symbols)
            self.assertEqual(rc, 0, output)
            combined = b"\n".join(files.values()).decode("utf-8")
            self.assertNotIn("nk_enh_store", combined)
            self.assertNotIn("nk_enh_ready", combined)
            self.assertNotIn("Enhancement store sites", combined)


class TestHoldAndEvenStaticChecks(unittest.TestCase):
    """The resolver's static checks behind gate_30hz_hold and count_x2_even (section 9.2):
    a held callee drops nothing but its result, and an even bound compares a stored count."""

    JAL_EASE = 0x0C000800       # jal 0x2000
    NOP, JR_RA = 0x00000000, 0x03E00008
    SUB = 0x460C6841            # sub.s f1, f13, f12
    MUL = 0x460E0842            # mul.s f1, f1, f14
    ADD_F0 = 0x46016000         # add.s f0, f12, f1
    LW_A0 = 0x8CA40000          # lw a0, 0(a1)
    SLTI = 0x28830064           # slti v1, a0, 100

    def _resolve(self, words: dict[int, int], site: dict[str, Any]) -> dict[int, Any]:
        pkg = {
            "schema_version": 1, "id": "synth.hold", "display_name": "Synthetic hold and even sites",
            "version": "1.0.0", "category": "timing_frame_presentation",
            "target_title_id": "synthetic-allegrex-v1", "game_version": "1.00",
            "executable_sha256": "a" * 64, "capabilities": ["gate_30hz"],
            "rng_policy": {"mode": "subtick_30hz", "reason": "No random rolls in this fixture."},
            "update_boundary": {"function": "caller", "insn_offset": 8,
                                "stock_digest": compute_stock_words_digest([self.JR_RA])},
            "sites": [site],
        }
        resolved: dict[int, Any] = verify_and_resolve_sites(
            pkg, executable_sha256="a" * 64,
            symbol_to_addr={"caller": 0x1000, "counter": 0x1100},
            read_words_fn=lambda a, n: ([words[a + 4 * i] for i in range(n)]
                                        if all(a + 4 * i in words for i in range(n)) else None))
        return resolved

    def _hold(self, callee: list[int]) -> dict[int, Any]:
        words = {0x1000: self.JAL_EASE, 0x1004: self.NOP, 0x1008: self.JR_RA, 0x100C: self.NOP}
        words.update({0x2000 + 4 * i: w for i, w in enumerate(callee)})
        return self._resolve(words, {
            "id": "HOLD", "function": "caller", "insn_offset": 0,
            "stock_digest": compute_stock_words_digest([self.JAL_EASE]), "transform": "gate_30hz_hold"})

    def _even(self, body: list[int]) -> dict[int, Any]:
        words = {0x1100 + 4 * i: w for i, w in enumerate(body)}
        offset = 4 * body.index(self.SLTI)
        return self._resolve(words, {
            "id": "EVEN", "function": "counter", "insn_offset": offset,
            "stock_digest": compute_stock_words_digest([self.SLTI]), "transform": "count_x2_even"})

    def test_an_ease_callee_is_held(self) -> None:
        ease = [self.SUB, self.MUL, self.ADD_F0, self.JR_RA, self.NOP]
        self.assertEqual(self._hold(ease)[0x1000]["rewrite"], {"kind": "gate", "hold_f": 12})
        # A forward branch inside the body (bc1f to the return) is allowed.
        self.assertIn(0x1000, self._hold([self.SUB, 0x45000002, self.MUL, self.ADD_F0, self.JR_RA, self.NOP]))

    def test_hold_callee_refusals(self) -> None:
        cases = (
            ("store", [self.SUB, 0xE4810000, self.ADD_F0, self.JR_RA, self.NOP], "writes memory at 0x00002004"),
            ("call", [self.SUB, 0x0C000C00, self.ADD_F0, self.JR_RA, self.NOP], "jumps or calls"),
            ("jalr", [self.SUB, 0x0320F809, self.ADD_F0, self.JR_RA, self.NOP], "calls through a register"),
            ("tail jump", [self.SUB, self.ADD_F0, 0x03200008, self.NOP, self.JR_RA, self.NOP],
             "jumps through a register"),
            ("syscall", [self.SUB, 0x0000000C, self.ADD_F0, self.JR_RA, self.NOP], "traps"),
            ("store in the delay slot", [self.SUB, self.ADD_F0, self.JR_RA, 0xAC850000], "writes memory"),
            ("branch out", [self.SUB, 0x10000040, self.ADD_F0, self.JR_RA, self.NOP], "branches from 0x00002004"),
            ("f12 written first", [0x46006B06, self.ADD_F0, self.JR_RA, self.NOP],
             "does not read f12 before it writes it"),
            ("no f0", [self.SUB, 0x46016080, self.JR_RA, self.NOP], "does not write f0"),
            ("no return", [self.NOP] * 300, r"has no jr \$ra in its first 256"),
            ("unreadable", [self.SUB, self.ADD_F0], "cannot be read at 0x00002008"),
            ("no delay slot", [self.SUB, self.ADD_F0, self.JR_RA], "cannot be read at its return's delay slot"),
            ("ra moved", [self.SUB, self.ADD_F0, 0x27FF0004, self.JR_RA, self.NOP], r"writes \$ra at 0x00002008"),
            ("ra set", [self.SUB, self.ADD_F0, 0x3C1F0800, self.JR_RA, self.NOP], r"writes \$ra"),
            ("ra loaded", [self.SUB, self.ADD_F0, 0x8FBF0000, self.JR_RA, self.NOP], r"writes \$ra"),
            ("sp moved", [self.SUB, 0x27BDFFF0, self.ADD_F0, self.JR_RA, self.NOP], r"writes \$sp"),
            ("fcsr", [self.SUB, 0x44C0F800, self.ADD_F0, self.JR_RA, self.NOP], "writes an FPU control register"),
            ("unknown word", [self.SUB, 0x78000000, self.ADD_F0, self.JR_RA, self.NOP], "cannot read"),
            # beq to the return's delay slot: the slot runs, then execution falls through to the sw.
            ("branch to the return's slot", [self.SUB, 0x10000003, self.NOP, self.ADD_F0, self.JR_RA, self.NOP,
                                             0xAC850000, self.JR_RA, self.NOP],
             "branches from 0x00002004 to 0x00002014, outside the body up to its jr"),
        )
        for name, callee, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._hold(callee)

    def test_even_counter_is_loaded(self) -> None:
        self.assertEqual(self._even([self.LW_A0, self.SLTI, self.JR_RA, self.NOP])[0x1104]["rewrite"],
                         {"kind": "word_even", "word": 0x288300C8, "reg": 4, "rt": 3})
        # Unrelated instructions between the load and the compare are allowed.
        self.assertIn(0x1108, self._even([self.LW_A0, 0x00031080, self.SLTI, self.JR_RA, self.NOP]))
        # A plain branch's delay slot runs on both paths, so a load there reaches the compare.
        self.assertIn(0x1108, self._even([0x14400002, self.LW_A0, self.SLTI, self.JR_RA, self.NOP]))

    def test_even_counter_refusals(self) -> None:
        cases = (
            ("computed", [0x24840001, self.SLTI, self.JR_RA, self.NOP], r"counter \$4 is computed at 0x00001100"),
            ("at the entry", [self.SLTI, self.JR_RA, self.NOP], "not loaded from memory in the straight-line"),
            ("call between", [self.LW_A0, 0x0C000C00, self.NOP, self.SLTI, self.JR_RA, self.NOP],
             "not loaded from memory in the straight-line"),
            ("branch between", [self.LW_A0, 0x10000002, self.NOP, self.SLTI, self.JR_RA, self.NOP],
             "not loaded from memory in the straight-line"),
            ("unknown word", [self.LW_A0, 0x78000000, self.SLTI, self.JR_RA, self.NOP], "cannot tell whether"),
            ("too far", [self.LW_A0] + [self.NOP] * 16 + [self.SLTI, self.JR_RA, self.NOP],
             "not loaded from memory in the straight-line"),
            # The callee runs between a call's delay slot and the compare.
            ("call's slot", [0x0C000C00, self.LW_A0, self.SLTI, self.JR_RA, self.NOP],
             "loaded at 0x00001104, in the delay slot of 0x0c000c00"),
            ("jalr's slot", [0x0320F809, self.LW_A0, self.SLTI, self.JR_RA, self.NOP], "in the delay slot of"),
            # bnel skips its slot on the fall-through path to the compare.
            ("branch-likely's slot", [0x54400002, self.LW_A0, self.SLTI, self.JR_RA, self.NOP],
             "in the delay slot of 0x54400002"),
        )
        for name, body, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._even(body)


def _r(rs: int, rt: int, rd: int, sa: int, fn: int) -> int:
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn


def _beq(rs: int, rt: int, pc: int, to: int) -> int:
    return _i(0x04, rs, rt, (to - pc - 4) >> 2)   # _i, _jal and _move are defined above


ZERO, AT, V0, V1, A0, A1, T0, S0, S1, SP, RA = 0, 1, 2, 3, 4, 5, 8, 16, 17, 29, 31
NOP, JR_RA = 0, 0x03E00008


def _addiu(rt: int, rs: int, imm: int) -> int:
    return _i(0x09, rs, rt, imm)


def _lw(rt: int, off: int, base: int) -> int:
    return _i(0x23, base, rt, off)


def _sw(rt: int, off: int, base: int) -> int:
    return _i(0x2B, base, rt, off)


def _lwc1(ft: int, off: int, base: int) -> int:
    return _i(0x31, base, ft, off)


def _swc1(ft: int, off: int, base: int) -> int:
    return _i(0x39, base, ft, off)


def _add_s(fd: int, fs: int, ft: int) -> int:
    return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6)


class TestAltGateStaticChecks(unittest.TestCase):
    """The resolver's static proof behind gate_30hz_alt (section 9.2), on a synthetic guest: a
    caller makes the gated call to TARGET, a three-way table switch whose arms all reach a tail
    that calls TICK1(a0) and TICK2(a0); TICK2 calls LEAF with f12 set."""

    CALLER, TARGET, TABLE, TICK1, TICK2, LEAF, OTHER, END = (
        0x1000, 0x2000, 0x3000, 0x4000, 0x4100, 0x4200, 0x4300, 0x4400)
    SITE = 0x100C

    def _caller(self) -> list[int]:
        return [_addiu(SP, SP, -8), _sw(RA, 4, SP), _addiu(A0, ZERO, 0x40), _jal(self.TARGET), NOP,
                _i(0x0F, 0, V0, 1), _jal(self.OTHER), NOP, _lw(RA, 4, SP), JR_RA, _addiu(SP, SP, 8)]

    def _target(self, tail: list[int] | None = None, arm1: list[int] | None = None) -> list[int]:
        t = self.TARGET
        tail_at = t + 0x50
        body = [_addiu(SP, SP, -16), _sw(RA, 12, SP), _sw(S1, 8, SP), _lw(V0, 0x1B0, A0),
                _i(0x0B, V0, AT, 3), _beq(AT, ZERO, t + 0x14, tail_at), _move(S1, A0),
                _i(0x0F, 0, V1, 0), _r(0, V0, V0, 2, 0x00), _addiu(V1, V1, self.TABLE),
                _r(V0, V1, V0, 0, 0x21), _lw(V0, 0, V0), _r(V0, 0, 0, 0, 0x08), NOP,
                # arm 0 at +0x38: branches to the tail
                _addiu(V0, ZERO, 1), _beq(ZERO, ZERO, t + 0x3C, tail_at), _sw(V0, 0x1B4, S1)]
        body += arm1 if arm1 is not None else [_addiu(V0, ZERO, 2), _sw(V0, 0x1B4, S1)]  # arm 1 at +0x44
        body += [NOP]  # arm 2 at +0x4c falls into the tail
        assert len(body) == 0x50 // 4, len(body)
        body += tail if tail is not None else [
            _move(A0, S1), _jal(self.TICK1), NOP, _jal(self.TICK2), _move(A0, S1),
            _lw(S1, 8, SP), _lw(RA, 12, SP), JR_RA, _addiu(SP, SP, 16)]
        return body

    def _tick1(self) -> list[int]:
        return [_lwc1(0, 0x108, A0), _lwc1(1, 0x10C, A0), _add_s(0, 0, 1), JR_RA, _swc1(0, 0x108, A0)]

    def _tick2(self, call: int | None = None) -> list[int]:
        return [_addiu(SP, SP, -8), _sw(RA, 4, SP), _sw(S0, 0, SP), _lwc1(12, 0xCC, A0),
                call if call is not None else _jal(self.LEAF), _move(S0, A0), _swc1(0, 0xCC, S0),
                _lw(S0, 0, SP), _lw(RA, 4, SP), JR_RA, _addiu(SP, SP, 8)]

    def _leaf(self) -> list[int]:
        return [_add_s(0, 12, 12), JR_RA, NOP]

    def _words(self, **parts: list[int]) -> dict[int, int]:
        layout = {"caller": self.CALLER, "target": self.TARGET, "tick1": self.TICK1, "tick2": self.TICK2,
                  "leaf": self.LEAF, "other": self.OTHER}
        words: dict[int, int] = {}
        for name, base in layout.items():
            code = parts.get(name) or getattr(self, f"_{name}", lambda: [JR_RA, NOP])()
            words.update({base + 4 * i: w for i, w in enumerate(code)})
        for addr in range(self.CALLER, self.END, 4):
            words.setdefault(addr, NOP)  # alignment padding after each return
        table = parts.get("table", [self.TARGET + 0x38, self.TARGET + 0x44, self.TARGET + 0x4C])
        words.update({self.TABLE + 4 * i: w for i, w in enumerate(table)})
        return words

    def _other(self) -> list[int]:
        return [JR_RA, NOP]

    def _symbols(self) -> dict[str, int]:
        return {"caller": self.CALLER, "target": self.TARGET, "tick1": self.TICK1, "tick2": self.TICK2,
                "leaf": self.LEAF, "other": self.OTHER}

    def _package(self, words: dict[int, int], sha256: str, *, alternates: list[str] | None = None,
                 extra_sites: list[dict[str, Any]] | None = None) -> dict[str, Any]:
        site = {"id": "ALT", "function": "caller", "insn_offset": self.SITE - self.CALLER,
                "stock_digest": compute_stock_words_digest([words[self.SITE]]), "transform": "gate_30hz_alt",
                "alternates": alternates if alternates is not None else ["tick1", "tick2"]}
        return {
            "schema_version": 1, "id": "synth.alt", "display_name": "Synthetic alternate gate",
            "version": "1.0.0", "category": "timing_frame_presentation",
            "target_title_id": "synthetic-allegrex-v1", "game_version": "1.00",
            "executable_sha256": sha256, "capabilities": ["gate_30hz"],
            "rng_policy": {"mode": "subtick_30hz", "reason": "No random rolls in this fixture."},
            "update_boundary": {"function": "caller", "insn_offset": 0,
                                "stock_digest": compute_stock_words_digest([words[self.CALLER]])},
            "sites": [site] + (extra_sites or []),
        }

    def _resolve(self, words: dict[int, int], *, alternates: list[str] | None = None,
                 extra_sites: list[dict[str, Any]] | None = None) -> dict[int, Any]:
        pkg = self._package(words, "a" * 64, alternates=alternates, extra_sites=extra_sites)
        symbols = self._symbols()
        resolved: dict[int, Any] = verify_and_resolve_sites(
            pkg, executable_sha256="a" * 64, symbol_to_addr=symbols,
            read_words_fn=lambda a, n: ([words[a + 4 * i] for i in range(n)]
                                        if all(a + 4 * i in words for i in range(n)) else None),
            known_functions=[self.CALLER, self.TARGET, self.TABLE, self.TICK1, self.TICK2, self.LEAF,
                             self.OTHER, self.END])
        return resolved

    def test_the_proof_accepts_the_switch_and_its_tail(self) -> None:
        resolved = self._resolve(self._words())
        self.assertEqual(resolved[self.SITE]["rewrite"], {"kind": "gate", "alternates": [self.TICK1, self.TICK2]})

    def test_schema_refusals(self) -> None:
        cases: tuple[tuple[str, list[str], str], ...] = (
            ("none", [], "a list of 1 to 4 function symbols"),
            ("too many", ["tick1", "tick2", "leaf", "other", "target"], "a list of 1 to 4"),
            ("twice", ["tick1", "tick1"], "lists a function twice"),
            ("unknown", ["tick1", "nowhere"], "'nowhere' is not in the symbol map"),
            ("literal", ["0x00004000"], "alternate symbol"),
        )
        for name, alternates, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(self._words(), alternates=alternates)
        bad = {"id": "G", "function": "caller", "insn_offset": 24, "transform": "gate_30hz",
               "stock_digest": compute_stock_words_digest([_jal(self.OTHER)]), "alternates": ["tick1"]}
        with self.assertRaisesRegex(EnhancementPackageError, "alternates belongs only to gate_30hz_alt"):
            self._resolve(self._words(), extra_sites=[bad])
        jalr = self._caller()
        jalr[3] = _r(25, 0, RA, 0, 0x09)
        with self.assertRaisesRegex(EnhancementPackageError, "gate_30hz_alt takes a jal"):
            self._resolve(self._words(caller=jalr))

    def test_target_refusals(self) -> None:
        early_return = [JR_RA, _addiu(SP, SP, 16)]
        cases = (
            ("an arm returns early", {"target": self._target(arm1=early_return)},
             "has a path that returns without calling alternate 0x00004000"),
            ("a tick is called twice", {"target": self._target(tail=[
                _move(A0, S1), _jal(self.TICK1), NOP, _jal(self.TICK1), _move(A0, S1), _jal(self.TICK2),
                _move(A0, S1), _lw(S1, 8, SP), _lw(RA, 12, SP), JR_RA, _addiu(SP, SP, 16)])},
             "calls alternate 0x00004000 directly 2 times"),
            ("a0 not the entry a0", {"target": self._target(tail=[
                _move(A0, S0), _jal(self.TICK1), NOP, _jal(self.TICK2), _move(A0, S1),
                _lw(S1, 8, SP), _lw(RA, 12, SP), JR_RA, _addiu(SP, SP, 16)])},
             "does not pass alternate 0x00004000 its own entry"),
            ("s1 rewritten in an arm", {"target": self._target(arm1=[_addiu(S1, S1, 4), NOP])},
             "does not pass alternate 0x00004000 its own entry"),
            ("a0 lost across the first tick", {"target": self._target(tail=[
                _move(A0, S1), _jal(self.TICK1), NOP, _jal(self.TICK2), NOP,
                _lw(S1, 8, SP), _lw(RA, 12, SP), JR_RA, _addiu(SP, SP, 16)])},
             "does not pass alternate 0x00004100 its own entry"),
            ("a table arm in another function", {"table": [self.TARGET + 0x38, self.TARGET + 0x44, self.OTHER]},
             "has a path that returns without calling alternate 0x00004000"),
            ("a table entry in a delay slot", {"table": [self.TARGET + 0x38, self.TARGET + 0x40, self.TARGET + 0x4C]},
             "branches into the delay slot at 0x00002040"),
        )
        for name, parts, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(self._words(**parts))
        with self.subTest("order"), self.assertRaisesRegex(EnhancementPackageError, "in the order the target calls"):
            self._resolve(self._words(), alternates=["tick2", "tick1"])
        unbounded = self._target()
        unbounded[4] = NOP  # no sltiu bound before the jr
        with self.subTest("no bound"), self.assertRaisesRegex(EnhancementPackageError, "no bounded table switch"):
            self._resolve(self._words(target=unbounded))
        with self.subTest("the target itself"), self.assertRaisesRegex(EnhancementPackageError, "gated function itself"):
            self._resolve(self._words(), alternates=["target"])

    def test_caller_refusals(self) -> None:
        reads_v0 = self._caller()
        reads_v0[5] = _addiu(V1, V0, 1)
        returns = [_addiu(SP, SP, -8), _sw(RA, 4, SP), _addiu(A0, ZERO, 0x40), _jal(self.TARGET), NOP,
                   _lw(RA, 4, SP), JR_RA, _addiu(SP, SP, 8)]
        branch_read = self._caller()
        branch_read[5:5] = [_beq(V0, ZERO, self.SITE + 8, self.SITE + 0x14), NOP]
        for name, caller, why in (("reads v0", reads_v0, r"reads \$v0 at 0x00001014"),
                                  ("returns", returns, r"returns after the gated call without writing \$v0"),
                                  ("branches on v0", branch_read, r"reads \$v0 at 0x00001014")):
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(self._words(caller=caller))

    def test_a_register_call_below_the_alternate_follows_the_abi(self) -> None:
        # The documented limit (section 9.2): below the alternate's own body, a call through a
        # register is taken to read only the arguments its caller set.
        leaf = [_addiu(SP, SP, -8), _sw(RA, 4, SP), _r(25, 0, RA, 0, 0x09), NOP, _lw(RA, 4, SP), JR_RA,
                _addiu(SP, SP, 8)]
        self.assertIn(self.SITE, self._resolve(self._words(leaf=leaf)))

    def test_alternate_refusals(self) -> None:
        cases = (
            ("reads a1", {"tick1": [_lw(V0, 0, A1), JR_RA, NOP]}, r"may read \$a1 as its caller left them"),
            ("its callee reads t0", {"leaf": [_add_s(0, 12, 12), _sw(T0, 0, A0), JR_RA, NOP]},
             r"0x00004100 may read \$t0"),
            ("its callee reads f13", {"leaf": [_add_s(0, 12, 13), JR_RA, NOP]}, "may read f13"),
            ("two calls down", {"leaf": [_addiu(SP, SP, -8), _sw(RA, 4, SP), _jal(self.OTHER), NOP,
                                         _lw(RA, 4, SP), JR_RA, _addiu(SP, SP, 8)],
                                "other": [_sw(T0, 0, A0), JR_RA, NOP]}, r"0x00004100 may read \$t0"),
            ("calls through a register", {"tick2": self._tick2(call=_r(25, 0, RA, 0, 0x09))},
             "calls through a register at 0x00004110"),
            ("re-enters the target", {"leaf": [_addiu(SP, SP, -8), _sw(RA, 4, SP), _jal(self.TARGET), NOP,
                                               _lw(RA, 4, SP), JR_RA, _addiu(SP, SP, 8)]},
             "reaches 0x00002000, the gated function"),
        )
        for name, parts, why in cases:
            with self.subTest(name), self.assertRaisesRegex(EnhancementPackageError, why):
                self._resolve(self._words(**parts))
        gated = {"id": "G", "function": "tick1", "insn_offset": 0, "transform": "gate_30hz",
                 "stock_digest": compute_stock_words_digest([_add_s(0, 0, 0)])}
        with self.subTest("holds a gated site"), self.assertRaisesRegex(
                EnhancementPackageError, "which holds gated site G"):
            self._resolve(self._words(tick1=[_add_s(0, 0, 0)] + self._tick1()[1:]), extra_sites=[gated])

    def _run_target(self) -> list[int]:
        # A straight-line update: its own work (a count at a0+0x1b0), then tick1(a0) and tick2(a0).
        t1, t2 = self.TICK1, self.TICK2
        return [_addiu(SP, SP, -16), _sw(RA, 12, SP), _sw(S1, 8, SP), _move(S1, A0), _lw(V0, 0x1B0, A0),
                _addiu(V0, V0, 1), _sw(V0, 0x1B0, S1), _move(A0, S1), _jal(t1), NOP, _jal(t2), _move(A0, S1),
                _lw(S1, 8, SP), _lw(RA, 12, SP), JR_RA, _addiu(SP, SP, 16)]

    def _site_statement(self, tmp: Path, name: str, with_package: bool) -> str:
        """Generate the fixture with the package (or without, the Authentic build) and return the
        site's statement: from the jal's trace hook to the end of its call."""
        words = self._words(target=self._run_target(), table=[])
        elf = _write_synthetic_mips_elf(tmp / "alt.elf", words, self.CALLER)
        args = []
        if with_package:
            pkg_path = tmp / f"{name}.json"
            pkg_path.write_text(json.dumps(self._package(words, hashlib.sha256(elf).hexdigest())), encoding="utf-8")
            (tmp / f"{name}.symbols.json").write_text(
                json.dumps({k: f"0x{v:08x}" for k, v in self._symbols().items()}), encoding="utf-8")
            args = [f"--enhancement-package={pkg_path}"]
        out_dir = tmp / name
        out_dir.mkdir()
        rc, output, files = TestEnhancementAotCodegenAndCache._run_codegen(tmp / "alt.elf", out_dir / "out.c", args)
        self.assertEqual(rc, 0, output)
        lines = b"\n".join(files.values()).decode("utf-8").splitlines()
        at = next(i for i, line in enumerate(lines) if f"sr_begin(s, 0x{self.SITE:08x}u" in line)
        if with_package:
            end = next(i for i in range(at, len(lines)) if lines[i] == "    } }")
        else:
            end = next(i for i in range(at, len(lines)) if "sr_aot_return_boundary_end" in lines[i])
        return "\n".join(lines[at:end + 1])

    @unittest.skipUnless(GCC, "needs a host C compiler")
    def test_gate_30hz_alt_runs_its_alternates_on_the_skipped_updates(self) -> None:
        """Compile the site statement and run it over a schedule of logic updates.

        Toggle on, the site makes the stock call in an update unless it made the stock call in the
        update just before; then it calls tick1 and tick2 in its place, each with the site's $a0 and
        $ra = site + 8, and a second visit in one update repeats that update's choice. Toggle off,
        it prints what the Authentic statement prints, byte for byte.
        """
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            alt = self._site_statement(tmp, "alt", with_package=True)
            stock = self._site_statement(tmp, "authentic", with_package=False)
            # Toggle off, the site runs the Authentic call statement.
            stock_call = stock.splitlines()[-1]
            self.assertIn(stock_call, alt)
            # Visits per update: a gap after an alternate update (4, 5), two visits in one
            # update (7), and a gap after a stock update (9).
            visits = [1, 1, 1, 1, 0, 0, 1, 2, 1, 0, 1, 1, 1, 1]

            def program(name: str, statement: str) -> list[str]:
                callees = re.findall(r"(\w+)\(s\); SR_TAIL_DRAIN", statement)
                self.assertTrue(callees, statement)
                tags = ["target", "tick1", "tick2"]
                stubs = "".join(
                    f"static void {fn}(CpuState *s) {{ printf(\"  {tags[k]} a0=%x ra=%x\\n\", "
                    f"(unsigned)s->r[4], (unsigned)s->r[31]); s->r[4] = 0x{0xD0 + k:x}u; "
                    f"s->r[31] = 0x{0xE0 + k:x}u; s->r[2] = {k}u; }}\n"
                    for k, fn in enumerate(dict.fromkeys(callees)))
                harness = tmp / f"{name}.c"
                harness.write_text(
                    "#include \"recomp.h\"\n"
                    "/* The real CpuState and trace hooks; the callees are stubs that print what they\n"
                    " * are given and clobber $a0 and $ra, and only the return boundary is stubbed. */\n"
                    "#undef SR_TAIL_DRAIN\n#define SR_TAIL_DRAIN(s) ((void)0)\n"
                    "#define sr_aot_return_boundary_begin(s, r, c, k) 0\n"
                    "#define sr_aot_return_boundary_end(s) 0\n"
                    "static int on;\nint nk_enhancement_on(void) { return on; }\n"
                    "uint32_t nk_enhancement_updates;\n"
                    f"{stubs}"
                    f"static void site(CpuState *s) {{\n{statement}\n}}\n"
                    f"static const int visits[] = {{{', '.join(map(str, visits))}}};\n"
                    "int main(void) {\n"
                    "    CpuState st; CpuState *s = &st; memset(&st, 0, sizeof st);\n"
                    "    for (on = 0; on < 2; on++) {\n"
                    "        nk_enhancement_updates = on ? 100u : 0u;\n"
                    "        for (int u = 0; u < (int)(sizeof visits / sizeof visits[0]); u++) {\n"
                    "            nk_enhancement_updates++;  /* also while off: the toggle alone keeps it stock */\n"
                    "            for (int v = 0; v < visits[u]; v++) {\n"
                    "                printf(\"%s %d %d\\n\", on ? \"on\" : \"off\", u, v);\n"
                    "                s->r[4] = 0x40u + (uint32_t)u; s->r[31] = 0u; site(s);\n"
                    "            }\n"
                    "        }\n"
                    "    }\n"
                    "    return 0;\n"
                    "}\n",
                    encoding="utf-8",
                )
                exe = tmp / f"{name}.exe"
                assert GCC is not None, "skipUnless(GCC) runs this test only with a compiler"
                built = subprocess.run([GCC, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                                        "-Wno-misleading-indentation", "-Wno-unused-variable",
                                        f"-I{ROOT / 'src' / 'rt'}", "-o", str(exe), str(harness)],
                                       capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                run = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
                return run.stdout.splitlines()

            out = program("alt", alt)
            authentic = program("authentic", stock)
            ra = f"{self.SITE + 8:x}"

            def expect(mode: str, choices: str) -> list[str]:
                lines: list[str] = []
                k = 0
                for u, n in enumerate(visits):
                    for v in range(n):
                        lines.append(f"{mode} {u} {v}")
                        a0 = f"{0x40 + u:x}"
                        if choices[k] == "S":
                            lines.append(f"  target a0={a0} ra={ra}")
                        else:
                            lines += [f"  tick1 a0={a0} ra={ra}", f"  tick2 a0={a0} ra={ra}"]
                        k += 1
                return lines

            def part(lines: list[str], mode: str) -> list[str]:
                keep, kept = False, []
                for line in lines:
                    if not line.startswith("  "):
                        keep = line.startswith(mode + " ")
                    if keep:
                        kept.append(line)
                return kept

            # Section 11.2, item 1: toggle off is the Authentic statement's output, byte for byte.
            self.assertEqual(part(out, "off"), part(authentic, "off"))
            self.assertEqual(part(out, "off"), expect("off", "S" * sum(visits)))
            # Toggle on, per visit: updates 0-3 alternate; after the gap at 4-5 update 6 is stock;
            # update 7's two visits both take the alternates; after the gap at 9, update 10 is stock.
            self.assertEqual(part(out, "on"), expect("on", "SASA" "SAAS" "SASA"))


if __name__ == "__main__":
    unittest.main()
