#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from __future__ import annotations

import copy
import json
import re
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
                terminal_sequence=None, terminal_kind=0, terminal_arg0=0, version=1,
                triggers=None):
    if recorded is None:
        recorded = len(events) + dropped
    if terminal_sequence is None:
        terminal_sequence = events[-1]["sequence"] if events else 0
    if triggers is None:
        # validate_bundle pins fired to the terminal reason: a trigger-fired
        # bundle terminates at its trigger, an untriggered one at exit.
        triggers = {"first_fatal": True, "first_unsupported_nid": True,
                    "fired": 1 if terminal_reason != "exit" else 0}
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
        "runtime": {"name": "nakagawa-recomp", "cpu_state_abi": 3},
        "build": build,
        "recorder": {
            "enabled_classes": ["hle", "unsupported", "sched", "prx", "fault", "fatal", "media"]
            + (["ge", "present"] if version >= 4 else []),
            "limit": 4,
            "recorded": recorded,
            "dropped": dropped,
            "triggers": triggers,
        },
        "terminal": {
            "reason": terminal_reason,
            "sequence": terminal_sequence,
            "kind": terminal_kind,
            "arg0": terminal_arg0,
        },
        "events": events,
    }


def event(sequence, event_class, kind=1, arg0=0, arg1=0, arg2=0, arg3=0, version=1,
          arguments=None, return_value=None):
    record = {
        "schema_version": version,
        "sequence": sequence,
        "class": event_class,
        "kind": kind,
        "arg0": arg0,
        "arg1": arg1,
        "arg2": arg2,
        "arg3": arg3,
    }
    if arguments is not None:
        record["arguments"] = list(arguments)
        record["return_value"] = return_value
    return record


