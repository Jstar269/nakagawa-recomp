# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The record-gap inventory must agree with the gate that produces the finding.

``tools/provenance_record_gap.py`` exists so a maintainer can admit every
unrecorded path in one batch instead of one pull request at a time.  If it
disagreed with ``_exact_record_finding_for_change`` -- the function that decides
whether a change fails with ``TRUSTED_PATH_MISSING`` -- the batch would clear
some paths and not others, and the inventory would be worse than useless.

So the inventory is tested against that function directly, and against the
committed ledger, rather than against a list frozen into the test.
"""

from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import provenance_attest_verify as verifier  # noqa: E402
import provenance_ledger  # noqa: E402
import provenance_record_gap as gap  # noqa: E402

LEDGER = ROOT / "assets" / "public_provenance_ledger.json"


class TestInventoryShape(unittest.TestCase):
    def test_the_ledger_is_the_source_of_the_default_answer(self) -> None:
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        self.assertTrue(covered, "the committed public ledger names records")
        document = json.loads(LEDGER.read_text(encoding="utf-8"))
        for entry in document["entries"]:
            classification, record_id = verifier._claim(entry)
            listed = entry["path"] in covered
            expected = classification in verifier.IMPLEMENTATION_CLASSES and bool(record_id)
            self.assertEqual(listed, expected, entry["path"])

    def test_the_inventory_is_deterministic_and_sorted(self) -> None:
        first = gap.record_gaps()
        second = gap.record_gaps()
        self.assertEqual(first, second)
        paths = [item["path"] for item in first]
        self.assertEqual(paths, sorted(paths))
        self.assertEqual(len(paths), len(set(paths)))

    def test_a_covered_path_is_never_reported(self) -> None:
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        reported = {item["path"] for item in gap.record_gaps()}
        self.assertEqual(reported & covered, set())

    def test_documentation_and_configuration_that_need_no_record_are_not_reported(self) -> None:
        """A path the gate never asks about must not become a maintainer chore."""
        reported = {item["path"] for item in gap.record_gaps()}
        for path in ("README.md", "LICENSE", "docs/CI.md", "docs/SETUP.md"):
            if path in reported:
                self.assertTrue(
                    provenance_ledger._admission_requires_implementation(path),
                    f"{path} is reported but the gate would never ask for a record",
                )

    def test_every_uncovered_path_the_gate_asks_about_is_reported(self) -> None:
        """The complement: nothing the gate would ask about is left out."""
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        reported = {item["path"] for item in gap.record_gaps(covered=covered)}
        policy = gap.load_policy(ROOT / gap.POLICY_PATH)
        expected = {
            path for path in gap.tracked_paths(ROOT)
            if policy.resolve(path).disposition == "included"
            and provenance_ledger._admission_requires_implementation(path)
            and path not in covered
        }
        self.assertEqual(reported, expected)
        self.assertTrue(expected, "the batch is empty, which would mean the gate stopped asking")


class TestAgreementWithTheGate(unittest.TestCase):
    """The decisive property: the inventory predicts the gate exactly."""

    def test_every_reported_path_would_fail_with_a_missing_record(self) -> None:
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        for item in gap.record_gaps(covered=covered):
            path = item["path"]
            self.assertTrue(
                provenance_ledger._admission_requires_implementation(path),
                f"{path} is in the inventory but needs no record",
            )
            finding = verifier._exact_record_finding_for_change(
                path,
                base_blobs={},
                candidate_blobs={path: b"changed"},
                exact_records={},
            )
            self.assertEqual(
                finding, "TRUSTED_PATH_MISSING",
                f"{path} is in the inventory but the gate would not report it",
            )

    def test_a_path_the_gate_accepts_is_not_in_the_inventory(self) -> None:
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        reported = {item["path"] for item in gap.record_gaps(covered=covered)}
        sample = sorted(covered)[:: max(1, len(covered) // 20)][:20]
        for path in sample:
            if provenance_ledger._admission_requires_implementation(path):
                self.assertNotIn(path, reported)

    def test_the_gate_would_not_report_a_deterministic_documentation_path(self) -> None:
        finding = verifier._exact_record_finding_for_change(
            "docs/CI.md",
            base_blobs={"docs/CI.md": b"old"},
            candidate_blobs={"docs/CI.md": b"new"},
            exact_records={},
        )
        self.assertIsNone(finding)


class TestOutput(unittest.TestCase):
    def test_check_mode_fails_closed_while_gaps_exist(self) -> None:
        self.assertEqual(gap.main(["--repo", str(ROOT), "--check"]), 1)

    def test_the_inventory_mode_is_not_a_gate(self) -> None:
        self.assertEqual(gap.main(["--repo", str(ROOT)]), 0)

    def test_json_output_names_every_gap(self) -> None:
        import io
        from contextlib import redirect_stdout

        buffer = io.StringIO()
        with redirect_stdout(buffer):
            gap.main(["--repo", str(ROOT), "--json"])
        document = json.loads(buffer.getvalue())
        self.assertEqual(document["gap_count"], len(document["paths"]))
        self.assertEqual(
            [item["path"] for item in document["paths"]],
            [item["path"] for item in gap.record_gaps()],
        )


if __name__ == "__main__":
    unittest.main()
