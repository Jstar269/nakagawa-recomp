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
try:
    import scratch_dir
except ModuleNotFoundError:
    from tools import scratch_dir

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

    def test_ge_frontend_estimates_are_ranked_and_old_summaries_remain_valid(self):
        value = summary()
        stages = {
            "command_dispatch": {"estimated_ns": 12},
            "draw_setup": {"estimated_ns": 7},
            "vertex_fetch_decode": {"estimated_ns": 0},
            "transform": {"estimated_ns": 31},
            "lighting": {"estimated_ns": 0},
            "clipping_acceptance": {"estimated_ns": 4},
            "assembly": {"estimated_ns": 0},
        }
        value["ge"]["frontend_profile"] = {"stages": stages}
        ranking = perf_summary_diff.diff_summaries(value, value)["ranking"]
        self.assertEqual(
            [item["metric"] for item in ranking
             if item["metric"].startswith("ge.frontend_profile.")],
            ["ge.frontend_profile.stages.transform.estimated_ns",
             "ge.frontend_profile.stages.command_dispatch.estimated_ns",
             "ge.frontend_profile.stages.draw_setup.estimated_ns",
             "ge.frontend_profile.stages.clipping_acceptance.estimated_ns"],
        )
        old_ranking = perf_summary_diff.diff_summaries(summary(), summary())["ranking"]
        self.assertFalse(any(item["metric"].startswith("ge.frontend_profile.")
                             for item in old_ranking))

    def test_rejects_nonfinite_tolerance(self):
        before = summary()
        after = copy.deepcopy(before)
        after["guest"]["aot_ns"] += 1
        for tolerance in (float("inf"), float("nan")):
            with self.subTest(tolerance=tolerance):
                with self.assertRaisesRegex(perf_summary_diff.PerfSummaryError, "finite"):
                    perf_summary_diff.diff_summaries(before, after, tolerance)

    def test_rejects_nonfinite_metric(self):
        for metric, value in (("guest.aot_ns", float("nan")),
                              ("vulkan.wait_ns", float("inf"))):
            with self.subTest(metric=metric, value=value), tempfile.TemporaryDirectory() as temp:
                candidate = summary()
                group, field = metric.split(".")
                candidate[group][field] = value
                path = Path(temp) / "summary.json"
                path.write_text(json.dumps(candidate), encoding="utf-8")
                with self.assertRaisesRegex(perf_summary_diff.PerfSummaryError, "finite"):
                    perf_summary_diff.load_summary(path)

    def test_tolerance_handling_with_large_integers(self):
        before = summary()
        after = copy.deepcopy(before)
        before["guest"]["aot_ns"] = 10**20
        after["guest"]["aot_ns"] = 10**20 + 1000

        result = perf_summary_diff.diff_summaries(before, after, tolerance=1e-18)

        changed_paths = {change["path"] for change in result["changes"]}
        self.assertIn("$.guest.aot_ns", changed_paths)

    def test_tolerance_boundary_uses_the_decimal_value_typed(self):
        # 0.3 is not exact in binary. A change of exactly 30% of 10 is within --tolerance 0.3.
        for after_value in (13, 7):
            with self.subTest(after=after_value):
                before = summary()
                after = copy.deepcopy(before)
                before["guest"]["aot_ns"] = 10
                after["guest"]["aot_ns"] = after_value
                result = perf_summary_diff.diff_summaries(before, after, tolerance=0.3)
                self.assertNotIn("$.guest.aot_ns", {change["path"] for change in result["changes"]})

    def test_tolerance_handles_integers_beyond_float_range(self):
        before = summary()
        after = copy.deepcopy(before)
        before["guest"]["aot_ns"] = 10**400
        after["guest"]["aot_ns"] = 10**400 + 1
        close = perf_summary_diff.diff_summaries(before, after, tolerance=1e-18)
        self.assertNotIn("$.guest.aot_ns", {change["path"] for change in close["changes"]})
        after["guest"]["aot_ns"] = 10**400 + 10**390
        far = perf_summary_diff.diff_summaries(before, after, tolerance=1e-18)
        self.assertIn("$.guest.aot_ns", {change["path"] for change in far["changes"]})

    def test_zero_tolerance_preserves_adjacent_large_integer_differences(self):
        before = summary()
        after = copy.deepcopy(before)
        before["guest"]["aot_ns"] = 1 << 53
        after["guest"]["aot_ns"] = (1 << 53) + 1
        before["transitions"]["aot_to_interpreter_count"] = 1 << 53
        after["transitions"]["aot_to_interpreter_count"] = (1 << 53) + 1

        result = perf_summary_diff.diff_summaries(before, after)

        changed_paths = {change["path"] for change in result["changes"]}
        self.assertIn("$.guest.aot_ns", changed_paths)
        self.assertIn("$.transitions.aot_to_interpreter_count", changed_paths)

    def test_runtime_transition_summary_and_disabled_path(self):
        compiler = shutil.which("gcc")
        if compiler is None:
            self.skipTest("gcc is not available")
        with scratch_dir.TemporaryDirectory() as temp:
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

    def _run_aot_vfpu_harness(self, census: bool) -> dict:
        compiler = shutil.which("gcc")
        if compiler is None:
            self.skipTest("gcc is not available")
        # 4096 straight-line vadd.s words (opcode 0x18) at consecutive PCs, then 4096 vdot.q
        # (opcode 0x19) words each followed by a jump-away PC, then 8192 scalar words.
        harness = (
            "#include \"perf.h\"\n"
            "int main(void) {\n"
            "    sr_perf_init();\n"
            "    sr_perf_guest_begin();\n"
            "    sr_perf_aot_begin(0x1000u);\n"
            "    uint32_t pc = 0x1000u;\n"
            "    for (unsigned i = 0; i < 4096u; i++, pc += 4u) sr_perf_aot_instruction(pc, 0x60000000u);\n"
            "    for (unsigned i = 0; i < 4096u; i++) {\n"
            "        sr_perf_aot_instruction(0x20000u + i * 16u, 0x64008080u);\n"
            "        sr_perf_aot_instruction(0x40000u + i * 16u, 0x00000000u);\n"
            "    }\n"
            "    pc = 0x80000u;\n"
            "    for (unsigned i = 0; i < 8192u; i++, pc += 4u) sr_perf_aot_instruction(pc, 0x24020001u);\n"
            "    sr_perf_aot_end();\n"
            "    sr_perf_guest_end();\n"
            "    sr_perf_shutdown();\n"
            "    return 0;\n"
            "}\n"
        )
        with scratch_dir.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "aot_vfpu_harness.c"
            executable = root / ("aot_vfpu_harness.exe" if os.name == "nt" else "aot_vfpu_harness")
            source.write_text(harness, encoding="ascii")
            flags = ["-DPERF_AOT_INSTRUCTIONS=1"] if census else []
            built = subprocess.run(
                [compiler, "-std=c11", *flags, "-I", str(ROOT / "src" / "rt"), str(source),
                 str(ROOT / "src" / "rt" / "perf.c"), "-o", str(executable)],
                capture_output=True, text=True, check=False)
            self.assertEqual(built.returncode, 0, built.stderr)
            out = root / "summary.json"
            env = os.environ.copy()
            env.update({"SR_PERF": "1", "SR_PERF_JSON": str(out)})
            env.pop("SR_PERF_CSV", None)
            ran = subprocess.run([str(executable)], env=env, capture_output=True, text=True, check=False)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            value = json.loads(out.read_text(encoding="utf-8"))
        perf_summary_diff.validate_summary(value)
        return value

    def test_census_build_samples_native_aot_vfpu_time_per_family(self):
        value = self._run_aot_vfpu_harness(census=True)
        families = value["vfpu"]["aot_families"]
        self.assertEqual(families["status"], "sampled")
        rows = families["families"]
        self.assertEqual(rows["vfpu0"]["count"], 4096)
        self.assertEqual(rows["vfpu1"]["count"], 4096)
        self.assertEqual(sum(row["count"] for row in rows.values()), 8192)
        # Straight-line VFPU words are sampled once per stride; a word whose successor is not at
        # pc + 4 never yields a sample, so control flow cannot land in a family's time.
        self.assertEqual(rows["vfpu0"]["samples"], 4096 // families["stride"] - 1)
        self.assertEqual(rows["vfpu1"]["samples"], 0)
        self.assertEqual(rows["vfpu1"]["estimated_ns"], 0)
        self.assertGreater(families["baseline_samples"], 0)
        statuses = {row["metric"]: row["status"] for row in value["boundaries"]}
        self.assertEqual(statuses["aot_vfpu_time"], "sampled")

    def test_default_build_reports_aot_vfpu_time_not_compiled(self):
        value = self._run_aot_vfpu_harness(census=False)
        families = value["vfpu"]["aot_families"]
        self.assertEqual(families["status"], "not_compiled")
        statuses = {row["metric"]: row["status"] for row in value["boundaries"]}
        self.assertEqual(statuses["aot_vfpu_time"], "partial")

    def test_runtime_csv_exports_gpu_replay_counter_deltas(self):
        compiler = shutil.which("gcc")
        if compiler is None:
            self.skipTest("gcc is not available")
        names = ["queue_submits", "batches", "draws", "snapshot_requests", "snapshot_copies",
                 "upload_ring_reservations", "upload_ring_bytes", "upload_ring_wraps",
                 "upload_ring_fallbacks", "snapshot_copy_px", "fallback_prims",
                 "fallback_stencil", "fallback_color_test", "fallback_logic_op",
                 "fallback_write_mask", "fallback_ram_fb", "fallback_wide_stride",
                 "fallback_mips", "fallback_line", "fallback_point"]
        with scratch_dir.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "gpu_replay_harness.c"
            executable = root / ("gpu_replay_harness.exe" if os.name == "nt" else "gpu_replay_harness")
            # The reader stands in for the GPU backend's snapshot. The 100 queued before
            # sr_perf_init is the baseline, so the CSV must report only what moved after it.
            source.write_text(
                "#include \"perf.h\"\n"
                "static uint64_t counts[SR_PERF_GPU_REPLAY_COUNT];\n"
                "static void snapshot(uint64_t out[SR_PERF_GPU_REPLAY_COUNT]) {\n"
                "    for (unsigned i = 0; i < SR_PERF_GPU_REPLAY_COUNT; i++) out[i] = counts[i];\n"
                "}\n"
                "int main(void) {\n"
                "    counts[SR_PERF_GPU_QUEUE_SUBMITS] = 100;\n"
                "    sr_perf_set_gpu_replay_source(snapshot);\n"
                "    sr_perf_init();\n"
                "    counts[SR_PERF_GPU_QUEUE_SUBMITS] = 107;\n"
                "    counts[SR_PERF_GPU_DRAWS] = 3;\n"
                "    counts[SR_PERF_GPU_UPLOAD_RING_FALLBACKS] = 2;\n"
                "    counts[SR_PERF_GPU_SNAPSHOT_COPY_PX] = 4096;\n"
                "    counts[SR_PERF_GPU_FALLBACK_COLOR_TEST] = 5;\n"
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
            out_csv = root / "gpu.csv"
            env = os.environ.copy()
            env.update({"SR_PERF": "1", "SR_PERF_CSV": str(out_csv)})
            run = subprocess.run([str(executable)], env=env, capture_output=True, text=True,
                                 check=False)
            self.assertEqual(run.returncode, 0, run.stderr)
            with out_csv.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.reader(handle))
            header = rows[0]
            self.assertGreaterEqual(len(rows), 2)
            expected = {"queue_submits": 7, "batches": 0, "draws": 3, "snapshot_requests": 0,
                        "snapshot_copies": 0, "upload_ring_reservations": 0,
                        "upload_ring_bytes": 0, "upload_ring_wraps": 0,
                        "upload_ring_fallbacks": 2, "snapshot_copy_px": 4096,
                        "fallback_prims": 0, "fallback_stencil": 0, "fallback_color_test": 5,
                        "fallback_logic_op": 0, "fallback_write_mask": 0, "fallback_ram_fb": 0,
                        "fallback_wide_stride": 0, "fallback_mips": 0, "fallback_line": 0,
                        "fallback_point": 0}
            for name in names:
                column = f"gpu_replay_{name}"
                self.assertIn(column, header)
                # The shutdown row carries the interval counts; every row has the same width.
                self.assertEqual(int(rows[-1][header.index(column)]), expected[name], column)
            for row in rows[1:]:
                self.assertEqual(len(header), len(row))

    def test_gpu_replay_baseline_is_seeded_when_the_source_registers_after_init(self):
        compiler = shutil.which("gcc")
        if compiler is None:
            self.skipTest("gcc is not available")
        with scratch_dir.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "gpu_replay_late_harness.c"
            executable = root / ("gpu_replay_late_harness.exe" if os.name == "nt" else "gpu_replay_late_harness")
            # The CSV is opened by init, but the backend registers its reader afterwards (as
            # gegpu_init does). The 100 queued before registration must not show up as this
            # run's submits: registration seeds the baseline, so only the 7 after it count.
            source.write_text(
                "#include \"perf.h\"\n"
                "static uint64_t counts[SR_PERF_GPU_REPLAY_COUNT];\n"
                "static void snapshot(uint64_t out[SR_PERF_GPU_REPLAY_COUNT]) {\n"
                "    for (unsigned i = 0; i < SR_PERF_GPU_REPLAY_COUNT; i++) out[i] = counts[i];\n"
                "}\n"
                "int main(void) {\n"
                "    counts[SR_PERF_GPU_QUEUE_SUBMITS] = 100;\n"
                "    sr_perf_init();\n"
                "    sr_perf_set_gpu_replay_source(snapshot);\n"
                "    counts[SR_PERF_GPU_QUEUE_SUBMITS] = 107;\n"
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
            out_csv = root / "late.csv"
            env = os.environ.copy()
            env.update({"SR_PERF": "1", "SR_PERF_CSV": str(out_csv)})
            run = subprocess.run([str(executable)], env=env, capture_output=True, text=True,
                                 check=False)
            self.assertEqual(run.returncode, 0, run.stderr)
            with out_csv.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.reader(handle))
            column = rows[0].index("gpu_replay_queue_submits")
            self.assertEqual(int(rows[-1][column]), 7)


if __name__ == "__main__":
    unittest.main()
