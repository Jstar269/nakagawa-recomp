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
    present_timing_keys = sorted(set(data.keys()) & TIMING_PACKAGE_KEYS)
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

            fn_name = site["function"]
            fn_path = f"{site_path}.function"
            check_not_address_literal(fn_name, fn_path, "site function")
            if not SYMBOL_RE.match(fn_name):
                fail(fn_path, f"site function {fn_name!r} does not match required pattern {SYMBOL_RE.pattern}")

            insn_offset = site["insn_offset"]
            off_path = f"{site_path}.insn_offset"
            if (
                not isinstance(insn_offset, int)
                or isinstance(insn_offset, bool)
                or insn_offset < 0
                or insn_offset > 65536
                or (insn_offset % 4) != 0
            ):
                fail(
                    off_path,
                    f"insn_offset must be a 4-byte-aligned integer in [0, 65536], got {insn_offset!r}",
                )

            pos_key = (fn_name, insn_offset)
            if pos_key in seen_positions:
                fail(
                    site_path,
                    f"duplicate site target ({fn_name!r}, insn_offset={insn_offset})",
                )
            seen_positions.add(pos_key)

            stock_digest = site["stock_digest"]
            digest_path = f"{site_path}.stock_digest"
            if not isinstance(stock_digest, str) or not SHA256_HEX_RE.match(stock_digest):
                fail(
                    digest_path,
                    f"stock_digest {stock_digest!r} must be a 64-character lowercase hex SHA-256 digest",
                )

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

    return data


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

        fn_addr: int | None = None
        if fn_name in symbol_to_addr:
            fn_addr = int(symbol_to_addr[fn_name])
        elif fn_name in unresolved_fn_cache:
            fn_addr = unresolved_fn_cache[fn_name]
        elif known_functions:
            # Deterministic fallback: find all functions in the ELF whose instruction words
            # at all sites referencing fn_name match their expected stock_digest.
            fn_sites = [s for s in validated["sites"] if s["function"] == fn_name]
            candidates: list[int] = []
            for cand_fn in known_functions:
                all_match = True
                for s_item in fn_sites:
                    s_off = int(s_item["insn_offset"])
                    s_wc = int(s_item.get("word_count", 1))
                    s_words = read_words_fn(int(cand_fn) + s_off, s_wc)
                    if s_words is None or len(s_words) != s_wc:
                        all_match = False
                        break
                    if compute_stock_words_digest(s_words) != s_item["stock_digest"]:
                        all_match = False
                        break
                if all_match:
                    candidates.append(int(cand_fn))
            if len(candidates) == 1:
                fn_addr = candidates[0]
                unresolved_fn_cache[fn_name] = fn_addr
            elif len(candidates) > 1:
                fail(
                    f"{site_path}.function",
                    f"Ambiguous symbolic function {fn_name!r} without symbol map: "
                    f"matched {len(candidates)} candidate functions (fail-closed refusal)",
                )

        if fn_addr is None:
            fail(
                f"{site_path}.function",
                f"Cannot resolve symbolic function {fn_name!r} in guest executable "
                f"(fail-closed refusal)",
            )
        if known_set and fn_addr not in known_set:
            fail(
                f"{site_path}.function",
                f"Resolved address 0x{fn_addr:08x} for {fn_name!r} is not a known function entry "
                f"(fail-closed refusal)",
            )

        site_addr = fn_addr + insn_offset
        words = read_words_fn(site_addr, word_count)
        if words is None or len(words) != word_count:
            fail(
                f"{site_path}.stock_digest",
                f"Cannot read {word_count} instruction word(s) at {fn_name}+{insn_offset} "
                f"(fail-closed refusal)",
            )
        actual_digest = compute_stock_words_digest(words)
        if actual_digest != expected_digest:
            fail(
                f"{site_path}.stock_digest",
                f"Stock instruction digest mismatch at {fn_name}+{insn_offset} "
                f"(fail-closed refusal): expected {expected_digest}, got {actual_digest}",
            )

        if site_addr in resolved_by_addr:
            fail(
                site_path,
                f"Duplicate resolved instruction address 0x{site_addr:08x} (fail-closed refusal)",
            )

        resolved_entry = dict(site)
        resolved_entry["fn_addr"] = fn_addr
        resolved_entry["site_addr"] = site_addr
        resolved_entry["stock_words"] = [int(w) & 0xFFFFFFFF for w in words]
        resolved_by_addr[site_addr] = resolved_entry

    return resolved_by_addr


