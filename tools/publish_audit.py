#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
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
import io
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unicodedata

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nk_core.git_isolation import isolated_git_env  # noqa: E402
from publication_policy import (  # noqa: E402
    INCLUDED,
    Policy,
    PolicyError,
    Resolution,
    load_policy,
)

ROOT = Path(__file__).resolve().parent.parent

TOOL_VERSION = "0.4.0"
SCHEMA_VERSION = "1.2.0"

#: Canonical publication-eligibility policy. This is the authoritative source for
#: "may this path be published"; the auditor consumes it directly rather than
#: re-deriving eligibility from path prefixes.
#: Resolved from ROOT at call time, not bound at import. Tests patch ROOT to a
#: hermetic fixture repository, and a constant captured at import would silently
#: keep pointing the gate at the real repository's policy.
def default_policy_path() -> Path:
    return ROOT / "assets" / "public_source_profile.json"


#: Generated evidence artifact that must carry the current policy digest.
def default_export_path() -> Path:
    return ROOT / "PUBLIC_EXPORT.json"

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
CONTENT_COMMITTED = "committed"

PROVENANCE_LEDGER_REL = "assets/public_provenance_ledger.json"
PROVENANCE_CLASSES = frozenset({
    "project_authored_attested",
    "upstream_derived",
    "generated_from_public_source",
    "synthetic_fixture",
    "public_factual_metadata",
    "reviewed_configuration",
    "reviewed_documentation",
    "reviewed_other",
    "unresolved",
})

