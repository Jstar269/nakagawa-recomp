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
    "test_generic_title_planning_proof",
    "test_publish_audit",
    "test_title_runtime_config",
    "test_codegen_profile_isolation",
    "test_history_audit",
    "test_iso_parity",
    "test_provenance_attestation_gate",
    "test_title_manager_adapter",
    "test_publication_policy_gate",
    "test_savedata_spans",
    "test_hst_doctor_hardening",
    "test_vfs_contained",
    "test_visual_oracle",
    "test_hst_manager_manifest",
    "test_fp_scalar_mutations",
    "test_dispatch_call_boundary",
    "test_native_host_backends",
    "test_manager_safety",
    "test_title_manifest_parity",
    "test_progress_tracker",
    "test_native_gate_stub_link",
    "test_codegen_fp_convert",
    "test_elf_bounds",
    "test_codegen_madd_msub",
    "test_codegen_gate_b_encoding",
    "test_codegen_entry_semantics",
    "test_analyzer_span_scope",
]


def _init_worker(root_str: str, tools_str: str) -> None:
    import os
    import sys
    if root_str not in sys.path:
        sys.path.insert(0, root_str)
    if tools_str not in sys.path:
        sys.path.insert(0, tools_str)
    os.chdir(root_str)


def _run_module_worker(module_name: str) -> dict[str, object]:
    root = Path(__file__).resolve().parent.parent
    tools_dir = root / "tools"
    for p in (str(root), str(tools_dir)):
        if p not in sys.path:
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


def _contract_report(*, execute: bool, jobs: int = 1, verbose: bool = False) -> dict[str, object]:
    loader = unittest.TestLoader()
    suite = loader.discover(DISCOVERY_START, pattern=DISCOVERY_PATTERN)
    inventory_a = _ids(_flatten(suite))
    report: dict[str, object] = {
        "discovery_start": DISCOVERY_START,
        "discovery_pattern": DISCOVERY_PATTERN,
        "canonical_command": CANONICAL_COMMAND,
        "execution_requested": execute,
        "inventory_a": _counts(inventory_a),
    }
    if not execute:
        return report

    t0 = time.perf_counter()
    if jobs <= 1:
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
        root = Path(__file__).resolve().parent.parent
        tools_dir = root / "tools"
        modules = [
            f.stem for f in tools_dir.glob(DISCOVERY_PATTERN)
            if f.is_file() and not f.name.startswith((".", "_"))
        ]
        priority_map = {name: idx for idx, name in enumerate(_HEAVY_FIRST)}
        modules.sort(key=lambda m: (priority_map.get(m, 9999), m))

        if verbose:
            print(f"Running {len(modules)} test modules across {jobs} workers...", file=sys.stderr)

        inventory_b = []
        skip_records = []
        failures = 0
        errors = 0
        skipped = 0
        successful = True

        with concurrent.futures.ProcessPoolExecutor(
            max_workers=jobs,
            initializer=_init_worker,
            initargs=(str(root), str(tools_dir)),
        ) as executor:
            futures = {executor.submit(_run_module_worker, m): m for m in modules}
            for future in concurrent.futures.as_completed(futures):
                mod_name = futures[future]
                mod_result = future.result()
                inventory_b.extend(mod_result["started_ids"])
                skip_records.extend(mod_result["skip_records"])
                failures += len(mod_result["failures"])
                errors += len(mod_result["errors"])
                skipped += mod_result["skipped"]
                if not mod_result["successful"]:
                    successful = False
                if not mod_result["successful"] or mod_result["failures"] or mod_result["errors"]:
                    print(f"FAILED: {mod_name}: failures={mod_result['failures']}, errors={mod_result['errors']}", file=sys.stderr)
                if verbose:
                    status = "OK" if mod_result["successful"] else "FAIL"
                    print(f"  [{status}] {mod_name} ({len(mod_result['started_ids'])} tests)", file=sys.stderr)

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
            "failures": failures,
            "errors": errors,
            "skipped": skipped,
            "successful": successful,
            "wall_clock_seconds": round(elapsed, 2),
            "workers": jobs,
        }
    )
    return report


def _assert_contract(report: dict[str, object]) -> None:
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
    if not report.get("successful"):
        raise ValueError("canonical suite failed; discovery accounting is not a green contract")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="execute the suite and record startTest IDs")
    parser.add_argument("-j", "--jobs", type=int, default=1, help="number of parallel workers (0 for all CPU cores)")
    parser.add_argument("--parallel", action="store_true", help="run tests in parallel using all available CPU cores")
    parser.add_argument("-v", "--verbose", action="store_true", help="display progress information")
    parser.add_argument("--assert-contract", action="store_true", help="fail if the observed difference is not class-skip-only")
    parser.add_argument("--output", type=Path, help="write deterministic JSON to this path")
    args = parser.parse_args(argv)

    jobs = args.jobs
    if args.parallel or jobs <= 0:
        jobs = os.cpu_count() or 1
    execute = args.run or args.parallel or (args.jobs > 1)

    try:
        report = _contract_report(execute=execute, jobs=jobs, verbose=args.verbose)
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

