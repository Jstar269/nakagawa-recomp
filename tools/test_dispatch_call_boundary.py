# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Mutation proof for the production interpreter CALL/RETURN boundary.

Each mutant is applied only to a temporary copy of the production dispatch or
interpreter source.  The source-owned dispatch-isolation selftest must compile
cleanly and then fail on the semantic assertion; a compiler failure is reported
as a test failure rather than accepted as a kill.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
CC = shutil.which("gcc")
SELFTEST = ROOT / "src" / "rt" / "dispatch_isolation_selftest.c"
RECOMP = ROOT / "src" / "rt" / "recomp.c"
GUEST_INTERP = ROOT / "src" / "rt" / "guest_interp.c"
TITLE_CONFIG_TOOL = ROOT / "tools" / "title_runtime_config.py"


def _build_and_run(mutated_recomp: str | None = None,
                   mutated_interp: str | None = None) -> tuple[int, str, int, str]:
    """Build and run the real selftest against temporary source copies."""
    assert CC is not None
    with tempfile.TemporaryDirectory(prefix="dispatch_call_boundary_mut_") as tmp:
        work = Path(tmp)
        (work / "dispatch_isolation_selftest.c").write_text(
            SELFTEST.read_text(encoding="utf-8"), encoding="utf-8", newline="\n"
        )
        (work / "recomp.c").write_text(
            mutated_recomp if mutated_recomp is not None
            else RECOMP.read_text(encoding="utf-8"),
            encoding="utf-8", newline="\n",
        )
        interp_path = work / "guest_interp.c"
        interp_path.write_text(
            mutated_interp if mutated_interp is not None
            else GUEST_INTERP.read_text(encoding="utf-8"),
            encoding="utf-8", newline="\n",
        )
        config = subprocess.run(
            [sys.executable, str(TITLE_CONFIG_TOOL), "--output", str(work / "sr_title_config.h")],
            cwd=ROOT, capture_output=True, text=True,
        )
        if config.returncode != 0:
            return config.returncode, config.stderr + config.stdout, 1, ""

        exe = work / "dispatch_isolation_selftest.exe"
        compile_result = subprocess.run(
            [
                CC, "-std=c11", "-O0", "-fno-strict-aliasing",
                "-Wall", "-Wextra", "-DSR_SDL3VK", "-D_CRT_SECURE_NO_WARNINGS",
                "-I", str(work), "-I", str(ROOT / "src" / "rt"),
                str(work / "dispatch_isolation_selftest.c"), str(interp_path),
                str(ROOT / "src" / "rt" / "title_config.c"),
                str(ROOT / "src" / "rt" / "vfpu_tables.c"),
                "-lm", "-o", str(exe),
            ],
            cwd=ROOT, capture_output=True, text=True,
        )
        if compile_result.returncode != 0:
            return compile_result.returncode, compile_result.stderr + compile_result.stdout, 1, ""
        run_result = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True)
        return 0, compile_result.stderr + compile_result.stdout, run_result.returncode, run_result.stderr + run_result.stdout


