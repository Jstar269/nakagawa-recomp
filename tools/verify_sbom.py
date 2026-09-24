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
    "shadcn-ui",
    "vfpu",
})


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


def verify_dashboard_toolchain_compatibility(pkg_json_path: Path) -> list[str]:
    errors = []
    if not pkg_json_path.is_file():
        return [f"Dashboard package.json missing: {pkg_json_path}"]

    try:
        data = json.loads(pkg_json_path.read_text(encoding="utf-8"))
        dev_deps = data.get("devDependencies", {})

        ts_ver = dev_deps.get("typescript", "")
        eslint_ver = dev_deps.get("eslint", "")

        if ts_ver.startswith("^7.") or ts_ver.startswith("7."):
            errors.append(
                f"Incompatible TypeScript version '{ts_ver}' in {pkg_json_path}. "
                "typescript-eslint v8 requires typescript < 6.1.0 (Issue #248)."
            )

        if eslint_ver.startswith("^10.") or eslint_ver.startswith("10."):
            errors.append(
                f"Incompatible ESLint version '{eslint_ver}' in {pkg_json_path}. "
                "eslint-config-next 16.x requires eslint < 10.0.0 (Issue #248)."
            )
    except Exception as exc:
        errors.append(f"Failed to verify dashboard toolchain compatibility in {pkg_json_path}: {exc}")

    return errors


