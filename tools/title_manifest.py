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
C_SYMBOL_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
BUILD_TARGET_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
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
    "runtime_contract", "profile_zero", "runtime_bindings",
}

#: Optional title bindings the compiled runtime may consume. Every field is
#: individually optional so a generic build has none of them; the paired VBLANK
#: counters are the one exception because a half-configured pair has no meaning.
RUNTIME_BINDING_FIELDS = (
    "fallback_entry",
    "worker_thread_entry",
    "launcher_thread_entry",
    "vblank_frame_counter_addr",
    "vblank_vsync_counter_addr",
)
RUNTIME_BINDING_PAIRS = (("vblank_frame_counter_addr", "vblank_vsync_counter_addr"),)

#: Optional *typed collections* of title bindings. Unlike the scalar fields above, each
#: names a set of semantic sites rather than one address, so the runtime carries a table
#: instead of a single value. Both are individually optional and both are empty in a
#: generic build.
RUNTIME_BINDING_COLLECTIONS = ("dispatch_aliases", "callback_terminators")

#: Per-collection ceilings. These are semantic-debt inventories, not general relocation
#: tables: a manifest needing dozens of entries is describing a codegen or analysis gap
#: that should be fixed upstream, and the bound keeps a malformed manifest from turning
#: into an unbounded runtime table.
MAX_DISPATCH_ALIASES = 32
MAX_CALLBACK_TERMINATORS = 32

#: Dispatch-target values the CORE runtime has already claimed, mirroring
#: ``SR_DISPATCH_VFPU_TAG``/``SR_DISPATCH_VFPU_MASK`` in ``src/rt/recomp.h``. A target
#: satisfying ``target & MASK == TAG`` encodes a per-instruction VFPU fallback, which
#: ``dispatch()`` consumes before any title binding is consulted -- deliberately, because
#: the VFPU encoding is core dispatch vocabulary a title must not be able to shadow.
#:
#: The converse needs enforcing too: a title binding whose *own* match value falls in
#: that window can never be honoured, because dispatch() will have interpreted the value
#: as a VFPU instruction address first. Accepting one would produce a build whose
#: configuration silently does nothing (and, when the value is also a plausible guest
#: address, dispatches into the VFPU interpreter instead). Rejecting it at manifest
#: validation turns that into a precise build-time error.
#:
#: ``tools/test_title_runtime_config.py`` reads the two constants back out of
#: ``src/rt/recomp.h`` so this mirror cannot drift from the runtime it describes.
SR_DISPATCH_VFPU_TAG = 0x40000000
SR_DISPATCH_VFPU_MASK = 0xFC000000


def is_core_reserved_target(value: int) -> bool:
    """True when ``dispatch()`` claims this target value before any title binding."""
    return value & SR_DISPATCH_VFPU_MASK == SR_DISPATCH_VFPU_TAG


def reject_core_reserved_target(value: int, path: str, role: str) -> None:
    """Fail closed on a match value the core dispatch vocabulary has already claimed."""
    if is_core_reserved_target(value):
        fail(
            path,
            f"0x{value:08x} is inside the core VFPU dispatch-target range "
            f"[0x{SR_DISPATCH_VFPU_TAG:08x}, "
            f"0x{SR_DISPATCH_VFPU_TAG | ~SR_DISPATCH_VFPU_MASK & 0xFFFFFFFF:08x}]; "
            f"dispatch() consumes such a target as a VFPU instruction address before any "
            f"title binding is consulted, so this {role} could never match",
        )

CORE_CONTRACT_CAPABILITIES = frozenset({
    "allegrex", "vfpu", "guest-memory", "scheduler", "interrupts", "callbacks",
    "generic-hle", "ge-display", "audio", "io", "backend-contracts", "evidence",
})
EVIDENCE_CLASSES = frozenset({
    "PSP_HARDWARE", "PRODUCTION_DISPATCH", "PRODUCTION_HELPER", "MODEL_REFERENCE",
    "HOST_DIFFERENTIAL", "SOURCE_SHAPE", "PRIVATE_TITLE_ACCEPTANCE",
})
# Profile zero is the public synthetic surface: it may never cite evidence that
# only a private title run can produce. These two names are the authoritative
# vocabulary; assets/title_manifest.schema.json must publish exactly the same
# set, and tools/test_title_manifest.py asserts that mechanically.
PROFILE_ZERO_FORBIDDEN_EVIDENCE_CLASSES = frozenset({"PRIVATE_TITLE_ACCEPTANCE"})
PROFILE_ZERO_EVIDENCE_CLASSES = EVIDENCE_CLASSES - PROFILE_ZERO_FORBIDDEN_EVIDENCE_CLASSES


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
    if not isinstance(path, Path):
        raise TitleManifestError(f"{path!r}: manifest path must be a pathlib.Path")
    if type(max_bytes) is not int or max_bytes <= 0:
        raise ValueError("max_bytes must be a positive integer")
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


