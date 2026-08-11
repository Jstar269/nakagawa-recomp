#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Audit the prospective, tracked Git tree or candidate release directory before publication."""

from __future__ import annotations

import argparse
import ast
import csv
from dataclasses import asdict, dataclass
import hashlib
import json
import os
import fnmatch
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import unicodedata

ROOT = Path(__file__).resolve().parent.parent

TOOL_VERSION = "0.3.0"
SCHEMA_VERSION = "1.1.0"

# Which bytes an audit actually reads. The path set always comes from Git (or from a
# materialized candidate directory); this selects the *content* behind each path, and
# the three answers are genuinely different trees:
#
#   index      the staged blob behind every tracked path. This is what a commit would
#              publish, so it is the correct source for pre-commit / pre-push and for
#              the release-export gate, and unstaged working-tree edits are invisible
#              to it by design (see test_hermetic_git_index_vs_working_tree_contract).
#   worktree   the bytes on disk right now. This is what a developer means by "audit my
#              tree", so it is the correct source for an interactive aggregate gate.
#   candidate  a materialized export directory with no Git index to consult.
#
# A run must say which one it used: reporting "publication audit: OK" without naming the
# content source is how a green aggregate gate came to describe bytes nobody had checked.
CONTENT_INDEX = "index"
CONTENT_WORKTREE = "worktree"
CONTENT_CANDIDATE = "candidate"

FORBIDDEN_PREFIXES = (
    "build/",
    "docs/opengrip_ref/",
    "fs/",
    "logs/",
    "memstick/",
    "opengrip_ref/",
    "OpenGrip_For_Inspiration/",
    "oracle/",
    "original_game/",
    "place_game_here/",
    "third_party/ghidra/exports/",
    "third_party/ghidra/projects/",
)
FORBIDDEN_EXTENSIONS = {
    ".at3",
    ".bin",
    ".chd",
    ".cso",
    ".dax",
    ".dmp",
    ".edat",
    ".elf",
    ".gim",
    ".iso",
    ".pbp",
    ".pmf",
    ".prx",
    ".psar",
    ".psess",
    ".sfo",
    ".sqlite",
    ".trace",
    ".vag",
}
REQUIRED_PATHS = (
    "LICENSE",
    "NOTICE.md",
    "README.md",
    "AGENTS.md",
)
SOURCE_EXTENSIONS = {".c", ".h", ".py", ".sh"}
KEY_NAME = re.compile(r"(?:vkey|seed|iv|secret|token)", re.IGNORECASE)
HEX_16_BYTES = re.compile(r"^[0-9a-fA-F]{32}$")

WINDOWS_USER_PATH = re.compile(
    r"[a-zA-Z]:\\(?:" + r"Us" + r"ers|Documents and Settings)\\[^\s\\/]+",
    re.IGNORECASE,
)
POSIX_USER_PATH = re.compile(r"/(?:" + r"ho" + r"me|Us" + r"ers)/[^\s/]+")
MAC_USER_PATH = re.compile(r"/(?:" + r"Us" + r"ers)/[^\s/]+")
WSL_USER_PATH = re.compile(r"/mnt/[a-z]/(?:" + r"Us" + r"ers)/[^\s/]+")
UNC_PATH = re.compile(r"\\\\[a-zA-Z0-9_.-]+\\[a-zA-Z0-9_.-]+")
ONEDRIVE_PATH = re.compile(
    r"\bOne" + r"Drive\b(?:\s*-\s*[^/\\]+)?",
    re.IGNORECASE,
)
TEMP_PATH = re.compile(
    r"(?:/tm" + r"p/|/var/tm" + r"p/|[a-zA-Z]:\\(?:[^\s\\/]+\\)*(?:Windows\\Te" + r"mp|AppData\\Local\\Te" + r"mp)\\)[a-zA-Z0-9_.-]+",
    re.IGNORECASE,
)

ACTION_USE = re.compile(r"uses:\s*([^\s#]+)")
FULL_SHA_ACTION = re.compile(r"^[a-zA-Z0-9_.-]+/[a-zA-Z0-9_.-]+(?:/[a-zA-Z0-9_.-]+)*@[0-9a-fA-F]{40}$")
MARKDOWN_LINK = re.compile(r"\[[^\]]+\]\(([^)]+)\)")

DANGEROUS_UNICODE_CHARS = {
    "\u202a", "\u202b", "\u202c", "\u202d", "\u202e",  # Bidi overrides
    "\u2066", "\u2067", "\u2068", "\u2069",          # Bidi isolates
    "\u200b", "\u200c", "\u200d",                    # Zero-width chars
    "\ufeff",                                         # BOM
}

RESERVED_WIN_NAMES = {
    "con", "prn", "aux", "nul",
    "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
    "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
}

# VCS metadata directories that are scaffolding of a materialized candidate
# (e.g. build_public_export.py initializes a fresh Git repository in the
# export) rather than release content. They are never tracked by Git itself,
# so they must not be scanned as candidate files.
_VCS_METADATA_DIRS = {".git", ".hg", ".svn"}


@dataclass(frozen=True)
class Finding:
    code: str
    path: str
    detail: str


@dataclass(frozen=True)
class GitEntry:
    mode: str
    sha: str
    stage: str
    path: str
    kind: str
    working_mode: str = ""


@dataclass
class FileSemantics:
    path: str
    mode: str
    working_mode: str
    index_sha: str
    size: int
    sha256: str
    kind: str
    text_binary: str
    magic: str
    provenance_class: str
    license_expression: str
    notice_owner: str
    generated_source: str
    release_disposition: str
    public_scope_included: bool
    status: str
    findings: list[str]


def is_git_lfs_pointer(text: str) -> bool:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if not lines or lines[0] != "version https://git-lfs.github.com/spec/v1":
        return False
    has_oid = any(line.startswith("oid sha256:") and len(line.split(":")[-1].strip()) == 64 for line in lines)
    has_size = any(line.startswith("size ") and line.split(" ", 1)[-1].strip().isdigit() for line in lines)
    return has_oid and has_size


