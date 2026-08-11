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
    r"github\.com/Jstar269/nakagawa-recomp/(?:issues|pull)/(?:98|99|102|104|339)\b"
)


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


def lint_doc_links_and_topology(doc_path: pathlib.Path) -> list[str]:
    errors = []
    if not doc_path.is_file():
        return errors

    rel_path = doc_path.relative_to(ROOT).as_posix()
    text = doc_path.read_text(encoding="utf-8")

    for idx, line in enumerate(text.splitlines(), 1):
        if RETIRED_PRIVATE_ISSUE_URLS.search(line):
            errors.append(f"{rel_path}:{idx}: contains dead private-era issue URL")
        for pat in OBSOLETE_TOPOLOGY_PATTERNS:
            if pat.search(line):
                errors.append(f"{rel_path}:{idx}: contains obsolete private-repository topology statement")

    return errors


def run_all_doc_lints(repo_root: pathlib.Path = ROOT) -> list[str]:
    all_errors = []
    readme_path = repo_root / "README.md"
    all_errors.extend(lint_readme(readme_path))

    # Scan all markdown files in root and docs/
    md_files = list(repo_root.glob("*.md")) + list((repo_root / "docs").glob("**/*.md"))
    for md_file in sorted(md_files):
        all_errors.extend(lint_doc_links_and_topology(md_file))

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
