# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for tools/scratch_dir.py, the temporary directory that waits out a Windows lock.

The retry policy is checked on every host by making the standard cleanup fail on cue. On
Windows the real failure is reproduced too: a file held open without delete sharing makes
removal fail with WinError 32, the error the hosted runtime job reported for a selftest
executable. The standard class errors on that lock; this one waits until the holder lets
go, and still raises the real error when the lock outlasts its bound.
"""

from __future__ import annotations

import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import scratch_dir  # noqa: E402

ERROR_SHARING_VIOLATION = 32


def windows_error(winerror: int | None) -> OSError:
    """An OSError carrying ``winerror``, as Windows reports it, on any host."""
    error = PermissionError(13, "synthetic removal failure", "locked.exe")
    if winerror is not None:
        error.winerror = winerror
    return error


class RetryPolicyTests(unittest.TestCase):
    """The policy alone: the standard cleanup fails on cue and no real time passes."""

    def setUp(self) -> None:
        self.directory = scratch_dir.TemporaryDirectory(prefix="scratch-dir-policy-")
        # The patched cleanup below never removes anything; the real one runs at the end.
        self.addCleanup(tempfile.TemporaryDirectory.cleanup, self.directory)
        self.pauses: list[float] = []
        patcher = mock.patch.object(scratch_dir.time, "sleep", side_effect=self.pauses.append)
        patcher.start()
        self.addCleanup(patcher.stop)

    def run_cleanup(self, *outcomes: BaseException | None) -> mock.MagicMock:
        """Run cleanup with the standard cleanup failing as ``outcomes`` says, in order."""
        with mock.patch.object(
            tempfile.TemporaryDirectory, "cleanup", autospec=True, side_effect=list(outcomes)
        ) as standard:
            self.directory.cleanup()
        return standard

    def test_each_transient_windows_error_is_retried_until_removal_succeeds(self) -> None:
        for winerror in sorted(scratch_dir.TRANSIENT_WINERRORS):
            with self.subTest(winerror=winerror):
                self.pauses.clear()
                standard = self.run_cleanup(windows_error(winerror), windows_error(winerror), None)
                self.assertEqual(standard.call_count, 3)
                self.assertEqual(self.pauses, [0.05, 0.1])

    def test_pauses_double_up_to_the_cap(self) -> None:
        failures = [windows_error(ERROR_SHARING_VIOLATION)] * 7
        self.run_cleanup(*failures, None)
        self.assertEqual(self.pauses, [0.05, 0.1, 0.2, 0.4, 0.8, 1.0, 1.0])

    def test_other_errors_are_raised_at_once(self) -> None:
        cases = {
            "no winerror (any non-Windows host)": windows_error(None),
            "a Windows error no lock produces": windows_error(3),  # ERROR_PATH_NOT_FOUND
            "an error that is not an OSError": RuntimeError("not a removal error"),
        }
        for label, error in cases.items():
            with self.subTest(label):
                self.pauses.clear()
                with self.assertRaises(type(error)) as raised:
                    self.run_cleanup(error, None)
                self.assertIs(raised.exception, error)
                self.assertEqual(self.pauses, [])

    def test_a_lock_that_outlasts_the_bound_raises_the_real_error(self) -> None:
        self.directory.retry_seconds = 0.0
        error = windows_error(ERROR_SHARING_VIOLATION)
        with self.assertRaises(PermissionError) as raised:
            self.run_cleanup(error, None)
        self.assertIs(raised.exception, error)
        self.assertEqual(self.pauses, [])
        self.assertEqual(len(error.__notes__), 1)
        self.assertIn(self.directory.name, error.__notes__[0])
        self.assertIn("still locked after 0 s", error.__notes__[0])

    def test_the_wait_never_runs_past_the_bound(self) -> None:
        # The clock reads at the start and after each failure: 0.4 s are left after the
        # first failure, 0.05 s after the second and none after the third.
        clock = iter([0.0, 0.0, 0.35, 0.4])
        error = windows_error(ERROR_SHARING_VIOLATION)
        self.directory.retry_seconds = 0.4
        with mock.patch.object(scratch_dir.time, "monotonic", side_effect=lambda: next(clock)):
            with self.assertRaises(PermissionError) as raised:
                self.run_cleanup(error, error, error)
        self.assertIs(raised.exception, error)
        self.assertEqual(len(self.pauses), 2)
        self.assertEqual(self.pauses[0], 0.05)
        self.assertAlmostEqual(self.pauses[1], 0.05)  # cut from 0.1 to what the bound leaves

    def test_with_statement_uses_the_retrying_cleanup(self) -> None:
        directory = scratch_dir.TemporaryDirectory(prefix="scratch-dir-with-")
        self.addCleanup(tempfile.TemporaryDirectory.cleanup, directory)
        with mock.patch.object(
            tempfile.TemporaryDirectory, "cleanup", autospec=True,
            side_effect=[windows_error(ERROR_SHARING_VIOLATION), None],
        ) as standard:
            with directory as name:
                self.assertTrue(os.path.isdir(name))
        self.assertEqual(standard.call_count, 2)
        self.assertEqual(self.pauses, [0.05])


@unittest.skipUnless(sys.platform == "win32", "only Windows refuses to remove a file that is open")
class WindowsLockTests(unittest.TestCase):
    """The real failure: a file held open without delete sharing blocks its removal."""

    def hold_open(self, directory: tempfile.TemporaryDirectory):
        # Python's open() on Windows does not grant delete sharing, so removing the file
        # fails with ERROR_SHARING_VIOLATION until the handle closes.
        handle = open(Path(directory.name) / "selftest.exe", "wb")
        self.addCleanup(handle.close)
        return handle

    def test_the_standard_class_errors_on_the_lock(self) -> None:
        # The failure the hosted job reported. Once Python retries on its own, this
        # fails and scratch_dir can be retired.
        directory = tempfile.TemporaryDirectory(prefix="scratch-dir-standard-")
        self.addCleanup(directory.cleanup)
        handle = self.hold_open(directory)
        with self.assertRaises(PermissionError) as raised:
            directory.cleanup()
        self.assertEqual(raised.exception.winerror, ERROR_SHARING_VIOLATION)
        handle.close()

    def test_a_lock_released_within_the_bound_is_waited_out(self) -> None:
        directory = scratch_dir.TemporaryDirectory(prefix="scratch-dir-released-")
        self.addCleanup(tempfile.TemporaryDirectory.cleanup, directory)
        handle = self.hold_open(directory)
        release = threading.Timer(0.3, handle.close)
        self.addCleanup(release.cancel)
        release.start()
        directory.cleanup()
        self.assertFalse(os.path.exists(directory.name))

    def test_a_lock_held_past_the_bound_raises_the_real_error(self) -> None:
        directory = scratch_dir.TemporaryDirectory(prefix="scratch-dir-held-")
        self.addCleanup(tempfile.TemporaryDirectory.cleanup, directory)
        directory.retry_seconds = 0.3
        handle = self.hold_open(directory)
        with self.assertRaises(PermissionError) as raised:
            directory.cleanup()
        self.assertEqual(raised.exception.winerror, ERROR_SHARING_VIOLATION)
        self.assertIn("still locked after 0.3 s", raised.exception.__notes__[0])
        self.assertTrue(os.path.exists(directory.name))
        handle.close()


if __name__ == "__main__":
    unittest.main()
