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

EXPECTED_ALL_CASES = SECTION_A_CASES + SECTION_B_CASES + SECTION_C_CASES + SECTION_D_CASES


@dataclass(frozen=True)
class PhaseBReport:
    raw_record_count: int
    section_a_count: int
    section_b_count: int
    section_c_count: int
    section_d_count: int
    completed: bool
    all_passed: bool
    results: dict[str, TestResult]


def parse_phaseb_output(text: str) -> PhaseBReport:
    """Parse and validate a captured PSP-PHASEB-001 stream."""
    parsed = parse_output(text)
    results: dict[str, TestResult] = {}
    for r in parsed.results:
        if r.test_id == EXPECTED_TEST_ID:
            results[r.case_id] = r

    sec_a = sum(1 for cid in SECTION_A_CASES if cid in results)
    sec_b = sum(1 for cid in SECTION_B_CASES if cid in results)
    sec_c = sum(1 for cid in SECTION_C_CASES if cid in results)
    sec_d = sum(1 for cid in SECTION_D_CASES if cid in results)

    completed = "PHASEB-COMPLETE" in results
    all_passed = completed and all(r.status == "PASS" for r in results.values())

    return PhaseBReport(
        raw_record_count=len(parsed.results),
        section_a_count=sec_a,
        section_b_count=sec_b,
        section_c_count=sec_c,
        section_d_count=sec_d,
        completed=completed,
        all_passed=all_passed,
        results=results,
    )
