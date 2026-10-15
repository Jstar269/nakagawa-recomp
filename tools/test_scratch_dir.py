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


class PathIsolationTests(unittest.TestCase):
    """Path isolation and containment tests for TemporaryDirectory."""

    def setUp(self) -> None:
        self.outside_dir = tempfile.TemporaryDirectory(prefix="scratch-isolation-outside-")
        self.addCleanup(self.outside_dir.cleanup)
        self.outside_path = Path(self.outside_dir.name).resolve()
        self.outside_file = self.outside_path / "user_file.txt"
        self.outside_file.write_text("SAFE_USER_DATA", encoding="utf-8")

        self.scratch = scratch_dir.TemporaryDirectory(prefix="scratch-isolation-root-")
        self.addCleanup(self.scratch.cleanup)
        self.root_path = Path(self.scratch.name).resolve()

    def test_valid_relative_and_subpath_writes_and_removals_stay_inside_root(self) -> None:
        p1 = self.scratch.write_text("sub", "file1.txt", text="content1")
        self.assertEqual(p1.read_text(encoding="utf-8"), "content1")
        self.assertEqual(p1, self.root_path / "sub" / "file1.txt")

        p2 = self.scratch.write_bytes("file2.bin", data=b"bytes2")
        self.assertEqual(p2.read_bytes(), b"bytes2")

        self.scratch.remove_path("sub", "file1.txt")
        self.assertFalse(p1.exists())

        self.scratch.remove_path("sub")
        self.assertFalse((self.root_path / "sub").exists())

    def test_symlinked_parent_directory_of_scratch_root_does_not_cause_false_escapes(self) -> None:
        probe = self.outside_path / "symlink_parent_probe"
        try:
            probe.symlink_to(self.root_path, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"Host cannot create symlinks: {error}")

        # Open scratch using the symlinked parent route
        aliased = scratch_dir.TemporaryDirectory(dir=str(probe))
        self.addCleanup(aliased.cleanup)

        written = aliased.write_text("inside_alias.txt", text="aliased_ok")
        self.assertTrue(written.exists())
        self.assertEqual(written.read_text(encoding="utf-8"), "aliased_ok")

    def test_absolute_path_outside_root_raises_and_cannot_write_or_delete_outside_file(self) -> None:
        outside_str = str(self.outside_file)
        with self.assertRaises(ValueError) as ctx:
            self.scratch.write_text(outside_str, text="HACKED")
        self.assertIn("escapes scratch directory root", str(ctx.exception))
        self.assertEqual(self.outside_file.read_text(encoding="utf-8"), "SAFE_USER_DATA")

        with self.assertRaises(ValueError) as ctx:
            self.scratch.write_bytes(outside_str, data=b"HACKED")
        self.assertIn("escapes scratch directory root", str(ctx.exception))
        self.assertEqual(self.outside_file.read_text(encoding="utf-8"), "SAFE_USER_DATA")

        with self.assertRaises(ValueError) as ctx:
            self.scratch.remove_path(outside_str)
        self.assertIn("escapes scratch directory root", str(ctx.exception))
        self.assertTrue(self.outside_file.exists())

    def test_relative_traversal_outside_root_raises_and_cannot_write_or_delete_outside_file(self) -> None:
        rel_escape = f"../../{self.outside_path.name}/{self.outside_file.name}"

        with self.assertRaises(ValueError) as ctx:
            self.scratch.write_text(rel_escape, text="HACKED")
        self.assertIn("escapes scratch directory root", str(ctx.exception))
        self.assertEqual(self.outside_file.read_text(encoding="utf-8"), "SAFE_USER_DATA")

        with self.assertRaises(ValueError) as ctx:
            self.scratch.remove_path(rel_escape)
        self.assertIn("escapes scratch directory root", str(ctx.exception))
        self.assertTrue(self.outside_file.exists())

    def test_symlink_escape_target_outside_root_raises_and_cannot_write_or_delete_outside_file(self) -> None:
        link_path = self.root_path / "outside_link"
        try:
            link_path.symlink_to(self.outside_path, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"Host cannot create symlinks: {error}")

        with self.assertRaises(ValueError) as ctx:
            self.scratch.write_text("outside_link", "user_file.txt", text="HACKED")
        self.assertIn("escapes scratch directory root", str(ctx.exception))
        self.assertEqual(self.outside_file.read_text(encoding="utf-8"), "SAFE_USER_DATA")

        with self.assertRaises(ValueError) as ctx:
            self.scratch.remove_path("outside_link", "user_file.txt")
        self.assertIn("escapes scratch directory root", str(ctx.exception))
        self.assertTrue(self.outside_file.exists())

    def test_remove_path_removes_an_in_root_symlink_and_keeps_its_target(self) -> None:
        # The final component is the link itself: removing it must not delete what it names.
        real_file = self.scratch.write_text("real.txt", text="keep me")
        inner = self.scratch.write_text("real_dir", "inner.txt", text="keep me too")
        file_link = self.root_path / "file_link.txt"
        dir_link = self.root_path / "dir_link"
        try:
            file_link.symlink_to(real_file)
            dir_link.symlink_to(inner.parent, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"Host cannot create symlinks: {error}")

        self.scratch.remove_path("file_link.txt")
        self.scratch.remove_path("dir_link")

        self.assertFalse(os.path.lexists(file_link))
        self.assertFalse(os.path.lexists(dir_link))
        self.assertEqual(real_file.read_text(encoding="utf-8"), "keep me")
        self.assertEqual(inner.read_text(encoding="utf-8"), "keep me too")


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
