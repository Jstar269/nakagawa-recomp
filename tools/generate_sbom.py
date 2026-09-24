#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Generate SPDX 2.3, SPDX 3.0.1 JSON-LD, and CycloneDX 1.5 SBOM artifacts for Nakagawa Recomp releases."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent

SPDX_VERSION = "SPDX-2.3"
DATA_LICENSE = "CC0-1.0"
DOCUMENT_NAME = "nakagawa-recomp-sbom"
DOCUMENT_NAMESPACE_BASE = "https://spdx.org/spdxdocs/nakagawa-recomp"

# npm package-lock.json schema this tool knows how to inventory. Anything
# outside the supported set is a hard parse error (issue #375): guessing at an
# unknown schema silently drops dependencies and manufactures a plausible but
# incomplete SBOM.
# lockfileVersion 1 records its inventory in the legacy `dependencies` map only
# and is explicitly rejected: this tool implements the `packages` representation
# (lockfileVersion 2/3), never best-effort guesses at an unimplemented schema.
SUPPORTED_NPM_LOCKFILE_VERSIONS = (2, 3)
SUPPORTED_NPM_PACKAGES_FORMATS = (2, 3)


class LockfileParseError(Exception):
    """A declared dependency lockfile could not be parsed completely.

    Raised on unreadable files, malformed JSON, unsupported schema/version,
    missing or invalid required records, and requirement lines that do not
    conform to the repository's declared lockfile format. Callers must abort
    rather than treat the failure as "no dependencies" (issue #375).
    """


#: Schema version of the generated lockfile evidence binding.
LOCKFILE_EVIDENCE_SCHEMA_VERSION = 1

# Canonical repo-relative identities of the declared dependency lockfiles.
# These are the only identities release evidence may carry: machine-local
# absolute or temp paths must never reach a published SBOM.
NPM_LOCKFILE_IDENTITY = "interface/package-lock.json"
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


def resolve_declared_lockfiles(manifest_data: object, manifest_path: Path) -> tuple[Path, Path, dict]:
    """Resolve the lockfiles declared by the release manifest, fail closed.

    Release evidence may only be generated from the exact lockfiles the
    manifest declares. Missing/non-string declarations, declarations that
    escape the repository root, and declarations pointing outside the repo
    are all rejected. Returns (npm_lock_path, py_lock_path, lockfiles_map).
    """
    if not isinstance(manifest_data, dict):
        raise LockfileParseError(
            f"release manifest {manifest_path} must contain a JSON object"
        )
    lockfiles = manifest_data.get("lockfiles")
    if not isinstance(lockfiles, dict):
        raise LockfileParseError(
            f"release manifest {manifest_path} has no `lockfiles` object"
        )
    repo_root = manifest_path.parent.parent
    resolved: dict[str, Path] = {}
    for ecosystem in ("npm", "python"):
        rel_path = lockfiles.get(ecosystem)
        if not isinstance(rel_path, str) or not rel_path:
            raise LockfileParseError(
                f"release manifest {manifest_path}: {ecosystem} lockfiles entry is "
                f"missing or not a non-empty string ({rel_path!r})"
            )
        declared = (repo_root / rel_path).resolve()
        try:
            declared.relative_to(repo_root)
        except ValueError:
            raise LockfileParseError(
                f"release manifest {manifest_path}: {ecosystem} lockfiles entry "
                f"{rel_path!r} escapes the repository root"
            ) from None
        if not declared.is_file():
            raise LockfileParseError(
                f"release manifest {manifest_path}: declared {ecosystem} lockfile "
                f"{rel_path!r} is missing ({declared})"
            )
        resolved[ecosystem] = declared
    return resolved["npm"], resolved["python"], lockfiles