def validate_runtime_contract(value: Any, path: str) -> dict[str, Any]:
    """Validate the versioned core/profile boundary.

    The core capability vocabulary is intentionally closed. A profile may select
    capabilities and explicitly document HLE overrides, but it cannot introduce
    an unknown capability or an implicit PSP-semantic replacement. Runtime code
    should apply the same fail-closed rule when loading a contract.
    """
    value = obj(value, path, {
        "schema_version", "core_contract", "profile_id", "capability_requirements",
        "unknown_capability_policy", "boot", "resources", "hle_overrides",
        "input_mapping", "enhancements", "evidence_profile",
    })
    require(value, path, "schema_version", "core_contract", "profile_id", "capability_requirements",
            "unknown_capability_policy", "boot", "resources", "hle_overrides",
            "input_mapping", "enhancements", "evidence_profile")
    if uint(value["schema_version"], f"{path}.schema_version") != 1:
        fail(f"{path}.schema_version", "only contract version 1 is supported")
    core = identifier(value["core_contract"], f"{path}.core_contract")
    if core != "psp-core-v1":
        fail(f"{path}.core_contract", "unsupported PSP core contract")
    profile_id = identifier(value["profile_id"], f"{path}.profile_id")
    policy = text(value["unknown_capability_policy"], f"{path}.unknown_capability_policy", 32)
    if policy != "fail-closed":
        fail(f"{path}.unknown_capability_policy", "must be fail-closed")

    capabilities: set[str] = set()
    for index, capability in enumerate(array(value["capability_requirements"], f"{path}.capability_requirements", 32)):
        capability = identifier(capability, f"{path}.capability_requirements[{index}]")
        if capability not in CORE_CONTRACT_CAPABILITIES:
            fail(f"{path}.capability_requirements[{index}]", "unknown core capability; contracts fail closed")
        if capability in capabilities:
            fail(f"{path}.capability_requirements[{index}]", "duplicate capability")
        capabilities.add(capability)

    boot = obj(value["boot"], f"{path}.boot", {"entry_policy", "thread_policy", "arguments"})
    require(boot, f"{path}.boot", "entry_policy", "thread_policy", "arguments")
    entry_policy = text(boot["entry_policy"], f"{path}.boot.entry_policy", 32)
    if entry_policy not in {"image-entry", "manifest-entry"}:
        fail(f"{path}.boot.entry_policy", "unsupported entry policy")
    thread_policy = text(boot["thread_policy"], f"{path}.boot.thread_policy", 32)
    if thread_policy not in {"profile-default", "no-main-thread"}:
        fail(f"{path}.boot.thread_policy", "unsupported thread policy")
    arguments = [
        text(argument, f"{path}.boot.arguments[{index}]", 128, 0)
        for index, argument in enumerate(array(boot["arguments"], f"{path}.boot.arguments", 16))
    ]

    resources = obj(value["resources"], f"{path}.resources", {"mode", "locators"})
    require(resources, f"{path}.resources", "mode", "locators")
    resource_mode = text(resources["mode"], f"{path}.resources.mode", 32)
    if resource_mode not in {"none", "manifest-filesystem"}:
        fail(f"{path}.resources.mode", "unsupported resource mode")
    locators = [
        portable_path(locator, f"{path}.resources.locators[{index}]")
        for index, locator in enumerate(array(resources["locators"], f"{path}.resources.locators", 32))
    ]
    if resource_mode == "none" and locators:
        fail(f"{path}.resources.locators", "must be empty when resource mode is none")

    overrides = []
    override_names: set[str] = set()
    for index, item in enumerate(array(value["hle_overrides"], f"{path}.hle_overrides", 32)):
        item_path = f"{path}.hle_overrides[{index}]"
        item = obj(item, item_path, {"capability", "disposition", "reason", "evidence_class"})
        require(item, item_path, "capability", "disposition", "reason", "evidence_class")
        capability = identifier(item["capability"], f"{item_path}.capability")
        if capability not in CORE_CONTRACT_CAPABILITIES:
            fail(f"{item_path}.capability", "unknown capability; HLE overrides fail closed")
        if capability in override_names:
            fail(f"{item_path}.capability", "duplicate HLE override")
        override_names.add(capability)
        disposition = text(item["disposition"], f"{item_path}.disposition", 32)
        if disposition not in {"core", "explicit-override", "unavailable"}:
            fail(f"{item_path}.disposition", "unsupported HLE disposition")
        evidence_class = text(item["evidence_class"], f"{item_path}.evidence_class", 32)
        if evidence_class not in EVIDENCE_CLASSES:
            fail(f"{item_path}.evidence_class", "unknown evidence class")
        overrides.append({
            "capability": capability,
            "disposition": disposition,
            "reason": text(item["reason"], f"{item_path}.reason", 512),
            "evidence_class": evidence_class,
        })
    overrides.sort(key=lambda item: item["capability"])

    mapping = obj(value["input_mapping"], f"{path}.input_mapping", {"labels", "replay"})
    require(mapping, f"{path}.input_mapping", "labels", "replay")
    labels = []
    for index, label in enumerate(array(mapping["labels"], f"{path}.input_mapping.labels", 32)):
        label = identifier(label, f"{path}.input_mapping.labels[{index}]")
        if label in labels:
            fail(f"{path}.input_mapping.labels[{index}]", "duplicate input label")
        labels.append(label)
    replay = text(mapping["replay"], f"{path}.input_mapping.replay", 32)
    if replay not in {"none", "deterministic"}:
        fail(f"{path}.input_mapping.replay", "unsupported input replay policy")

    enhancements = obj(value["enhancements"], f"{path}.enhancements", {"enabled_by_default", "capabilities"})
    require(enhancements, f"{path}.enhancements", "enabled_by_default", "capabilities")
    enhancement_capabilities = []
    for index, capability in enumerate(array(enhancements["capabilities"], f"{path}.enhancements.capabilities", 16)):
        capability = identifier(capability, f"{path}.enhancements.capabilities[{index}]")
        if capability in enhancement_capabilities:
            fail(f"{path}.enhancements.capabilities[{index}]", "duplicate enhancement capability")
        enhancement_capabilities.append(capability)
    return {
        "schema_version": 1,
        "core_contract": core,
        "profile_id": profile_id,
        "capability_requirements": sorted(capabilities),
        "unknown_capability_policy": policy,
        "boot": {"entry_policy": entry_policy, "thread_policy": thread_policy, "arguments": arguments},
        "resources": {"mode": resource_mode, "locators": sorted(locators)},
        "hle_overrides": overrides,
        "input_mapping": {"labels": sorted(labels), "replay": replay},
        "enhancements": {
            "enabled_by_default": boolean(enhancements["enabled_by_default"], f"{path}.enhancements.enabled_by_default"),
            "capabilities": sorted(enhancement_capabilities),
        },
        "evidence_profile": identifier(value["evidence_profile"], f"{path}.evidence_profile"),
    }


