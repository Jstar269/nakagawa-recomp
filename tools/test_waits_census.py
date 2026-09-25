# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the psp-recomp authors
"""Tests for tools/waits_census.py (issue #339 acceptance)."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import waits_census


SAMPLE_MATRIX = """
# Sample

Exact names: `sceKernelCreateMbx` and `sceKernelDelayThread`.

Globs and calls are skipped: `sceIoWaitAsync*`, `sceKernelWaitSema(sema, 9, NULL)`,
`sce*`, `sceDisplayWaitVblankStartMulti(0)`.
"""


class ExtractApisTests(unittest.TestCase):
    def test_exact_names_only(self) -> None:
        apis = waits_census.extract_apis(SAMPLE_MATRIX)
        self.assertEqual(apis, ["sceKernelCreateMbx", "sceKernelDelayThread"])

    def test_globs_and_call_forms_excluded(self) -> None:
        apis = waits_census.extract_apis(SAMPLE_MATRIX)
        self.assertNotIn("sceIoWaitAsync", apis)
        self.assertNotIn("sceKernelWaitSema", apis)

    def test_underscore_and_digit_names(self) -> None:
        text = "use `__sceSasSetADSR` and `sceKernelWaitEventFlagCB`."
        self.assertEqual(
            waits_census.extract_apis(text),
            ["__sceSasSetADSR", "sceKernelWaitEventFlagCB"],
        )


class DispositionTests(unittest.TestCase):
    def test_missing_row_owner_is_339(self) -> None:
        census = waits_census.build_census(SAMPLE_MATRIX, manifest={"registrations": []})
        by_api = {r["api"]: r for r in census["rows"]}
        for api in ("sceKernelCreateMbx", "sceKernelDelayThread"):
            self.assertEqual(by_api[api]["disposition"], "missing")
            self.assertEqual(by_api[api]["owner"], "#339")

    def test_fake_success_maps_to_registered_and_281(self) -> None:
        manifest = {
            "registrations": [
                {
                    "nid": "0x00000010",
                    "name": "sceKernelCreateMbx",
                    "handler": "h_ok",
                    "classification": "fake_success",
                    "status": "stub",
                }
            ]
        }
        census = waits_census.build_census(SAMPLE_MATRIX, manifest=manifest)
        row = next(r for r in census["rows"] if r["api"] == "sceKernelCreateMbx")
        self.assertEqual(row["disposition"], "registered")
        self.assertEqual(row["owner"], "#281")

    def test_controlled_unsupported_has_no_owner(self) -> None:
        manifest = {
            "registrations": [
                {
                    "nid": "0x00000010",
                    "name": "sceKernelCreateMbx",
                    "handler": "h_ControlledUnsupported",
                    "classification": "controlled_unsupported",
                    "status": "controlled_unsupported",
                }
            ]
        }
        census = waits_census.build_census(SAMPLE_MATRIX, manifest=manifest)
        row = next(r for r in census["rows"] if r["api"] == "sceKernelCreateMbx")
        self.assertEqual(row["disposition"], "controlled-unsupported")
        self.assertEqual(row["owner"], "-")

    def test_unreviewed_dedicated_owner_is_341(self) -> None:
        manifest = {
            "registrations": [
                {
                    "nid": "0x00000010",
                    "name": "sceKernelCreateMbx",
                    "handler": "h_CreateMbx",
                    "classification": "dedicated",
                    "status": "unreviewed",
                }
            ]
        }
        census = waits_census.build_census(SAMPLE_MATRIX, manifest=manifest)
        row = next(r for r in census["rows"] if r["api"] == "sceKernelCreateMbx")
        self.assertEqual(row["disposition"], "implemented")
        self.assertEqual(row["owner"], "#341")

    def test_partial_dedicated_has_no_default_owner(self) -> None:
        manifest = {
            "registrations": [
                {
                    "nid": "0x00000010",
                    "name": "sceKernelCreateMbx",
                    "handler": "h_CreateMbx",
                    "classification": "dedicated",
                    "status": "partial",
                }
            ]
        }
        census = waits_census.build_census(SAMPLE_MATRIX, manifest=manifest)
        row = next(r for r in census["rows"] if r["api"] == "sceKernelCreateMbx")
        self.assertEqual(row["disposition"], "implemented")
        self.assertEqual(row["owner"], "-")

    def test_summary_counts_match_rows(self) -> None:
        census = waits_census.build_census(SAMPLE_MATRIX, manifest={"registrations": []})
        s = census["summary"]
        self.assertEqual(s["total_apis"], 2)
        self.assertEqual(s["by_disposition"]["missing"], 2)
        self.assertEqual(s["by_owner"]["#339"], 2)


class LiveManifestTests(unittest.TestCase):
    """Join against the real hle.c registrations (not a synthetic manifest)."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.census = waits_census.build_census()

    def test_matrix_is_non_empty(self) -> None:
        self.assertGreaterEqual(self.census["summary"]["total_apis"], 50)

    def test_mailbox_apis_in_the_matrix_are_no_longer_missing(self) -> None:
        by_api = {r["api"]: r for r in self.census["rows"]}
        # Only the mailbox names the matrix itself names appear in the census;
        # Send/Poll/Cancel/Refer are registered in hle.c but are not matrix cells.
        for api in (
            "sceKernelCreateMbx",
            "sceKernelDeleteMbx",
            "sceKernelReceiveMbx",
            "sceKernelReceiveMbxCB",
        ):
            self.assertIn(api, by_api, f"{api} must appear in the matrix census")
            self.assertNotEqual(
                by_api[api]["disposition"],
                "missing",
                f"{api} is registered in hle.c but census reports missing",
            )
            self.assertEqual(by_api[api]["disposition"], "implemented")
            self.assertEqual(by_api[api]["status"], "partial")

    def test_all_eight_mailbox_nids_are_registered(self) -> None:
        from hle_manifest import build_manifest

        names = {r["name"] for r in build_manifest()["registrations"]}
        for api in (
            "sceKernelCreateMbx",
            "sceKernelDeleteMbx",
            "sceKernelSendMbx",
            "sceKernelReceiveMbx",
            "sceKernelReceiveMbxCB",
            "sceKernelPollMbx",
            "sceKernelCancelReceiveMbx",
            "sceKernelReferMbxStatus",
        ):
            self.assertIn(api, names, f"{api} must be registered in hle.c")

    def test_every_row_has_an_allowed_disposition(self) -> None:
        for r in self.census["rows"]:
            self.assertIn(r["disposition"], waits_census.DISPOSITIONS, r["api"])

    def test_every_open_disposition_has_an_owner(self) -> None:
        for r in self.census["rows"]:
            if r["disposition"] in ("missing", "registered"):
                self.assertNotEqual(r["owner"], "-", r["api"])
            elif r["disposition"] == "implemented" and r["status"] == "unreviewed":
                self.assertEqual(r["owner"], "#341", r["api"])
            else:
                self.assertEqual(r["owner"], "-", r["api"])

    def test_rows_are_sorted_by_api(self) -> None:
        apis = [r["api"] for r in self.census["rows"]]
        self.assertEqual(apis, sorted(apis))

    def test_render_is_deterministic(self) -> None:
        a = waits_census.render_markdown(self.census)
        b = waits_census.render_markdown(waits_census.build_census())
        self.assertEqual(a, b)


class CommittedDocTests(unittest.TestCase):
    def test_checked_in_census_is_current(self) -> None:
        self.assertEqual(
            waits_census.main(["--check"]),
            0,
            "docs/PSP_INTR_WAITS_CENSUS.md is stale; run python tools/waits_census.py",
        )


if __name__ == "__main__":
    unittest.main()
