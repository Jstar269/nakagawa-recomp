#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

try:
    import flight_diff
except ModuleNotFoundError:
    from tools import flight_diff

ROOT = Path(__file__).resolve().parent.parent


def make_bundle(events, *, recorded=None, dropped=0, terminal_reason="exit",
                terminal_sequence=None, version=1):
    if recorded is None:
        recorded = len(events) + dropped
    if terminal_sequence is None:
        terminal_sequence = events[-1]["sequence"] if events else 0
    if version >= 3:
        build = {
            "compiler": "gcc",
            "source_date_epoch": 1758742400,
            "build_id": "0123456789abcdef0123456789abcdef01234567",
            "pointer_bits": 64,
        }
    else:
        build = {
            "compiler": "gcc",
            "compiled_date": "Sep 24 2026",
            "compiled_time": "12:34:56",
            "pointer_bits": 64,
        }
    return {
        "schema_version": version,
        "runtime": {"name": "nakagawa-recomp", "cpu_state_abi": 2},
        "build": build,
        "recorder": {
            "enabled_classes": ["hle", "unsupported", "sched", "prx", "fault", "fatal"],
            "limit": 4,
            "recorded": recorded,
            "dropped": dropped,
            "triggers": {
                "first_fatal": True,
                "first_unsupported_nid": True,
                "fired": 0,
            },
        },
        "terminal": {
            "reason": terminal_reason,
            "sequence": terminal_sequence,
            "kind": 0,
            "arg0": 0,
        },
        "events": events,
    }


def event(sequence, event_class, kind=1, arg0=0, arg1=0, arg2=0, arg3=0, version=1):
    return {
        "schema_version": version,
        "sequence": sequence,
        "class": event_class,
        "kind": kind,
        "arg0": arg0,
        "arg1": arg1,
        "arg2": arg2,
        "arg3": arg3,
    }


