#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Verify reproducible dependency locks, release manifest, and generated SBOM artifacts for Nakagawa Recomp."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

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


def verify_release_locks(manifest_path: Path) -> list[str]:
    errors = []
    if not manifest_path.is_file():
        return [f"Release manifest file missing: {manifest_path}"]

    try:
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
        errors.extend(verify_provenance_families(data))
        locks = data.get("release_locks", {})
        status = locks.get("status")
        if not status:
            errors.append("release_locks missing status field")
        lockfiles = data.get("lockfiles", {})
        if not lockfiles.get("npm") or not (manifest_path.parent.parent / lockfiles["npm"]).is_file():
            errors.append(f"npm lockfile missing or unreadable: {lockfiles.get('npm')}")
        if not lockfiles.get("python") or not (manifest_path.parent.parent / lockfiles["python"]).is_file():
            errors.append(f"python lockfile missing or unreadable: {lockfiles.get('python')}")

        pkg_json_path = manifest_path.parent.parent / "interface" / "package.json"
        errors.extend(verify_dashboard_toolchain_compatibility(pkg_json_path))
    except Exception as exc:
        errors.append(f"Failed to parse release manifest {manifest_path}: {exc}")

    return errors


def verify_sbom_matches(spdx_path: Path, manifest_path: Path, npm_lock_path: Path, py_lock_path: Path) -> list[str]:
    errors = []
    if not spdx_path.is_file():
        return [f"SPDX file missing: {spdx_path}"]

    try:
        spdx_data = json.loads(spdx_path.read_text(encoding="utf-8"))
        manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
        errors.extend(verify_provenance_families(manifest_data))

        npm_pkgs = generate_sbom.parse_npm_lockfile(npm_lock_path)
        py_pkgs = generate_sbom.parse_python_lockfile(py_lock_path)

        spdx_packages = spdx_data.get("packages", [])
        pkg_names = {p.get("name") for p in spdx_packages if isinstance(p, dict)}

        family_names = {
            family.get("name")
            for family in manifest_data.get("provenance_families", [])
            if isinstance(family, dict)
        }
        for family_name in family_names:
            if family_name not in pkg_names:
                errors.append(f"provenance family {family_name} missing from SPDX SBOM")

        if manifest_data.get("name") not in pkg_names:
            errors.append(f"Root package {manifest_data.get('name')} missing from SPDX SBOM")

        for pkg in npm_pkgs:
            if pkg["name"] not in pkg_names:
                errors.append(f"NPM dependency {pkg['name']} missing from SPDX SBOM")

        for pkg in py_pkgs:
            if pkg["name"] not in pkg_names:
                errors.append(f"Python dependency {pkg['name']} missing from SPDX SBOM")

    except Exception as exc:
        errors.append(f"Failed to verify SPDX SBOM {spdx_path}: {exc}")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=ROOT / "assets" / "release_manifest.json")
    parser.add_argument("--npm-lock", type=Path, default=ROOT / "interface" / "package-lock.json")
    parser.add_argument("--py-lock", type=Path, default=ROOT / "tools" / "requirements-lock.txt")
    parser.add_argument("--spdx", type=Path, help="Path to SPDX 2.3 JSON SBOM to verify")

    args = parser.parse_args(argv)

    errors = verify_release_locks(args.manifest)
    if args.spdx:
        errors.extend(verify_sbom_matches(args.spdx, args.manifest, args.npm_lock, args.py_lock))

    if errors:
        for err in errors:
            print(f"SBOM Verification FAIL: {err}", file=sys.stderr)
        return 1

    print("SBOM Verification: OK (All release dependency locks and SBOM elements verified)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