@unittest.skipUnless(CC, "gcc is required for the compiled mutation proof")
class DispatchCallBoundaryMutationTests(unittest.TestCase):
    """The CALL contract must be load-bearing, not just source decoration."""

    def assert_killed(self, name: str, *, recomp_old: str | None = None,
                      recomp_new: str | None = None,
                      interp_old: str | None = None,
                      interp_new: str | None = None,
                      diagnostic: str) -> None:
        original_recomp = RECOMP.read_text(encoding="utf-8")
        original_interp = GUEST_INTERP.read_text(encoding="utf-8")
        if recomp_old is not None:
            self.assertIsNotNone(recomp_new)
            self.assertIn(recomp_old, original_recomp, f"{name}: recomp mutation anchor drifted")
            mutated_recomp = original_recomp.replace(recomp_old, recomp_new, 1)
        else:
            mutated_recomp = None
        if interp_old is not None:
            self.assertIsNotNone(interp_new)
            self.assertIn(interp_old, original_interp, f"{name}: interpreter mutation anchor drifted")
            mutated_interp = original_interp.replace(interp_old, interp_new, 1)
        else:
            mutated_interp = None

        compile_rc, compile_output, run_rc, run_output = _build_and_run(
            mutated_recomp=mutated_recomp, mutated_interp=mutated_interp
        )
        self.assertEqual(
            compile_rc, 0,
            f"{name}: MUTANT_BUILD_FAILED (not a semantic kill)\n{compile_output}",
        )
        self.assertNotEqual(
            run_rc, 0,
            f"{name}: MUTANT_SURVIVED the production selftest\n{run_output}",
        )
        self.assertIn(
            diagnostic, run_output,
            f"{name}: failure did not identify the intended boundary semantic\n{run_output}",
        )
        print(f"{name}: MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED")

    def test_pristine_production_selftest_passes(self):
        compile_rc, compile_output, run_rc, run_output = _build_and_run()
        self.assertEqual(compile_rc, 0, compile_output)
        self.assertEqual(run_rc, 0, run_output)
        self.assertIn("dispatch-isolation-selftest: OK", run_output)

    def test_M1_untyped_interpreter_dispatch_reexecutes_native_continuation(self):
        self.assert_killed(
            "M1-untyped-dispatch",
            recomp_old=(
                "SrGuestInterpResult interp_result = call_boundary\n"
                "            ? sr_guest_interp_run_with_boundary(s, target, call_boundary, &fault)\n"
                "            : sr_guest_interp_run(s, target, &fault);"
            ),
            recomp_new="SrGuestInterpResult interp_result = sr_guest_interp_run(s, target, &fault);",
            diagnostic="CALL boundary handed the interpreted callee through the native outer return",
        )

    def test_M2_resume_boundary_one_instruction_early_skips_return(self):
        self.assert_killed(
            "M2-early-resume",
            interp_old="if (boundary && instruction_count != 0u && pc == boundary->resume_pc) {",
            interp_new="if (boundary && instruction_count != 0u && pc == boundary->resume_pc - 4u) {",
            diagnostic="CALL frame/outer return state was not restored exactly",
        )

    def test_M3_resume_boundary_one_instruction_late_executes_continuation(self):
        self.assert_killed(
            "M3-late-resume",
            interp_old="if (boundary && instruction_count != 0u && pc == boundary->resume_pc) {",
            interp_new="if (boundary && instruction_count != 0u && pc == boundary->resume_pc + 4u) {",
            diagnostic="AOT continuation observed wrong caller-saved/store state",
        )

    def test_M4_live_ra_instead_of_explicit_boundary_is_wrong(self):
        self.assert_killed(
            "M4-live-ra-boundary",
            interp_old="if (boundary && instruction_count != 0u && pc == boundary->resume_pc) {",
            interp_new="if (boundary && instruction_count != 0u && pc == s->r[31]) {",
            diagnostic="AOT continuation observed wrong caller-saved/store state",
        )

    def test_M5_return_delay_slot_is_skipped(self):
        self.assert_killed(
            "M5-skip-return-delay",
            interp_old=(
                "SrGuestInterpResult delay_result =\n"
                "                execute_noncontrol(s, pc + 4u, delay_opcode, fault);"
            ),
            interp_new=(
                "SrGuestInterpResult delay_result =\n"
                "                execute_noncontrol(s, pc + 4u, 0x24000000u, fault);"
            ),
            diagnostic="return delay slot did not execute exactly once",
        )

    def test_M6_return_delay_slot_executes_twice(self):
        self.assert_killed(
            "M6-duplicate-return-delay",
            interp_old="            instruction_count += 2u;\n            pc = target;",
            interp_new=(
                "            (void)execute_noncontrol(s, pc + 4u, delay_opcode, fault);\n"
                "            instruction_count += 2u;\n"
                "            pc = target;"
            ),
            diagnostic="return delay slot did not execute exactly once",
        )


if __name__ == "__main__":
    unittest.main()