def validate_profile_zero(value: Any, path: str) -> dict[str, Any]:
    """Validate the source-owned Wave-1 profile-zero scaffold."""
    value = obj(value, path, {"schema_version", "runnable", "source_program", "build", "acceptance"})
    require(value, path, "schema_version", "runnable", "source_program", "build", "acceptance")
    if uint(value["schema_version"], f"{path}.schema_version") != 1:
        fail(f"{path}.schema_version", "only profile-zero schema version 1 is supported")
    source = obj(value["source_program"], f"{path}.source_program", {"source_files", "entry_symbol", "ownership"})
    require(source, f"{path}.source_program", "source_files", "entry_symbol", "ownership")
    source_files = [
        portable_path(item, f"{path}.source_program.source_files[{index}]")
        for index, item in enumerate(array(source["source_files"], f"{path}.source_program.source_files", 16))
    ]
    if not source_files:
        fail(f"{path}.source_program.source_files", "must not be empty")
    source_file_keys = [item.casefold() for item in source_files]
    if len(source_file_keys) != len(set(source_file_keys)):
        fail(f"{path}.source_program.source_files", "duplicate source file")
    ownership = text(source["ownership"], f"{path}.source_program.ownership", 64)
    entry_symbol = text(source["entry_symbol"], f"{path}.source_program.entry_symbol", 128)
    if not C_SYMBOL_RE.fullmatch(entry_symbol):
        fail(f"{path}.source_program.entry_symbol", "must be a portable C symbol")
    if ownership != "project-authored-public":
        fail(f"{path}.source_program.ownership", "profile zero requires project-authored source")
    build = obj(value["build"], f"{path}.build", {"makefile", "working_directory", "target", "toolchain"})
    require(build, f"{path}.build", "makefile", "working_directory", "target", "toolchain")
    toolchain = text(build["toolchain"], f"{path}.build.toolchain", 128)
    if toolchain.startswith(("/", "\\")) or "\\" in toolchain or ":" in toolchain:
        fail(f"{path}.build.toolchain", "must be a portable toolchain label, not a host path")
    build_target = text(build["target"], f"{path}.build.target", 64)
    if not BUILD_TARGET_RE.fullmatch(build_target):
        fail(f"{path}.build.target", "must be a portable build target name")
    build_normalized = {
        "makefile": portable_path(build["makefile"], f"{path}.build.makefile"),
        "working_directory": portable_path(build["working_directory"], f"{path}.build.working_directory"),
        "target": build_target,
        "toolchain": toolchain,
    }
    acceptance = obj(value["acceptance"], f"{path}.acceptance", {"schema_version", "status", "private_inputs_allowed", "cases"})
    require(acceptance, f"{path}.acceptance", "schema_version", "status", "private_inputs_allowed", "cases")
    if uint(acceptance["schema_version"], f"{path}.acceptance.schema_version") != 1:
        fail(f"{path}.acceptance.schema_version", "only acceptance schema version 1 is supported")
    status = text(acceptance["status"], f"{path}.acceptance.status", 32)
    if status not in {"scaffold", "ready", "blocked"}:
        fail(f"{path}.acceptance.status", "unsupported profile-zero acceptance status")
    cases = []
    case_ids: set[str] = set()
    for index, item in enumerate(array(acceptance["cases"], f"{path}.acceptance.cases", 32)):
        item_path = f"{path}.acceptance.cases[{index}]"
        item = obj(item, item_path, {"id", "status", "evidence_class", "assertion"})
        require(item, item_path, "id", "status", "evidence_class", "assertion")
        case_id = identifier(item["id"], f"{item_path}.id")
        if case_id in case_ids:
            fail(f"{item_path}.id", "duplicate acceptance case")
        case_ids.add(case_id)
        case_status = text(item["status"], f"{item_path}.status", 32)
        if case_status not in {"planned", "implemented", "blocked"}:
            fail(f"{item_path}.status", "unsupported acceptance case status")
        evidence_class = text(item["evidence_class"], f"{item_path}.evidence_class", 32)
        if evidence_class not in EVIDENCE_CLASSES:
            fail(f"{item_path}.evidence_class", "unknown evidence class")
        if evidence_class in PROFILE_ZERO_FORBIDDEN_EVIDENCE_CLASSES:
            fail(f"{item_path}.evidence_class", "profile zero cannot contain private-title evidence")
        cases.append({
            "id": case_id,
            "status": case_status,
            "evidence_class": evidence_class,
            "assertion": text(item["assertion"], f"{item_path}.assertion", 512),
        })
    if not cases:
        fail(f"{path}.acceptance.cases", "must contain at least one case")
    cases.sort(key=lambda item: item["id"])
    runnable = boolean(value["runnable"], f"{path}.runnable")
    if runnable != (status == "ready"):
        fail(f"{path}.runnable", "must be true exactly when acceptance status is ready")
    if status == "ready" and any(item["status"] != "implemented" for item in cases):
        fail(f"{path}.acceptance.cases", "ready profile zero requires every case to be implemented")
    if boolean(acceptance["private_inputs_allowed"], f"{path}.acceptance.private_inputs_allowed"):
        fail(f"{path}.acceptance.private_inputs_allowed", "profile zero cannot require private inputs")
    return {
        "schema_version": 1,
        "runnable": runnable,
        "source_program": {
            "source_files": sorted(source_files),
            "entry_symbol": entry_symbol,
            "ownership": ownership,
        },
        "build": build_normalized,
        "acceptance": {
            "schema_version": 1,
            "status": status,
            "private_inputs_allowed": False,
            "cases": cases,
        },
    }


