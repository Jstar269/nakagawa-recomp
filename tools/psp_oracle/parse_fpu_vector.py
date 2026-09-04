# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Host-side parser for PSP-FPU-001 vector probe output."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Any

from .protocol import parse_output, ProtocolError, TestResult

EXPECTED_TEST_ID = "PSP-FPU-001"
EXPECTED_CELLS = [
    "fpu-boot-fcr31",
    "fpu-cvt-rm0",
    "fpu-cvt-rm1",
    "fpu-cvt-rm2",
    "fpu-cvt-rm3",
    "fpu-ccast-trunc",
    "fpu-cvt-s-w",
    "fpu-flag-overflow",
    "fpu-flag-div0",
    "fpu-flag-invalid",
    "fpu-flag-underflow",
    "fpu-flag-inexact",
    "fpu-ftz-contrast",
    "fpu-signed-zero",
    "fpu-nan-payload",
    "fpu-done",
]


@dataclass(frozen=True)
class FpuVectorReport:
    raw_record_count: int
    cell_count: int
    boot_fcr31: int
    trap_enables: int
    ftz_contrast_delta: int
    all_passed: bool
    results: dict[str, TestResult]


def parse_fpu_vector_output(text: str, *, require_complete: bool = True) -> FpuVectorReport:
    """Parse and validate a captured PSP-FPU-001 probe stream."""
    parsed = parse_output(text)
    fpu_ordered_results: list[TestResult] = []
    seen: set[str] = set()
    for r in parsed.results:
        if r.test_id == EXPECTED_TEST_ID:
            if r.case_id in seen:
                raise ProtocolError(f"duplicate fpu cell: {r.case_id}")
            seen.add(r.case_id)
            fpu_ordered_results.append(r)

    fpu_results = {r.case_id: r for r in fpu_ordered_results}

    if "fpu-boot-fcr31" not in fpu_results:
        raise ProtocolError("missing required fpu-boot-fcr31 diagnostic cell")

    actual_order = [r.case_id for r in fpu_ordered_results]
    if require_complete:
        if actual_order != EXPECTED_CELLS:
            missing = [cid for cid in EXPECTED_CELLS if cid not in fpu_results]
            if missing:
                raise ProtocolError(f"fpu stream incomplete, missing cells: {missing}")
            extra = [cid for cid in actual_order if cid not in EXPECTED_CELLS]
            if extra:
                raise ProtocolError(f"fpu stream contains unexpected cells: {extra}")
            raise ProtocolError(
                f"fpu cells out of order: expected {EXPECTED_CELLS}, got {actual_order}"
            )
    else:
        expected_indices = {cid: idx for idx, cid in enumerate(EXPECTED_CELLS)}
        indices = [expected_indices.get(cid, -1) for cid in actual_order if cid in expected_indices]
        if any(indices[i] > indices[i + 1] for i in range(len(indices) - 1)):
            raise ProtocolError(f"fpu cells out of order: {actual_order}")

    boot_rec = fpu_results["fpu-boot-fcr31"]
    val_dict = dict(boot_rec.values)
    boot_fcr = int(val_dict.get("result", "0"), 0)
    enables = (boot_fcr >> 7) & 0x1F

    ftz_delta = 0
    if "fpu-ftz-contrast" in fpu_results:
        ftz_rec = fpu_results["fpu-ftz-contrast"]
        ftz_dict = dict(ftz_rec.values)
        ftz_delta = int(ftz_dict.get("result", "0"), 0)

    all_passed = (
        (len(fpu_results) == len(EXPECTED_CELLS) if require_complete else True)
        and all(r.status == "PASS" for r in fpu_results.values())
        and ("fpu-done" in fpu_results)
    )

    return FpuVectorReport(
        raw_record_count=len(parsed.results),
        cell_count=len(fpu_results),
        boot_fcr31=boot_fcr,
        trap_enables=enables,
        ftz_contrast_delta=ftz_delta,
        all_passed=all_passed,
        results=fpu_results,
    )
