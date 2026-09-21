# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-09-21.
# See NOTICE.md for upstream lineage and modification provenance.
#
# Synthetic fail-closed tests for tools/funcdiff_cmp.py (issue #381).
# No retail assets, traces, or golden hashes: every fixture is synthesized here.
# The suite was first run against the pre-fix tool to record the failing-before
# behavior (empty recomp -> MATCH 0 steps, truncated oracle -> MATCH on the
# common prefix, entry beyond oracle -> MATCH 0 steps); now it pins the
# fail-closed contract.
import os
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(TOOLS, "funcdiff_cmp.py")


def run_tool(argv):
    proc = subprocess.run(
        [sys.executable, "-I", TOOL] + argv,
        capture_output=True,
        text=True,
        cwd=TOOLS,
        encoding="utf-8",
        errors="replace",
        timeout=60,
    )
    return proc.returncode, proc.stdout + proc.stderr


class TestFuncdiffCmpFailClosed(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.d = self._tmp.name
        self.n = 0

    def path(self, lines, name=None):
        self.n += 1
        p = os.path.join(self.d, name or f"f{self.n}.trace")
        with open(p, "w", newline="\n") as f:
            if lines:
                f.write("\n".join(lines) + "\n")
        return p

    def steps(self, n, pc0=0x100, tag="a"):
        return [f"{i} pc=0x{pc0+i:08x} op=0x00000021 r1=0x{tag}0{i:02x}" for i in range(n)]

    def test_exact_nonempty_match_exits_zero(self):
        oracle = self.path(self.steps(4))
        mine = self.path(self.steps(4))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 0, out)
        self.assertIn("MATCH: 4 steps identical", out)
        self.assertIn("oracle step 0", out)

    def test_first_step_mismatch_fails(self):
        oracle = self.path(self.steps(3))
        mine = self.path(self.steps(3, tag="b"))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("DIVERGENCE at my step 0", out)
        self.assertIn("oracle:", out)
        self.assertIn("recomp:", out)

    def test_divergence_after_matching_prefix_fails(self):
        oracle = self.path(self.steps(3))
        mine = self.path(self.steps(3)[:-1] + [self.steps(3)[-1].replace("pc=0x00000102", "pc=0x00000202")])
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("DIVERGENCE at my step 2 (oracle step 2)", out)

    def test_empty_recomp_trace_fails(self):
        oracle = self.path(self.steps(3))
        mine = self.path([])
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("empty recomp trace", out)
        self.assertNotIn("MATCH", out)

    def test_blank_only_recomp_trace_fails(self):
        oracle = self.path(self.steps(3))
        mine = self.path(["", ""])
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)

    def test_empty_oracle_fails(self):
        oracle = self.path([])
        mine = self.path(self.steps(2))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("no steps at or after entry-step 0", out)

    def test_entry_beyond_oracle_fails(self):
        oracle = self.path(self.steps(3))
        mine = self.path(self.steps(2))
        rc, out = run_tool([oracle, mine, "50"])
        self.assertEqual(rc, 1, out)
        self.assertIn("entry-step 50", out)

    def test_oracle_truncated_before_recomp_trace_ends_fails(self):
        oracle = self.path(self.steps(1))
        mine = self.path(self.steps(3))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("oracle truncated before recomp trace ends", out)
        self.assertIn("need 3 steps", out)
        self.assertIn("found 1", out)

    def test_matching_recomp_strict_prefix_with_enough_oracle_steps_passes(self):
        # The oracle legitimately continues beyond the requested recomp-length slice.
        oracle = self.path(self.steps(5))
        mine = self.path(self.steps(3))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 0, out)
        self.assertIn("MATCH: 3 steps identical", out)

    def test_oracle_prefix_at_nonzero_entry_passes(self):
        oracle_lines = self.steps(6)
        oracle = self.path(oracle_lines)
        mine = self.path(oracle_lines[4:6])  # identical to oracle steps 4..5
        rc, out = run_tool([oracle, mine, "4"])
        self.assertEqual(rc, 0, out)
        self.assertIn("MATCH: 2 steps identical", out)
        self.assertIn("oracle step 4", out)

    def test_malformed_recomp_record_fails_explicitly(self):
        oracle = self.path(self.steps(3))
        mine = self.path(self.steps(3)[:1] + ["7 pc=0x1 onlytwofields"] + self.steps(3)[2:])
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("malformed trace record", out)
        self.assertNotIn("MATCH", out)

    def test_malformed_oracle_record_cannot_shrink_coverage(self):
        # One malformed oracle record inside the requested slice must fail the run,
        # not silently shrink the compared coverage into a MATCH.
        oracle = self.path(self.steps(3)[:1] + ["garbage line"] + self.steps(3)[2:])
        mine = self.path(self.steps(3))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("malformed oracle record", out)
        self.assertNotIn("MATCH", out)

    def test_malformed_oracle_step_number_fails(self):
        oracle = self.path(["x1 pc=0x00000100 op=0x00000021"] + self.steps(2)[1:])
        mine = self.path(self.steps(2))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("malformed oracle step number", out)

    def test_write_token_without_equals_fails(self):
        oracle = self.path(self.steps(2)[:1] + ["1 pc=0x00000101 op=0x00000021 r1 junk"] + self.steps(2)[1:])
        mine = self.path(self.steps(2))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("malformed write token", out)

    def test_blank_oracle_line_fails(self):
        oracle = self.path([""] + self.steps(2))
        mine = self.path(self.steps(2))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("malformed oracle record", out)

    def test_comment_lines_are_ignored(self):
        oracle = self.path(["# header comment"] + self.steps(3))
        mine = self.path(["# header comment"] + self.steps(3))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 0, out)
        self.assertIn("MATCH: 3 steps identical", out)

    def test_missing_recomp_file_is_a_usage_error(self):
        oracle = self.path(self.steps(2))
        rc, out = run_tool([oracle, os.path.join(self.d, "nope.trace"), "0"])
        self.assertEqual(rc, 2, out)
        self.assertIn("cannot read recomp trace", out)

    def test_missing_oracle_file_is_a_usage_error(self):
        mine = self.path(self.steps(2))
        rc, out = run_tool([os.path.join(self.d, "nope.trace"), mine, "0"])
        self.assertEqual(rc, 2, out)
        self.assertIn("cannot read oracle trace", out)

    def test_wrong_arity_and_bad_entry_step_are_usage_errors(self):
        t = self.path(self.steps(1))
        for argv, needle in (
            ([t], "usage:"),
            ([t, t, "notanumber"], "entry-step must be a nonnegative integer"),
            ([t, t, "-1"], "entry-step must be a nonnegative integer"),
        ):
            rc, out = run_tool(argv)
            self.assertEqual(rc, 2, out)
            self.assertIn(needle, out)

    def test_crlf_traces_compare_equal(self):
        p1 = os.path.join(self.d, "crlf_o.trace")
        p2 = os.path.join(self.d, "crlf_m.trace")
        with open(p1, "w", newline="\r\n") as f:
            f.write("\n".join(self.steps(2)) + "\n")
        with open(p2, "w", newline="\r\n") as f:
            f.write("\n".join(self.steps(2)) + "\n")
        rc, out = run_tool([p1, p2, "0"])
        self.assertEqual(rc, 0, out)
        self.assertIn("MATCH: 2 steps identical", out)


