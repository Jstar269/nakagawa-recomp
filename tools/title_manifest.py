#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Validate and deterministically normalize public title manifests.

These manifests contain source-owned configuration only. Private workspace
bindings, keys, hashes, decompiler output, and game-derived evidence are outside
this format and must remain in ignored local state.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from typing import Any, NoReturn

UINT32_MAX = 0xFFFFFFFF
UINT32_END_MAX = 0x100000000
PUBLIC_MANIFEST_MAX_BYTES = 256 * 1024
MAX_JSON_DEPTH = 16

ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
DISC_ID_RE = re.compile(r"^[A-Z]{4}[0-9]{5}$")
FILENAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
PATH_COMPONENT_RE = re.compile(r"^[A-Za-z0-9._-]+$")
DEVICE_RE = re.compile(r"^[A-Za-z][A-Za-z0-9]{0,15}:$")
WINDOWS_RESERVED = {
    "CON", "PRN", "AUX", "NUL",
    *(f"COM{i}" for i in range(1, 10)),
    *(f"LPT{i}" for i in range(1, 10)),
}

ROOT_KEYS = {
    "schema_version", "id", "display_name", "kind", "disc", "executable",
    "modules", "filesystem", "hle_profile", "feature_requirements",
    "compatibility_manifest", "verification_profile", "codegen_profile", "notes",
}


class TitleManifestError(ValueError):
    """Deterministic user-facing validation failure."""


def fail(path: str, message: str) -> NoReturn:
    raise TitleManifestError(f"{path}: {message}")


def obj(value: Any, path: str, allowed: set[str]) -> dict[str, Any]:
    if not isinstance(value, dict):
        fail(path, "must be an object")
    unknown = sorted(set(value) - allowed)
    if unknown:
        fail(path, f"unknown field(s): {', '.join(unknown)}")
    return value


def array(value: Any, path: str, maximum: int) -> list[Any]:
    if not isinstance(value, list):
        fail(path, "must be an array")
    if len(value) > maximum:
        fail(path, f"contains {len(value)} items; maximum is {maximum}")
    return value


def text(value: Any, path: str, maximum: int, minimum: int = 1) -> str:
    if not isinstance(value, str):
        fail(path, "must be a string")
    if value != value.strip():
        fail(path, "must not have leading or trailing whitespace")
    if not minimum <= len(value) <= maximum:
        fail(path, f"length must be in range {minimum}..{maximum}")
    if any(ord(char) < 0x20 for char in value):
        fail(path, "must not contain control characters")
    return value


def uint(value: Any, path: str, maximum: int = UINT32_MAX) -> int:
    if type(value) is not int:
        fail(path, "must be an integer")
    if value < 0 or value > maximum:
        fail(path, f"must be in range 0..{maximum}")
    return value


def boolean(value: Any, path: str) -> bool:
    if type(value) is not bool:
        fail(path, "must be a boolean")
    return value


def identifier(value: Any, path: str) -> str:
    value = text(value, path, 64)
    if not ID_RE.fullmatch(value):
        fail(path, "must match ^[a-z0-9][a-z0-9._-]{0,63}$")
    return value


def portable_path(value: Any, path: str) -> str:
    value = text(value, path, 240)
    if len(value.encode("utf-8")) > 240:
        fail(path, "UTF-8 representation exceeds 240 bytes")
    if value.startswith(("/", "\\")) or "\\" in value or ":" in value:
        fail(path, "must be a portable relative POSIX-style path")
    parts = value.split("/")
    for part in parts:
        if part in ("", ".", ".."):
            fail(path, "must not contain empty, '.' or '..' components")
        if not PATH_COMPONENT_RE.fullmatch(part) or part.endswith((".", " ")):
            fail(path, f"invalid or non-portable path component: {part!r}")
        if part.split(".", 1)[0].upper() in WINDOWS_RESERVED:
            fail(path, f"reserved Windows path component: {part!r}")
    return "/".join(parts)


def require(mapping: dict[str, Any], path: str, *names: str) -> None:
    missing = [name for name in names if name not in mapping]
    if missing:
        fail(path, f"missing required field(s): {', '.join(missing)}")


def depth(value: Any, path: str = "$", level: int = 0) -> None:
    if level > MAX_JSON_DEPTH:
        fail(path, f"JSON nesting exceeds {MAX_JSON_DEPTH}")
    if isinstance(value, dict):
        for key, child in value.items():
            depth(child, f"{path}.{key}", level + 1)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            depth(child, f"{path}[{index}]", level + 1)


def no_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise TitleManifestError(f"$: duplicate JSON object key: {key!r}")
        result[key] = value
    return result


def loads_manifest(raw: str) -> dict[str, Any]:
    try:
        value = json.loads(raw, object_pairs_hook=no_duplicate_keys)
    except TitleManifestError:
        raise
    except json.JSONDecodeError as exc:
        raise TitleManifestError(f"$: invalid JSON: {exc}") from exc
    if not isinstance(value, dict):
        fail("$", "must be an object")
    depth(value)
    return value


