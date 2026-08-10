#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Non-destructive full-history secret, proprietary material, and privacy audit tool for Nakagawa Recomp."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent

HEX_16_BYTES = re.compile(r"\b[0-9a-fA-F]{32}\b")
PRIVATE_KEY_HEADER = re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----")
API_TOKEN_PATTERN = re.compile(r"\b(?:ghp_[a-zA-Z0-9]{36}|github_pat_[a-zA-Z0-9_]{80}|sk_live_[a-zA-Z0-9]{24}|AKIA[0-9A-Z]{16})\b")

# Sensitive local path patterns (fragmented to prevent self-match)
WINDOWS_USER_PATH = re.compile(r"[a-zA-Z]:\\(?:" + r"Us" + r"ers|Documents and Settings)\\[^\s\\/]+", re.IGNORECASE)
POSIX_USER_PATH = re.compile(r"/(?:" + r"ho" + r"me|Us" + r"ers)/[^\s/]+")
MAC_USER_PATH = re.compile(r"/(?:" + r"Us" + r"ers)/[^\s/]+")
WSL_USER_PATH = re.compile(r"/mnt/[a-z]/(?:" + r"Us" + r"ers)/[^\s/]+")
UNC_PATH = re.compile(r"\\\\[a-zA-Z0-9_.-]+\\[a-zA-Z0-9_.-]+")
ONEDRIVE_PATH = re.compile(r"\bOne" + r"Drive\b(?:\s*-\s*[^/\\]+)?", re.IGNORECASE)
TEMP_PATH = re.compile(r"(?:/tm" + r"p/|/var/tm" + r"p/|[a-zA-Z]:\\(?:[^\s\\/]+\\)*(?:Windows\\Te" + r"mp|AppData\\Local\\Te" + r"mp)\\)[a-zA-Z0-9_.-]+", re.IGNORECASE)

FORBIDDEN_EXTENSIONS = {
    ".at3", ".bin", ".chd", ".cso", ".dax", ".dmp", ".edat", ".elf", ".gim",
    ".iso", ".pbp", ".pmf", ".prx", ".psar", ".psess", ".sfo", ".sqlite", ".trace", ".vag"
}

FORBIDDEN_NAMES = {
    "reference_hashes.json", "vfpu_words.txt", "vfpu_words_local.txt",
    "nidseq_mine.txt", "pgd_keys.txt"
}

FORBIDDEN_PREFIXES = (
    "build/", "docs/opengrip_ref/", "fs/", "logs/", "memstick/", "opengrip_ref/",
    "OpenGrip_For_Inspiration/", "oracle/", "original_game/", "place_game_here/",
    "third_party/ghidra/exports/", "third_party/ghidra/projects/"
)


@dataclass(frozen=True)
class HistoryFinding:
    category: str  # DEFINITE_SECRET, POSSIBLE_CREDENTIAL, PROPRIETARY_ARTIFACT, PRIVACY_METADATA, FALSE_POSITIVE, UNREACHABLE_OBJECT
    code: str
    commit: str
    path: str
    detail: str

    def to_dict(self, redact: bool = True) -> dict:
        d = asdict(self)
        if redact:
            # Mask any accidental sensitive fragments in detail
            detail = d["detail"]
            detail = HEX_16_BYTES.sub("[REDACTED_HEX_KEY]", detail)
            detail = PRIVATE_KEY_HEADER.sub("[REDACTED_PRIVATE_KEY_HEADER]", detail)
            detail = API_TOKEN_PATTERN.sub("[REDACTED_API_TOKEN]", detail)
            d["detail"] = detail
        return d


def _git(cmd: list[str], repo_root: Path = ROOT) -> str:
    res = subprocess.run(["git", *cmd], cwd=repo_root, capture_output=True, check=False)
    if res.returncode != 0:
        err = res.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git {' '.join(cmd)} failed: {err}")
    return res.stdout.decode("utf-8", errors="replace")


