#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Verify reproducible dependency locks, release manifest, and generated SBOM artifacts for Nakagawa Recomp."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from collections import Counter

ROOT = Path(__file__).resolve().parent.parent

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_sbom

EXPECTED_PROVENANCE_FAMILIES = frozenset({
    "sal063",
    "ppsspp",
    "pspsdk",
    "ffmpeg-atrac3p",
    "sdl3",
    "vulkan",
    "vfpu",
})

EXPECTED_TOOLCHAIN_COMPONENTS = frozenset({
    "compiler",
    "make",
    "python",
    "sdl3",
    "vulkan_sdk",
})


def parse_policy_spec(spec: str) -> list[tuple[str, tuple[int, ...]]]:
    """Parse range constraints from a toolchain policy specification string.

    Example:
        '>=4.9.0 (rolling-msys2)' -> [('>=', (4, 9, 0))]
        '>=3.14,<3.15' -> [('>=', (3, 14)), ('<', (3, 15))]
    """
    if not isinstance(spec, str):
        raise ValueError("specification must be a string")
    clean = re.sub(r"\s*\([^)]*\)\s*$", "", spec).strip()
    if not clean:
        raise ValueError("empty range specification")
    parts = [p.strip() for p in clean.split(",")]
    constraints = []
    for part in parts:
        if not part:
            raise ValueError("empty constraint in range specification")
        m = re.match(r"^([<>!=]=?|[<>])\s*(\d+(?:\.\d+)*)$", part)
        if not m:
            raise ValueError(f"invalid constraint syntax: {part!r}")
        op = m.group(1)
        ver = tuple(int(x) for x in m.group(2).split("."))
        constraints.append((op, ver))
    return constraints


def extract_observed_version(entry: object) -> str:
    """Extract version string from observed toolchain entry (flat string or object)."""
    if isinstance(entry, str):
        val = entry.strip()
        if not val:
            raise ValueError("version string cannot be empty")
        return val
    if isinstance(entry, dict) and "version" in entry and isinstance(entry["version"], str):
        val = entry["version"].strip()
        if not val:
            raise ValueError("object 'version' string cannot be empty")
        return val
    raise ValueError(f"expected version string or object with 'version' key, got {entry!r}")


def compare_version(v_obs: tuple[int, ...], op: str, v_target: tuple[int, ...]) -> bool:
    """Compare observed version tuple against target tuple with operator."""
    max_len = max(len(v_obs), len(v_target))
    padded_obs = v_obs + (0,) * (max_len - len(v_obs))
    padded_target = v_target + (0,) * (max_len - len(v_target))
    if op == ">=":
        return padded_obs >= padded_target
    elif op == "<=":
        return padded_obs <= padded_target
    elif op == ">":
        return padded_obs > padded_target
    elif op == "<":
        return padded_obs < padded_target
    elif op == "==":
        return padded_obs == padded_target
    elif op == "!=":
        return padded_obs != padded_target
    raise ValueError(f"unsupported operator: {op}")


def satisfies_policy(obs_version_str: str, constraints: list[tuple[str, tuple[int, ...]]]) -> bool:
    """Check whether observed version string satisfies all policy constraints."""
    m = re.search(r"(\d+(?:\.\d+)*)", obs_version_str)
    if not m:
        raise ValueError(f"cannot extract version numbers from {obs_version_str!r}")
    obs_ver = tuple(int(x) for x in m.group(1).split("."))
    for op, target in constraints:
        if not compare_version(obs_ver, op, target):
            return False
    return True



def verify_provenance_families(manifest_data: dict) -> list[str]:
    """Require the reviewed upstream families independently of component rows."""
    families = manifest_data.get("provenance_families")
    if not isinstance(families, list):
        return ["release manifest missing provenance_families inventory"]
    by_id = {f.get("id"): f for f in families if isinstance(f, dict)}
    errors: list[str] = []
    for family_id in sorted(EXPECTED_PROVENANCE_FAMILIES):
        family = by_id.get(family_id)
        if not isinstance(family, dict):
            errors.append(f"provenance family missing: {family_id}")
            continue
        for key in ("name", "license", "origin", "revision", "notice_path", "evidence_path", "disposition"):
            if not isinstance(family.get(key), str) or not family[key].strip():
                errors.append(f"provenance family {family_id} missing {key}")
    return errors


