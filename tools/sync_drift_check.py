# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Fail-closed reconciliation between a public-safe export and the public repository.

The public repository and this private archive deliberately have **unrelated Git
histories**, so ancestry can never be used to decide whether they agree. The only
sound comparison is content: take a fresh ``public-safe-v1`` export produced from
private ``main`` and compare it, path by path, against a checkout of public ``main``.

The question this answers is::

    Would exporting private main today regress a generic fix that already
    landed on public main?

Every path is placed in exactly one category:

``EXPECTED_PUBLIC_ONLY``
    The path is on the curated public-only allowlist. Public documentation that
    describes the reader's own repository is legitimately public-only; copying it
    into the private archive would make the archive's docs false.

``EXPECTED_PRIVATE_EXCLUSION``
    The path is excluded by the public-safe profile and is correctly absent from
    both sides.

``GENERIC_DRIFT``
    A path that should be identical is not. This is the regression signal.

``UNKNOWN``
    Anything that cannot be confidently placed, including an excluded path that is
    actually present. Unknown is always a failure.

Exit codes: ``0`` clean, ``1`` drift or unknown findings, ``2`` usage/IO error.

Limitations, stated plainly:

* Content hashing cannot by itself tell which side is *older*. A differing text
  file is reported as ``GENERIC_DRIFT`` with both digests and must be read by a
  human. The one place direction is decidable is the npm lockfile, where package
  versions are ordered, so :func:`compare_lockfiles` reports an explicit
  ``export_behind`` verdict.
* The tool never copies, writes or repairs anything. It only classifies.
* A clean report means the two trees agree on generic content at this instant. It
  is not a legal clearance, not a publication gate, and not a substitute for
  ``publish_audit.py``.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent

PROFILE_PATH = "assets/public_source_profile.json"
EXPORT_MANIFEST = "PUBLIC_EXPORT.json"

# Paths that are legitimately allowed to differ between the export and public main.
# Keep this list short and justified; every entry is a place where the two
# repositories intentionally say different things.
PUBLIC_ONLY_PATHS: dict[str, str] = {
    "README.md": (
        "The public copy describes the sanitized public source repository (public-safe-v1). "
        "The archive really is private, so the private copy must keep its own wording."
    ),
    "docs/PUBLICATION_READINESS.md": (
        "The public copy tells the reader their repository is the sanitized public one. "
        "The archive really is private, so the private copy must keep its own wording."
    ),
    EXPORT_MANIFEST: (
        "Export provenance manifest; regenerated per export and not expected to be "
        "byte-stable against a published copy."
    ),
}

CLEAN_CATEGORIES = frozenset({"EXPECTED_PUBLIC_ONLY", "EXPECTED_PRIVATE_EXCLUSION"})


def sha256_file(path: Path) -> str:
    """Content digest, normalized for line endings on text files.

    The exporter materializes its working tree with the host's line endings, while
    a fresh ``git clone`` applies ``.gitattributes`` (``* text=auto eol=lf``). Hashing
    raw bytes therefore reports every CRLF text file as drift even when the committed
    content is identical -- which is noise that would train a reader to ignore the
    gate. Text files are normalized to LF before hashing; binary files (any NUL byte)
    are hashed byte-exactly, so pinned LUTs and fonts are never silently equated.
    """
    data = path.read_bytes()
    if b"\x00" not in data:
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def index_tree(root: Path) -> dict[str, str]:
    """Map repo-relative POSIX path -> sha256 for every file under *root*.

    ``.git`` and build/dependency output are skipped: they are never release
    content and their presence differs by how the tree was materialized.
    """
    skip_dirs = {".git", "node_modules", "build", ".next", "__pycache__", ".ruff_cache"}
    out: dict[str, str] = {}
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(root)
        if any(part in skip_dirs for part in rel.parts):
            continue
        out[rel.as_posix()] = sha256_file(path)
    return out


