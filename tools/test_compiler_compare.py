#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The compiler comparison must not publish a number its veto did not earn.

Issue #317 is only answerable if a candidate compiler is shown to preserve the
accepted guest contract before its speed is measured, so these tests pin the
ordering itself rather than the timings: the cosimulation and stale-code /
exception gates decide the outcome, a failed gate names itself and suppresses
every timing, a compiler missing from PATH is reported as such, and the report
shape is stable enough to diff between runs.
"""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

import compiler_compare  # noqa: E402

MAKE = "mingw32-make"
#: Stand-ins for what a real corpus build would leave behind.
FAKE_EXE = b"ELFfake"
FAKE_OBJECT_BYTES = 32
GATE_TARGETS = {name: target for name, target, _ in compiler_compare.GATE_CHAIN}


def _matrix(*statuses: str) -> dict:
    return {
        "schema": compiler_compare.PERF_SCHEMA,
        "public_source_owned": True,
        "runs": [
            {
                "name": name,
                "targets": [name],
                "status": status,
                "returncode": 0 if status == "PASS" else 1,
                "duration_s": 1.5 + index,
                "summary": f"build/compiler-compare/x/perf/{name}/perf.json",
                "output": f"{name}: SR_PERF summary was not produced",
            }
            for index, (name, status) in enumerate(zip(("cosim", "audio"), statuses, strict=False))
        ],
    }


class _Fake:
    """Subprocess, PATH and clock doubles; no build and no compiler runs."""

    def __init__(self, *, available: tuple[str, ...] = ("gcc", "clang"),
                 failing_gate: str | None = None,
                 matrix: dict | None = None,
                 make_missing: bool = False,
                 matrix_report: bool = True) -> None:
        self.available = available
        self.failing_gate = failing_gate
        self.matrix = _matrix("PASS", "PASS") if matrix is None else matrix
        self.make_missing = make_missing
        self.matrix_report = matrix_report
        self.commands: list[list[str]] = []

    def which(self, name: str) -> str | None:
        if name == MAKE:
            return None if self.make_missing else f"/usr/bin/{MAKE}"
        return f"/usr/bin/{name}" if name in self.available else None

    @staticmethod
    def _arg(command: list[str], flag: str) -> str | None:
        """Value of a make ``NAME=value`` assignment or of a ``--flag value`` pair."""
        if flag.endswith("="):
            for token in command:
                if token.startswith(flag):
                    return token[len(flag):]
            return None
        return command[command.index(flag) + 1] if flag in command else None

    def run(self, command: list[str], **kwargs) -> subprocess.CompletedProcess:  # noqa: ARG002
        self.commands.append(list(command))
        name = Path(command[0]).name
        if name == MAKE or name == "make" or name == "gmake":
            return self._make(list(command))
        if self._is_perf(command):
            output = Path(self._arg(command, "--output") or "")
            if self.matrix_report:
                output.mkdir(parents=True, exist_ok=True)
                (output / "benchmark.json").write_text(json.dumps(self.matrix), encoding="utf-8")
            return subprocess.CompletedProcess(command, 0, "ok")
        if "--version" in command:
            return subprocess.CompletedProcess(command, 0, f"{name} (fake) 0.0\n")
        return subprocess.CompletedProcess(command, 0, "")

    def _make(self, command: list[str]) -> subprocess.CompletedProcess:
        if "compiler-info" in command:
            # Real `compiler-info` echoes one KEY=value line per setting and also
            # triggers build-profile bookkeeping; only the echoes belong in a report.
            return subprocess.CompletedProcess(
                command, 0,
                "python tools/build_profile.py record --section runtime\n"
                "CC=fake\nCFLAGS=-O2\nSDL3_DIR=C:/fake\n")
        for gate, target in GATE_TARGETS.items():
            if target not in command:
                continue
            if gate == compiler_compare.CORPUS_GATE:
                corpus = Path(self._arg(command, "PRODUCTION_SMOKE_GAP_DIR=") or "")
                corpus.mkdir(parents=True, exist_ok=True)
                (corpus / f"{compiler_compare.CORPUS_GAME}.exe").write_bytes(FAKE_EXE)
                (corpus / "chunk.o").write_bytes(b"\x00" * FAKE_OBJECT_BYTES)
            if gate == self.failing_gate:
                return subprocess.CompletedProcess(command, 1, f"{gate}: FAIL\n")
            return subprocess.CompletedProcess(command, 0, f"{gate}: PASS\n")
        return subprocess.CompletedProcess(command, 0, "")

    @staticmethod
    def _is_make(command: list[str]) -> bool:
        return Path(command[0]).name in ("make", "gmake", MAKE, f"{MAKE}.EXE")

    @staticmethod
    def _is_perf(command: list[str]) -> bool:
        return any(token.endswith("run_perf_benchmarks.py") for token in command)

    def make_calls(self) -> list[list[str]]:
        return [command for command in self.commands if self._is_make(command)]

    def gate_calls(self) -> list[list[str]]:
        """make calls that run a veto gate, excluding the `compiler-info` probe."""
        return [command for command in self.make_calls()
                if any(target in command for target in GATE_TARGETS.values())]

    def perf_calls(self) -> list[list[str]]:
        return [command for command in self.commands if self._is_perf(command)]

    def index_of(self, command: list[str]) -> int:
        return self.commands.index(command)


class _Harness:
    """Run `compare` against the doubles, yielding the report and the fake.

    Only subprocess and PATH are doubled; durations come from the real clock,
    because "some positive wall time was measured" is all the contract claims.
    """

    def __init__(self, case: unittest.TestCase, tmp: Path, fake: _Fake) -> None:
        self.fake = fake
        for target, attribute, replacement in (
            (compiler_compare.subprocess, "run", fake.run),
            (compiler_compare.shutil, "which", fake.which),
        ):
            patch = mock.patch.object(target, attribute, replacement)
            patch.start()
            case.addCleanup(patch.stop)
        self.output = tmp / "build" / "compiler-compare"

    def compare(self, compilers: list[str]) -> dict:
        return compiler_compare.compare(compilers, make=MAKE, python=sys.executable, output=self.output)


class _CompareCase:
    """Mixin that runs `compare` against the doubles inside a scratch tree."""

    def harness(self, **kwargs) -> _Harness:
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        return _Harness(self, Path(holder.name), _Fake(**kwargs))


class TestVetoOrdering(_CompareCase, unittest.TestCase):
    def test_a_passing_veto_records_the_timings_it_earned(self) -> None:
        harness = self.harness()
        entry = harness.compare(["gcc"])["compilers"][0]
        self.assertEqual(entry["status"], compiler_compare.MEASURED)
        self.assertIsNone(entry["veto"]["failed_gate"])
        self.assertGreater(entry["measurements"]["build_seconds"], 0.0)
        self.assertEqual(entry["measurements"]["executable_bytes"], len(FAKE_EXE))
        self.assertEqual(entry["measurements"]["object_bytes"], FAKE_OBJECT_BYTES)
        self.assertEqual(entry["measurements"]["workloads_status"], "PASS")

    def test_the_matrix_runs_only_after_every_gate_passes(self) -> None:
        harness = self.harness()
        harness.compare(["gcc"])
        self.assertEqual(len(harness.fake.perf_calls()), 1)
        last_gate = harness.fake.gate_calls()[-1]
        self.assertIn(compiler_compare.GATE_ORDER[-1], last_gate)
        self.assertGreater(harness.fake.index_of(harness.fake.perf_calls()[0]),
                           harness.fake.index_of(last_gate))

    def test_a_failing_gate_suppresses_the_matrix_and_every_timing(self) -> None:
        harness = self.harness(failing_gate="cosim-mutants")
        report = harness.compare(["gcc"])
        entry = report["compilers"][0]
        self.assertEqual(entry["status"], compiler_compare.VETOED)
        self.assertEqual(entry["veto"]["failed_gate"], "cosim-mutants")
        self.assertIsNone(entry["measurements"])
        self.assertEqual(compiler_compare._timing_keys(entry), [])
        self.assertEqual(harness.fake.perf_calls(), [])

    def test_the_chain_stops_at_the_first_failing_gate(self) -> None:
        harness = self.harness(failing_gate="cosim-selftest")
        gates = harness.compare(["gcc"])["compilers"][0]["veto"]["gates"]
        self.assertEqual([gate["name"] for gate in gates], ["corpus-build", "cosim-selftest"])
        self.assertEqual(len(harness.fake.gate_calls()), 2)

    def test_a_failing_corpus_build_is_its_own_veto_gate(self) -> None:
        harness = self.harness(failing_gate="corpus-build")
        entry = harness.compare(["gcc"])["compilers"][0]
        self.assertEqual(entry["status"], compiler_compare.VETOED)
        self.assertEqual(entry["veto"]["failed_gate"], "corpus-build")
        self.assertEqual([gate["name"] for gate in entry["veto"]["gates"]], ["corpus-build"])

    def test_the_gates_run_in_the_fixed_documented_order(self) -> None:
        harness = self.harness()
        gates = harness.compare(["gcc"])["compilers"][0]["veto"]["gates"]
        self.assertEqual([gate["name"] for gate in gates],
                         ["corpus-build", "cosim-selftest", "cosim-mutants",
                          "stale-code-selftest", "cpu-lle-selftest"])

    def test_each_compiler_gets_its_own_build_root_and_workload_prefix(self) -> None:
        harness = self.harness()
        report = harness.compare(["gcc", "clang"])
        roots = [entry["build_root"] for entry in report["compilers"]]
        self.assertEqual(len(set(roots)), 2)
        prefixes = [call[call.index("--build-prefix") + 1] for call in harness.fake.perf_calls()]
        self.assertEqual(len(set(prefixes)), 2)
        for prefix, root in zip(prefixes, roots, strict=True):
            self.assertTrue(prefix.startswith(root.replace("\\", "/")), prefix)

    def test_a_missing_executable_fails_the_gate_without_a_timing(self) -> None:
        harness = self.harness(make_missing=True)
        with self.assertRaises(compiler_compare.CompareError):
            harness.compare(["gcc"])


class TestStatusVocabulary(_CompareCase, unittest.TestCase):
    def test_a_missing_compiler_is_not_available_and_never_a_pass(self) -> None:
        harness = self.harness(available=("gcc",))
        report = harness.compare(["gcc", "clang"])
        unavailable = report["compilers"][1]
        self.assertEqual(unavailable["name"], "clang")
        self.assertEqual(unavailable["status"], compiler_compare.NOT_AVAILABLE)
        self.assertIsNone(unavailable["veto"])
        self.assertIsNone(unavailable["measurements"])
        self.assertEqual(compiler_compare._timing_keys(unavailable), [])
        self.assertEqual(report["summary"][compiler_compare.NOT_AVAILABLE], ["clang"])
        self.assertEqual(report["summary"][compiler_compare.MEASURED], ["gcc"])
        # An absent compiler is never handed to make.
        self.assertFalse([call for call in harness.fake.make_calls() if "CC=clang" in call])
        self.assertTrue(harness.fake.gate_calls())

    def test_a_vetoed_compiler_is_reported_and_the_run_fails(self) -> None:
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        harness = _Harness(self, Path(holder.name), _Fake(failing_gate="cosim-selftest"))
        self.assertEqual(compiler_compare.main(["--compilers", "gcc", "--output", str(harness.output)]), 1)
        report = json.loads((harness.output / "compiler_compare.json").read_text(encoding="utf-8"))
        self.assertEqual(report["summary"][compiler_compare.VETOED], ["gcc"])

    def test_a_missing_make_is_an_error_not_a_pass(self) -> None:
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        missing = _Fake(make_missing=True)
        harness = _Harness(self, Path(holder.name), missing)
        self.assertEqual(compiler_compare.main(["--compilers", "gcc", "--output", str(harness.output)]), 2)
        self.assertFalse((harness.output / "compiler_compare.json").exists())

    def test_a_failing_workload_matrix_is_not_reported_as_measured(self) -> None:
        harness = self.harness(matrix=_matrix("PASS", "FAIL"))
        entry = harness.compare(["gcc"])["compilers"][0]
        self.assertEqual(entry["status"], compiler_compare.WORKLOAD_FAILED)
        self.assertEqual(entry["veto"]["failed_gate"], None)
        self.assertEqual(entry["measurements"]["workloads_status"], "FAIL")

    def test_a_failing_workload_keeps_the_evidence_for_its_failure(self) -> None:
        harness = self.harness(matrix=_matrix("PASS", "FAIL"))
        entry = harness.compare(["gcc"])["compilers"][0]
        failed = [run for run in entry["measurements"]["workloads"] if run["status"] == "FAIL"]
        self.assertEqual([run["name"] for run in failed], ["audio"])
        self.assertIn("SR_PERF summary was not produced", failed[0]["output"])

    def test_the_recorded_settings_are_the_echoed_ones_not_the_build_bookkeeping(self) -> None:
        harness = self.harness()
        entry = harness.compare(["gcc"])["compilers"][0]
        settings = entry["build_settings"]
        self.assertIn("CC=fake", settings)
        self.assertNotIn("build_profile.py", settings)
        self.assertEqual(entry["version"], "gcc (fake) 0.0")

    def test_a_skipped_workload_is_carried_through_as_skipped(self) -> None:
        harness = self.harness(matrix=_matrix("SKIP", "PASS"))
        entry = harness.compare(["gcc"])["compilers"][0]
        self.assertEqual(entry["status"], compiler_compare.MEASURED)
        self.assertEqual([run["status"] for run in entry["measurements"]["workloads"]], ["SKIP", "PASS"])

    def test_a_missing_benchmark_report_fails_the_matrix(self) -> None:
        harness = self.harness(matrix_report=False)
        entry = harness.compare(["gcc"])["compilers"][0]
        self.assertEqual(entry["status"], compiler_compare.WORKLOAD_FAILED)
        self.assertIn("benchmark report absent", entry["measurements"]["workloads_detail"])


class TestReportShape(unittest.TestCase):
    """The report is the evidence issue #317 rests on; its shape must be stable."""

    def _report(self, root: Path | None = None, **kwargs) -> dict:
        with tempfile.TemporaryDirectory() as tmp:
            fake = _Fake(**kwargs)
            with mock.patch.object(compiler_compare.subprocess, "run", fake.run), \
                    mock.patch.object(compiler_compare.shutil, "which", fake.which):
                report = compiler_compare.compare(["gcc", "clang"], make=MAKE,
                                                  python=sys.executable,
                                                  output=root or Path(tmp) / "out")
        compiler_compare.validate_report(report)
        return report

    @staticmethod
    def _shape(value, path: str = "$"):
        """The key/type skeleton of a report, independent of any measured value."""
        if isinstance(value, dict):
            return {key: TestReportShape._shape(child, f"{path}.{key}")
                    for key, child in sorted(value.items())}
        if isinstance(value, list):
            return {"list": [TestReportShape._shape(child, f"{path}[{index}]")
                             for index, child in enumerate(value)]}
        return type(value).__name__

    def test_the_successful_shape_is_exactly_named(self) -> None:
        report = self._report()
        self.assertEqual(report["schema"], "nakagawa-compiler-compare-v1")
        self.assertEqual(report["issue"], 317)
        self.assertIs(report["public_source_owned"], True)
        self.assertEqual(sorted(report["summary"]), sorted(compiler_compare.STATUSES))
        entry = report["compilers"][0]
        self.assertEqual(sorted(entry["measurements"]), [
            "build_seconds", "executable_bytes", "object_bytes", "workloads",
            "workloads_detail", "workloads_status",
        ])
        self.assertEqual(sorted(entry["veto"]["gates"][0]), [
            "command", "name", "output", "returncode", "status", "target",
        ])

    def test_the_shape_is_deterministic_across_runs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "out"
            first = self._shape(self._report(root))
            second = self._shape(self._report(root))
        self.assertEqual(json.dumps(first, sort_keys=True), json.dumps(second, sort_keys=True))

    def test_a_vetoed_entry_reports_no_measurement_shape(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            report = self._report(Path(tmp) / "out", failing_gate="cpu-lle-selftest")
        for entry in report["compilers"]:
            self.assertEqual(entry["status"], compiler_compare.VETOED)
            self.assertEqual(entry["veto"]["failed_gate"], "cpu-lle-selftest")
            self.assertEqual(self._shape(entry["measurements"]), "NoneType")
            self.assertEqual([gate["name"] for gate in entry["veto"]["gates"]],
                             list(compiler_compare.GATE_ORDER))

    def test_an_unavailable_compiler_is_reported_next_to_a_measured_one(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            report = self._report(Path(tmp) / "out", available=("gcc",))
        statuses = {entry["name"]: entry["status"] for entry in report["compilers"]}
        self.assertEqual(statuses, {"gcc": compiler_compare.MEASURED,
                                    "clang": compiler_compare.NOT_AVAILABLE})
        self.assertEqual(report["summary"][compiler_compare.NOT_AVAILABLE], ["clang"])


class TestReportValidator(unittest.TestCase):
    """The ordering invariant is enforced in the product, not only in a test."""

    def _vetoed_entry(self) -> dict:
        gates = [{"name": name, "target": target, "status": "PASS", "returncode": 0,
                  "command": ["make", target], "output": ""}
                 for name, target, _ in compiler_compare.GATE_CHAIN[:2]]
        gates[-1]["status"] = "FAIL"
        gates[-1]["returncode"] = 1
        return {
            "name": "clang", "path": "/usr/bin/clang", "version": "clang 1", "build_root": "b",
            "build_settings": "", "measurements": None,
            "veto": {"status": "FAIL", "failed_gate": gates[-1]["name"], "gates": gates},
            "status": compiler_compare.VETOED,
        }

    def _report(self, entry: dict) -> dict:
        return {"schema": compiler_compare.SCHEMA, "issue": compiler_compare.ISSUE,
                "public_source_owned": True, "compilers": [entry]}

    def test_a_clean_vetoed_entry_is_accepted(self) -> None:
        compiler_compare.validate_report(self._report(self._vetoed_entry()))

    def test_a_timing_published_under_a_veto_is_rejected(self) -> None:
        entry = self._vetoed_entry()
        entry["build_seconds"] = 3.5
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(self._report(entry))

    def test_a_workload_timing_published_under_a_veto_is_rejected(self) -> None:
        entry = self._vetoed_entry()
        entry["measurements"] = {"workloads": [{"name": "cosim", "duration_s": 1.0}]}
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(self._report(entry))

    def test_a_measurement_published_under_a_veto_is_rejected(self) -> None:
        entry = self._vetoed_entry()
        entry["measurements"] = {"build_seconds": 1.0, "executable_bytes": 1, "object_bytes": 1,
                                 "workloads_status": "PASS", "workloads": []}
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(self._report(entry))

    def test_a_veto_naming_a_gate_that_did_not_fail_is_rejected(self) -> None:
        entry = self._vetoed_entry()
        entry["veto"]["failed_gate"] = compiler_compare.CORPUS_GATE
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(self._report(entry))

    def test_out_of_order_gates_are_rejected(self) -> None:
        entry = self._vetoed_entry()
        entry["veto"]["gates"].reverse()
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(self._report(entry))

    def test_an_unknown_gate_is_rejected(self) -> None:
        entry = self._vetoed_entry()
        entry["veto"]["gates"][0]["name"] = "smoke-of-the-week"
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(self._report(entry))

    def test_a_measured_compiler_must_carry_its_measurements(self) -> None:
        entry = self._vetoed_entry()
        entry["status"] = compiler_compare.MEASURED
        entry["veto"] = {"status": "PASS", "failed_gate": None, "gates": entry["veto"]["gates"][:2]}
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(self._report(entry))

    def test_an_unavailable_compiler_may_not_carry_a_veto(self) -> None:
        entry = self._vetoed_entry()
        entry["status"] = compiler_compare.NOT_AVAILABLE
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(self._report(entry))

    def test_a_foreign_schema_is_rejected(self) -> None:
        report = self._report(self._vetoed_entry())
        report["schema"] = "something-else"
        with self.assertRaises(compiler_compare.CompareError):
            compiler_compare.validate_report(report)


if __name__ == "__main__":
    unittest.main()
