# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Focused regressions for the strict LOCAL trace coverage contract.

The strict v2 hardware lane already refuses incomplete streams, but the local
codegen/microtest gates run without any PSP metadata and used to share the
permissive v1 loader. Equal lengths are not coverage, so an empty, gapped or
short oracle could previously be truncated into a still-matching comparison.
These tests lock the local contract:

* empty / header-only, arbitrary-header, duplicate, gapped and reordered index
  streams are rejected with a named nonzero status;
* a positive contiguous pair passes without PSP metadata;
* ``--expect-steps N`` requires exactly N records on BOTH sides;
* the first divergence is still reported for register, memory and opcode drift;
* the gate callers state the required length instead of truncating blindly.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import codegen_gate  # noqa: E402
import microtest_gate  # noqa: E402

TRACEDIFF = Path(__file__).resolve().parent / "tracediff.py"
HEADER = ("# psp-recomp trace v1 oracle=interp target=fixture "
          "start_pc=0x08900000\n")
RECORDS = (
    "0 pc=0x08900000 op=0x24080001 r8=0x00000001\n"
    "1 pc=0x08900004 op=0x25080001 r8=0x00000002 m32[0x09ffff00]=0x00000002\n"
    "2 pc=0x08900008 op=0x0008430C r2=0x00000000\n"
)


