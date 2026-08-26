# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Committed mutation regressions for the scalar FCR31 semantics slice.

The manual campaign that drove the original fix is encoded here so the kill
evidence stays reproducible instead of living only in a PR description.

Two complementary layers:

* Behavioral mutants (M1..M4, M9..M12): each one patches a COPY of
  ``fp_convert.h`` in a temporary directory (the selftest TU is copied beside
  it so quoted-include resolution picks up the mutant) and requires the
  optimized native selftest to FAIL. Following tools/test_build_truth.py's
  mutation pattern, every replacement asserts its anchor exists first, so a
  refactor that silently orphans a mutant cannot produce a vacuous pass.

* Structural regressions (fast path removal, FCC0 coherence, cvt.s.w
  routing): instant source-shape anchors over tools/codegen.py,
  src/rt/fp_convert.h and src/ref/interp.cpp. These are the committed-form
  counterparts of mutants M5/M6/M7/M8 plus the Disposition-A rule that no
  native fast path may come back without a validated host-invariant design.
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

HEADER = ROOT / "src" / "rt" / "fp_convert.h"
SELFTEST = ROOT / "src" / "rt" / "fp_convert_selftest.c"
CODEGEN = ROOT / "tools" / "codegen.py"
REF_INTERP = ROOT / "src" / "ref" / "interp.cpp"


def _build_mutated_selftest(header_text: str) -> int:
    """Compile+run the real selftest against a mutated header copy at -O2."""
    assert CC is not None
    with tempfile.TemporaryDirectory(prefix="fp_scalar_mut_") as tmp:
        work = Path(tmp)
        (work / "fp_convert.h").write_text(header_text, encoding="utf-8", newline="\n")
        shutil.copyfile(SELFTEST, work / "fp_convert_selftest.c")
        exe = work / "mutant_selftest.exe"
        command = [
            CC, "-O2", "-fno-strict-aliasing",
            "-DSR_SDL3VK", "-D_CRT_SECURE_NO_WARNINGS",
            "-I", str(work),
            str(work / "fp_convert_selftest.c"), "-lm", "-o", str(exe),
        ]
        compiled = subprocess.run(command, capture_output=True, text=True)
        if compiled.returncode != 0:
            return 1  # a mutant that breaks compilation is dead too
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
        return ran.returncode


class FpScalarHeaderMutantTests(unittest.TestCase):
    """Each behavioral mutant must be killed by the committed selftest."""

    def assert_killed(self, old: str, new: str) -> None:
        original = HEADER.read_text(encoding="utf-8")
        self.assertIn(old, original, f"mutation anchor vanished from {HEADER.name}: {old!r}")
        rc = _build_mutated_selftest(original.replace(old, new))
        self.assertNotEqual(
            rc, 0,
            f"mutation SURVIVED the -O2 selftest (anchor={old!r}): "
            "the committed semantic regression did not fire")

    @unittest.skipUnless(CC, "gcc required")
    def test_M1_ignore_rounding_mode(self):
        self.assert_killed(
            "csr |= rm_to_mxcsr[fcr31 & SR_FCR31_RM_MASK] << 13;",
            "csr |= 0u << 13;")

    @unittest.skipUnless(CC, "gcc required")
    def test_M2_force_RN(self):
        self.assert_killed("{0u, 3u, 2u, 1u}", "{0u, 0u, 0u, 0u}")

    @unittest.skipUnless(CC, "gcc required")
    def test_M3_ignore_FS_flush_gate(self):
        self.assert_killed("csr |= 1u << 15;", "(void)fcr31;")

    @unittest.skipUnless(CC, "gcc required")
    def test_M4_force_FTZ_regardless_of_guest(self):
        self.assert_killed("csr &= ~(1u << 15);", "csr |= 1u << 15;")

    @unittest.skipUnless(CC, "gcc required")
    def test_M9_helper_leaks_modified_mxcsr(self):
        self.assert_killed(
            "    volatile float vr = va * vb;\n"
            "    sr_fpu_scalar_barrier();\n"
            "    sr_fpu_env_restore(saved);",
            "    volatile float vr = va * vb;\n"
            "    sr_fpu_scalar_barrier();")

    @unittest.skipUnless(CC, "gcc required")
    def test_M11_preserve_ambient_DAZ(self):
        self.assert_killed(
            "csr &= ~(1u << 6);                              /* DAZ off: inputs stay gradual */",
            "")

    @unittest.skipUnless(CC, "gcc required")
    def test_M12_drop_volatile_result_window(self):
        # The pre-fix mechanism: barrier-bracketed plain expression. Killed by
        # the folding/reorder guards and the hostile matrix at -O2.
        self.assert_killed("volatile float vr = va * vb;", "const float vr = va * vb;")


class FastPathRemovedStructuralTests(unittest.TestCase):
    """Disposition A: no native fast path may exist without a validated host
    invariant, and the scoped helpers must be the only scalar mechanism."""

    def test_no_fast_path_predicate_anywhere(self):
        # Anchor on definition/usage shapes so the disposition comment may
        # still NAME the removed mechanism without tripping the guard.
        header = HEADER.read_text(encoding="utf-8")
        self.assertNotIn(
            "static inline int sr_fpu_scalar_fast", header,
            "native fast-path predicate redefined; see fp_convert.h FAST PATH "
            "DISPOSITION before reintroducing it")
        codegen = CODEGEN.read_text(encoding="utf-8")
        self.assertNotIn(
            "sr_fpu_scalar_fast(", codegen,
            "emitted code consults a native fast path again; a validated "
            "host-invariant design is a prerequisite (RISK-8)")

    def test_codegen_routes_every_scalar_op_through_helpers(self):
        text = CODEGEN.read_text(encoding="utf-8")
        for helper in ("sr_fpu_add_s(_a,_b,s->fcr31)",
                       "sr_fpu_sub_s(_a,_b,s->fcr31)",
                       "sr_fpu_mul_s(_a,_b,s->fcr31)",
                       "sr_fpu_div_s(_a,_b,s->fcr31)"):
            self.assertIn(helper, text, f"scalar op no longer routed through {helper}")
        self.assertIn(
            "sr_fpu_cvt_s_w(sr_u32_as_s32(s->fi[{fs}]), s->fcr31)", text,
            "cvt.s.w must honor guest RM through the pinned helper")
        self.assertNotIn(
            "(float)sr_u32_as_s32(", text,
            "bare host-cast cvt.s.w reintroduced (mutation M7 shape)")

    def test_cond_compare_writes_architectural_fcc0(self):
        text = CODEGEN.read_text(encoding="utf-8")
        self.assertIn(
            "s->fcr31 = (s->fcr31 & ~0x00800000u) | (s->fpcond << 23);", text,
            "c.cond must keep FCC0 architectural in fcr31 (mutation M5 shape)")
        self.assertNotIn(
            "(s->fpcond ^ 1u) << 23", text,
            "inverted FCC0 write reintroduced (mutation M6 shape)")
        self.assertIn(
            "s->fpcond = (s->fcr31 >> 23) & 1u;", text,
            "ctc1 must resync the cached fpcond mirror from written bits")

    def test_reference_model_shares_the_helper_contract(self):
        text = REF_INTERP.read_text(encoding="utf-8")
        self.assertIn("sr_fpu_add_s(", text)
        self.assertIn("sr_fpu_mul_s(", text)
        self.assertIn("sr_fpu_div_s(", text)
        self.assertIn("sr_fpu_cvt_s_w(", text)
        self.assertIn(
            "SHARED_HELPER", text,
            "reference model must disclose shared-helper evidence class")


if __name__ == "__main__":
    unittest.main()
