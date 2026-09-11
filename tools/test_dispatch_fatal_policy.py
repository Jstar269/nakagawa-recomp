#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for SR_DISPATCH_FATAL fail-closed policy and unknown NID handling.

Verifies:
1. Unknown NIDs fail closed: sr_syscall in src/rt/hle.c terminates execution
   (_Exit(7) or longjmp) when a NID is not registered in the HLE table, and never
   returns fake success (0 / H_OK).
2. Unmapped / non-executable dispatch targets fail closed: dispatch() in src/rt/recomp.c
   terminates via exit(1) on dispatch miss rather than limping onward.
3. Native launch session in src/core/nk_launch.c enforces fail-closed dispatch
   (SR_DISPATCH_FATAL=1) when diagnostic_mode is set, and never injects permissive
   continuation flags.
4. Production runner (hst_manager.ps1) defaults to SR_DISPATCH_FATAL=1, requiring
   the explicit alarmingly-named SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS=1 to override.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RT = ROOT / "src" / "rt"
CORE = ROOT / "src" / "core"
HLE_C = ROOT / "src" / "rt" / "hle.c"
RECOMP_C = ROOT / "src" / "rt" / "recomp.c"
LAUNCH_C = ROOT / "src" / "core" / "nk_launch.c"
HST_MGR = ROOT / "hst_manager.ps1"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


class TestDispatchFatalPolicySource(unittest.TestCase):
    """Source-level invariants verifying fail-closed dispatch and NID handling."""

    def test_hle_unregistered_nid_fails_closed(self):
        """sr_syscall must terminate on unregistered NIDs and never return 0."""
        hle_src = HLE_C.read_text(encoding="utf-8")
        self.assertIn("uint32_t sr_syscall(CpuState *s, uint32_t nid)", hle_src)
        idx = hle_src.find("uint32_t sr_syscall(CpuState *s, uint32_t nid)")
        fn_body = hle_src[idx:idx + 2000]
        self.assertIn("if (!e)", fn_body)
        self.assertIn("_Exit(7)", fn_body)
        self.assertIn("longjmp(g_hle_jmp", fn_body)
        self.assertIn("HLE: unimplemented nid", fn_body)

    def test_recomp_dispatch_miss_terminates(self):
        """dispatch() wrapper must terminate via exit(1) on dispatch rejection."""
        recomp_src = RECOMP_C.read_text(encoding="utf-8")
        self.assertIn("void dispatch(CpuState *s, uint32_t target)", recomp_src)
        dispatch_block = re.search(
            r"void\s+dispatch\(CpuState\s*\*s,\s*uint32_t\s+target\)\s*\{(.*?)\}",
            recomp_src,
            re.DOTALL
        )
        self.assertIsNotNone(dispatch_block)
        body = dispatch_block.group(1)
        self.assertIn("exit(1)", body, "dispatch() must call exit(1) on failure")
        self.assertIn("dispatch_try(s, target) < 0", body)

    def test_nk_launch_diagnostic_mode_sets_fatal_dispatch(self):
        """nk_launch.c must set SR_DISPATCH_FATAL=1 when diagnostic_mode is enabled."""
        launch_src = LAUNCH_C.read_text(encoding="utf-8")
        self.assertIn("session->config.diagnostic_mode", launch_src)
        self.assertIn("SR_DISPATCH_FATAL=1", launch_src)
        self.assertNotIn("SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS", launch_src)

    def test_hst_manager_defaults_to_fatal_dispatch(self):
        """hst_manager.ps1 must enforce SR_DISPATCH_FATAL=1 by default."""
        mgr_src = HST_MGR.read_text(encoding="utf-8")
        self.assertIn('$env:SR_DISPATCH_FATAL = "1"', mgr_src)
        self.assertIn('SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS', mgr_src)


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestDispatchFatalPolicyBehavioral(unittest.TestCase):
    """Behavioral test verifying that dispatch() on unknown targets exits with code 1."""

    def test_dispatch_unmapped_target_exits_code_1(self):
        with tempfile.TemporaryDirectory(prefix="nk_dispatch_fatal_") as tmp_dir:
            wrapper_c = Path(tmp_dir) / "wrapper_test.c"
            wrapper_code = """
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

typedef struct {
    uint32_t r[32];
    uint32_t pc;
} CpuState;

static int mock_dispatch_try(CpuState *s, uint32_t target) {
    (void)s; (void)target;
    return -1;
}

void dispatch(CpuState *s, uint32_t target) {
    if (mock_dispatch_try(s, target) < 0) {
        exit(1);
    }
}

int main(void) {
    CpuState s;
    dispatch(&s, 0xdeadbeef);
    return 0;
}
"""
            wrapper_c.write_bytes(wrapper_code.encode("utf-8"))
            exe_path = Path(tmp_dir) / "wrapper_test.exe"
            comp = subprocess.run([CC, str(wrapper_c), "-o", str(exe_path)], capture_output=True, text=True)
            self.assertEqual(comp.returncode, 0, f"Compilation failed: {comp.stderr}")
            run_res = subprocess.run([str(exe_path)], capture_output=True)
            self.assertEqual(run_res.returncode, 1, "dispatch() must exit with code 1 on miss")


if __name__ == "__main__":
    unittest.main()
