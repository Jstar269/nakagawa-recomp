#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Verify a candidate tree's public provenance against external authority.

``provenance_ledger.py --check`` validates the checked-in public ledger
*structurally* and says so explicitly: it "cannot authenticate attestation
claims by itself".  ``publish_audit.py --provenance-ledger`` authenticates by
byte-matching a release-controlled public snapshot, which only exists for a
tree the release process already blessed.  Neither one can run in ordinary pull
request CI, so until this tool existed the merge path verified only that the
candidate agreed with itself.

This tool closes that gap.  It is the *verifier*, never a candidate-controlled
generator: its normal mode verifies a committed ledger, while ``--ephemeral``
generates fresh ledger/export bytes only in a caller-supplied temporary
directory.  It writes no repository artifact and treats every byte of the
candidate tree as untrusted input.

Trust boundary
--------------

Trusted (never candidate-controlled):

* this file, executed from the base/trusted ref, not from the candidate;
* the detailed implementation ledger, supplied as an external file that must
  live outside the candidate repository;
* the publication policy and previous public baseline, read from the trusted
  base commit or an externally bound snapshot rather than from the candidate's
  working tree;
* when a candidate changes policy, an externally supplied blessed copy of the
  candidate policy and a separately bound policy-delta authority;
* the canonical generator imported from this trusted checkout.

Untrusted (data only, never executed, never a trust anchor):

* every blob in the candidate tree, including its ``.github/`` workflows, its
  copy of this file, its policy, and any legacy public ledger/export.

The candidate tree is read through ``git cat-file`` and is never checked out,
so no candidate hook, build file, or module can run in the verifier's process.

Rule tiers
----------

Tier A -- absolute, whole tree, never grandfathered:

``LEDGER_SCHEMA``
    the candidate ledger is structurally valid, has no duplicates, and declares
    no ``unresolved`` path.
``LEDGER_COVERAGE``
    the ledger's paths are exactly the policy-included paths of the candidate
    tree.  A new public file cannot arrive without an entry, and an entry
    cannot survive its file.
``CONTENT_MISMATCH``
    every entry's ``sha256`` equals the SHA-256 of the candidate blob.  This is
    what binds the verdict to the exact bytes under review.
``TRUSTED_RECORD_UNRESOLVED``
    a ``record_id`` the candidate ledger names does not resolve to trusted
    authority covering that exact path.  Absent record and non-covering record
    deliberately share one code and one wording, and the claimed id is not
    echoed, so the gate is not an oracle for which private ids exist.
``TRUSTED_SCOPE_VIOLATION``
    the candidate narrowed the protected universe, or left a new
    implementation-bearing path outside it.  Scope is anchored to the trusted
    base policy over the trusted base tree and may only ever widen; letting the
    candidate policy decide was a complete bypass of every rule below.
``CLASSIFICATION_DOWNGRADE``
    a path authority still calls implementation was relabelled into a class
    that is not content-gated.
``EXPORT_FIELD_MISMATCH``
    the public export disagrees with a full canonical recomputation from the
    candidate tree.  Checking one digest let every other field be forged.
``MERGE_BASE_STALE``
    the base is not an ancestor of the candidate, so the tree that would result
    from merging is not the tree this run attested.
``POLICY_SUBSTITUTION``
    the candidate policy is loadable and never removes an exclusion that the
    trusted policy carries without an externally blessed policy and an exact
    policy-delta authority.  Publication scope may tighten in a pull request;
    it may not loosen behind the gate's back without that separate authority.
``TRUSTED_WORKFLOW_WEAKENED`` / ``CI_CONTEXT_COLLISION``
    the candidate does not disarm this gate's own workflow, and no other
    candidate workflow declares the gate's required check name.  Required
    status checks are matched by context name, so a second job answering to the
    same name is a merge-path bypass and is refused.

Tier B -- the grandfathering predicate.

The trusted authority derives exactly one claim for a path.  A candidate claim
that disagrees with it survives **only while both halves of the reviewed state
are frozen**: the claim as the trusted *base ledger* recorded it, and the bytes
as the trusted *base tree* recorded them.

``CLAIM_UNBACKED``
    the claim is new or changed and does not match what authority derives.
``CONTENT_UNATTESTED``
    the claim is inherited unchanged, but the bytes are not the reviewed bytes.
    Freezing the claim alone would let a candidate modify implementation
    content, update the public hash coherently, and inherit an attestation that
    was never made about those bytes.  Content identity is therefore part of
    the tuple, not a side note.

Tier B2 -- path-authorized implementation content.

An exact trusted record authorizes an implementation *path*.  The candidate
still has to bind its public ledger to the actual Git bytes and pass the
ordinary export, policy, scope, and CI checks, but it does not need a second
private approval for every later digest.  This removes the duplicate
per-revision approval that made routine implementation work wait on authority
maintenance while preserving the independent path decision.

New implementation paths still need exact external path authority; wildcard
records remain inert for classification.  The legacy ``reviewed_blobs`` array
is accepted as optional audit history.  ``--require-reviewed-blobs`` restores
the former exact-digest gate for a deliberately higher-assurance run, but is
not part of the normal merge/readiness path.

Implementation classes and added or changed executable/security-sensitive paths
require an exact trusted path record. Documentation, configuration,
non-executable fixtures and public metadata are classified by what a file *is*,
re-derived every run, and need no per-revision approval.

Tier C -- reported, non-fatal:

    paths where both halves are frozen and the claim still disagrees with
    authority.  Each carries a ``backing`` value naming what the authority does
    say about it, because that decides the remedy: ``exact`` or
    ``deterministic`` backing means the public entry can simply be corrected;
    ``blanket`` or ``none`` means a trusted record has to exist first.  Tier B
    guarantees this set can never grow.

Output discipline
-----------------

The trusted detailed ledger is private.  This tool prints only data that is
already public -- repository paths, record ids that the public ledger itself
names, classification names, and finding codes -- plus a SHA-256 of the trusted
ledger bytes for run-to-run comparison.  Record and approval bodies -- every
descriptive, evidentiary, ownership and review field they carry -- are never
printed and never written to the JSON verdict.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import re
import subprocess
import sys
import tempfile
import threading
import unicodedata
from fnmatch import fnmatchcase
from pathlib import Path

try:
    from .nk_core.git_isolation import isolated_git_env
    from .provenance_ledger import (
        ALLOWED_CLASSES as ALLOWED_CLASSES, RefreshError, _admission_requires_implementation,
        _canonical_json_bytes, _class_for, _classify_policy_delta, _read_policy_delta_authority,
        validate_ledger,
    )
    from .public_export import EXPORT_TREE_DERIVED_FIELDS as _EXPORT_TREE_DERIVED_FIELDS
    from .public_export import build_control_document as _build_control_document
    from .public_export import build_document as _build_export_document
    from .publication_policy import PolicyError, load_policy
except ImportError:
    from nk_core.git_isolation import isolated_git_env
    from provenance_ledger import (
        ALLOWED_CLASSES as ALLOWED_CLASSES, RefreshError, _admission_requires_implementation,
        _canonical_json_bytes, _class_for, _classify_policy_delta, _read_policy_delta_authority,
        validate_ledger,
    )
    from public_export import EXPORT_TREE_DERIVED_FIELDS as _EXPORT_TREE_DERIVED_FIELDS
    from public_export import build_control_document as _build_control_document
    from public_export import build_document as _build_export_document
    from publication_policy import PolicyError, load_policy

#: Paths the ledger deliberately carries without a content hash: they are the
#: generated outputs whose own bytes depend on the ledger.
UNHASHED_PATHS = frozenset({"assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json"})

LEDGER_PATH = "assets/public_provenance_ledger.json"
POLICY_PATH = "assets/public_source_profile.json"
EXPORT_PATH = "PUBLIC_EXPORT.json"
MANIFEST_PATH = "assets/release_manifest.json"
CONTROL_PATHS = frozenset({LEDGER_PATH, EXPORT_PATH})

# A baseline supplied in the integration-time workflow is either the exact
# legacy ledger blob from the trusted base (compatibility window), or this
# explicit envelope.  The envelope is what lets the external baseline survive
# after the legacy path is removed from the repository.
BASELINE_KIND = "public-provenance-baseline"
BASELINE_SCHEMA_VERSION = 1

#: The workflow that runs this verifier, and the required status check it
#: reports under.  Both are matched literally against candidate bytes.
TRUSTED_WORKFLOW = ".github/workflows/provenance-attestation.yml"
TRUSTED_CONTEXT = "Trusted provenance attestation"
WORKFLOW_PREFIX = ".github/workflows/"

#: Classes that assert something about implementation provenance.  Anything
#: else is a deterministic statement about what a file *is*.
IMPLEMENTATION_CLASSES = frozenset({
    "project_authored_attested",
    "upstream_derived",
    "generated_from_public_source",
})


def _exact_record_finding_for_change(
    path: str,
    *,
    base_blobs: dict[str, bytes],
    candidate_blobs: dict[str, bytes],
    exact_records: dict[str, dict],
) -> str | None:
    if path in base_blobs and base_blobs[path] == candidate_blobs.get(path):
        return None
    if not _admission_requires_implementation(path):
        return None
    record = exact_records.get(path)
    if record is None:
        return "TRUSTED_PATH_MISSING"
    classification, _ = _class_for(path, record)
    if classification not in IMPLEMENTATION_CLASSES:
        return "TRUSTED_PATH_UNQUALIFIED"
    return None


