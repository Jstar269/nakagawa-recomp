# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import psp_issue_matrix


class PspIssueMatrixTests(unittest.TestCase):
    def test_checked_in_snapshot_covers_every_open_issue_snapshot(self) -> None:
        path = Path(__file__).resolve().parents[1] / "docs" / "PSP_ISSUE_MATRIX.json"
        matrix = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(matrix["issue_count"], len(matrix["rows"]))
        self.assertEqual(matrix["issue_count"], 85)
        self.assertEqual({row["primary_state"] for row in matrix["rows"]}, psp_issue_matrix.STATES)
        self.assertEqual(len({row["issue"] for row in matrix["rows"]}), 85)

        manifest = json.loads(
            (Path(__file__).resolve().parent / "psp_oracle" / "manifest.json").read_text(encoding="utf-8")
        )
        manifest_ids = {entry["id"] for entry in manifest["tests"]}
        for row in matrix["rows"]:
            self.assertTrue(row["local_test_command"])
            if row["primary_state"] == "PSP_HARDWARE_READY":
                self.assertIn(row["hardware_test_id"], manifest_ids)

    def test_unknown_issue_defaults_to_future_work(self) -> None:
        matrix = psp_issue_matrix.build_matrix(
            [{"number": 999, "title": "synthetic", "body": "Claim", "url": None, "updatedAt": None}],
            generated_at="2026-01-01T00:00:00Z",
        )
        self.assertEqual(matrix["rows"][0]["primary_state"], "MAJOR_FUTURE_WORK")


if __name__ == "__main__":
    unittest.main()