class StrictLocalTraceCoverageTests(unittest.TestCase):
    """tools/tracediff.py --strict-local, driven as a subprocess like CI does."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="tracediff_local_")
        self.addCleanup(self._cleanup)

    def _cleanup(self):
        for name in os.listdir(self.dir):
            os.unlink(os.path.join(self.dir, name))
        os.rmdir(self.dir)

    def write(self, name, text):
        path = Path(self.dir) / name
        path.write_text(text, encoding="utf-8", newline="\n")
        return path

    def run_tool(self, path_a, path_b, expect_steps=None):
        command = [sys.executable, str(TRACEDIFF), "--strict-local", str(path_a), str(path_b)]
        if expect_steps is not None:
            command += ["--expect-steps", str(expect_steps)]
        proc = subprocess.run(command, capture_output=True, text=True)
        return proc.returncode, proc.stdout + proc.stderr

    def complete_pair(self):
        return self.write("a.trace", HEADER + RECORDS), self.write("b.trace", HEADER + RECORDS)

    def test_complete_positive_pair_passes_without_psp_metadata(self):
        a, b = self.complete_pair()
        rc, output = self.run_tool(a, b, expect_steps=3)
        self.assertEqual(rc, 0, output)
        self.assertIn("local traces identical", output)
        self.assertIn("3 steps covered", output)
        self.assertNotIn("v2", output)

    def test_header_only_stream_is_rejected(self):
        a = self.write("header-only-a.trace", HEADER)
        b = self.write("header-only-b.trace", HEADER)
        rc, output = self.run_tool(a, b)
        self.assertEqual(rc, 2, output)
        self.assertIn("carries no step records", output)

    def test_arbitrary_comment_header_is_rejected(self):
        a = self.write("comment-a.trace", "# runner note\n" + RECORDS)
        b = self.write("comment-b.trace", "# runner note\n" + RECORDS)
        rc, output = self.run_tool(a, b, expect_steps=3)
        self.assertEqual(rc, 2, output)
        self.assertIn("identity header", output)

    def test_duplicate_gap_and_reorder_indices_are_rejected(self):
        cases = {
            "duplicate": "0 pc=0x08900000 op=0x24080001 r8=0x00000001\n"
                         "0 pc=0x08900004 op=0x24080001 r8=0x00000002\n",
            "gap": "0 pc=0x08900000 op=0x24080001 r8=0x00000001\n"
                   "2 pc=0x08900008 op=0x0008430C r2=0x00000000\n",
            "reorder": "1 pc=0x08900004 op=0x24080001 r8=0x00000002\n"
                       "0 pc=0x08900000 op=0x24080001 r8=0x00000001\n",
        }
        for name, records in cases.items():
            with self.subTest(stream=name):
                a = self.write(f"{name}-a.trace", HEADER + records)
                b = self.write(f"{name}-b.trace", HEADER + records)
                rc, output = self.run_tool(a, b)
                self.assertEqual(rc, 2, output)
                self.assertIn("not contiguous", output)

    def test_required_coverage_shorter_on_either_side_is_rejected(self):
        a, b = self.complete_pair()
        rc, output = self.run_tool(a, b, expect_steps=4)
        self.assertEqual(rc, 2, output)
        self.assertIn("required coverage of 4 step records is incomplete: captured 3", output)

        short = self.write("short.trace", HEADER + "0 pc=0x08900000 op=0x24080001 r8=0x00000001\n")
        rc, output = self.run_tool(short, b, expect_steps=3)
        self.assertEqual(rc, 2, output)
        self.assertIn("captured 1", output)

    def test_over_long_stream_is_rejected_when_length_is_stated(self):
        a, b = self.complete_pair()
        rc, output = self.run_tool(a, b, expect_steps=2)
        self.assertEqual(rc, 2, output)
        self.assertIn("exceeds the required 2 records", output)

    def test_non_positive_expected_length_is_a_usage_error(self):
        a, b = self.complete_pair()
        for value in ("0", "-1", "abc", "99999999999"):
            with self.subTest(value=value):
                rc, output = self.run_tool(a, b, expect_steps=value)
                self.assertEqual(rc, 2, output)
                self.assertIn("positive decimal record count", output)

    def test_identity_start_pc_mismatch_is_rejected(self):
        a, _ = self.complete_pair()
        other_header = ("# psp-recomp trace v1 oracle=recomp target=fixture "
                        "start_pc=0x08910000\n")
        b = self.write("other.trace", other_header + RECORDS)
        rc, output = self.run_tool(a, b, expect_steps=3)
        self.assertEqual(rc, 2, output)
        self.assertIn("start_pc differs", output)

    def test_first_divergence_is_still_reported(self):
        a, _ = self.complete_pair()
        for name, records, expected in (
            ("register", RECORDS.replace("r8=0x00000002", "r8=0x00000009"),
             "writes differ: A[r8=0x00000002] B[r8=0x00000009]"),
            ("memory", RECORDS.replace("m32[0x09ffff00]=0x00000002",
                                        "m32[0x09ffff00]=0x00000007"),
             "m32[0x09ffff00]=0x00000007"),
            ("opcode", RECORDS.replace("op=0x25080001", "op=0x25080003"),
             "op 0x25080001 vs 0x25080003"),
        ):
            with self.subTest(field=name):
                b = self.write(f"divergent-{name}.trace", HEADER + records)
                rc, output = self.run_tool(a, b, expect_steps=3)
                self.assertEqual(rc, 1, output)
                self.assertIn("DIVERGENCE at step 1, pc 0x08900004", output)
                self.assertIn(expected, output)

    def test_legacy_v1_mode_is_unchanged_for_informational_callers(self):
        """The compatibility boundary is explicit: only gate callers opt in.

        tools/test_gate_exit_resolution.py keeps the legacy route authoritative
        for maintained informational callers; this test proves the strict local
        route is additional and does not redefine that route.
        """
        a = self.write("legacy-a.trace", HEADER + RECORDS)
        b = self.write("legacy-b.trace", HEADER + RECORDS)
        proc = subprocess.run([sys.executable, str(TRACEDIFF), str(a), str(b)],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("traces identical, 3 steps", proc.stdout)


class GateRequiredCoverageTests(unittest.TestCase):
    """The gate callers must state the length they require, not just truncate."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="gate_coverage_")
        self.addCleanup(self._cleanup)

    def _cleanup(self):
        for root, _, files in os.walk(self.dir):
            for name in files:
                os.unlink(os.path.join(root, name))
        os.rmdir(self.dir)

    def write_oracle(self, name, indices, exit_index=None):
        """Write a real oracle trace; ``exit_index`` carries the exit syscall."""
        lines = [HEADER]
        for index in indices:
            is_exit = index == exit_index
            op = "0x0008430C" if is_exit else "0x24080001"
            lines.append(f"{index} pc=0x{0x08900000 + 4 * index:08x} "
                         f"op={op} r8=0x0000000{index}\n")
        path = Path(self.dir) / name
        path.write_text("".join(lines), encoding="utf-8", newline="\n")
        return path

    def test_pre_exit_record_count_rejects_an_incomplete_oracle(self):
        gapped = self.write_oracle("gapped.trace", (0, 2, 3))
        with self.assertRaises(codegen_gate.CoverageError) as caught:
            codegen_gate.pre_exit_record_count(gapped, 3)
        self.assertIn("required pre-exit coverage is 3 records", str(caught.exception))
        self.assertIn("supplied 2", str(caught.exception))

    def test_pre_exit_record_count_accepts_a_complete_oracle(self):
        complete = self.write_oracle("complete.trace", (0, 1, 2, 3, 4))
        self.assertEqual(codegen_gate.pre_exit_record_count(complete, 3), 3)

    def test_truncate_reports_how_many_records_it_wrote(self):
        complete = self.write_oracle("count.trace", (0, 1, 2, 3))
        out = Path(self.dir) / "truncated.trace"
        self.assertEqual(codegen_gate.truncate(complete, out, 3), 3)
        self.assertEqual(microtest_gate.write_truncated(complete, out, 2), 2)

    def test_microtest_gate_refuses_an_incomplete_pre_exit_range(self):
        """A gapped oracle must not reach the comparator at all."""
        oracle = self.write_oracle("microtest-gapped.trace", (0, 2, 3), exit_index=3)
        with mock.patch.object(microtest_gate, "find_exit_syscall_pc",
                               return_value=0x0890000C), \
             mock.patch.object(microtest_gate.subprocess, "run") as runner:
            rc = microtest_gate.main(["microtest_gate.py", "run_elf.exe", "m.elf",
                                      str(oracle), self.dir])
        self.assertEqual(rc, 2)
        runner.assert_not_called()

    def test_microtest_gate_states_the_required_length_to_the_comparator(self):
        oracle = self.write_oracle("microtest-complete.trace", (0, 1, 2, 3), exit_index=3)
        completed = subprocess.CompletedProcess(args=[], returncode=0, stdout="", stderr="")
        with mock.patch.object(microtest_gate, "find_exit_syscall_pc",
                               return_value=0x0890000C), \
             mock.patch.object(microtest_gate.subprocess, "run",
                               return_value=completed) as runner:
            rc = microtest_gate.main(["microtest_gate.py", "run_elf.exe", "m.elf",
                                      str(oracle), self.dir])
        self.assertEqual(rc, 0)
        commands = [call.args[0] for call in runner.call_args_list]
        self.assertTrue(
            any("--strict-local" in command
                and command[-2:] == ["--expect-steps", "3"]
                for command in commands), commands)

    def test_microtest_gate_refuses_a_zero_pre_exit_fixture(self):
        oracle = self.write_oracle("microtest-zero.trace", (0,), exit_index=0)
        with mock.patch.object(microtest_gate, "find_exit_syscall_pc",
                               return_value=0x08900000), \
             mock.patch.object(microtest_gate.subprocess, "run") as runner:
            rc = microtest_gate.main(["microtest_gate.py", "run_elf.exe", "m.elf",
                                      str(oracle), self.dir])
        self.assertEqual(rc, 2)
        runner.assert_not_called()


if __name__ == "__main__":
    unittest.main()
