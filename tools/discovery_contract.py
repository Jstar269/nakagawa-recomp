# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Inventory and verify the repository's unittest discovery contract.

``unittest.TestLoader`` can enumerate methods that are never started when a
test class's ``setUpClass`` raises ``SkipTest``.  The canonical runner's
``Ran N tests`` value is therefore the set of cases that reach ``startTest``.
This tool records both inventories so a count difference is explained rather
than treated as a cleanup opportunity.

Fast inventory (no test execution)::

    python tools/discovery_contract.py --output build/discovery-a.json

Authoritative comparison (executes the canonical suite once)::

    python tools/discovery_contract.py --run --assert-contract \
        --output build/discovery-contract.json

The normal project command remains ``python -m unittest discover -s tools
-p 'test_*.py'``; this diagnostic uses the same start directory and pattern.
"""

from __future__ import annotations

import argparse
from collections import Counter
import concurrent.futures
import io
import json
import os
from pathlib import Path
import random
import re
import sys
import importlib
import time
import unittest


ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))

DISCOVERY_START = "tools"
DISCOVERY_PATTERN = "test_*.py"
CANONICAL_COMMAND = "python -m unittest discover -s tools -p 'test_*.py'"
_CLASS_SKIP_RE = re.compile(r"^setUpClass \(([^)]+)\)$")
_MODULE_SKIP_RE = re.compile(r"^setUpModule \(([^)]+)\)$")

# Prioritize long-running and compiler/subprocess-heavy modules first
# so they don't bottleneck the end of the parallel run.
_HEAVY_FIRST = [
    "test_build_truth",
    "test_provenance_ledger",
    "test_parse_fuzz",
    "test_provenance_attest_verify",
    "test_public_export",
    "test_publish_audit",
    "test_generic_title_planning_proof",
    "test_fp_scalar_mutations",
    "test_iso_parity",
    "test_title_manifest_parity",
    "test_native_host_backends",
    "test_second_title_ingest",
    "test_title_catalog",
    "test_title_manager_adapter",
    "test_production_smoke",
    "test_platform_ladder",
    "test_fast_math_primitives",
    "test_hle_title_config_behavior",
    "test_history_audit",
    "test_recomp_ast_visitor",
    "test_recomp_symbols",
    "test_relocs",
    "test_recompiler_differential",
    "test_recompiler_matrix",
    "test_recompiler_unit",
    "test_roundtrip",
    "test_elf",
    "test_elf_reloc_applied",
    "test_codegen_entry_contract",
    "test_codegen_gate_b_encoding",
    "test_codegen_entry_semantics",
    "test_analyzer_span_scope",
]


# Modules that deliberately mutate tracked repository state, which every other
# module may be reading at the same time.  Rewriting such a test to work on a
# temp-directory copy is the right answer when its claim is about a *mechanism*
# and the wrong answer when its claim is about the *tracked tree itself*: a copy
# outside the repository carries no publication-policy identity, so the
# rewritten assertion quietly tests a weaker property than the original.  These
# run in their own sequential lane once the pool has drained.
_SERIAL_ONLY = frozenset({
    "test_title_catalog",
    "test_publication_policy_gate",
})


def _init_worker(root_str: str, tools_str: str, extra_dirs: tuple[str, ...] = ()) -> None:
    import os
    import sys
    for p in (root_str, tools_str, *extra_dirs):
        if p and p not in sys.path:
            sys.path.insert(0, p)
    os.chdir(root_str)


def _run_module_worker(module_name: str, search_dirs: tuple[str, ...] = ()) -> dict[str, object]:
    root = Path(__file__).resolve().parent.parent
    tools_dir = root / "tools"
    for p in (str(root), str(tools_dir), *search_dirs):
        if p and p not in sys.path:
            sys.path.insert(0, p)
    os.chdir(root)
    try:
        mod = importlib.import_module(module_name)
        suite = unittest.defaultTestLoader.loadTestsFromModule(mod)
        stream = io.StringIO()
        runner = unittest.TextTestRunner(
            stream=stream,
            verbosity=0,
            resultclass=_RecordingResult,
        )
        result = runner.run(suite)
        return {
            "module": module_name,
            "started_ids": result.started_ids,
            "skip_records": result.skip_records,
            "failures": [(t.id() if hasattr(t, "id") else str(t), err) for t, err in result.failures],
            "errors": [(t.id() if hasattr(t, "id") else str(t), err) for t, err in result.errors],
            "skipped": len(result.skipped),
            "successful": result.wasSuccessful(),
        }
    except Exception as exc:
        return {
            "module": module_name,
            "started_ids": [],
            "skip_records": [],
            "failures": [],
            "errors": [(module_name, str(exc))],
            "skipped": 0,
            "successful": False,
        }




def _flatten(suite: unittest.TestSuite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from _flatten(item)
        else:
            yield item


def _ids(tests) -> list[str]:
    return [test.id() for test in tests]


def _counts(ids: list[str]) -> dict[str, object]:
    duplicates = {
        test_id: count for test_id, count in Counter(ids).items() if count > 1
    }
    return {
        "count": len(ids),
        "unique_count": len(set(ids)),
        "duplicates": duplicates,
        "ids": sorted(ids),
    }


class _RecordingResult(unittest.TestResult):
    """Keep the IDs that actually reached the runner's startTest hook."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.started_ids: list[str] = []
        self.skip_records: list[dict[str, str]] = []

    def startTest(self, test: unittest.TestCase) -> None:
        self.started_ids.append(test.id())
        super().startTest(test)

    def addSkip(self, test: unittest.TestCase, reason: str) -> None:
        self.skip_records.append({"id": test.id(), "reason": str(reason)})
        super().addSkip(test, reason)