def verify_release_locks(
    manifest_path: Path,
    release_index_path: Path = generate_sbom.PYTHON_RELEASE_INDEX_PATH,
    fetch_live_metadata: bool = False,
    metadata_timeout: float = 30.0,
    observed_toolchain: Path | dict | str | None = None,
) -> list[str]:
    """Validate the declared Python dependency lock and toolchain policy."""
    verify_release_locks.last_validated_py_lock = None
    verify_release_locks.last_validated_toolchain_policy = None
    verify_release_locks.last_validated_observed_toolchain = None
    errors: list[str] = []
    if not manifest_path.is_file():
        return [f"Release manifest file missing: {manifest_path}"]
    try:
        python_artifact_metadata = generate_sbom.resolve_pypi_release_index(
            release_index_path, fetch_live=fetch_live_metadata,
            timeout=metadata_timeout,
        )
    except generate_sbom.LockfileParseError as exc:
        return [str(exc)]

    py_lock_path: Path | None = None
    toolchain_policy: dict | None = None
    observed_data: dict | None = None
    try:
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
        errors.extend(verify_provenance_families(data))
        locks = data.get("release_locks", {})
        if not locks.get("status"):
            errors.append("release_locks missing status field")
        try:
            declared_path = generate_sbom.resolve_declared_lockfile(data, manifest_path)
        except generate_sbom.LockfileParseError as exc:
            errors.append(str(exc))
        else:
            try:
                packages = generate_sbom.parse_python_lockfile(
                    declared_path, python_artifact_metadata)
            except generate_sbom.LockfileParseError as exc:
                errors.append(str(exc))
            else:
                py_lock_path = declared_path
                rel_path = data["lockfiles"]["python"]
                print(f"Parsed Python lockfile {rel_path}: {len(packages)} package record(s)")

        toolchain_policy_raw = data.get("toolchain_policy")
        if toolchain_policy_raw is None:
            errors.append("release manifest missing toolchain_policy object")
        elif not isinstance(toolchain_policy_raw, dict):
            errors.append("release manifest toolchain_policy must be a JSON object")
        else:
            toolchain_policy = toolchain_policy_raw
            for comp in sorted(EXPECTED_TOOLCHAIN_COMPONENTS):
                if comp not in toolchain_policy:
                    errors.append(f"missing required component in toolchain_policy: {comp}")
            for comp in sorted(toolchain_policy.keys()):
                if comp not in EXPECTED_TOOLCHAIN_COMPONENTS:
                    errors.append(f"unknown component in toolchain_policy: {comp}")
            for comp, spec in sorted(toolchain_policy.items()):
                if comp not in EXPECTED_TOOLCHAIN_COMPONENTS:
                    continue
                if not isinstance(spec, str) or not spec.strip():
                    errors.append(f"toolchain_policy component '{comp}' must have a non-empty string value")
                    continue
                try:
                    parse_policy_spec(spec)
                except ValueError as exc:
                    errors.append(f"toolchain_policy component '{comp}' has invalid range syntax: {spec!r} ({exc})")

        if observed_toolchain is not None:
            if isinstance(observed_toolchain, dict):
                observed_data = observed_toolchain
            else:
                obs_path = Path(observed_toolchain)
                if not obs_path.is_file():
                    errors.append(f"observed toolchain file missing or unreadable: {obs_path}")
                else:
                    try:
                        observed_data = json.loads(obs_path.read_text(encoding="utf-8"))
                    except Exception as exc:
                        errors.append(f"failed to parse observed toolchain file {obs_path}: {exc}")
            if observed_data is not None:
                if not isinstance(observed_data, dict):
                    errors.append("observed toolchain must be a JSON object")
                elif isinstance(toolchain_policy, dict):
                    for comp in sorted(EXPECTED_TOOLCHAIN_COMPONENTS):
                        if comp not in observed_data:
                            errors.append(f"observed toolchain missing required component: {comp}")
                            continue
                        try:
                            obs_ver = extract_observed_version(observed_data[comp])
                        except ValueError as exc:
                            errors.append(f"observed toolchain component '{comp}' invalid: {exc}")
                            continue
                        spec = toolchain_policy.get(comp)
                        if isinstance(spec, str):
                            try:
                                if not satisfies_policy(obs_ver, parse_policy_spec(spec)):
                                    errors.append(
                                        f"observed toolchain component '{comp}' version '{obs_ver}' "
                                        f"does not satisfy policy '{spec}'"
                                    )
                            except ValueError:
                                pass
    except Exception as exc:
        errors.append(f"Failed to parse release manifest {manifest_path}: {exc}")

    verify_release_locks.last_validated_py_lock = py_lock_path
    verify_release_locks.last_validated_toolchain_policy = toolchain_policy
    verify_release_locks.last_validated_observed_toolchain = observed_data
    return errors