def parse_lockfiles(npm_lock_path: Path, py_lock_path: Path,
                    repo_root: Path | None = None) -> dict:
    """Parse both lockfiles from single snapshots and derive all binding data.

    Returns the dependency inventories, the lock evidence, and the
    standards-conformant SPDX 2.3 `files` entries plus
    `DEPENDENCY_MANIFEST_OF` relationships for the two lockfiles. Parsing and
    evidence share one byte snapshot per lockfile, closing the read-before /
    read-after TOCTOU gap (issue #375 revision 3).
    """
    npm_snap = snapshot_lockfile(npm_lock_path, "npm", repo_root)
    py_snap = snapshot_lockfile(py_lock_path, "Python", repo_root)
    npm_packages = _parse_npm_lock_text(npm_snap, npm_lock_path)
    py_packages = _parse_python_lock_text(py_snap, py_lock_path)
    lock_files = build_lock_file_entries(npm_snap, py_snap)
    lock_relationships = build_lock_relationship_entries(lock_files)
    return {
        "npm_packages": npm_packages,
        "py_packages": py_packages,
        "lock_evidence": {
            "schema_version": LOCKFILE_EVIDENCE_SCHEMA_VERSION,
            "npm": _lock_evidence_entry(npm_snap),
            "python": _lock_evidence_entry(py_snap),
        },
        "lock_files": lock_files,
        "lock_relationships": lock_relationships,
    }


def build_lock_file_entries(npm_snap: LockfileSnapshot,
                            py_snap: LockfileSnapshot) -> list[dict]:
    """SPDX 2.3 `files` entries for the declared dependency lockfiles.

    Standards-conformant lock binding: each lockfile is a normal SPDX file with
    a stable repo-relative fileName and a SHA256 checksum over the exact lock
    bytes, instead of a custom non-schema document property.
    """
    entries: list[dict] = []
    for snap, spdx_id in (
        (npm_snap, "SPDXRef-File-npm-package-lock"),
        (py_snap, "SPDXRef-File-python-requirements-lock"),
    ):
        if snap.lockfile_identity is None:
            raise LockfileParseError(
                f"lockfile {snap.path} is outside the repository root; release evidence "
                "requires the canonical repo-relative lockfile identity "
                f"({NPM_LOCKFILE_IDENTITY} / {PYTHON_LOCKFILE_IDENTITY})"
            )
        entries.append({
            "SPDXID": spdx_id,
            "fileName": snap.lockfile_identity,
            "checksums": [{"algorithm": "SHA256", "checksumValue": snap.sha256}],
            "copyrightText": "NOASSERTION",
            "licenseConcluded": "NOASSERTION",
            "licenseInfoInFiles": ["NOASSERTION"],
            "comment": (
                "Declared dependency lockfile; binding evidence for the generated "
                "dependency inventory (sha256 over the exact lockfile bytes)."
            ),
        })
    return entries


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


