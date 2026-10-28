#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Admit new public paths with one command or a batch.

Adding a public path used to be a manual ritual in five steps, and each skipped
or misordered step failed with its own code (``CANDIDATE_TREE_STALE``,
``ADMISSION_CLASS_MISMATCH``, ``REFRESH_NEW_PATH_REQUIRES_ADMISSION``,
``TRUSTED_BASE_NOT_ANCESTOR``)::

    python tools/admit_paths.py <pr-worktree> <path> [<path> ...] \\
        --approval-basis "Owner request of <date>: ..." --private-repo <authority checkout>

    python tools/admit_paths.py --batch admissions.json --base <shared-base> \\
        --private-repo <authority checkout>

    python tools/admit_paths.py --merge-profile <worktree>

The command does the mechanical parts and nothing else:

1. reads each path's exact committed bytes from the pull request's ``HEAD``;
2. derives each path's classification with the gate's own classifier
   (``provenance_ledger``); an implementation path needs an exact record that
   already exists in the private ``IMPLEMENTATION_PROVENANCE.json`` -- this
   command never writes one;
3. writes the private records under ``docs/provenance/`` in the authority
   checkout: each admission has its own document and nested policy delta,
   keyed by candidate and baseline profile SHA-256, then commits only those
   files locally. ``--batch`` accepts two or more independent PR entries,
   writes one admission per PR, and pre-issues every possible landing-order
   delta for batches up to ``--any-order-limit`` (default 6). Larger batches
   get a chain for the listed order, with no batch-size cap;
4. builds a clean candidate commit holding only the base, the new paths and
   their profile entries, runs ``admit-new-reviewed`` on it, and commits the
   generated controls on a local ``admit/<slug>-<date>`` branch;
5. prints exactly what is left: the private push, the merge and the refresh.

``--rekey`` is for a pull request whose paths were already admitted outside a
batch but whose base moved because another path-adding pull request landed
first. It reuses the admission already recorded for exactly these paths and
bytes and records no new decision. ``--batch-resume`` reissues a listed-order
chain for remaining entries from their recorded admissions after earlier
entries landed. PRs admitted together in an all-order batch may land in any
order; run ``--merge-profile`` during the merge to write the three-way merge
of the path lists before staging the profile.

It pushes nothing unless ``--push`` is given, and then only the private
``main``. The approval basis is the human decision and is taken verbatim from
the caller; the command never invents one, a record, an identity, or a
``Signed-off-by:`` trailer.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import provenance_ledger  # noqa: E402
from provenance_ledger import RefreshError  # noqa: E402
import public_export  # noqa: E402
from publication_policy import load_policy  # noqa: E402

POLICY = "assets/public_source_profile.json"
EXPORT = "PUBLIC_EXPORT.json"
TRUSTED_LEDGER = "docs/provenance/IMPLEMENTATION_PROVENANCE.json"
ADMISSIONS = "docs/provenance/admissions"
POLICY_DELTAS = "docs/provenance/policy-deltas"

#: Variables that would point a Git command at a different repository or index.
#: Identity and configuration are kept: commits are the operator's own.
_REPOSITORY_VARIABLES = (
    "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
    "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_COMMON_DIR", "GIT_NAMESPACE",
)


