# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import psp_readiness  # noqa: E402


class ClientDirDiscoveryTests(unittest.TestCase):
    """PSPLINK PC clients are found via an operator-configured directory."""

    def test_unset_environment_finds_nothing(self) -> None:
        with mock.patch.dict(os.environ, {psp_readiness.ENV_CLIENT_DIR: ""}, clear=False):
            self.assertEqual(psp_readiness._client_dir_tools(), set())

    def test_missing_directory_is_not_an_error(self) -> None:
        with mock.patch.dict(
            os.environ, {psp_readiness.ENV_CLIENT_DIR: str(Path(tempfile.gettempdir()) / "no-such-dir-xyz")}
        ):
            self.assertEqual(psp_readiness._client_dir_tools(), set())

    def test_windows_and_bare_client_names_are_both_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            (directory / "usbhostfs_pc.exe").write_bytes(b"")
            (directory / "pspsh").write_bytes(b"")
            with mock.patch.dict(os.environ, {psp_readiness.ENV_CLIENT_DIR: raw}):
                self.assertEqual(psp_readiness._client_dir_tools(), {"usbhostfs_pc", "pspsh"})


class PpssppHeadlessDiscoveryTests(unittest.TestCase):
    def test_explicit_override_must_exist(self) -> None:
        with mock.patch.dict(os.environ, {psp_readiness.ENV_PPSSPP_HEADLESS: "/definitely/not/here.exe"}):
            self.assertIsNone(psp_readiness._ppsspp_headless())

    def test_explicit_override_is_returned_when_present(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            candidate = Path(raw) / "PPSSPPHeadless.exe"
            candidate.write_bytes(b"")
            with mock.patch.dict(os.environ, {psp_readiness.ENV_PPSSPP_HEADLESS: str(candidate)}):
                self.assertEqual(psp_readiness._ppsspp_headless(), candidate)


class PsplinkDeviceDetectionTests(unittest.TestCase):
    """Tool presence must never be mistaken for a reachable device."""

    def test_no_endpoint_reports_not_connected(self) -> None:
        with mock.patch.object(psp_readiness.sys, "platform", "win32"), \
             mock.patch.object(psp_readiness, "_run", return_value=(0, "0")):
            connected, detail = psp_readiness._psplink_device()
        self.assertFalse(connected)
        self.assertIn("mass-storage mode does not count", detail)

    def test_present_endpoint_reports_connected(self) -> None:
        with mock.patch.object(psp_readiness.sys, "platform", "win32"), \
             mock.patch.object(psp_readiness, "_run", return_value=(0, "1")):
            connected, _detail = psp_readiness._psplink_device()
        self.assertTrue(connected)

    def test_query_failure_is_treated_as_not_connected(self) -> None:
        with mock.patch.object(psp_readiness.sys, "platform", "win32"), \
             mock.patch.object(psp_readiness, "_run", return_value=(1, "")):
            connected, _detail = psp_readiness._psplink_device()
        self.assertFalse(connected)

    def test_non_windows_does_not_assume_a_device(self) -> None:
        with mock.patch.object(psp_readiness.sys, "platform", "linux"):
            connected, detail = psp_readiness._psplink_device()
        self.assertFalse(connected)
        self.assertIn("host0", detail)

    def test_query_uses_an_array_subexpression(self) -> None:
        """Regression: the probe silently reported "not connected" forever.

        `psp_readiness` shells out to `powershell.exe`, i.e. Windows PowerShell
        5.1, where a single pipeline object has no `.Count` property and the
        expression evaluates to $null. The original query lacked `@(...)`, so
        both the zero-match and the one-match case produced empty output and the
        check could never report a connected device -- a false negative that
        looks correct because the common case really is disconnected. Mocks
        cannot catch this, so the command text itself is asserted.
        """

        captured: dict[str, list[str]] = {}

        def fake_run(command, *, timeout=5.0):
            captured["command"] = command
            return 0, "1"

        with mock.patch.object(psp_readiness.sys, "platform", "win32"), \
             mock.patch.object(psp_readiness, "_run", fake_run):
            psp_readiness._psplink_device()

        script = captured["command"][-1]
        self.assertIn("@(", script, "query must force an array before reading .Count")
        self.assertIn("VID_054C&PID_01C9", script)
        self.assertNotIn("PID_02D2", script, "mass-storage mode must not count as a link")


if __name__ == "__main__":
    unittest.main()
