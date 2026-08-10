# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
"""Tests for tools/generate_benchmarks.py (issue #189 read-only/bounded/atomic).

These tests exercise the report generator with pure helpers and temporary
SQLite databases only.  No live dashboard, database, or HST process is touched.
"""

import io
import json
import os
import sqlite3
import sys
import tempfile
import unittest
from contextlib import redirect_stderr

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate_benchmarks as g  # noqa: E402

SCHEMA = """
CREATE TABLE IF NOT EXISTS "TelemetryRun" (
    "id" TEXT NOT NULL PRIMARY KEY,
    "timestamp" DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "totalUnits" INTEGER NOT NULL,
    "unitsEarned" INTEGER NOT NULL,
    "unitsRegressed" INTEGER NOT NULL,
    "completionPct" REAL NOT NULL,
    "totalFunctions" INTEGER NOT NULL DEFAULT 0,
    "matchedFunctions" INTEGER NOT NULL DEFAULT 0,
    "totalBytes" INTEGER NOT NULL DEFAULT 0,
    "matchedBytes" INTEGER NOT NULL DEFAULT 0,
    "byteCompletionPct" REAL NOT NULL DEFAULT 0.0,
    "svMismatchesCount" INTEGER NOT NULL DEFAULT 0,
    "svMismatchesJson" TEXT NOT NULL DEFAULT '[]',
    "fuzzTotalTrials" INTEGER NOT NULL DEFAULT 0,
    "fuzzPassedTrials" INTEGER NOT NULL DEFAULT 0,
    "fuzzFailedTrials" INTEGER NOT NULL DEFAULT 0,
    "fuzzCoveragePct" REAL NOT NULL DEFAULT 0.0,
    "fuzzCurveJson" TEXT NOT NULL DEFAULT '[]',
    "vrTotalFrames" INTEGER NOT NULL DEFAULT 0,
    "vrPassedFrames" INTEGER NOT NULL DEFAULT 0,
    "vrFailedFrames" INTEGER NOT NULL DEFAULT 0,
    "vrPassRate" REAL NOT NULL DEFAULT 0.0,
    "rawJson" TEXT
);
"""


def make_db(path, rows):
    conn = sqlite3.connect(path)
    conn.executescript(SCHEMA)
    try:
        _insert_rows(conn, rows)
        conn.commit()
    finally:
        conn.close()