def guest_address(value: Any, path: str) -> int:
    """Validate one guest address used as a runtime binding.

    Zero is the runtime's "not configured" sentinel, so an explicitly configured
    zero is rejected rather than silently disabling a binding. Every binding is
    either a MIPS entry point or a 32-bit guest counter, so a misaligned address
    is a malformed binding, not a supported one.
    """
    value = uint(value, path)
    if value == 0:
        fail(path, "must not be zero; omit the field to leave the binding unconfigured")
    if value % 4 != 0:
        fail(path, "must be 4-byte aligned")
    return value


def validate_dispatch_aliases(value: Any, path: str) -> list[dict[str, int]]:
    """Validate the dispatch-alias collection.

    An alias says "a computed call to `from` must enter the body registered at `to`".
    It exists for a *registration* gap -- a tail-call landing past a callee's prologue,
    for instance -- and never invents behavior: the runtime still executes the ordinary
    registered function, and an unaliased target is still an ordinary dispatch miss.

    The runtime resolves exactly one step, so the rules below make one step sufficient:
    no self-alias, no duplicate source, and no alias whose target is itself aliased.
    A source inside the core VFPU dispatch-target range is rejected outright: dispatch()
    claims that encoding first, so such an alias could never fire.
    """
    items = array(value, path, MAX_DISPATCH_ALIASES)
    if not items:
        fail(path, "must not be empty; omit the field instead")
    result: list[dict[str, int]] = []
    sources: dict[int, int] = {}
    for index, item in enumerate(items):
        item_path = f"{path}[{index}]"
        item = obj(item, item_path, {"from", "to"})
        require(item, item_path, "from", "to")
        source = guest_address(item["from"], f"{item_path}.from")
        # `from` is compared against a dispatch TARGET, so it is subject to the core
        # reservation. `to` is not: it is only ever handed to sr_lookup().
        reject_core_reserved_target(source, f"{item_path}.from", "alias source")
        destination = guest_address(item["to"], f"{item_path}.to")
        if source == destination:
            fail(item_path, "from and to must differ; an alias to itself redirects nothing")
        if source in sources:
            fail(
                f"{item_path}.from",
                f"duplicates the alias source already declared at {path}[{sources[source]}]; "
                "one source cannot redirect to two bodies",
            )
        sources[source] = index
        result.append({"from": source, "to": destination})
    for index, entry in enumerate(result):
        if entry["to"] in sources:
            fail(
                f"{path}[{index}].to",
                f"is itself an alias source (declared at {path}[{sources[entry['to']]}]); "
                "the runtime resolves one step, so a chained alias would silently stop short",
            )
    result.sort(key=lambda entry: entry["from"])
    return result