def _parse_npm_lock_text(snap: LockfileSnapshot, lock_path: Path) -> list[dict]:
    """Parse the snapshotted npm package-lock text into package records.

    Fails closed (raises LockfileParseError) on malformed JSON, an unsupported
    lockfile shape/version, or any package record that is missing required
    metadata. A lockfile whose `packages` map is genuinely empty is valid and
    yields an empty inventory.
    """
    text = snap.text
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise LockfileParseError(
            f"npm lockfile {lock_path} is not valid JSON (line {exc.lineno}, column {exc.colno}): {exc.msg}"
        ) from exc
    if not isinstance(data, dict):
        raise LockfileParseError(
            f"npm lockfile {lock_path} must contain a JSON object, got {type(data).__name__}"
        )

    raw_version = data.get("lockfileVersion")
    if not isinstance(raw_version, int) or isinstance(raw_version, bool):
        raise LockfileParseError(
            f"npm lockfile {lock_path} has missing or invalid lockfileVersion {raw_version!r}; "
            f"supported versions: {', '.join(str(v) for v in SUPPORTED_NPM_LOCKFILE_VERSIONS)}"
        )
    if raw_version not in SUPPORTED_NPM_LOCKFILE_VERSIONS:
        hint = ""
        if raw_version == 1:
            hint = (
                " lockfileVersion 1 records its inventory in the legacy `dependencies` map, "
                "which this tool does not implement; regenerate with a current npm version"
            )
        raise LockfileParseError(
            f"npm lockfile {lock_path} uses unsupported lockfileVersion {raw_version}; "
            f"supported versions: {', '.join(str(v) for v in SUPPORTED_NPM_LOCKFILE_VERSIONS)}.{hint}"
        )

    raw_packages = data.get("packages")
    if raw_packages is None:
        raise LockfileParseError(
            f"npm lockfile {lock_path} (lockfileVersion {raw_version}) has no `packages` map; "
            "this tool requires a lockfile that records dependencies under `packages` "
            f"(lockfileVersions {', '.join(str(v) for v in SUPPORTED_NPM_PACKAGES_FORMATS)}); "
            "regenerate with a current npm version"
        )
    if not isinstance(raw_packages, dict):
        raise LockfileParseError(
            f"npm lockfile {lock_path} has a non-object `packages` member "
            f"({type(raw_packages).__name__})"
        )

    # A package can be installed at multiple lockfile paths (e.g. a nested
    # node_modules copy with the same name+version). SPDX element IDs must be
    # unique, so duplicate name-version IDs are disambiguated with a short
    # digest of the lockfile path; the deterministic sort keeps this stable.
    seen_ids: set[str] = set()

    def unique_spdx_id(base: str, pkg_path: str) -> str:
        if base not in seen_ids:
            seen_ids.add(base)
            return base
        suffix = hashlib.sha256(pkg_path.encode("utf-8")).hexdigest()[:10]
        return f"{base}-{suffix}"

    packages: list[dict] = []
    for pkg_path, meta in sorted(raw_packages.items()):
        if not isinstance(pkg_path, str):
            raise LockfileParseError(
                f"npm lockfile {lock_path} has an invalid `packages` key {pkg_path!r}; "
                "keys must be install-path strings"
            )
        if not pkg_path:
            # The root entry ("" in npm's packages map) is the project itself,
            # which generate_spdx23 already emits from the release manifest; it
            # is not a lockfile dependency and needs no name/version.
            continue
        if not isinstance(meta, dict):
            raise LockfileParseError(
                f"npm lockfile {lock_path}: package record {pkg_path!r} must be an object, "
                f"got {type(meta).__name__}"
            )
        name = meta.get("name")
        if name is None:
            name = pkg_path.split("node_modules/")[-1]
        if not isinstance(name, str) or not name:
            raise LockfileParseError(
                f"npm lockfile {lock_path}: package record {pkg_path!r} has invalid name {name!r}"
            )
        version = meta.get("version")
        if not isinstance(version, str) or not version:
            raise LockfileParseError(
                f"npm lockfile {lock_path}: package record {pkg_path!r} ({name}) is missing "
                "required version metadata"
            )
        license_exp = meta.get("license", "NOASSERTION")
        integrity = meta.get("integrity", "")
        resolved = meta.get("resolved", "")
        spdx_id = unique_spdx_id(
            f"SPDXRef-npm-{name.replace('/', '-')}-{version}", pkg_path
        )
        packages.append({
            "name": name,
            "version": version,
            "spdx_id": spdx_id,
            "license": license_exp,
            "integrity": integrity,
            "resolved": resolved,
            "purl": f"pkg:npm/{name}@{version}",
            "ecosystem": "npm",
        })

    return packages


def parse_npm_lockfile(lock_path: Path) -> list[dict]:
    """Parse an npm package-lock.json path from a single byte snapshot."""
    snap = snapshot_lockfile(lock_path, "npm")
    return _parse_npm_lock_text(snap, lock_path)


def parse_python_lockfile(lock_path: Path) -> list[dict]:
    """Parse the pinned Python requirements path from a single byte snapshot."""
    snap = snapshot_lockfile(lock_path, "Python")
    return _parse_python_lock_text(snap, lock_path)


def _parse_python_lock_text(snap: LockfileSnapshot, lock_path: Path) -> list[dict]:
    """Parse the snapshotted Python requirements text into package records.

    The repository's declared format is one `name==version` pin per nonblank,
    non-comment line, with an optional single `--hash=sha256:<64 hex>` digest.
    Every other nonblank/non-comment line is a parse error (issue #375): the
    previous best-effort loop silently skipped requirement lines it could not
    parse, dropping them from the release inventory. A file whose only
    non-comment content is blank or comments is a valid empty lockfile and
    yields an empty inventory.
    """
    text = snap.text

    packages: list[dict] = []
    seen_pypi: set[tuple[str, str]] = set()
    for lineno, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        match = re.match(
            r"^([a-zA-Z0-9_.-]+)==([a-zA-Z0-9_.-]+)"
            r"(?:\s+--hash=sha256:([0-9a-fA-F]{64}))?$",
            line,
        )
        if not match:
            raise LockfileParseError(
                f"Python lockfile {lock_path} line {lineno}: requirement does not match the "
                "declared pinned format 'name==version[ --hash=sha256:<64 hex>]': "
                f"{line!r}"
            )
        name, version, sha256_hash = match.groups()
        if (name, version) in seen_pypi:
            raise LockfileParseError(
                f"Python lockfile {lock_path} line {lineno}: duplicate requirement pin "
                f"{name}=={version}"
            )
        seen_pypi.add((name, version))
        packages.append({
            "name": name,
            "version": version,
            "spdx_id": f"SPDXRef-pip-{name}-{version}",
            "license": "NOASSERTION",
            "sha256": sha256_hash or "",
            "purl": f"pkg:pypi/{name}@{version}",
            "ecosystem": "pypi",
        })

    return packages


