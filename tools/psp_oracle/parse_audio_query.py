# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Host-side parser for PSP-AUDIO-001 audio reservation/query probe output.

The record contract is derived from ``fixtures/psp_oracle/probe.c``
``run_audio_query`` at the frozen source commit: four self-contained
reserve/release cells emitted in order by ``emit_record_extended``, then the
``audio-done`` completion sentinel whose ``out0`` is the literal ``4u`` the
probe emits.  Nothing here is inherited from an earlier manifest.

The audio probe emits per record rather than deferring, because no audio
observable spans a cell boundary and ``host0:`` I/O cannot alter ``sceAudio``
channel reservation state.  That is a property of this probe, not a licence to
log inside any other probe's measured window.
"""

from __future__ import annotations

from dataclasses import dataclass

from .protocol import SequenceReport, StreamSpec, parse_sequence

EXPECTED_TEST_ID = "PSP-AUDIO-001"

SEMANTIC_CASES = (
    "audio-ch-reserve",
    "audio-ch-release",
    "audio-out2-query",
    "audio-src-reserve",
)
TERMINAL_CASE = "audio-done"
HOST0_LOG = "host0:/audio_query_log.txt"

SPEC = StreamSpec(
    test_id=EXPECTED_TEST_ID,
    semantic_cases=SEMANTIC_CASES,
    terminal_case=TERMINAL_CASE,
)
EXPECTED_CASES = SPEC.ordered_cases
EXPECTED_TERMINAL_COUNT = SPEC.terminal_count


@dataclass(frozen=True)
class AudioQueryReport:
    sequence: SequenceReport
    channel_reserve_rc: int | None
    channel_rest_len: int | None

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


def parse_audio_query_output(
    text: str, *, require_complete: bool = True
) -> AudioQueryReport:
    """Parse a captured PSP-AUDIO-001 stream from one channel.

    ``require_complete=True`` is complete-or-error.  ``require_complete=False``
    diagnoses an ordered prefix and can never report ``all_passed``.
    """

    sequence = parse_sequence(text, SPEC, require_complete=require_complete)
    return AudioQueryReport(
        sequence=sequence,
        channel_reserve_rc=_observable(sequence, "audio-ch-reserve", "out0"),
        channel_rest_len=_observable(sequence, "audio-ch-reserve", "out1"),
    )
