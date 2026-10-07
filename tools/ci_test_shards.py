# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Deterministic cost-weighted partition of the Python test modules into CI shards.

The hosted ``python_tools`` job runs the ``tools/test_*.py`` modules on several
runners.  Alphabetical round-robin put the heaviest modules on the same runner by
accident, so one shard set the job's wall clock while the others idled.  This tool
replaces it with Longest-Processing-Time-first (LPT) bin packing over measured
per-module durations:

1. every module gets a weight -- its measured seconds from
   ``tools/ci_test_weights.json``, or that file's ``default_seconds`` when the module
   has not been measured yet (a new test therefore never needs a data edit to run);
2. modules are taken heaviest first, ties broken by name, and each goes to the
   least-loaded shard, ties broken by the lowest shard index;
3. a module that belongs to a ``separate`` group never joins a shard that already
   holds another member of that group.

The partition depends only on the discovered module names and the data file, so
every runner computes the same plan independently and no coordination is needed.
Each shard's modules are printed in name order, matching the order unittest used for
the round-robin lists, so moving a module between shards never changes the relative
order of the modules that share a runner.

Usage::

    python tools/ci_test_shards.py --shard 0          # module names for shard 0
    python tools/ci_test_shards.py --plan             # whole plan with estimates
    python tools/ci_test_shards.py --check            # validate the data and plan

