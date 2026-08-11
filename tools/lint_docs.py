#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Lightweight documentation freshness and staleness linter.

Detects generalized classes of staleness anti-patterns in public documentation:
- Ephemeral CI run numbers and "latest run #<N>" language in evergreen README.md
- Volatile "as of <date>" status claims inside evergreen README.md sections
- Obsolete private-repository topology statements across public documentation
- Dead private-era issue URLs matching retired historical issue numbers
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Class pattern: Ephemeral CI run numbers in evergreen README.md
EPHEMERAL_RUN_PATTERNS = [
    re.compile(r"latest (?:full |successful )?(?:hosted )?run (?:was |is )?(?:run )?`?\d+`?", re.IGNORECASE),
    re.compile(r"run `\d{8,}`", re.IGNORECASE),
]

# Class pattern: Dated status claims in evergreen README.md
README_DATED_STATUS_PATTERNS = [
    re.compile(r"\bas of (?:january|february|march|april|may|june|july|august|september|october|november|december|\d{4}-\d{2}-\d{2})\b", re.IGNORECASE),
]

# Class pattern: Obsolete private-repo topology statements
OBSOLETE_TOPOLOGY_PATTERNS = [
    re.compile(r"(?:The|This) repository is \*\*(?:still )?private\*\*", re.IGNORECASE),
    re.compile(r"Keep this repository private", re.IGNORECASE),
    re.compile(r"create a \*\*(?:new )?public repository", re.IGNORECASE),
]

# Explicit denylist: Retired historical issue numbers from pre-export era that do not exist on public main
RETIRED_PRIVATE_ISSUE_URLS = re.compile(
    r"github\.com/Jstar269/nakagawa-recomp/(?:issues|pull)/(?:98|99|102|103|104|105|139|142|143|145|146|147|149|150|151|152|154|179|187|188|196|197|234|286|304|339)\b"
)

# Role-aware historical evidence files that preserve dated historical issue URLs
HISTORICAL_EVIDENCE_DOCS = {
    "docs/STATUS_HISTORY.md",
    "docs/ROADMAP.md",
    "docs/AUDIO_OUTPUT_ACCEPTANCE_20260807.md",
    "docs/COVERAGE_LEDGER.md",
    "docs/provenance/INDEPENDENCE_BACKLOG.md",
}


def get_tracked_markdown_files(repo_root: pathlib.Path = ROOT) -> list[pathlib.Path]:
    """Return all tracked .md files using git ls-files."""
    try:
        res = subprocess.run(
            ["git", "ls-files", "*.md"],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=True,
        )
        files = [repo_root / line for line in res.stdout.splitlines() if line.strip()]
        if files:
            return sorted(files)
    except Exception:
        pass

    # Fallback if git is not available
    return sorted(repo_root.glob("*.md")) + sorted((repo_root / "docs").glob("**/*.md"))


def lint_readme(readme_path: pathlib.Path) -> list[str]:
    errors = []
    if not readme_path.is_file():
        return errors

    text = readme_path.read_text(encoding="utf-8")
    for idx, line in enumerate(text.splitlines(), 1):
        for pat in EPHEMERAL_RUN_PATTERNS:
            if pat.search(line):
                errors.append(f"README.md:{idx}: contains ephemeral CI run ID reference ('{line.strip()}')")
        for pat in README_DATED_STATUS_PATTERNS:
            if pat.search(line):
                errors.append(f"README.md:{idx}: contains volatile dated status claim ('{line.strip()}'); move to ISSUES.md or STATUS_HISTORY.md")
    return errors


def lint_doc_links_and_topology(doc_path: pathlib.Path, repo_root: pathlib.Path = ROOT) -> list[str]:
    errors = []
    if not doc_path.is_file():
        return errors

    try:
        rel_path = doc_path.relative_to(repo_root).as_posix()
    except ValueError:
        rel_path = doc_path.name

    # Skip dead-issue check for historical evidence logs that preserve exact dates/history
    is_historical_evidence = rel_path in HISTORICAL_EVIDENCE_DOCS

    text = doc_path.read_text(encoding="utf-8")

    for idx, line in enumerate(text.splitlines(), 1):
        if not is_historical_evidence and RETIRED_PRIVATE_ISSUE_URLS.search(line):
            errors.append(f"{rel_path}:{idx}: contains dead private-era issue URL")
        for pat in OBSOLETE_TOPOLOGY_PATTERNS:
            if pat.search(line):
                errors.append(f"{rel_path}:{idx}: contains obsolete private-repository topology statement")

    return errors


def run_all_doc_lints(repo_root: pathlib.Path = ROOT) -> list[str]:
    all_errors = []
    readme_path = repo_root / "README.md"
    all_errors.extend(lint_readme(readme_path))

    md_files = get_tracked_markdown_files(repo_root)
    for md_file in md_files:
        all_errors.extend(lint_doc_links_and_topology(md_file, repo_root))

    return all_errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Documentation freshness and staleness linter")
    parser.add_argument("--repo-root", type=pathlib.Path, default=ROOT, help="Repository root path")
    args = parser.parse_args()

    errors = run_all_doc_lints(args.repo_root)
    if errors:
        print("Documentation Freshness Linter: FAIL", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return 1

    print("Documentation Freshness Linter: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
