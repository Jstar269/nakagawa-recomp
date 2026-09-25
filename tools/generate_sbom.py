#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Generate SPDX 2.3, SPDX 3.0.1 JSON-LD, and CycloneDX 1.5 SBOM artifacts for Nakagawa Recomp releases."""

from __future__ import annotations

import argparse
from datetime import date
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from typing import Any
from urllib.parse import quote
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parent.parent

SPDX_VERSION = "SPDX-2.3"
DATA_LICENSE = "CC0-1.0"
DOCUMENT_NAME = "nakagawa-recomp-sbom"
DOCUMENT_NAMESPACE_BASE = "https://spdx.org/spdxdocs/nakagawa-recomp"

PYTHON_RELEASE_INDEX_SCHEMA_VERSION = 1
PYTHON_HASH_EVIDENCE_SCHEMA = "nakagawa-python-artifact-hash:v1"
PYTHON_RELEASE_INDEX_IDENTITY = "assets/pypi_tool_metadata_2026-09-24.json"
PYTHON_RELEASE_INDEX_PATH = ROOT / PYTHON_RELEASE_INDEX_IDENTITY
SBOM_CREATED = "2026-08-06T00:00:00Z"
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
PYTHON_NAME_PATTERN = re.compile(r"[a-zA-Z0-9_.-]+")
PYTHON_VERSION_PATTERN = re.compile(r"[a-zA-Z0-9_.-]+")
PythonArtifactMetadata = dict[tuple[str, str], list[dict[str, str]]]


class LockfileParseError(Exception):
    """A declared dependency lockfile could not be parsed completely.

    Raised on unreadable files, malformed JSON, unsupported schema/version,
    missing or invalid required records, and requirement lines that do not
    conform to the repository's declared lockfile format. Callers must abort
    rather than treat the failure as "no dependencies" (issue #375).
    """


