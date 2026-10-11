#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Refresh the two public provenance controls from the public controls alone.

``assets/public_provenance_ledger.json`` and ``PUBLIC_EXPORT.json`` are derived
from the tracked tree, so almost any change makes them stale: the ledger records
a SHA-256 per included path, and the export records the publication policy and
its digest.  Until now the only tool that could regenerate them needed the
private trusted detailed ledger, so an outside contributor could not make their
own pull request green and a maintainer had to push a refresh commit onto the
branch.

This tool regenerates both files from inputs a contributor already has: the
committed public ledger, the committed publication policy, and the candidate
tree (the staged index, exactly as the maintainer's refresh and the publication
audit read it).  It is the *same generation* every time: for every changed path
that already has a public entry it keeps that entry verbatim and replaces only
its ``sha256``; the export is rebuilt through the verifier's own
``_generate_ephemeral_export`` and ``public_export.build_control_document``, so
no part of the control format is reimplemented here.

What it does and does not guarantee
-----------------------------------

It **does** reproduce, byte for byte, the two control files the maintainer's
``provenance_refresh.py`` produces for the ordinary case: a change to one or more
paths that already have public entries and whose classification is not
``unresolved``.  It never reads, writes, or needs the private trusted ledger.

It **refuses** (named code, non-zero exit, nothing written) every case it cannot
describe safely:

* ``NEW_PUBLIC_PATH`` -- a changed path has no committed public entry, so there
  is no claim to carry forward;
* ``REMOVED_PUBLIC_PATH`` -- a changed path left the tree, so the refresh would
  have to decide that a published path stops being published;
* ``CHANGED_PATH_NOT_PUBLIC`` -- a changed path is in neither the committed
  ledger nor the change's scope;
* ``UNRESOLVED_ENTRY`` -- the changed path's committed entry carries no resolved
  claim, so there is nothing to carry forward;
* ``POLICY_CHANGED`` -- the publication profile itself changed, which needs the
  externally blessed candidate policy and policy-delta authority.

Usage-level refusals are separate and also fatal: ``CANDIDATE_INDEX_STALE`` (the
index has unstaged edits, so it is not the candidate the audit reads),
``TRUSTED_BASE_REQUIRED`` / ``TRUSTED_BASELINE_MISSING`` /
``TRUSTED_BASE_NOT_ANCESTOR`` (the base commit cannot be resolved),
``CANDIDATE_TREE_INVALID`` (the staged tree is unreadable) and
``GENERATED_LEDGER_INVALID`` (the regenerated ledger fails the public ledger's own
validator, which would be a bug here rather than a bad change).

It **does not** grant anything.  The public ledger it reads is candidate-editable,
so its output is an ordinary pull-request change, never a trusted generation.
The hosted **Trusted provenance attestation** re-derives the same controls from
the private authority at landing and refuses any disagreement
(``LEGACY_CONTROL_MISMATCH``), and it is the only party that can decide whether a
claim is backed.

It **cannot see** two things, because the public ledger does not carry them:

* **frozen debt** — a public entry whose claim disagrees with trusted derivation.
  The tool copies such an entry forward with a fresh hash, exactly as an honest
  refresh would; the landing verifier reports ``CLAIM_UNBACKED`` /
  ``CLASSIFICATION_DOWNGRADE`` / ``CONTENT_UNATTESTED`` for it.
* **a changed executable/security-sensitive path with no exact trusted record** —
  the landing verifier reports ``TRUSTED_PATH_MISSING`` / ``TRUSTED_PATH_UNQUALIFIED``.

So: run this to keep your own pull request honest and green, then let the
maintainer's trusted attestation re-check it.  If it refuses, the reason is in
the output; say so in the pull request and a maintainer admits the path.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import hashlib
import json
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

# Reused from the real tooling; none of these needs the trusted ledger.
import provenance_attest_verify as verifier  # noqa: E402
import provenance_ledger as ledger_tool  # noqa: E402
import public_export  # noqa: E402
from nk_core.git_isolation import isolated_git_env  # noqa: E402