class AdmitError(RuntimeError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def _git(repo: Path, *args: str, stdin: bytes | None = None) -> bytes:
    env = {key: value for key, value in os.environ.items() if key not in _REPOSITORY_VARIABLES}
    result = subprocess.run(
        ["git", *args], cwd=repo, env=env, input=stdin, capture_output=True, check=False,
    )
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise AdmitError("GIT_ERROR", detail or f"git {' '.join(args)} failed")
    return result.stdout


def _text(repo: Path, *args: str) -> str:
    return _git(repo, *args).decode("utf-8").strip()


def _canonical(document: dict) -> bytes:
    """The byte form of every private authority record: sorted keys, LF, final newline."""
    return (json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")


def _canonical_profile_bytes(document: dict) -> bytes:
    """Serialize a profile with sorted path lists, two-space JSON and one final LF."""
    canonical = dict(document)
    for field in ("include_paths", "exclude_paths"):
        paths = canonical.get(field)
        if not isinstance(paths, list) or not all(isinstance(path, str) for path in paths):
            raise AdmitError("POLICY_PROFILE_INVALID", f"profile {field} must be a list of strings")
        canonical[field] = sorted(set(paths))
    return (json.dumps(canonical, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def _candidate_policy(base_policy: bytes, added_paths: set[str]) -> tuple[bytes, dict]:
    """Build the canonical profile for a base profile plus exact included paths."""
    try:
        document = json.loads(base_policy.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AdmitError("POLICY_PROFILE_INVALID", f"cannot read base profile JSON: {error}") from error
    if not isinstance(document, dict):
        raise AdmitError("POLICY_PROFILE_INVALID", "base profile must be a JSON object")
    includes = document.get("include_paths")
    if not isinstance(includes, list) or not all(isinstance(path, str) for path in includes):
        raise AdmitError("POLICY_PROFILE_INVALID", "profile include_paths must be a list of strings")
    candidate = dict(document)
    candidate["include_paths"] = sorted(set(includes) | set(added_paths))
    raw = _canonical_profile_bytes(candidate)
    return raw, candidate


def _policy_delta_records(baseline_policy: bytes, candidate_policy: bytes, added_paths: list[str]) -> tuple[str, bytes]:
    """Return the candidate digest and exact authority for one nested policy delta."""
    try:
        baseline_document = json.loads(baseline_policy.decode("utf-8"))
        candidate_document = json.loads(candidate_policy.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AdmitError("POLICY_PROFILE_INVALID", f"cannot read policy JSON: {error}") from error
    delta = provenance_ledger._classify_policy_delta(baseline_document, candidate_document)
    if delta["include_added"] != sorted(added_paths) or any(
        delta[key] for key in ("include_removed", "exclude_added", "exclude_removed", "rule_changes")
    ):
        raise AdmitError("POLICY_DELTA_UNEXPECTED", f"the derived policy delta is not exactly the new paths: {delta}")
    candidate_sha = hashlib.sha256(candidate_policy).hexdigest()
    authority = _canonical({
        "allowed_delta": delta,
        "baseline_policy_sha256": hashlib.sha256(baseline_policy).hexdigest(),
        "candidate_policy_sha256": candidate_sha,
        "kind": provenance_ledger.POLICY_DELTA_KIND,
        "schema_version": provenance_ledger.POLICY_DELTA_SCHEMA_VERSION,
    })
    return candidate_sha, authority


def _blob(repo: Path, rev: str, path: str) -> tuple[str, str]:
    """Return ``(mode, blob sha)`` for ``path`` at ``rev``, or refuse a non-blob."""
    listing = _git(repo, "ls-tree", "-z", rev, "--", path).decode("utf-8")
    entry = listing.rstrip("\0")
    if not entry or "\0" in entry:
        raise AdmitError("ADMIT_PATH_MISSING", f"{path} is not committed at the pull request HEAD")
    meta, _, listed = entry.partition("\t")
    mode, kind, sha = meta.split()
    if kind != "blob" or listed != path or mode not in ("100644", "100755"):
        raise AdmitError("ADMIT_PATH_NOT_FILE", f"{path} is not a regular file at the pull request HEAD")
    return mode, sha


def default_slug(branch: str, paths: list[str]) -> str:
    """``docs/recomp-fyi-listing-20261022`` -> ``recomp-fyi-listing``."""
    name = branch.rsplit("/", 1)[-1] if branch else ""
    name = re.sub(r"-(\d{8}|\d{4}-\d{2}-\d{2})$", "", name)
    if not name:
        name = PurePosixPath(paths[0]).name.lstrip(".")
    slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
    if not slug:
        raise AdmitError("ADMIT_SLUG_REQUIRED", "cannot derive a record name; pass --slug")
    return slug


def classify(
    paths: list[str], records: dict[str, dict], origins: dict[str, dict], *, require_origin: bool = True,
) -> list[dict]:
    """Derive one admission statement per path with the gate's own classifier.

    Deterministic material (documentation, configuration, metadata, data
    fixtures) takes the class the path rule derives and carries no record id.
    Executable or security-sensitive paths, and anything the path rule cannot
    classify, need an exact trusted record; the statement then cites it and
    carries the reviewer's origin metadata.
    """
    statements = []
    for path in paths:
        derived, _ = provenance_ledger._class_for(path, None)
        if not provenance_ledger.admission_requires_implementation(path) and derived != "unresolved":
            statements.append({"path": path, "classification": derived})
            continue
        record = records.get(path)
        if record is None:
            raise AdmitError(
                "PROVENANCE_UNRESOLVED",
                f"{path} needs an exact record in the private {TRUSTED_LEDGER} before it can be admitted; "
                "this command never writes one",
            )
        classification, _ = provenance_ledger._refresh_class_for(path, record)
        if classification not in provenance_ledger.REFRESHABLE_CLASSES:
            raise AdmitError(
                "ADMISSION_CLASS_MISMATCH",
                f"the trusted record {record['id']!r} gives {path} the unadmittable class {classification!r}",
            )
        statement = {"path": path, "classification": classification, "record_id": record["id"]}
        if not require_origin:
            statements.append(statement)
            continue
        origin = origins.get(path, {})
        missing = [key for key in ("origin_kind", "origin", "license") if not origin.get(key)]
        if missing:
            raise AdmitError(
                "ADMISSION_AUTHORITY_INCOMPLETE",
                f"{path} is an implementation path: pass {', '.join('--' + key.replace('_', '-') for key in missing)}"
                " (or --origin-json)",
            )
        if origin["origin_kind"] not in provenance_ledger.ADMISSION_ORIGIN_KINDS:
            raise AdmitError(
                "ADMISSION_AUTHORITY_INCOMPLETE",
                f"origin kind must be one of {sorted(provenance_ledger.ADMISSION_ORIGIN_KINDS)}",
            )
        statements.append({
            **statement,
            "origin_kind": origin["origin_kind"], "origin": origin["origin"], "license": origin["license"],
        })
    return statements


def find_admission(authority_root: Path, statements: list[dict], digests: dict[str, str]) -> Path:
    """The one existing admission that already decided exactly these paths and bytes.

    Re-keying reuses that decision for a new base; it never makes a new one, so
    a path set or digest that no admission names is refused. The recorded
    classes are the decision and are reused as recorded: a reviewer may have
    admitted a path under a stricter class than the path rule derives today
    (for example an upstream licence file recorded as upstream_derived with its
    import record), and that must not block a re-key (2026-10-22, the Clay
    admission).
    """
    def key(statement: dict) -> tuple:
        return (statement["path"], statement["sha256"])

    wanted = sorted(key({**statement, "sha256": digests[statement["path"]]}) for statement in statements)
    matches = []
    for candidate in sorted((authority_root / ADMISSIONS).glob("*.json")):
        try:
            document = json.loads(candidate.read_text(encoding="utf-8"))
            named = sorted(key(statement) for statement in document["reviewed_new_paths"])
        except (ValueError, KeyError, TypeError):
            continue
        if document.get("kind") == provenance_ledger.ADMISSION_KIND and named == wanted:
            matches.append(candidate)
    if len(matches) != 1:
        raise AdmitError(
            "REKEY_ADMISSION_MISSING" if not matches else "REKEY_ADMISSION_AMBIGUOUS",
            f"--rekey needs exactly one existing admission naming exactly these paths and bytes; found {len(matches)}",
        )
    return matches[0]


def build_records(
    *,
    base_policy: bytes,
    paths: list[str],
    digests: dict[str, str],
    statements: list[dict],
    approval_basis: str,
) -> tuple[bytes, str, bytes, bytes]:
    """Return ``(candidate policy, its sha256, delta authority, admission)`` bytes."""
    candidate_policy, _candidate_document = _candidate_policy(base_policy, set(paths))
    candidate_sha, authority = _policy_delta_records(base_policy, candidate_policy, paths)
    admission = _canonical({
        "approval_basis": approval_basis,
        "kind": provenance_ledger.ADMISSION_KIND,
        "reviewed_new_paths": [
            {**statement, "sha256": digests[statement["path"]]}
            for statement in sorted(statements, key=lambda statement: statement["path"])
        ],
        "schema_version": provenance_ledger.ADMISSION_SCHEMA_VERSION,
    })
    return candidate_policy, candidate_sha, authority, admission


def _control_export(candidate: Path, policy_bytes: bytes) -> bytes:
    """The committed ``PUBLIC_EXPORT.json`` control for the candidate index (``policy_sync --regen-export``)."""
    with tempfile.TemporaryDirectory(prefix="nakagawa-admit-export-") as scratch:
        policy_path = Path(scratch) / "policy.json"
        policy_path.write_bytes(policy_bytes)
        policy = load_policy(policy_path)
    files = dict(public_export.index_files(candidate))
    document = public_export.build_document(
        policy, sorted(files.items()),
        provenance_ledger=files.get("assets/public_provenance_ledger.json"),
        manifest=files.get("assets/release_manifest.json"),
    )
    control = public_export.build_control_document(document)
    return (json.dumps(control, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def _stage_bytes(repo: Path, path: str, raw: bytes) -> None:
    """Stage exact bytes, bypassing line-ending filters, so the tree is host-independent."""
    sha = _git(repo, "hash-object", "-w", "--no-filters", "--stdin", stdin=raw).decode("ascii").strip()
    _git(repo, "update-index", "--add", "--cacheinfo", f"100644,{sha},{path}")


def _commit(repo: Path, message: str, assisted_by: str | None, *paths: str) -> str:
    if assisted_by:
        message += f"\n\nAssisted-by: {assisted_by}"
    _git(repo, "commit", "-q", "-m", message, *(("--", *paths) if paths else ()))
    return _text(repo, "rev-parse", "HEAD")


def _remove_scratch(path: Path) -> None:
    """``rmtree`` that clears the read-only bit Git sets on object files on Windows."""

    def _onexc(func, target, _exc):
        try:
            os.chmod(target, stat.S_IWRITE)
            func(target)
        except OSError:
            pass

    shutil.rmtree(path, onexc=_onexc)


def _write_record(target: Path, raw: bytes, created: list[Path]) -> None:
    if target.exists():
        if target.read_bytes() != raw:
            raise AdmitError(
                "ADMISSION_RECORD_CONFLICT",
                f"{target.name} already exists in the authority checkout with different bytes",
            )
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(raw)
    created.append(target)


def _authority_inputs(private_repo: Path) -> tuple[Path, Path, dict[str, dict]]:
    """Validate the private checkout once and return its committed ledger records."""
    authority_root = Path(_text(private_repo, "rev-parse", "--show-toplevel"))
    if _text(authority_root, "rev-parse", "--abbrev-ref", "HEAD") != "main":
        raise AdmitError("AUTHORITY_NOT_MAIN", "the authority checkout must have main checked out")
    if _text(authority_root, "status", "--porcelain=v1", "--untracked-files=all", "--", "docs/provenance"):
        raise AdmitError("AUTHORITY_DIRTY", "docs/provenance in the authority checkout has uncommitted changes")
    authority_ledger = authority_root / TRUSTED_LEDGER
    records = provenance_ledger._detailed_records(json.loads(authority_ledger.read_text(encoding="utf-8")))
    return authority_root, authority_ledger, records


def _prepare_admission(
    *,
    pr_worktree: Path,
    paths: list[str],
    approval_basis: str | None,
    authority_root: Path,
    authority_ledger: Path,
    records: dict[str, dict],
    base: str | None,
    slug: str | None,
    date: str,
    origins: dict[str, dict] | None,
    assisted_by: str | None,
    rekey: bool,
    scratch_root: Path | None,
) -> dict:
    """Build one admission branch and return its private record bytes without committing them."""
    if rekey and approval_basis is not None:
        raise AdmitError("REKEY_BASIS_REJECTED", "--rekey reuses the recorded decision; omit --approval-basis")
    if not rekey and not (approval_basis or "").strip():
        raise AdmitError("ADMISSION_BASIS_REQUIRED", "--approval-basis must state the human decision")
    try:
        exact = [provenance_ledger._exact_path(path, code="ADMIT_PATH_NOT_EXACT") for path in paths]
    except RefreshError as error:
        raise AdmitError(error.code, str(error)) from error
    if not exact or len(exact) != len(set(exact)):
        raise AdmitError("ADMIT_PATHS_INVALID", "name each new path exactly once")
    paths = sorted(exact)
    for path in paths:
        if path in provenance_ledger.REFRESH_CONTROL_PATHS:
            raise AdmitError("ADMISSION_PATH_FORBIDDEN", f"generated/control path cannot be admitted: {path}")

    pr = Path(_text(pr_worktree, "rev-parse", "--show-toplevel"))
    head = _text(pr, "rev-parse", "HEAD")
    if base is None:
        base = _text(pr, "merge-base", "HEAD", "origin/main")
    base = _text(pr, "rev-parse", "--verify", f"{base}^{{commit}}")
    try:
        _git(pr, "merge-base", "--is-ancestor", base, head)
    except AdmitError:
        raise AdmitError(
            "TRUSTED_BASE_NOT_ANCESTOR", f"base {base[:12]} is not an ancestor of the pull request HEAD; merge it first",
        ) from None
    branch = _text(pr, "rev-parse", "--abbrev-ref", "HEAD")
    slug = slug or default_slug("" if branch == "HEAD" else branch, paths)
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", date) or not re.fullmatch(r"[a-z0-9][a-z0-9-]*", slug):
        raise AdmitError("ADMIT_NAME_INVALID", "--slug must be lowercase words joined by '-', --date YYYY-MM-DD")

    blobs: dict[str, tuple[str, str]] = {}
    digests: dict[str, str] = {}
    dirty = _text(pr, "status", "--porcelain=v1", "--", *paths)
    if dirty:
        raise AdmitError("ADMIT_PATH_UNCOMMITTED", f"commit the new paths before admitting them:\n{dirty}")
    base_policy = _git(pr, "cat-file", "blob", f"{base}:{POLICY}")
    with tempfile.TemporaryDirectory(prefix="nakagawa-admit-base-") as policy_scratch:
        policy_path = Path(policy_scratch) / "policy.json"
        policy_path.write_bytes(base_policy)
        policy = load_policy(policy_path)
    for path in paths:
        if _git(pr, "ls-tree", base, "--", path):
            raise AdmitError("ADMISSION_PATH_EXISTING", f"{path} already exists at the base; it is not a new path")
        disposition = policy.resolve(path).disposition
        if disposition != "unclassified":
            raise AdmitError(
                "ADMISSION_PATH_EXCLUDED" if disposition == "excluded" else "ADMISSION_POLICY_MISMATCH",
                f"{path} is already {disposition} by the base publication policy",
            )
        blobs[path] = _blob(pr, head, path)
        digests[path] = hashlib.sha256(_git(pr, "cat-file", "blob", blobs[path][1])).hexdigest()

    statements = classify(paths, records, origins or {}, require_origin=not rekey)
    admission_path = authority_root / ADMISSIONS / f"admission-{slug}-{date}.json"
    admit_branch = f"admit/{slug}-{date}"
    if rekey:
        admission_path = find_admission(authority_root, statements, digests)
        recorded = json.loads(admission_path.read_text(encoding="utf-8"))
        approval_basis = recorded["approval_basis"]
        statements = [
            {key: value for key, value in statement.items() if key != "sha256"}
            for statement in recorded["reviewed_new_paths"]
        ]
        admit_branch = f"admit/{slug}-{date}-{base[:8]}"
    candidate_policy, candidate_sha, authority_bytes, admission_bytes = build_records(
        base_policy=base_policy, paths=paths, digests=digests,
        statements=statements, approval_basis=approval_basis,
    )
    if rekey:
        admission_bytes = admission_path.read_bytes()
    baseline_sha = hashlib.sha256(base_policy).hexdigest()
    admission_rel = admission_path.relative_to(authority_root).as_posix()
    delta_dir = f"{POLICY_DELTAS}/{candidate_sha}/{baseline_sha}"
    record_bytes = {
        admission_rel: admission_bytes,
        f"{delta_dir}/candidate-policy.json": candidate_policy,
        f"{delta_dir}/policy-delta-authority.json": authority_bytes,
    }
    if _git(pr, "branch", "--list", admit_branch):
        raise AdmitError("ADMIT_BRANCH_EXISTS", f"local branch {admit_branch} already exists")

    scratch = Path(tempfile.mkdtemp(prefix="nakagawa-admit-", dir=scratch_root))
    try:
        candidate = scratch / "candidate"
        _git(scratch, "clone", "-q", "--shared", "--no-checkout", str(pr), str(candidate))
        _git(candidate, "checkout", "-q", "--detach", base)
        for path, (mode, sha) in blobs.items():
            _git(candidate, "update-index", "--add", "--cacheinfo", f"{mode},{sha},{path}")
        _stage_bytes(candidate, POLICY, candidate_policy)
        _stage_bytes(candidate, EXPORT, _control_export(candidate, candidate_policy))
        _git(candidate, "checkout-index", "-f", "--", *paths, POLICY, EXPORT)
        candidate_commit = _commit(candidate, f"candidate: admit {slug}", assisted_by)

        admission_authority = scratch / "admission-authority.json"
        admission_authority.write_bytes(admission_bytes)
        base_policy_path = scratch / "base-policy.json"
        base_policy_path.write_bytes(base_policy)
        try:
            provenance_ledger.admit_new_reviewed(
                trusted_ledger=authority_ledger,
                admission_authority=admission_authority,
                candidate_tree=str(candidate),
                trusted_tree=base,
                paths=paths,
                trusted_policy=base_policy_path,
            )
        except RefreshError as error:
            raise AdmitError(error.code, str(error)) from error
        _git(candidate, "add", "-A")
        admission_commit = _commit(candidate, f"provenance: admit {slug}", assisted_by)
        ledger = _git(candidate, "cat-file", "blob", f"{admission_commit}:assets/public_provenance_ledger.json")
        candidate_tree = _text(candidate, "rev-parse", f"{candidate_commit}^{{tree}}")
        _git(pr, "fetch", "-q", str(candidate), f"{admission_commit}:refs/heads/{admit_branch}")
    finally:
        _remove_scratch(scratch)
    return {
        "paths": paths,
        "statements": statements,
        "base": base,
        "candidate_commit": candidate_commit,
        "candidate_tree": candidate_tree,
        "admission_commit": admission_commit,
        "admit_branch": admit_branch,
        "candidate_policy_sha256": candidate_sha,
        "records": sorted(record_bytes),
        "record_bytes": record_bytes,
        "private_commit": None,
        "pushed": False,
        "ledger": ledger,
        "pr_worktree": pr,
        "authority_root": authority_root,
        "slug": slug,
        "date": date,
        "rekey": rekey,
    }


def _write_authority_commit(
    *, authority_root: Path, records: dict[str, bytes], message: str, assisted_by: str | None, push: bool,
) -> tuple[list[str], str, bool]:
    """Write a set of authority files and commit them together, optionally pushing private main."""
    created: list[Path] = []
    try:
        for relative, raw in sorted(records.items()):
            _write_record(authority_root / relative, raw, created)
    except BaseException:
        for target in created:
            target.unlink(missing_ok=True)
        raise
    private_commit = _text(authority_root, "rev-parse", "HEAD")
    if created:
        paths = sorted(target.relative_to(authority_root).as_posix() for target in created)
        _git(authority_root, "add", "--", *paths)
        private_commit = _commit(authority_root, message, assisted_by, *paths)
    pushed = False
    if push:
        _git(authority_root, "push", "-q", "origin", "HEAD:main")
        pushed = True
    return sorted(records), private_commit, pushed


def _remove_created_branches(plans: list[dict]) -> None:
    """Remove only admission refs created by this in-memory batch after a failure."""
    seen: set[tuple[str, str]] = set()
    for plan in plans:
        key = (str(plan["pr_worktree"]), plan["admit_branch"])
        if key in seen:
            continue
        seen.add(key)
        try:
            _git(Path(key[0]), "update-ref", "-d", f"refs/heads/{key[1]}")
        except AdmitError:
            pass


def admit(
    *,
    pr_worktree: Path,
    paths: list[str],
    approval_basis: str | None,
    private_repo: Path,
    base: str | None = None,
    slug: str | None = None,
    date: str | None = None,
    origins: dict[str, dict] | None = None,
    assisted_by: str | None = None,
    push: bool = False,
    rekey: bool = False,
    scratch_root: Path | None = None,
) -> dict:
    """Admit ``paths`` and write a nested candidate/baseline policy delta."""
    authority_root, authority_ledger, records = _authority_inputs(private_repo)
    resolved_date = date or datetime.date.today().isoformat()
    plan = _prepare_admission(
        pr_worktree=pr_worktree, paths=paths, approval_basis=approval_basis,
        authority_root=authority_root, authority_ledger=authority_ledger, records=records,
        base=base, slug=slug, date=resolved_date, origins=origins, assisted_by=assisted_by,
        rekey=rekey, scratch_root=scratch_root,
    )
    commit_kind = "re-key" if rekey else "admit"
    base_policy = _git(plan["pr_worktree"], "cat-file", "blob", f"{plan['base']}:{POLICY}")
    baseline_sha = hashlib.sha256(base_policy).hexdigest()
    record_paths, private_commit, pushed = _write_authority_commit(
        authority_root=authority_root,
        records=plan["record_bytes"],
        message=(
            f"provenance: {commit_kind} {plan['slug']} ({resolved_date})\n\n"
            f"Policy delta for candidate profile {plan['candidate_policy_sha256'][:8]} "
            f"(baseline {baseline_sha[:8]}); "
            f"admission binds {len(plan['paths'])} new path(s) to their exact SHA-256."
        ),
        assisted_by=assisted_by, push=push,
    )
    plan.update(records=record_paths, private_commit=private_commit, pushed=pushed)
    return plan


def _batch_entries(document: object, *, resume: bool = False) -> list[dict]:
    """Validate JSON entries and cross-entry path uniqueness before repository mutation."""
    if not isinstance(document, list):
        raise AdmitError("BATCH_FILE_INVALID", "--batch JSON must be a list of entries")
    if len(document) < 2:
        raise AdmitError("BATCH_SIZE_INVALID", "batch operations need at least 2 entries")
    normalized = []
    slugs: set[str] = set()
    all_paths: set[str] = set()
    for index, entry in enumerate(document):
        required = {"pr_worktree", "paths", "slug"}
        if not resume:
            required.add("approval_basis")
        allowed = required | {"approval_basis", "origins"}
        if not isinstance(entry, dict) or not required.issubset(entry) or set(entry) - allowed:
            expected_fields = "pr_worktree, paths, slug, and optional approval_basis/origins" if resume else (
                "pr_worktree, paths, slug, approval_basis, and optional origins"
            )
            raise AdmitError(
                "BATCH_ENTRY_INVALID",
                f"entry {index + 1} must have {expected_fields}",
            )
        if not isinstance(entry["pr_worktree"], str) or not entry["pr_worktree"]:
            raise AdmitError("BATCH_ENTRY_INVALID", f"entry {index + 1} pr_worktree must be a non-empty path")
        if not isinstance(entry["paths"], list) or not entry["paths"] or not all(
            isinstance(path, str) for path in entry["paths"]
        ):
            raise AdmitError("BATCH_ENTRY_INVALID", f"entry {index + 1} paths must be a non-empty string list")
        slug = entry["slug"]
        if not isinstance(slug, str) or not re.fullmatch(r"[a-z0-9][a-z0-9-]*", slug):
            raise AdmitError("ADMIT_NAME_INVALID", f"entry {index + 1} slug must be lowercase words joined by '-'")
        if slug in slugs:
            raise AdmitError("BATCH_SLUG_DUPLICATE", f"batch entries must use distinct slugs: {slug}")
        slugs.add(slug)
        approval_basis = entry.get("approval_basis")
        if approval_basis is not None and (not isinstance(approval_basis, str) or not approval_basis.strip()):
            raise AdmitError("BATCH_ENTRY_INVALID", f"entry {index + 1} approval_basis must be a non-empty string")
        if not resume and approval_basis is None:
            raise AdmitError("ADMISSION_BASIS_REQUIRED", f"entry {index + 1} approval_basis must state the human decision")
        origins = entry.get("origins", {})
        if not isinstance(origins, dict):
            raise AdmitError("BATCH_ENTRY_INVALID", f"entry {index + 1} origins must map paths to origin fields")
        try:
            exact = [provenance_ledger._exact_path(path, code="ADMIT_PATH_NOT_EXACT") for path in entry["paths"]]
        except RefreshError as error:
            raise AdmitError(error.code, str(error)) from error
        if len(exact) != len(set(exact)):
            raise AdmitError("ADMIT_PATHS_INVALID", f"entry {index + 1} must name each new path exactly once")
        overlap = all_paths.intersection(exact)
        if overlap:
            raise AdmitError("BATCH_PATH_OVERLAP", f"batch entries name the same path: {', '.join(sorted(overlap))}")
        all_paths.update(exact)
        normalized.append({
            "pr_worktree": Path(entry["pr_worktree"]),
            "paths": exact,
            "slug": slug,
            "approval_basis": approval_basis,
            "origins": origins,
        })
    return normalized


def admit_batch(
    *, batch: list[dict], private_repo: Path, base: str | None = None, date: str | None = None,
    assisted_by: str | None = None, push: bool = False, scratch_root: Path | None = None,
    any_order_limit: int = 6, resume: bool = False,
) -> dict:
    """Admit an uncapped batch or re-issue its remaining listed-order chain."""
    if not isinstance(any_order_limit, int) or any_order_limit < 0:
        raise AdmitError("ADMIT_ARGUMENT_INVALID", "--any-order-limit must be a non-negative integer")
    entries = _batch_entries(batch, resume=resume)
    authority_root, authority_ledger, ledger_records = _authority_inputs(private_repo)
    resolved_date = date or datetime.date.today().isoformat()
    first_pr = Path(_text(entries[0]["pr_worktree"], "rev-parse", "--show-toplevel"))
    shared_base = base or _text(first_pr, "merge-base", "HEAD", "origin/main")
    shared_base = _text(first_pr, "rev-parse", "--verify", f"{shared_base}^{{commit}}")

    plans: list[dict] = []
    private_committed = False
    try:
        for entry in entries:
            plans.append(_prepare_admission(
                pr_worktree=entry["pr_worktree"], paths=entry["paths"],
                approval_basis=None if resume else entry["approval_basis"],
                authority_root=authority_root, authority_ledger=authority_ledger, records=ledger_records,
                base=shared_base, slug=entry["slug"], date=resolved_date, origins=entry["origins"],
                assisted_by=assisted_by, rekey=resume, scratch_root=scratch_root,
            ))

        base_policy = _git(first_pr, "cat-file", "blob", f"{shared_base}:{POLICY}")
        base_path_sets = [set(plan["paths"]) for plan in plans]
        all_record_bytes: dict[str, bytes] = {}
        for plan in plans:
            for path, raw in plan["record_bytes"].items():
                # The subset loop below writes every delta with the shared canonical serializer.
                if path.startswith(f"{POLICY_DELTAS}/"):
                    continue
                if path in all_record_bytes and all_record_bytes[path] != raw:
                    raise AdmitError("BATCH_RECORD_CONFLICT", f"batch records disagree at {path}")
                all_record_bytes[path] = raw

        deltas: list[dict] = []

        def issue_delta(selected: list[int], candidate_index: int) -> None:
            subset_paths = set().union(*(base_path_sets[i] for i in selected)) if selected else set()
            baseline_policy = base_policy if not selected else _candidate_policy(base_policy, subset_paths)[0]
            candidate_paths = sorted(base_path_sets[candidate_index])
            candidate_policy, _ = _candidate_policy(base_policy, subset_paths | base_path_sets[candidate_index])
            candidate_sha, authority_bytes = _policy_delta_records(baseline_policy, candidate_policy, candidate_paths)
            baseline_sha = hashlib.sha256(baseline_policy).hexdigest()
            delta_dir = f"{POLICY_DELTAS}/{candidate_sha}/{baseline_sha}"
            for path, raw in (
                (f"{delta_dir}/candidate-policy.json", candidate_policy),
                (f"{delta_dir}/policy-delta-authority.json", authority_bytes),
            ):
                if path in all_record_bytes and all_record_bytes[path] != raw:
                    raise AdmitError("BATCH_RECORD_CONFLICT", f"batch records disagree at {path}")
                all_record_bytes[path] = raw
            deltas.append({
                "entry_slug": plans[candidate_index]["slug"],
                "subset_slugs": [plans[i]["slug"] for i in selected],
                "paths": candidate_paths,
                "baseline_policy_sha256": baseline_sha,
                "candidate_policy_sha256": candidate_sha,
                "records": [f"{delta_dir}/candidate-policy.json", f"{delta_dir}/policy-delta-authority.json"],
            })

        if resume or len(plans) > any_order_limit:
            for candidate_index in range(len(plans)):
                issue_delta(list(range(candidate_index)), candidate_index)
        else:
            for subset_mask in range(1 << len(plans)):
                selected = [i for i in range(len(plans)) if subset_mask & (1 << i)]
                for candidate_index in range(len(plans)):
                    if candidate_index not in selected:
                        issue_delta(selected, candidate_index)

        delta_count = len(deltas)
        message = (
            f"provenance: batch admit {len(plans)} pull requests ({resolved_date})\n\n"
            f"Pre-issue {delta_count} exact policy deltas for every subset and next landing."
        )

        _record_paths, private_commit, pushed = _write_authority_commit(
            authority_root=authority_root,
            records=all_record_bytes,
            message=message,
            assisted_by=assisted_by, push=False,
        )
        private_committed = True
        if push:
            _git(authority_root, "push", "-q", "origin", "HEAD:main")
            pushed = True
    except BaseException:
        if not private_committed:
            _remove_created_branches(plans)
        raise

    for plan in plans:
        plan.update(private_commit=private_commit, pushed=pushed)
        plan.pop("record_bytes", None)
    return {
        "base": shared_base,
        "entries": plans,
        "private_commit": private_commit,
        "pushed": pushed,
        "records": sorted(all_record_bytes),
        "delta_count": delta_count,
        "deltas": deltas,
    }


def merge_profile(worktree: Path) -> dict:
    """Write the three-way merge of both sides' include and exclude path lists.

    Each list keeps the merge base's paths, adds what either side added and drops what
    either side removed, so a path one side moved between the lists or dropped is not
    brought back by the other side's copy of the list."""
    repo = Path(_text(worktree, "rev-parse", "--show-toplevel"))
    try:
        head = _text(repo, "rev-parse", "--verify", "HEAD")
        merge_head = _text(repo, "rev-parse", "--verify", "MERGE_HEAD")
    except AdmitError as error:
        raise AdmitError("MERGE_PROFILE_NOT_IN_PROGRESS", "--merge-profile requires an active git merge") from error
    try:
        merge_base = _text(repo, "merge-base", head, merge_head)
    except AdmitError as error:
        raise AdmitError("MERGE_PROFILE_INPUT_INVALID", "the merge sides have no merge base") from error
    side_documents = []
    for revision in (merge_base, head, merge_head):
        try:
            document = json.loads(_git(repo, "show", f"{revision}:{POLICY}").decode("utf-8"))
        except (AdmitError, UnicodeDecodeError, json.JSONDecodeError) as error:
            raise AdmitError("MERGE_PROFILE_INPUT_INVALID", f"cannot read {POLICY} from merge side {revision[:12]}") from error
        if not isinstance(document, dict):
            raise AdmitError("MERGE_PROFILE_INPUT_INVALID", f"{POLICY} must be a JSON object on both merge sides")
        for field in ("include_paths", "exclude_paths"):
            if not isinstance(document.get(field), list) or not all(
                isinstance(path, str) for path in document[field]
            ):
                raise AdmitError("MERGE_PROFILE_INPUT_INVALID", f"{POLICY} {field} must be a list of strings")
        side_documents.append(document)
    base, ours, theirs = side_documents
    path_fields = {"include_paths", "exclude_paths"}
    ours_other = {key: value for key, value in ours.items() if key not in path_fields}
    theirs_other = {key: value for key, value in theirs.items() if key not in path_fields}
    if ours_other != theirs_other:
        raise AdmitError(
            "MERGE_PROFILE_NON_PATH_FIELD_CHANGED",
            "merge sides changed a profile field other than include_paths or exclude_paths",
        )
    current = repo
    for part in PurePosixPath(POLICY).parts:
        current /= part
        if current.is_symlink():
            raise AdmitError("MERGE_PROFILE_UNSAFE_PATH", f"refusing symlinked profile path component: {current}")
    merged = dict(ours)
    for field in path_fields:
        before, mine, other = set(base[field]), set(ours[field]), set(theirs[field])
        merged[field] = sorted((before | mine | other) - (before - mine) - (before - other))
    both = sorted(set(merged["include_paths"]) & set(merged["exclude_paths"]))
    if both:
        raise AdmitError(
            "MERGE_PROFILE_PATH_CONFLICT",
            f"the merge would include and exclude the same path: {', '.join(both)}",
        )
    raw = _canonical_profile_bytes(merged)
    profile_path = repo.joinpath(*PurePosixPath(POLICY).parts)
    profile_path.write_bytes(raw)
    return {
        "worktree": repo,
        "profile": profile_path,
        "profile_sha256": hashlib.sha256(raw).hexdigest(),
        "include_count": len(merged["include_paths"]),
        "exclude_count": len(merged["exclude_paths"]),
    }


def _read_batch_file(path: Path) -> object:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AdmitError("BATCH_FILE_INVALID", f"cannot read --batch JSON file {path}: {error}") from error
    return document


def remaining_steps(result: dict) -> list[str]:
    """The exact commands left after ``admit``, in order."""
    pr = result["pr_worktree"]
    steps = []
    if not result["pushed"]:
        steps.append(f'git -C "{result["authority_root"]}" push origin main')
    steps += [
        f'git -C "{pr}" merge --no-edit {result["admit_branch"]}',
        f'cd "{pr}" && python tools/provenance_refresh.py --base {result["admission_commit"]} '
        f'--trusted-ledger "{result["authority_root"] / TRUSTED_LEDGER}"',
        f'git -C "{pr}" commit -m "provenance: refresh controls after admitting {", ".join(result["paths"])}"',
        f'git -C "{pr}" push',
    ]
    return steps


def _origins(args: argparse.Namespace) -> dict[str, dict]:
    origins: dict[str, dict] = {}
    if args.origin_json is not None:
        document = json.loads(args.origin_json.read_text(encoding="utf-8"))
        if not isinstance(document, dict):
            raise AdmitError("ADMIT_ARGUMENT_INVALID", "--origin-json must map each path to its origin fields")
        origins.update(document)
    shared = {"origin_kind": args.origin_kind, "origin": args.origin, "license": args.license}
    if any(shared.values()):
        for path in args.paths:
            origins.setdefault(path, shared)
    return origins


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pr_worktree", type=Path, nargs="?", help="worktree of the pull request that adds the paths")
    parser.add_argument("paths", nargs="*", help="exact new repository paths, committed at the pull request HEAD")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--batch", type=Path, help="JSON list of two or more independent pull-request admissions")
    modes.add_argument("--batch-resume", type=Path,
                       help="JSON list of remaining entries; re-issue their chain from recorded admissions")
    modes.add_argument("--merge-profile", type=Path, metavar="WORKTREE",
                       help="write the three-way merge of the profile paths during an active merge")
    parser.add_argument("--any-order-limit", type=int, default=6,
                        help="pre-issue every landing order up to this batch size (default: 6)")
    parser.add_argument("--approval-basis", help="the human decision, recorded verbatim (not with --rekey)")
    parser.add_argument(
        "--rekey", action="store_true",
        help="re-issue the delta and admission commit for a new base from the admission already recorded",
    )
    parser.add_argument(
        "--private-repo", type=Path, default=os.environ.get("NK_PRIVATE_AUTHORITY"),
        help="private authority checkout on main (default: $NK_PRIVATE_AUTHORITY)",
    )
    parser.add_argument("--base", help="exact base commit (default: merge-base of HEAD and origin/main)")
    parser.add_argument("--slug", help="record name (default: the pull request branch name without its date)")
    parser.add_argument("--date", help="record date, YYYY-MM-DD (default: today)")
    parser.add_argument("--origin-kind", help="implementation paths: " + ", ".join(
        sorted(provenance_ledger.ADMISSION_ORIGIN_KINDS)))
    parser.add_argument("--origin", help="implementation paths: reviewer's origin statement")
    parser.add_argument("--license", help="implementation paths: licence basis")
    parser.add_argument("--origin-json", type=Path, help="per-path {origin_kind, origin, license} overrides")
    parser.add_argument("--assisted-by", help="append an Assisted-by: trailer to the commits this makes")
    parser.add_argument("--push", action="store_true", help="also push the private records to the authority main")
    args = parser.parse_args(argv)
    try:
        if args.merge_profile is not None:
            if args.pr_worktree is not None or args.paths:
                raise AdmitError("ADMIT_ARGUMENT_INVALID", "--merge-profile cannot be combined with admission arguments")
            result = merge_profile(args.merge_profile)
            print(f"admit paths: wrote canonical merge profile {result['profile']}")
            print(f"  profile sha256 {result['profile_sha256']}")
            print("  git add assets/public_source_profile.json")
            return 0

        if args.batch is not None or args.batch_resume is not None:
            if args.pr_worktree is not None or args.paths or args.approval_basis or args.rekey or args.slug \
                    or args.origin_kind or args.origin or args.license or args.origin_json is not None:
                raise AdmitError("ADMIT_ARGUMENT_INVALID", "batch modes cannot be combined with single-admission arguments")
            if args.private_repo is None:
                raise AdmitError("AUTHORITY_REQUIRED", "pass --private-repo or set NK_PRIVATE_AUTHORITY")
            resuming = args.batch_resume is not None
            batch_path = args.batch_resume if resuming else args.batch
            assert batch_path is not None
            result = admit_batch(
                batch=_read_batch_file(batch_path), private_repo=args.private_repo, base=args.base,
                date=args.date, assisted_by=args.assisted_by, push=args.push,
                any_order_limit=args.any_order_limit, resume=resuming,
            )
            action = "re-issued the remaining chain for" if resuming else "admitted"
            detail = "remaining listed-order chain" if resuming else "subset/next-landing policy deltas"
            print(f"admit paths: {action} {len(result['entries'])} pull requests on shared base {result['base']}")
            print(f"  pre-issued {result['delta_count']} {detail}")
            print(f"  private records ({'pushed' if result['pushed'] else 'committed locally'} at "
                  f"{result['private_commit'][:12]}):")
            for record in result["records"]:
                print(f"    {record}")
            print("\nLeft to do, in order:")
            number = 1
            if not result["pushed"]:
                authority_root = result["entries"][0]["authority_root"]
                print(f'  {number}. git -C "{authority_root}" push origin main')
                number += 1
            for entry in result["entries"]:
                print(f'  {number}. git -C "{entry["pr_worktree"]}" merge --no-edit {entry["admit_branch"]}')
                number += 1
                print(f'  {number}. cd "{entry["pr_worktree"]}" && python tools/provenance_refresh.py '
                      f'--base {entry["admission_commit"]} --trusted-ledger '
                      f'"{entry["authority_root"] / TRUSTED_LEDGER}"')
                number += 1
                print(f'  {number}. git -C "{entry["pr_worktree"]}" commit -m '
                      f'"provenance: refresh controls after admitting {", ".join(entry["paths"])}"')
                number += 1
                print(f'  {number}. git -C "{entry["pr_worktree"]}" push')
                number += 1
            print("During each merge to main, run python tools/admit_paths.py --merge-profile <worktree>, then git add "
                  "assets/public_source_profile.json.")
            return 0

        if args.pr_worktree is None or not args.paths:
            raise AdmitError("ADMIT_ARGUMENT_INVALID", "pass <pr-worktree> and at least one path, or use --batch/--merge-profile")
        if args.private_repo is None:
            raise AdmitError("AUTHORITY_REQUIRED", "pass --private-repo or set NK_PRIVATE_AUTHORITY")
        result = admit(
            pr_worktree=args.pr_worktree, paths=args.paths, approval_basis=args.approval_basis,
            private_repo=args.private_repo, base=args.base, slug=args.slug, date=args.date,
            origins=_origins(args), assisted_by=args.assisted_by, push=args.push, rekey=args.rekey,
        )
    except (AdmitError, RefreshError) as error:
        print(f"admit paths: {error.code}: {error}", file=sys.stderr)
        return 1
    print(f"admit paths: admitted {len(result['paths'])} path(s) on base {result['base']}")
    for statement in result["statements"]:
        print(f"  {statement['path']}: {statement['classification']}")
    print(f"  candidate profile sha256 {result['candidate_policy_sha256']}")
    print(f"  private records ({'pushed' if result['pushed'] else 'committed locally'} at "
          f"{result['private_commit'][:12]}):")
    for record in result["records"]:
        print(f"    {record}")
    print(f"  admission commit {result['admission_commit']} on local branch {result['admit_branch']}")
    print("\nLeft to do, in order:")
    for number, step in enumerate(remaining_steps(result), 1):
        print(f"  {number}. {step}")
    print("If the merge conflicts only in the generated ledger, export or profile, take the admission branch's copy "
          f"(git checkout {result['admit_branch']} -- <file>); the refresh regenerates them.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
