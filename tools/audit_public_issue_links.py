#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Public GitHub Issue & PR Link Verification Tool.

Audits tracked Markdown files for GitHub issue/PR links (both full URLs and shorthand #N
in current-facing documents) against live GitHub API state for Jstar269/nakagawa-recomp.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Current-facing documents where shorthand #N references must be verified live
CURRENT_FACING_DOCS = {
    "README.md",
    "AGENTS.md",
    "ISSUES.md",
    "CONTRIBUTING.md",
    "NOTICE.md",
    "assets/README.md",
    "assets/titles/README.md",
    "docs/README.md",
    "docs/SETUP.md",
    "docs/CI.md",
    "docs/PUBLICATION_READINESS.md",
    "docs/PUBLIC_SOURCE_PROFILE.md",
    "docs/DCO_POLICY.md",
    "docs/KEY_HISTORY_SCRUB.md",
    "docs/PGD_AMCTRL_REVIEW_PACKET.md",
    "docs/PGF_LICENSE_REVIEW_PACKET.md",
    "docs/PGD_KEYS.md",
    "font/README.md",
    "interface/README.md",
    "src/rt/gpu_sdl3vk/README.md",
    "tools/README.md",
}

# Role-aware historical evidence files that preserve dated historical issue URLs
HISTORICAL_EVIDENCE_DOCS = {
    "docs/STATUS_HISTORY.md",
    "docs/ROADMAP.md",
    "docs/HARDWARE_ORACLE.md",
    "docs/IMPORT_AUDIT.md",
    "docs/NEXT_SESSION.md",
    "docs/PHASE5_HARDWARE_EVIDENCE.md",
    "docs/provenance/INDEPENDENCE_BACKLOG.md",
    "docs/PSP_INTR_WAITS_MATRIX.md",
    "docs/AUDIO_OUTPUT_ACCEPTANCE_20260807.md",
    "docs/COVERAGE_LEDGER.md",
    "docs/TOOLCHAIN_BASELINE_2026-08.md",
}

FULL_URL_PAT = re.compile(
    r"https://github\.com/Jstar269/nakagawa-recomp/(issues|pull)/(\d+)\b"
)
SHORTHAND_PAT = re.compile(r"(?<![A-Fa-f0-9_#])#(\d+)\b")


def get_tracked_markdown_files(repo_root: pathlib.Path = ROOT) -> list[pathlib.Path]:
    """Return all tracked .md files in the repository using git ls-files."""
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

    # Fallback if git command is unavailable
    return sorted(repo_root.glob("*.md")) + sorted((repo_root / "docs").glob("**/*.md"))


def fetch_public_issues_map() -> dict[int, dict] | None:
    """Fetch and paginate all issues/PRs from GitHub API for Jstar269/nakagawa-recomp.

    Returns dict mapping number -> {number, is_pr, state, title, url} or None if network fails.
    """
    issues_map: dict[int, dict] = {}
    page = 1
    headers = {"User-Agent": "Mozilla/5.0 (Python)"}

    while True:
        url = f"https://api.github.com/repos/Jstar269/nakagawa-recomp/issues?state=all&per_page=100&page={page}"
        req = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                if not data or not isinstance(data, list):
                    break
                for item in data:
                    num = item["number"]
                    is_pr = "pull_request" in item
                    issues_map[num] = {
                        "number": num,
                        "is_pr": is_pr,
                        "type": "PR" if is_pr else "Issue",
                        "state": item["state"],
                        "title": item["title"],
                        "url": item["html_url"],
                    }
                if len(data) < 100:
                    break
                page += 1
        except Exception as err:
            print(f"Network query to GitHub API failed: {err}", file=sys.stderr)
            return None

    return issues_map