def _module_class(test_id: str) -> str:
    parts = test_id.split(".")
    return ".".join(parts[:2]) if len(parts) >= 2 else test_id


def _module(test_id: str) -> str:
    return test_id.split(".", 1)[0]


def _contract_report(
    *,
    execute: bool,
    jobs: int = 1,
    verbose: bool = False,
    seed: int | None = None,
    start_dir: str | Path | None = None,
    pattern: str | None = None,
    serial_only: frozenset[str] | None = None,
    quiet: bool = False,
) -> dict[str, object]:
    root = Path(__file__).resolve().parent.parent
    tools_dir = root / "tools"
    target_dir = (
        (Path(start_dir) if Path(start_dir).is_absolute() else (root / start_dir))
        if start_dir is not None
        else tools_dir
    )
    pattern_str = pattern or DISCOVERY_PATTERN
    active_serial = serial_only if serial_only is not None else _SERIAL_ONLY

    # Clear any cached modules matching pattern so repeated discovery across tempdirs does not collide
    for f in target_dir.glob(pattern_str):
        sys.modules.pop(f.stem, None)

    loader = unittest.TestLoader()
    suite = loader.discover(str(target_dir), pattern=pattern_str)
    inventory_a = _ids(_flatten(suite))
    report: dict[str, object] = {
        "discovery_start": str(target_dir),
        "discovery_pattern": pattern_str,
        "canonical_command": CANONICAL_COMMAND,
        "execution_requested": execute,
        "inventory_a": _counts(inventory_a),
    }
    if not execute:
        return report

    t0 = time.perf_counter()
    if jobs <= 1:
        if str(target_dir) not in sys.path:
            sys.path.insert(0, str(target_dir))
        stream = io.StringIO()
        runner = unittest.TextTestRunner(
            stream=stream,
            verbosity=0,
            resultclass=_RecordingResult,
        )
        result = runner.run(suite)
        inventory_b = result.started_ids
        skip_records = result.skip_records
        failures = len(result.failures)
        errors = len(result.errors)
        skipped = len(result.skipped)
        successful = result.wasSuccessful()
    else:
        modules = [
            f.stem for f in target_dir.glob(pattern_str)
            if f.is_file() and not f.name.startswith((".", "_"))
        ]
        priority_map = {name: idx for idx, name in enumerate(_HEAVY_FIRST)}
        modules.sort(key=lambda m: (priority_map.get(m, 9999), m))
        parallel_modules = [m for m in modules if m not in active_serial]
        serial_modules = [m for m in modules if m in active_serial]

        if seed is not None:
            rng = random.Random(seed)
            rng.shuffle(parallel_modules)
            if len(serial_modules) > 1:
                rng.shuffle(serial_modules)

        if verbose:
            print(
                f"Running {len(parallel_modules)} test modules across {jobs} workers, "
                f"then {len(serial_modules)} serial-only module(s)...",
                file=sys.stderr,
            )

        inventory_b = []
        skip_records = []
        failures = 0
        errors = 0
        skipped = 0
        successful = True

        def _absorb(mod_name: str, mod_result: dict[str, object]) -> None:
            nonlocal failures, errors, skipped, successful
            inventory_b.extend(mod_result["started_ids"])
            skip_records.extend(mod_result["skip_records"])
            failures += len(mod_result["failures"])
            errors += len(mod_result["errors"])
            skipped += mod_result["skipped"]
            if not mod_result["successful"]:
                successful = False
            # quiet is for callers that are deliberately running a failing
            # module to prove this aggregator notices. Without it a green suite
            # prints FAILED lines from test_discovery_contract's own fixtures.
            if not quiet and (
                not mod_result["successful"] or mod_result["failures"] or mod_result["errors"]
            ):
                print(f"FAILED: {mod_name}: failures={mod_result['failures']}, errors={mod_result['errors']}", file=sys.stderr)
            if verbose:
                status = "OK" if mod_result["successful"] else "FAIL"
                print(f"  [{status}] {mod_name} ({len(mod_result['started_ids'])} tests)", file=sys.stderr)

        search_dirs = (str(target_dir),) if target_dir.resolve() != tools_dir.resolve() else ()
        with concurrent.futures.ProcessPoolExecutor(
            max_workers=jobs,
            initializer=_init_worker,
            initargs=(str(root), str(tools_dir), search_dirs),
        ) as executor:
            futures = {executor.submit(_run_module_worker, m, search_dirs): m for m in parallel_modules}
            for future in concurrent.futures.as_completed(futures):
                _absorb(futures[future], future.result())

        # Sequential lane.  Nothing from the pool is still in flight here, so a
        # module in this list may mutate the tracked tree as long as it restores
        # it.  Kept in a worker process rather than inlined so that both lanes
        # collect results through exactly the same code path.
        if serial_modules:
            with concurrent.futures.ProcessPoolExecutor(
                max_workers=1,
                initializer=_init_worker,
                initargs=(str(root), str(tools_dir), search_dirs),
            ) as executor:
                for mod_name in serial_modules:
                    _absorb(mod_name, executor.submit(_run_module_worker, mod_name, search_dirs).result())

    elapsed = time.perf_counter() - t0
    a_set = set(inventory_a)
    b_set = set(inventory_b)
    class_skips = []
    module_skips = []
    for record in skip_records:
        if _CLASS_SKIP_RE.match(record["id"]):
            class_skips.append(record)
        elif _MODULE_SKIP_RE.match(record["id"]):
            module_skips.append(record)
    sorted_skips = sorted(skip_records, key=lambda r: (r.get("id", ""), r.get("reason", "")))
    report.update(
        {
            "inventory_b": _counts(inventory_b),
            "a_only": sorted(a_set - b_set),
            "b_only": sorted(b_set - a_set),
            "module_a_only": sorted(
                {_module(test_id) for test_id in a_set}
                - {_module(test_id) for test_id in b_set}
            ),
            "module_b_only": sorted(
                {_module(test_id) for test_id in b_set}
                - {_module(test_id) for test_id in a_set}
            ),
            "class_a_only": sorted(
                {_module_class(test_id) for test_id in a_set}
                - {_module_class(test_id) for test_id in b_set}
            ),
            "class_b_only": sorted(
                {_module_class(test_id) for test_id in b_set}
                - {_module_class(test_id) for test_id in a_set}
            ),
            "class_level_skips": class_skips,
            "module_level_skips": module_skips,
            "skip_records": sorted_skips,
            "skip_reasons": {r["id"]: r.get("reason", "") for r in sorted_skips},
            "failures": failures,
            "errors": errors,
            "skipped": skipped,
            "successful": successful,
            "wall_clock_seconds": round(elapsed, 2),
            "workers": jobs,
            "seed": seed,
        }
    )
    return report