def _lock_file_entry_errors(spdx_data: dict,
                            expected: list[tuple[str, str, str]]) -> list[str]:
    """Require standards-conformant lockfile `files` records in the SBOM.

    For each declared lockfile the SBOM must contain a `files` entry with the
    stable repo-relative fileName and a SHA256 checksum exactly equal to the
    current snapshot's digest (issue #375).
    """
    errors: list[str] = []
    files = spdx_data.get("files")
    if not isinstance(files, list):
        return ["SPDX SBOM has no `files` entries; lockfile binding evidence missing"]
    by_filename = {
        entry.get("fileName"): entry
        for entry in files
        if isinstance(entry, dict)
    }
    for lockfile_identity, spdx_id, current_sha256 in expected:
        entry = by_filename.get(lockfile_identity)
        if not isinstance(entry, dict):
            errors.append(
                f"SPDX SBOM files entry missing for declared dependency lockfile "
                f"{lockfile_identity} (expected {spdx_id})"
            )
            continue
        if entry.get("SPDXID") != spdx_id:
            errors.append(
                f"SPDX SBOM files entry for {lockfile_identity} has SPDXID "
                f"{entry.get('SPDXID')!r}, expected {spdx_id!r}"
            )
        checksums = entry.get("checksums")
        sha_values = [
            c.get("checksumValue")
            for c in (checksums or [])
            if isinstance(c, dict) and c.get("algorithm") == "SHA256"
        ]
        if current_sha256 not in sha_values:
            errors.append(
                f"lockfile binding evidence mismatch for {lockfile_identity}: the SBOM was "
                f"generated from different lockfile bytes (SHA256 {sha_values or 'none'} vs "
                f"current {current_sha256}); regenerate the SBOM from the current lockfiles"
            )
    return errors


def _lock_relationship_errors(spdx_data: dict) -> list[str]:
    """Require the Python lock entry to relate to the root package."""
    required = {
        ("SPDXRef-File-python-requirements-lock", "SPDXRef-Package-nakagawa-recomp")
    }
    for rel in spdx_data.get("relationships", []):
        if not isinstance(rel, dict):
            continue
        if rel.get("relationshipType") == "DEPENDENCY_MANIFEST_OF":
            required.discard((rel.get("spdxElementId"), rel.get("relatedSpdxElement")))
    return [
        f"SPDX SBOM missing DEPENDENCY_MANIFEST_OF relationship: {element} -> {related}"
        for element, related in sorted(required)
    ]


