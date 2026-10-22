#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Admit new public paths with one command.

Adding a public path used to be a manual ritual in five steps, and each skipped
or misordered step failed with its own code (``CANDIDATE_TREE_STALE``,
``ADMISSION_CLASS_MISMATCH``, ``REFRESH_NEW_PATH_REQUIRES_ADMISSION``,
``TRUSTED_BASE_NOT_ANCESTOR``)::

    python tools/admit_paths.py <pr-worktree> <path> [<path> ...] \\
        --approval-basis "Owner request of <date>: ..." --private-repo <authority checkout>

The command does the mechanical parts and nothing else:

1. reads each path's exact committed bytes from the pull request's ``HEAD``;
2. derives each path's classification with the gate's own classifier
   (``provenance_ledger``); an implementation path needs an exact record that
   already exists in the private ``IMPLEMENTATION_PROVENANCE.json`` -- this
   command never writes one;
3. writes the private records under ``docs/provenance/`` in the authority
   checkout: the admission document and the policy delta
   (``candidate-policy.json`` plus ``policy-delta-authority.json``) keyed by the
   SHA-256 of the new profile, then commits only those files locally;
4. builds a clean candidate commit holding only the base, the new paths and
   their profile entries, runs ``admit-new-reviewed`` on it, and commits the
   generated controls on a local ``admit/<slug>-<date>`` branch;
5. prints exactly what is left: the private push, the merge and the refresh.

