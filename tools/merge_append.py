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
  markers for a person, exactly as ``git merge`` does without a driver.

Setup, once per clone (worktrees share it), is in ``docs/CI.md``::

    git config merge.nk-append.name "append-only list merge"
    git config merge.nk-append.driver "python tools/merge_append.py %O %A %B %L %P"

Without that setting Git ignores the ``merge=nk-append`` attribute and merges as usual, so a
contributor who never sets it loses nothing. GitHub's own merge does not run drivers either.

Arguments are Git's: ``%O`` ancestor, ``%A`` ours (the result is written here), ``%B`` theirs,
``%L`` conflict marker size, ``%P`` path. Exit status 0 means merged, 1 means conflicts remain.
"""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys

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
                out.extend(ours)
                out.extend(theirs)
                state = "text"
            else:
                theirs.append(line)
    if state != "text":
        return None
    return b"".join(out)


def merge(base: Path, current: Path, other: Path, marker: int) -> int:
    status, merged = _merge_file(current, base, other, marker)
    if status == 0:
        current.write_bytes(merged)
        return 0
    if status < 0 or status > 127:
        return 1  # git merge-file failed outright (for example a binary file): leave ours as it is
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
    return merge(Path(argv[1]), Path(argv[2]), Path(argv[3]), marker)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