def _python_hash_evidence_errors(spdx_data: dict, py_packages: list[dict]) -> list[str]:
    errors: list[str] = []
    spdx_packages = [package for package in spdx_data.get("packages", []) if isinstance(package, dict)]
    for expected in py_packages:
        matches = [
            package for package in spdx_packages
            if package.get("name") == expected["name"]
            and package.get("versionInfo") == expected["version"]
            and any(
                ref.get("referenceLocator") == expected["purl"]
                for ref in package.get("externalRefs", [])
                if isinstance(ref, dict)
            )
        ]
        if len(matches) != 1:
            errors.append(
                f"Python artifact hash metadata requires exactly one SPDX package for "
                f"{expected['purl']}, found {len(matches)}"
            )
            continue
        package = matches[0]
        checksums = package.get("checksums")
        actual_verified: Counter = Counter()
        if isinstance(checksums, list):
            for checksum in checksums:
                digest = checksum.get("checksumValue") if isinstance(checksum, dict) else None
                if not isinstance(checksum, dict) or checksum.get("algorithm") != "SHA256" \
                        or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
                    errors.append(
                        f"malformed verified Python artifact checksum for {expected['name']}=="
                        f"{expected['version']}"
                    )
                    continue
                actual_verified[digest] += 1
        else:
            errors.append(
                f"verified Python artifact checksum metadata missing for {expected['name']}=="
                f"{expected['version']}"
            )
        expected_verified = Counter(expected["verified_sha256"])
        if actual_verified != expected_verified:
            errors.append(
                f"verified Python artifact hash metadata mismatch for {expected['name']}=="
                f"{expected['version']}: expected SHA-256 {sorted(expected_verified.elements())}, "
                f"found {sorted(actual_verified.elements())}"
            )

        declared: Counter = Counter()
        verified: Counter = Counter()
        annotations = package.get("annotations")
        if not isinstance(annotations, list):
            errors.append(
                f"declared Python artifact hash metadata missing for {expected['name']}=="
                f"{expected['version']}"
            )
            annotations = []
        for annotation in annotations:
            if not isinstance(annotation, dict):
                continue
            comment = annotation.get("comment")
            if not isinstance(comment, str) \
                    or not comment.startswith(generate_sbom.PYTHON_HASH_EVIDENCE_SCHEMA + ";"):
                continue
            fields: dict[str, str] = {}
            malformed = False
            for field in comment.split(";")[1:]:
                if "=" not in field:
                    malformed = True
                    break
                key, value = field.split("=", 1)
                if key in fields:
                    malformed = True
                    break
                fields[key] = value
            if malformed or fields.get("name") != expected["name"] \
                    or fields.get("version") != expected["version"]:
                errors.append(
                    f"malformed Python artifact hash annotation for {expected['name']}=="
                    f"{expected['version']}"
                )
                continue
            digest = fields.get("sha256")
            if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
                errors.append(
                    f"malformed Python artifact hash annotation for {expected['name']}=="
                    f"{expected['version']}"
                )
                continue
            status = fields.get("status")
            base_fields = {"status", "name", "version", "sha256"}
            if status == "declared" and set(fields) == base_fields:
                declared[digest] += 1
            elif status == "verified" and set(fields) == base_fields | {"filename"}:
                verified[(fields["filename"], digest)] += 1
            else:
                errors.append(
                    f"malformed Python artifact hash annotation status for {expected['name']}=="
                    f"{expected['version']}"
                )
        expected_declared = Counter(expected["declared_sha256"])
        expected_verified_artifacts = Counter(
            (artifact["filename"], artifact["sha256"]) for artifact in expected["artifacts"]
        )
        if declared != expected_declared:
            errors.append(
                f"declared Python artifact hash metadata mismatch for {expected['name']}=="
                f"{expected['version']}: expected {sorted(expected_declared.elements())}, "
                f"found {sorted(declared.elements())}"
            )
        if verified != expected_verified_artifacts:
            errors.append(
                f"verified Python artifact annotation metadata mismatch for {expected['name']}=="
                f"{expected['version']}"
            )
    return errors


