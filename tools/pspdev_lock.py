#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Validate and report the pinned PSPDEV source/toolchain lock.

This command is deliberately offline and side-effect free. It validates source
identity, provenance fields, and the distinction between a remotely reviewed
source snapshot and local executable/artifact fingerprints. It never downloads,
installs, or updates PSPDEV.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOCK = ROOT / "assets" / "upstream" / "pspdev.lock.json"

SCHEMA = 1
HEX40_RE = re.compile(r"^[0-9a-f]{40}$")
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
RELEASE_RE = re.compile(r"^v20[0-9]{6}$")
SEMVER_TAG_RE = re.compile(r"^v[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$")
CONTAINER_DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
WINDOWS_PATH_RE = re.compile(r"^[A-Za-z]:[\\/]")
UNC_PATH_RE = re.compile(r"^\\\\[^\\]+\\[^\\]+")
POSIX_PRIVATE_PATH_RE = re.compile(r"^/(?:home|Users)/[^/]+(?:/|$)")

EXPECTED_COMPONENTS = {
    "pspsdk": "https://github.com/pspdev/pspsdk",
    "psptoolchain": "https://github.com/pspdev/psptoolchain",
    "psptoolchain_allegrex": "https://github.com/pspdev/psptoolchain-allegrex",
    "psptoolchain_extra": "https://github.com/pspdev/psptoolchain-extra",
    "psp_packages": "https://github.com/pspdev/psp-packages",
    "psplinkusb": "https://github.com/pspdev/psplinkusb",
}
EXPECTED_TOOLS = {
    "psp-config",
    "psp-gcc",
    "psp-ld",
    "psp-objdump",
    "psp-readelf",
    "psp-nm",
    "psp-prxgen",
    "psp-build-exports",
    "psp-fixup-imports",
    "mksfoex",
    "pack-pbp",
    "pspsh",
    "usbhostfs_pc",
}


class LockError(ValueError):
    """The PSPDEV lock is malformed, ambiguous, or overclaims its evidence."""