def load_excluded_paths(export_root: Path, source_root: Path = ROOT) -> tuple[frozenset[str], list[str]]:
    """Return (excluded exact paths, excluded globs) for the public-safe profile.

    Prefers the export's own ``PUBLIC_EXPORT.json`` because that records what the
    export actually removed; falls back to the profile in the source tree.
    """
    manifest = export_root / EXPORT_MANIFEST
    if manifest.is_file():
        try:
            data = json.loads(manifest.read_text(encoding="utf-8"))
        except ValueError as exc:
            raise ValueError(f"{EXPORT_MANIFEST} is not valid JSON: {exc}") from exc
        paths = data.get("excluded_paths")
        if isinstance(paths, list) and paths:
            return frozenset(p for p in paths if isinstance(p, str)), []

    profile = source_root / PROFILE_PATH
    if not profile.is_file():
        raise FileNotFoundError(f"no {EXPORT_MANIFEST} in export and no {PROFILE_PATH} in source tree")
    data = json.loads(profile.read_text(encoding="utf-8"))
    exact = frozenset(p for p in data.get("exclude_paths", []) if isinstance(p, str))
    globs = [g for g in data.get("exclude_globs", []) if isinstance(g, str)]
    return exact, globs


def is_excluded(path: str, exact: frozenset[str], globs: list[str]) -> bool:
    if path in exact:
        return True
    return any(Path(path).match(pattern) for pattern in globs)


def classify(
    export_files: dict[str, str],
    public_files: dict[str, str],
    excluded_exact: frozenset[str],
    excluded_globs: list[str],
) -> list[dict[str, str]]:
    """Classify every path present on either side. Identical paths are omitted."""
    # Iterate the profile's excluded paths as well as everything actually present, so
    # absence of an excluded component is positively confirmed rather than merely
    # never observed. Without this the EXPECTED_PRIVATE_EXCLUSION category would be
    # unreachable and the "export leaks excluded material" check would be vacuous.
    findings: list[dict[str, str]] = []
    for path in sorted(set(export_files) | set(public_files) | set(excluded_exact)):
        in_export = path in export_files
        in_public = path in public_files
        excluded = is_excluded(path, excluded_exact, excluded_globs)

        if excluded:
            if in_export or in_public:
                # An excluded component is present where it must not be. Never
                # silently tolerate this: it is the leak case.
                findings.append(
                    {
                        "path": path,
                        "category": "UNKNOWN",
                        "detail": "path is excluded by the public-safe profile but is present"
                        f" ({'export' if in_export else ''}{'+' if in_export and in_public else ''}"
                        f"{'public' if in_public else ''})",
                    }
                )
            else:
                findings.append(
                    {
                        "path": path,
                        "category": "EXPECTED_PRIVATE_EXCLUSION",
                        "detail": "excluded by the public-safe profile and absent from both sides",
                    }
                )
            continue

        if in_export and in_public:
            if export_files[path] == public_files[path]:
                continue
            if path in PUBLIC_ONLY_PATHS:
                findings.append(
                    {
                        "path": path,
                        "category": "EXPECTED_PUBLIC_ONLY",
                        "detail": PUBLIC_ONLY_PATHS[path],
                    }
                )
            else:
                findings.append(
                    {
                        "path": path,
                        "category": "GENERIC_DRIFT",
                        "detail": f"content differs: export {export_files[path][:12]} != public {public_files[path][:12]}",
                    }
                )
            continue

        if in_public and not in_export:
            category = "EXPECTED_PUBLIC_ONLY" if path in PUBLIC_ONLY_PATHS else "GENERIC_DRIFT"
            detail = (
                PUBLIC_ONLY_PATHS[path]
                if path in PUBLIC_ONLY_PATHS
                else "present on public main but absent from the export; exporting today would remove it"
            )
            findings.append({"path": path, "category": category, "detail": detail})
            continue

        # Present in the export but not on public main.
        findings.append(
            {
                "path": path,
                "category": "UNKNOWN",
                "detail": "export introduces a path that public main does not have and that no rule classifies",
            }
        )
    return findings