def verify_sbom_matches(
    spdx_path: Path,
    manifest_path: Path,
    py_lock_path: Path,
    release_index_path: Path = generate_sbom.PYTHON_RELEASE_INDEX_PATH,
    fetch_live_metadata: bool = False,
    metadata_timeout: float = 30.0,
) -> tuple[list[str], dict]:
    """Verify the SPDX document against the exact Python lock inventory."""
    errors: list[str] = []
    verified_digests: dict[str, str | None] = {"python": None}
    if not spdx_path.is_file():
        return errors, verified_digests
    try:
        spdx_data = json.loads(spdx_path.read_text(encoding="utf-8"))
        manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
        errors.extend(verify_provenance_families(manifest_data))
        try:
            metadata = generate_sbom.resolve_pypi_release_index(
                release_index_path, fetch_live=fetch_live_metadata,
                timeout=metadata_timeout,
            )
            snapshot = generate_sbom.snapshot_lockfile(py_lock_path, "Python")
            verified_digests["python"] = snapshot.sha256
            py_packages = generate_sbom._parse_python_lock_text(snapshot, py_lock_path, metadata)
        except generate_sbom.LockfileParseError as exc:
            errors.append(str(exc))
            return errors, verified_digests

        errors.extend(_python_hash_evidence_errors(spdx_data, py_packages))
        errors.extend(_lock_file_entry_errors(spdx_data, [(
            generate_sbom.PYTHON_LOCKFILE_IDENTITY,
            "SPDXRef-File-python-requirements-lock",
            snapshot.sha256,
        )]))
        errors.extend(_lock_relationship_errors(spdx_data))

        spdx_packages = spdx_data.get("packages", [])
        identity_counter: Counter = Counter(
            (purl, package.get("name"), package.get("versionInfo"))
            for package in spdx_packages
            if isinstance(package, dict)
            for purl in [
                next((ref.get("referenceLocator", "")
                      for ref in (package.get("externalRefs") or [])
                      if isinstance(ref, dict) and ref.get("referenceType") == "purl"), None)
            ]
            if purl is not None
        )
        family_names = {
            family.get("name") for family in manifest_data.get("provenance_families", [])
            if isinstance(family, dict)
        }
        package_names = {package.get("name") for package in spdx_packages if isinstance(package, dict)}
        for family_name in family_names:
            if family_name not in package_names:
                errors.append(f"provenance family {family_name} missing from SPDX SBOM")
        if manifest_data.get("name") not in package_names:
            errors.append(f"Root package {manifest_data.get('name')} missing from SPDX SBOM")

        expected_counter = Counter(
            (pkg["purl"], pkg["name"], pkg["version"]) for pkg in py_packages
        )
        if identity_counter != expected_counter:
            missing = expected_counter - identity_counter
            unexpected = identity_counter - expected_counter
            for (purl, name, version), _count in sorted(missing.items()):
                found = identity_counter.get((purl, name, version), 0)
                errors.append(
                    f"Lock dependency identity missing or under-represented in SPDX SBOM: "
                    f"{purl} (name {name!r}, version {version!r}) "
                    f"(expected {expected_counter[(purl, name, version)]} record(s), found {found})"
                )
            for (purl, name, version), _count in sorted(unexpected.items()):
                expected_count = expected_counter.get((purl, name, version), 0)
                found = identity_counter[(purl, name, version)]
                label = (
                    "Lock dependency identity over-represented in SPDX SBOM"
                    if expected_count > 0 else "Unexpected package-manager package record in SPDX SBOM"
                )
                errors.append(
                    f"{label}: {purl} (name {name!r}, version {version!r}) "
                    f"(expected {expected_count} record(s), found {found}); the SBOM's "
                    "package-manager records must equal the lockfile dependency inventory exactly"
                )
    except Exception as exc:
        errors.append(f"Failed to verify SPDX SBOM {spdx_path}: {exc}")
    return errors, verified_digests


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=ROOT / "assets" / "release_manifest.json")
    parser.add_argument("--py-lock", type=Path, default=ROOT / "tools" / "requirements-lock.txt")
    parser.add_argument(
        "--trusted-metadata", dest="release_index", type=Path,
        default=generate_sbom.PYTHON_RELEASE_INDEX_PATH,
        help="Trusted PyPI name/version/filename/SHA-256 metadata snapshot",
    )
    parser.add_argument(
        "--fetch-live-metadata", action="store_true",
        default=generate_sbom.live_metadata_requested(),
        help="Fetch exact pinned releases from the official PyPI JSON API before verification",
    )
    parser.add_argument(
        "--metadata-timeout", type=float, default=30.0,
        help="Timeout in seconds for each live PyPI metadata request",
    )
    parser.add_argument(
        "--observed-toolchain", type=Path, default=None,
        help="Path to observed toolchain JSON to verify against toolchain_policy",
    )
    parser.add_argument("--spdx", type=Path, help="Path to SPDX 2.3 JSON SBOM to verify")
    args = parser.parse_args(argv)
    errors = verify_release_locks(
        args.manifest, release_index_path=args.release_index,
        fetch_live_metadata=args.fetch_live_metadata,
        metadata_timeout=args.metadata_timeout,
        observed_toolchain=args.observed_toolchain,
    )
    verified_digests: dict = {"python": None}
    if args.spdx:
        validated_lock = verify_release_locks.last_validated_py_lock
        if validated_lock is not None and validated_lock.resolve() != args.py_lock.resolve():
            errors.append(
                f"Python lockfile passed via --py-lock ({args.py_lock}) is not the lockfile declared "
                f"by the manifest ({validated_lock})"
            )
        match_errors, verified_digests = verify_sbom_matches(
            args.spdx, args.manifest, args.py_lock,
            release_index_path=args.release_index,
            fetch_live_metadata=args.fetch_live_metadata,
            metadata_timeout=args.metadata_timeout,
        )
        errors.extend(match_errors)
    if errors:
        seen = set()
        for err in errors:
            if err not in seen:
                seen.add(err)
                print(f"SBOM Verification FAIL: {err}", file=sys.stderr)
        return 1
    checked_items = ["Release dependency lockfile", "artifact hashes", "toolchain policy"]
    if args.observed_toolchain:
        checked_items.append("observed toolchain")
    if args.spdx:
        checked_items.append("SBOM elements")
        py_digest = verified_digests["python"]
        if py_digest is None:
            return 1
        print(
            f"SBOM Verification: OK ({', '.join(checked_items)} verified)\n"
            f"  python lockfile sha256: {py_digest}"
        )
    else:
        print(f"SBOM Verification: OK ({', '.join(checked_items)} verified)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
