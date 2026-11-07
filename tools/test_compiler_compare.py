#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Contract tests for the GCC/Clang comparison and its correctness veto."""

from __future__ import annotations

import json
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compiler_compare  # noqa: E402


class FakeToolchain:
    """Fake compiler, linker and Make output without executing a build."""

    def __init__(self, *, available: tuple[str, ...] = ("gcc", "clang"),
                 failing_gate: str | None = None) -> None:
        self.available = available
        self.failing_gate = failing_gate
        self.workload_status = compiler_compare.GATE_PASS
        self.gate_calls: list[str] = []
        self.measurement_calls: list[tuple[str, Path]] = []
        self.workload_calls: list[dict[str, Any]] = []
        self.events: list[str] = []
        self.commands: list[list[str]] = []

    def which(self, name: str) -> str | None:
        if name in ("mingw32-make", "make"):
            return "/fake/bin/mingw32-make"
        linker_name = Path(name).name.lower()
        if name in ("ld", "ld.exe") or linker_name == "ld.exe":
            return "/fake/bin/ld.exe"
        if name in ("ld.lld", "ld.lld.exe") or linker_name == "ld.lld.exe":
            return "/fake/bin/ld.lld.exe"
        return f"/fake/bin/{name}.exe" if name in self.available else None

    def run(self, command: list[str], *, env: dict[str, str],
            cwd: Path = compiler_compare.ROOT) -> compiler_compare.Outcome:
        del env, cwd
        self.commands.append(list(command))
        name = Path(command[0]).name.lower()
        if "-print-prog-name=ld" in command:
            return compiler_compare.Outcome(0, "/fake/bin/ld.exe\n", 0.001)
        if "-dumpmachine" in command:
            return compiler_compare.Outcome(0, "x86_64-w64-mingw32\n", 0.001)
        if "--version" in command:
            return compiler_compare.Outcome(0, f"{name} fake version 1.0\nTarget: x86_64-w64-windows-gnu\n", 0.001)
        if "compiler-info" in command:
            assignments = {token.split("=", 1)[0]: token.split("=", 1)[1]
                           for token in command if "=" in token}
            cflags = assignments.get(
                "CFLAGS", "-O0 -fno-strict-aliasing -Isrc/rt -Isrc/core",
            )
            ldflags = assignments.get("LDFLAGS", "-lm")
            return compiler_compare.Outcome(
                0,
                f"CC={assignments.get('CC', 'fake')}\n"
                f"CFLAGS={cflags}\nGE_CFLAGS={assignments.get('GE_CFLAGS', '-O2')}\n"
                f"LDFLAGS={ldflags}\nLIBS=-lm\n"
                "RUNTIME_OPT=-O0\nRECOMP_OPT=-O0\nRECOMP_FLAGS=-O0\n"
                "CODEGEN_BACKEND=c\nCODEGEN_USER_ARGS=\nCOSIM_CODEGEN_ARGS=--stack-census\n"
                "SDL3_DIR=C:/msys64/ucrt64\n",
                0.001,
            )
        if Path(command[0]).name.lower().startswith("mingw32-make"):
            matches = [(name, target) for name, target in compiler_compare.GATE_CHAIN if target in command]
            if matches:
                name, _target = matches[0]
                self.gate_calls.append(name)
                self.events.append(f"gate:{name}")
                if name == self.failing_gate:
                    return compiler_compare.Outcome(1, f"FIRST ERROR: {name} semantic mismatch\n", 0.002)
                return compiler_compare.Outcome(0, f"{name}: PASS\n", 0.002)
            return compiler_compare.Outcome(0, "no-op\n", 0.001)
        return compiler_compare.Outcome(0, "", 0.001)

    def trials(self, compiler: str, linker: str, python: str, settings: dict[str, str],
               build_root: Path, repeats: int, *, env: dict[str, str]) -> list[dict[str, Any]]:
        del python, settings, env
        self.events.append("compile-measurements")
        self.measurement_calls.append((compiler, build_root))
        return [
            {
                "trial": index,
                "generated_c_compile_wall_seconds": 2.0 + index,
                "peak_working_set_bytes": 20_000_000 + index,
                "link_wall_seconds": 0.5 + index / 10,
                "object_bytes": 1000,
                "executable_bytes": 4000,
                "semantic_sha256": "a" * 64,
                "generated_c_commands": [{"compiler": compiler, "argv": ["-O0", "-c", "fixture_recomp_0.c"]}],
                "link_commands": [{"compiler": compiler, "argv": ["-O0", "-o", "fixture.exe"]}],
            }
            for index in range(1, repeats + 1)
        ]

    def workloads(self, compiler_path: str, make: str, python: str, settings: dict[str, str],
                  perf_dir: Path, build_prefix: Path, *,
                  env: dict[str, str]) -> dict[str, Any]:
        del make, python, env
        self.events.append("runtime-workloads")
        self.workload_calls.append({
            "compiler_path": compiler_path,
            "linker": settings["LINKER_SELECTION"],
            "build_prefix": build_prefix,
            "perf_dir": perf_dir,
            "cc": compiler_path,
            "ldflags": settings["LDFLAGS"],
        })
        return {
            "status": self.workload_status,
            "detail": "fake workload detail" if self.workload_status != compiler_compare.GATE_PASS else None,
            "workloads": [{
                "name": "audio", "targets": ["audio-selftest"],
                "status": self.workload_status, "duration_s": 1.25,
                "summary": "build/perf/audio/perf.json", "output": "fake workload output",
            }],
        }


class CompilerCompareTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.output = Path(self.temp.name) / "out"
        self.fake = FakeToolchain()
        patches: list[Any] = [
            mock.patch.object(compiler_compare.shutil, "which", self.fake.which),
            mock.patch.object(compiler_compare, "_run", self.fake.run),
            mock.patch.object(compiler_compare, "_measurement_trials", self.fake.trials),
            mock.patch.object(compiler_compare, "_workload_measurements", self.fake.workloads),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def compare(self, *, compilers: list[str] | None = None,
                linkers: list[str] | None = None) -> dict[str, Any]:
        return compiler_compare.compare(
            compilers or ["gcc"], linkers or ["default"], make="mingw32-make",
            python=sys.executable, output=self.output, repeats=3,
        )

    def test_settings_parser_keeps_exact_effective_flag_lines(self) -> None:
        parsed = compiler_compare._parse_settings(
            "build-profile bookkeeping\nCC=clang\nCFLAGS=-O0 -fno-fast-math\n"
            "LDFLAGS=-fuse-ld=lld\nCODEGEN_USER_ARGS=--omit-aot=0x08800000\n"
        )
        self.assertEqual(parsed["CC"], "clang")
        self.assertEqual(parsed["CFLAGS"], "-O0 -fno-fast-math")
        self.assertEqual(parsed["LDFLAGS"], "-fuse-ld=lld")
        self.assertEqual(parsed["CODEGEN_USER_ARGS"], "--omit-aot=0x08800000")

    def test_windows_make_paths_use_forward_slashes(self) -> None:
        self.assertEqual(
            compiler_compare._make_path(r"C:\msys64\ucrt64\bin\mingw32-make.exe"),
            "C:/msys64/ucrt64/bin/mingw32-make.exe",
        )
        self.assertEqual(
            compiler_compare._make_build_path(compiler_compare.ROOT / "build" / "cc317"),
            "build/cc317",
        )

    def test_first_error_prefers_compiler_and_source_owned_failure_lines(self) -> None:
        linker = "ld.exe: cannot find generated_fixture.o: No such file or directory\ncollect2.exe: error: ld returned 1"
        smoke = "PRODUCTION_SMOKE_VERIFY status=FAIL: link map omitted object\nmake: *** Error 1"
        self.assertEqual(compiler_compare._first_error(linker), linker.splitlines()[0])
        self.assertEqual(compiler_compare._first_error(smoke), smoke.splitlines()[0])

    def test_fast_math_and_reassociation_are_rejected(self) -> None:
        for flag in compiler_compare.FORBIDDEN_FP_FLAGS:
            with self.subTest(flag=flag), self.assertRaises(compiler_compare.CompareError):
                compiler_compare._fp_flags_ok({"CFLAGS": f"-O2 {flag}"})

    def test_ucrt_linker_may_live_in_the_target_specific_toolchain_subtree(self) -> None:
        compiler_compare._check_ucrt_paths(
            "C:/msys64/ucrt64/bin/gcc.exe",
            "C:/msys64/ucrt64/x86_64-w64-mingw32/bin/ld.exe",
            {"SDL3_PROVIDER": "msys2_ucrt64", "SDL3_DIR": "C:/msys64/ucrt64"},
        )

    def test_gcc_only_macro_tracking_flag_is_removed_for_controlled_comparison(self) -> None:
        flags = compiler_compare._comparison_recomp_flags(
            "-O0 -w -fno-strict-aliasing -ftrack-macro-expansion=0 "
            "-DSR_FLIGHT_RECORDER_LINKED",
        )
        self.assertNotIn(compiler_compare.GCC_ONLY_TRACKING_FLAG, flags)
        self.assertIn("-fno-strict-aliasing", flags)

    def test_optimization_level_is_pinned_after_existing_make_flags(self) -> None:
        flags = compiler_compare._set_optimization_level("-O2 -Wall -fno-strict-aliasing", "-O0")
        self.assertEqual(flags, "-Wall -fno-strict-aliasing -O0")

    def test_veto_precedes_every_measurement_and_runs_required_tests(self) -> None:
        report = self.compare()
        entry = report["toolchains"][0]
        self.assertEqual(entry["status"], compiler_compare.STATUS_MEASURED)
        self.assertEqual(self.fake.gate_calls, [name for name, _ in compiler_compare.GATE_CHAIN])
        self.assertEqual(len(self.fake.measurement_calls), 1)
        self.assertEqual(len(self.fake.workload_calls), 1)
        self.assertEqual(self.fake.events[-2:], ["compile-measurements", "runtime-workloads"])
        self.assertEqual(entry["veto"]["status"], "PASS")
        self.assertEqual(entry["measurements"]["repeats"], 3)
        self.assertEqual(entry["measurements"]["workloads_status"], compiler_compare.GATE_PASS)
        self.assertEqual(entry["build_settings"]["CONTROL_FLAGS"], " ".join(compiler_compare.CONTROL_FLAGS))
        self.assertIn("fake version", entry["compiler_version"])
        self.assertIn("fake version", entry["linker_version"])
        self.assertEqual(entry["compiler_target"], "x86_64-w64-mingw32")

    def test_failed_veto_stops_at_first_error_and_publishes_no_measurement(self) -> None:
        self.fake.failing_gate = "display-smoke"
        report = self.compare()
        entry = report["toolchains"][0]
        self.assertEqual(entry["status"], compiler_compare.STATUS_FAILED_VETO)
        self.assertEqual(entry["veto"]["failed_gate"], "display-smoke")
        self.assertIn("FIRST ERROR", entry["veto"]["gates"][-1]["output"])
        self.assertIn("FIRST ERROR", entry["veto"]["gates"][-1]["first_error"])
        self.assertIsNone(entry["measurements"])
        self.assertEqual(compiler_compare._timing_keys(entry), [])
        self.assertEqual(self.fake.measurement_calls, [])
        self.assertEqual(self.fake.workload_calls, [])
        self.assertEqual(len(self.fake.gate_calls), 3)

    def test_runtime_matrix_uses_each_toolchain_and_its_own_build_prefix(self) -> None:
        report = self.compare(compilers=["gcc", "clang"], linkers=["default", "lld"])
        self.assertEqual(len(self.fake.workload_calls), 4)
        prefixes = [call["build_prefix"] for call in self.fake.workload_calls]
        self.assertEqual(len(set(prefixes)), 4)
        for call in self.fake.workload_calls:
            self.assertEqual(call["cc"], call["compiler_path"])
            self.assertEqual(
                "-fuse-ld=lld" in call["ldflags"], call["linker"] == "lld",
            )
        self.assertEqual([entry["status"] for entry in report["toolchains"]],
                         [compiler_compare.STATUS_MEASURED] * 4)

    def test_failed_runtime_matrix_is_retained_after_the_veto(self) -> None:
        self.fake.workload_status = compiler_compare.GATE_FAIL
        report = self.compare()
        entry = report["toolchains"][0]
        self.assertEqual(entry["status"], compiler_compare.STATUS_WORKLOAD_FAILED)
        self.assertEqual(entry["measurements"]["workloads_status"], compiler_compare.GATE_FAIL)
        self.assertIn("fake workload detail", entry["measurements"]["workloads_detail"])
        self.assertIn("fake workload output", entry["measurements"]["workloads"][0]["output"])
        compiler_compare.validate_report(report)

    def test_skipped_runtime_workload_is_never_reported_as_measured(self) -> None:
        self.fake.workload_status = compiler_compare.GATE_SKIP
        report = self.compare()
        entry = report["toolchains"][0]
        self.assertEqual(entry["status"], compiler_compare.STATUS_WORKLOAD_FAILED)
        self.assertEqual(entry["measurements"]["workloads_status"], compiler_compare.GATE_SKIP)
        compiler_compare.validate_report(report)

    def test_veto_gate_order_includes_native_vfpu_and_exception_paths(self) -> None:
        self.assertEqual(compiler_compare.GATE_ORDER, (
            "production-smoke", "production-smoke-gap", "display-smoke", "cosim-selftest",
            "cosim-mutants", "native-core-tests", "vfpu-interp-selftest", "fp-convert-selftest",
            "cpu-lle-selftest", "stale-code-selftest",
        ))
        self.assertIn(("cosim-selftest", "cosim-selftest"), compiler_compare.GATE_CHAIN)
        self.assertIn(("display-smoke", "display-smoke-run"), compiler_compare.GATE_CHAIN)
        self.assertIn(("native-core-tests", "native-core-tests"), compiler_compare.GATE_CHAIN)
        self.assertIn(("vfpu-interp-selftest", "vfpu-interp-selftest"), compiler_compare.GATE_CHAIN)
        self.assertIn(("cpu-lle-selftest", "cpu-lle-selftest"), compiler_compare.GATE_CHAIN)

    def test_default_and_lld_are_distinct_toolchain_rows(self) -> None:
        report = self.compare(linkers=["default", "lld"])
        self.assertEqual([entry["name"] for entry in report["toolchains"]],
                         ["gcc/default", "gcc/lld"])
        self.assertEqual([entry["linker_selection"] for entry in report["toolchains"]],
                         ["default", "lld"])
        lld_settings = report["toolchains"][1]["build_settings"]
        self.assertIn("-fuse-ld=lld", lld_settings["LDFLAGS"])

    def test_hashes_are_compared_across_compiler_rows(self) -> None:
        report = self.compare(compilers=["gcc", "clang"])
        self.assertEqual(report["semantic_hash_comparison"]["status"], "MATCH")
        clang_settings = report["toolchains"][1]["build_settings"]
        self.assertEqual(clang_settings["RECOMP_CC_IS_CLANG_OVERRIDE"], "clang")

    def test_minimum_median_and_hash_stability_are_reported(self) -> None:
        report = self.compare()
        summary = report["toolchains"][0]["measurements"]["summary"]
        self.assertEqual(summary["generated_c_compile_wall_seconds"]["min"], 3.0)
        self.assertEqual(summary["generated_c_compile_wall_seconds"]["median"], 4.0)
        self.assertEqual(summary["semantic_sha256"], "a" * 64)
        self.assertTrue(summary["semantic_hash_stable"])

    def test_v2_report_keeps_runtime_workload_results_in_its_shape(self) -> None:
        report = self.compare()
        self.assertEqual(report["schema"], "nakagawa-compiler-compare-v2")
        self.assertIs(report["public_source_owned"], True)
        settings = report["toolchains"][0]["build_settings"]
        self.assertEqual(settings["CC"], "/fake/bin/gcc.exe")
        self.assertNotIn("build_profile.py", json.dumps(settings))
        measurements = report["toolchains"][0]["measurements"]
        self.assertEqual(sorted(measurements), [
            "repeats", "summary", "trials", "workloads", "workloads_detail", "workloads_status",
        ])
        self.assertEqual(measurements["workloads"][0]["name"], "audio")

    def test_report_shape_is_deterministic_across_runs(self) -> None:
        def shape(value):
            if isinstance(value, dict):
                return {key: shape(child) for key, child in sorted(value.items())}
            if isinstance(value, list):
                return [shape(child) for child in value]
            return type(value).__name__

        first = shape(self.compare())
        second = shape(self.compare())
        self.assertEqual(first, second)

    def test_report_validator_rejects_foreign_schema_unknown_and_out_of_order_gates(self) -> None:
        self.fake.failing_gate = "display-smoke"
        report = self.compare()
        gates = report["toolchains"][0]["veto"]["gates"]
        original_name = gates[0]["name"]
        gates.reverse()
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(report)
        gates.reverse()
        gates[0]["name"] = "not-a-veto-gate"
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(report)
        gates[0]["name"] = original_name
        report["toolchains"][0]["veto"]["failed_gate"] = "production-smoke"
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(report)
        report["toolchains"][0]["veto"]["failed_gate"] = gates[-1]["name"]
        report["schema"] = "foreign-schema"
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(report)

    def test_report_validator_rejects_timing_under_unavailable_compiler(self) -> None:
        self.fake.available = ()
        report = self.compare(compilers=["clang"])
        report["toolchains"][0]["duration_s"] = 1.0
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(report)

    def test_report_validator_rejects_missing_measurements_and_unavailable_veto(self) -> None:
        report = self.compare()
        measured = report["toolchains"][0]
        measured["measurements"] = None
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(report)

        report = self.compare()
        unavailable = report["toolchains"][0]
        unavailable["status"] = compiler_compare.STATUS_NOT_AVAILABLE
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(report)

    def test_missing_compiler_is_not_available_and_never_measured(self) -> None:
        self.fake.available = ("gcc",)
        report = self.compare(compilers=["clang"])
        entry = report["toolchains"][0]
        self.assertEqual(entry["status"], compiler_compare.STATUS_NOT_AVAILABLE)
        self.assertIsNone(entry["measurements"])
        self.assertEqual(self.fake.measurement_calls, [])

    def test_missing_compiler_can_be_reported_beside_a_measured_pair(self) -> None:
        report = self.compare(compilers=["gcc", "missing"])
        statuses = {entry["name"]: entry["status"] for entry in report["toolchains"]}
        self.assertEqual(statuses, {
            "gcc/default": compiler_compare.STATUS_MEASURED,
            "missing/default": compiler_compare.STATUS_NOT_AVAILABLE,
        })
        self.assertFalse(any("CC=missing" in command for command in self.fake.commands))

    def test_missing_make_is_an_error_and_never_a_measurement(self) -> None:
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.compare(["gcc"], ["default"], make="missing-make",
                                     python=sys.executable, output=self.output)
        self.assertEqual(self.fake.gate_calls, [])
        self.assertEqual(self.fake.measurement_calls, [])

    def test_report_validator_rejects_timing_leaked_under_failed_veto(self) -> None:
        self.fake.failing_gate = "cosim-selftest"
        report = self.compare()
        entry = report["toolchains"][0]
        entry["generated_c_compile_wall_seconds"] = 1.0
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(report)

    def test_report_markdown_has_summary_table_and_no_timing_for_failure(self) -> None:
        self.fake.failing_gate = "cosim-selftest"
        report = self.compare()
        rendered = compiler_compare.render_markdown(report)
        self.assertIn("| Compiler | Linker | Veto |", rendered)
        self.assertIn("FAILED_VETO", rendered)
        self.assertIn("cosim-selftest", rendered)
        self.assertIn("| — | — | — | — | — | — | — |", rendered)

    def test_main_writes_json_and_markdown_report_files(self) -> None:
        with mock.patch("sys.stdout", io.StringIO()):
            result = compiler_compare.main([
                "--compilers", "gcc", "--linkers", "default",
                "--make", "mingw32-make", "--python", sys.executable,
                "--repeats", "3", "--output", str(self.output),
            ])
        self.assertEqual(result, 0)
        report = json.loads((self.output / "compiler_compare.json").read_text(encoding="utf-8"))
        self.assertEqual(report["schema"], compiler_compare.SCHEMA)
        markdown = (self.output / "compiler_compare.md").read_text(encoding="utf-8")
        self.assertIn("Semantic SHA-256", markdown)

    def test_main_writes_veto_report_and_returns_failure(self) -> None:
        self.fake.failing_gate = "cosim-selftest"
        with mock.patch("sys.stdout", io.StringIO()):
            result = compiler_compare.main([
                "--compilers", "gcc", "--linkers", "default",
                "--make", "mingw32-make", "--python", sys.executable,
                "--repeats", "3", "--output", str(self.output),
            ])
        self.assertEqual(result, 1)
        report = json.loads((self.output / "compiler_compare.json").read_text(encoding="utf-8"))
        self.assertEqual(report["summary"][compiler_compare.STATUS_FAILED_VETO], ["gcc/default"])

    def test_main_missing_make_returns_two_without_a_report(self) -> None:
        original_which = self.fake.which

        def missing_make(name: str) -> str | None:
            return None if name == "mingw32-make" else original_which(name)

        with mock.patch.object(compiler_compare.shutil, "which", side_effect=missing_make), \
                mock.patch("sys.stderr", io.StringIO()):
            result = compiler_compare.main([
                "--compilers", "gcc", "--linkers", "default",
                "--make", "mingw32-make", "--python", sys.executable,
                "--repeats", "3", "--output", str(self.output),
            ])
        self.assertEqual(result, 2)
        self.assertFalse((self.output / "compiler_compare.json").exists())

    def test_report_reader_rejects_malformed_fake_compiler_output(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "events.jsonl"
            path.write_text("{not-json}\n", encoding="utf-8")
            with self.assertRaises(compiler_compare.CompareError):
                compiler_compare._read_events(path)

    def test_fake_compiler_events_parse_into_times_sizes_and_semantic_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "fixture.o").write_bytes(b"o" * 11)
            (root / "fixture.exe").write_bytes(b"e" * 17)
            events = [
                {
                    "category": "compile", "generated_c": True, "duration_s": 0.25,
                    "peak_working_set_bytes": 12345, "compiler": "gcc",
                    "argv": ["-O0", "-c", "fixture_recomp_0.c"],
                },
                {
                    "category": "link", "generated_c": False, "duration_s": 0.10,
                    "peak_working_set_bytes": 23456, "compiler": "gcc",
                    "argv": ["-O0", "-o", "fixture.exe", "fixture.o"],
                },
            ]
            event_file = root / "compiler_events.jsonl"
            event_file.write_text(
                "\n".join(json.dumps(event) for event in events) + "\n", encoding="utf-8",
            )
            parsed = compiler_compare._read_events(event_file)
            summary = compiler_compare._trial_summary(parsed, root, [("cosim-selftest", "PASS\n")])
        self.assertEqual(summary["generated_c_compile_wall_seconds"], 0.25)
        self.assertEqual(summary["peak_working_set_bytes"], 12345)
        self.assertEqual(summary["link_wall_seconds"], 0.10)
        self.assertEqual(summary["object_bytes"], 11)
        self.assertEqual(summary["executable_bytes"], 17)
        self.assertEqual(len(summary["semantic_sha256"]), 64)
        self.assertEqual(summary["compiler_commands"][0]["argv"][0], "-O0")
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare._trial_summary(
                parsed, root, [("cosim-selftest", "PASS\n")], linker="lld",
            )

    def test_trial_without_an_executable_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            events = [
                {"category": "compile", "generated_c": True, "duration_s": 0.25,
                 "peak_working_set_bytes": 12345, "compiler": "gcc", "argv": ["-c", "x_recomp.c"]},
                {"category": "link", "generated_c": False, "duration_s": 0.10,
                 "peak_working_set_bytes": 23456, "compiler": "gcc", "argv": ["-o", "x.exe"]},
            ]
            with self.assertRaisesRegex(compiler_compare.CompareError, "produced no executable"):
                compiler_compare._trial_summary(events, root, [("production-smoke", "PASS\n")])

    def test_compiler_argument_parser_identifies_generated_c_and_link_commands(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            for source_name in ("fixture_recomp.c", "fixture_recomp_0.c"):
                source = root / source_name
                source.write_text("void fixture(void) {}\n", encoding="utf-8")
                category, generated = compiler_compare._classify_compiler_args(
                    ["-O0", "-c", str(source), "-o", str(root / (source.stem + ".o"))], root,
                )
                self.assertEqual((category, generated), ("compile", True), source_name)
            category, generated = compiler_compare._classify_compiler_args(
                ["-O0", "-o", str(root / "fixture.exe"), str(root / "fixture.o")], root,
            )
            self.assertEqual((category, generated), ("link", False))

    def test_semantic_hash_normalizes_trial_build_roots(self) -> None:
        first = compiler_compare._semantic_digest([("display-smoke-run", "PASS: C:/root/a")],
                                                  Path("C:/root/a"))
        second = compiler_compare._semantic_digest([("display-smoke-run", "PASS: C:/root/b")],
                                                   Path("C:/root/b"))
        self.assertEqual(first, second)

    def test_semantic_hash_normalizes_relative_make_build_roots(self) -> None:
        first_root = compiler_compare.ROOT / "build" / "cc317" / "trial-1"
        second_root = compiler_compare.ROOT / "build" / "cc317" / "trial-2"
        first = compiler_compare._semantic_digest(
            [("production-smoke", "wrote build/cc317/trial-1/game.exe\nPASS\n")], first_root,
        )
        second = compiler_compare._semantic_digest(
            [("production-smoke", "wrote build/cc317/trial-2/game.exe\nPASS\n")], second_root,
        )
        self.assertEqual(first, second)

    def test_semantic_hash_ignores_host_addresses_and_timing_fields(self) -> None:
        first = "SR_HOST=0x00000277B35C4040 -> calling fn 00007FF763502801 aot_ms=111.519 encoder_write_ms=4242 status=PASS"
        second = "SR_HOST=0x000001A2B1B57040 -> calling fn 00007FF6BA002801 aot_ms=144.635 encoder_write_ms=4218 status=PASS"
        a = compiler_compare._semantic_digest([("production-smoke", first)], compiler_compare.ROOT / "build" / "a")
        b = compiler_compare._semantic_digest([("production-smoke", second)], compiler_compare.ROOT / "build" / "b")
        self.assertEqual(a, b)


class RuntimeWorkloadRunnerTests(unittest.TestCase):
    def _run_with_matrix(self, matrix: dict[str, Any] | None, captured: list[dict[str, Any]]):
        def fake_run(command: list[str], *, env: dict[str, str], **_kwargs):
            output_dir = Path(command[command.index("--output") + 1])
            captured.append({
                "command": command,
                "cc": env.get("CC"),
                "ldflags": env.get("LDFLAGS"),
                "cflags": env.get("CFLAGS"),
            })
            if matrix is not None:
                output_dir.mkdir(parents=True, exist_ok=True)
                (output_dir / "benchmark.json").write_text(json.dumps(matrix), encoding="utf-8")
            return compiler_compare.Outcome(0, "fake runner output\n", 0.1)

        return fake_run

    @staticmethod
    def _matrix() -> dict[str, Any]:
        return {
            "schema": compiler_compare.PERF_SCHEMA,
            "runs": [{
                "name": "audio", "targets": ["audio-selftest"], "status": "PASS",
                "duration_s": 1.25, "summary": "build/perf/audio/perf.json", "output": "PASS",
            }],
        }

    def test_runtime_matrix_inherits_compiler_flags_and_keeps_workload_times(self) -> None:
        captured: list[dict[str, Any]] = []
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            settings = {
                "CFLAGS": "-O0 -fno-fast-math", "LDFLAGS": "-lm -fuse-ld=lld",
                "RECOMP_CC_IS_CLANG_OVERRIDE": "clang", "LINKER_SELECTION": "lld",
            }
            with mock.patch.object(compiler_compare, "_run",
                                  self._run_with_matrix(self._matrix(), captured)):
                result = compiler_compare._workload_measurements(
                    "C:/msys64/ucrt64/bin/clang.exe", "C:/msys64/ucrt64/bin/mingw32-make.exe",
                    sys.executable, settings, root / "perf", root / "perf-build",
                    env={"PATH": "fake"},
                )
        self.assertEqual(result["status"], compiler_compare.GATE_PASS)
        self.assertEqual(result["workloads"][0]["duration_s"], 1.25)
        self.assertEqual(captured[0]["cc"], "C:/msys64/ucrt64/bin/clang.exe")
        self.assertIn("-fuse-ld=lld", captured[0]["ldflags"])
        self.assertIn("-fno-fast-math", captured[0]["cflags"])
        self.assertIn("--build-prefix", captured[0]["command"])
        make_args = [captured[0]["command"][index + 1]
                     for index, value in enumerate(captured[0]["command"][:-1])
                     if value == "--make-arg"]
        self.assertIn("CC=C:/msys64/ucrt64/bin/clang.exe", make_args)
        self.assertIn("LDFLAGS=-lm -fuse-ld=lld", make_args)

    def test_missing_runtime_matrix_report_fails_with_its_reason(self) -> None:
        captured: list[dict[str, Any]] = []
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "perf").mkdir()
            (root / "perf" / "benchmark.json").write_text('{"stale": true}', encoding="utf-8")
            with mock.patch.object(compiler_compare, "_run",
                                  self._run_with_matrix(None, captured)):
                result = compiler_compare._workload_measurements(
                    "C:/msys64/ucrt64/bin/gcc.exe", "C:/msys64/ucrt64/bin/mingw32-make.exe",
                    sys.executable, {"CFLAGS": "-O0", "LDFLAGS": "-lm"},
                    root / "perf", root / "perf-build",
                    env={"PATH": "fake"},
                )
        self.assertEqual(result["status"], compiler_compare.GATE_FAIL)
        self.assertIn("benchmark report absent", result["detail"])
        self.assertEqual(result["workloads"], [])


class TimingOnlyWorkloadTests(unittest.TestCase):
    """The platform ladder clears SR_*, so its benchmark runs are timed, not failed."""

    def _run(self, name: str) -> dict:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import run_perf_benchmarks
        done = subprocess.CompletedProcess(args=[], returncode=0, stdout="built and ran\n")
        with tempfile.TemporaryDirectory() as temp, \
                mock.patch.object(run_perf_benchmarks.subprocess, "run", return_value=done):
            return run_perf_benchmarks._run_one("make", sys.executable, name, (name,), Path(temp),
                                                True, "build/perf-benchmark")

    def test_a_ladder_workload_without_a_summary_is_timed_not_failed(self) -> None:
        for name in ("platform-ladder-gap", "platform-ladder-sched", "platform-ladder-fs"):
            record = self._run(name)
            self.assertEqual(record["status"], "PASS", name)
            self.assertIsNone(record["summary"])
            self.assertIn("SR_* namespace rule", record["note"])

    def test_any_other_workload_without_a_summary_still_fails(self) -> None:
        record = self._run("production-smoke-gap")
        self.assertEqual(record["status"], "FAIL")
        self.assertIn("SR_PERF summary was not produced", record["output"])
        self.assertNotIn("note", record)

    def test_every_timing_only_workload_is_a_real_ladder_workload(self) -> None:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import run_perf_benchmarks
        names = {name for name, _targets in run_perf_benchmarks.WORKLOADS}
        self.assertTrue(run_perf_benchmarks.TIMING_ONLY <= names)
        self.assertTrue(all(name.startswith("platform-ladder-") for name in run_perf_benchmarks.TIMING_ONLY))

    def test_runtime_runner_passes_explicit_compiler_and_linker_to_make(self) -> None:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import run_perf_benchmarks
        captured: list[list[str]] = []
        done = subprocess.CompletedProcess(args=[], returncode=0, stdout="built and ran\n")

        def fake_run(command, **_kwargs):
            captured.append(list(command))
            return done

        with tempfile.TemporaryDirectory() as temp, \
                mock.patch.object(run_perf_benchmarks.subprocess, "run", side_effect=fake_run):
            run_perf_benchmarks._run_one(
                "make", sys.executable, "audio", ("audio-selftest",), Path(temp), False,
                "build/cc317/gcc/lld", make_assignments=("CC=C:/msys64/ucrt64/bin/gcc.exe",
                                                        "LDFLAGS=-fuse-ld=lld"),
            )
        self.assertIn("CC=C:/msys64/ucrt64/bin/gcc.exe", captured[0])
        self.assertIn("LDFLAGS=-fuse-ld=lld", captured[0])

    def test_runtime_runner_cli_forwards_make_assignments(self) -> None:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import run_perf_benchmarks
        captured: list[tuple[str, ...]] = []

        def fake_run_one(*_args, make_assignments=(), **_kwargs):
            captured.append(make_assignments)
            return {"name": "audio", "status": "PASS"}

        with tempfile.TemporaryDirectory() as temp, \
                mock.patch.object(run_perf_benchmarks.shutil, "which", return_value="make"), \
                mock.patch.object(run_perf_benchmarks, "WORKLOADS", (("audio", ("audio-selftest",)),)), \
                mock.patch.object(run_perf_benchmarks, "_run_one", side_effect=fake_run_one):
            result = run_perf_benchmarks.main([
                "--make", "make", "--output", temp, "--make-arg", "CC=clang",
                "--make-arg", "LDFLAGS=-fuse-ld=lld",
            ])
        self.assertEqual(result, 0)
        self.assertEqual(captured, [("CC=clang", "LDFLAGS=-fuse-ld=lld")])


if __name__ == "__main__":
    unittest.main()
