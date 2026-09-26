#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Single authoritative PUBLIC_EXPORT.json generator.

The export is deterministic evidence derived from the canonical policy and the
exact bytes supplied by the caller.  ``PUBLIC_EXPORT.json`` is excluded from
its own content digest to avoid a self-referential hash; the exclusion is
explicitly recorded and enforced by ``publish_audit.py``.

Two shapes come out of this module.  :func:`build_document` produces the full
document, which is what the release export and every verification-time
recomputation use.  :func:`build_control_document` produces the *committed*
control written into the repository: the same document with the tree-wide
fields removed.  Those fields describe the whole tree at one instant -- the
included-content digest, the per-file counts, the digest of the ledger blob --
so committing them makes every unrelated merge rewrite the same line, and every
open pull request conflict on it.  They are recomputed and compared by
``provenance_attest_verify.py`` and ``publish_audit.py`` instead, so nothing is
weakened: a declared tree-derived field that does not match the recomputation
is still refused, and omitting one is the normal committed shape.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess

try:
    from .nk_core.git_isolation import isolated_git_env
except ImportError:
    from nk_core.git_isolation import isolated_git_env

ROOT = Path(__file__).resolve().parent.parent
EXPORT_PATH = "PUBLIC_EXPORT.json"
PROVENANCE_LEDGER_PATH = "assets/public_provenance_ledger.json"
EXPORT_SCHEMA_VERSION = "3.0.0"

#: Fields whose value depends on the whole tree rather than on the policy alone.
#: They are recomputed at verification time instead of being committed, which is
#: what lets two pull requests that touch different paths merge without a
#: conflict.  The list is part of the document itself, so a candidate cannot
#: reclassify a security-relevant field as "tree-derived" to escape the check.
EXPORT_TREE_DERIVED_FIELDS = (
    "excluded_file_count",
    "excluded_present_paths",
    "exported_file_count",
    "included_content_sha256",
    "included_file_count",
    "manifest_sha256",
    "provenance_ledger_sha256",
    "tracked_file_count",
)


#: Fields the committed control also omits: the advisory tree identifiers.
#: They are never verified -- the verdict binds the candidate tree through Git
#: objects -- and committing a commit id would put one more line in the same
#: every-merge-rewrites-it category.
EXPORT_CONTROL_OMITTED_FIELDS = EXPORT_TREE_DERIVED_FIELDS + ("candidate_tree", "source_tree")


def build_control_document(document: dict) -> dict:
    """Return the committed form of a full export document.

    Only the tree-wide and commit-bound fields are dropped.  Everything the
    policy decides -- the profile, the policy version and digest, the complete
    exclusion disposition, the schema version -- stays, because those are the
    fields a human decision can move, and moving them must still conflict
    visibly in review.
    """
    return {
        key: value
        for key, value in document.items()
        if key not in EXPORT_CONTROL_OMITTED_FIELDS
    }


def is_control_document(document: dict) -> bool:
    """True when *document* omits at least one tree-derived field."""
    return any(field not in document for field in EXPORT_TREE_DERIVED_FIELDS)


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
    exclude_generated_controls: bool = False,
) -> dict:
    source_files = [
        (path, raw) for path, raw in files
        if not (exclude_generated_controls and path in {EXPORT_PATH, PROVENANCE_LEDGER_PATH})
    ]
    included = [path for path, _ in source_files if policy.resolve(path).disposition == "included"]
    excluded_present = [path for path, _ in source_files if policy.resolve(path).disposition == "excluded"]
    document = {
        "export_schema_version": EXPORT_SCHEMA_VERSION,
        "tool": "tools/public_export.py",
        "generated_evidence": (
            "Generated from the canonical publication policy and exact source bytes. "
            "Evidence is not authorization to publish. PUBLIC_EXPORT.json is excluded "
            "from its own included-content digest to avoid a self-reference. The fields "
            "named in tree_derived_fields are recomputed at verification time; the "
            "committed control omits them so unrelated merges do not conflict."
        ),
        "profile": policy.name,
        "policy_version": policy.profile_version,
        "policy_sha256": policy.digest,
        "audit_tool_version": "0.5.0",
        "tree_derived_fields": list(EXPORT_TREE_DERIVED_FIELDS),
        "tracked_file_count": len(source_files),
        "included_file_count": len(included),
        "exported_file_count": len(included),
        "excluded_file_count": len(excluded_present) if excluded_file_count is None else excluded_file_count,
        "included_content_sha256": content_digest([
            (path, raw) for path, raw in source_files if policy.resolve(path).disposition == "included"
        ]),
        "digest_excludes": sorted(
            {EXPORT_PATH, PROVENANCE_LEDGER_PATH}
            if exclude_generated_controls
            else {EXPORT_PATH}
        ),
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


def write_control_document(path: Path, document: dict) -> None:
    """Write the committed control: the full document minus tree-wide fields."""
    write_document(path, build_control_document(document))


def write_document(path: Path, document: dict) -> None:
    """Write canonical UTF-8 JSON with LF bytes on every supported host."""
    path.write_text(
        json.dumps(document, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def index_files(repo_root: Path = ROOT) -> list[tuple[str, bytes]]:
    env = isolated_git_env(root=repo_root)
    raw = subprocess.run(["git", "ls-files", "-s", "-z"], cwd=repo_root,
                         env=env, capture_output=True, check=True).stdout.decode("utf-8", errors="surrogateescape")
    requests: list[tuple[str, str]] = []
    for item in raw.split("\0"):
        parts = item.split(None, 3)
        if len(parts) == 4:
            requests.append((parts[3], parts[1]))
    proc = subprocess.Popen(["git", "cat-file", "--batch"], cwd=repo_root,
                            env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
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
