#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import copy
import csv
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

try:
    import perf_summary_diff
except ModuleNotFoundError:
    from tools import perf_summary_diff

ROOT = Path(__file__).resolve().parent.parent


def summary() -> dict:
    return {
        "schema": "nakagawa-perf-v1",
        "build": {"aot_instruction_hook": False},
        "wall_ns": 100,
        "interval_count": 1,
        "vblanks": {"count": 1, "presents": 1, "present_skips": 0},
        "guest": {
            "ns": 10,
            "idle_ns": 0,
            "aot_ns": 6,
            "aot_calls": 2,
            "aot_instruction_count": None,
            "interpreter_ns": 4,
            "interpreter_calls": 1,
            "interpreter_instructions": 3,
        },
        "transitions": {
            "aot_to_interpreter_count": 1,
            "interpreter_to_aot_count": 1,
            "top_pcs": {
                "aot_to_interpreter": [{"pc": "0x1000", "reason": "dispatch-miss", "count": 1}],
                "interpreter_to_aot": [{"pc": "0x2000", "reason": "aot-handoff", "count": 1}],
            },
        },
        "scheduler": {
            "running_ns": 8,
            "runnable_ns": 1,
            "blocked_ns": 0,
            "idle_ns": 1,
            "context_switches": 1,
            "blocked_transitions": 0,
            "wake_transitions": 0,
        },
        "vfpu": {
            "ns": 0,
            "count": 0,
            "interpreter_count": 0,
            "aot_instruction_count": 0,
            "interpreter_ns": 0,
            "aot_ns": 0,
            "errors": 0,
            "families": {"load": 0, "store": 0, "arithmetic": 0, "prefix": 0, "other": 0},
        },
        "ge": {"cpu_ns": 0, "cpu_calls": 0, "transform_sample_ns": 0, "primitive_ns": 0,
               "submits": 0, "waits": 0, "wait_ns": 0, "present_submits": 0,
               "present_waits": 0, "present_wait_ns": 0},
        "vulkan": {"submit_ns": 0, "submits": 0, "wait_ns": 0, "waits": 0,
                    "readback_ns": 0, "readbacks": 0, "readback_bytes": 0,
                    "pipeline_creation_ns": 0, "pipeline_creations": 0},
        "textures": {"decode_ns": 0, "decodes": 0, "decoded_bytes": 0,
                      "cache_hits": 0, "cache_misses": 0},
        "storage": {"iso_read_ns": 0, "iso_reads": 0, "iso_read_bytes": 0,
                     "iso_failures": 0, "vfs_read_ns": 0, "vfs_reads": 0,
                     "vfs_read_bytes": 0, "vfs_failures": 0},
        "media": {"h264_decode_ns": 0, "h264_decodes": 0, "h264_failures": 0,
                   "atrac_decode_ns": 0, "atrac_decodes": 0, "atrac_failures": 0},
        "audio": {"mix_ns": 0, "mix_calls": 0, "output_ns": 0, "output_calls": 0,
                   "output_frames": 0},
        "boundaries": [],
    }