def audit_markdown_files(
    repo_root: pathlib.Path, issues_map: dict[int, dict]
) -> list[tuple[str, int, str, str, bool]]:
    """Audit tracked Markdown files for dead or mismatched GitHub links.

    Returns list of tuples: (rel_path, line_no, raw_match, description, is_ok)
    """
    findings = []
    md_files = get_tracked_markdown_files(repo_root)

    for doc_path in md_files:
        try:
            rel = doc_path.relative_to(repo_root).as_posix()
        except ValueError:
            rel = doc_path.name

        is_current_doc = rel in CURRENT_FACING_DOCS or doc_path.name in CURRENT_FACING_DOCS
        is_historical_doc = rel in HISTORICAL_EVIDENCE_DOCS

        text = doc_path.read_text(encoding="utf-8")
        for idx, line in enumerate(text.splitlines(), 1):
            # Check full URLs
            for m in FULL_URL_PAT.finditer(line):
                url_type, num_str = m.group(1), m.group(2)
                num = int(num_str)
                raw = m.group(0)

                if num not in issues_map:
                    if is_historical_doc:
                        findings.append(
                            (rel, idx, raw, f"HISTORICAL EVIDENCE REFERENCE #{num} (404 on public)", True)
                        )
                    else:
                        findings.append(
                            (rel, idx, raw, f"DEAD / UNRESOLVED PUBLIC REFERENCE #{num}", False)
                        )
                else:
                    obj = issues_map[num]
                    expected_is_pr = url_type == "pull"
                    if obj["is_pr"] != expected_is_pr:
                        actual_type = "PR" if obj["is_pr"] else "Issue"
                        findings.append(
                            (
                                rel,
                                idx,
                                raw,
                                f"TYPE MISMATCH: URL says '{url_type}' but #{num} is a public {actual_type}",
                                False,
                            )
                        )
                    else:
                        findings.append(
                            (
                                rel,
                                idx,
                                raw,
                                f"LIVE PUBLIC {obj['type']} (#{num}: {obj['title']} [{obj['state']}])",
                                True,
                            )
                        )

            # Check shorthand #N in current-facing documents
            if is_current_doc:
                for m in SHORTHAND_PAT.finditer(line):
                    num = int(m.group(1))
                    raw = f"#{num}"
                    # Ignore if this shorthand is part of a URL (e.g. github.com link)
                    if "github.com/" in line and f"/{num}" in line:
                        continue

                    if num not in issues_map:
                        findings.append(
                            (
                                rel,
                                idx,
                                raw,
                                f"DEAD / UNRESOLVED SHORTHAND PUBLIC REFERENCE #{num} in current doc {rel}",
                                False,
                            )
                        )
                    else:
                        obj = issues_map[num]
                        findings.append(
                            (
                                rel,
                                idx,
                                raw,
                                f"LIVE SHORTHAND #{num} -> {obj['type']} ({obj['title']})",
                                True,
                            )
                        )

    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description="Public GitHub Issue & PR Link Verification Tool")
    parser.add_argument("--repo-root", type=pathlib.Path, default=ROOT, help="Repository root path")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Fail (exit 1) if network query fails or GitHub API is unreachable",
    )
    args = parser.parse_args()

    issues_map = fetch_public_issues_map()
    if issues_map is None:
        if args.strict:
            print("Public Link Audit: FAIL (Network query failed in --strict mode)", file=sys.stderr)
            return 1
        else:
            print("Public Link Audit: SKIPPED (GitHub API network query unavailable)", file=sys.stderr)
            return 0

    findings = audit_markdown_files(args.repo_root, issues_map)

    failed_findings = [f for f in findings if not f[4]]

    print(f"\n--- PUBLIC ISSUE & PR LINK AUDIT ---")
    print(f"Tracked files checked: {len(get_tracked_markdown_files(args.repo_root))}")
    print(f"Total issue/PR references evaluated: {len(findings)}")

    if failed_findings:
        print(f"\nDEAD / MISMATCHED REFERENCES DETECTED ({len(failed_findings)}):", file=sys.stderr)
        for rel, idx, raw, desc, _ in failed_findings:
            print(f"  [FAIL] {rel}:{idx} -> {raw} ({desc})", file=sys.stderr)
        print("\nPublic Link Audit: FAIL", file=sys.stderr)
        return 1

    print("Public Link Audit: PASS (All public issue/PR references verified live)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