class FlightBundleTests(unittest.TestCase):
    def test_schema_accepts_source_safe_bundle(self):
        bundle = make_bundle(
            [
                event(1, "sched", arg0=0x10),
                event(2, "hle", arg0=0x1234),
                event(3, "prx", arg0=0x20),
                event(4, "fatal", arg0=0x30),
            ],
            terminal_reason="fatal",
            terminal_sequence=4,
        )
        bundle["recorder"]["triggers"]["fired"] = 1
        flight_diff.validate_bundle(bundle)

    def test_schema_v2_accepts_hle_return_value_and_diff_detects_changes(self):
        bundle = make_bundle([event(1, "hle", arg0=0x1234)])
        bundle["schema_version"] = 2
        bundle["events"][0]["schema_version"] = 2
        bundle["events"][0]["arguments"] = [1, 2, 3, 4]
        bundle["events"][0]["return_value"] = 0x80020001
        flight_diff.validate_bundle(bundle)

        candidate = copy.deepcopy(bundle)
        candidate["events"][0]["return_value"] = 0
        divergence = flight_diff.diff_bundles(bundle, candidate, "sequence")
        self.assertIsNotNone(divergence)
        self.assertEqual(divergence[0], "sequence 1")

    def test_schema_v2_requires_return_field_for_hle_import(self):
        bundle = make_bundle([event(1, "hle")])
        bundle["schema_version"] = 2
        bundle["events"][0]["schema_version"] = 2
        bundle["events"][0]["arguments"] = [0, 0, 0, 0]
        with self.assertRaisesRegex(flight_diff.FlightDiffError, "return_value"):
            flight_diff.validate_bundle(bundle)

    def test_schema_v2_rejects_hle_event_with_mismatched_version(self):
        bundle = make_bundle([event(1, "hle")])
        bundle["schema_version"] = 2
        bundle["events"][0]["arguments"] = [0, 0, 0, 0]
        bundle["events"][0]["return_value"] = None
        with self.assertRaisesRegex(flight_diff.FlightDiffError, "forbidden schema"):
            flight_diff.validate_bundle(bundle)

    def test_sanitizer_rejects_guest_derived_event_string(self):
        bundle = make_bundle([event(1, "hle")])
        bundle["events"][0]["guest_text"] = "private title payload"
        with self.assertRaisesRegex(flight_diff.FlightDiffError, "not allowed"):
            flight_diff.validate_bundle(bundle)

    def test_sanitizer_rejects_path_string_even_in_allowed_shape(self):
        bundle = make_bundle([event(1, "hle")])
        bundle["build"]["compiled_date"] = "C:/private/trace.txt"
        with self.assertRaises(flight_diff.FlightDiffError):
            flight_diff.validate_bundle(bundle)

    def test_sanitizer_rejects_path_string_in_build_id(self):
        bundle = make_bundle([event(1, "sched", version=3)], version=3)
        bundle["build"]["build_id"] = "C:/private/trace.txt"
        with self.assertRaises(flight_diff.FlightDiffError):
            flight_diff.validate_bundle(bundle)

    def test_schema_v2_clock_stamp_bundle_is_still_readable(self):
        bundle = make_bundle([event(1, "sched", version=2)], version=2)
        flight_diff.validate_bundle(bundle)

    def test_schema_v3_records_reproducible_build_identity(self):
        bundle = make_bundle([event(1, "hle", arg0=0x1234, version=3)], version=3)
        bundle["events"][0]["arguments"] = [0, 0, 0, 0]
        bundle["events"][0]["return_value"] = None
        self.assertEqual(bundle["build"]["source_date_epoch"], 1758742400)
        self.assertEqual(
            bundle["build"]["build_id"],
            "0123456789abcdef0123456789abcdef01234567",
        )
        flight_diff.validate_bundle(bundle)

    def test_schema_v3_rejects_compiled_clock_fields(self):
        bundle = make_bundle([event(1, "sched", version=3)], version=3)
        bundle["build"]["compiled_date"] = "Sep 24 2026"
        with self.assertRaises(flight_diff.FlightDiffError):
            flight_diff.validate_bundle(bundle)

    def test_schema_v1_rejects_reproducible_identity_fields(self):
        bundle = make_bundle([event(1, "sched")])
        bundle["build"]["source_date_epoch"] = 1758742400
        with self.assertRaises(flight_diff.FlightDiffError):
            flight_diff.validate_bundle(bundle)

    def test_schema_v3_requires_a_reproducible_identity(self):
        bundle = make_bundle([event(1, "sched", version=3)], version=3)
        bundle["build"]["source_date_epoch"] = None
        bundle["build"]["build_id"] = None
        with self.assertRaisesRegex(flight_diff.FlightDiffError, "reproducible build identity"):
            flight_diff.validate_bundle(bundle)

    def test_schema_itself_rejects_an_identity_less_v3_build(self):
        schema = json.loads(flight_diff.SCHEMA_PATH.read_text(encoding="utf-8"))
        bundle = make_bundle([event(1, "sched", version=3)], version=3)
        bundle["build"]["source_date_epoch"] = None
        bundle["build"]["build_id"] = None
        with self.assertRaises(flight_diff.FlightDiffError):
            flight_diff._validate_schema(bundle, schema)
        bundle["build"]["build_id"] = "0123456789abcdef"
        flight_diff._validate_schema(bundle, schema)

    def test_schema_v3_rejects_out_of_range_epoch(self):
        bundle = make_bundle([event(1, "sched", version=3)], version=3)
        bundle["build"]["source_date_epoch"] = -1
        with self.assertRaises(flight_diff.FlightDiffError):
            flight_diff.validate_bundle(bundle)

    def test_truncation_invariants_are_checked(self):
        bundle = make_bundle(
            [event(2, "sched"), event(3, "hle"), event(4, "prx")],
            recorded=4,
            dropped=2,
        )
        with self.assertRaisesRegex(flight_diff.FlightDiffError, "retained plus dropped"):
            flight_diff.validate_bundle(bundle)

    def test_sequence_diff_reports_first_wrong_event(self):
        baseline = make_bundle([event(1, "sched"), event(2, "hle"), event(3, "prx")])
        candidate = copy.deepcopy(baseline)
        candidate["events"][1]["arg0"] = 0x99
        divergence = flight_diff.diff_bundles(baseline, candidate, "sequence")
        self.assertIsNotNone(divergence)
        self.assertEqual(divergence[0], "sequence 2")
        self.assertEqual(divergence[1]["arg0"], 0)
        self.assertEqual(divergence[2]["arg0"], 0x99)

    def test_class_diff_pairs_repeated_events_by_class(self):
        baseline = make_bundle(
            [event(1, "sched", arg0=1), event(2, "sched", arg0=2), event(3, "hle", arg0=3)]
        )
        candidate = copy.deepcopy(baseline)
        candidate["events"][1]["arg0"] = 7
        divergence = flight_diff.diff_bundles(baseline, candidate, "class")
        self.assertIsNotNone(divergence)
        self.assertEqual(divergence[0], "class sched occurrence 1")

    def test_cli_returns_divergence_status(self):
        baseline = make_bundle([event(1, "sched")])
        candidate = copy.deepcopy(baseline)
        candidate["events"][0]["arg0"] = 4
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            left = temp_path / "left.json"
            right = temp_path / "right.json"
            left.write_text(json.dumps(baseline), encoding="utf-8")
            right.write_text(json.dumps(candidate), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools" / "flight_diff.py"), str(left), str(right)],
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("DIVERGENCE: sequence 1", result.stdout)


if __name__ == "__main__":
    unittest.main()
