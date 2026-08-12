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