def _normalize_python_name(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


def live_metadata_requested() -> bool:
    return os.environ.get("NAKAGAWA_SBOM_FETCH_LIVE_METADATA", "").strip().lower() in {
        "1", "true", "yes",
    }


def _parse_pypi_release_index(document: object, source_label: str) -> PythonArtifactMetadata:
    if not isinstance(document, dict):
        raise LockfileParseError(f"trusted Python metadata {source_label} must be a JSON object")
    expected_keys = {"schema_version", "retrieved_utc", "sources"}
    if set(document) != expected_keys:
        raise LockfileParseError(
            f"trusted Python metadata {source_label} must contain exactly "
            f"{sorted(expected_keys)}"
        )
    if document["schema_version"] != PYTHON_RELEASE_INDEX_SCHEMA_VERSION:
        raise LockfileParseError(
            f"trusted Python metadata {source_label} has unsupported schema_version "
            f"{document['schema_version']!r}"
        )
    retrieved_utc = document["retrieved_utc"]
    if not isinstance(retrieved_utc, str):
        raise LockfileParseError(
            f"trusted Python metadata {source_label} has invalid retrieved_utc"
        )
    try:
        parsed_date = date.fromisoformat(retrieved_utc)
    except ValueError as exc:
        raise LockfileParseError(
            f"trusted Python metadata {source_label} has invalid retrieved_utc {retrieved_utc!r}"
        ) from exc
    if parsed_date.isoformat() != retrieved_utc:
        raise LockfileParseError(
            f"trusted Python metadata {source_label} retrieved_utc must use YYYY-MM-DD"
        )
    sources = document["sources"]
    if not isinstance(sources, list) or not sources:
        raise LockfileParseError(
            f"trusted Python metadata {source_label} has no non-empty sources list"
        )

    result: PythonArtifactMetadata = {}
    for source_index, source in enumerate(sources, start=1):
        label = f"{source_label} source {source_index}"
        if not isinstance(source, dict) or set(source) != {"url", "name", "version", "artifacts"}:
            raise LockfileParseError(
                f"trusted Python metadata {label} must contain exactly url, name, version, and artifacts"
            )
        name = source["name"]
        version = source["version"]
        if not isinstance(name, str) or not PYTHON_NAME_PATTERN.fullmatch(name):
            raise LockfileParseError(f"trusted Python metadata {label} has invalid name {name!r}")
        if not isinstance(version, str) or not PYTHON_VERSION_PATTERN.fullmatch(version):
            raise LockfileParseError(
                f"trusted Python metadata {label} has invalid version {version!r}"
            )
        expected_url = (
            f"https://pypi.org/pypi/{quote(name, safe='')}/{quote(version, safe='')}/json"
        )
        if source["url"] != expected_url:
            raise LockfileParseError(
                f"trusted Python metadata {label} URL {source['url']!r} does not match "
                f"the official release URL {expected_url!r}"
            )
        key = (_normalize_python_name(name), version)
        if key in result:
            raise LockfileParseError(
                f"trusted Python metadata {source_label} has duplicate release {name}=={version}"
            )
        artifacts = source["artifacts"]
        if not isinstance(artifacts, list) or not artifacts:
            raise LockfileParseError(
                f"trusted Python metadata {label} has no wheel or sdist artifact records"
            )
        filenames: set[str] = set()
        records: list[dict[str, str]] = []
        for artifact_index, artifact in enumerate(artifacts, start=1):
            artifact_label = f"{label} artifact {artifact_index}"
            if not isinstance(artifact, dict) or set(artifact) != {"filename", "sha256"}:
                raise LockfileParseError(
                    f"trusted Python metadata {artifact_label} must contain exactly filename and sha256"
                )
            filename = artifact["filename"]
            digest = artifact["sha256"]
            if (
                not isinstance(filename, str)
                or not filename
                or "/" in filename
                or "\\" in filename
                or not (filename.endswith(".whl") or filename.endswith(".tar.gz"))
            ):
                raise LockfileParseError(
                    f"trusted Python metadata {artifact_label} has invalid wheel/sdist filename {filename!r}"
                )
            if filename in filenames:
                raise LockfileParseError(
                    f"trusted Python metadata {label} has duplicate filename {filename}"
                )
            if not isinstance(digest, str) or not SHA256_PATTERN.fullmatch(digest):
                raise LockfileParseError(
                    f"trusted Python metadata {artifact_label} has malformed SHA-256 hash {digest!r}; "
                    "expected 64 lowercase hexadecimal characters"
                )
            filenames.add(filename)
            records.append({"filename": filename, "sha256": digest})
        result[key] = records
    return result


def load_pypi_release_index(path: Path) -> PythonArtifactMetadata:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise LockfileParseError(
            f"cannot read trusted Python artifact metadata {path}: {exc}"
        ) from exc
    try:
        document = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise LockfileParseError(
            f"trusted Python artifact metadata {path} is not valid UTF-8 JSON: {exc}"
        ) from exc
    return _parse_pypi_release_index(document, str(path))


def fetch_live_python_metadata(
    releases: list[tuple[str, str]], timeout: float = 30.0,
) -> PythonArtifactMetadata:
    if timeout <= 0:
        raise LockfileParseError("trusted Python metadata fetch timeout must be positive")
    sources: list[dict] = []
    for name, version in sorted(set(releases)):
        url = f"https://pypi.org/pypi/{quote(name, safe='')}/{quote(version, safe='')}/json"
        request = Request(
            url,
            headers={
                "Accept": "application/json",
                "User-Agent": "nakagawa-recomp-sbom-verifier/1",
            },
        )
        try:
            with urlopen(request, timeout=timeout) as response:
                raw = response.read(8 * 1024 * 1024 + 1)
            if len(raw) > 8 * 1024 * 1024:
                raise LockfileParseError(
                    f"live PyPI metadata for {name}=={version} exceeds the 8 MiB safety limit"
                )
            document = json.loads(raw.decode("utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise LockfileParseError(
                f"cannot fetch live PyPI metadata for {name}=={version} from {url}: {exc}"
            ) from exc
        info = document.get("info") if isinstance(document, dict) else None
        urls = document.get("urls") if isinstance(document, dict) else None
        if not isinstance(info, dict) or not isinstance(urls, list):
            raise LockfileParseError(f"live PyPI metadata for {name}=={version} has an invalid shape")
        if _normalize_python_name(str(info.get("name", ""))) != _normalize_python_name(name) \
                or info.get("version") != version:
            raise LockfileParseError(
                f"live PyPI metadata identity does not match requested release {name}=={version}"
            )
        artifacts: list[dict] = []
        for artifact in urls:
            if not isinstance(artifact, dict) \
                    or artifact.get("packagetype") not in {"bdist_wheel", "sdist"}:
                continue
            digests = artifact.get("digests")
            artifacts.append({
                "filename": artifact.get("filename"),
                "sha256": digests.get("sha256") if isinstance(digests, dict) else None,
            })
        sources.append({
            "url": url,
            "name": info["name"],
            "version": info["version"],
            "artifacts": artifacts,
        })
    return _parse_pypi_release_index(
        {"schema_version": 1, "retrieved_utc": date.today().isoformat(), "sources": sources},
        "live PyPI API",
    )


def resolve_pypi_release_index(
    path: Path, fetch_live: bool = False, timeout: float = 30.0,
) -> PythonArtifactMetadata:
    snapshot = load_pypi_release_index(path)
    if not fetch_live:
        return snapshot
    return fetch_live_python_metadata(list(snapshot), timeout=timeout)


#: Schema version of the generated lockfile evidence binding.
LOCKFILE_EVIDENCE_SCHEMA_VERSION = 1

# Canonical repo-relative identities of the declared dependency lockfiles.
# These are the only identities release evidence may carry: machine-local
# absolute or temp paths must never reach a published SBOM.
PYTHON_LOCKFILE_IDENTITY = "tools/requirements-lock.txt"


class LockfileSnapshot:
    """One immutable byte snapshot of a lockfile, read exactly once.

    Parsing, checksum computation, and evidence all derive from the same
    captured bytes, so a lockfile changing between accesses can never make the
    dependency inventory describe bytes A while recorded evidence describes
    bytes B (issue #375 revision 3).
    """

    __slots__ = ("path", "raw", "sha256", "lockfile_identity")

    def __init__(self, path: Path, raw: bytes, lockfile_identity: str | None) -> None:
        self.path = path
        self.raw = raw
        self.sha256 = hashlib.sha256(raw).hexdigest()
        self.lockfile_identity = lockfile_identity

    @property
    def size(self) -> int:
        return len(self.raw)

    @property
    def text(self) -> str:
        """UTF-8 decode of the snapshot; raises LockfileParseError on bad bytes."""
        try:
            return self.raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise LockfileParseError(
                f"lockfile is not valid UTF-8: {self.path}: {exc}"
            ) from exc


def _repo_relative_identity(path: Path, repo_root: Path | None) -> str | None:
    """Canonical repo-relative identity for release evidence, or None.

    Paths outside the repository root have no canonical repo-relative identity
    and record None rather than leaking a machine-local absolute path.
    """
    if repo_root is None:
        return None
    try:
        resolved = path.resolve()
        root = repo_root.resolve()
    except OSError:
        return None
    try:
        return resolved.relative_to(root).as_posix()
    except ValueError:
        return None


def snapshot_lockfile(lock_path: Path, kind: str, repo_root: Path | None = None) -> LockfileSnapshot:
    """Read a lockfile's bytes exactly once and capture its immutable snapshot."""
    try:
        if not lock_path.is_file():
            raise LockfileParseError(f"{kind} lockfile is not an existing regular file: {lock_path}")
        raw = lock_path.read_bytes()
    except LockfileParseError:
        raise
    except OSError as exc:
        raise LockfileParseError(f"cannot read {kind} lockfile {lock_path}: {exc}") from exc
    return LockfileSnapshot(lock_path, raw, _repo_relative_identity(lock_path, repo_root))


def _lock_evidence_entry(snap: LockfileSnapshot) -> dict:
    """Evidence summary for one snapshot; identity is repo-relative or None.

    This is the internal evidence record derived from the byte snapshot (used
    for reporting and tests); the SBOM itself carries the standards-conformant
    `files` checksum binding built by build_lock_file_entries().
    """
    return {
        "lockfile": snap.lockfile_identity,
        "sha256": snap.sha256,
        "size": snap.size,
    }


def resolve_declared_lockfile(manifest_data: object, manifest_path: Path) -> Path:
    """Resolve the Python dependency lock declared by the release manifest."""
    if not isinstance(manifest_data, dict):
        raise LockfileParseError(
            f"release manifest {manifest_path} must contain a JSON object"
        )
    lockfiles = manifest_data.get("lockfiles")
    if not isinstance(lockfiles, dict):
        raise LockfileParseError(
            f"release manifest {manifest_path} has no `lockfiles` object"
        )
    if set(lockfiles) != {"python"}:
        raise LockfileParseError(
            f"release manifest {manifest_path} lockfiles must declare only Python"
        )
    rel_path = lockfiles.get("python")
    if not isinstance(rel_path, str) or not rel_path:
        raise LockfileParseError(
            f"release manifest {manifest_path}: Python lockfiles entry is "
            f"missing or not a non-empty string ({rel_path!r})"
        )
    repo_root = manifest_path.parent.parent.resolve()
    declared = (repo_root / rel_path).resolve()
    try:
        declared.relative_to(repo_root)
    except ValueError:
        raise LockfileParseError(
            f"release manifest {manifest_path}: Python lockfiles entry "
            f"{rel_path!r} escapes the repository root"
        ) from None
    if not declared.is_file():
        raise LockfileParseError(
            f"release manifest {manifest_path}: declared Python lockfile "
            f"{rel_path!r} is missing ({declared})"
        )
    return declared


def parse_lockfiles(lock_path: Path,
                    repo_root: Path | None = None,
                    python_artifact_metadata: PythonArtifactMetadata | None = None) -> dict:
    """Parse the Python lock and derive its immutable checksum evidence."""
    snap = snapshot_lockfile(lock_path, "Python", repo_root)
    if python_artifact_metadata is None:
        python_artifact_metadata = load_pypi_release_index(PYTHON_RELEASE_INDEX_PATH)
    packages = _parse_python_lock_text(snap, lock_path, python_artifact_metadata)
    lock_files = build_lock_file_entries(snap)
    lock_relationships = build_lock_relationship_entries(lock_files)
    return {
        "py_packages": packages,
        "lock_evidence": {
            "schema_version": LOCKFILE_EVIDENCE_SCHEMA_VERSION,
            "python": _lock_evidence_entry(snap),
        },
        "lock_files": lock_files,
        "lock_relationships": lock_relationships,
    }


def build_lock_file_entries(snap: LockfileSnapshot) -> list[dict]:
    """Build the SPDX file entry binding the Python dependency lock bytes."""
    if snap.lockfile_identity is None:
        raise LockfileParseError(
            f"lockfile {snap.path} is outside the repository root; release evidence "
            f"requires the canonical repo-relative lockfile identity ({PYTHON_LOCKFILE_IDENTITY})"
        )
    return [{
        "SPDXID": "SPDXRef-File-python-requirements-lock",
        "fileName": snap.lockfile_identity,
        "checksums": [{"algorithm": "SHA256", "checksumValue": snap.sha256}],
        "copyrightText": "NOASSERTION",
        "licenseConcluded": "NOASSERTION",
        "licenseInfoInFiles": ["NOASSERTION"],
        "comment": (
            "Declared dependency lockfile; binding evidence for the generated "
            "dependency inventory (sha256 over the exact lockfile bytes)."
        ),
    }]


def build_lock_relationship_entries(lock_files: list[dict]) -> list[dict]:
    """Relate each lockfile to the root package (manifest -> package)."""
    root_pkg_id = "SPDXRef-Package-nakagawa-recomp"
    return [
        {
            "spdxElementId": entry["SPDXID"],
            "relationshipType": "DEPENDENCY_MANIFEST_OF",
            "relatedSpdxElement": root_pkg_id,
            "comment": "Declared dependency lockfile binding evidence.",
        }
        for entry in lock_files
    ]


def _normalize_spdx_license(raw: str) -> str:
    cleaned = raw.strip()
    mapping = {
        "mit": "MIT",
        "bsd-3-clause": "BSD-3-Clause",
        "gpl-3.0-or-later": "GPL-3.0-or-later",
        "gplv3+": "GPL-3.0-or-later",
        "gplv3": "GPL-3.0-only",
        "apache-2.0": "Apache-2.0",
        "zlib": "Zlib",
        "lgpl-2.1-or-later": "LGPL-2.1-or-later",
    }
    return mapping.get(cleaned.lower(), cleaned if cleaned in mapping.values() else "NOASSERTION")


def recorded_python_package_licenses(repo_root: Path = ROOT) -> dict[str, str]:
    """Return the checked-in name -> SPDX license map for the declared Python lockfile.

    The release SBOM must not depend on whichever Python environment happens to
    run generation, so the recorded value from the component inventory is
    authoritative; the live environment lookup stays a fallback for a package
    that was added to the lockfile without a recorded license.
    """
    from package_notices import load_component_inventory

    inventory = load_component_inventory()
    recorded = inventory.get("python_packages", {}).get("packages", {})
    return {name: entry["spdx_id"] for name, entry in recorded.items() if entry.get("spdx_id")}


def _resolve_python_package_license(name: str) -> str:
    """Resolve an SPDX license identifier for an installed Python package from local dist-info metadata.

    Reads locally from the current Python environment using importlib.metadata; never uses the network.
    """
    recorded = recorded_python_package_licenses().get(_normalize_python_name(name))
    if recorded:
        return recorded
    try:
        import importlib.metadata as md
        dist_meta = md.metadata(name)
    except Exception:
        dist_meta = None

    if dist_meta is not None:
        lic_expr = dist_meta.get("License-Expression")
        if lic_expr and lic_expr.strip():
            return lic_expr.strip()

        raw_lic = dist_meta.get("License")
        if raw_lic and raw_lic.strip():
            norm = _normalize_spdx_license(raw_lic.strip())
            if norm != "NOASSERTION":
                return norm

        classifiers = dist_meta.get_all("Classifier", [])
        for c in classifiers:
            if "GNU General Public License v3 or later" in c or "GPLv3+" in c:
                return "GPL-3.0-or-later"
            if "GNU General Public License v3" in c or "GPLv3" in c:
                return "GPL-3.0-only"
            if "MIT License" in c:
                return "MIT"
            if "Apache Software License" in c:
                return "Apache-2.0"

    # No guessed licenses: an unresolvable package stays NOASSERTION so the release gate sees it.
    return "NOASSERTION"


def parse_python_lockfile(
    lock_path: Path,
    artifact_metadata: PythonArtifactMetadata | None = None,
) -> list[dict]:
    snap = snapshot_lockfile(lock_path, "Python")
    if artifact_metadata is None:
        artifact_metadata = load_pypi_release_index(PYTHON_RELEASE_INDEX_PATH)
    return _parse_python_lock_text(snap, lock_path, artifact_metadata)


def _python_lock_logical_lines(text: str, lock_path: Path):
    pending = ""
    start_line = 0
    for lineno, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not pending and (not line or line.startswith("#")):
            continue
        if pending and (not line or line.startswith("#")):
            raise LockfileParseError(
                f"Python lockfile {lock_path} line {lineno}: incomplete backslash continuation"
            )
        if not pending:
            start_line = lineno
        if line.endswith("\\"):
            pending += f"{line[:-1].rstrip()} "
            continue
        logical_line = f"{pending}{line}"
        pending = ""
        yield start_line, logical_line
    if pending:
        raise LockfileParseError(
            f"Python lockfile {lock_path} line {start_line}: incomplete backslash continuation"
        )


def _parse_python_lock_text(
    snap: LockfileSnapshot,
    lock_path: Path,
    artifact_metadata: PythonArtifactMetadata,
) -> list[dict]:
    packages: list[dict] = []
    seen_pypi: set[tuple[str, str]] = set()
    for lineno, logical_line in _python_lock_logical_lines(snap.text, lock_path):
        fields = logical_line.split()
        if not fields:
            continue
        match = re.fullmatch(
            r"([a-zA-Z0-9_.-]+)==([a-zA-Z0-9_.-]+)", fields[0]
        )
        if not match:
            raise LockfileParseError(
                f"Python lockfile {lock_path} line {lineno}: requirement must start with an exact "
                f"name==version pin: {logical_line!r}"
            )
        name, version = match.groups()
        if not fields[1:]:
            raise LockfileParseError(
                f"Python lockfile {lock_path} line {lineno}: requirement {name}=={version} "
                "has no SHA-256 hashes"
            )
        declared_hashes: list[str] = []
        for field in fields[1:]:
            if not field.startswith("--hash="):
                raise LockfileParseError(
                    f"Python lockfile {lock_path} line {lineno}: unsupported token {field!r}; "
                    "only repeated --hash=sha256:<64 lowercase hex> options are accepted"
                )
            digest = field.removeprefix("--hash=")
            if not digest.startswith("sha256:") \
                    or not SHA256_PATTERN.fullmatch(digest.removeprefix("sha256:")):
                raise LockfileParseError(
                    f"Python lockfile {lock_path} line {lineno}: malformed SHA-256 hash {digest!r}; "
                    "expected sha256: followed by 64 lowercase hexadecimal characters"
                )
            sha256_hash = digest.removeprefix("sha256:")
            if sha256_hash in declared_hashes:
                raise LockfileParseError(
                    f"Python lockfile {lock_path} line {lineno}: duplicate SHA-256 hash "
                    f"{sha256_hash} for {name}=={version}"
                )
            declared_hashes.append(sha256_hash)
        key = (name, version)
        if key in seen_pypi:
            raise LockfileParseError(
                f"Python lockfile {lock_path} line {lineno}: duplicate requirement pin "
                f"{name}=={version}"
            )
        seen_pypi.add(key)
        release_artifacts = artifact_metadata.get((_normalize_python_name(name), version))
        if release_artifacts is None:
            raise LockfileParseError(
                f"Python lockfile {lock_path} line {lineno}: no trusted artifact metadata for "
                f"{name}=={version}"
            )
        release_by_hash = {
            artifact["sha256"]: artifact for artifact in release_artifacts
        }
        unmatched = [digest for digest in declared_hashes if digest not in release_by_hash]
        if unmatched:
            raise LockfileParseError(
                f"Python lockfile {lock_path} line {lineno}: declared SHA-256 hash "
                f"{unmatched[0]} matches no trusted artifact for {name}=={version}"
            )
        missing_release = sorted(set(release_by_hash) - set(declared_hashes))
        if missing_release:
            raise LockfileParseError(
                f"Python lockfile {lock_path} line {lineno}: {name}=={version} does not declare "
                f"every trusted artifact SHA-256; missing {missing_release[0]}"
            )
        verified_hashes = [artifact["sha256"] for artifact in release_artifacts]
        packages.append({
            "name": name,
            "version": version,
            "spdx_id": f"SPDXRef-pip-{name}-{version}",
            "license": _resolve_python_package_license(name),
            "sha256": declared_hashes[0],
            "declared_sha256": declared_hashes,
            "verified_sha256": verified_hashes,
            "artifacts": [dict(artifact) for artifact in release_artifacts],
            "purl": f"pkg:pypi/{name}@{version}",
            "ecosystem": "pypi",
        })

    return packages


def _python_hash_annotation_comment(
    status: str,
    package: dict,
    sha256_hash: str,
    filename: str | None = None,
) -> str:
    fields = [
        PYTHON_HASH_EVIDENCE_SCHEMA,
        f"status={status}",
        f"name={package['name']}",
        f"version={package['version']}",
        f"sha256={sha256_hash}",
    ]
    if filename is not None:
        fields.append(f"filename={filename}")
    return ";".join(fields)


def _python_hash_annotations(package: dict) -> list[dict]:
    annotations = [
        {
            "annotationDate": SBOM_CREATED,
            "annotationType": "OTHER",
            "annotator": "Tool: nakagawa-recomp-generate_sbom-0.1.0",
            "comment": _python_hash_annotation_comment(
                "declared", package, sha256_hash),
        }
        for sha256_hash in package["declared_sha256"]
    ]
    annotations.extend({
        "annotationDate": SBOM_CREATED,
        "annotationType": "OTHER",
        "annotator": "Tool: nakagawa-recomp-generate_sbom-0.1.0",
        "comment": _python_hash_annotation_comment(
            "verified", package, artifact["sha256"], artifact["filename"]),
    } for artifact in package["artifacts"])
    return annotations


def resolve_shipped_dlls(
    package_dir: Path | None = None,
    manifest_data: dict | None = None,
) -> list[dict[str, Any]]:
    """Resolve the list of shipped native DLLs with their SPDX license identifiers.

    Reads from package_dir/THIRD_PARTY_NOTICES/index.json when a built package is
    available, and otherwise from the checked-in component inventory
    (assets/third_party_components.json), which is the source of truth for every
    DLL a packaging route can copy. Versions come from the toolchain's own pacman
    database; no version is a literal here.
    """
    from package_notices import (
        installed_package_version,
        load_component_inventory,
        resolve_toolchain_root,
    )

    shipped: dict[str, dict[str, Any]] = {}

    def add(name: str, license_id: str, version: str, origin: str, source_path: str) -> None:
        key = name.lower()
        if key in shipped:
            return
        slug = re.sub(r"[^A-Za-z0-9._-]", "_", name)
        shipped[key] = {
            "name": name,
            "spdx_id": f"SPDXRef-dll-{slug}",
            "version": version,
            "license": license_id,
            "downloadLocation": origin,
            "source_path": source_path,
        }

    if package_dir is not None and package_dir.is_dir():
        index_path = package_dir / "THIRD_PARTY_NOTICES" / "index.json"
        if index_path.is_file():
            try:
                index_data = json.loads(index_path.read_text(encoding="utf-8"))
                for comp in index_data.get("components", []):
                    bin_name = comp.get("binary")
                    if bin_name and bin_name.lower().endswith(".dll"):
                        add(bin_name, comp.get("spdx_id", "NOASSERTION"),
                            comp.get("version", "unknown"),
                            comp.get("upstream_origin", "NOASSERTION"),
                            comp.get("source_path", bin_name))
            except (OSError, json.JSONDecodeError):
                pass

        inventory = load_component_inventory()
        for dll_key, record in sorted(inventory["native"]["dlls"].items()):
            if (package_dir / dll_key).is_file():
                add(dll_key, record["spdx_id"],
                    installed_package_version(resolve_toolchain_root(), record.get("pacman_package")),
                    record.get("upstream_origin", "NOASSERTION"),
                    record["license_texts"][0]["file"])
        for record in inventory["native"].get("host_resolved_components", []):
            dll = record.get("dll")
            if dll and (package_dir / dll).is_file():
                add(dll, record["spdx_id"],
                    installed_package_version(resolve_toolchain_root(), record.get("pacman_package")),
                    record.get("upstream_origin", "NOASSERTION"),
                    record["license_texts"][0]["file"])

    if manifest_data:
        for comp in manifest_data.get("components", []):
            src_path = comp.get("source_path", "")
            if src_path.lower().endswith(".dll") or comp.get("id") == "sdl3-runtime":
                dll_name = Path(src_path).name if src_path else "SDL3.dll"
                add(dll_name, comp.get("license", "Zlib"), comp.get("version", ">=3.0.0"),
                    comp.get("upstream_origin", "https://github.com/libsdl-org/SDL"),
                    src_path or dll_name)

    if not shipped:
        # No built package and no manifest override: the SBOM still has to name
        # every DLL a packaging route can copy, so it falls back to the inventory
        # rather than to a single hardcoded entry that would under-report the set.
        inventory = load_component_inventory()
        toolchain_root = resolve_toolchain_root()
        records = dict(inventory["native"]["dlls"])
        for record in inventory["native"].get("host_resolved_components", []):
            if record.get("dll"):
                records[record["dll"]] = record
        for dll_key, record in sorted(records.items()):
            add(dll_key, record["spdx_id"],
                installed_package_version(toolchain_root, record.get("pacman_package")),
                record.get("upstream_origin", "NOASSERTION"),
                record["license_texts"][0]["file"])

    return [shipped[k] for k in sorted(shipped)]


def generate_spdx23(manifest_data: dict, py_packages: list[dict],
                    lock_files: list[dict] | None = None,
                    lock_relationships: list[dict] | None = None,
                    shipped_dlls: list[dict] | None = None) -> dict:
    """Build an SPDX 2.3 document with optional Python lock byte binding."""
    if shipped_dlls is None:
        shipped_dlls = resolve_shipped_dlls(manifest_data=manifest_data)
    doc_id = "SPDXRef-DOCUMENT"
    root_pkg_id = "SPDXRef-Package-nakagawa-recomp"
    packages = [{
        "SPDXID": root_pkg_id,
        "name": manifest_data.get("name", "nakagawa-recomp"),
        "versionInfo": manifest_data.get("version", "0.1.0"),
        "downloadLocation": "https://github.com/Jstar269/nakagawa-recomp",
        "filesAnalyzed": False,
        "licenseConcluded": manifest_data.get("license", "GPL-3.0-or-later"),
        "licenseDeclared": manifest_data.get("license", "GPL-3.0-or-later"),
        "copyrightText": "Copyright (C) 2025-2026 the psp-recomp authors",
        "summary": manifest_data.get("description", ""),
    }]
    relationships = [{
        "spdxElementId": doc_id,
        "relationshipType": "DESCRIBES",
        "relatedSpdxElement": root_pkg_id,
    }]

    for comp in manifest_data.get("components", []):
        c_id = f"SPDXRef-comp-{comp.get('id', 'unknown')}"
        packages.append({
            "SPDXID": c_id,
            "name": comp.get("name", comp.get("id")),
            "downloadLocation": comp.get("upstream_origin", "NOASSERTION"),
            "filesAnalyzed": False,
            "licenseConcluded": comp.get("license", "NOASSERTION"),
            "licenseDeclared": comp.get("license", "NOASSERTION"),
            "copyrightText": "NOASSERTION",
            "comment": comp.get("comment", ""),
        })
        relationships.append({
            "spdxElementId": root_pkg_id,
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": c_id,
        })

    for family in manifest_data.get("provenance_families", []):
        family_id = str(family.get("id", "unknown"))
        c_id = f"SPDXRef-family-{family_id}"
        packages.append({
            "SPDXID": c_id,
            "name": family.get("name", family_id),
            "versionInfo": family.get("revision", "reviewed-source"),
            "downloadLocation": family.get("origin", "NOASSERTION"),
            "filesAnalyzed": False,
            "licenseConcluded": family.get("license", "NOASSERTION"),
            "licenseDeclared": family.get("license", "NOASSERTION"),
            "copyrightText": "NOASSERTION",
            "comment": (
                f"notice={family.get('notice_path', 'NOASSERTION')}; "
                f"evidence={family.get('evidence_path', 'NOASSERTION')}; "
                f"disposition={family.get('disposition', 'NOASSERTION')}"
            ),
        })
        relationships.append({
            "spdxElementId": root_pkg_id,
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": c_id,
        })

    for dll in shipped_dlls:
        if any(p.get("name") in (dll["name"], dll.get("id")) or p.get("SPDXID") == dll["spdx_id"] for p in packages):
            continue
        packages.append({
            "SPDXID": dll["spdx_id"],
            "name": dll["name"],
            "versionInfo": dll.get("version", "unknown"),
            "downloadLocation": dll.get("downloadLocation", "NOASSERTION"),
            "filesAnalyzed": False,
            "licenseConcluded": dll["license"],
            "licenseDeclared": dll["license"],
            "copyrightText": "NOASSERTION",
        })
        relationships.append({
            "spdxElementId": root_pkg_id,
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": dll["spdx_id"],
        })

    for pkg in py_packages:
        packages.append({
            "SPDXID": pkg["spdx_id"],
            "name": pkg["name"],
            "versionInfo": pkg["version"],
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": False,
            "licenseConcluded": pkg["license"],
            "licenseDeclared": pkg["license"],
            "copyrightText": "NOASSERTION",
            "checksums": [
                {"algorithm": "SHA256", "checksumValue": value}
                for value in pkg["verified_sha256"]
            ],
            "annotations": _python_hash_annotations(pkg),
            "externalRefs": [{
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": pkg["purl"],
            }],
        })
        relationships.append({
            "spdxElementId": root_pkg_id,
            "relationshipType": "DEV_DEPENDENCY_OF",
            "relatedSpdxElement": pkg["spdx_id"],
        })

    document: dict = {
        "spdxVersion": SPDX_VERSION,
        "dataLicense": DATA_LICENSE,
        "SPDXID": doc_id,
        "name": DOCUMENT_NAME,
        "documentNamespace": f"{DOCUMENT_NAMESPACE_BASE}-{manifest_data.get('version', '0.1.0')}",
        "creationInfo": {
            "creators": ["Tool: nakagawa-recomp-generate_sbom-0.1.0", "Organization: psp-recomp"],
            "created": SBOM_CREATED,
        },
        "packages": packages,
        "relationships": relationships,
    }
    if lock_files is not None:
        document["files"] = list(lock_files)
    if lock_relationships is not None:
        document["relationships"].extend(lock_relationships)
    return document


def generate_spdx301(manifest_data: dict, py_packages: list[dict],
                     shipped_dlls: list[dict] | None = None) -> dict:
    if shipped_dlls is None:
        shipped_dlls = resolve_shipped_dlls(manifest_data=manifest_data)
    base_id = f"{DOCUMENT_NAMESPACE_BASE}-{manifest_data.get('version', '0.1.0')}"
    graph = [
        {
            "@id": f"{base_id}#Document",
            "@type": "spdx:SpdxDocument",
            "spdx:name": DOCUMENT_NAME,
            "spdx:specVersion": "3.0.1",
            "spdx:dataLicense": "http://spdx.org/licenses/CC0-1.0",
        },
        {
            "@id": f"{base_id}#Package-nakagawa-recomp",
            "@type": "spdx:Package",
            "spdx:name": manifest_data.get("name", "nakagawa-recomp"),
            "spdx:packageVersion": manifest_data.get("version", "0.1.0"),
            "spdx:concludedLicense": f"http://spdx.org/licenses/{manifest_data.get('license', 'GPL-3.0-or-later')}",
        },
    ]
    for dll in shipped_dlls:
        graph.append({
            "@id": f"{base_id}#{dll['spdx_id']}",
            "@type": "spdx:Package",
            "spdx:name": dll["name"],
            "spdx:packageVersion": dll.get("version", "unknown"),
            "spdx:concludedLicense": f"http://spdx.org/licenses/{dll['license']}",
        })
    for pkg in py_packages:
        graph.append({
            "@id": f"{base_id}#{pkg['spdx_id']}",
            "@type": "spdx:Package",
            "spdx:name": pkg["name"],
            "spdx:packageVersion": pkg["version"],
            "spdx:purl": pkg["purl"],
            "spdx:sourceInfo": "\n".join(
                annotation["comment"] for annotation in _python_hash_annotations(pkg)
                if "status=declared" in annotation["comment"]
            ),
            "spdx:verifiedUsing": [{
                "@type": "spdx:Hash",
                "spdx:algorithm": "sha256",
                "spdx:hashValue": artifact["sha256"],
            } for artifact in pkg["artifacts"]],
        })
    for family in manifest_data.get("provenance_families", []):
        family_id = str(family.get("id", "unknown"))
        graph.append({
            "@id": f"{base_id}#family-{family_id}",
            "@type": "spdx:Package",
            "spdx:name": family.get("name", family_id),
            "spdx:packageVersion": family.get("revision", "reviewed-source"),
            "spdx:downloadLocation": family.get("origin", "NOASSERTION"),
            "spdx:declaredLicense": family.get("license", "NOASSERTION"),
        })
    return {"@context": "https://spdx.org/rdf/3.0.1/spdx-context.jsonld", "@graph": graph}


def generate_cyclonedx(manifest_data: dict, py_packages: list[dict],
                       shipped_dlls: list[dict] | None = None) -> dict:
    if shipped_dlls is None:
        shipped_dlls = resolve_shipped_dlls(manifest_data=manifest_data)
    components = [{
        "type": "application",
        "name": manifest_data.get("name", "nakagawa-recomp"),
        "version": manifest_data.get("version", "0.1.0"),
        "licenses": [{"license": {"id": manifest_data.get("license", "GPL-3.0-or-later")}}],
        "description": manifest_data.get("description", ""),
    }]
    for family in manifest_data.get("provenance_families", []):
        family_id = str(family.get("id", "unknown"))
        revision = str(family.get("revision", "reviewed-source"))
        components.append({
            "type": "library",
            "name": family.get("name", family_id),
            "version": revision,
            "bom-ref": f"family:{family_id}",
            "purl": f"pkg:generic/nakagawa-provenance/{family_id}@reviewed",
            "licenses": [{"license": {"id": family.get("license", "NOASSERTION")}}],
            "externalReferences": [{"type": "vcs", "url": family.get("origin", "NOASSERTION")}],
        })
    for dll in shipped_dlls:
        components.append({
            "type": "library", "name": dll["name"],
            "version": dll.get("version", "unknown"),
            "licenses": [{"license": {"id": dll["license"]}}],
        })
    for pkg in py_packages:
        entry = {
            "type": "library", "name": pkg["name"], "version": pkg["version"],
            "purl": pkg["purl"],
            "hashes": [{"alg": "SHA-256", "content": value} for value in pkg["verified_sha256"]],
            "properties": [
                {"name": "nakagawa:python-lock-sha256", "value": (
                    f"{PYTHON_HASH_EVIDENCE_SCHEMA};status=declared;sha256="
                    f"{','.join(pkg['declared_sha256'])}"
                )},
                {"name": "nakagawa:pypi-artifact-sha256", "value": (
                    f"{PYTHON_HASH_EVIDENCE_SCHEMA};status=verified;artifacts="
                    + ",".join(f"{artifact['filename']}@{artifact['sha256']}" for artifact in pkg["artifacts"])
                )},
            ],
        }
        license_id = pkg.get("license", "NOASSERTION")
        if license_id != "NOASSERTION":
            entry["licenses"] = [{"license": {"id": license_id}}]
        components.append(entry)
    return {"bomFormat": "CycloneDX", "specVersion": "1.5", "version": 1, "components": components}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=ROOT / "assets" / "release_manifest.json")
    parser.add_argument("--py-lock", type=Path, default=ROOT / "tools" / "requirements-lock.txt")
    parser.add_argument(
        "--trusted-metadata", dest="release_index", type=Path,
        default=PYTHON_RELEASE_INDEX_PATH,
        help="Trusted PyPI name/version/filename/SHA-256 metadata snapshot",
    )
    parser.add_argument(
        "--fetch-live-metadata", action="store_true",
        default=live_metadata_requested(),
        help="Fetch exact pinned releases from the official PyPI JSON API before generation",
    )
    parser.add_argument(
        "--metadata-timeout", type=float, default=30.0,
        help="Timeout in seconds for each live PyPI metadata request",
    )
    parser.add_argument("--spdx-out", type=Path, help="Output path for SPDX 2.3 JSON")
    parser.add_argument("--spdx3-out", type=Path, help="Output path for SPDX 3.0.1 JSON-LD")
    parser.add_argument("--cyclonedx-out", type=Path, help="Output path for CycloneDX 1.5 JSON")
    parser.add_argument(
        "--package-dir", type=Path,
        help="Native package directory containing shipped DLLs and notices",
    )
    args = parser.parse_args(argv)
    if not args.manifest.is_file():
        print(f"Error: manifest file missing: {args.manifest}", file=sys.stderr)
        return 1
    manifest_data = json.loads(args.manifest.read_text(encoding="utf-8"))
    manifest_repo_root = args.manifest.parent.parent
    try:
        declared_py_lock = resolve_declared_lockfile(manifest_data, args.manifest)
        if args.py_lock.resolve() != declared_py_lock:
            raise LockfileParseError(
                f"Python lockfile {args.py_lock} is not the lockfile declared by the "
                f"release manifest ({declared_py_lock}); release evidence may only be "
                "generated from the manifest-declared dependency lockfile"
            )
        python_artifact_metadata = resolve_pypi_release_index(
            args.release_index, fetch_live=args.fetch_live_metadata,
            timeout=args.metadata_timeout,
        )
        parsed = parse_lockfiles(
            declared_py_lock, repo_root=manifest_repo_root,
            python_artifact_metadata=python_artifact_metadata,
        )
    except LockfileParseError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    py_packages = parsed["py_packages"]
    shipped_dlls = resolve_shipped_dlls(package_dir=args.package_dir, manifest_data=manifest_data)
    spdx23_doc = generate_spdx23(
        manifest_data, py_packages, lock_files=parsed["lock_files"],
        lock_relationships=parsed["lock_relationships"], shipped_dlls=shipped_dlls,
    )
    spdx301_doc = generate_spdx301(manifest_data, py_packages, shipped_dlls=shipped_dlls)
    cyclonedx_doc = generate_cyclonedx(manifest_data, py_packages, shipped_dlls=shipped_dlls)
    if args.spdx_out:
        args.spdx_out.parent.mkdir(parents=True, exist_ok=True)
        args.spdx_out.write_text(json.dumps(spdx23_doc, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"Wrote SPDX 2.3 SBOM to {args.spdx_out}")
        print(f"Bound to Python lock evidence: {parsed['lock_evidence']['python']['sha256'][:16]}...")
    if args.spdx3_out:
        args.spdx3_out.parent.mkdir(parents=True, exist_ok=True)
        args.spdx3_out.write_text(json.dumps(spdx301_doc, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"Wrote SPDX 3.0.1 JSON-LD SBOM to {args.spdx3_out}")
    if args.cyclonedx_out:
        args.cyclonedx_out.parent.mkdir(parents=True, exist_ok=True)
        args.cyclonedx_out.write_text(json.dumps(cyclonedx_doc, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"Wrote CycloneDX 1.5 SBOM to {args.cyclonedx_out}")
    if not (args.spdx_out or args.spdx3_out or args.cyclonedx_out):
        print(json.dumps(spdx23_doc, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