def _rescale_f32_bits(bits: int, scale: float) -> int:
    """Multiply an IEEE-754 binary32 word by scale and return the new 32-bit word."""
    val = struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]
    scaled = val * scale
    return struct.unpack("<I", struct.pack("<f", scaled))[0]


def _decay_f32_bits(bits: int) -> int:
    """Rescale a 30 Hz per-frame lerp/decay factor to 60 Hz: k -> 1 - sqrt(1 - k) or m -> sqrt(m)."""
    import math
    val = struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]
    if val <= 0.0 or val >= 1.0:
        raise EnhancementPackageError(f"decay constant out of (0, 1) range: {val}")
    # Direct multiplicative retention m (e.g. 0.93) vs lerp weight k (e.g. 0.2, 0.3, 0.6)
    if val > 0.75:
        res = math.sqrt(val)
    else:
        res = 1.0 - math.sqrt(1.0 - val)
    return struct.unpack("<I", struct.pack("<f", res))[0]


def apply_site_transform_word(
    site: Mapping[str, Any],
    w: int,
    next_w: int | None = None,
) -> tuple[int, str | None]:
    """Compute a transformed instruction word or C statement override for a verified site.

    Returns:
        ``(new_word, c_override)`` where ``new_word`` is the rescaled 32-bit MIPS
        instruction word when the transform is representable as an immediate/shift
        change, and ``c_override`` is ``None`` or a special directive tag
        (e.g. ``"gate_30hz"``, ``"fpu_scale_0.5"``, ``"fpu_decay_sqrt"``).
    """
    tf = site["transform"]
    op = (w >> 26) & 0x3F
    fn = w & 0x3F
    imm16 = w & 0xFFFF
    simm16 = imm16 - 0x10000 if (imm16 & 0x8000) else imm16

    if tf == "vblank_wait_2_to_1":
        # sltiu / slti / addiu / ori immediate 2 -> 1
        if op in (0x08, 0x09, 0x0A, 0x0B, 0x0D) and simm16 == 2:
            return (w & 0xFFFF0000) | 0x0001, None
        raise EnhancementPackageError(
            f"Unsupported instruction 0x{w:08x} for vblank_wait_2_to_1 at site {site['id']}"
        )

    if tf == "gate_30hz":
        return w, "gate_30hz"

    if tf == "count_x2":
        # Integer immediate: addi, addiu, slti, sltiu, ori
        if op in (0x08, 0x09, 0x0A, 0x0B, 0x0D):
            if simm16 < 0:
                # Negative offset (e.g. n - 4 sample span -> 2*n + 1 = -7, or -1 decrement gated)
                if simm16 == -1:
                    return w, "gate_30hz"
                new_imm = (2 * simm16 + 1) & 0xFFFF
            else:
                new_imm = (simm16 * 2) & 0xFFFF
            return (w & 0xFFFF0000) | new_imm, None
        # Bitmask modulo period: andi rt, rs, mask (e.g. 0x1 -> 0x3)
        if op == 0x0C:
            new_mask = ((imm16 << 1) | 1) & 0xFFFF
            return (w & 0xFFFF0000) | new_mask, None
        # Float upper-half load: lui rt, hi16
        if op == 0x0F:
            # Check if upper 16 bits represent a positive IEEE-754 float (exponent in bits 14..7)
            exp = (imm16 >> 7) & 0xFF
            if 0 < exp < 254:
                new_imm = (imm16 + 0x0080) & 0xFFFF
                return (w & 0xFFFF0000) | new_imm, None
        # Other instructions (e.g. lw / lwc1 / addu): scale destination register or gate
        return w, "count_x2_stmt"

    if tf == "step_x0.5":
        # Float upper-half load: lui rt, hi16
        if op == 0x0F:
            exp = (imm16 >> 7) & 0xFF
            # Check if this is IEEE-754 double upper half (0x3fc9 -> exp11 in bits 14..4)
            if (imm16 & 0x7FF0) == 0x3FC0:
                new_imm = (imm16 - 0x0010) & 0xFFFF
                return (w & 0xFFFF0000) | new_imm, None
            if 1 < exp < 255:
                new_imm = (imm16 - 0x0080) & 0xFFFF
                return (w & 0xFFFF0000) | new_imm, None
        # ori low half of jump-smash vy0 (-0.3 = 0xbe999999 -> -0.14591666 = 0xbe156b2d)
        if op == 0x0D and imm16 == 0x9999:
            return (w & 0xFFFF0000) | 0x6B2D, None
        # Integer immediate: addiu / addi
        if op in (0x08, 0x09) and simm16 > 1 and (simm16 % 2) == 0:
            new_imm = (simm16 // 2) & 0xFFFF
            return (w & 0xFFFF0000) | new_imm, None
        if op in (0x08, 0x09) and simm16 == 1:
            return w, "gate_30hz"
        # Shift left logical: sll rd, rt, sa -> sa - 1
        if op == 0 and fn == 0x00:
            sh = (w >> 6) & 0x1F
            if sh >= 1:
                return (w & ~(0x1F << 6)) | ((sh - 1) << 6), None
        return w, "step_x0.5_stmt"

    if tf == "probability_per_update_x0.5":
        # Float upper-half threshold: lui rt, hi16 -> exponent - 1 (* 0.5f)
        if op == 0x0F:
            exp = (imm16 >> 7) & 0xFF
            if 1 < exp < 254:
                # Special case: when constant is 100.0f (0x42c8) used as rand*100.0 < stat,
                # doubling the multiplier (200.0f = 0x4348) halves the probability!
                if imm16 == 0x42C8:
                    new_imm = (imm16 + 0x0080) & 0xFFFF
                else:
                    new_imm = (imm16 - 0x0080) & 0xFFFF
                return (w & 0xFFFF0000) | new_imm, None
        # Integer modulus / counter period: addiu/addi (e.g. % 100 < 5 -> % 200 < 5, or period 1 -> 2)
        if op in (0x08, 0x09) and simm16 > 0:
            new_imm = (simm16 * 2) & 0xFFFF
            return (w & 0xFFFF0000) | new_imm, None
        # Integer threshold compare: slti / sltiu -> imm // 2
        if op in (0x0A, 0x0B) and simm16 >= 2:
            new_imm = (simm16 // 2) & 0xFFFF
            return (w & 0xFFFF0000) | new_imm, None
        return w, "prob_x0.5_stmt"

    if tf == "decay_k_to_1-sqrt(1-k)":
        # Float upper-half load: lui rt, hi16 (optionally followed by ori rt, rt, lo16)
        if op == 0x0F:
            rt_reg = (w >> 16) & 0x1F
            lo16 = 0
            if (
                next_w is not None
                and ((next_w >> 26) & 0x3F) == 0x0D
                and ((next_w >> 21) & 0x1F) == rt_reg
                and ((next_w >> 16) & 0x1F) == rt_reg
            ):
                lo16 = next_w & 0xFFFF
            full_bits = (imm16 << 16) | lo16
            new_bits = _decay_f32_bits(full_bits)
            return (w & 0xFFFF0000) | ((new_bits >> 16) & 0xFFFF), None
        if op == 0x0D:
            # ori rt, rs, lo16 low-half of a decay constant (e.g. 0.93 = 0x3f6e147b -> 0x3f76e0a1)
            if imm16 == 0x147B:
                new_bits = _decay_f32_bits(0x3F6E147B)
                return (w & 0xFFFF0000) | (new_bits & 0xFFFF), None
        return w, "decay_sqrt_stmt"

    raise EnhancementPackageError(f"Unknown transform {tf!r} at site {site['id']}")


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