def _assert_contract(
    report: dict[str, object],
    *,
    expected_skip_reasons: dict[str, str] | None = None,
) -> None:
    if not report.get("execution_requested"):
        raise ValueError("--assert-contract requires --run")
    if report.get("b_only") or report.get("module_b_only"):
        raise ValueError("authoritative execution discovered IDs absent from loader inventory")
    class_skip_ids = {
        match.group(1)
        for record in report.get("class_level_skips", [])
        if (match := _CLASS_SKIP_RE.match(record["id"]))
    }
    unexplained = [
        test_id
        for test_id in report.get("a_only", [])
        if _module_class(test_id) not in class_skip_ids
    ]
    if unexplained:
        raise ValueError(
            "loader-only IDs are not covered by a class-level SkipTest: "
            + ", ".join(unexplained)
        )
    for record in report.get("skip_records", []):
        if not record.get("reason"):
            raise ValueError(f"skip record has empty reason: {record.get('id')}")
    if expected_skip_reasons is not None:
        actual_reasons = {r["id"]: r.get("reason", "") for r in report.get("skip_records", [])}
        if actual_reasons != expected_skip_reasons:
            diff = set(actual_reasons.items()) ^ set(expected_skip_reasons.items())
            raise ValueError(f"skip reasons disagree with expected contract: {diff}")
    if not report.get("successful"):
        raise ValueError("canonical suite failed; discovery accounting is not a green contract")