FORBIDDEN_PREFIXES = (
    "build/",
    "cache/",
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
SPDX_PROVENANCE_SOURCE_EXTENSIONS = SOURCE_EXTENSIONS | {
    ".bat", ".cjs", ".cmd", ".cpp", ".css", ".frag", ".go", ".inc",
    ".java", ".js", ".jsx", ".mjs", ".mk", ".mts", ".ps1", ".rs",
    ".s", ".scss", ".ts", ".tsx", ".vert",
}
SPDX_PROVENANCE_SOURCE_NAMES = frozenset({"CMakeLists.txt", "Makefile"})
KEY_NAME = re.compile(r"(?:kirk|amctrl|pgd|key|vkey|seed|iv|secret|token)", re.IGNORECASE)
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

#: Technical debt management ceilings (Issue #188 Finding 11 O-11).
#: Ceilings are non-increasing: counts must not rise without explicit budget edit and rationale.
DEBT_BUDGETS: dict[str, int] = {
    "ruff_select_rule_families": 4,
    "first_party_todos": 7,
    "powershell_silently_continue": 53,
}

POWERSHELL_SILENTLY_CONTINUE_INVENTORY: dict[str, int] = {
    "copy_build_assets.ps1": 1,
    "nk_manager.ps1": 25,
    "tools/nk_safety.ps1": 11,
    "tools/test_manager_safety.ps1": 3,
    "tools/test_visual_oracle.ps1": 3,
    "tools/title_manager_plan.ps1": 9,
    "tools/vulkan_sdk.ps1": 1,
}

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


def parse_secret_scan_report(report_path: Path) -> list[Finding]:
    """Parse a Betterleaks/Gitleaks-compatible JSON report without echoing secrets.

    Betterleaks deliberately keeps the report shape compatible with the older
    Gitleaks JSON output.  The publication audit therefore consumes the
    neutral schema and emits neutral finding codes; callers do not need to
    know which compatible scanner produced the report.
    """
    if not report_path.is_file():
        return [Finding("SECRET_SCAN", str(report_path), "secret scanner report file not found")]
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
                    findings.append(Finding("SECRET_SCAN_LEAK", file_p, detail))
    except Exception as exc:
        findings.append(Finding("SECRET_SCAN_PARSE", str(report_path), f"failed to parse secret scanner report: {exc}"))
    return findings


def parse_gitleaks_report(report_path: Path) -> list[Finding]:
    """Backward-compatible alias for integrations using the old function name."""
    return parse_secret_scan_report(report_path)


def _git_output(cmd: list[str], repo_root: Path = ROOT) -> str:
    res = subprocess.run(
        ["git", *cmd],
        cwd=repo_root,
        env=isolated_git_env(root=repo_root),
        capture_output=True,
        check=False,
    )
    if res.returncode != 0:
        err = res.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git {' '.join(cmd)} failed: {err}")
    return res.stdout.decode("utf-8", errors="replace")


GITATTRIBUTES_REL = ".gitattributes"


def _parse_gitattributes_lfs_patterns(text: str) -> set[str]:
    """Extract the path patterns this file declares for Git LFS.

    Only an exact ``filter=lfs`` attribute token counts; substring matches such
    as ``subfilter=lfs`` are a different attribute and must not be silently
    treated as an LFS declaration.
    """
    patterns = set()
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 2 and any(part == "filter=lfs" for part in parts[1:]):
            patterns.add(parts[0])
    return patterns


def _read_git_lfs_attributes(
    repo_root: Path = ROOT,
    entries: list[GitEntry] | None = None,
    content_map: dict[str, tuple[bytes | None, str | None]] | None = None,
    content_source: str = CONTENT_INDEX,
) -> tuple[set[str], list[Finding]]:
    """Read ``.gitattributes`` from the SAME selected content source as the audit.

    The attribute policy decides tracked-entry findings (``LFS_MISMATCH`` and
    ``LFS_UNTRACKED_POINTER``), so reading it from the raw filesystem while the
    audited bytes come from the index would be a mixed-tree seam: an index-vs-
    worktree divergence in ``.gitattributes`` could silently change the verdict.
    The three sources therefore read it exactly like every other control file:

    * ``index`` -- the staged blob behind ``.gitattributes`` (disk edits are
      invisible, matching how indexed file content is read);
    * ``worktree`` / ``candidate`` / ``committed`` -- the materialized bytes
      under *repo_root*.

    A ``.gitattributes`` that is part of the audited source but cannot be read
    or decoded fails closed with ``GITATTRIBUTES_UNREADABLE``; an absent file
    means no declared LFS policy and yields an empty pattern set without a
    finding.
    """

    def _disk_read() -> tuple[bytes | None, Finding | None]:
        path = repo_root / GITATTRIBUTES_REL
        listed = entries is not None and GITATTRIBUTES_REL in {entry.path for entry in entries}
        if not path.exists():
            if listed:
                return None, Finding(
                    "GITATTRIBUTES_UNREADABLE", GITATTRIBUTES_REL,
                    "declared attribute policy is listed in the audited source but missing on disk",
                )
            return None, None
        try:
            return path.read_bytes(), None
        except OSError as error:
            return None, Finding(
                "GITATTRIBUTES_UNREADABLE", GITATTRIBUTES_REL,
                f"declared attribute policy cannot be read: {error}",
            )

    raw: bytes | None = None
    if content_source == CONTENT_INDEX:
        if content_map is not None and GITATTRIBUTES_REL in content_map:
            cached, read_error = content_map[GITATTRIBUTES_REL]
            if cached is None:
                return set(), [Finding(
                    "GITATTRIBUTES_UNREADABLE", GITATTRIBUTES_REL,
                    f"declared attribute policy cannot be read from the index: "
                    f"{read_error or 'missing'}",
                )]
            raw = cached
        else:
            # Not among the audited entries (selected-file helper) or no cache
            # supplied: resolve the staged blob directly so the patterns still
            # come from the index, never from worktree edits.
            try:
                listing = _git_output(
                    ["ls-files", "-s", "--", GITATTRIBUTES_REL], repo_root=repo_root
                ).split()
            except RuntimeError:
                # No usable index (a real index audit has already driven Git
                # successfully to build `entries`, so this only fires for
                # in-process helper calls on a non-Git fixture root). There the
                # filesystem is the only coherent source; read it fail closed.
                raw, disk_finding = _disk_read()
                if disk_finding is not None:
                    return set(), [disk_finding]
            else:
                if not listing:
                    # Not part of the audited index; untracked disk copies are
                    # outside an index audit's content source.
                    return set(), []
                res = subprocess.run(
                    ["git", "cat-file", "blob", listing[1]],
                    cwd=repo_root,
                    env=isolated_git_env(root=repo_root),
                    capture_output=True,
                    check=False,
                )
                if res.returncode != 0:
                    return set(), [Finding(
                        "GITATTRIBUTES_UNREADABLE", GITATTRIBUTES_REL,
                        "git cat-file failed to read the indexed attribute policy blob",
                    )]
                raw = res.stdout
    else:
        raw, disk_finding = _disk_read()
        if disk_finding is not None:
            return set(), [disk_finding]

    if raw is None:
        return set(), []
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        return set(), [Finding(
            "GITATTRIBUTES_UNREADABLE", GITATTRIBUTES_REL,
            f"declared attribute policy cannot be parsed: {error}",
        )]
    return _parse_gitattributes_lfs_patterns(text), []


def _is_contained_in_root(path: Path | str, root: Path | str) -> bool:
    """Verify that path physically resolves strictly inside root.

    Uses os.path.realpath to resolve all symlinks, NTFS junctions, 8.3 short-name
    aliases, and reparse points, then asserts canonical prefix containment.
    """
    try:
        resolved_path = os.path.realpath(path)
        resolved_root = os.path.realpath(root)
        common = os.path.commonpath([resolved_path, resolved_root])
        return os.path.normcase(common) == os.path.normcase(resolved_root)
    except (ValueError, OSError):
        return False


def _is_host_junction(path: Path) -> bool:
    """True for an NTFS junction, as distinct from a symlink.

    A symlink's target text is tracked content -- it is literally the blob Git
    stores and would publish -- so it is auditable. A junction is host state
    Git never records, and its target is an absolute local path, so it must not
    be treated as content. ``os.path.isjunction`` exists only on newer Pythons
    and only reports true on Windows.
    """
    isjunction = getattr(os.path, "isjunction", None)
    if isjunction is None:
        return False
    try:
        return bool(isjunction(path)) and not path.is_symlink()
    except OSError:
        return False


def _filesystem_link_on_path(path: Path, root: Path | None = None) -> Path | None:
    """Return the first symlink or host directory-link on *path*, if any.

    ``os.walk(..., followlinks=False)`` prunes POSIX directory symlinks, but on
    Windows an NTFS junction is not reported by ``Path.is_symlink()`` and the
    walk otherwise descends through it.  Keep that host distinction behind one
    seam so every audit path uses the same fail-closed link classification.  A
    root also makes Git-reported descendants safe: Git may enumerate a regular
    file below a junction even though the final file is not itself a link.
    """
    candidates = [path]
    if root is not None:
        root_absolute = root.absolute()
        try:
            relative = path.absolute().relative_to(root_absolute)
        except ValueError as error:
            raise RuntimeError(f"audit path escapes root: {path}") from error
        current = root_absolute
        candidates = [current]
        for part in relative.parts:
            current /= part
            candidates.append(current)

    isjunction = getattr(os.path, "isjunction", None)
    for candidate in candidates:
        if candidate.is_symlink() or (isjunction is not None and isjunction(candidate)):
            return candidate
        if root is not None and not _is_contained_in_root(candidate, root):
            return candidate
    return None


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
                if _filesystem_link_on_path(full_p, repo_root) is not None:
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
                link_path = _filesystem_link_on_path(full_p, repo_root)
                if link_path is not None:
                    link_rel = link_path.relative_to(repo_root).as_posix()
                    if link_rel not in tracked_paths:
                        entries.append(
                            GitEntry(
                                mode="120000",
                                sha="",
                                stage="0",
                                path=link_rel,
                                kind="symlink",
                                working_mode="120000",
                            )
                        )
                        tracked_paths.add(link_rel)
                    continue
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
    if _filesystem_link_on_path(repo_root) is not None:
        raise RuntimeError(f"candidate root must not be a filesystem link: {repo_root}")
    entries: list[GitEntry] = []
    for directory, dirnames, filenames in os.walk(repo_root, followlinks=False):
        base = Path(directory)
        # Prune VCS metadata directories before descending into them.
        dirnames[:] = [name for name in dirnames if name not in _VCS_METADATA_DIRS]
        for name in list(dirnames):
            path = base / name
            if _filesystem_link_on_path(path, repo_root) is not None:
                rel = path.relative_to(repo_root).as_posix()
                entries.append(GitEntry("120000", "", "0", rel, "symlink", working_mode="120000"))
                dirnames.remove(name)
        for name in filenames:
            path = base / name
            rel = path.relative_to(repo_root).as_posix()
            mode = "100644"
            kind = "file"
            if _filesystem_link_on_path(path, repo_root) is not None:
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
            env=isolated_git_env(root=repo_root),
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
            env=isolated_git_env(root=repo_root),
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

    if entry.kind == "gitlink":
        return entry.sha.encode("utf-8"), None

    # An index audit is bound to the indexed blob, so the recorded sha always
    # wins. A worktree copy replaced by a symlink -- or a tracked regular file
    # that now sits below a junction -- would otherwise substitute the link
    # target text for the staged bytes, letting a content scan clear benign
    # link text while the tree being published holds something else entirely.
    # For an index entry that genuinely *is* a symlink (mode 120000) the blob
    # content is the target text, so the same read is correct there too. The
    # worktree link is reported separately by the audit rather than by changing
    # which bytes are read.
    if entry.sha:
        try:
            res = subprocess.run(
                ["git", "cat-file", "-p", entry.sha],
                cwd=repo_root,
                env=isolated_git_env(root=repo_root),
                capture_output=True,
                check=False,
            )
            if res.returncode == 0:
                return res.stdout, None
            return None, f"git cat-file failed to read blob {entry.sha}"
        except Exception as exc:
            return None, f"failed to execute git cat-file: {exc}"

    # Fallback for untracked prospective files (sha is empty)
    if disk_path.is_file() and _filesystem_link_on_path(disk_path, repo_root) is None:
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
        if entry.kind == "gitlink":
            result[entry.path] = (entry.sha.encode("utf-8"), None)
            continue

        # See read_indexed_blob: the indexed blob is authoritative for an index
        # audit, whatever the worktree copy has since become.
        if entry.sha:
            shas_to_query.append((entry.path, entry.sha))
        else:
            if disk_path.is_file() and _filesystem_link_on_path(disk_path, repo_root) is None:
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
            env=isolated_git_env(root=repo_root),
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
        link_path = _filesystem_link_on_path(disk_path, repo_root)
        if link_path is not None:
            # Same rule as read_candidate_file: a junction's target is an
            # absolute host path Git never stores, so it is not this entry's
            # content and must not become its audited bytes -- doing so records
            # that path's length and SHA-256 into the JSON and CSV output. A
            # real symlink's target text *is* the blob Git would publish, so it
            # stays readable and stays analysed.
            if _is_host_junction(link_path):
                result[entry.path] = (
                    None, "path lies on a host directory junction and was not followed")
                continue
            try:
                result[entry.path] = (str(link_path.readlink()).encode("utf-8"), None)
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
        link_path = _filesystem_link_on_path(disk_path, candidate_root)
        if link_path is None:
            return None, "candidate link entry is not a filesystem link"
        # A junction's target is an absolute host path that Git never stores,
        # so it is not the entry's content and must not become its bytes: it
        # would flow into hashes, content previews and finding details, and a
        # junction into a private directory would then print that path into
        # audit and CI logs. Refuse it generically. A real symlink's target text
        # *is* the blob Git would publish, so it is still returned and still
        # analysed for containment.
        if _is_host_junction(link_path):
            return None, "candidate link is a host directory junction and was not followed"
        try:
            target = link_path.readlink()
        except OSError as exc:
            return None, f"failed to read candidate symlink target: {exc}"
        return str(target).encode("utf-8"), None

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
    normalized = path
    while normalized.startswith("./"):
        normalized = normalized[2:]
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
    # PSP PGF fonts carry a 4-byte header size/offset field before the "PGF0"
    # signature, so the real layout is <4 bytes> "PGF0" -- e.g. the checked-in
    # replacement fonts begin b"\x00\x00\x88\x01PGF0". The previous checks
    # (startswith b"\x00PGF" / b"PGF") therefore never fired on an actual PGF
    # file, leaving this defence-in-depth layer blind to exactly the file type
    # under provenance review. Match the signature at its real offset, and keep
    # the offset-0 forms for any variant that omits the header word.
    if data[4:8] == b"PGF0" or data.startswith((b"PGF0", b"\x00PGF")):
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


def _is_16_byte_array_text(body: str) -> bool:
    tokens = [t.strip() for t in body.split(",") if t.strip()]
    if len(tokens) != 16:
        return False
    for t in tokens:
        if t.startswith(("0x", "0X")):
            try:
                val = int(t, 16)
                if not (0 <= val <= 255):
                    return False
            except ValueError:
                return False
        elif t.isdigit():
            try:
                val = int(t, 10)
                if not (0 <= val <= 255):
                    return False
            except ValueError:
                return False
        else:
            return False
    return True


def _strip_c_comments(code: str) -> str:
    def replacer(match: re.Match[str]) -> str:
        s = match.group(0)
        if s.startswith("/"):
            return "".join("\n" if c == "\n" else " " for c in s)
        return s

    pattern = re.compile(
        r"//.*?$|/\*.*?\*/|'(?:\\.|[^\\'])*'|\"(?:\\.|[^\\\"])*\"",
        re.DOTALL | re.MULTILINE,
    )
    return re.sub(pattern, replacer, code)


def _c_key_assignment_lines(code: str) -> list[int]:
    clean = _strip_c_comments(code)
    lines: list[int] = []

    # 1. Macro definitions: #define NAME "32hex" or #define NAME { 16 bytes }
    define_pat = re.compile(
        r"^[ \t]*#[ \t]*define[ \t]+([a-zA-Z0-9_]+)[ \t]+(?:(?:"
        r'"([0-9a-fA-F]{32})"'
        r")|\{([^}]+)\})",
        re.MULTILINE,
    )
    for m in define_pat.finditer(clean):
        name = m.group(1)
        hex_val = m.group(2)
        array_val = m.group(3)
        if KEY_NAME.search(name):
            if hex_val:
                lines.append(clean[: m.start()].count("\n") + 1)
            elif array_val and _is_16_byte_array_text(array_val):
                lines.append(clean[: m.start()].count("\n") + 1)

    # 2. Variable declarations/assignments:
    var_pat = re.compile(
        r"\b([a-zA-Z_][a-zA-Z0-9_]*)\s*(?:\[[^\]]*\])?\s*=\s*(?:(?:"
        r'"([0-9a-fA-F]{32})"'
        r")|\{([^}]+)\})",
        re.MULTILINE,
    )
    for m in var_pat.finditer(clean):
        name = m.group(1)
        hex_val = m.group(2)
        array_val = m.group(3)
        if KEY_NAME.search(name):
            if hex_val:
                lines.append(clean[: m.start()].count("\n") + 1)
            elif array_val and _is_16_byte_array_text(array_val):
                lines.append(clean[: m.start()].count("\n") + 1)

    return sorted(set(lines))


def _json_key_assignment_lines(text: str) -> list[int]:
    lines: list[int] = []
    json_pat = re.compile(
        r'"([a-zA-Z0-9_]+)"\s*:\s*(?:(?:"([0-9a-fA-F]{32})")|\[([^\]]+)\])',
        re.MULTILINE,
    )
    for m in json_pat.finditer(text):
        key = m.group(1)
        hex_val = m.group(2)
        array_val = m.group(3)
        if KEY_NAME.search(key):
            if hex_val:
                lines.append(text[: m.start()].count("\n") + 1)
            elif array_val and _is_16_byte_array_text(array_val):
                lines.append(text[: m.start()].count("\n") + 1)
    return sorted(set(lines))


def _py_key_assignment_lines(text: str) -> list[int]:
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return []
    lines: list[int] = []
    for node in ast.walk(tree):
        targets: list[ast.expr] = []
        value: ast.AST | None = None
        if isinstance(node, (ast.Assign, ast.AnnAssign)):
            targets = list(node.targets) if isinstance(node, ast.Assign) else [node.target]
            value = node.value

        is_key_value = False
        literal = _assigned_literal(value)
        if literal is not None and HEX_16_BYTES.fullmatch(literal) is not None:
            is_key_value = True
        elif isinstance(value, ast.Constant) and isinstance(value.value, bytes) and len(value.value) == 16:
            is_key_value = True
        elif isinstance(value, (ast.List, ast.Tuple)) and len(value.elts) == 16:
            if all(
                isinstance(e, ast.Constant) and isinstance(e.value, int) and 0 <= e.value <= 255
                for e in value.elts
            ):
                is_key_value = True
        elif (
            isinstance(value, ast.Call)
            and isinstance(value.func, ast.Name)
            and value.func.id in ("bytes", "bytearray")
        ):
            if value.args and isinstance(value.args[0], (ast.List, ast.Tuple)) and len(value.args[0].elts) == 16:
                if all(
                    isinstance(e, ast.Constant) and isinstance(e.value, int) and 0 <= e.value <= 255
                    for e in value.args[0].elts
                ):
                    is_key_value = True

        if not is_key_value:
            continue

        for target in targets:
            if isinstance(target, ast.Name) and KEY_NAME.search(target.id):
                assert isinstance(node, (ast.Assign, ast.AnnAssign))
                lines.append(node.lineno)
                break
    return sorted(set(lines))


def private_key_assignment_lines(text: str, filename: str = "") -> list[int]:
    ext = ""
    if filename:
        ext = PurePosixPath(filename).suffix.lower()
    if ext in {".c", ".h"}:
        return _c_key_assignment_lines(text)
    if ext == ".json":
        return _json_key_assignment_lines(text)
    if ext == ".py":
        return _py_key_assignment_lines(text)

    # Fallback when filename is omitted: try Python parser, then C, then JSON
    py_lines = _py_key_assignment_lines(text)
    if py_lines:
        return py_lines
    c_lines = _c_key_assignment_lines(text)
    if c_lines:
        return c_lines
    return _json_key_assignment_lines(text)


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

# A small set of byte-exact FFmpeg headers already carry an SPDX identifier.
# Keep those existing lineage declarations without requiring new SPDX headers
# across the imported trees.
SPDX_BYTE_EXACT_IMPORT_HEADER_PATHS = frozenset({
    "src/rt/atrac3p/libavcodec/avcodec.h",
    "src/rt/atrac3p/libavcodec/config.h",
    "src/rt/atrac3p/libavcodec/internal.h",
    "src/rt/atrac3p/libavutil/avconfig.h",
    "src/rt/atrac3p/libavutil/avutil.h",
    "src/rt/atrac3p/libavutil/config.h",
    "src/rt/atrac3p/libavutil/libm.h",
    "src/rt/atrac3p/libavutil/mathematics.h",
    "src/rt/atrac3p/libavutil/thread.h",
})

PROJECT_SPDX_IDENTIFIER = "GPL-3.0-or-later"
SPDX_IDENTIFIER_LINE = re.compile(r"SPDX-License-Identifier:\s*(.*?)\s*(?:\*/)?\s*$")


def _spdx_required(path: str) -> bool:
    pure = PurePosixPath(path)
    if pure.suffix.lower() not in SOURCE_EXTENSIONS:
        return False
    if path.startswith(SPDX_BYTE_EXACT_IMPORT_TREES):
        return False
    if path.startswith(("src/", "tools/")):
        return True
    return len(pure.parts) == 1


def _spdx_identifiers(raw_bytes: bytes) -> list[str]:
    try:
        lines = raw_bytes.decode("utf-8-sig").splitlines()[:8]
    except UnicodeDecodeError:
        return []
    return [match.group(1) for line in lines if (match := SPDX_IDENTIFIER_LINE.search(line))]


# SPIR-V embeddings generated by tools/shader_embed.py are bare uint32 word
# arrays whose byte-exact contract admits no comment text. Their lineage is
# carried by (and audited on) the GLSL source each one is compiled from.
SPDX_GENERATED_EMBEDDING_SOURCES = {
    "src/rt/gpu_sdl3vk/psp_frag.inc": "src/rt/gpu_sdl3vk/shaders/psp.frag",
    "src/rt/gpu_sdl3vk/psp_vert.inc": "src/rt/gpu_sdl3vk/shaders/psp.vert",
}


def _spdx_provenance_findings(path: str, record: dict, raw_bytes: bytes) -> list[Finding]:
    """Check source SPDX declarations against the public provenance class.

    The public profile supplies the audited path set and the public provenance
    ledger supplies each path's class and, for inherited sources, its license.
    Byte-exact FFmpeg paths that already have an SPDX line are checked too,
    while neighboring imported files remain exempt from acquiring one.
    """
    classification = record.get("classification")
    pure = PurePosixPath(path)
    if pure.suffix.lower() not in SPDX_PROVENANCE_SOURCE_EXTENSIONS and pure.name not in SPDX_PROVENANCE_SOURCE_NAMES:
        return []

    if path in SPDX_GENERATED_EMBEDDING_SOURCES:
        return []
    identifiers = _spdx_identifiers(raw_bytes)
    if classification == "project_authored_attested":
        if not identifiers and not _spdx_required(path):
            return []
        wrong = [identifier for identifier in identifiers if identifier != PROJECT_SPDX_IDENTIFIER]
        if not identifiers or wrong:
            actual = ", ".join(identifiers) if identifiers else "missing"
            return [Finding(
                "SPDX_PROJECT_LICENSE",
                path,
                f"project-authored source must declare {PROJECT_SPDX_IDENTIFIER}; found {actual}",
            )]
    elif classification == "upstream_derived":
        byte_exact_import = path.startswith(SPDX_BYTE_EXACT_IMPORT_TREES)
        if not identifiers and byte_exact_import and path not in SPDX_BYTE_EXACT_IMPORT_HEADER_PATHS:
            return []
        evidence = record.get("evidence") if isinstance(record.get("evidence"), dict) else {}
        expected = evidence.get("license")
        if not isinstance(expected, str) or not expected or expected == "see NOTICE.md":
            return [Finding("SPDX_UPSTREAM_LICENSE_UNKNOWN", path, "upstream provenance does not name an SPDX license")]
        if expected not in identifiers:
            return [Finding(
                "SPDX_UPSTREAM_LINEAGE",
                path,
                f"upstream SPDX lineage {expected} is missing from the source header",
            )]
    return []


#: Known, unresolved contradictory lineage claims tracked under Issue #342.
#: The publication audit gate enforces that any contradiction must be listed
#: here with a reason and issue tracking, and that no stale exceptions remain.
LINEAGE_CONTRADICTION_EXCEPTIONS: dict[str, dict[str, str | int]] = {
    "src/rt/evf.h": {
        "issue": "#342",
        "reason": "header declares derivation from PPSSPP while ledger classifies as project_authored_attested (behavior-informed)",
    },
    "tools/gen_nidnames.py": {
        "issue": "#342",
        "reason": "header declares derivation from sal063 while ledger classifies as project_authored_attested",
    },
    "tools/test_funcdiff_cmp.py": {
        "issue": "#342",
        "reason": "header declares derivation from sal063 while ledger classifies as project_authored_attested",
    },
    "tools/test_nidseq.py": {
        "issue": "#342",
        "reason": "header declares derivation from sal063 while ledger classifies as project_authored_attested",
    },
}

LINEAGE_DERIVED_LINE = re.compile(
    r"^\s*(?://|#|/\*|\*)\s*(?:.*?\b)?(?:"
    r"(?:derived\s+from|ported\s+from)\s+(?!(?:the|this|a|an|its|each|our)\b)([A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)?)"
    r"|based\s+on\s+(?:.*?\b)?(?:ppsspp|sal063|jpcsp|upstream|[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)"
    r"|(?:ports?|porting)\s+(?:of\s+)?(?:ppsspp|sal063|jpcsp)\b"
    r"|(?:ppsspp|sal063|jpcsp)\s+(?:port|derived|derivative)\b"
    r")",
    re.IGNORECASE,
)

LINEAGE_INDEPENDENT_LINE = re.compile(
    r"^\s*(?://|#|/\*|\*)\s*(?:.*?\b)?(?:"
    r"\b(?:independently?\s+(?:authored|implemented|developed|written)"
    r"|(?:authored|implemented|developed|written)\s+independently"
    r"|clean[- ]room"
    r"|independent\s+implementation)\b"
    r")",
    re.IGNORECASE,
)


def _lineage_contradiction_findings(
    path: str,
    record: dict,
    raw_bytes: bytes,
    exceptions: dict[str, dict[str, str | int]] | None = None,
) -> tuple[list[Finding], bool]:
    """Detect contradictory lineage claims between source headers and ledger classification.

    Flags a source file whose header declares upstream derivation while its public
    ledger classification is project-authored or independent, or whose header claims
    independence while its ledger classification is upstream-derived.
    """
    pure = PurePosixPath(path)
    if pure.suffix.lower() not in SPDX_PROVENANCE_SOURCE_EXTENSIONS and pure.name not in SPDX_PROVENANCE_SOURCE_NAMES:
        return [], False

    if path in SPDX_GENERATED_EMBEDDING_SOURCES:
        return [], False

    try:
        lines = raw_bytes.decode("utf-8-sig").splitlines()[:15]
    except UnicodeDecodeError:
        return [], False

    derived_match: str | None = None
    indep_match: str | None = None
    for line in lines:
        if derived_match is None and (m := LINEAGE_DERIVED_LINE.search(line)):
            derived_match = m.group(0).strip()
        if indep_match is None and (m := LINEAGE_INDEPENDENT_LINE.search(line)):
            indep_match = m.group(0).strip()

    classification = record.get("classification")
    is_contradiction = False
    detail = ""

    if classification == "project_authored_attested" and derived_match is not None:
        is_contradiction = True
        detail = (
            f"source header indicates upstream derivation ({derived_match!r}) "
            f"while ledger classification is {classification!r}"
        )
    elif classification == "upstream_derived" and indep_match is not None:
        is_contradiction = True
        detail = (
            f"source header claims independence ({indep_match!r}) "
            f"while ledger classification is {classification!r}"
        )

    if not is_contradiction:
        return [], False

    effective_exceptions = exceptions if exceptions is not None else LINEAGE_CONTRADICTION_EXCEPTIONS
    if path in effective_exceptions:
        exc_entry = effective_exceptions[path]
        if not isinstance(exc_entry, dict) or not exc_entry.get("issue") or not exc_entry.get("reason"):
            return [Finding(
                "LINEAGE_EXCEPTION_INVALID",
                path,
                f"lineage contradiction exception for {path} must specify 'issue' and 'reason'",
            )], True
        return [], True

    return [Finding("LINEAGE_CONTRADICTION", path, detail)], True


def _stale_lineage_exception_findings(
    exceptions: dict[str, dict[str, str | int]],
    matched_exceptions: set[str],
    entry_by_path: dict[str, GitEntry] | None = None,
    policy_include_paths: set[str] | None = None,
    is_custom_exceptions: bool = False,
) -> list[Finding]:
    """Fail on any lineage contradiction exception whose contradiction no longer exists."""
    findings: list[Finding] = []
    for exc_path, info in sorted(exceptions.items()):
        if not isinstance(info, dict) or not info.get("issue") or not info.get("reason"):
            findings.append(Finding(
                "LINEAGE_EXCEPTION_INVALID",
                exc_path,
                "lineage contradiction exception must specify 'issue' and 'reason'",
            ))
        if exc_path in matched_exceptions:
            continue
        if is_custom_exceptions:
            findings.append(Finding(
                "LINEAGE_CONTRADICTION_STALE",
                exc_path,
                f"lineage contradiction exception for {exc_path} is stale; no contradictory lineage claim found in source header",
            ))
        elif entry_by_path is not None and policy_include_paths is not None:
            if exc_path in entry_by_path and exc_path in policy_include_paths:
                findings.append(Finding(
                    "LINEAGE_CONTRADICTION_STALE",
                    exc_path,
                    f"lineage contradiction exception for {exc_path} is stale; no contradictory lineage claim found in source header",
                ))
            elif len(entry_by_path) > 50:
                findings.append(Finding(
                    "LINEAGE_CONTRADICTION_STALE",
                    exc_path,
                    f"lineage contradiction exception for {exc_path} is stale; path not found in audited source",
                ))
    return findings


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


def _debt_budget_findings(repo_root: Path = ROOT, paths: list[str] | None = None) -> list[Finding]:
    """Audit unmanaged debt surfaces against non-increasing budgets (Issue #188 Finding 11 O-11).

    Fails when any of the three measured surfaces (ruff select families,
    first-party debt markers, or PowerShell SilentlyContinue) exceeds its configured ceiling.
    """
    findings: list[Finding] = []

    # 1. Ruff select rule families (pyproject.toml)
    pyproject = repo_root / "pyproject.toml"
    if pyproject.is_file():
        text = _text(pyproject) or ""
        m = re.search(r"select\s*=\s*\[(.*?)\]", text, re.DOTALL)
        if m:
            items = re.findall(r'["\']([a-zA-Z0-9_-]+)["\']', m.group(1))
            obs = len(items)
            ceiling = DEBT_BUDGETS["ruff_select_rule_families"]
            if obs > ceiling:
                findings.append(
                    Finding("DEBT_BUDGET", "pyproject.toml",
                            f"Ruff select rule families count {obs} exceeds debt ceiling {ceiling}")
                )

    # 2. First-party debt markers
    marker_pat = re.compile(r"\b(?:" + r"TO" + r"DO|FIX" + r"ME|HA" + r"CK)\b")
    excluded_prefixes = (
        "third_party/",
        "src/rt/atrac3p/libavcodec/",
        "build/",
        "fs/",
    )
    all_paths = paths
    if all_paths is None:
        try:
            res = subprocess.run(
                ["git", "ls-files"],
                cwd=repo_root,
                env=isolated_git_env(root=repo_root),
                capture_output=True,
                text=True,
                check=True,
            )
            all_paths = [f.strip().replace("\\", "/") for f in res.stdout.splitlines() if f.strip()]
        except Exception:
            all_paths = []
    todo_count = 0
    for rel in all_paths:
        if any(rel.startswith(p) for p in excluded_prefixes):
            continue
        full_path = repo_root / rel
        if not full_path.is_file():
            continue
        text = _text(full_path)
        if text is None:
            continue
        for line in text.splitlines():
            if marker_pat.search(line):
                todo_count += 1
    ceiling = DEBT_BUDGETS["first_party_todos"]
    if todo_count > ceiling:
        findings.append(
            Finding("DEBT_BUDGET", "tools/publish_audit.py",
                    f"First-party debt marker count {todo_count} exceeds debt ceiling {ceiling}")
        )

    # 3. PowerShell SilentlyContinue
    ps_pat = re.compile(r"SilentlyContinue", re.IGNORECASE)
    ps_count = 0
    for rel in all_paths:
        if not rel.endswith(".ps1"):
            continue
        full_path = repo_root / rel
        if not full_path.is_file():
            continue
        text = _text(full_path)
        if text is None:
            continue
        m = ps_pat.findall(text)
        ps_count += len(m)
    ceiling = DEBT_BUDGETS["powershell_silently_continue"]
    if ps_count > ceiling:
        findings.append(
            Finding("DEBT_BUDGET", "nk_manager.ps1",
                    f"PowerShell SilentlyContinue count {ps_count} exceeds debt ceiling {ceiling}")
        )

    return findings


#: Text bytes the repository governs as UTF-8 without BOM, LF, final newline.
#: `.gitattributes` normalises on commit, so the index is clean by construction
#: and these findings fire almost entirely in worktree audits -- which is the
#: point. A CRLF-polluted checkout produces content hashes that disagree with the
#: provenance ledger, and the resulting wall of PROVENANCE_CONTENT_MISMATCH says
#: nothing about the real cause. Naming the cause turns a confusing hash failure
#: into a one-line diagnosis.
TEXT_HYGIENE_EXEMPT_SUFFIXES = frozenset({".dat", ".bin", ".png", ".jpg", ".jpeg",
                                          ".gif", ".ico", ".pdf", ".zip", ".ttf",
                                          ".otf", ".woff", ".woff2", ".spv", ".wav"})


def _text_hygiene_is_text_suffix(path: str) -> bool:
    """Return whether a path is governed text rather than an explicitly binary suffix."""
    return PurePosixPath(path).suffix.lower() not in TEXT_HYGIENE_EXEMPT_SUFFIXES


def check_text_hygiene(content_map: dict[str, tuple[bytes | None, str | None]]) -> list[Finding]:
    """Flag encoding and line-ending drift in LF-governed tracked text.

    Encoding is checked before the binary heuristic. UTF-16 text commonly contains
    NUL bytes, so looking for binary content first would silently skip it. A
    text-suffixed path whose bytes are not decodable as UTF-8 is also a finding:
    otherwise a candidate can hide a private path (or any other invalid text) in
    an undecodable blob while the text block treats it as binary and moves on.
    Explicitly binary suffixes remain exempt.
    """
    findings: list[Finding] = []
    for path in sorted(content_map):
        data, read_error = content_map[path]
        if read_error:
            continue
        if not data or not _text_hygiene_is_text_suffix(path):
            continue
        if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
            findings.append(Finding(
                "TEXT_ENCODING_UTF16", path,
                "file is UTF-16; repository text is UTF-8 without BOM"))
            continue
        if data[:3] == b"\xef\xbb\xbf":
            findings.append(Finding(
                "TEXT_ENCODING_BOM", path,
                "file begins with a UTF-8 BOM; repository text is UTF-8 without BOM"))
            continue
        try:
            data.decode("utf-8")
        except UnicodeDecodeError as error:
            findings.append(Finding(
                "TEXT_ENCODING_UNDECODABLE", path,
                f"text-suffixed file is not valid UTF-8: {error}"))
            continue
        if _is_binary_bytes(data):
            continue
        if b"\r\n" in data:
            findings.append(Finding(
                "TEXT_LINE_ENDING_CRLF", path,
                "file contains CRLF; repository text is LF (.gitattributes eol=lf). "
                "A writer used a host default instead of an explicit LF newline"))
        if not data.endswith(b"\n"):
            findings.append(Finding(
                "TEXT_FINAL_NEWLINE", path,
                "text file does not end with a newline"))
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

    for matches in case_map.values():
        if len(matches) > 1:
            findings.append(Finding("COLLISION_CASE", matches[0], f"case-insensitive collision between {matches}"))

    for matches in nfc_map.values():
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
            for excluded in data.get("excluded_paths", []):
                if isinstance(excluded, str):
                    sp = excluded
                    comp = {
                        "id": f"policy-exclusion:{sp}",
                        "source_path": sp,
                        "type": "documentation",
                        "presence": "excluded_from_public_profile",
                        "disposition": "excluded_by_publication_policy",
                        "public_scope_included": False,
                    }
                elif isinstance(excluded, dict):
                    sp = excluded.get("path") or excluded.get("source_path")
                    comp = {
                        "id": excluded.get("id", f"policy-exclusion:{sp}"),
                        "source_path": sp,
                        "type": excluded.get("type", "documentation"),
                        "presence": "excluded_from_public_profile",
                        "disposition": excluded.get("disposition", "excluded_by_publication_policy"),
                        "public_scope_included": False,
                        "comment": excluded.get("comment", ""),
                    }
                else:
                    continue
                if isinstance(sp, str) and sp:
                    if sp in manifest_entries:
                        continue
                    manifest_entries[sp] = comp
    except Exception as exc:
        findings.append(Finding("MANIFEST_ERROR", str(manifest_path), f"failed to load/parse manifest: {exc}"))

    return manifest_entries, findings


def load_release_manifest_bytes(raw: bytes | None, display_path: str) -> tuple[dict[str, dict], list[Finding]]:
    """Parse a manifest blob read from the audited source (index/candidate)."""
    findings: list[Finding] = []
    manifest_entries: dict[str, dict] = {}
    if raw is None:
        return manifest_entries, [Finding("MANIFEST_ERROR", display_path, "release manifest is missing from the audited source")]
    try:
        data = json.loads(raw.decode("utf-8"))
        if not isinstance(data, dict) or "components" not in data or not isinstance(data["components"], list):
            return manifest_entries, [Finding("MANIFEST_ERROR", display_path, "manifest missing valid 'components' array")]
        for comp in data["components"]:
            if not isinstance(comp, dict):
                continue
            source_path = comp.get("source_path") or comp.get("provenance_path")
            if source_path:
                if source_path in manifest_entries:
                    findings.append(Finding("MANIFEST_DUPLICATE_PATH", source_path, f"duplicate manifest entry for path {source_path}"))
                manifest_entries[source_path] = comp
        for excluded in data.get("excluded_paths", []):
            if isinstance(excluded, str):
                source_path = excluded
                comp = {
                    "id": f"policy-exclusion:{source_path}",
                    "source_path": source_path,
                    "type": "documentation",
                    "presence": "excluded_from_public_profile",
                    "disposition": "excluded_by_publication_policy",
                    "public_scope_included": False,
                }
            elif isinstance(excluded, dict):
                source_path = excluded.get("path") or excluded.get("source_path")
                comp = {
                    "id": excluded.get("id", f"policy-exclusion:{source_path}"),
                    "source_path": source_path,
                    "type": excluded.get("type", "documentation"),
                    "presence": "excluded_from_public_profile",
                    "disposition": excluded.get("disposition", "excluded_by_publication_policy"),
                    "public_scope_included": False,
                    "comment": excluded.get("comment", ""),
                }
            else:
                continue
            if isinstance(source_path, str) and source_path and source_path not in manifest_entries:
                manifest_entries[source_path] = comp
    except (UnicodeDecodeError, json.JSONDecodeError, TypeError) as error:
        findings.append(Finding("MANIFEST_ERROR", display_path, f"failed to load/parse manifest: {error}"))
    return manifest_entries, findings


def _default_provenance(
    rel_path: str,
    manifest_comp: dict | None,
    resolution: Resolution | None = None,
) -> tuple[str, str, str, str, str, bool]:
    """Return provenance/licence *metadata* for a path.

    Publication eligibility is NOT decided here. It is decided by the canonical
    policy (``assets/public_source_profile.json``) and passed in as ``resolution``.
    This function only answers "what provenance, licence and notice apply", which
    is a notice-correctness question, not a safety-critical one.

    The historical behaviour — treating any unlisted path under ``src/`` or
    ``tools/`` as project-authored *and therefore publishable* — is exactly what
    let nine of the fifteen excluded paths through on 2026-08-11. The prefix rules
    below survive only as metadata defaults; they can no longer grant publication.

    The manifest may make a path *more* restrictive than the policy, never less.
    """
    policy_public = resolution.disposition == INCLUDED if resolution is not None else False

    if manifest_comp:
        prov = manifest_comp.get("provenance_class") or manifest_comp.get("type") or "unclassified"
        lic = manifest_comp.get("license", "unspecified")
        notice = manifest_comp.get("notice_path", "NOTICE.md")
        gen = manifest_comp.get("generated_source") or ("asset" if manifest_comp.get("type") == "asset" else "source")
        disp = manifest_comp.get("disposition", "included")
        pub = bool(manifest_comp.get("public_scope_included", True)) and policy_public
        return prov, lic, notice, gen, disp, pub

    pure = PurePosixPath(rel_path)
    ext = pure.suffix.lower()

    # Metadata-only classification below. Every branch returns ``policy_public``
    # for the publication flag: these prefixes describe what a file *is*, never
    # whether it may be published. Only the canonical policy decides that.
    if rel_path in REQUIRED_PATHS or rel_path in ("NOTICE.md", "LICENSE", "README.md", "AGENTS.md", "CODE_OF_CONDUCT.md", "CONTRIBUTING.md", "DEDICATION.md", "ISSUES.md", "SECURITY.md", "TODO.md"):
        return "notice_doc", "GPL-3.0-or-later", rel_path if rel_path in ("LICENSE", "NOTICE.md") else "NOTICE.md", "documentation", "included", policy_public

    if rel_path.startswith("THIRD_PARTY_LICENSES/"):
        return "notice_doc", "BSD-3-Clause", rel_path, "documentation", "included", policy_public

    if rel_path.startswith("assets/vfpu/"):
        return "ppsspp_derived", "GPL-2.0-or-later", "NOTICE.md", "data", "included", policy_public

    if rel_path.startswith(".github/") or rel_path.startswith(".") or rel_path in (".gitignore", ".gitattributes", ".clang-format", ".editorconfig", ".markdownlint-cli2.jsonc", ".pre-commit-config.yaml"):
        return "project_authored", "GPL-2.0-or-later", "NOTICE.md", "configuration", "included", policy_public

    if rel_path.startswith(("src/", "tools/", "mk/", "assets/", "fixtures/", "docs/")) or ext in SOURCE_EXTENSIONS or ext in (".md", ".txt", ".json", ".jsonc", ".yml", ".yaml", ".toml", ".ps1") or rel_path in ("Makefile", "pyproject.toml", "copy_build_assets.ps1", "nk.ps1", "nk_manager.ps1"):
        gen_kind = "documentation" if (rel_path.startswith("docs/") or ext == ".md") else ("data" if ext in (".json", ".jsonc", ".dat") else ("script" if ext in (".ps1", ".sh") else "source"))
        return "project_authored", "GPL-2.0-or-later", "NOTICE.md", gen_kind, "included", policy_public

    if rel_path.startswith("font/"):
        return "unresolved", "Unresolved", "THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt", "asset", "optional_excluded_if_unverified", False

    return "unclassified", "unspecified", "NOTICE.md", "source", "unclassified", False


def _export_staleness_findings(
    policy: Policy, export_path: Path | None, repo_root: Path, document: dict | None = None
) -> list[Finding]:
    """The generated export must carry the *current* policy digest.

    ``PUBLIC_EXPORT.json`` is generated evidence, not policy. If the policy has
    changed since the export was produced, the export describes a decision nobody
    made and must not be trusted.
    """
    resolved = export_path or (repo_root / "PUBLIC_EXPORT.json")
    if document is None:
        if not resolved.is_file():
            return [Finding("POLICY_EXPORT_MISSING", str(resolved),
                            "generated publication export is missing; cannot confirm it matches the policy")]
        try:
            document = json.loads(resolved.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            return [Finding("POLICY_EXPORT_UNREADABLE", str(resolved), f"cannot parse generated export: {error}")]

    findings: list[Finding] = []
    recorded = document.get("policy_sha256") or document.get("profile_sha256")
    if not recorded:
        findings.append(Finding("POLICY_EXPORT_STALE", str(resolved),
                                "generated export records no policy digest"))
    elif recorded != policy.digest:
        findings.append(Finding(
            "POLICY_EXPORT_STALE", str(resolved),
            f"generated export was produced against policy digest {recorded[:16]}... but the current "
            f"policy digest is {policy.digest[:16]}...; regenerate the export and re-audit"))
    if document.get("profile") not in (None, policy.name):
        findings.append(Finding("POLICY_EXPORT_STALE", str(resolved),
                                f"generated export names profile {document.get('profile')!r}, "
                                f"policy is {policy.name!r}"))
    return findings


def _load_json_document(raw: bytes | None, display_path: str) -> tuple[dict | None, list[Finding]]:
    if raw is None:
        return None, [Finding("CONTROL_MISSING", display_path, "required control file is absent from the audited source")]
    try:
        document = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        return None, [Finding("CONTROL_UNREADABLE", display_path, f"cannot parse audited control file: {error}")]
    if not isinstance(document, dict):
        return None, [Finding("CONTROL_UNREADABLE", display_path, "audited control file must contain a JSON object")]
    return document, []


def _source_control_bytes(
    rel_path: str,
    entries: list[GitEntry],
    content_map: dict[str, tuple[bytes | None, str | None]],
    content_source: str,
    repo_root: Path,
) -> tuple[bytes | None, str | None]:
    """Read a control blob from the same source as the audited file set."""
    if content_source in (CONTENT_INDEX, CONTENT_WORKTREE):
        if content_source == CONTENT_INDEX:
            return content_map.get(rel_path, (None, "control path is not present in the index"))
        path = repo_root / rel_path
        try:
            return path.read_bytes(), None
        except OSError as error:
            return None, str(error)
    if content_source == CONTENT_CANDIDATE:
        path = repo_root / rel_path
        try:
            return path.read_bytes(), None
        except OSError as error:
            return None, str(error)
    # CONTENT_COMMITTED is materialized by the CLI into a temporary directory;
    # the normal source-reader then follows the candidate path here.
    path = repo_root / rel_path
    try:
        return path.read_bytes(), None
    except OSError as error:
        return None, str(error)


def _provenance_ledger_findings(
    policy: Policy,
    entries: list[GitEntry],
    content_map: dict[str, tuple[bytes | None, str | None]],
    content_source: str,
    repo_root: Path,
    trusted_ledger_path: Path | None,
    self_consistency: bool = False,
    lineage_exceptions: dict[str, dict[str, str | int]] | None = None,
) -> list[Finding]:
    """Require explicit, externally trusted provenance for every included path.

    The candidate's own checked-in ledger is evidence, never an authorization
    source.  With ``--provenance-ledger`` the audited ledger is compared against
    an externally trusted copy; any divergence fails, so a candidate cannot
    self-authorize by naming a plausible detailed-ledger record that is not in
    the trusted release evidence.

    Without an externally trusted ledger the audit cannot verify attestation
    claims (the candidate's own ledger would be a self-referential trust
    anchor), so it reports ``PROVENANCE_UNVERIFIED`` and fails rather than
    passing on the candidate's own bytes.  The explicit ``self_consistency``
    scope keeps the developer tripwire useful: coverage, resolution, and
    content hashes are still enforced against the audited ledger itself, but
    the audit makes no attestation claim and says so.
    """
    findings: list[Finding] = []
    if trusted_ledger_path is not None:
        try:
            trusted_raw = trusted_ledger_path.read_bytes()
            trusted_document = json.loads(trusted_raw.decode("utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            return [Finding("PROVENANCE_LEDGER_UNREADABLE", str(trusted_ledger_path), str(error))]
        anchor_label = str(trusted_ledger_path)
    else:
        audited_raw, audited_error = _source_control_bytes(
            PROVENANCE_LEDGER_REL, entries, content_map, content_source, repo_root
        )
        if audited_error or audited_raw is None:
            findings.append(Finding(
                "PROVENANCE_LEDGER_MISSING", PROVENANCE_LEDGER_REL,
                audited_error or "missing",
            ))
            return findings
        trusted_raw = audited_raw
        try:
            trusted_document = json.loads(trusted_raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            findings.append(Finding("PROVENANCE_LEDGER_UNREADABLE", PROVENANCE_LEDGER_REL, str(error)))
            return findings
        anchor_label = PROVENANCE_LEDGER_REL
        if not self_consistency:
            findings.append(Finding(
                "PROVENANCE_UNVERIFIED", PROVENANCE_LEDGER_REL,
                "no externally trusted provenance ledger supplied via --provenance-ledger; the candidate's "
                "own ledger cannot attest its own provenance, so attestation claims are unverified and the "
                "audit cannot clear them (the release flow must supply the trusted ledger)",
            ))
    if not isinstance(trusted_document, dict) or not isinstance(trusted_document.get("entries"), list):
        return [Finding("PROVENANCE_LEDGER_UNREADABLE", anchor_label, "trusted ledger has no entries array")]

    trusted_by_path: dict[str, dict] = {}
    for record in trusted_document["entries"]:
        if not isinstance(record, dict) or not isinstance(record.get("path"), str):
            findings.append(Finding("PROVENANCE_LEDGER_INVALID", anchor_label, "ledger contains malformed entry"))
            continue
        path = record["path"]
        if path in trusted_by_path:
            findings.append(Finding("PROVENANCE_LEDGER_INVALID", path, "duplicate trusted provenance record"))
        trusted_by_path[path] = record
        if record.get("classification") not in PROVENANCE_CLASSES:
            findings.append(Finding("PROVENANCE_LEDGER_INVALID", path, "unsupported provenance classification"))
        if not isinstance(record.get("evidence"), dict) or not record["evidence"]:
            findings.append(Finding("PROVENANCE_MISSING", path, "record has no explicit provenance evidence"))

    audited_ledger, read_error = _source_control_bytes(
        PROVENANCE_LEDGER_REL, entries, content_map, content_source, repo_root
    )
    if trusted_ledger_path is not None:
        if read_error or audited_ledger is None:
            findings.append(Finding("PROVENANCE_LEDGER_MISSING", PROVENANCE_LEDGER_REL, read_error or "missing"))
        elif audited_ledger.replace(b"\r\n", b"\n") != trusted_raw.replace(b"\r\n", b"\n"):
            findings.append(Finding(
                "PROVENANCE_LEDGER_MISMATCH", PROVENANCE_LEDGER_REL,
                "audited ledger differs from the externally trusted ledger; candidate content cannot self-authorize provenance",
            ))

    entry_by_path = {entry.path: entry for entry in entries}
    record_phrase = "externally trusted provenance record" if trusted_ledger_path is not None else "provenance record"
    active_lineage_exceptions = (
        lineage_exceptions if lineage_exceptions is not None else LINEAGE_CONTRADICTION_EXCEPTIONS
    )
    matched_lineage_exceptions: set[str] = set()

    for path in sorted(policy.include_paths):
        if path not in entry_by_path:
            continue
        record = trusted_by_path.get(path)
        if record is None:
            findings.append(Finding(
                "PROVENANCE_MISSING", path,
                "included substantive path has no explicit trusted provenance record",
            ))
            continue
        if record.get("classification") == "unresolved":
            findings.append(Finding("PROVENANCE_UNRESOLVED", path, "trusted provenance record is explicitly unresolved"))
        raw, error = _source_control_bytes(path, entries, content_map, content_source, repo_root)
        expected = record.get("sha256")
        if path not in (PROVENANCE_LEDGER_REL, "PUBLIC_EXPORT.json") and (raw is None or error):
            findings.append(Finding("PROVENANCE_CONTENT_UNREADABLE", path, error or "missing audited bytes"))
        elif path not in (PROVENANCE_LEDGER_REL, "PUBLIC_EXPORT.json") and expected and hashlib.sha256(raw).hexdigest() != expected:
            findings.append(Finding(
                "PROVENANCE_CONTENT_MISMATCH", path,
                f"audited bytes differ from the hash in the {record_phrase}",
            ))
        pure = PurePosixPath(path)
        if raw is not None and (
            pure.suffix.lower() in SPDX_PROVENANCE_SOURCE_EXTENSIONS
            or pure.name in SPDX_PROVENANCE_SOURCE_NAMES
        ):
            findings.extend(_spdx_provenance_findings(path, record, raw))
            lineage_findings, was_contradiction = _lineage_contradiction_findings(
                path, record, raw, exceptions=active_lineage_exceptions
            )
            findings.extend(lineage_findings)
            if was_contradiction:
                matched_lineage_exceptions.add(path)

    findings.extend(_stale_lineage_exception_findings(
        active_lineage_exceptions,
        matched_lineage_exceptions,
        entry_by_path=entry_by_path,
        policy_include_paths=set(policy.include_paths),
        is_custom_exceptions=(lineage_exceptions is not None),
    ))
    return findings


def _trusted_control_match_findings(
    rel_path: str,
    trusted_path: Path,
    entries: list[GitEntry],
    content_map: dict[str, tuple[bytes | None, str | None]],
    content_source: str,
    repo_root: Path,
    code: str,
) -> list[Finding]:
    """Ensure a candidate cannot substitute a control artifact from its tree."""
    try:
        trusted = trusted_path.read_bytes()
    except OSError as error:
        return [Finding("CONTROL_TRUST_UNREADABLE", str(trusted_path), str(error))]
    actual, error = _source_control_bytes(rel_path, entries, content_map, content_source, repo_root)
    if actual is None or error:
        return [Finding("CONTROL_MISSING", rel_path, error or "control file is missing from audited source")]
    if actual != trusted:
        return [Finding(code, rel_path, "audited control bytes differ from the externally trusted release control")]
    return []


def _export_content_findings(
    policy: Policy,
    export_document: dict | None,
    entries: list[GitEntry],
    content_map: dict[str, tuple[bytes | None, str | None]],
    content_source: str,
    repo_root: Path,
) -> list[Finding]:
    """Check export claims against the exact bytes read by this audit."""
    if export_document is None:
        return [Finding("POLICY_EXPORT_UNREADABLE", "PUBLIC_EXPORT.json", "generated export is missing from the audited source")]
    findings: list[Finding] = []
    included_all = [e for e in entries if policy.resolve(e.path).disposition == INCLUDED]
    included = [e for e in included_all if e.path != "PUBLIC_EXPORT.json"]
    digest = hashlib.sha256()
    for entry in sorted(included, key=lambda item: item.path):
        raw, error = _source_control_bytes(entry.path, entries, content_map, content_source, repo_root)
        if raw is None or error:
            continue
        digest.update(entry.path.encode("utf-8") + b"\0" + hashlib.sha256(raw).hexdigest().encode("ascii") + b"\n")
    recorded = export_document.get("included_content_sha256")
    if recorded != digest.hexdigest():
        findings.append(Finding("POLICY_EXPORT_STALE", "PUBLIC_EXPORT.json", "included-content digest does not match the audited bytes"))
    if export_document.get("included_file_count", export_document.get("exported_file_count")) != len(included_all):
        findings.append(Finding("POLICY_EXPORT_STALE", "PUBLIC_EXPORT.json", "included-file count does not match the audited source"))
    if export_document.get("tracked_file_count", len(entries)) != len(entries):
        findings.append(Finding("POLICY_EXPORT_STALE", "PUBLIC_EXPORT.json", "tracked-file count does not match the audited source"))
    expected_excluded = sorted(policy.exclude_paths)
    if sorted(export_document.get("excluded_paths", [])) != expected_excluded:
        findings.append(Finding("POLICY_EXPORT_STALE", "PUBLIC_EXPORT.json", "complete exclusion disposition differs from the canonical policy"))
    # The export records the digest of the ledger blob it was generated with.
    # The ledger blob is read from the same audited source, so this catches a
    # ledger edit that was never followed by a regeneration (and vice versa),
    # independently of the included-content digest above.  A candidate that
    # edits its checked-in ledger must regenerate the export against that exact
    # blob; the recorded digest is the release process's pin of the ledger it
    # attested against.
    ledger_raw, ledger_error = _source_control_bytes(
        PROVENANCE_LEDGER_REL, entries, content_map, content_source, repo_root
    )
    recorded_ledger = export_document.get("provenance_ledger_sha256")
    if recorded_ledger and ledger_raw is not None and ledger_error is None:
        if hashlib.sha256(ledger_raw).hexdigest() != recorded_ledger:
            findings.append(Finding(
                "POLICY_EXPORT_STALE", "PUBLIC_EXPORT.json",
                "export records a provenance-ledger digest that does not match the audited ledger bytes",
            ))
    return findings


def _tree_binding_findings(
    expected_tree_sha: str | None, repo_root: Path, content_source: str, committed_tree_ref: str | None = None,
    tree_repo_root: Path | None = None,
) -> list[Finding]:
    """Bind the result to the exact Git tree whose blobs were read.

    Index mode reads the staged index, so ``git write-tree`` is authoritative.
    ``HEAD^{tree}`` is deliberately not consulted: a staged publication may
    differ from the last commit. Worktree and materialized-candidate bytes have
    no tree identity unless the release process constructs one explicitly, so
    claiming a binding for either mode is rejected.
    """
    if not expected_tree_sha:
        return []
    if content_source == CONTENT_CANDIDATE:
        return [Finding("POLICY_TREE_UNBINDABLE", "<candidate-root>",
                        "--expect-tree requires a Git tree; a materialized candidate directory has none")]
    if content_source == CONTENT_WORKTREE:
        return [Finding(
            "POLICY_TREE_UNBINDABLE", str(repo_root),
            "--expect-tree cannot bind a --worktree audit because disk bytes do not have a Git tree identity; "
            "stage the exact bytes or use --committed-tree",
        )]
    if content_source == CONTENT_COMMITTED:
        if not committed_tree_ref:
            return [Finding("POLICY_TREE_UNBINDABLE", str(repo_root), "committed-tree audit has no tree reference")]
        try:
            actual = _git_output(
                ["rev-parse", f"{committed_tree_ref}^{{tree}}"], tree_repo_root or repo_root
            ).strip()
        except RuntimeError as error:
            return [Finding("POLICY_TREE_UNBINDABLE", str(repo_root), str(error))]
        if actual != expected_tree_sha:
            return [Finding(
                "POLICY_TREE_MISMATCH", str(repo_root),
                f"audited committed tree {actual} does not match the tree proposed for publication {expected_tree_sha}",
            )]
        return []
    try:
        actual = _git_output(["write-tree"], repo_root).strip()
    except RuntimeError as error:
        return [Finding("POLICY_TREE_UNBINDABLE", str(repo_root), str(error))]
    if actual != expected_tree_sha:
        return [Finding("POLICY_TREE_MISMATCH", str(repo_root),
                        f"audited tree {actual} does not match the tree proposed for publication "
                        f"{expected_tree_sha}")]
    return []


def _untracked_tree_binding_findings(
    entries: list[GitEntry], expected_tree_sha: str | None, content_source: str
) -> list[Finding]:
    """Reject index bindings that also read bytes outside the index.

    ``git write-tree`` cannot represent an untracked file.  If the caller asks
    the default (non-``--tracked-only``) audit to bind to a tree, silently
    ignoring that file would make the attestation incomplete.
    """
    if not expected_tree_sha or content_source != CONTENT_INDEX:
        return []
    untracked = sorted(entry.path for entry in entries if not entry.sha)
    if not untracked:
        return []
    return [Finding(
        "POLICY_TREE_UNBINDABLE", untracked[0],
        f"index binding cannot include {len(untracked)} untracked path(s); stage the exact candidate first",
    )]


def _policy_manifest_findings(policy: Policy, manifest_map: dict[str, dict]) -> list[Finding]:
    """Reconcile the release manifest against the canonical policy.

    Two directions matter:

    * every explicitly excluded path must have a manifest component recording
      *why* it is excluded — this is what was missing for ``pgd.h``, ``pgf.h`` and
      the seven ``tools/pgd_*`` files on 2026-08-11; and
    * no manifest component may claim public scope for a path the policy excludes.
    """
    findings: list[Finding] = []
    for path in sorted(policy.exclude_paths):
        component = manifest_map.get(path)
        if component is None:
            findings.append(Finding(
                "POLICY_MANIFEST_MISSING", path,
                "path is excluded by the canonical policy but has no release-manifest component "
                "recording its provenance and exclusion rationale"))
            continue
        if component.get("public_scope_included", True) is not False:
            findings.append(Finding(
                "POLICY_MANIFEST_CONFLICT", path,
                "release manifest does not mark this policy-excluded path as "
                "public_scope_included=false"))
    for path, component in sorted(manifest_map.items()):
        resolution = policy.resolve(path)
        if resolution.is_excluded and component.get("public_scope_included", True) is True:
            findings.append(Finding(
                "POLICY_MANIFEST_CONFLICT", path,
                f"release manifest claims public scope for a path the policy excludes "
                f"({resolution.rule})"))
    return findings


def _resolve_content_source(content_source: str, is_candidate_root: bool) -> str:
    """Reconcile the explicit content source with the older is_candidate_root flag.

    `is_candidate_root` predates the three-way selector and existing callers still pass
    it on its own, so it keeps winning: a materialized export has no index to read.
    """
    if is_candidate_root:
        return CONTENT_CANDIDATE
    if content_source not in (CONTENT_INDEX, CONTENT_WORKTREE, CONTENT_CANDIDATE, CONTENT_COMMITTED):
        raise ValueError(f"unknown content source: {content_source!r}")
    return content_source


def audit_entries_with_semantics(
    entries: list[GitEntry],
    manifest_path: Path | None = None,
    public_scope: bool = False,
    secret_scan_report: Path | None = None,
    repo_root: Path = ROOT,
    exhaustive: bool = False,
    is_candidate_root: bool = False,
    content_source: str = CONTENT_INDEX,
    policy_path: Path | None = None,
    export_path: Path | None = None,
    expected_tree_sha: str | None = None,
    provenance_ledger_path: Path | None = None,
    trusted_manifest_path: Path | None = None,
    committed_tree_ref: str | None = None,
    tree_repo_root: Path | None = None,
    provenance_self_consistency: bool = False,
    extra_private_roots: list[str] | None = None,
) -> tuple[list[Finding], list[FileSemantics]]:

    content_source = _resolve_content_source(content_source, is_candidate_root)
    findings: list[Finding] = []
    semantics_list: list[FileSemantics] = []
    paths = [e.path for e in entries]

    all_private_root_patterns: list[re.Pattern[str]] = []
    raw_roots: list[str] = []
    raw_env = os.environ.get("PUBLISH_AUDIT_PRIVATE_ROOTS", "").strip()
    if raw_env:
        for part in re.split(r"[;\n,]+", raw_env):
            root = part.strip()
            if root:
                raw_roots.append(root)
    if extra_private_roots:
        for root in extra_private_roots:
            root = root.strip()
            if root:
                raw_roots.append(root)

    for r in raw_roots:
        for variant in {r, r.replace("\\", "/"), r.replace("/", "\\")}:
            all_private_root_patterns.append(
                re.compile(re.escape(variant) + r"(?![A-Za-z0-9_.\-])", re.IGNORECASE)
            )

    # ---- canonical publication policy -------------------------------------
    # The policy is loaded first and its failures are unconditional. An auditor
    # that cannot read or validate its own policy must never report success, and
    # policy findings are never gated behind --public-scope: an excluded path in
    # the tree is a failure in every mode.
    policy: Policy | None = None
    resolved_policy_path = policy_path or default_policy_path()
    try:
        policy = load_policy(resolved_policy_path)
    except PolicyError as error:
        findings.append(Finding("POLICY_UNREADABLE", str(resolved_policy_path), str(error)))

    # The canonical public-safe profile requires the full control set. A
    # materialized candidate using a synthetic policy is also strict when the
    # policy explicitly declares the same control paths; this keeps the
    # hermetic fail-closed tests meaningful without making selected-file helper
    # calls pretend to be release audits.
    control_paths = {"PUBLIC_EXPORT.json", "assets/public_source_profile.json", PROVENANCE_LEDGER_REL}
    controls_declared = policy is not None and control_paths.issubset(set(policy.include_paths))
    strict_controls = (
        policy is not None
        and (
            (policy.name == "public-safe-v1" and (exhaustive or bool(control_paths.intersection(paths))))
            or (is_candidate_root and controls_declared)
        )
    )
    if policy is not None:
        for message in policy.tool_compatibility_errors(TOOL_VERSION):
            findings.append(Finding("POLICY_VERSION_UNSUPPORTED", str(resolved_policy_path), message))

    # Tree identity is a binding property of the byte source, not of policy
    # parsing. Even a malformed/missing policy must not suppress a tree mismatch.
    findings.extend(_tree_binding_findings(
        expected_tree_sha, repo_root, content_source,
        committed_tree_ref=committed_tree_ref, tree_repo_root=tree_repo_root,
    ))
    findings.extend(_untracked_tree_binding_findings(entries, expected_tree_sha, content_source))

    for required in REQUIRED_PATHS:
        if not (repo_root / required).is_file():
            findings.append(Finding("REQUIRED", required, "required publication file is missing"))

    findings.extend(check_collisions(paths))

    if secret_scan_report:
        findings.extend(parse_secret_scan_report(secret_scan_report))

    content_map: dict[str, tuple[bytes | None, str | None]] = {}
    if content_source == CONTENT_INDEX:
        content_map = read_indexed_blobs_batch(entries, repo_root)
    elif content_source == CONTENT_WORKTREE:
        content_map = read_worktree_blobs(entries, repo_root)
    elif content_source in (CONTENT_CANDIDATE, CONTENT_COMMITTED):
        # Candidate and committed audits read materialized filesystem bytes in
        # the per-entry loop below. Populate the same map here so text hygiene
        # runs against every content source, not only Git-backed modes.
        for entry in entries:
            content_map[entry.path] = read_candidate_file(entry, repo_root)

    manifest_map: dict[str, dict] = {}
    if manifest_path:
        canonical_manifest = False
        try:
            canonical_manifest = manifest_path.resolve() == (repo_root / "assets" / "release_manifest.json").resolve()
        except OSError:
            canonical_manifest = manifest_path.as_posix().endswith("assets/release_manifest.json")
        if content_source == CONTENT_INDEX and canonical_manifest:
            manifest_raw, manifest_error = _source_control_bytes(
                "assets/release_manifest.json", entries, content_map, content_source, repo_root
            )
            manifest_map, manifest_findings = load_release_manifest_bytes(
                manifest_raw, "assets/release_manifest.json"
            )
            if manifest_error:
                manifest_findings.append(Finding("MANIFEST_ERROR", "assets/release_manifest.json", manifest_error))
        elif content_source in (CONTENT_WORKTREE, CONTENT_CANDIDATE, CONTENT_COMMITTED) and canonical_manifest:
            manifest_raw, manifest_error = _source_control_bytes(
                "assets/release_manifest.json", entries, content_map, content_source, repo_root
            )
            manifest_map, manifest_findings = load_release_manifest_bytes(
                manifest_raw, "assets/release_manifest.json"
            )
            if manifest_error:
                manifest_findings.append(Finding("MANIFEST_ERROR", "assets/release_manifest.json", manifest_error))
        else:
            manifest_map, manifest_findings = load_release_manifest(manifest_path)
        findings.extend(manifest_findings)
        if policy is not None:
            findings.extend(_policy_manifest_findings(policy, manifest_map))

    findings.extend(check_text_hygiene(content_map))

    lfs_patterns, lfs_attribute_findings = _read_git_lfs_attributes(
        repo_root, entries=entries, content_map=content_map, content_source=content_source,
    )
    findings.extend(lfs_attribute_findings)

    # Check for orphan manifest paths
    entry_path_set = set(paths)
    for m_path, comp in manifest_map.items():
        comp_type = comp.get("type", "")
        presence = comp.get("presence", "")
        # A path the canonical policy excludes is *expected* to be absent from any
        # tree the auditor reads -- that is the whole point of excluding it. Its
        # absence is compliance, not an orphan, and that is true in every mode.
        # (Previously this skip applied only under --public-scope, so a default
        # run reported the four excluded fonts as orphans purely for obeying the
        # policy.)
        if policy is not None and policy.resolve(m_path).is_excluded:
            continue
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

    if policy is not None and strict_controls:
        # Control artifacts are read from the same source as the audited paths.
        # The policy itself remains loaded from the externally trusted path.
        export_raw, export_error = _source_control_bytes(
            "PUBLIC_EXPORT.json", entries, content_map, content_source, repo_root
        )
        export_document, export_findings = _load_json_document(export_raw, "PUBLIC_EXPORT.json")
        findings.extend(export_findings)
        if export_error and export_raw is not None:
            findings.append(Finding("POLICY_EXPORT_UNREADABLE", "PUBLIC_EXPORT.json", export_error))
        findings.extend(_export_staleness_findings(policy, export_path, repo_root, document=export_document))
        findings.extend(_export_content_findings(
            policy, export_document, entries, content_map, content_source, repo_root
        ))
        findings.extend(_trusted_control_match_findings(
            "assets/public_source_profile.json", resolved_policy_path,
            entries, content_map, content_source, repo_root, "POLICY_SOURCE_MISMATCH"
        ))
        if trusted_manifest_path is not None:
            findings.extend(_trusted_control_match_findings(
                "assets/release_manifest.json", trusted_manifest_path,
                entries, content_map, content_source, repo_root, "MANIFEST_SOURCE_MISMATCH"
            ))
        findings.extend(_provenance_ledger_findings(
            policy, entries, content_map, content_source, repo_root, provenance_ledger_path,
            self_consistency=provenance_self_consistency,
        ))

    for entry in entries:
        rel = entry.path
        entry_findings: list[Finding] = []

        if content_source in (CONTENT_CANDIDATE, CONTENT_COMMITTED):
            raw_bytes, read_err = read_candidate_file(entry, repo_root)
        else:
            raw_bytes, read_err = content_map.get(rel, (None, "file missing or unreadable"))

        if read_err and entry.kind not in ("symlink", "gitlink"):
            f = Finding("UNREADABLE", rel, read_err)
            entry_findings.append(f)

        size = len(raw_bytes) if raw_bytes is not None else 0
        sha256 = hashlib.sha256(raw_bytes).hexdigest() if raw_bytes is not None else ""

        manifest_comp = manifest_map.get(rel)

        # Canonical policy decides publication eligibility. These two findings are
        # unconditional: an excluded or unclassified path is a failure in every
        # mode, not only under --public-scope. Gating them behind a flag is how the
        # default pre-commit run reported OK on a tree containing all fifteen
        # excluded paths.
        resolution = policy.resolve(rel) if policy is not None else None
        if resolution is not None:
            if resolution.is_excluded:
                detail = f"path is excluded by the canonical publication policy ({resolution.rule})"
                if resolution.rationale:
                    detail += f": {resolution.rationale}"
                entry_findings.append(Finding("POLICY_EXCLUDED_PRESENT", rel, detail))
            elif resolution.is_unclassified:
                entry_findings.append(Finding(
                    "POLICY_UNCLASSIFIED", rel,
                    "path has no explicit disposition in the canonical publication policy; "
                    "unknown paths are rejected. Add it to include_paths only as a reviewed "
                    "publication decision, or to exclude_paths with a rationale."))

        prov, lic, notice, gen, disp, pub_scope = _default_provenance(rel, manifest_comp, resolution)

        if public_scope and pub_scope is False:
            f = Finding("UNRESOLVED_PUBLIC", rel, "unresolved asset must be excluded from candidate public tree scope")
            entry_findings.append(f)

        reason = _forbidden_path(rel)
        if reason:
            f = Finding("PATH", rel, reason)
            entry_findings.append(f)

        path = repo_root / rel

        # An index audit now reads the indexed blob even when the worktree copy
        # has become a link, so the divergence itself is reported here rather
        # than silently changing which bytes were scanned.
        if (
            content_source == CONTENT_INDEX
            and entry.kind != "symlink"
            and entry.working_mode == "120000"
        ):
            entry_findings.append(Finding(
                "WORKTREE_LINK", rel,
                "tracked regular file is a filesystem link, or lies below one, in the "
                "working tree; the audit read the indexed blob",
            ))

        # Check symlink. The target text is only in raw_bytes when the bytes
        # being audited are a link's: an index entry whose own mode is 120000,
        # or a worktree/candidate read of a link. For an index audit of a
        # tracked regular file shadowed by a link, raw_bytes is now the staged
        # file content, which must not be reinterpreted as a target.
        link_bytes_are_target = (
            entry.kind == "symlink"
            or (content_source != CONTENT_INDEX and entry.working_mode == "120000")
        )
        if link_bytes_are_target:
            try:
                target_str = raw_bytes.decode("utf-8") if raw_bytes else ""
                target_pure = PurePosixPath(target_str)
                if target_pure.is_absolute():
                    entry_findings.append(Finding("SYMLINK_ABSOLUTE", rel, "absolute symlink target exposes local layout"))
                if ".." in target_pure.parts or any(p == ".." for p in PurePosixPath(rel).parts):
                    entry_findings.append(Finding("SYMLINK_ESCAPE", rel, "symlink target contains relative escape ('..')"))
                resolved = (path.parent / target_pure).resolve()
                if not resolved.is_relative_to(repo_root.resolve()):
                    # The resolved target is an absolute host path, and on
                    # Windows a junction into a private directory resolves to
                    # exactly the kind of path this audit exists to keep out of
                    # published output. Both non-JSON CLIs print a finding's
                    # detail verbatim into audit and CI logs, so state the
                    # containment failure without echoing where it pointed.
                    entry_findings.append(Finding("SYMLINK_ESCAPE", rel, "symlink target escapes repository root"))
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
            # Magic detection is an independent defence-in-depth layer and must
            # stay active for explicitly included files. The previous rule
            # exempted any PGF or SPIR-V blob merely for living under font/ or
            # src/ -- a location-based pass that would have waved through a font
            # binary added under a new name in an included directory.
            #
            # A path the canonical policy already excludes has produced the
            # stronger POLICY_EXCLUDED_PRESENT finding above, so it is not
            # re-reported here; everything else with binary magic must be either
            # explicitly dispositioned in the release manifest or reported.
            policy_excluded = resolution is not None and resolution.is_excluded
            if policy_excluded:
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

            if PurePosixPath(rel).suffix.lower() in {".c", ".h", ".py", ".json"}:
                for line in private_key_assignment_lines(text_str, filename=rel):
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
            elif policy and policy.private_roots and rel != "assets/public_source_profile.json":
                text_lower = text_str.lower()
                for root_str in policy.private_roots:
                    if root_str.lower() in text_lower:
                        entry_findings.append(
                            Finding("LOCAL_PATH", rel, f"contains configured private root {root_str!r}")
                        )
                        break

            if rel != "assets/public_source_profile.json" and all_private_root_patterns:
                for pat in all_private_root_patterns:
                    if pat.search(text_str):
                        entry_findings.append(
                            Finding("LOCAL_PATH", rel, "contains configured out-of-band private root")
                        )
                        break

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
    if public_scope or exhaustive:
        findings.extend(_debt_budget_findings(repo_root, paths))

    sorted_findings = sorted(findings, key=lambda item: (item.path.lower(), item.code, item.detail))
    sorted_semantics = sorted(semantics_list, key=lambda s: s.path.lower())

    return sorted_findings, sorted_semantics


def audit_entries(
    entries: list[GitEntry],
    manifest_path: Path | None = None,
    public_scope: bool = False,
    secret_scan_report: Path | None = None,
    repo_root: Path = ROOT,
    exhaustive: bool = False,
    is_candidate_root: bool = False,
    content_source: str = CONTENT_INDEX,
    policy_path: Path | None = None,
    export_path: Path | None = None,
    expected_tree_sha: str | None = None,
    provenance_ledger_path: Path | None = None,
    trusted_manifest_path: Path | None = None,
    committed_tree_ref: str | None = None,
    tree_repo_root: Path | None = None,
    extra_private_roots: list[str] | None = None,
) -> list[Finding]:
    findings, _ = audit_entries_with_semantics(
        entries=entries,
        manifest_path=manifest_path,
        public_scope=public_scope,
        secret_scan_report=secret_scan_report,
        repo_root=repo_root,
        exhaustive=exhaustive,
        is_candidate_root=is_candidate_root,
        content_source=content_source,
        policy_path=policy_path,
        export_path=export_path,
        expected_tree_sha=expected_tree_sha,
        provenance_ledger_path=provenance_ledger_path,
        trusted_manifest_path=trusted_manifest_path,
        committed_tree_ref=committed_tree_ref,
        tree_repo_root=tree_repo_root,
        extra_private_roots=extra_private_roots,
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
    secret_scan_report: Path | None = None,
    is_candidate_root: bool = False,
    content_source: str = CONTENT_INDEX,
) -> dict:

    content_source = _resolve_content_source(content_source, is_candidate_root)

    if semantics is None:
        _, semantics = audit_entries_with_semantics(
            entries,
            repo_root=repo_root,
            secret_scan_report=secret_scan_report,
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

    secret_scan_status = "ingested" if secret_scan_report else "not_run"

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
            "secret_scan_status": secret_scan_status,
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


def _materialize_committed_tree(ref: str, source_repo: Path) -> tuple[Path, str]:
    """Materialize a commit/tree into a disposable directory for exact-byte audit."""
    tree_sha = _git_output(["rev-parse", f"{ref}^{{tree}}"], source_repo).strip()
    destination = Path(tempfile.mkdtemp(prefix="nakagawa-committed-audit-"))
    archive = subprocess.run(
        ["git", "archive", "--format=tar", ref], cwd=source_repo,
        env=isolated_git_env(root=source_repo),
        capture_output=True, check=True,
    ).stdout
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as tar:
        for member in tar.getmembers():
            rel = PurePosixPath(member.name)
            if rel.is_absolute() or ".." in rel.parts:
                shutil.rmtree(destination, ignore_errors=True)
                raise RuntimeError(f"unsafe path in committed tree archive: {member.name}")
        tar.extractall(destination, filter="data")
    return destination, tree_sha


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
        "--repo-root",
        type=Path,
        default=None,
        help="Repository whose index/worktree is audited (defaults to the repository containing this tool)",
    )
    parser.add_argument(
        "--committed-tree",
        type=str,
        help="Audit an exact commit/tree ref by materializing its blobs; policy and ledger remain externally trusted",
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
        help=(
            "Report label for the policy profile. This does NOT select policy: the "
            "canonical policy is always read from --policy (see below). The label is "
            "cross-checked against the policy's own name."
        ),
    )
    parser.add_argument(
        "--policy",
        type=Path,
        default=None,
        help=(
            "Path to the canonical publication-eligibility policy "
            "(default: <audit root>/assets/public_source_profile.json). This file is "
            "authoritative for which paths may be published; unknown paths are rejected."
        ),
    )
    parser.add_argument(
        "--trusted-manifest",
        type=Path,
        default=None,
        help="Externally trusted release manifest used to prevent candidate control substitution",
    )
    parser.add_argument(
        "--private-root",
        action="append",
        default=[],
        help=(
            "Out-of-band private root path to forbid across tracked files (O-01). "
            "May also be specified via PUBLISH_AUDIT_PRIVATE_ROOTS."
        ),
    )
    parser.add_argument(
        "--provenance-ledger",
        type=Path,
        default=None,
        help=(
            "Externally trusted explicit provenance ledger. Without one, attestation claims in the "
            "candidate's own ledger cannot be verified and the audit reports PROVENANCE_UNVERIFIED "
            "(blocked). The release flow must supply this; the developer tripwire uses "
            "--provenance-self-consistency instead."
        ),
    )
    parser.add_argument(
        "--provenance-self-consistency",
        action="store_true",
        help=(
            "Provenance scope for developer tripwires: check the audited ledger for candidate-internal "
            "consistency (coverage, resolution, content hashes) without asserting attestation authenticity; "
            "attestation requires --provenance-ledger in the release flow."
        ),
    )
    parser.add_argument(
        "--export",
        type=Path,
        default=None,
        help="Path to the generated publication export whose policy digest must be current "
             "(default: <audit root>/PUBLIC_EXPORT.json)",
    )
    parser.add_argument(
        "--expect-tree",
        type=str,
        default=None,
        help=(
            "Git tree SHA proposed for publication. The audit fails unless the tree it "
            "actually read is this exact tree, which binds the audit result to the bytes "
            "being pushed."
        ),
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
        "--secret-scan-report",
        type=Path,
        help="Path to external Betterleaks/Gitleaks-compatible JSON findings report to ingest and check",
    )
    parser.add_argument(
        "--gitleaks-report",
        dest="secret_scan_report",
        type=Path,
        help=argparse.SUPPRESS,
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

    if args.committed_tree and (args.candidate_root or args.worktree):
        print(
            "publication audit failed: --committed-tree cannot be combined with --candidate-root or --worktree",
            file=sys.stderr,
        )
        return 2

    audit_repo_root = args.repo_root.absolute() if args.repo_root else ROOT
    committed_audit_root: Path | None = None
    committed_tree_sha: str | None = None
    if args.committed_tree:
        try:
            committed_audit_root, committed_tree_sha = _materialize_committed_tree(args.committed_tree, audit_repo_root)
        except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
            print(f"publication audit failed: cannot materialize committed tree: {error}", file=sys.stderr)
            return 2
        audit_root = committed_audit_root
    else:
        audit_root = args.candidate_root.absolute() if args.candidate_root else audit_repo_root
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

    content_source = (
        CONTENT_COMMITTED if args.committed_tree else
        (CONTENT_CANDIDATE if is_cand_root else (CONTENT_WORKTREE if args.worktree else CONTENT_INDEX))
    )

    try:
        entries = (
            _get_filesystem_entries(audit_root)
            if args.candidate_root or args.committed_tree
            else _get_git_entries(args.tracked_only, repo_root=audit_root, content_source=content_source)
        )
    except (RuntimeError, UnicodeDecodeError) as error:
        print(f"publication audit failed: {error}", file=sys.stderr)
        return 2

    findings, semantics = audit_entries_with_semantics(
        entries,
        manifest_path=manifest_path,
        public_scope=args.public_scope,
        secret_scan_report=args.secret_scan_report,
        repo_root=audit_root,
        exhaustive=is_exhaustive,
        is_candidate_root=is_cand_root,
        content_source=content_source,
        # The policy comes from the *auditing* repository, never from the tree under
        # audit. A materialized candidate carries its own copy of
        # assets/public_source_profile.json; honouring that copy would let the
        # artifact being gated supply the rules it is gated by -- an old or edited
        # profile inside the candidate would silently weaken or disable the gate.
        # --policy remains available to point at a specific trusted policy on purpose.
        policy_path=args.policy or (audit_repo_root / "assets" / "public_source_profile.json"),
        export_path=args.export or (audit_root / "PUBLIC_EXPORT.json"),
        expected_tree_sha=args.expect_tree or committed_tree_sha,
        provenance_ledger_path=args.provenance_ledger,
        provenance_self_consistency=args.provenance_self_consistency,
        trusted_manifest_path=args.trusted_manifest,
        committed_tree_ref=args.committed_tree,
        tree_repo_root=audit_repo_root,
        extra_private_roots=args.private_root,
    )
    report = generate_manifest_report(
        entries,
        findings,
        semantics,
        repo_root=audit_root,
        profile=args.profile,
        secret_scan_report=args.secret_scan_report,
        is_candidate_root=is_cand_root,
        content_source=content_source,
    )

    if args.manifest_out:
        args.manifest_out.parent.mkdir(parents=True, exist_ok=True)
        args.manifest_out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
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
        # Secret-scan findings are redacted at ingestion, but keep the machine
        # readable stdout contract conservative too: manifest files retain the
        # detailed audit record, while CLI output never logs finding details.
        safe_report = {
            "status": "FAIL" if findings else "OK",
            "total_files": len(semantics),
            "total_findings": len(findings),
            "meta": {
                "tool_version": TOOL_VERSION,
                "schema_version": SCHEMA_VERSION,
                "git_commit": report["meta"]["git_commit"],
                "content_source": content_source,
                "profile": args.profile,
                "secret_scan_status": "ingested" if args.secret_scan_report else "not_run",
                "aggregate_manifest_sha256": report["meta"]["aggregate_manifest_sha256"],
            },
            "summary": report["summary"],
            "findings": [{"code": finding.code, "path": finding.path} for finding in findings],
        }
        print(json.dumps(safe_report, indent=2))
        return 1 if findings else 0

    scope = (
        "committed" if args.committed_tree else
        ("candidate" if args.candidate_root else ("tracked" if args.tracked_only else "prospective"))
    )

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