class ComparabilityContractTests(unittest.TestCase):
    """The #320 comparator contract: MATCH only for comparable, agreeing pairs.

    Every bundle below is individually schema-valid; the point is that validity
    alone must not decide MATCH. The three-way outcome model is MATCH /
    DIVERGENCE / INCOMPARABLE plus the pre-existing error path for malformed
    bundles, and the CLI maps them to exit codes 0 / 1 / 3 / 2.
    """

    def _comparable_pair(self):
        baseline = make_bundle(
            [event(1, "sched", arg0=1, version=3),
             event(2, "hle", arg0=2, version=3, arguments=[7, 0, 0, 0], return_value=None),
             event(3, "prx", arg0=3, version=3)],
            version=3,
        )
        return baseline, copy.deepcopy(baseline)

    def test_identical_fully_retained_bundles_match(self):
        baseline, candidate = self._comparable_pair()
        self.assertIsNone(flight_diff.diff_bundles(baseline, candidate, "sequence"))

    def test_differing_build_identities_still_compare(self):
        baseline, candidate = self._comparable_pair()
        candidate["build"]["build_id"] = "fedcba9876543210fedcba9876543210fedcba98"
        candidate["build"]["source_date_epoch"] = 1000000000
        candidate["build"]["compiler"] = "clang"
        self.assertIsNone(flight_diff.diff_bundles(baseline, candidate, "sequence"))

    def test_differing_recorder_limit_with_no_drops_still_compares(self):
        baseline, candidate = self._comparable_pair()
        candidate["recorder"]["limit"] = 4096
        self.assertIsNone(flight_diff.diff_bundles(baseline, candidate, "sequence"))

    def test_cli_reports_both_identities_on_match(self):
        baseline, candidate = self._comparable_pair()
        candidate["build"]["build_id"] = "fedcba9876543210fedcba9876543210fedcba98"
        result = self._run_cli(baseline, candidate)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("MATCH:", result.stdout)
        self.assertIn("identity baseline:", result.stdout)
        self.assertIn("identity candidate:", result.stdout)
        self.assertIn("fedcba98", result.stdout)

    def test_dropped_history_never_matches(self):
        baseline, candidate = self._comparable_pair()
        for bundle in (baseline, candidate):
            # Same retained events, same dropped count, same limit: the windows
            # are aligned but each side's dropped events came from a different
            # execution. Sequences 3..5 retained of 5 recorded, limit 3.
            bundle["recorder"]["dropped"] = 2
            bundle["recorder"]["recorded"] = 5
            bundle["recorder"]["limit"] = 3
            for item in bundle["events"]:
                item["sequence"] += 2
            bundle["terminal"]["sequence"] = 5
        for align in ("sequence", "class"):
            with self.subTest(align=align):
                with self.assertRaises(flight_diff.BundleIncomparable) as caught:
                    flight_diff.diff_bundles(baseline, candidate, align)
                self.assertTrue(
                    any("dropped" in reason for reason in caught.exception.reasons)
                )

    def test_dropped_history_violating_coverage_arithmetic_is_malformed(self):
        # The truncation invariant pins dropped to recorded - min(recorded,
        # limit); a bundle claiming a different dropped count does not describe
        # a ring-buffer capture at all, so it is an error, not INCOMPARABLE.
        # recorded 5 at limit 4 would permit dropped 1, so claim 2.
        baseline, candidate = self._comparable_pair()
        candidate["recorder"]["recorded"] = 5
        candidate["recorder"]["dropped"] = 2
        with self.assertRaisesRegex(flight_diff.FlightDiffError, "dropped must equal"):
            flight_diff.diff_bundles(baseline, candidate, "sequence")

    def test_incompatible_enabled_class_coverage_never_matches(self):
        baseline, candidate = self._comparable_pair()
        # The narrower candidate stays individually valid: sched-only classes
        # and two retained sched events with contiguous sequences.
        candidate["recorder"]["enabled_classes"] = ["sched"]
        candidate["events"] = [event(1, "sched", arg0=1, version=3),
                               event(2, "sched", arg0=2, version=3)]
        candidate["recorder"]["recorded"] = 2
        candidate["terminal"]["sequence"] = 2
        for align in ("sequence", "class"):
            with self.subTest(align=align):
                with self.assertRaises(flight_diff.BundleIncomparable) as caught:
                    flight_diff.diff_bundles(baseline, candidate, align)
                self.assertTrue(
                    any("classes" in reason for reason in caught.exception.reasons)
                )

    def test_incompatible_trigger_policy_never_matches(self):
        baseline, candidate = self._comparable_pair()
        baseline["recorder"]["triggers"]["first_unsupported_nid"] = False
        candidate["recorder"]["triggers"]["first_fatal"] = False
        with self.assertRaises(flight_diff.BundleIncomparable) as caught:
            flight_diff.diff_bundles(baseline, candidate, "sequence")
        self.assertTrue(any("trigger" in reason for reason in caught.exception.reasons))

    def test_triggered_bundles_with_identical_policy_stay_comparable(self):
        # A fired trigger is an outcome field, not part of the armed policy, so
        # it never blocks a comparison. A *differing* fired value is in fact
        # unreachable through the public API: validate_bundle pins fired to
        # "reason != exit", so any pair that differs in fired also differs in
        # terminal reason and diverges there instead (see the terminal-reason
        # test below). What is reachable -- and asserted here -- is a triggered
        # pair that shares the armed policy and diverges only in terminal kind.
        baseline = make_bundle(
            [event(1, "fatal", arg0=1, version=3)],
            terminal_reason="fatal",
            terminal_sequence=1,
            version=3,
        )
        candidate = copy.deepcopy(baseline)
        candidate["terminal"] = {
            "reason": "fatal", "sequence": 1, "kind": 11, "arg0": 7,
        }
        divergence = flight_diff.diff_bundles(baseline, candidate, "sequence")
        self.assertIsNotNone(divergence)
        self.assertEqual(divergence[0], "terminal kind")

    def test_same_events_different_terminal_semantics_diverge(self):
        baseline, candidate = self._comparable_pair()
        # Both terminals must be individually valid: a fatal outcome pairs with
        # a fired trigger, an exit outcome with an unfired one.
        candidate["terminal"] = {"reason": "fatal", "sequence": 3, "kind": 11, "arg0": 5}
        candidate["recorder"]["triggers"]["fired"] = 1
        for align in ("sequence", "class"):
            with self.subTest(align=align):
                divergence = flight_diff.diff_bundles(baseline, candidate, align)
                self.assertIsNotNone(divergence)
                self.assertEqual(divergence[0], "terminal reason")
                self.assertEqual(divergence[1]["reason"], "exit")
                self.assertEqual(divergence[2]["reason"], "fatal")

    def test_terminal_kind_and_arg0_divergences_are_named(self):
        baseline, candidate = self._comparable_pair()
        candidate["terminal"]["kind"] = 13
        divergence = flight_diff.diff_bundles(baseline, candidate, "sequence")
        self.assertEqual(divergence[0], "terminal kind")
        baseline2, candidate2 = self._comparable_pair()
        candidate2["terminal"]["arg0"] = 9
        divergence = flight_diff.diff_bundles(baseline2, candidate2, "sequence")
        self.assertEqual(divergence[0], "terminal arg0")

    def test_unfinished_running_capture_is_a_terminal_divergence(self):
        # A running terminal is only schema-valid beside a fired trigger (the
        # validator's own pairing): a mid-run snapshot dump whose run went on.
        baseline, candidate = self._comparable_pair()
        for bundle in (baseline, candidate):
            bundle["recorder"]["triggers"]["fired"] = 1
        baseline["terminal"] = {"reason": "running", "sequence": 0, "kind": 0, "arg0": 0}
        candidate["terminal"] = {"reason": "fatal", "sequence": 3, "kind": 11, "arg0": 5}
        divergence = flight_diff.diff_bundles(baseline, candidate, "sequence")
        self.assertIsNotNone(divergence)
        self.assertEqual(divergence[0], "terminal reason")
        self.assertEqual(divergence[1]["reason"], "running")
        self.assertEqual(divergence[2]["reason"], "fatal")

    def test_same_events_same_running_terminal_can_match(self):
        baseline, candidate = self._comparable_pair()
        for bundle in (baseline, candidate):
            bundle["recorder"]["triggers"]["fired"] = 1
            bundle["terminal"] = {"reason": "running", "sequence": 0, "kind": 0, "arg0": 0}
        self.assertIsNone(flight_diff.diff_bundles(baseline, candidate, "sequence"))

    def test_schema_version_mismatch_is_incomparable(self):
        baseline, candidate = self._comparable_pair()
        candidate = make_bundle(
            [event(1, "sched", arg0=1, version=2),
             event(2, "hle", arg0=2, version=2, arguments=[7, 0, 0, 0],
                   return_value=None),
             event(3, "prx", arg0=3, version=2)],
            version=2,
        )
        with self.assertRaises(flight_diff.BundleIncomparable) as caught:
            flight_diff.diff_bundles(baseline, candidate, "sequence")
        self.assertTrue(any("schema versions" in reason for reason in caught.exception.reasons))

    def test_runtime_block_mismatch_is_flagged_by_comparability(self):
        # The schema pins both runtime fields, so two schema-valid bundles can
        # never disagree here; the check is defense-in-depth for validators
        # that do not see the schema, and is asserted on the raw reasoning
        # function with minimal hand-built record shapes.
        left = {"schema_version": 3,
                "runtime": {"name": "nakagawa-recomp", "cpu_state_abi": 3},
                "recorder": {"enabled_classes": ["sched"], "dropped": 0,
                             "triggers": {"first_fatal": True,
                                          "first_unsupported_nid": True,
                                          "fired": 0}},
                "terminal": {"reason": "exit"}}
        right = {**left, "runtime": {"name": "nakagawa-recomp", "cpu_state_abi": 4}}
        reasons = flight_diff._comparability_reasons(left, right)
        self.assertTrue(any("runtime" in reason for reason in reasons))

    def test_incomparability_reasons_are_cumulative(self):
        baseline, candidate = self._comparable_pair()
        candidate["recorder"]["enabled_classes"] = ["sched", "hle", "prx"]
        # baseline: one event slid out of a limit-3 ring, recorded 4, kept 2..4.
        baseline["recorder"]["limit"] = 3
        baseline["recorder"]["dropped"] = 1
        baseline["recorder"]["recorded"] = 4
        for item in baseline["events"]:
            item["sequence"] += 1
        baseline["terminal"]["sequence"] = 4
        with self.assertRaises(flight_diff.BundleIncomparable) as caught:
            flight_diff.diff_bundles(baseline, candidate, "sequence")
        self.assertGreaterEqual(len(caught.exception.reasons), 2)

    def test_first_event_divergence_is_still_reported_first(self):
        baseline, candidate = self._comparable_pair()
        candidate["events"][2]["arg0"] = 0x99
        candidate["terminal"]["kind"] = 13  # would also diverge; events win
        divergence = flight_diff.diff_bundles(baseline, candidate, "sequence")
        self.assertIsNotNone(divergence)
        self.assertEqual(divergence[0], "sequence 3")
        self.assertEqual(divergence[1]["arg0"], 3)
        self.assertEqual(divergence[2]["arg0"], 0x99)

    def test_cli_distinguishes_incomparable_from_error_and_divergence(self):
        baseline, candidate = self._comparable_pair()
        # The incomparable candidate stays individually valid: only its enabled
        # classes differ, and its retained events all belong to them.
        candidate["recorder"]["enabled_classes"] = ["sched"]
        candidate["events"] = [event(1, "sched", arg0=1, version=3),
                               event(2, "sched", arg0=2, version=3),
                               event(3, "sched", arg0=3, version=3)]
        divergent = copy.deepcopy(baseline)
        divergent["events"][0]["arg0"] = 0x55
        malformed = copy.deepcopy(baseline)
        malformed["recorder"]["recorded"] = 99  # breaks retained+dropped
        incomparable = self._run_cli(baseline, candidate)
        divergence = self._run_cli(baseline, divergent)
        error = self._run_cli(baseline, malformed)
        self.assertEqual(incomparable.returncode, 3, incomparable.stdout)
        self.assertIn("INCOMPARABLE:", incomparable.stdout)
        self.assertIn("enabled event classes differ", incomparable.stdout)
        self.assertEqual(divergence.returncode, 1, divergence.stdout)
        self.assertIn("DIVERGENCE: sequence 1", divergence.stdout)
        self.assertEqual(error.returncode, 2, error.stderr)
        self.assertIn("error:", error.stderr)

    def test_cli_scopes_a_match_to_the_enabled_classes(self):
        # The MATCH line and the printed class list are what keep the verdict
        # readable as "these captured events and outcomes agree", not "the two
        # builds are equivalent".
        baseline, candidate = self._comparable_pair()
        for bundle in (baseline, candidate):
            bundle["recorder"]["enabled_classes"] = ["sched", "hle", "prx"]
        result = self._run_cli(baseline, candidate)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("MATCH:", result.stdout)
        self.assertIn("enabled classes: sched, hle, prx", result.stdout)
        self.assertIn("agrees on these enabled classes and the terminal outcome",
                      result.stdout)

    def _run_cli(self, left, right) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            left_path = temp_path / "left.json"
            right_path = temp_path / "right.json"
            left_path.write_text(json.dumps(left), encoding="utf-8")
            right_path.write_text(json.dumps(right), encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(ROOT / "tools" / "flight_diff.py"),
                 str(left_path), str(right_path)],
                capture_output=True, text=True, check=False,
            )


