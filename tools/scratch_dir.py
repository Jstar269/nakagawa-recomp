# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Temporary directories for tests that run a program from inside the directory.

On Windows a program that has just exited can stay locked for a moment after its process
is reaped, for instance while an on-access scanner still holds the image open. The
standard ``tempfile.TemporaryDirectory`` removes its tree once, so its cleanup fails with
``PermissionError: [WinError 32]`` and the test errors after every assertion has passed.
The hosted Windows runtime job hit this in tools/test_flash0_font.py.

:class:`TemporaryDirectory` here is the standard class with one change to ``cleanup``:
when removal fails with one of the Windows errors a short-lived lock produces, it tries
again after a growing pause, for at most :attr:`TemporaryDirectory.retry_seconds`, then
raises the last real error with a note naming the wait. Any other error, and any error off
Windows, is raised at once, as the standard class raises it. A lock that outlasts the
bound, such as a program still running, therefore still fails the test.

Use it, in place of ``tempfile.TemporaryDirectory``, wherever a test runs a program that
lives in its scratch directory, whether the test, Make or a launcher starts it.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import tempfile
import time

#: Windows errors removal reports while a short-lived lock holds a file:
#: ERROR_ACCESS_DENIED (5) while an exited program's image is still mapped,
#: ERROR_SHARING_VIOLATION (32) while another process holds a file open without delete
#: sharing, and ERROR_DIR_NOT_EMPTY (145) while a deleted file waits for that handle to
#: close.
TRANSIENT_WINERRORS = frozenset({5, 32, 145})


def is_transient_lock(error: OSError) -> bool:
    """True when ``error`` is a Windows error a short-lived file lock produces."""
    return getattr(error, "winerror", None) in TRANSIENT_WINERRORS


class TemporaryDirectory(tempfile.TemporaryDirectory):
    """``tempfile.TemporaryDirectory`` whose cleanup waits out a short-lived Windows lock."""

    #: Longest total wait, in seconds, before the last removal error is raised.
    retry_seconds = 10.0
    #: First pause between attempts; each later pause doubles, up to ``max_pause``.
    first_pause = 0.05
    max_pause = 1.0

    def cleanup(self) -> None:
        # The standard cleanup is safe to repeat: a later call removes what an earlier,
        # failed one left, and once the tree is gone it does nothing.
        deadline = time.monotonic() + self.retry_seconds
        pause = self.first_pause
        while True:
            try:
                super().cleanup()
                return
            except OSError as error:
                if not is_transient_lock(error):
                    raise
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    error.add_note(
                        f"{self.name} was still locked after {self.retry_seconds:g} s of "
                        "removal retries (tools/scratch_dir.py)"
                    )
                    raise
            time.sleep(min(pause, remaining))
            pause = min(pause * 2, self.max_pause)

    def resolve_path(self, *path_parts: str | os.PathLike) -> Path:
        """Safely resolve path components contained strictly within this scratch directory root.

        Resolves symlinked parent directories of the scratch root first, normalizes relative
        traversals and absolute paths, and asserts that the resulting canonical target lies within
        the scratch root. Raises :exc:`ValueError` if the resolved path escapes the root.
        """
        root = Path(self.name).resolve()
        if not path_parts:
            return root
        p = Path(*path_parts)
        if p.is_absolute():
            resolved = p.resolve()
        else:
            resolved = root.joinpath(p).resolve()
        try:
            resolved.relative_to(root)
        except ValueError:
            raise ValueError(f"Path {p!r} escapes scratch directory root {root!r}") from None
        return resolved

    def write_text(
        self,
        *path_parts: str | os.PathLike,
        text: str,
        encoding: str = "utf-8",
        errors: str | None = None,
    ) -> Path:
        """Safely write text to a path contained strictly within the scratch directory."""
        target = self.resolve_path(*path_parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        self.resolve_path(target.parent)
        target.write_text(text, encoding=encoding, errors=errors)
        return target

    def write_bytes(
        self,
        *path_parts: str | os.PathLike,
        data: bytes,
    ) -> Path:
        """Safely write bytes to a path contained strictly within the scratch directory."""
        target = self.resolve_path(*path_parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        self.resolve_path(target.parent)
        target.write_bytes(data)
        return target

    def remove_path(
        self,
        *path_parts: str | os.PathLike,
        missing_ok: bool = False,
    ) -> None:
        """Safely remove a file or directory tree contained strictly within the scratch directory.

        The parent is resolved and contained like any other path, but the final component is
        taken as written: a symlink there is removed itself, never the target it points to.
        """
        root = Path(self.name).resolve()
        lexical = Path(*path_parts)
        if lexical.name in ("", ".", ".."):
            target = self.resolve_path(*path_parts)
        else:
            target = self.resolve_path(lexical.parent) / lexical.name
        if target == root:
            raise ValueError(f"Cannot remove root scratch directory {target!r} via remove_path")
        if not target.exists() and not target.is_symlink():
            if missing_ok:
                return
            raise FileNotFoundError(f"Path {target!r} does not exist")
        if target.is_dir() and not target.is_symlink():
            shutil.rmtree(target)
        else:
            target.unlink()