def assert_parity(report1: dict[str, object], report2: dict[str, object]) -> None:
    """Assert identical collection, skip reasons, and execution outcomes between two runs."""
    ids1 = report1.get("inventory_b", {}).get("ids", [])
    ids2 = report2.get("inventory_b", {}).get("ids", [])
    if ids1 != ids2:
        diff_1_not_2 = set(ids1) - set(ids2)
        diff_2_not_1 = set(ids2) - set(ids1)
        raise AssertionError(
            f"Collection parity mismatch: {len(diff_1_not_2)} in report1 only, {len(diff_2_not_1)} in report2 only"
        )

    skips1 = Counter((r["id"], r.get("reason", "")) for r in report1.get("skip_records", []))
    skips2 = Counter((r["id"], r.get("reason", "")) for r in report2.get("skip_records", []))
    if skips1 != skips2:
        diff_skips1 = skips1 - skips2
        diff_skips2 = skips2 - skips1
        raise AssertionError(
            f"Skip parity mismatch:\nreport1 extra: {diff_skips1}\nreport2 extra: {diff_skips2}"
        )

    for metric in ("failures", "errors", "skipped", "successful"):
        val1 = report1.get(metric)
        val2 = report2.get(metric)
        if val1 != val2:
            raise AssertionError(f"Outcome parity mismatch on '{metric}': {val1} != {val2}")


def resolve_jobs(requested: int | None, *, parallel: bool) -> int:
    """Return the worker count for the command line.

    An explicit ``-j N`` always wins, including alongside ``--parallel``: a
    caller who caps the pool (to keep a machine responsive, or to bound power
    draw) must not be silently overridden with every core. ``--parallel`` alone,
    or ``-j 0``, means all CPU cores; neither means a single serial worker.
    """
    cores = os.cpu_count() or 1
    if requested is None:
        return cores if parallel else 1
    if requested <= 0:
        return cores
    return requested


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="execute the suite and record startTest IDs")
    parser.add_argument("-j", "--jobs", type=int, default=None, help="number of parallel workers (0 for all CPU cores)")
    parser.add_argument(
        "--parallel",
        action="store_true",
        help="run tests in parallel; uses all CPU cores unless -j/--jobs sets the worker count",
    )
    parser.add_argument("--start-dir", default=DISCOVERY_START, help="directory to discover tests in")
    parser.add_argument("-p", "--pattern", default=DISCOVERY_PATTERN, help="pattern to match test files")
    parser.add_argument("--seed", type=int, default=None, help="random seed to shuffle module execution order")
    parser.add_argument("-v", "--verbose", action="store_true", help="display progress information")
    parser.add_argument("--assert-contract", action="store_true", help="fail if the observed difference is not class-skip-only")
    parser.add_argument("--output", type=Path, help="write deterministic JSON to this path")
    args = parser.parse_args(argv)

    jobs = resolve_jobs(args.jobs, parallel=args.parallel)
    execute = args.run or args.parallel or jobs > 1

    try:
        report = _contract_report(
            execute=execute,
            jobs=jobs,
            verbose=args.verbose,
            seed=args.seed,
            start_dir=args.start_dir,
            pattern=args.pattern,
        )
    except (OSError, ValueError, unittest.case.SkipTest) as exc:
        print(f"discovery contract: {exc}", file=sys.stderr)
        return 2

    if execute and not args.output:
        status_str = "OK" if report["successful"] else "FAILED"
        print(
            f"Ran {report['inventory_b']['count']} tests in {report.get('wall_clock_seconds', 0.0):.2f}s "
            f"(workers={jobs}): {status_str} (skipped={report['skipped']}, failures={report['failures']}, errors={report['errors']})",
            file=sys.stderr,
        )

    if args.assert_contract:
        try:
            _assert_contract(report)
        except ValueError as exc:
            print(f"discovery contract: {exc}", file=sys.stderr)
            if not report.get("successful"):
                print(f"Failures: {report.get('failures')}, Errors: {report.get('errors')}", file=sys.stderr)
            return 2

    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8", newline="\n")
    elif not execute:
        print(encoded, end="")
    return 0 if report.get("successful", True) else 1


if __name__ == "__main__":
    raise SystemExit(main())
