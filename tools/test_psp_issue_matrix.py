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
    def test_current_dma_issue_routes_to_the_dedicated_probe(self) -> None:
        self.assertEqual(psp_issue_matrix.HARDWARE_IDS[23], "PSP-DMAC-001")

    def test_checked_in_snapshot_covers_every_open_issue_snapshot(self) -> None:
        path = Path(__file__).resolve().parents[1] / "docs" / "PSP_ISSUE_MATRIX.json"
        if not path.is_file():
            self.skipTest(
                "volatile issue snapshot is intentionally excluded from the sanitized public tree"
            )
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

    def test_issue_routing_cleanup(self) -> None:
        # Issue 23: Genuine DMAC hardware semantics
        self.assertEqual(psp_issue_matrix._state(23), "PSP_HARDWARE_READY")
        self.assertEqual(psp_issue_matrix.HARDWARE_IDS[23], "PSP-DMAC-001")

        # Issue 63: Private-title save/ceremony route, not generic IO
        self.assertEqual(psp_issue_matrix._state(63), "LOCAL_PRIVATE_ROUTE_READY")
        self.assertNotIn(63, psp_issue_matrix.HARDWARE_IDS)

        # Issue 69: Private-title model corruption route, not generic audio
        self.assertEqual(psp_issue_matrix._state(69), "LOCAL_PRIVATE_ROUTE_READY")
        self.assertNotIn(69, psp_issue_matrix.HARDWARE_IDS)

        # Issue 70: VBLANK / clock rate drift, routes to PSP-SYSTEM-001 (not audio)
        self.assertEqual(psp_issue_matrix._state(70), "PSP_HARDWARE_READY")
        self.assertEqual(psp_issue_matrix.HARDWARE_IDS[70], "PSP-SYSTEM-001")

        # Issue 98: Compatibility-override surface, not legal block
        self.assertEqual(psp_issue_matrix._state(98), "LOCAL_IMPLEMENTATION_READY")
        self.assertEqual(psp_issue_matrix._local_command("LOCAL_IMPLEMENTATION_READY", 98), "python tools/compat_overrides.py --check")


if __name__ == "__main__":
    unittest.main()