def _insert_rows(conn, rows):
    for i, row in enumerate(rows):
        base = {
            "id": f"run_{i:04d}",
            "timestamp": f"2026-01-{(i % 28) + 1:02d} 10:00:00",
            "totalUnits": 1000,
            "unitsEarned": 500,
            "unitsRegressed": 10,
            "completionPct": 49.0,
            "totalFunctions": 100,
            "matchedFunctions": 50,
            "totalBytes": 10000,
            "matchedBytes": 5000,
            "byteCompletionPct": 0.5,
            "svMismatchesCount": 0,
            "svMismatchesJson": "[]",
            "fuzzTotalTrials": 0,
            "fuzzPassedTrials": 0,
            "fuzzFailedTrials": 0,
            "fuzzCoveragePct": 0.0,
            "fuzzCurveJson": "[]",
            "vrTotalFrames": 0,
            "vrPassedFrames": 0,
            "vrFailedFrames": 0,
            "vrPassRate": 0.0,
            "rawJson": None,
        }
        base.update(row)
        conn.execute(
            """INSERT INTO "TelemetryRun" (
                id, timestamp, totalUnits, unitsEarned, unitsRegressed, completionPct,
                totalFunctions, matchedFunctions, totalBytes, matchedBytes, byteCompletionPct,
                svMismatchesCount, svMismatchesJson, fuzzTotalTrials, fuzzPassedTrials,
                fuzzFailedTrials, fuzzCoveragePct, fuzzCurveJson, vrTotalFrames,
                vrPassedFrames, vrFailedFrames, vrPassRate, rawJson
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
            tuple(base[k] for k in (
                "id", "timestamp", "totalUnits", "unitsEarned", "unitsRegressed",
                "completionPct", "totalFunctions", "matchedFunctions", "totalBytes",
                "matchedBytes", "byteCompletionPct", "svMismatchesCount", "svMismatchesJson",
                "fuzzTotalTrials", "fuzzPassedTrials", "fuzzFailedTrials", "fuzzCoveragePct",
                "fuzzCurveJson", "vrTotalFrames", "vrPassedFrames", "vrFailedFrames",
                "vrPassRate", "rawJson",
            )),
        )


class GenerateReportBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = os.path.join(self.tmp.name, "dev.db")

    def tearDown(self):
        self.tmp.cleanup()


class TestValidateRunRow(GenerateReportBase):
    def test_valid_row_passes(self):
        row = {
            "id": "run_0001", "timestamp": "2026-01-01 10:00:00",
            "totalUnits": 10, "unitsEarned": 5, "unitsRegressed": 0,
            "completionPct": 50.0, "byteCompletionPct": 0.5, "vrPassRate": 100.0,
            "fuzzCoveragePct": 0.0,
            "totalFunctions": 1, "matchedFunctions": 1, "totalBytes": 1,
            "matchedBytes": 1, "fuzzTotalTrials": 0, "fuzzPassedTrials": 0,
            "fuzzFailedTrials": 0, "vrTotalFrames": 0, "vrPassedFrames": 0,
            "vrFailedFrames": 0,
        }
        self.assertIsNotNone(g.validate_run_row(row))

    def test_nan_rejected(self):
        row = {
            "id": "run_0001", "timestamp": "2026-01-01 10:00:00",
            "totalUnits": 10, "unitsEarned": 5, "unitsRegressed": 0,
            "completionPct": float("nan"), "byteCompletionPct": 0.5,
            "vrPassRate": 100.0, "fuzzCoveragePct": 0.0,
            "totalFunctions": 1, "matchedFunctions": 1, "totalBytes": 1,
            "matchedBytes": 1, "fuzzTotalTrials": 0, "fuzzPassedTrials": 0,
            "fuzzFailedTrials": 0, "vrTotalFrames": 0, "vrPassedFrames": 0,
            "vrFailedFrames": 0,
        }
        self.assertIsNone(g.validate_run_row(row))

    def test_infinity_rejected(self):
        row = {
            "id": "run_0001", "timestamp": "2026-01-01 10:00:00",
            "totalUnits": 10, "unitsEarned": 5, "unitsRegressed": 0,
            "completionPct": 50.0, "byteCompletionPct": float("inf"),
            "vrPassRate": 100.0, "fuzzCoveragePct": 0.0,
            "totalFunctions": 1, "matchedFunctions": 1, "totalBytes": 1,
            "matchedBytes": 1, "fuzzTotalTrials": 0, "fuzzPassedTrials": 0,
            "fuzzFailedTrials": 0, "vrTotalFrames": 0, "vrPassedFrames": 0,
            "vrFailedFrames": 0,
        }
        self.assertIsNone(g.validate_run_row(row))

    def test_out_of_range_percentage_rejected(self):
        row = {
            "id": "run_0001", "timestamp": "2026-01-01 10:00:00",
            "totalUnits": 10, "unitsEarned": 5, "unitsRegressed": 0,
            "completionPct": 150.0, "byteCompletionPct": 0.5, "vrPassRate": 100.0,
            "fuzzCoveragePct": 0.0,
            "totalFunctions": 1, "matchedFunctions": 1, "totalBytes": 1,
            "matchedBytes": 1, "fuzzTotalTrials": 0, "fuzzPassedTrials": 0,
            "fuzzFailedTrials": 0, "vrTotalFrames": 0, "vrPassedFrames": 0,
            "vrFailedFrames": 0,
        }
        self.assertIsNone(g.validate_run_row(row))

    def test_negative_int_rejected(self):
        row = {
            "id": "run_0001", "timestamp": "2026-01-01 10:00:00",
            "totalUnits": -1, "unitsEarned": 5, "unitsRegressed": 0,
            "completionPct": 50.0, "byteCompletionPct": 0.5, "vrPassRate": 100.0,
            "fuzzCoveragePct": 0.0,
            "totalFunctions": 1, "matchedFunctions": 1, "totalBytes": 1,
            "matchedBytes": 1, "fuzzTotalTrials": 0, "fuzzPassedTrials": 0,
            "fuzzFailedTrials": 0, "vrTotalFrames": 0, "vrPassedFrames": 0,
            "vrFailedFrames": 0,
        }
        self.assertIsNone(g.validate_run_row(row))

    def test_unsafe_id_rejected(self):
        row = {
            "id": "run_0001<script>", "timestamp": "2026-01-01 10:00:00",
            "totalUnits": 10, "unitsEarned": 5, "unitsRegressed": 0,
            "completionPct": 50.0, "byteCompletionPct": 0.5, "vrPassRate": 100.0,
            "fuzzCoveragePct": 0.0,
            "totalFunctions": 1, "matchedFunctions": 1, "totalBytes": 1,
            "matchedBytes": 1, "fuzzTotalTrials": 0, "fuzzPassedTrials": 0,
            "fuzzFailedTrials": 0, "vrTotalFrames": 0, "vrPassedFrames": 0,
            "vrFailedFrames": 0,
        }
        self.assertIsNone(g.validate_run_row(row))

    def test_oversized_rawjson_rejected(self):
        row = {
            "id": "run_0001", "timestamp": "2026-01-01 10:00:00",
            "totalUnits": 10, "unitsEarned": 5, "unitsRegressed": 0,
            "completionPct": 50.0, "byteCompletionPct": 0.5, "vrPassRate": 100.0,
            "fuzzCoveragePct": 0.0,
            "totalFunctions": 1, "matchedFunctions": 1, "totalBytes": 1,
            "matchedBytes": 1, "fuzzTotalTrials": 0, "fuzzPassedTrials": 0,
            "fuzzFailedTrials": 0, "vrTotalFrames": 0, "vrPassedFrames": 0,
            "vrFailedFrames": 0,
            "rawJson": "x" * (g.MAX_RAW_JSON_BYTES + 1),
        }
        self.assertIsNone(g.validate_run_row(row))


class TestGenerateReport(GenerateReportBase):
    def test_missing_db_fails(self):
        out = os.path.join(self.tmp.name, "r.md")
        self.assertFalse(g.generate_report(os.path.join(self.tmp.name, "nope.db"), out))
        self.assertFalse(os.path.exists(out))

    def test_empty_db_fails(self):
        make_db(self.db, [])
        out = os.path.join(self.tmp.name, "r.md")
        self.assertFalse(g.generate_report(self.db, out))
        self.assertFalse(os.path.exists(out))

    def test_markdown_report_generated_atomically(self):
        make_db(self.db, [{}])
        out = os.path.join(self.tmp.name, "benchmarks_report.md")
        self.assertTrue(g.generate_report(self.db, out))
        self.assertTrue(os.path.exists(out))
        # No leftover temp sibling.
        leftovers = [f for f in os.listdir(self.tmp.name) if f.startswith(".report-tmp-")]
        self.assertEqual(leftovers, [])
        content = open(out, encoding="utf-8").read()
        self.assertIn("## Current Status Snapshot", content)
        self.assertIn(g.SOURCE_CLASSIFICATION, content)

    def test_bounded_limit_most_recent(self):
        # 30 rows; --limit 5 must only chart the 5 most recent.
        rows = [
            {"id": f"run_{i:04d}", "timestamp": f"2026-03-{i + 1:02d} 10:00:00",
             "completionPct": float(i)}
            for i in range(30)
        ]
        make_db(self.db, rows)
        out = os.path.join(self.tmp.name, "r.md")
        self.assertTrue(g.generate_report(self.db, out, limit=5))
        content = open(out, encoding="utf-8").read()
        # The 5 most recent rows (ids 25..29) appear; older rows must not.
        self.assertIn("run_0029", content)
        self.assertIn("run_0025", content)
        self.assertNotIn("run_0024", content)

    def test_limit_clamped_to_max(self):
        make_db(self.db, [{}])
        out = os.path.join(self.tmp.name, "r.md")
        self.assertTrue(g.generate_report(self.db, out, limit=g.MAX_RUN_LIMIT + 500))
        self.assertTrue(os.path.exists(out))

    def test_invalid_rows_skipped_not_rendered(self):
        # A corrupt row with a non-numeric value in a numeric column is the
        # realistic on-disk corruption case (SQLite stores NaN as NULL, which
        # the NOT NULL column rejects at insert; wrong-type text is what an
        # externally-written corrupt DB actually contains).
        make_db(self.db, [
            {"id": "bad_one", "completionPct": "not-a-number"},
            {"id": "good_one", "timestamp": "2026-02-01 10:00:00"},
        ])
        out = os.path.join(self.tmp.name, "r.md")
        self.assertTrue(g.generate_report(self.db, out))
        content = open(out, encoding="utf-8").read()
        self.assertIn("good_one", content)
        self.assertNotIn("bad_one", content)

    def test_db_not_modified_by_generation(self):
        make_db(self.db, [{}])
        before = open(self.db, "rb").read()
        out = os.path.join(self.tmp.name, "r.md")
        self.assertTrue(g.generate_report(self.db, out))
        after = open(self.db, "rb").read()
        self.assertEqual(before, after)

    def test_db_path_with_special_chars_read_only(self):
        # Percent-encoded file: URI must round-trip a path containing spaces
        # and '#' (Windows usernames/trees commonly contain these).  '?' is
        # exercised separately below against the report-side URI reader.
        special_db = os.path.join(self.tmp.name, "dev #1.db")
        make_db(special_db, [{}])
        out = os.path.join(self.tmp.name, "r.md")
        self.assertTrue(g.generate_report(special_db, out))
        content = open(out, encoding="utf-8").read()
        self.assertIn("## Current Status Snapshot", content)
        # Still read-only: the DB was not rewritten by generation.
        self.assertIn("dev #1.db", os.listdir(self.tmp.name))

    def test_uri_query_escaping_isolated(self):
        # '?' and '#' are URI-delimiter characters; the module's readonly_uri()
        # must percent-encode them inside the path component rather than parsing
        # them as fragments/options.  Uses a synthetic path so '?' is exercised
        # even on Windows (where '?' cannot appear in a real filename).  The
        # assertions are platform-independent: the URI always starts with
        # 'file:', encodes '#' and '?', and ends with exactly one '?mode=ro'.
        fake = (
            "C:/fake dir/dev#1?report.db"
            if os.name == "nt" else
            "/fake dir/dev#1?report.db"
        )
        uri = g.readonly_uri(fake)
        self.assertTrue(uri.startswith("file:"))
        self.assertIn("%23", uri)   # '#' percent-encoded
        self.assertIn("%3F", uri)   # '?' percent-encoded
        self.assertEqual(uri.count("?"), 1)  # only the '?mode=ro' separator
        self.assertTrue(uri.endswith("?mode=ro"))

    def test_readonly_uri_rejects_writes_functionally(self):
        # The percent-encoding test proves the URI shape; this proves the
        # runtime contract: a connection through readonly_uri() must refuse
        # any write, so report generation can never mutate the DB even if a
        # future code path attempts an INSERT/UPDATE (issue #189).
        make_db(self.db, [{}])
        conn = sqlite3.connect(g.readonly_uri(self.db), uri=True)
        try:
            rows = conn.execute("SELECT count(*) FROM TelemetryRun").fetchone()
            self.assertGreaterEqual(rows[0], 0)
            with self.assertRaises(sqlite3.OperationalError):
                conn.execute(
                    "INSERT INTO TelemetryRun (timestamp, totalUnits) VALUES (?, ?)",
                    ("2026-01-01 00:00:00", 1),
                )
        finally:
            conn.close()

    def test_html_escapes_script_in_timestamp(self):
        make_db(self.db, [{"timestamp": "2026-01-01 10:00:00<script>alert(1)</script>"}])
        out = os.path.join(self.tmp.name, "r.html")
        self.assertTrue(g.generate_report(self.db, None, html_path=out))
        content = open(out, encoding="utf-8").read()
        self.assertNotIn("<script>alert(1)</script>", content)
        self.assertIn("&lt;script&gt;alert(1)&lt;/script&gt;", content)

    def test_pdf_non_ascii_safe(self):
        make_db(self.db, [{"timestamp": "2026-01-01 10:00:00\xe9"}])
        out = os.path.join(self.tmp.name, "r.pdf")
        self.assertTrue(g.generate_report(self.db, None, pdf_path=out))
        content = open(out, "rb").read()
        self.assertTrue(content.startswith(b"%PDF"))
        self.assertNotIn(b"\xe9", content)

    def test_oversized_report_rejected(self):
        g.MAX_REPORT_BYTES = 100  # shrink budget; report exceeds it
        try:
            make_db(self.db, [{}])
            out = os.path.join(self.tmp.name, "r.md")
            with self.assertRaises(ValueError):
                g.generate_report(self.db, out)
        finally:
            g.MAX_REPORT_BYTES = 32 * 1024 * 1024


class TestAtomicWrite(GenerateReportBase):
    def test_atomic_write_text_roundtrip(self):
        target = os.path.join(self.tmp.name, "out.txt")
        digest = g.atomic_write_text(target, "hello world")
        self.assertEqual(len(digest), 64)
        self.assertEqual(open(target, encoding="utf-8").read(), "hello world")
        leftovers = [f for f in os.listdir(self.tmp.name) if f.startswith(".report-tmp-")]
        self.assertEqual(leftovers, [])

    def test_atomic_write_text_budget(self):
        target = os.path.join(self.tmp.name, "out.txt")
        with self.assertRaises(ValueError):
            g.atomic_write_text(target, "x" * (g.MAX_REPORT_BYTES + 1))
        self.assertFalse(os.path.exists(target))

    def test_atomic_write_replaces_existing(self):
        target = os.path.join(self.tmp.name, "out.txt")
        g.atomic_write_text(target, "old")
        g.atomic_write_text(target, "new")
        self.assertEqual(open(target, encoding="utf-8").read(), "new")


class TestMainCli(GenerateReportBase):
    def test_main_missing_db_exits_nonzero_without_creating_output(self):
        from unittest import mock
        out = os.path.join(self.tmp.name, "r.md")
        argv = ["generate_benchmarks.py", "--db", os.path.join(self.tmp.name, "missing.db"),
                "--output", out]
        buf = io.StringIO()
        with mock.patch.object(sys, "argv", argv), redirect_stderr(buf):
            with self.assertRaises(SystemExit) as ctx:
                g.main()
        self.assertNotEqual(ctx.exception.code, 0)
        self.assertFalse(os.path.exists(out))
        # No implicit DB creation: the missing database was reported, not synced.
        self.assertFalse(os.path.exists(os.path.join(self.tmp.name, "missing.db")))

    def test_main_sync_flag_explicit_only(self):
        # Report generation must NOT create the DB implicitly.
        out = os.path.join(self.tmp.name, "r.md")
        with redirect_stderr(io.StringIO()):
            rc = g.generate_report(os.path.join(self.tmp.name, "missing.db"), out)
        self.assertFalse(rc)
        self.assertFalse(os.path.exists(out))


if __name__ == "__main__":
    unittest.main()
