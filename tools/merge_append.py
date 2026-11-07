#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""A Git merge driver that keeps both sides when each only added lines at one spot.

Some hand-maintained files get a new item from almost every pull request: a self-test block in
``mk/selftests.mk``, a target in ``mk/public_targets.mk``, a source in ``CMakeLists.txt``. Two pull
requests that add at the same place conflict although neither touches the other's lines.

Git's own ``merge=union`` fixes that, but it also "resolves" a conflict where both sides EDITED the
same line by keeping both versions, and the file then holds two contradictory lines with no marker.
This driver is the guarded form. It runs the ordinary three-way merge and looks at each conflict:

* if the common ancestor has nothing there (both sides only inserted), it keeps ours, then theirs;
* if the ancestor has lines there (someone changed or removed them), it leaves the ordinary conflict
  markers for a person, exactly as ``git merge`` does without a driver;
* it also leaves them when the two insertions share a non-blank line, and when a tab-indented line of an
  insertion would belong to a rule header outside its own insertion (comments and blank lines between recipe
  lines do not end a recipe), where the order of lines matters. Inserts between targets and in lists merge.

Only the paths in ``ALLOWED_PATHS`` are ever unioned, so editing ``.gitattributes`` cannot attach the
driver to code. Setup, once per clone (worktrees share it), is in ``docs/CI.md``; the landing desk runs a
reviewed copy outside every worktree, never the file in the tree being merged::

    git config merge.nk-append.name "append-only list merge"
    git config merge.nk-append.driver "python tools/merge_append.py %O %A %B %L %P"

Without that setting Git ignores the ``merge=nk-append`` attribute and merges as usual, so a
contributor who never sets it loses nothing. GitHub's own merge does not run drivers either.

Arguments are Git's: ``%O`` ancestor, ``%A`` ours (the result is written here), ``%B`` theirs,
``%L`` conflict marker size, ``%P`` path. Exit status 0 means merged, 1 means conflicts remain.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys

# The driver acts only on these files, whatever .gitattributes of the tree being merged says: a pull request
# that edits .gitattributes cannot attach it to code. tools/test_merge_append.py keeps this equal to the
# attribute list.
ALLOWED_PATHS = frozenset({
    "mk/selftests.mk",
    "mk/public_targets.mk",
    "mk/runtime_sources.mk",
    "CMakeLists.txt",
    "docs/DEBUGGING.md",
})
_ASSIGNMENT = re.compile(rb"^[A-Za-z_][A-Za-z0-9_.\-]*\s*(?:[:+?]|::|:::)?=")

OURS = b"ours"
BASE = b"base"
THEIRS = b"theirs"


def _git_executable(path_var: str | None = None, cwd: str | os.PathLike[str] | None = None) -> str | None:
    """Return the absolute path of git found on PATH, or None.

    Only absolute PATH entries count. An empty or ``.`` entry, a relative one, and any entry equal to or
    inside the working directory are skipped, because on Windows a ``git.exe`` in the working directory (the
    root of the tree being merged) would otherwise run before the real one.
    """
    if path_var is None:
        path_var = os.environ.get("PATH", "")
    here = Path(cwd if cwd is not None else os.getcwd()).resolve()
    if os.name == "nt":
        extensions = [e.lower() for e in os.environ.get("PATHEXT", ".COM;.EXE;.BAT;.CMD").split(";") if e]
        if ".exe" not in extensions:
            extensions.insert(0, ".exe")
        names = ["git" + e for e in extensions]
    else:
        names = ["git"]
    for entry in path_var.split(os.pathsep):
        entry = entry.strip().strip('"')
        if entry in ("", "."):
            continue
        folder = Path(entry)
        if not folder.is_absolute():
            continue
        try:
            folder = folder.resolve()
        except OSError:
            continue
        if folder == here or here in folder.parents:
            continue
        for name in names:
            candidate = folder / name
            if candidate.is_file() and (os.name == "nt" or os.access(candidate, os.X_OK)):
                return str(candidate)
    return None


def _merge_file(current: Path, base: Path, other: Path, marker: int, *flags: str) -> tuple[int, bytes]:
    """Run ``git merge-file -p`` and return its status and the merged text, without touching ``current``.

    Status -1 means git could not be found safely; callers treat it like any other merge-file failure.
    """
    git = _git_executable()
    if git is None:
        return -1, b""
    proc = subprocess.run(
        [git, "merge-file", "-p", f"--marker-size={marker}", *flags,
         "-L", OURS.decode(), "-L", BASE.decode(), "-L", THEIRS.decode(),
         str(current), str(base), str(other)],
        capture_output=True, check=False,
    )
    return proc.returncode, proc.stdout


def _blank(line: bytes) -> bool:
    return not line.strip()


def _owner(layout: list[bytes], index: int) -> int | None:
    """Index of the line that owns the tab-indented line at ``index``: the nearest line above it that is not
    blank, not a ``#`` comment (make ignores both inside a recipe) and not itself tab-indented."""
    for j in range(index - 1, -1, -1):
        line = layout[j]
        if _blank(line) or line.startswith((b"#", b"\t")):
            continue
        return j
    return None