LEDGER = verifier.LEDGER_PATH
EXPORT = verifier.EXPORT_PATH
POLICY = verifier.POLICY_PATH

#: Control files and the profile are never "changed paths": the profile has its
#: own refusal, and the controls are this tool's output.
_NOT_A_CHANGED_PATH = verifier.CONTROL_PATHS | {POLICY}


class RefreshRefused(RuntimeError):
    """One named refusal; the tool never proceeds past one."""

    def __init__(self, code: str, path: str, detail: str) -> None:
        super().__init__(detail)
        self.code = code
        self.path = path
        self.detail = detail


def _git(repo: Path, *args: str, check: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args],
        cwd=repo,
        env=isolated_git_env(root=repo),
        capture_output=True,
        text=True,
        check=check,
    )


def _resolve_base(repo: Path, requested: str | None) -> str:
    if requested is not None:
        try:
            return verifier._rev_commit(repo, requested)
        except verifier.VerifyError as error:
            raise RefreshRefused("TRUSTED_BASE_REQUIRED", "", str(error)) from error
    result = _git(repo, "merge-base", "HEAD", "origin/main")
    value = result.stdout.strip()
    if result.returncode or len(value) != 40:
        raise RefreshRefused(
            "TRUSTED_BASELINE_MISSING",
            "",
            "cannot resolve a base from merge-base(HEAD, origin/main); pass --base <commit>",
        )
    return value


def _changed_paths(base_blobs: dict[str, bytes], candidate_blobs: dict[str, bytes]) -> list[str]:
    """Paths whose bytes differ, excluding the profile and the two controls.

    This is the same set the verifier's ``_reconcile_refresh_audit`` computes
    inline; it is reimplemented here only because that comprehension is not
    exported.
    """
    return sorted(
        path
        for path in set(base_blobs) | set(candidate_blobs)
        if path not in _NOT_A_CHANGED_PATH
        and base_blobs.get(path) != candidate_blobs.get(path)
    )