class GePresentFlightTests(unittest.TestCase):
    def _display_bundle(self):
        bundle = make_bundle(
            [
                event(1, "ge", kind=25, arg0=0x08900000, arg1=0x08900018,
                      arg2=6, arg3=2, version=4),
                event(2, "present", kind=29, arg0=0x04000000, arg1=3,
                      arg2=512, arg3=4, version=4),
            ],
            version=4,
        )
        bundle["recorder"]["enabled_classes"] = ["ge", "present"]
        return bundle

    def test_identical_ge_present_runs_match(self):
        baseline = self._display_bundle()
        candidate = copy.deepcopy(baseline)
        flight_diff.validate_bundle(baseline)
        result = self._run_cli(baseline, candidate)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("MATCH: 2 events aligned by sequence", result.stdout)

    def test_first_ge_draw_count_divergence_names_event_and_field(self):
        baseline = self._display_bundle()
        candidate = copy.deepcopy(baseline)
        candidate["events"][0]["arg3"] = 3
        result = self._run_cli(baseline, candidate)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("DIVERGENCE: sequence 1", result.stdout)
        self.assertIn("event index 0", result.stdout)
        self.assertIn("class=ge", result.stdout)
        self.assertIn("kind=ge-draw (25)", result.stdout)
        self.assertIn("arg3", result.stdout)

    def test_first_present_framebuffer_divergence_names_event_and_field(self):
        baseline = self._display_bundle()
        candidate = copy.deepcopy(baseline)
        candidate["events"][1]["arg0"] = 0x04044000
        result = self._run_cli(baseline, candidate)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("DIVERGENCE: sequence 2", result.stdout)
        self.assertIn("event index 1", result.stdout)
        self.assertIn("class=present", result.stdout)
        self.assertIn("kind=present-frame (29)", result.stdout)
        self.assertIn("arg0", result.stdout)

    def test_different_ge_present_coverage_is_incomparable(self):
        baseline = self._display_bundle()
        candidate = copy.deepcopy(baseline)
        candidate["recorder"]["enabled_classes"] = ["present"]
        candidate["events"] = [candidate["events"][1]]
        candidate["events"][0]["sequence"] = 1
        candidate["recorder"]["recorded"] = 1
        candidate["terminal"]["sequence"] = 1
        with self.assertRaises(flight_diff.BundleIncomparable) as caught:
            flight_diff.diff_bundles(baseline, candidate, "sequence")
        self.assertTrue(any("enabled event classes differ" in reason
                            for reason in caught.exception.reasons))

    def _run_cli(self, left, right):
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            left_path = temp_path / "left.json"
            right_path = temp_path / "right.json"
            left_path.write_text(json.dumps(left), encoding="utf-8")
            right_path.write_text(json.dumps(right), encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(ROOT / "tools" / "flight_diff.py"),
                 str(left_path), str(right_path)],
                capture_output=True, text=True, check=False,
            )


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


