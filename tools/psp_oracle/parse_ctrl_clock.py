# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Host-side parser for PSP-CTRL-001 controller and clock probe output."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
from typing import Any

from .protocol import parse_output, ProtocolError, TestResult

EXPECTED_TEST_IDS = frozenset({"PSP-CTRL-001", "PSP-SYSTEM-001"})

EXPECTED_SYSTEM_CASES = [
    "ctrl-ts-pairs",
    "ctrl-ts-vcount",
    "clock-snapshot",
    "delay-10ms",
    "ctrl-zero-count",
    "ctrl-peek-read",
]

EXPECTED_CTRL_CASES = [
    "ctrl-clock-freqs",
    "ctrl-delay-calibration",
]


@dataclass(frozen=True)
class CtrlClockReport:
    raw_record_count: int
    cpu_freq_mhz: int
    bus_freq_mhz: int
    delay_measured_us: int
    complete: bool
    all_passed: bool
    results: dict[str, TestResult]


def parse_ctrl_clock_output(text: str, *, require_complete: bool = True) -> CtrlClockReport:
    """Parse and validate a captured PSP-CTRL-001 or PSP-SYSTEM-001 stream.

    ``require_complete`` decides whether an incomplete stream raises or is
    returned for inspection; it never relaxes a verdict.  ``complete`` is
    computed from the observed case sequence and gates ``all_passed`` in both
    modes, so a partial stream can never report ``all_passed``.
    """
    parsed = parse_output(text)
    ordered_results: list[TestResult] = []
    seen: set[str] = set()
    active_test_id: str | None = None

    for r in parsed.results:
        if r.test_id in EXPECTED_TEST_IDS:
            if active_test_id is None:
                active_test_id = r.test_id
            elif r.test_id != active_test_id:
                raise ProtocolError(f"mixed test_ids in stream: {active_test_id} and {r.test_id}")
            if r.case_id in seen:
                raise ProtocolError(f"duplicate ctrl/clock case: {r.case_id}")
            seen.add(r.case_id)
            ordered_results.append(r)

    if not ordered_results or active_test_id is None:
        raise ProtocolError("missing required ctrl/clock test records")

    expected_cases = (
        EXPECTED_SYSTEM_CASES if active_test_id == "PSP-SYSTEM-001" else EXPECTED_CTRL_CASES
    )
    results = {r.case_id: r for r in ordered_results}
    actual_order = [r.case_id for r in ordered_results]

    if require_complete:
        if actual_order != expected_cases:
            missing = [cid for cid in expected_cases if cid not in results]
            if missing:
                raise ProtocolError(f"ctrl/clock stream incomplete, missing: {missing}")
            extra = [cid for cid in actual_order if cid not in expected_cases]
            if extra:
                raise ProtocolError(f"ctrl/clock stream contains unexpected cases: {extra}")
            raise ProtocolError(
                f"ctrl/clock cases out of order: expected {expected_cases}, got {actual_order}"
            )
    else:
        expected_indices = {cid: idx for idx, cid in enumerate(expected_cases)}
        indices = [expected_indices.get(cid, -1) for cid in actual_order if cid in expected_indices]
        if any(indices[i] > indices[i + 1] for i in range(len(indices) - 1)):
            raise ProtocolError(f"ctrl/clock cases out of order: {actual_order}")

    cpu_mhz = 0
    bus_mhz = 0
    delay_us = 0

    if "ctrl-clock-freqs" in results:
        v = dict(results["ctrl-clock-freqs"].values)
        cpu_mhz = int(v.get("out0", "0"), 0)
        bus_mhz = int(v.get("out1", "0"), 0)

    if "clock-snapshot" in results:
        v = dict(results["clock-snapshot"].values)
        cpu_mhz = int(v.get("out5", "0"), 0)
        bus_mhz = cpu_mhz // 2 if cpu_mhz > 0 else 0

    if "ctrl-delay-calibration" in results:
        v = dict(results["ctrl-delay-calibration"].values)
        delay_us = int(v.get("out0", "0"), 0)

    if "delay-10ms" in results:
        v = dict(results["delay-10ms"].values)
        delay_us = int(v.get("out2", "0"), 0)

    # Completeness is a property of the observed case sequence, never of the
    # caller's strictness flag.
    complete = actual_order == expected_cases
    all_passed = complete and all(r.status == "PASS" for r in results.values())

    return CtrlClockReport(
        raw_record_count=len(parsed.results),
        cpu_freq_mhz=cpu_mhz,
        bus_freq_mhz=bus_mhz,
        delay_measured_us=delay_us,
        complete=complete,
        all_passed=all_passed,
        results=results,
    )
