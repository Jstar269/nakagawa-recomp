#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Public GitHub Issue & PR Link Verification Tool.

Queries GitHub API for Jstar269/nakagawa-recomp and validates every issue/PR link
found across tracked documentation files. Optional networked audit.
"""

from __future__ import annotations

import json
import pathlib
import re
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Live public issues/PRs map: number -> {type, state, title, url}
def fetch_public_issues_map() -> dict[int, dict]:
    url = "https://api.github.com/repos/Jstar269/nakagawa-recomp/issues?state=all&per_page=100"
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (Python)"})
    issues_map = {}
    try:
        with urllib.request.urlopen(req) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            for item in data:
                num = item["number"]
                is_pr = "pull_request" in item
                issues_map[num] = {
                    "number": num,
                    "type": "PR" if is_pr else "Issue",
                    "state": item["state"],
                    "title": item["title"],
                    "url": item["html_url"],
                }
    except Exception as e:
        print(f"Warning: Failed to fetch live issues from GitHub API: {e}", file=sys.stderr)
    return issues_map


def audit_docs() -> list[tuple]:
    public_map = fetch_public_issues_map()
    if not public_map:
        print("Live GitHub API unavailable; skipping live reference audit.")
        return []

    full_url_pat = re.compile(
        r"https://github\.com/Jstar269/nakagawa-recomp/(issues|pull)/(\d+)"
    )

    md_files = list(ROOT.glob("*.md")) + list((ROOT / "docs").glob("**/*.md"))

    findings = []

    for doc_path in sorted(md_files):
        rel = doc_path.relative_to(ROOT).as_posix()
        text = doc_path.read_text(encoding="utf-8")

        for idx, line in enumerate(text.splitlines(), 1):
            for m in full_url_pat.finditer(line):
                num = int(m.group(2))
                if num in public_map:
                    obj = public_map[num]
                    status_str = f"PUBLIC {obj['state'].upper()} {obj['type']}"
                    findings.append((rel, idx, m.group(0), num, status_str, obj["title"], True))
                else:
                    findings.append((rel, idx, m.group(0), num, "HISTORICAL PRIVATE NUMBER (404 on public)", "", False))

    print("\n--- PUBLIC ISSUE LINK AUDIT RESULTS ---")
    live_count = 0
    private_count = 0
    for rel, idx, match_str, num, status, title, is_live in findings:
        if is_live:
            live_count += 1
            print(f"[LIVE] {rel}:{idx} -> {match_str} (#{num}: {title} [{status}])")
        else:
            private_count += 1
            print(f"[HISTORICAL/404] {rel}:{idx} -> {match_str} (Historical Private #{num})")

    print(f"\nTotal references found: {len(findings)} (Live public: {live_count}, Historical private / 404: {private_count})")
    return findings


if __name__ == "__main__":
    audit_docs()