REFUSED_NID = 0x1579A159
SECOND_REFUSED_NID = 0x34B78343
REFUSAL_RETURN = 0x80110001


def v5_refusals(nids, *, unlisted=0, first_pc=0x08900100, first_sequence=2):
    """A schema-5 refusals block built from per-NID counts (first refusal first)."""
    count = sum(entry["count"] for entry in nids) + unlisted
    return {
        "count": count,
        "first_nid": nids[0]["nid"] if count else None,
        "first_pc": first_pc if count else None,
        "first_sequence": first_sequence if count else 0,
        "nids": nids,
        "nids_unlisted": unlisted,
    }


def v5_bundle(events, *, reason, kind=0, arg0=0, sequence=None, fired=0, refusals):
    bundle = make_bundle(
        events, recorded=len(events), terminal_reason=reason, terminal_sequence=sequence,
        terminal_kind=kind, terminal_arg0=arg0, version=5,
        triggers={"first_fatal": True, "first_unsupported_nid": True, "fired": fired},
    )
    bundle["refusals"] = refusals
    return bundle


def v5_hle(sequence, nid=REFUSED_NID):
    return event(sequence, "hle", kind=1, arg0=nid, arg1=0, arg2=0, arg3=0, version=5,
                 arguments=[0, 0, 0, 0], return_value=REFUSAL_RETURN)


