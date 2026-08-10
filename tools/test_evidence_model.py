# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import evidence_model as evidence


COMMIT = "1" * 40
BINARY = "2" * 64
PROFILE = "3" * 64
INPUT = "4" * 64


def identity(**changes: str | None) -> evidence.EvidenceIdentity:
    values: dict[str, str | None] = {
        "source_commit": COMMIT,
        "binary_sha256": BINARY,
        "profile_sha256": PROFILE,
        "input_manifest_sha256": INPUT,
        "generated_at": "2026-07-31T10:00:00Z",
    }
    values.update(changes)
    return evidence.EvidenceIdentity(**values)  # type: ignore[arg-type]


class EvidenceModelTests(unittest.TestCase):
    def test_grade_ladder_is_fail_closed(self) -> None:
        expected = identity()
        self.assertEqual(evidence.grade(evidence.EvidenceRecord("unknown"), expected), evidence.EvidenceGrade.UNKNOWN)
        self.assertEqual(
            evidence.grade(evidence.EvidenceRecord("proxy", heuristic=True), expected),
            evidence.EvidenceGrade.HEURISTIC,
        )
        self.assertEqual(
            evidence.grade(evidence.EvidenceRecord("ran", executed=True), expected),
            evidence.EvidenceGrade.EXECUTED,
        )
        bound = evidence.EvidenceRecord("bound", executed=True, identity=expected)
        self.assertEqual(evidence.grade(bound, expected), evidence.EvidenceGrade.FRESHNESS_BOUND)
        validated = replace(bound, claim="pixels validated", content_validated=True)
        self.assertEqual(evidence.grade(validated, expected), evidence.EvidenceGrade.CONTENT_VALIDATED)

    def test_stale_identity_is_stale_and_missing_expectation_is_unknown(self) -> None:
        expected = identity()
        stale = evidence.EvidenceRecord(
            "stale",
            executed=True,
            content_validated=True,
            identity=identity(source_commit="a" * 40),
        )
        self.assertEqual(evidence.grade(stale, expected), evidence.EvidenceGrade.STALE)
        self.assertEqual(evidence.grade(replace(stale, identity=expected), None), evidence.EvidenceGrade.UNKNOWN)

    def test_identity_matches_all_exact_fields_including_timestamp(self) -> None:
        expected = identity()
        later = identity(generated_at="2026-07-31T10:01:00Z")
        self.assertFalse(later.matches(expected))
        self.assertFalse(identity(profile_sha256="f" * 64).matches(expected))
        self.assertFalse(identity(input_manifest_sha256=None).matches(expected))

    def test_completion_requires_explicit_grade(self) -> None:
        expected = identity()
        executed = evidence.EvidenceRecord("ran", executed=True)
        bound = evidence.EvidenceRecord("bound", executed=True, identity=expected)
        validated = evidence.EvidenceRecord("validated", executed=True, identity=expected, content_validated=True)
        self.assertFalse(evidence.satisfies(bound, expected))
        self.assertTrue(evidence.satisfies(bound, expected, evidence.EvidenceGrade.FRESHNESS_BOUND))
        self.assertEqual(evidence.completion_units([executed, bound, validated, validated], expected), 1)
        for invalid in (
            evidence.EvidenceGrade.STALE,
            evidence.EvidenceGrade.UNKNOWN,
            evidence.EvidenceGrade.HEURISTIC,
        ):
            with self.subTest(required=invalid):
                with self.assertRaisesRegex(evidence.EvidenceError, "EXECUTED or stronger"):
                    evidence.satisfies(validated, expected, invalid)

    def test_record_invariants_reject_inflated_claims(self) -> None:
        with self.assertRaisesRegex(evidence.EvidenceError, "requires executed"):
            evidence.EvidenceRecord("bad", content_validated=True)
        with self.assertRaisesRegex(evidence.EvidenceError, "identity-bound"):
            evidence.EvidenceRecord("bad", identity=identity())
        with self.assertRaisesRegex(evidence.EvidenceError, "heuristic evidence"):
            evidence.EvidenceRecord("bad", heuristic=True, executed=True)
        with self.assertRaisesRegex(evidence.EvidenceError, "booleans"):
            evidence.EvidenceRecord("bad", executed=1)  # type: ignore[arg-type]

    def test_strict_mapping_parsers(self) -> None:
        record = evidence.EvidenceRecord.from_mapping(
            {
                "claim": "first frame pixels",
                "executed": True,
                "content_validated": True,
                "identity": {
                    "source_commit": COMMIT,
                    "binary_sha256": BINARY,
                    "profile_sha256": PROFILE,
                    "input_manifest_sha256": INPUT,
                    "generated_at": "2026-07-31T10:00:00Z",
                },
            }
        )
        self.assertEqual(evidence.grade(record, identity()), evidence.EvidenceGrade.CONTENT_VALIDATED)
        with self.assertRaisesRegex(evidence.EvidenceError, "unknown field"):
            evidence.EvidenceRecord.from_mapping({"claim": "x", "verified": True})
        with self.assertRaisesRegex(evidence.EvidenceError, "missing required"):
            evidence.EvidenceIdentity.from_mapping({})

    def test_identity_format_validation(self) -> None:
        with self.assertRaisesRegex(evidence.EvidenceError, "source_commit"):
            identity(source_commit="ABC")
        with self.assertRaisesRegex(evidence.EvidenceError, "binary_sha256"):
            identity(binary_sha256="g" * 64)
        with self.assertRaisesRegex(evidence.EvidenceError, "ending in Z"):
            identity(generated_at="2026-07-31T10:00:00+00:00")
        with self.assertRaisesRegex(evidence.EvidenceError, "invalid RFC"):
            identity(generated_at="2026-99-99T10:00:00Z")
        with self.assertRaisesRegex(evidence.EvidenceError, "ending in Z"):
            identity(generated_at="2026-07-31Z")

    def test_milestones_require_causal_order(self) -> None:
        required = ["process-start", "runtime-ready", "first-frame", "route-complete"]
        self.assertTrue(evidence.milestones_in_order(["noise", *required, "cleanup"], required))
        self.assertFalse(
            evidence.milestones_in_order(
                ["process-start", "first-frame", "runtime-ready", "route-complete"],
                required,
            )
        )
        with self.assertRaisesRegex(evidence.EvidenceError, "unique"):
            evidence.milestones_in_order([], ["same", "same"])

    def test_absence_claim_requires_route_and_bound_evidence(self) -> None:
        expected = identity()
        bound = evidence.EvidenceRecord("no dispatch miss", executed=True, identity=expected)
        self.assertFalse(evidence.absence_observed(bound, expected, route_exercised=False))
        self.assertTrue(evidence.absence_observed(bound, expected, route_exercised=True))
        stale = replace(bound, identity=identity(binary_sha256="f" * 64))
        self.assertFalse(evidence.absence_observed(stale, expected, route_exercised=True))
        with self.assertRaisesRegex(evidence.EvidenceError, "must be a boolean"):
            evidence.absence_observed(bound, expected, route_exercised=1)  # type: ignore[arg-type]


if __name__ == "__main__":
    unittest.main()
