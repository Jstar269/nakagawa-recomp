#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Single authoritative PUBLIC_EXPORT.json generator.

The export is deterministic evidence derived from the canonical policy and the
exact bytes supplied by the caller.  ``PUBLIC_EXPORT.json`` is excluded from
its own content digest to avoid a self-referential hash; the exclusion is
explicitly recorded and enforced by ``publish_audit.py``.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
EXPORT_PATH = "PUBLIC_EXPORT.json"
EXPORT_SCHEMA_VERSION = "2.0.0"


def content_digest(files: list[tuple[str, bytes]]) -> str:
    digest = hashlib.sha256()
    for path, raw in sorted(files, key=lambda item: item[0]):
        if path == EXPORT_PATH:
            continue
        digest.update(path.encode("utf-8") + b"\0" + hashlib.sha256(raw).hexdigest().encode("ascii") + b"\n")
    return digest.hexdigest()


def build_document(
    policy,
    files: list[tuple[str, bytes]],
    *,
    source_tree: str | None = None,
    candidate_tree: str | None = None,
    provenance_ledger: bytes | None = None,
    manifest: bytes | None = None,
    sbom_hashes: dict[str, str] | None = None,
    excluded_file_count: int | None = None,
) -> dict:
    included = [path for path, _ in files if policy.resolve(path).disposition == "included"]
    excluded_present = [path for path, _ in files if policy.resolve(path).disposition == "excluded"]
    document = {
        "export_schema_version": EXPORT_SCHEMA_VERSION,
        "tool": "tools/public_export.py",
        "generated_evidence": (
            "Generated from the canonical publication policy and exact source bytes. "
            "Evidence is not authorization to publish. PUBLIC_EXPORT.json is excluded "
            "from its own included-content digest to avoid a self-reference."
        ),
        "profile": policy.name,
        "policy_version": policy.profile_version,
        "policy_sha256": policy.digest,
        "audit_tool_version": "0.4.0",
        "tracked_file_count": len(files),
        "included_file_count": len(included),
        "exported_file_count": len(included),
        "excluded_file_count": len(excluded_present) if excluded_file_count is None else excluded_file_count,
        "included_content_sha256": content_digest([
            (path, raw) for path, raw in files if policy.resolve(path).disposition == "included"
        ]),
        "digest_excludes": [EXPORT_PATH],
        "excluded_paths": sorted(policy.exclude_paths),
        "excluded_globs": sorted(policy.exclude_globs),
        "excluded_present_paths": sorted(excluded_present),
    }
    if provenance_ledger is not None:
        document["provenance_ledger_sha256"] = hashlib.sha256(provenance_ledger).hexdigest()
    if manifest is not None:
        document["manifest_sha256"] = hashlib.sha256(manifest).hexdigest()
    if sbom_hashes:
        document["sbom_sha256"] = dict(sorted(sbom_hashes.items()))
    if source_tree:
        document["source_tree"] = source_tree
    if candidate_tree:
        document["candidate_tree"] = candidate_tree
    return document


def write_document(path: Path, document: dict) -> None:
    path.write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def index_files(repo_root: Path = ROOT) -> list[tuple[str, bytes]]:
    raw = subprocess.run(["git", "ls-files", "-s", "-z"], cwd=repo_root,
                         capture_output=True, check=True).stdout.decode("utf-8", errors="surrogateescape")
    requests: list[tuple[str, str]] = []
    for item in raw.split("\0"):
        parts = item.split(None, 3)
        if len(parts) == 4:
            requests.append((parts[3], parts[1]))
    proc = subprocess.Popen(["git", "cat-file", "--batch"], cwd=repo_root,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    assert proc.stdin and proc.stdout
    output, _ = proc.communicate(("".join(f"{sha}\n" for _, sha in requests)).encode("ascii"))
    result: list[tuple[str, bytes]] = []
    pos = 0
    for path, _ in requests:
        end = output.find(b"\n", pos)
        if end < 0:
            break
        header = output[pos:end].split()
        pos = end + 1
        if len(header) < 3 or header[1] != b"blob":
            continue
        size = int(header[2])
        result.append((path, output[pos:pos + size]))
        pos += size + 1
    return sorted(result)
