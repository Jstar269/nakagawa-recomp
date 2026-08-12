# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import pspdev_probe


class ProbeTests(unittest.TestCase):
    def test_probe_executable_records_hash_and_bounded_output_without_path(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            exe = Path(td) / "psp-gcc"
            exe.write_bytes(b"synthetic executable")

            def runner(command, **kwargs):
                self.assertFalse(kwargs["shell"])
                self.assertEqual(command[0], str(exe.resolve()))
                return subprocess.CompletedProcess(
                    command,
                    0,
                    stdout=b"psp-gcc synthetic 1.0\r\n",
                    stderr=b"",
                )

            result = pspdev_probe.probe_executable(
                "psp-gcc", exe, ("--version",), runner=runner
            )
            self.assertEqual(result["status"], "ok")
            self.assertEqual(
                result["binary_sha256"],
                hashlib.sha256(b"synthetic executable").hexdigest(),
            )
            self.assertEqual(result["stdout"], "psp-gcc synthetic 1.0\n")
            self.assertNotIn("path", result)
            self.assertEqual(result["resolved_name"], "psp-gcc")

    def test_version_output_paths_are_redacted_by_default(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            exe = Path(td) / "psp-config"
            exe.write_bytes(b"x")

            def runner(command, **kwargs):
                return subprocess.CompletedProcess(
                    command,
                    0,
                    stdout=(
                        str(exe.parent) + "\n" + "C:" + r"\Users\Example\pspdev" + "\n"
                    ).encode(),
                    stderr=b"",
                )

            result = pspdev_probe.probe_executable(
                "psp-config", exe, ("--pspsdk-path",), runner=runner
            )
            self.assertNotIn(str(exe.parent), result["stdout"])
            self.assertNotIn("Users", result["stdout"])
            self.assertIn("<redacted-path>", result["stdout"])

    def test_include_paths_is_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            exe = Path(td) / "psp-ld"
            exe.write_bytes(b"x")

            def runner(command, **kwargs):
                return subprocess.CompletedProcess(
                    command, 0, stdout=b"", stderr=b""
                )

            result = pspdev_probe.probe_executable(
                "psp-ld",
                exe,
                ("--version",),
                include_paths=True,
                runner=runner,
            )
            self.assertEqual(result["path"], str(exe.resolve()))

    def test_output_is_capped_and_marked(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            exe = Path(td) / "tool"
            exe.write_bytes(b"x")

            def runner(command, **kwargs):
                return subprocess.CompletedProcess(
                    command,
                    1,
                    stdout=b"A" * (pspdev_probe.MAX_OUTPUT_BYTES + 100),
                    stderr=b"B" * (pspdev_probe.MAX_OUTPUT_BYTES + 200),
                )

            result = pspdev_probe.probe_executable(
                "tool", exe, (), runner=runner
            )
            self.assertEqual(result["status"], "nonzero")
            self.assertTrue(result["stdout_truncated"])
            self.assertTrue(result["stderr_truncated"])
            self.assertEqual(len(result["stdout"]), pspdev_probe.MAX_OUTPUT_BYTES)
            self.assertEqual(len(result["stderr"]), pspdev_probe.MAX_OUTPUT_BYTES)

    def test_timeout_is_reported_without_retry(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            exe = Path(td) / "tool"
            exe.write_bytes(b"x")
            calls = 0

            def runner(command, **kwargs):
                nonlocal calls
                calls += 1
                raise subprocess.TimeoutExpired(command, kwargs["timeout"])

            result = pspdev_probe.probe_executable(
                "tool", exe, (), runner=runner
            )
            self.assertEqual(result["status"], "timeout")
            self.assertEqual(calls, 1)

    def test_probe_tools_is_deterministic_and_marks_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            existing = root / "psp-gcc"
            existing.write_bytes(b"x")

            def resolver(name, path=None):
                return str(existing) if name == "psp-gcc" else None

            def runner(command, **kwargs):
                return subprocess.CompletedProcess(
                    command, 0, stdout=b"v\n", stderr=b""
                )

            a = pspdev_probe.probe_tools(resolver=resolver, runner=runner)
            b = pspdev_probe.probe_tools(resolver=resolver, runner=runner)
            self.assertEqual(a, b)
            statuses = {item["name"]: item["status"] for item in a["tools"]}
            self.assertEqual(statuses["psp-gcc"], "ok")
            self.assertEqual(statuses["psp-ld"], "missing")
            self.assertEqual(
                [item["name"] for item in a["tools"]],
                sorted(pspdev_probe.TOOL_ARGS),
            )

    def test_real_runner_kills_output_flood_at_cap(self) -> None:
        result = pspdev_probe._run_bounded_process(
            [
                sys.executable,
                "-c",
                "import sys; sys.stdout.buffer.write(b'x' * 200000); "
                "sys.stdout.flush()",
            ],
            timeout=5.0,
            max_output=1024,
        )
        self.assertEqual(result["status"], "output_limit")
        self.assertTrue(result["stdout_truncated"])
        self.assertEqual(len(result["stdout"]), 1024)

    def test_oversized_executable_fails_before_execution(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            exe = Path(td) / "tool"
            exe.write_bytes(b"0123456789")
            with self.assertRaises(pspdev_probe.ProbeError):
                pspdev_probe._sha256_file(exe, max_bytes=5)


if __name__ == "__main__":
    unittest.main()
