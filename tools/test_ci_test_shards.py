# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regression tests for the cost-weighted CI test sharding tool (#693)."""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

try:
    import ci_test_shards
except ModuleNotFoundError:  # Running as ``python -m unittest tools.test_ci_test_shards``.
    import tools.ci_test_shards as ci_test_shards


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "ci_test_shards.py"
WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"
MUTATING_PAIR = frozenset({"test_title_catalog", "test_publication_policy_gate"})


def _data(weights: dict[str, float], *, default: float = 0.5, separate: list[list[str]] | None = None):
    return ci_test_shards.parse_weight_data(
        {"default_seconds": default, "weights": weights, "separate": separate or []}
    )


def _run_cli(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


class LiveRepositoryPlanTests(unittest.TestCase):
    """The committed data and the tracked test modules, exactly as CI consumes them."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.modules = ci_test_shards.discover_modules()
        cls.data = ci_test_shards.load_weight_data()
        workflow = WORKFLOW.read_text(encoding="utf-8")
        cls.python_job = workflow.split("\n  python_tools:", 1)[1].split("\n  native_tools:", 1)[0]
        matrix = re.search(r"(?m)^\s+shard:\s*\[([0-9,\s]+)\]\s*$", cls.python_job)
        if matrix is None:
            raise AssertionError("python_tools must declare an explicit shard matrix")
        cls.matrix = [int(value) for value in matrix.group(1).split(",")]
        num = re.search(r"ci_test_shards\.py --shard \"\$SHARD\" --num-shards (\d+)", cls.python_job)
        if num is None:
            raise AssertionError("python_tools must run tools/ci_test_shards.py with an explicit --num-shards")
        cls.num_shards = int(num.group(1))

    def test_every_test_module_runs_exactly_once_across_the_cli_shards(self) -> None:
        self.assertEqual(self.matrix, list(range(self.num_shards)),
                         "the workflow matrix and --num-shards must describe the same shards")
        scheduled: list[str] = []
        for shard in self.matrix:
            with self.subTest(shard=shard):
                result = _run_cli("--shard", str(shard), "--num-shards", str(self.num_shards))
                self.assertEqual(result.returncode, 0, result.stderr)
                names = result.stdout.splitlines()
                self.assertTrue(names, f"shard {shard} is empty")
                scheduled.extend(names)
        self.assertEqual(len(scheduled), len(set(scheduled)), "a module is scheduled on two shards")
        expected = sorted(path.stem for path in (ROOT / "tools").glob("test_*.py"))
        self.assertEqual(sorted(scheduled), expected)
        self.assertIn("test_ci_test_shards", scheduled)

    def test_mutating_modules_are_declared_separate_and_never_share_a_shard(self) -> None:
        self.assertIn(MUTATING_PAIR, [group & MUTATING_PAIR for group in self.data.separate],
                      "the weight data must keep the tracked-state mutators in one separation group")
        plan = ci_test_shards.plan_shards(self.modules, self.data, self.num_shards)
        homes = [index for index, shard in enumerate(plan.shards) for name in shard if name in MUTATING_PAIR]
        self.assertEqual(len(homes), 2)
        self.assertNotEqual(homes[0], homes[1])

    def test_committed_data_passes_check(self) -> None:
        result = _run_cli("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_balance_beats_the_round_robin_it_replaces(self) -> None:
        plan = ci_test_shards.plan_shards(self.modules, self.data, self.num_shards)

        def cost(name: str) -> float:
            return self.data.weights.get(name, self.data.default_seconds)

        round_robin = [0.0] * self.num_shards
        for index, name in enumerate(self.modules):
            round_robin[index % self.num_shards] += cost(name)
        self.assertLessEqual(max(plan.loads), max(round_robin))
        # Greedy placement bound: the module that finished the heaviest shard joined
        # it when that shard was the lightest, i.e. at most the mean load.
        bound = sum(plan.loads) / self.num_shards + max(cost(name) for name in self.modules)
        self.assertLessEqual(max(plan.loads), bound + 1e-6)

    def test_workflow_captures_the_listing_so_a_planner_failure_stops_the_step(self) -> None:
        self.assertNotIn("(NR-1)%4", self.python_job, "the alphabetical round-robin must be gone")
        self.assertNotIn("< <(python tools/ci_test_shards.py", self.python_job,
                         "process substitution hides the planner's exit status")
        self.assertIn("set -euo pipefail", self.python_job)
        self.assertIn('listing="$(python tools/ci_test_shards.py --shard "$SHARD"', self.python_job)


class PlannerTests(unittest.TestCase):
    def test_lpt_assigns_heaviest_first_to_the_least_loaded_shard(self) -> None:
        data = _data({"test_a": 7, "test_b": 6, "test_c": 5, "test_d": 4, "test_e": 3, "test_f": 3})
        plan = ci_test_shards.plan_shards(
            ["test_a", "test_b", "test_c", "test_d", "test_e", "test_f"], data, 2
        )
        # a->0, b->1, c->1 (6<7), d->0 (7<11), e->0 (11=11, lower index), f->1.
        self.assertEqual(plan.shards, (("test_a", "test_d", "test_e"), ("test_b", "test_c", "test_f")))
        self.assertEqual(plan.loads, (14.0, 14.0))

    def test_separation_overrides_the_least_loaded_choice(self) -> None:
        data = _data({"test_a": 10, "test_b": 9, "test_x": 1}, separate=[["test_x", "test_b"]])
        plan = ci_test_shards.plan_shards(["test_a", "test_b", "test_x"], data, 2)
        # Unconstrained, test_x would join test_b on the lighter shard 1.
        self.assertEqual(plan.shards, (("test_a", "test_x"), ("test_b",)))
        self.assertEqual(plan.loads, (11.0, 9.0))
        unconstrained = ci_test_shards.plan_shards(["test_a", "test_b", "test_x"], _data(data.weights), 2)
        self.assertEqual(unconstrained.shards, (("test_a",), ("test_b", "test_x")))

    def test_unknown_modules_get_the_default_weight_and_are_reported(self) -> None:
        data = _data({"test_a": 2.0}, default=0.25)
        plan = ci_test_shards.plan_shards(["test_a", "test_new"], data, 2)
        self.assertEqual(plan.unmeasured, ("test_new",))
        self.assertEqual(plan.loads, (2.0, 0.25))

    def test_plan_is_deterministic_and_independent_of_discovery_order(self) -> None:
        names = [f"test_m{index:03d}" for index in range(60)]
        data = _data({name: 1 + (index * 37 % 11) for index, name in enumerate(names)})
        first = ci_test_shards.plan_shards(names, data, 4)
        self.assertEqual(first, ci_test_shards.plan_shards(list(reversed(names)), data, 4))
        self.assertEqual(first, ci_test_shards.plan_shards(sorted(names, key=hash), data, 4))
        with_new = ci_test_shards.plan_shards(names + ["test_zz_new"], data, 4)
        self.assertEqual(with_new, ci_test_shards.plan_shards(["test_zz_new"] + names, data, 4))
        scheduled = [name for shard in with_new.shards for name in shard]
        self.assertEqual(sorted(scheduled), sorted(names + ["test_zz_new"]))
        self.assertEqual(ci_test_shards.check_plan(names + ["test_zz_new"], data, with_new), [])

    def test_shard_lists_are_name_ordered(self) -> None:
        data = _data({"test_c": 5, "test_a": 1, "test_b": 1})
        plan = ci_test_shards.plan_shards(["test_a", "test_b", "test_c"], data, 1)
        self.assertEqual(plan.shards, (("test_a", "test_b", "test_c"),))

    def test_more_shards_than_modules_leaves_trailing_shards_empty_without_error(self) -> None:
        data = _data({"test_a": 1})
        plan = ci_test_shards.plan_shards(["test_a"], data, 3)
        self.assertEqual(plan.shards, (("test_a",), (), ()))
        self.assertEqual(ci_test_shards.check_plan(["test_a"], data, plan), [])

    def test_group_larger_than_the_shard_count_is_refused(self) -> None:
        data = _data({"test_a": 1}, separate=[["test_a", "test_b", "test_c"]])
        with self.assertRaises(ci_test_shards.ShardDataError):
            ci_test_shards.plan_shards(["test_a", "test_b", "test_c"], data, 2)
        # Absent members do not count against the shard budget.
        plan = ci_test_shards.plan_shards(["test_a", "test_b"], data, 2)
        self.assertEqual(len([shard for shard in plan.shards if shard]), 2)

    def test_invalid_requests_are_refused(self) -> None:
        data = _data({"test_a": 1})
        for shards in (0, -1, True, 2.0):
            with self.subTest(shards=shards), self.assertRaises(ci_test_shards.ShardDataError):
                ci_test_shards.plan_shards(["test_a"], data, shards)  # type: ignore[arg-type]
        with self.assertRaises(ci_test_shards.ShardDataError):
            ci_test_shards.plan_shards(["test_a", "test_a"], data, 2)


class CheckPlanTests(unittest.TestCase):
    def test_stale_weights_and_missing_group_members_fail_the_check(self) -> None:
        data = _data({"test_a": 1, "test_gone": 1}, separate=[["test_a", "test_also_gone"]])
        plan = ci_test_shards.plan_shards(["test_a", "test_b"], data, 2)
        errors = ci_test_shards.check_plan(["test_a", "test_b"], data, plan)
        self.assertTrue(any("test_gone" in error for error in errors), errors)
        self.assertTrue(any("test_also_gone" in error for error in errors), errors)

    def test_a_dropped_duplicated_or_colocated_module_fails_the_check(self) -> None:
        data = _data({"test_a": 1, "test_b": 1}, separate=[["test_a", "test_b"]])
        modules = ["test_a", "test_b", "test_c"]
        for shards, reason in (
            ((("test_a",), ("test_b",)), "dropped=['test_c']"),
            ((("test_a", "test_c"), ("test_b", "test_c")), "duplicated=['test_c']"),
            ((("test_a", "test_b", "test_c"), ()), "together on shard 0"),
        ):
            with self.subTest(reason=reason):
                plan = ci_test_shards.ShardPlan(shards=shards, loads=(0.0, 0.0), unmeasured=())
                errors = ci_test_shards.check_plan(modules, data, plan)
                self.assertTrue(any(reason in error for error in errors), errors)


class WeightDataValidationTests(unittest.TestCase):
    def test_malformed_documents_are_rejected(self) -> None:
        good = {"default_seconds": 0.5, "weights": {"test_a": 1.0}, "separate": []}
        cases = {
            "not an object": [],
            "unknown key": {**good, "pins": {}},
            "zero default": {**good, "default_seconds": 0},
            "boolean weight": {**good, "weights": {"test_a": True}},
            "negative weight": {**good, "weights": {"test_a": -1}},
            "infinite weight": {**good, "weights": {"test_a": float("inf")}},
            "non-test key": {**good, "weights": {"helper": 1.0}},
            "empty weights": {**good, "weights": {}},
            "singleton group": {**good, "separate": [["test_a"]]},
            "repeated group member": {**good, "separate": [["test_a", "test_a"]]},
            "group not a list": {**good, "separate": {"test_a": "test_b"}},
        }
        for label, raw in cases.items():
            with self.subTest(label), self.assertRaises(ci_test_shards.ShardDataError):
                ci_test_shards.parse_weight_data(raw)

    def test_cli_exits_2_with_no_module_list_on_bad_data(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            bad = Path(scratch) / "weights.json"
            for content in ("{not json", json.dumps({"default_seconds": -1, "weights": {"test_a": 1}})):
                with self.subTest(content=content):
                    bad.write_text(content, encoding="utf-8")
                    result = _run_cli("--shard", "0", "--weights", str(bad))
                    self.assertEqual(result.returncode, 2)
                    self.assertEqual(result.stdout, "")
            result = _run_cli("--shard", "0", "--weights", str(Path(scratch) / "absent.json"))
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, "")

    def test_cli_rejects_an_out_of_range_shard(self) -> None:
        result = _run_cli("--shard", "4", "--num-shards", "4")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")

    def test_cli_check_fails_on_stale_data(self) -> None:
        live = json.loads((ROOT / "tools" / "ci_test_weights.json").read_text(encoding="utf-8"))
        live["weights"]["test_module_that_was_deleted"] = 1.0
        with tempfile.TemporaryDirectory() as scratch:
            stale = Path(scratch) / "weights.json"
            stale.write_text(json.dumps(live), encoding="utf-8")
            result = _run_cli("--check", "--weights", str(stale))
        self.assertEqual(result.returncode, 1)
        self.assertIn("test_module_that_was_deleted", result.stderr)


if __name__ == "__main__":
    unittest.main()