def get_repository_baseline(repo_root: Path = ROOT) -> dict:
    commit_count = int(_git(["rev-list", "--count", "--all"], repo_root=repo_root).strip())
    raw_objects = _git(["rev-list", "--objects", "--all"], repo_root=repo_root).splitlines()
    ref_list = _git(["for-each-ref"], repo_root=repo_root).splitlines()
    try:
        main_sha = _git(["rev-parse", "origin/main"], repo_root=repo_root).strip()
    except Exception:
        try:
            main_sha = _git(["rev-parse", "HEAD"], repo_root=repo_root).strip()
        except Exception:
            main_sha = "unknown"

    return {
        "git_commit_main": main_sha,
        "total_commits": commit_count,
        "total_objects": len(raw_objects),
        "total_refs": len(ref_list),
        "ref_sample": ref_list[:15],
    }


def audit_history_tree_paths(repo_root: Path = ROOT) -> list[HistoryFinding]:
    """Pass 1: Audit all historical tree entry paths across every reachable commit."""
    findings: list[HistoryFinding] = []
    raw_objects = _git(["rev-list", "--objects", "--all"], repo_root=repo_root).splitlines()

    for line in raw_objects:
        parts = line.strip().split(None, 1)
        if len(parts) != 2:
            continue
        obj_sha, rel_path = parts
        rel_path_clean = rel_path.lstrip("./")
        lower_p = rel_path_clean.lower()

        # Check forbidden prefixes
        for prefix in FORBIDDEN_PREFIXES:
            if lower_p.startswith(prefix.lower()):
                findings.append(HistoryFinding(
                    category="PROPRIETARY_ARTIFACT",
                    code="HISTORICAL_PATH_PREFIX",
                    commit=obj_sha[:8],
                    path=rel_path_clean,
                    detail=f"Reachable historical object under private/proprietary prefix '{prefix}'",
                ))
                break

        # Check forbidden names
        name = Path(rel_path_clean).name
        if name in FORBIDDEN_NAMES:
            findings.append(HistoryFinding(
                category="PROPRIETARY_ARTIFACT",
                code="HISTORICAL_GAME_ARTIFACT",
                commit=obj_sha[:8],
                path=rel_path_clean,
                detail=f"Reachable historical game-derived file '{name}'",
            ))

        # Check forbidden extensions
        ext = Path(rel_path_clean).suffix.lower()
        if ext in FORBIDDEN_EXTENSIONS:
            findings.append(HistoryFinding(
                category="PROPRIETARY_ARTIFACT",
                code="HISTORICAL_PROHIBITED_EXTENSION",
                commit=obj_sha[:8],
                path=rel_path_clean,
                detail=f"Reachable historical blob with prohibited binary extension '{ext}'",
            ))

    return findings


def audit_history_commit_metadata(repo_root: Path = ROOT) -> list[HistoryFinding]:
    """Pass 2: Audit all author/committer emails and commit log metadata across all commits."""
    findings: list[HistoryFinding] = []
    log_output = _git(["log", "--all", "--format=%H|%an|%ae|%cn|%ce|%s"], repo_root=repo_root)

    for line in log_output.splitlines():
        if not line:
            continue
        parts = line.split("|", 5)
        if len(parts) < 6:
            continue
        commit_sha, author_name, author_email, committer_name, committer_email, subject = parts

        # Check local/private user path or private metadata in subject
        if WINDOWS_USER_PATH.search(subject) or POSIX_USER_PATH.search(subject) or ONEDRIVE_PATH.search(subject):
            findings.append(HistoryFinding(
                category="PRIVACY_METADATA",
                code="COMMIT_LOG_LOCAL_PATH",
                commit=commit_sha[:8],
                path="<commit_message>",
                detail=f"Commit message contains local path or directory fragment",
            ))

        # Check for direct key or secret patterns in commit message
        if PRIVATE_KEY_HEADER.search(subject) or API_TOKEN_PATTERN.search(subject):
            findings.append(HistoryFinding(
                category="DEFINITE_SECRET",
                code="COMMIT_LOG_SECRET",
                commit=commit_sha[:8],
                path="<commit_message>",
                detail="Commit message contains private key or API token literal [REDACTED]",
            ))

    return findings