class TestFuncdiffCmpContractUnit(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, TOOLS)
        for name in list(sys.modules):
            if name == "funcdiff_cmp":
                del sys.modules[name]
        global funcdiff_cmp
        import funcdiff_cmp as m

        self.m = m

    def test_norm_rejects_short_and_shapeless_records(self):
        for bad in ("1 pc=0x1", "1 2 3", "step pc=0x1 op=0x2 w", ""):
            with self.assertRaises(self.m.TraceFormatError):
                self.m.norm(bad, 1, "synthetic")

    def test_norm_keeps_write_sets_as_sets(self):
        pc, op, writes = self.m.norm("3 pc=0x00000100 op=0x00000021 r1=0x1 r2=0x2", 1, "s")
        self.assertEqual((pc, op), ("pc=0x00000100", "op=0x00000021"))
        self.assertEqual(writes, frozenset({"r1=0x1", "r2=0x2"}))

    def test_oracle_slice_requires_full_coverage_but_not_file_end(self):
        import tempfile

        def slice_of(lines, entry, need):
            with tempfile.NamedTemporaryFile("w", suffix=".trace", delete=False, newline="\n") as fh:
                fh.write("\n".join(lines) + "\n")
                name = fh.name
            self.addCleanup(os.unlink, name)
            return self.m.load_oracle_slice(name, entry, need)

        rows = [f"{i} pc=0x0000{i:04x} op=0x00000021" for i in range(6)]
        got = slice_of(rows, 2, 3)
        self.assertEqual([r[0] for r in got], ["pc=0x00000002", "pc=0x00000003", "pc=0x00000004"])
        with self.assertRaises(self.m.TraceFormatError):
            slice_of([f"{i} pc=0x1 op=0x2" for i in range(3)], 0, 4)
        with self.assertRaises(self.m.TraceFormatError):
            slice_of(rows, 6, 1)


if __name__ == "__main__":
    unittest.main()