def parse_gitleaks_report(report_path: Path) -> list[Finding]:
    if not report_path.is_file():
        return [Finding("GITLEAKS", str(report_path), "Gitleaks report file not found")]
    findings = []
    try:
        with report_path.open("r", encoding="utf-8") as stream:
            leaks = json.load(stream)
            if isinstance(leaks, list):
                for leak in leaks:
                    rule_id = leak.get("RuleID", leak.get("ruleID", "unknown-rule"))
                    file_p = leak.get("File", leak.get("file", "unknown-file"))
                    start_line = leak.get("StartLine", leak.get("startLine", 0))
                    commit_val = leak.get("Commit", leak.get("commit", "uncommitted"))
                    detail = f"Rule: {rule_id}; Line: {start_line}; Commit: {commit_val} [REDACTED]"
                    findings.append(Finding("GITLEAKS_LEAK", file_p, detail))
    except Exception as exc:
        findings.append(Finding("GITLEAKS_PARSE", str(report_path), f"failed to parse Gitleaks report: {exc}"))
    return findings


def _git_output(cmd: list[str], repo_root: Path = ROOT) -> str:
    res = subprocess.run(["git", *cmd], cwd=repo_root, capture_output=True, check=False)
    if res.returncode != 0:
        err = res.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git {' '.join(cmd)} failed: {err}")
    return res.stdout.decode("utf-8", errors="replace")


def _read_git_lfs_attributes(repo_root: Path = ROOT) -> set[str]:
    gitattributes_file = repo_root / ".gitattributes"
    patterns = set()
    if gitattributes_file.is_file():
        try:
            text = gitattributes_file.read_text(encoding="utf-8")
            for line in text.splitlines():
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split()
                if len(parts) >= 2 and any("filter=lfs" in p for p in parts[1:]):
                    patterns.add(parts[0])
        except OSError:
            pass
    return patterns


def _get_git_entries(
    tracked_only: bool = False,
    repo_root: Path = ROOT,
    content_source: str = CONTENT_INDEX,
) -> list[GitEntry]:
    raw_ls = _git_output(["ls-files", "-s", "-z"], repo_root=repo_root)
    entries: list[GitEntry] = []

    if raw_ls:
        for item in raw_ls.split("\0"):
            if not item:
                continue
            parts = item.split(None, 3)
            if len(parts) == 4:
                mode, sha, stage, rel_path = parts
                kind = "file"
                if mode == "120000":
                    kind = "symlink"
                elif mode == "160000":
                    kind = "gitlink"
                elif mode == "100755":
                    kind = "executable"

                full_p = repo_root / rel_path
                working_mode = ""
                if full_p.is_symlink():
                    working_mode = "120000"
                elif full_p.is_file():
                    st = full_p.stat()
                    working_mode = "100755" if (st.st_mode & 0o111) else "100644"
                    if kind == "file":
                        probe = GitEntry(mode, sha, stage, rel_path, kind)
                        if content_source == CONTENT_WORKTREE:
                            raw_blob, _ = read_candidate_file(probe, repo_root)
                        else:
                            raw_blob, _ = read_indexed_blob(probe, repo_root)
                        txt = _decode_text_safe(raw_blob)
                        if txt and is_git_lfs_pointer(txt):
                            kind = "lfs_pointer"

                entries.append(
                    GitEntry(
                        mode=mode,
                        sha=sha,
                        stage=stage,
                        path=rel_path,
                        kind=kind,
                        working_mode=working_mode,
                    )
                )

    if not tracked_only:
        raw_others = _git_output(["ls-files", "-o", "--exclude-standard", "-z"], repo_root=repo_root)
        if raw_others:
            tracked_paths = {e.path for e in entries}
            for rel_path in raw_others.split("\0"):
                if not rel_path or rel_path in tracked_paths:
                    continue
                full_p = repo_root / rel_path
                mode_str = "100644"
                kind = "file"
                if full_p.is_symlink():
                    mode_str = "120000"
                    kind = "symlink"
                elif full_p.is_file():
                    if full_p.stat().st_mode & 0o111:
                        mode_str = "100755"
                        kind = "executable"
                    raw_bytes, _ = read_indexed_blob(GitEntry(mode_str, "", "0", rel_path, kind), repo_root)
                    txt = _decode_text_safe(raw_bytes)
                    if txt and is_git_lfs_pointer(txt):
                        kind = "lfs_pointer"
                entries.append(
                    GitEntry(
                        mode=mode_str,
                        sha="",
                        stage="0",
                        path=rel_path,
                        kind=kind,
                        working_mode=mode_str,
                    )
                )

    return sorted(entries, key=lambda e: e.path.lower())


def _get_filesystem_entries(repo_root: Path) -> list[GitEntry]:
    """Describe a materialized candidate directory without consulting Git index."""
    entries: list[GitEntry] = []
    for directory, dirnames, filenames in os.walk(repo_root, followlinks=False):
        base = Path(directory)
        # Prune VCS metadata directories before descending into them.
        dirnames[:] = [name for name in dirnames if name not in _VCS_METADATA_DIRS]
        for name in list(dirnames):
            path = base / name
            if path.is_symlink():
                rel = path.relative_to(repo_root).as_posix()
                entries.append(GitEntry("120000", "", "0", rel, "symlink", working_mode="120000"))
                dirnames.remove(name)
        for name in filenames:
            path = base / name
            rel = path.relative_to(repo_root).as_posix()
            mode = "100644"
            kind = "file"
            if path.is_symlink():
                mode = "120000"
                kind = "symlink"
            elif path.stat().st_mode & 0o111:
                mode = "100755"
                kind = "executable"
            raw_bytes, _ = read_candidate_file(GitEntry(mode, "", "0", rel, kind, working_mode=mode), repo_root)
            text_value = _decode_text_safe(raw_bytes)
            if text_value and is_git_lfs_pointer(text_value):
                kind = "lfs_pointer"
            entries.append(GitEntry(mode, "", "0", rel, kind, working_mode=mode))
    entries = _prune_gitignored_untracked(entries, repo_root)
    return sorted(entries, key=lambda entry: entry.path.lower())


