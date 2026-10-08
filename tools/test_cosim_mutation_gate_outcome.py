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
import os
import signal
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


# A surviving grandchild must still be alive when the liveness probe gives up, so
# it sleeps well past POST_REAP_DEADLINE_S. Tests kill any survivor they find, so
# this bounds only a failing run.
LONG_CHILD_S = 30
# Ceiling, not a sleep: a reaped descendant is normally gone within milliseconds.
POST_REAP_DEADLINE_S = 5.0
PID_READY_DEADLINE_S = 20.0


def _child(code: str) -> list[str]:
    """One synthetic gate invocation: the same python this test runs on."""
    return [sys.executable, "-c", textwrap.dedent(code)]


def _grandchild(pid_file: Path) -> list[str]:
    """A descendant that records its own PID before it sleeps.

    The PID is written to a staging file and atomically moved into place, so a
    reader never sees a partial value.
    """
    code = (
        "import os,pathlib,time;"
        f"target=pathlib.Path({str(pid_file)!r});"
        "staging=target.with_name(target.name + '.tmp');"
        "staging.write_text(str(os.getpid()));"
        "os.replace(staging, target);"
        f"time.sleep({LONG_CHILD_S})"
    )
    return [sys.executable, "-c", code]


def _gate_spawning(grandchild: list[str], pid_file: Path) -> str:
    """A synthetic gate: reports FAIL, starts the grandchild, then hangs.

    The gate waits until the grandchild has recorded its PID before hanging, so a
    reap always happens with a live descendant rather than before it started. The
    grandchild's stdio is detached so a survivor never holds the gate's pipes open.
    """
    return (
        "import os, subprocess, sys, time\n"
        'print("cosim: FAIL")\n'
        f"subprocess.Popen({grandchild!r}, stdin=subprocess.DEVNULL, "
        "stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)\n"
        f"pid_file = {str(pid_file)!r}\n"
        f"deadline = time.monotonic() + {PID_READY_DEADLINE_S}\n"
        "while not os.path.exists(pid_file) and time.monotonic() < deadline:\n"
        "    time.sleep(0.02)\n"
        "time.sleep(120)\n"
    )


def _poll(predicate, deadline_s: float) -> bool:
    """Return True as soon as predicate() holds, or False at the deadline."""
    end = time.monotonic() + deadline_s
    while True:
        if predicate():
            return True
        if time.monotonic() >= end:
            return False
        time.sleep(0.05)


def _pid_alive(pid: int) -> bool:
    """True while the OS still runs ``pid``. Fails closed: it raises when unsure.

    Windows: OpenProcess(SYNCHRONIZE) on a PID with no process object fails with
    ERROR_INVALID_PARAMETER; a live process's handle does not signal, while an
    exited one does. os.kill(pid, 0) is deliberately not used there, because on
    Windows it calls TerminateProcess.
    POSIX: signal 0 probes existence. A killed child that its parent has not
    reaped is a zombie and no longer runs, so it counts as gone.
    """
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel32.WaitForSingleObject.restype = wintypes.DWORD
        kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        synchronize, wait_timeout, wait_object_0 = 0x00100000, 0x102, 0x0
        error_invalid_parameter = 87
        handle = kernel32.OpenProcess(synchronize, False, pid)
        if not handle:
            error = ctypes.get_last_error()
            if error == error_invalid_parameter:
                return False
            raise OSError(error, f"OpenProcess({pid}) failed")
        try:
            state = kernel32.WaitForSingleObject(handle, 0)
        finally:
            kernel32.CloseHandle(handle)
        if state == wait_timeout:
            return True
        if state == wait_object_0:
            return False
        raise OSError(f"WaitForSingleObject({pid}) returned {state:#x}")

    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    try:
        with open(f"/proc/{pid}/stat", encoding="utf-8") as stat:
            state = stat.read().rsplit(")", 1)[1].split()[0]
    except OSError:
        return True
    return state != "Z"


def _reap_survivor(pid: int) -> None:
    """Best-effort kill of a grandchild that a failing test found still alive."""
    try:
        if os.name == "nt":
            subprocess.run(["taskkill", "/PID", str(pid), "/F"],
                           capture_output=True, check=False, timeout=10)
        else:
            os.kill(pid, signal.SIGKILL)
    except (OSError, subprocess.TimeoutExpired):
        pass


def _assert_no_survivor(test: unittest.TestCase, pid_file: Path) -> None:
    """Prove the recorded grandchild is gone, with a bounded deadline.

    A grandchild that never recorded its PID would make "gone" vacuous, so that
    case fails first.
    """
    test.assertTrue(pid_file.exists(),
                    "the grandchild never recorded its PID; the reap check would be vacuous")
    pid = int(pid_file.read_text(encoding="ascii"))
    gone = _poll(lambda: not _pid_alive(pid), POST_REAP_DEADLINE_S)
    if not gone:
        _reap_survivor(pid)
    test.assertTrue(gone, f"a build child (pid {pid}) survived the driver's termination")


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
        # The gate's own build children must be gone, not merely abandoned: a
        # surviving build child keeps the CI slot busy after the driver reported.
        pid_file = self.dir / "grandchild.pid"
        verdict, detail = self.run_with(
            _gate_spawning(_grandchild(pid_file), pid_file),
            timeout_s=2,
        )
        self.assertEqual(verdict, "timeout", detail)
        self.assertIn("neither a pass nor a semantic kill", detail)
        _assert_no_survivor(self, pid_file)

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


class SurvivorWindowTests(unittest.TestCase):
    def test_a_surviving_grandchild_outlives_the_liveness_deadline(self):
        # If the grandchild could exit on its own before the probe gives up, a
        # survivor would look reaped. This keeps that impossible.
        self.assertGreater(LONG_CHILD_S, POST_REAP_DEADLINE_S + 10)


class ProcessTreeReapingTests(unittest.TestCase):
    def test_terminator_kills_the_whole_child_tree(self):
        with tempfile.TemporaryDirectory(prefix="cosim_reap_") as tmp:
            pid_file = Path(tmp) / "grandchild.pid"
            process = subprocess.Popen(
                [sys.executable, "-c", _gate_spawning(_grandchild(pid_file), pid_file)],
                start_new_session=(sys.platform != "win32"),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                # Terminate only once the grandchild has started, so a reap that
                # never reached it cannot pass by accident.
                _poll(pid_file.exists, PID_READY_DEADLINE_S)
                mutate._terminate_process_tree(process)
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=60)
            self.assertIsNotNone(process.returncode, "gate process was not reaped")
            _assert_no_survivor(self, pid_file)


if __name__ == "__main__":
    unittest.main()
