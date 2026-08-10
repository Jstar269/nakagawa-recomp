#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Behavioral and wiring checks for the private GE fixture format."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

try:
    from tools.ge_replay_metrics import (
        CPU_PHASES, GE_CPU_PHASES, GE_PRIM_PROFILE_PHASES, HIERARCHY_FIELDS, WALL_PHASES,
        parse_cpu_profile, parse_ge_cpu_profile, parse_hierarchy,
        parse_hook_cpu_profile, parse_primitive_profile, parse_wall_profile,
    )
except ModuleNotFoundError:  # direct execution from tools/
    from ge_replay_metrics import (
        CPU_PHASES, GE_CPU_PHASES, GE_PRIM_PROFILE_PHASES, HIERARCHY_FIELDS, WALL_PHASES,
        parse_cpu_profile, parse_ge_cpu_profile, parse_hierarchy,
        parse_hook_cpu_profile, parse_primitive_profile, parse_wall_profile,
    )

ROOT = Path(__file__).resolve().parent.parent
RT = ROOT / "src" / "rt"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestGeCaptureC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        cls.tmp = Path(tempfile.mkdtemp(prefix="gecapture_"))
        cls.exe = cls.tmp / "ge_capture_selftest.exe"
        result = subprocess.run(
            [CC, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             f"-I{RT}", "-o", os.fspath(cls.exe),
             os.fspath(RT / "ge_capture_selftest.c"),
             os.fspath(RT / "ge_capture.c")],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise AssertionError("GE capture selftest did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_sparse_round_trip(self):
        fixture = self.tmp / "synthetic.ngef"
        result = subprocess.run(
            [os.fspath(self.exe), os.fspath(fixture)], capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ge capture selftest: OK", result.stdout)


class TestGeCaptureWiring(unittest.TestCase):
    def test_runtime_build_contains_capture_but_not_fixture(self):
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("src/rt/ge_capture.c", makefile)
        self.assertIn("ge-replay", makefile)
        tracked = subprocess.run(
            ["git", "ls-files", "*.ngef"], cwd=ROOT,
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        self.assertEqual(tracked, "", "private GE fixtures must never be tracked")

    def test_replay_links_production_optimized_ge_object(self):
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        replay_rule = makefile.split("ge-replay:", 1)[1].split("# selftest", 1)[0]
        self.assertIn("$(RT_GE_O)", replay_rule)
        self.assertNotIn("src/rt/ge.c", replay_rule)

    def test_ge_accesses_are_routed_through_first_touch_tracking(self):
        ge = (RT / "ge.c").read_text(encoding="utf-8")
        self.assertIn("g_ge_capture_active ? ge_capture_r32((a))", ge)
        self.assertIn("ge_capture_note_memory(so, w * bpp)", ge)
        self.assertIn("capture_boundary", ge)
        self.assertIn(
            "ge_capture_begin(s_capture_path, s_ge_frame, list_addr, &ge, s_zbuf)",
            ge,
        )
        self.assertIn("s_ge_frame >= s_capture_frame", ge)
        self.assertIn("s_ge_frame <= s_capture_frame_end", ge)
        self.assertIn("end_value < value", ge)

    def test_ge_rect_can_arm_dynamic_vertex_writer_watches(self):
        ge = (RT / "ge.c").read_text(encoding="utf-8")
        self.assertIn('getenv("SR_GE_ARM_RECT")', ge)
        self.assertIn('sr_add_mem_watch(va0, va0 + stride, "ge_rect_v0")', ge)
        self.assertIn('sr_add_mem_watch(va1, va1 + stride, "ge_rect_v1")', ge)
        self.assertIn("GE_ARM_RECT frame=", ge)
        self.assertIn("GE_RECT_WRITER frame=", ge)
        self.assertIn("sr_find_last_writer", ge)

    def test_async_submit_has_exact_fallback(self):
        gpu = (RT / "gpu_sdl3vk" / "ge_gpu.c").read_text(encoding="utf-8")
        self.assertIn('getenv("SR_GPU_SYNC_SUBMIT")', gpu)
        self.assertIn('getenv("SR_GPU_SUBMIT_BATCH")', gpu)
        self.assertIn("vkWaitForFences(s_dev, count, fences, VK_TRUE", gpu)
        self.assertIn("reason == SR_PERF_GE_SNAPSHOT_COPY", gpu)

    def test_batched_draws_use_distinct_vertex_arena_offsets(self):
        gpu = (RT / "gpu_sdl3vk" / "ge_gpu.c").read_text(encoding="utf-8")
        self.assertIn("VERT_ARENA_VERTS", gpu)
        self.assertIn("s_cmd_slot->vmap + vertex_base", gpu)
        self.assertIn("vertex_base * sizeof(GpuVert)", gpu)
        self.assertIn("s_cmd_slot->vused += s_nverts", gpu)

    def test_mixed_waits_and_transfer_boundaries_are_explicit(self):
        gpu = (RT / "gpu_sdl3vk" / "ge_gpu.c").read_text(encoding="utf-8")
        perf = (RT / "perf.h").read_text(encoding="utf-8")
        self.assertIn("SR_PERF_GE_MIXED_DRAIN", perf)
        self.assertIn("if (!cmd_batch_flush()) return 0;", gpu)
        self.assertIn("Presentation/readback command buffers are submitted outside", gpu)

    def test_replay_reports_exact_boundary_reasons(self):
        gpu = (RT / "gpu_sdl3vk" / "ge_gpu.c").read_text(encoding="utf-8")
        replay = (RT / "ge_replay.c").read_text(encoding="utf-8")
        header = (RT / "gpu_sdl3vk" / "ge_gpu.h").read_text(encoding="utf-8")
        for reason in ("TEXTURE_UPLOAD", "TARGET_UPLOAD", "DEPTH_UPLOAD", "READBACK",
                       "PRESENT", "LIFETIME", "OTHER"):
            self.assertIn(f"GEGPU_BOUNDARY_{reason}", header)
        self.assertIn("replay_note_submit", gpu)
        self.assertIn("replay_note_wait", gpu)
        self.assertIn("GE_REPLAY_BOUNDARY reason=", replay)

    def test_upload_ring_has_fenced_ranges_and_exact_fallback(self):
        gpu = (RT / "gpu_sdl3vk" / "ge_gpu.c").read_text(encoding="utf-8")
        self.assertIn('getenv("SR_GPU_XFER_RING_KB")', gpu)
        self.assertIn("if (value == 0) s_xfer_ring_bytes = 0", gpu)
        self.assertIn("optimalBufferCopyOffsetAlignment", gpu)
        self.assertIn("s_cmd_slot->xfer_used", gpu)
        self.assertIn("out->map = s_cmd_slot->xfer_map + offset", gpu)
        self.assertIn("out->buffer = s_xfer", gpu)
        self.assertIn("upload_ring_fallbacks", gpu)

    def test_cpu_profile_and_texture_shadow_have_exact_fallbacks(self):
        gpu = (RT / "gpu_sdl3vk" / "ge_gpu.c").read_text(encoding="utf-8")
        ge = (RT / "ge.c").read_text(encoding="utf-8")
        replay = (RT / "ge_replay.c").read_text(encoding="utf-8")
        self.assertIn('getenv("SR_GPU_CPU_PROFILE")', gpu)
        self.assertIn('env_enabled("SR_GE_REPLAY_WALL_PROFILE")', replay)
        self.assertIn('getenv("SR_GE_STRIP_CACHE_DISABLE")', ge)
        self.assertIn("strip_prev[0] = strip_prev[1]", ge)
        self.assertIn("shadow[invalidations=%llu checks=%llu hits=%llu", gpu)
        self.assertIn("stats_emit(1)", gpu)
        self.assertIn('getenv("SR_GPU_TEX_SHADOW_DISABLE")', gpu)
        self.assertIn("TEX_SHADOW_MAX_BYTES", gpu)
        self.assertIn("memcmp(e->shadow, src, e->bytes) == 0", gpu)
        self.assertIn("memcmp(e->shadow_clut, s_ge->clutram", gpu)
        for phase in CPU_PHASES:
            self.assertIn(f'"{phase}"', replay)
        self.assertIn('getenv("SR_GE_PRIM_PROFILE")', ge)
        self.assertIn('getenv("SR_GE_PRIM_PROFILE_STRIDE")', ge)
        self.assertIn('getenv("SR_GE_PRIM_CALIBRATION")', ge)
        self.assertIn("GE_REPLAY_GE_PRIM_PROFILE_TOTAL", replay)
        self.assertIn("GE_REPLAY_GE_PRIM_PROFILE_CONTROL", replay)
        for phase in GE_PRIM_PROFILE_PHASES:
            self.assertIn(f'"{phase}"', replay)

    def test_cpu_profile_parser_rejects_merged_or_missing_phases(self):
        lines = [f"GE_REPLAY_CPU phase={phase} calls=2 ns=30 ms=0.000030"
                 for phase in CPU_PHASES]
        lines.append("GE_REPLAY_CPU_COUNTS pipeline_hits=7 pipeline_misses=1")
        phases, counts = parse_cpu_profile("\n".join(lines))
        self.assertEqual(tuple(phases), CPU_PHASES)
        self.assertEqual(counts, {"pipeline_hits": 7, "pipeline_misses": 1})
        with self.assertRaisesRegex(ValueError, "duplicate CPU phase"):
            parse_cpu_profile("\n".join(lines + [lines[0]]))
        with self.assertRaisesRegex(ValueError, "CPU phase mismatch"):
            parse_cpu_profile("\n".join(lines[1:]))
        with self.assertRaisesRegex(ValueError, "CPU phase mismatch"):
            parse_cpu_profile(lines[-1])
        with self.assertRaisesRegex(ValueError, "missing CPU count summary"):
            parse_cpu_profile("\n".join(lines[:-1]))

    def test_wall_profile_parser_rejects_merged_missing_or_misclassified_phases(self):
        lines = [
            f"GE_REPLAY_WALL_PHASE phase={phase} classification={classification} "
            "calls=2 ns=30 ms=0.000030"
            for phase, classification in WALL_PHASES.items()
        ]
        phases = parse_wall_profile("\n".join(lines))
        self.assertEqual(tuple(phases), tuple(WALL_PHASES))
        with self.assertRaisesRegex(ValueError, "duplicate wall phase"):
            parse_wall_profile("\n".join(lines + [lines[0]]))
        with self.assertRaisesRegex(ValueError, "wall phase mismatch"):
            parse_wall_profile("\n".join(lines[1:]))
        bad = lines.copy()
        bad[0] = bad[0].replace("HARNESS-ONLY", "PRODUCTION-RELEVANT")
        with self.assertRaisesRegex(ValueError, "classification mismatch"):
            parse_wall_profile("\n".join(bad))

    def test_ge_cpu_profile_parser_rejects_merged_or_missing_phases(self):
        lines = [f"GE_REPLAY_GE_CPU phase={phase} calls=2 ns=30 ms=0.000030"
                 for phase in GE_CPU_PHASES]
        lines.append("GE_REPLAY_GE_CPU_COUNTS commands=7 primitive_commands=1")
        phases, counts = parse_ge_cpu_profile("\n".join(lines))
        self.assertEqual(tuple(phases), GE_CPU_PHASES)
        self.assertEqual(counts, {"commands": 7, "primitive_commands": 1})
        with self.assertRaisesRegex(ValueError, "duplicate GE CPU phase"):
            parse_ge_cpu_profile("\n".join(lines + [lines[0]]))
        with self.assertRaisesRegex(ValueError, "GE CPU phase mismatch"):
            parse_ge_cpu_profile("\n".join(lines[1:]))

    def test_primitive_profile_parser_rejects_merged_or_missing_phases(self):
        lines = ["GE_REPLAY_GE_PRIM_PROFILE_CONFIG stride=512 timer_pair_ns=12"]
        lines += [
            f"GE_REPLAY_GE_PRIM_PROFILE phase={phase} calls=2 ns=30 ms=0.000030 "
            "eligible=1024 estimated_ns=15360 estimated_ms=0.015360"
            for phase in GE_PRIM_PROFILE_PHASES
        ]
        lines.append(
            "GE_REPLAY_GE_PRIM_PROFILE_COUNTS vertices=1024 "
            "transform_vertices=768 triangle_candidates=512"
        )
        result = parse_primitive_profile("\n".join(lines))
        self.assertEqual(result["config"], {"stride": 512, "timer_pair_ns": 12})
        self.assertEqual(tuple(result["phases"]), GE_PRIM_PROFILE_PHASES)
        self.assertEqual(result["counts"]["vertices"], 1024)
        self.assertIsNone(result["population"])
        self.assertIsNone(result["type_counts"])
        self.assertIsNone(result["calibration"])
        with self.assertRaisesRegex(ValueError, "duplicate primitive profile phase"):
            parse_primitive_profile("\n".join(lines + [lines[1]]))
        with self.assertRaisesRegex(ValueError, "primitive profile phase mismatch"):
            parse_primitive_profile("\n".join(lines[:-2] + [lines[-1]]))
        bad = lines.copy()
        bad[2] = bad[2].replace("calls=2", "calls=2048")
        with self.assertRaisesRegex(ValueError, "samples > eligible"):
            parse_primitive_profile("\n".join(bad))

    def test_primitive_profile_parser_accepts_calibration(self):
        lines = ["GE_REPLAY_GE_PRIM_PROFILE_CONFIG stride=64 timer_pair_ns=12"]
        lines += [
            f"GE_REPLAY_GE_PRIM_PROFILE phase={phase} calls=2 ns=30 ms=0.000030 "
            "eligible=1024 estimated_ns=15360 estimated_ms=0.015360"
            for phase in GE_PRIM_PROFILE_PHASES
        ]
        lines += [
            "GE_REPLAY_GE_PRIM_PROFILE_CALIBRATION enabled=1 adjustment=none",
            "GE_REPLAY_GE_PRIM_PROFILE_CONTROL calls=10 ns=90 ms=0.000090 per_call_ns=9",
            "GE_REPLAY_GE_PRIM_PROFILE_TOTAL calls=5 ns=40 ms=0.000040 eligible=512 "
            "estimated_ns=4096 estimated_ms=0.004096",
            "GE_REPLAY_GE_PRIM_PROFILE_TOTAL_ADJUSTED estimated_ns=4096 estimated_ms=0.004096",
        ]
        lines += [
            f"GE_REPLAY_GE_PRIM_PROFILE_ADJUSTED phase={phase} "
            "estimated_ns=15360 estimated_ms=0.015360"
            for phase in GE_PRIM_PROFILE_PHASES
        ]
        lines += [
            "GE_REPLAY_GE_PRIM_PROFILE_POPULATION commands=28 submitted=90 "
            "vertex_references=72 triangle_vertex_references=48 "
            "non_triangle_vertex_references=24 vertex_uses=60 triangle_vertex_uses=36 "
            "non_triangle_vertex_uses=24 through_vertex_uses=20 "
            "transform_vertex_uses=40 actual_decoded_vertices=60 actual_transformed_vertices=40 "
            "actual_through_vertices=20 strip_cache_commands=2 strip_cache_hits=4 "
            "through_triangle_candidates=20 transform_triangle_candidates=35 "
            "transform_triangles_drawn=5 transform_triangles_clipped=1 "
            "transform_triangles_rejected=2 non_triangle_primitives=35 vertex_rejects=3 "
            "patch_commands=0 patch_control_vertices=0",
            "GE_REPLAY_GE_PRIM_PROFILE_TYPES commands_type0=1 commands_type1=2 "
            "commands_type2=3 commands_type3=4 commands_type4=5 commands_type5=6 "
            "commands_type6=7 commands_type7=0 submitted_type0=10 submitted_type1=10 "
            "submitted_type2=10 submitted_type3=20 submitted_type4=20 submitted_type5=15 "
            "submitted_type6=5 submitted_type7=0",
        ]
        lines.append(
            "GE_REPLAY_GE_PRIM_PROFILE_COUNTS vertices=1024 "
            "transform_vertices=768 triangle_candidates=512"
        )
        result = parse_primitive_profile("\n".join(lines))
        self.assertEqual(result["calibration"]["adjustment"], "none")
        self.assertEqual(result["population"]["vertex_references"], 72)
        self.assertEqual(result["type_counts"]["submitted_type4"], 20)
        self.assertEqual(result["calibration"]["control"]["calls"], 10)
        self.assertEqual(result["calibration"]["sampled_total"]["eligible"], 512)
        self.assertEqual(
            result["calibration"]["sampled_total"]["estimated_ns"],
            result["calibration"]["sampled_total_adjusted"]["estimated_ns"],
        )
        self.assertEqual(
            tuple(result["calibration"]["adjusted_phases"]), GE_PRIM_PROFILE_PHASES
        )

    def test_hook_profile_and_hierarchy_reconcile_strictly(self):
        hook_lines = [f"GE_REPLAY_HOOK_CPU phase={phase} calls=2 ns=30 ms=0.000030"
                      for phase in CPU_PHASES]
        hook_lines.append("GE_REPLAY_HOOK_COUNTS calls=2 submit_ns=3 wait_ns=4")
        phases, counts = parse_hook_cpu_profile("\n".join(hook_lines))
        self.assertEqual(tuple(phases), CPU_PHASES)
        self.assertEqual(counts["calls"], 2)
        with self.assertRaisesRegex(ValueError, "duplicate hook CPU phase"):
            parse_hook_cpu_profile("\n".join(hook_lines + [hook_lines[0]]))

        values = {field: 0 for field in HIERARCHY_FIELDS}
        values.update({
            "list_ns": 100, "command_ns": 10, "primitive_ns": 80,
            "primitive_frontend_ns": 30, "gpu_hook_ns": 50,
            "clut_ns": 5, "flush_ns": 2, "list_residual_ns": 3,
            "hook_renderer_ns": 30, "hook_submit_ns": 10,
            "hook_wait_ns": 5, "hook_residual_ns": 5,
        })
        line = "GE_REPLAY_HIERARCHY " + " ".join(
            f"{field}={values[field]}" for field in sorted(HIERARCHY_FIELDS)
        )
        self.assertEqual(parse_hierarchy(line), values)
        bad = line.replace("list_ns=100", "list_ns=101")
        with self.assertRaisesRegex(ValueError, "does not reconcile"):
            parse_hierarchy(bad)


if __name__ == "__main__":
    unittest.main()
