# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Host-side parsers for the PSP hardware golden streams.

Consolidated from the six per-probe modules ``parse_audio_query.py``,
``parse_cache_alias.py``, ``parse_ctrl_clock.py``, ``parse_fpu_vector.py``,
``parse_io_matrix.py`` and ``parse_phaseb.py``.  This is a relocation, not a
rewrite.  Every parser keeps its strict and non-strict semantics, its error
messages, its exception type, its field names, its field ordering and its
constants unchanged; each section below is the former module verbatim apart from
the per-probe constant prefixes described next.

The mailbox delete/wait parser is a new strict record contract for the
source-owned PSP kernel-object probe.  It validates stream completeness and
exact field sets without asserting any measured PSP return code.

Two constant names were defined by several of those modules with *different*
values, so each keeps its own value under a distinct per-probe name rather than
being merged: ``EXPECTED_TEST_ID`` and ``EXPECTED_TERMINAL_COUNT`` (audio, cache,
fpu, io, phaseb) and the audio/cache/io trio's ``SEMANTIC_CASES``,
``TERMINAL_CASE``, ``HOST0_LOG``, ``SPEC`` and ``EXPECTED_CASES``.  The only code
shared across probes here is the sequence-observable reader, which was
byte-identical in the three modules that used it.

A parser asserts the *stream*, never the PSP.  These inputs come from a
hardware-gated probe, so anything unparseable must stay a named failure rather
than a lenient pass.
"""

from __future__ import annotations

from dataclasses import dataclass

from .protocol import ProtocolError, SequenceReport, StreamSpec, TestResult, parse_output, parse_sequence


def _observable(report: SequenceReport, case: str, field: str) -> int | None:
    record = report.results.get(case)
    if record is None:
        return None
    value = dict(record.values).get(field)
    if value is None:
        return None
    return int(value, 0)


# ---------------------------------------------------------------------------
# PSP-AUDIO-001 audio reservation/query probe
#
# The record contract is derived from ``fixtures/psp_oracle/probe.c``
# ``run_audio_query`` at the frozen source commit: four self-contained
# reserve/release cells emitted in order by ``emit_record_extended``, then the
# ``audio-done`` completion sentinel whose ``out0`` is the literal ``4u`` the
# probe emits.  Nothing here is inherited from an earlier manifest.
#
# The audio probe emits per record rather than deferring, because no audio
# observable spans a cell boundary and ``host0:`` I/O cannot alter ``sceAudio``
# channel reservation state.  That is a property of this probe, not a licence to
# log inside any other probe's measured window.
# ---------------------------------------------------------------------------

AUDIO_EXPECTED_TEST_ID = "PSP-AUDIO-001"

AUDIO_SEMANTIC_CASES = (
    "audio-ch-reserve",
    "audio-ch-query-before",
    "audio-ch-output-blocking-0",
    "audio-ch-output-blocking-1",
    "audio-ch-query-after",
    "audio-ch-release",
    "audio-ch-query-released",
    "audio-out2-reserve",
    "audio-out2-query-before",
    "audio-out2-output-blocking",
    "audio-out2-query-after",
    "audio-out2-release",
    "audio-src-reserve",
)
AUDIO_TERMINAL_CASE = "audio-done"
AUDIO_HOST0_LOG = "host0:/audio_query_log.txt"

AUDIO_SPEC = StreamSpec(
    test_id=AUDIO_EXPECTED_TEST_ID,
    semantic_cases=AUDIO_SEMANTIC_CASES,
    terminal_case=AUDIO_TERMINAL_CASE,
)
AUDIO_EXPECTED_CASES = AUDIO_SPEC.ordered_cases
AUDIO_EXPECTED_TERMINAL_COUNT = AUDIO_SPEC.terminal_count
AUDIO_OUT_COUNTS = {
    "audio-ch-reserve": 3,
    "audio-ch-query-before": 2,
    "audio-ch-output-blocking-0": 7,
    "audio-ch-output-blocking-1": 7,
    "audio-ch-query-after": 2,
    "audio-ch-release": 1,
    "audio-ch-query-released": 2,
    "audio-out2-reserve": 2,
    "audio-out2-query-before": 1,
    "audio-out2-output-blocking": 4,
    "audio-out2-query-after": 1,
    "audio-out2-release": 1,
    "audio-src-reserve": 2,
    "audio-done": 1,
}


def _validate_scalar_shape(sequence: SequenceReport,
                           out_counts: dict[str, int]) -> None:
    for case_id, record in sequence.results.items():
        values = dict(record.values)
        expected = {"result", *(f"out{i}" for i in range(out_counts[case_id]))}
        if set(values) != expected:
            raise ProtocolError(
                f"{sequence.spec.test_id}: {case_id} fields must be "
                f"{sorted(expected)}, got {sorted(values)}"
            )
        for field, value in values.items():
            try:
                int(value, 0)
            except ValueError as exc:
                raise ProtocolError(
                    f"{sequence.spec.test_id}: {case_id} {field} is not an integer"
                ) from exc


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


def parse_audio_query_output(
    text: str, *, require_complete: bool = True
) -> AudioQueryReport:
    """Parse a captured PSP-AUDIO-001 stream from one channel.

    ``require_complete=True`` is complete-or-error.  ``require_complete=False``
    diagnoses an ordered prefix and can never report ``all_passed``.
    """

    sequence = parse_sequence(text, AUDIO_SPEC, require_complete=require_complete)
    _validate_scalar_shape(sequence, AUDIO_OUT_COUNTS)
    return AudioQueryReport(
        sequence=sequence,
        channel_reserve_rc=_observable(sequence, "audio-ch-reserve", "out0"),
        channel_rest_len=_observable(sequence, "audio-ch-query-before", "out0"),
    )


# ---------------------------------------------------------------------------
# PSP-GE-001 raw non-finite input probe
# ---------------------------------------------------------------------------

GE_NAN_WORDS = {
    "qnan": 0x7FC00000,
    "pinf": 0x7F800000,
    "ninf": 0xFF800000,
    "nzero": 0x80000000,
    "denorm": 0x00000001,
}
GE_NAN_MODES = ("screen2d", "clip3d", "litnormal")
GE_NAN_SEMANTIC_CASES = tuple(
    case_id
    for name in GE_NAN_WORDS
    for case_id in (
        f"ge-nan-vfpu-{name}",
        *(f"ge-nan-{mode}-{name}" for mode in GE_NAN_MODES),
    )
)
GE_NAN_SPEC = StreamSpec(
    test_id="PSP-GE-001",
    semantic_cases=GE_NAN_SEMANTIC_CASES,
    terminal_case="ge-nan-done",
)
GE_NAN_OUT_COUNTS = {
    **{f"ge-nan-vfpu-{name}": 3 for name in GE_NAN_WORDS},
    **{
        f"ge-nan-{mode}-{name}": 4
        for name in GE_NAN_WORDS
        for mode in GE_NAN_MODES
    },
    "ge-nan-done": 1,
}


def parse_ge_nan_output(text: str, *, require_complete: bool = True) -> SequenceReport:
    sequence = parse_sequence(text, GE_NAN_SPEC, require_complete=require_complete)
    _validate_scalar_shape(sequence, GE_NAN_OUT_COUNTS)
    for case_id, record in sequence.results.items():
        if case_id == GE_NAN_SPEC.terminal_case:
            continue
        sample = case_id.rsplit("-", 1)[1]
        actual = int(dict(record.values)["out0"], 0)
        if actual != GE_NAN_WORDS[sample]:
            raise ProtocolError(
                f"PSP-GE-001: {case_id} input bits {actual:#010x} do not match "
                f"the named raw input {GE_NAN_WORDS[sample]:#010x}"
            )
    return sequence


# ---------------------------------------------------------------------------
# PSP-DMAC-001 safe alignment/overlap cells and isolated invalid-tail launches
# ---------------------------------------------------------------------------

DMAC_CELL_OFFSETS = (1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15)
DMAC_OVERLAP_OFFSETS = (1, 2, 3, 7, 15)
DMAC_CELL_SEMANTIC_CASES = tuple(
    [
        f"align-{api}-{side}-{offset:02x}"
        for api in ("memcpy", "try")
        for offset in DMAC_CELL_OFFSETS
        for side in ("src", "dst", "both")
    ]
    + [
        f"overlap-{api}-{direction}-{delta:02x}"
        for api in ("memcpy", "try")
        for delta in DMAC_OVERLAP_OFFSETS
        for direction in ("forward", "backward")
    ]
)
DMAC_CELL_SPEC = StreamSpec(
    test_id="PSP-DMAC-001",
    semantic_cases=DMAC_CELL_SEMANTIC_CASES,
    terminal_case="dmac-cells-done",
)
DMAC_CELL_OUT_COUNTS = {
    **{case_id: 10 for case_id in DMAC_CELL_SEMANTIC_CASES},
    "dmac-cells-done": 1,
}
DMAC_INVALID_CASES = {
    "dma-invalid-tail-memcpy-dst": ("invalid-tail-memcpy-dst", 0, 0),
    "dma-invalid-tail-memcpy-src": ("invalid-tail-memcpy-src", 1, 0),
    "dma-invalid-tail-try-dst": ("invalid-tail-try-dst", 0, 1),
    "dma-invalid-tail-try-src": ("invalid-tail-try-src", 1, 1),
}


def parse_dmac_cells_output(text: str, *, require_complete: bool = True) -> SequenceReport:
    sequence = parse_sequence(text, DMAC_CELL_SPEC, require_complete=require_complete)
    _validate_scalar_shape(sequence, DMAC_CELL_OUT_COUNTS)
    for case_id, record in sequence.results.items():
        if case_id == DMAC_CELL_SPEC.terminal_case:
            continue
        values = {key: int(value, 0) for key, value in record.values}
        if case_id.startswith("align-"):
            _, api_name, side, offset_text = case_id.split("-")
            offset = int(offset_text, 16)
            expected_api = 0 if api_name == "memcpy" else 1
            src_offset = offset if side in {"src", "both"} else 0
            dst_offset = offset if side in {"dst", "both"} else 0
            wanted = {
                "out0": expected_api,
                "out1": src_offset,
                "out2": dst_offset,
                "out3": 64,
            }
            source_address, destination_address = values["out8"], values["out9"]
            if source_address % 16 != src_offset or destination_address % 16 != dst_offset:
                raise ProtocolError(f"PSP-DMAC-001: {case_id} address alignment is false")
            safe = values["out6"] == 0 and values["out7"] == 1
        else:
            _, api_name, direction, delta_text = case_id.split("-")
            delta = int(delta_text, 16)
            expected_api = 0 if api_name == "memcpy" else 1
            expected_direction = 0 if direction == "forward" else 1
            wanted = {
                "out0": expected_api,
                "out1": expected_direction,
                "out2": delta,
                "out3": 64,
            }
            source_address, destination_address = values["out8"], values["out9"]
            address_delta = (destination_address - source_address) & 0xFFFFFFFF
            wanted_delta = delta if expected_direction == 0 else (0x100000000 - delta)
            if address_delta != wanted_delta:
                raise ProtocolError(f"PSP-DMAC-001: {case_id} overlap direction is false")
            safe = values["out6"] == 0
        for field, expected in wanted.items():
            if values[field] != expected:
                raise ProtocolError(
                    f"PSP-DMAC-001: {case_id} {field}={values[field]:#x}, "
                    f"expected {expected:#x}"
                )
        if record.status == "PASS" and not safe:
            raise ProtocolError(f"PSP-DMAC-001: {case_id} PASS has a changed guard/source")
    return sequence


def parse_dmac_invalid_tail_output(text: str, campaign_case: str):
    expected = DMAC_INVALID_CASES.get(campaign_case)
    if expected is None:
        raise ProtocolError(f"unknown isolated DMAC invalid-tail case {campaign_case!r}")
    parsed = parse_output(text)
    if len(parsed.results) != 1:
        raise ProtocolError(f"{campaign_case}: stream must contain exactly one result record")
    record = parsed.results[0]
    expected_id, direction, api = expected
    if (record.test_id, record.case_id, record.status) != (
        "PSP-DMAC-001", expected_id, "SKIP"
    ):
        raise ProtocolError(
            f"{campaign_case}: expected one safe-boundary SKIP for {expected_id}"
        )
    values = {key: int(value, 0) for key, value in record.values}
    expected_fields = {"result", *(f"out{i}" for i in range(7))}
    if set(values) != expected_fields:
        raise ProtocolError(f"{campaign_case}: result field set is incomplete or unexpected")
    wanted = {"out1": 0xC001, "out2": 0xC000, "out3": direction, "out4": api,
              "out6": 0x10000}
    for field, value in wanted.items():
        if values[field] != value:
            raise ProtocolError(f"{campaign_case}: {field} is not the documented safe shape")
    return parsed


# ---------------------------------------------------------------------------
# PSP-KERNEL-002 zero-duration delay/yield/callback probe
# ---------------------------------------------------------------------------

DELAY_ZERO_SEMANTIC_CASES = ("delay-threadcb-zero", "delay-thread-zero")
DELAY_ZERO_SPEC = StreamSpec(
    test_id="PSP-KERNEL-002",
    semantic_cases=DELAY_ZERO_SEMANTIC_CASES,
    terminal_case="delay-zero-done",
)
DELAY_ZERO_OUT_COUNTS = {case_id: 10 for case_id in DELAY_ZERO_SEMANTIC_CASES}
DELAY_ZERO_OUT_COUNTS["delay-zero-done"] = 1


def parse_delay_zero_output(text: str, *, require_complete: bool = True) -> SequenceReport:
    sequence = parse_sequence(text, DELAY_ZERO_SPEC, require_complete=require_complete)
    _validate_scalar_shape(sequence, DELAY_ZERO_OUT_COUNTS)
    for case_id, record in sequence.results.items():
        if case_id == DELAY_ZERO_SPEC.terminal_case or record.status != "PASS":
            continue
        values = {key: int(value, 0) for key, value in record.values}
        if values["out0"] != 0 or values["out2"] != 0 or \
                values["out3"] != 0 or values["out6"] != values["out7"] or \
                values["out8"] != 2:
            raise ProtocolError(
                f"PSP-KERNEL-002: {case_id} PASS did not start with an equal-priority "
                "ready thread and pending callback"
            )
    return sequence


# ---------------------------------------------------------------------------
# PSP-CACHE-001 dcache/uncached-alias probe
#
# The record contract is derived from ``fixtures/psp_oracle/probe.c``
# ``run_cache_alias`` at the frozen source commit: four ``defer_record`` calls in
# emission order followed by the ``cache-done`` completion sentinel, all flushed
# by ``flush_deferred(emulated, "PSP-CACHE-001")`` after the last cache-sensitive
# cell.  Nothing here is inherited from an earlier manifest.
#
# ``cache-inval-contrast`` records ``c_stale`` in ``out0`` -- a value that is only
# meaningful while the line written by ``cache-writeback-contrast`` is still
# resident.  It is an observable, not an assertion, and it is only trustworthy
# because the deferred-record buffer keeps every ``host0:`` round trip out of the
# window between those two cells.
# ---------------------------------------------------------------------------

CACHE_EXPECTED_TEST_ID = "PSP-CACHE-001"

CACHE_SEMANTIC_CASES = (
    "cache-alias-init",
    "cache-writeback-contrast",
    "cache-inval-contrast",
    "cache-wball",
)
CACHE_TERMINAL_CASE = "cache-done"
CACHE_HOST0_LOG = "host0:/cache_alias_log.txt"

CACHE_SPEC = StreamSpec(
    test_id=CACHE_EXPECTED_TEST_ID,
    semantic_cases=CACHE_SEMANTIC_CASES,
    terminal_case=CACHE_TERMINAL_CASE,
)
CACHE_EXPECTED_CASES = CACHE_SPEC.ordered_cases
CACHE_EXPECTED_TERMINAL_COUNT = CACHE_SPEC.terminal_count


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


def parse_cache_alias_output(
    text: str, *, require_complete: bool = True
) -> CacheAliasReport:
    """Parse a captured PSP-CACHE-001 stream from one channel.

    ``require_complete=True`` is complete-or-error.  ``require_complete=False``
    diagnoses an ordered prefix and can never report ``all_passed``.
    """

    sequence = parse_sequence(text, CACHE_SPEC, require_complete=require_complete)
    return CacheAliasReport(
        sequence=sequence,
        stale_cached_read=_observable(sequence, "cache-inval-contrast", "out0"),
        fresh_cached_read=_observable(sequence, "cache-inval-contrast", "out1"),
    )


# ---------------------------------------------------------------------------
# PSP-IO-001 IoFileMgr matrix probe
#
# The record contract is derived from ``fixtures/psp_oracle/probe.c``
# ``run_io_matrix`` at the frozen source commit: six ``defer_record`` calls in
# emission order followed by the ``io-done`` completion sentinel, all flushed by
# ``flush_deferred(emulated, "PSP-IO-001")`` after the last measured IoFileMgr
# operation.  Nothing here is inherited from an earlier manifest.
#
# Every measured value is a raw ``SceUID`` / return code recorded as an
# observable.  This parser asserts the *stream*, not the PSP: a complete strict
# stream establishes that the probe ran to completion and reported its own
# verdicts, which is the precondition for reading those observables at all.
# ---------------------------------------------------------------------------

IO_EXPECTED_TEST_ID = "PSP-IO-001"

# ``run_io_matrix`` cell order.  Cells 2 and 4 are nested under a successful
# open, so a probe that cannot create its scratch file emits fewer records --
# which is precisely why a short stream must never be read as a pass.
IO_SEMANTIC_CASES = (
    "io-open-create",
    "io-write",
    "io-read-verify",
    "io-lseek",
    "io-append",
    "io-errors",
)
IO_TERMINAL_CASE = "io-done"
IO_HOST0_LOG = "host0:/io_matrix_log.txt"

IO_SPEC = StreamSpec(
    test_id=IO_EXPECTED_TEST_ID,
    semantic_cases=IO_SEMANTIC_CASES,
    terminal_case=IO_TERMINAL_CASE,
)
IO_EXPECTED_CASES = IO_SPEC.ordered_cases
IO_EXPECTED_TERMINAL_COUNT = IO_SPEC.terminal_count


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


def parse_io_matrix_output(text: str, *, require_complete: bool = True) -> IoMatrixReport:
    """Parse a captured PSP-IO-001 stream from one channel.

    ``require_complete=True`` is complete-or-error.  ``require_complete=False``
    diagnoses an ordered prefix and can never report ``all_passed``; see
    :func:`tools.psp_oracle.protocol.parse_sequence` for the exact boundary.
    """

    sequence = parse_sequence(text, IO_SPEC, require_complete=require_complete)
    return IoMatrixReport(
        sequence=sequence,
        # out0 of io-open-create is the raw SceUID the probe was handed.  It is
        # only an IoFileMgr observable because no log descriptor is allocated
        # inside the measured window; see the deferred-record buffer in probe.c.
        open_fd=_observable(sequence, "io-open-create", "out0"),
        removed_rc=_observable(sequence, "io-errors", "out2"),
    )


# ---------------------------------------------------------------------------
# PSP-KERNEL-001 mailbox delete/wait measurement
#
# The two ordered semantic rows record raw values from the isolated
# ``mbx-delete-wait`` fixture case.  The parser checks only that the complete
# measurement stream and its exact output fields arrived; none of the observed
# return codes, pointer values, wait states, or timing values are PSP
# expectations.
# ---------------------------------------------------------------------------

MBX_DELETE_WAIT_SPEC = StreamSpec(
    test_id="PSP-KERNEL-001",
    semantic_cases=("mbx-delete-wait", "mbx-timeout-control"),
    terminal_case="mbx-done",
)
MBX_DELETE_WAIT_EXPECTED_FIELDS = {
    "mbx-delete-wait": frozenset(
        {"result", *(f"out{i}" for i in range(26))}
    ),
    "mbx-timeout-control": frozenset(
        {"result", *(f"out{i}" for i in range(8))}
    ),
    "mbx-done": frozenset({"result", "out0"}),
}


@dataclass(frozen=True)
class MbxDeleteWaitReport:
    sequence: SequenceReport

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


def parse_mbx_delete_wait_output(
    text: str, *, require_complete: bool = True
) -> MbxDeleteWaitReport:
    """Parse one exact mailbox delete/wait observation stream.

    This parser validates stream completeness and exact record fields.  It does
    not qualify transport or PSP semantics; raw API outcomes remain
    observations for later PSP-versus-HLE review, not parser constants.
    """

    sequence = parse_sequence(
        text, MBX_DELETE_WAIT_SPEC, require_complete=require_complete
    )
    for case_id, expected_fields in MBX_DELETE_WAIT_EXPECTED_FIELDS.items():
        record = sequence.results.get(case_id)
        if record is None:
            continue
        actual_fields = set(dict(record.values))
        if actual_fields != expected_fields:
            missing = sorted(expected_fields - actual_fields)
            unexpected = sorted(actual_fields - expected_fields)
            details = []
            if missing:
                details.append(f"missing {', '.join(missing)}")
            if unexpected:
                details.append(f"unexpected {', '.join(unexpected)}")
            raise ProtocolError(
                f"{MBX_DELETE_WAIT_SPEC.test_id}: {case_id} field set mismatch: "
                + "; ".join(details)
            )
    return MbxDeleteWaitReport(sequence=sequence)


# ---------------------------------------------------------------------------
# PSP-CTRL-001 controller and clock probe (also PSP-SYSTEM-001)
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# PSP-FPU-001 vector probe
# ---------------------------------------------------------------------------

FPU_EXPECTED_TEST_ID = "PSP-FPU-001"
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

# ``fpu-done`` is the terminal completion sentinel, not a measurement.  It is
# never counted as a semantic cell, and it carries the number of semantic cells
# the probe believes it executed in ``out0``.  The probe emits cells 0..14 and
# then reports 15, so the sentinel is cross-checkable against the protocol.
TERMINAL_CELL = "fpu-done"
SEMANTIC_CELLS = [cid for cid in EXPECTED_CELLS if cid != TERMINAL_CELL]
FPU_EXPECTED_TERMINAL_COUNT = len(SEMANTIC_CELLS)


@dataclass(frozen=True)
class FpuVectorReport:
    raw_record_count: int
    cell_count: int
    boot_fcr31: int
    trap_enables: int
    ftz_contrast_delta: int
    complete: bool
    terminal_present: bool
    all_passed: bool
    results: dict[str, TestResult]


def parse_fpu_vector_output(text: str, *, require_complete: bool = True) -> FpuVectorReport:
    """Parse and validate a captured PSP-FPU-001 probe stream.

    ``require_complete=True`` is complete-or-error: an incomplete, reordered or
    over-long cell sequence raises :class:`ProtocolError`.

    ``require_complete=False`` permits *inspection* of an ordered prefix so a
    truncated capture can still be classified.  It does not relax any verdict:
    ``complete`` is computed from the cell sequence alone and ``all_passed``
    is gated on it in both modes, so a partial stream can never report
    ``all_passed``.  Presence of the ``fpu-done`` terminal record is reported
    separately as ``terminal_present`` and never substitutes for completeness --
    a probe that emits its terminal record after losing middle records is
    exactly the shape the transport defect produces.
    """
    parsed = parse_output(text)
    fpu_ordered_results: list[TestResult] = []
    seen: set[str] = set()
    known = set(EXPECTED_CELLS)
    for r in parsed.results:
        # A foreign test_id or an unknown cell is never a truncation, so both
        # fail closed in *both* strictness modes.  Filtering them out instead
        # would let a complete FPU run plus an appended record from another
        # probe's run parse as an unqualified pass.
        if r.test_id != FPU_EXPECTED_TEST_ID:
            raise ProtocolError(
                f"fpu stream carries a foreign test_id {r.test_id!r} (case {r.case_id!r})"
            )
        if r.case_id not in known:
            raise ProtocolError(f"fpu stream contains unknown cell: {r.case_id}")
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
            # Unknown cells are rejected above, in both strictness modes, so a
            # complete-set mismatch that is not a missing cell can only be an
            # ordering fault.
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

    # Completeness is a property of the observed cell sequence, never of the
    # caller's strictness flag.  ``require_complete`` only decides whether an
    # incomplete stream raises or is returned for inspection.
    complete = actual_order == EXPECTED_CELLS
    terminal_present = TERMINAL_CELL in fpu_results

    # A terminal record that disagrees with the protocol's semantic cell count
    # is a corrupted or mismatched stream, not a truncation, so it fails closed
    # in both strictness modes.  Without this check a sentinel claiming any
    # count at all would be accepted, and the completion record would carry no
    # verifiable meaning.
    if terminal_present:
        done_values = dict(fpu_results[TERMINAL_CELL].values)
        if "out0" not in done_values:
            raise ProtocolError(
                f"fpu terminal record {TERMINAL_CELL!r} is missing its out0 completion count"
            )
        try:
            claimed_count = int(done_values["out0"], 0)
        except ValueError as exc:
            raise ProtocolError(
                f"fpu terminal count is not an integer: {done_values['out0']!r}"
            ) from exc
        if claimed_count != FPU_EXPECTED_TERMINAL_COUNT:
            raise ProtocolError(
                f"fpu terminal count mismatch: {TERMINAL_CELL} claims {claimed_count} "
                f"semantic cells, protocol expects {FPU_EXPECTED_TERMINAL_COUNT}"
            )
        observed_semantic = len([cid for cid in actual_order if cid != TERMINAL_CELL])
        if complete and observed_semantic != claimed_count:
            raise ProtocolError(
                f"fpu terminal count mismatch: {TERMINAL_CELL} claims {claimed_count} "
                f"semantic cells, stream carries {observed_semantic}"
            )

    all_passed = complete and all(r.status == "PASS" for r in fpu_results.values())

    return FpuVectorReport(
        raw_record_count=len(parsed.results),
        cell_count=len(fpu_results),
        boot_fcr31=boot_fcr,
        trap_enables=enables,
        ftz_contrast_delta=ftz_delta,
        complete=complete,
        terminal_present=terminal_present,
        all_passed=all_passed,
        results=fpu_results,
    )


# ---------------------------------------------------------------------------
# PSP-PHASEB-001 resident money-launch probe
# ---------------------------------------------------------------------------

PHASEB_EXPECTED_TEST_ID = "PSP-PHASEB-001"

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

# ``PHASEB-COMPLETE`` is the terminal completion sentinel, never a semantic
# measurement.  It carries the number of semantic cases the probe believes it
# executed in ``out0`` (41 = 6 C + 4 D + 19 B + 12 A).
PHASEB_EXPECTED_TERMINAL_COUNT = len(EXPECTED_ALL_CASES)


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
    known = set(EXPECTED_ORDERED_CASES)
    for r in parsed.results:
        # A foreign test_id or an unknown case is never a truncation, so both
        # fail closed in *both* strictness modes.  Filtering them out instead
        # would let a complete Phase-B run plus an appended record from another
        # run parse as an unqualified pass.
        if r.test_id != PHASEB_EXPECTED_TEST_ID:
            raise ProtocolError(
                f"phaseb stream carries a foreign test_id {r.test_id!r} (case {r.case_id!r})"
            )
        if r.case_id not in known:
            raise ProtocolError(f"phaseb stream contains unknown case: {r.case_id}")
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
            # Unknown cases are rejected above, in both strictness modes, so a
            # complete-set mismatch that is not a missing case can only be an
            # ordering fault.
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

    # A summary record disagreeing with the protocol's semantic case count is a
    # corrupted or mismatched stream, not a truncation, so it fails closed in
    # both strictness modes.
    if summary_present:
        summary_values = dict(results[SUMMARY_CASE].values)
        if "out0" not in summary_values:
            raise ProtocolError(
                f"phaseb terminal record {SUMMARY_CASE!r} is missing its out0 completion count"
            )
        try:
            claimed_count = int(summary_values["out0"], 0)
        except ValueError as exc:
            raise ProtocolError(
                f"phaseb terminal count is not an integer: {summary_values['out0']!r}"
            ) from exc
        if claimed_count != PHASEB_EXPECTED_TERMINAL_COUNT:
            raise ProtocolError(
                f"phaseb terminal count mismatch: {SUMMARY_CASE} claims {claimed_count} "
                f"semantic cases, protocol expects {PHASEB_EXPECTED_TERMINAL_COUNT}"
            )
        observed_semantic = len([cid for cid in actual_order if cid != SUMMARY_CASE])
        if actual_order == EXPECTED_ORDERED_CASES and observed_semantic != claimed_count:
            raise ProtocolError(
                f"phaseb terminal count mismatch: {SUMMARY_CASE} claims {claimed_count} "
                f"semantic cases, stream carries {observed_semantic}"
            )

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
