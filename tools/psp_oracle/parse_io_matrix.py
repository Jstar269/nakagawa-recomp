# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Host-side parser for PSP-IO-001 IoFileMgr matrix probe output.

The record contract is derived from ``fixtures/psp_oracle/probe.c``
``run_io_matrix`` at the frozen source commit: six ``defer_record`` calls in
emission order followed by the ``io-done`` completion sentinel, all flushed by
``flush_deferred(emulated, "PSP-IO-001")`` after the last measured IoFileMgr
operation.  Nothing here is inherited from an earlier manifest.

Every measured value is a raw ``SceUID`` / return code recorded as an
observable.  This parser asserts the *stream*, not the PSP: a complete strict
stream establishes that the probe ran to completion and reported its own
verdicts, which is the precondition for reading those observables at all.
"""

from __future__ import annotations

from dataclasses import dataclass

from .protocol import SequenceReport, StreamSpec, parse_sequence

EXPECTED_TEST_ID = "PSP-IO-001"

# ``run_io_matrix`` cell order.  Cells 2 and 4 are nested under a successful
# open, so a probe that cannot create its scratch file emits fewer records --
# which is precisely why a short stream must never be read as a pass.
SEMANTIC_CASES = (
    "io-open-create",
    "io-write",
    "io-read-verify",
    "io-lseek",
    "io-append",
    "io-errors",
)
TERMINAL_CASE = "io-done"
HOST0_LOG = "host0:/io_matrix_log.txt"

SPEC = StreamSpec(
    test_id=EXPECTED_TEST_ID,
    semantic_cases=SEMANTIC_CASES,
    terminal_case=TERMINAL_CASE,
)
EXPECTED_CASES = SPEC.ordered_cases
EXPECTED_TERMINAL_COUNT = SPEC.terminal_count


@dataclass(frozen=True)
class IoMatrixReport:
    sequence: SequenceReport
    open_fd: int | None
    removed_rc: int | None

    @property
    def complete(self) -> bool:
        return self.sequence.complete

    @property
    def terminal_present(self) -> bool:
        return self.sequence.terminal_present

    @property
    def all_passed(self) -> bool:
        return self.sequence.all_passed

    @property
    def record_count(self) -> int:
        return self.sequence.record_count

    @property
    def results(self):
        return self.sequence.results


def _observable(report: SequenceReport, case: str, field: str) -> int | None:
    record = report.results.get(case)
    if record is None:
        return None
    value = dict(record.values).get(field)
    if value is None:
        return None
    return int(value, 0)


def parse_io_matrix_output(text: str, *, require_complete: bool = True) -> IoMatrixReport:
    """Parse a captured PSP-IO-001 stream from one channel.

    ``require_complete=True`` is complete-or-error.  ``require_complete=False``
    diagnoses an ordered prefix and can never report ``all_passed``; see
    :func:`tools.psp_oracle.protocol.parse_sequence` for the exact boundary.
    """

    sequence = parse_sequence(text, SPEC, require_complete=require_complete)
    return IoMatrixReport(
        sequence=sequence,
        # out0 of io-open-create is the raw SceUID the probe was handed.  It is
        # only an IoFileMgr observable because no log descriptor is allocated
        # inside the measured window; see the deferred-record buffer in probe.c.
        open_fd=_observable(sequence, "io-open-create", "out0"),
        removed_rc=_observable(sequence, "io-errors", "out2"),
    )
