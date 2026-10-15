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
