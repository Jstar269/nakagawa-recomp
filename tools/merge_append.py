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
* it also leaves them when the two insertions share a non-blank line, and when an insertion starts with a
  tab inside a make recipe, where the order of lines matters. Inserts between targets and in lists merge.

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


def _merge_file(current: Path, base: Path, other: Path, marker: int, *flags: str) -> tuple[int, bytes]:
    """Run ``git merge-file -p`` and return its status and the merged text, without touching ``current``."""
    proc = subprocess.run(
        ["git", "merge-file", "-p", f"--marker-size={marker}", *flags,
         "-L", OURS.decode(), "-L", BASE.decode(), "-L", THEIRS.decode(),
         str(current), str(base), str(other)],
        capture_output=True, check=False,
    )
    return proc.returncode, proc.stdout


def _blank(line: bytes) -> bool:
    return not line.strip()


def _in_recipe(out: list[bytes]) -> bool:
    """True when the next line would continue a make recipe: the last line above that is neither tab-indented
    nor blank is a rule, not a variable assignment (a tab-indented list item continues an assignment)."""
    for line in reversed(out):
        if line.startswith(b"\t") or _blank(line):
            continue
        return _ASSIGNMENT.match(line) is None
    return True


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
                # Recipe line order matters: a tab-indented insert inside a recipe is a person's call.
                if any(side and side[0].startswith(b"\t") for side in (ours, theirs)) and _in_recipe(out):
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
        _, diff3 = _merge_file(current, base, other, marker, "--diff3")
        unioned = union_pure_inserts(diff3, marker)
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
