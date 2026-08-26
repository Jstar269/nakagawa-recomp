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

import hashlib
import os
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

    @unittest.skipUnless(CC, "gcc required")
    def test_M13_partial_restore_preserves_stickies(self):
        """A helper that restores control fields but leaves sticky flags raised
        inside its window violates bit-for-bit caller restoration.

        Deterministic dedicated harness (ambient-state independent): establish
        a controlled caller state by clearing ONLY the sticky bits, capture
        before, execute a runtime inexact operation through sr_fpu_div_s
        (volatile operands cannot constant-fold), capture after. Production
        helper must restore exactly (after == before); the M13 partial-restore
        mutant must leak a newly-raised sticky bit (after != before).

        Staged evidence only -- MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED with
        an explicit sticky-delta diagnostic counts; build/harness failures do
        not. Both directions are proven: pristine header PASSES the same
        harness, so the discriminator is not vacuous.
        """
        if CC is None:
            self.skipTest("gcc required")

        harness_c = """\
#include <stdio.h>
#include <xmmintrin.h>
#include "fp_convert.h"

int main(void) {
    const uint32_t outer = _mm_getcsr();
    /* TEST-ONLY setup: clear ONLY sticky status bits; every non-sticky field
     * (RC, masks, FTZ, DAZ) is preserved from the outer environment. */
    const uint32_t controlled = outer & ~0x3fu;
    _mm_setcsr(controlled);
    const uint32_t before = _mm_getcsr();

    volatile float one = 1.0f;
    volatile float three = 3.0f;
    float r = sr_fpu_div_s(one, three, 0u);   /* runtime inexact: raises PE */

    const uint32_t after = _mm_getcsr();
    const uint32_t delta = after ^ before;
    printf("before=%08x\\nafter=%08x\\ndelta=%08x\\n", before, after, delta);
    if (sr_float_bits(r) != 0x3eaaaaabu) {
        fprintf(stderr, "RESULT_WRONG got=%08x\\n", sr_float_bits(r));
        _mm_setcsr(outer);
        return 3;
    }
    _mm_setcsr(outer);
    if (after != before) {
        fprintf(stderr, "STICKY_LEAK before=%08x after=%08x delta=%08x\\n",
                before, after, delta);
        return 1;
    }
    printf("restoration exact\\n");
    return 0;
}
"""

        def build_and_run(header_text: str, tag: str):
            with tempfile.TemporaryDirectory(prefix=f"fp_scalar_m13_{tag}_") as tmp:
                work = Path(tmp)
                (work / "fp_convert.h").write_text(header_text, encoding="utf-8", newline="\n")
                (work / "m13_harness.c").write_text(harness_c, encoding="utf-8", newline="\n")
                exe = work / "m13.exe"
                command = [CC, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                           "-I", str(work),
                           str(work / "m13_harness.c"), "-lm", "-o", str(exe)]
                compiled = subprocess.run(command, capture_output=True, text=True)
                if compiled.returncode != 0:
                    return None, compiled.stderr + compiled.stdout
                ran = subprocess.run([str(exe)], capture_output=True, text=True)
                return ran.returncode, ran.stderr + ran.stdout

        original = HEADER.read_text(encoding="utf-8")
        anchor = ("static inline void sr_fpu_env_restore(uint32_t saved) {\n"
                  "    _mm_setcsr(saved);\n"
                  "}")
        mutant_restore = ("static inline void sr_fpu_env_restore(uint32_t saved) {\n"
                          "    _mm_setcsr((saved & ~0x3fu) | (_mm_getcsr() & 0x3fu));\n"
                          "}")
        self.assertIn(anchor, original, "restore anchor drifted")

        # Direction 1 (discriminator validity): PRISTINE header must pass.
        pristine_rc, pristine_out = build_and_run(original, "pristine")
        self.assertEqual(pristine_rc, 0,
                         f"M13 harness rejected the PRODUCTION helper; "
                         f"production restoration defect:\n{pristine_out}")
        self.assertIn("restoration exact", pristine_out)

        # Direction 2: the M13 mutant must build, run, and fail SEMANTICALLY
        # with a newly-leaked sticky bit in the diagnostic delta.
        mutant_rc, mutant_out = build_and_run(
            original.replace(anchor, mutant_restore), "mutant")
        self.assertIsNotNone(
            mutant_rc,
            "M13_MUTANT_BUILD_FAILED: compilation failure is not a "
            "behavioral kill\n" + mutant_out)
        self.assertNotEqual(
            mutant_rc, 0,
            "M13_SURVIVED: partial-restore mutant passed the controlled "
            "sticky-leak harness\n" + mutant_out)
        self.assertIn("STICKY_LEAK", mutant_out,
                      "M13_NON_SEMANTIC_FAILURE: no sticky-leak diagnostic\n"
                      + mutant_out)
        delta_line = next((line for line in mutant_out.splitlines()
                           if line.startswith("delta=")), "")
        self.assertTrue(delta_line, f"M13 diagnostic missing delta=\n{mutant_out}")
        delta = int(delta_line.split("=")[1], 16)
        self.assertTrue(
            delta & 0x3f,
            f"M13_EXPECTED_STICKY_LEAK_OBSERVED failed: delta={delta_line} "
            "carries no sticky bit")
        print(f"M13_EXPECTED_STICKY_LEAK_OBSERVED ({delta_line.strip()})")


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

    def test_mul_inf_zero_classifier_is_raw_bit(self):
        """Structural anchor for the DAZ-precheck repair.

        The emitted mul.s inf*0 classification must read raw register-file
        bits; a floating isinf()/==0.0f precheck is guest-sensitive FP work
        outside the scoped window and misfires under hostile ambient DAZ.
        Anchors are contiguous fragments of codegen.py's emission template.
        """
        text = CODEGEN.read_text(encoding="utf-8")
        self.assertIn(
            'const uint32_t _ab=s->fi[{fs}],_bb=s->fi[{ft}];', text,
            "mul.s must classify inf*0 from raw fi[] bits")
        self.assertIn(
            'f"if((((_ab & 0x7fffffffu) == 0x7f800000u && (_bb & 0x7fffffffu) == 0u)) || "',
            text, "raw-bit inf classification fragment drifted")
        self.assertNotIn("isinf(_a)&&_b==0.0f", text,
                         "floating inf*0 precheck reintroduced into mul.s emission")

    def test_behavioral_mul_daz_precheck_mutant_is_killed(self):
        """Behavioral guard: a cleanly rebuilt PRE-REPAIR classifier (floating
        isinf()/==0.0f comparisons, no leftover raw-bit locals) must compile
        AND run AND fail the hostile-DAZ generated case semantically:
        inf * min-subnormal misclassified as inf * zero -> qNaN.

        The mutation is applied to a TEMP COPY of codegen.py; tracked bytes are
        never touched. Only MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED counts as
        a valid kill; generation or compilation failure is reported and
        rejected as evidence.
        """
        if CC is None:
            self.skipTest("gcc required")
        original = CODEGEN.read_text(encoding="utf-8")
        original_sha = hashlib.sha256(original.encode("utf-8")).hexdigest()

        # Exact contiguous fragments of the current raw-bit emission template.
        frag_decl = 'f"{{ const uint32_t _ab=s->fi[{fs}],_bb=s->fi[{ft}]; "'
        frag_if = ('f"if((((_ab & 0x7fffffffu) == 0x7f800000u '
                   '&& (_bb & 0x7fffffffu) == 0u)) || "')
        frag_sym = ('f"(((_bb & 0x7fffffffu) == 0x7f800000u '
                    '&& (_ab & 0x7fffffffu) == 0u))) "')
        frag_store = 'f"s->fi[{fdv}]=0x7fc00000u; "'
        for frag in (frag_decl, frag_if, frag_sym, frag_store):
            self.assertIn(frag, original, f"raw-bit classifier anchor drifted: {frag!r}")

        # Rebuild the pre-repair emission shape CLEANLY: no _ab/_bb leftovers,
        # floating classifier only. Emitted shape becomes exactly:
        #   { float _a=..., _b=...;
        #     if ((isinf(_a)&&_b==0.0f)||(isinf(_b)&&_a==0.0f)) canonical qNaN;
        #     else sr_fpu_mul_s(...); }
        mutant = (original
                  .replace(frag_decl, 'f"{{ "')
                  .replace(frag_if, "")
                  .replace(frag_sym, "")
                  .replace(frag_store,
                           'f"if((isinf(_a)&&_b==0.0f)||(isinf(_b)&&_a==0.0f)) '
                           's->fi[{fdv}]=0x7fc00000u; "'))
        self.assertNotEqual(mutant, original, "mutant construction produced no change")

        with tempfile.TemporaryDirectory(prefix="fp_scalar_codegen_mut_") as tmp:
            tmp_codegen = Path(tmp) / "mutant_codegen.py"
            tmp_codegen.write_text(mutant, encoding="utf-8", newline="\n")

            # Drive the real fixture pipeline through the mutated COPY.
            import tools.test_codegen_fp_convert as fixture

            # Stage 1: generation must succeed through the mutated codegen.
            gen_words = [
                fixture._ctc1(8),
                fixture._fps3(1, 0, 8, 0x02), fixture._mfc1(9, 8),
                0x03E00008, 0x00000000,
            ]
            elf_bytes = fixture._synthetic_elf(gen_words)
            gen_dir = Path(tmp) / "gen"
            gen_dir.mkdir()
            elf_path = gen_dir / "m.elf"
            elf_path.write_bytes(elf_bytes)
            import os as _os
            env = dict(_os.environ)
            env["HST_EXTRA_SPANS"] = ""
            env["PYTHONPATH"] = str(ROOT / "tools") + _os.pathsep + env.get("PYTHONPATH", "")
            gen_proc = subprocess.run(
                [sys.executable, str(tmp_codegen), str(elf_path),
                 str(gen_dir / "m.c"), "--profile=hst"],
                cwd=ROOT, env=env, capture_output=True, text=True)
            self.assertEqual(
                gen_proc.returncode, 0,
                "MUTANT_GENERATION_FAILED: mutated codegen.py could not run")

            # Stage 2: generated C must COMPILE under normal test flags.
            chunks = sorted(gen_dir.glob("m_*.c"))
            (gen_dir / "recomp.h").write_text(fixture.ISOLATED_RECOMP_H, encoding="ascii")
            harness = (f'#include "m_funcs.h"\n'
                       + fixture.ISOLATED_STUBS_C + fixture._CELL_INFZERO_MAIN)
            (gen_dir / "harness.c").write_text(harness, encoding="ascii")
            exe = gen_dir / "mutant_test.exe"
            command = [CC, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                       "-I", str(gen_dir), "-I", str(ROOT / "src" / "rt")]
            if os.name != "nt":
                command.extend(["-fsanitize=undefined,float-cast-overflow",
                                "-fno-sanitize-recover=all"])
            command.extend([str(gen_dir / "harness.c"), str(gen_dir / "m.c"),
                            *(str(p) for p in chunks), "-lm", "-o", str(exe)])
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(
                compiled.returncode, 0,
                "MUTANT_BUILD_FAILED: mutant must compile cleanly to prove a "
                "SEMANTIC kill; build output:\n" + (compiled.stderr + compiled.stdout)[-1500:])

            # Stage 3: executable must RUN and fail SEMANTICALLY.
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertNotEqual(
                ran.returncode, 0,
                "MUTANT_SURVIVED: FP-comparison classifier passed the "
                "hostile-DAZ fixture")
            combined = ran.stderr + ran.stdout
            self.assertIn(
                "+inf * +minsub", combined,
                "MUTANT_NON_SEMANTIC_FAILURE: fixture failed without reaching "
                "the inf*min-subnormal classification row\n" + combined[-800:])
            self.assertIn(
                "got=7fc00000", combined,
                "MUTANT_NON_SEMANTIC_FAILURE: misclassification did not take "
                "the canonical-qNaN path\n" + combined[-800:])
            # Explicit verdict marker for audit trails.
            print("MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED (valid behavioral kill)")

        # Hygiene guard: tracked codegen.py untouched for the whole test.
        final_sha = hashlib.sha256(CODEGEN.read_text(encoding="utf-8").encode("utf-8")).hexdigest()
        self.assertEqual(final_sha, original_sha,
                         "tracked tools/codegen.py bytes changed by mutation test")

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
