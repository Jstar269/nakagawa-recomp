# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Tests for tools/discovery_contract.py parallel test execution and contract verification.

Covers:
1. Collection parity: serial and parallel produce identical test-ID sets.
2. Skip parity by reason, not count: identical skip-reason multisets across workers.
3. Worker-count invariance: identical results at multiple -j values.
4. Order independence: seed-based module shuffling yields invariant outcomes.
5. Failure and error detection: aggregator accurately captures failures, errors,
   and non-zero exit codes.
6. Worker import errors: syntax/import errors in worker processes are captured.
7. Serial lane isolation: _SERIAL_ONLY modules run sequentially after the parallel pool drains.
8. Worker-count resolution: an explicit -j is never overridden by --parallel.
"""

from __future__ import annotations

from collections import Counter
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))

import discovery_contract  # noqa: E402


class DiscoveryContractTests(unittest.TestCase):
    """Verify discovery_contract runner and parity gates using hermetic synthetic suites."""

    def tearDown(self) -> None:
        for k in list(sys.modules.keys()):
            if k.startswith("test_synth_"):
                sys.modules.pop(k, None)

    def _create_clean_synthetic_suite(self, base_dir: Path) -> dict[str, str]:
        """Create a multi-module synthetic suite with pass, method skips, and setUpClass skips."""
        mod1 = base_dir / "test_synth_alpha.py"
        mod1.write_text(
            """# synthetic module alpha
import unittest

class AlphaTests(unittest.TestCase):
    def test_alpha_pass_1(self):
        self.assertEqual(1 + 1, 2)

    def test_alpha_pass_2(self):
        self.assertTrue(True)

    @unittest.skip("skip reason alpha method")
    def test_alpha_skipped_method(self):
        self.fail("should not execute")


