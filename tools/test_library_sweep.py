# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regressions for the private, resumable library compatibility sweep."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import library_sweep  # noqa: E402
import nk_cli  # noqa: E402
from test_iso_parity import build_psp_container, create_test_iso_with_executables  # noqa: E402


SOURCE_COMMIT = "a" * 40
SOURCE_FINGERPRINT = "b" * 64


def _write_iso(root: Path, name: str, disc_id: str, title: str) -> Path:
    path = root / name
    create_test_iso_with_executables(
        path,
        build_psp_container(),
        disc_id=disc_id,
        title=title,
    )
    return path


def _bringup_report(*, failure: str = "UNSUPPORTED_IMPORT") -> dict:
    return {
        "failure_class": failure,
        "issue_numbers": [308] if failure != "NONE" else [],
        "stages": {
            "inspect": {"status": "PASS"},
            "prepare_import": {"status": "PASS"},
            "analyze": {"status": "PASS"},
            "codegen": {"status": "PASS"},
            "compile": {"status": "PASS"},
            "build_package": {"status": "PASS"},
            "launch": {"status": "FAIL" if failure != "NONE" else "PASS"},
        },
        "unsupported_imports": [{"library": "sceAudio", "nid_name": "sceAudioInit"}],
        "runtime_imports": [],
        "presentation": {"frame_submissions": 0},
    }


def _sweep_row(stage: str, *, run_status: str = "COMPLETED") -> dict:
    return {
        "source_key": f"private/{stage}.iso",
        "source_file": f"{stage}.iso",
        "disc_id": "UCUS99999",
        "title_name": "PRIVATE TITLE",
        "furthest_stage": stage,
        "boundary_code": "NONE",
        "issue_numbers": [],
        "nid_families": [],
        "run_status": run_status,
    }


class LibrarySweepTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="library_sweep_")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.iso_dir = self.root / "isos"
        self.iso_dir.mkdir()
        self.private_dir = self.root / "private"
        self.public_output = self.root / "public" / "aggregate.json"

    def _run_kwargs(self) -> dict:
        return {
            "source_commit": SOURCE_COMMIT,
            "process_reader": lambda: set(),
            "sleeper": lambda _seconds: None,
            "poll_seconds": 1,
            "max_wait_seconds": 0,
        }

    def test_interrupted_run_resumes_only_unrecorded_titles(self) -> None:
        first = _write_iso(self.iso_dir, "one.iso", "UCUS99991", "Resume Sentinel One")
        _write_iso(self.iso_dir, "two.iso", "UCUS99992", "Resume Sentinel Two")
        first_calls: list[str] = []

        def interrupt_after_first(iso_path, _work, _report, _sidecar, _budget):
            first_calls.append(iso_path.name)
            if len(first_calls) == 2:
                raise KeyboardInterrupt
            return library_sweep.RouteOutcome(_bringup_report())

        with mock.patch.object(library_sweep, "_run_bringup", side_effect=interrupt_after_first):
            with self.assertRaises(KeyboardInterrupt):
                library_sweep.run_sweep(
                    self.iso_dir, self.private_dir, self.public_output, **self._run_kwargs()
                )

        saved = json.loads((self.private_dir / "library-sweep.json").read_text(encoding="utf-8"))
        self.assertEqual(len(saved["rows"]), 1)
        self.assertEqual(saved["rows"][0]["disc_id"], "UCUS99991")
        self.assertEqual(saved["rows"][0]["source_file"], first.name)
        partial_public = json.loads(self.public_output.read_text(encoding="utf-8"))
        self.assertEqual(partial_public["coverage"]["iso_count"], 2)
        self.assertEqual(partial_public["coverage"]["recorded_routes"], 1)
        self.assertEqual(
            partial_public["ratchet"]["furthest_stage_high_water"]["launch"], 1
        )

        resumed_calls: list[str] = []

        def complete(iso_path, _work, _report, _sidecar, _budget):
            resumed_calls.append(iso_path.name)
            return library_sweep.RouteOutcome(_bringup_report())

        with mock.patch.object(library_sweep, "_run_bringup", side_effect=complete):
            result = library_sweep.run_sweep(
                self.iso_dir,
                self.private_dir,
                self.public_output,
                previous_public_output=self.public_output,
                **self._run_kwargs(),
            )
        self.assertEqual(resumed_calls, ["two.iso"])
        self.assertEqual(result["resumed_this_invocation"], 1)
        self.assertEqual(result["ran_this_invocation"], 1)
        self.assertEqual(len(result["rows"]), 2)
        completed_public = json.loads(self.public_output.read_text(encoding="utf-8"))
        self.assertEqual(completed_public["comparison"]["status"], "MATCHED")
        self.assertEqual(completed_public["coverage"]["iso_count"], 2)
        self.assertEqual(completed_public["coverage"]["recorded_routes"], 2)
        self.assertEqual(
            completed_public["ratchet"]["furthest_stage_high_water"]["launch"], 2
        )

    def test_route_enforces_the_per_title_time_budget(self) -> None:
        work_dir = self.private_dir / "work"
        work_dir.mkdir(parents=True)
        iso = _write_iso(self.iso_dir, "timeout.iso", "UCUS99994", "Timeout Sentinel")
        observed_timeouts: list[int] = []

        class TimedOutProcess:
            pid = 9001
            returncode = None

            def communicate(self, timeout=None):
                observed_timeouts.append(timeout)
                raise subprocess.TimeoutExpired("nk_cli bringup", timeout)

        process = TimedOutProcess()
        with mock.patch.object(library_sweep.subprocess, "Popen", return_value=process):
            with mock.patch.object(library_sweep, "_terminate_process_tree") as terminate:
                outcome = library_sweep._run_bringup(
                    iso,
                    work_dir,
                    work_dir / "bringup.json",
                    work_dir / "sweep-imports.json",
                    7,
                )
        self.assertTrue(outcome.timed_out)
        self.assertEqual(observed_timeouts, [7])
        terminate.assert_called_once_with(process)

    def test_public_aggregate_contains_only_aggregated_title_neutral_data(self) -> None:
        _write_iso(
            self.iso_dir,
            "PRIVATE_SOURCE_FILENAME.iso",
            "UCUS99993",
            "PRIVATE_TITLE_SENTINEL",
        )

        def run_with_private_nid(_iso, _work, _report, sidecar, _budget):
            sidecar.parent.mkdir(parents=True, exist_ok=True)
            sidecar.write_text(json.dumps({
                "schema_version": 1,
                "unsupported_imports": [{
                    "library": "sceAudio",
                    "nid": "0x12345678",
                    "nid_name": "sceAudioInit",
                }, {
                    "library": "PRIVATE_TITLE",
                    "nid": "0x87654321",
                    "nid_name": "privateTitleEntry",
                }, {
                    "library": "GAME123",
                    "nid": "0xABCDEF01",
                    "nid_name": "gamePrivateEntry",
                }],
            }), encoding="utf-8")
            return library_sweep.RouteOutcome(_bringup_report())

        with mock.patch.object(library_sweep, "_run_bringup", side_effect=run_with_private_nid):
            library_sweep.run_sweep(
                self.iso_dir, self.private_dir, self.public_output, **self._run_kwargs()
            )

        public_text = self.public_output.read_text(encoding="utf-8")
        for private_value in (
            "PRIVATE_SOURCE_FILENAME",
            "PRIVATE_TITLE_SENTINEL",
            "UCUS99993",
            "0x12345678",
            "PRIVATE_TITLE",
            "GAME123",
            "0x87654321",
            "0xABCDEF01",
        ):
            self.assertNotIn(private_value, public_text)
        aggregate = json.loads(public_text)
        self.assertEqual(aggregate["furthest_stage_histogram"]["launch"], 1)
        self.assertEqual(aggregate["input_responsiveness"], "NOT_MEASURED_BY_HEADLESS_BRINGUP")
        self.assertEqual(aggregate["nid_families_by_title_count"], [
            {"library_family": "sceAudio", "title_count": 1},
        ])
        private = json.loads((self.private_dir / "library-sweep.json").read_text(encoding="utf-8"))
        self.assertEqual(private["rows"][0]["disc_id"], "UCUS99993")
        self.assertEqual(private["rows"][0]["first_missing_nids"][0]["nid"], "0x12345678")

    def test_public_aggregate_starts_a_title_free_ratchet(self) -> None:
        aggregate = library_sweep._public_aggregate(
            [_sweep_row("launch")], SOURCE_COMMIT, SOURCE_FINGERPRINT, 120, 1
        )

        self.assertEqual(aggregate["comparison"], {
            "status": "NO_BASELINE",
            "reason": "no_previous_aggregate",
            "stage_delta": None,
            "completed_routes_delta": None,
        })
        self.assertEqual(
            aggregate["ratchet"]["furthest_stage_high_water"]["launch"], 1
        )
        public_text = json.dumps(aggregate, sort_keys=True)
        for private_value in ("source_key", "source_file", "disc_id", "title_name"):
            self.assertNotIn(private_value, public_text)

    def test_public_aggregate_includes_every_source_backed_blocker(self) -> None:
        blocker_codes = sorted(library_sweep._PUBLIC_BLOCKER_CODES - {"NONE"})
        self.assertGreater(len(blocker_codes), 10)
        rows = [_sweep_row("launch") for _ in blocker_codes]
        for row, blocker_code in zip(rows, blocker_codes, strict=True):
            row["boundary_code"] = blocker_code

        aggregate = library_sweep._public_aggregate(
            rows, SOURCE_COMMIT, SOURCE_FINGERPRINT, 120, len(rows)
        )

        self.assertEqual(
            [item["boundary_code"] for item in aggregate["top_blocker_classes"]],
            blocker_codes,
        )
        self.assertEqual(
            library_sweep._validate_public_aggregate(aggregate), aggregate
        )

    def test_public_aggregate_requires_strict_json_scalar_types(self) -> None:
        valid = library_sweep._public_aggregate(
            [_sweep_row("launch")], SOURCE_COMMIT, SOURCE_FINGERPRINT, 120, 1
        )
        boolean_schema = json.loads(json.dumps(valid))
        boolean_schema["schema_version"] = True
        with self.assertRaisesRegex(ValueError, "unsupported schema"):
            library_sweep._validate_public_aggregate(boolean_schema)

        for field, bad_value in (
            ("status", []),
            ("reason", {}),
        ):
            with self.subTest(field=field):
                malformed = json.loads(json.dumps(valid))
                malformed["comparison"][field] = bad_value
                with self.assertRaisesRegex(ValueError, "comparison status is invalid"):
                    library_sweep._validate_public_aggregate(malformed)

    def test_cli_rejects_malformed_baseline_without_echoing_untrusted_key(self) -> None:
        _write_iso(self.iso_dir, "synthetic.iso", "UCUS99990", "Synthetic Fixture")
        baseline = library_sweep._public_aggregate(
            [_sweep_row("launch")], SOURCE_COMMIT, SOURCE_FINGERPRINT, 120, 1
        )
        baseline["UNTRUSTED_KEY_SENTINEL"] = "untrusted"
        baseline_path = self.root / "baseline.json"
        baseline_path.write_text(json.dumps(baseline), encoding="utf-8")

        completed = subprocess.run(
            [
                sys.executable,
                str(TOOLS / "library_sweep.py"),
                str(self.iso_dir),
                "--private-dir",
                str(self.private_dir),
                "--public-output",
                str(self.public_output),
                "--previous-public-output",
                str(baseline_path),
            ],
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("previous public aggregate is invalid", completed.stderr)
        self.assertIn("unrecognized fields", completed.stderr)
        self.assertNotIn("UNTRUSTED_KEY_SENTINEL", completed.stderr)
        self.assertNotIn("untrusted", completed.stderr)

    def test_previous_aggregate_documentation_disclaims_authentication(self) -> None:
        compatibility = (ROOT / "docs" / "COMPATIBILITY.md").read_text(encoding="utf-8")

        self.assertIn("user-supplied prior progress claim", compatibility)
        self.assertIn("not cryptographically authenticated", compatibility)
        self.assertIn("trusted prior run", compatibility)

    def test_matching_baseline_only_ratchets_high_water_and_reports_delta(self) -> None:
        previous = library_sweep._public_aggregate(
            [_sweep_row("launch")], SOURCE_COMMIT, SOURCE_FINGERPRINT, 120, 2
        )
        # A prior run can have a high-water count above its current stage histogram.
        previous["ratchet"]["furthest_stage_high_water"]["launch"] = 2
        current = library_sweep._public_aggregate(
            [_sweep_row("analyze")],
            SOURCE_COMMIT,
            SOURCE_FINGERPRINT,
            120,
            2,
            previous_aggregate=previous,
        )

        self.assertEqual(current["comparison"]["status"], "MATCHED")
        self.assertEqual(current["comparison"]["reason"], "same_source_and_input_count")
        self.assertEqual(current["comparison"]["stage_delta"]["launch"], -1)
        self.assertEqual(current["comparison"]["stage_delta"]["analyze"], 1)
        self.assertEqual(current["ratchet"]["furthest_stage_high_water"]["launch"], 2)
        self.assertEqual(current["ratchet"]["furthest_stage_high_water"]["analyze"], 1)
        self.assertEqual(current["furthest_stage_histogram"]["launch"], 0)

    def test_mismatched_or_unsanitized_baseline_fails_closed(self) -> None:
        previous = library_sweep._public_aggregate(
            [_sweep_row("launch")], SOURCE_COMMIT, SOURCE_FINGERPRINT, 120, 1
        )
        current = library_sweep._public_aggregate(
            [_sweep_row("analyze"), _sweep_row("analyze")],
            SOURCE_COMMIT,
            SOURCE_FINGERPRINT,
            120,
            2,
            previous_aggregate=previous,
        )
        self.assertEqual(current["comparison"]["status"], "NEW_BASELINE")
        self.assertEqual(current["comparison"]["reason"], "input_count_changed")
        self.assertEqual(current["ratchet"]["furthest_stage_high_water"]["launch"], 0)

        unsanitized = dict(previous)
        unsanitized["title_name"] = "PRIVATE TITLE"
        self.public_output.parent.mkdir(parents=True, exist_ok=True)
        self.public_output.write_text(json.dumps(unsanitized), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "previous public aggregate is invalid"):
            library_sweep._read_previous_public_aggregate(self.public_output)
        with self.assertRaisesRegex(ValueError, "unrecognized fields"):
            library_sweep._public_aggregate(
                [_sweep_row("analyze")],
                SOURCE_COMMIT,
                SOURCE_FINGERPRINT,
                120,
                1,
                previous_aggregate=unsanitized,
            )
        for private_boundary in ("PRIVATE_TITLE", "GAME123"):
            private_value_row = _sweep_row("analyze")
            private_value_row["boundary_code"] = private_boundary
            with self.assertRaisesRegex(ValueError, "boundary_code is not source-backed"):
                library_sweep._public_aggregate(
                    [private_value_row], SOURCE_COMMIT, SOURCE_FINGERPRINT, 120, 1
                )

    def test_imported_aggregate_rejects_impossible_ratchet_and_summaries(self) -> None:
        valid = library_sweep._public_aggregate(
            [_sweep_row("launch")], SOURCE_COMMIT, SOURCE_FINGERPRINT, 120, 2
        )

        high_water = json.loads(json.dumps(valid))
        high_water["ratchet"]["furthest_stage_high_water"]["launch"] = 999
        with self.assertRaisesRegex(ValueError, "high-water"):
            library_sweep._validate_public_aggregate(high_water)

        null_ratchet = json.loads(json.dumps(valid))
        null_ratchet["ratchet"] = None
        with self.assertRaisesRegex(ValueError, "ratchet cannot be null"):
            library_sweep._validate_public_aggregate(null_ratchet)

        coverage = json.loads(json.dumps(valid))
        coverage["coverage"]["recorded_routes"] = 2
        coverage["coverage"]["completed_routes"] = 2
        with self.assertRaisesRegex(ValueError, "stage histogram"):
            library_sweep._validate_public_aggregate(coverage)

        route_counts = json.loads(json.dumps(valid))
        route_counts["coverage"]["completed_routes"] = 0
        with self.assertRaisesRegex(ValueError, "route status counts"):
            library_sweep._validate_public_aggregate(route_counts)

        current_unknown_issue = _sweep_row("launch")
        current_unknown_issue["boundary_code"] = "UNSUPPORTED_IMPORT"
        current_unknown_issue["issue_numbers"] = [999]
        with self.assertRaisesRegex(ValueError, "issue numbers are not source-backed"):
            library_sweep._public_aggregate(
                [current_unknown_issue], SOURCE_COMMIT, SOURCE_FINGERPRINT, 120, 1
            )

        imported_unknown_issue = json.loads(json.dumps(valid))
        imported_unknown_issue["top_blocker_classes"] = [{
            "boundary_code": "UNSUPPORTED_IMPORT",
            "title_count": 1,
            "issue_numbers": [999],
            "tracking": "#999",
        }]
        with self.assertRaisesRegex(ValueError, "issue numbers are invalid"):
            library_sweep._validate_public_aggregate(imported_unknown_issue)

        blocker_rows = [_sweep_row("launch"), _sweep_row("launch")]
        blocker_rows[0]["boundary_code"] = "UNSUPPORTED_IMPORT"
        blocker_rows[1]["boundary_code"] = "ENTRY_NOT_COMPILED"
        blockers = library_sweep._public_aggregate(
            blocker_rows, SOURCE_COMMIT, SOURCE_FINGERPRINT, 120, 2
        )
        blockers["top_blocker_classes"].append(dict(blockers["top_blocker_classes"][0]))
        with self.assertRaisesRegex(ValueError, "blocker count or ordering"):
            library_sweep._validate_public_aggregate(blockers)

        blocker_sum = json.loads(json.dumps(valid))
        blocker_sum["top_blocker_classes"] = [
            {
                "boundary_code": "ENTRY_NOT_COMPILED",
                "title_count": 1,
                "issue_numbers": [],
                "tracking": "no issue: propose one",
            },
            {
                "boundary_code": "UNSUPPORTED_IMPORT",
                "title_count": 1,
                "issue_numbers": [],
                "tracking": "no issue: propose one",
            },
        ]
        with self.assertRaisesRegex(ValueError, "exceed recorded routes"):
            library_sweep._validate_public_aggregate(blocker_sum)

        for private_boundary in ("PRIVATE_TITLE", "GAME123"):
            imported_private = json.loads(json.dumps(valid))
            imported_private["top_blocker_classes"] = [{
                "boundary_code": private_boundary,
                "title_count": 1,
                "issue_numbers": [],
                "tracking": "no issue: propose one",
            }]
            with self.assertRaisesRegex(ValueError, "blocker code is not source-backed"):
                library_sweep._validate_public_aggregate(imported_private)

        family_rows = [_sweep_row("launch"), _sweep_row("launch")]
        family_rows[0]["nid_families"] = ["sceAudio"]
        family_rows[1]["nid_families"] = ["scePower"]
        families = library_sweep._public_aggregate(
            family_rows, SOURCE_COMMIT, SOURCE_FINGERPRINT, 120, 2
        )
        families["nid_families_by_title_count"].append(
            dict(families["nid_families_by_title_count"][0])
        )
        with self.assertRaisesRegex(ValueError, "family count or ordering"):
            library_sweep._validate_public_aggregate(families)

        comparison = json.loads(json.dumps(valid))
        comparison["comparison"] = {
            "status": "MATCHED",
            "reason": "same_source_and_input_count",
            "stage_delta": None,
            "completed_routes_delta": None,
        }
        with self.assertRaisesRegex(ValueError, "matched comparison"):
            library_sweep._validate_public_aggregate(comparison)

    def test_malformed_retained_rows_fail_closed_as_value_error(self) -> None:
        boundary_list = _sweep_row("launch")
        boundary_list["boundary_code"] = ["UNSUPPORTED_IMPORT"]
        issues_none = _sweep_row("launch")
        issues_none["boundary_code"] = "UNSUPPORTED_IMPORT"
        issues_none["issue_numbers"] = None
        families_none = _sweep_row("launch")
        families_none["nid_families"] = None
        stage_list = _sweep_row("launch")
        stage_list["furthest_stage"] = ["launch"]
        mixed_issues = _sweep_row("launch")
        mixed_issues["issue_numbers"] = [308, "not-an-issue"]
        non_object = "not-a-row"

        for malformed in (
            boundary_list,
            issues_none,
            families_none,
            stage_list,
            mixed_issues,
            non_object,
        ):
            with self.assertRaisesRegex(ValueError, "sweep row 0"):
                library_sweep._public_aggregate(
                    [malformed], SOURCE_COMMIT, SOURCE_FINGERPRINT, 120, 1
                )

    def test_malformed_retained_row_leaves_both_outputs_unchanged(self) -> None:
        self.private_dir.mkdir(parents=True, exist_ok=True)
        self.public_output.parent.mkdir(parents=True, exist_ok=True)
        private_path = self.private_dir / "library-sweep.json"
        private_path.write_text("PRIVATE_CHECKPOINT", encoding="utf-8")
        self.public_output.write_text("PUBLIC_CHECKPOINT", encoding="utf-8")
        malformed = _sweep_row("launch")
        malformed["nid_families"] = None

        with self.assertRaisesRegex(ValueError, "sweep row 0"):
            library_sweep._write_outputs(
                private_path,
                self.public_output,
                SOURCE_COMMIT,
                SOURCE_FINGERPRINT,
                120,
                [malformed],
                total_isos=1,
                ran_this_invocation=0,
                resumed_this_invocation=0,
            )

        self.assertEqual(private_path.read_bytes(), b"PRIVATE_CHECKPOINT")
        self.assertEqual(self.public_output.read_bytes(), b"PUBLIC_CHECKPOINT")

    def test_aggregate_failure_leaves_both_outputs_unchanged(self) -> None:
        self.private_dir.mkdir(parents=True, exist_ok=True)
        self.public_output.parent.mkdir(parents=True, exist_ok=True)
        private_path = self.private_dir / "library-sweep.json"
        private_path.write_bytes(b"PRIVATE_CHECKPOINT")
        self.public_output.write_bytes(b"PUBLIC_CHECKPOINT")

        with mock.patch.object(
            library_sweep, "_public_aggregate", side_effect=ValueError("invalid aggregate")
        ):
            with self.assertRaisesRegex(ValueError, "invalid aggregate"):
                library_sweep._write_outputs(
                    private_path,
                    self.public_output,
                    SOURCE_COMMIT,
                    SOURCE_FINGERPRINT,
                    120,
                    [_sweep_row("launch")],
                    total_isos=1,
                    ran_this_invocation=0,
                    resumed_this_invocation=0,
                )

        self.assertEqual(private_path.read_bytes(), b"PRIVATE_CHECKPOINT")
        self.assertEqual(self.public_output.read_bytes(), b"PUBLIC_CHECKPOINT")

    def test_value_invalid_retained_row_leaves_both_outputs_unchanged(self) -> None:
        self.private_dir.mkdir(parents=True, exist_ok=True)
        self.public_output.parent.mkdir(parents=True, exist_ok=True)
        private_path = self.private_dir / "library-sweep.json"
        invalid_rows = (
            (
                "private boundary",
                {"boundary_code": "PRIVATE_TITLE"},
                "boundary_code is not source-backed",
            ),
            (
                "private stage",
                {"furthest_stage": "PRIVATE_STAGE"},
                "furthest_stage is not source-backed",
            ),
            (
                "private status",
                {"run_status": "PRIVATE_STATUS"},
                "run_status is not supported",
            ),
            (
                "unknown issue",
                {"boundary_code": "UNSUPPORTED_IMPORT", "issue_numbers": [999]},
                "issue numbers are not source-backed",
            ),
        )

        for label, updates, message in invalid_rows:
            with self.subTest(label=label):
                private_path.write_bytes(b"PRIVATE_CHECKPOINT")
                self.public_output.write_bytes(b"PUBLIC_CHECKPOINT")
                invalid = _sweep_row("launch")
                invalid.update(updates)
                with self.assertRaisesRegex(ValueError, message):
                    library_sweep._write_outputs(
                        private_path,
                        self.public_output,
                        SOURCE_COMMIT,
                        SOURCE_FINGERPRINT,
                        120,
                        [invalid],
                        total_isos=1,
                        ran_this_invocation=0,
                        resumed_this_invocation=0,
                    )
                self.assertEqual(private_path.read_bytes(), b"PRIVATE_CHECKPOINT")
                self.assertEqual(self.public_output.read_bytes(), b"PUBLIC_CHECKPOINT")

    def test_retained_row_schema_failure_refuses_before_any_output_advances(self) -> None:
        iso = _write_iso(self.iso_dir, "retained.iso", "UCUS99996", "Retained Sentinel")
        iso_root = self.iso_dir.resolve()
        stat = iso.stat()
        malformed = _sweep_row("launch")
        malformed.update({
            "source_key": library_sweep._source_key(iso_root / iso.name, iso_root),
            "source_file": iso.name,
            "disc_id": "UCUS99996",
            "input_size_bytes": stat.st_size,
            "input_mtime_ns": stat.st_mtime_ns,
            "boundary_code": ["UNSUPPORTED_IMPORT"],
        })
        self.private_dir.mkdir(parents=True, exist_ok=True)
        private_path = self.private_dir / "library-sweep.json"
        private_path.write_text(json.dumps({
            "schema_version": 1,
            "source_commit": SOURCE_COMMIT,
            "source_fingerprint": library_sweep._source_fingerprint(),
            "time_budget_seconds": 120,
            "rows": [malformed],
        }), encoding="utf-8")
        private_before = private_path.read_bytes()
        self.public_output.parent.mkdir(parents=True, exist_ok=True)
        self.public_output.write_text("PUBLIC_CHECKPOINT", encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "sweep row 0"):
            library_sweep.run_sweep(
                self.iso_dir, self.private_dir, self.public_output, **self._run_kwargs()
            )

        self.assertEqual(private_path.read_bytes(), private_before)
        self.assertEqual(self.public_output.read_bytes(), b"PUBLIC_CHECKPOINT")

    def test_private_nid_sidecar_is_contained_and_whitelisted(self) -> None:
        work_dir = self.private_dir / "bringup"
        work_dir.mkdir(parents=True)
        sidecar = work_dir / "sweep-imports.json"
        imports = [{
            "library": "sceAudio",
            "nid": 0x12345678,
            "name": "sceAudioInit",
            "module": "synthetic.prx",
            "boundary": "private diagnostic detail",
        }, {
            "library": "sceAudio",
            "nid": "0x87654321",
            "name": "sceAudioTerm",
            "module": "synthetic.prx",
            "boundary": "private diagnostic detail",
        }]
        with self.assertRaises(ValueError):
            nk_cli._write_private_sweep_import_report(
                self.private_dir / "outside.json", work_dir, imports
            )
        nk_cli._write_private_sweep_import_report(sidecar, work_dir, imports)
        payload = json.loads(sidecar.read_text(encoding="utf-8"))
        self.assertEqual(payload, {
            "schema_version": 1,
            "unsupported_imports": [
                {
                    "library": "sceAudio",
                    "nid": "0x12345678",
                    "nid_name": "sceAudioInit",
                },
                {
                    "library": "sceAudio",
                    "nid": "0x87654321",
                    "nid_name": "sceAudioTerm",
                },
            ],
        })

    def test_empty_sidecar_recovers_nids_from_private_build_report(self) -> None:
        work_dir = self.private_dir / "work" / "synthetic"
        output_dir = work_dir / "package"
        output_dir.mkdir(parents=True)
        sidecar = work_dir / "sweep-imports.json"
        sidecar.write_text(json.dumps({
            "schema_version": 1,
            "unsupported_imports": [],
        }), encoding="utf-8")
        (output_dir / "build-report.json").write_text(json.dumps({
            "unsupported": {
                "imports": [{
                    "library": "sceAudio",
                    "nid": "0x12345678",
                    "name": "sceAudioInit",
                }],
            },
        }), encoding="utf-8")

        rows, status = library_sweep._read_private_nid_rows(
            sidecar,
            _bringup_report(),
        )

        self.assertEqual(status, "BUILD_REPORT")
        self.assertEqual(rows, [{
            "library": "sceAudio",
            "nid": "0x12345678",
            "nid_name": "sceAudioInit",
        }])

    def test_decrypted_inputs_are_staged_from_the_disc_id_folder(self) -> None:
        decrypted_root = self.root / "decrypted-titles"
        source_dir = decrypted_root / "ULUS99995" / "decrypted"
        source_dir.mkdir(parents=True)
        eboot = b"synthetic decrypted executable"
        module = b"synthetic decrypted module"
        (source_dir / "EBOOT.elf").write_bytes(eboot)
        (source_dir / "libsample.prx").write_bytes(module)
        (source_dir / "notes.txt").write_text("ignored", encoding="utf-8")

        copied, status = library_sweep._stage_decrypted_inputs(
            decrypted_root,
            "ULUS99995",
            self.private_dir / "work",
        )
        production_dir = (
            self.private_dir / "work" / "user-data" / "titles" / "ULUS99995" / "decrypted"
        )
        self.assertEqual(copied, 2)
        self.assertEqual(status, "DIRECT_FOLDER")
        self.assertEqual((production_dir / "EBOOT.elf").read_bytes(), eboot)
        self.assertEqual((production_dir / "libsample.prx").read_bytes(), module)
        self.assertFalse((production_dir / "notes.txt").exists())

    def test_unmatched_disc_id_stages_nothing(self) -> None:
        decrypted_root = self.root / "decrypted-titles"
        (decrypted_root / "ULUS99994" / "decrypted").mkdir(parents=True)

        copied, status = library_sweep._stage_decrypted_inputs(
            decrypted_root,
            "ULUS99995",
            self.private_dir / "work",
        )
        self.assertEqual((copied, status), (0, "NO_MATCH"))

if __name__ == "__main__":
    unittest.main()
