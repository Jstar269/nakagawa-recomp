# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Gate-outcome binding and bounded-wait regressions for the cosim mutation driver.

A mutation verdict is only evidence when it agrees with what actually happened.
``fixtures/cosim/mutate.py`` used to classify by substring alone, so a log
containing ``cosim: OK`` was scored as a pass even when the process exited
nonzero, a bare ``cosim: FAIL`` counted as a kill without a located divergence,
and an unbounded wait let a hung build own the rest of a CI job.

These cases drive the real ``run_gate`` with synthetic children on any host: no
compiler, no guest binary, and no repository mutation.
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUTATE = ROOT / "fixtures" / "cosim" / "mutate.py"

_spec = importlib.util.spec_from_file_location("cosim_mutate", MUTATE)
mutate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mutate)


# Long enough that a surviving grandchild is guaranteed to produce its marker
# inside the post-termination wait, so "no marker" means "no survivor".
LONG_CHILD_S = 6


def _child(code: str) -> list[str]:
    """One synthetic gate invocation: the same python this test runs on."""
    return [sys.executable, "-c", textwrap.dedent(code)]


class GateOutcomeBindingTests(unittest.TestCase):
    """The verdict must follow the process outcome, never the marker alone."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="cosim_gate_outcome_")
        self.dir = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)
        self.source = self.dir / "guest_interp.c"
        self.source.write_text("/* synthetic mutant source */\n", encoding="utf-8")

    def run_with(self, code: str, timeout_s: int | None = None):
        original = mutate.make_command
        mutate.make_command = lambda source, target: _child(code)
        try:
            return mutate.run_gate(self.source, timeout_s=timeout_s)
        finally:
            mutate.make_command = original

    def test_zero_exit_with_ok_marker_passes(self):
        verdict, detail = self.run_with(
            """
            print("cosim: OK: 128 cells")
            """
        )
        self.assertEqual(verdict, "ok", detail)

    def test_nonzero_exit_never_passes_on_ok_text(self):
        verdict, detail = self.run_with(
            """
            import sys
            print("cosim: OK: 128 cells")
            print("make: *** Error 2", file=sys.stderr)
            sys.exit(2)
            """
        )
        self.assertEqual(verdict, "inconsistent", detail)
        self.assertIn("nonzero process never passes on OK text", detail)

    def test_zero_exit_with_fail_marker_is_inconsistent(self):
        verdict, detail = self.run_with(
            """
            print("cosim: FAIL")
            """
        )
        self.assertEqual(verdict, "inconsistent", detail)
        self.assertIn("contradicts the process outcome", detail)

    def test_bare_fail_without_a_divergence_is_not_a_kill(self):
        verdict, detail = self.run_with(
            """
            import sys
            print("cosim: FAIL")
            sys.exit(1)
            """
        )
        self.assertEqual(verdict, "no-run", detail)
        self.assertIn("not a kill", detail)

    def test_nonzero_exit_with_a_located_divergence_is_a_kill(self):
        verdict, detail = self.run_with(
            """
            import sys
            print("COSIM DIVERGENCE cell=case_0007 pc=0x08900100 op=0x24080001")
            print("cosim: FAIL: 1 divergence")
            sys.exit(1)
            """
        )
        self.assertEqual(verdict, "fail", detail)
        self.assertIn("COSIM DIVERGENCE cell=case_0007", detail)

    def test_build_failure_without_markers_stays_no_run(self):
        verdict, detail = self.run_with(
            """
            import sys
            print("gcc: error: no such file", file=sys.stderr)
            sys.exit(2)
            """
        )
        self.assertEqual(verdict, "no-run", detail)

    def test_timeout_is_neither_pass_nor_kill_and_reaps_the_child(self):
        # The grandchild writes its marker only AFTER a delay longer than the wait
        # below, so a surviving child is guaranteed to show up as evidence rather
        # than racing the assertion.
        marker = self.dir / "grandchild-finished.marker"
        verdict, detail = self.run_with(
            f"""
            import subprocess, sys, time
            print("cosim: FAIL")
            subprocess.Popen([sys.executable, "-c",
                "import pathlib,time;"
                "time.sleep({LONG_CHILD_S});"
                f"pathlib.Path({str(marker)!r}).write_text('leaked')"])
            time.sleep(120)
            """,
            timeout_s=1,
        )
        self.assertEqual(verdict, "timeout", detail)
        self.assertIn("neither a pass nor a semantic kill", detail)
        # The gate's own build children must be gone, not merely abandoned: a
        # surviving build child keeps the CI slot busy after the driver reported.
        time.sleep(LONG_CHILD_S + 2)
        self.assertFalse(marker.exists(),
                         "a build child survived the driver's termination")

    def test_baseline_rejects_an_inconsistent_gate(self):
        verdict, _ = self.run_with(
            """
            import sys
            print("cosim: OK")
            sys.exit(3)
            """
        )
        self.assertNotEqual(verdict, "ok",
                            "the campaign baseline must not accept an inconsistent gate")


class ProcessTreeReapingTests(unittest.TestCase):
    def test_terminator_kills_the_whole_child_tree(self):
        with tempfile.TemporaryDirectory(prefix="cosim_reap_") as tmp:
            marker = Path(tmp) / "grandchild-finished.marker"
            grandchild = [sys.executable, "-c",
                          f"import pathlib,time; time.sleep({LONG_CHILD_S}); "
                          f"pathlib.Path({str(marker)!r}).write_text('leaked')"]
            process = subprocess.Popen(
                [sys.executable, "-c",
                 "import subprocess,sys,time;"
                 f"subprocess.Popen({grandchild!r});"
                 "time.sleep(120)"],
                start_new_session=(sys.platform != "win32"),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                mutate._terminate_process_tree(process)
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=60)
            self.assertIsNotNone(process.returncode, "gate process was not reaped")
            time.sleep(LONG_CHILD_S + 2)
            self.assertFalse(marker.exists(),
                             "a build child survived the driver's termination")


if __name__ == "__main__":
    unittest.main()