class AlphaClassSetUpSkipped(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        raise unittest.SkipTest("skip reason alpha setUpClass")

    def test_class_skip_item_1(self):
        self.fail("should not execute")

    def test_class_skip_item_2(self):
        self.fail("should not execute")
""",
            encoding="utf-8",
        )

        mod2 = base_dir / "test_synth_beta.py"
        mod2.write_text(
            """# synthetic module beta
import unittest

class BetaTests(unittest.TestCase):
    def test_beta_pass_1(self):
        self.assertIn("k", {"k": "v"})

    def test_beta_dynamic_skip(self):
        raise unittest.SkipTest("skip reason beta dynamic")
""",
            encoding="utf-8",
        )

        mod3 = base_dir / "test_synth_gamma.py"
        mod3.write_text(
            """# synthetic module gamma
import unittest

class GammaTests(unittest.TestCase):
    def test_gamma_pass_1(self):
        self.assertEqual("psp", "psp")
""",
            encoding="utf-8",
        )

        return {
            "test_synth_alpha.AlphaTests.test_alpha_skipped_method": "skip reason alpha method",
            "setUpClass (test_synth_alpha.AlphaClassSetUpSkipped)": "skip reason alpha setUpClass",
            "test_synth_beta.BetaTests.test_beta_dynamic_skip": "skip reason beta dynamic",
        }

    def test_collection_and_skip_parity_between_serial_and_parallel(self):
        """Serial and parallel execution must yield identical test ID sets and skip-reason multisets."""
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp_path = Path(tmp_str)
            expected_skips = self._create_clean_synthetic_suite(tmp_path)

            report_serial = discovery_contract._contract_report(
                execute=True,
                jobs=1,
                start_dir=tmp_path,
                serial_only=frozenset(),
            )
            report_parallel = discovery_contract._contract_report(
                execute=True,
                jobs=4,
                start_dir=tmp_path,
                serial_only=frozenset(),
            )

            # Assert parity using the contract assertion helper
            discovery_contract.assert_parity(report_serial, report_parallel)

            # Check individual contract assertions
            discovery_contract._assert_contract(
                report_serial, expected_skip_reasons=expected_skips
            )
            discovery_contract._assert_contract(
                report_parallel, expected_skip_reasons=expected_skips
            )

            # Verify collection IDs match exactly
            ids_serial = report_serial["inventory_b"]["ids"]
            ids_parallel = report_parallel["inventory_b"]["ids"]
            self.assertEqual(ids_serial, ids_parallel)
            self.assertEqual(len(ids_serial), 6)  # 4 passed + 1 method skip + 1 dynamic skip

            # Verify skip reasons match by multiset
            skips_serial = Counter(
                (r["id"], r["reason"]) for r in report_serial["skip_records"]
            )
            skips_parallel = Counter(
                (r["id"], r["reason"]) for r in report_parallel["skip_records"]
            )
            self.assertEqual(skips_serial, skips_parallel)
            self.assertEqual(len(skips_serial), 3)

    def test_skip_parity_fails_on_reason_mismatch(self):
        """assert_parity and _assert_contract must reject identical counts if reasons drift."""
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp_path = Path(tmp_str)
            self._create_clean_synthetic_suite(tmp_path)

            report1 = discovery_contract._contract_report(
                execute=True, jobs=1, start_dir=tmp_path, serial_only=frozenset()
            )
            report2 = json.loads(json.dumps(report1))

            # Alter a skip reason while keeping IDs and counts identical
            report2["skip_records"][0]["reason"] = "tampered reason string"
            report2["skip_reasons"][report2["skip_records"][0]["id"]] = "tampered reason string"

            with self.assertRaises(AssertionError) as ctx:
                discovery_contract.assert_parity(report1, report2)
            self.assertIn("Skip parity mismatch", str(ctx.exception))

    def test_worker_count_invariance(self):
        """Outcome, test IDs, and skip reasons must be invariant across different worker counts."""
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp_path = Path(tmp_str)
            self._create_clean_synthetic_suite(tmp_path)

            baseline = discovery_contract._contract_report(
                execute=True, jobs=1, start_dir=tmp_path, serial_only=frozenset()
            )

            for j in (2, 3, 4):
                with self.subTest(jobs=j):
                    run_j = discovery_contract._contract_report(
                        execute=True, jobs=j, start_dir=tmp_path, serial_only=frozenset()
                    )
                    discovery_contract.assert_parity(baseline, run_j)
                    discovery_contract._assert_contract(run_j)

    def test_order_independence_with_random_seeds(self):
        """Module submission order shuffling via --seed must produce invariant test outcomes."""
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp_path = Path(tmp_str)
            self._create_clean_synthetic_suite(tmp_path)

            baseline = discovery_contract._contract_report(
                execute=True, jobs=3, start_dir=tmp_path, seed=None, serial_only=frozenset()
            )

            for seed_val in (11, 42, 999, 1337):
                with self.subTest(seed=seed_val):
                    seeded_run = discovery_contract._contract_report(
                        execute=True,
                        jobs=3,
                        start_dir=tmp_path,
                        seed=seed_val,
                        serial_only=frozenset(),
                    )
                    discovery_contract.assert_parity(baseline, seeded_run)
                    discovery_contract._assert_contract(seeded_run)
                    self.assertEqual(seeded_run["seed"], seed_val)

    def test_aggregator_catches_failures_and_errors(self):
        """The parallel aggregator must accurately report failures, errors, and unsuccessful status."""
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp_path = Path(tmp_str)

            fail_mod = tmp_path / "test_synth_failing.py"
            fail_mod.write_text(
                """import unittest

class FailureTests(unittest.TestCase):
    def test_assertion_failure(self):
        self.assertEqual(1, 2, "intentional failure")

    def test_runtime_error(self):
        raise RuntimeError("intentional exception in test")

    def test_passing_alongside(self):
        self.assertTrue(True)
""",
                encoding="utf-8",
            )

            report = discovery_contract._contract_report(
                execute=True, jobs=2, start_dir=tmp_path, serial_only=frozenset(),
                quiet=True,
            )

            self.assertFalse(report["successful"])
            self.assertEqual(report["failures"], 1)
            self.assertEqual(report["errors"], 1)
            self.assertEqual(report["inventory_b"]["count"], 3)

            # _assert_contract must fail-closed on unsuccessful suite
            with self.assertRaises(ValueError) as ctx:
                discovery_contract._assert_contract(report)
            self.assertIn("canonical suite failed", str(ctx.exception))

    def test_worker_import_error_captured_in_report(self):
        """If a test module raises on import, the worker catches it and reports an error."""
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp_path = Path(tmp_str)

            broken_mod = tmp_path / "test_synth_broken_import.py"
            broken_mod.write_text(
                """# broken syntax or module-level exception
raise ImportError("simulated module import failure")
""",
                encoding="utf-8",
            )

            report = discovery_contract._contract_report(
                execute=True, jobs=2, start_dir=tmp_path, serial_only=frozenset(),
                quiet=True,
            )

            self.assertFalse(report["successful"])
            self.assertEqual(report["errors"], 1)
            with self.assertRaises(ValueError):
                discovery_contract._assert_contract(report)

    def test_serial_only_lane_executes_strictly_after_parallel_pool(self):
        """Modules in _SERIAL_ONLY must execute sequentially after all parallel workers have finished."""
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp_path = Path(tmp_str)
            timeline_file = tmp_path / "timeline.json"
            timeline_file.write_text("[]", encoding="utf-8")

            timeline_code = f"""
import json, time, unittest

TIMELINE_FILE = r'{timeline_file}'

def record_event(name, phase):
    try:
        data = json.loads(open(TIMELINE_FILE, encoding='utf-8').read())
    except Exception:
        data = []
    data.append({{'name': name, 'phase': phase, 'time': time.perf_counter()}})
    with open(TIMELINE_FILE, 'w', encoding='utf-8') as f:
        json.dump(data, f)
"""

            # Parallel module: runs, sleeps briefly
            (tmp_path / "test_synth_par1.py").write_text(
                timeline_code
                + """
class ParTests(unittest.TestCase):
    def test_par(self):
        record_event("par1", "start")
        time.sleep(0.08)
        record_event("par1", "end")
""",
                encoding="utf-8",
            )

            # Serial module 1
            (tmp_path / "test_synth_ser1.py").write_text(
                timeline_code
                + """
class SerTests1(unittest.TestCase):
    def test_ser1(self):
        record_event("ser1", "start")
        time.sleep(0.04)
        record_event("ser1", "end")
""",
                encoding="utf-8",
            )

            # Serial module 2
            (tmp_path / "test_synth_ser2.py").write_text(
                timeline_code
                + """
class SerTests2(unittest.TestCase):
    def test_ser2(self):
        record_event("ser2", "start")
        time.sleep(0.04)
        record_event("ser2", "end")
""",
                encoding="utf-8",
            )

            serial_set = frozenset({"test_synth_ser1", "test_synth_ser2"})
            report = discovery_contract._contract_report(
                execute=True, jobs=2, start_dir=tmp_path, serial_only=serial_set
            )
            self.assertTrue(report["successful"])
            self.assertEqual(report["inventory_b"]["count"], 3)

            events = json.loads(timeline_file.read_text(encoding="utf-8"))
            par1_end = [e["time"] for e in events if e["name"] == "par1" and e["phase"] == "end"][0]
            ser1_start = [e["time"] for e in events if e["name"] == "ser1" and e["phase"] == "start"][0]
            ser1_end = [e["time"] for e in events if e["name"] == "ser1" and e["phase"] == "end"][0]
            ser2_start = [e["time"] for e in events if e["name"] == "ser2" and e["phase"] == "start"][0]

            # Serial modules must start strictly AFTER the parallel pool module finishes
            self.assertGreaterEqual(
                ser1_start,
                par1_end,
                "ser1 started before parallel pool finished",
            )

            # Serial modules must execute sequentially without overlapping
            if ser1_start < ser2_start:
                self.assertGreaterEqual(
                    ser2_start,
                    ser1_end,
                    "ser2 started before ser1 finished",
                )
            else:
                ser2_end = [e["time"] for e in events if e["name"] == "ser2" and e["phase"] == "end"][0]
                self.assertGreaterEqual(
                    ser1_start,
                    ser2_end,
                    "ser1 started before ser2 finished",
                )

    def test_cli_invocation_on_synthetic_suite(self):
        """CLI invocation of discovery_contract.py succeeds on valid suite and fails on bad suite."""
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp_path = Path(tmp_str)
            self._create_clean_synthetic_suite(tmp_path)

            # Successful CLI run
            res_ok = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "discovery_contract.py"),
                    "--run",
                    "-j",
                    "2",
                    "--start-dir",
                    str(tmp_path),
                    "--assert-contract",
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertEqual(res_ok.returncode, 0, res_ok.stderr)
            self.assertIn("Ran 6 tests", res_ok.stderr)

            # Failing CLI run
            (tmp_path / "test_synth_broken.py").write_text(
                "import unittest\nclass B(unittest.TestCase):\n def test_f(self): self.fail('fail')\n",
                encoding="utf-8",
            )
            res_fail = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "discovery_contract.py"),
                    "--run",
                    "-j",
                    "2",
                    "--start-dir",
                    str(tmp_path),
                    "--assert-contract",
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(res_fail.returncode, 0)
            self.assertIn("FAILED", res_fail.stderr)

    def test_explicit_jobs_is_not_overridden_by_parallel(self):
        """-j N caps the pool even with --parallel; only its absence or -j 0 means every core."""
        cores = discovery_contract.os.cpu_count() or 1
        cases = [
            ((None, False), 1),
            ((None, True), cores),
            ((0, False), cores),
            ((0, True), cores),
            ((-1, False), cores),
            ((1, True), 1),
            ((3, True), 3),
            ((3, False), 3),
        ]
        for (requested, parallel), expected in cases:
            with self.subTest(requested=requested, parallel=parallel):
                self.assertEqual(
                    discovery_contract.resolve_jobs(requested, parallel=parallel), expected
                )

        # End to end through argument parsing: the reported worker count is the
        # capped one, not os.cpu_count().
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            (tmp_path / "test_synth_cap.py").write_text(
                "import unittest\nclass C(unittest.TestCase):\n def test_ok(self): pass\n",
                encoding="utf-8",
            )
            res = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "discovery_contract.py"),
                    "--parallel",
                    "-j",
                    "2",
                    "--start-dir",
                    str(tmp_path),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertIn("(workers=2)", res.stderr)


if __name__ == "__main__":
    unittest.main()
