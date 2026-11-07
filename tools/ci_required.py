# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Evaluate the stable aggregate status for the path-gated CI workflow."""

from __future__ import annotations

import os
import sys
from typing import Mapping


# The path-gated jobs. The Markdown lint and the main smoke are steps of the
# always-required hygiene job, and the profile-zero fixture build is a step of the
# always-required public-export job (.github/workflows/ci.yml), so their results
# reach this aggregate through those two jobs.
_GATES = (
    ("python-tools", "PYTHON_RESULT", "RUN_PYTHON"),
    ("native-tools", "NATIVE_RESULT", "RUN_NATIVE"),
    # #360: the Android launcher build runs whenever the native gates do.
    ("android-build", "ANDROID_RESULT", "RUN_NATIVE"),
    ("windows-runtime", "WINDOWS_RESULT", "RUN_WINDOWS"),
)

# The run flags of the steps folded into hygiene. Their results arrive through
# HYGIENE_RESULT, but each flag must still be an explicit boolean: a missing or
# misnamed classifier output would otherwise skip the Markdown lint or the main
# smoke step silently while hygiene stays green.
_FOLDED_STEP_FLAGS = ("RUN_MARKDOWN", "RUN_MAIN_SMOKE")

_SUBSTANTIVE_GATES = frozenset(
    {"python-tools", "native-tools", "android-build", "windows-runtime"}
)

#: The reason a Draft pull request's run is never green. A Draft runs only the quick
#: jobs (tools/ci_paths.py sets ``allow_substantive`` false for it); the full checks run
#: when it is marked Ready. tools/ci_trust_verdict.py reports the same code from the pull
#: request's own draft state (docs/CI.md, "Draft pull requests").
DRAFT_LIGHT_CI = "DRAFT_LIGHT_CI"

#: The jobs a Draft's run still executes, as named in this aggregate's results.
_QUICK_JOBS = (
    ("classify", "CLASSIFY_RESULT"),
    ("hygiene", "HYGIENE_RESULT"),
    ("public-export", "PUBLIC_EXPORT_RESULT"),
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
    classifier, hygiene, or public-export failures can never be hidden by path
    gating. The public-export job always runs, so it must report success.

    A Draft pull request's run never passes (``DRAFT_LIGHT_CI``), whatever its
    jobs did: it ran only the quick jobs, so it cannot stand for the head. The
    full checks run when the pull request is marked Ready. ``allow_substantive``
    false without ``draft`` is the normal main-push policy, which skips the
    substantive jobs after the merged pull request has already run them.
    """

    if (
        results.get("classify") != "success"
        or results.get("hygiene") != "success"
        or results.get("public-export") != "success"
    ):
        return False

    if draft:
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
        "public-export": environment.get("PUBLIC_EXPORT_RESULT", ""),
    }
    for run_key in _FOLDED_STEP_FLAGS:
        if _parse_boolean(environment.get(run_key)) is None:
            return False
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


def draft_light_run(environment: Mapping[str, str]) -> bool:
    """Whether the run belongs to a Draft pull request (the classifier's ``draft`` output)."""

    return _parse_boolean(environment.get("DRAFT")) is True


def main() -> int:
    if evaluate_environment(os.environ):
        print("All applicable CI gates passed; skipped jobs were intentionally irrelevant or policy-suppressed.")
        return 0
    if draft_light_run(os.environ):
        quick = ", ".join(f"{name} {os.environ.get(key) or 'missing'}" for name, key in _QUICK_JOBS)
        print(
            f"::error title={DRAFT_LIGHT_CI}::Draft pull request: only the quick checks ran. "
            "Mark it Ready for review to run the full checks."
        )
        print(
            f"{DRAFT_LIGHT_CI}: this head belongs to a Draft pull request, so CI ran only the quick checks "
            f"({quick}) and skipped the Python, native and Windows gates. A Draft's run is never green. "
            "Mark the pull request Ready for review to run the full checks (docs/CI.md, \"Draft pull requests\").",
            file=sys.stderr,
        )
        return 1
    print(
        "An applicable CI gate failed, was cancelled, was suppressed by policy, or the classifier, hygiene, or public-export gate did not pass.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
