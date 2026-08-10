#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Verify checked-in VFPU data against its pinned PPSSPP provenance manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
ASSET_DIR = ROOT / "assets" / "vfpu"
MANIFEST = ASSET_DIR / "PROVENANCE.json"


def git_blob_id(data: bytes) -> str:
    header = f"blob {len(data)}\0".encode()
    return hashlib.sha1(header + data, usedforsecurity=False).hexdigest()


def load_manifest(path: Path = MANIFEST) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema_version") != 1 or not isinstance(data.get("files"), list):
        raise ValueError("unsupported or malformed VFPU provenance manifest")
    return data


def verify_local(manifest: dict, asset_dir: Path = ASSET_DIR) -> list[str]:
    errors: list[str] = []
    expected_names: set[str] = set()
    for entry in manifest["files"]:
        name = entry.get("path")
        if not isinstance(name, str) or Path(name).name != name:
            errors.append(f"invalid manifest path: {name!r}")
            continue
        expected_names.add(name)
        path = asset_dir / name
        if not path.is_file():
            errors.append(f"missing: {path}")
            continue
        data = path.read_bytes()
        if len(data) != entry.get("bytes"):
            errors.append(f"size mismatch: {name} ({len(data)} != {entry.get('bytes')})")
        actual_blob = git_blob_id(data)
        if actual_blob != entry.get("git_blob"):
            errors.append(f"blob mismatch: {name} ({actual_blob} != {entry.get('git_blob')})")

    actual_names = {path.name for path in asset_dir.glob("*.dat")}
    for extra in sorted(actual_names - expected_names):
        errors.append(f"unmanifested VFPU data: {extra}")
    return errors


def verify_upstream(manifest: dict, checkout: Path) -> list[str]:
    commit = manifest["source_commit"]
    command = ["git", "-C", str(checkout), "ls-tree", "-r", commit, "--", "assets/vfpu"]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        return [f"unable to inspect upstream checkout: {result.stderr.strip()}"]

    upstream = {}
    for line in result.stdout.splitlines():
        metadata, path = line.split("\t", 1)
        _mode, object_type, blob = metadata.split()
        if object_type == "blob":
            upstream[path] = blob

    errors = []
    for entry in manifest["files"]:
        upstream_path = entry["upstream_path"]
        if upstream.get(upstream_path) != entry["git_blob"]:
            errors.append(
                f"upstream mismatch: {upstream_path} "
                f"({upstream.get(upstream_path, 'missing')} != {entry['git_blob']})"
            )
    return errors


def _upstream_repo(manifest: dict) -> str:
    """owner/name parsed from the manifest's source_repository URL."""
    url = manifest.get("source_repository", "").rstrip("/")
    if url.endswith(".git"):
        url = url[:-4]
    parts = url.split("/")
    if len(parts) < 2:
        raise ValueError(f"cannot parse owner/name from {url!r}")
    return f"{parts[-2]}/{parts[-1]}"


def verify_upstream_api(manifest: dict) -> list[str]:
    """Verify manifest blobs against upstream via the GitHub REST API.

    Git blob ids are content-addressed identically across repositories, so a blob
    id that matches upstream at the pinned commit proves the file is byte-for-byte
    identical -- without cloning the (multi-GB) source repository. Requires the
    `gh` CLI and network access; a clear, single "skipped" error is returned when
    either is missing so this never silently passes and never hard-fails a
    hermetic build that opted in by mistake.
    """
    repo = _upstream_repo(manifest)
    commit = manifest["source_commit"]
    # One tree read for the whole subdirectory rather than a request per file.
    ref = f"{commit}:assets/vfpu"
    try:
        result = subprocess.run(
            ["gh", "api", f"repos/{repo}/git/trees/{ref}"],
            capture_output=True, text=True, check=False,
        )
    except FileNotFoundError:
        return ["upstream-api skipped: the `gh` CLI is not available"]
    if result.returncode != 0:
        return [f"upstream-api skipped: could not read {repo}@{ref}: {result.stderr.strip()}"]

    try:
        tree = json.loads(result.stdout).get("tree", [])
    except json.JSONDecodeError as error:
        return [f"upstream-api skipped: malformed tree response: {error}"]
    upstream = {e.get("path"): e.get("sha") for e in tree if e.get("type") == "blob"}

    errors: list[str] = []
    for entry in manifest["files"]:
        # upstream_path is "assets/vfpu/<name>"; the tree is already rooted there.
        name = Path(entry["upstream_path"]).name
        if upstream.get(name) != entry["git_blob"]:
            errors.append(
                f"upstream-api mismatch: {entry['upstream_path']} "
                f"({upstream.get(name, 'missing')} != {entry['git_blob']})"
            )
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--upstream-checkout",
        type=Path,
        help="optional local PPSSPP Git checkout containing the pinned commit",
    )
    parser.add_argument(
        "--upstream-api",
        action="store_true",
        help="verify blobs against upstream via the GitHub API (needs `gh` + network; "
             "no clone required)",
    )
    args = parser.parse_args(argv)

    try:
        manifest = load_manifest()
        errors = verify_local(manifest)
        if args.upstream_checkout:
            errors.extend(verify_upstream(manifest, args.upstream_checkout))
        if args.upstream_api:
            errors.extend(verify_upstream_api(manifest))
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"VFPU provenance verification failed: {error}", file=sys.stderr)
        return 1

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(
        f"VFPU provenance OK: {len(manifest['files'])} files match "
        f"PPSSPP {manifest['source_commit']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
