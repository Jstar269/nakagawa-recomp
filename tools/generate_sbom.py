#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
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


#: Schema version of the generated dependencyLockEvidence binding.
LOCKFILE_EVIDENCE_SCHEMA_VERSION = 1


def compute_lock_evidence(npm_lock_path: Path, py_lock_path: Path) -> dict:
    """Bind the generated SBOM to the exact lockfile bytes it was built from.

    Returns a deterministic, JSON-serializable record of the sha256 digest and
    size of each lockfile as read at generation time. Verification recomputes
    the digests of the current lockfiles and requires exact equality, so a
    stale SBOM (generated from different lock bytes) cannot pass even when the
    dependency names it contains happen to still match (issue #375).
    """
    evidence: dict = {
        "schema_version": LOCKFILE_EVIDENCE_SCHEMA_VERSION,
    }
    for kind, path in (("npm", npm_lock_path), ("python", py_lock_path)):
        try:
            raw = path.read_bytes()
        except OSError as exc:
            raise LockfileParseError(
                f"cannot read {kind} lockfile {path} for evidence binding: {exc}"
            ) from exc
        evidence[kind] = {
            "path": str(path),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "size": len(raw),
        }
    return evidence


def verify_lock_evidence(evidence: object, npm_lock_path: Path, py_lock_path: Path) -> list[str]:
    """Recompute current lockfile digests and require exact evidence equality."""
    errors: list[str] = []
    if not isinstance(evidence, dict):
        return ["dependencyLockEvidence missing or not an object"]
    if evidence.get("schema_version") != LOCKFILE_EVIDENCE_SCHEMA_VERSION:
        errors.append(
            f"unsupported dependencyLockEvidence schema_version {evidence.get('schema_version')!r}; "
            f"supported: {LOCKFILE_EVIDENCE_SCHEMA_VERSION}"
        )
    for kind, path in (("npm", npm_lock_path), ("python", py_lock_path)):
        entry = evidence.get(kind)
        if not isinstance(entry, dict):
            errors.append(f"dependencyLockEvidence missing {kind} lockfile entry")
            continue
        try:
            raw = path.read_bytes()
        except OSError as exc:
            errors.append(
                f"cannot read current {kind} lockfile {path} for lock-evidence verification: {exc}"
            )
            continue
        current_digest = hashlib.sha256(raw).hexdigest()
        if entry.get("sha256") != current_digest:
            errors.append(
                f"{kind} lockfile evidence mismatch: the SBOM was generated from different "
                f"lockfile bytes (evidence sha256 {entry.get('sha256')!r} vs current {path} "
                f"sha256 {current_digest}); regenerate the SBOM from the current lockfiles"
            )
        recorded_size = entry.get("size")
        if isinstance(recorded_size, int) and recorded_size != len(raw):
            errors.append(
                f"{kind} lockfile evidence size mismatch: evidence {recorded_size} byte(s) vs "
                f"current {len(raw)} byte(s)"
            )
    return errors


def expected_spdx23_dependency_ids(npm_packages: list[dict], py_packages: list[dict]) -> list[str]:
    """Return the exact SPDX element ids the lockfile inventory must produce.

    The list preserves multiplicity: a package installed at two lockfile paths
    yields two entries (the second disambiguated by path digest), and a verifier
    may only accept an SBOM that carries every one of them.
    """
    return [p["spdx_id"] for p in npm_packages] + [p["spdx_id"] for p in py_packages]


def _read_lockfile_text(lock_path: Path, kind: str) -> str:
    """Read a lockfile as UTF-8 text, failing closed on any access problem."""
    try:
        if not lock_path.is_file():
            raise LockfileParseError(f"{kind} lockfile is not an existing regular file: {lock_path}")
        return lock_path.read_text(encoding="utf-8")
    except LockfileParseError:
        raise
    except UnicodeDecodeError as exc:
        raise LockfileParseError(
            f"{kind} lockfile is not valid UTF-8: {lock_path}: {exc}"
        ) from exc
    except OSError as exc:
        raise LockfileParseError(f"cannot read {kind} lockfile {lock_path}: {exc}") from exc


def parse_npm_lockfile(lock_path: Path) -> list[dict]:
    """Parse an npm package-lock.json into an inventory of package records.

    Fails closed (raises LockfileParseError) on unreadable input, malformed
    JSON, an unsupported lockfile shape/version, or any package record that is
    missing required metadata. A lockfile whose `packages` map is genuinely
    empty is valid and yields an empty inventory.
    """
    text = _read_lockfile_text(lock_path, "npm")
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


def parse_python_lockfile(lock_path: Path) -> list[dict]:
    """Parse the pinned Python requirements lockfile into package records.

    The repository's declared format is one `name==version` pin per nonblank,
    non-comment line, with an optional single `--hash=sha256:<64 hex>` digest.
    Every other nonblank/non-comment line is a parse error (issue #375): the
    previous best-effort loop silently skipped requirement lines it could not
    parse, dropping them from the release inventory. A file whose only
    non-comment content is blank or comments is a valid empty lockfile and
    yields an empty inventory.
    """
    text = _read_lockfile_text(lock_path, "Python")

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


def generate_spdx23(manifest_data: dict, npm_packages: list[dict], py_packages: list[dict]) -> dict:
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

    return {
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
    try:
        npm_packages = parse_npm_lockfile(args.npm_lock)
        py_packages = parse_python_lockfile(args.py_lock)
    except LockfileParseError as exc:
        # Fail closed: never emit a partial/empty-inventory SBOM because a
        # declared lockfile could not be parsed completely (issue #375).
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    spdx23_doc = generate_spdx23(manifest_data, npm_packages, py_packages)
    spdx301_doc = generate_spdx301(manifest_data, npm_packages, py_packages)
    cyclonedx_doc = generate_cyclonedx(manifest_data, npm_packages, py_packages)

    # Bind the generated SBOM to the exact lockfile bytes it was built from.
    # Any later lockfile change makes old SBOMs fail verification.
    try:
        lock_evidence = compute_lock_evidence(args.npm_lock, args.py_lock)
    except LockfileParseError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    spdx23_doc["dependencyLockEvidence"] = lock_evidence

    if args.spdx_out:
        args.spdx_out.parent.mkdir(parents=True, exist_ok=True)
        args.spdx_out.write_text(json.dumps(spdx23_doc, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"Wrote SPDX 2.3 SBOM to {args.spdx_out}")
        print(f"Bound to lockfile evidence: npm {lock_evidence['npm']['sha256'][:16]}... / "
              f"python {lock_evidence['python']['sha256'][:16]}...")

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