``--rekey`` is for a pull request whose paths were already admitted but whose
base moved because another path-adding pull request landed first: the delta is
keyed to the whole profile, so it must be re-issued for the new base.  It
reuses the admission already recorded for exactly these paths and bytes and
records no new decision.

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
    """The one existing admission that already decided exactly these paths, bytes and classes.

    Re-keying reuses that decision for a new base; it never makes a new one, so
    a path set, digest or class that no admission names is refused.
    """
    def key(statement: dict) -> tuple:
        return (statement["path"], statement["sha256"], statement["classification"], statement.get("record_id"))

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
    with tempfile.TemporaryDirectory(prefix="nakagawa-admit-policy-") as scratch:
        baseline_path = Path(scratch) / "baseline-policy.json"
        baseline_path.write_bytes(base_policy)
        candidate_policy, candidate_document = provenance_ledger._extended_policy_bytes(baseline_path, set(paths))
    candidate_sha = hashlib.sha256(candidate_policy).hexdigest()
    delta = provenance_ledger._classify_policy_delta(json.loads(base_policy.decode("utf-8")), candidate_document)
    if delta["include_added"] != sorted(paths) or any(
        delta[key] for key in ("include_removed", "exclude_added", "exclude_removed", "rule_changes")
    ):
        raise AdmitError("POLICY_DELTA_UNEXPECTED", f"the derived policy delta is not exactly the new paths: {delta}")
    authority = _canonical({
        "allowed_delta": delta,
        "baseline_policy_sha256": hashlib.sha256(base_policy).hexdigest(),
        "candidate_policy_sha256": candidate_sha,
        "kind": provenance_ledger.POLICY_DELTA_KIND,
        "schema_version": provenance_ledger.POLICY_DELTA_SCHEMA_VERSION,
    })
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
    """Admit ``paths`` (see the module docstring).

    ``rekey`` re-issues the policy delta and the admission commit for a new
    base -- typically after another pull request that adds paths has landed and
    ``main`` was merged -- from the admission already recorded for exactly these
    paths and bytes.  It takes no approval basis because it records no new
    decision.
    """
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
        # provenance_refresh.py would refuse later with TRUSTED_BASE_NOT_ANCESTOR.
        raise AdmitError(
            "TRUSTED_BASE_NOT_ANCESTOR", f"base {base[:12]} is not an ancestor of the pull request HEAD; merge it first",
        ) from None
    branch = _text(pr, "rev-parse", "--abbrev-ref", "HEAD")
    slug = slug or default_slug("" if branch == "HEAD" else branch, paths)
    date = date or datetime.date.today().isoformat()
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", date) or not re.fullmatch(r"[a-z0-9][a-z0-9-]*", slug):
        raise AdmitError("ADMIT_NAME_INVALID", "--slug must be lowercase words joined by '-', --date YYYY-MM-DD")

    # -- the new bytes are the committed ones -------------------------------
    blobs: dict[str, tuple[str, str]] = {}
    digests: dict[str, str] = {}
    dirty = _text(pr, "status", "--porcelain=v1", "--", *paths)
    if dirty:
        raise AdmitError("ADMIT_PATH_UNCOMMITTED", f"commit the new paths before admitting them:\n{dirty}")
    base_policy = _git(pr, "cat-file", "blob", f"{base}:{POLICY}")
    with tempfile.TemporaryDirectory(prefix="nakagawa-admit-base-") as scratch:
        (Path(scratch) / "policy.json").write_bytes(base_policy)
        policy = load_policy(Path(scratch) / "policy.json")
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

    # -- the private authority checkout -------------------------------------
    authority_root = Path(_text(private_repo, "rev-parse", "--show-toplevel"))
    if _text(authority_root, "rev-parse", "--abbrev-ref", "HEAD") != "main":
        raise AdmitError("AUTHORITY_NOT_MAIN", "the authority checkout must have main checked out")
    if _text(authority_root, "status", "--porcelain=v1", "--untracked-files=all", "--", "docs/provenance"):
        raise AdmitError("AUTHORITY_DIRTY", "docs/provenance in the authority checkout has uncommitted changes")
    trusted_ledger = _git(authority_root, "cat-file", "blob", f"HEAD:{TRUSTED_LEDGER}")
    records = provenance_ledger._detailed_records(json.loads(trusted_ledger.decode("utf-8")))
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
        # The recorded bytes stay authoritative even if this tool's formatting changes.
        admission_bytes = admission_path.read_bytes()
    delta_dir = authority_root / POLICY_DELTAS / candidate_sha
    record_paths = [
        admission_path, delta_dir / "candidate-policy.json", delta_dir / "policy-delta-authority.json",
    ]
    if _git(pr, "branch", "--list", admit_branch):
        raise AdmitError("ADMIT_BRANCH_EXISTS", f"local branch {admit_branch} already exists")

    created: list[Path] = []
    scratch = Path(tempfile.mkdtemp(prefix="nakagawa-admit-", dir=scratch_root))
    try:
        for target, raw in zip(record_paths, (admission_bytes, candidate_policy, authority_bytes), strict=True):
            _write_record(target, raw, created)

        # -- clean candidate: base + new paths + their profile entries -------
        candidate = scratch / "candidate"
        _git(scratch, "clone", "-q", "--shared", "--no-checkout", str(pr), str(candidate))
        _git(candidate, "checkout", "-q", "--detach", base)
        for path, (mode, sha) in blobs.items():
            _git(candidate, "update-index", "--add", "--cacheinfo", f"{mode},{sha},{path}")
        _stage_bytes(candidate, POLICY, candidate_policy)
        _stage_bytes(candidate, EXPORT, _control_export(candidate, candidate_policy))
        _git(candidate, "checkout-index", "-f", "--", *paths, POLICY, EXPORT)
        candidate_commit = _commit(candidate, f"candidate: admit {slug}", assisted_by)

        trusted_inputs = scratch / "trusted"
        trusted_inputs.mkdir()
        (trusted_inputs / "IMPLEMENTATION_PROVENANCE.json").write_bytes(trusted_ledger)
        (trusted_inputs / "trusted-policy.json").write_bytes(base_policy)
        try:
            provenance_ledger.admit_new_reviewed(
                trusted_ledger=trusted_inputs / "IMPLEMENTATION_PROVENANCE.json",
                admission_authority=admission_path,
                candidate_tree=str(candidate),
                trusted_tree=base,
                paths=paths,
                trusted_policy=trusted_inputs / "trusted-policy.json",
            )
        except RefreshError as error:
            raise AdmitError(error.code, str(error)) from error
        _git(candidate, "add", "-A")
        admission_commit = _commit(candidate, f"provenance: admit {slug}", assisted_by)
        ledger = _git(candidate, "cat-file", "blob", f"{admission_commit}:assets/public_provenance_ledger.json")
        candidate_tree = _text(candidate, "rev-parse", f"{candidate_commit}^{{tree}}")
        _git(pr, "fetch", "-q", str(candidate), f"{admission_commit}:refs/heads/{admit_branch}")
    except BaseException:
        for target in created:
            target.unlink(missing_ok=True)
        raise
    finally:
        _remove_scratch(scratch)

    # -- private records: commit only our own files ---------------------------
    relative = [target.relative_to(authority_root).as_posix() for target in record_paths]
    private_commit = _text(authority_root, "rev-parse", "HEAD")
    if created:
        _git(authority_root, "add", "--", *relative)
        private_commit = _commit(
            authority_root,
            f"provenance: {'re-key' if rekey else 'admit'} {slug} ({date})\n\n"
            f"Policy delta for candidate profile {candidate_sha[:8]} "
            f"(baseline {hashlib.sha256(base_policy).hexdigest()[:8]}); "
            f"admission binds {len(paths)} new path(s) to their exact SHA-256.",
            assisted_by, *relative,
        )
    pushed = False
    if push:
        _git(authority_root, "push", "-q", "origin", "HEAD:main")
        pushed = True
    return {
        "paths": paths,
        "statements": statements,
        "base": base,
        "candidate_commit": candidate_commit,
        "candidate_tree": candidate_tree,
        "admission_commit": admission_commit,
        "admit_branch": admit_branch,
        "candidate_policy_sha256": candidate_sha,
        "records": relative,
        "private_commit": private_commit,
        "pushed": pushed,
        "ledger": ledger,
        "pr_worktree": pr,
        "authority_root": authority_root,
    }


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
    parser.add_argument("pr_worktree", type=Path, help="worktree of the pull request that adds the paths")
    parser.add_argument("paths", nargs="+", help="exact new repository paths, committed at the pull request HEAD")
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