``--check`` fails (exit 1) when the data names a module that no longer exists, a
weight is not a positive finite number, a separation group is malformed or larger
than the shard count, or the plan does not schedule every discovered module exactly
once.  Unmeasured modules are reported but are not an error.  Any usage or data
error exits 2 without printing a module list, so a caller that captures the list
can never mistake a failure for an empty shard.
"""

from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass
import json
import math
from pathlib import Path
import re
import sys
from typing import Iterable, Mapping, Sequence


ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
DEFAULT_WEIGHTS = TOOLS_DIR / "ci_test_weights.json"
DEFAULT_NUM_SHARDS = 4
TEST_GLOB = "test_*.py"
SELECTIONS = ("all", "makefile")


class ShardDataError(ValueError):
    """The weight data or a requested plan is unusable."""


@dataclass(frozen=True)
class WeightData:
    default_seconds: float
    weights: Mapping[str, float]
    separate: tuple[frozenset[str], ...]


@dataclass(frozen=True)
class ShardPlan:
    shards: tuple[tuple[str, ...], ...]
    loads: tuple[float, ...]
    unmeasured: tuple[str, ...]


def discover_modules(tools_dir: Path = TOOLS_DIR) -> list[str]:
    """Return the sorted module names unittest discovery would load from ``tools_dir``."""

    return sorted(path.stem for path in tools_dir.glob(TEST_GLOB) if path.is_file())


def _positive_seconds(value: object, label: str) -> float:
    # bool is an int subclass; a JSON ``true`` is a data error, not one second.
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ShardDataError(f"{label} must be a number of seconds, got {value!r}")
    seconds = float(value)
    if not math.isfinite(seconds) or seconds <= 0.0:
        raise ShardDataError(f"{label} must be a positive finite number of seconds, got {value!r}")
    return seconds


def parse_weight_data(raw: object) -> WeightData:
    """Validate the decoded JSON weight document and return it as ``WeightData``."""

    if not isinstance(raw, dict):
        raise ShardDataError("weight data must be a JSON object")
    unknown_keys = sorted(set(raw) - {"description", "default_seconds", "separate", "weights"})
    if unknown_keys:
        raise ShardDataError(f"weight data has unknown keys: {', '.join(unknown_keys)}")
    default_seconds = _positive_seconds(raw.get("default_seconds"), "default_seconds")

    weights_raw = raw.get("weights")
    if not isinstance(weights_raw, dict) or not weights_raw:
        raise ShardDataError("weights must be a non-empty object of module -> seconds")
    weights: dict[str, float] = {}
    for module, seconds in weights_raw.items():
        if not isinstance(module, str) or not module.startswith("test_"):
            raise ShardDataError(f"weights key {module!r} is not a test module name")
        weights[module] = _positive_seconds(seconds, f"weights[{module!r}]")

    separate_raw = raw.get("separate", [])
    if not isinstance(separate_raw, list):
        raise ShardDataError("separate must be a list of module-name lists")
    separate: list[frozenset[str]] = []
    for index, group in enumerate(separate_raw):
        if (
            not isinstance(group, list)
            or len(group) < 2
            or not all(isinstance(name, str) and name.startswith("test_") for name in group)
        ):
            raise ShardDataError(f"separate[{index}] must list at least two test module names")
        if len(set(group)) != len(group):
            raise ShardDataError(f"separate[{index}] repeats a module")
        separate.append(frozenset(group))
    return WeightData(default_seconds, weights, tuple(separate))


def load_weight_data(path: Path = DEFAULT_WEIGHTS) -> WeightData:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise ShardDataError(f"cannot read weight data {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ShardDataError(f"weight data {path} is not valid JSON: {exc}") from exc
    return parse_weight_data(raw)


def plan_shards(modules: Iterable[str], data: WeightData, num_shards: int) -> ShardPlan:
    """Partition ``modules`` into ``num_shards`` shards with constrained LPT.

    Raises ``ShardDataError`` when the request cannot be satisfied: no shards, a
    duplicated module name, or a separation group with more present members than
    there are shards.
    """

    if isinstance(num_shards, bool) or not isinstance(num_shards, int) or num_shards < 1:
        raise ShardDataError(f"num_shards must be a positive integer, got {num_shards!r}")
    module_list = list(modules)
    if len(set(module_list)) != len(module_list):
        raise ShardDataError("module list contains duplicates")
    present = set(module_list)

    groups_of: dict[str, list[frozenset[str]]] = {}
    for group in data.separate:
        members = group & present
        if len(members) > num_shards:
            raise ShardDataError(
                f"separation group {sorted(group)} needs {len(members)} shards, only {num_shards} exist"
            )
        for member in members:
            groups_of.setdefault(member, []).append(members)

    def weight(module: str) -> float:
        return data.weights.get(module, data.default_seconds)

    shards: list[list[str]] = [[] for _ in range(num_shards)]
    loads = [0.0] * num_shards
    for module in sorted(module_list, key=lambda name: (-weight(name), name)):
        blocked: set[int] = set()
        for members in groups_of.get(module, ()):
            for index, shard in enumerate(shards):
                if members.intersection(shard):
                    blocked.add(index)
        candidates = [index for index in range(num_shards) if index not in blocked]
        # Unreachable while every group fits in num_shards (checked above): a group
        # of k members blocks at most k - 1 shards before its last member lands.
        if not candidates:
            raise ShardDataError(f"no shard can accept {module} without breaking a separation group")
        target = min(candidates, key=lambda index: (loads[index], index))
        shards[target].append(module)
        loads[target] += weight(module)

    unmeasured = tuple(sorted(module for module in module_list if module not in data.weights))
    return ShardPlan(
        shards=tuple(tuple(sorted(shard)) for shard in shards),
        loads=tuple(round(load, 3) for load in loads),
        unmeasured=unmeasured,
    )


def check_plan(
    modules: Sequence[str],
    data: WeightData,
    plan: ShardPlan,
    *,
    tracked: Sequence[str] | None = None,
) -> list[str]:
    """Return every reason the data or plan is not acceptable for CI (empty when clean).

    ``modules`` are the planned modules; ``tracked`` (default: the same) are every
    module that exists, against which the data is checked for stale names.
    """

    errors: list[str] = []
    present = set(modules)
    existing = set(tracked if tracked is not None else modules)
    stale = sorted(set(data.weights) - existing)
    if stale:
        errors.append("weights name modules that no longer exist: " + ", ".join(stale))
    for group in data.separate:
        missing = sorted(group - existing)
        if missing:
            errors.append(f"separation group {sorted(group)} names missing modules: {', '.join(missing)}")
    scheduled = [module for shard in plan.shards for module in shard]
    if sorted(scheduled) != sorted(modules):
        duplicated = sorted({module for module in scheduled if scheduled.count(module) > 1})
        dropped = sorted(present - set(scheduled))
        extra = sorted(set(scheduled) - present)
        errors.append(
            f"plan does not schedule every module exactly once (duplicated={duplicated}, "
            f"dropped={dropped}, unknown={extra})"
        )
    for group in data.separate:
        for index, shard in enumerate(plan.shards):
            together = sorted(group.intersection(shard))
            if len(together) > 1:
                errors.append(f"plan puts separated modules together on shard {index}: {', '.join(together)}")
    if len(modules) >= len(plan.shards):
        for index, shard in enumerate(plan.shards):
            if not shard:
                errors.append(f"shard {index} is empty")
    return errors


# ---- Makefile-coupled selection (#702) ---------------------------------------------
#
# A recipe-only Makefile change (tools/ci_paths.py proves that property) can only
# change the outcome of a Python test that reads the Makefile or runs make, directly
# or through a tools module it imports or launches.  ``makefile_coupled_modules``
# computes that set statically and errs toward inclusion:
#
# * a module's dependencies are its local imports (``import x``, ``from tools import
#   x``, ``from psp_oracle import y``) *and* every local tool whose file name appears
#   in one of its string literals, which is how a test launches a tool as a
#   subprocess (``[sys.executable, "tools/codegen.py", ...]``);
# * the closure is transitive, and a package dependency pulls in every file in it;
# * a file that cannot be read or parsed counts as coupled.
#
# The selection is therefore a superset of the modules a recipe edit can affect,
# never a guess at a subset.

MAKEFILE_COUPLING = re.compile(r"\bMakefile\b|\bmingw32-make\b|\bgmake\b|[\"']make[\"']|\.mk\b")
_PY_FILE_LITERAL = re.compile(r"(?:^|[/\\])([A-Za-z_][A-Za-z0-9_]*)\.py\b")


def _local_units(tools_dir: Path) -> dict[str, list[Path]]:
    units: dict[str, list[Path]] = {path.stem: [path] for path in tools_dir.glob("*.py") if path.is_file()}
    for package in tools_dir.iterdir():
        if package.is_dir() and not package.name.startswith((".", "__")):
            files = sorted(package.rglob("*.py"))
            if files:
                units.setdefault(package.name, []).extend(files)
    return units


def _unit_edges(path: Path, local: frozenset[str]) -> tuple[set[str], bool]:
    """Return the local units ``path`` depends on, and whether it is unparseable."""

    try:
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    except (OSError, UnicodeDecodeError, SyntaxError, ValueError):
        return set(), True
    edges: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                parts = alias.name.split(".")
                edges.add(parts[1] if parts[0] == "tools" and len(parts) > 1 else parts[0])
        elif isinstance(node, ast.ImportFrom):
            parts = (node.module or "").split(".")
            if node.level or parts == ["tools"] or not node.module:
                edges.update(alias.name for alias in node.names)
                if node.level:
                    # A relative import inside a package depends on that package.
                    edges.add(path.parent.name)
            edges.add(parts[1] if parts[0] == "tools" and len(parts) > 1 else parts[0])
        elif isinstance(node, ast.Constant) and isinstance(node.value, str):
            edges.update(_PY_FILE_LITERAL.findall(node.value))
    return edges & local, False


def makefile_coupled_modules(modules: Iterable[str], tools_dir: Path = TOOLS_DIR) -> list[str]:
    """Return the test modules whose transitive tool closure can observe the Makefile."""

    units = _local_units(tools_dir)
    local = frozenset(units)
    edges: dict[str, set[str]] = {}
    coupled_unit: dict[str, bool] = {}
    for name, files in units.items():
        deps: set[str] = set()
        coupled = False
        for file in files:
            file_edges, unparseable = _unit_edges(file, local)
            deps |= file_edges
            try:
                text = file.read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                coupled = True
                continue
            coupled = coupled or unparseable or bool(MAKEFILE_COUPLING.search(text))
        deps.discard(name)
        edges[name] = deps
        coupled_unit[name] = coupled

    selected: list[str] = []
    for module in modules:
        if module not in units:
            selected.append(module)  # cannot be inspected, so it cannot be excluded
            continue
        seen = {module}
        stack = [module]
        hit = False
        while stack and not hit:
            current = stack.pop()
            hit = coupled_unit.get(current, True)
            for dep in edges.get(current, ()):
                if dep not in seen:
                    seen.add(dep)
                    stack.append(dep)
        if hit:
            selected.append(module)
    return sorted(selected)


def _plan_summary(plan: ShardPlan) -> dict[str, object]:
    return {
        "num_shards": len(plan.shards),
        "unmeasured": list(plan.unmeasured),
        "shards": [
            {"index": index, "estimated_seconds": load, "modules": list(shard)}
            for index, (shard, load) in enumerate(zip(plan.shards, plan.loads, strict=True))
        ],
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--shard", type=int, help="print this shard's module names, one per line")
    mode.add_argument("--plan", action="store_true", help="print the whole plan as JSON")
    mode.add_argument("--check", action="store_true", help="validate the weight data and the plan")
    parser.add_argument("--num-shards", type=int, default=DEFAULT_NUM_SHARDS)
    parser.add_argument(
        "--select",
        choices=SELECTIONS,
        default="all",
        help="'all' plans every module; 'makefile' plans only the Makefile-coupled ones (#702)",
    )
    parser.add_argument("--weights", type=Path, default=DEFAULT_WEIGHTS)
    parser.add_argument("--tools-dir", type=Path, default=TOOLS_DIR, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    try:
        data = load_weight_data(args.weights)
        modules = tracked = discover_modules(args.tools_dir)
        if not modules:
            raise ShardDataError(f"no {TEST_GLOB} modules found under {args.tools_dir}")
        if args.select == "makefile":
            modules = makefile_coupled_modules(modules, args.tools_dir)
            if not modules:
                raise ShardDataError("the Makefile-coupled selection is empty")
        plan = plan_shards(modules, data, args.num_shards)
    except ShardDataError as exc:
        print(f"ci_test_shards: {exc}", file=sys.stderr)
        return 2

    if args.shard is not None:
        if not 0 <= args.shard < args.num_shards:
            print(f"ci_test_shards: --shard must be in [0, {args.num_shards})", file=sys.stderr)
            return 2
        # Stale or unmeasured data never stops a shard from running its tests (the
        # --check gate owns that); a plan that drops, duplicates or co-locates
        # modules, or leaves this shard empty, does.
        blocking = [
            error
            for error in check_plan(modules, data, plan, tracked=tracked)
            if error.startswith(("plan ", f"shard {args.shard} "))
        ]
        if blocking:
            for error in blocking:
                print(f"ci_test_shards: {error}", file=sys.stderr)
            return 2
        shard = plan.shards[args.shard]
        print(
            f"ci_test_shards: shard {args.shard} of {args.num_shards} ({args.select}): {len(shard)} modules, "
            f"estimated {plan.loads[args.shard]:.1f}s "
            f"(shard estimates: {', '.join(f'{load:.1f}s' for load in plan.loads)})",
            file=sys.stderr,
        )
        for module in shard:
            print(module)
        return 0

    if args.plan:
        print(json.dumps(_plan_summary(plan), indent=2))
        return 0

    errors = check_plan(modules, data, plan, tracked=tracked)
    for index, (shard, load) in enumerate(zip(plan.shards, plan.loads, strict=True)):
        print(f"shard {index}: {len(shard)} modules, estimated {load:.1f}s")
    if plan.unmeasured:
        print(
            f"{len(plan.unmeasured)} unmeasured modules use default_seconds={data.default_seconds:g}: "
            + ", ".join(plan.unmeasured)
        )
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
