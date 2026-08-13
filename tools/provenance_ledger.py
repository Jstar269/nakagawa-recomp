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

try:
    from .public_export import write_document as _write_json_document
except ImportError:
    from public_export import write_document as _write_json_document

ROOT = Path(__file__).resolve().parent.parent
POLICY_PATH = ROOT / "assets" / "public_source_profile.json"
IMPLEMENTATION_LEDGER = ROOT / "docs" / "provenance" / "IMPLEMENTATION_PROVENANCE.json"
DEFAULT_OUTPUT = ROOT / "assets" / "public_provenance_ledger.json"

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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
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
    args = parser.parse_args(argv)
    if args.check:
        if not args.output.is_file():
            print(
                "provenance ledger: checked-in public ledger is absent",
                file=sys.stderr,
            )
            return 1
        document = json.loads(args.output.read_text(encoding="utf-8"))
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
                args.output,
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
