# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Invariant tests for the public VFPU synthetic corpus and coverage report.

These tests are cross-platform and require no game ELF:

 * Synthetic corpus is deterministic and non-empty.
 * Synthetic corpus contains no duplicate words.
 * Self-comparison invariant in vfpu_coverage_report: no category in the
   coverage matrix has has_differential_test=True and has_static_emitter=False
   (which would mean comparing interpreter vs interpreter).
 * vfpu_words.txt is NOT present in the committed source tree (game-derived,
   must be git-ignored).
"""

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))


class SyntheticCorpusTests(unittest.TestCase):
    def setUp(self):
        from vfpu_synth_gen import generate_synthetic_corpus
        self._corpus = generate_synthetic_corpus()

    # ------------------------------------------------------------------
    # Decode-shape invariants.  The historical iterators packed fields per
    # a generic "Allegrex layout" guess, so every corpus word decoded as a
    # different op than the iterator claimed (e.g. all "0x19 unary" words
    # decoded as vmuls and 0x1B/0x34/0x3C words fell into Unsupported or
    # skewed sub-ops), which silently removed vdot/vhdp/vcrs/vscl/vmmul/
    # vtfm/vmscl/vmmov/vqmul/vcrsp from public differential fuzz.  These
    # tests pin every word to the REAL decoder (codegen.vfpu_effect): each
    # must emit a static body with no interpreter fallback, and the matrix/
    # vector families must actually be present.
    # ------------------------------------------------------------------

    def test_every_word_decodes_without_fallback(self) -> None:
        """No corpus word may raise Unsupported (skipped by the fuzzer) or
        fall back to sr_vfpu_interp (self-compare, excluded by the fuzzer).
        Either would silently remove the word from differential coverage."""
        import codegen
        bad: list[str] = []
        for w in self._corpus:
            try:
                body, _, _ = codegen.vfpu_effect(0x08900000, w)
            except codegen.Unsupported as e:
                bad.append(f"0x{w:08x} unsupported: {e}")
                continue
            if "sr_vfpu_interp" in body:
                bad.append(f"0x{w:08x} falls back to sr_vfpu_interp")
        self.assertEqual(
            bad, [],
            "corpus words must decode through the static emitter:\n"
            + "\n".join(bad[:20]),
        )

    def test_matrix_and_vector_ops_are_present(self) -> None:
        """The corpus must contain words the runtime decoder classifies as each
        of the matrix/vector ops the fuzzer is meant to cover (issue: synthetic
        corpus scramble).  Fields per codegen.vfpu_effect decode: sub = bits
        25:23, VFPUMatrix1 idx = bits 25:21 = 28, which = bits 20:16."""
        def classify(w: int) -> str | None:
            op = w >> 26
            sub = (w >> 23) & 7
            if op == 0x3C:
                if sub == 0:
                    return "vmmul"
                if sub in (1, 2, 3):
                    return "vtfm"
                if sub == 4:
                    return "vmscl"
                if sub == 5:
                    return "vqmul/vcrsp"
                if sub == 7 and ((w >> 21) & 0x1F) == 28:
                    return "vmmov/vmscl-alias/vmidt/vmzero/vmone"
                return None
            if op == 0x19:
                return {0: "vmuls", 1: "vdot", 2: "vscl", 4: "vhdp", 5: "vcrs"}.get(sub)
            if op == 0x1B:
                return {0: "vcmp", 2: "vmin", 3: "vmax"}.get(sub)
            if op == 0x34:
                jump = (w >> 21) & 0x1F
                return "vv2op/trans" if jump == 0 else f"vfp4-jump{jump}"
            return None

        present = {name for w in self._corpus if (name := classify(w))}
        required = {
            "vmmul", "vtfm", "vmscl", "vmmov/vmscl-alias/vmidt/vmzero/vmone",
            "vdot", "vhdp", "vcrs", "vscl", "vqmul/vcrsp", "vcmp", "vmin", "vmax",
            "vv2op/trans",
        }
        missing = required - present
        self.assertEqual(
            missing, set(),
            "corpus must cover every matrix/vector/compare family:"
            f" missing {sorted(missing)}",
        )

    def test_corpus_is_nonempty(self):
        self.assertGreater(
            len(self._corpus), 0,
            "Synthetic VFPU corpus must not be empty",
        )

    def test_corpus_has_no_duplicates(self):
        self.assertEqual(
            len(self._corpus),
            len(set(self._corpus)),
            "Synthetic VFPU corpus must not contain duplicate instruction words",
        )

    def test_corpus_is_deterministic(self):
        from vfpu_synth_gen import generate_synthetic_corpus
        second_run = generate_synthetic_corpus()
        self.assertEqual(
            self._corpus,
            second_run,
            "Synthetic corpus must be deterministic across two calls in the same process",
        )

    def test_corpus_words_are_32bit(self):
        for w in self._corpus:
            self.assertGreaterEqual(w, 0)
            self.assertLessEqual(w, 0xFFFFFFFF, f"Word 0x{w:x} exceeds 32 bits")

    def test_corpus_covers_multiple_opcode_families(self):
        """The corpus must cover at least 4 of the 6 target VFPU opcode families
        (0x18, 0x19, 0x1B, 0x34, 0x37, 0x3C)."""
        target_families = {0x18, 0x19, 0x1B, 0x34, 0x37, 0x3C}
        covered = set()
        for w in self._corpus:
            covered.add((w >> 26) & 0x3F)
        # Some families may not produce any surviving words after dedup; require >= 4.
        covered_targets = covered & target_families
        self.assertGreaterEqual(
            len(covered_targets),
            4,
            f"Corpus covers only {covered_targets} of target families {target_families}",
        )


class CoverageReportSelfCompareInvariantTests(unittest.TestCase):
    def test_no_diff_test_without_emitter(self):
        """No coverage matrix entry should have has_differential_test=True and
        has_static_emitter=False.  Such an entry would compare sr_vfpu_interp
        against itself, providing no independent verification signal."""
        from vfpu_coverage_report import check_no_self_compare
        violations = check_no_self_compare()
        self.assertEqual(
            violations,
            [],
            "Self-compare violations found in VFPU coverage matrix:\n"
            + "\n".join(violations),
        )


class VfpuWordsTxtAbsenceTest(unittest.TestCase):
    def test_vfpu_words_txt_is_not_committed(self):
        """tools/vfpu_words.txt contains words extracted from the private game ELF
        and must not appear in the committed source tree.  If this file is present
        it must be listed in .gitignore (which is tested separately by test_publish_audit).
        This test fails if the file exists AND is not listed in .gitignore, indicating
        it might accidentally end up in a commit.

        NOTE: This test only verifies that the gitignore entry for vfpu_words.txt
        exists; actual enforcement requires git-level check at commit/push time.
        """
        gitignore = (ROOT / ".gitignore").read_text(encoding="utf-8")
        self.assertIn(
            "vfpu_words.txt",
            gitignore,
            "tools/vfpu_words.txt (game-derived corpus) must be listed in .gitignore",
        )


if __name__ == "__main__":
    unittest.main()
