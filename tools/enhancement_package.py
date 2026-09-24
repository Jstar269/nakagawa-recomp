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
import json
from pathlib import Path
import re
import sys
from typing import Any, NoReturn

SUPPORTED_SCHEMA_VERSION = 1

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

HOOK_KEYS = frozenset({
    "symbol",
    "type",
})

ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
SEMVER_RE = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$"
)
MIN_RUNTIME_VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
TARGET_TITLE_ID_RE = re.compile(r"^([a-z0-9][a-z0-9._-]{0,63}|\*)$")
CAPABILITY_RE = re.compile(r"^[a-z][a-z0-9_]{1,63}$")
SYMBOL_RE = re.compile(r"^[a-zA-Z_][a-zA-Z0-9_]{0,127}$")

HEX_ADDRESS_RE = re.compile(r"^0[xX][0-9a-fA-F]+$")
DECOMP_ADDRESS_RE = re.compile(r"^(?:sub|loc|func)_[0-9a-fA-F]+$", re.IGNORECASE)
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

    return data


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
