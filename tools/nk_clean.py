# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Preview-first workspace cleaner for allowlisted output roots (issue #368).

Deletion is opt-in: the default invocation plans and prints the exact file set
and exits without touching anything. Selection is restricted to an explicit
allowlist of repository-owned output roots. Git-tracked files, reparse points
(symlinks and Windows junctions), and every unlisted path are never selected.
An optional age bound keeps the preview contract but restricts it to files
older than N days. Scan or containment uncertainty fails closed with a
nonzero exit and no deletion.
"""

from __future__ import annotations

import argparse
import os
import stat
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

#: Files that must exist at the workspace root before any clean is planned.
WORKSPACE_ANCHORS: tuple[str, ...] = (
    "Makefile",
    "AGENTS.md",
    "src/rt/recomp.c",
    "tools/codegen.py",
)

#: Top-level components that are never scanned, selected, or pruned.
PROTECTED_COMPONENTS: frozenset[str] = frozenset(
    {
        ".git",
        "assets",
        "docs",
        "font",
        "keys",
        "memstick",
        "fs",
        "oracle",
        "original_game",
        "place_game_here",
        "src",
        "tools",
        "interface",
        "third_party",
        "node_modules",
    }
)

CATEGORY_ORDER: tuple[str, ...] = ("build outputs", "logs", "tool scratch")

#: build/ subdirectories that hold public, regenerable outputs. Any OTHER build/<name> is
#: treated as a title output (for example a private title's generated code and executable,
#: expensive to rebuild) and is skipped unless --include-title-outputs is given.
PUBLIC_BUILD_DIRS: frozenset[str] = frozenset(
    {"mygame", "fixtures", "cosim", "codegen", "packaging", "portable-core", "snapshots",
     "verify", "vfpu_oracle", "runtime"}
)
PUBLIC_BUILD_PREFIXES: tuple[str, ...] = (
    "synthetic", "test", "platform-ladder", "production", "pspdev", "nakagawa_player", "psmf",
)
#: Logs written by running a (possibly private) title; kept unless --include-title-outputs.
TITLE_RUN_LOGS: frozenset[str] = frozenset({"logs/stdout_run.log", "logs/stderr_run.log"})


def _is_public_build_dir(name: str) -> bool:
    return name in PUBLIC_BUILD_DIRS or name.startswith(PUBLIC_BUILD_PREFIXES)


class CleanRefusedError(Exception):
    """A fail-closed refusal to plan or delete a path."""


@dataclass(frozen=True)
class AllowRule:
    """One allowlisted output root: a whole tree or a single exact file."""

    rel: str
    category: str
    kind: str


@dataclass(frozen=True)
class SelectedFile:
    rel: str
    category: str
    size: int


@dataclass(frozen=True)
class SkippedEntry:
    rel: str
    reason: str


@dataclass
class Preview:
    root: Path
    root_real: str
    tracked: frozenset[str]
    older_than_days: int | None
    files: list[SelectedFile] = field(default_factory=list)
    skipped: list[SkippedEntry] = field(default_factory=list)
    errors: list[tuple[str, str]] = field(default_factory=list)
    age_excluded: int = 0
    age_excluded_bytes: int = 0

    @property
    def ok(self) -> bool:
        return not self.errors


TREE_RULES: tuple[AllowRule, ...] = (
    AllowRule("build", "build outputs", "tree"),
    AllowRule("fixtures/psp_oracle/build", "build outputs", "tree"),
    AllowRule("tmp", "tool scratch", "tree"),
    AllowRule(".tmp", "tool scratch", "tree"),
    AllowRule("scratch", "tool scratch", "tree"),
    AllowRule(".pytest_cache", "tool scratch", "tree"),
    AllowRule(".ruff_cache", "tool scratch", "tree"),
    AllowRule(".mypy_cache", "tool scratch", "tree"),
    AllowRule(".cache", "tool scratch", "tree"),
    AllowRule("htmlcov", "tool scratch", "tree"),
)

FILE_RULES: tuple[AllowRule, ...] = (
    AllowRule("logs/build_out_recomp.log", "logs", "file"),
    AllowRule("logs/build_err_recomp.log", "logs", "file"),
    AllowRule("logs/recomp_err.log", "logs", "file"),
    AllowRule("logs/obj_err.log", "logs", "file"),
    AllowRule("logs/link_err.log", "logs", "file"),
    AllowRule("logs/stdout_run.log", "logs", "file"),
    AllowRule("logs/stderr_run.log", "logs", "file"),
    AllowRule("link_err.log", "logs", "file"),
    AllowRule("compile_commands.json", "tool scratch", "file"),
    AllowRule(".coverage", "tool scratch", "file"),
    AllowRule(".eslintcache", "tool scratch", "file"),
)

#: Directories that empty-dir pruning must never remove: protected roots,
#: allowlisted rule roots, and the parents of the exact-file log rules.
NEVER_PRUNE: frozenset[str] = (
    frozenset(PROTECTED_COMPONENTS)
    | frozenset(rule.rel for rule in TREE_RULES)
    | frozenset(
        {
            "logs",
            "fixtures",
        }
    )
)


def format_size(size: int) -> str:
    """Render a byte count for the grouped preview output."""
    if size < 1024:
        return f"{size} B"
    value = float(size)
    for unit in ("KiB", "MiB", "GiB", "TiB"):
        value /= 1024.0
        if value < 1024.0 or unit == "TiB":
            return f"{value:.1f} {unit}"
    return f"{value:.1f} TiB"


def _confirm_requested(yes: bool) -> bool:
    if yes:
        return True
    return os.environ.get("CONFIRM") == "1"


def _is_reparse_stat(st: os.stat_result) -> bool:
    if stat.S_ISLNK(st.st_mode):
        return True
    return bool(getattr(st, "st_file_attributes", 0) & 0x400)


def _is_reparse(path: Path) -> bool:
    try:
        st = os.lstat(path)
    except OSError:
        return False
    return _is_reparse_stat(st)


def _is_within(root_real: str, candidate_real: str) -> bool:
    root_key = os.path.normcase(root_real)
    candidate_key = os.path.normcase(candidate_real)
    if candidate_key == root_key:
        return True
    prefix = root_key if root_key.endswith(os.sep) else root_key + os.sep
    return candidate_key.startswith(prefix)


def _require_within(root_real: str, candidate: str | Path) -> None:
    """Prove a candidate path resolves inside the workspace root, or refuse."""
    candidate_text = os.fspath(candidate)
    try:
        candidate_real = os.path.realpath(os.path.abspath(candidate_text))
    except OSError as exc:
        raise CleanRefusedError(f"cannot resolve path {candidate_text!r}: {exc}") from exc
    if not _is_within(root_real, candidate_real):
        raise CleanRefusedError(f"path escapes the workspace root: {candidate_text}")


def _require_workspace(root: Path) -> None:
    missing = [marker for marker in WORKSPACE_ANCHORS if not (root / marker).is_file()]
    if missing:
        raise CleanRefusedError(f"{root} is not a workspace root (missing {', '.join(missing)})")


def git_tracked(root: Path) -> frozenset[str]:
    """Return the repository's tracked file list as POSIX-relative paths."""
    proc = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0:
        detail = proc.stderr.decode("utf-8", "replace").strip()
        raise OSError(detail or f"git ls-files exited {proc.returncode}")
    listing = proc.stdout.decode("utf-8", "surrogateescape")
    return frozenset(entry for entry in listing.split("\0") if entry)