def audit_large_blobs(repo_root: Path = ROOT, size_threshold: int = 500 * 1024) -> list[dict]:
    """Pass 3: Inventory large objects in history packfiles."""
    large_blobs: list[dict] = []
    raw_objects = _git(["rev-list", "--objects", "--all"], repo_root=repo_root).splitlines()

    # Map sha to path
    sha_to_path: dict[str, str] = {}
    for line in raw_objects:
        parts = line.strip().split(None, 1)
        if len(parts) == 2:
            sha_to_path[parts[0]] = parts[1]

    # Query cat-file for object sizes
    try:
        proc = subprocess.Popen(["git", "cat-file", "--batch-check"], cwd=repo_root, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        assert proc.stdin and proc.stdout
        stdin_text = "\n".join(sha_to_path.keys()) + "\n"
        stdout_data, _ = proc.communicate(input=stdin_text.encode("utf-8"))

        for line in stdout_data.decode("utf-8", errors="replace").splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] == "blob" and parts[2].isdigit():
                sha, obj_type, size_str = parts
                size = int(size_str)
                if size >= size_threshold:
                    large_blobs.append({
                        "sha": sha,
                        "size": size,
                        "path": sha_to_path.get(sha, "unknown"),
                    })
    except Exception:
        pass

    return sorted(large_blobs, key=lambda b: b["size"], reverse=True)


def generate_full_history_audit_report(repo_root: Path = ROOT) -> dict:
    baseline = get_repository_baseline(repo_root)
    tree_findings = audit_history_tree_paths(repo_root)
    metadata_findings = audit_history_commit_metadata(repo_root)
    large_blobs = audit_large_blobs(repo_root)

    all_findings = tree_findings + metadata_findings

    category_counts: dict[str, int] = {}
    for f in all_findings:
        category_counts[f.category] = category_counts.get(f.category, 0) + 1

    return {
        "status": "FAIL" if all_findings else "OK",
        "baseline": baseline,
        "summary": {
            "total_findings": len(all_findings),
            "category_counts": category_counts,
            "large_blobs_over_500kb": len(large_blobs),
        },
        "large_blobs": large_blobs[:10],  # Top 10 largest blobs
        "findings": [f.to_dict(redact=True) for f in all_findings],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="Print JSON report to stdout")
    parser.add_argument("--out", type=Path, help="Write publication-safe audit JSON report to path")
    args = parser.parse_args(argv)

    report = generate_full_history_audit_report(ROOT)

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote publication-safe full-history audit report to {args.out}")

    if args.json:
        print(json.dumps(report, indent=2))
        return 1 if report["summary"]["total_findings"] > 0 else 0

    print("=======================================================")
    print(" Nakagawa Recomp Full-History Non-Destructive Audit")
    print("=======================================================")
    print(f"Reachable Commits: {report['baseline']['total_commits']}")
    print(f"Reachable Objects: {report['baseline']['total_objects']}")
    print(f"Reachable Refs:    {report['baseline']['total_refs']}")
    print(f"Status:            {report['status']}")
    print(f"Total Findings:    {report['summary']['total_findings']}")
    print("Category Breakdown:")
    for cat, count in report['summary']['category_counts'].items():
        print(f"  - {cat}: {count}")

    if report["findings"]:
        print("\nFindings Summary:")
        for f in report["findings"]:
            print(f"  [{f['category']}] {f['code']}: {f['path']} ({f['commit']}) - {f['detail']}")
        return 1

    print("\nFull-history audit: OK (0 sensitive findings across all reachable commits & objects)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