def _terminator_context(entry: dict[str, int]) -> tuple[int, int]:
    """Sort/compare key for one terminator: absent pc/ra sort as 0 (never a valid value)."""
    return (entry.get("pc", 0), entry.get("ra", 0))


def validate_callback_terminators(value: Any, path: str) -> list[dict[str, int]]:
    """Validate the callback-terminator collection.

    A terminator says "at this exact call site, this sentinel target means the guest's
    callback walk is COMPLETE" -- report completion to the caller instead of treating
    the sentinel as a permissive dispatch miss, which a circular walker would read as
    "keep going" and loop forever.

    ``sentinel`` is a raw target value, not an address: the real ones are 0 and
    0xFFFFFFFF, so neither the non-zero nor the alignment rule for a guest address
    applies to it. Being a target value is also what subjects it to the core VFPU
    reservation, which ``pc``/``ra`` are not subject to. ``pc`` and ``ra`` are genuine guest addresses and *at least one*
    is required -- an entry with neither would make the sentinel terminate everywhere,
    which is exactly the address-global behavior this collection exists to avoid.
    """
    items = array(value, path, MAX_CALLBACK_TERMINATORS)
    if not items:
        fail(path, "must not be empty; omit the field instead")
    result: list[dict[str, int]] = []
    for index, item in enumerate(items):
        item_path = f"{path}[{index}]"
        item = obj(item, item_path, {"sentinel", "pc", "ra"})
        require(item, item_path, "sentinel")
        entry: dict[str, int] = {"sentinel": uint(item["sentinel"], f"{item_path}.sentinel")}
        # The sentinel is compared against a dispatch TARGET. `pc`/`ra` are compared
        # against CpuState fields at the call site, never against a target, so the core
        # reservation does not apply to them.
        reject_core_reserved_target(entry["sentinel"], f"{item_path}.sentinel", "sentinel")
        for field in ("pc", "ra"):
            if field in item:
                entry[field] = guest_address(item[field], f"{item_path}.{field}")
        if "pc" not in entry and "ra" not in entry:
            fail(
                item_path,
                "must constrain at least one of pc/ra; an unconstrained terminator would "
                "match this sentinel at every call site in the program",
            )
        result.append(entry)
    for index, entry in enumerate(result):
        for other_index, other in enumerate(result):
            if other_index == index:
                continue
            if other["sentinel"] != entry["sentinel"]:
                continue
            # `other` subsumes `entry` when it constrains a subset of the same context:
            # every site matching `entry` already matches `other`, so `entry` can never
            # decide anything. Two identical entries subsume each other; report the
            # duplicate first because it has the clearer fix.
            subsumes = all(
                field not in other or other.get(field) == entry.get(field)
                for field in ("pc", "ra")
            )
            if not subsumes:
                continue
            if other == entry and other_index > index:
                fail(
                    f"{path}[{other_index}]",
                    f"duplicates the terminator already declared at {path}[{index}]",
                )
            if other != entry:
                fail(
                    f"{path}[{index}]",
                    f"is unreachable behind the broader terminator at {path}[{other_index}], "
                    "which already matches every site this entry could",
                )
    result.sort(key=lambda entry: (entry["sentinel"], *_terminator_context(entry)))
    return result