def generate_spdx23(manifest_data: dict, npm_packages: list[dict], py_packages: list[dict],
                    lock_files: list[dict] | None = None,
                    lock_relationships: list[dict] | None = None) -> dict:
    """Build the SPDX 2.3 document; optionally embed standards-conformant
    lockfile evidence (files entries + DEPENDENCY_MANIFEST_OF relationships).
    """
    doc_id = "SPDXRef-DOCUMENT"
    root_pkg_id = "SPDXRef-Package-nakagawa-recomp"

    packages = [
        {
            "SPDXID": root_pkg_id,
            "name": manifest_data.get("name", "nakagawa-recomp"),
            "versionInfo": manifest_data.get("version", "0.1.0"),
            "downloadLocation": "https://github.com/Jstar269/nakagawa-recomp",
            "filesAnalyzed": False,
            "licenseConcluded": manifest_data.get("license", "GPL-3.0-or-later"),
            "licenseDeclared": manifest_data.get("license", "GPL-3.0-or-later"),
            "copyrightText": "Copyright (C) 2025-2026 the psp-recomp authors",
            "summary": manifest_data.get("description", ""),
        }
    ]

    relationships = [
        {
            "spdxElementId": doc_id,
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": root_pkg_id,
        }
    ]

    # Ingest components from release_manifest.json
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

    # Provenance families are first-class SBOM entries.  Keeping them separate
    # from release components prevents a component list change from silently
    # dropping an upstream lineage that still governs retained source/data.
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

    # Ingest NPM packages
    for pkg in npm_packages:
        packages.append({
            "SPDXID": pkg["spdx_id"],
            "name": pkg["name"],
            "versionInfo": pkg["version"],
            "downloadLocation": pkg["resolved"] or "NOASSERTION",
            "filesAnalyzed": False,
            "licenseConcluded": pkg["license"],
            "licenseDeclared": pkg["license"],
            "copyrightText": "NOASSERTION",
            "externalRefs": [
                {
                    "referenceCategory": "PACKAGE-MANAGER",
                    "referenceType": "purl",
                    "referenceLocator": pkg["purl"],
                }
            ],
        })
        relationships.append({
            "spdxElementId": root_pkg_id,
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": pkg["spdx_id"],
        })

    # Ingest PyPI packages
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
            "externalRefs": [
                {
                    "referenceCategory": "PACKAGE-MANAGER",
                    "referenceType": "purl",
                    "referenceLocator": pkg["purl"],
                }
            ],
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
            "created": "2026-08-06T00:00:00Z",
        },
        "packages": packages,
        "relationships": relationships,
    }
    if lock_files is not None:
        # Standards-conformant lock binding: schema-defined `files` entries and
        # DEPENDENCY_MANIFEST_OF relationships instead of a custom root property
        # (the SPDX 2.3 schema sets top-level additionalProperties: false).
        document["files"] = list(lock_files)
    if lock_relationships is not None:
        document["relationships"].extend(lock_relationships)
    return document