def _prune_gitignored_untracked(entries: list[GitEntry], repo_root: Path) -> list[GitEntry]:
    """Drop untracked files the candidate's own .gitignore excludes.

    A materialized candidate is itself a working tree (the export generator
    initializes a fresh Git repository in it), so contributor tooling can
    leave gitignored scaffolding behind (e.g. ``.ruff_cache/`` after running
    pre-commit). Such files are not release content; the candidate walk must
    honor the tree's own ignore rules instead of reporting them as unknown
    binaries. Tracked files are never pruned, even if a pattern happens to
    match them.
    """
    if not entries:
        return entries
    try:
        tracked = subprocess.run(
            ["git", "ls-files", "-z", "--cached"],
            cwd=repo_root,
            capture_output=True,
            check=False,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return entries
    if tracked.returncode != 0:
        return entries
    tracked_paths = {p.decode("utf-8", errors="replace") for p in tracked.stdout.split(b"\x00") if p}
    candidates = [e for e in entries if e.path not in tracked_paths]
    if not candidates:
        return entries
    stdin_data = "\n".join(e.path for e in candidates).encode("utf-8")
    try:
        ignored = subprocess.run(
            ["git", "check-ignore", "--no-index", "--stdin"],
            cwd=repo_root,
            input=stdin_data,
            capture_output=True,
            check=False,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return entries
    if ignored.returncode not in (0, 1):
        return entries
    ignored_paths = {p.decode("utf-8", errors="replace") for p in ignored.stdout.splitlines() if p}
    if not ignored_paths:
        return entries
    kept = [e for e in entries if e.path not in ignored_paths]
    return kept


def read_indexed_blob(
    entry: GitEntry,
    repo_root: Path = ROOT,
) -> tuple[bytes | None, str | None]:
    """Read exact byte content of a Git index entry via git cat-file -p <sha>."""
    disk_path = repo_root / entry.path

    if entry.kind == "symlink":
        if disk_path.is_symlink():
            try:
                target = disk_path.readlink()
                return str(target).encode("utf-8"), None
            except OSError as exc:
                return None, f"failed to read symlink target: {exc}"
        elif entry.sha:
            try:
                res = subprocess.run(
                    ["git", "cat-file", "-p", entry.sha],
                    cwd=repo_root,
                    capture_output=True,
                    check=False,
                )
                if res.returncode == 0:
                    return res.stdout, None
            except Exception:
                pass

    if entry.kind == "gitlink":
        return entry.sha.encode("utf-8"), None

    if entry.sha:
        try:
            res = subprocess.run(
                ["git", "cat-file", "-p", entry.sha],
                cwd=repo_root,
                capture_output=True,
                check=False,
            )
            if res.returncode == 0:
                return res.stdout, None
            return None, f"git cat-file failed to read blob {entry.sha}"
        except Exception as exc:
            return None, f"failed to execute git cat-file: {exc}"

    # Fallback for untracked prospective files (sha is empty)
    if disk_path.is_file() and not disk_path.is_symlink():
        try:
            return disk_path.read_bytes(), None
        except OSError as exc:
            return None, f"failed to read untracked file from disk: {exc}"

    return None, "file missing or unreadable"


def read_indexed_blobs_batch(
    entries: list[GitEntry],
    repo_root: Path = ROOT,
) -> dict[str, tuple[bytes | None, str | None]]:
    """Batch-read all exact Git index blobs in a single subprocess using git cat-file --batch."""
    result: dict[str, tuple[bytes | None, str | None]] = {}
    shas_to_query: list[tuple[str, str]] = []

    for entry in entries:
        disk_path = repo_root / entry.path
        if entry.kind == "symlink":
            if disk_path.is_symlink():
                try:
                    target = disk_path.readlink()
                    result[entry.path] = (str(target).encode("utf-8"), None)
                    continue
                except OSError as exc:
                    result[entry.path] = (None, f"failed to read symlink target: {exc}")
                    continue
        elif entry.kind == "gitlink":
            result[entry.path] = (entry.sha.encode("utf-8"), None)
            continue

        if entry.sha:
            shas_to_query.append((entry.path, entry.sha))
        else:
            if disk_path.is_file() and not disk_path.is_symlink():
                try:
                    result[entry.path] = (disk_path.read_bytes(), None)
                except OSError as exc:
                    result[entry.path] = (None, f"failed to read untracked file from disk: {exc}")
            else:
                result[entry.path] = (None, "untracked file missing or unreadable")

    if not shas_to_query:
        return result

    try:
        proc = subprocess.Popen(
            ["git", "cat-file", "--batch"],
            cwd=repo_root,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
        )
        assert proc.stdin and proc.stdout
        stdin_data = "".join(f"{sha}\n" for _, sha in shas_to_query).encode("utf-8")
        out_bytes, _ = proc.communicate(input=stdin_data)

        pos = 0
        out_len = len(out_bytes)
        for rel_path, expected_sha in shas_to_query:
            if pos >= out_len:
                result[rel_path] = (None, f"batch read ended unexpectedly for {expected_sha}")
                continue

            nl_idx = out_bytes.find(b"\n", pos)
            if nl_idx == -1:
                result[rel_path] = (None, f"invalid batch output header for {expected_sha}")
                break

            header = out_bytes[pos:nl_idx].decode("utf-8", errors="replace").strip()
            pos = nl_idx + 1

            if "missing" in header:
                result[rel_path] = (None, f"git blob missing: {expected_sha}")
                continue

            parts = header.split()
            if len(parts) >= 3 and parts[2].isdigit():
                size = int(parts[2])
                content = out_bytes[pos : pos + size]
                pos += size
                if pos < out_len and out_bytes[pos : pos + 1] == b"\n":
                    pos += 1
                result[rel_path] = (content, None)
            else:
                result[rel_path] = (None, f"malformed batch header '{header}' for {expected_sha}")
    except Exception:
        for rel_path, sha in shas_to_query:
            result[rel_path] = read_indexed_blob(GitEntry("100644", sha, "0", rel_path, "file"), repo_root)

    return result


def read_worktree_blobs(
    entries: list[GitEntry],
    repo_root: Path = ROOT,
) -> dict[str, tuple[bytes | None, str | None]]:
    """Read each entry's current working-tree bytes, falling back to its index blob.

    Content is taken from whatever is actually on disk, not from the index entry's
    recorded kind: an unstaged edit may have replaced a tracked symlink with a regular
    file, and the audit has to see the bytes that exist rather than the bytes Git last
    recorded.

    A path that is absent from the working tree is not a hiding place. Deleting a
    tracked file without committing the deletion still leaves its blob staged for
    publication, so those entries fall back to `git cat-file` instead of being skipped
    -- otherwise `rm` would silently clear a finding that a commit would reintroduce.
    """
    result: dict[str, tuple[bytes | None, str | None]] = {}
    fallback: list[GitEntry] = []

    for entry in entries:
        disk_path = repo_root / entry.path
        if entry.kind == "gitlink":
            result[entry.path] = (entry.sha.encode("utf-8"), None)
            continue
        if disk_path.is_symlink():
            try:
                result[entry.path] = (str(disk_path.readlink()).encode("utf-8"), None)
            except OSError as exc:
                result[entry.path] = (None, f"failed to read symlink target: {exc}")
            continue
        if disk_path.is_file():
            try:
                result[entry.path] = (disk_path.read_bytes(), None)
            except OSError as exc:
                result[entry.path] = (None, f"failed to read working-tree file from disk: {exc}")
            continue
        fallback.append(entry)

    if fallback:
        result.update(read_indexed_blobs_batch(fallback, repo_root))

    return result


def read_candidate_file(
    entry: GitEntry,
    candidate_root: Path,
) -> tuple[bytes | None, str | None]:
    """Read exact byte content of a candidate file from a materialized filesystem directory."""
    disk_path = candidate_root / entry.path

    if entry.kind == "symlink":
        try:
            target = disk_path.readlink()
            return str(target).encode("utf-8"), None
        except OSError as exc:
            return None, f"failed to read candidate symlink target: {exc}"

    if disk_path.is_file():
        try:
            return disk_path.read_bytes(), None
        except OSError as exc:
            return None, f"failed to read candidate file from disk: {exc}"

    return None, "candidate file missing or unreadable"


def _decode_text_safe(data: bytes | None) -> str | None:
    if data is None:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def _is_binary_bytes(data: bytes) -> bool:
    if b"\x00" in data:
        return True
    try:
        decoded = data.decode("utf-8")
        if not decoded:
            return False
        non_printable = sum(1 for ch in decoded if ord(ch) < 32 and ch not in "\t\n\r")
        return (non_printable / len(decoded)) > 0.05
    except UnicodeDecodeError:
        return True


def _forbidden_path(path: str) -> str | None:
    normalized = path.lstrip("./")
    lower = normalized.lower()
    if any(lower.startswith(prefix.lower()) for prefix in FORBIDDEN_PREFIXES):
        return "private/generated path"
    name = PurePosixPath(normalized).name
    if name == "reference_hashes.json":
        return "game-derived hash manifest"
    if name in ("vfpu_words.txt", "vfpu_words_local.txt", "nidseq_mine.txt", "pgd_keys.txt"):
        return "private/game-derived data artifact"
    if re.fullmatch(r"EBOOT\.BIN\.dec(?:\..+)?", name, re.IGNORECASE):
        return "raw decrypted/decompiler artifact"
    if re.search(r"(?:^|/)[^/]*_recomp(?:_\d+)?\.c$", normalized, re.IGNORECASE):
        return "generated recompiled source"
    if PurePosixPath(normalized).suffix.lower() in FORBIDDEN_EXTENSIONS:
        return f"prohibited extension {PurePosixPath(normalized).suffix}"
    return None


def _magic_kind(path_or_bytes: Path | bytes | None, path: str = "") -> str | None:
    if path_or_bytes is None:
        return None
    data: bytes | None = None
    if isinstance(path_or_bytes, Path):
        try:
            with path_or_bytes.open("rb") as stream:
                data = stream.read(65536)
        except OSError:
            return "unreadable file"
    else:
        data = path_or_bytes

    if not data:
        return None
    if data.startswith(b"\x7fELF"):
        return "ELF executable"
    if data.startswith(b"MZ"):
        return "PE executable"
    if data.startswith((b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08")):
        return "ZIP archive"
    if data.startswith(b"SQLite format 3\0"):
        return "SQLite database"
    if data.startswith(b"\0PBP"):
        return "PSP PBP"
    if data.startswith((b"~PSP", b"~SCE")):
        return "encrypted PSP module"
    if data.startswith(b"\x03\x02\x23\x07") or data.startswith(b"\x07\x23\x02\x03"):
        return "SPIR-V shader bytecode"
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "PNG image"
    if data.startswith(b"\xff\xd8\xff"):
        return "JPEG image"
    if data.startswith(b"RIFF") and data[8:12] == b"WEBP":
        return "WebP image"
    if data.startswith(b"RIFF") and data[8:12] == b"WAVE":
        return "WAV audio"
    if data.startswith(b"OggS"):
        return "OGG audio"
    if data.startswith(b"\x00PGF") or data.startswith(b"PGF"):
        return "PSP PGF font"
    txt = _decode_text_safe(data)
    if txt and is_git_lfs_pointer(txt):
        return "Git LFS pointer"
    if len(data) >= 0x8006 and data[0x8001:0x8006] == b"CD001":
        return "ISO9660 image"
    return None


def _assigned_literal(node: ast.AST | None) -> str | None:
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.Call) and node.args:
        function = node.func
        if (
            isinstance(function, ast.Attribute)
            and function.attr == "fromhex"
            and isinstance(node.args[0], ast.Constant)
            and isinstance(node.args[0].value, str)
        ):
            return node.args[0].value
    return None


def private_key_assignment_lines(text: str) -> list[int]:
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return []
    lines = []
    for node in ast.walk(tree):
        targets: list[ast.expr] = []
        value: ast.AST | None = None
        if isinstance(node, (ast.Assign, ast.AnnAssign)):
            targets = list(node.targets) if isinstance(node, ast.Assign) else [node.target]
            value = node.value
        literal = _assigned_literal(value)
        if literal is None or HEX_16_BYTES.fullmatch(literal) is None:
            continue
        for target in targets:
            if isinstance(target, ast.Name) and KEY_NAME.search(target.id):
                assert isinstance(node, (ast.Assign, ast.AnnAssign))
                lines.append(node.lineno)
                break
    return sorted(set(lines))


# Byte-exact third-party import trees under src/. Their files are copied
# verbatim from an upstream pin and must stay byte-identical to preserve the
# recorded blob SHAs (see src/rt/atrac3p/PROVENANCE.md); they carry the
# upstream's own license headers, which predate SPDX identifiers. The audit
# classifies them via their ledger record instead of requiring a header edit
# that would break the byte-identity claim. The project-authored wrapper and
# selftest outside these subdirectories still require SPDX.
SPDX_BYTE_EXACT_IMPORT_TREES = (
    "src/rt/atrac3p/libavcodec/",
    "src/rt/atrac3p/libavutil/",
)


def _spdx_required(path: str) -> bool:
    pure = PurePosixPath(path)
    if pure.suffix.lower() not in SOURCE_EXTENSIONS:
        return False
    if path.startswith(SPDX_BYTE_EXACT_IMPORT_TREES):
        return False
    if path.startswith(("src/", "tools/")):
        return True
    return len(pure.parts) == 1


def _text(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return None


def _notice_link_findings(repo_root: Path = ROOT) -> list[Finding]:
    notice = repo_root / "NOTICE.md"
    text = _text(notice)
    if text is None:
        return [Finding("NOTICE", "NOTICE.md", "missing or unreadable")]
    findings = []
    for target in MARKDOWN_LINK.findall(text):
        if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.IGNORECASE) or target.startswith("#"):
            continue
        target_path = target.split("#", 1)[0]
        if target_path and not (notice.parent / target_path).exists():
            findings.append(Finding("NOTICE_LINK", "NOTICE.md", f"missing link target: {target_path}"))
    return findings


def _action_pin_findings(repo_root: Path = ROOT, audited_paths: set[str] | None = None) -> list[Finding]:
    findings = []
    workflow_dir = repo_root / ".github" / "workflows"
    if workflow_dir.is_dir():
        for path in sorted(workflow_dir.glob("*.y*ml")):
            rel = path.relative_to(repo_root).as_posix()
            if audited_paths is not None and rel not in audited_paths:
                continue
            text = _text(path) or ""
            for match in ACTION_USE.finditer(text):
                use = match.group(1).strip("'\"")
                if use.startswith(("./", "docker://")):
                    continue
                if not FULL_SHA_ACTION.fullmatch(use):
                    findings.append(Finding("ACTION_PIN", rel, f"action is not pinned to a full SHA: {use}"))
    return findings


def check_collisions(paths: list[str]) -> list[Finding]:
    findings: list[Finding] = []
    case_map: dict[str, list[str]] = {}
    nfc_map: dict[str, list[str]] = {}

    for p in paths:
        if any(ord(ch) < 32 and ch not in "\t\n\r" for ch in p):
            findings.append(Finding("FILENAME_CONTROL", p, "filename contains unprintable control characters"))

        if any(ch in DANGEROUS_UNICODE_CHARS for ch in p):
            findings.append(Finding("PATH_DANGEROUS_UNICODE", p, "path contains dangerous bidi override, zero-width, or BOM formatting character"))

        pure = PurePosixPath(p)
        stem_lower = pure.stem.lower()
        if stem_lower in RESERVED_WIN_NAMES:
            findings.append(Finding("FILENAME_RESERVED", p, f"filename stem uses Windows reserved device name '{pure.stem}'"))

        name = pure.name
        if name.endswith(" ") or name.endswith("."):
            findings.append(Finding("FILENAME_TRAILING", p, "filename ends with trailing space or dot"))

        lower_p = p.lower()
        case_map.setdefault(lower_p, []).append(p)

        nfc_p = unicodedata.normalize("NFC", p).lower()
        nfc_map.setdefault(nfc_p, []).append(p)

    for lower_p, matches in case_map.items():
        if len(matches) > 1:
            findings.append(Finding("COLLISION_CASE", matches[0], f"case-insensitive collision between {matches}"))

    for nfc_p, matches in nfc_map.items():
        if len(matches) > 1 and matches not in case_map.values():
            findings.append(Finding("COLLISION_UNICODE", matches[0], f"Unicode normalization collision between {matches}"))

    return findings


def load_release_manifest(manifest_path: Path) -> tuple[dict[str, dict], list[Finding]]:
    findings: list[Finding] = []
    manifest_entries: dict[str, dict] = {}
    if not manifest_path.is_file():
        findings.append(Finding("MANIFEST_ERROR", str(manifest_path), "release manifest file is missing or unreadable"))
        return manifest_entries, findings

    try:
        with manifest_path.open("r", encoding="utf-8") as stream:
            data = json.load(stream)
            if not isinstance(data, dict) or "components" not in data or not isinstance(data["components"], list):
                findings.append(Finding("MANIFEST_ERROR", str(manifest_path), "manifest missing valid 'components' array"))
                return manifest_entries, findings

            for comp in data["components"]:
                if not isinstance(comp, dict):
                    continue
                sp = comp.get("source_path") or comp.get("provenance_path")
                if sp:
                    if sp in manifest_entries:
                        findings.append(Finding("MANIFEST_DUPLICATE_PATH", sp, f"duplicate manifest entry for path {sp}"))
                    manifest_entries[sp] = comp
    except Exception as exc:
        findings.append(Finding("MANIFEST_ERROR", str(manifest_path), f"failed to load/parse manifest: {exc}"))

    return manifest_entries, findings


def _default_provenance(rel_path: str, manifest_comp: dict | None) -> tuple[str, str, str, str, str, bool]:
    if manifest_comp:
        prov = manifest_comp.get("provenance_class") or manifest_comp.get("type") or "unclassified"
        lic = manifest_comp.get("license", "unspecified")
        notice = manifest_comp.get("notice_path", "NOTICE.md")
        gen = manifest_comp.get("generated_source") or ("asset" if manifest_comp.get("type") == "asset" else "source")
        disp = manifest_comp.get("disposition", "included")
        pub = manifest_comp.get("public_scope_included", True)
        return prov, lic, notice, gen, disp, pub

    pure = PurePosixPath(rel_path)
    ext = pure.suffix.lower()

    if rel_path in REQUIRED_PATHS or rel_path in ("NOTICE.md", "LICENSE", "README.md", "AGENTS.md", "CODE_OF_CONDUCT.md", "CONTRIBUTING.md", "DEDICATION.md", "ISSUES.md", "SECURITY.md", "TODO.md"):
        return "notice_doc", "GPL-3.0-or-later", rel_path if rel_path in ("LICENSE", "NOTICE.md") else "NOTICE.md", "documentation", "included", True

    if rel_path.startswith("THIRD_PARTY_LICENSES/"):
        return "notice_doc", "BSD-3-Clause", rel_path, "documentation", "included", True

    if rel_path.startswith("assets/vfpu/"):
        return "ppsspp_derived", "GPL-2.0-or-later", "NOTICE.md", "data", "included", True

    if rel_path.startswith(".github/") or rel_path.startswith(".") or rel_path in (".gitignore", ".gitattributes", ".clang-format", ".editorconfig", ".markdownlint-cli2.jsonc", ".pre-commit-config.yaml"):
        return "project_authored", "GPL-2.0-or-later", "NOTICE.md", "configuration", "included", True

    if rel_path.startswith(("src/", "tools/", "interface/", "mk/", "assets/", "fixtures/", "docs/")) or ext in SOURCE_EXTENSIONS or ext in (".md", ".txt", ".json", ".jsonc", ".yml", ".yaml", ".toml", ".ps1") or rel_path in ("Makefile", "pyproject.toml", "copy_build_assets.ps1", "hst.ps1", "hst_manager.ps1"):
        gen_kind = "documentation" if (rel_path.startswith("docs/") or ext == ".md") else ("data" if ext in (".json", ".jsonc", ".dat") else ("script" if ext in (".ps1", ".sh") else "source"))
        return "project_authored", "GPL-2.0-or-later", "NOTICE.md", gen_kind, "included", True

    if rel_path.startswith("font/"):
        return "unresolved", "Unresolved", "THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt", "asset", "optional_excluded_if_unverified", False

    return "unclassified", "unspecified", "NOTICE.md", "source", "unclassified", True


def _resolve_content_source(content_source: str, is_candidate_root: bool) -> str:
    """Reconcile the explicit content source with the older is_candidate_root flag.

    `is_candidate_root` predates the three-way selector and existing callers still pass
    it on its own, so it keeps winning: a materialized export has no index to read.
    """
    if is_candidate_root:
        return CONTENT_CANDIDATE
    if content_source not in (CONTENT_INDEX, CONTENT_WORKTREE, CONTENT_CANDIDATE):
        raise ValueError(f"unknown content source: {content_source!r}")
    return content_source


def audit_entries_with_semantics(
    entries: list[GitEntry],
    manifest_path: Path | None = None,
    public_scope: bool = False,
    gitleaks_report: Path | None = None,
    repo_root: Path = ROOT,
    exhaustive: bool = False,
    is_candidate_root: bool = False,
    content_source: str = CONTENT_INDEX,
) -> tuple[list[Finding], list[FileSemantics]]:

    content_source = _resolve_content_source(content_source, is_candidate_root)
    findings: list[Finding] = []
    semantics_list: list[FileSemantics] = []
    paths = [e.path for e in entries]

    for required in REQUIRED_PATHS:
        if not (repo_root / required).is_file():
            findings.append(Finding("REQUIRED", required, "required publication file is missing"))

    findings.extend(check_collisions(paths))

    if gitleaks_report:
        findings.extend(parse_gitleaks_report(gitleaks_report))

    manifest_map: dict[str, dict] = {}
    if manifest_path:
        manifest_map, manifest_findings = load_release_manifest(manifest_path)
        findings.extend(manifest_findings)

    lfs_patterns = _read_git_lfs_attributes(repo_root)

    # Check for orphan manifest paths
    entry_path_set = set(paths)
    for m_path, comp in manifest_map.items():
        comp_type = comp.get("type", "")
        presence = comp.get("presence", "")
        # Components declared as excluded from the public scope are expected to
        # be absent from a public-scope candidate; their absence is not an
        # orphan. Outside a public-scope audit the declaration is still
        # honored as a disposition, not as permission to be missing.
        if public_scope and comp.get("public_scope_included", True) is False:
            continue
        if comp_type not in ("source_lineage", "library") and presence not in ("notice_lineage", "optional_local_or_external"):
            if m_path not in entry_path_set:
                findings.append(Finding("MANIFEST_ORPHAN_PATH", m_path, "manifest path not found in candidate repository tree"))

    finding_map: dict[str, list[Finding]] = {}
    content_map: dict[str, tuple[bytes | None, str | None]] = {}
    if content_source == CONTENT_INDEX:
        content_map = read_indexed_blobs_batch(entries, repo_root)
    elif content_source == CONTENT_WORKTREE:
        content_map = read_worktree_blobs(entries, repo_root)

    for entry in entries:
        rel = entry.path
        entry_findings: list[Finding] = []

        if content_source == CONTENT_CANDIDATE:
            raw_bytes, read_err = read_candidate_file(entry, repo_root)
        else:
            raw_bytes, read_err = content_map.get(rel, (None, "file missing or unreadable"))

        if read_err and entry.kind not in ("symlink", "gitlink"):
            f = Finding("UNREADABLE", rel, read_err)
            entry_findings.append(f)

        size = len(raw_bytes) if raw_bytes is not None else 0
        sha256 = hashlib.sha256(raw_bytes).hexdigest() if raw_bytes is not None else ""

        manifest_comp = manifest_map.get(rel)
        prov, lic, notice, gen, disp, pub_scope = _default_provenance(rel, manifest_comp)

        if public_scope and pub_scope is False:
            f = Finding("UNRESOLVED_PUBLIC", rel, "unresolved asset must be excluded from candidate public tree scope")
            entry_findings.append(f)

        reason = _forbidden_path(rel)
        if reason:
            f = Finding("PATH", rel, reason)
            entry_findings.append(f)

        path = repo_root / rel

        # Check symlink
        if entry.kind == "symlink" or entry.working_mode == "120000":
            try:
                target_str = raw_bytes.decode("utf-8") if raw_bytes else ""
                target_pure = PurePosixPath(target_str)
                if target_pure.is_absolute():
                    entry_findings.append(Finding("SYMLINK_ABSOLUTE", rel, "absolute symlink target exposes local layout"))
                if ".." in target_pure.parts or any(p == ".." for p in PurePosixPath(rel).parts):
                    entry_findings.append(Finding("SYMLINK_ESCAPE", rel, "symlink target contains relative escape ('..')"))
                resolved = (path.parent / target_pure).resolve()
                if not resolved.is_relative_to(repo_root.resolve()):
                    entry_findings.append(Finding("SYMLINK_ESCAPE", rel, f"symlink target {resolved} escapes repository root"))
            except (OSError, ValueError):
                entry_findings.append(Finding("SYMLINK_UNREADABLE", rel, "unreadable symlink target"))

        # Check gitlink
        if entry.kind == "gitlink" or entry.mode == "160000":
            if not manifest_comp or manifest_comp.get("disposition") != "approved_submodule":
                entry_findings.append(Finding("GITLINK", rel, f"submodule/gitlink reference at {entry.sha} requires explicit manifest approval"))

        # Check size ceiling
        if size > 25 * 1024 * 1024:
            entry_findings.append(Finding("SIZE", rel, f"unexpectedly large candidate file: {size} bytes"))

        # Magic detection
        magic = _magic_kind(raw_bytes, rel) or "none"
        if magic != "none":
            if magic in ("PSP PGF font", "SPIR-V shader bytecode") and (rel.startswith("font/") or rel.startswith("src/")):
                pass
            elif disp in ("optional_excluded_if_unverified", "approved_binary"):
                pass
            else:
                entry_findings.append(Finding("MAGIC", rel, magic))

        text_str = _decode_text_safe(raw_bytes)
        is_binary = _is_binary_bytes(raw_bytes) if raw_bytes is not None else False
        text_binary_kind = "binary" if is_binary else "text"

        if is_binary and magic == "none" and entry.kind not in ("symlink", "gitlink"):
            if rel.startswith("assets/vfpu/"):
                pass
            elif disp in ("unclassified", "excluded_pending_qualified_review") or exhaustive:
                if not manifest_comp:
                    entry_findings.append(Finding("MAGIC_UNKNOWN", rel, "unknown binary magic requires explicit manifest review/disposition"))

        if (exhaustive or public_scope) and not manifest_comp and prov == "unclassified":
            entry_findings.append(Finding("UNCLASSIFIED_PATH", rel, "tracked path has no explicit manifest provenance classification"))

        # LFS Pointer & .gitattributes check
        matches_lfs_attr = any(fnmatch.fnmatch(rel, pat) for pat in lfs_patterns)
        if entry.kind == "lfs_pointer" or (text_str and is_git_lfs_pointer(text_str)):
            if not matches_lfs_attr and lfs_patterns:
                entry_findings.append(Finding("LFS_UNTRACKED_POINTER", rel, "Git LFS pointer file is not tracked in .gitattributes"))
        elif matches_lfs_attr and entry.kind == "file":
            entry_findings.append(Finding("LFS_MISMATCH", rel, "file matches .gitattributes LFS pattern but is not a valid LFS pointer"))

        if text_str and not is_binary:
            if _spdx_required(rel) and "SPDX-License-Identifier:" not in "\n".join(text_str.splitlines()[:8]):
                entry_findings.append(Finding("SPDX", rel, "missing SPDX identifier in first eight lines"))

            if PurePosixPath(rel).suffix.lower() == ".py":
                for line in private_key_assignment_lines(text_str):
                    entry_findings.append(Finding("PRIVATE_KEY", rel, f"direct 16-byte key literal at line {line}"))

            if (
                WINDOWS_USER_PATH.search(text_str)
                or POSIX_USER_PATH.search(text_str)
                or MAC_USER_PATH.search(text_str)
                or WSL_USER_PATH.search(text_str)
                or UNC_PATH.search(text_str)
                or ONEDRIVE_PATH.search(text_str)
                or TEMP_PATH.search(text_str)
            ):
                entry_findings.append(Finding("LOCAL_PATH", rel, "contains an absolute user-profile or local path"))

        findings.extend(entry_findings)
        for f in entry_findings:
            finding_map.setdefault(rel, []).append(f)

        status_str = "FAIL" if entry_findings else "OK"
        file_finding_strs = [f"{f.code}: {f.detail}" for f in entry_findings]

        semantics_list.append(
            FileSemantics(
                path=rel,
                mode=entry.mode,
                working_mode=entry.working_mode or entry.mode,
                index_sha=entry.sha,
                size=size,
                sha256=sha256,
                kind=entry.kind,
                text_binary=text_binary_kind,
                magic=magic,
                provenance_class=prov,
                license_expression=lic,
                notice_owner=notice,
                generated_source=gen,
                release_disposition=disp,
                public_scope_included=pub_scope,
                status=status_str,
                findings=file_finding_strs,
            )
        )

    audited_paths = {e.path for e in entries}
    findings.extend(_notice_link_findings(repo_root))
    findings.extend(_action_pin_findings(repo_root, audited_paths))

    sorted_findings = sorted(findings, key=lambda item: (item.path.lower(), item.code, item.detail))
    sorted_semantics = sorted(semantics_list, key=lambda s: s.path.lower())

    return sorted_findings, sorted_semantics


def audit_entries(
    entries: list[GitEntry],
    manifest_path: Path | None = None,
    public_scope: bool = False,
    gitleaks_report: Path | None = None,
    repo_root: Path = ROOT,
    exhaustive: bool = False,
    is_candidate_root: bool = False,
    content_source: str = CONTENT_INDEX,
) -> list[Finding]:
    findings, _ = audit_entries_with_semantics(
        entries=entries,
        manifest_path=manifest_path,
        public_scope=public_scope,
        gitleaks_report=gitleaks_report,
        repo_root=repo_root,
        exhaustive=exhaustive,
        is_candidate_root=is_candidate_root,
        content_source=content_source,
    )
    return findings


def audit(paths: list[str]) -> list[Finding]:
    entries = [GitEntry(mode="100644", sha="", stage="0", path=p, kind="file", working_mode="100644") for p in paths]
    return audit_entries(entries)


def generate_manifest_report(
    entries: list[GitEntry],
    findings: list[Finding],
    semantics: list[FileSemantics] | None = None,
    repo_root: Path = ROOT,
    profile: str = "public-safe-v1",
    gitleaks_report: Path | None = None,
    is_candidate_root: bool = False,
    content_source: str = CONTENT_INDEX,
) -> dict:

    content_source = _resolve_content_source(content_source, is_candidate_root)

    if semantics is None:
        _, semantics = audit_entries_with_semantics(
            entries,
            repo_root=repo_root,
            gitleaks_report=gitleaks_report,
            is_candidate_root=is_candidate_root,
            content_source=content_source,
        )

    git_commit = ""
    try:
        git_commit = _git_output(["rev-parse", "HEAD"], repo_root=repo_root).strip()
    except Exception:
        git_commit = "unknown"

    canonical_entries = [asdict(s) for s in semantics]
    entry_json_canonical = json.dumps(canonical_entries, sort_keys=True)
    aggregate_hash = hashlib.sha256(entry_json_canonical.encode("utf-8")).hexdigest()

    gitleaks_status = "ingested" if gitleaks_report else "not_run"

    text_count = sum(1 for s in semantics if s.text_binary == "text")
    binary_count = sum(1 for s in semantics if s.text_binary == "binary")
    symlink_count = sum(1 for s in semantics if s.kind == "symlink")
    gitlink_count = sum(1 for s in semantics if s.kind == "gitlink")
    lfs_count = sum(1 for s in semantics if s.kind == "lfs_pointer")

    return {
        "status": "FAIL" if findings else "OK",
        "total_files": len(semantics),
        "total_findings": len(findings),
        "meta": {
            "tool_version": TOOL_VERSION,
            "schema_version": SCHEMA_VERSION,
            "git_commit": git_commit,
            "content_source": content_source,
            "profile": profile,
            "gitleaks_status": gitleaks_status,
            "aggregate_manifest_sha256": aggregate_hash,
        },
        "summary": {
            "total_files": len(semantics),
            "total_findings": len(findings),
            "text_files": text_count,
            "binary_files": binary_count,
            "symlinks": symlink_count,
            "gitlinks": gitlink_count,
            "lfs_pointers": lfs_count,
        },
        "findings": [asdict(f) for f in findings],
        "files": canonical_entries,
    }


def export_csv_manifest_report(
    entries_or_semantics: list[GitEntry] | list[FileSemantics],
    findings_or_output: list[Finding] | Path,
    output_path_or_none: Path | None = None,
    repo_root: Path = ROOT,
    is_candidate_root: bool = False,
    content_source: str = CONTENT_INDEX,
) -> None:
    semantics: list[FileSemantics] = []
    output_path: Path

    if isinstance(output_path_or_none, Path):
        # Called with (entries, findings, output_path)
        entries = entries_or_semantics  # type: ignore
        findings = findings_or_output  # type: ignore
        output_path = output_path_or_none
        _, semantics = audit_entries_with_semantics(
            entries,
            repo_root=repo_root,
            is_candidate_root=is_candidate_root,
            content_source=content_source,
        )
    else:
        # Called with (semantics, output_path)
        semantics = entries_or_semantics  # type: ignore
        output_path = findings_or_output  # type: ignore

    fieldnames = [
        "path",
        "mode",
        "working_mode",
        "index_sha",
        "size",
        "sha256",
        "kind",
        "text_binary",
        "magic",
        "provenance_class",
        "license_expression",
        "notice_owner",
        "generated_source",
        "release_disposition",
        "public_scope_included",
        "status",
        "findings",
    ]
    with output_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        for s in semantics:
            row = asdict(s)
            row["findings"] = "; ".join(row["findings"])
            writer.writerow(row)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--tracked-only",
        action="store_true",
        help="audit only Git-tracked files (FAST pre-commit tripwire mode); default includes untracked candidates",
    )
    parser.add_argument(
        "--worktree",
        action="store_true",
        help=(
            "audit the bytes currently on disk instead of the staged Git blobs; use this "
            "for an interactive check of the tree you are looking at, and the default "
            "index mode for pre-commit/pre-push and release-export gates"
        ),
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=None,
        help="Path to release manifest for disposition lookups",
    )
    parser.add_argument(
        "--candidate-root",
        type=Path,
        help="Audit a materialized candidate directory instead of the current Git worktree",
    )
    parser.add_argument(
        "--candidate-tree",
        action="store_true",
        help="Run exhaustive candidate-public-tree manifest gate audit",
    )
    parser.add_argument(
        "--profile",
        type=str,
        default="public-safe-v1",
        help="Public scope policy profile (default: public-safe-v1)",
    )
    parser.add_argument(
        "--manifest-out",
        type=Path,
        help="Path to write full JSON audit manifest output",
    )
    parser.add_argument(
        "--csv-out",
        type=Path,
        help="Path to write full CSV audit manifest report",
    )
    parser.add_argument(
        "--gitleaks-report",
        type=Path,
        help="Path to external Gitleaks JSON findings report to ingest and check",
    )
    parser.add_argument(
        "--public-scope",
        action="store_true",
        help="Audit candidate public tree scope (fails if unresolved assets marked non-public are present)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print JSON report to stdout instead of text summary",
    )
    args = parser.parse_args(argv)

    audit_root = args.candidate_root.resolve() if args.candidate_root else ROOT
    manifest_path = args.manifest or (audit_root / "assets" / "release_manifest.json")
    is_exhaustive = args.candidate_tree or bool(args.manifest_out) or bool(args.csv_out) or args.public_scope
    is_cand_root = bool(args.candidate_root)

    if args.worktree and args.candidate_root:
        print(
            "publication audit failed: --worktree audits the Git working tree and cannot be "
            "combined with --candidate-root, which audits a materialized directory",
            file=sys.stderr,
        )
        return 2

    content_source = CONTENT_CANDIDATE if is_cand_root else (CONTENT_WORKTREE if args.worktree else CONTENT_INDEX)

    try:
        entries = (
            _get_filesystem_entries(audit_root)
            if args.candidate_root
            else _get_git_entries(args.tracked_only, repo_root=audit_root, content_source=content_source)
        )
    except (RuntimeError, UnicodeDecodeError) as error:
        print(f"publication audit failed: {error}", file=sys.stderr)
        return 2

    findings, semantics = audit_entries_with_semantics(
        entries,
        manifest_path=manifest_path,
        public_scope=args.public_scope,
        gitleaks_report=args.gitleaks_report,
        repo_root=audit_root,
        exhaustive=is_exhaustive,
        is_candidate_root=is_cand_root,
        content_source=content_source,
    )
    report = generate_manifest_report(
        entries,
        findings,
        semantics,
        repo_root=audit_root,
        profile=args.profile,
        gitleaks_report=args.gitleaks_report,
        is_candidate_root=is_cand_root,
        content_source=content_source,
    )

    if args.manifest_out:
        args.manifest_out.parent.mkdir(parents=True, exist_ok=True)
        args.manifest_out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote audit manifest to {args.manifest_out}")

    if args.csv_out:
        args.csv_out.parent.mkdir(parents=True, exist_ok=True)
        export_csv_manifest_report(
            semantics,
            args.csv_out,
            is_candidate_root=is_cand_root,
            content_source=content_source,
        )
        print(f"Wrote CSV audit manifest report to {args.csv_out}")

    if args.json:
        print(json.dumps(report, indent=2))
        return 1 if findings else 0

    scope = "candidate" if args.candidate_root else ("tracked" if args.tracked_only else "prospective")

    if findings:
        for finding in findings:
            print(f"{finding.code}: {finding.path}: {finding.detail}", file=sys.stderr)
        print(
            f"publication audit: FAIL ({len(findings)} findings across {len(entries)} "
            f"{scope} files, {content_source} content)",
            file=sys.stderr,
        )
        return 1

    print(f"publication audit: OK ({len(entries)} {scope} files, {content_source} content)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
