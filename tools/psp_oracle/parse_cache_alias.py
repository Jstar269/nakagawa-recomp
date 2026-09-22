# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Host-side parser for PSP-CACHE-001 dcache/uncached-alias probe output.

The record contract is derived from ``fixtures/psp_oracle/probe.c``
``run_cache_alias`` at the frozen source commit: four ``defer_record`` calls in
emission order followed by the ``cache-done`` completion sentinel, all flushed
by ``flush_deferred(emulated, "PSP-CACHE-001")`` after the last cache-sensitive
cell.  Nothing here is inherited from an earlier manifest.

``cache-inval-contrast`` records ``c_stale`` in ``out0`` -- a value that is only
meaningful while the line written by ``cache-writeback-contrast`` is still
resident.  It is an observable, not an assertion, and it is only trustworthy
because the deferred-record buffer keeps every ``host0:`` round trip out of the
window between those two cells.
"""

from __future__ import annotations

from dataclasses import dataclass

from .protocol import SequenceReport, StreamSpec, parse_sequence

EXPECTED_TEST_ID = "PSP-CACHE-001"

SEMANTIC_CASES = (
    "cache-alias-init",
    "cache-writeback-contrast",
    "cache-inval-contrast",
    "cache-wball",
)
TERMINAL_CASE = "cache-done"
HOST0_LOG = "host0:/cache_alias_log.txt"

SPEC = StreamSpec(
    test_id=EXPECTED_TEST_ID,
    semantic_cases=SEMANTIC_CASES,
    terminal_case=TERMINAL_CASE,
)
EXPECTED_CASES = SPEC.ordered_cases
EXPECTED_TERMINAL_COUNT = SPEC.terminal_count


@dataclass(frozen=True)
class CacheAliasReport:
    sequence: SequenceReport
    stale_cached_read: int | None
    fresh_cached_read: int | None

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


def parse_cache_alias_output(
    text: str, *, require_complete: bool = True
) -> CacheAliasReport:
    """Parse a captured PSP-CACHE-001 stream from one channel.

    ``require_complete=True`` is complete-or-error.  ``require_complete=False``
    diagnoses an ordered prefix and can never report ``all_passed``.
    """

    sequence = parse_sequence(text, SPEC, require_complete=require_complete)
    return CacheAliasReport(
        sequence=sequence,
        stale_cached_read=_observable(sequence, "cache-inval-contrast", "out0"),
        fresh_cached_read=_observable(sequence, "cache-inval-contrast", "out1"),
    )
