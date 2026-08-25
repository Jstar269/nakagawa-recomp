# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression: tools/test_* paths must classify through the canonical
deterministic classifier, never through the forbidden wildcard-backed
``tooling-general`` record.

Background: candidate 81fc98e carried a stale public-ledger entry for
``tools/test_native_driver_hardening.py`` citing ``record_id: tooling-general``
whose only authority is the ``tools/*`` wildcard. The canonical generator
deliberately refuses to expand wildcard records, so a regenerated ledger must
classify this path via the deterministic fixture/test rule instead.
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import provenance_ledger  # noqa: E402


PATH = "tools/test_native_driver_hardening.py"
FORBIDDEN_WILDCARD_RECORDS = ("tooling-general",)


class TestWildcardBackedClassificationRepair(unittest.TestCase):
    def _ledger(self) -> dict:
        return json.loads(
            (ROOT / "assets" / "public_provenance_ledger.json").read_text(encoding="utf-8")
        )

    def test_canonical_classifier_is_record_free_and_deterministic(self):
        """With no detailed-ledger record (wildcards are never expanded), the
        current canonical classifier yields synthetic_fixture census evidence."""
        classification, evidence = provenance_ledger._class_for(PATH, None)
        self.assertEqual(classification, "synthetic_fixture")
        self.assertEqual(
            evidence,
            {
                "source": "path-reviewed fixture/test census",
                "statement": "fixture or test data is synthetic and contains no retail bytes",
            },
        )
        # The deterministic rule must be reachable for this exact path shape,
        # so a future classifier change cannot silently restore record-backed
        # authority for unrecorded test paths without this test noticing.
        self.assertTrue(PATH.startswith("tools/test_") or "/test_" in PATH)

    def test_checked_in_entry_matches_classifier_and_current_bytes(self):
        entries = {e["path"]: e for e in self._ledger()["entries"]}
        self.assertIn(PATH, entries)
        entry = entries[PATH]
        self.assertEqual(entry["classification"], "synthetic_fixture")
        self.assertNotIn("record_id", entry["evidence"])
        for forbidden in FORBIDDEN_WILDCARD_RECORDS:
            self.assertNotEqual(entry["evidence"].get("record_id"), forbidden)
            self.assertNotIn(forbidden, json.dumps(entry["evidence"]))
        digest = hashlib.sha256((ROOT / PATH).read_bytes()).hexdigest()
        self.assertEqual(
            entry.get("sha256"),
            digest,
            "public ledger hash is stale for this tracked file; regenerate metadata",
        )


if __name__ == "__main__":
    unittest.main()
