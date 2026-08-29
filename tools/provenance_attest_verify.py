#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Verify a candidate tree's public provenance ledger against external authority.

``provenance_ledger.py --check`` validates the checked-in public ledger
*structurally* and says so explicitly: it "cannot authenticate attestation
claims by itself".  ``publish_audit.py --provenance-ledger`` authenticates by
byte-matching a release-controlled public snapshot, which only exists for a
tree the release process already blessed.  Neither one can run in ordinary pull
request CI, so until this tool existed the merge path verified only that the
candidate agreed with itself.

This tool closes that gap.  It is the *verifier*, never a generator: it writes
no repository artifact and it treats every byte of the candidate tree as
untrusted input.

Trust boundary
--------------

Trusted (never candidate-controlled):

* this file, executed from the base/trusted ref, not from the candidate;
* the detailed implementation ledger, supplied as an external file that must
  live outside the candidate repository;
* the publication policy and the previous public ledger, read from the trusted
  base commit through Git rather than from the candidate's working tree.

Untrusted (data only, never executed, never a trust anchor):

* every blob in the candidate tree, including its ``.github/`` workflows, its
  copy of this file, its policy, and its public ledger.

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
    trusted policy carries.  Publication scope may tighten in a pull request;
    it may not loosen behind the gate's back.
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

Tier B2 -- exact-blob authorization.

A record authorizes a *path*.  It does not authorize arbitrary new bytes placed
at that path.  Records carry ``paths``, not digests, so path-level authority
alone would let a candidate replace every byte of an authority-backed file and
keep its attestation.

``BLOB_UNAPPROVED``
    an implementation-class path whose bytes are new or changed, with no
    ``reviewed_blobs`` approval in the trusted authority naming exactly this
    path and this SHA-256.
``BLOB_APPROVAL_RECORD_MISMATCH`` / ``BLOB_APPROVAL_CLASS_MISMATCH``
    an approval exists but cites a record that is not the exact record covering
    the path, or authorizes a different classification than the one claimed.

Only implementation classes are content-gated.  Documentation, configuration,
fixtures and public metadata are classified by what a file *is*, re-derived
every run, and need no per-revision approval.  Unchanged blobs keep whatever
authorization they already had, so introducing the rule does not require
approving the entire existing tree.

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
import hashlib
import json
import subprocess
import sys
import threading
import unicodedata
from fnmatch import fnmatchcase
from pathlib import Path, PurePosixPath

try:
    from .provenance_ledger import (
        ALLOWED_CLASSES, _class_for, is_implementation_path, validate_ledger,
    )
    from .public_export import build_document as _build_export_document
    from .publication_policy import PolicyError, load_policy
except ImportError:
    from provenance_ledger import (
        ALLOWED_CLASSES, _class_for, is_implementation_path, validate_ledger,
    )
    from public_export import build_document as _build_export_document
    from publication_policy import PolicyError, load_policy

#: Paths the ledger deliberately carries without a content hash: they are the
#: generated outputs whose own bytes depend on the ledger.
UNHASHED_PATHS = frozenset({"assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json"})

LEDGER_PATH = "assets/public_provenance_ledger.json"
POLICY_PATH = "assets/public_source_profile.json"
EXPORT_PATH = "PUBLIC_EXPORT.json"
MANIFEST_PATH = "assets/release_manifest.json"

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


class VerifyError(RuntimeError):
    """A fail-closed error in the verifier's own inputs."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


class Finding:
    """One fatal or reported observation about the candidate tree."""

    __slots__ = ("code", "path", "detail", "fatal")

    def __init__(self, code: str, path: str, detail: str, *, fatal: bool = True) -> None:
        self.code = code
        self.path = path
        self.detail = detail
        self.fatal = fatal

    def as_dict(self) -> dict:
        return {"code": self.code, "path": self.path, "detail": self.detail, "fatal": self.fatal}


def _git(repo: Path, *args: str) -> bytes:
    result = subprocess.run(["git", *args], cwd=repo, capture_output=True, check=False)
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise VerifyError("GIT_ERROR", detail or f"git {' '.join(args)} failed")
    return result.stdout


def _is_ancestor(repo: Path, ancestor: str, descendant: str) -> bool:
    result = subprocess.run(
        ["git", "merge-base", "--is-ancestor", ancestor, descendant],
        cwd=repo, capture_output=True, check=False,
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
    """Read the trusted authority's exact-blob approvals.

    Path coverage is not content approval.  A record says *what a path is*; an
    approval says *these exact bytes at this exact path were reviewed under
    that record*.  The two are deliberately separate documents in the trusted
    ledger: records are low-churn prose attestations, approvals are high-churn
    digests, and mixing them would make every content review rewrite a
    provenance statement.

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
        path = entry["path"]
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


