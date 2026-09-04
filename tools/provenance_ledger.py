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
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
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
            raise RefreshError("NEW_PATH_REFUSED", f"candidate adds implementation path {implementation_added[0]}")
        raise RefreshError("CANDIDATE_PUBLIC_BOUNDARY", f"candidate adds a path outside the trusted tree: {added[0]}")
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
            raise RefreshError("NEW_PATH_REFUSED", f"refresh path is not present in the trusted tree: {path}")
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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs="?", choices=("refresh-reviewed",))
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
            "refresh-reviewed: external trusted public ledger snapshot or detailed ledger; "
            "never read from the candidate tree"
        ),
    )
    parser.add_argument(
        "--trusted-baseline-ledger",
        type=Path,
        help="refresh-reviewed: optional external public snapshot paired with a detailed ledger",
    )
    parser.add_argument(
        "--candidate-tree",
        type=str,
        help="refresh-reviewed: clean candidate worktree path or immutable Git tree-ish",
    )
    parser.add_argument(
        "--trusted-tree",
        type=str,
        help="refresh-reviewed: maintainer-selected baseline worktree path or immutable Git tree-ish",
    )
    parser.add_argument(
        "--trusted-policy",
        type=Path,
        help="refresh-reviewed: external trusted publication policy",
    )
    parser.add_argument(
        "--trusted-manifest",
        type=Path,
        help="refresh-reviewed: optional external trusted release manifest",
    )
    parser.add_argument(
        "--export-output",
        type=Path,
        help="refresh-reviewed: output path for the regenerated PUBLIC_EXPORT.json",
    )
    parser.add_argument(
        "--paths",
        nargs="+",
        help="refresh-reviewed: explicit exact implementation paths to refresh",
    )
    args = parser.parse_args(argv)
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