def plan_clean(
    root: Path | str,
    *,
    tracked: frozenset[str],
    older_than_days: int | None = None,
    now: float | None = None,
    include_title_outputs: bool = False,
) -> Preview:
    """Plan deletions against the allowlist without touching any file."""
    root_path = Path(root)
    _require_workspace(root_path)
    if older_than_days is not None and older_than_days < 0:
        raise CleanRefusedError("--older-than must be >= 0 days")
    root_real = os.path.realpath(os.path.abspath(str(root_path)))
    now_ts = time.time() if now is None else now
    cutoff = None if older_than_days is None else now_ts - older_than_days * 86400.0
    preview = Preview(
        root=root_path,
        root_real=root_real,
        tracked=tracked,
        older_than_days=older_than_days,
    )

    def consider_file(path: Path, rel: str, category: str) -> None:
        try:
            st = os.lstat(path)
        except OSError as exc:
            preview.errors.append((rel, f"stat failed: {exc}"))
            return
        if _is_reparse_stat(st):
            preview.skipped.append(SkippedEntry(rel, "reparse point; never followed"))
            return
        if not stat.S_ISREG(st.st_mode):
            preview.skipped.append(SkippedEntry(rel, "not a regular file"))
            return
        if rel in preview.tracked:
            preview.skipped.append(SkippedEntry(rel, "git-tracked; protected"))
            return
        if cutoff is not None and st.st_mtime >= cutoff:
            preview.age_excluded += 1
            preview.age_excluded_bytes += st.st_size
            return
        preview.files.append(SelectedFile(rel=rel, category=category, size=st.st_size))

    def scan_dir(dir_path: Path, dir_rel: str, category: str) -> None:
        try:
            with os.scandir(dir_path) as iterator:
                entries = sorted(iterator, key=lambda item: item.name)
        except OSError as exc:
            preview.errors.append((dir_rel, f"scan failed: {exc}"))
            return
        for entry in entries:
            rel = f"{dir_rel}/{entry.name}"
            try:
                entry_stat = entry.stat(follow_symlinks=False)
            except OSError as exc:
                preview.errors.append((rel, f"stat failed: {exc}"))
                continue
            if _is_reparse_stat(entry_stat):
                preview.skipped.append(SkippedEntry(rel, "reparse point; never followed"))
                continue
            if entry.is_dir(follow_symlinks=False):
                if (dir_rel == "build" and not include_title_outputs
                        and not _is_public_build_dir(entry.name)):
                    preview.skipped.append(SkippedEntry(
                        rel, "title output; pass --include-title-outputs to clean it"))
                    continue
                scan_dir(Path(entry.path), rel, category)
            elif entry.is_file(follow_symlinks=False):
                consider_file(Path(entry.path), rel, category)
            else:
                preview.skipped.append(SkippedEntry(rel, "not a regular file"))

    for rule in TREE_RULES:
        rule_path = root_path / rule.rel
        if not os.path.lexists(rule_path):
            continue
        try:
            rule_stat = os.lstat(rule_path)
        except OSError as exc:
            preview.errors.append((rule.rel, f"stat failed: {exc}"))
            continue
        if _is_reparse_stat(rule_stat):
            preview.skipped.append(SkippedEntry(rule.rel, "reparse point; never followed"))
            continue
        if not stat.S_ISDIR(rule_stat.st_mode):
            preview.errors.append((rule.rel, "allowlisted tree root is not a directory"))
            continue
        scan_dir(rule_path, rule.rel, rule.category)

    for rule in FILE_RULES:
        rule_path = root_path / rule.rel
        if not os.path.lexists(rule_path):
            continue
        if rule.rel in TITLE_RUN_LOGS and not include_title_outputs:
            preview.skipped.append(SkippedEntry(
                rule.rel, "title run log; pass --include-title-outputs to clean it"))
            continue
        consider_file(rule_path, rule.rel, rule.category)

    return preview


