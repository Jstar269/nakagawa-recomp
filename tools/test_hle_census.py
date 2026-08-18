# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for the public-safe HLE capability census (tools/hle_census.py).

The census must stay exactly consistent with the canonical registration
manifest (tools/hle_manifest.py), must never invent evidence classes, and must
keep the unreviewed triage limited to genuinely unreviewed registrations
ordered by the documented generic-PSP relevance score.
"""

from __future__ import annotations

import json
from pathlib import Path
import unittest

import hle_census
import hle_manifest
from hle_census import build_census, derive_module, scheduler_class

ROOT = Path(__file__).resolve().parents[1]

REQUIRED_FIELDS = {
    "module", "nid", "name", "handler", "handler_class", "status",
    "semantic_annotation", "guest_span_behavior", "scheduler_interaction",
    "tests", "evidence_links", "title_use_evidence",
}


class ModuleDerivationTests(unittest.TestCase):
    def test_camel_case_module_bucket(self) -> None:
        self.assertEqual(derive_module("sceKernelCreateThread"), "sceKernel")
        self.assertEqual(derive_module("sceDisplayGetFramePerSec"), "sceDisplay")
        self.assertEqual(derive_module("sceAtracGetAtracID"), "sceAtrac")
        self.assertEqual(derive_module("__sceSasSetADSR"), "sceSas")

    def test_newlib_and_unknown_buckets(self) -> None:
        self.assertEqual(derive_module("newlibModuleStreamWrite"), "newlib")
        self.assertEqual(derive_module("someUnknownSymbol"), "other")


class SchedulerClassTests(unittest.TestCase):
    def test_wait_vblank(self) -> None:
        cls = scheduler_class("int x = sched_wait_vblank();")
        self.assertIn("vblank", cls)
        self.assertIn("wait_or_switch", cls)

    def test_none(self) -> None:
        self.assertEqual(scheduler_class("return 0;"), "none_static")

    def test_thread_lifecycle(self) -> None:
        cls = scheduler_class("sched_create_thread(A1, ...);")
        self.assertIn("thread_lifecycle", cls)


class LiveCensusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        source = hle_manifest.HLE_C.read_text(encoding="utf-8")
        cls.census = build_census(source, top=30)
        cls.manifest = hle_manifest.build_manifest()

    def test_counts_match_manifest_exactly(self) -> None:
        m_counts = {}
        for r in self.manifest["registrations"]:
            m_counts[r["status"]] = m_counts.get(r["status"], 0) + 1
        self.assertEqual(self.census["counts"]["total"], len(self.manifest["registrations"]))
        for status, n in m_counts.items():
            self.assertEqual(self.census["counts"][status], n, status)

    def test_every_registration_has_all_required_fields(self) -> None:
        for r in self.census["registrations"]:
            self.assertTrue(REQUIRED_FIELDS.issubset(r.keys()), r["nid"])
            self.assertEqual(r["title_use_evidence"], "none_public", r["nid"])
            self.assertEqual(r["guest_span_behavior"]["evidence"], "SOURCE_SHAPE")
            self.assertEqual(r["scheduler_interaction"]["evidence"], "SOURCE_SHAPE")

    def test_triage_only_unreviewed_and_sorted(self) -> None:
        top = self.census["unreviewed_triage_top"]
        self.assertEqual(len(top), 30)
        for t in top:
            self.assertEqual(t["rank"] > 0, True)
            self.assertEqual(t["rank"], top.index(t) + 1)
        scores = [t["score"] for t in top]
        self.assertEqual(scores, sorted(scores, reverse=True))
        statuses = {r["status"] for r in self.census["registrations"] if r["nid"] in {t["nid"] for t in top}}
        self.assertEqual(statuses, {"unreviewed"})

    def test_curated_complete_handlers_present(self) -> None:
        by_nid = {r["nid"]: r for r in self.census["registrations"]}
        self.assertEqual(by_nid["0xdba6c4c4"]["status"], "complete")  # sceDisplayGetFramePerSec
        self.assertEqual(by_nid["0x1b4217bc"]["status"], "complete")  # SetCompiledSdkVersion603_605

    def test_deterministic(self) -> None:
        source = hle_manifest.HLE_C.read_text(encoding="utf-8")
        a = json.dumps(build_census(source, top=30), sort_keys=True)
        b = json.dumps(build_census(source, top=30), sort_keys=True)
        self.assertEqual(a, b)


if __name__ == "__main__":
    unittest.main()
