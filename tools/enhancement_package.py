#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Validate and verify enhancement package declarations.

Enhancement packages declare optional host capabilities (e.g. presentation
overlays, asset replacements, projection overrides) layered strictly above the
untouched authentic execution baseline.

Following the repository convention of tools/title_manifest.py, this module
provides a self-contained normative hand-written validator with zero third-party
dependencies. The JSON schema in assets/enhancement_package.schema.json serves as
an editor and review aid.

Contract rules enforced:
- Schema version must be 1.
- All required fields (schema_version, id, display_name, version, category,
  target_title_id, capabilities) must be present and well-formed.
- Category must be an explicitly recognized capability category.
- Hook targets and capability names must be symbolic identifiers; raw guest address
  literals (0x-hex, decompiler-style sub_/loc_/func_ stems, bare 6-8 digit hex strings
  including all-letter ones like 'deadbeef', and numeric addresses) are strictly forbidden.
- Hooks contain symbol and type only.
- Undeclared / unknown properties fail closed.
"""

from __future__ import annotations

import argparse
from collections.abc import Mapping, Sequence
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys
from typing import Any, NoReturn

SUPPORTED_SCHEMA_VERSION = 1

AOT_ENHANCEMENT_PACKAGE_EPOCH = "aot-enhancement-package-v1"

VALID_CATEGORIES = frozenset({
    "asset_replacement",
    "presentation_only",
    "guest_memory_code_hook",
    "save_data_modification",
    "camera_projection_override",
    "timing_frame_presentation",
})

HOOK_TYPES = frozenset({
    "pre_call",
    "post_call",
    "replace",
    "notify",
})

SITE_TRANSFORMS = frozenset({
    "count_x2",
    "step_x0.5",
    "decay_k_to_1-sqrt(1-k)",
    "probability_per_update_x0.5",
    "gate_30hz",
    "vblank_wait_2_to_1",
    "retention_m_to_sqrt(m)",
})

RNG_POLICY_MODES = frozenset({
    "rescale_probability",
    "subtick_30hz",
})

ROOT_KEYS = frozenset({
    "schema_version",
    "id",
    "display_name",
    "version",
    "category",
    "target_title_id",
    "min_runtime_version",
    "capabilities",
    "hooks",
    "game_version",
    "executable_sha256",
    "rng_policy",
    "sites",
    "update_boundary",
    "data_sites",
    "data_files",
})

REQUIRED_ROOT_KEYS = frozenset({
    "schema_version",
    "id",
    "display_name",
    "version",
    "category",
    "target_title_id",
    "capabilities",
})

TIMING_PACKAGE_KEYS = frozenset({
    "game_version",
    "executable_sha256",
    "rng_policy",
    "sites",
})

# Timing-package fields that may be left out.
OPTIONAL_TIMING_PACKAGE_KEYS = frozenset({
    "update_boundary",
    "data_sites",
    "data_files",
})

# A data site: constants in the executable's loaded data, rescaled once at load.
DATA_SITE_KEYS = frozenset({
    "id",
    "symbol",
    "offset",
    "type",
    "count",
    "stock_digest",
    "transform",
})

# Element types a data site may name: (size in bytes, struct format).
DATA_TYPES = {
    "u8": (1, "<B"),
    "s8": (1, "<b"),
    "u16": (2, "<H"),
    "s16": (2, "<h"),
    "u32": (4, "<I"),
    "s32": (4, "<i"),
    "f32": (4, "<f"),
}

# Transforms a data value or a data-file cell may declare: the operand scalings of
# section 9.2. Gating and the vblank wait act on code, not on data.
DATA_TRANSFORMS = frozenset({
    "count_x2",
    "step_x0.5",
    "probability_per_update_x0.5",
    "decay_k_to_1-sqrt(1-k)",
    "retention_m_to_sqrt(m)",
})

# A data file: a file the title reads from its disc, rewritten when it is opened.
DATA_FILE_KEYS = frozenset({
    "id",
    "path",
    "sha256",
    "format",
    "header_row",
    "first_data_row",
    "cells",
})

DATA_FILE_FORMATS = frozenset({
    "csv",
})

CSV_CELL_KEYS = frozenset({
    "column",
    "rows",
    "transform",
})

HOOK_KEYS = frozenset({
    "symbol",
    "type",
})

RNG_POLICY_KEYS = frozenset({
    "mode",
    "reason",
})

SITE_KEYS = frozenset({
    "id",
    "function",
    "insn_offset",
    "stock_digest",
    "word_count",
    "transform",
})

# The instruction where one logic update of the title begins: it runs exactly once per update.
UPDATE_BOUNDARY_KEYS = frozenset({
    "function",
    "insn_offset",
    "stock_digest",
})

ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
SEMVER_RE = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$"
)
MIN_RUNTIME_VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
TARGET_TITLE_ID_RE = re.compile(r"^([a-zA-Z0-9][a-zA-Z0-9._-]{0,63}|\*)$")
GAME_VERSION_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._-]{0,31}$")
SHA256_HEX_RE = re.compile(r"^[0-9a-f]{64}$")
CAPABILITY_RE = re.compile(r"^[a-z][a-z0-9_]{1,63}$")
SYMBOL_RE = re.compile(r"^[a-zA-Z_][a-zA-Z0-9_]{0,127}$")
SITE_ID_RE = re.compile(r"^[a-zA-Z_][a-zA-Z0-9_.-]{0,63}$")
# A disc path relative to the disc root: forward slashes, no empty, '.' or '..' part.
DATA_FILE_PATH_RE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]*(?:/[A-Za-z0-9_][A-Za-z0-9_.-]*)*$")
CSV_COLUMN_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,63}$")

HEX_ADDRESS_RE = re.compile(r"^0[xX][0-9a-fA-F]+$")
DECOMP_ADDRESS_RE = re.compile(r"^(?:sub|loc|func|fun)_[0-9a-fA-F]+$", re.IGNORECASE)
BARE_HEX_RE = re.compile(r"^[0-9a-fA-F]{6,8}$")


class EnhancementPackageError(ValueError):
    """Raised when an enhancement package fails contract validation."""


def fail(path: str, message: str) -> NoReturn:
    """Raise an EnhancementPackageError formatted with failure path."""
    raise EnhancementPackageError(f"{path}: {message}")


def is_address_literal(value: str) -> bool:
    """True when value represents a raw address literal or decompiler symbol.

    Rejects:
    - 0x-prefixed hex literals (e.g. 0x08804000, 0xdeadbeef)
    - Decompiler-generated address symbols (e.g. sub_08804000, loc_08804000, func_08804000)
    - Bare 6-8 digit hex strings (e.g. 08804000, 123456, deadbeef, cafebabe)
    """
    return bool(
        HEX_ADDRESS_RE.match(value)
        or DECOMP_ADDRESS_RE.match(value)
        or BARE_HEX_RE.match(value)
    )


def check_not_address_literal(value: Any, path: str, role: str) -> str:
    """Validate that value is a symbolic identifier and not an address literal."""
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        fail(
            path,
            f"Address-literal {role} rejected: expected symbolic name, "
            f"got numeric address {value!r}",
        )
    if not isinstance(value, str):
        fail(path, f"{role} must be a string, got {type(value).__name__}")
    if value != value.strip():
        fail(path, f"{role} must not have leading or trailing whitespace")
    if not value:
        fail(path, f"{role} must not be empty")
    if is_address_literal(value):
        fail(
            path,
            f"Address-literal {role} rejected: symbolic name required, "
            f"found address literal or decompiler symbol {value!r}",
        )
    return value


def load_package(path: Path | str) -> dict[str, Any]:
    """Load an enhancement package JSON file."""
    package_path = Path(path)
    if not package_path.is_file():
        raise EnhancementPackageError(f"Package file not found: {package_path}")
    try:
        with package_path.open("r", encoding="utf-8") as f:
            data = json.load(f)
    except json.JSONDecodeError as exc:
        raise EnhancementPackageError(f"Failed to parse JSON in {package_path}: {exc}") from exc
    if not isinstance(data, dict):
        raise EnhancementPackageError(f"Package root must be a JSON object, got {type(data).__name__}")
    return data


def load_and_validate_package(path: Path | str) -> dict[str, Any]:
    """Load an enhancement package JSON file and perform full normative validation."""
    return validate_package(load_package(path))


def _check_instruction_location(obj: Mapping[str, Any], path: str, role: str) -> None:
    """Check the symbolic function, 4-byte-aligned insn_offset and stock_digest of one instruction."""
    fn_name = obj["function"]
    fn_path = f"{path}.function"
    check_not_address_literal(fn_name, fn_path, f"{role} function")
    if not SYMBOL_RE.match(fn_name):
        fail(fn_path, f"{role} function {fn_name!r} does not match required pattern {SYMBOL_RE.pattern}")

    insn_offset = obj["insn_offset"]
    if (
        not isinstance(insn_offset, int)
        or isinstance(insn_offset, bool)
        or insn_offset < 0
        or insn_offset > 65536
        or (insn_offset % 4) != 0
    ):
        fail(
            f"{path}.insn_offset",
            f"insn_offset must be a 4-byte-aligned integer in [0, 65536], got {insn_offset!r}",
        )

    stock_digest = obj["stock_digest"]
    if not isinstance(stock_digest, str) or not SHA256_HEX_RE.match(stock_digest):
        fail(
            f"{path}.stock_digest",
            f"stock_digest {stock_digest!r} must be a 64-character lowercase hex SHA-256 digest",
        )


def validate_package(data: Any) -> dict[str, Any]:
    """Validate an enhancement package declaration dictionary.

    Performs full normative hand-written validation without third-party dependencies.

    Args:
        data: Parsed package JSON data.

    Returns:
        The validated package dictionary.

    Raises:
        EnhancementPackageError: If validation fails for any reason.
    """
    if not isinstance(data, dict):
        fail("$", f"Package declaration must be an object, got {type(data).__name__}")

    # Check unknown root properties (fail-closed)
    unknown = sorted(set(data.keys()) - ROOT_KEYS)
    if unknown:
        fail("$", f"Unknown properties forbidden: {unknown}")

    # Check schema_version first
    if "schema_version" not in data:
        fail("$", "Missing required property 'schema_version'")
    schema_ver = data["schema_version"]
    if not isinstance(schema_ver, int) or isinstance(schema_ver, bool) or schema_ver != SUPPORTED_SCHEMA_VERSION:
        fail(
            "schema_version",
            f"Incompatible schema version: expected {SUPPORTED_SCHEMA_VERSION}, got {schema_ver!r}",
        )

    # Check all remaining required root properties
    for req_field in ("id", "display_name", "version", "category", "target_title_id", "capabilities"):
        if req_field not in data:
            fail("$", f"Missing required property {req_field!r}")

    # Validate id
    pkg_id = data["id"]
    if not isinstance(pkg_id, str):
        fail("id", f"id must be a string, got {type(pkg_id).__name__}")
    if pkg_id != pkg_id.strip():
        fail("id", "id must not have leading or trailing whitespace")
    if not ID_RE.match(pkg_id):
        fail("id", f"id {pkg_id!r} does not match required pattern {ID_RE.pattern}")

    # Validate display_name
    name = data["display_name"]
    if not isinstance(name, str):
        fail("display_name", f"display_name must be a string, got {type(name).__name__}")
    if name != name.strip():
        fail("display_name", "display_name must not have leading or trailing whitespace")
    if not (1 <= len(name) <= 128):
        fail("display_name", f"display_name length must be between 1 and 128 characters, got {len(name)}")

    # Validate version
    ver = data["version"]
    if not isinstance(ver, str):
        fail("version", f"version must be a string, got {type(ver).__name__}")
    if ver != ver.strip():
        fail("version", "version must not have leading or trailing whitespace")
    if not SEMVER_RE.match(ver):
        fail("version", f"version {ver!r} must be a valid semantic version")

    # Validate category (single canonical validation point)
    category = data["category"]
    if not isinstance(category, str):
        fail("category", f"category must be a string, got {type(category).__name__}")
    if category not in VALID_CATEGORIES:
        fail(
            "category",
            f"Unknown enhancement category: {category!r}; must be one of {sorted(VALID_CATEGORIES)}",
        )

    # Validate target_title_id
    title_id = data["target_title_id"]
    if not isinstance(title_id, str):
        fail("target_title_id", f"target_title_id must be a string, got {type(title_id).__name__}")
    if title_id != title_id.strip():
        fail("target_title_id", "target_title_id must not have leading or trailing whitespace")
    if not TARGET_TITLE_ID_RE.match(title_id):
        fail("target_title_id", f"target_title_id {title_id!r} does not match required pattern")

    # Validate optional min_runtime_version
    if "min_runtime_version" in data:
        min_rt = data["min_runtime_version"]
        if not isinstance(min_rt, str):
            fail("min_runtime_version", f"min_runtime_version must be a string, got {type(min_rt).__name__}")
        if min_rt != min_rt.strip():
            fail("min_runtime_version", "min_runtime_version must not have leading or trailing whitespace")
        if not MIN_RUNTIME_VERSION_RE.match(min_rt):
            fail("min_runtime_version", f"min_runtime_version {min_rt!r} must be a valid semantic version")

    # Validate capabilities
    caps = data["capabilities"]
    if not isinstance(caps, list):
        fail("capabilities", f"capabilities must be an array, got {type(caps).__name__}")
    if len(caps) < 1:
        fail("capabilities", "capabilities array must contain at least 1 item")
    seen_caps: set[str] = set()
    for idx, cap in enumerate(caps):
        cap_path = f"capabilities[{idx}]"
        # Address-literal check applied to capability names
        check_not_address_literal(cap, cap_path, "capability name")
        if not CAPABILITY_RE.match(cap):
            fail(cap_path, f"capability name {cap!r} does not match required pattern {CAPABILITY_RE.pattern}")
        if cap in seen_caps:
            fail(cap_path, f"duplicate capability {cap!r} in capabilities list")
        seen_caps.add(cap)

    # Validate optional hooks (symbol + type only)
    if "hooks" in data:
        hooks = data["hooks"]
        if not isinstance(hooks, list):
            fail("hooks", f"hooks must be an array, got {type(hooks).__name__}")
        for idx, hook in enumerate(hooks):
            hook_path = f"hooks[{idx}]"
            if not isinstance(hook, dict):
                fail(hook_path, f"hook entry must be an object, got {type(hook).__name__}")
            unknown_hook_keys = sorted(set(hook.keys()) - HOOK_KEYS)
            if unknown_hook_keys:
                fail(hook_path, f"Unknown properties forbidden in hook: {unknown_hook_keys}")
            for req in ("symbol", "type"):
                if req not in hook:
                    fail(hook_path, f"Missing required property {req!r} in hook")

            # Address-literal check applied to hook symbols
            symbol = hook["symbol"]
            symbol_path = f"{hook_path}.symbol"
            check_not_address_literal(symbol, symbol_path, "hook symbol")
            if not SYMBOL_RE.match(symbol):
                fail(symbol_path, f"hook symbol {symbol!r} does not match required pattern {SYMBOL_RE.pattern}")

            hook_type = hook["type"]
            type_path = f"{hook_path}.type"
            if not isinstance(hook_type, str):
                fail(type_path, f"hook type must be a string, got {type(hook_type).__name__}")
            if hook_type not in HOOK_TYPES:
                fail(type_path, f"Unknown hook type: {hook_type!r}; must be one of {sorted(HOOK_TYPES)}")

    # Validate timing / frame-rate rescale fields (fail-closed consistency)
    present_timing_keys = sorted(set(data.keys()) & (TIMING_PACKAGE_KEYS | OPTIONAL_TIMING_PACKAGE_KEYS))
    if present_timing_keys:
        if category != "timing_frame_presentation":
            fail(
                "category",
                f"Fields {present_timing_keys} are only valid for category "
                f"'timing_frame_presentation', got {category!r}",
            )
        for req_timing in ("game_version", "executable_sha256", "rng_policy", "sites"):
            if req_timing not in data:
                fail("$", f"Missing required timing package property {req_timing!r}")
        if title_id == "*":
            fail("target_title_id", "Timing rescale package must target a specific title id, not '*'")

        # Validate game_version
        game_ver = data["game_version"]
        if not isinstance(game_ver, str):
            fail("game_version", f"game_version must be a string, got {type(game_ver).__name__}")
        if game_ver != game_ver.strip():
            fail("game_version", "game_version must not have leading or trailing whitespace")
        if not GAME_VERSION_RE.match(game_ver):
            fail("game_version", f"game_version {game_ver!r} does not match required pattern")

        # Validate executable_sha256
        exe_sha = data["executable_sha256"]
        if not isinstance(exe_sha, str):
            fail("executable_sha256", f"executable_sha256 must be a string, got {type(exe_sha).__name__}")
        if not SHA256_HEX_RE.match(exe_sha):
            fail(
                "executable_sha256",
                f"executable_sha256 {exe_sha!r} must be a 64-character lowercase hex SHA-256 digest",
            )

        # Validate rng_policy
        rng = data["rng_policy"]
        if not isinstance(rng, dict):
            fail("rng_policy", f"rng_policy must be an object, got {type(rng).__name__}")
        unknown_rng_keys = sorted(set(rng.keys()) - RNG_POLICY_KEYS)
        if unknown_rng_keys:
            fail("rng_policy", f"Unknown properties forbidden in rng_policy: {unknown_rng_keys}")
        for req_rng in ("mode", "reason"):
            if req_rng not in rng:
                fail("rng_policy", f"Missing required property {req_rng!r} in rng_policy")
        rng_mode = rng["mode"]
        if not isinstance(rng_mode, str) or rng_mode not in RNG_POLICY_MODES:
            fail(
                "rng_policy.mode",
                f"Unknown rng_policy mode: {rng_mode!r}; must be one of {sorted(RNG_POLICY_MODES)}",
            )
        rng_reason = rng["reason"]
        if not isinstance(rng_reason, str) or not rng_reason.strip():
            fail("rng_policy.reason", "rng_policy.reason must be a non-empty string")
        if rng_reason != rng_reason.strip():
            fail("rng_policy.reason", "rng_policy.reason must not have leading or trailing whitespace")

        # Validate sites
        sites = data["sites"]
        if not isinstance(sites, list):
            fail("sites", f"sites must be an array, got {type(sites).__name__}")
        if len(sites) < 1:
            fail("sites", "sites array must contain at least 1 site")
        seen_site_ids: set[str] = set()
        seen_positions: set[tuple[str, int]] = set()
        for idx, site in enumerate(sites):
            site_path = f"sites[{idx}]"
            if not isinstance(site, dict):
                fail(site_path, f"site entry must be an object, got {type(site).__name__}")
            unknown_site_keys = sorted(set(site.keys()) - SITE_KEYS)
            if unknown_site_keys:
                fail(site_path, f"Unknown properties forbidden in site: {unknown_site_keys}")
            for req_site in ("id", "function", "insn_offset", "stock_digest", "transform"):
                if req_site not in site:
                    fail(site_path, f"Missing required property {req_site!r} in site")

            site_id = site["id"]
            id_path = f"{site_path}.id"
            check_not_address_literal(site_id, id_path, "site id")
            if not SITE_ID_RE.match(site_id):
                fail(id_path, f"site id {site_id!r} does not match required pattern {SITE_ID_RE.pattern}")
            if site_id in seen_site_ids:
                fail(id_path, f"duplicate site id {site_id!r}")
            seen_site_ids.add(site_id)

            _check_instruction_location(site, site_path, "site")
            fn_name = site["function"]
            insn_offset = site["insn_offset"]
            pos_key = (fn_name, insn_offset)
            if pos_key in seen_positions:
                fail(
                    site_path,
                    f"duplicate site target ({fn_name!r}, insn_offset={insn_offset})",
                )
            seen_positions.add(pos_key)

            if "word_count" in site:
                word_count = site["word_count"]
                wc_path = f"{site_path}.word_count"
                if (
                    not isinstance(word_count, int)
                    or isinstance(word_count, bool)
                    or word_count < 1
                    or word_count > 16
                ):
                    fail(wc_path, f"word_count must be an integer in [1, 16], got {word_count!r}")
            else:
                site["word_count"] = 1

            transform = site["transform"]
            tf_path = f"{site_path}.transform"
            if not isinstance(transform, str) or transform not in SITE_TRANSFORMS:
                fail(
                    tf_path,
                    f"Unknown site transform: {transform!r}; must be one of {sorted(SITE_TRANSFORMS)}",
                )

        # Validate update_boundary: where one logic update begins. Gated sites run on
        # alternate updates counted there, so a package that gates must declare it.
        if "update_boundary" in data:
            boundary = data["update_boundary"]
            if not isinstance(boundary, dict):
                fail("update_boundary", f"update_boundary must be an object, got {type(boundary).__name__}")
            unknown_boundary_keys = sorted(set(boundary.keys()) - UPDATE_BOUNDARY_KEYS)
            if unknown_boundary_keys:
                fail("update_boundary", f"Unknown properties forbidden in update_boundary: {unknown_boundary_keys}")
            for req_boundary in sorted(UPDATE_BOUNDARY_KEYS):
                if req_boundary not in boundary:
                    fail("update_boundary", f"Missing required property {req_boundary!r} in update_boundary")
            _check_instruction_location(boundary, "update_boundary", "update_boundary")
            if (boundary["function"], boundary["insn_offset"]) in seen_positions:
                fail("update_boundary", "update_boundary must not be a rewritten site")
        elif any(site["transform"] == "gate_30hz" for site in sites):
            fail(
                "update_boundary",
                "A package with gate_30hz sites must declare update_boundary: gated sites run on "
                "alternate logic updates, counted where each update begins",
            )

        if "data_sites" in data:
            _validate_data_sites(data["data_sites"])
        if "data_files" in data:
            _validate_data_files(data["data_files"])

    return data


def _check_id(value: Any, path: str, seen: set[str], role: str) -> None:
    check_not_address_literal(value, path, f"{role} id")
    if not SITE_ID_RE.match(value):
        fail(path, f"{role} id {value!r} does not match required pattern {SITE_ID_RE.pattern}")
    if value in seen:
        fail(path, f"duplicate {role} id {value!r}")
    seen.add(value)


def _check_keys(obj: Any, path: str, allowed: frozenset[str], required: Sequence[str], role: str) -> None:
    if not isinstance(obj, dict):
        fail(path, f"{role} entry must be an object, got {type(obj).__name__}")
    unknown = sorted(set(obj.keys()) - allowed)
    if unknown:
        fail(path, f"Unknown properties forbidden in {role}: {unknown}")
    for req in required:
        if req not in obj:
            fail(path, f"Missing required property {req!r} in {role}")


def _check_int(value: Any, path: str, lo: int, hi: int) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or not lo <= value <= hi:
        fail(path, f"must be an integer in [{lo}, {hi}], got {value!r}")
    return value


def _validate_data_sites(data_sites: Any) -> None:
    if not isinstance(data_sites, list) or not data_sites:
        fail("data_sites", "data_sites must be a non-empty array")
    seen_ids: set[str] = set()
    seen_spans: list[tuple[str, int, int]] = []
    for idx, site in enumerate(data_sites):
        path = f"data_sites[{idx}]"
        _check_keys(site, path, DATA_SITE_KEYS,
                    ("id", "symbol", "offset", "type", "stock_digest", "transform"), "data site")
        _check_id(site["id"], f"{path}.id", seen_ids, "data site")
        symbol = site["symbol"]
        check_not_address_literal(symbol, f"{path}.symbol", "data site symbol")
        if not SYMBOL_RE.match(symbol):
            fail(f"{path}.symbol", f"data site symbol {symbol!r} does not match required pattern {SYMBOL_RE.pattern}")
        dtype = site["type"]
        if dtype not in DATA_TYPES:
            fail(f"{path}.type", f"Unknown data type {dtype!r}; must be one of {sorted(DATA_TYPES)}")
        size = DATA_TYPES[dtype][0]
        offset = _check_int(site["offset"], f"{path}.offset", 0, 1 << 20)
        if offset % size:
            fail(f"{path}.offset", f"offset {offset} is not aligned to its {size}-byte type")
        count = _check_int(site.setdefault("count", 1), f"{path}.count", 1, 256)
        digest = site["stock_digest"]
        if not isinstance(digest, str) or not SHA256_HEX_RE.match(digest):
            fail(f"{path}.stock_digest", "stock_digest must be a 64-character lowercase hex SHA-256 digest")
        if site["transform"] not in DATA_TRANSFORMS:
            fail(f"{path}.transform",
                 f"Transform {site['transform']!r} has no data rescale; must be one of {sorted(DATA_TRANSFORMS)}")
        lo, hi = offset, offset + size * count
        for other_symbol, other_lo, other_hi in seen_spans:
            if other_symbol == symbol and lo < other_hi and other_lo < hi:
                fail(path, f"data site overlaps another data site of {symbol!r}")
        seen_spans.append((symbol, lo, hi))


def _validate_data_files(data_files: Any) -> None:
    if not isinstance(data_files, list) or not data_files:
        fail("data_files", "data_files must be a non-empty array")
    seen_ids: set[str] = set()
    seen_files: set[tuple[str, str]] = set()
    for idx, entry in enumerate(data_files):
        path = f"data_files[{idx}]"
        _check_keys(entry, path, DATA_FILE_KEYS,
                    ("id", "path", "sha256", "format", "header_row", "first_data_row", "cells"),
                    "data file")
        _check_id(entry["id"], f"{path}.id", seen_ids, "data file")
        file_path = entry["path"]
        if not isinstance(file_path, str) or len(file_path) > 255 or not DATA_FILE_PATH_RE.match(file_path):
            fail(f"{path}.path", f"path {file_path!r} must be a relative disc path of plain names separated by '/'")
        digest = entry["sha256"]
        if not isinstance(digest, str) or not SHA256_HEX_RE.match(digest):
            fail(f"{path}.sha256", "sha256 must be a 64-character lowercase hex SHA-256 digest")
        key = (file_path.lower(), digest)
        if key in seen_files:
            fail(path, f"duplicate data file {file_path!r} with the same sha256")
        seen_files.add(key)
        if entry["format"] not in DATA_FILE_FORMATS:
            fail(f"{path}.format", f"Unknown data file format {entry['format']!r}; must be one of {sorted(DATA_FILE_FORMATS)}")
        header_row = _check_int(entry["header_row"], f"{path}.header_row", 0, 255)
        first_data_row = _check_int(entry["first_data_row"], f"{path}.first_data_row", 1, 256)
        if first_data_row <= header_row:
            fail(f"{path}.first_data_row", "first_data_row must come after header_row")
        cells = entry["cells"]
        if not isinstance(cells, list) or not cells:
            fail(f"{path}.cells", "cells must be a non-empty array")
        seen_columns: set[str] = set()
        for cidx, cell in enumerate(cells):
            cpath = f"{path}.cells[{cidx}]"
            _check_keys(cell, cpath, CSV_CELL_KEYS, ("column", "transform"), "csv cell")
            column = cell["column"]
            if not isinstance(column, str) or not CSV_COLUMN_RE.match(column):
                fail(f"{cpath}.column", f"column {column!r} must be an ASCII header name matching {CSV_COLUMN_RE.pattern}")
            if column in seen_columns:
                fail(f"{cpath}.column", f"column {column!r} is rescaled twice")
            seen_columns.add(column)
            if cell["transform"] not in DATA_TRANSFORMS:
                fail(f"{cpath}.transform",
                     f"Transform {cell['transform']!r} has no data rescale; must be one of {sorted(DATA_TRANSFORMS)}")
            if "rows" in cell:
                rows = cell["rows"]
                if not isinstance(rows, list) or not rows:
                    fail(f"{cpath}.rows", "rows must be a non-empty array of data-row indexes")
                for ridx, row in enumerate(rows):
                    _check_int(row, f"{cpath}.rows[{ridx}]", 0, 65535)
                if len(set(rows)) != len(rows):
                    fail(f"{cpath}.rows", "rows must not repeat")


def compute_stock_words_digest(words: Sequence[int]) -> str:
    """Compute the SHA-256 digest of a sequence of 32-bit stock MIPS instruction words."""
    if not words:
        raise ValueError("words must not be empty")
    raw = b"".join(struct.pack("<I", int(w) & 0xFFFFFFFF) for w in words)
    return hashlib.sha256(raw).hexdigest()


def compute_package_digest(pkg_data: Mapping[str, Any]) -> str:
    """Compute a deterministic canonical SHA-256 digest of a validated package declaration."""
    canonical = json.dumps(pkg_data, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def extract_elf_func_symbols(elf: Any) -> dict[str, int]:
    """Extract named STT_FUNC symbols from an analyze.Elf instance if .symtab is present."""
    symtab = elf.sec(".symtab")
    strtab = elf.sec(".strtab")
    if not symtab or not strtab or not symtab.get("entsz"):
        return {}
    d = elf.data
    str_bytes = d[strtab["off"] : strtab["off"] + strtab["size"]]
    bias = int(elf.base) if getattr(elf, "reloc", None) is not None and elf.base else 0
    out: dict[str, int] = {}
    for i in range(symtab["size"] // symtab["entsz"]):
        o = symtab["off"] + i * symtab["entsz"]
        if o + 16 > len(d):
            break
        st_name, st_value, _st_size, st_info, _st_other, _st_shndx = struct.unpack(
            "<IIIBBH", d[o : o + 16]
        )
        if (st_info & 0xF) == 2 and st_value != 0 and st_name < len(str_bytes):
            end = str_bytes.find(b"\x00", st_name)
            if end > st_name:
                name = str_bytes[st_name:end].decode("ascii", "replace")
                if SYMBOL_RE.match(name) and not is_address_literal(name):
                    out[name] = (st_value + bias) & 0xFFFFFFFF
    return out


def load_symbol_map(path: Path | str) -> dict[str, int]:
    """Load an optional sidecar symbol map JSON (mapping symbolic function name -> vaddr)."""
    sym_path = Path(path)
    if not sym_path.is_file():
        raise EnhancementPackageError(f"Symbol map file not found: {sym_path}")
    try:
        raw = json.loads(sym_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise EnhancementPackageError(f"Failed to parse symbol map JSON {sym_path}: {exc}") from exc
    if not isinstance(raw, dict):
        raise EnhancementPackageError(f"Symbol map root must be a JSON object in {sym_path}")
    table = raw.get("symbols", raw)
    if not isinstance(table, dict):
        raise EnhancementPackageError(f"Symbol map 'symbols' must be an object in {sym_path}")
    out: dict[str, int] = {}
    for k, v in table.items():
        check_not_address_literal(k, f"symbols.{k}", "symbol name")
        if isinstance(v, int) and not isinstance(v, bool):
            out[k] = v & 0xFFFFFFFF
        elif isinstance(v, str):
            try:
                out[k] = int(v, 16 if v.lower().startswith("0x") else 10) & 0xFFFFFFFF
            except ValueError as exc:
                raise EnhancementPackageError(
                    f"Invalid address {v!r} for symbol {k!r} in {sym_path}"
                ) from exc
        else:
            raise EnhancementPackageError(
                f"Invalid address {v!r} for symbol {k!r} in {sym_path}"
            )
    return out


def verify_and_resolve_sites(
    pkg_data: Mapping[str, Any],
    *,
    executable_sha256: str,
    symbol_to_addr: Mapping[str, int],
    read_words_fn: Any,
    known_functions: Sequence[int] = (),
) -> dict[int, dict[str, Any]]:
    """Verify package executable and site stock-word digests fail-closed.

    Args:
        pkg_data: Validated package dict.
        executable_sha256: Actual lowercase hex SHA-256 of the guest executable.
        symbol_to_addr: Mapping of symbolic function name -> function entry vaddr.
        read_words_fn: Callable ``(vaddr, count) -> Sequence[int] | None`` reading
            contiguous 32-bit little-endian instruction words from the guest ELF.
        known_functions: Sorted sequence of known function start vaddrs in the ELF.
            Used as a deterministic fallback when ``symbol_to_addr`` does not map a
            symbol and exactly one function in the ELF matches ``(insn_offset, stock_digest)``.

    Returns:
        Mapping of resolved instruction ``vaddr -> site dict`` (augmented with
        ``fn_addr``, ``site_addr``, and ``stock_words``).

    Raises:
        EnhancementPackageError: If the executable digest, any symbol resolution, or
            any site stock-word digest does not match (fail-closed refusal).
    """
    validated = validate_package(dict(pkg_data))
    if "sites" not in validated:
        return {}

    expected_exe_sha = validated["executable_sha256"]
    actual_exe_sha = executable_sha256.strip().lower()
    if actual_exe_sha != expected_exe_sha:
        fail(
            "executable_sha256",
            f"Executable SHA-256 mismatch (fail-closed refusal): package expects "
            f"{expected_exe_sha}, executable has {actual_exe_sha}",
        )

    known_set = set(int(a) for a in known_functions)
    resolved_by_addr: dict[int, dict[str, Any]] = {}
    unresolved_fn_cache: dict[str, int] = {}

    for idx, site in enumerate(validated["sites"]):
        site_path = f"sites[{idx}]({site['id']})"
        fn_name = site["function"]
        insn_offset = int(site["insn_offset"])
        word_count = int(site.get("word_count", 1))
        expected_digest = site["stock_digest"]

        fn_addr = _resolve_function_address(
            validated, fn_name, site_path, symbol_to_addr, read_words_fn, known_functions,
            unresolved_fn_cache,
        )
        site_addr, words = _read_verified_words(
            fn_name, fn_addr, insn_offset, word_count, expected_digest, site_path,
            read_words_fn, known_set,
        )

        if site_addr in resolved_by_addr:
            fail(
                site_path,
                f"Duplicate resolved instruction address 0x{site_addr:08x} (fail-closed refusal)",
            )

        stock_words = [int(w) & 0xFFFFFFFF for w in words]
        if word_count == 1:
            rewrites = [plan_site_rewrite(site, stock_words[0])]
        elif word_count == 2:
            rewrites = plan_float_pair_rewrite(site, stock_words[0], stock_words[1])
        else:
            fail(
                f"{site_path}.word_count",
                "Only one-word sites and lui/ori float pairs have a defined rewrite (fail-closed refusal)",
            )
        for word_index, rewrite in enumerate(rewrites):
            word_addr = site_addr + 4 * word_index
            if word_addr in resolved_by_addr:
                fail(
                    site_path,
                    f"Duplicate resolved instruction address 0x{word_addr:08x} (fail-closed refusal)",
                )
            resolved_entry = dict(site)
            resolved_entry["fn_addr"] = fn_addr
            resolved_entry["site_addr"] = word_addr
            resolved_entry["stock_words"] = [stock_words[word_index]]
            resolved_entry["rewrite"] = rewrite
            resolved_by_addr[word_addr] = resolved_entry

    if "update_boundary" not in validated:
        gated = sorted(e["id"] for e in resolved_by_addr.values() if e["rewrite"]["kind"] == "gate")
        if gated:
            fail(
                "update_boundary",
                f"Sites {gated} run on alternate logic updates, but the package declares no "
                f"update_boundary to count them (fail-closed refusal)",
            )
    return resolved_by_addr


def verify_and_resolve_update_boundary(
    pkg_data: Mapping[str, Any],
    *,
    symbol_to_addr: Mapping[str, int],
    read_words_fn: Any,
    known_functions: Sequence[int] = (),
) -> dict[str, Any] | None:
    """Resolve and verify a package's ``update_boundary`` fail-closed.

    Returns ``None`` when the package declares none, else a dict with ``fn_addr``,
    ``site_addr`` and ``stock_word``. The executable digest is checked by
    :func:`verify_and_resolve_sites`, which callers run first.
    """
    validated = validate_package(dict(pkg_data))
    boundary = validated.get("update_boundary")
    if boundary is None:
        return None
    fn_name = boundary["function"]
    fn_addr = _resolve_function_address(
        validated, fn_name, "update_boundary", symbol_to_addr, read_words_fn, known_functions, {},
    )
    site_addr, words = _read_verified_words(
        fn_name, fn_addr, int(boundary["insn_offset"]), 1, boundary["stock_digest"],
        "update_boundary", read_words_fn, set(int(a) for a in known_functions),
    )
    return {"fn_addr": fn_addr, "site_addr": site_addr, "stock_word": int(words[0]) & 0xFFFFFFFF}


def _function_locations(validated: Mapping[str, Any], fn_name: str) -> list[tuple[int, int, str]]:
    """Every (insn_offset, word_count, stock_digest) the package pins inside ``fn_name``."""
    locations = [
        (int(s["insn_offset"]), int(s.get("word_count", 1)), s["stock_digest"])
        for s in validated["sites"] if s["function"] == fn_name
    ]
    boundary = validated.get("update_boundary")
    if boundary is not None and boundary["function"] == fn_name:
        locations.append((int(boundary["insn_offset"]), 1, boundary["stock_digest"]))
    return locations


def _resolve_function_address(
    validated: Mapping[str, Any],
    fn_name: str,
    path: str,
    symbol_to_addr: Mapping[str, int],
    read_words_fn: Any,
    known_functions: Sequence[int],
    cache: dict[str, int],
) -> int:
    """A symbolic function's entry: the symbol map's, else the one known function whose
    words match every location the package pins in it (a deterministic fallback)."""
    if fn_name in symbol_to_addr:
        return int(symbol_to_addr[fn_name])
    if fn_name in cache:
        return cache[fn_name]
    if known_functions:
        locations = _function_locations(validated, fn_name)
        candidates: list[int] = []
        for cand_fn in known_functions:
            for off, wc, digest in locations:
                words = read_words_fn(int(cand_fn) + off, wc)
                if words is None or len(words) != wc or compute_stock_words_digest(words) != digest:
                    break
            else:
                candidates.append(int(cand_fn))
        if len(candidates) == 1:
            cache[fn_name] = candidates[0]
            return candidates[0]
        if len(candidates) > 1:
            fail(
                f"{path}.function",
                f"Ambiguous symbolic function {fn_name!r} without symbol map: "
                f"matched {len(candidates)} candidate functions (fail-closed refusal)",
            )
    fail(
        f"{path}.function",
        f"Cannot resolve symbolic function {fn_name!r} in guest executable "
        f"(fail-closed refusal)",
    )


def _read_verified_words(
    fn_name: str,
    fn_addr: int,
    insn_offset: int,
    word_count: int,
    expected_digest: str,
    path: str,
    read_words_fn: Any,
    known_set: set[int],
) -> tuple[int, list[int]]:
    """Read the stock words at ``fn_name + insn_offset`` and check their digest fail-closed."""
    if known_set and fn_addr not in known_set:
        fail(
            f"{path}.function",
            f"Resolved address 0x{fn_addr:08x} for {fn_name!r} is not a known function entry "
            f"(fail-closed refusal)",
        )
    site_addr = fn_addr + insn_offset
    words = read_words_fn(site_addr, word_count)
    if words is None or len(words) != word_count:
        fail(
            f"{path}.stock_digest",
            f"Cannot read {word_count} instruction word(s) at {fn_name}+{insn_offset} "
            f"(fail-closed refusal)",
        )
    actual_digest = compute_stock_words_digest(words)
    if actual_digest != expected_digest:
        fail(
            f"{path}.stock_digest",
            f"Stock instruction digest mismatch at {fn_name}+{insn_offset} "
            f"(fail-closed refusal): expected {expected_digest}, got {actual_digest}",
        )
    return site_addr, list(words)


def _scale_value(transform: str, value: float) -> float:
    """The rescaled value of one data operand; the caller checks it is representable."""
    if transform == "count_x2":
        return value * 2
    if transform in ("step_x0.5", "probability_per_update_x0.5"):
        return value / 2
    if not 0.0 < value < 1.0:
        raise ValueError(f"{value!r} is outside (0, 1)")
    if transform == "decay_k_to_1-sqrt(1-k)":
        return 1.0 - math.sqrt(1.0 - value)
    return math.sqrt(value)


def plan_data_value(transform: str, dtype: str, raw: bytes) -> bytes:
    """Rescale one little-endian data element; refuse a value with no exact rescale."""
    size, fmt = DATA_TYPES[dtype]
    (value,) = struct.unpack(fmt, raw)
    if dtype == "f32":
        if not math.isfinite(value):
            raise ValueError(f"float {value!r} is not finite")
        scaled = _scale_value(transform, value)
        out = struct.pack(fmt, scaled)
        if not math.isfinite(struct.unpack(fmt, out)[0]):
            raise ValueError(f"float {value!r} overflows when rescaled")
        return out
    if transform not in ("count_x2", "step_x0.5", "probability_per_update_x0.5"):
        raise ValueError(f"{transform} needs a float, not {dtype}")
    scaled = _scale_value(transform, value)
    if scaled != int(scaled):
        raise ValueError(f"{dtype} {value} has no exact rescale")
    try:
        return struct.pack(fmt, int(scaled))
    except struct.error as exc:
        raise ValueError(f"{dtype} {value} overflows when rescaled") from exc


def verify_and_resolve_data_sites(
    pkg_data: Mapping[str, Any],
    *,
    symbol_to_addr: Mapping[str, int],
    read_data_fn: Any,
) -> list[dict[str, Any]]:
    """Resolve, digest-check and rescale a package's executable data sites fail-closed.

    ``read_data_fn(vaddr, size)`` returns the stock bytes of initialised loaded data
    (never code or zero-fill), or ``None``. Returns one entry per site with its
    ``addr``, ``stock`` bytes and rescaled ``data`` bytes.
    """
    validated = validate_package(dict(pkg_data))
    resolved: list[dict[str, Any]] = []
    for idx, site in enumerate(validated.get("data_sites", ())):
        path = f"data_sites[{idx}]({site['id']})"
        symbol = site["symbol"]
        if symbol not in symbol_to_addr:
            fail(f"{path}.symbol", f"Cannot resolve data symbol {symbol!r} (fail-closed refusal)")
        size = DATA_TYPES[site["type"]][0]
        addr = int(symbol_to_addr[symbol]) + int(site["offset"])
        length = size * int(site["count"])
        stock = read_data_fn(addr, length)
        if stock is None or len(stock) != length:
            fail(path, f"{symbol}+{site['offset']} is not {length} bytes of initialised data (fail-closed refusal)")
        stock = bytes(stock)
        actual = hashlib.sha256(stock).hexdigest()
        if actual != site["stock_digest"]:
            fail(f"{path}.stock_digest",
                 f"Stock data digest mismatch at {symbol}+{site['offset']} (fail-closed refusal): "
                 f"expected {site['stock_digest']}, got {actual}")
        out = bytearray()
        for elem in range(int(site["count"])):
            raw = stock[elem * size:(elem + 1) * size]
            try:
                out += plan_data_value(site["transform"], site["type"], raw)
            except ValueError as exc:
                fail(path, f"element {elem}: {exc} (fail-closed refusal)")
        resolved.append({"id": site["id"], "addr": addr, "stock": stock, "data": bytes(out)})
    resolved.sort(key=lambda e: e["addr"])
    for a, b in zip(resolved, resolved[1:], strict=False):
        if a["addr"] + len(a["stock"]) > b["addr"]:
            fail("data_sites", f"data sites {a['id']} and {b['id']} overlap (fail-closed refusal)")
    return resolved


_CSV_NUMBER_RE = re.compile(rb"^(-?)([0-9]+)(?:\.([0-9]+))?$")


def _rescale_csv_number(transform: str, text: bytes) -> bytes:
    """Rescale one numeric CSV cell, keeping it an integer when it was one."""
    m = _CSV_NUMBER_RE.match(text)
    if m is None:
        raise ValueError(f"cell {text!r} is not a plain decimal number")
    sign, whole, frac = m.group(1), m.group(2), m.group(3)
    if frac is None:
        value = int(sign + whole)
        if transform not in ("count_x2", "step_x0.5", "probability_per_update_x0.5"):
            raise ValueError(f"{transform} needs a decimal cell, not the integer {value}")
        scaled = _scale_value(transform, value)
        if scaled != int(scaled):
            raise ValueError(f"integer cell {value} has no exact rescale")
        return str(int(scaled)).encode("ascii")
    # A decimal cell is read into a float: rescale that float and write the shortest
    # decimal that reads back to the same float.
    value = struct.unpack("<f", struct.pack("<f", float(text.decode("ascii"))))[0]
    scaled = struct.unpack("<f", struct.pack("<f", _scale_value(transform, value)))[0]
    for digits in range(1, 18):
        rendered = f"{scaled:.{digits}f}"
        if struct.unpack("<f", struct.pack("<f", float(rendered)))[0] == scaled:
            return rendered.encode("ascii")
    raise ValueError(f"cell {text!r} has no decimal rendering")


def transform_data_file(entry: Mapping[str, Any], stock: bytes) -> bytes:
    """Apply a validated ``data_files`` entry to the stock bytes of its file, fail-closed."""
    if hashlib.sha256(stock).hexdigest() != entry["sha256"]:
        fail(f"data_files({entry['id']})", f"{entry['path']} does not match its sha256 (fail-closed refusal)")
    lines = stock.split(b"\n")
    header_row = int(entry["header_row"])
    first = int(entry["first_data_row"])
    if len(lines) <= first:
        fail(f"data_files({entry['id']})", f"{entry['path']} has no data rows (fail-closed refusal)")
    header = lines[header_row].rstrip(b"\r").split(b",")
    data_rows = [i for i in range(first, len(lines)) if lines[i].rstrip(b"\r")]
    rows = [line.split(b",") for line in lines]
    for cell in entry["cells"]:
        column = cell["column"].encode("ascii")
        if header.count(column) != 1:
            fail(f"data_files({entry['id']})",
                 f"column {cell['column']!r} is not exactly one header of {entry['path']} (fail-closed refusal)")
        col = header.index(column)
        wanted = cell.get("rows")
        targets = data_rows if wanted is None else []
        if wanted is not None:
            for r in wanted:
                if r >= len(data_rows):
                    fail(f"data_files({entry['id']})", f"row {r} is past the last data row (fail-closed refusal)")
                targets.append(data_rows[r])
        for line_no in targets:
            fields = rows[line_no]
            last = len(fields) - 1
            if col > last:
                fail(f"data_files({entry['id']})",
                     f"line {line_no + 1} has no column {cell['column']!r} (fail-closed refusal)")
            text = fields[col]
            crlf = col == last and text.endswith(b"\r")
            try:
                new = _rescale_csv_number(cell["transform"], text[:-1] if crlf else text)
            except ValueError as exc:
                fail(f"data_files({entry['id']})",
                     f"line {line_no + 1}, column {cell['column']!r}: {exc} (fail-closed refusal)")
            fields[col] = new + (b"\r" if crlf else b"")
    return b"\n".join(b",".join(fields) for fields in rows)


def find_data_file(entry: Mapping[str, Any], data_root: Path | str) -> bytes:
    """The stock bytes of a data file under the title's extracted data root.

    Any file whose path ends with the entry's disc path (compared without case, as
    the disc's file system does) and whose bytes match ``sha256`` is the file;
    none refuses the package.
    """
    root = Path(data_root)
    want = entry["path"].lower().split("/")
    for candidate in sorted(root.rglob(want[-1]), key=str):
        parts = [p.lower() for p in candidate.relative_to(root).parts]
        if parts[-len(want):] != want or not candidate.is_file():
            continue
        data = candidate.read_bytes()
        if hashlib.sha256(data).hexdigest() == entry["sha256"]:
            return data
    fail(f"data_files({entry['id']})",
         f"No file {entry['path']} with sha256 {entry['sha256']} under the title's data (fail-closed refusal)")


# Constant factor each scaling transform applies to the operand the site produces.
SCALE_FACTORS = {
    "count_x2": 2.0,
    "step_x0.5": 0.5,
    "probability_per_update_x0.5": 0.5,
}

_IMM_ARITH_OPS = (0x08, 0x09, 0x0A, 0x0B)  # addi, addiu, slti, sltiu
_INT_LOAD_OPS = (0x20, 0x21, 0x23, 0x24, 0x25)  # lb, lh, lw, lbu, lhu
_FPU_RESULT_FUNCS = (0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07)  # add sub mul div abs mov neg


def _f32(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def _f32_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def _refuse(site: Mapping[str, Any], w: int, why: str) -> NoReturn:
    raise EnhancementPackageError(
        f"site {site['id']}: transform {site['transform']!r} has no defined rewrite for "
        f"instruction 0x{w:08x} ({why}) (fail-closed refusal)"
    )


def _with_imm(w: int, imm: int) -> int:
    return (w & 0xFFFF0000) | (imm & 0xFFFF)


def plan_float_pair_rewrite(site: Mapping[str, Any], w0: int, w1: int) -> list[dict[str, Any]]:
    """The rewrite of a float constant built by ``lui rt, hi`` then ``ori rt, rt, lo``.

    The two words name the whole 32-bit value, so a scaling transform rewrites it
    exactly (rounded to nearest) instead of only the upper half a lone ``lui`` holds.
    Returns one ``word`` rewrite per instruction.
    """
    w0, w1 = int(w0) & 0xFFFFFFFF, int(w1) & 0xFFFFFFFF
    rt = (w0 >> 16) & 0x1F
    if (w0 >> 26) != 0x0F or (w0 >> 21) & 0x1F or rt == 0:
        _refuse(site, w0, "a two-word site starts with lui")
    if (w1 >> 26) != 0x0D or (w1 >> 21) & 0x1F != rt or (w1 >> 16) & 0x1F != rt:
        _refuse(site, w1, "a two-word site ends with ori of the lui's register")
    tf = site["transform"]
    if tf not in DATA_TRANSFORMS:
        _refuse(site, w0, "a float pair takes a scaling transform")
    value = _f32(((w0 & 0xFFFF) << 16) | (w1 & 0xFFFF))
    if not math.isfinite(value):
        _refuse(site, w0, f"float constant {value!r} is not finite")
    try:
        new_value = _scale_value(tf, value)
    except ValueError as exc:
        _refuse(site, w0, str(exc))
    bits = _f32_bits(new_value)
    if not math.isfinite(_f32(bits)):
        _refuse(site, w0, f"float constant {value!r} overflows when rescaled")
    return [
        {"kind": "word", "word": _with_imm(w0, bits >> 16)},
        {"kind": "word", "word": _with_imm(w1, bits & 0xFFFF)},
    ]


def plan_site_rewrite(site: Mapping[str, Any], w: int) -> dict[str, Any]:
    """Return the one defined rewrite of a verified stock word for its declared transform.

    The rewrite depends only on the declared transform and the instruction's own
    encoding; nothing here knows a title. Every (transform, instruction) pair
    without a defined rewrite raises, so one unsupported site refuses the whole
    package instead of emitting a silent no-op.

    Returned kinds:
      ``word``            execute ``word`` instead of the stock word;
      ``gate``            execute the stock instruction on alternate 60 Hz updates only;
      ``scale_int_reg``   after the stock load, multiply GPR ``reg`` by 2;
      ``scale_f_reg``     after the stock instruction, multiply FPR ``reg`` by ``factor``;
      ``scale_f_store``   store FPR ``reg`` multiplied by ``factor`` (the register is kept);
      ``scale_f_addend``  run ``fd = fd +/- ft`` with ``ft`` multiplied by ``factor``.
    """
    w = int(w) & 0xFFFFFFFF
    tf = site["transform"]
    op = (w >> 26) & 0x3F
    rs = (w >> 21) & 0x1F
    rt = (w >> 16) & 0x1F
    rd = (w >> 11) & 0x1F
    sa = (w >> 6) & 0x1F
    fn = w & 0x3F
    imm16 = w & 0xFFFF
    simm16 = imm16 - 0x10000 if (imm16 & 0x8000) else imm16
    is_jal = op == 0x03
    is_jalr = op == 0x00 and fn == 0x09
    is_fpu_s = op == 0x11 and rs == 0x10

    if tf == "vblank_wait_2_to_1":
        if op in _IMM_ARITH_OPS + (0x0D,) and simm16 == 2:
            return {"kind": "word", "word": _with_imm(w, 1)}
        _refuse(site, w, "expects an immediate of 2")

    if tf == "gate_30hz":
        if is_jal or is_jalr:
            return {"kind": "gate"}
        if op in (0x08, 0x09) and rt != 0 and rt == rs:
            return {"kind": "gate"}
        if op == 0x00 and fn in (0x20, 0x21, 0x22, 0x23) and rd != 0 and rd in (rs, rt):
            return {"kind": "gate"}
        if is_fpu_s and fn in (0x00, 0x01) and sa in (rd, rt):
            return {"kind": "gate"}
        _refuse(site, w, "gate_30hz needs a call or an accumulating update")

    if tf in ("decay_k_to_1-sqrt(1-k)", "retention_m_to_sqrt(m)"):
        if op != 0x0F:
            _refuse(site, w, "expects the lui of a float constant")
        value = _f32(imm16 << 16)
        if not 0.0 < value < 1.0:
            _refuse(site, w, f"float constant {value!r} is outside (0, 1)")
        if tf == "decay_k_to_1-sqrt(1-k)":
            new_value = 1.0 - math.sqrt(1.0 - value)
        else:
            new_value = math.sqrt(value)
        bits = _f32_bits(new_value)
        hi = ((bits + 0x8000) >> 16) & 0xFFFF
        return {"kind": "word", "word": _with_imm(w, hi)}

    factor = SCALE_FACTORS.get(tf)
    if factor is None:
        raise EnhancementPackageError(f"Unknown transform {tf!r} at site {site['id']}")
    halve = factor == 0.5

    if op in (0x08, 0x09) and rt != 0 and rt == rs:
        # Accumulating counter update (x += imm): a unit step under a halved rate
        # (or a unit countdown under a doubled duration) runs on alternate updates.
        if simm16 in (1, -1) and (halve or simm16 == -1):
            return {"kind": "gate"}
        if halve and simm16 % 2 == 0 and simm16 != 0:
            return {"kind": "word", "word": _with_imm(w, simm16 // 2)}
        _refuse(site, w, "accumulating immediate without an exact rescale")
    if op in _IMM_ARITH_OPS:
        if simm16 <= 0:
            _refuse(site, w, "non-positive immediate")
        scaled = simm16 * factor
        if scaled != int(scaled) or int(scaled) > 0x7FFF or int(scaled) < 1:
            _refuse(site, w, f"immediate {simm16} has no exact x{factor:g} rescale")
        return {"kind": "word", "word": _with_imm(w, int(scaled))}
    if op == 0x0C:  # andi period mask 2^n - 1
        if imm16 == 0 or imm16 & (imm16 + 1):
            _refuse(site, w, "andi mask is not 2^n - 1")
        mask = (imm16 << 1) | 1 if not halve else imm16 >> 1
        if mask == 0 or mask > 0xFFFF:
            _refuse(site, w, "andi mask has no exact rescale")
        return {"kind": "word", "word": _with_imm(w, mask)}
    if op == 0x0F:  # lui of a float constant: an exponent step is an exact x2 / x0.5
        exponent = (imm16 >> 7) & 0xFF
        if not (2 if halve else 1) <= exponent <= (254 if halve else 253):
            _refuse(site, w, "lui value is not a normal float constant")
        return {"kind": "word", "word": _with_imm(w, imm16 + (-0x80 if halve else 0x80))}
    if op == 0x00 and fn == 0x00 and w != 0:  # sll: shift count is a power-of-two factor
        new_sa = sa - 1 if halve else sa + 1
        if not 0 <= new_sa <= 31 or (halve and sa == 0):
            _refuse(site, w, "sll shift has no exact rescale")
        return {"kind": "word", "word": (w & ~(0x1F << 6)) | (new_sa << 6)}
    if op == 0x00 and fn in (0x21, 0x23) and halve and rd != 0 and rd in (rs, rt):
        return {"kind": "gate"}  # accumulating addu/subu at half rate
    if op in _INT_LOAD_OPS and not halve and rt != 0:
        return {"kind": "scale_int_reg", "reg": rt}
    if op == 0x31:  # lwc1
        return {"kind": "scale_f_reg", "reg": rt, "factor": factor}
    if op == 0x39:  # swc1
        return {"kind": "scale_f_store", "reg": rt, "factor": factor}
    if is_fpu_s and fn in (0x00, 0x01) and sa == rd and rt != rd:
        return {"kind": "scale_f_addend", "reg": rt, "factor": factor}
    if is_fpu_s and fn in _FPU_RESULT_FUNCS:
        return {"kind": "scale_f_reg", "reg": sa, "factor": factor}
    if op == 0x11 and rs == 0x14 and fn == 0x20:  # cvt.s.w
        return {"kind": "scale_f_reg", "reg": sa, "factor": factor}
    _refuse(site, w, "instruction class has no defined rescale")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate Nakagawa enhancement package declarations.")
    parser.add_argument("packages", nargs="+", type=Path, help="Package JSON files to validate.")
    args = parser.parse_args(argv)

    failures = 0
    for pkg_path in args.packages:
        try:
            raw = load_package(pkg_path)
            validate_package(raw)
            print(f"{pkg_path}: OK")
        except EnhancementPackageError as exc:
            print(f"{pkg_path}: FAIL: {exc}", file=sys.stderr)
            failures += 1

    return 1 if failures > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
