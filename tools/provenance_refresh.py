#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Generate the public provenance controls with the hosted attestation logic.

Stage the intended candidate changes first. The trusted base defaults to the
merge-base of HEAD and origin/main; set ``PROVENANCE_BASE_SHA`` when the exact
pull-request base differs. The detailed authority remains external and is
never synthesized by this command.
"""

from __future__ import annotations

import argparse
from dataclasses import replace
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
LEDGER = "assets/public_provenance_ledger.json"
EXPORT = "PUBLIC_EXPORT.json"
POLICY = "assets/public_source_profile.json"

try:
    from . import provenance_attest_verify as verifier
    from . import provenance_ledger
    from .nk_core.git_isolation import isolated_git_env
except ImportError:
    import provenance_attest_verify as verifier
    import provenance_ledger
    from nk_core.git_isolation import isolated_git_env


def _run(repo: Path, *args: str, capture: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        list(args),
        cwd=repo,
        env=isolated_git_env(root=repo),
        text=True,
        capture_output=capture,
        check=False,
    )


def _base_commit(repo: Path, requested: str | None) -> str:
    if requested:
        if len(requested) != 40 or any(character not in "0123456789abcdef" for character in requested):
            raise verifier.VerifyError(
                "TRUSTED_BASE_REQUIRED", "the provenance base must be a full 40-character commit SHA",
            )
        return verifier._rev_commit(repo, requested)
    result = _run(repo, "git", "merge-base", "HEAD", "origin/main")
    value = result.stdout.strip()
    if result.returncode or len(value) != 40 or any(character not in "0123456789abcdef" for character in value):
        raise verifier.VerifyError(
            "TRUSTED_BASELINE_MISSING",
            "cannot resolve a trusted base; set PROVENANCE_BASE_SHA to the exact base commit",
        )
    return value


def generate_controls(
    *,
    repo: Path,
    base_rev: str,
    candidate_tree: str,
    trusted_ledger: Path,
    workdir: Path,
    trusted_candidate_policy: Path | None = None,
    policy_delta_authority: Path | None = None,
) -> verifier.EphemeralControls:
    """Call the same generator used by the hosted ephemeral verifier."""

    repo = repo.resolve()
    if (trusted_candidate_policy is None) != (policy_delta_authority is None):
        raise verifier.VerifyError(
            "POLICY_DELTA_ARGUMENT_REQUIRED",
            "the blessed candidate policy and policy-delta authority must be supplied together",
        )
    trusted_ledger = verifier._external_input(
        trusted_ledger, repo=repo, label="trusted detailed ledger",
    )
    if trusted_candidate_policy is not None:
        trusted_candidate_policy = verifier._external_input(
            trusted_candidate_policy, repo=repo, label="trusted candidate policy",
        )
    if policy_delta_authority is not None:
        policy_delta_authority = verifier._external_input(
            policy_delta_authority, repo=repo, label="policy delta authority",
        )
    workdir = workdir.resolve()
    if verifier._path_is_within(workdir, repo):
        raise verifier.VerifyError(
            "OUTPUT_CANDIDATE_CONTROLLED", "refresh scratch must live outside the candidate repository",
        )
    if workdir.exists() and not workdir.is_dir():
        raise verifier.VerifyError("OUTPUT_INVALID", "refresh scratch path is not a directory")
    base_commit = verifier._rev_commit(repo, base_rev)
    # The same authority-generated baseline the hosted attestation uses; the
    # base commit's committed ledger is not an input.
    baseline_bytes = verifier.generate_authority_baseline(
        repo=repo, base_rev=base_commit, trusted_ledger=trusted_ledger,
        workdir=workdir / "baseline-scratch",
    )
    baseline_path = workdir / "inputs" / "trusted-baseline.json"
    trusted_paths = {trusted_ledger.resolve()}
    if trusted_candidate_policy is not None:
        trusted_paths.add(trusted_candidate_policy.resolve())
    if policy_delta_authority is not None:
        trusted_paths.add(policy_delta_authority.resolve())
    if baseline_path.resolve() in trusted_paths:
        raise verifier.VerifyError(
            "OUTPUT_TRUSTED_INPUT_COLLISION", "refresh scratch would overwrite a trusted input",
        )
    if baseline_path.exists():
        raise verifier.VerifyError("OUTPUT_FILE_COLLISION", "refresh scratch already contains its baseline file")
    workdir.mkdir(parents=True, exist_ok=True)
    baseline_path.parent.mkdir(parents=True, exist_ok=True)
    baseline_path.write_bytes(baseline_bytes)
    controls = verifier.generate_ephemeral_controls(
        repo=repo,
        candidate_tree=candidate_tree,
        base_rev=base_commit,
        trusted_ledger=trusted_ledger,
        trusted_baseline=baseline_path,
        workdir=workdir,
        trusted_candidate_policy=trusted_candidate_policy,
        policy_delta_authority=policy_delta_authority,
    )
    if (
        controls.candidate_blobs.get(verifier.POLICY_PATH)
        != controls.base_blobs.get(verifier.POLICY_PATH)
        and controls.policy_delta is None
    ):
        raise verifier.VerifyError(
            "POLICY_DELTA_AUTHORITY_REQUIRED",
            "a changed publication profile needs the external blessed policy and delta authority",
        )
    changed = sorted(
        path for path in set(controls.base_blobs) | set(controls.candidate_blobs)
        if path not in verifier.CONTROL_PATHS | {verifier.POLICY_PATH}
        and controls.base_blobs.get(path) != controls.candidate_blobs.get(path)
    )
    if not changed:
        return controls
    if any(path not in controls.base_blobs or path not in controls.candidate_blobs for path in changed):
        raise verifier.VerifyError(
            "REFRESH_NEW_PATH_REQUIRES_ADMISSION",
            "public path additions or removals require the separate admission workflow",
        )
    if not set(changed) <= {entry["path"] for entry in controls.generated_ledger["entries"]}:
        raise verifier.VerifyError(
            "REFRESH_PATH_NOT_PUBLIC", "changed paths must all be present in the trusted public scope",
        )

    reconciled = verifier._reconcile_refresh_audit(
        generated=controls.generated_ledger,
        candidate_blobs=controls.candidate_blobs,
        base_blobs=controls.base_blobs,
        base_tree=controls.base_tree,
        authorized_policy_delta=controls.policy_delta,
    )
    candidate_ledger = controls.candidate_blobs.get(LEDGER)
    if candidate_ledger != verifier._canonical_json_bytes(reconciled):
        audit = {
            "workflow": "refresh-reviewed",
            "trusted_tree": controls.base_tree,
            "candidate_tree": controls.candidate_tree,
            "refreshed_paths": changed,
        }
        if controls.policy_delta is not None:
            audit["policy_delta"] = controls.policy_delta
            audit["blessed_candidate_policy_sha256"] = hashlib.sha256(
                controls.candidate_policy_raw
            ).hexdigest()
        declared = dict(controls.generated_ledger)
        declared["refresh"] = audit
        candidate_blobs = dict(controls.candidate_blobs)
        candidate_blobs[LEDGER] = verifier._canonical_json_bytes(declared)
        reconciled = verifier._reconcile_refresh_audit(
            generated=controls.generated_ledger,
            candidate_blobs=candidate_blobs,
            base_blobs=controls.base_blobs,
            base_tree=controls.base_tree,
            authorized_policy_delta=controls.policy_delta,
        )
        if reconciled.get("refresh") != audit:
            raise verifier.VerifyError(
                "REFRESH_AUDIT_INVALID", "generated refresh metadata did not pass attestation reconciliation",
            )
    ledger_bytes = verifier._canonical_json_bytes(reconciled)
    export = verifier._generate_ephemeral_export(
        candidate_blobs=controls.candidate_blobs,
        candidate_policy=controls.candidate_policy,
        ledger_bytes=ledger_bytes,
    )
    return replace(
        controls,
        generated_ledger=reconciled,
        generated_ledger_bytes=ledger_bytes,
        generated_export=export,
        generated_export_bytes=verifier._canonical_json_bytes(export),
    )


def _policy_sync(repo: Path, *, apply_policy: bool) -> None:
    command = [sys.executable, "tools/policy_sync.py"]
    result = _run(repo, *command)
    if result.returncode == 0:
        return
    if not apply_policy:
        raise verifier.VerifyError(
            "POLICY_UNCLASSIFIED",
            "tracked paths need a reviewed publication-policy decision; use --apply-policy only for routine paths",
        )
    result = _run(repo, *command, "--apply")
    if result.returncode:
        raise verifier.VerifyError(
            "POLICY_SYNC_FAILED", "policy_sync refused the publication-policy update",
        )
    staged = _run(repo, "git", "add", "--", POLICY)
    if staged.returncode:
        raise verifier.VerifyError("GIT_ERROR", "could not stage the generated publication profile")


def _audit(repo: Path, *, worktree: bool) -> subprocess.CompletedProcess:
    command = [
        sys.executable, "tools/publish_audit.py", "--tracked-only", "--public-scope",
        "--provenance-self-consistency",
    ]
    if worktree:
        command.append("--worktree")
    return _run(repo, *command)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--trusted-ledger", "--implementation-ledger", dest="trusted_ledger", type=Path,
        default=os.environ.get("NK_TRUSTED_LEDGER"),
        help="external maintainer-controlled IMPLEMENTATION_PROVENANCE.json",
    )
    parser.add_argument(
        "--base", default=os.environ.get("PROVENANCE_BASE_SHA"),
        help="exact trusted base commit; defaults to merge-base(HEAD, origin/main)",
    )
    parser.add_argument(
        "--trusted-candidate-policy", type=Path,
        default=os.environ.get("PROVENANCE_TRUSTED_CANDIDATE_POLICY"),
        help="external blessed candidate publication policy for a reviewed policy delta",
    )
    parser.add_argument(
        "--policy-delta-authority", type=Path,
        default=os.environ.get("PROVENANCE_POLICY_DELTA_AUTHORITY"),
        help="external authority binding the baseline and blessed policy digests and exact delta",
    )
    parser.add_argument(
        "--apply-policy", action="store_true",
        help="add routine unclassified tracked paths after the publication decision has been made",
    )
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    try:
        if args.trusted_ledger is None:
            raise verifier.VerifyError(
                "TRUSTED_INPUT_MISSING", "set NK_TRUSTED_LEDGER to the external detailed authority",
            )
        repo = ROOT.resolve()
        trusted_ledger = verifier._external_input(
            args.trusted_ledger, repo=repo, label="trusted detailed ledger",
        )
        if (args.trusted_candidate_policy is None) != (args.policy_delta_authority is None):
            raise verifier.VerifyError(
                "POLICY_DELTA_ARGUMENT_REQUIRED",
                "the blessed candidate policy and policy-delta authority must be supplied together",
            )
        if args.trusted_candidate_policy is not None:
            verifier._external_input(
                args.trusted_candidate_policy, repo=repo, label="trusted candidate policy",
            )
            verifier._external_input(
                args.policy_delta_authority, repo=repo, label="policy delta authority",
            )
        clean = _run(repo, "git", "diff", "--quiet")
        if clean.returncode:
            raise verifier.VerifyError(
                "CANDIDATE_INDEX_STALE", "stage intended candidate changes before provenance refresh",
            )
        base_commit = _base_commit(repo, args.base)
        if not verifier._is_ancestor(repo, base_commit, "HEAD"):
            raise verifier.VerifyError(
                "TRUSTED_BASE_NOT_ANCESTOR", "the trusted base must be an ancestor of HEAD",
            )
        _policy_sync(repo, apply_policy=args.apply_policy)
        tree = _run(repo, "git", "write-tree")
        candidate_tree = tree.stdout.strip()
        if tree.returncode or len(candidate_tree) != 40:
            raise verifier.VerifyError("CANDIDATE_TREE_INVALID", "cannot read the staged candidate tree")

        with tempfile.TemporaryDirectory(prefix="nakagawa-provenance-refresh-") as scratch:
            controls = generate_controls(
                repo=repo,
                base_rev=base_commit,
                candidate_tree=candidate_tree,
                trusted_ledger=trusted_ledger,
                workdir=Path(scratch),
                trusted_candidate_policy=args.trusted_candidate_policy,
                policy_delta_authority=args.policy_delta_authority,
            )
            provenance_ledger._write_controls_atomic(
                [
                    (repo / LEDGER, controls.generated_ledger_bytes),
                    (repo / EXPORT, controls.generated_export_bytes),
                ],
                code="REFRESH_OUTPUT_ERROR",
            )

        staged = _run(repo, "git", "add", "--", LEDGER, EXPORT)
        if staged.returncode:
            raise verifier.VerifyError("GIT_ERROR", "could not stage generated provenance controls")
        failures = []
        for label, worktree in (("index", False), ("worktree", True)):
            audit = _audit(repo, worktree=worktree)
            if audit.returncode:
                failures.append(label)
                sys.stdout.write(audit.stdout or "")
                sys.stderr.write(audit.stderr or "")
        if failures:
            print(f"provenance refresh: FAIL ({', '.join(failures)} audit leg)")
            return 1
        print("provenance refresh: OK (hosted attestation generator; controls staged)")
        return 0
    except verifier.VerifyError as error:
        print(f"provenance refresh: {error.code}: {error}", file=sys.stderr)
        return 1
    except provenance_ledger.RefreshError as error:
        print(f"provenance refresh: {error.code}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
