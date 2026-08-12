# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Evaluate the stable aggregate status for the path-gated CI workflow."""

from __future__ import annotations

import os
import sys
from typing import Mapping


_GATES = (
    ("markdown", "MARKDOWN_RESULT", "RUN_MARKDOWN"),
    ("python-tools", "PYTHON_RESULT", "RUN_PYTHON"),
    ("native-tools", "NATIVE_RESULT", "RUN_NATIVE"),
    ("dashboard", "DASHBOARD_RESULT", "RUN_DASHBOARD"),
    ("windows-runtime", "WINDOWS_RESULT", "RUN_WINDOWS"),
    ("main-smoke", "MAIN_SMOKE_RESULT", "RUN_MAIN_SMOKE"),
)

_SUBSTANTIVE_GATES = frozenset(
    {"python-tools", "native-tools", "dashboard", "windows-runtime"}
)


def _parse_boolean(value: str | None) -> bool | None:
    """Parse the exact boolean contract emitted through GitHub job outputs."""

    if value is None:
        return None
    normalised = value.strip().lower()
    if normalised == "true":
        return True
    if normalised == "false":
        return False
    return None


def required_gate_passes(
    results: Mapping[str, str],
    applicable: Mapping[str, bool],
    *,
    allow_substantive: bool = True,
    draft: bool = False,
) -> bool:
    """Return whether every required or applicable job completed successfully.

    ``skipped`` is accepted only for a gate that is not applicable. A failed,
    cancelled, or otherwise incomplete applicable gate fails the aggregate, and
    classifier or hygiene failures can never be hidden by path gating. Draft
    PRs are fail-closed when path classification requested a substantive gate:
    the suppressed run stays red until a ready-for-review run executes it.
    """

    if results.get("classify") != "success" or results.get("hygiene") != "success":
        return False

    # A draft PR may request substantive gates by path, but the workflow
    # deliberately suppresses those jobs until ready_for_review.  That
    # suppression must never be interpreted as a green exact-head result: if
    # the ready transition does not produce a new run, the draft's last check
    # must remain visibly non-green.  ``draft=False`` is intentional for the
    # normal main-push policy, which also skips substantive jobs after their PR
    # gates have already run.
    draft_suppressed = draft and not allow_substantive
    if draft_suppressed and any(applicable.get(name, False) for name in _SUBSTANTIVE_GATES):
        return False

    for name, _result_key, _run_key in _GATES:
        should_run = applicable.get(name, False)
        if not allow_substantive and name in _SUBSTANTIVE_GATES:
            should_run = False
        if should_run and results.get(name) != "success":
            return False
    return True


def evaluate_environment(environment: Mapping[str, str]) -> bool:
    results = {
        "classify": environment.get("CLASSIFY_RESULT", ""),
        "hygiene": environment.get("HYGIENE_RESULT", ""),
    }
    applicable: dict[str, bool] = {}
    for name, result_key, run_key in _GATES:
        results[name] = environment.get(result_key, "")
        parsed = _parse_boolean(environment.get(run_key))
        if parsed is None:
            return False
        applicable[name] = parsed
    allow_substantive = _parse_boolean(environment.get("ALLOW_SUBSTANTIVE"))
    if allow_substantive is None:
        return False
    draft = _parse_boolean(environment.get("DRAFT"))
    if draft is None:
        return False
    return required_gate_passes(
        results,
        applicable,
        allow_substantive=allow_substantive,
        draft=draft,
    )


def main() -> int:
    if evaluate_environment(os.environ):
        print("All applicable CI gates passed; skipped jobs were intentionally irrelevant or policy-suppressed.")
        return 0
    print(
        "An applicable CI gate failed, was cancelled, was suppressed for a draft, or the classifier/hygiene gate did not pass.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