def _prune_empty_dirs(preview: Preview, deleted: list[Path]) -> None:
    """Remove directories emptied by this run, never rule roots or protected paths."""
    candidates: list[Path] = []
    for path in deleted:
        current = path.parent
        seen: set[Path] = set()
        while current != preview.root and current not in seen:
            seen.add(current)
            try:
                rel = current.relative_to(preview.root).as_posix()
            except ValueError:
                break
            if rel in NEVER_PRUNE:
                break
            candidates.append(current)
            current = current.parent
    unique: list[Path] = []
    for candidate in candidates:
        if candidate not in unique:
            unique.append(candidate)
    for directory in sorted(unique, key=lambda item: len(item.parts), reverse=True):
        if not directory.is_dir() or _is_reparse(directory):
            continue
        try:
            next(directory.iterdir())
        except StopIteration:
            try:
                os.rmdir(directory)
            except OSError:
                pass
        except OSError:
            continue


def execute(preview: Preview) -> list[str]:
    """Delete exactly the previewed set after revalidating every path."""
    failures: list[str] = []
    deleted: list[Path] = []
    for selected in preview.files:
        path = preview.root / selected.rel
        try:
            _require_within(preview.root_real, path)
        except CleanRefusedError as exc:
            failures.append(f"{selected.rel}: {exc}")
            continue
        try:
            st = os.lstat(path)
        except OSError as exc:
            failures.append(f"{selected.rel}: stat failed: {exc}")
            continue
        if _is_reparse_stat(st):
            failures.append(f"{selected.rel}: reparse point appeared; refusing")
            continue
        if not stat.S_ISREG(st.st_mode):
            failures.append(f"{selected.rel}: no longer a regular file")
            continue
        if selected.rel in preview.tracked:
            failures.append(f"{selected.rel}: became git-tracked; refusing")
            continue
        try:
            os.unlink(path)
        except OSError as exc:
            failures.append(f"{selected.rel}: unlink failed: {exc}")
            continue
        deleted.append(path)
    _prune_empty_dirs(preview, deleted)
    return failures