def v5_refused(sequence, nid=REFUSED_NID, pc=0x08900100):
    return event(sequence, "unsupported", kind=2, arg0=nid, arg1=REFUSAL_RETURN, arg2=0,
                 arg3=pc, version=5)


class RefusalTerminalSchemaTests(unittest.TestCase):
    """Schema 5: a named refusal is a counted event, and the terminal is what ended the run."""

    def test_refusal_then_later_fatal_terminates_at_the_fatal(self):
        events = [
            v5_hle(1),
            v5_refused(2),
            event(3, "fatal", kind=12, arg0=0x08900100, arg1=0x0C, arg2=0, arg3=0, version=5),
        ]
        bundle = v5_bundle(events, reason="fatal", kind=12, arg0=0x0C, sequence=3, fired=1,
                           refusals=v5_refusals([{"nid": REFUSED_NID, "count": 1}]))
        flight_diff.validate_bundle(bundle)
        self.assertEqual(bundle["terminal"]["reason"], "fatal")
        self.assertEqual(bundle["refusals"]["first_nid"], REFUSED_NID)

    def test_refusal_then_clean_exit_terminates_at_the_exit(self):
        events = [v5_hle(1), v5_refused(2)]
        bundle = v5_bundle(events, reason="exit", arg0=7, sequence=2, fired=0,
                           refusals=v5_refusals([{"nid": REFUSED_NID, "count": 1}]))
        flight_diff.validate_bundle(bundle)
        self.assertEqual(bundle["terminal"]["reason"], "exit")

    def test_refusal_before_any_terminal_is_a_running_record(self):
        events = [v5_hle(1), v5_refused(2)]
        bundle = v5_bundle(events, reason="running", sequence=2, fired=0,
                           refusals=v5_refusals([{"nid": REFUSED_NID, "count": 1}]))
        flight_diff.validate_bundle(bundle)

    def test_budget_and_watchdog_hang_are_their_own_terminals(self):
        budget = v5_bundle([v5_hle(1)], reason="budget", arg0=41200, sequence=1, fired=0,
                           refusals=v5_refusals([]))
        flight_diff.validate_bundle(budget)
        hang_events = [
            v5_hle(1),
            event(2, "fatal", kind=17, arg0=0, arg1=600, arg2=600, arg3=0, version=5),
        ]
        hang = v5_bundle(hang_events, reason="hang", kind=17, arg0=600, sequence=2, fired=1,
                         refusals=v5_refusals([]))
        flight_diff.validate_bundle(hang)

    def test_budget_stop_cannot_be_a_fired_trigger(self):
        bundle = v5_bundle([v5_hle(1)], reason="budget", arg0=41200, sequence=1, fired=1,
                           refusals=v5_refusals([]))
        with self.assertRaisesRegex(flight_diff.FlightDiffError, "without a fired trigger"):
            flight_diff.validate_bundle(bundle)

    def test_watchdog_hang_must_be_a_fired_trigger(self):
        bundle = v5_bundle([v5_hle(1)], reason="hang", kind=17, arg0=600, sequence=1, fired=0,
                           refusals=v5_refusals([]))
        with self.assertRaisesRegex(flight_diff.FlightDiffError, "is a fired trigger"):
            flight_diff.validate_bundle(bundle)

    def test_refusal_counts_must_account_for_every_refusal(self):
        bogus = v5_refusals([{"nid": REFUSED_NID, "count": 1}])
        bogus["count"] = 2
        bundle = v5_bundle([v5_hle(1), v5_refused(2)], reason="exit", arg0=0, sequence=2,
                           fired=0, refusals=bogus)
        with self.assertRaisesRegex(flight_diff.FlightDiffError, "must account for every refusal"):
            flight_diff.validate_bundle(bundle)

    def test_unlisted_refusals_are_accounted_for(self):
        refusals = v5_refusals([{"nid": REFUSED_NID, "count": 1}], unlisted=2)
        bundle = v5_bundle([v5_hle(1), v5_refused(2)], reason="exit", arg0=0, sequence=2,
                           fired=0, refusals=refusals)
        flight_diff.validate_bundle(bundle)
        self.assertEqual(bundle["refusals"]["count"], 3)

    def test_refusal_with_no_first_nid_is_malformed(self):
        refusals = v5_refusals([])
        refusals["count"] = 1
        bundle = v5_bundle([v5_hle(1)], reason="exit", arg0=0, sequence=1, fired=0,
                           refusals=refusals)
        with self.assertRaisesRegex(flight_diff.FlightDiffError, "first refused NID"):
            flight_diff.validate_bundle(bundle)

    def test_schema_4_frozen_refusal_bundle_still_reads(self):
        # Pre-fix bundles froze at their first refusal; they keep validating, and carry no
        # refusals block of their own.
        events = [
            event(1, "hle", kind=1, arg0=REFUSED_NID, arg1=0, arg2=0, arg3=0, version=4,
                  arguments=[0, 0, 0, 0], return_value=REFUSAL_RETURN),
            event(2, "unsupported", kind=2, arg0=REFUSED_NID, arg1=REFUSAL_RETURN, arg2=0,
                  arg3=0x08900100, version=4),
        ]
        bundle = make_bundle(events, terminal_reason="unsupported-nid", terminal_kind=2,
                             terminal_arg0=REFUSED_NID, version=4)
        flight_diff.validate_bundle(bundle)
        self.assertNotIn("refusals", bundle)

    def test_schema_4_rejects_schema_5_terminals_and_blocks(self):
        bundle = make_bundle([event(1, "hle", kind=1, arg0=1, arg1=0, arg2=0, arg3=0, version=4,
                                    arguments=[0, 0, 0, 0], return_value=0)],
                             terminal_reason="exit", version=4)
        bundle["terminal"]["reason"] = "budget"
        with self.assertRaisesRegex(flight_diff.FlightDiffError, "terminal.reason"):
            flight_diff.validate_bundle(bundle)