def validate_runtime_bindings(value: Any, path: str) -> dict[str, Any]:
    """Validate the optional, strictly-checked title bindings the runtime consumes.

    This block carries title-specific *addresses and roles* only. It cannot redefine
    generic PSP scheduler/kernel/GE semantics: the runtime decides what a worker
    entry or a VBLANK counter means, and this block decides only whether -- and at
    which address -- that meaning applies. A manifest without the block, or without
    a given field, leaves the corresponding runtime behavior disabled.
    """
    value = obj(value, path,
                {"schema_version", *RUNTIME_BINDING_FIELDS, *RUNTIME_BINDING_COLLECTIONS})
    require(value, path, "schema_version")
    if uint(value["schema_version"], f"{path}.schema_version") != 1:
        fail(f"{path}.schema_version", "only runtime-binding schema version 1 is supported")
    result: dict[str, Any] = {"schema_version": 1}
    for name in RUNTIME_BINDING_FIELDS:
        if name in value:
            result[name] = guest_address(value[name], f"{path}.{name}")
    if "dispatch_aliases" in value:
        result["dispatch_aliases"] = validate_dispatch_aliases(
            value["dispatch_aliases"], f"{path}.dispatch_aliases")
    if "callback_terminators" in value:
        result["callback_terminators"] = validate_callback_terminators(
            value["callback_terminators"], f"{path}.callback_terminators")
    if len(result) == 1:
        fail(path, "must configure at least one binding; omit the block instead")
    for left, right in RUNTIME_BINDING_PAIRS:
        present = [name for name in (left, right) if name in result]
        if len(present) == 1:
            fail(
                f"{path}.{present[0]}",
                f"is paired with {left if present[0] == right else right}; configure both or neither",
            )
        if len(present) == 2 and result[left] == result[right]:
            fail(path, f"{left} and {right} must be distinct addresses")
    worker = result.get("worker_thread_entry")
    launcher = result.get("launcher_thread_entry")
    if worker is not None and worker == launcher:
        fail(path, "worker_thread_entry and launcher_thread_entry must be distinct roles")
    return result


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
    if "runtime_contract" in value:
        result["runtime_contract"] = validate_runtime_contract(value["runtime_contract"], "$.runtime_contract")
    if "runtime_bindings" in value:
        result["runtime_bindings"] = validate_runtime_bindings(value["runtime_bindings"], "$.runtime_bindings")
    if "profile_zero" in value:
        if kind != "synthetic":
            fail("$.profile_zero", "is permitted only for synthetic manifests")
        if "runtime_contract" not in result or result["runtime_contract"]["profile_id"] != "profile-zero-v1":
            fail("$.profile_zero", "requires runtime_contract.profile_id=profile-zero-v1")
        result["profile_zero"] = validate_profile_zero(value["profile_zero"], "$.profile_zero")
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
