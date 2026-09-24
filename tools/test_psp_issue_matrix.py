# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import psp_issue_matrix


class PspIssueMatrixTests(unittest.TestCase):
    def routing_file(self, entries: list[dict[str, object]]) -> Path:
        directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        path = directory / "routing.json"
        path.write_text(
            json.dumps({"schema_version": 1, "entries": entries}),
            encoding="utf-8",
        )
        return path

    def oracle_file(self, issues: list[int], *, group: str = "kernel") -> Path:
        directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        path = directory / "oracle.json"
        path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "tests": [{"id": "ORACLE-001", "group": group, "issues": issues}],
                }
            ),
            encoding="utf-8",
        )
        return path

    def issue(
        self,
        number: int,
        *,
        state: str = "OPEN",
        milestone: dict[str, object] | None = None,
    ) -> dict[str, object]:
        result: dict[str, object] = {
            "number": number,
            "title": f"Synthetic Issue {number}",
            "body": f"Synthetic claim for {number}",
            "state": state,
        }
        if milestone is not None:
            result["milestone"] = milestone
        return result

    def build_fixture_matrix(
        self,
        issues: list[dict[str, object]],
        entries: list[dict[str, object]],
        *,
        oracle_issues: list[int] | None = None,
    ) -> dict[str, object]:
        return psp_issue_matrix.build_matrix(
            issues,
            generated_at="2026-01-01T00:00:00Z",
            routing_manifest=self.routing_file(entries),
            oracle_manifest=self.oracle_file(oracle_issues or []),
        )

    def test_hardware_route_uses_fixture_oracle_link(self) -> None:
        matrix = self.build_fixture_matrix(
            [self.issue(23)],
            [
                {
                    "issue": 23,
                    "primary_state": "hardware",
                    "hardware_test_id": "ORACLE-001",
                    "oracle_groups": ["kernel"],
                }
            ],
            oracle_issues=[23],
        )
        row = matrix["rows"][0]
        self.assertEqual(row["primary_state"], "PSP_HARDWARE_READY")
        self.assertEqual(row["hardware_test_id"], "ORACLE-001")
        self.assertEqual(matrix["findings"], [])

    def test_classifications_are_manifest_backed(self) -> None:
        entries = [
            {"issue": 27, "primary_state": "legal"},
            {"issue": 248, "primary_state": "upstream"},
            {"issue": 54, "primary_state": "environment"},
            {"issue": 31, "primary_state": "private_route", "depends_on": [38, 69]},
            {"issue": 148, "primary_state": "implementation"},
            {"issue": 45, "primary_state": "local_acceptance"},
            {"issue": 23, "primary_state": "hardware", "hardware_test_id": "ORACLE-001"},
            {"issue": 278, "primary_state": "future_work"},
        ]
        routes = psp_issue_matrix.load_routing_manifest(self.routing_file(entries))
        expected = {
            27: "LEGAL_HUMAN_BLOCKED",
            248: "UPSTREAM_BLOCKED",
            54: "ENVIRONMENT_BLOCKED",
            31: "LOCAL_PRIVATE_ROUTE_READY",
            148: "LOCAL_IMPLEMENTATION_READY",
            45: "LOCAL_ACCEPTANCE_READY",
            23: "PSP_HARDWARE_READY",
            278: "MAJOR_FUTURE_WORK",
        }
        for number, state in expected.items():
            self.assertEqual(psp_issue_matrix._state(number, routes), state)
        self.assertEqual(routes[31]["depends_on"], [38, 69])
        self.assertEqual(routes[23]["hardware_test_id"], "ORACLE-001")
        self.assertEqual(
            psp_issue_matrix._local_command("LOCAL_IMPLEMENTATION_READY", 148, routes),
            "python tools/psp_readiness.py --json",
        )

    def test_fixture_matrix_covers_every_issue_snapshot(self) -> None:
        issues = [self.issue(501), self.issue(502), self.issue(503)]
        entries = [
            {"issue": 501, "primary_state": "implementation"},
            {"issue": 502, "primary_state": "legal"},
            {
                "issue": 503,
                "primary_state": "hardware",
                "hardware_test_id": "ORACLE-001",
                "oracle_groups": ["kernel"],
            },
        ]
        matrix = self.build_fixture_matrix(issues, entries, oracle_issues=[503])
        self.assertEqual(matrix["issue_count"], len(matrix["rows"]))
        self.assertEqual(matrix["issue_count"], len(issues))
        self.assertTrue(
            {row["primary_state"] for row in matrix["rows"]}.issubset(
                psp_issue_matrix.STATES | {psp_issue_matrix.MISSING_ROUTING_STATE}
            )
        )
        self.assertEqual(len({row["issue"] for row in matrix["rows"]}), len(issues))
        for row in matrix["rows"]:
            self.assertTrue(row["local_test_command"])
        hardware_row = next(row for row in matrix["rows"] if row["issue"] == 503)
        self.assertEqual(hardware_row["hardware_test_id"], "ORACLE-001")
        self.assertEqual(matrix["findings"], [])

    def test_new_issue_is_routed_by_manifest_entry_without_python_edit(self) -> None:
        matrix = self.build_fixture_matrix(
            [self.issue(501, milestone={"number": 9, "title": "v9.9"})],
            [{"issue": 501, "primary_state": "implementation"}],
        )
        row = matrix["rows"][0]
        self.assertEqual(row["primary_state"], "LOCAL_IMPLEMENTATION_READY")
        self.assertEqual(row["routing_status"], "routed")
        self.assertEqual(row["version_target"], "v9.9")
        self.assertEqual(matrix["findings"], [])

    def test_oracle_group_is_derived_when_route_omits_it(self) -> None:
        matrix = self.build_fixture_matrix(
            [self.issue(501)],
            [{"issue": 501, "primary_state": "hardware"}],
            oracle_issues=[501],
        )
        self.assertEqual(matrix["rows"][0]["oracle_groups"], ["kernel"])
        self.assertEqual(matrix["rows"][0]["oracle_test_ids"], ["ORACLE-001"])
        self.assertEqual(matrix["findings"], [])

    def test_missing_routing_is_reported_instead_of_generic_fallback(self) -> None:
        matrix = self.build_fixture_matrix(
            [self.issue(501), self.issue(502)],
            [{"issue": 501, "primary_state": "future_work"}],
        )
        self.assertEqual(matrix["rows"][1]["primary_state"], psp_issue_matrix.MISSING_ROUTING_STATE)
        self.assertIn("missing_routing", {finding["code"] for finding in matrix["findings"]})

    def test_closed_route_is_reported_as_stale(self) -> None:
        matrix = self.build_fixture_matrix(
            [self.issue(501, state="CLOSED")],
            [{"issue": 501, "primary_state": "future_work"}],
        )
        self.assertIn("stale_route", {finding["code"] for finding in matrix["findings"]})

    def test_nonexistent_route_is_reported_as_stale(self) -> None:
        matrix = self.build_fixture_matrix(
            [self.issue(502)],
            [{"issue": 501, "primary_state": "future_work"}],
        )
        finding = next(item for item in matrix["findings"] if item["code"] == "stale_route")
        self.assertEqual(finding["issue"], 501)

    def test_missing_dependency_is_reported(self) -> None:
        matrix = self.build_fixture_matrix(
            [self.issue(501)],
            [{"issue": 501, "primary_state": "future_work", "depends_on": [999]}],
        )
        finding = next(item for item in matrix["findings"] if item["code"] == "missing_dependency")
        self.assertEqual(finding["dependency"], 999)

    def test_dependency_cycle_is_reported(self) -> None:
        matrix = self.build_fixture_matrix(
            [self.issue(501), self.issue(502)],
            [
                {"issue": 501, "primary_state": "future_work", "depends_on": [502]},
                {"issue": 502, "primary_state": "future_work", "depends_on": [501]},
            ],
        )
        finding = next(item for item in matrix["findings"] if item["code"] == "dependency_cycle")
        self.assertEqual(finding["cycle"], [501, 502])

    def test_oracle_link_disagreement_is_reported(self) -> None:
        matrix = self.build_fixture_matrix(
            [self.issue(501)],
            [{"issue": 501, "primary_state": "hardware", "oracle_groups": ["display"]}],
            oracle_issues=[501],
        )
        finding = next(item for item in matrix["findings"] if item["code"] == "oracle_link_disagreement")
        self.assertEqual(finding["declared"], ["display"])
        self.assertEqual(finding["derived"], ["kernel"])

    def test_milestone_is_preserved_in_output(self) -> None:
        matrix = self.build_fixture_matrix(
            [self.issue(501, milestone={"number": 2, "title": "v0.0.2", "dueOn": None})],
            [{"issue": 501, "primary_state": "future_work"}],
        )
        row = matrix["rows"][0]
        self.assertEqual(row["milestone"]["title"], "v0.0.2")
        self.assertEqual(row["version_target"], "v0.0.2")

    def test_strict_cli_returns_nonzero_for_findings(self) -> None:
        directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        input_path = directory / "issues.json"
        output_path = directory / "matrix.json"
        routing_path = self.routing_file([{"issue": 501, "primary_state": "future_work"}])
        oracle_path = self.oracle_file([])
        input_path.write_text(json.dumps([self.issue(501), self.issue(502)]), encoding="utf-8")
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            result = psp_issue_matrix.main(
                [
                    "--input",
                    str(input_path),
                    "--out",
                    str(output_path),
                    "--routing",
                    str(routing_path),
                    "--oracle-manifest",
                    str(oracle_path),
                    "--strict",
                ]
            )
        self.assertEqual(result, 1)
        self.assertIn("missing_routing", stderr.getvalue())
        self.assertIn("missing_routing", output_path.read_text(encoding="utf-8"))

    def test_issue_routing_fixture_distinguishes_private_and_hardware(self) -> None:
        entries = [
            {"issue": 23, "primary_state": "hardware", "hardware_test_id": "ORACLE-001"},
            {"issue": 63, "primary_state": "private_route"},
            {"issue": 69, "primary_state": "private_route"},
            {"issue": 70, "primary_state": "hardware", "hardware_test_id": "ORACLE-001"},
            {"issue": 98, "primary_state": "implementation"},
        ]
        routes = psp_issue_matrix.load_routing_manifest(self.routing_file(entries))
        self.assertEqual(psp_issue_matrix._state(23, routes), "PSP_HARDWARE_READY")
        self.assertEqual(routes[23]["hardware_test_id"], "ORACLE-001")
        self.assertEqual(psp_issue_matrix._state(63, routes), "LOCAL_PRIVATE_ROUTE_READY")
        self.assertNotIn("hardware_test_id", routes[63])
        self.assertEqual(psp_issue_matrix._state(69, routes), "LOCAL_PRIVATE_ROUTE_READY")
        self.assertNotIn("hardware_test_id", routes[69])
        self.assertEqual(psp_issue_matrix._state(70, routes), "PSP_HARDWARE_READY")
        self.assertEqual(routes[70]["hardware_test_id"], "ORACLE-001")
        self.assertEqual(psp_issue_matrix._state(98, routes), "LOCAL_IMPLEMENTATION_READY")


if __name__ == "__main__":
    unittest.main()
