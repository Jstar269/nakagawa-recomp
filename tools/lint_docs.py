#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Lightweight documentation freshness and staleness linter.

Detects deterministic classes of staleness that should not require network access:
- ephemeral CI-run wording in the evergreen README;
- volatile ``as of`` status claims in the evergreen README;
- obsolete private/public repository-topology statements;
- known retired public URLs outside explicitly historical evidence records;
- repository-relative Markdown links whose targets are absent or escape the tree.

Live GitHub object existence/state is intentionally handled by ``audit_public_issue_links.py``.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
from urllib.parse import unquote

ROOT = pathlib.Path(__file__).resolve().parent.parent

EPHEMERAL_RUN_PATTERNS = [
    re.compile(
        r"latest (?:full |successful )?(?:hosted )?run (?:was |is )?(?:run )?`?\d+`?",
        re.IGNORECASE,
    ),
    re.compile(r"run `\d{8,}`", re.IGNORECASE),
]

README_DATED_STATUS_PATTERNS = [
    re.compile(
        r"\bas of (?:january|february|march|april|may|june|july|august|september|october|november|december|\d{4}-\d{2}-\d{2})\b",
        re.IGNORECASE,
    ),
]

OBSOLETE_TOPOLOGY_PATTERNS = [
    re.compile(r"(?:The|This) repository is \*\*(?:still )?private\*\*", re.IGNORECASE),
    re.compile(r"Keep this repository private", re.IGNORECASE),
    re.compile(r"create a \*\*(?:new )?public repository", re.IGNORECASE),
    re.compile(r"For the \*\*new public repository\*\*", re.IGNORECASE),
]

# Regression denylist for URLs known to have belonged to pre-export/private-era tracking.
# This is not a general proof of object existence; the networked auditor owns that question.
#
# EXPIRY.  GitHub numbers issues and pull requests from one sequence, so the public
# repository steadily REALLOCATES these numbers to real objects of its own.  A denylist
# entry is only meaningful while the public repository has not yet reached it; once it
# has, the URL resolves to a live public object and flagging it is a false positive that
# blocks legitimate work -- which is how this was found: citing the (real, new) public
# issue 98 failed this lint.
#
# An offline check that fires on live objects is worse than no check at all. Retiring an
# entry loses only an offline regression check; leaving one in place past its number
# breaks the build. The guard below refuses any entry at or below the frontier, so the
# two can never drift apart silently.
#
# The frontier is a LOWER BOUND on what the public repository has allocated, and it only
# ever moves up. State it as a bound rather than as an exact count: an exact count is stale
# the moment the next object is opened, and the dangerous direction is a frontier that is
# too LOW, because that is what lets a soon-to-be-live number sit in the denylist unnoticed.
# Raise it whenever this file is touched during a sweep. The public sequence had allocated
# at least through 133 as of 2026-08-27 -- the pull request carrying this change is 133 --
# so 98-133 are live public objects, which is why the 98-105 entries this comment used to
# argue about are gone.
PUBLIC_ISSUE_NUMBER_FRONTIER = 133

RETIRED_PRIVATE_ISSUE_NUMBERS = (
    139, 142, 143, 145, 146, 147, 149, 150, 151, 152,
    154, 179, 187, 188, 196, 197, 234, 247, 248, 249,
    253, 286, 293, 294, 296, 298, 299, 300, 301, 303,
    304, 339, 346,
)

_reallocated = sorted(n for n in RETIRED_PRIVATE_ISSUE_NUMBERS
                      if n <= PUBLIC_ISSUE_NUMBER_FRONTIER)
if _reallocated:
    # A raise, not an assert: `python -O` strips asserts, and this guard exists to stop
    # the linter flagging live public objects.
    raise ValueError(
        "denylisted issue number(s) already reallocated by the public repository "
        f"(frontier {PUBLIC_ISSUE_NUMBER_FRONTIER}): {_reallocated}; remove them "
        "rather than flagging a live object"
    )

RETIRED_PRIVATE_ISSUE_URLS = re.compile(
    r"github\.com/Jstar269/nakagawa-recomp/(?:issues|pull)/(?:"
    + "|".join(str(n) for n in RETIRED_PRIVATE_ISSUE_NUMBERS)
    + r")\b"
)

