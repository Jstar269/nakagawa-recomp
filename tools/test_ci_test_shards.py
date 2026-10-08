# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regression tests for the cost-weighted CI test sharding tool (#693)."""

from __future__ import annotations

import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

try:
    import ci_paths
    import ci_test_shards
except ModuleNotFoundError:  # Running as ``python -m unittest tools.test_ci_test_shards``.
    import tools.ci_paths as ci_paths
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
        cls.num_shards = ci_paths.PYTHON_SHARDS_FULL

    def run_shards(self, num_shards: int, select: str) -> list[str]:
        scheduled: list[str] = []
        for shard in range(num_shards):
            with self.subTest(shard=shard, select=select):
                result = _run_cli("--shard", str(shard), "--num-shards", str(num_shards), "--select", select)
                self.assertEqual(result.returncode, 0, result.stderr)
                names = result.stdout.splitlines()
                self.assertTrue(names, f"shard {shard} is empty")
                scheduled.extend(names)
        self.assertEqual(len(scheduled), len(set(scheduled)), "a module is scheduled on two shards")
        return scheduled

    def test_every_test_module_runs_exactly_once_across_the_cli_shards(self) -> None:
        scheduled = self.run_shards(self.num_shards, "all")
        expected = sorted(path.stem for path in (ROOT / "tools").glob("test_*.py"))
        self.assertEqual(sorted(scheduled), expected)
        self.assertIn("test_ci_test_shards", scheduled)

    def test_makefile_selection_runs_each_coupled_module_exactly_once(self) -> None:
        scheduled = self.run_shards(ci_paths.PYTHON_SHARDS_MAKEFILE, "makefile")
        coupled = ci_test_shards.makefile_coupled_modules(self.modules)
        self.assertEqual(sorted(scheduled), coupled)
        # Modules that read the Makefile text or build through make must be in it.
        for module in ("test_title_runtime_config", "test_build_truth", "test_public_ci_wiring",
                       "test_ci_paths", "test_production_smoke"):
            self.assertIn(module, coupled)

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

    def test_workflow_takes_its_shards_from_the_classifier_and_fails_closed(self) -> None:
        self.assertNotIn("(NR-1)%4", self.python_job, "the alphabetical round-robin must be gone")
        self.assertIn(
            "shard: ${{ fromJSON(needs.classify.outputs.python_shards || '[0, 1, 2, 3]') }}", self.python_job
        )
        fallback = re.search(r"python_shards \|\| '(\[[^']*\])'", self.python_job)
        self.assertIsNotNone(fallback)
        self.assertEqual(json.loads(fallback.group(1)), list(range(ci_paths.PYTHON_SHARDS_FULL)))
        self.assertIn("NUM_SHARDS: ${{ needs.classify.outputs.python_num_shards }}", self.python_job)
        self.assertIn("PYTHON_SCOPE: ${{ needs.classify.outputs.python_scope }}", self.python_job)
        self.assertNotIn("< <(python tools/ci_test_shards.py", self.python_job,
                         "process substitution hides the planner's exit status")
        self.assertIn("set -euo pipefail", self.python_job)
        self.assertIn(
            'listing="$(python tools/ci_test_shards.py --shard "$SHARD" --num-shards "$NUM_SHARDS" '
            '--select "$PYTHON_SCOPE")"',
            self.python_job,
        )
        workflow = WORKFLOW.read_text(encoding="utf-8")
        classify_outputs = workflow.split("\n  classify:", 1)[1].split("\n    steps:", 1)[0]
        for output in ("python_scope", "python_num_shards", "python_shards"):
            self.assertIn(f"{output}: ${{{{ steps.paths.outputs.{output} }}}}", classify_outputs)

    @unittest.skipIf(shutil.which("bash") is None, "the workflow step is a bash script")
    def test_workflow_step_stops_on_a_failing_or_empty_planner(self) -> None:
        """Execute the step's own script with stand-in planners: a planner that fails
        or prints nothing must fail the step before unittest runs."""
        lines = self.python_job.splitlines()
        start = next(i for i, line in enumerate(lines) if line.strip() == "run: |"
                     and any("ci_test_shards.py" in later for later in lines[i:i + 4]))
        indent = len(lines[start + 1]) - len(lines[start + 1].lstrip())
        body = []
        for line in lines[start + 1:]:
            if line.strip() and len(line) - len(line.lstrip()) < indent:
                break
            body.append(line[indent:])
        script = "\n".join(body) + "\n"
        for label, planner, expect_rc, expect_modules in (
            ("planner fails", "exit 2", False, None),
            ("planner prints nothing", "exit 0", False, None),
            ("planner lists modules", "printf 'test_a\\ntest_b\\n'", True, "test_a test_b"),
        ):
            with self.subTest(label), tempfile.TemporaryDirectory() as scratch:
                stub = Path(scratch) / "python"
                ran = Path(scratch) / "unittest-args"
                stub.write_text(
                    "#!/bin/sh\n"
                    'if [ "$1" = "-m" ]; then shift 3; echo "$@" > "' + str(ran) + '"; exit 0; fi\n'
                    + planner + "\n",
                    encoding="utf-8",
                )
                stub.chmod(0o755)
                env = {"PATH": f"{scratch}:/usr/bin:/bin", "SHARD": "0", "NUM_SHARDS": "4",
                       "PYTHON_SCOPE": "all"}
                result = subprocess.run(["bash", "-c", script], env=env, capture_output=True,
                                        text=True, check=False)
                self.assertEqual(result.returncode == 0, expect_rc, result.stdout + result.stderr)
                if expect_modules is None:
                    self.assertFalse(ran.exists(), "unittest ran after a planner failure")
                else:
                    self.assertEqual(ran.read_text(encoding="utf-8").strip(), expect_modules)

    def test_classifier_shard_outputs_match_the_planner_choices(self) -> None:
        for paths, scope in ((["tools/codegen.py"], "all"), (["Makefile"], "makefile")):
            with self.subTest(scope=scope):
                change = ("player-ui-regressions",) if scope == "makefile" else None
                result = ci_paths.classify(paths, makefile_change=change)
                self.assertEqual(result["python_scope"], scope)
                self.assertIn(result["python_scope"], ci_test_shards.SELECTIONS)
                self.assertEqual(json.loads(result["python_shards"]),
                                 list(range(int(result["python_num_shards"]))))


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


