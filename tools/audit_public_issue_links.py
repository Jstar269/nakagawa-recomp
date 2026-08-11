#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Verify public GitHub Issue and PR references in tracked documentation.

The audit has two purposes:

* full ``github.com/Jstar269/nakagawa-recomp/{issues,pull}/N`` URLs are checked
  in every tracked Markdown document;
* shorthand ``#N`` references are checked in explicitly current-facing documents.

Historical evidence documents may intentionally preserve pre-export tracker numbers. A 404 in
one of those documents is reported as historical evidence rather than silently reclassified as a
current public issue. ``--strict`` additionally fails when the live GitHub query itself cannot run.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Current-facing documents where shorthand #N references must be verified live.
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

# Role-aware historical evidence files that preserve dated tracker references.
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
STATE_LABEL_PAT = re.compile(r"\[(OPEN ISSUE|CLOSED ISSUE|OPEN PR|CLOSED PR|MERGED PR)\]")
TRACKER_SECTION = "## Public tracker and implementation references"


def get_tracked_markdown_files(repo_root: pathlib.Path = ROOT) -> list[pathlib.Path]:
    """Return tracked Markdown files, using Git as the authority when available."""
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

    # Test/standalone fallback when Git is unavailable. Production repository runs use git ls-files.
    return sorted(
        path
        for path in repo_root.rglob("*.md")
        if ".git" not in path.relative_to(repo_root).parts
    )


def fetch_public_issues_map() -> dict[int, dict] | None:
    """Fetch all public Issues/PRs, including merged-state metadata, with pagination."""
    issues_map: dict[int, dict] = {}
    page = 1
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "nakagawa-recomp-doc-link-audit",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"

    while True:
        url = (
            "https://api.github.com/repos/Jstar269/nakagawa-recomp/issues"
            f"?state=all&per_page=100&page={page}"
        )
        req = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except Exception as err:
            print(f"Network query to GitHub API failed: {err}", file=sys.stderr)
            return None

        if not isinstance(data, list):
            print("GitHub API returned an unexpected non-list response", file=sys.stderr)
            return None
        if not data:
            break

        for item in data:
            num = item["number"]
            pull_meta = item.get("pull_request")
            is_pr = pull_meta is not None
            issues_map[num] = {
                "number": num,
                "is_pr": is_pr,
                "type": "PR" if is_pr else "Issue",
                "state": item["state"],
                "merged_at": pull_meta.get("merged_at") if is_pr else None,
                "title": item["title"],
                "url": item["html_url"],
            }

        if len(data) < 100:
            break
        page += 1

    return issues_map


def public_status_label(obj: dict) -> str:
    """Return the canonical tracker label for a live public object."""
    if not obj["is_pr"]:
        return "OPEN ISSUE" if obj["state"] == "open" else "CLOSED ISSUE"
    if obj["state"] == "open":
        return "OPEN PR"
    return "MERGED PR" if obj.get("merged_at") else "CLOSED PR"


def audit_markdown_files(
    repo_root: pathlib.Path, issues_map: dict[int, dict]
) -> list[tuple[str, int, str, str, bool]]:
    """Audit tracked Markdown for dead, mismatched, or stale public tracker references."""
    findings: list[tuple[str, int, str, str, bool]] = []

    for doc_path in get_tracked_markdown_files(repo_root):
        try:
            rel = doc_path.relative_to(repo_root).as_posix()
        except ValueError:
            rel = doc_path.name

        is_current_doc = rel in CURRENT_FACING_DOCS
        is_historical_doc = rel in HISTORICAL_EVIDENCE_DOCS
        in_tracker_section = False

        text = doc_path.read_text(encoding="utf-8")
        for idx, line in enumerate(text.splitlines(), 1):
            if line.startswith("## "):
                in_tracker_section = rel == "ISSUES.md" and line.strip() == TRACKER_SECTION

            url_matches = list(FULL_URL_PAT.finditer(line))
            full_nums = {int(match.group(2)) for match in url_matches}

            for match in url_matches:
                url_type, num_str = match.group(1), match.group(2)
                num = int(num_str)
                raw = match.group(0)

                if num not in issues_map:
                    if is_historical_doc:
                        findings.append(
                            (
                                rel,
                                idx,
                                raw,
                                f"HISTORICAL EVIDENCE REFERENCE #{num} (404 on public)",
                                True,
                            )
                        )
                    else:
                        findings.append(
                            (
                                rel,
                                idx,
                                raw,
                                f"DEAD / UNRESOLVED PUBLIC REFERENCE #{num}",
                                False,
                            )
                        )
                    continue

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

            # Shorthand references are only authoritative in explicitly current-facing documents.
            if is_current_doc:
                for match in SHORTHAND_PAT.finditer(line):
                    num = int(match.group(1))
                    if num in full_nums:
                        # Link text such as "Issue #23" is already validated by its full URL.
                        continue
                    raw = f"#{num}"
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

            # The current dashboard has explicit state labels; verify those labels, not just existence.
            if in_tracker_section and url_matches:
                labels = STATE_LABEL_PAT.findall(line)
                if len(labels) != len(url_matches):
                    findings.append(
                        (
                            rel,
                            idx,
                            line.strip(),
                            "TRACKER STATUS LABEL COUNT MISMATCH",
                            False,
                        )
                    )
                else:
                    for match, label in zip(url_matches, labels, strict=True):
                        num = int(match.group(2))
                        obj = issues_map.get(num)
                        if obj is None:
                            continue
                        expected = public_status_label(obj)
                        if label != expected:
                            findings.append(
                                (
                                    rel,
                                    idx,
                                    f"#{num} [{label}]",
                                    f"STALE TRACKER STATUS: expected [{expected}]",
                                    False,
                                )
                            )

    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description="Public GitHub Issue & PR link verification")
    parser.add_argument("--repo-root", type=pathlib.Path, default=ROOT, help="Repository root path")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Fail if the live GitHub query cannot run; dead current references always fail",
    )
    args = parser.parse_args()

    issues_map = fetch_public_issues_map()
    if issues_map is None:
        if args.strict:
            print("Public Link Audit: FAIL (network query failed in --strict mode)", file=sys.stderr)
            return 1
        print("Public Link Audit: SKIPPED (GitHub API unavailable)", file=sys.stderr)
        return 0

    findings = audit_markdown_files(args.repo_root, issues_map)
    failed = [finding for finding in findings if not finding[4]]
    historical = [finding for finding in findings if finding[3].startswith("HISTORICAL EVIDENCE")]

    print("\n--- PUBLIC ISSUE & PR LINK AUDIT ---")
    print(f"Tracked Markdown files checked: {len(get_tracked_markdown_files(args.repo_root))}")
    print(f"Total issue/PR references evaluated: {len(findings)}")
    print(f"Historical 404 references preserved: {len(historical)}")

    if failed:
        print(f"\nDEAD / MISMATCHED / STALE REFERENCES DETECTED ({len(failed)}):", file=sys.stderr)
        for rel, idx, raw, desc, _ in failed:
            print(f"  [FAIL] {rel}:{idx} -> {raw} ({desc})", file=sys.stderr)
        print("\nPublic Link Audit: FAIL", file=sys.stderr)
        return 1

    print(
        "Public Link Audit: PASS "
        "(current-facing references are live/type-correct; historical evidence remains explicitly scoped)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