def load_manifest(path: Path, *, max_bytes: int = PUBLIC_MANIFEST_MAX_BYTES) -> dict[str, Any]:
    if max_bytes <= 0:
        raise ValueError("max_bytes must be positive")
    if path.is_symlink():
        raise TitleManifestError(f"{path}: symbolic links are not accepted")
    if not path.is_file():
        raise TitleManifestError(f"{path}: manifest is not a regular file")
    try:
        with path.open("rb") as stream:
            raw = stream.read(max_bytes + 1)
        if len(raw) > max_bytes:
            raise TitleManifestError(f"{path}: input exceeds the {max_bytes}-byte limit")
        return loads_manifest(raw.decode("utf-8"))
    except TitleManifestError:
        raise
    except (OSError, UnicodeError) as exc:
        raise TitleManifestError(f"{path}: unable to read UTF-8 manifest: {exc}") from exc


def validate_disc(value: Any, path: str) -> dict[str, Any]:
    value = obj(value, path, {"id", "region", "revision_policy", "compatible_revisions"})
    require(value, path, "id", "region", "revision_policy")
    disc_id = text(value["id"], f"{path}.id", 9)
    if not DISC_ID_RE.fullmatch(disc_id):
        fail(f"{path}.id", "must match a nine-character PSP disc ID such as TEST00001")
    region = text(value["region"], f"{path}.region", 5)
    if region not in {"JP", "NA", "EU", "KR", "ASIA", "OTHER"}:
        fail(f"{path}.region", "unsupported region")
    policy = text(value["revision_policy"], f"{path}.revision_policy", 32)
    if policy not in {"exact-disc-id", "explicit-compatible-revisions"}:
        fail(f"{path}.revision_policy", "unsupported revision policy")
    result: dict[str, Any] = {"id": disc_id, "region": region, "revision_policy": policy}
    revisions = value.get("compatible_revisions")
    if policy == "exact-disc-id":
        if revisions is not None:
            fail(f"{path}.compatible_revisions", "is forbidden for exact-disc-id policy")
        return result
    if revisions is None:
        fail(path, "compatible_revisions is required for explicit-compatible-revisions")
    revisions = array(revisions, f"{path}.compatible_revisions", 16)
    if not revisions:
        fail(f"{path}.compatible_revisions", "must not be empty")
    normalized: set[str] = set()
    for index, revision in enumerate(revisions):
        revision = text(revision, f"{path}.compatible_revisions[{index}]", 9)
        if not DISC_ID_RE.fullmatch(revision) or revision == disc_id:
            fail(f"{path}.compatible_revisions[{index}]", "must be a distinct PSP disc ID")
        if revision in normalized:
            fail(f"{path}.compatible_revisions[{index}]", "duplicate disc ID")
        normalized.add(revision)
    result["compatible_revisions"] = sorted(normalized)
    return result


def validate_executable(value: Any, path: str) -> dict[str, Any]:
    allowed = {"base", "entry", "bss_metadata_source", "extra_executable_spans"}
    value = obj(value, path, allowed)
    require(value, path, *allowed)
    source = text(value["bss_metadata_source"], f"{path}.bss_metadata_source", 32)
    if source not in {"elf", "psp-header", "none"}:
        fail(f"{path}.bss_metadata_source", "unsupported metadata source")
    spans: list[dict[str, int]] = []
    for index, item in enumerate(array(value["extra_executable_spans"], f"{path}.extra_executable_spans", 64)):
        item_path = f"{path}.extra_executable_spans[{index}]"
        item = obj(item, item_path, {"start", "end"})
        require(item, item_path, "start", "end")
        start = uint(item["start"], f"{item_path}.start")
        end = uint(item["end"], f"{item_path}.end", UINT32_END_MAX)
        if end <= start:
            fail(item_path, "end must be greater than start")
        spans.append({"start": start, "end": end})
    spans.sort(key=lambda span: (span["start"], span["end"]))
    for left, right in zip(spans, spans[1:]):
        if right["start"] < left["end"]:
            fail(f"{path}.extra_executable_spans", "spans must not overlap")
    return {
        "base": uint(value["base"], f"{path}.base"),
        "entry": uint(value["entry"], f"{path}.entry"),
        "bss_metadata_source": source,
        "extra_executable_spans": spans,
    }


def validate_modules(value: Any, path: str) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    names: set[str] = set()
    addresses: set[int] = set()
    for index, item in enumerate(array(value, path, 32)):
        item_path = f"{path}[{index}]"
        item = obj(item, item_path, {"name", "load_address", "required", "role"})
        require(item, item_path, "name", "load_address", "required", "role")
        name = text(item["name"], f"{item_path}.name", 128)
        if not FILENAME_RE.fullmatch(name) or name.endswith("."):
            fail(f"{item_path}.name", "must be a portable module filename without path separators")
        if name.split(".", 1)[0].upper() in WINDOWS_RESERVED:
            fail(f"{item_path}.name", "must not use a reserved Windows filename")
        if name.casefold() in names:
            fail(f"{item_path}.name", "duplicate module name under case-insensitive comparison")
        names.add(name.casefold())
        address = uint(item["load_address"], f"{item_path}.load_address")
        if address in addresses:
            fail(f"{item_path}.load_address", "duplicate module load address")
        addresses.add(address)
        required = boolean(item["required"], f"{item_path}.required")
        role = text(item["role"], f"{item_path}.role", 32)
        if role not in {"guest-prx", "hle-capability", "optional-guest-prx"}:
            fail(f"{item_path}.role", "unsupported module role")
        if role == "optional-guest-prx" and required:
            fail(item_path, "optional-guest-prx cannot be marked required")
        if role == "hle-capability" and not required:
            fail(item_path, "hle-capability must be marked required")
        result.append({"name": name, "load_address": address, "required": required, "role": role})
    return result