class MakefileCouplingTests(unittest.TestCase):
    """--select makefile must be a superset of the modules a recipe edit can affect."""

    def test_selection_follows_imports_launches_and_unparseable_files(self) -> None:
        files = {
            "helper.py": "MAKEFILE = 'Makefile'\n",
            "middle.py": "import helper\n",
            "runner.py": "import subprocess\nsubprocess.run(['make', 'all'])\n",
            "plain.py": "VALUE = 1\n",
            "pkg/__init__.py": "",
            "pkg/inner.py": "from . import deep\n",
            "pkg/deep.py": "GOAL = 'gmake'\n",
            "test_direct.py": "TEXT = open('Makefile').read()\n",
            "test_import.py": "import helper\n",
            "test_transitive.py": "from tools import middle\n",
            "test_launch.py": "import subprocess, sys\nsubprocess.run([sys.executable, 'tools/runner.py'])\n",
            "test_package.py": "from pkg import inner\n",
            "test_broken.py": "def broken(:\n",
            "test_plain.py": "import plain\nimport json\n",
            "test_named_only.py": "NOTE = 'see plain.py for details'\n",
            "shell_runner.py": "import subprocess\nsubprocess.run(f\"make {'all'}\", shell=True)\n",
            "env_runner.py": "import os\nos.environ.get(\"MAKE\")\n",
            "oracle/__init__.py": "",
            "oracle/launch.py": "GOAL = '$(MAKE) smoke'\n",
            "test_shell.py": "import subprocess\nsubprocess.run(['python', 'tools/shell_runner.py'])\n",
            "test_env.py": "import subprocess, sys\nsubprocess.run([sys.executable, '-m', 'env_runner'])\n",
            "test_nested_path.py": "SCRIPT = 'tools/oracle/launch.py'\n",
            "test_dotted.py": "MODULE = 'oracle.launch'\n",
            "test_prose.py": "NOTE = 'we make no claims about plain'\n",
        }
        with tempfile.TemporaryDirectory() as scratch:
            tools_dir = Path(scratch)
            for name, text in files.items():
                (tools_dir / name).parent.mkdir(parents=True, exist_ok=True)
                (tools_dir / name).write_text(text, encoding="utf-8")
            modules = ci_test_shards.discover_modules(tools_dir)
            selected = ci_test_shards.makefile_coupled_modules(modules, tools_dir)
        self.assertEqual(
            selected,
            [
                "test_broken",
                "test_direct",
                "test_dotted",
                "test_env",
                "test_import",
                "test_launch",
                "test_nested_path",
                "test_package",
                "test_shell",
                "test_transitive",
            ],
        )

    def test_unknown_module_names_are_kept(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            self.assertEqual(
                ci_test_shards.makefile_coupled_modules(["test_missing"], Path(scratch)), ["test_missing"]
            )


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
