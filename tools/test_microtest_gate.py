# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Offline tests for tools/microtest_gate.py (no emulator runner or game module)."""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

import microtest_gate  # noqa: E402


class MicrotestGateTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = tmp.name

    def run_gate(self, *, step=5, written=None, runner_rc=0, diff_rc=0,
                 exit_error=None, argv=None):
        calls = []

        def fake_run(cmd, **kwargs):
            calls.append(cmd)
            return SimpleNamespace(returncode=runner_rc if len(calls) == 1 else diff_rc)

        patches = [
            patch.object(microtest_gate, "find_exit_syscall_pc",
                         side_effect=exit_error, return_value=0x1000),
            patch.object(microtest_gate, "first_syscall_step", return_value=step),
            patch.object(microtest_gate, "pre_exit_record_count", return_value=step),
            patch.object(microtest_gate, "write_truncated",
                         return_value=step if written is None else written),
            patch.object(microtest_gate.subprocess, "run", side_effect=fake_run),
        ]
        out, err = io.StringIO(), io.StringIO()
        with contextlib.ExitStack() as stack:
            for p in patches:
                stack.enter_context(p)
            stack.enter_context(contextlib.redirect_stdout(out))
            stack.enter_context(contextlib.redirect_stderr(err))
            rc = microtest_gate.main(argv or ["gate", "run.exe", "m.elf", "o.trace", self.dir])
        return rc, out.getvalue(), err.getvalue(), calls

    def test_matching_traces_pass(self):
        rc, out, _, calls = self.run_gate()
        self.assertEqual(rc, 0)
        self.assertIn("microtest gate OK", out)
        self.assertIn("--expect-stop=step-limit", calls[0])
        self.assertIn("--expect-steps", calls[1])

    def test_diverging_traces_fail(self):
        rc, out, _, _ = self.run_gate(diff_rc=1)
        self.assertEqual(rc, 1)
        self.assertNotIn("microtest gate OK", out)

    def test_runner_failure_fails(self):
        rc, _, err, calls = self.run_gate(runner_rc=3)
        self.assertEqual(rc, 2)
        self.assertIn("run_elf exit 3", err)
        self.assertEqual(len(calls), 1)

    def test_zero_pre_exit_instructions_is_not_evidence(self):
        rc, _, err, _ = self.run_gate(step=0)
        self.assertEqual(rc, 2)
        self.assertIn("step 0", err)

    def test_missing_syscall_in_trace_fails(self):
        rc, _, err, _ = self.run_gate(step=None)
        self.assertEqual(rc, 2)
        self.assertIn("no syscall", err)

    def test_short_truncated_trace_fails(self):
        rc, _, err, _ = self.run_gate(written=3)
        self.assertEqual(rc, 2)
        self.assertIn("carries 3 records", err)

    def test_malformed_module_fails(self):
        rc, _, err, calls = self.run_gate(exit_error=ValueError("missing exit_stub symbol"))
        self.assertEqual(rc, 1)
        self.assertIn("Exit resolution failed", err)
        self.assertEqual(calls, [])

    def test_garbage_module_file_is_rejected_without_mocks(self):
        bad = Path(self.dir) / "bad.elf"
        bad.write_bytes(b"not an elf" * 8)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = microtest_gate.main(["gate", "run.exe", str(bad), "o.trace", self.dir])
        self.assertEqual(rc, 1)
        self.assertIn("Exit resolution failed", err.getvalue())

    def test_wrong_argument_count_is_usage_error(self):
        rc, _, err, _ = self.run_gate(argv=["gate", "only-one"])
        self.assertEqual(rc, 2)
        self.assertIn("usage", err)


if __name__ == "__main__":
    unittest.main()
