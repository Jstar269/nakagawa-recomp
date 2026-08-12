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

# Content signatures are deliberately contextual.  Public SHA-256 values and
# ordinary PSP NIDs are allowed; the audit looks for credentials, private
# operational vocabulary, known proprietary paths, and binary file signatures.
# Keep these signatures high-confidence.  Generic checkout instructions use
# names such as ``place_game_here`` and ``oracle/`` as *absence* or routing
# examples; treating every such public placeholder as proprietary made the
# candidate audit report its own policy and documentation.  The history gate
# still catches those directories when they occur as actual historical paths
# (``FORBIDDEN_PREFIXES`` above), while content scanning is reserved for
# identifiers that denote a real title/private artifact or operational dump.
# Fragment the private-repository literal so this scanner does not report its
# own source blob as a finding.
PRIVATE_REPO_URL = re.compile(
    r"github\.com/" + r"Jstar269/" + r"nakagawa-recomp-" + r"history-private",
    re.IGNORECASE,
)
PRIVATE_OPERATIONAL_VOCABULARY = re.compile(
    r"(?:GAMEDATA\." + r"BDL|HST" + r"_PGD_VKEY(?:_HEX)?|"
    r"private[_ -](?:save|trace|dump)\s*(?:baseline|identity|path|location|hash|capture|evidence)\b)",
    re.IGNORECASE,
)
SUSPICIOUS_ENCODED = re.compile(r"^[A-Za-z0-9+/]{256,}={0,2}$")

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
        rel_path_clean = rel_path
        while rel_path_clean.startswith("./"):
            rel_path_clean = rel_path_clean[2:]
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


def _reachable_blob_ids(repo_root: Path) -> dict[str, str]:
    """Return each reachable blob object exactly once, with one observed path."""
    raw_objects = _git(["rev-list", "--objects", "--all"], repo_root=repo_root).splitlines()
    candidates: dict[str, str] = {}
    for line in raw_objects:
        parts = line.strip().split(None, 1)
        if len(parts) != 2:
            continue
        object_id, path = parts
        candidates.setdefault(object_id, path)
    if not candidates:
        return {}
    ids = list(candidates)
    checked = subprocess.run(
        ["git", "cat-file", "--batch-check"], cwd=repo_root,
        input=("".join(f"{object_id}\n" for object_id in ids)).encode("ascii"),
        capture_output=True, check=True,
    ).stdout.decode("ascii", errors="replace").splitlines()
    return {
        line.split()[0]: candidates[line.split()[0]]
        for line in checked
        if len(line.split()) == 3 and line.split()[1] == "blob"
    }


def _binary_magic(data: bytes) -> str | None:
    if data.startswith((b"~PSP", b"~SCE")):
        return "encrypted PSP module"
    if data.startswith(b"\x7fELF"):
        return "ELF executable"
    if data.startswith(b"\0PBP"):
        return "PSP PBP"
    if data[4:8] == b"PGF0" or data.startswith((b"PGF0", b"\0PGF")):
        return "PSP PGF font"
    if data.startswith((b"\x03\x02\x23\x07", b"\x07\x23\x02\x03")):
        return "SPIR-V shader bytecode"
    if len(data) >= 0x8006 and data[0x8001:0x8006] == b"CD001":
        return "ISO9660 image"
    return None


def audit_history_blob_contents(repo_root: Path = ROOT) -> list[HistoryFinding]:
    """Pass 2: scan every reachable blob's content once, not just its path."""
    findings: list[HistoryFinding] = []
    blobs = _reachable_blob_ids(repo_root)
    if not blobs:
        return findings
    proc = subprocess.Popen(["git", "cat-file", "--batch"], cwd=repo_root,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    assert proc.stdin and proc.stdout
    output, _ = proc.communicate(("".join(f"{object_id}\n" for object_id in blobs)).encode("ascii"))
    pos = 0
    for object_id, path in blobs.items():
        end = output.find(b"\n", pos)
        if end < 0:
            break
        header = output[pos:end].split()
        pos = end + 1
        if len(header) < 3 or header[1] != b"blob":
            continue
        size = int(header[2])
        data = output[pos:pos + size]
        pos += size + 1
        commit = f"blob:{object_id[:12]}"
        # Do not run text signatures over arbitrary binary blobs.  Replacement
        # decoding turns random bytes (for example public lookup tables) into
        # plausible path/token text and creates false positives.  Strict UTF-8
        # plus a NUL/control-byte check is conservative for source, JSON and
        # documentation while preserving the separate binary-magic checks.
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = None
        if text is not None and (
            b"\0" in data
            or any(ord(ch) < 32 and ch not in "\t\r\n" for ch in text)
        ):
            text = None

        if text is not None and (PRIVATE_KEY_HEADER.search(text) or API_TOKEN_PATTERN.search(text)):
            findings.append(HistoryFinding("DEFINITE_SECRET", "HISTORICAL_BLOB_SECRET", commit, path,
                                           "reachable blob contains a private-key or API-token signature [REDACTED]"))
        if text is not None and (WINDOWS_USER_PATH.search(text) or POSIX_USER_PATH.search(text) or MAC_USER_PATH.search(text) or WSL_USER_PATH.search(text) or UNC_PATH.search(text) or ONEDRIVE_PATH.search(text) or TEMP_PATH.search(text)):
            findings.append(HistoryFinding("PRIVACY_METADATA", "HISTORICAL_BLOB_LOCAL_PATH", commit, path,
                                           "reachable blob contains a private local path [REDACTED]"))
        if text is not None and PRIVATE_REPO_URL.search(text):
            findings.append(HistoryFinding("PRIVACY_METADATA", "HISTORICAL_BLOB_PRIVATE_REPO", commit, path,
                                           "reachable blob names a private repository [REDACTED]"))
        if text is not None and PRIVATE_OPERATIONAL_VOCABULARY.search(text):
            findings.append(HistoryFinding("PROPRIETARY_ARTIFACT", "HISTORICAL_BLOB_PRIVATE_VOCABULARY", commit, path,
                                           "reachable blob contains private/game-derived operational vocabulary [REDACTED]"))
        magic = _binary_magic(data)
        if magic:
            findings.append(HistoryFinding("PROPRIETARY_ARTIFACT", "HISTORICAL_BLOB_MAGIC", commit, path,
                                           f"reachable blob contains forbidden/proprietary magic: {magic}"))
        if text is not None and size >= 1024 * 1024 and SUSPICIOUS_ENCODED.fullmatch(text.strip()):
            findings.append(HistoryFinding("PROPRIETARY_ARTIFACT", "HISTORICAL_BLOB_ENCODED_PAYLOAD", commit, path,
                                           "large reachable blob is a suspicious encoded payload"))
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
    blob_findings = audit_history_blob_contents(repo_root)
    large_blobs = audit_large_blobs(repo_root)

    all_findings = tree_findings + metadata_findings + blob_findings

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