def _recipe_order_at_risk(out: list[bytes], ours: list[bytes], theirs: list[bytes]) -> bool:
    """True when a tab-indented line of either insert is owned by a line outside its own insert.

    Such a line is part of a recipe that continues from above, so the union would put it under another
    rule or detach it from its own. An owner in ``out`` that is a variable assignment is a list item
    and is fine. A line of ``theirs`` owned by a line of ``ours`` is always outside its side.
    """
    layout = out + ours + theirs
    start_ours = len(out)
    start_theirs = len(out) + len(ours)
    for first, side in ((start_ours, ours), (start_theirs, theirs)):
        for offset, line in enumerate(side):
            if not line.startswith(b"\t"):
                continue
            owner = _owner(layout, first + offset)
            if owner is None:
                return True
            if owner >= first:
                continue  # its own insert brings the header (or an earlier line of that insert owns it)
            if owner < start_ours and _ASSIGNMENT.match(layout[owner]):
                continue
            return True
    return False


def _diff3_marker_size(marker: int, *texts: bytes) -> int:
    """A marker size longer than any run of ``<``, ``|``, ``=`` or ``>`` that starts a line of the inputs, so
    a content line can never be read as a marker or as the separator."""
    longest = 0
    for text in texts:
        for line in text.splitlines():
            if line[:1] and line[:1] in b"<|=>":
                longest = max(longest, len(line) - len(line.lstrip(line[:1])))
    return max(marker + 1, longest + 1)


def union_pure_inserts(diff3_text: bytes, marker: int) -> bytes | None:
    """Return the merge with every conflict resolved ours-then-theirs, or None if any conflict edits ancestor lines.

    ``diff3_text`` is ``git merge-file --diff3`` output. A conflict whose ancestor section is empty is an
    insertion by both sides. Anything unexpected in the markers returns None, which keeps the conflict.
    """
    open_m = b"<" * marker + b" "
    base_m = b"|" * marker + b" "
    mid_m = b"=" * marker
    close_m = b">" * marker + b" "
    out: list[bytes] = []
    state = "text"
    ours: list[bytes] = []
    base: list[bytes] = []
    theirs: list[bytes] = []
    for line in diff3_text.splitlines(keepends=True):
        bare = line.rstrip(b"\r\n")
        if state == "text":
            if bare.startswith(open_m) and bare.endswith(OURS):
                state, ours, base, theirs = "ours", [], [], []
            else:
                out.append(line)
        elif state == "ours":
            if bare.startswith(base_m) and bare.endswith(BASE):
                state = "base"
            else:
                ours.append(line)
        elif state == "base":
            if bare == mid_m:
                state = "theirs"
            else:
                base.append(line)
        else:
            if bare.startswith(close_m) and bare.endswith(THEIRS):
                if base:
                    return None
                if ours and not ours[-1].endswith(b"\n"):
                    return None
                # Both sides repeating a line means the inserts overlap. diff3 does not factor that out, so a
                # union would duplicate it. Blank lines may repeat.
                shared = {l.rstrip(b"\r\n") for l in ours if not _blank(l)}
                if shared & {l.rstrip(b"\r\n") for l in theirs if not _blank(l)}:
                    return None
                # Recipe line order matters: a tab-indented line owned from outside its insert is a person's call.
                if _recipe_order_at_risk(out, ours, theirs):
                    return None
                out.extend(ours)
                out.extend(theirs)
                state = "text"
            else:
                theirs.append(line)
    if state != "text":
        return None
    return b"".join(out)


def merge(base: Path, current: Path, other: Path, marker: int, path: str = "") -> int:
    """Merge ``other`` into ``current``; ``path`` must be in ALLOWED_PATHS for inserts to be unioned."""
    allowed = path.replace("\\", "/") in ALLOWED_PATHS
    status, merged = _merge_file(current, base, other, marker)
    if status == 0:
        current.write_bytes(merged)
        return 0
    if status < 0 or status > 127:
        return 1  # git merge-file failed outright (for example a binary file): leave ours as it is
    unioned = None
    if allowed:
        # A larger marker for this analysis run only, so no content line can pass for the separator.
        size = _diff3_marker_size(marker, current.read_bytes(), base.read_bytes(), other.read_bytes())
        diff3_status, diff3 = _merge_file(current, base, other, size, "--diff3")
        if 0 <= diff3_status <= 127:
            unioned = union_pure_inserts(diff3, size)
    if unioned is None:
        current.write_bytes(merged)  # the ordinary conflict markers
        return 1
    current.write_bytes(unioned)
    return 0


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        print("usage: merge_append.py BASE CURRENT OTHER [MARKER_SIZE [PATH]]", file=sys.stderr)
        return 2
    marker = int(argv[4]) if len(argv) > 4 and argv[4].isdigit() else 7
    return merge(Path(argv[1]), Path(argv[2]), Path(argv[3]), marker, argv[5] if len(argv) > 5 else "")


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