def _policy_findings(candidate_policy, trusted_policy) -> list[Finding]:
    """Publication scope may tighten in a pull request; it may never loosen."""
    findings: list[Finding] = []
    dropped = sorted(trusted_policy.exclude_paths - candidate_policy.exclude_paths)
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
#: ``candidate_tree`` is deliberately absent: the release process writes the
#: pre-export tree there, so it is informational and the real tree binding is
#: the verdict's own ``candidate_tree``.
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
    findings: list[Finding] = []
    for field in sorted(set(canonical) | set(declared)):
        if field in EXPORT_ADVISORY_FIELDS:
            continue
        if field not in declared:
            findings.append(Finding(
                "EXPORT_FIELD_MISMATCH", EXPORT_PATH, f"export omits the required field {field!r}",
            ))
        elif field not in canonical:
            findings.append(Finding(
                "EXPORT_FIELD_MISMATCH", EXPORT_PATH,
                f"export declares {field!r}, which the canonical recomputation does not produce",
            ))
        elif declared[field] != canonical[field]:
            findings.append(Finding(
                "EXPORT_FIELD_MISMATCH", EXPORT_PATH,
                f"export field {field!r} does not match the canonical recomputation from the "
                "candidate tree and the trusted-scope policy",
            ))
    return findings


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
    approvals = load_trusted_approvals(trusted_raw)
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
        if not is_implementation_path(path):
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

        # -- exact-blob authorization ------------------------------------
        #
        # A record authorizes a *path*.  It does not authorize arbitrary new
        # bytes placed at that path.  Every implementation-bearing blob this
        # candidate introduces or changes must be named, by digest, in the
        # trusted authority.  This runs before the claim ratchet's early exit
        # precisely because a path whose claim agrees with authority is exactly
        # the case the ratchet would otherwise wave through.
        if classification in IMPLEMENTATION_CLASSES and not content_frozen:
            digest = hashlib.sha256(candidate_blobs[path]).hexdigest()
            approval = approvals.get((path, digest))
            reason = "new implementation path" if path not in base_blobs else "implementation bytes changed"
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
        "blob_approvals_available": len(approvals),
        "blobs_approved_this_candidate": sorted(approved_blobs, key=lambda i: i["path"]),
        "blobs_unapproved": sorted(unapproved_blobs, key=lambda i: i["path"]),
        "grandfathered_debt_count": len(debt),
        "grandfathered_debt": debt,
        "changed_unattested_paths": sorted(changed_unattested, key=lambda item: item["path"]),
    }


def _print_report(verdict: dict, *, show_debt: bool) -> None:
    scope = verdict["repository_scope"]
    print("trusted provenance attestation")
    print(f"  candidate commit : {scope['candidate_commit']}")
    print(f"  candidate tree   : {scope['candidate_tree']}")
    print(f"  base commit      : {scope['base_commit']}")
    print(f"  trusted ledger   : sha256={verdict['trusted_ledger_sha256']} "
          f"({verdict['trusted_record_count']} records, "
          f"{verdict['blob_approvals_available']} blob approvals)")
    if verdict.get("authority_revision"):
        print(f"  authority rev    : {verdict['authority_revision']}")
    print(f"  public paths     : {verdict['public_path_count']}")
    fatal = [item for item in verdict["findings"] if item["fatal"]]
    reported = [item for item in verdict["findings"] if not item["fatal"]]
    for item in reported:
        print(f"  NOTE  {item['code']}: {item['path']}: {item['detail']}")
    for item in fatal:
        print(f"  FAIL  {item['code']}: {item['path']}: {item['detail']}")
    approved = verdict["blobs_approved_this_candidate"]
    if approved:
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
            print(f"          [{item['backing']}] {item['path']}: claims {item['claimed']} "
                  f"(record_id={item['claimed_record_id']}), trusted derives {item['trusted']} "
                  f"(record_id={item['trusted_record_id']})")
    print(f"  verdict: {verdict['verdict'].upper()} ({verdict['fatal_count']} fatal finding(s))")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Verify a candidate provenance ledger against external authority.")
    parser.add_argument("--repo", type=Path, default=Path.cwd(),
                        help="repository holding both the candidate and base objects")
    parser.add_argument("--candidate", required=True, help="candidate commit-ish under verification")
    parser.add_argument("--base", required=True, help="trusted base commit-ish supplying policy and prior ledger")
    parser.add_argument("--trusted-ledger", type=Path, required=True,
                        help="external detailed implementation ledger; must be outside --repo")
    parser.add_argument("--workdir", type=Path, default=None,
                        help="scratch directory for trusted inputs; must be outside --repo")
    parser.add_argument("--json", type=Path, default=None, help="write the machine-readable verdict here")
    parser.add_argument("--show-debt", action="store_true", help="list every grandfathered ledger disagreement")
    parser.add_argument(
        "--require-immutable-revisions", action="store_true",
        help="refuse anything but full 40-hex commit SHAs for --candidate and --base",
    )
    parser.add_argument(
        "--authority-revision", default=None,
        help="the immutable revision the trusted ledger was read at; recorded in the verdict",
    )
    args = parser.parse_args(argv)

    workdir = args.workdir or args.trusted_ledger.resolve().parent
    try:
        workdir.mkdir(parents=True, exist_ok=True)
        verdict = verify(
            repo=args.repo,
            candidate_rev=args.candidate,
            base_rev=args.base,
            trusted_ledger=args.trusted_ledger,
            workdir=workdir,
            require_immutable_revisions=args.require_immutable_revisions,
            authority_revision=args.authority_revision,
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