def _expect_object(value: Any, where: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise LockError(f"{where} must be a JSON object")
    return value


def _expect_keys(
    obj: dict[str, Any],
    required: Iterable[str],
    optional: Iterable[str],
    where: str,
) -> None:
    required_set = set(required)
    optional_set = set(optional)
    missing = sorted(required_set - set(obj))
    unknown = sorted(set(obj) - required_set - optional_set)
    if missing:
        raise LockError(f"{where} is missing required keys: {missing}")
    if unknown:
        raise LockError(f"{where} has unknown keys: {unknown}")


def _expect_string(value: Any, where: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str):
        raise LockError(f"{where} must be a string")
    if not allow_empty and not value.strip():
        raise LockError(f"{where} must not be empty")
    return value


def _expect_bool(value: Any, where: str) -> bool:
    if not isinstance(value, bool):
        raise LockError(f"{where} must be a boolean")
    return value


def _validate_commit(value: Any, where: str) -> str:
    commit = _expect_string(value, where)
    if not HEX40_RE.fullmatch(commit):
        raise LockError(f"{where} must be a lowercase full 40-hex commit")
    return commit


def _validate_nullable_sha256(value: Any, where: str) -> None:
    if value is not None and (
        not isinstance(value, str) or not HEX64_RE.fullmatch(value)
    ):
        raise LockError(f"{where} must be null or a lowercase 64-hex SHA-256")


def _validate_reviewed_at(value: Any) -> str:
    reviewed = _expect_string(value, "reviewed_at")
    if not reviewed.endswith("Z"):
        raise LockError("reviewed_at must use an explicit UTC Z suffix")
    try:
        parsed = datetime.fromisoformat(reviewed[:-1] + "+00:00")
    except ValueError as exc:
        raise LockError(f"reviewed_at is not RFC 3339/ISO-8601: {exc}") from exc
    if parsed.tzinfo != timezone.utc:
        raise LockError("reviewed_at must be UTC")
    return reviewed


def _walk_strings(value: Any, where: str = "$") -> Iterable[tuple[str, str]]:
    if isinstance(value, str):
        yield where, value
    elif isinstance(value, dict):
        for key, child in value.items():
            yield from _walk_strings(child, f"{where}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from _walk_strings(child, f"{where}[{index}]")


def _reject_private_paths(data: dict[str, Any]) -> None:
    for where, value in _walk_strings(data):
        if (
            WINDOWS_PATH_RE.match(value)
            or UNC_PATH_RE.match(value)
            or POSIX_PRIVATE_PATH_RE.match(value)
        ):
            raise LockError(f"{where} embeds a host-specific/private absolute path")


def validate_lock(data: dict[str, Any], *, require_local: bool = False) -> list[str]:
    """Validate one parsed lock and return non-fatal pending-evidence warnings."""

    data = _expect_object(data, "$")
    _expect_keys(
        data,
        required={
            "schema",
            "reviewed_at",
            "snapshot",
            "distribution",
            "components",
            "local_verification",
            "policy",
        },
        optional=set(),
        where="$",
    )
    if data["schema"] != SCHEMA:
        raise LockError(f"schema must be {SCHEMA}")

    _validate_reviewed_at(data["reviewed_at"])

    snapshot = _expect_object(data["snapshot"], "snapshot")
    _expect_keys(
        snapshot,
        required={"kind", "relationship_to_distribution"},
        optional=set(),
        where="snapshot",
    )
    if snapshot["kind"] != "source-review":
        raise LockError("snapshot.kind must be 'source-review'")
    relationship = _expect_string(
        snapshot["relationship_to_distribution"],
        "snapshot.relationship_to_distribution",
    )
    if "not assert" not in relationship.lower():
        raise LockError(
            "snapshot.relationship_to_distribution must explicitly avoid asserting "
            "that independently pinned component heads built the release artifacts"
        )

    distribution = _expect_object(data["distribution"], "distribution")
    _expect_keys(
        distribution,
        required={
            "repository",
            "release",
            "commit",
            "license_expression",
            "archive_sha256",
            "container_digest",
        },
        optional=set(),
        where="distribution",
    )
    if distribution["repository"] != "https://github.com/pspdev/pspdev":
        raise LockError(
            "distribution.repository must be the official pspdev/pspdev repository"
        )
    release = _expect_string(distribution["release"], "distribution.release")
    if not RELEASE_RE.fullmatch(release):
        raise LockError("distribution.release must have the form vYYYYMMDD")
    _validate_commit(distribution["commit"], "distribution.commit")
    _expect_string(
        distribution["license_expression"], "distribution.license_expression"
    )
    _validate_nullable_sha256(
        distribution["archive_sha256"], "distribution.archive_sha256"
    )
    digest = distribution["container_digest"]
    if digest is not None and (
        not isinstance(digest, str) or not CONTAINER_DIGEST_RE.fullmatch(digest)
    ):
        raise LockError(
            "distribution.container_digest must be null or sha256:<64 lowercase hex>"
        )

    components = _expect_object(data["components"], "components")
    if set(components) != set(EXPECTED_COMPONENTS):
        missing = sorted(set(EXPECTED_COMPONENTS) - set(components))
        extra = sorted(set(components) - set(EXPECTED_COMPONENTS))
        raise LockError(
            f"components must match the reviewed set; missing={missing}, extra={extra}"
        )
    for name, expected_repo in EXPECTED_COMPONENTS.items():
        entry = _expect_object(components[name], f"components.{name}")
        required = {
            "repository",
            "default_branch",
            "commit",
            "license_expression",
            "license_notes",
        }
        optional = {"release"} if name == "psplinkusb" else set()
        _expect_keys(
            entry,
            required=required,
            optional=optional,
            where=f"components.{name}",
        )
        if entry["repository"] != expected_repo:
            raise LockError(
                f"components.{name}.repository must be the official repository "
                f"{expected_repo}"
            )
        _expect_string(entry["default_branch"], f"components.{name}.default_branch")
        _validate_commit(entry["commit"], f"components.{name}.commit")
        _expect_string(
            entry["license_expression"], f"components.{name}.license_expression"
        )
        _expect_string(entry["license_notes"], f"components.{name}.license_notes")
        if name == "psplinkusb":
            tag = _expect_string(entry["release"], "components.psplinkusb.release")
            if not SEMVER_TAG_RE.fullmatch(tag):
                raise LockError(
                    "components.psplinkusb.release must be a v-prefixed semantic version"
                )

    local = _expect_object(data["local_verification"], "local_verification")
    _expect_keys(
        local,
        required={"status", "installation_method", "host_platform", "tool_versions"},
        optional=set(),
        where="local_verification",
    )
    if local["status"] not in {"pending_local", "complete"}:
        raise LockError(
            "local_verification.status must be pending_local or complete"
        )
    for field in ("installation_method", "host_platform"):
        if local[field] is not None:
            _expect_string(local[field], f"local_verification.{field}")
    tools = _expect_object(
        local["tool_versions"], "local_verification.tool_versions"
    )
    if set(tools) != EXPECTED_TOOLS:
        missing = sorted(EXPECTED_TOOLS - set(tools))
        extra = sorted(set(tools) - EXPECTED_TOOLS)
        raise LockError(
            f"local_verification.tool_versions mismatch; "
            f"missing={missing}, extra={extra}"
        )
    for name, version in tools.items():
        if version is not None:
            _expect_string(version, f"local_verification.tool_versions.{name}")

    policy = _expect_object(data["policy"], "policy")
    expected_policy = {
        "network_access_from_normal_build",
        "mandatory_for_hst_build_or_runtime",
        "allow_moving_refs",
        "automatic_runtime_rewrite",
        "allow_retail_or_firmware_material",
        "tracked_generated_binaries_require_manifest",
    }
    _expect_keys(policy, required=expected_policy, optional=set(), where="policy")
    for key in expected_policy:
        _expect_bool(policy[key], f"policy.{key}")
    required_false = {
        "network_access_from_normal_build",
        "mandatory_for_hst_build_or_runtime",
        "allow_moving_refs",
        "automatic_runtime_rewrite",
        "allow_retail_or_firmware_material",
    }
    for key in required_false:
        if policy[key]:
            raise LockError(f"policy.{key} must remain false")
    if not policy["tracked_generated_binaries_require_manifest"]:
        raise LockError(
            "policy.tracked_generated_binaries_require_manifest must remain true"
        )

    _reject_private_paths(data)

    pending = []
    if distribution["archive_sha256"] is None:
        pending.append(
            "distribution archive SHA-256 is pending local/download verification"
        )
    if distribution["container_digest"] is None:
        pending.append(
            "distribution container digest is pending local/registry verification"
        )
    missing_tools = sorted(name for name, version in tools.items() if version is None)
    if missing_tools:
        pending.append("local tool identities are pending: " + ", ".join(missing_tools))
    if local["installation_method"] is None:
        pending.append("local installation method is pending")
    if local["host_platform"] is None:
        pending.append("local host platform is pending")

    if local["status"] == "complete" and pending:
        raise LockError(
            "local_verification.status is complete but evidence remains pending"
        )
    if local["status"] == "pending_local" and not pending:
        raise LockError(
            "local_verification.status is pending_local but no evidence is pending"
        )
    if require_local and pending:
        raise LockError("complete local verification required: " + "; ".join(pending))
    return pending


def load_lock(path: Path) -> tuple[dict[str, Any], list[str]]:
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise LockError(f"cannot read {path}: {exc}") from exc
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise LockError(f"{path} is invalid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise LockError(f"{path} root must be an object")
    return data, validate_lock(data)


def canonical_json(data: dict[str, Any]) -> str:
    return json.dumps(data, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def render_report(data: dict[str, Any], pending: list[str]) -> str:
    components = data["components"]
    lines = [
        "# PSPDEV lock audit",
        "",
        f"- Schema: `{data['schema']}`",
        f"- Reviewed: `{data['reviewed_at']}`",
        f"- Distribution: `{data['distribution']['release']}` at "
        f"`{data['distribution']['commit']}`",
        f"- Snapshot kind: `{data['snapshot']['kind']}`",
        f"- Local verification: `{data['local_verification']['status']}`",
        "",
        "## Source pins",
        "",
        "| Component | Commit | License disposition |",
        "| --- | --- | --- |",
    ]
    for name in sorted(components):
        entry = components[name]
        lines.append(
            f"| `{name}` | `{entry['commit']}` | "
            f"`{entry['license_expression']}` |"
        )
    lines.extend(["", "## Pending evidence", ""])
    if pending:
        lines.extend(f"- {item}" for item in pending)
    else:
        lines.append("- None.")
    lines.extend(
        [
            "",
            "## Enforced policy",
            "",
            "- No PSPDEV network/download side effect in the normal Nakagawa build.",
            "- PSPDEV is not mandatory for the HST build or runtime.",
            "- Moving refs and automatic HLE/runtime rewrites are forbidden.",
            "- Retail, firmware, key, and private game material are forbidden.",
            "- Tracked generated binaries require a source/provenance manifest.",
            "",
        ]
    )
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lock", type=Path, default=DEFAULT_LOCK)
    parser.add_argument("--require-local", action="store_true")
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)

    try:
        data, _ = load_lock(args.lock)
        pending = validate_lock(data, require_local=args.require_local)
    except LockError as exc:
        print(f"pspdev_lock: {exc}", file=sys.stderr)
        return 1

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(
            canonical_json(data), encoding="ascii", newline="\n"
        )
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(
            render_report(data, pending), encoding="utf-8", newline="\n"
        )

    status = (
        "complete"
        if not pending
        else f"valid with {len(pending)} pending local item(s)"
    )
    print(f"pspdev_lock: {status}: {args.lock}")
    for item in pending:
        print(f"pspdev_lock: pending: {item}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