def verify_release_locks(
    manifest_path: Path,
    release_index_path: Path = generate_sbom.PYTHON_RELEASE_INDEX_PATH,
    fetch_live_metadata: bool = False,
    metadata_timeout: float = 30.0,
) -> list[str]:
    """Parse and validate the declared dependency lockfiles, not just their existence.

    The declared lockfile paths are taken from the release manifest itself and
    each lockfile is fully parsed with the fail-closed parsers; a malformed,
    unsupported, truncated, or unreadable lockfile is a verification error
    (issue #375). A lockfile that parses to an intentionally empty inventory
    is valid and passes.
    """
    # Reset the module-level validation state first so an early return can
    # never leave a previous call's paths behind (issue #375 hygiene).
    verify_release_locks.last_validated_npm_lock = None
    verify_release_locks.last_validated_py_lock = None
    errors = []
    if not manifest_path.is_file():
        return [f"Release manifest file missing: {manifest_path}"]
    try:
        python_artifact_metadata = generate_sbom.resolve_pypi_release_index(
            release_index_path,
            fetch_live=fetch_live_metadata,
            timeout=metadata_timeout,
        )
    except generate_sbom.LockfileParseError as exc:
        return [str(exc)]

    npm_lock_path: Path | None = None
    py_lock_path: Path | None = None
    try:
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
        errors.extend(verify_provenance_families(data))
        locks = data.get("release_locks", {})
        status = locks.get("status")
        if not status:
            errors.append("release_locks missing status field")
        lockfiles = data.get("lockfiles", {})
        # Declared lockfile paths are repo-root relative; the canonical
        # manifest lives at <root>/assets/release_manifest.json, so the repo
        # root is the parent of the manifest's assets directory.
        repo_root = manifest_path.parent.parent

        for ecosystem in ("npm", "python"):
            rel_path = lockfiles.get(ecosystem)
            if not rel_path or not isinstance(rel_path, str):
                errors.append(f"{ecosystem} lockfile path missing from release manifest lockfiles")
                continue
            declared_path = repo_root / rel_path
            if not declared_path.is_file():
                errors.append(f"{ecosystem} lockfile missing or unreadable: {rel_path}")
                continue
            # Parse it now: existence alone proves nothing about the dependency
            # inventory (issue #375).
            try:
                if ecosystem == "npm":
                    parse_packages = generate_sbom.parse_npm_lockfile(declared_path)
                    npm_lock_path = declared_path
                else:
                    parse_packages = generate_sbom.parse_python_lockfile(
                        declared_path, python_artifact_metadata)
                    py_lock_path = declared_path
            except generate_sbom.LockfileParseError as exc:
                errors.append(str(exc))
            else:
                print(
                    f"Parsed {ecosystem} lockfile {rel_path}: {len(parse_packages)} package record(s)"
                )

        pkg_json_path = repo_root / "interface" / "package.json"
        errors.extend(verify_dashboard_toolchain_compatibility(pkg_json_path))
    except Exception as exc:
        errors.append(f"Failed to parse release manifest {manifest_path}: {exc}")

    # Expose the successfully validated lockfile paths for callers that need to
    # verify an SBOM against exactly these files.
    verify_release_locks.last_validated_npm_lock = npm_lock_path
    verify_release_locks.last_validated_py_lock = py_lock_path
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
    """Require each lockfile files entry to be a DEPENDENCY_MANIFEST_OF the root package."""
    errors: list[str] = []
    lock_ids = {"SPDXRef-File-npm-package-lock", "SPDXRef-File-python-requirements-lock"}
    required = {(lid, "SPDXRef-Package-nakagawa-recomp") for lid in lock_ids}
    for rel in spdx_data.get("relationships", []):
        if not isinstance(rel, dict):
            continue
        if rel.get("relationshipType") == "DEPENDENCY_MANIFEST_OF":
            required.discard((rel.get("spdxElementId"), rel.get("relatedSpdxElement")))
    for spdx_element_id, related in sorted(required):
        errors.append(
            f"SPDX SBOM missing DEPENDENCY_MANIFEST_OF relationship: {spdx_element_id} -> {related}"
        )
    return errors


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
    npm_lock_path: Path,
    py_lock_path: Path,
    release_index_path: Path = generate_sbom.PYTHON_RELEASE_INDEX_PATH,
    fetch_live_metadata: bool = False,
    metadata_timeout: float = 30.0,
) -> tuple[list[str], dict]:
    """Verify the SPDX 2.3 SBOM against the exact current lockfile inventory.

    The Counter of package-manager dependency identities (purl, name,
    versionInfo) recorded in the SBOM must be exactly equal to the Counter
    derived from the parsed npm/Python lock inventories: full-tuple equality
    with exact multiplicity, in both directions. A correct name with the
    wrong version, a removed duplicate installation, an over-represented
    duplicate installation, and any PACKAGE-MANAGER tuple outside the lock
    inventory (including one reusing a valid purl with a wrong name or
    version) all fail. The SBOM's `files` checksum entries for the declared
    lockfiles must exactly match the current lockfile bytes, so a stale SBOM
    cannot pass after the lockfiles change (issue #375).

    Returns (errors, verified_digests) where verified_digests carries the
    sha256 of each lockfile snapshot whose inventory and checksum binding
    were actually verified above, so callers can report those digests
    without rereading the files (issue #375 revision 5: no read after the
    verification decision).
    """
    errors = []
    verified_digests: dict = {"npm": None, "python": None}
    if not spdx_path.is_file():
        return errors, verified_digests

    npm_pkgs: list[dict] = []
    py_pkgs: list[dict] = []
    try:
        spdx_data = json.loads(spdx_path.read_text(encoding="utf-8"))
        manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
        errors.extend(verify_provenance_families(manifest_data))
        try:
            python_artifact_metadata = generate_sbom.resolve_pypi_release_index(
                release_index_path,
                fetch_live=fetch_live_metadata,
                timeout=metadata_timeout,
            )
        except generate_sbom.LockfileParseError as exc:
            errors.append(str(exc))
            return errors, verified_digests

        # One stable snapshot per lockfile: the dependency inventory, the
        # identity checks, and the checksum comparison below all refer to the
        # same captured bytes (issue #375 revision 3).
        npm_snap = generate_sbom.snapshot_lockfile(npm_lock_path, "npm")
        py_snap = generate_sbom.snapshot_lockfile(py_lock_path, "Python")
        # The digests that describe the snapshots every check below operated
        # on; carried to the caller instead of a post-verification reread.
        verified_digests = {"npm": npm_snap.sha256, "python": py_snap.sha256}
        try:
            npm_pkgs = generate_sbom._parse_npm_lock_text(npm_snap, npm_lock_path)
        except generate_sbom.LockfileParseError as exc:
            errors.append(str(exc))
        try:
            py_pkgs = generate_sbom._parse_python_lock_text(
                py_snap, py_lock_path, python_artifact_metadata)
        except generate_sbom.LockfileParseError as exc:
            errors.append(str(exc))
        errors.extend(_python_hash_evidence_errors(spdx_data, py_pkgs))

        # Standards-conformant lock binding: schema-defined files entries with
        # the exact repo-relative identity and SHA256 of the current snapshots.
        errors.extend(_lock_file_entry_errors(spdx_data, [
            (generate_sbom.NPM_LOCKFILE_IDENTITY, "SPDXRef-File-npm-package-lock", npm_snap.sha256),
            (generate_sbom.PYTHON_LOCKFILE_IDENTITY, "SPDXRef-File-python-requirements-lock", py_snap.sha256),
        ]))
        errors.extend(_lock_relationship_errors(spdx_data))

        # Exact identities with multiplicity: (purl, name, version) pairs from
        # the SBOM's PACKAGE-MANAGER purl external refs. A bare set of names
        # cannot distinguish versions nor prove duplicate installations.
        spdx_packages = spdx_data.get("packages", [])
        identity_counter: Counter = Counter(
            (purl, p.get("name"), p.get("versionInfo"))
            for p in spdx_packages
            if isinstance(p, dict)
            for purl in [
                next(
                    (
                        ref.get("referenceLocator", "")
                        for ref in (p.get("externalRefs") or [])
                        if isinstance(ref, dict) and ref.get("referenceType") == "purl"
                    ),
                    None,
                )
            ]
            if purl is not None
        )

        family_names = {
            family.get("name")
            for family in manifest_data.get("provenance_families", [])
            if isinstance(family, dict)
        }
        pkg_names = {p.get("name") for p in spdx_packages if isinstance(p, dict)}
        for family_name in family_names:
            if family_name not in pkg_names:
                errors.append(f"provenance family {family_name} missing from SPDX SBOM")

        if manifest_data.get("name") not in pkg_names:
            errors.append(f"Root package {manifest_data.get('name')} missing from SPDX SBOM")

        expected_identities: list[tuple[str, str, str]] = [
            (pkg["purl"], pkg["name"], pkg["version"]) for pkg in [*npm_pkgs, *py_pkgs]
        ]
        # Full-tuple Counter equality in BOTH directions: the SBOM's
        # package-manager dependency Counter must equal the lock-derived
        # Counter exactly — under-representation (removed/wrong-version
        # dependencies), over-representation (extra duplicate records), and
        # any PACKAGE-MANAGER record not in the lock inventory (unrelated
        # purl, or a malformed tuple reusing a valid purl with a wrong name
        # or version) all fail. Membership is never reduced to purl-only:
        # every (purl, name, version) tuple participates in the exact
        # equality. The root package, provenance-family packages, and
        # release-manifest components carry no PACKAGE-MANAGER purl and are
        # never mistaken for lock dependencies; outside the declared
        # lockfiles there is no legitimate class of non-lock PACKAGE-MANAGER
        # record.
        expected_counter = Counter(expected_identities)
        if identity_counter != expected_counter:
            missing = expected_counter - identity_counter
            unexpected = identity_counter - expected_counter
            for (purl, name, version), _shortfall in sorted(missing.items()):
                found = identity_counter.get((purl, name, version), 0)
                errors.append(
                    f"Lock dependency identity missing or under-represented in SPDX SBOM: "
                    f"{purl} (name {name!r}, version {version!r}) "
                    f"(expected {expected_counter[(purl, name, version)]} record(s), found {found})"
                )
            for (purl, name, version), _surplus in sorted(unexpected.items()):
                expected_count = expected_counter.get((purl, name, version), 0)
                found = identity_counter[(purl, name, version)]
                # A surplus of an identity the lock declares at all is an
                # over-representation; a tuple the lock never declares (a
                # foreign purl, or a malformed record reusing a valid purl
                # with a wrong name or version) is an unexpected record.
                label = (
                    "Lock dependency identity over-represented in SPDX SBOM"
                    if expected_count > 0
                    else "Unexpected package-manager package record in SPDX SBOM"
                )
                errors.append(
                    f"{label}: {purl} (name {name!r}, version {version!r}) "
                    f"(expected {expected_count} record(s), found {found}); the SBOM's "
                    "package-manager records must equal the lockfile dependency "
                    "inventory exactly"
                )

    except Exception as exc:
        errors.append(f"Failed to verify SPDX SBOM {spdx_path}: {exc}")

    return errors, verified_digests


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=ROOT / "assets" / "release_manifest.json")
    parser.add_argument("--npm-lock", type=Path, default=ROOT / "interface" / "package-lock.json")
    parser.add_argument("--py-lock", type=Path, default=ROOT / "tools" / "requirements-lock.txt")
    parser.add_argument(
        "--trusted-metadata",
        dest="release_index",
        type=Path,
        default=generate_sbom.PYTHON_RELEASE_INDEX_PATH,
        help="Trusted PyPI name/version/filename/SHA-256 metadata snapshot",
    )
    parser.add_argument(
        "--fetch-live-metadata",
        action="store_true",
        default=generate_sbom.live_metadata_requested(),
        help="Fetch exact pinned releases from the official PyPI JSON API before verification",
    )
    parser.add_argument(
        "--metadata-timeout",
        type=float,
        default=30.0,
        help="Timeout in seconds for each live PyPI metadata request",
    )
    parser.add_argument("--spdx", type=Path, help="Path to SPDX 2.3 JSON SBOM to verify")

    args = parser.parse_args(argv)

    errors = verify_release_locks(
        args.manifest,
        release_index_path=args.release_index,
        fetch_live_metadata=args.fetch_live_metadata,
        metadata_timeout=args.metadata_timeout,
    )
    # Bind the verified SBOM to the exact lockfile bytes: when the lockfiles
    # declared by the manifest were just parsed successfully, require the SBOM
    # to have been generated from precisely those files (sha256 evidence), so a
    # stale SBOM from different lockfile content cannot pass (issue #375).
    if args.spdx:
        if (
            verify_release_locks.last_validated_npm_lock is not None
            and verify_release_locks.last_validated_npm_lock.resolve() != args.npm_lock.resolve()
        ):
            errors.append(
                f"npm lockfile passed via --npm-lock ({args.npm_lock}) is not the lockfile declared "
                f"by the manifest ({verify_release_locks.last_validated_npm_lock})"
            )
        if (
            verify_release_locks.last_validated_py_lock is not None
            and verify_release_locks.last_validated_py_lock.resolve() != args.py_lock.resolve()
        ):
            errors.append(
                f"Python lockfile passed via --py-lock ({args.py_lock}) is not the lockfile declared "
                f"by the manifest ({verify_release_locks.last_validated_py_lock})"
            )
        match_errors, verified_digests = verify_sbom_matches(
            args.spdx,
            args.manifest,
            args.npm_lock,
            args.py_lock,
            release_index_path=args.release_index,
            fetch_live_metadata=args.fetch_live_metadata,
            metadata_timeout=args.metadata_timeout,
        )
        errors.extend(match_errors)

    if errors:
        seen = set()
        for err in errors:
            if err in seen:
                continue
            seen.add(err)
            print(f"SBOM Verification FAIL: {err}", file=sys.stderr)
        return 1

    if args.spdx:
        # Report the digests of the snapshots that were actually verified —
        # never a fresh read, which could describe bytes other than the ones
        # whose inventory and checksum binding just passed (issue #375).
        npm_digest = verified_digests["npm"]
        py_digest = verified_digests["python"]
        if npm_digest is None or py_digest is None:
            # Unreachable when errors is empty (snapshot failure always adds
            # an error); refuse to print an unverified digest anyway.
            return 1
        print(
            "SBOM Verification: OK (All release dependency locks and SBOM elements verified)\n"
            f"  npm lockfile sha256: {npm_digest}\n"
            f"  python lockfile sha256: {py_digest}"
        )
    else:
        print("SBOM Verification: OK (All release dependency locks verified)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