class RuntimeAbiPinTests(unittest.TestCase):
    """The schema's ``cpu_state_abi`` pin follows the one header definition the recorder stamps.

    #812 moved CpuState to v3 while the recorder kept writing the literal 2 and the schema kept
    pinning 2, so flight_diff compared v2 and v3 bundles as the same runtime. The number has
    one source (src/rt/recomp.h); this test fails the moment the schema or a recorder literal
    drifts from it.
    """

    def test_schema_pins_the_header_version_and_the_recorder_stamps_the_macro(self):
        header = (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8")
        match = re.search(r"^#define SR_CPUSTATE_ABI_VERSION (\d+)u$", header, re.MULTILINE)
        self.assertIsNotNone(match, "recomp.h must define SR_CPUSTATE_ABI_VERSION <n>u")
        schema = json.loads(flight_diff.SCHEMA_PATH.read_text(encoding="utf-8"))
        pinned = schema["properties"]["runtime"]["properties"]["cpu_state_abi"]["const"]
        self.assertEqual(pinned, int(match.group(1)))
        recorder = (ROOT / "src" / "rt" / "flight_recorder.c").read_text(encoding="utf-8")
        self.assertIn('#include "recomp.h"', recorder)
        # The one and only mention of the field in the recorder is the %u stamp of the macro.
        self.assertIn('\\"cpu_state_abi\\": %u},\\n",', recorder)
        self.assertIn("(unsigned)SR_CPUSTATE_ABI_VERSION) >= 0;", recorder)
        self.assertEqual(recorder.count("cpu_state_abi"), 1, "a second cpu_state_abi token would be a literal")


if __name__ == "__main__":
    unittest.main()