class PerfSummaryDiffTests(unittest.TestCase):
    def test_diff_ignores_wall_and_interval_but_reports_metrics(self):
        before = summary()
        after = copy.deepcopy(before)
        after["wall_ns"] = 999999
        after["interval_count"] = 8
        after["guest"]["aot_ns"] = 7
        result = perf_summary_diff.diff_summaries(before, after)
        self.assertTrue(result["changed"])
        self.assertNotIn({"path": "$.wall_ns"}, result["changes"])
        self.assertIn("$.guest.aot_ns", {change["path"] for change in result["changes"]})

    def test_null_is_not_zero(self):
        before = summary()
        after = copy.deepcopy(before)
        after["guest"]["aot_instruction_count"] = 0
        result = perf_summary_diff.diff_summaries(before, after)
        self.assertTrue(result["changed"])
        self.assertIn("$.guest.aot_instruction_count", {change["path"] for change in result["changes"]})

    def test_transition_order_is_not_a_difference(self):
        before = summary()
        before["transitions"]["top_pcs"]["aot_to_interpreter"].append({
            "pc": "0x3000", "reason": "stale-block", "count": 1,
        })
        after = copy.deepcopy(before)
        after["transitions"]["top_pcs"]["aot_to_interpreter"].reverse()
        result = perf_summary_diff.diff_summaries(before, after)
        self.assertFalse(any(change["path"].startswith("$.transitions.top_pcs") for change in result["changes"]))

    def test_ranking_keeps_overlapping_metrics_separate(self):
        value = summary()
        value["guest"]["aot_ns"] = 0
        value["guest"]["interpreter_ns"] = 0
        value["scheduler"]["runnable_ns"] = 0
        value["scheduler"]["idle_ns"] = 0
        value["vulkan"]["wait_ns"] = 12
        value["vulkan"]["readback_ns"] = 8
        value["storage"]["vfs_read_ns"] = 4
        ranking = perf_summary_diff.diff_summaries(value, value)["ranking"]
        self.assertEqual([item["metric"] for item in ranking],
                         ["vulkan.wait_ns", "vulkan.readback_ns", "storage.vfs_read_ns"])

    def test_runtime_transition_summary_and_disabled_path(self):
        compiler = shutil.which("gcc")
        if compiler is None:
            self.skipTest("gcc is not available")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "perf_harness.c"
            executable = root / ("perf_harness.exe" if os.name == "nt" else "perf_harness")
            source.write_text(
                "#include \"perf.h\"\n"
                "int main(void) {\n"
                "    sr_perf_init();\n"
                "    sr_perf_guest_begin();\n"
                "    sr_perf_aot_begin(0x1000u);\n"
                "    sr_perf_interp_begin(0x2000u);\n"
                "    sr_perf_interp_instruction();\n"
                "    sr_perf_aot_begin(0x3000u);\n"
                "    sr_perf_aot_instruction(0x3000u, 0u);\n"
                "    sr_perf_aot_end();\n"
                "    sr_perf_interp_end(0x3000u, 1);\n"
                "    sr_perf_aot_end();\n"
                "    sr_perf_guest_end();\n"
                "    sr_perf_shutdown();\n"
                "    return 0;\n"
                "}\n",
                encoding="ascii",
            )
            compile_result = subprocess.run(
                [compiler, "-std=c11", "-I", str(ROOT / "src" / "rt"),
                 str(source), str(ROOT / "src" / "rt" / "perf.c"), "-o", str(executable)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            enabled_json = root / "enabled.json"
            enabled_csv = root / "enabled.csv"
            enabled_env = os.environ.copy()
            enabled_env.update({"SR_PERF": "1", "SR_PERF_JSON": str(enabled_json),
                                "SR_PERF_CSV": str(enabled_csv)})
            enabled = subprocess.run([str(executable)], env=enabled_env,
                                     capture_output=True, text=True, check=False)
            self.assertEqual(enabled.returncode, 0, enabled.stderr)
            value = json.loads(enabled_json.read_text(encoding="utf-8"))
            self.assertEqual(value["transitions"]["aot_to_interpreter_count"], 1)
            self.assertEqual(value["transitions"]["interpreter_to_aot_count"], 1)
            self.assertEqual(value["transitions"]["top_pcs"]["aot_to_interpreter"][0]["pc"], "0x00002000")
            self.assertEqual(value["transitions"]["top_pcs"]["interpreter_to_aot"][0]["pc"], "0x00003000")
            self.assertEqual(value["guest"]["interpreter_instructions"], 1)
            with enabled_csv.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.reader(handle))
            self.assertGreaterEqual(len(rows), 2)
            self.assertTrue(rows[0])
            for row in rows[1:]:
                self.assertEqual(len(rows[0]), len(row))
            disabled_json = root / "disabled.json"
            disabled_csv = root / "disabled.csv"
            disabled_env = os.environ.copy()
            disabled_env.update({"SR_PERF": "0", "SR_PERF_JSON": str(disabled_json),
                                 "SR_PERF_CSV": str(disabled_csv)})
            disabled = subprocess.run([str(executable)], env=disabled_env,
                                      capture_output=True, text=True, check=False)
            self.assertEqual(disabled.returncode, 0, disabled.stderr)
            self.assertFalse(disabled_json.exists())
            self.assertFalse(disabled_csv.exists())

        value = summary()
        value["schema"] = "other"
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "bad.json"
            path.write_text(json.dumps(value), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools" / "perf_summary_diff.py"), str(path), str(path)],
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(result.returncode, 2)
        self.assertIn("expected 'nakagawa-perf-v1'", result.stderr)


if __name__ == "__main__":
    unittest.main()
