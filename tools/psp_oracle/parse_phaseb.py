# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Host-side parser for PSP-PHASEB-001 resident money-launch output."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
from typing import Any

from .protocol import parse_output, ProtocolError, TestResult

EXPECTED_TEST_ID = "PSP-PHASEB-001"

SECTION_A_CASES = [
    "ED2-R77", "ED2-R00", "ED2-RNEG", "ED2-RERR",
    "ED2-X77", "ED2-X00", "ED2-XNEG", "ED2-XERR",
    "ED2-D77", "ED2-D00", "ED2-DNEG", "ED2-DERR",
]

SECTION_B_CASES = [
    "MTX-CR-INIT0", "MTX-CR-INIT1", "MTX-CR-BADATTR", "MTX-CR-BADCNT", "MTX-CR-REC",
    "MTX-LK-UNCONT", "MTX-LK-NOREC", "MTX-UL-UNCONT", "MTX-LK-REC", "MTX-UL-NOTOWN",
    "MTX-TRY-LOCK", "MTX-TRY-FAIL", "MTX-TO-ZERO", "MTX-TO-FINITE",
    "MTX-CANCEL", "MTX-DEL-WAKE", "MTX-WAIT-FIFO", "MTX-WAIT-PRIO",
    "LWM-CR-LK-UL",
]

SECTION_C_CASES = [
    "CB-ORDINARY-EXEC", "CB-STACK-LOC", "CB-STACK-DELTA",
    "CB-REG-PRESERVE", "CB-NESTED-DEPTH2", "CB-FCR31",
]

SECTION_D_CASES = [
    "GE-CB-FIRED", "GE-CB-THREAD", "GE-CB-STACK", "GE-CB-CLEANUP",
]

SUMMARY_CASE = "PHASEB-COMPLETE"
EXPECTED_ALL_CASES = SECTION_C_CASES + SECTION_D_CASES + SECTION_B_CASES + SECTION_A_CASES
EXPECTED_ORDERED_CASES = EXPECTED_ALL_CASES + [SUMMARY_CASE]


@dataclass(frozen=True)
class PhaseBReport:
    raw_record_count: int
    section_a_count: int
    section_b_count: int
    section_c_count: int
    section_d_count: int
    summary_present: bool
    completed: bool
    all_passed: bool
    results: dict[str, TestResult]


def parse_phaseb_output(text: str, *, require_complete: bool = True) -> PhaseBReport:
    """Parse and validate a captured PSP-PHASEB-001 stream.

    ``require_complete=True`` is complete-or-error.  ``require_complete=False``
    permits *inspection* of an ordered prefix after a crash, and nothing more:
    ``completed`` means the exact 42-record C-D-B-A + summary sequence was
    observed, and ``all_passed`` is gated on it in both modes.  The presence of
    the ``PHASEB-COMPLETE`` summary record is reported separately as
    ``summary_present`` and never substitutes for completeness.
    """
    parsed = parse_output(text)
    ordered_results: list[TestResult] = []
    seen: set[str] = set()
    for r in parsed.results:
        if r.test_id == EXPECTED_TEST_ID:
            if r.case_id in seen:
                raise ProtocolError(f"duplicate phaseb case: {r.case_id}")
            seen.add(r.case_id)
            ordered_results.append(r)

    results = {r.case_id: r for r in ordered_results}
    actual_order = [r.case_id for r in ordered_results]

    if require_complete:
        if actual_order != EXPECTED_ORDERED_CASES:
            missing = [cid for cid in EXPECTED_ORDERED_CASES if cid not in results]
            if missing:
                raise ProtocolError(
                    f"phaseb stream incomplete, missing {len(missing)} cases: {missing[:5]}"
                )
            extra = [cid for cid in actual_order if cid not in EXPECTED_ORDERED_CASES]
            if extra:
                raise ProtocolError(f"phaseb stream contains unexpected cases: {extra}")
            raise ProtocolError(
                f"phaseb cases out of order: expected {EXPECTED_ORDERED_CASES}, got {actual_order}"
            )
    else:
        expected_indices = {cid: idx for idx, cid in enumerate(EXPECTED_ORDERED_CASES)}
        indices = [expected_indices.get(cid, -1) for cid in actual_order if cid in expected_indices]
        if any(indices[i] > indices[i + 1] for i in range(len(indices) - 1)):
            raise ProtocolError(f"phaseb cases out of order: {actual_order}")

    sec_a = sum(1 for cid in SECTION_A_CASES if cid in results)
    sec_b = sum(1 for cid in SECTION_B_CASES if cid in results)
    sec_c = sum(1 for cid in SECTION_C_CASES if cid in results)
    sec_d = sum(1 for cid in SECTION_D_CASES if cid in results)

    # Completeness is a property of the observed case sequence, never of the
    # caller's strictness flag, and never of the summary record alone.
    summary_present = SUMMARY_CASE in results
    completed = actual_order == EXPECTED_ORDERED_CASES
    all_passed = completed and all(r.status == "PASS" for r in results.values())

    return PhaseBReport(
        raw_record_count=len(parsed.results),
        section_a_count=sec_a,
        section_b_count=sec_b,
        section_c_count=sec_c,
        section_d_count=sec_d,
        summary_present=summary_present,
        completed=completed,
        all_passed=all_passed,
        results=results,
    )
