#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Build and validate the explicit public provenance ledger.

The publication policy answers *whether* a path may be considered.  This
ledger answers *what evidence supports* that consideration.  Every included
tracked path is expanded to an individual record with a content hash; a new
path therefore cannot become publishable merely by editing the policy and
refreshing ``PUBLIC_EXPORT.json``.

The ledger is evidence, not an authorization source.  Publication gates should
load a trusted copy supplied by the release process and compare the candidate's
ledger blob to it before using its records.

Classification is fail-closed:

* a path-specific record in the detailed development ledger is the only way an
  implementation-bearing path is attested (``project_authored_attested``,
  ``upstream_derived``, ``generated_from_public_source``, or ``unresolved``);
* wildcard records such as ``tools/*`` are never expanded, so adding a new tool
  cannot inherit an old blanket authorship attestation without its own record;
* documentation, configuration, public factual metadata, and explicitly
  synthetic fixtures keep narrow deterministic classifications that need no
  ledger record;
* any other path -- in particular an unrecorded implementation path under
  ``src/``, ``tools/``, or the dashboard -- resolves to ``unresolved``, and the
  generator refuses to write release evidence while any included path is
  unresolved.

``--check`` validates the checked-in ledger structurally (coverage, resolution,
content hashes) and cannot authenticate attestation claims by itself: without
the detailed development ledger it states that attestations are unverified.
Only the release flow asserts attestation -- either by regenerating the ledger
from the detailed ledger or by supplying an externally trusted copy.

The detailed development ledger may stay outside the public tree and is never
synthesized when absent.

Two mutually exclusive workflows mutate this evidence:

* ``refresh-reviewed`` refreshes the content hash of an *existing* trusted
  public path whose authority (exact record or deterministic class) is
  already established.  It refuses any path that is not already in the
  trusted tree;
* ``admit-new-reviewed`` establishes the initial trusted authority for a
  *genuinely new* exact path.  It refuses any path that is already in the
  trusted tree (that is a refresh), refuses mixed batches, and requires an
  external *admission authority* document -- never candidate-authored -- that
  independently names the exact path and the exact SHA-256 of the bytes an
  independent reviewer approved.  Candidate bytes therefore can never create
  their own authority: policy include, ledger entry, and export are all
  mechanical outputs derived from the external trusted policy, the external
  admission authority, and the external trusted ledger.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tempfile
from dataclasses import dataclass

try:
    from .public_export import build_document as _build_export_document
    from .public_export import write_document as _write_json_document
    from .publication_policy import load_policy as _load_publication_policy
except ImportError:
    from public_export import build_document as _build_export_document
    from public_export import write_document as _write_json_document
    from publication_policy import load_policy as _load_publication_policy

ROOT = Path(__file__).resolve().parent.parent
POLICY_PATH = ROOT / "assets" / "public_source_profile.json"
IMPLEMENTATION_LEDGER = ROOT / "docs" / "provenance" / "IMPLEMENTATION_PROVENANCE.json"
DEFAULT_OUTPUT = ROOT / "assets" / "public_provenance_ledger.json"

# The refresh command is deliberately narrower than the public ledger schema.
# These are the only classes that can describe implementation-bearing content;
# configuration, documentation, fixtures, and unresolved paths must use their
# own deterministic or human-review workflow.
REFRESHABLE_CLASSES = frozenset({
    "project_authored_attested",
    "upstream_derived",
    "generated_from_public_source",
})
# Existing public documentation, configuration, metadata, and synthetic test
# paths can be refreshed mechanically when their trusted baseline already
# carries the same deterministic class.  They do not become implementation
# authorization by virtue of this set; implementation paths still require an
# exact detailed-ledger record.
DETERMINISTIC_REFRESH_CLASSES = frozenset({
    "synthetic_fixture",
    "public_factual_metadata",
    "reviewed_configuration",
    "reviewed_documentation",
    "reviewed_other",
})
REFRESH_CONTROL_PATHS = frozenset({
    "assets/public_provenance_ledger.json",
    "PUBLIC_EXPORT.json",
    "assets/public_source_profile.json",
    "assets/release_manifest.json",
})

ALLOWED_CLASSES = frozenset({
    "project_authored_attested",
    "upstream_derived",
    "generated_from_public_source",
    "synthetic_fixture",
    "public_factual_metadata",
    "reviewed_configuration",
    "reviewed_documentation",
    "reviewed_other",
    "unresolved",
})


def _git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True)
    return result.stdout


def _tracked_paths() -> list[str]:
    return sorted(path for path in _git("ls-files").splitlines() if path)


def _index_blobs() -> dict[str, bytes]:
    """Read every index blob in one batch, avoiding a process per file."""
    raw = _git("ls-files", "-s", "-z")
    requests: list[tuple[str, str]] = []
    for item in raw.split("\0"):
        parts = item.split(None, 3)
        if len(parts) == 4:
            requests.append((parts[3], parts[1]))
    if not requests:
        return {}
    proc = subprocess.Popen(["git", "cat-file", "--batch"], cwd=ROOT,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    assert proc.stdin and proc.stdout
    stdout, _ = proc.communicate(("".join(f"{sha}\n" for _, sha in requests)).encode("ascii"))
    blobs: dict[str, bytes] = {}
    pos = 0
    for path, _ in requests:
        header_end = stdout.find(b"\n", pos)
        if header_end < 0:
            break
        header = stdout[pos:header_end].split()
        pos = header_end + 1
        if len(header) < 3 or header[1] != b"blob":
            continue
        size = int(header[2])
        blobs[path] = stdout[pos:pos + size]
        pos += size + 1
    return blobs


def _implementation_records(implementation_ledger: Path = IMPLEMENTATION_LEDGER) -> dict[str, dict]:
    """Map exact path -> record from the detailed development ledger.

    Fail-closed by construction: a record is honored only when the path appears
    verbatim in one of the record's ``paths`` entries.  Wildcard patterns such as
    ``tools/*`` are deliberately never expanded; a newly added tool therefore
    cannot inherit an old blanket authorship attestation without an explicit,
    path-specific record of its own.
    """
    if not implementation_ledger.is_file():
        return {}
    data = json.loads(implementation_ledger.read_text(encoding="utf-8"))
    records: dict[str, dict] = {}
    for record in data.get("records", []):
        if not isinstance(record, dict):
            continue
        for path in record.get("paths", []):
            if isinstance(path, str) and "*" not in path:
                records[path] = record
    return records


#: Deterministic non-implementation rules.  These describe what a file *is* by
#: its path shape (documentation, configuration, synthetic fixture, public
#: metadata) and need no path-specific ledger record.  They are deliberately
#: narrow: a path that matches none of them is implementation-bearing or
#: otherwise substantive, and that case requires explicit evidence.
DOCUMENTATION_SUFFIXES = (".md", ".txt")
DOCUMENTATION_NAMES = frozenset({"README.md", "NOTICE.md", "LICENSE"})
CONFIGURATION_SUFFIXES = (".json", ".jsonc", ".yaml", ".yml", ".toml", ".lock", ".prisma")
CONFIGURATION_PREFIXES = (".github/", "mk/")
CONFIGURATION_NAMES = frozenset({
    ".clang-format", ".clangd", ".editorconfig", ".gitattributes", ".gitignore", "Makefile",
})

#: Suffixes whose content is project implementation rather than documentation,
#: configuration, or synthetic fixtures.  Used to name the fail-closed outcome
#: precisely; the classification decision itself simply defaults to
#: ``unresolved`` for anything the deterministic rules do not cover.
IMPLEMENTATION_SUFFIXES = frozenset({
    ".c", ".h", ".cpp", ".hpp", ".cc", ".cxx", ".py", ".ps1", ".sh",
    ".ts", ".tsx", ".mjs", ".mts", ".js", ".css",
})


def is_implementation_path(path: str) -> bool:
    """True when the path's content is project implementation or tooling.

    ``src/`` and ``tools/`` are implementation by location; other paths count
    when they carry a source/script suffix.  ``mk/`` and root ``Makefile`` are
    build configuration and are deliberately not counted here.
    """
    if path.startswith(("src/", "tools/")):
        return True
    return PurePosixPath(path).suffix.lower() in IMPLEMENTATION_SUFFIXES


def _is_configuration_path(path: str) -> bool:
    """Narrow deterministic configuration rule.

    Matches CI/template prefixes (``.github/``), build fragments (``mk/``),
    configuration extensions, well-known configuration filenames, dotfiles, and
    ``*.config.*`` files.  ``interface/`` as a whole is deliberately *not*
    configuration: the dashboard's ``src/`` is implementation and needs a
    ledger record, only its config-shaped files match this rule.
    """
    if path.startswith(CONFIGURATION_PREFIXES):
        return True
    if path.endswith(CONFIGURATION_SUFFIXES):
        return True
    name = PurePosixPath(path).name
    if name in CONFIGURATION_NAMES:
        return True
    if name.startswith("."):
        return True
    return ".config." in name and name.count(".") >= 2


MISSING_RECORD_EVIDENCE = {
    "source": "missing path-specific provenance record",
    "statement": (
        "no path-specific record exists in the detailed implementation ledger; "
        "independent authorship or derivation cannot be attested without one"
    ),
}


def _class_for(path: str, record: dict | None) -> tuple[str, dict]:
    if path == "font/README.md":
        return "reviewed_documentation", {
            "source": "public documentation review",
            "statement": "generic user-supplied optional font instructions; unresolved font binaries and license packet are excluded",
        }
    if record:
        classification = record.get("classification")
        evidence = {
            "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
            "record_id": record.get("id"),
            "evidence_tier": record.get("evidence_tier"),
        }
        if classification in ("derived-translated", "upstream-third-party"):
            upstream = record.get("upstream") or "documented upstream family"
            return "upstream_derived", {**evidence, "upstream": upstream,
                                         "upstream_paths": record.get("upstream_paths", []),
                                         "upstream_revision": record.get("upstream_revision"),
                                         "license": record.get("upstream_license") or "see NOTICE.md",
                                         "modification_status": "modified_or_translated; see record"}
        if classification == "derived-data":
            return "generated_from_public_source", {**evidence, "generator": "documented public-source data path",
                                                       "source_family": record.get("upstream") or "public PSP data"}
        if classification == "unresolved":
            return "unresolved", {**evidence, "reason": "implementation ledger marks provenance unresolved"}
        if classification in ("behavior-informed", "project-authored-independent"):
            return "project_authored_attested", {**evidence, "authorship": "independent implementation record",
                                                   "upstream_attribution": record.get("upstream")}

    if path.startswith(("fixtures/",)) or "/test_" in path or path.startswith("tools/test_"):
        return "synthetic_fixture", {"source": "path-reviewed fixture/test census",
                                      "statement": "fixture or test data is synthetic and contains no retail bytes"}
    if path.startswith("docs/") or path.endswith(DOCUMENTATION_SUFFIXES) or path in DOCUMENTATION_NAMES:
        return "reviewed_documentation", {"source": "public documentation review", "statement": "generic/public documentation; no private operational evidence"}
    if path.startswith("assets/titles/"):
        return "public_factual_metadata", {"source": "title-manifest schema and public PSP metadata review",
                                            "statement": "manifest contains user-supplied title metadata, not retail content"}
    if _is_configuration_path(path):
        return "reviewed_configuration", {"source": "configuration review", "statement": "configuration or dependency metadata reviewed for public release"}
    # Fail closed: no specific record and no deterministic non-implementation
    # rule.  An implementation-bearing path must never receive a blanket
    # ``project_authored_attested`` attestation merely because it is unrecorded.
    return "unresolved", dict(MISSING_RECORD_EVIDENCE)


def build_ledger(
    output: Path = DEFAULT_OUTPUT,
    *,
    implementation_ledger: Path = IMPLEMENTATION_LEDGER,
) -> dict:
    if not implementation_ledger.is_file():
        raise RuntimeError(
            "detailed development provenance ledger is not present; the checked-in public ledger "
            "is release evidence and must not be regenerated from broad defaults"
        )
    policy = json.loads(POLICY_PATH.read_text(encoding="utf-8"))
    included = set(policy.get("include_paths", []))
    tracked = _tracked_paths()
    records = _implementation_records(implementation_ledger)
    blobs = _index_blobs()
    entries: list[dict] = []
    for path in tracked:
        if path not in included:
            continue
        classification, evidence = _class_for(path, records.get(path))
        entry = {
            "path": path,
            "classification": classification,
            "evidence": evidence,
        }
        if path not in ("assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json"):
            entry["sha256"] = hashlib.sha256(blobs.get(path, (ROOT / path).read_bytes())).hexdigest()
        entries.append(entry)
    unresolved = sorted(e["path"] for e in entries if e["classification"] == "unresolved")
    if unresolved:
        shown = ", ".join(unresolved[:10])
        if len(unresolved) > 10:
            shown += f" ... and {len(unresolved) - 10} more"
        raise RuntimeError(
            "refusing to generate public provenance evidence while included path(s) have "
            "no path-specific provenance record ("
            f"{len(unresolved)} unresolved): {shown}"
        )
    document = {
        "schema_version": 1,
        "generated_by": "tools/provenance_ledger.py",
        "policy_profile": policy.get("name"),
        "classification_vocabulary": sorted(ALLOWED_CLASSES),
        "entries": entries,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    _write_json_document(output, document)
    return document


def validate_ledger(document: dict, *, require_hashes: bool = True, require_resolved: bool = False) -> list[str]:
    errors: list[str] = []
    entries = document.get("entries")
    if not isinstance(entries, list):
        return ["ledger entries must be a list"]
    seen: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
            errors.append("ledger contains malformed entry")
            continue
        path = entry["path"]
        if path in seen:
            errors.append(f"duplicate ledger path: {path}")
        seen.add(path)
        if entry.get("classification") not in ALLOWED_CLASSES:
            errors.append(f"unsupported provenance class for {path}")
        if require_resolved and entry.get("classification") == "unresolved":
            errors.append(f"unresolved provenance is not release evidence: {path}")
        if not isinstance(entry.get("evidence"), dict) or not entry["evidence"]:
            errors.append(f"missing explicit provenance evidence for {path}")
        if require_hashes and path not in ("assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json"):
            digest = entry.get("sha256")
            if not isinstance(digest, str) or len(digest) != 64:
                errors.append(f"missing content hash for {path}")
    return errors


class RefreshError(RuntimeError):
    """A fail-closed error raised by the trusted refresh workflow."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class _TreeSnapshot:
    """The immutable Git-tree view used by a refresh operation."""

    repo_root: Path
    selector: str
    tree_sha: str
    blobs: dict[str, bytes]
    worktree_root: Path | None = None


def _canonical_json_bytes(document: dict) -> bytes:
    return (json.dumps(document, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def _git_at(repo_root: Path, *args: str) -> bytes:
    result = subprocess.run(
        ["git", *args], cwd=repo_root, capture_output=True, check=False,
    )
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise RefreshError("GIT_ERROR", detail or f"git {' '.join(args)} failed")
    return result.stdout


def _tree_blobs(repo_root: Path, tree_sha: str) -> dict[str, bytes]:
    """Read a complete immutable tree using Git's object database."""

    raw = _git_at(repo_root, "ls-tree", "-r", "-z", "--full-tree", tree_sha)
    requests: list[tuple[str, str]] = []
    seen_paths: set[str] = set()
    for item in raw.split(b"\0"):
        if not item:
            continue
        try:
            metadata, raw_path = item.split(b"\t", 1)
            _mode, kind, object_id = metadata.split()
        except ValueError as error:
            raise RefreshError("TRUSTED_TREE_INVALID", "Git tree contains a malformed entry") from error
        if kind != b"blob":
            raise RefreshError("TRUSTED_TREE_INVALID", "refresh supports regular Git blobs only")
        try:
            path = raw_path.decode("utf-8")
        except UnicodeDecodeError as error:
            raise RefreshError("TRUSTED_TREE_INVALID", "Git tree contains a non-UTF-8 path") from error
        if not path or path in seen_paths:
            raise RefreshError("TRUSTED_TREE_INVALID", "Git tree contains a duplicate or empty path")
        seen_paths.add(path)
        requests.append((path, object_id.decode("ascii")))

    if not requests:
        return {}

    proc = subprocess.Popen(
        ["git", "cat-file", "--batch"], cwd=repo_root,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert proc.stdin and proc.stdout
    stdout, stderr = proc.communicate(("".join(f"{oid}\n" for _, oid in requests)).encode("ascii"))
    if proc.returncode:
        detail = stderr.decode("utf-8", errors="replace").strip()
        raise RefreshError("GIT_ERROR", detail or "git cat-file --batch failed")

    blobs: dict[str, bytes] = {}
    position = 0
    for path, _ in requests:
        header_end = stdout.find(b"\n", position)
        if header_end < 0:
            raise RefreshError("TRUSTED_TREE_INVALID", "Git returned a truncated blob response")
        header = stdout[position:header_end].split()
        position = header_end + 1
        if len(header) < 3 or header[1] != b"blob":
            raise RefreshError("TRUSTED_TREE_INVALID", "Git tree contains a missing or non-blob object")
        try:
            size = int(header[2])
        except ValueError as error:
            raise RefreshError("TRUSTED_TREE_INVALID", "Git returned a malformed blob size") from error
        content = stdout[position:position + size]
        if len(content) != size:
            raise RefreshError("TRUSTED_TREE_INVALID", "Git returned a truncated blob")
        blobs[path] = content
        position += size + 1  # the batch protocol terminates each blob with LF
    return blobs


def _path_is_within(path: Path, root: Path, *, resolve: bool) -> bool:
    try:
        candidate = path.resolve() if resolve else path.absolute()
        base = root.resolve() if resolve else root.absolute()
        candidate.relative_to(base)
        return True
    except (OSError, ValueError):
        return False


def _resolve_tree_selector(
    selector: str,
    *,
    default_repo: Path,
    role: str,
) -> _TreeSnapshot:
    """Resolve a clean worktree or Git tree-ish to exact blob bytes."""

    selector_path = Path(selector)
    worktree_root: Path | None = None
    repo_root = default_repo.resolve()
    git_selector = selector
    if selector_path.exists():
        if not selector_path.is_dir():
            raise RefreshError(f"{role.upper()}_TREE_INVALID", f"{role} tree selector is not a directory")
        try:
            repo_root = Path(
                _git_at(selector_path, "rev-parse", "--show-toplevel")
                .decode("utf-8")
                .strip()
            ).resolve()
        except (UnicodeDecodeError, OSError) as error:
            raise RefreshError(f"{role.upper()}_TREE_INVALID", f"cannot resolve {role} worktree") from error
        status = _git_at(repo_root, "status", "--porcelain=v1", "--untracked-files=all").decode(
            "utf-8", errors="replace"
        ).strip()
        if status:
            raise RefreshError(
                f"{role.upper()}_TREE_DIRTY",
                f"{role} worktree must be clean before its tree can be trusted",
            )
        worktree_root = repo_root
        git_selector = "HEAD"

    try:
        tree_sha = _git_at(repo_root, "rev-parse", f"{git_selector}^{{tree}}").decode("ascii").strip()
    except (UnicodeDecodeError, RefreshError) as error:
        if isinstance(error, RefreshError):
            raise RefreshError(
                f"{role.upper()}_TREE_INVALID", f"cannot resolve {role} tree selector"
            ) from error
        raise RefreshError(f"{role.upper()}_TREE_INVALID", f"cannot resolve {role} tree selector") from error
    if len(tree_sha) != 40:
        raise RefreshError(f"{role.upper()}_TREE_INVALID", f"{role} selector did not resolve to a Git tree")
    return _TreeSnapshot(
        repo_root=repo_root,
        selector=selector,
        tree_sha=tree_sha,
        blobs=_tree_blobs(repo_root, tree_sha),
        worktree_root=worktree_root,
    )


def _read_json_file(path: Path, *, code: str) -> dict:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RefreshError(code, "trusted input is missing, unreadable, or invalid JSON") from error
    if not isinstance(document, dict):
        raise RefreshError(code, "trusted input must be a JSON object")
    return document


def _exact_path(path: object, *, code: str) -> str:
    if not isinstance(path, str) or not path:
        raise RefreshError(code, "authorization must name a non-empty exact path")
    if "\\" in path or path.startswith("/") or PurePosixPath(path).is_absolute():
        raise RefreshError(code, f"path is not a repository-relative POSIX path: {path!r}")
    # Brackets are valid literal path characters (for example a Next.js
    # dynamic route directory named ``[id]``).  Only glob operators are
    # treated as wildcard authorization here; bracketed paths are still
    # required to match an exact path in the trusted tree.
    if any(character in path for character in "*?"):
        raise RefreshError(code, f"wildcard authorization is forbidden: {path!r}")
    pure = PurePosixPath(path)
    if pure.as_posix() != path or any(part in ("", ".", "..") for part in pure.parts):
        raise RefreshError(code, f"path is not an exact file path: {path!r}")
    return path


def _external_input(path: Path, *, candidate_root: Path, label: str) -> Path:
    """Require a trusted input to be outside both lexical and resolved candidate paths."""

    if not path.is_file():
        raise RefreshError("TRUSTED_INPUT_MISSING", f"{label} is unavailable")
    if _path_is_within(path, candidate_root, resolve=False) or _path_is_within(path, candidate_root, resolve=True):
        raise RefreshError("TRUSTED_INPUT_CANDIDATE_CONTROLLED", f"{label} is inside the candidate tree")
    return path.resolve()


def _ledger_entry_map(document: dict, *, label: str) -> dict[str, dict]:
    entries = document.get("entries")
    if not isinstance(entries, list):
        raise RefreshError("TRUSTED_LEDGER_INVALID", f"{label} does not contain public ledger entries")
    result: dict[str, dict] = {}
    for entry in entries:
        if not isinstance(entry, dict):
            raise RefreshError("TRUSTED_LEDGER_INVALID", f"{label} contains a malformed entry")
        path = _exact_path(entry.get("path"), code="TRUSTED_LEDGER_INVALID")
        if path in result:
            raise RefreshError("TRUSTED_LEDGER_INVALID", f"{label} contains duplicate path {path}")
        result[path] = entry
    return result


def _validate_public_snapshot(
    document: dict,
    *,
    tree: _TreeSnapshot,
    policy,
    label: str,
) -> dict[str, dict]:
    errors = validate_ledger(document, require_hashes=True, require_resolved=True)
    if errors:
        raise RefreshError("TRUSTED_LEDGER_INVALID", f"{label}: {errors[0]}")
    if document.get("policy_profile") not in (None, policy.name):
        raise RefreshError("TRUSTED_LEDGER_POLICY_MISMATCH", f"{label} names a different policy profile")
    # ``refresh`` is audit ancestry, not an identity claim about the tree that
    # carries this document.  ``refresh-reviewed`` records the trusted tree it
    # read and the candidate tree it read *before* writing the regenerated
    # ledger and export, so a shipped ledger's recorded trees can never equal
    # the tree that then contains those regenerated bytes.  Requiring equality
    # here made every generated snapshot permanently unusable as the next
    # baseline while adding no authority: a snapshot is bound to a tree below,
    # by requiring every non-control entry hash to equal that tree's blob.
    # Content is the anchor; the recorded tree ids stay informational.
    entries = _ledger_entry_map(document, label=label)
    tree_paths = set(tree.blobs)
    included = {
        path for path in tree_paths if policy.resolve(path).disposition == "included"
    }
    for path in sorted(tree_paths - included):
        raise RefreshError(
            "TRUSTED_TREE_PUBLIC_BOUNDARY",
            f"trusted tree contains a path outside the explicitly included public scope: {path}",
        )
    if set(entries) != included:
        missing = sorted(included - set(entries))
        extra = sorted(set(entries) - included)
        detail = f"missing {missing[0]}" if missing else f"unexpected {extra[0]}"
        raise RefreshError("TRUSTED_LEDGER_COVERAGE", f"{label} does not exactly cover the trusted public tree ({detail})")

    for path, entry in entries.items():
        if path in ("assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json"):
            continue
        expected = hashlib.sha256(tree.blobs[path]).hexdigest()
        if entry.get("sha256") != expected:
            raise RefreshError(
                "TRUSTED_LEDGER_TREE_MISMATCH",
                f"{label} hash does not match the trusted tree for {path}",
            )
    return entries


def _detailed_records(document: dict) -> dict[str, dict]:
    records = document.get("records")
    if not isinstance(records, list):
        raise RefreshError("TRUSTED_LEDGER_INVALID", "trusted detailed ledger does not contain records")
    result: dict[str, dict] = {}
    for record in records:
        if not isinstance(record, dict):
            raise RefreshError("TRUSTED_LEDGER_INVALID", "trusted detailed ledger contains a malformed record")
        paths = record.get("paths")
        if not isinstance(paths, list):
            raise RefreshError("TRUSTED_LEDGER_INVALID", "trusted detailed ledger record has no paths list")
        if not isinstance(record.get("id"), str) or not record["id"]:
            raise RefreshError("TRUSTED_LEDGER_INVALID", "trusted detailed ledger record has no stable id")
        for raw_path in paths:
            if not isinstance(raw_path, str):
                raise RefreshError("TRUSTED_LEDGER_INVALID", "trusted detailed ledger contains a non-string path")
            # Patterns remain deliberately inert.  They can never authorize a
            # refresh; an exact path must be present in its own record.
            if any(character in raw_path for character in "*?"):
                continue
            path = _exact_path(raw_path, code="TRUSTED_LEDGER_INVALID")
            if path in result and result[path] != record:
                raise RefreshError("TRUSTED_LEDGER_INVALID", f"duplicate exact detailed record for {path}")
            result[path] = record
    return result


def _refresh_class_for(path: str, record: dict | None) -> tuple[str, dict]:
    classification, _ = _class_for(path, record)
    if not record:
        return classification, {"source": "trusted detailed implementation ledger"}
    # Do not copy arbitrary detailed-ledger fields into public evidence.  In
    # particular, upstream paths and operational notes may be private.  The
    # exact record id is retained because it is the machine-comparable trust
    # anchor, while descriptive values remain deliberately generic.
    evidence = {
        "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
        "record_id": record["id"],
        "evidence_tier": record.get("evidence_tier"),
    }
    if classification == "project_authored_attested":
        evidence.update({"authorship": "independent implementation record", "upstream_attribution": None})
    elif classification == "upstream_derived":
        evidence.update({
            "upstream": "documented upstream family",
            "license": "see NOTICE.md",
            "modification_status": "modified_or_translated; see trusted record",
        })
    elif classification == "generated_from_public_source":
        evidence.update({
            "generator": "documented public-source data path",
            "source_family": "public source data",
        })
    return classification, evidence


def _refresh_class_allowed(path: str, classification: object) -> bool:
    """Return whether a trusted baseline class may be hash-refreshed.

    Implementation classes are allowed only through the trusted detailed
    ledger.  Deterministic classes are allowed only when the path itself still
    resolves to that same deterministic class; a candidate cannot relabel an
    implementation path as configuration or documentation to bypass the
    provenance gate.
    """
    if classification in REFRESHABLE_CLASSES:
        return True
    return (
        classification in DETERMINISTIC_REFRESH_CLASSES
        and _class_for(path, None)[0] == classification
    )


def _ledger_from_detailed(
    *,
    tree: _TreeSnapshot,
    policy,
    records: dict[str, dict],
) -> dict:
    entries: list[dict] = []
    included = sorted(path for path in tree.blobs if policy.resolve(path).disposition == "included")
    for path in included:
        classification, evidence = _refresh_class_for(path, records.get(path))
        if classification == "unresolved":
            raise RefreshError(
                "TRUSTED_PATH_UNQUALIFIED",
                f"trusted detailed ledger has no exact qualifying record for {path}",
            )
        entry = {"path": path, "classification": classification, "evidence": evidence}
        if path not in ("assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json"):
            entry["sha256"] = hashlib.sha256(tree.blobs[path]).hexdigest()
        entries.append(entry)
    document = {
        "schema_version": 1,
        "generated_by": "tools/provenance_ledger.py",
        "policy_profile": policy.name,
        "classification_vocabulary": sorted(ALLOWED_CLASSES),
        "entries": entries,
    }
    errors = validate_ledger(document, require_hashes=True, require_resolved=True)
    if errors:
        raise RefreshError("TRUSTED_LEDGER_INVALID", errors[0])
    return document


def _public_scope(tree: _TreeSnapshot, policy) -> set[str]:
    return {path for path in tree.blobs if policy.resolve(path).disposition == "included"}


def _validate_candidate_against_trusted(
    *,
    candidate: _TreeSnapshot,
    trusted: _TreeSnapshot,
    policy,
    refreshed_paths: set[str],
) -> None:
    if trusted.blobs.get("assets/public_source_profile.json") is None:
        raise RefreshError("TRUSTED_TREE_PUBLIC_BOUNDARY", "trusted tree has no canonical publication policy")
    policy_bytes = trusted.blobs["assets/public_source_profile.json"]
    if candidate.blobs.get("assets/public_source_profile.json") != policy_bytes:
        raise RefreshError("CANDIDATE_POLICY_MISMATCH", "candidate policy differs from the externally trusted policy")

    candidate_paths = set(candidate.blobs)
    trusted_paths = set(trusted.blobs)
    outside_scope = sorted(
        path for path in trusted_paths if policy.resolve(path).disposition != "included"
    )
    if outside_scope:
        raise RefreshError(
            "TRUSTED_TREE_PUBLIC_BOUNDARY",
            f"trusted tree contains a path outside the explicitly included public scope: {outside_scope[0]}",
        )
    added = sorted(candidate_paths - trusted_paths)
    removed = sorted(trusted_paths - candidate_paths)
    if added:
        public_added = [path for path in added if policy.resolve(path).disposition == "included"]
        implementation_added = [path for path in added if is_implementation_path(path)]
        if implementation_added:
            raise RefreshError(
                "NEW_PATH_REFUSED",
                f"candidate adds implementation path {implementation_added[0]}; "
                "admit-new-reviewed is the route for a genuinely new exact path",
            )
        raise RefreshError(
            "CANDIDATE_PUBLIC_BOUNDARY",
            f"candidate adds a path outside the trusted tree: {added[0]}; "
            "admit-new-reviewed is the route for a genuinely new exact path",
        )
    if removed:
        raise RefreshError("CANDIDATE_TREE_SCOPE_CHANGED", f"candidate removes a path from the trusted tree: {removed[0]}")

    if _public_scope(candidate, policy) != _public_scope(trusted, policy):
        raise RefreshError("CANDIDATE_PUBLIC_BOUNDARY", "candidate public scope differs from the trusted tree")

    for path in sorted(candidate_paths - refreshed_paths):
        if path in ("assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json"):
            # These are the generated outputs of this operation.  Their
            # candidate bytes are intentionally ignored and replaced from the
            # external authority below; they are never trusted as inputs.
            continue
        if candidate.blobs[path] != trusted.blobs[path]:
            raise RefreshError(
                "CANDIDATE_TREE_STALE",
                f"candidate changed unrequested path {path}; refresh paths must be explicit",
            )


def _refresh_document(
    *,
    trusted_document: dict,
    candidate: _TreeSnapshot,
    trusted: _TreeSnapshot,
    policy,
    refreshed_paths: list[str],
    detailed_records: dict[str, dict] | None,
) -> dict:
    if detailed_records is not None:
        baseline_entries = _ledger_entry_map(trusted_document, label="trusted detailed baseline") if "entries" in trusted_document else None
        if baseline_entries is None:
            document = _ledger_from_detailed(tree=trusted, policy=policy, records=detailed_records)
        else:
            document = trusted_document
            for path in refreshed_paths:
                classification, _ = _refresh_class_for(path, detailed_records.get(path))
                if classification != baseline_entries[path].get("classification"):
                    raise RefreshError("TRUSTED_PATH_UNQUALIFIED", f"detailed record class disagrees for {path}")
    else:
        document = trusted_document

    entries = _ledger_entry_map(document, label="trusted ledger")
    for path in refreshed_paths:
        entry = entries.get(path)
        if entry is None:
            raise RefreshError("TRUSTED_PATH_MISSING", f"trusted ledger has no exact entry for {path}")
        if not _refresh_class_allowed(path, entry.get("classification")):
            raise RefreshError(
                "TRUSTED_PATH_UNQUALIFIED",
                f"{path} has non-implementation provenance class {entry.get('classification')!r}",
            )
        # An implementation class in a public snapshot is only as good as the
        # exact detailed record behind it.  Historical snapshots still carry
        # entries minted by removed fail-open rules -- the ``tools/*`` wildcard
        # expansion and the ``interface/`` configuration prefix -- so a snapshot
        # alone must never re-attest *new* bytes on an implementation path.
        # The detailed ledger is required for that, and wildcards stay inert in
        # it, so a wildcard-derived entry cannot be carried onto new content.
        if entry.get("classification") in REFRESHABLE_CLASSES:
            if detailed_records is None:
                raise RefreshError(
                    "TRUSTED_RECORD_REQUIRED",
                    f"implementation-class refresh requires the trusted detailed ledger: {path}",
                )
            if path not in detailed_records:
                raise RefreshError(
                    "TRUSTED_PATH_MISSING",
                    f"trusted detailed ledger has no exact record for {path}",
                )
        if detailed_records is not None and path not in detailed_records and _class_for(path, None)[0] == "unresolved":
            raise RefreshError("TRUSTED_PATH_MISSING", f"trusted detailed ledger has no exact record for {path}")

    output = json.loads(json.dumps(document, ensure_ascii=False))
    output_entries = {entry["path"]: entry for entry in output["entries"]}
    for path in refreshed_paths:
        if detailed_records is not None and path in detailed_records:
            classification, evidence = _refresh_class_for(path, detailed_records[path])
            output_entries[path]["classification"] = classification
            output_entries[path]["evidence"] = evidence
        output_entries[path]["sha256"] = hashlib.sha256(candidate.blobs[path]).hexdigest()
    output["refresh"] = {
        "workflow": "refresh-reviewed",
        "trusted_tree": trusted.tree_sha,
        "candidate_tree": candidate.tree_sha,
        "refreshed_paths": refreshed_paths,
    }
    errors = validate_ledger(output, require_hashes=True, require_resolved=True)
    if errors:
        raise RefreshError("REFRESH_OUTPUT_INVALID", errors[0])
    for path, entry in output_entries.items():
        if path in ("assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json"):
            continue
        expected = hashlib.sha256(candidate.blobs[path]).hexdigest()
        if entry.get("sha256") != expected:
            raise RefreshError("REFRESH_OUTPUT_INVALID", f"output hash does not match candidate bytes for {path}")
    return output


def _refresh_export_bytes(
    *,
    candidate: _TreeSnapshot,
    policy,
    ledger_bytes: bytes,
) -> bytes:
    manifest = candidate.blobs.get("assets/release_manifest.json")
    if manifest is None:
        raise RefreshError("CANDIDATE_MANIFEST_MISSING", "candidate tree has no release manifest")
    if "assets/public_provenance_ledger.json" not in candidate.blobs or "PUBLIC_EXPORT.json" not in candidate.blobs:
        raise RefreshError("CANDIDATE_CONTROL_MISSING", "candidate tree has no public ledger or export control file")
    files = []
    for path, raw in sorted(candidate.blobs.items()):
        if path == "assets/public_provenance_ledger.json":
            raw = ledger_bytes
        elif path == "PUBLIC_EXPORT.json":
            raw = b""
        files.append((path, raw))
    document = _build_export_document(
        policy,
        files,
        candidate_tree=candidate.tree_sha,
        provenance_ledger=ledger_bytes,
        manifest=manifest,
    )
    return _canonical_json_bytes(document)


def refresh_reviewed(
    *,
    trusted_ledger: Path,
    candidate_tree: str,
    trusted_tree: str,
    paths: list[str],
    output: Path | None = None,
    export_output: Path | None = None,
    trusted_policy: Path,
    trusted_manifest: Path | None = None,
    trusted_baseline_ledger: Path | None = None,
) -> dict:
    """Refresh explicit hashes using only external trusted inputs.

    ``trusted_ledger`` may be a release-controlled public ledger snapshot, or an
    external detailed development ledger containing exact ``records``.  The
    latter is converted from the immutable trusted tree; an optional external
    baseline snapshot preserves its existing public entry objects.  No trusted
    input may be read from the candidate tree.
    """

    normalized_paths = sorted({_exact_path(path, code="REFRESH_PATH_NOT_EXACT") for path in paths})
    if not normalized_paths:
        raise RefreshError("REFRESH_PATH_REQUIRED", "at least one exact refresh path is required")
    for path in normalized_paths:
        if path in REFRESH_CONTROL_PATHS:
            raise RefreshError("REFRESH_PATH_FORBIDDEN", f"generated/control path cannot be refreshed: {path}")
        if not is_implementation_path(path) and _class_for(path, None)[0] not in DETERMINISTIC_REFRESH_CLASSES:
            raise RefreshError(
                "REFRESH_PATH_NOT_REFRESHABLE",
                f"refresh-reviewed requires an implementation record or deterministic public path: {path}",
            )

    candidate = _resolve_tree_selector(candidate_tree, default_repo=ROOT, role="candidate")
    controlled_root = (candidate.worktree_root or candidate.repo_root).resolve()
    trusted = _resolve_tree_selector(trusted_tree, default_repo=candidate.repo_root, role="trusted")
    if trusted.worktree_root is not None and (
        _path_is_within(trusted.worktree_root, controlled_root, resolve=False)
        or _path_is_within(trusted.worktree_root, controlled_root, resolve=True)
    ):
        raise RefreshError(
            "TRUSTED_TREE_CANDIDATE_CONTROLLED",
            "trusted tree worktree is inside the candidate tree",
        )

    trusted_ledger_path = _external_input(trusted_ledger, candidate_root=controlled_root, label="trusted ledger")
    trusted_policy_path = _external_input(trusted_policy, candidate_root=controlled_root, label="trusted policy")
    trusted_baseline_path = None
    if trusted_baseline_ledger is not None:
        trusted_baseline_path = _external_input(
            trusted_baseline_ledger, candidate_root=controlled_root, label="trusted baseline ledger"
        )
    trusted_manifest_path = None
    if trusted_manifest is not None:
        trusted_manifest_path = _external_input(
            trusted_manifest, candidate_root=controlled_root, label="trusted manifest"
        )

    try:
        policy = _load_publication_policy(trusted_policy_path)
    except Exception as error:
        raise RefreshError("TRUSTED_POLICY_INVALID", "trusted policy is invalid") from error
    policy_raw = trusted_policy_path.read_bytes()
    if trusted.blobs.get("assets/public_source_profile.json") != policy_raw:
        raise RefreshError("TRUSTED_POLICY_MISMATCH", "trusted tree does not contain the trusted policy bytes")
    _validate_candidate_against_trusted(
        candidate=candidate,
        trusted=trusted,
        policy=policy,
        refreshed_paths=set(normalized_paths),
    )
    for path in normalized_paths:
        if path not in trusted.blobs:
            raise RefreshError(
                "NEW_PATH_REFUSED",
                f"refresh path is not present in the trusted tree: {path}; "
                "admit-new-reviewed is the route for a genuinely new exact path",
            )
        if path not in candidate.blobs:
            raise RefreshError("CANDIDATE_PATH_MISSING", f"refresh path is not present in the candidate tree: {path}")
        if policy.resolve(path).disposition != "included":
            raise RefreshError("REFRESH_PATH_NOT_PUBLIC", f"refresh path is not explicitly public: {path}")

    trusted_document = _read_json_file(trusted_ledger_path, code="TRUSTED_LEDGER_INVALID")
    detailed_records: dict[str, dict] | None = None
    if "entries" in trusted_document and "records" in trusted_document:
        raise RefreshError("TRUSTED_LEDGER_INVALID", "trusted ledger cannot mix public entries and detailed records")
    if "entries" in trusted_document:
        _validate_public_snapshot(
            trusted_document, tree=trusted, policy=policy, label="trusted ledger"
        )
    elif "records" in trusted_document:
        detailed_records = _detailed_records(trusted_document)
        for path in normalized_paths:
            if path not in detailed_records and _class_for(path, None)[0] == "unresolved":
                raise RefreshError("TRUSTED_PATH_MISSING", f"trusted detailed ledger has no exact record for {path}")
        if trusted_baseline_path is not None:
            trusted_document = _read_json_file(trusted_baseline_path, code="TRUSTED_LEDGER_INVALID")
            _validate_public_snapshot(
                trusted_document, tree=trusted, policy=policy, label="trusted baseline ledger"
            )
    else:
        raise RefreshError("TRUSTED_LEDGER_INVALID", "trusted ledger must contain entries or detailed records")

    if trusted_manifest_path is not None:
        candidate_manifest = candidate.blobs.get("assets/release_manifest.json")
        trusted_tree_manifest = trusted.blobs.get("assets/release_manifest.json")
        trusted_manifest = trusted_manifest_path.read_bytes()
        if trusted_tree_manifest != trusted_manifest:
            raise RefreshError("TRUSTED_MANIFEST_MISMATCH", "trusted tree differs from the trusted manifest")
        if candidate_manifest != trusted_manifest:
            raise RefreshError("TRUSTED_MANIFEST_MISMATCH", "candidate manifest differs from trusted manifest")

    document = _refresh_document(
        trusted_document=trusted_document,
        candidate=candidate,
        trusted=trusted,
        policy=policy,
        refreshed_paths=normalized_paths,
        detailed_records=detailed_records,
    )
    ledger_bytes = _canonical_json_bytes(document)
    export_bytes = _refresh_export_bytes(candidate=candidate, policy=policy, ledger_bytes=ledger_bytes)

    if output is None:
        if candidate.worktree_root is None:
            raise RefreshError("REFRESH_OUTPUT_REQUIRED", "--output is required when candidate-tree is a ref")
        output = candidate.worktree_root / "assets" / "public_provenance_ledger.json"
    if export_output is None:
        if candidate.worktree_root is None:
            raise RefreshError("REFRESH_OUTPUT_REQUIRED", "--export-output is required when candidate-tree is a ref")
        export_output = candidate.worktree_root / "PUBLIC_EXPORT.json"
    output = output.resolve()
    export_output = export_output.resolve()
    if output == export_output:
        raise RefreshError("REFRESH_OUTPUT_INVALID", "ledger and export outputs must be different files")
    if candidate.worktree_root is not None:
        candidate_root = candidate.worktree_root.resolve()
        if not _path_is_within(output, candidate_root, resolve=True) or not _path_is_within(export_output, candidate_root, resolve=True):
            raise RefreshError("REFRESH_OUTPUT_INVALID", "outputs for a worktree candidate must stay inside that candidate")
    if trusted.worktree_root is not None:
        trusted_root = trusted.worktree_root.resolve()
        if _path_is_within(output, trusted_root, resolve=True) or _path_is_within(export_output, trusted_root, resolve=True):
            raise RefreshError("REFRESH_OUTPUT_INVALID", "outputs must not overwrite the trusted tree")
    for path in (output, export_output):
        if path in (trusted_ledger_path, trusted_policy_path, trusted_baseline_path, trusted_manifest_path):
            raise RefreshError("REFRESH_OUTPUT_INVALID", "output would overwrite a trusted input")
    if candidate.worktree_root is not None:
        standard_output = (candidate.worktree_root / "assets" / "public_provenance_ledger.json").resolve()
        standard_export = (candidate.worktree_root / "PUBLIC_EXPORT.json").resolve()
        if output != standard_output or export_output != standard_export:
            raise RefreshError("REFRESH_OUTPUT_INVALID", "worktree outputs must be the canonical public ledger and export")
    else:
        for path in (output, export_output):
            if _path_is_within(path, candidate.repo_root, resolve=True):
                relative = path.relative_to(candidate.repo_root.resolve()).as_posix()
                if relative in candidate.blobs and relative not in ("assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json"):
                    raise RefreshError("REFRESH_OUTPUT_INVALID", "output would overwrite a candidate source path")

    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        export_output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(ledger_bytes)
        export_output.write_bytes(export_bytes)
    except OSError as error:
        raise RefreshError("REFRESH_OUTPUT_ERROR", "cannot write refreshed public artifacts") from error
    return {
        "candidate_tree": candidate.tree_sha,
        "trusted_tree": trusted.tree_sha,
        "paths": normalized_paths,
        "output": output,
        "export_output": export_output,
        "ledger": document,
    }


# ---------------------------------------------------------------------------
# Trusted admission of genuinely new public paths (``admit-new-reviewed``)
# ---------------------------------------------------------------------------
#
# ``refresh-reviewed`` changes hashes for paths a trusted baseline already
# authorizes. A genuinely new path has no baseline authority at all, so
# admitting one is a different operation with a different trust requirement:
# the candidate that introduced the bytes must not be the source of the
# authority that admits them.
#
# The new command therefore takes an external *admission authority* document
# that independently names each exact path together with the exact SHA-256 of
# the bytes an independent reviewer approved. Path + bytes + class are the
# whole admission; the command derives the policy include, the ledger entry,
# and the export from that authority plus the external trusted policy and
# ledger, and it refuses every softer form of authority: wildcards,
# directories, prefix/extension records, candidate-authored ledger entries,
# candidate policy/export bytes, and authority that names different paths or
# different bytes than the candidate actually carries.
#
# Authority classes are distinct:
#
# * deterministic public material (documentation, configuration, synthetic
#   fixtures/tests) is admitted from the independent path+hash review alone;
#   the deterministic classifier derives the class and the entry carries no
#   record id;
# * implementation/source paths require, in addition, an exact record in the
#   external trusted detailed ledger and a ``reviewed_blobs`` approval naming
#   this exact path and this exact digest, so the private authority -- not
#   the admission document and not the candidate -- supplies path and blob
#   authority;
# * prohibited/private classes are unadmittable: any path the trusted policy
#   excludes fails closed here.
#
# No identity is invented: the output records the trusted/candidate trees,
# the admitted paths, and a SHA-256 of the admission-authority bytes for
# audit ancestry, and never fabricates a person, DCO trailer, or attestation.

ADMISSION_KIND = "admission-authority"
ADMISSION_SCHEMA_VERSION = 1

#: Origin shapes an implementation-class admission must declare.  The value is
#: reviewed by the independent authority, never inferred from the candidate.
ADMISSION_ORIGIN_KINDS = frozenset({"authored_from_scratch", "derived_adapted", "third_party"})


def _read_admission_authority(
    path: Path,
    *,
    candidate_root: Path,
    requested: set[str],
) -> dict[str, dict]:
    """Read and validate the external admission authority document.

    Returns ``{path: statement}`` where every statement names an exact path
    with an exact lowercase SHA-256 and a supported public classification.
    The authority must live outside the candidate tree and must name exactly
    the requested path set -- no extras (an authority for path A cannot admit
    path B) and no omissions.
    """
    trusted = _external_input(path, candidate_root=candidate_root, label="admission authority")
    document = _read_json_file(trusted, code="ADMISSION_AUTHORITY_INVALID")
    if document.get("kind") != ADMISSION_KIND:
        raise RefreshError(
            "ADMISSION_AUTHORITY_INVALID",
            f"admission authority must declare kind {ADMISSION_KIND!r}",
        )
    if document.get("schema_version") != ADMISSION_SCHEMA_VERSION:
        raise RefreshError(
            "ADMISSION_AUTHORITY_INVALID",
            f"admission authority schema_version must be {ADMISSION_SCHEMA_VERSION}",
        )
    statements = document.get("reviewed_new_paths")
    if not isinstance(statements, list) or not statements:
        raise RefreshError(
            "ADMISSION_AUTHORITY_INVALID",
            "admission authority carries no reviewed_new_paths statements",
        )
    result: dict[str, dict] = {}
    for statement in statements:
        if not isinstance(statement, dict):
            raise RefreshError("ADMISSION_AUTHORITY_INVALID", "admission statement is malformed")
        path = _exact_path(statement.get("path"), code="ADMISSION_AUTHORITY_INVALID")
        digest = statement.get("sha256")
        if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            raise RefreshError(
                "ADMISSION_AUTHORITY_INVALID", f"admission statement for {path} has no lowercase sha256"
            )
        classification = statement.get("classification")
        if classification not in ALLOWED_CLASSES or classification == "unresolved":
            raise RefreshError(
                "ADMISSION_AUTHORITY_INVALID",
                f"admission statement for {path} names unsupported classification {classification!r}",
            )
        if path in result:
            raise RefreshError(
                "ADMISSION_AUTHORITY_INVALID", f"admission authority names {path} more than once"
            )
        result[path] = {
            "sha256": digest,
            "classification": classification,
            "origin_kind": statement.get("origin_kind"),
            "origin": statement.get("origin"),
            "license": statement.get("license"),
            "record_id": statement.get("record_id"),
        }
    if set(result) != requested:
        missing = sorted(requested - set(result))
        extra = sorted(set(result) - requested)
        detail = (
            f"missing {missing[0]!r}" if missing else f"unexpected {extra[0]!r}"
        )
        raise RefreshError(
            "ADMISSION_AUTHORITY_SCOPE",
            f"admission authority must name exactly the requested paths ({detail})",
        )
    return result


def _blob_approval_map(document: dict) -> dict[tuple[str, str], dict]:
    """Map ``(path, sha256)`` to a trusted detailed-ledger blob approval.

    A detailed record authorizes a *path*; a blob approval authorizes these
    exact bytes at that path.  ``admit-new-reviewed`` needs the latter for
    implementation-class admission because the bytes being admitted are new
    and have never been approved under any earlier public snapshot.
    """
    approvals: dict[tuple[str, str], dict] = {}
    entries = document.get("reviewed_blobs")
    if not isinstance(entries, list):
        return approvals
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        path = entry.get("path")
        digest = entry.get("sha256")
        if not isinstance(path, str) or not isinstance(digest, str):
            continue
        if any(character in path for character in "*?"):
            raise RefreshError("TRUSTED_LEDGER_INVALID", "a blob approval path contains a wildcard")
        path = _exact_path(path, code="TRUSTED_LEDGER_INVALID")
        if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            raise RefreshError("TRUSTED_LEDGER_INVALID", f"a blob approval for {path} has no lowercase sha256")
        key = (path, digest)
        if key in approvals:
            raise RefreshError("TRUSTED_LEDGER_INVALID", f"duplicate blob approval for {path} at one digest")
        approvals[key] = entry
    return approvals


def _extended_policy_bytes(trusted_policy: Path, admitted_paths: set[str]) -> tuple[bytes, dict]:
    """Mechanically extend the external trusted policy with the admitted paths.

    The include addition is a deterministic consequence of the admission
    authority naming those exact paths; it is generated here, never accepted
    from the candidate.  Exclusion rules are untouched, so an excluded path
    can never become include-eligible through this route.
    """
    document = _read_json_file(trusted_policy, code="TRUSTED_POLICY_INVALID")
    document["include_paths"] = sorted(set(document["include_paths"]) | set(admitted_paths))
    return _canonical_json_bytes(document), document


def admit_new_reviewed(
    *,
    trusted_ledger: Path,
    admission_authority: Path,
    candidate_tree: str,
    trusted_tree: str,
    paths: list[str],
    trusted_policy: Path,
    trusted_manifest: Path | None = None,
    trusted_baseline_ledger: Path | None = None,
) -> dict:
    """Admit genuinely new exact public paths from external trusted inputs only.

    Every input that carries authority -- the trusted ledger, the trusted
    policy, the optional trusted manifest, and the admission authority -- must
    live outside the candidate tree.  The candidate's own ledger, policy,
    export, and provenance records are never trusted inputs; the candidate
    policy is accepted only when it is semantically exactly the external
    trusted policy plus include entries for the admitted paths, and its
    ledger/export bytes are regenerated mechanically from the trusted inputs.
    """

    raw_paths = [_exact_path(path, code="ADMISSION_PATH_NOT_EXACT") for path in paths]
    if len(raw_paths) != len(set(raw_paths)):
        raise RefreshError("ADMISSION_PATH_DUPLICATE", "requested admission paths must not repeat")
    normalized_paths = sorted(set(raw_paths))
    if not normalized_paths:
        raise RefreshError("ADMISSION_PATH_REQUIRED", "at least one exact new path is required")
    for path in normalized_paths:
        if path in REFRESH_CONTROL_PATHS:
            raise RefreshError("ADMISSION_PATH_FORBIDDEN", f"generated/control path cannot be admitted: {path}")
        if is_implementation_path(path) and not path.startswith(("src/", "tools/")):
            # Root-level scripts are implementation by suffix too; nothing to
            # special-case -- the classifier decides below.
            pass

    candidate = _resolve_tree_selector(candidate_tree, default_repo=ROOT, role="candidate")
    controlled_root = (candidate.worktree_root or candidate.repo_root).resolve()
    trusted = _resolve_tree_selector(trusted_tree, default_repo=candidate.repo_root, role="trusted")
    if trusted.worktree_root is not None and (
        _path_is_within(trusted.worktree_root, controlled_root, resolve=False)
        or _path_is_within(trusted.worktree_root, controlled_root, resolve=True)
    ):
        raise RefreshError(
            "TRUSTED_TREE_CANDIDATE_CONTROLLED",
            "trusted tree worktree is inside the candidate tree",
        )

    trusted_ledger_path = _external_input(trusted_ledger, candidate_root=controlled_root, label="trusted ledger")
    trusted_policy_path = _external_input(trusted_policy, candidate_root=controlled_root, label="trusted policy")
    trusted_baseline_path = None
    if trusted_baseline_ledger is not None:
        trusted_baseline_path = _external_input(
            trusted_baseline_ledger, candidate_root=controlled_root, label="trusted baseline ledger"
        )
    trusted_manifest_path = None
    if trusted_manifest is not None:
        trusted_manifest_path = _external_input(
            trusted_manifest, candidate_root=controlled_root, label="trusted manifest"
        )
    authority = _read_admission_authority(
        admission_authority,
        candidate_root=controlled_root,
        requested=set(normalized_paths),
    )
    authority_path = _external_input(admission_authority, candidate_root=controlled_root, label="admission authority")
    authority_bytes = authority_path.read_bytes()

    try:
        policy = _load_publication_policy(trusted_policy_path)
    except Exception as error:
        raise RefreshError("TRUSTED_POLICY_INVALID", "trusted policy is invalid") from error
    policy_raw = trusted_policy_path.read_bytes()
    if trusted.blobs.get("assets/public_source_profile.json") != policy_raw:
        raise RefreshError("TRUSTED_POLICY_MISMATCH", "trusted tree does not contain the trusted policy bytes")

    # -- per-path admission preconditions ----------------------------------
    for path in normalized_paths:
        if path in trusted.blobs:
            raise RefreshError(
                "ADMISSION_PATH_EXISTING",
                f"{path} is already present in the trusted tree; use refresh-reviewed for an existing path",
            )
        if path not in candidate.blobs:
            raise RefreshError("CANDIDATE_PATH_MISSING", f"admission path is not present in the candidate tree: {path}")
        disposition = policy.resolve(path).disposition
        if disposition == "excluded":
            raise RefreshError(
                "ADMISSION_PATH_EXCLUDED",
                f"{path} is excluded by the trusted publication policy and cannot be admitted",
            )
        if disposition == "included":
            raise RefreshError(
                "ADMISSION_POLICY_MISMATCH",
                f"{path} is already included by the trusted policy but absent from the trusted tree",
            )
        candidate_hash = hashlib.sha256(candidate.blobs[path]).hexdigest()
        statement = authority[path]
        if statement["sha256"] != candidate_hash:
            raise RefreshError(
                "ADMISSION_HASH_MISMATCH",
                f"admission authority approved different bytes for {path}; candidate hash is {candidate_hash}",
            )

    # -- candidate tree differs from trusted only by the admitted paths -----
    candidate_paths = set(candidate.blobs)
    trusted_paths = set(trusted.blobs)
    removed = sorted(trusted_paths - candidate_paths)
    if removed:
        raise RefreshError("CANDIDATE_TREE_SCOPE_CHANGED", f"candidate removes a path from the trusted tree: {removed[0]}")
    added = sorted(candidate_paths - trusted_paths)
    if added != normalized_paths:
        unexpected = sorted(set(added) - set(normalized_paths))
        raise RefreshError(
            "ADMISSION_TREE_SCOPE",
            f"candidate adds a path outside the admitted set: {unexpected[0] if unexpected else added[0]}",
        )
    for path in sorted(candidate_paths & trusted_paths):
        if path in REFRESH_CONTROL_PATHS:
            # Policy is validated semantically below; ledger and export are
            # regenerated outputs whose candidate bytes are never trusted.
            continue
        if candidate.blobs[path] != trusted.blobs[path]:
            raise RefreshError(
                "CANDIDATE_TREE_STALE",
                f"candidate changed unrequested path {path}; admission changes must be explicit",
            )

    # -- candidate policy is the trusted policy plus exactly the admits -----
    extended_bytes, extended_document = _extended_policy_bytes(trusted_policy_path, set(normalized_paths))
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as _tmp_policy:
        _tmp_policy.write(extended_bytes.decode("utf-8"))
        _tmp_policy_path = Path(_tmp_policy.name)
    try:
        extended_policy = _load_publication_policy(_tmp_policy_path)
    finally:
        _tmp_policy_path.unlink(missing_ok=True)

    candidate_policy_raw = candidate.blobs.get("assets/public_source_profile.json")
    if candidate_policy_raw is None:
        raise RefreshError("CANDIDATE_POLICY_MISSING", "candidate tree has no publication policy")
    try:
        candidate_policy_document = json.loads(candidate_policy_raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RefreshError("ADMISSION_POLICY_MISMATCH", "candidate policy is not valid JSON") from error
    if candidate_policy_document != extended_document:
        raise RefreshError(
            "ADMISSION_POLICY_MISMATCH",
            "candidate policy must equal the trusted policy plus include entries for exactly the admitted paths",
        )

    if trusted_manifest_path is not None:
        candidate_manifest = candidate.blobs.get("assets/release_manifest.json")
        trusted_tree_manifest = trusted.blobs.get("assets/release_manifest.json")
        trusted_manifest_raw = trusted_manifest_path.read_bytes()
        if trusted_tree_manifest != trusted_manifest_raw:
            raise RefreshError("TRUSTED_MANIFEST_MISMATCH", "trusted tree differs from the trusted manifest")
        if candidate_manifest != trusted_manifest_raw:
            raise RefreshError("TRUSTED_MANIFEST_MISMATCH", "candidate manifest differs from trusted manifest")

    # -- trusted ledger: public snapshot and/or detailed development ledger ---
    # The detailed ledger carries ``records`` (path authority) and
    # ``reviewed_blobs`` (exact-bytes approval); both must be read from the
    # same external document before it is swapped for a baseline snapshot.
    trusted_document = _read_json_file(trusted_ledger_path, code="TRUSTED_LEDGER_INVALID")
    detailed_records: dict[str, dict] | None = None
    approvals: dict[tuple[str, str], dict] = {}
    has_entries = "entries" in trusted_document
    has_records = "records" in trusted_document
    if has_entries and has_records:
        raise RefreshError("TRUSTED_LEDGER_INVALID", "trusted ledger cannot mix public entries and detailed records")
    if has_entries:
        _validate_public_snapshot(trusted_document, tree=trusted, policy=extended_policy, label="trusted ledger")
    elif has_records:
        detailed_records = _detailed_records(trusted_document)
        approvals = _blob_approval_map(trusted_document)
        if trusted_baseline_path is not None:
            trusted_document = _read_json_file(trusted_baseline_path, code="TRUSTED_LEDGER_INVALID")
            _validate_public_snapshot(trusted_document, tree=trusted, policy=extended_policy, label="trusted baseline ledger")
    else:
        raise RefreshError("TRUSTED_LEDGER_INVALID", "trusted ledger must contain entries or detailed records")

    # -- per-path class + authority ----------------------------------------
    new_entries: list[dict] = []
    implementation_admitted = False
    for path in normalized_paths:
        statement = authority[path]
        classification = statement["classification"]
        candidate_hash = hashlib.sha256(candidate.blobs[path]).hexdigest()
        derived_class, derived_evidence = _class_for(path, None)
        if classification in DETERMINISTIC_REFRESH_CLASSES:
            if derived_class != classification or derived_class == "unresolved":
                raise RefreshError(
                    "ADMISSION_CLASS_MISMATCH",
                    f"{path} is not {classification}; deterministic classification derives {derived_class!r}",
                )
            evidence = derived_evidence
        else:
            if classification not in REFRESHABLE_CLASSES:
                raise RefreshError(
                    "ADMISSION_CLASS_MISMATCH",
                    f"{path} cannot be admitted as {classification!r}",
                )
            implementation_admitted = True
            if detailed_records is None:
                raise RefreshError(
                    "TRUSTED_RECORD_REQUIRED",
                    f"implementation-class admission requires the trusted detailed ledger: {path}",
                )
            record = detailed_records.get(path)
            if record is None:
                raise RefreshError(
                    "TRUSTED_PATH_MISSING",
                    f"trusted detailed ledger has no exact record for {path}",
                )
            if statement["origin_kind"] not in ADMISSION_ORIGIN_KINDS:
                raise RefreshError(
                    "ADMISSION_AUTHORITY_INCOMPLETE",
                    f"implementation admission for {path} must declare a supported origin_kind",
                )
            if not isinstance(statement["origin"], str) or not statement["origin"].strip():
                raise RefreshError(
                    "ADMISSION_AUTHORITY_INCOMPLETE",
                    f"implementation admission for {path} must declare an origin statement",
                )
            if not isinstance(statement["license"], str) or not statement["license"].strip():
                raise RefreshError(
                    "ADMISSION_AUTHORITY_INCOMPLETE",
                    f"implementation admission for {path} must declare a license basis",
                )
            public_class, evidence = _refresh_class_for(path, record)
            if public_class != classification:
                raise RefreshError(
                    "ADMISSION_CLASS_MISMATCH",
                    f"trusted detailed record class {public_class!r} disagrees with admitted {classification!r} for {path}",
                )
            approval = approvals.get((path, candidate_hash))
            if approval is None:
                raise RefreshError(
                    "BLOB_UNAPPROVED",
                    f"implementation-class admission requires an exact reviewed-blob approval for {path} at sha256 {candidate_hash}",
                )
            expected_record = record["id"]
            if approval.get("record_id") != expected_record:
                raise RefreshError(
                    "BLOB_APPROVAL_RECORD_MISMATCH",
                    f"blob approval for {path} cites a record that is not the exact covering record",
                )
            approved_class, _ = _class_for(path, {"classification": approval.get("classification"), "id": None})
            if approved_class != classification:
                raise RefreshError(
                    "BLOB_APPROVAL_CLASS_MISMATCH",
                    f"blob approval for {path} authorizes class {approved_class!r}, not {classification!r}",
                )
        entry = {"path": path, "classification": classification, "evidence": evidence,
                 "sha256": candidate_hash}
        new_entries.append(entry)

    # -- compose the output ledger -----------------------------------------
    # Baseline entries come from the public snapshot (``has_entries``, or the
    # snapshot paired with a detailed ledger via ``trusted_baseline_path``) so
    # an admission changes only what it must; with a detailed ledger alone the
    # baseline is regenerated from the trusted tree and its records.
    if has_entries or trusted_baseline_path is not None:
        document = json.loads(json.dumps(trusted_document, ensure_ascii=False))
    else:
        assert detailed_records is not None
        generated = _ledger_from_detailed(tree=trusted, policy=extended_policy, records=detailed_records)
        document = json.loads(json.dumps(generated, ensure_ascii=False))
    document_entries = {entry["path"]: entry for entry in document["entries"]}
    for path in normalized_paths:
        if path in document_entries:
            raise RefreshError("TRUSTED_PATH_MISSING", f"trusted ledger already carries an entry for {path}")
    # The publication policy is itself a mechanical output of this command (it
    # gains include entries for exactly the admitted paths), so its ledger
    # entry must track the regenerated bytes rather than the trusted policy
    # bytes the snapshot recorded.
    policy_entry = document_entries.get("assets/public_source_profile.json")
    if policy_entry is None:
        raise RefreshError("TRUSTED_LEDGER_INVALID", "trusted ledger has no entry for the publication policy")
    policy_entry["sha256"] = hashlib.sha256(extended_bytes).hexdigest()
    document["entries"] = sorted(document["entries"] + new_entries, key=lambda entry: entry["path"])
    document["admission"] = {
        "workflow": "admit-new-reviewed",
        "trusted_tree": trusted.tree_sha,
        "candidate_tree": candidate.tree_sha,
        "admitted_paths": normalized_paths,
        "authority_sha256": hashlib.sha256(authority_bytes).hexdigest(),
    }
    errors = validate_ledger(document, require_hashes=True, require_resolved=True)
    if errors:
        raise RefreshError("ADMISSION_OUTPUT_INVALID", errors[0])
    # Bind every admitted entry to the exact bytes the authority approved.
    # Existing snapshot entries were already bound to the trusted tree above,
    # and every non-admitted candidate path is byte-identical to that tree, so
    # no further all-entries sweep is needed -- control-file entries describe
    # the trusted bytes, which legitimately differ from the regenerated ones.
    for entry in new_entries:
        actual = hashlib.sha256(candidate.blobs[entry["path"]]).hexdigest()
        if entry["sha256"] != actual:
            raise RefreshError("ADMISSION_OUTPUT_INVALID",
                               f"output hash does not match candidate bytes for {entry['path']}")

    ledger_bytes = _canonical_json_bytes(document)
    export_bytes = _refresh_export_bytes(candidate=candidate, policy=extended_policy, ledger_bytes=ledger_bytes)

    # -- write the three mechanical outputs --------------------------------
    if candidate.worktree_root is None:
        raise RefreshError("ADMISSION_OUTPUT_REQUIRED", "admission requires a clean candidate worktree to write its outputs")
    candidate_root = candidate.worktree_root.resolve()
    trusted_inputs = [
        path for path in (
            trusted_ledger_path, trusted_policy_path, trusted_baseline_path,
            trusted_manifest_path, authority_path,
        ) if path is not None
    ]
    written: dict[str, bytes] = {
        "assets/public_provenance_ledger.json": ledger_bytes,
        "PUBLIC_EXPORT.json": export_bytes,
        "assets/public_source_profile.json": extended_bytes,
    }
    for relative, bytes_value in written.items():
        target = candidate_root / relative
        target = target.resolve()
        if not _path_is_within(target, candidate_root, resolve=True):
            raise RefreshError("ADMISSION_OUTPUT_INVALID", "output escapes the candidate worktree")
        if any(target == trusted_input.resolve() for trusted_input in trusted_inputs):
            raise RefreshError("ADMISSION_OUTPUT_INVALID", "output would overwrite a trusted input")
    try:
        for relative, bytes_value in written.items():
            target = candidate_root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(bytes_value)
    except OSError as error:
        raise RefreshError("ADMISSION_OUTPUT_ERROR", "cannot write admitted public artifacts") from error
    return {
        "candidate_tree": candidate.tree_sha,
        "trusted_tree": trusted.tree_sha,
        "paths": normalized_paths,
        "implementation_admitted": implementation_admitted,
        "ledger": document,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "command", nargs="?", choices=("refresh-reviewed", "admit-new-reviewed"),
    )
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument(
        "--implementation-ledger",
        type=Path,
        default=IMPLEMENTATION_LEDGER,
        help=(
            "trusted detailed development ledger; may remain outside the public tree "
            "and its path is never written to generated output"
        ),
    )
    parser.add_argument("--check", action="store_true")
    parser.add_argument(
        "--trusted-ledger",
        type=Path,
        help=(
            "refresh-reviewed / admit-new-reviewed: external trusted public ledger snapshot or "
            "detailed ledger; never read from the candidate tree"
        ),
    )
    parser.add_argument(
        "--trusted-baseline-ledger",
        type=Path,
        help="refresh-reviewed / admit-new-reviewed: optional external public snapshot paired with a detailed ledger",
    )
    parser.add_argument(
        "--candidate-tree",
        type=str,
        help="refresh-reviewed / admit-new-reviewed: clean candidate worktree path or immutable Git tree-ish",
    )
    parser.add_argument(
        "--trusted-tree",
        type=str,
        help="refresh-reviewed / admit-new-reviewed: maintainer-selected baseline worktree path or immutable Git tree-ish",
    )
    parser.add_argument(
        "--trusted-policy",
        type=Path,
        help="refresh-reviewed / admit-new-reviewed: external trusted publication policy",
    )
    parser.add_argument(
        "--trusted-manifest",
        type=Path,
        help="refresh-reviewed / admit-new-reviewed: optional external trusted release manifest",
    )
    parser.add_argument(
        "--admission-authority",
        type=Path,
        help=(
            "admit-new-reviewed: external admission-authority document naming each exact new path, "
            "its approved SHA-256, and its public classification; never read from the candidate tree"
        ),
    )
    parser.add_argument(
        "--export-output",
        type=Path,
        help="refresh-reviewed: output path for the regenerated PUBLIC_EXPORT.json",
    )
    parser.add_argument(
        "--paths",
        nargs="+",
        help=(
            "refresh-reviewed / admit-new-reviewed: explicit exact implementation paths to refresh "
            "or exact new public paths to admit"
        ),
    )
    args = parser.parse_args(argv)
    if args.command == "admit-new-reviewed":
        missing = [
            name for name, value in (
                ("--trusted-ledger", args.trusted_ledger),
                ("--admission-authority", args.admission_authority),
                ("--candidate-tree", args.candidate_tree),
                ("--trusted-tree", args.trusted_tree),
                ("--trusted-policy", args.trusted_policy),
                ("--paths", args.paths),
            ) if value is None
        ]
        if missing:
            print(
                f"provenance ledger admission: ADMISSION_ARGUMENT_REQUIRED: missing {', '.join(missing)}",
                file=sys.stderr,
            )
            return 1
        try:
            result = admit_new_reviewed(
                trusted_ledger=args.trusted_ledger,
                admission_authority=args.admission_authority,
                candidate_tree=args.candidate_tree,
                trusted_tree=args.trusted_tree,
                paths=args.paths,
                trusted_policy=args.trusted_policy,
                trusted_manifest=args.trusted_manifest,
                trusted_baseline_ledger=args.trusted_baseline_ledger,
            )
        except RefreshError as error:
            print(f"provenance ledger admission: {error.code}: {error}", file=sys.stderr)
            return 1
        print(
            "provenance ledger admission: admitted "
            f"{len(result['paths'])} exact new path(s); generated "
            f"{len(result['ledger']['entries'])} entries; candidate_tree={result['candidate_tree']}"
        )
        return 0
    if args.command == "refresh-reviewed":
        missing = [
            name for name, value in (
                ("--trusted-ledger", args.trusted_ledger),
                ("--candidate-tree", args.candidate_tree),
                ("--trusted-tree", args.trusted_tree),
                ("--trusted-policy", args.trusted_policy),
                ("--paths", args.paths),
            ) if value is None
        ]
        if missing:
            print(
                f"provenance ledger refresh: REFRESH_ARGUMENT_REQUIRED: missing {', '.join(missing)}",
                file=sys.stderr,
            )
            return 1
        try:
            result = refresh_reviewed(
                trusted_ledger=args.trusted_ledger,
                candidate_tree=args.candidate_tree,
                trusted_tree=args.trusted_tree,
                paths=args.paths,
                output=args.output,
                export_output=args.export_output,
                trusted_policy=args.trusted_policy,
                trusted_manifest=args.trusted_manifest,
                trusted_baseline_ledger=args.trusted_baseline_ledger,
            )
        except RefreshError as error:
            print(f"provenance ledger refresh: {error.code}: {error}", file=sys.stderr)
            return 1
        print(
            "provenance ledger refresh: generated "
            f"{len(result['ledger']['entries'])} entries for {len(result['paths'])} exact path(s); "
            f"candidate_tree={result['candidate_tree']}"
        )
        return 0
    if args.check:
        check_output = args.output or DEFAULT_OUTPUT
        if not check_output.is_file():
            print(
                "provenance ledger: checked-in public ledger is absent",
                file=sys.stderr,
            )
            return 1
        document = json.loads(check_output.read_text(encoding="utf-8"))
    else:
        if not args.implementation_ledger.is_file():
            print(
                "provenance ledger: detailed development ledger is absent; refusing to synthesize "
                "public provenance from broad defaults (use --check on the checked-in ledger)",
                file=sys.stderr,
            )
            return 1
        try:
            document = build_ledger(
                args.output or DEFAULT_OUTPUT,
                implementation_ledger=args.implementation_ledger,
            )
        except RuntimeError as error:
            print(f"provenance ledger: {error}", file=sys.stderr)
            return 1
    errors = validate_ledger(document, require_resolved=True)
    if errors:
        for error in errors:
            print(f"provenance ledger: {error}", file=sys.stderr)
        return 1
    print(f"provenance ledger: {'checked' if args.check else 'generated'} {len(document['entries'])} explicit entries")
    if args.check and not args.implementation_ledger.is_file():
        print(
            "provenance ledger: note: detailed development ledger is absent; the checked-in ledger is "
            "validated structurally (coverage, resolution, hashes) but attestation claims are not "
            "authenticated here -- attestation is asserted by the release flow against the detailed "
            "development ledger or an externally trusted copy",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
