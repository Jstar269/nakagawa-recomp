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
import io
import json
from pathlib import Path
import re
import sys
import unittest


DISCOVERY_START = "tools"
DISCOVERY_PATTERN = "test_*.py"
CANONICAL_COMMAND = "python -m unittest discover -s tools -p 'test_*.py'"
_CLASS_SKIP_RE = re.compile(r"^setUpClass \(([^)]+)\)$")
_MODULE_SKIP_RE = re.compile(r"^setUpModule \(([^)]+)\)$")


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


def _contract_report(*, execute: bool) -> dict[str, object]:
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

    stream = io.StringIO()
    runner = unittest.TextTestRunner(
        stream=stream,
        verbosity=0,
        resultclass=_RecordingResult,
    )
    result = runner.run(suite)
    inventory_b = result.started_ids
    a_set = set(inventory_a)
    b_set = set(inventory_b)
    class_skips = []
    module_skips = []
    for record in result.skip_records:
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
            "failures": len(result.failures),
            "errors": len(result.errors),
            "skipped": len(result.skipped),
            "successful": result.wasSuccessful(),
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
    parser.add_argument("--assert-contract", action="store_true", help="fail if the observed difference is not class-skip-only")
    parser.add_argument("--output", type=Path, help="write deterministic JSON to this path")
    args = parser.parse_args(argv)

    try:
        report = _contract_report(execute=args.run)
        if args.assert_contract:
            _assert_contract(report)
    except (OSError, ValueError, unittest.case.SkipTest) as exc:
        print(f"discovery contract: {exc}", file=sys.stderr)
        return 2

    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8", newline="\n")
    else:
        print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
