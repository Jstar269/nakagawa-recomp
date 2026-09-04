# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Host-side parser for PSP-CTRL-001 controller and clock probe output."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
from typing import Any

from .protocol import parse_output, ProtocolError, TestResult

EXPECTED_TEST_ID = "PSP-CTRL-001"


@dataclass(frozen=True)
class CtrlClockReport:
    raw_record_count: int
    cpu_freq_mhz: int
    bus_freq_mhz: int
    delay_measured_us: int
    all_passed: bool
    results: dict[str, TestResult]


def parse_ctrl_clock_output(text: str) -> CtrlClockReport:
    """Parse and validate a captured PSP-CTRL-001 stream."""
    parsed = parse_output(text)
    results: dict[str, TestResult] = {}
    for r in parsed.results:
        if r.test_id == EXPECTED_TEST_ID:
            results[r.case_id] = r

    cpu_mhz = 0
    bus_mhz = 0
    if "ctrl-clock-freqs" in results:
        r_freq = results["ctrl-clock-freqs"]
        v = dict(r_freq.values)
        cpu_mhz = int(v.get("out0", "0"), 0)
        bus_mhz = int(v.get("out1", "0"), 0)

    delay_us = 0
    if "ctrl-delay-calibration" in results:
        r_del = results["ctrl-delay-calibration"]
        v = dict(r_del.values)
        delay_us = int(v.get("out0", "0"), 0)

    all_passed = len(results) > 0 and all(r.status == "PASS" for r in results.values())

    return CtrlClockReport(
        raw_record_count=len(parsed.results),
        cpu_freq_mhz=cpu_mhz,
        bus_freq_mhz=bus_mhz,
        delay_measured_us=delay_us,
        all_passed=all_passed,
        results=results,
    )