def render(preview: Preview, *, confirm: bool) -> str:
    """Render the grouped, size-annotated plan for stdout."""
    total = sum(selected.size for selected in preview.files)
    count = len(preview.files)
    lines: list[str] = []
    if confirm:
        lines.append(f"CLEAN (confirmed): {count} files, {format_size(total)} will be deleted")
    else:
        lines.append(f"DRY-RUN: {count} files, {format_size(total)} would be deleted")
    if preview.older_than_days is not None:
        lines.append(
            f"Age bound: {preview.older_than_days} day(s); "
            f"{preview.age_excluded} newer file(s) excluded"
        )
    order = {category: index for index, category in enumerate(CATEGORY_ORDER)}
    ordered = sorted(
        preview.files,
        key=lambda selected: (order.get(selected.category, len(CATEGORY_ORDER)), selected.rel),
    )
    for category in CATEGORY_ORDER:
        lines.append(f"[{category}]")
        members = [selected for selected in ordered if selected.category == category]
        if not members:
            lines.append("  (none)")
        for selected in members:
            lines.append(f"  {selected.rel}  {format_size(selected.size)}")
    if preview.skipped:
        lines.append(
            f"Skipped {len(preview.skipped)} entries "
            "(tracked, protected, reparse points, or non-regular files)"
        )
    for rel, message in preview.errors:
        lines.append(f"ERROR {rel}: {message}")
    if confirm:
        lines.append(f"{count} files selected for deletion.")
    else:
        lines.append("No files were deleted. Re-run with --yes or CONFIRM=1 to apply.")
    return "\n".join(lines) + "\n"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="nk_clean.py",
        description=(
            "Preview-first workspace clean for allowlisted output roots (issue #368). "
            "Runs as a dry-run unless --yes or CONFIRM=1 is given."
        ),
    )
    parser.add_argument(
        "--yes",
        action="store_true",
        help="apply the previewed plan (the default is a dry-run)",
    )
    parser.add_argument(
        "--older-than",
        type=int,
        default=None,
        metavar="DAYS",
        help="only select files whose mtime is older than DAYS",
    )
    parser.add_argument(
        "--include-title-outputs",
        action="store_true",
        help="also clean title build directories (build/<title>) and title run logs",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    root = REPO_ROOT
    try:
        tracked = git_tracked(root)
        preview = plan_clean(
            root, tracked=tracked, older_than_days=args.older_than,
            include_title_outputs=args.include_title_outputs,
        )
    except CleanRefusedError as exc:
        print(f"nk_clean: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"nk_clean: git ls-files failed: {exc}", file=sys.stderr)
        return 1
    confirm = _confirm_requested(args.yes)
    sys.stdout.write(render(preview, confirm=confirm))
    if preview.errors:
        return 1
    if not confirm:
        return 0
    failures = execute(preview)
    for failure in failures:
        print(f"nk_clean: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