def generate_spdx301(manifest_data: dict, npm_packages: list[dict], py_packages: list[dict]) -> dict:
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
        }
    ]

    for pkg in npm_packages:
        graph.append({
            "@id": f"{base_id}#{pkg['spdx_id']}",
            "@type": "spdx:Package",
            "spdx:name": pkg["name"],
            "spdx:packageVersion": pkg["version"],
            "spdx:purl": pkg["purl"],
        })

    for pkg in py_packages:
        graph.append({
            "@id": f"{base_id}#{pkg['spdx_id']}",
            "@type": "spdx:Package",
            "spdx:name": pkg["name"],
            "spdx:packageVersion": pkg["version"],
            "spdx:purl": pkg["purl"],
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

    return {
        "@context": "https://spdx.org/rdf/3.0.1/spdx-context.jsonld",
        "@graph": graph,
    }


def generate_cyclonedx(manifest_data: dict, npm_packages: list[dict], py_packages: list[dict]) -> dict:
    components = [
        {
            "type": "application",
            "name": manifest_data.get("name", "nakagawa-recomp"),
            "version": manifest_data.get("version", "0.1.0"),
            "licenses": [{"license": {"id": manifest_data.get("license", "GPL-3.0-or-later")}}],
            "description": manifest_data.get("description", ""),
        }
    ]

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
            "externalReferences": [{
                "type": "vcs",
                "url": family.get("origin", "NOASSERTION"),
            }],
        })

    # CycloneDX identifies a library by its purl, and a purl is unique per
    # package name+version regardless of install path. The npm lockfile can
    # list the same package at several paths, so deduplicate by purl here
    # (unlike SPDX, where each parsed path entry is kept as a distinct
    # element with a path-disambiguated id).
    seen_purls: set[str] = set()

    def add_library(pkg: dict) -> None:
        purl = pkg.get("purl", "")
        if purl in seen_purls:
            return
        seen_purls.add(purl)
        entry = {
            "type": "library",
            "name": pkg["name"],
            "version": pkg["version"],
            "purl": purl,
        }
        if pkg.get("ecosystem") == "npm":
            entry["licenses"] = [{"license": {"id": pkg["license"] if pkg["license"] != "NOASSERTION" else "unspecified"}}]
        components.append(entry)

    for pkg in npm_packages:
        add_library(pkg)

    for pkg in py_packages:
        add_library(pkg)

    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "components": components,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=ROOT / "assets" / "release_manifest.json")
    parser.add_argument("--npm-lock", type=Path, default=ROOT / "interface" / "package-lock.json")
    parser.add_argument("--py-lock", type=Path, default=ROOT / "tools" / "requirements-lock.txt")
    parser.add_argument("--spdx-out", type=Path, help="Output path for SPDX 2.3 JSON")
    parser.add_argument("--spdx3-out", type=Path, help="Output path for SPDX 3.0.1 JSON-LD")
    parser.add_argument("--cyclonedx-out", type=Path, help="Output path for CycloneDX 1.5 JSON")

    args = parser.parse_args(argv)

    if not args.manifest.is_file():
        print(f"Error: manifest file missing: {args.manifest}", file=sys.stderr)
        return 1

    manifest_data = json.loads(args.manifest.read_text(encoding="utf-8"))
    # The repository root is the parent of the manifest's assets directory;
    # lockfile identities in release evidence derive from the manifest, not
    # from a hardcoded path (issue #375 revision 4).
    manifest_repo_root = args.manifest.parent.parent
    try:
        # Release generation may only use the exact lockfiles declared by the
        # release manifest; the SPDX fileNames derive from those declarations,
        # never from an arbitrary CLI path (issue #375 revision 4).
        declared_npm_lock, declared_py_lock, _lockfiles_map = resolve_declared_lockfiles(
            manifest_data, args.manifest
        )
        for supplied, declared, label in (
            (args.npm_lock, declared_npm_lock, "npm"),
            (args.py_lock, declared_py_lock, "Python"),
        ):
            if supplied.resolve() != declared:
                raise LockfileParseError(
                    f"{label} lockfile {supplied} is not the lockfile declared by the "
                    f"release manifest ({declared}); release evidence may only be "
                    "generated from the manifest-declared dependency lockfiles"
                )
        parsed = parse_lockfiles(declared_npm_lock, declared_py_lock,
                                 repo_root=manifest_repo_root)
    except LockfileParseError as exc:
        # Fail closed: never emit a partial/empty-inventory SBOM because a
        # declared lockfile could not be parsed completely (issue #375).
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    npm_packages = parsed["npm_packages"]
    py_packages = parsed["py_packages"]

    spdx23_doc = generate_spdx23(
        manifest_data, npm_packages, py_packages,
        lock_files=parsed["lock_files"],
        lock_relationships=parsed["lock_relationships"],
    )
    spdx301_doc = generate_spdx301(manifest_data, npm_packages, py_packages)
    cyclonedx_doc = generate_cyclonedx(manifest_data, npm_packages, py_packages)

    if args.spdx_out:
        args.spdx_out.parent.mkdir(parents=True, exist_ok=True)
        args.spdx_out.write_text(json.dumps(spdx23_doc, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"Wrote SPDX 2.3 SBOM to {args.spdx_out}")
        print(f"Bound to lockfile evidence: npm {parsed['lock_evidence']['npm']['sha256'][:16]}... / "
              f"python {parsed['lock_evidence']['python']['sha256'][:16]}...")

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
