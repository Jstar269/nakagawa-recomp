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

    def test_a_path_the_gate_asks_about_is_reported_once_uncovered(self) -> None:
        """Remove one covered implementation path: the inventory must name it.

        The live repository may have no gaps at all once every path is admitted,
        so the gate's question is proved on a constructed gap instead."""
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        policy = gap.load_policy(ROOT / gap.POLICY_PATH)
        asked = sorted(
            path for path in covered
            if policy.resolve(path).disposition == "included"
            and provenance_ledger.admission_requires_implementation(path)
        )
        self.assertTrue(asked, "the gate asks about no covered path, which means it stopped asking")
        victim = asked[0]
        reported = {item["path"] for item in gap.record_gaps(covered=covered - {victim})}
        self.assertIn(victim, reported)

    def test_upstream_production_gap_requires_campaign_disposition(self) -> None:
        """A derived production gap without a roadmap owner fails closed."""
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        victim = "src/core/nk_psp_aes.c"
        self.assertIn(victim, covered)
        item = next(
            item for item in gap.record_gaps(covered=covered - {victim})
            if item["path"] == victim
        )
        self.assertEqual(item["deterministic_class"], "upstream_derived")
        self.assertEqual(item["independence"]["status"], "missing")
        self.assertIn("owner or disposition", item["independence"]["reason"])

    def test_upstream_production_gap_accepts_explicit_campaign_disposition(self) -> None:
        """A group named in the campaign carries its owner and disposition."""
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        victim = "src/rt/sched.c"
        self.assertIn(victim, covered)
        item = next(
            item for item in gap.record_gaps(covered=covered - {victim})
            if item["path"] == victim
        )
        self.assertEqual(item["independence"], {
            "status": "mapped",
            "owner": "G1",
            "disposition": "clean-room-rewrite",
            "source": gap.INDEPENDENCE_PLAN_PATH,
        })

    def test_bounded_campaign_disposition_is_explicit(self) -> None:
        metadata = gap.independence_metadata("src/rt/ge.c")
        self.assertIsNotNone(metadata)
        self.assertEqual(metadata["owner"], "G10")
        self.assertEqual(metadata["disposition"], "bounded-exclusion")


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
    def _check(self, **patches: object) -> tuple[int, str]:
        import io
        from contextlib import redirect_stderr, redirect_stdout
        from unittest import mock

        with mock.patch.multiple(gap, **patches):
            stderr = io.StringIO()
            with redirect_stderr(stderr), redirect_stdout(io.StringIO()):
                rc = gap.main(["--repo", str(ROOT), "--check"])
        return rc, stderr.getvalue()

    def test_check_mode_passes_on_the_repository_with_the_reviewed_baseline(self) -> None:
        rc, err = self._check(KNOWN_DISPOSITION_GAPS=dict(gap.KNOWN_DISPOSITION_GAPS))
        self.assertEqual(rc, 0, err)

    def test_check_mode_passes_when_no_gap_exists(self) -> None:
        rc, _ = self._check(disposition_gaps=lambda **_kwargs: [], KNOWN_DISPOSITION_GAPS={})
        self.assertEqual(rc, 0)

    def test_a_trusted_record_does_not_hide_a_missing_disposition(self) -> None:
        """A covered upstream-derived path with no disposition still fails the gate.

        FAILING_BEFORE: the gate only looked at paths without an exact trusted
        record, and every tracked path has one, so it could never fail.
        """
        victim = "src/core/nk_psp_aes.c"
        self.assertIn(victim, gap.exact_paths_from_public_ledger(LEDGER))
        self.assertIn(victim, gap.KNOWN_DISPOSITION_GAPS)
        baseline = {path: entry for path, entry in gap.KNOWN_DISPOSITION_GAPS.items()
                    if path != victim}
        rc, err = self._check(KNOWN_DISPOSITION_GAPS=baseline)
        self.assertEqual(rc, 1)
        self.assertIn(victim, err)
        self.assertIn("status=missing", err)

    def test_every_baseline_entry_is_a_live_gap(self) -> None:
        live = {str(item["path"]) for item in gap.disposition_gaps(repo=ROOT)}
        self.assertEqual(set(gap.KNOWN_DISPOSITION_GAPS), live)
        gap.validate_baseline(gap.KNOWN_DISPOSITION_GAPS)

    def test_a_mapped_path_is_not_a_disposition_gap(self) -> None:
        live = {str(item["path"]) for item in gap.disposition_gaps(repo=ROOT)}
        self.assertIsNotNone(gap.independence_metadata("src/rt/hle.c"))
        self.assertNotIn("src/rt/hle.c", live)

    def test_check_mode_fails_closed_on_stale_baseline_entry(self) -> None:
        """A baseline entry whose gap is resolved must be removed (stale entries fail)."""
        stale_path = "src/rt/resolved_gap.c"
        rc, err = self._check(
            disposition_gaps=lambda **_kwargs: [],
            KNOWN_DISPOSITION_GAPS={stale_path: {"issue": "#355", "reason": "resolved since"}},
        )
        self.assertEqual(rc, 1)
        self.assertIn(stale_path, err)
        self.assertIn("stale", err)

    def test_baseline_validation_enforces_owner_and_reason(self) -> None:
        """Every baseline entry must carry an issue owner (#N) and a non-empty reason."""
        gap.validate_baseline({"src/a.c": {"issue": "#355", "reason": "unresolved"}})
        for bad in (
            {"src/a.c": {"reason": "missing owner"}},
            {"src/a.c": {"issue": "ISSUES.md", "reason": "not a live issue"}},
            {"src/a.c": {"issue": "#355", "reason": ""}},
            {"src/a.c": "not-a-dict"},
        ):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                gap.validate_baseline(bad)  # type: ignore[arg-type]

    def test_check_records_mode_fails_when_any_unrecorded_gap_remains(self) -> None:
        from unittest import mock

        covered = gap.exact_paths_from_public_ledger(LEDGER)
        policy = gap.load_policy(ROOT / gap.POLICY_PATH)
        victim = min(
            path for path in covered
            if policy.resolve(path).disposition == "included"
            and provenance_ledger.admission_requires_implementation(path)
        )
        with mock.patch.object(gap, "exact_paths_from_public_ledger",
                               return_value=covered - {victim}):
            self.assertEqual(gap.main(["--repo", str(ROOT), "--check-records"]), 1)
        with mock.patch.object(gap, "record_gaps", return_value=[]):
            self.assertEqual(gap.main(["--repo", str(ROOT), "--check-records"]), 0)

    def test_notice_and_format_documents_are_not_production_code(self) -> None:
        """Licences, provenance notes and format docs never enter the ratchet."""
        for path in ("src/rt/atrac3p/LICENSE.LGPLv2.1.txt", "src/rt/atrac3p/PROVENANCE.md",
                     "tools/TRACE_FORMAT.md"):
            with self.subTest(path=path):
                self.assertFalse(gap._is_production_code(path))
                self.assertNotIn(path, gap.KNOWN_DISPOSITION_GAPS)
                self.assertNotIn(path, {str(item["path"]) for item in gap.disposition_gaps(repo=ROOT)})
        self.assertTrue(gap._is_production_code("src/core/nk_psp_aes.c"))

    def test_trusted_classification_wins_over_the_public_projection(self) -> None:
        """A path the authority calls upstream-derived is a gap even if the public ledger disagrees."""
        from unittest import mock

        victim = "src/core/nk_psp_aes.c"
        self.assertIn(victim, gap.KNOWN_DISPOSITION_GAPS)
        live_public = {str(item["path"]) for item in gap.disposition_gaps(repo=ROOT)}
        self.assertIn(victim, live_public)
        # Reclassify the victim away from upstream-derived in a copy of the public
        # ledger view; only the authority's answer may put it back.
        reclassified = {victim: "project_authored"}
        with mock.patch.object(gap, "public_classifications",
                                        return_value=reclassified):
            silenced = {str(item["path"]) for item in gap.disposition_gaps(repo=ROOT)}
            restored = {
                str(item["path"])
                for item in gap.disposition_gaps(
                    repo=ROOT, trusted_classifications={victim: "upstream_derived"})
            }
        self.assertNotIn(victim, silenced)
        self.assertIn(victim, restored)

    def test_check_with_trusted_ledger_passes_the_authority_classification(self) -> None:
        import io
        from contextlib import redirect_stderr, redirect_stdout
        from unittest import mock

        seen: dict[str, object] = {}

        def fake_gaps(**kwargs: object) -> list[dict[str, object]]:
            seen.update(kwargs)
            return []

        with mock.patch.object(gap, "exact_paths_from_trusted_ledger", return_value=set()), \
                mock.patch.object(gap, "classifications_from_trusted_ledger",
                                  return_value={"src/a.c": "upstream_derived"}), \
                mock.patch.object(gap, "record_gaps", return_value=[]), \
                mock.patch.object(gap, "disposition_gaps", side_effect=fake_gaps), \
                mock.patch.object(gap, "KNOWN_DISPOSITION_GAPS", {}), \
                redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
            rc = gap.main(["--repo", str(ROOT), "--check", "--trusted-ledger", str(LEDGER)])
        self.assertEqual(rc, 0)
        self.assertEqual(seen.get("trusted_classifications"), {"src/a.c": "upstream_derived"})

    def test_documented_flag_behaviour_matches_main(self) -> None:
        doc = gap.__doc__ or ""
        self.assertIn("--check-records", doc)
        self.assertIn("disposition ratchet", doc)
        self.assertNotIn("``--check`` fails closed when they do not", doc)
        parser_help = (ROOT / "tools" / "README.md").read_text(encoding="utf-8")
        self.assertIn("disposition", parser_help.split("`provenance_record_gap.py`", 1)[1].splitlines()[0])

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