def _lock_versions(path: Path) -> dict[str, str]:
    data = json.loads(path.read_text(encoding="utf-8"))
    out: dict[str, str] = {}
    for key, value in (data.get("packages") or {}).items():
        if not key or not isinstance(value, dict):
            continue
        version = value.get("version")
        if isinstance(version, str):
            out[key] = version
    return out


def _version_key(version: str) -> tuple[int, ...]:
    parts: list[int] = []
    for chunk in version.split("-")[0].split("."):
        try:
            parts.append(int(chunk))
        except ValueError:
            parts.append(0)
    return tuple(parts)


def compare_lockfiles(export_root: Path, public_root: Path, rel: str = "interface/package-lock.json") -> list[dict]:
    """Report npm packages whose resolved version differs between the two trees.

    This is the one comparison where direction is decidable, so it answers the
    "public security fix missing privately" case directly.
    """
    export_lock, public_lock = export_root / rel, public_root / rel
    if not (export_lock.is_file() and public_lock.is_file()):
        return []
    exported, published = _lock_versions(export_lock), _lock_versions(public_lock)
    findings: list[dict] = []
    for name in sorted(set(exported) & set(published)):
        if exported[name] == published[name]:
            continue
        findings.append(
            {
                "package": name,
                "export": exported[name],
                "public": published[name],
                "export_behind": _version_key(exported[name]) < _version_key(published[name]),
            }
        )
    return findings


def render(findings: list[dict[str, str]], lock_findings: list[dict]) -> tuple[str, bool]:
    counts: dict[str, int] = {}
    for finding in findings:
        counts[finding["category"]] = counts.get(finding["category"], 0) + 1

    lines = ["=== sync drift report ==="]
    for category in ("GENERIC_DRIFT", "UNKNOWN", "EXPECTED_PUBLIC_ONLY", "EXPECTED_PRIVATE_EXCLUSION"):
        lines.append(f"{category}: {counts.get(category, 0)}")

    behind = [entry for entry in lock_findings if entry["export_behind"]]
    if lock_findings:
        lines.append("")
        lines.append(f"npm lockfile: {len(lock_findings)} differing package(s), {len(behind)} where the export is BEHIND public")
        for entry in lock_findings:
            marker = "EXPORT BEHIND" if entry["export_behind"] else "export ahead"
            lines.append(f"  [{marker}] {entry['package']}: export={entry['export']} public={entry['public']}")

    for category in ("GENERIC_DRIFT", "UNKNOWN"):
        rows = [f for f in findings if f["category"] == category]
        if rows:
            lines.append("")
            lines.append(f"--- {category} ---")
            for row in rows:
                lines.append(f"  {row['path']}: {row['detail']}")

    failed = bool(counts.get("GENERIC_DRIFT")) or bool(counts.get("UNKNOWN")) or bool(behind)
    lines.append("")
    lines.append("RESULT: FAIL (unknown findings are always failures)" if failed else "RESULT: OK")
    return "\n".join(lines), failed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Classify content differences between a public-safe export and public main. Fail-closed."
    )
    parser.add_argument("--export-dir", required=True, help="fresh public-safe-v1 export produced from private main")
    parser.add_argument("--public-dir", required=True, help="checkout of public main")
    parser.add_argument("--json", action="store_true", help="emit the report as JSON")
    args = parser.parse_args(argv)

    export_root, public_root = Path(args.export_dir), Path(args.public_dir)
    for label, root in (("--export-dir", export_root), ("--public-dir", public_root)):
        if not root.is_dir():
            print(f"error: {label} is not a directory: {root}", file=sys.stderr)
            return 2

    try:
        excluded_exact, excluded_globs = load_excluded_paths(export_root)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    findings = classify(index_tree(export_root), index_tree(public_root), excluded_exact, excluded_globs)
    lock_findings = compare_lockfiles(export_root, public_root)
    text, failed = render(findings, lock_findings)

    if args.json:
        print(json.dumps({"findings": findings, "lockfile": lock_findings, "failed": failed}, indent=2))
    else:
        print(text)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
