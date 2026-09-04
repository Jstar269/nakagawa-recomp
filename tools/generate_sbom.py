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


def parse_npm_lockfile(lock_path: Path) -> list[dict]:
    packages: list[dict] = []
    if not lock_path.is_file():
        return packages

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

    try:
        data = json.loads(lock_path.read_text(encoding="utf-8"))
        raw_packages = data.get("packages", {})
        if isinstance(raw_packages, dict):
            for pkg_path, meta in sorted(raw_packages.items()):
                if not pkg_path or not isinstance(meta, dict):
                    continue
                name = meta.get("name") or pkg_path.split("node_modules/")[-1]
                version = meta.get("version", "0.0.0")
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
    except Exception:
        pass

    return packages


def parse_python_lockfile(lock_path: Path) -> list[dict]:
    packages: list[dict] = []
    if not lock_path.is_file():
        return packages

    try:
        text = lock_path.read_text(encoding="utf-8")
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            match = re.match(r"^([a-zA-Z0-9_.-]+)==([a-zA-Z0-9_.-]+)(?:\s+--hash=sha256:([0-9a-fA-F]{64}))?", line)
            if match:
                name, version, sha256_hash = match.groups()
                packages.append({
                    "name": name,
                    "version": version,
                    "spdx_id": f"SPDXRef-pip-{name}-{version}",
                    "license": "NOASSERTION",
                    "sha256": sha256_hash or "",
                    "purl": f"pkg:pypi/{name}@{version}",
                    "ecosystem": "pypi",
                })
    except Exception:
        pass

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
    npm_packages = parse_npm_lockfile(args.npm_lock)
    py_packages = parse_python_lockfile(args.py_lock)

    spdx23_doc = generate_spdx23(manifest_data, npm_packages, py_packages)
    spdx301_doc = generate_spdx301(manifest_data, npm_packages, py_packages)
    cyclonedx_doc = generate_cyclonedx(manifest_data, npm_packages, py_packages)

    if args.spdx_out:
        args.spdx_out.parent.mkdir(parents=True, exist_ok=True)
        args.spdx_out.write_text(json.dumps(spdx23_doc, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"Wrote SPDX 2.3 SBOM to {args.spdx_out}")

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