HISTORICAL_EVIDENCE_DOCS = {
    "docs/STATUS_HISTORY.md",
    "docs/ROADMAP.md",
    "docs/AUDIO_OUTPUT_ACCEPTANCE_20260807.md",
    "docs/COVERAGE_LEDGER.md",
    "docs/provenance/INDEPENDENCE_BACKLOG.md",
}

MARKDOWN_LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)\)")
EXTERNAL_LINK_PREFIXES = ("http://", "https://", "mailto:", "tel:", "data:")


def get_tracked_markdown_files(repo_root: pathlib.Path = ROOT) -> list[pathlib.Path]:
    """Return tracked Markdown files, using Git as authority when available."""
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
    except (OSError, subprocess.SubprocessError):
        pass

    return sorted(
        path
        for path in repo_root.rglob("*.md")
        if ".git" not in path.relative_to(repo_root).parts
    )


def lint_readme(readme_path: pathlib.Path) -> list[str]:
    errors: list[str] = []
    if not readme_path.is_file():
        return errors

    text = readme_path.read_text(encoding="utf-8")
    for idx, line in enumerate(text.splitlines(), 1):
        for pattern in EPHEMERAL_RUN_PATTERNS:
            if pattern.search(line):
                errors.append(
                    f"README.md:{idx}: contains ephemeral CI run ID reference ('{line.strip()}')"
                )
        for pattern in README_DATED_STATUS_PATTERNS:
            if pattern.search(line):
                errors.append(
                    f"README.md:{idx}: contains volatile dated status claim ('{line.strip()}'); "
                    "move it to the current status dashboard or a dated private record"
                )
    return errors


def lint_doc_links_and_topology(
    doc_path: pathlib.Path, repo_root: pathlib.Path = ROOT
) -> list[str]:
    errors: list[str] = []
    if not doc_path.is_file():
        return errors

    try:
        rel_path = doc_path.relative_to(repo_root).as_posix()
    except ValueError:
        rel_path = doc_path.name

    is_historical_evidence = rel_path in HISTORICAL_EVIDENCE_DOCS
    text = doc_path.read_text(encoding="utf-8")

    in_fence = False
    root_resolved = repo_root.resolve()
    for idx, line in enumerate(text.splitlines(), 1):
        stripped = line.lstrip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            in_fence = not in_fence
            continue
        if not is_historical_evidence and RETIRED_PRIVATE_ISSUE_URLS.search(line):
            errors.append(f"{rel_path}:{idx}: contains dead private-era issue URL")
        for pattern in OBSOLETE_TOPOLOGY_PATTERNS:
            if pattern.search(line):
                errors.append(
                    f"{rel_path}:{idx}: contains obsolete private-repository topology statement"
                )
        if in_fence:
            continue
        for match in MARKDOWN_LINK_RE.finditer(line):
            raw_target = match.group(1).strip("<>")
            if raw_target.startswith("#") or raw_target.lower().startswith(
                EXTERNAL_LINK_PREFIXES
            ):
                continue
            target = unquote(raw_target.split("#", 1)[0])
            if not target:
                continue
            resolved = (doc_path.parent / target).resolve()
            try:
                resolved.relative_to(root_resolved)
            except ValueError:
                errors.append(
                    f"{rel_path}:{idx}: repository-relative link escapes the tree: {raw_target}"
                )
                continue
            if not resolved.exists():
                errors.append(
                    f"{rel_path}:{idx}: missing repository-relative link target: {raw_target}"
                )

    return errors


def run_all_doc_lints(repo_root: pathlib.Path = ROOT) -> list[str]:
    errors = lint_readme(repo_root / "README.md")
    for md_file in get_tracked_markdown_files(repo_root):
        errors.extend(lint_doc_links_and_topology(md_file, repo_root))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Documentation freshness and staleness linter")
    parser.add_argument("--repo-root", type=pathlib.Path, default=ROOT, help="Repository root path")
    args = parser.parse_args()

    errors = run_all_doc_lints(args.repo_root)
    if errors:
        print("Documentation Freshness Linter: FAIL", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1

    print("Documentation Freshness Linter: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