def _generate(repo: Path, base: str) -> tuple[bytes, bytes, list[tuple[str, str, str]]]:
    """Return (ledger bytes, export bytes, rehash log) for the staged candidate."""

    if _git(repo, "diff", "--quiet").returncode:
        raise RefreshRefused(
            "CANDIDATE_INDEX_STALE", "",
            "stage your changes first (git add), so the refresh reads the same index the "
            "publication audit does",
        )

    base_commit = _resolve_base(repo, base)
    if not verifier._is_ancestor(repo, base_commit, "HEAD"):
        raise RefreshRefused("TRUSTED_BASE_NOT_ANCESTOR", "", "the base must be an ancestor of HEAD")

    base_tree = verifier._rev_tree(repo, base_commit)
    base_blobs = verifier.read_tree(repo, base_tree)
    tree_result = _git(repo, "write-tree")
    candidate_tree = tree_result.stdout.strip()
    if tree_result.returncode or len(candidate_tree) != 40:
        raise RefreshRefused("CANDIDATE_TREE_INVALID", "", "cannot read the staged candidate tree")
    candidate_blobs = verifier.read_tree(repo, candidate_tree)

    if candidate_blobs.get(POLICY) != base_blobs.get(POLICY):
        raise RefreshRefused(
            "POLICY_CHANGED", POLICY,
            "a change to the publication profile needs the externally blessed policy and "
            "policy-delta authority, which only a maintainer has; revert the profile or "
            "explain the change in the pull request",
        )

    base_ledger = verifier.strict_json(
        base_blobs[LEDGER], code="BASE_LEDGER_INVALID", label="committed public ledger",
    )
    base_entries = verifier._entry_map(
        base_ledger, code="BASE_LEDGER_INVALID", label="committed public ledger",
    )

    changed = _changed_paths(base_blobs, candidate_blobs)
    rehashed: list[tuple[str, str, str]] = []
    for path in changed:
        if path not in base_blobs:
            raise RefreshRefused(
                "NEW_PUBLIC_PATH", path,
                "this file has no committed public entry, so it cannot be refreshed; say in the "
                "pull request what it is and where it came from, and a maintainer admits the path",
            )
        if path not in candidate_blobs:
            raise RefreshRefused(
                "REMOVED_PUBLIC_PATH", path,
                "this file left the tree; removing a public path is a maintainer decision, not a "
                "hash refresh",
            )
        entry = base_entries.get(path)
        if entry is None:
            raise RefreshRefused(
                "CHANGED_PATH_NOT_PUBLIC", path,
                "this changed path has no committed public entry, so its change is not describable",
            )
        if entry.get("classification") == "unresolved":
            raise RefreshRefused(
                "UNRESOLVED_ENTRY", path,
                "the committed public entry is unresolved and cannot be refreshed; a maintainer "
                "must record its provenance first",
            )
        rehashed.append((path, str(entry.get("sha256")), hashlib.sha256(candidate_blobs[path]).hexdigest()))

    new_ledger = json.loads(json.dumps(base_ledger, ensure_ascii=False))
    by_path = {entry["path"]: entry for entry in new_ledger["entries"]}
    for path in changed:
        by_path[path]["sha256"] = hashlib.sha256(candidate_blobs[path]).hexdigest()

    errors = ledger_tool.validate_ledger(new_ledger, require_hashes=True, require_resolved=True)
    if errors:
        raise RefreshRefused("GENERATED_LEDGER_INVALID", "", errors[0])
    ledger_bytes = ledger_tool._canonical_json_bytes(new_ledger)

    with tempfile.TemporaryDirectory(prefix="contrib-refresh-policy-") as scratch:
        policy = verifier._load_policy_bytes(
            candidate_blobs[POLICY], Path(scratch), "policy.json", code="CANDIDATE_POLICY_INVALID",
        )
        export_document = verifier._generate_ephemeral_export(
            candidate_blobs=candidate_blobs,
            candidate_policy=policy,
            ledger_bytes=ledger_bytes,
        )
    export_bytes = ledger_tool._canonical_json_bytes(
        public_export.build_control_document(export_document)
    )
    return ledger_bytes, export_bytes, rehashed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", type=Path, default=ROOT,
                        help="repository to refresh (default: the repository this tool lives in)")
    parser.add_argument("--base", default=None,
                        help="base commit to diff against (default: merge-base(HEAD, origin/main))")
    parser.add_argument("--check", action="store_true",
                        help="report what would change; write nothing")
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    repo = args.repo.resolve()
    try:
        ledger_bytes, export_bytes, rehashed = _generate(repo, args.base)
    except RefreshRefused as error:
        print("contrib-refresh: REFUSED", file=sys.stderr)
        print(f"  {error.code}: {error.path or '<repository>'}: {error.detail}", file=sys.stderr)
        return 1
    except verifier.VerifyError as error:
        print(f"contrib-refresh: {error.code}: {error}", file=sys.stderr)
        return 2

    for path, old, new in rehashed:
        print(f"rehash {path} {old} -> {new}")

    if args.check:
        print(f"contrib-refresh: would rehash {len(rehashed)} path(s); wrote nothing (--check)")
        return 0

    ledger_target = repo / LEDGER
    export_target = repo / EXPORT
    ledger_target.parent.mkdir(parents=True, exist_ok=True)
    ledger_target.write_bytes(ledger_bytes)
    export_target.write_bytes(export_bytes)
    staged = _git(repo, "add", "--", LEDGER, EXPORT)
    if staged.returncode:
        print("contrib-refresh: GIT_ERROR: could not stage the refreshed controls", file=sys.stderr)
        return 2
    print(f"contrib-refresh: OK ({len(rehashed)} path(s) rehashed; controls staged)")
    print("  the maintainer's trusted attestation re-derives these files at landing")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