def validate_filesystem(value: Any, path: str) -> dict[str, Any]:
    value = obj(value, path, {"data_root", "memory_stick_root", "device_prefixes"})
    require(value, path, "data_root", "memory_stick_root", "device_prefixes")
    prefixes: set[str] = set()
    for index, prefix in enumerate(array(value["device_prefixes"], f"{path}.device_prefixes", 16)):
        prefix = text(prefix, f"{path}.device_prefixes[{index}]", 17)
        if not DEVICE_RE.fullmatch(prefix):
            fail(f"{path}.device_prefixes[{index}]", "must look like host0: or ms0:")
        prefix = prefix.lower()
        if prefix in prefixes:
            fail(f"{path}.device_prefixes[{index}]", "duplicate device prefix")
        prefixes.add(prefix)
    return {
        "data_root": portable_path(value["data_root"], f"{path}.data_root"),
        "memory_stick_root": portable_path(value["memory_stick_root"], f"{path}.memory_stick_root"),
        "device_prefixes": sorted(prefixes),
    }


def validate_manifest(value: Any) -> dict[str, Any]:
    value = obj(value, "$", ROOT_KEYS)
    required = {
        "schema_version", "id", "display_name", "kind", "executable", "modules",
        "filesystem", "hle_profile", "feature_requirements", "verification_profile",
    }
    require(value, "$", *sorted(required))
    version = uint(value["schema_version"], "$.schema_version")
    if version != 1:
        fail("$.schema_version", "only schema version 1 is supported")
    kind = text(value["kind"], "$.kind", 16)
    if kind not in {"retail", "homebrew", "synthetic"}:
        fail("$.kind", "unsupported title kind")
    result: dict[str, Any] = {
        "schema_version": version,
        "id": identifier(value["id"], "$.id"),
        "display_name": text(value["display_name"], "$.display_name", 128),
        "kind": kind,
    }
    if kind == "retail":
        if "disc" not in value:
            fail("$", "retail manifests require disc")
        result["disc"] = validate_disc(value["disc"], "$.disc")
    elif "disc" in value:
        fail("$.disc", "is permitted only for retail manifests")
    result["executable"] = validate_executable(value["executable"], "$.executable")
    result["modules"] = validate_modules(value["modules"], "$.modules")
    result["filesystem"] = validate_filesystem(value["filesystem"], "$.filesystem")
    result["hle_profile"] = identifier(value["hle_profile"], "$.hle_profile")
    if "codegen_profile" in value:
        profile = text(value["codegen_profile"], "$.codegen_profile", 16)
        if profile not in {"none", "hst"}:
            fail("$.codegen_profile", "unsupported codegen profile")
        result["codegen_profile"] = profile
    features: set[str] = set()
    for index, feature in enumerate(array(value["feature_requirements"], "$.feature_requirements", 64)):
        feature = identifier(feature, f"$.feature_requirements[{index}]")
        if feature in features:
            fail(f"$.feature_requirements[{index}]", "duplicate feature requirement")
        features.add(feature)
    result["feature_requirements"] = sorted(features)
    if "compatibility_manifest" in value:
        result["compatibility_manifest"] = portable_path(value["compatibility_manifest"], "$.compatibility_manifest")
    result["verification_profile"] = identifier(value["verification_profile"], "$.verification_profile")
    if "notes" in value:
        result["notes"] = text(value["notes"], "$.notes", 2048, 0)
    return result


def canonical_json(value: Any) -> str:
    return json.dumps(validate_manifest(value), ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n"


def write_normalized(path: Path, value: Any) -> None:
    rendered = canonical_json(value)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        raise TitleManifestError(f"{path}: refusing to replace a symbolic link")
    fd, temp_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temp_path = Path(temp_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(rendered)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_path, path)
    except Exception:
        temp_path.unlink(missing_ok=True)
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--normalize-out", type=Path)
    parser.add_argument("--print-normalized", action="store_true")
    parser.add_argument("--max-bytes", type=int, default=PUBLIC_MANIFEST_MAX_BYTES)
    args = parser.parse_args(argv)
    try:
        normalized = validate_manifest(load_manifest(args.manifest, max_bytes=args.max_bytes))
        if args.normalize_out:
            write_normalized(args.normalize_out, normalized)
        if args.print_normalized:
            print(canonical_json(normalized), end="")
        else:
            print(f"OK: {normalized['id']} (schema v{normalized['schema_version']})")
        return 0
    except (TitleManifestError, OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