class VerifyError(RuntimeError):
    """A fail-closed error in the verifier's own inputs."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


class Finding:
    """One fatal or reported observation about the candidate tree."""

    __slots__ = ("code", "path", "detail", "fatal", "maintainer_action")

    def __init__(self, code: str, path: str, detail: str, *, fatal: bool = True,
                 maintainer_action: bool = False) -> None:
        self.code = code
        self.path = path
        self.detail = detail
        self.fatal = fatal
        # True when a maintainer, not the pull-request author, is the only party
        # who can clear this.  It never relaxes the verdict: the finding stays
        # fatal and fails closed.  It only lets the report say who has to act,
        # so "add a file" does not read to a contributor as "your change is
        # wrong".  See MAINTAINER_ACTION_CODES.
        self.maintainer_action = maintainer_action

    def as_dict(self) -> dict:
        return {
            "code": self.code,
            "path": self.path,
            "detail": self.detail,
            "fatal": self.fatal,
            "maintainer_action": self.maintainer_action,
        }


def _git(repo: Path, *args: str) -> bytes:
    result = subprocess.run(
        ["git", *args],
        cwd=repo,
        env=isolated_git_env(root=repo),
        capture_output=True,
        check=False,
    )
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise VerifyError("GIT_ERROR", detail or f"git {' '.join(args)} failed")
    return result.stdout


def _is_ancestor(repo: Path, ancestor: str, descendant: str) -> bool:
    result = subprocess.run(
        ["git", "merge-base", "--is-ancestor", ancestor, descendant],
        cwd=repo,
        env=isolated_git_env(root=repo),
        capture_output=True,
        check=False,
    )
    return result.returncode == 0


def _rev_tree(repo: Path, rev: str) -> str:
    sha = _git(repo, "rev-parse", "--verify", "--quiet", rev + "^{tree}").decode("ascii").strip()
    if len(sha) != 40:
        raise VerifyError("TREE_INVALID", f"selector did not resolve to a Git tree: {rev}")
    return sha


def _rev_commit(repo: Path, rev: str) -> str:
    sha = _git(repo, "rev-parse", "--verify", "--quiet", rev + "^{commit}").decode("ascii").strip()
    if len(sha) != 40:
        raise VerifyError("COMMIT_INVALID", f"selector did not resolve to a commit: {rev}")
    return sha


#: Ceilings on what one candidate tree may make this verifier hold in memory.
#: The gate runs on a hosted runner against a tree an untrusted author
#: controls, so "read every blob" needs a stated bound rather than an implicit
#: one, and the bound has to bite *before* the bytes are materialized.
#:
#: Measured on this repository at the fourth-pass HEAD: 658 paths, longest path
#: 68 bytes, longest component 38 bytes, deepest path 9 components, largest
#: blob 2 MiB, 14.8 MB of content, ~30 KB of path text.  Every ceiling below
#: leaves at least an order of magnitude of headroom over that.
MAX_TREE_PATHS = 50_000
MAX_TREE_BYTES = 512 * 1024 * 1024
MAX_BLOB_BYTES = 64 * 1024 * 1024
MAX_PATH_BYTES = 512
MAX_COMPONENT_BYTES = 255
MAX_PATH_DEPTH = 32
MAX_TREE_PATH_BYTES = 8 * 1024 * 1024
MAX_JSON_BYTES = 32 * 1024 * 1024
MAX_REVIEWED_BLOBS = 100_000


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict:
    """``object_pairs_hook`` that refuses a repeated key.

    ``json.loads`` keeps the last value for a duplicated key.  Two readers that
    disagree about which one wins -- this verifier and whatever consumes the
    artifact downstream -- is a place to smuggle a second value past review, so
    a document that contains one at all is refused before any semantics run.
    """
    seen: set[str] = set()
    for key, _value in pairs:
        if key in seen:
            raise ValueError(f"duplicate JSON object key: {key!r}")
        seen.add(key)
    return dict(pairs)


def strict_json(raw: bytes, *, code: str, label: str) -> dict:
    """Parse a security-relevant JSON document, failing closed on ambiguity."""
    if len(raw) > MAX_JSON_BYTES:
        raise VerifyError(code, f"{label} exceeds the {MAX_JSON_BYTES} byte ceiling")
    try:
        document = json.loads(raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys)
    except UnicodeDecodeError as error:
        raise VerifyError(code, f"{label} is not valid UTF-8") from error
    except ValueError as error:
        raise VerifyError(code, f"{label} is not valid JSON: {error}") from error
    if not isinstance(document, dict):
        raise VerifyError(code, f"{label} must be a JSON object")
    return document


#: Characters a repository path may never contain.  Backslash is excluded
#: because Git stores it as an ordinary byte while Windows reads it as a
#: separator, so the same tree would name two different files.
_FORBIDDEN_PATH_CHARS = frozenset({"\\", "\r", "\n"})


def canonical_path(path: object, *, code: str, label: str) -> str:
    """Return ``path`` if it is the one canonical spelling, else fail closed.

    Git permits almost any byte string as a path.  Provenance needs a single
    unambiguous identity per file, so this project accepts only NFC-normalized
    UTF-8 POSIX relative paths with no control characters and no dot
    components.  Anything else is refused rather than normalized: silently
    folding two distinct Git paths onto one identity would let a second file
    inherit the first one's approval.
    """
    if not isinstance(path, str) or not path:
        raise VerifyError(code, f"{label} does not name a path")
    raw = path.encode("utf-8")
    if len(raw) > MAX_PATH_BYTES:
        raise VerifyError(code, f"{label} path exceeds {MAX_PATH_BYTES} bytes")
    for character in path:
        if ord(character) < 0x20 or ord(character) == 0x7F:
            raise VerifyError(code, f"{label} path contains a control character")
        if character in _FORBIDDEN_PATH_CHARS:
            raise VerifyError(code, f"{label} path contains {character!r}")
    if unicodedata.normalize("NFC", path) != path:
        raise VerifyError(
            code, f"{label} path is not Unicode NFC normalized; two spellings of one name",
        )
    if path.startswith("/") or path.endswith("/"):
        raise VerifyError(code, f"{label} path is not repository-relative")
    components = path.split("/")
    if len(components) > MAX_PATH_DEPTH:
        raise VerifyError(code, f"{label} path is deeper than {MAX_PATH_DEPTH} components")
    for component in components:
        if component in ("", ".", ".."):
            raise VerifyError(code, f"{label} path has an empty or dot component")
        if len(component.encode("utf-8")) > MAX_COMPONENT_BYTES:
            raise VerifyError(code, f"{label} path component exceeds {MAX_COMPONENT_BYTES} bytes")
    return path


def _stream_tree_entries(repo: Path, tree_sha: str):
    """Yield (path, object_id) from ``git ls-tree``, bounded as it streams.

    ``subprocess.run`` would buffer the whole listing before a single check
    could run, so an attacker-shaped tree would be materialized first and
    rejected second. Reading incrementally lets every ceiling bite on the way
    in, and lets the reader be killed the moment one does.
    """
    proc = subprocess.Popen(
        ["git", "ls-tree", "-r", "-z", "--full-tree", tree_sha], cwd=repo,
        env=isolated_git_env(root=repo),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert proc.stdout
    pending = b""
    count = 0
    path_bytes = 0
    try:
        while True:
            chunk = proc.stdout.read(65536)
            if not chunk:
                break
            pending += chunk
            while True:
                terminator = pending.find(b"\0")
                if terminator < 0:
                    break
                item, pending = pending[:terminator], pending[terminator + 1:]
                if not item:
                    continue
                count += 1
                path_bytes += len(item)
                if count > MAX_TREE_PATHS:
                    raise VerifyError(
                        "TREE_TOO_LARGE",
                        f"tree has more than {MAX_TREE_PATHS} paths",
                    )
                if path_bytes > MAX_TREE_PATH_BYTES:
                    raise VerifyError(
                        "TREE_TOO_LARGE",
                        f"tree path text exceeds {MAX_TREE_PATH_BYTES} bytes",
                    )
                yield _parse_tree_entry(item)
            # A listing with no NUL at all is not a listing; do not grow forever.
            if len(pending) > MAX_PATH_BYTES * 4:
                raise VerifyError("TREE_INVALID", "Git tree listing is malformed")
        if pending.strip(b"\0"):
            raise VerifyError("TREE_INVALID", "Git tree listing ended mid-entry")
    finally:
        failed = proc.poll() not in (0, None) or proc.poll() is None
        if proc.poll() is None:
            proc.kill()
        stderr = proc.stderr.read() if proc.stderr else b""
        for stream in (proc.stdout, proc.stderr):
            if stream is not None:
                stream.close()
        proc.wait()
        if failed and proc.returncode not in (0, -9, -15) and sys.exc_info()[1] is None:
            raise VerifyError(
                "GIT_ERROR", stderr.decode("utf-8", errors="replace").strip() or "ls-tree failed")


def _parse_tree_entry(item: bytes) -> tuple[str, str]:
    try:
        metadata, raw_path = item.split(b"\t", 1)
        mode, kind, object_id = metadata.split()
    except ValueError as error:
        raise VerifyError("TREE_INVALID", "Git tree contains a malformed entry") from error
    if kind != b"blob":
        # A gitlink is kind "commit"; a subtree is "tree". Neither has content
        # this gate can attest.
        raise VerifyError("TREE_INVALID", "verification supports regular Git blobs only")
    # A symlink is stored as a blob whose content is its target, so hashing one
    # would attest a pointer rather than the bytes it resolves to.
    if mode not in (b"100644", b"100755"):
        raise VerifyError(
            "TREE_INVALID",
            "tree contains a non-regular file (symlink or special mode); provenance is "
            "only defined over regular file content",
        )
    try:
        decoded = raw_path.decode("utf-8")
    except UnicodeDecodeError as error:
        raise VerifyError("TREE_INVALID", "Git tree contains a non-UTF-8 path") from error
    return canonical_path(decoded, code="TREE_INVALID", label="tree entry"), object_id.decode("ascii")


def _read_exact(stream, size: int) -> bytes:
    parts = []
    remaining = size
    while remaining:
        chunk = stream.read(min(remaining, 1024 * 1024))
        if not chunk:
            raise VerifyError("TREE_INVALID", "Git returned a truncated blob")
        parts.append(chunk)
        remaining -= len(chunk)
    return b"".join(parts)


def read_tree(repo: Path, tree_sha: str) -> dict[str, bytes]:
    """Read every blob of an immutable tree through Git's object database.

    ``git cat-file`` returns raw object bytes, so no ``.gitattributes`` filter,
    smudge driver, or other checkout-time hook runs. The tree is data, and it
    is read under explicit ceilings that are enforced as the bytes arrive
    rather than after the whole response is in memory.
    """
    requests: list[tuple[str, str]] = []
    seen: set[str] = set()
    for path, object_id in _stream_tree_entries(repo, tree_sha):
        if path in seen:
            raise VerifyError("TREE_INVALID", "Git tree contains a duplicate path")
        seen.add(path)
        requests.append((path, object_id))
    if not requests:
        return {}

    proc = subprocess.Popen(
        ["git", "cat-file", "--batch"], cwd=repo,
        env=isolated_git_env(root=repo),
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert proc.stdin and proc.stdout
    blobs: dict[str, bytes] = {}
    total = 0

    def _feed() -> None:
        # Must run concurrently with the reader below: git blocks once its
        # stdout pipe fills, and then stops draining stdin, so writing the
        # whole request list before reading anything deadlocks both ends.
        try:
            proc.stdin.write("".join(f"{oid}\n" for _, oid in requests).encode("ascii"))
            proc.stdin.close()
        except OSError:
            pass  # the reader hit a ceiling and killed the process

    writer = threading.Thread(target=_feed, daemon=True)
    writer.start()
    try:
        for path, _ in requests:
            header = proc.stdout.readline()
            if not header:
                raise VerifyError("TREE_INVALID", "Git returned a truncated blob response")
            fields = header.split()
            if len(fields) < 3 or fields[1] != b"blob":
                raise VerifyError("TREE_INVALID", "Git tree contains a missing or non-blob object")
            try:
                size = int(fields[2])
            except ValueError as error:
                raise VerifyError("TREE_INVALID", "Git returned a malformed blob size") from error
            # Refuse before reading: the size is known from the header, so an
            # oversized blob never has to be held to be rejected.
            if size > MAX_BLOB_BYTES:
                raise VerifyError(
                    "TREE_TOO_LARGE",
                    f"{path} is {size} bytes, above the {MAX_BLOB_BYTES} per-blob ceiling",
                )
            if total + size > MAX_TREE_BYTES:
                raise VerifyError(
                    "TREE_TOO_LARGE", f"tree content exceeds the {MAX_TREE_BYTES} byte ceiling",
                )
            content = _read_exact(proc.stdout, size)
            proc.stdout.read(1)  # the batch protocol terminates each blob with LF
            total += size
            blobs[path] = content
    finally:
        if proc.poll() is None:
            proc.kill()
        writer.join(timeout=5)
        for stream in (proc.stdin, proc.stdout, proc.stderr):
            if stream is not None and not stream.closed:
                stream.close()
        proc.wait()
    return blobs


def load_trusted_records(raw: bytes) -> tuple[dict[str, dict], dict[str, list[str]], set[str]]:
    """Split the trusted detailed ledger into exact, pattern, and id views.

    ``exact`` is what classification is allowed to consult: it is the same
    wildcard-inert view ``provenance_ledger.build_ledger`` uses, so a blanket
    record can never classify a path on its own.  ``patterns`` is used only for
    anchor integrity -- deciding whether a record id the public ledger *already*
    names does in fact speak about that path.  Conflating the two would let a
    blanket record silently attest new files.
    """
    document = strict_json(raw, code="TRUSTED_LEDGER_INVALID", label="trusted detailed ledger")
    if not isinstance(document.get("records"), list):
        raise VerifyError("TRUSTED_LEDGER_INVALID", "trusted detailed ledger has no records array")

    exact: dict[str, dict] = {}
    patterns: dict[str, list[str]] = {}
    ids: set[str] = set()
    for record in document["records"]:
        if not isinstance(record, dict):
            raise VerifyError("TRUSTED_LEDGER_INVALID", "trusted detailed ledger has a malformed record")
        record_id = record.get("id")
        if not isinstance(record_id, str) or not record_id:
            raise VerifyError("TRUSTED_LEDGER_INVALID", "trusted detailed ledger record has no stable id")
        if record_id in ids:
            raise VerifyError("TRUSTED_LEDGER_INVALID", f"duplicate trusted record id: {record_id}")
        ids.add(record_id)
        paths = record.get("paths")
        if not isinstance(paths, list):
            raise VerifyError("TRUSTED_LEDGER_INVALID", "trusted detailed ledger record has no paths list")
        for path in paths:
            if not isinstance(path, str) or not path:
                raise VerifyError("TRUSTED_LEDGER_INVALID", "trusted detailed ledger has a non-string path")
            patterns.setdefault(record_id, []).append(path)
            if any(character in path for character in "*?"):
                continue
            if path in exact and exact[path] is not record:
                raise VerifyError("TRUSTED_LEDGER_INVALID", f"duplicate exact trusted record for {path}")
            exact[path] = record
    return exact, patterns, ids


def _patterns_cover(path: str, patterns) -> bool:
    for pattern in patterns:
        if pattern == path or fnmatchcase(path, pattern):
            return True
        # ``tools/*`` is written as a directory blanket, not a recursive glob;
        # honour it for nested paths so anchor integrity does not report a
        # false positive on a record the authority plainly meant to cover.
        if pattern.endswith("/*") and path.startswith(pattern[:-1]):
            return True
    return False


def _record_covers(path: str, record_id: str, exact: dict[str, dict], patterns: dict[str, list[str]]) -> bool:
    if exact.get(path, {}).get("id") == record_id:
        return True
    return _patterns_cover(path, patterns.get(record_id, ()))


#: How strongly the trusted authority speaks about a path.  Only ``exact`` and
#: ``deterministic`` are authorization; the other two name *why* a path is not
#: authorized, which is what tells a maintainer which remedy applies.
BACKING_EXACT = "exact"                  # a trusted record names this path verbatim
BACKING_DETERMINISTIC = "deterministic"  # documentation/configuration/fixture/metadata by path rule
BACKING_BLANKET = "blanket"              # only a wildcard record covers it, and wildcards are inert
BACKING_NONE = "none"                    # the authority says nothing about this path


def _backing(path: str, exact: dict[str, dict], patterns: dict[str, list[str]]) -> str:
    """Classify how the trusted authority covers ``path``.

    A wildcard is deliberately *not* authorization.  ``provenance_ledger.py``
    refuses to expand one so that a new file cannot inherit an old blanket
    attestation; letting a blanket authorize replacement *content* for an
    already-listed file would reintroduce the same hole through the back door.
    """
    if path in exact:
        return BACKING_EXACT
    if _class_for(path, None)[0] != "unresolved":
        return BACKING_DETERMINISTIC
    for record_patterns in patterns.values():
        if _patterns_cover(path, record_patterns):
            return BACKING_BLANKET
    return BACKING_NONE


def load_trusted_approvals(raw: bytes) -> dict[tuple[str, str], dict]:
    """Read optional legacy exact-blob approvals for strict mode.

    Normal verification is path-authorized and does not consult this optional
    high-churn history.  The parser remains available for
    ``--require-reviewed-blobs`` so a maintainer can opt into the former
    exact-digest policy for a higher-assurance run.

    Schema -- a top-level ``reviewed_blobs`` array, sibling to ``records``::

        {"path": "src/rt/foo.c",
         "sha256": "<64 lowercase hex>",
         "classification": "<private vocabulary term>",
         "record_id": "<id of the record that justifies it>"}

    Any further keys (reviewer, date, notes) are ignored here and never
    printed, so private review metadata cannot escape through this gate.
    """
    document = strict_json(raw, code="TRUSTED_LEDGER_INVALID", label="trusted detailed ledger")
    entries = document.get("reviewed_blobs", [])
    if not isinstance(entries, list):
        raise VerifyError("TRUSTED_LEDGER_INVALID", "trusted reviewed_blobs must be a list")
    if len(entries) > MAX_REVIEWED_BLOBS:
        raise VerifyError("TRUSTED_LEDGER_INVALID", "trusted reviewed_blobs exceeds its ceiling")
    approvals: dict[tuple[str, str], dict] = {}
    for entry in entries:
        if not isinstance(entry, dict):
            raise VerifyError("TRUSTED_LEDGER_INVALID", "trusted reviewed_blobs contains a malformed entry")
        path = entry.get("path")
        if isinstance(path, str) and any(character in path for character in "*?"):
            raise VerifyError("TRUSTED_LEDGER_INVALID", "a blob approval path contains a wildcard")
        path = canonical_path(path, code="TRUSTED_LEDGER_INVALID", label="blob approval")
        digest = entry.get("sha256")
        if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            raise VerifyError("TRUSTED_LEDGER_INVALID", f"a blob approval for {path} has no lowercase sha256")
        if not isinstance(entry.get("record_id"), str) or not entry["record_id"]:
            raise VerifyError("TRUSTED_LEDGER_INVALID", f"a blob approval for {path} names no record")
        if not isinstance(entry.get("classification"), str) or not entry["classification"]:
            raise VerifyError("TRUSTED_LEDGER_INVALID", f"a blob approval for {path} names no classification")
        key = (path, digest)
        # Explicit semantics: one approval per (path, digest). A repeated pair
        # is ambiguous authority even when the two copies agree, so it is
        # refused rather than silently deduplicated. Several *different*
        # digests for one path remain legitimate -- approvals are a revision
        # history -- and only the exact candidate digest ever matches.
        if key in approvals:
            raise VerifyError(
                "TRUSTED_LEDGER_INVALID", f"duplicate blob approval for {path} at the same digest",
            )
        approvals[key] = entry
    return approvals


def public_class_for(path: str, private_classification: str) -> str:
    """Map a private vocabulary term to the public class, without duplicating it.

    ``_class_for`` already owns that mapping; calling it with a synthetic
    record keeps one source of truth, so an approval cannot be read under
    different rules from the record it cites.
    """
    classification, _ = _class_for(path, {"classification": private_classification, "id": None})
    return classification


WITHHELD = "<withheld: private record id>"


def _disclosable(record_id: str | None, public_ids: set[str]) -> str | None:
    """Never widen what the public tree already discloses.

    A trusted record id is public knowledge only when the candidate's own public
    ledger already names it.  Reporting any other id -- for instance the record
    covering a path the public ledger classifies deterministically -- would
    publish a private subsystem name through a CI log.
    """
    if record_id is None or record_id in public_ids:
        return record_id
    return WITHHELD


def _claim(entry: dict) -> tuple[str | None, str | None]:
    evidence = entry.get("evidence")
    record_id = evidence.get("record_id") if isinstance(evidence, dict) else None
    return entry.get("classification"), record_id if isinstance(record_id, str) else None


def _entry_map(document: object, *, code: str, label: str) -> dict[str, dict]:
    if not isinstance(document, dict) or not isinstance(document.get("entries"), list):
        raise VerifyError(code, f"{label} has no entries array")
    result: dict[str, dict] = {}
    for entry in document["entries"]:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str) or not entry["path"]:
            raise VerifyError(code, f"{label} contains a malformed entry")
        path = canonical_path(entry["path"], code=code, label=f"{label} entry")
        if path in result:
            raise VerifyError(code, f"{label} contains duplicate path {path}")
        result[path] = entry
    return result


def _parse_json_blob(blobs: dict[str, bytes], path: str, *, code: str) -> dict:
    raw = blobs.get(path)
    if raw is None:
        raise VerifyError(code, f"tree has no {path}")
    return strict_json(raw, code=code, label=path)


def _load_policy_bytes(raw: bytes, workdir: Path, name: str, *, code: str):
    # publication_policy.load_policy uses a permissive json.loads, so the
    # duplicate-key check has to happen on the bytes here or a policy could
    # carry two values for one key and be read differently downstream.
    strict_json(raw, code=code, label=name)
    workdir.mkdir(parents=True, exist_ok=True)
    target = workdir / name
    target.write_bytes(raw)
    try:
        return load_policy(target)
    except PolicyError as error:
        raise VerifyError(code, str(error)) from error


def _path_is_within(path: Path, root: Path) -> bool:
    """Return whether a path is inside ``root`` after resolving aliases."""

    try:
        path.resolve().relative_to(root.resolve())
    except (OSError, ValueError):
        return False
    return True


def _external_input(path: Path, *, repo: Path, label: str) -> Path:
    """Require an authority/baseline input to live outside the candidate repo."""

    resolved = path.resolve()
    if _path_is_within(resolved, repo):
        raise VerifyError(
            "TRUSTED_INPUT_CANDIDATE_CONTROLLED",
            f"{label} must live outside the repository under verification",
        )
    if not resolved.is_file():
        raise VerifyError("TRUSTED_INPUT_MISSING", f"{label} is unavailable")
    return resolved


def _validate_public_baseline(
    raw: bytes,
    *,
    base_commit: str,
    base_tree: str,
    base_blobs: dict[str, bytes],
    trusted_policy,
) -> tuple[dict, dict[str, dict]]:
    """Load a public baseline and bind it to the exact trusted base.

    During the compatibility window a plain baseline is accepted only when it
    is byte-identical to the ledger blob in the trusted base tree.  An external
    release snapshot can instead use the explicit envelope below::

        {
          "kind": "public-provenance-baseline",
          "schema_version": 1,
          "binding": {"base_commit": ..., "base_tree": ...,
                      "policy_sha256": ..., "ledger_sha256": ...},
          "ledger": { ... public ledger document ... }
        }

    The detailed authority is intentionally not part of this document.  The
    baseline preserves historical public claims; path and blob authority still
    comes from the separately supplied trusted detailed ledger.
    """

    document = strict_json(raw, code="TRUSTED_BASELINE_INVALID", label="trusted public baseline")
    if "ledger" in document or "binding" in document or "kind" in document:
        if document.get("kind") != BASELINE_KIND or document.get("schema_version") != BASELINE_SCHEMA_VERSION:
            raise VerifyError(
                "TRUSTED_BASELINE_INVALID",
                "trusted public baseline envelope has an unsupported kind or schema",
            )
        binding = document.get("binding")
        ledger = document.get("ledger")
        if not isinstance(binding, dict) or not isinstance(ledger, dict):
            raise VerifyError("TRUSTED_BASELINE_INVALID", "trusted public baseline envelope is incomplete")
        expected = {
            "base_commit": base_commit,
            "base_tree": base_tree,
            "policy_sha256": trusted_policy.digest,
            "ledger_sha256": hashlib.sha256(_canonical_json_bytes(ledger)).hexdigest(),
        }
        for key, value in expected.items():
            if binding.get(key) != value:
                raise VerifyError(
                    "TRUSTED_BASELINE_BINDING",
                    f"trusted public baseline binding does not match the exact trusted {key}",
                )
        document = ledger
    else:
        expected_raw = base_blobs.get(LEDGER_PATH)
        if expected_raw is None or raw != expected_raw:
            raise VerifyError(
                "TRUSTED_BASELINE_BINDING",
                "plain trusted public baseline is not the exact ledger blob from the trusted base tree",
            )

    errors = validate_ledger(document, require_hashes=True, require_resolved=True)
    if errors:
        raise VerifyError("TRUSTED_BASELINE_INVALID", errors[0])
    if document.get("policy_profile") not in (None, trusted_policy.name):
        raise VerifyError("TRUSTED_BASELINE_POLICY_MISMATCH", "trusted public baseline names a different policy")

    entries = _entry_map(document, code="TRUSTED_BASELINE_INVALID", label="trusted public baseline")
    expected_scope = {
        path for path in base_blobs
        if trusted_policy.resolve(path).disposition == "included"
    }
    if set(entries) != expected_scope:
        missing = sorted(expected_scope - set(entries))
        extra = sorted(set(entries) - expected_scope)
        detail = f"missing {missing[0]}" if missing else f"unexpected {extra[0]}"
        raise VerifyError(
            "TRUSTED_BASELINE_COVERAGE",
            f"trusted public baseline does not exactly cover the trusted public tree ({detail})",
        )
    for path, entry in entries.items():
        if path in CONTROL_PATHS:
            continue
        expected_hash = hashlib.sha256(base_blobs[path]).hexdigest()
        if entry.get("sha256") != expected_hash:
            raise VerifyError(
                "TRUSTED_BASELINE_CONTENT_MISMATCH",
                f"trusted public baseline hash does not match the trusted base bytes for {path}",
            )
    return document, entries


def _safe_public_claim(path: str, record: dict | None) -> tuple[str, dict]:
    """Derive a public claim without copying private record descriptions."""

    classification, evidence = _class_for(path, record)
    if record is None or classification not in IMPLEMENTATION_CLASSES:
        return classification, dict(evidence)

    safe = {
        "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
        "record_id": record.get("id"),
        "evidence_tier": record.get("evidence_tier"),
    }
    if classification == "project_authored_attested":
        safe.update({
            "authorship": "independent implementation record",
            "upstream_attribution": None,
        })
    elif classification == "upstream_derived":
        safe.update({
            "upstream": "documented upstream family",
            # The SPDX identifier is public (it is in the file header) and the
            # publication audit checks the header against it.
            "license": record.get("upstream_license") or "see NOTICE.md",
            "modification_status": "modified_or_translated; see trusted record",
        })
    elif classification == "generated_from_public_source":
        safe.update({
            "generator": "documented public-source data path",
            "source_family": "public source data",
        })
    return classification, safe


def generate_authority_baseline(*, repo: Path, base_rev: str, trusted_ledger: Path, workdir: Path) -> bytes:
    """Build the public baseline envelope for ``base_rev`` from the trusted authority alone.

    The committed ledger on the base branch is not consulted: every path the
    base policy publishes gets the same safe public claim the verifier gives a
    newly admitted path, derived from its exact trusted record.  The result is
    bound to the base commit, tree and policy exactly like any external
    baseline, and is then checked by ``_validate_public_baseline`` on use.  A
    published path without a qualifying record makes the baseline unresolved,
    which that validation refuses.
    """

    repo = repo.resolve()
    trusted_ledger = _external_input(trusted_ledger, repo=repo, label="trusted detailed ledger")
    workdir = workdir.resolve()
    if _path_is_within(workdir, repo):
        raise VerifyError(
            "OUTPUT_CANDIDATE_CONTROLLED",
            "baseline scratch must live outside the repository under verification",
        )
    base_commit = _rev_commit(repo, base_rev)
    base_tree = _rev_tree(repo, base_commit)
    base_blobs = read_tree(repo, base_tree)
    trusted_policy = _load_policy_bytes(
        base_blobs.get(POLICY_PATH) or b"", workdir, "baseline_policy.json", code="TRUSTED_POLICY_INVALID",
    )
    exact_records, _patterns, _ids = load_trusted_records(trusted_ledger.read_bytes())

    entries: list[dict] = []
    for path in sorted(p for p in base_blobs if trusted_policy.resolve(p).disposition == "included"):
        classification, evidence = _safe_public_claim(path, exact_records.get(path))
        entry = {"path": path, "classification": classification, "evidence": evidence}
        if path not in CONTROL_PATHS:
            entry["sha256"] = hashlib.sha256(base_blobs[path]).hexdigest()
        entries.append(entry)
    ledger = {
        "schema_version": 1,
        "generated_by": "tools/provenance_ledger.py",
        "policy_profile": trusted_policy.name,
        "classification_vocabulary": sorted(ALLOWED_CLASSES),
        "entries": entries,
    }
    envelope = {
        "kind": BASELINE_KIND,
        "schema_version": BASELINE_SCHEMA_VERSION,
        "binding": {
            "base_commit": base_commit,
            "base_tree": base_tree,
            "policy_sha256": trusted_policy.digest,
            "ledger_sha256": hashlib.sha256(_canonical_json_bytes(ledger)).hexdigest(),
        },
        "ledger": ledger,
    }
    return _canonical_json_bytes(envelope)


def _generate_ephemeral_ledger(
    *,
    baseline: dict,
    baseline_entries: dict[str, dict],
    base_blobs: dict[str, bytes],
    candidate_blobs: dict[str, bytes],
    trusted_policy,
    candidate_policy,
    exact_records: dict[str, dict],
) -> tuple[dict, set[str], set[str]]:
    """Generate the candidate ledger in memory from trusted inputs only."""

    trusted_scope = {
        path for path in base_blobs
        if trusted_policy.resolve(path).disposition == "included"
    }
    candidate_included = {
        path for path in candidate_blobs
        if candidate_policy.resolve(path).disposition == "included"
    }
    inherited = trusted_scope & set(candidate_blobs)
    protected = (inherited | candidate_included) & set(candidate_blobs)

    output = json.loads(json.dumps(baseline, ensure_ascii=False))
    output_entries: list[dict] = []
    for path in sorted(protected):
        if path in baseline_entries:
            entry = json.loads(json.dumps(baseline_entries[path], ensure_ascii=False))
        else:
            classification, evidence = _safe_public_claim(path, exact_records.get(path))
            entry = {"path": path, "classification": classification, "evidence": evidence}
        if path not in CONTROL_PATHS:
            entry["sha256"] = hashlib.sha256(candidate_blobs[path]).hexdigest()
        else:
            entry.pop("sha256", None)
        output_entries.append(entry)
    output["entries"] = output_entries

    # An unresolved generated claim remains visible in the temporary evidence
    # so the verdict can report the exact missing/unqualified authority.  It is
    # never treated as release evidence or allowed to produce PASS.
    errors = validate_ledger(output, require_hashes=True, require_resolved=False)
    if errors:
        raise VerifyError("GENERATED_LEDGER_INVALID", errors[0])
    if {entry["path"] for entry in output_entries} != protected:
        raise VerifyError("GENERATED_LEDGER_INVALID", "generated ledger coverage is not exact")
    return output, protected, trusted_scope


def _reconcile_refresh_audit(
    *,
    generated: dict,
    candidate_blobs: dict[str, bytes],
    base_blobs: dict[str, bytes],
    base_tree: str,
    authorized_policy_delta: dict | None,
) -> dict:
    """Accept canonical refresh audit metadata without trusting its claims.

    ``refresh-reviewed`` replaces the previous audit block and drops an old
    admission block.  Neither block grants path or content authority.  The
    generated entries and all other top-level fields must still match the
    trusted integration-time generation exactly; the audit block may only
    describe the changed, existing public paths of this candidate.  The
    pre-refresh candidate tree can be unreachable after a PR is pushed, so its
    SHA is checked for syntax only.  The verdict binds the actual candidate
    tree independently through Git blobs and content hashes.
    """

    raw = candidate_blobs.get(LEDGER_PATH)
    if raw is None:
        return generated
    try:
        declared = strict_json(raw, code="CANDIDATE_LEDGER_INVALID", label="legacy candidate ledger")
    except VerifyError:
        return generated
    if not isinstance(declared, dict):
        return generated
    refresh = declared.get("refresh")
    expected_fields = {"workflow", "trusted_tree", "candidate_tree", "refreshed_paths"}
    if authorized_policy_delta is not None:
        expected_fields |= {"policy_delta", "blessed_candidate_policy_sha256"}
    if not isinstance(refresh, dict) or set(refresh) != expected_fields:
        return generated
    if refresh["workflow"] != "refresh-reviewed" or refresh["trusted_tree"] != base_tree:
        return generated
    if authorized_policy_delta is not None:
        if refresh["policy_delta"] != authorized_policy_delta:
            return generated
        if refresh["blessed_candidate_policy_sha256"] != hashlib.sha256(
            candidate_blobs[POLICY_PATH]
        ).hexdigest():
            return generated
    candidate_tree = refresh["candidate_tree"]
    if not isinstance(candidate_tree, str) or re.fullmatch(r"[0-9a-f]{40}", candidate_tree) is None:
        return generated

    changed = sorted(
        path for path in set(base_blobs) | set(candidate_blobs)
        if path not in CONTROL_PATHS | {POLICY_PATH}
        and base_blobs.get(path) != candidate_blobs.get(path)
    )
    if not changed or any(path not in base_blobs or path not in candidate_blobs for path in changed):
        return generated
    if not set(changed) <= {entry["path"] for entry in generated["entries"]}:
        return generated
    if refresh["refreshed_paths"] != changed:
        return generated

    expected_claims = dict(generated)
    declared_claims = dict(declared)
    for document in (expected_claims, declared_claims):
        document.pop("refresh", None)
        document.pop("admission", None)
    if declared_claims != expected_claims:
        return generated

    reconciled = dict(generated)
    reconciled.pop("admission", None)
    reconciled["refresh"] = refresh
    if _canonical_json_bytes(reconciled) != raw:
        return generated
    return reconciled


def _generate_ephemeral_export(
    *,
    candidate_blobs: dict[str, bytes],
    candidate_policy,
    ledger_bytes: bytes,
) -> dict:
    """Build the export from candidate bytes and the newly generated ledger.

    While candidates still commit the legacy ledger, ``policy_sync.py`` counts
    that file and excludes only the self-referential export. The current
    generated ledger replaces the candidate's old ledger in the digest input,
    so the export reaches its fixed point in this invocation. The commit tree
    is reported separately in the verdict; it is not embedded in this
    self-referential control output.
    """

    files = sorted(
        (path, ledger_bytes if path == LEDGER_PATH else raw)
        for path, raw in candidate_blobs.items()
    )
    legacy_ledger_present = LEDGER_PATH in candidate_blobs
    return _build_export_document(
        candidate_policy,
        files,
        provenance_ledger=ledger_bytes,
        manifest=candidate_blobs.get(MANIFEST_PATH),
        exclude_generated_controls=not legacy_ledger_present,
    )


def _legacy_export_findings(
    candidate_blobs: dict[str, bytes],
    generated_export: dict,
) -> list[Finding]:
    """Compare a legacy candidate export without using it as an input."""

    raw = candidate_blobs.get(EXPORT_PATH)
    if raw is None:
        return []
    try:
        declared = strict_json(raw, code="EXPORT_UNREADABLE", label="legacy public export")
    except VerifyError as error:
        return [Finding(error.code, EXPORT_PATH, str(error))]
    return _export_field_findings(declared, generated_export, label="legacy export")


def _export_field_findings(declared: dict, generated: dict, *, label: str) -> list[Finding]:
    """Compare a declared export against the recomputed one, field for field.

    A tree-derived field may be absent from the committed control -- that is the
    shape this repository now commits, so that an unrelated merge cannot rewrite
    the line and conflict with an open pull request.  Absence is not trust: the
    value is recomputed here from the candidate tree and the policy, and a
    declared value that disagrees is refused exactly like any other field.  Every
    other field, and any field the recomputation does not produce, stays required.
    """
    findings: list[Finding] = []
    for field in sorted(set(generated) | set(declared)):
        if field in EXPORT_ADVISORY_FIELDS:
            continue
        if field not in declared:
            if field in _EXPORT_TREE_DERIVED_FIELDS:
                continue
            findings.append(Finding(
                "EXPORT_FIELD_MISMATCH", EXPORT_PATH,
                f"{label} omits the required field {field!r}",
            ))
        elif field not in generated:
            findings.append(Finding(
                "EXPORT_FIELD_MISMATCH", EXPORT_PATH,
                f"{label} declares {field!r}, which the recomputation does not produce",
            ))
        elif declared[field] != generated[field]:
            findings.append(Finding(
                "EXPORT_FIELD_MISMATCH", EXPORT_PATH,
                f"{label} field {field!r} does not match the trusted recomputation "
                "from the candidate tree and the canonical policy",
            ))
    return findings


def _ephemeral_verdict_findings(
    *,
    candidate_commit: str,
    base_commit: str,
    candidate_blobs: dict[str, bytes],
    base_blobs: dict[str, bytes],
    candidate_policy,
    trusted_policy,
    candidate_policy_matches_trusted: bool,
    authorized_policy_delta: dict | None,
    trusted_scope: set[str],
    protected: set[str],
    baseline_entries: dict[str, dict],
    exact_records: dict[str, dict],
    record_patterns: dict[str, list[str]],
    approvals: dict[tuple[str, str], dict],
    require_exact_blob_approvals: bool,
    generated_ledger_bytes: bytes,
    generated_export: dict,
) -> tuple[list[Finding], list[dict], list[dict], list[dict]]:
    """Apply the finding vocabulary to the trusted generated state.

    Path authority is the normal policy.  Exact blob approvals are retained as
    an explicit opt-in for higher-assurance callers and compatibility tests.
    """

    findings: list[Finding] = []
    debt: list[dict] = []
    blob_approved: list[dict] = []
    blob_unapproved: list[dict] = []

    if not candidate_policy_matches_trusted:
        findings.append(Finding(
            "POLICY_SUBSTITUTION", POLICY_PATH,
            "candidate publication policy differs from the trusted base; an external blessed policy "
            "and policy-delta authority are required for an intentional change",
        ))
    findings.extend(_policy_findings(
        candidate_policy,
        trusted_policy,
        authorized_delta=authorized_policy_delta,
    ))
    findings.extend(_ci_findings(candidate_blobs, base_blobs))

    candidate_included = {
        path for path in candidate_blobs
        if candidate_policy.resolve(path).disposition == "included"
    }
    inherited = trusted_scope & set(candidate_blobs)
    for path in sorted(inherited - candidate_included):
        findings.append(Finding(
            "TRUSTED_SCOPE_VIOLATION", path,
            "the path is protected in the trusted base universe and still exists in the candidate tree, "
            "but the candidate policy no longer includes it; publication scope may widen, never narrow",
        ))
    for path in sorted(set(candidate_blobs) - set(base_blobs) - candidate_included):
        if not _admission_requires_implementation(path):
            continue
        if trusted_policy.resolve(path).disposition == "excluded":
            continue
        findings.append(Finding(
            "TRUSTED_SCOPE_VIOLATION", path,
            "new implementation-bearing path is neither included by the candidate policy nor excluded "
            "by the trusted policy, so it would enter the tree unverified",
            maintainer_action=True,
        ))

    legacy_entries: dict[str, dict] | None = None
    legacy_raw = candidate_blobs.get(LEDGER_PATH)
    if legacy_raw is not None:
        try:
            legacy_document = strict_json(legacy_raw, code="CANDIDATE_LEDGER_INVALID", label="legacy candidate ledger")
            legacy_entries = _entry_map(
                legacy_document, code="CANDIDATE_LEDGER_INVALID", label="legacy candidate ledger"
            )
            for error in validate_ledger(legacy_document, require_hashes=True, require_resolved=True):
                findings.append(Finding("LEDGER_SCHEMA", LEDGER_PATH, error))
        except VerifyError as error:
            findings.append(Finding(error.code, LEDGER_PATH, str(error)))
        if legacy_raw != generated_ledger_bytes:
            findings.append(Finding(
                "LEGACY_CONTROL_MISMATCH", LEDGER_PATH,
                "candidate carries legacy ledger bytes different from the trusted integration-time output; "
                "the legacy file is compatibility data only",
            ))

    findings.extend(_legacy_export_findings(candidate_blobs, generated_export))

    if legacy_entries is not None:
        for path in sorted(protected - set(legacy_entries)):
            findings.append(Finding("LEDGER_COVERAGE", path, "protected path has no legacy ledger entry"))
        for path in sorted(set(legacy_entries) - protected):
            findings.append(Finding(
                "LEDGER_COVERAGE", path,
                "legacy ledger entry does not correspond to a protected candidate path",
            ))

    for path in sorted(protected):
        expected_class, expected_evidence = _class_for(path, exact_records.get(path))
        expected_record = expected_evidence.get("record_id")
        base_entry = baseline_entries.get(path)
        base_claim = _claim(base_entry) if base_entry is not None else None
        content_frozen = path in base_blobs and base_blobs[path] == candidate_blobs[path]
        is_new = path not in base_blobs

        record_finding = _exact_record_finding_for_change(
            path,
            base_blobs=base_blobs,
            candidate_blobs=candidate_blobs,
            exact_records=exact_records,
        )
        if record_finding == "TRUSTED_PATH_MISSING":
            findings.append(Finding(
                record_finding, path,
                "trusted detailed authority has no exact path-specific record for this added or changed "
                "executable/security-sensitive path",
                maintainer_action=True,
            ))
        elif record_finding == "TRUSTED_PATH_UNQUALIFIED":
            findings.append(Finding(
                record_finding, path,
                "the exact trusted record for this added or changed executable/security-sensitive path "
                "is not implementation-grade",
                maintainer_action=True,
            ))

        # Optional exact-blob authorization is independent of the candidate
        # ledger.  It is deliberately disabled for the normal path-authorized
        # merge policy; a caller that needs the former higher-assurance policy
        # opts in explicitly.
        blob_gate = (
            not content_frozen
            and (
                expected_class in IMPLEMENTATION_CLASSES
                or _admission_requires_implementation(path)
            )
        )
        if require_exact_blob_approvals and blob_gate:
            digest = hashlib.sha256(candidate_blobs[path]).hexdigest()
            approval = approvals.get((path, digest))
            reason = (
                "new executable/security-sensitive path" if is_new
                else "executable/security-sensitive bytes changed"
            )
            if approval is None:
                findings.append(Finding(
                    "BLOB_UNAPPROVED", path,
                    f"{reason}: the trusted authority has no reviewed-blob approval for this exact content "
                    f"(sha256 {digest}); path coverage is not content approval",
                ))
                blob_unapproved.append({"path": path, "sha256": digest, "reason": reason})
            else:
                approved_class = public_class_for(path, approval["classification"])
                if approval["record_id"] != expected_record:
                    findings.append(Finding(
                        "BLOB_APPROVAL_RECORD_MISMATCH", path,
                        "the blob approval cites a record that is not the exact record covering this path",
                    ))
                elif approved_class != expected_class:
                    findings.append(Finding(
                        "BLOB_APPROVAL_CLASS_MISMATCH", path,
                        f"the blob approval authorizes classification {approved_class!r}, but trusted "
                        f"derivation is {expected_class!r}",
                    ))
                else:
                    blob_approved.append({"path": path, "sha256": digest})

        if legacy_entries is None or path not in legacy_entries:
            continue
        entry = legacy_entries[path]
        classification, record_id = _claim(entry)
        if path not in CONTROL_PATHS:
            actual = hashlib.sha256(candidate_blobs[path]).hexdigest()
            if entry.get("sha256") != actual:
                findings.append(Finding(
                    "CONTENT_MISMATCH", path,
                    "ledger hash does not match the candidate bytes under review",
                ))

        if record_id is not None and not _record_covers(path, record_id, exact_records, record_patterns):
            findings.append(Finding(
                "TRUSTED_RECORD_UNRESOLVED", path,
                "the record id this entry names is not resolvable to trusted authority covering this path; "
                "the attestation is unbacked",
            ))

        if (
            base_entry is not None
            and base_entry.get("classification") in IMPLEMENTATION_CLASSES
            and classification not in IMPLEMENTATION_CLASSES
            and expected_class in IMPLEMENTATION_CLASSES
        ):
            findings.append(Finding(
                "CLASSIFICATION_DOWNGRADE", path,
                f"the trusted base ledger classifies this path {base_entry['classification']!r}, which is "
                f"content-gated; the candidate reclassifies it {classification!r}, which is not",
            ))

        claim = (classification, record_id)
        agrees = claim == (expected_class, expected_record) and expected_class != "unresolved"
        claim_frozen = base_claim == claim
        if agrees:
            continue
        if not claim_frozen:
            findings.append(Finding(
                "CLAIM_UNBACKED", path,
                "restated or new legacy provenance claim does not match trusted derivation",
            ))
        elif not content_frozen:
            findings.append(Finding(
                "CONTENT_UNATTESTED", path,
                "content changed under an inherited provenance claim that trusted authority does not support",
            ))
        else:
            debt.append({
                "path": path,
                "claimed": classification,
                "claimed_record_id": record_id,
                "trusted": expected_class,
                "trusted_record_id": expected_record,
                "backing": _backing(path, exact_records, record_patterns),
            })
    return findings, debt, blob_approved, blob_unapproved


@dataclass(frozen=True)
class EphemeralControls:
    """The canonical control bytes and validated context used to produce them."""

    base_commit: str
    base_tree: str
    candidate_tree: str
    candidate_blobs: dict[str, bytes]
    base_blobs: dict[str, bytes]
    trusted_raw: bytes
    baseline_bytes: bytes
    candidate_policy_raw: bytes
    exact_records: dict[str, dict]
    record_patterns: dict[str, list[str]]
    record_ids: set[str]
    approvals: dict[tuple[str, str], dict]
    trusted_policy: object
    candidate_policy: object
    candidate_policy_matches_trusted: bool
    policy_delta: dict | None
    baseline_entries: dict[str, dict]
    protected: set[str]
    trusted_scope: set[str]
    generated_ledger: dict
    generated_ledger_bytes: bytes
    generated_export: dict
    generated_export_bytes: bytes


def generate_ephemeral_controls(
    *,
    repo: Path,
    candidate_tree: str,
    base_rev: str,
    trusted_ledger: Path,
    trusted_baseline: Path,
    workdir: Path,
    trusted_candidate_policy: Path | None = None,
    policy_delta_authority: Path | None = None,
    require_exact_blob_approvals: bool = False,
) -> EphemeralControls:
    """Generate the hosted attestation controls from an exact candidate tree.

    Local refresh and hosted attestation both call this function. Candidate
    controls are replaced from the trusted base ledger and the external detailed
    authority; the candidate ledger and export are never trust inputs.
    """

    repo = repo.resolve()
    trusted_ledger = _external_input(trusted_ledger, repo=repo, label="trusted detailed ledger")
    baseline_file = _external_input(trusted_baseline, repo=repo, label="trusted public baseline")
    if (trusted_candidate_policy is None) != (policy_delta_authority is None):
        raise VerifyError(
            "POLICY_DELTA_ARGUMENT_REQUIRED",
            "--trusted-candidate-policy and --policy-delta-authority must be supplied together",
        )
    blessed_policy_file: Path | None = None
    policy_delta_authority_file: Path | None = None
    if trusted_candidate_policy is not None:
        blessed_policy_file = _external_input(
            trusted_candidate_policy, repo=repo, label="trusted candidate policy",
        )
        policy_delta_authority_file = _external_input(
            policy_delta_authority, repo=repo, label="policy delta authority",
        )
    workdir = workdir.resolve()
    if _path_is_within(workdir, repo):
        raise VerifyError(
            "OUTPUT_CANDIDATE_CONTROLLED",
            "ephemeral provenance scratch must live outside the repository under verification",
        )
    workdir.mkdir(parents=True, exist_ok=True)
    trusted_inputs = {trusted_ledger.resolve(), baseline_file.resolve()}
    if blessed_policy_file is not None:
        trusted_inputs.add(blessed_policy_file.resolve())
    if policy_delta_authority_file is not None:
        trusted_inputs.add(policy_delta_authority_file.resolve())
    generated_targets = {
        (workdir / "public_provenance_ledger.json").resolve(),
        (workdir / "PUBLIC_EXPORT.json").resolve(),
        (workdir / "inputs" / "trusted_policy.json").resolve(),
        (workdir / "inputs" / "candidate_policy.json").resolve(),
        (workdir / "inputs" / "blessed_candidate_policy.json").resolve(),
    }
    collisions = sorted(trusted_inputs & generated_targets, key=str)
    if collisions:
        raise VerifyError(
            "OUTPUT_TRUSTED_INPUT_COLLISION",
            "ephemeral output would overwrite trusted input: " + str(collisions[0]),
        )

    base_commit = _rev_commit(repo, base_rev)
    base_tree = _rev_tree(repo, base_commit)
    candidate_tree = _rev_tree(repo, candidate_tree)
    candidate_blobs = read_tree(repo, candidate_tree)
    base_blobs = read_tree(repo, base_tree)

    trusted_raw = trusted_ledger.read_bytes()
    exact_records, record_patterns, record_ids = load_trusted_records(trusted_raw)
    approvals = load_trusted_approvals(trusted_raw) if require_exact_blob_approvals else {}
    for (approved_path, _digest), approval in approvals.items():
        if approval["record_id"] not in record_ids:
            raise VerifyError(
                "TRUSTED_LEDGER_INVALID",
                f"a blob approval for {approved_path} cites a record that does not exist",
            )

    base_policy_bytes = base_blobs.get(POLICY_PATH) or b""
    trusted_policy = _load_policy_bytes(
        base_policy_bytes, workdir / "inputs", "trusted_policy.json", code="TRUSTED_POLICY_INVALID",
    )
    candidate_policy_raw = candidate_blobs.get(POLICY_PATH)
    if candidate_policy_raw is None:
        raise VerifyError("CANDIDATE_POLICY_MISSING", f"candidate tree has no {POLICY_PATH}")
    candidate_policy = _load_policy_bytes(
        candidate_policy_raw, workdir / "inputs", "candidate_policy.json", code="CANDIDATE_POLICY_INVALID",
    )
    candidate_policy_matches_trusted = candidate_policy_raw == base_policy_bytes
    policy_delta: dict | None = None
    if blessed_policy_file is not None:
        delta_policy_bytes = blessed_policy_file.read_bytes()
        if delta_policy_bytes == base_policy_bytes:
            raise VerifyError(
                "POLICY_DELTA_EMPTY",
                "blessed candidate policy equals the trusted base policy; omit policy-delta inputs when unchanged",
            )
        if candidate_policy_raw != delta_policy_bytes:
            raise VerifyError(
                "CANDIDATE_POLICY_MISMATCH",
                "candidate publication policy does not match the externally blessed candidate policy",
            )
        candidate_policy = _load_policy_bytes(
            delta_policy_bytes,
            workdir / "inputs",
            "blessed_candidate_policy.json",
            code="CANDIDATE_POLICY_INVALID",
        )
        baseline_document = strict_json(
            base_policy_bytes, code="TRUSTED_POLICY_INVALID", label="trusted policy",
        )
        candidate_document = strict_json(
            delta_policy_bytes, code="CANDIDATE_POLICY_INVALID", label="blessed candidate policy",
        )
        try:
            allowed_delta = _read_policy_delta_authority(
                policy_delta_authority_file,
                candidate_root=repo,
                baseline_policy_bytes=base_policy_bytes,
                candidate_policy_bytes=delta_policy_bytes,
            )
        except RefreshError as error:
            raise VerifyError(error.code, str(error)) from error
        policy_delta = _classify_policy_delta(baseline_document, candidate_document)
        if policy_delta != allowed_delta:
            raise VerifyError(
                "POLICY_DELTA_UNAUTHORIZED",
                "policy delta differs from the independently approved delta; "
                f"computed={policy_delta} allowed={allowed_delta}",
            )
        candidate_policy_matches_trusted = True

    baseline_bytes = baseline_file.read_bytes()
    baseline, baseline_entries = _validate_public_baseline(
        baseline_bytes,
        base_commit=base_commit,
        base_tree=base_tree,
        base_blobs=base_blobs,
        trusted_policy=trusted_policy,
    )
    generated_ledger, protected, trusted_scope = _generate_ephemeral_ledger(
        baseline=baseline,
        baseline_entries=baseline_entries,
        base_blobs=base_blobs,
        candidate_blobs=candidate_blobs,
        trusted_policy=trusted_policy,
        candidate_policy=candidate_policy,
        exact_records=exact_records,
    )
    generated_ledger = _reconcile_refresh_audit(
        generated=generated_ledger,
        candidate_blobs=candidate_blobs,
        base_blobs=base_blobs,
        base_tree=base_tree,
        authorized_policy_delta=policy_delta,
    )
    generated_ledger_bytes = _canonical_json_bytes(generated_ledger)
    generated_export = _generate_ephemeral_export(
        candidate_blobs=candidate_blobs,
        candidate_policy=candidate_policy,
        ledger_bytes=generated_ledger_bytes,
    )
    # The generated export has ONE committed shape: the policy-derived control.
    # The tree-wide digests stay in ``generated_export`` for the field-by-field
    # comparison, and every writer -- the hosted scratch output and the
    # maintainer's ``make provenance-refresh`` -- emits the same control bytes.
    generated_export_bytes = _canonical_json_bytes(_build_control_document(generated_export))
    return EphemeralControls(
        base_commit=base_commit,
        base_tree=base_tree,
        candidate_tree=candidate_tree,
        candidate_blobs=candidate_blobs,
        base_blobs=base_blobs,
        trusted_raw=trusted_raw,
        baseline_bytes=baseline_bytes,
        candidate_policy_raw=candidate_policy_raw,
        exact_records=exact_records,
        record_patterns=record_patterns,
        record_ids=record_ids,
        approvals=approvals,
        trusted_policy=trusted_policy,
        candidate_policy=candidate_policy,
        candidate_policy_matches_trusted=candidate_policy_matches_trusted,
        policy_delta=policy_delta,
        baseline_entries=baseline_entries,
        protected=protected,
        trusted_scope=trusted_scope,
        generated_ledger=generated_ledger,
        generated_ledger_bytes=generated_ledger_bytes,
        generated_export=generated_export,
        generated_export_bytes=generated_export_bytes,
    )


def verify_ephemeral(
    *,
    repo: Path,
    candidate_rev: str,
    base_rev: str,
    trusted_ledger: Path,
    trusted_baseline: Path,
    output_dir: Path,
    trusted_candidate_policy: Path | None = None,
    policy_delta_authority: Path | None = None,
    require_immutable_revisions: bool = False,
    authority_revision: str | None = None,
    require_exact_blob_approvals: bool = False,
) -> dict:
    """Verify a candidate while materializing provenance controls outside Git.

    The candidate tree is read only through Git objects.  Its legacy ledger and
    export, when present, are compared as untrusted compatibility data; the
    temporary outputs used for the verdict are generated from the trusted base
    baseline, candidate blobs, trusted policy, and external detailed authority.
    """

    repo = repo.resolve()

    if require_immutable_revisions:
        for role, selector in (("candidate", candidate_rev), ("base", base_rev)):
            if len(selector) != 40 or any(c not in "0123456789abcdef" for c in selector):
                raise VerifyError(
                    "MUTABLE_REVISION_REFUSED",
                    f"{role} revision {selector!r} is not a full 40-hex commit SHA",
                )

    candidate_commit = _rev_commit(repo, candidate_rev)
    candidate_tree = _rev_tree(repo, candidate_commit)
    output_dir = output_dir.resolve()
    controls = generate_ephemeral_controls(
        repo=repo,
        candidate_tree=candidate_tree,
        base_rev=base_rev,
        trusted_ledger=trusted_ledger,
        trusted_baseline=trusted_baseline,
        workdir=output_dir,
        trusted_candidate_policy=trusted_candidate_policy,
        policy_delta_authority=policy_delta_authority,
        require_exact_blob_approvals=require_exact_blob_approvals,
    )
    base_commit = controls.base_commit
    base_tree = controls.base_tree
    candidate_tree = controls.candidate_tree
    candidate_blobs = controls.candidate_blobs
    base_blobs = controls.base_blobs
    trusted_raw = controls.trusted_raw
    baseline_bytes = controls.baseline_bytes
    candidate_policy_raw = controls.candidate_policy_raw
    exact_records = controls.exact_records
    record_patterns = controls.record_patterns
    record_ids = controls.record_ids
    approvals = controls.approvals
    trusted_policy = controls.trusted_policy
    candidate_policy = controls.candidate_policy
    candidate_policy_matches_trusted = controls.candidate_policy_matches_trusted
    policy_delta = controls.policy_delta
    baseline_entries = controls.baseline_entries
    protected = controls.protected
    trusted_scope = controls.trusted_scope
    generated_ledger_bytes = controls.generated_ledger_bytes
    generated_export = controls.generated_export
    generated_export_bytes = controls.generated_export_bytes

    findings, debt, blob_approved, blob_unapproved = _ephemeral_verdict_findings(
        candidate_commit=candidate_commit,
        base_commit=base_commit,
        candidate_blobs=candidate_blobs,
        base_blobs=base_blobs,
        candidate_policy=candidate_policy,
        trusted_policy=trusted_policy,
        candidate_policy_matches_trusted=candidate_policy_matches_trusted,
        trusted_scope=trusted_scope,
        protected=protected,
        baseline_entries=baseline_entries,
        exact_records=exact_records,
        record_patterns=record_patterns,
        approvals=approvals,
        require_exact_blob_approvals=require_exact_blob_approvals,
        generated_ledger_bytes=generated_ledger_bytes,
        generated_export=generated_export,
        authorized_policy_delta=policy_delta,
    )
    if not _is_ancestor(repo, base_commit, candidate_commit):
        findings.insert(0, Finding(
            "MERGE_BASE_STALE", "",
            f"base {base_commit} is not an ancestor of candidate {candidate_commit}; the tree that would "
            "result from merging is not the tree this run attested",
        ))

    ledger_output = output_dir / "public_provenance_ledger.json"
    export_output = output_dir / "PUBLIC_EXPORT.json"
    ledger_output.write_bytes(generated_ledger_bytes)
    export_output.write_bytes(generated_export_bytes)

    fatal = [finding for finding in findings if finding.fatal]
    return {
        "tool": "tools/provenance_attest_verify.py",
        "mode": "ephemeral",
        "verdict": "fail" if fatal else "pass",
        "repository_scope": {
            "candidate_commit": candidate_commit,
            "candidate_tree": candidate_tree,
            "base_commit": base_commit,
            "base_tree": base_tree,
        },
        "trusted_ledger_sha256": hashlib.sha256(trusted_raw).hexdigest(),
        "trusted_baseline_sha256": hashlib.sha256(baseline_bytes).hexdigest(),
        "authority_revision": authority_revision,
        "policy_delta": policy_delta,
        "blessed_candidate_policy_sha256": (
            hashlib.sha256(candidate_policy_raw).hexdigest() if policy_delta is not None else None
        ),
        "trusted_record_count": len(record_ids),
        "public_path_count": len(protected),
        "generated_outputs": {
            "ledger_sha256": hashlib.sha256(generated_ledger_bytes).hexdigest(),
            "export_sha256": hashlib.sha256(generated_export_bytes).hexdigest(),
            "ledger_path": str(ledger_output),
            "export_path": str(export_output),
        },
        "legacy_controls_present": {
            "ledger": LEDGER_PATH in candidate_blobs,
            "export": EXPORT_PATH in candidate_blobs,
        },
        "findings": [finding.as_dict() for finding in findings],
        "fatal_count": len(fatal),
        "maintainer_action_paths": sorted(
            {finding.path for finding in findings if finding.maintainer_action and finding.fatal}
        ),
        "exact_blob_approvals_required": require_exact_blob_approvals,
        "blob_approvals_available": len(approvals),
        "blobs_approved_this_candidate": sorted(blob_approved, key=lambda item: item["path"]),
        "blobs_unapproved": sorted(blob_unapproved, key=lambda item: item["path"]),
        "grandfathered_debt_count": len(debt),
        "grandfathered_debt": debt,
    }


def _policy_findings(
    candidate_policy,
    trusted_policy,
    *,
    authorized_delta: dict | None = None,
) -> list[Finding]:
    """Publication scope may tighten in a pull request; it may never loosen."""
    findings: list[Finding] = []
    authorized_exclude_removed = set((authorized_delta or {}).get("exclude_removed", ()))
    dropped = sorted(
        (trusted_policy.exclude_paths - candidate_policy.exclude_paths)
        - authorized_exclude_removed
    )
    for path in dropped:
        findings.append(Finding(
            "POLICY_SUBSTITUTION", POLICY_PATH,
            f"candidate policy removes the trusted exclusion of {path}",
        ))
    for attribute in ("exclude_globs", "exclude_prefixes"):
        trusted_values = set(getattr(trusted_policy, attribute, ()) or ())
        candidate_values = set(getattr(candidate_policy, attribute, ()) or ())
        for value in sorted(trusted_values - candidate_values):
            findings.append(Finding(
                "POLICY_SUBSTITUTION", POLICY_PATH,
                f"candidate policy removes the trusted {attribute} entry {value}",
            ))
    return findings


#: Export fields whose value is security-relevant and therefore recomputed.
#: Older exports may carry ``candidate_tree``; it is advisory and the actual
#: candidate tree is bound by the verdict's repository_scope.
EXPORT_ADVISORY_FIELDS = frozenset({"candidate_tree", "source_tree"})


def _export_findings(candidate: dict[str, bytes], policy) -> list[Finding]:
    """Recompute the public export and compare it, field for field.

    Checking one digest was not enough. Every other field -- the policy digest,
    the included-content digest, the counts, the exclusion lists, the schema
    version -- was accepted from the candidate, so a coherent ledger digest
    bought silence on all of them. Rather than hand-writing a check per field,
    build the canonical document with the same generator the release process
    uses and compare the whole object.
    """
    export_raw = candidate.get(EXPORT_PATH)
    if export_raw is None:
        return [Finding("EXPORT_MISSING", EXPORT_PATH, "candidate tree has no public export control file")]
    try:
        declared = strict_json(export_raw, code="EXPORT_UNREADABLE", label="public export")
    except VerifyError as error:
        return [Finding("EXPORT_UNREADABLE", EXPORT_PATH, str(error))]
    ledger_raw = candidate.get(LEDGER_PATH)
    manifest_raw = candidate.get(MANIFEST_PATH)
    if ledger_raw is None:
        return [Finding("EXPORT_UNREADABLE", EXPORT_PATH, "candidate tree has no provenance ledger to export")]

    files = [(path, b"" if path == EXPORT_PATH else raw) for path, raw in sorted(candidate.items())]
    canonical = _build_export_document(
        policy, files,
        provenance_ledger=ledger_raw,
        manifest=manifest_raw,
    )
    return _export_field_findings(declared, canonical, label="export")


def _ci_findings(candidate: dict[str, bytes], trusted: dict[str, bytes]) -> list[Finding]:
    """Refuse a candidate that disarms this gate or answers to its check name.

    GitHub matches required status checks by *context name*.  A candidate
    workflow declaring a job named like this gate's check therefore produces a
    second, candidate-controlled result under the required context.  That is a
    merge-path bypass whatever the verifier concludes, so the name is reserved.
    """
    findings: list[Finding] = []
    needle = TRUSTED_CONTEXT.encode("utf-8")
    for path, raw in sorted(candidate.items()):
        if not path.startswith(WORKFLOW_PREFIX) or path == TRUSTED_WORKFLOW:
            continue
        if needle in raw:
            findings.append(Finding(
                "CI_CONTEXT_COLLISION", path,
                f"workflow names the reserved required-check context {TRUSTED_CONTEXT!r}; "
                "a second job under that context would satisfy the merge rule without this gate",
            ))
    candidate_workflow = candidate.get(TRUSTED_WORKFLOW)
    trusted_workflow = trusted.get(TRUSTED_WORKFLOW)
    if trusted_workflow is not None and candidate_workflow is None:
        findings.append(Finding(
            "TRUSTED_WORKFLOW_WEAKENED", TRUSTED_WORKFLOW,
            "candidate deletes the trusted provenance gate workflow",
        ))
    elif candidate_workflow is not None and candidate_workflow != trusted_workflow:
        # A workflow the base does not have yet is the change that introduces
        # this gate.  It cannot weaken a predecessor that does not exist, but
        # its content must still be the shape the gate depends on.
        for token in (b"pull_request_target", needle):
            if token not in candidate_workflow:
                findings.append(Finding(
                    "TRUSTED_WORKFLOW_WEAKENED", TRUSTED_WORKFLOW,
                    f"candidate workflow does not carry {token.decode('utf-8')!r}",
                ))
        if trusted_workflow is not None:
            findings.append(Finding(
                "TRUSTED_WORKFLOW_MODIFIED", TRUSTED_WORKFLOW,
                "candidate modifies the trusted gate workflow; the base version governed this run, "
                "so the edit takes effect only after a human reviews and merges it",
                fatal=False,
            ))
    return findings


def verify(
    *,
    repo: Path,
    candidate_rev: str,
    base_rev: str,
    trusted_ledger: Path,
    workdir: Path,
    require_immutable_revisions: bool = False,
    authority_revision: str | None = None,
    require_exact_blob_approvals: bool = False,
) -> dict:
    """Return a verdict for ``candidate_rev`` against external trusted authority."""
    repo = repo.resolve()
    if require_immutable_revisions:
        # A branch name is a moving target: resolving it twice can name two
        # different trees.  CI must pass the exact SHAs the event carried, so
        # the verdict is about the commits actually under review.
        for role, selector in (("candidate", candidate_rev), ("base", base_rev)):
            if len(selector) != 40 or any(c not in "0123456789abcdef" for c in selector):
                raise VerifyError(
                    "MUTABLE_REVISION_REFUSED",
                    f"{role} revision {selector!r} is not a full 40-hex commit SHA",
                )
    trusted_ledger = trusted_ledger.resolve()
    try:
        trusted_ledger.relative_to(repo)
    except ValueError:
        pass
    else:
        raise VerifyError(
            "TRUSTED_INPUT_CANDIDATE_CONTROLLED",
            "the trusted detailed ledger must live outside the repository under verification",
        )
    if not trusted_ledger.is_file():
        raise VerifyError("TRUSTED_INPUT_MISSING", "the trusted detailed ledger is unavailable")

    candidate_commit = _rev_commit(repo, candidate_rev)
    base_commit = _rev_commit(repo, base_rev)
    candidate_tree = _rev_tree(repo, candidate_commit)
    base_tree = _rev_tree(repo, base_commit)
    candidate_blobs = read_tree(repo, candidate_tree)
    base_blobs = read_tree(repo, base_tree)

    trusted_raw = trusted_ledger.read_bytes()
    exact_records, record_patterns, record_ids = load_trusted_records(trusted_raw)
    approvals = load_trusted_approvals(trusted_raw) if require_exact_blob_approvals else {}
    for (approved_path, _digest), approval in approvals.items():
        if approval["record_id"] not in record_ids:
            raise VerifyError(
                "TRUSTED_LEDGER_INVALID",
                f"a blob approval for {approved_path} cites a record that does not exist",
            )

    trusted_policy = _load_policy_bytes(
        base_blobs.get(POLICY_PATH) or b"", workdir, "trusted_policy.json", code="TRUSTED_POLICY_INVALID",
    )
    candidate_policy_raw = candidate_blobs.get(POLICY_PATH)
    if candidate_policy_raw is None:
        raise VerifyError("CANDIDATE_POLICY_MISSING", f"candidate tree has no {POLICY_PATH}")
    candidate_policy = _load_policy_bytes(
        candidate_policy_raw, workdir, "candidate_policy.json", code="CANDIDATE_POLICY_INVALID",
    )

    candidate_ledger = _parse_json_blob(candidate_blobs, LEDGER_PATH, code="CANDIDATE_LEDGER_INVALID")
    candidate_entries = _entry_map(candidate_ledger, code="CANDIDATE_LEDGER_INVALID", label="candidate ledger")
    base_entries: dict[str, dict] = {}
    if LEDGER_PATH in base_blobs:
        base_entries = _entry_map(
            _parse_json_blob(base_blobs, LEDGER_PATH, code="TRUSTED_BASE_LEDGER_INVALID"),
            code="TRUSTED_BASE_LEDGER_INVALID", label="trusted base ledger",
        )

    findings: list[Finding] = []
    # -- merge-tree identity -------------------------------------------
    #
    # This gate attests the pull request HEAD tree. The repository allows
    # merge, squash and rebase. All three produce a final tree equal to the
    # head tree -- but only when the base is already an ancestor of the head,
    # because then there is nothing to reconcile and no conflict-resolution
    # bytes can appear after verification. When the head is behind, the merge
    # result is a tree this run never saw, so the verdict does not describe
    # what would land.
    if not _is_ancestor(repo, base_commit, candidate_commit):
        findings.append(Finding(
            "MERGE_BASE_STALE", "",
            f"base {base_commit} is not an ancestor of candidate {candidate_commit}; the tree "
            "that would result from merging is not the tree this run attested, so no verdict "
            "is issued for it. Update the branch and re-run",
        ))
    findings.extend(_policy_findings(candidate_policy, trusted_policy))
    findings.extend(_ci_findings(candidate_blobs, base_blobs))

    for error in validate_ledger(candidate_ledger, require_hashes=True, require_resolved=True):
        findings.append(Finding("LEDGER_SCHEMA", LEDGER_PATH, error))

    # -- the trusted protected universe --------------------------------
    #
    # The candidate's own policy must not decide what the gate looks at.
    # Letting it do so was a complete bypass: change an implementation file,
    # drop it from include_paths, delete its ledger entry, regenerate the
    # export, and every downstream check simply stopped seeing the path.
    #
    # Scope is therefore anchored to the TRUSTED base policy over the TRUSTED
    # base tree, and may only ever widen.
    trusted_scope = {
        path for path in base_blobs
        if trusted_policy.resolve(path).disposition == "included"
    }
    candidate_included = {
        path for path in candidate_blobs
        if candidate_policy.resolve(path).disposition == "included"
    }
    # Still present in the candidate tree, so deletion is not what happened.
    inherited = {path for path in trusted_scope if path in candidate_blobs}
    for path in sorted(inherited - candidate_included):
        findings.append(Finding(
            "TRUSTED_SCOPE_VIOLATION", path,
            "the path is protected in the trusted base universe and still exists in the "
            "candidate tree, but the candidate policy no longer includes it; publication "
            "scope may widen, never narrow",
        ))
    # A new implementation-bearing file must enter the universe. It may be
    # legitimately out only if the TRUSTED policy's own exclusion rules cover
    # it -- the candidate cannot except itself.
    for path in sorted(set(candidate_blobs) - set(base_blobs) - candidate_included):
        if not _admission_requires_implementation(path):
            continue
        if trusted_policy.resolve(path).disposition == "excluded":
            continue
        findings.append(Finding(
            "TRUSTED_SCOPE_VIOLATION", path,
            "new implementation-bearing path is neither included by the candidate policy nor "
            "excluded by the trusted policy, so it would enter the tree unverified",
        ))

    # Everything the gate must reason about: never narrower than the trusted
    # universe, widened by whatever the candidate additionally publishes.
    protected = (inherited | candidate_included) & set(candidate_blobs)
    for path in sorted(protected - set(candidate_entries)):
        findings.append(Finding("LEDGER_COVERAGE", path, "protected path has no provenance ledger entry"))
    for path in sorted(set(candidate_entries) - protected):
        findings.append(Finding(
            "LEDGER_COVERAGE", path,
            "ledger entry does not correspond to a protected candidate path",
        ))
    included = protected

    # The export pins the digest of the ledger blob it was generated against.
    # publish_audit already checks this, but publish_audit runs in a
    # candidate-defined workflow; re-stating it here means a neutered hygiene
    # job cannot hide a ledger edited without regeneration.
    findings.extend(_export_findings(candidate_blobs, candidate_policy))

    public_record_ids = {
        record_id for record_id in
        (_claim(entry)[1] for entry in candidate_entries.values())
        if record_id is not None
    }
    debt: list[dict] = []
    changed_unattested: list[dict] = []
    unapproved_blobs: list[dict] = []
    approved_blobs: list[dict] = []

    for path in sorted(included & set(candidate_entries)):
        entry = candidate_entries[path]
        classification, record_id = _claim(entry)

        # Tier A -- content binding.
        if path not in UNHASHED_PATHS:
            actual = hashlib.sha256(candidate_blobs[path]).hexdigest()
            if entry.get("sha256") != actual:
                findings.append(Finding(
                    "CONTENT_MISMATCH", path,
                    "ledger hash does not match the candidate bytes under review",
                ))

        # Tier A -- anchor integrity.  A named record must exist and speak
        # about this path; neither is negotiable and neither is grandfathered.
        # ``_record_covers`` is the whole question: does trusted authority
        # cover this path under this id?  An id the authority does not hold
        # covers nothing, so a separate existence test would be a branch no
        # test could distinguish -- and an untestable check reads like a
        # guarantee without being one.  The property is pinned directly in
        # test_an_unknown_record_id_covers_nothing.
        if record_id is not None and not _record_covers(
            path, record_id, exact_records, record_patterns
        ):
            # One code, one wording, whether the named record is absent from
            # the trusted ledger or merely says nothing about this path.
            # Distinguishing them externally would turn the gate into an
            # oracle for which private record ids exist, so the candidate's
            # own guess is not echoed back either.
            findings.append(Finding(
                "TRUSTED_RECORD_UNRESOLVED", path,
                "the record id this entry names is not resolvable to trusted authority "
                "covering this path; the attestation is unbacked",
            ))

        # Tier B -- the grandfathering predicate.
        #
        # The authority derives exactly one claim for a path.  When the
        # candidate's claim differs from it, that difference survives only
        # while BOTH halves of the reviewed state are frozen: the claim as the
        # trusted base ledger recorded it, and the bytes as the trusted base
        # tree recorded them.  Freezing the claim alone would let a candidate
        # swap implementation content underneath an inherited attestation --
        # attestation inheritance -- so content identity is part of the tuple,
        # not a side note.
        expected_class, expected_evidence = _class_for(path, exact_records.get(path))
        # Take the record id from the evidence the generator would emit, not
        # from the raw record map: a deterministic classification carries no
        # record id even when some record happens to name the path.
        expected_record = expected_evidence.get("record_id")
        # Comparison always uses the real id; only what is *reported* is
        # redacted, so redaction can never change a verdict.
        shown_record = _disclosable(expected_record, public_record_ids)
        backing = _backing(path, exact_records, record_patterns)

        base_entry = base_entries.get(path)
        base_claim = _claim(base_entry) if base_entry is not None else None
        # A path the trusted base treated as implementation may not be
        # relabelled into a class that is not content-gated. Doing so would
        # buy exemption from exact-blob approval by editing the candidate's
        # own ledger.
        #
        # The floor is trusted *derivation*, not the base claim: when authority
        # itself derives a non-implementation class, moving the ledger onto it
        # is a correction of an over-claim, not a downgrade. Only a path
        # authority still calls implementation is protected here.
        if (
            base_entry is not None
            and base_entry.get("classification") in IMPLEMENTATION_CLASSES
            and classification not in IMPLEMENTATION_CLASSES
            and expected_class in IMPLEMENTATION_CLASSES
        ):
            findings.append(Finding(
                "CLASSIFICATION_DOWNGRADE", path,
                f"the trusted base ledger classifies this path {base_entry['classification']!r}, "
                f"which is content-gated; the candidate reclassifies it {classification!r}, "
                "which is not. Security treatment may not be reduced",
            ))
        claim = (classification, record_id)
        agrees = claim == (expected_class, expected_record) and expected_class != "unresolved"
        claim_frozen = base_claim == claim
        content_frozen = path in base_blobs and base_blobs[path] == candidate_blobs[path]

        record_finding = _exact_record_finding_for_change(
            path,
            base_blobs=base_blobs,
            candidate_blobs=candidate_blobs,
            exact_records=exact_records,
        )
        if record_finding == "TRUSTED_PATH_MISSING":
            findings.append(Finding(
                record_finding, path,
                "trusted detailed authority has no exact path-specific record for this added or changed "
                "executable/security-sensitive path",
                maintainer_action=True,
            ))
        elif record_finding == "TRUSTED_PATH_UNQUALIFIED":
            findings.append(Finding(
                record_finding, path,
                "the exact trusted record for this added or changed executable/security-sensitive path "
                "is not implementation-grade",
                maintainer_action=True,
            ))

        # -- optional exact-blob authorization ----------------------------
        #
        # Path authority is the normal policy.  The former per-revision blob
        # gate remains available only when a caller explicitly requests the
        # higher-assurance mode, and it still runs before the claim ratchet's
        # early exit.
        if (
            require_exact_blob_approvals
            and not content_frozen
            and (classification in IMPLEMENTATION_CLASSES or _admission_requires_implementation(path))
        ):
            digest = hashlib.sha256(candidate_blobs[path]).hexdigest()
            approval = approvals.get((path, digest))
            reason = (
                "new executable/security-sensitive path" if path not in base_blobs
                else "executable/security-sensitive bytes changed"
            )
            if approval is None:
                findings.append(Finding(
                    "BLOB_UNAPPROVED", path,
                    f"{reason}: the trusted authority has no reviewed-blob approval for this exact "
                    f"content (sha256 {digest}); path coverage is not content approval",
                ))
                unapproved_blobs.append({"path": path, "sha256": digest, "reason": reason})
            else:
                approved_class = public_class_for(path, approval["classification"])
                if approval["record_id"] != expected_record:
                    findings.append(Finding(
                        "BLOB_APPROVAL_RECORD_MISMATCH", path,
                        "the blob approval cites a record that is not the exact record covering this "
                        f"path; authority derives record_id={shown_record!r}",
                    ))
                elif approved_class != classification or approved_class != expected_class:
                    findings.append(Finding(
                        "BLOB_APPROVAL_CLASS_MISMATCH", path,
                        f"the blob approval authorizes classification {approved_class!r}, but the "
                        f"ledger claims {classification!r} and authority derives {expected_class!r}",
                    ))
                else:
                    approved_blobs.append({"path": path, "sha256": digest})

        if agrees:
            continue

        # The claimed record id is deliberately absent. It is attacker-authored
        # text, and echoing it into a public log alongside a resolution result
        # is the shape of an oracle for which private ids exist.
        detail_suffix = (
            f"trusted authority derives ({expected_class!r}, record_id={shown_record!r}) "
            f"but the ledger claims classification {classification!r}; "
            f"authority coverage for this path is {backing!r}"
        )
        if not claim_frozen:
            reason = "new public path" if base_claim is None else "restated provenance claim"
            findings.append(Finding("CLAIM_UNBACKED", path, f"{reason}: {detail_suffix}"))
        elif not content_frozen:
            # The claim is inherited but the bytes are not the reviewed bytes.
            findings.append(Finding(
                "CONTENT_UNATTESTED", path,
                "content changed under an inherited provenance claim that the trusted "
                f"authority does not support: {detail_suffix}",
            ))
            changed_unattested.append({"path": path, "backing": backing})
        else:
            # Tier C -- both halves frozen; report, never fail.
            debt.append({
                "path": path,
                "claimed": classification,
                "claimed_record_id": record_id,
                "trusted": expected_class,
                "trusted_record_id": shown_record,
                "backing": backing,
            })

    fatal = [finding for finding in findings if finding.fatal]
    return {
        "tool": "tools/provenance_attest_verify.py",
        "verdict": "fail" if fatal else "pass",
        "repository_scope": {
            "candidate_commit": candidate_commit,
            "candidate_tree": candidate_tree,
            "base_commit": base_commit,
            "base_tree": base_tree,
        },
        "trusted_ledger_sha256": hashlib.sha256(trusted_raw).hexdigest(),
        "authority_revision": authority_revision,
        "trusted_record_count": len(record_ids),
        "public_path_count": len(included),
        "findings": [finding.as_dict() for finding in findings],
        "fatal_count": len(fatal),
        "maintainer_action_paths": sorted(
            {finding.path for finding in findings if finding.maintainer_action and finding.fatal}
        ),
        "exact_blob_approvals_required": require_exact_blob_approvals,
        "blob_approvals_available": len(approvals),
        "blobs_approved_this_candidate": sorted(approved_blobs, key=lambda i: i["path"]),
        "blobs_unapproved": sorted(unapproved_blobs, key=lambda i: i["path"]),
        "grandfathered_debt_count": len(debt),
        "grandfathered_debt": debt,
        "changed_unattested_paths": sorted(changed_unattested, key=lambda item: item["path"]),
    }


def _print_report(verdict: dict, *, show_debt: bool) -> None:
    scope = verdict["repository_scope"]
    print("trusted provenance attestation" + (" (ephemeral controls)" if verdict.get("mode") == "ephemeral" else ""))
    print(f"  candidate commit : {scope['candidate_commit']}")
    print(f"  candidate tree   : {scope['candidate_tree']}")
    print(f"  base commit      : {scope['base_commit']}")
    # Keep the public text report to aggregate counts.  The machine-readable
    # verdict still carries the digest for local binding, but a CI summary must
    # not publish even a derived value from the private authority.
    print("  trusted ledger   : external authority (details withheld)")
    if verdict.get("mode") == "ephemeral":
        print(f"  trusted baseline : {verdict['trusted_baseline_sha256']}")
        generated = verdict.get("generated_outputs", {})
        print(f"  generated ledger : {generated.get('ledger_sha256', 'unavailable')}")
        print(f"  generated export : {generated.get('export_sha256', 'unavailable')}")
        legacy = verdict.get("legacy_controls_present", {})
        print(
            "  legacy controls  : "
            f"ledger={'present' if legacy.get('ledger') else 'absent'}, "
            f"export={'present' if legacy.get('export') else 'absent'} (untrusted compatibility data)"
        )
    if verdict.get("authority_revision"):
        print(f"  authority rev    : {verdict['authority_revision']}")
    print(
        "  content policy   : "
        + (
            "exact reviewed-blob approvals required"
            if verdict.get("exact_blob_approvals_required")
            else "trusted path authority; exact reviewed-blob approvals optional"
        )
    )
    print(f"  public paths     : {verdict['public_path_count']}")
    fatal = [item for item in verdict["findings"] if item["fatal"]]
    reported = [item for item in verdict["findings"] if not item["fatal"]]
    for item in reported:
        print(f"  NOTE  {item['code']}: {item['path']}: {item['detail']}")
    for item in fatal:
        print(f"  FAIL  {item['code']}: {item['path']}: {item['detail']}")
    approved = verdict.get("blobs_approved_this_candidate", [])
    if approved and verdict.get("exact_blob_approvals_required"):
        print(f"  exact-blob approvals matched: {len(approved)}")
        for item in approved:
            print(f"          {item['path']} sha256={item['sha256']}")
    debt = verdict["grandfathered_debt"]
    by_backing: dict[str, int] = {}
    for item in debt:
        by_backing[item["backing"]] = by_backing.get(item["backing"], 0) + 1
    summary = ", ".join(f"{count} {name}" for name, count in sorted(by_backing.items()))
    print(f"  grandfathered ledger debt: {verdict['grandfathered_debt_count']} path(s)"
          + (f" ({summary})" if summary else ""))
    print("      remedy: 'exact'/'deterministic' backing means the public entry can simply be "
          "corrected to what authority derives; 'blanket'/'none' needs a trusted record first")
    if show_debt:
        for item in debt:
            print(f"          [{item['backing']}] {item['path']}: "
                  "public claim differs; private record details withheld")
    _print_guidance(verdict)
    print(f"  verdict: {verdict['verdict'].upper()} ({verdict['fatal_count']} fatal finding(s))")


def _print_guidance(verdict: dict) -> None:
    """Say, in plain words, who has to act on the remaining findings.

    A pull request that only adds or touches a path the trusted authority has no
    exact record for fails closed by design -- the record lives outside the
    repository and no contributor can create one.  Reporting that as a bare
    "FAIL" reads as "your change is wrong", which is both untrue and the single
    biggest source of contributor friction.  The verdict is unchanged; only the
    explanation names the actor.
    """
    fatal = [item for item in verdict["findings"] if item["fatal"]]
    # The per-finding flag is authoritative; the code set is the fallback for a
    # verdict written before the flag existed, so an older verdict still gets
    # the plain-language explanation instead of a bare FAIL.
    maintainer = [
        item for item in fatal
        if item.get("maintainer_action") or item["code"] in MAINTAINER_ACTION_CODES
    ]
    if not maintainer:
        if fatal:
            print("  what you need to do: fix the finding(s) above in this pull request. "
                  "They are about your change, not about maintainer bookkeeping.")
        return
    paths = sorted({item["path"] for item in maintainer})
    if len(maintainer) == len(fatal):
        print(f"  nothing for you to do: a maintainer will admit these {len(paths)} new or "
              "unrecorded file(s). The protected provenance ledger is maintained outside this "
              "repository, so only a maintainer can add the record. Your change itself is not "
              "the problem; this check will pass once the admission lands.")
    else:
        print(f"  a maintainer will admit these {len(paths)} new or unrecorded file(s) "
              "(nothing for you to do about those), but the other finding(s) above are about "
              "your change and do need your attention.")
    for path in paths:
        print(f"          {path}")


def _print_contributor_summary(verdict: dict) -> None:
    """Print the Markdown a pull-request author reads first.

    Written for the job summary rather than for a log: a contributor needs to
    know in one sentence whether anything is wrong with *their change*.
    """
    fatal = [item for item in verdict.get("findings", []) if item.get("fatal")]
    maintainer = [
        item for item in fatal
        if item.get("maintainer_action") or item.get("code") in MAINTAINER_ACTION_CODES
    ]
    # The verdict's own list is authoritative, but a finding carries its path
    # too, and a verdict written before that field existed still names them.
    paths = sorted(
        set(verdict.get("maintainer_action_paths") or [])
        | {item.get("path", "") for item in maintainer if item.get("path")}
    )
    if not fatal:
        print("### Provenance: passed")
        print("")
        print("Every changed path is covered by the trusted provenance authority.")
        return
    if maintainer and len(maintainer) == len(fatal):
        print(f"### Provenance: a maintainer admits {len(paths)} new file(s) -- nothing for you to do")
        print("")
        print(
            "These files need an entry in the project's protected provenance ledger, which is "
            "maintained outside this repository. No contributor can add that entry, and your "
            "change is not the problem. A maintainer admits the paths and this check passes. "
            "If you did not mean to add or edit one of them, say so in the pull request."
        )
        print("")
        for path in paths:
            print(f"- `{path}`")
        return
    print("### Provenance: this pull request has findings you can fix")
    print("")
    if paths:
        print(
            f"A maintainer will admit {len(paths)} of the listed file(s), but the remaining "
            f"findings below are about your change and need your attention:"
        )
    else:
        print("The findings below are about your change:")
    print("")
    for item in fatal:
        print(f"- `{item['code']}` on `{item['path']}`: {item['detail']}")


#: Finding codes a contributor cannot clear alone.  Exposed so the reporting
#: layer and its tests agree on one list rather than restating it per caller.
MAINTAINER_ACTION_CODES = frozenset({
    "TRUSTED_PATH_MISSING",
    "TRUSTED_PATH_UNQUALIFIED",
    "TRUSTED_SCOPE_VIOLATION",
})


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Verify a candidate provenance ledger against external authority.")
    parser.add_argument("--repo", type=Path, default=Path.cwd(),
                        help="repository holding both the candidate and base objects")
    parser.add_argument("--candidate", default=None,
                        help="candidate commit-ish under verification (required unless --emit-authority-baseline)")
    parser.add_argument("--base", help="trusted base commit-ish supplying policy and prior ledger")
    parser.add_argument(
        "--explain-verdict", metavar="VERDICT_JSON", type=Path, default=None,
        help=(
            "print the pull-request-author summary for a verdict written by --json and exit; "
            "used by the hosted job summary so the plain-language explanation is the same "
            "text the report shows"
        ),
    )
    parser.add_argument("--trusted-ledger", type=Path,
                        help="external detailed implementation ledger; must be outside --repo")
    parser.add_argument(
        "--ephemeral", action="store_true",
        help=(
            "generate the candidate ledger and PUBLIC_EXPORT.json outside the repository from the trusted "
            "base baseline; any legacy candidate controls are comparison-only"
        ),
    )
    parser.add_argument(
        "--emit-authority-baseline", type=Path, default=None, metavar="OUT",
        help=(
            "write the public baseline envelope for --base, generated from --trusted-ledger alone, to OUT "
            "(outside --repo) and exit; pass it to a later run as --trusted-baseline"
        ),
    )
    parser.add_argument(
        "--trusted-baseline", type=Path, default=None,
        help=(
            "ephemeral mode: external public ledger baseline bound to --base and its policy; during the "
            "compatibility window this is an exact copy of the trusted base ledger"
        ),
    )
    parser.add_argument(
        "--trusted-candidate-policy", type=Path, default=None,
        help=(
            "ephemeral mode: external blessed candidate publication policy bytes; must be outside --repo "
            "and supplied with --policy-delta-authority"
        ),
    )
    parser.add_argument(
        "--policy-delta-authority", type=Path, default=None,
        help=(
            "ephemeral mode: external authority binding the baseline/candidate policy digests and exact "
            "allowed semantic delta; must be supplied with --trusted-candidate-policy"
        ),
    )
    parser.add_argument("--workdir", type=Path, default=None,
                        help="scratch directory for trusted inputs; must be outside --repo")
    parser.add_argument(
        "--output-dir", type=Path, default=None,
        help="ephemeral mode: directory outside --repo for generated ledger/export and input scratch",
    )
    parser.add_argument("--json", type=Path, default=None, help="write the machine-readable verdict here")
    parser.add_argument("--show-debt", action="store_true", help="list every grandfathered ledger disagreement")
    parser.add_argument(
        "--require-immutable-revisions", action="store_true",
        help="refuse anything but full 40-hex commit SHAs for --candidate and --base",
    )
    parser.add_argument(
        "--require-reviewed-blobs", action="store_true",
        help=(
            "opt into the legacy exact-digest approval gate for implementation paths; "
            "normal verification uses trusted path authority"
        ),
    )
    parser.add_argument(
        "--authority-revision", default=None,
        help="the immutable revision the trusted ledger was read at; recorded in the verdict",
    )
    args = parser.parse_args(argv)

    if args.explain_verdict is not None:
        # A standalone explanation mode: the verdict already exists, and the
        # only question is who has to act on it.  Reading a verdict file is not
        # verification, so this never prints a verdict of its own.
        try:
            verdict = json.loads(args.explain_verdict.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"trusted provenance attestation: VERDICT_UNREADABLE: {error}", file=sys.stderr)
            return 2
        _print_contributor_summary(verdict)
        return 0

    # --base and --trusted-ledger are required for every verifying mode, and not
    # for --explain-verdict, which reads an existing verdict rather than
    # producing one. Enforced here so the explanation mode stays a single
    # command with no inputs a contributor does not have.
    for name, value in (("--base", args.base), ("--trusted-ledger", args.trusted_ledger)):
        if value is None:
            parser.error(f"the following arguments are required: {name}")

    try:
        if args.emit_authority_baseline is not None:
            if args.candidate is not None or args.ephemeral:
                raise VerifyError(
                    "BASELINE_ARGUMENT_CONFLICT",
                    "--emit-authority-baseline takes only --repo, --base, --trusted-ledger and --workdir",
                )
            out = args.emit_authority_baseline
            if _path_is_within(out.parent if not out.exists() else out, args.repo):
                raise VerifyError(
                    "OUTPUT_CANDIDATE_CONTROLLED",
                    "the emitted baseline must live outside the repository under verification",
                )
            with tempfile.TemporaryDirectory(prefix="nakagawa-baseline-") as scratch:
                envelope = generate_authority_baseline(
                    repo=args.repo, base_rev=args.base, trusted_ledger=args.trusted_ledger,
                    workdir=args.workdir or Path(scratch),
                )
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(envelope)
            print(f"authority baseline for {args.base}: sha256 {hashlib.sha256(envelope).hexdigest()}")
            return 0
        if args.candidate is None:
            raise VerifyError("CANDIDATE_REQUIRED", "--candidate is required")
        if not args.ephemeral and (
            args.trusted_candidate_policy is not None or args.policy_delta_authority is not None
        ):
            raise VerifyError(
                "POLICY_DELTA_ARGUMENT_REJECTED",
                "--trusted-candidate-policy and --policy-delta-authority belong to --ephemeral mode only",
            )
        if args.ephemeral:
            if args.trusted_baseline is None:
                raise VerifyError("TRUSTED_BASELINE_REQUIRED", "--ephemeral requires --trusted-baseline")
            if args.workdir is not None and args.output_dir is not None:
                raise VerifyError("OUTPUT_ARGUMENT_CONFLICT", "--workdir and --output-dir cannot be combined in ephemeral mode")
            output_dir = args.output_dir or args.workdir
            if output_dir is None:
                with tempfile.TemporaryDirectory(prefix="nakagawa-provenance-") as temporary:
                    verdict = verify_ephemeral(
                        repo=args.repo,
                        candidate_rev=args.candidate,
                        base_rev=args.base,
                        trusted_ledger=args.trusted_ledger,
                        trusted_baseline=args.trusted_baseline,
                        output_dir=Path(temporary),
                        trusted_candidate_policy=args.trusted_candidate_policy,
                        policy_delta_authority=args.policy_delta_authority,
                        require_immutable_revisions=args.require_immutable_revisions,
                        authority_revision=args.authority_revision,
                        require_exact_blob_approvals=args.require_reviewed_blobs,
                    )
                    if args.json is not None:
                        args.json.parent.mkdir(parents=True, exist_ok=True)
                        args.json.write_text(json.dumps(verdict, indent=2) + "\n", encoding="utf-8", newline="\n")
                    _print_report(verdict, show_debt=args.show_debt)
                    return 0 if verdict["verdict"] == "pass" else 1
            verdict = verify_ephemeral(
                repo=args.repo,
                candidate_rev=args.candidate,
                base_rev=args.base,
                trusted_ledger=args.trusted_ledger,
                trusted_baseline=args.trusted_baseline,
                output_dir=output_dir,
                trusted_candidate_policy=args.trusted_candidate_policy,
                policy_delta_authority=args.policy_delta_authority,
                require_immutable_revisions=args.require_immutable_revisions,
                authority_revision=args.authority_revision,
                require_exact_blob_approvals=args.require_reviewed_blobs,
            )
        else:
            workdir = args.workdir or args.trusted_ledger.resolve().parent
            workdir.mkdir(parents=True, exist_ok=True)
            verdict = verify(
                repo=args.repo,
                candidate_rev=args.candidate,
                base_rev=args.base,
                trusted_ledger=args.trusted_ledger,
                workdir=workdir,
                require_immutable_revisions=args.require_immutable_revisions,
                authority_revision=args.authority_revision,
                require_exact_blob_approvals=args.require_reviewed_blobs,
            )
    except VerifyError as error:
        print(f"trusted provenance attestation: {error.code}: {error}", file=sys.stderr)
        return 2
    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(verdict, indent=2) + "\n", encoding="utf-8", newline="\n")
    _print_report(verdict, show_debt=args.show_debt)
    return 0 if verdict["verdict"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
