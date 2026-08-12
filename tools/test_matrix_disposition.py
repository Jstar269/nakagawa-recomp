# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Guard the evidence-matrix `disposition` field against becoming uninformative.

`disposition` once restated `source_shape_classification`: every classified case
was emitted as "UNKNOWN". That inverted the intended reading, because the
"UNKNOWN" set was exactly the set of cases that had been categorised. These
tests keep the field independent and keep the deletion boundary conservative.
"""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import test_matrix_report  # noqa: E402

class DispositionMappingTests(unittest.TestCase):
    def test_every_shape_category_has_a_disposition(self) -> None:
        expected = {
            "NOT_APPLICABLE",
            "A_LEGITIMATE_STRUCTURAL_INVARIANT",
            "B_HISTORICAL_BEHAVIORAL_PROXY",
            "C_REDUNDANT_BEHAVIORAL_PROXY",
            "D_ONLY_AVAILABLE_EVIDENCE",
            "E_OBSOLETE_INVARIANT",
        }
        self.assertEqual(set(test_matrix_report._DISPOSITION_BY_SHAPE), expected)

    def test_no_category_maps_to_unknown(self) -> None:
        self.assertNotIn("UNKNOWN", set(test_matrix_report._DISPOSITION_BY_SHAPE.values()))

    def test_only_the_deletion_boundary_yields_delete_candidate(self) -> None:
        mapping = test_matrix_report._DISPOSITION_BY_SHAPE
        deletable = {shape for shape, verdict in mapping.items() if verdict == "DELETE_CANDIDATE"}
        self.assertEqual(deletable, {"C_REDUNDANT_BEHAVIORAL_PROXY", "E_OBSOLETE_INVARIANT"})

    def test_reviewed_structural_invariants_are_kept(self) -> None:
        mapping = test_matrix_report._DISPOSITION_BY_SHAPE
        self.assertEqual(mapping["A_LEGITIMATE_STRUCTURAL_INVARIANT"], "KEEP")
        self.assertEqual(mapping["NOT_APPLICABLE"], "KEEP")


class SyntheticMatrixTests(unittest.TestCase):
    """Exercise the mapping without requiring excluded historical matrix data."""

    def test_every_synthetic_shape_maps_to_its_disposition(self) -> None:
        mapping = test_matrix_report._DISPOSITION_BY_SHAPE
        cases = [
            {"id": shape, "source_shape_classification": shape, "disposition": verdict}
            for shape, verdict in mapping.items()
        ]
        for case in cases:
            self.assertEqual(case["disposition"], mapping[case["source_shape_classification"]])

    def test_synthetic_matrix_proposes_no_deletions(self) -> None:
        mapping = test_matrix_report._DISPOSITION_BY_SHAPE
        deletions = [shape for shape, verdict in mapping.items() if verdict == "DELETE_CANDIDATE"]
        self.assertEqual(
            deletions,
            ["C_REDUNDANT_BEHAVIORAL_PROXY", "E_OBSOLETE_INVARIANT"],
            "a delete verdict requires manual review, not generation",
        )


if __name__ == "__main__":
    unittest.main()
