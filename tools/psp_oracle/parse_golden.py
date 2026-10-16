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
import re

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
# PSP-VRAM-MIRROR-001 raw EDRAM mirror and depth-buffer observations
# ---------------------------------------------------------------------------

VRAM_MIRROR_BASES = (
    0x04000000, 0x04200000, 0x04400000, 0x04600000,
    0x44000000, 0x44200000, 0x44400000, 0x44600000,
)
VRAM_MIRROR_WRITE_BASES = (
    0x04200000, 0x04400000, 0x04600000,
    0x44200000, 0x44400000, 0x44600000,
)
VRAM_MIRROR_CACHED_ALIAS_COUNT = len(VRAM_MIRROR_BASES) // 2
VRAM_MIRROR_BYTES = 0x00200000
VRAM_MIRROR_ROW_BYTES = 0x400
VRAM_MIRROR_ROW_WORDS = VRAM_MIRROR_ROW_BYTES // 4
VRAM_MIRROR_DENSE_OFFSET = 0x00088000
VRAM_MIRROR_DENSE_ROWS = 8
VRAM_MIRROR_PAIR_BASES = (0x00000000, 0x00044000, 0x00100000, 0x001FF800)
VRAM_MIRROR_PAIR_ROWS = 2
VRAM_MIRROR_SWEEP_FIRST_BIT = 2
VRAM_MIRROR_SWEEP_LAST_BIT = 20
VRAM_MIRROR_SAMPLE_CHUNK_WORDS = 8
VRAM_MIRROR_CPU_SAMPLE_WORD_COUNT = (
    VRAM_MIRROR_DENSE_ROWS * VRAM_MIRROR_ROW_WORDS
    + len(VRAM_MIRROR_PAIR_BASES) * VRAM_MIRROR_PAIR_ROWS * VRAM_MIRROR_ROW_WORDS
    + 2 * (VRAM_MIRROR_SWEEP_LAST_BIT - VRAM_MIRROR_SWEEP_FIRST_BIT + 1)
)
VRAM_MIRROR_CPU_SAMPLE_CHUNK_COUNT = (
    (VRAM_MIRROR_CPU_SAMPLE_WORD_COUNT + VRAM_MIRROR_SAMPLE_CHUNK_WORDS - 1)
    // VRAM_MIRROR_SAMPLE_CHUNK_WORDS
)
VRAM_MIRROR_WRITE_OFFSETS = (0x000100, 0x044100, 0x088100)
VRAM_MIRROR_MARKER_PREFIX = 0xB6000000
VRAM_MIRROR_PATTERN_PREFIX = 0xA5000000
VRAM_MIRROR_FRAME_BYTES = 512 * 272 * 2
VRAM_MIRROR_DEPTH_STRIDE = 512
VRAM_MIRROR_DEPTH_ROWS = (0, 7, 8, 9, 15, 16, 100, 101, 108, 109)
VRAM_MIRROR_DEPTH_ROW_COUNT = len(VRAM_MIRROR_DEPTH_ROWS)
VRAM_MIRROR_DEPTH_ALIASES = (VRAM_MIRROR_BASES[4], VRAM_MIRROR_BASES[5])
VRAM_MIRROR_DEPTH_CHUNK_PIXELS = 32
VRAM_MIRROR_DEPTH_ROW_CHUNK_COUNT = (
    VRAM_MIRROR_DEPTH_STRIDE // VRAM_MIRROR_DEPTH_CHUNK_PIXELS
)
VRAM_MIRROR_COLOR_ROWS = (0, 8, 100)
VRAM_MIRROR_COLOR_CHUNK_PIXELS = 64
VRAM_MIRROR_COLOR_ROW_CHUNK_COUNT = (
    VRAM_MIRROR_DEPTH_STRIDE // VRAM_MIRROR_COLOR_CHUNK_PIXELS
)
VRAM_MIRROR_COLOR_ALIAS = VRAM_MIRROR_BASES[4]
VRAM_MIRROR_CLEAR_DEPTH = 0x5A5A
VRAM_MIRROR_DEPTH_FUNCTION_ALWAYS = 1
VRAM_MIRROR_DEPTH_MASK_WRITE_ENABLE = 0
VRAM_MIRROR_DEPTH_RANGE_NEAR = 0xFFFF
VRAM_MIRROR_DEPTH_RANGE_FAR = 0
VRAM_MIRROR_GU_SPRITES = 6
VRAM_MIRROR_GU_VERTEX_TYPE = (7 << 2) | (2 << 7) | (1 << 23)
VRAM_MIRROR_GE_SYNC_CASES = (
    ("vram-ge-clear-sync", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
    ("vram-ge-sprite-a-sync", 1, 0, 0, 0x1111, 32, 16, 0x1111,
     0xFFFF0000, 2, VRAM_MIRROR_GU_SPRITES, VRAM_MIRROR_GU_VERTEX_TYPE),
    ("vram-ge-sprite-b-sync", 2, 64, 8, 0x2222, 96, 12, 0x2222,
     0xFF00FF00, 2, VRAM_MIRROR_GU_SPRITES, VRAM_MIRROR_GU_VERTEX_TYPE),
    ("vram-ge-sprite-c-sync", 3, 200, 100, 0x3333, 208, 101, 0x3333,
     0xFF0000FF, 2, VRAM_MIRROR_GU_SPRITES, VRAM_MIRROR_GU_VERTEX_TYPE),
)
VRAM_MIRROR_GE_SYNC_OUT_COUNT = 27
VRAM_MIRROR_INFO_FIELDS = (
    "cached_base", "uncached_base", "display_offset", "display_bytes",
    "display_stride", "display_format", "color_offset", "color_frame_bytes",
    "depth_offset", "depth_frame_bytes", "vram_bytes", "cpu_sample_word_count",
    "cpu_dense_offset", "cpu_dense_rows", "cpu_row_bytes", "cpu_pair_region_count",
    "cpu_pair_rows", "cpu_sweep_first_bit", "cpu_sweep_last_bit",
    "write_alias_count", "write_sample_count", "write_offset_0", "write_offset_1",
    "write_offset_2", "write_scan_bytes", "depth_stride", "depth_row_count",
    *(f"depth_row_y{row}" for row in range(VRAM_MIRROR_DEPTH_ROW_COUNT)),
    "depth_alias_count", "depth_chunk_pixels", "color_row_count",
    *(f"color_row_y{row}" for row in range(len(VRAM_MIRROR_COLOR_ROWS))),
    "color_chunk_pixels",
    "depth_test_enabled", "depth_function", "depth_mask", "depth_range_near",
    "depth_range_far", "clear_depth", "pattern_prefix", "marker_prefix",
    "cached_alias_count",
)
VRAM_MIRROR_INFO_OUT_COUNT = len(VRAM_MIRROR_INFO_FIELDS)
VRAM_MIRROR_SEMANTIC_CASES = (
    ("vram-mirror-info",)
    + tuple(
        f"vram-cpu-pattern-{chunk:03d}"
        for chunk in range(VRAM_MIRROR_CPU_SAMPLE_CHUNK_COUNT)
    )
    + tuple(
        f"vram-cpu-write-mirror-{alias:02d}-{sample:02d}"
        for alias in range(len(VRAM_MIRROR_WRITE_BASES))
        for sample in range(len(VRAM_MIRROR_WRITE_OFFSETS))
    )
    + tuple(
        sync_case[0] for sync_case in VRAM_MIRROR_GE_SYNC_CASES
    )
    + tuple(
        f"vram-ge-depth-row{row}-{chunk:02d}"
        for row in range(len(VRAM_MIRROR_DEPTH_ROWS))
        for chunk in range(VRAM_MIRROR_DEPTH_ROW_CHUNK_COUNT)
    )
    + tuple(
        f"vram-ge-color-row{row}-{chunk:02d}"
        for row in range(len(VRAM_MIRROR_COLOR_ROWS))
        for chunk in range(VRAM_MIRROR_COLOR_ROW_CHUNK_COUNT)
    )
    + ("vram-restore",)
)
VRAM_MIRROR_SPEC = StreamSpec(
    test_id="PSP-VRAM-MIRROR-001",
    semantic_cases=VRAM_MIRROR_SEMANTIC_CASES,
    terminal_case="vram-mirror-done",
)
VRAM_MIRROR_OUT_COUNTS = {
    "vram-mirror-info": VRAM_MIRROR_INFO_OUT_COUNT,
    **{
        f"vram-cpu-pattern-{chunk:03d}":
            min(
                VRAM_MIRROR_SAMPLE_CHUNK_WORDS,
                VRAM_MIRROR_CPU_SAMPLE_WORD_COUNT
                - chunk * VRAM_MIRROR_SAMPLE_CHUNK_WORDS,
            ) * len(VRAM_MIRROR_BASES) * 2
        for chunk in range(VRAM_MIRROR_CPU_SAMPLE_CHUNK_COUNT)
    },
    **{
        f"vram-cpu-write-mirror-{alias:02d}-{sample:02d}": 4
        for alias in range(len(VRAM_MIRROR_WRITE_BASES))
        for sample in range(len(VRAM_MIRROR_WRITE_OFFSETS))
    },
    **{
        sync_case[0]: VRAM_MIRROR_GE_SYNC_OUT_COUNT
        for sync_case in VRAM_MIRROR_GE_SYNC_CASES
    },
    **{
        f"vram-ge-depth-row{row}-{chunk:02d}":
            VRAM_MIRROR_DEPTH_CHUNK_PIXELS * len(VRAM_MIRROR_DEPTH_ALIASES) * 2
        for row in range(len(VRAM_MIRROR_DEPTH_ROWS))
        for chunk in range(VRAM_MIRROR_DEPTH_ROW_CHUNK_COUNT)
    },
    **{
        f"vram-ge-color-row{row}-{chunk:02d}":
            VRAM_MIRROR_COLOR_CHUNK_PIXELS * 2
        for row in range(len(VRAM_MIRROR_COLOR_ROWS))
        for chunk in range(VRAM_MIRROR_COLOR_ROW_CHUNK_COUNT)
    },
    "vram-restore": 5,
    "vram-mirror-done": 1,
}


def _vram_mirror_integer(record: TestResult, field: str) -> int:
    try:
        value = int(dict(record.values)[field], 0)
    except (KeyError, ValueError) as exc:
        raise ProtocolError(
            f"PSP-VRAM-MIRROR-001: {record.case_id} {field} is not an integer"
        ) from exc
    if not 0 <= value <= 0xFFFFFFFF:
        raise ProtocolError(
            f"PSP-VRAM-MIRROR-001: {record.case_id} {field} is outside a raw word"
        )
    return value


def _validate_vram_mirror_shapes(sequence: SequenceReport) -> None:
    for case_id, record in sequence.results.items():
        values = dict(record.values)
        if record.status == "SKIP":
            expected = {"result"}
            if case_id == VRAM_MIRROR_SPEC.terminal_case:
                expected.add("out0")
        else:
            expected = {
                "result",
                *(f"out{index}" for index in range(VRAM_MIRROR_OUT_COUNTS[case_id])),
            }
        if set(values) != expected:
            raise ProtocolError(
                f"PSP-VRAM-MIRROR-001: {case_id} fields must be "
                f"{sorted(expected)}, got {sorted(values)}"
            )
        for field in values:
            _vram_mirror_integer(record, field)


def _vram_mirror_sample_offsets() -> tuple[int, ...]:
    offsets = [
        VRAM_MIRROR_DENSE_OFFSET + word * 4
        for word in range(VRAM_MIRROR_DENSE_ROWS * VRAM_MIRROR_ROW_WORDS)
    ]
    for base in VRAM_MIRROR_PAIR_BASES:
        for row in range(VRAM_MIRROR_PAIR_ROWS):
            offsets.extend(
                base + row * VRAM_MIRROR_ROW_BYTES + word * 4
                for word in range(VRAM_MIRROR_ROW_WORDS)
            )
    for bit in range(VRAM_MIRROR_SWEEP_FIRST_BIT, VRAM_MIRROR_SWEEP_LAST_BIT + 1):
        power = 1 << bit
        offsets.extend((VRAM_MIRROR_DENSE_OFFSET ^ power, power))
    if len(offsets) != VRAM_MIRROR_CPU_SAMPLE_WORD_COUNT:
        raise ProtocolError("PSP-VRAM-MIRROR-001: sample plan count is inconsistent")
    return tuple(offsets)


def _vram_mirror_validate_address_pairs(
    record: TestResult, expected_addresses: tuple[int, ...], *, value_max: int = 0xFFFFFFFF
) -> None:
    for index, expected_address in enumerate(expected_addresses):
        address = _vram_mirror_integer(record, f"out{index * 2}")
        sample = _vram_mirror_integer(record, f"out{index * 2 + 1}")
        if address != expected_address:
            raise ProtocolError(
                f"PSP-VRAM-MIRROR-001: {record.case_id} out{index * 2} "
                f"must name address {expected_address:#010x}, got {address:#010x}"
            )
        if sample > value_max:
            raise ProtocolError(
                f"PSP-VRAM-MIRROR-001: {record.case_id} raw sample "
                f"{sample:#010x} exceeds {value_max:#x}"
            )


def parse_vram_mirror_output(
    text: str, *, require_complete: bool = True
) -> SequenceReport:
    """Parse raw VRAM mirror observations without asserting hardware values."""

    sequence = parse_sequence(text, VRAM_MIRROR_SPEC, require_complete=require_complete)
    _validate_vram_mirror_shapes(sequence)
    info_record = sequence.results.get("vram-mirror-info")
    if info_record is None:
        return sequence
    if info_record.status == "SKIP":
        if any(
            record.status != "SKIP"
            for case_id, record in sequence.results.items()
            if case_id != "vram-mirror-info"
        ):
            raise ProtocolError("PSP-VRAM-MIRROR-001: skipped setup has measured records")
        return sequence
    if info_record.status != "PASS":
        raise ProtocolError("PSP-VRAM-MIRROR-001: setup must PASS or SKIP")

    info_words = tuple(
        _vram_mirror_integer(info_record, f"out{index}")
        for index in range(VRAM_MIRROR_INFO_OUT_COUNT)
    )
    info = dict(zip(VRAM_MIRROR_INFO_FIELDS, info_words, strict=True))
    cached_base = info["cached_base"]
    uncached_base = info["uncached_base"]
    display_offset = info["display_offset"]
    display_bytes = info["display_bytes"]
    display_stride = info["display_stride"]
    display_format = info["display_format"]
    color_offset = info["color_offset"]
    depth_offset = info["depth_offset"]
    frame_bytes = info["color_frame_bytes"]
    if cached_base != VRAM_MIRROR_BASES[0] or uncached_base != VRAM_MIRROR_BASES[4]:
        raise ProtocolError("PSP-VRAM-MIRROR-001: base address fields are not PSP VRAM aliases")
    if (
        display_bytes == 0
        or display_stride == 0
        or display_format > 3
        or display_offset + display_bytes > VRAM_MIRROR_BYTES
    ):
        raise ProtocolError("PSP-VRAM-MIRROR-001: display span is outside 2 MiB VRAM")
    display_bytes_per_pixel = 4 if display_format == 3 else 2
    if display_bytes != display_stride * 272 * display_bytes_per_pixel:
        raise ProtocolError("PSP-VRAM-MIRROR-001: display span does not match its format")
    if (
        color_offset % VRAM_MIRROR_ROW_BYTES != 0
        or color_offset < display_offset + display_bytes
        or frame_bytes != VRAM_MIRROR_FRAME_BYTES
        or info["depth_frame_bytes"] != VRAM_MIRROR_FRAME_BYTES
        or depth_offset != color_offset + frame_bytes
        or depth_offset + info["depth_frame_bytes"] > VRAM_MIRROR_BYTES
    ):
        raise ProtocolError("PSP-VRAM-MIRROR-001: scratch color/depth spans are invalid")
    expected_info = {
        "vram_bytes": VRAM_MIRROR_BYTES,
        "cpu_sample_word_count": VRAM_MIRROR_CPU_SAMPLE_WORD_COUNT,
        "cpu_dense_offset": VRAM_MIRROR_DENSE_OFFSET,
        "cpu_dense_rows": VRAM_MIRROR_DENSE_ROWS,
        "cpu_row_bytes": VRAM_MIRROR_ROW_BYTES,
        "cpu_pair_region_count": len(VRAM_MIRROR_PAIR_BASES),
        "cpu_pair_rows": VRAM_MIRROR_PAIR_ROWS,
        "cpu_sweep_first_bit": VRAM_MIRROR_SWEEP_FIRST_BIT,
        "cpu_sweep_last_bit": VRAM_MIRROR_SWEEP_LAST_BIT,
        "write_alias_count": len(VRAM_MIRROR_WRITE_BASES),
        "write_sample_count": len(VRAM_MIRROR_WRITE_OFFSETS),
        "write_offset_0": VRAM_MIRROR_WRITE_OFFSETS[0],
        "write_offset_1": VRAM_MIRROR_WRITE_OFFSETS[1],
        "write_offset_2": VRAM_MIRROR_WRITE_OFFSETS[2],
        "write_scan_bytes": VRAM_MIRROR_BYTES,
        "depth_stride": VRAM_MIRROR_DEPTH_STRIDE,
        "depth_row_count": len(VRAM_MIRROR_DEPTH_ROWS),
        **{
            f"depth_row_y{row}": y
            for row, y in enumerate(VRAM_MIRROR_DEPTH_ROWS)
        },
        "depth_alias_count": len(VRAM_MIRROR_DEPTH_ALIASES),
        "depth_chunk_pixels": VRAM_MIRROR_DEPTH_CHUNK_PIXELS,
        "color_row_count": len(VRAM_MIRROR_COLOR_ROWS),
        **{
            f"color_row_y{row}": y
            for row, y in enumerate(VRAM_MIRROR_COLOR_ROWS)
        },
        "color_chunk_pixels": VRAM_MIRROR_COLOR_CHUNK_PIXELS,
        "depth_test_enabled": 1,
        "depth_function": VRAM_MIRROR_DEPTH_FUNCTION_ALWAYS,
        "depth_mask": VRAM_MIRROR_DEPTH_MASK_WRITE_ENABLE,
        "depth_range_near": VRAM_MIRROR_DEPTH_RANGE_NEAR,
        "depth_range_far": VRAM_MIRROR_DEPTH_RANGE_FAR,
        "clear_depth": VRAM_MIRROR_CLEAR_DEPTH,
        "pattern_prefix": VRAM_MIRROR_PATTERN_PREFIX,
        "marker_prefix": VRAM_MIRROR_MARKER_PREFIX,
        "cached_alias_count": VRAM_MIRROR_CACHED_ALIAS_COUNT,
    }
    if any(info[field] != expected for field, expected in expected_info.items()):
        raise ProtocolError("PSP-VRAM-MIRROR-001: probe plan metadata does not match the contract")
    write_offsets = tuple(info[f"write_offset_{index}"]
                          for index in range(len(VRAM_MIRROR_WRITE_OFFSETS)))
    if (
        len(set(write_offsets)) != len(write_offsets)
        or any(offset & 3 or offset >= 0x00100000 for offset in write_offsets)
    ):
        raise ProtocolError("PSP-VRAM-MIRROR-001: write scan offsets are invalid")

    sample_offsets = _vram_mirror_sample_offsets()
    for chunk in range(VRAM_MIRROR_CPU_SAMPLE_CHUNK_COUNT):
        first = chunk * VRAM_MIRROR_SAMPLE_CHUNK_WORDS
        offsets = sample_offsets[first:first + VRAM_MIRROR_SAMPLE_CHUNK_WORDS]
        record = sequence.results[f"vram-cpu-pattern-{chunk:03d}"]
        addresses = tuple(
            alias + offset for offset in offsets for alias in VRAM_MIRROR_BASES
        )
        _vram_mirror_validate_address_pairs(record, addresses)

    write_offsets = tuple(info[f"write_offset_{index}"]
                          for index in range(len(VRAM_MIRROR_WRITE_OFFSETS)))
    for alias_index, write_base in enumerate(VRAM_MIRROR_WRITE_BASES):
        for sample_index, offset in enumerate(write_offsets):
            record = sequence.results[
                f"vram-cpu-write-mirror-{alias_index:02d}-{sample_index:02d}"
            ]
            values = tuple(
                _vram_mirror_integer(record, f"out{index}") for index in range(4)
            )
            expected_address = write_base + offset
            expected_marker = (
                VRAM_MIRROR_MARKER_PREFIX | (alias_index << 20) | offset
            )
            if values[0:2] != (expected_address, expected_marker):
                raise ProtocolError(
                    f"PSP-VRAM-MIRROR-001: {record.case_id} write address/marker "
                    "does not match the named mirror write"
                )
            found_offset, found_count = values[2:4]
            if found_count > VRAM_MIRROR_BYTES // 4:
                raise ProtocolError(
                    f"PSP-VRAM-MIRROR-001: {record.case_id} match count exceeds VRAM"
                )
            if found_count == 0:
                if found_offset != 0xFFFFFFFF:
                    raise ProtocolError(
                        f"PSP-VRAM-MIRROR-001: {record.case_id} no-match sentinel is invalid"
                    )
            elif found_offset == 0xFFFFFFFF or found_offset & 3 or found_offset >= VRAM_MIRROR_BYTES:
                raise ProtocolError(
                    f"PSP-VRAM-MIRROR-001: {record.case_id} first match offset is invalid"
                )

    for sync_case in VRAM_MIRROR_GE_SYNC_CASES:
        case_id = sync_case[0]
        record = sequence.results[case_id]
        if record.status == "SKIP":
            continue
        values = tuple(
            _vram_mirror_integer(record, f"out{index}")
            for index in range(VRAM_MIRROR_GE_SYNC_OUT_COUNT)
        )
        if values[7:16] != (
            VRAM_MIRROR_CACHED_ALIAS_COUNT,
            depth_offset,
            VRAM_MIRROR_DEPTH_STRIDE,
            info["depth_range_near"],
            info["depth_range_far"],
            info["depth_function"],
            info["depth_mask"],
            info["depth_test_enabled"],
            info["clear_depth"],
        ):
            raise ProtocolError(
                f"PSP-VRAM-MIRROR-001: {case_id} GE state metadata disagrees with setup"
            )
        if values[16:] != sync_case[1:]:
            raise ProtocolError(
                f"PSP-VRAM-MIRROR-001: {case_id} operation/vertex metadata is invalid"
            )
        if values[4] > 1 or values[6] > 1:
            raise ProtocolError(
                f"PSP-VRAM-MIRROR-001: {case_id} idle fields are not booleans"
            )

    depth_rows_y = tuple(info[f"depth_row_y{row}"]
                         for row in range(VRAM_MIRROR_DEPTH_ROW_COUNT))
    for row, y in enumerate(depth_rows_y):
        for chunk in range(VRAM_MIRROR_DEPTH_ROW_CHUNK_COUNT):
            record = sequence.results[f"vram-ge-depth-row{row}-{chunk:02d}"]
            if record.status == "SKIP":
                continue
            offsets = tuple(
                depth_offset + (y * VRAM_MIRROR_DEPTH_STRIDE
                                + chunk * VRAM_MIRROR_DEPTH_CHUNK_PIXELS + pixel) * 2
                for pixel in range(VRAM_MIRROR_DEPTH_CHUNK_PIXELS)
            )
            addresses = tuple(
                alias + offset
                for offset in offsets
                for alias in VRAM_MIRROR_DEPTH_ALIASES
            )
            _vram_mirror_validate_address_pairs(record, addresses, value_max=0xFFFF)

    for row, y in enumerate(VRAM_MIRROR_COLOR_ROWS):
        for chunk in range(VRAM_MIRROR_COLOR_ROW_CHUNK_COUNT):
            record = sequence.results[f"vram-ge-color-row{row}-{chunk:02d}"]
            if record.status == "SKIP":
                continue
            offsets = tuple(
                color_offset + (
                    y * VRAM_MIRROR_DEPTH_STRIDE
                    + chunk * VRAM_MIRROR_COLOR_CHUNK_PIXELS + pixel
                ) * 2
                for pixel in range(VRAM_MIRROR_COLOR_CHUNK_PIXELS)
            )
            addresses = tuple(VRAM_MIRROR_COLOR_ALIAS + offset for offset in offsets)
            _vram_mirror_validate_address_pairs(record, addresses, value_max=0xFFFF)

    restore = sequence.results["vram-restore"]
    if restore.status != "SKIP":
        restore_values = tuple(
            _vram_mirror_integer(restore, f"out{index}") for index in range(5)
        )
        if restore_values[0:2] != (VRAM_MIRROR_BASES[4], VRAM_MIRROR_BYTES):
            raise ProtocolError("PSP-VRAM-MIRROR-001: restore record does not cover all VRAM")
        if restore_values[2] != 2 * VRAM_MIRROR_CACHED_ALIAS_COUNT:
            raise ProtocolError(
                "PSP-VRAM-MIRROR-001: restore record must maintain every cached VRAM alias"
            )
        if restore_values[4] > 1:
            raise ProtocolError("PSP-VRAM-MIRROR-001: restore GE idle field is not boolean")
        if restore.status == "PASS" and restore_values[3] != 0:
            raise ProtocolError("PSP-VRAM-MIRROR-001: passing restore has byte mismatches")
        done = sequence.results[VRAM_MIRROR_SPEC.terminal_case]
        if restore.status != "PASS" and done.status == "PASS":
            raise ProtocolError("PSP-VRAM-MIRROR-001: terminal PASS contradicts failed restore")
    done = sequence.results[VRAM_MIRROR_SPEC.terminal_case]
    if done.status != "SKIP" and _vram_mirror_integer(done, "out0") != len(
        VRAM_MIRROR_SEMANTIC_CASES
    ):
        raise ProtocolError("PSP-VRAM-MIRROR-001: terminal record count is invalid")
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
DMAC_INVALID_K = 0xC000
DMAC_INVALID_DELTAS = (1, 4, 0x1000, 0x2000)
DMAC_INVALID_POST_GUARD_BYTES = 0x1000
DMAC_INVALID_OVERFLOW_BAND_BYTES = 0x2000


@dataclass(frozen=True)
class DmacInvalidLaunch:
    tier: str
    case_ids: tuple[str, ...]
    api: str | None = None
    endpoint: str | None = None
    cell: str | None = None


_DMAC_INVALID_S0_SHAPES = (("a", "dst"), ("b", "src"), ("c", "both"), ("d", "dst"))
# None means the endpoint must be non-null; it does not establish ownership.
# The ownership proof remains the producer's setup_mask and its source-owned
# allocation/module buffer contract.
_DMAC_INVALID_S0_POINTER_SHAPES = {
    "a": (None, 0),
    "b": (0, None),
    "c": (None, None),
    "d": (None, 0xFFFFFFFF),
}
_DMAC_INVALID_B_LAUNCHES = (
    ("dma-invalid-tail-memcpy-dst", "b1", "memcpy", "dst"),
    ("dma-invalid-tail-memcpy-src", "b2", "memcpy", "src"),
    ("dma-invalid-tail-try-dst", "b3", "try", "dst"),
    ("dma-invalid-tail-try-src", "b4", "try", "src"),
)
DMAC_INVALID_CASES = {
    "dma-invalid-tail-s0": DmacInvalidLaunch(
        tier="S",
        case_ids=tuple(
            f"invalid-tail-s0-{shape}-{api}"
            for api in ("memcpy", "try")
            for shape, _endpoint in _DMAC_INVALID_S0_SHAPES
        ),
    ),
    **{
        campaign_case: DmacInvalidLaunch(
            tier="B",
            case_ids=tuple(
                f"invalid-tail-{cell}-delta-{delta:04x}"
                for delta in DMAC_INVALID_DELTAS
            ),
            api=api,
            endpoint=endpoint,
            cell=cell,
        )
        for campaign_case, cell, api, endpoint in _DMAC_INVALID_B_LAUNCHES
    },
}

DMAC_INVALID_FIELDS = frozenset({
    "result", "rc", "P", "matches", "guards_outside", "post_guard",
    "overflow_band", "source_intact", "setup_mask", "K", "delta", "api",
    "endpoint", "cache_discipline", "tier", "executed", "payload_mutations",
    "source_addr", "destination_addr",
})


def _parse_dmac_invalid_values(record: TestResult, campaign_case: str) -> dict[str, int | str]:
    values: dict[str, int | str] = dict(record.values)
    if set(values) != DMAC_INVALID_FIELDS:
        raise ProtocolError(f"{campaign_case}: Tier {record.case_id} fields are incomplete or unexpected")
    numeric_fields = DMAC_INVALID_FIELDS - {"api", "endpoint", "tier"}
    for field in numeric_fields:
        value = values[field]
        if not isinstance(value, str) or not value.startswith("0x"):
            raise ProtocolError(f"{campaign_case}: {record.case_id} {field} is not hexadecimal")
        try:
            values[field] = int(value, 16)
        except ValueError as exc:
            raise ProtocolError(f"{campaign_case}: {record.case_id} {field} is not hexadecimal") from exc
    if values["result"] != values["rc"]:
        raise ProtocolError(f"{campaign_case}: {record.case_id} result does not match rc")
    if values["K"] != DMAC_INVALID_K:
        raise ProtocolError(f"{campaign_case}: {record.case_id} changed the disputed K boundary")
    if values["source_intact"] not in {0, 1} or values["cache_discipline"] not in {0, 1}:
        raise ProtocolError(f"{campaign_case}: {record.case_id} boolean field is outside 0/1")
    if values["executed"] not in {0, 1}:
        raise ProtocolError(f"{campaign_case}: {record.case_id} executed is outside 0/1")
    if any(not 0 <= values[field] <= 0xFFFFFFFF for field in (
        "source_addr", "destination_addr",
    )):
        raise ProtocolError(
            f"{campaign_case}: {record.case_id} diagnostic address is not a PSP address"
        )
    return values


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
    launch = DMAC_INVALID_CASES.get(campaign_case)
    if launch is None:
        raise ProtocolError(f"unknown isolated DMAC invalid-tail case {campaign_case!r}")
    parsed = parse_output(text)
    if len(parsed.results) != len(launch.case_ids):
        raise ProtocolError(f"{campaign_case}: stream does not contain its complete cell set")
    observed_ids = tuple(record.case_id for record in parsed.results)
    if observed_ids != launch.case_ids:
        raise ProtocolError(f"{campaign_case}: cell ids or order differ from the launch contract")
    if any(record.test_id != "PSP-DMAC-001" for record in parsed.results):
        raise ProtocolError(f"{campaign_case}: stream contains a foreign test id")
    statuses = {record.status for record in parsed.results}
    if len(statuses) != 1 or not statuses <= {"PASS", "SKIP"}:
        raise ProtocolError(f"{campaign_case}: stream has a mixed or failing cell status")

    for record in parsed.results:
        values = _parse_dmac_invalid_values(record, campaign_case)
        if values["tier"] != launch.tier:
            raise ProtocolError(f"{campaign_case}: {record.case_id} has the wrong tier")
        if launch.tier == "S":
            shape, api = record.case_id.removeprefix("invalid-tail-s0-").rsplit("-", 1)
            endpoint = dict(_DMAC_INVALID_S0_SHAPES).get(shape)
            if endpoint is None or values["api"] != api or values["endpoint"] != endpoint:
                raise ProtocolError(f"{campaign_case}: {record.case_id} pointer cell is mislabeled")
            if values["delta"] != 0:
                raise ProtocolError(f"{campaign_case}: {record.case_id} size-zero cell has a delta")
        else:
            if values["api"] != launch.api or values["endpoint"] != launch.endpoint:
                raise ProtocolError(f"{campaign_case}: {record.case_id} API or endpoint is mislabeled")
            delta = int(record.case_id.rsplit("-", 1)[1], 16)
            if values["delta"] != delta or delta not in DMAC_INVALID_DELTAS:
                raise ProtocolError(f"{campaign_case}: {record.case_id} delta is outside the cell matrix")
        if record.status == "SKIP":
            if values["executed"] != 0 or values["setup_mask"] == 0xF:
                raise ProtocolError(f"{campaign_case}: {record.case_id} SKIP did not fail a setup gate")
            if values["cache_discipline"] != 0:
                raise ProtocolError(f"{campaign_case}: {record.case_id} SKIP claims a transfer cache bracket")
            if values["source_intact"] != 0:
                raise ProtocolError(
                    f"{campaign_case}: {record.case_id} SKIP cannot claim source integrity"
                )
            if any(values[field] != 0 for field in (
                "P", "matches", "guards_outside", "post_guard", "overflow_band",
                "payload_mutations",
            )):
                raise ProtocolError(f"{campaign_case}: {record.case_id} SKIP contains transfer mutations")
            continue

        if values["executed"] != 1 or values["setup_mask"] != 0xF:
            raise ProtocolError(f"{campaign_case}: {record.case_id} PASS lacks ownership proof")
        if values["cache_discipline"] != 1 or values["source_intact"] != 1:
            raise ProtocolError(f"{campaign_case}: {record.case_id} PASS lacks cache/source integrity")
        if values["guards_outside"] != 0:
            raise ProtocolError(f"{campaign_case}: {record.case_id} changed an outside guard")

        if launch.tier == "S":
            if values["P"] != 0 or values["matches"] != 0:
                raise ProtocolError(f"{campaign_case}: {record.case_id} size-zero cell moved bytes")
            if any(values[field] != 0 for field in (
                "post_guard", "overflow_band", "payload_mutations",
            )):
                raise ProtocolError(f"{campaign_case}: {record.case_id} size-zero cell mutated scratch")
            expected_source, expected_destination = _DMAC_INVALID_S0_POINTER_SHAPES[shape]
            for field, expected in (
                ("source_addr", expected_source),
                ("destination_addr", expected_destination),
            ):
                matches_shape = (
                    values[field] != 0 if expected is None else values[field] == expected
                )
                if not matches_shape:
                    raise ProtocolError(
                        f"{campaign_case}: {record.case_id} {field} does not match its S0 pointer shape"
                    )
            continue

        delta_text = record.case_id.rsplit("-", 1)[1]
        delta = int(delta_text, 16)
        requested = DMAC_INVALID_K + delta
        if values["P"] > requested or values["matches"] > requested or values["P"] > values["matches"]:
            raise ProtocolError(f"{campaign_case}: {record.case_id} copy counts exceed the request")
        if launch.endpoint == "dst":
            if values["payload_mutations"] != 0:
                raise ProtocolError(
                    f"{campaign_case}: {record.case_id} destination cell claims source payload mutations"
                )
            if values["post_guard"] > DMAC_INVALID_POST_GUARD_BYTES or \
                    values["overflow_band"] > DMAC_INVALID_OVERFLOW_BAND_BYTES:
                raise ProtocolError(f"{campaign_case}: {record.case_id} mutation count exceeds its region")
            expected_overrun = max(values["P"] - DMAC_INVALID_K, 0)
            expected_post = min(expected_overrun, DMAC_INVALID_POST_GUARD_BYTES)
            expected_band = max(expected_overrun - DMAC_INVALID_POST_GUARD_BYTES, 0)
            if values["post_guard"] != expected_post or values["overflow_band"] != expected_band:
                raise ProtocolError(f"{campaign_case}: {record.case_id} overrun classification is incomplete")
        elif any(values[field] != 0 for field in (
            "post_guard", "overflow_band", "payload_mutations",
        )):
            raise ProtocolError(f"{campaign_case}: {record.case_id} source cell mutated its scratch block")
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
# Campaign cases whose measurements are intentionally still NOT_RUN.
# ---------------------------------------------------------------------------

CAMPAIGN_PROBE_CASES: dict[str, tuple[StreamSpec, dict[str, int]]] = {}


def _register_campaign_probe(
    campaign_case: str,
    test_id: str,
    semantic_cases: tuple[str, ...],
    out_counts: dict[str, int],
) -> None:
    spec = StreamSpec(test_id, semantic_cases, f"{campaign_case}-done")
    counts = dict(out_counts)
    counts[spec.terminal_case] = 1
    CAMPAIGN_PROBE_CASES[campaign_case] = (spec, counts)


_register_campaign_probe(
    "kernel-alarm", "PSP-ALARM-001",
    ("alarm-null-handler", "alarm-zero-clock", "alarm-table-exhaustion",
     "alarm-cancel-fired-once", "alarm-cancel-cancelled", "alarm-cancel-unknown",
     "alarm-rearm-base", "alarm-blocking-in-handler"),
    {"alarm-null-handler": 2, "alarm-zero-clock": 3, "alarm-table-exhaustion": 3,
     "alarm-cancel-fired-once": 3, "alarm-cancel-cancelled": 3,
     "alarm-cancel-unknown": 1, "alarm-rearm-base": 5,
     "alarm-blocking-in-handler": 5},
)
_register_campaign_probe(
    "thread-scheduler", "PSP-THREAD-003",
    ("thread-suspend-idle", "thread-suspend-self", "thread-resume-idle",
     "thread-rotate-range", "thread-ready-order-after-rotate",
     "thread-suspend-wait-timeout"),
    {"thread-suspend-idle": 4, "thread-suspend-self": 4,
     "thread-resume-idle": 1, "thread-rotate-range": 1,
     "thread-ready-order-after-rotate": 7, "thread-suspend-wait-timeout": 5},
)
_register_campaign_probe(
    "wait-outcomes", "PSP-WAIT-001",
    ("sema-signal-before-deadline-late-dispatch",
     "event-signal-before-deadline-late-dispatch",
     "sema-cancel-before-deadline-late-dispatch",
     "event-cancel-before-deadline-late-dispatch"),
    {case_id: 5 for case_id in (
        "sema-signal-before-deadline-late-dispatch",
        "event-signal-before-deadline-late-dispatch",
        "sema-cancel-before-deadline-late-dispatch",
        "event-cancel-before-deadline-late-dispatch",
    )},
)
_register_campaign_probe(
    "refer-status-size", "PSP-KERNEL-STATUS-001",
    tuple(f"{kind}-size-{size}" for kind in ("sema", "event", "mbx")
          for size in ("zero", "8", "40", "full")),
    {f"{kind}-size-{size}": 5
     for kind in ("sema", "event", "mbx")
     for size in ("zero", "8", "40", "full")},
)
_register_campaign_probe(
    "ge-break-continue", "PSP-GE-CONTROL-001",
    ("ge-break-no-active-list", "ge-continue-no-paused-list",
     "ge-break-invalid-mode", "ge-list-sync-paused", "ge-draw-sync-paused",
     "ge-continue-drain", "ge-quiesce-after-continue",
     "ge-break-reset-no-dequeue", "ge-list-sync-cancelled",
     "ge-draw-sync-cancelled", "ge-quiesce-after-cancel",
     "ge-list-dequeue-stalled", "ge-quiesce-after-dequeue"),
    {"ge-break-no-active-list": 2, "ge-continue-no-paused-list": 1,
     "ge-break-invalid-mode": 2, "ge-list-sync-paused": 3,
     "ge-draw-sync-paused": 3, "ge-continue-drain": 5,
     "ge-quiesce-after-continue": 4, "ge-break-reset-no-dequeue": 2,
     "ge-list-sync-cancelled": 3, "ge-draw-sync-cancelled": 3,
     "ge-quiesce-after-cancel": 4, "ge-list-dequeue-stalled": 4,
     "ge-quiesce-after-dequeue": 4},
)
_register_campaign_probe(
    "kernel-misc", "PSP-KERNEL-MISC-001",
    ("sysclock-wide", "ctrl-sampling-mode", "thread-profiler",
     "global-profiler", "vtimer-basic", "display-basic", "impose-basic"),
    {"sysclock-wide": 4, "ctrl-sampling-mode": 2, "thread-profiler": 2,
     "global-profiler": 2, "vtimer-basic": 13, "display-basic": 4,
     "impose-basic": 5},
)
# vfpu-compare: 15 operand pairs for each of vscmp.s, vsge.s and vslt.s, emitted
# op-major (every vscmp.s cell, then vsge.s, then vslt.s), each with three
# words: the model word, then the two operands. PASS means the compare wrote its
# destination; agreement with the model is the result word against out0, which a
# report reads (the status does not judge it). The parser checks shape.
_VFPU_COMPARE_PAIRS = (
    "lt", "eq", "gt", "zero-pos-neg", "zero-neg-pos", "nan-left-quiet",
    "nan-right-quiet", "nan-left-signal", "nan-right-signal", "nan-left-negative",
    "inf-pos-neg", "inf-neg-pos", "inf-pos-pos", "inf-neg-neg", "inf-pos-finite",
)
_register_campaign_probe(
    "vfpu-compare", "PSP-VFPU-CMP-001",
    tuple(f"{op}-{pair}" for op in ("vscmp", "vsge", "vslt") for pair in _VFPU_COMPARE_PAIRS),
    {f"{op}-{pair}": 3 for op in ("vscmp", "vsge", "vslt") for pair in _VFPU_COMPARE_PAIRS},
)


# HLE measurement families (probe_hle_measure.c; H-oracle-hle-measure-1009).
# Every cell is emitted, as PASS, FAIL or SKIP, so each stream has a fixed shape
# and order. The contracts check shape and order only: no PSP return value is
# asserted, because these probes exist to measure what the console returns. The one
# exception is validate_hle_edram_restore, which checks the GE probe's own restore
# of the state it changed, not a console semantic.
_register_campaign_probe(
    "hle-kernel-status", "PSP-HLE-KERNEL-STATUS-001",
    ("sys-status-size-0x1c", "sys-status-size-0x20", "sys-status-size-0x08",
     "sys-status-size-0x00", "fpl-create", "fpl-refer-full-all-free",
     "fpl-try-alloc-a", "fpl-try-alloc-b", "fpl-try-alloc-exhausted",
     "fpl-refer-full-two-held", "fpl-refer-size-0x08-two-held",
     "fpl-refer-size-0x00-two-held", "fpl-free-a", "fpl-refer-full-one-free",
     "fpl-free-b", "fpl-delete", "fpl-refer-full-after-delete"),
    {"sys-status-size-0x1c": 8, "sys-status-size-0x20": 8,
     "sys-status-size-0x08": 8, "sys-status-size-0x00": 8,
     "fpl-create": 1, "fpl-refer-full-all-free": 14,
     "fpl-try-alloc-a": 1, "fpl-try-alloc-b": 1, "fpl-try-alloc-exhausted": 1,
     "fpl-refer-full-two-held": 14, "fpl-refer-size-0x08-two-held": 14,
     "fpl-refer-size-0x00-two-held": 14, "fpl-free-a": 1,
     "fpl-refer-full-one-free": 14, "fpl-free-b": 1, "fpl-delete": 1,
     "fpl-refer-full-after-delete": 14},
)
_register_campaign_probe(
    "hle-vtimer", "PSP-HLE-VTIMER-001",
    ("vtimer-create", "vtimer-refer-created", "vtimer-stop-not-started",
     "vtimer-start", "vtimer-time-running-early", "vtimer-refer-running-early",
     "vtimer-delay-20ms", "vtimer-time-running-20ms", "vtimer-stop",
     "vtimer-time-stopped", "vtimer-refer-stopped", "vtimer-delay-20ms-stopped",
     "vtimer-time-stopped-later", "vtimer-start-restart", "vtimer-refer-size-0x08",
     "vtimer-delete-running", "vtimer-refer-after-delete", "vtimer-delete-again"),
    {"vtimer-create": 1, "vtimer-refer-created": 18, "vtimer-stop-not-started": 1,
     "vtimer-start": 1, "vtimer-time-running-early": 2,
     "vtimer-refer-running-early": 18, "vtimer-delay-20ms": 1,
     "vtimer-time-running-20ms": 2, "vtimer-stop": 1, "vtimer-time-stopped": 2,
     "vtimer-refer-stopped": 18, "vtimer-delay-20ms-stopped": 1,
     "vtimer-time-stopped-later": 2, "vtimer-start-restart": 1,
     "vtimer-refer-size-0x08": 18, "vtimer-delete-running": 1,
     "vtimer-refer-after-delete": 18, "vtimer-delete-again": 1},
)
# hle-power-clock: the scePower getters return the clock, so for the Int and bare
# getters the record's result IS the value (the frequency) and out0 repeats it. For
# the Float getters result is the IEEE-754 bit pattern and out0 the truncated integer
# part (all ones when out of range or NaN). The contract checks field shape only, so
# result is never read here as a return code.
_register_campaign_probe(
    "hle-power-clock", "PSP-HLE-POWER-001",
    ("pll-clock-int", "pll-clock-float", "cpu-clock-int", "cpu-clock-float",
     "cpu-clock-alias", "bus-clock-int", "bus-clock-float", "bus-clock-alias",
     "cpu-clock-int-repeat"),
    {case_id: 1 for case_id in (
        "pll-clock-int", "pll-clock-float", "cpu-clock-int", "cpu-clock-float",
        "cpu-clock-alias", "bus-clock-int", "bus-clock-float", "bus-clock-alias",
        "cpu-clock-int-repeat",
    )},
)
_register_campaign_probe(
    "hle-hprm", "PSP-HLE-HPRM-001",
    ("hprm-remote-first", "hprm-headphone-first", "hprm-microphone-first",
     "hprm-remote-second", "hprm-headphone-second", "hprm-microphone-second"),
    {case_id: 1 for case_id in (
        "hprm-remote-first", "hprm-headphone-first", "hprm-microphone-first",
        "hprm-remote-second", "hprm-headphone-second", "hprm-microphone-second",
    )},
)
_register_campaign_probe(
    "hle-ctrl-latch", "PSP-HLE-CTRL-LATCH-001",
    ("latch-read-initial", "latch-peek-initial", "latch-poll-window",
     "latch-read-after-poll", "latch-peek-after-poll",
     "latch-read-clears-peek-keeps"),
    {"latch-read-initial": 4, "latch-peek-initial": 4, "latch-poll-window": 6,
     "latch-read-after-poll": 4, "latch-peek-after-poll": 4,
     "latch-read-clears-peek-keeps": 5},
)
_register_campaign_probe(
    "hle-sysparam", "PSP-HLE-SYSPARAM-001",
    ("int-id-2", "int-id-3", "int-id-4", "int-id-5", "int-id-6", "int-id-7",
     "int-id-8", "int-id-9", "int-unknown-0", "int-unknown-10", "int-unknown-64",
     "string-id-1-len-0x80", "string-id-1-len-0x04"),
    {**{f"int-id-{i}": 1 for i in range(2, 10)},
     "int-unknown-0": 1, "int-unknown-10": 1, "int-unknown-64": 1,
     "string-id-1-len-0x80": 3, "string-id-1-len-0x04": 3},
)
_register_campaign_probe(
    "hle-ge-edram", "PSP-HLE-GE-EDRAM-001",
    ("edram-size", "edram-addr", "edram-width-query-initial",
     "edram-width-set-512", "edram-width-set-1024", "edram-width-set-2048",
     "edram-width-set-4096", "edram-width-restore", "edram-width-query-final"),
    {case_id: 1 for case_id in (
        "edram-size", "edram-addr", "edram-width-query-initial",
        "edram-width-set-512", "edram-width-set-1024", "edram-width-set-2048",
        "edram-width-set-4096", "edram-width-restore", "edram-width-query-final",
    )},
)

# Single-launch HLE cases (probe_hle_measure.c, one launch each). Shape only: a cell
# is PASS when its calls completed, SKIP when the object it needs was not obtained,
# and every out word is an observation. Nothing here compares a value with a PSP
# expectation. The fixed-pool delete cells of hle-kernel-pool-bounds are PASS with
# out0 0 when their create was refused (validate_hle_pool_bounds_deletes).
_register_campaign_probe(
    "hle-vtimer-unit", "PSP-HLE-VTIMER-002",
    ("vtimer-unit-create", "vtimer-unit-start", "vtimer-unit-time-a",
     "vtimer-unit-systime-a", "vtimer-unit-delay", "vtimer-unit-time-b",
     "vtimer-unit-systime-b", "vtimer-unit-base-running",
     "vtimer-unit-refer-running", "vtimer-unit-stop", "vtimer-unit-base-stopped",
     "vtimer-unit-refer-stopped", "vtimer-unit-delete"),
    {"vtimer-unit-create": 1, "vtimer-unit-start": 1, "vtimer-unit-time-a": 2,
     "vtimer-unit-systime-a": 2, "vtimer-unit-delay": 1, "vtimer-unit-time-b": 2,
     "vtimer-unit-systime-b": 2, "vtimer-unit-base-running": 2,
     "vtimer-unit-refer-running": 2, "vtimer-unit-stop": 1,
     "vtimer-unit-base-stopped": 2, "vtimer-unit-refer-stopped": 2,
     "vtimer-unit-delete": 1},
)
_register_campaign_probe(
    "hle-vtimer-errors", "PSP-HLE-VTIMER-003",
    ("vtimer-err-create-stale", "vtimer-err-delete-stale",
     "vtimer-err-start-deleted", "vtimer-err-stop-deleted",
     "vtimer-err-time-deleted", "vtimer-err-cleanup-stale",
     "vtimer-err-create-live", "vtimer-err-delete-live",
     "vtimer-err-noslot-create", "vtimer-err-noslot-cleanup"),
    {"vtimer-err-create-stale": 1, "vtimer-err-delete-stale": 1,
     "vtimer-err-start-deleted": 1, "vtimer-err-stop-deleted": 1,
     "vtimer-err-time-deleted": 2, "vtimer-err-cleanup-stale": 0,
     "vtimer-err-create-live": 1, "vtimer-err-delete-live": 1,
     "vtimer-err-noslot-create": 2, "vtimer-err-noslot-cleanup": 2},
)
_register_campaign_probe(
    "hle-kernel-pool-bounds", "PSP-HLE-KERNEL-STATUS-002",
    ("fpl-create-size-0x00", "fpl-delete-size-0x00",
     "fpl-create-size-0x7fffffff", "fpl-delete-size-0x7fffffff", "vpl-create",
     "vpl-try-alloc-refusal", "vpl-free-all", "vpl-delete"),
    {"fpl-create-size-0x00": 1, "fpl-delete-size-0x00": 1,
     "fpl-create-size-0x7fffffff": 1, "fpl-delete-size-0x7fffffff": 1,
     "vpl-create": 1, "vpl-try-alloc-refusal": 2, "vpl-free-all": 2,
     "vpl-delete": 1},
)
_register_campaign_probe(
    "hle-null-out-pointers", "PSP-HLE-NULL-OUT-001",
    ("null-vtimer-create", "null-sys-status", "null-vtimer-time", "null-vtimer-base",
     "null-vtimer-refer", "null-vtimer-delete"),
    {"null-vtimer-create": 0, "null-sys-status": 0, "null-vtimer-time": 0,
     "null-vtimer-base": 0, "null-vtimer-refer": 0, "null-vtimer-delete": 0},
)
_register_campaign_probe(
    "hle-power-float-v0", "PSP-HLE-POWER-002",
    ("pll-clock-float-v0", "cpu-clock-float-v0", "bus-clock-float-v0"),
    {"pll-clock-float-v0": 1, "cpu-clock-float-v0": 1, "bus-clock-float-v0": 1},
)
_HLE_AUDIO_SRC_INVALID_CASES = (
    "src-reserve-invalid-samples-zero", "src-reserve-invalid-samples-65",
    "src-reserve-invalid-samples-4096", "src-reserve-invalid-frequency",
    "src-reserve-invalid-channels-1", "src-reserve-invalid-channels-4",
)
_HLE_AUDIO_SRC_CASES = (
    "src-release-unreserved", "src-output-unreserved", "src-reserve-valid",
    "src-reserve-already-held", "src-output-blocking-0", "src-release-after-output",
    "src-release-again", *_HLE_AUDIO_SRC_INVALID_CASES, "out2-then-src-reserve",
    "src-then-out2-reserve", "src-final-cleanup", "out2-final-cleanup",
)
_register_campaign_probe(
    "hle-audio-src", "PSP-HLE-AUDIO-SRC-001", _HLE_AUDIO_SRC_CASES,
    {
        "src-release-unreserved": 0, "src-output-unreserved": 2,
        "src-reserve-valid": 3, "src-reserve-already-held": 3,
        "src-output-blocking-0": 2, "src-release-after-output": 0,
        "src-release-again": 0,
        **{case_id: 4 for case_id in _HLE_AUDIO_SRC_INVALID_CASES},
        "out2-then-src-reserve": 3, "src-then-out2-reserve": 3,
        "src-final-cleanup": 0, "out2-final-cleanup": 0,
    },
)
_HLE_TIMER_ORDER_CASES = (
    "alarm-same-delay-set", "alarm-same-delay-a", "alarm-same-delay-b",
    "alarm-long-then-short-set", "alarm-long-event", "alarm-short-event",
    "timer-order-vtimer-create", "timer-order-vtimer-handler",
    "timer-order-vtimer-alarm-set", "timer-order-vtimer-event",
    "timer-order-vtimer-alarm-event", "alarm-zero-set", "alarm-zero-event",
    "alarm-same-delay-cleanup", "alarm-long-short-cleanup",
    "timer-order-vtimer-cleanup", "alarm-zero-cleanup",
)
_register_campaign_probe(
    "hle-timer-order", "PSP-HLE-TIMER-ORDER-001", _HLE_TIMER_ORDER_CASES,
    {
        "alarm-same-delay-set": 1, "alarm-same-delay-a": 2,
        "alarm-same-delay-b": 2, "alarm-long-then-short-set": 1,
        "alarm-long-event": 2, "alarm-short-event": 2,
        "timer-order-vtimer-create": 1, "timer-order-vtimer-handler": 1,
        "timer-order-vtimer-alarm-set": 1, "timer-order-vtimer-event": 2,
        "timer-order-vtimer-alarm-event": 2, "alarm-zero-set": 1,
        "alarm-zero-event": 2, "alarm-same-delay-cleanup": 2,
        "alarm-long-short-cleanup": 2, "timer-order-vtimer-cleanup": 3,
        "alarm-zero-cleanup": 1,
    },
)

VFPU_TRANSCENDENTAL_OPS = (
    "vsin", "vcos", "vexp2", "vlog2", "vrsq", "vrcp", "vsqrt", "vasin",
    "vnrcp", "vnsin", "vrexp2",
)
VFPU_TRANSCENDENTAL_CHUNKS = 8
VFPU_TRANSCENDENTAL_SEMANTIC_CASES = tuple(
    f"{op}-chunk-{chunk:02d}"
    for op in VFPU_TRANSCENDENTAL_OPS
    for chunk in range(VFPU_TRANSCENDENTAL_CHUNKS)
)
_register_campaign_probe(
    "vfpu-transcendental-corpus", "PSP-VFPU-TRANSCENDENTAL-001",
    VFPU_TRANSCENDENTAL_SEMANTIC_CASES,
    {
        case_id: (7 if case_id.endswith("-chunk-07") else 8)
        for case_id in VFPU_TRANSCENDENTAL_SEMANTIC_CASES
    },
)

# llsc-link: three link windows and the sc behaviours they measure (see probe.c,
# run_llsc_link). Six words per cell: loaded, first sc result, A and B after the
# cell, and the model words for A and B. The parser checks shape only.
_register_campaign_probe(
    "llsc-link", "PSP-LLSC-001",
    ("llsc-control", "llsc-second-sc", "llsc-other-address"),
    {"llsc-control": 6, "llsc-second-sc": 6, "llsc-other-address": 6},
)

# fpu-cc: COP1 condition-code facts (see probe.c, run_fpu_cc). One FCR31 writability
# cell, then for each condition code 0..7 a c.eq.s cell (one word) and a bc1t/bc1f cell
# (four words). The condition code 0 cells are controls; the parser checks shape only.
_FPU_CC_CELLS = ("fpu-cc-fcr31-bits",) + tuple(
    f"fpu-cc{n}-{kind}" for n in range(8) for kind in ("ceq", "branch"))
_register_campaign_probe(
    "fpu-cc", "PSP-FPU-CC-001", _FPU_CC_CELLS,
    {cell: (2 if cell == "fpu-cc-fcr31-bits" else 1 if cell.endswith("-ceq") else 4)
     for cell in _FPU_CC_CELLS},
)


#: Documented GE eDRAM translation widths (pspge.h: 0 = do not set, else these).
HLE_EDRAM_WIDTHS = (512, 1024, 2048, 4096)


def validate_hle_edram_restore(text: str) -> None:
    """Fail closed unless the GE translation width was restored to its original value.

    The probe changes GE state only through sceGeEdramSetAddrTranslation and
    restores the width it found. sceGeEdramSetAddrTranslation(w) sets the width to w
    and returns the width it replaced (set-returning-previous, as captured on the
    PSP-3000 on 2026-10-10; that run was not acceptance-eligible, so the behaviour is
    CAPTURED, not MEASURED), so a read by Set(0) is itself a set to 0. The probe
    therefore verifies its restore with Set(initial), which returns the width now set
    and leaves it in place, and its final read is Set(initial) too. When the restore
    cell ran, its out0 and the final read must both equal the initial width. When the
    initial width was not restorable, every width cell must be SKIP and the final
    read, a Set(0), must return the 0 that the initial read left: the probe cannot put
    a non-documented width back, and says so by that 0.
    """

    parsed = parse_output(text)
    results = {record.case_id: record for record in parsed.results}
    try:
        initial = int(dict(results["edram-width-query-initial"].values)["result"], 0)
        final = int(dict(results["edram-width-query-final"].values)["result"], 0)
    except KeyError as exc:
        raise ProtocolError(f"PSP-HLE-GE-EDRAM-001: missing record {exc}") from exc
    width_cases = [f"edram-width-set-{width}" for width in HLE_EDRAM_WIDTHS]
    restorable = initial in HLE_EDRAM_WIDTHS
    if not restorable:
        if any(results[case].status != "SKIP" for case in width_cases + ["edram-width-restore"]):
            raise ProtocolError(
                "PSP-HLE-GE-EDRAM-001: a width cell ran although the original width "
                f"{initial:#x} is not restorable"
            )
        if final != 0:
            raise ProtocolError(
                f"PSP-HLE-GE-EDRAM-001: final read returned {final:#x}, not 0; with no restore "
                "cell the initial Set(0) left 0 and the final Set(0) must report that 0"
            )
        return
    if any(results[case].status == "SKIP" for case in width_cases + ["edram-width-restore"]):
        raise ProtocolError("PSP-HLE-GE-EDRAM-001: a restorable width cell was skipped")
    # The restore record's `result` holds the pre-restore width (the value its
    # Set(initial) returned) and its `out0` the verifying read (the value a second
    # Set(initial) returned), exactly as the probe records them. Both the verify and
    # the final read are Set(initial): under set-returning-previous each returns the
    # width the restore left and keeps it, so both must equal the original width.
    restore = dict(results["edram-width-restore"].values)
    if int(restore["out0"], 0) != initial:
        raise ProtocolError(
            f"PSP-HLE-GE-EDRAM-001: restore left width {int(restore['out0'], 0):#x}, "
            f"not the original {initial:#x}"
        )
    if final != initial:
        raise ProtocolError(
            f"PSP-HLE-GE-EDRAM-001: final read returned {final:#x}, not the restored "
            f"{initial:#x}; sceGeEdramSetAddrTranslation sets the width and returns the width "
            "it replaced (set-returning-previous), so the final Set(initial) must return the "
            "width the restore left"
        )


#: Single-launch HLE cases whose status is machinery-only: a record is PASS when its calls
#: completed and SKIP when the object it needs was not obtained, and never FAIL.
HLE_MACHINERY_STATUS_CASES = frozenset({
    "hle-vtimer-unit", "hle-vtimer-errors", "hle-kernel-pool-bounds", "hle-power-float-v0",
    "hle-null-out-pointers", "hle-audio-src", "hle-timer-order",
})
VFPU_RAW_WORD_CASES = frozenset({"vfpu-transcendental-corpus"})

#: hle-kernel-pool-bounds fixed-pool (create, delete) cell pairs.
HLE_POOL_BOUNDS_FPL_PAIRS = (
    ("fpl-create-size-0x00", "fpl-delete-size-0x00"),
    ("fpl-create-size-0x7fffffff", "fpl-delete-size-0x7fffffff"),
)


def validate_hle_pool_bounds_deletes(sequence: SequenceReport) -> None:
    """Fail closed unless each fixed-pool delete cell agrees with its own create cell.

    A refused create is what these creates measure, so the delete cell after it is PASS,
    not SKIP: its out0 is 1 when it issued a delete (the create's out0 says a pool was
    created) and 0 when there was no pool, in which case it made no call and its result
    is 0. This checks the probe's own bookkeeping, like validate_hle_edram_restore; it
    compares no value with a PSP expectation. A pair with a record still missing from a
    partial stream is not checked.
    """

    results = sequence.results
    test_id = sequence.spec.test_id
    for create_id, delete_id in HLE_POOL_BOUNDS_FPL_PAIRS:
        if create_id not in results or delete_id not in results:
            continue
        delete = results[delete_id]
        if delete.status != "PASS":
            raise ProtocolError(
                f"{test_id}: {delete_id} status {delete.status!r}; a fixed-pool delete cell is "
                "PASS whether or not its create returned a pool"
            )
        created = int(dict(results[create_id].values)["out0"], 0)
        values = dict(delete.values)
        issued = int(values["out0"], 0)
        if issued not in (0, 1) or issued != created:
            raise ProtocolError(
                f"{test_id}: {delete_id} out0 {issued:#x} must equal {create_id} out0 "
                f"{created:#x} (1 when a pool was created and deleted, 0 when there was none)"
            )
        if issued == 0 and int(values["result"], 0) != 0:
            raise ProtocolError(
                f"{test_id}: {delete_id} recorded no pool (out0 0) but a non-zero result; "
                "with no pool the cell makes no call"
            )


def parse_campaign_probe_output(
    text: str, campaign_case: str, *, require_complete: bool = True
) -> SequenceReport:
    """Parse one fixed-size campaign stream without asserting PSP outcomes."""

    try:
        spec, counts = CAMPAIGN_PROBE_CASES[campaign_case]
    except KeyError as exc:
        raise ProtocolError(f"unknown campaign probe {campaign_case!r}") from exc
    sequence = parse_sequence(text, spec, require_complete=require_complete)
    _validate_scalar_shape(sequence, counts)
    if campaign_case in HLE_MACHINERY_STATUS_CASES:
        for record in sequence.results.values():
            if record.status not in ("PASS", "SKIP"):
                raise ProtocolError(
                    f"{spec.test_id}: {record.case_id} status {record.status!r} is not "
                    "machinery-only (a cell is PASS or SKIP, never FAIL)"
                )
    if campaign_case == "hle-kernel-pool-bounds":
        validate_hle_pool_bounds_deletes(sequence)
    if campaign_case in VFPU_RAW_WORD_CASES:
        for record in sequence.results.values():
            if record.status != "PASS":
                raise ProtocolError(
                    f"{spec.test_id}: {record.case_id} must carry a produced raw-word record"
                )
    return sequence


REGISTRY_MODELED_KEYS = frozenset({
    "language", "button_assign", "date_format", "time_format", "timezone",
    "summer_time", "adhoc_channel",
})
REGISTRY_TEST_ID = "PSP-REGISTRY-001"
REGISTRY_HOST0_LOG = "host0:/registry_readonly_log.txt"


@dataclass(frozen=True)
class RegistryReadOnlyReport:
    parsed: object
    complete: bool
    categories: tuple[str, ...]
    keys: tuple[str, ...]


def parse_registry_readonly_output(
    text: str, *, require_complete: bool = True
) -> RegistryReadOnlyReport:
    """Validate the ordered, dynamically sized, read-only registry census."""

    parsed = parse_output(text)
    records = [record for record in parsed.results if record.test_id == REGISTRY_TEST_ID]
    if len(records) != len(parsed.results):
        raise ProtocolError(f"{REGISTRY_TEST_ID}: foreign test_id in registry stream")
    if not records:
        raise ProtocolError(f"{REGISTRY_TEST_ID}: registry stream has no records")

    expected_fixed = ("registry-open", "registry-errors")
    if tuple(record.case_id for record in records[:min(len(records), 2)]) != \
            expected_fixed[:min(len(records), 2)]:
        raise ProtocolError(f"{REGISTRY_TEST_ID}: stream must start with {list(expected_fixed)}")
    if len(records) > 0:
        _validate_registry_fields(records[0], {"result", "out0", "out1"})
    if len(records) > 1:
        _validate_registry_fields(
            records[1], {
                "result", "out0", "out1", "out2", "out3", "out4", "out5", "out6"
            }
        )

    categories: list[str] = []
    keys: list[str] = []
    terminal = records[-1].case_id == "registry-done"
    body = records[2: -1 if terminal else None]
    for index, record in enumerate(body):
        values = dict(record.values)
        if record.case_id == "registry-bad-handle":
            # The forged-handle call runs last so a fault there cannot hide
            # the census; nothing but registry-done may follow it.
            if index != len(body) - 1:
                raise ProtocolError(
                    f"{REGISTRY_TEST_ID}: registry-bad-handle must follow the census"
                )
            _validate_registry_fields(record, {"result", "out0", "out1"})
        elif record.case_id.startswith("registry-category-"):
            if set(values) != {"result", "out0", "out1", "detail"}:
                raise ProtocolError(f"{REGISTRY_TEST_ID}: {record.case_id} has invalid fields")
            _validate_registry_name(values["detail"], category=True)
            categories.append(values["detail"])
        elif record.case_id.startswith("registry-key-"):
            common = {"result", "out0", "out1", "out2", "out3", "detail"}
            if set(values) not in (common, common | {"value_hex"}):
                raise ProtocolError(f"{REGISTRY_TEST_ID}: {record.case_id} has invalid fields")
            _validate_registry_name(values["detail"], category=False)
            name = values["detail"].rsplit("/", 1)[-1]
            has_value = int(values["out2"], 0) == 1
            if name in REGISTRY_MODELED_KEYS:
                if has_value != ("value_hex" in values):
                    raise ProtocolError(
                        f"{REGISTRY_TEST_ID}: modeled key value presence disagrees for {name}"
                    )
            elif "value_hex" in values or has_value:
                raise ProtocolError(
                    f"{REGISTRY_TEST_ID}: non-modeled key value must not be recorded"
                )
            if "value_hex" in values and not re.fullmatch(r"(?:[0-9a-fA-F]{2})*", values["value_hex"]):
                raise ProtocolError(f"{REGISTRY_TEST_ID}: value_hex is not byte encoded")
            keys.append(values["detail"])
        else:
            raise ProtocolError(f"{REGISTRY_TEST_ID}: unexpected record {record.case_id!r}")

    if terminal:
        if len(records) < 2 or records[-2].case_id != "registry-bad-handle":
            raise ProtocolError(
                f"{REGISTRY_TEST_ID}: registry-bad-handle must precede registry-done"
            )
        done = records[-1]
        _validate_registry_fields(done, {"result", "out0", "out1", "out2"})
        done_values = {key: int(value, 0) for key, value in done.values if key != "detail"}
        if done_values["out0"] != len(categories) or done_values["out1"] != len(keys) or \
                done_values["out2"] != len(records) - 1:
            raise ProtocolError(f"{REGISTRY_TEST_ID}: registry completion counts disagree")
    elif require_complete:
        raise ProtocolError(f"{REGISTRY_TEST_ID}: registry stream is missing registry-done")

    ordered = [record.case_id for record in records]
    if ordered[0:2] != list(expected_fixed):
        raise ProtocolError(f"{REGISTRY_TEST_ID}: registry header order is invalid")
    if terminal and ordered[-1] != "registry-done":
        raise ProtocolError(f"{REGISTRY_TEST_ID}: registry-done must be last")
    if len(set(ordered)) != len(ordered):
        raise ProtocolError(f"{REGISTRY_TEST_ID}: duplicate record case_id")
    return RegistryReadOnlyReport(
        parsed=parsed,
        complete=terminal,
        categories=tuple(categories),
        keys=tuple(keys),
    )


def _validate_registry_fields(record, expected: set[str]) -> None:
    values = dict(record.values)
    if set(values) != expected:
        raise ProtocolError(
            f"{REGISTRY_TEST_ID}: {record.case_id} fields must be {sorted(expected)}, "
            f"got {sorted(values)}"
        )
    for field, value in values.items():
        if field != "detail":
            try:
                int(value, 0)
            except ValueError as exc:
                raise ProtocolError(
                    f"{REGISTRY_TEST_ID}: {record.case_id} {field} is not an integer"
                ) from exc


def _validate_registry_name(value: str, *, category: bool) -> None:
    parts = value.split("/")
    if not value or value.startswith("/") or ".." in parts or any(
        not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", part) for part in parts
    ):
        raise ProtocolError(f"{REGISTRY_TEST_ID}: unsafe registry name {value!r}")
    if category and value != "CONFIG" and not value.startswith("CONFIG/"):
        raise ProtocolError(f"{REGISTRY_TEST_ID}: category is outside /CONFIG: {value!r}")


# ---------------------------------------------------------------------------
# PSP-REGISTRY-002 read-only font-registry walk (fixtures/psp_oracle/registry_walk.h)
#
# The stream is font-registry-open, then font-category/font-key records in the
# walk's enumeration order, then font-registry-done. The parser re-checks the
# walk's safety contract on the evidence itself: a category appears only right
# after the key record that listed it as an on-route directory with a name
# that round-trips, the root is never opened (it is listed through the registry
# handle, and its children are opened only when that listing showed /CONFIG as
# a directory), values appear only inside /DATA/FONT and
# /SYSPROFILE/RESOLUTION, and every listed key is accounted for in order. It
# does not decide PSP values.
# ---------------------------------------------------------------------------

REGISTRY_FONT_TEST_ID = "PSP-REGISTRY-002"
REGISTRY_FONT_HOST0_LOG = "host0:/registry_font_log.txt"
REGISTRY_FONT_ROUTES = (("DATA", "FONT"), ("SYSPROFILE", "RESOLUTION"))
REGISTRY_FONT_NAME_MAX = 26
REGISTRY_FONT_KEYS_MAX = 64
REGISTRY_FONT_DEPTH_MAX = 6
REGISTRY_FONT_VALUE_MAX = 1024
REGISTRY_FONT_ERROR = 0xFFFFFFFF
REGISTRY_FONT_TYPE_DIR = 1
REGISTRY_FONT_KEY_METADATA = 0
REGISTRY_FONT_KEY_VALUE = 1
REGISTRY_FONT_KEY_OPENED = 2
REGISTRY_FONT_KEY_OFF_ROUTE = 3
REGISTRY_FONT_KEY_NAME_REFUSED = 4
REGISTRY_FONT_KEY_DEPTH_LIMIT = 5
REGISTRY_FONT_KEY_OPEN_LIMIT = 6
REGISTRY_FONT_KEY_VALUE_SIZE = 7
REGISTRY_FONT_KEY_INFO_FAILED = 8
REGISTRY_FONT_KEY_NAME_UNSAFE = 9
REGISTRY_FONT_KEY_ROOT_UNVERIFIED = 10
REGISTRY_FONT_ROOT_ANCHOR = "/CONFIG"
_REGISTRY_FONT_REFUSALS = frozenset({
    REGISTRY_FONT_KEY_NAME_REFUSED, REGISTRY_FONT_KEY_DEPTH_LIMIT, REGISTRY_FONT_KEY_OPEN_LIMIT,
    REGISTRY_FONT_KEY_ROOT_UNVERIFIED,
})
_REGISTRY_FONT_NAME_RE = re.compile(r"[A-Za-z0-9_.-]{1,26}")


@dataclass(frozen=True)
class RegistryFontKey:
    path: str
    index: int
    type: int
    size: int
    disposition: int
    value: bytes | None


@dataclass(frozen=True)
class RegistryFontReport:
    parsed: object
    complete: bool
    registry_opened: bool
    root_enumerated: bool
    root_verified: bool
    categories: tuple[str, ...]
    opened: tuple[str, ...]
    keys: tuple[RegistryFontKey, ...]


def _font_components(path: str) -> list[str]:
    if path == "/":
        return []
    if not path.startswith("/") or path.endswith("/"):
        raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: malformed registry path {path!r}")
    parts = path[1:].split("/")
    if any(not _REGISTRY_FONT_NAME_RE.fullmatch(part) for part in parts):
        raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: unsafe registry path {path!r}")
    return parts


def _font_on_route(parts: list[str]) -> bool:
    """Whether a directory at ``parts`` may be entered: a route prefix or below a route."""

    span = min(len(parts), 2)
    return bool(parts) and any(tuple(parts[:span]) == route[:span] for route in REGISTRY_FONT_ROUTES)


def _font_value_subtree(parent: list[str]) -> bool:
    """Whether keys of the directory ``parent`` are inside a value subtree."""

    return len(parent) >= 2 and tuple(parent[:2]) in REGISTRY_FONT_ROUTES


def _font_reopenable(name: str) -> bool:
    return 0 < len(name) < REGISTRY_FONT_NAME_MAX and not name.startswith("__NAKAGAWA")


def _font_fields(record, expected: set[str]) -> dict[str, int | str]:
    values = dict(record.values)
    if set(values) != expected:
        raise ProtocolError(
            f"{REGISTRY_FONT_TEST_ID}: {record.case_id} fields must be {sorted(expected)}, "
            f"got {sorted(values)}"
        )
    out: dict[str, int | str] = {}
    for field, value in values.items():
        if field in {"detail", "value_hex", "name_hex"}:
            if field != "detail" and not re.fullmatch(r"(?:[0-9a-f]{2})*", value):
                raise ProtocolError(
                    f"{REGISTRY_FONT_TEST_ID}: {record.case_id} {field} is not byte encoded"
                )
            out[field] = value
            continue
        try:
            out[field] = int(value, 0)
        except ValueError as exc:
            raise ProtocolError(
                f"{REGISTRY_FONT_TEST_ID}: {record.case_id} {field} is not an integer"
            ) from exc
    return out


def parse_registry_font_output(
    text: str, *, require_complete: bool = True
) -> RegistryFontReport:
    """Validate the read-only font-registry walk and its safety contract."""

    parsed = parse_output(text)
    records = list(parsed.results)
    if any(record.test_id != REGISTRY_FONT_TEST_ID for record in records):
        raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: foreign test_id in font-registry stream")
    if records[0].case_id != "font-registry-open":
        raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: stream must start with font-registry-open")
    opening = _font_fields(records[0], {"result", "out0", "out1"})
    registry_opened = opening["result"] == 0
    if (records[0].status == "PASS") != registry_opened:
        raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: font-registry-open status disagrees")
    terminal = records[-1].case_id == "font-registry-done" and len(records) > 1
    body = records[1:-1] if terminal else records[1:]
    if not registry_opened and body:
        raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: records after a failed registry open")

    categories: list[str] = []
    opened: list[str] = []
    keys: list[RegistryFontKey] = []
    stack: list[list] = []  # [path, count, next index]
    pending_open: str | None = None
    root_enumerated = False
    for record in body:
        if record.case_id.startswith("font-category-"):
            if record.case_id != f"font-category-{len(categories):04d}":
                raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: {record.case_id} is out of sequence")
            values = _font_fields(record, {"result", "out0", "out1", "out2", "out3", "detail"})
            path = str(values["detail"])
            parts = _font_components(path)
            usable = (values["result"] == 0 and values["out0"] == 0
                      and values["out1"] <= REGISTRY_FONT_KEYS_MAX
                      and (values["out1"] == 0 or values["out2"] == 0))
            if (record.status == "PASS") != usable:
                raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: {record.case_id} status disagrees")
            if values["out3"] != len(parts):
                raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: {record.case_id} depth disagrees")
            if not categories:
                if path != "/" or values["result"] != 0:
                    raise ProtocolError(
                        f"{REGISTRY_FONT_TEST_ID}: the first category must be the root listing"
                    )
                root_enumerated = usable
            else:
                # Safety: a category exists in the evidence only because the key record just
                # before it listed it as an on-route directory whose name round-trips.
                if pending_open != path:
                    raise ProtocolError(
                        f"{REGISTRY_FONT_TEST_ID}: {path} was opened without being listed "
                        "as an on-route directory just before"
                    )
                if (not _font_on_route(parts) or len(parts) > REGISTRY_FONT_DEPTH_MAX
                        or not all(_font_reopenable(part) for part in parts)):
                    raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: {path} must never be opened")
                if values["result"] == 0:
                    opened.append(path)
            pending_open = None
            categories.append(path)
            if usable and values["out1"] > 0:
                stack.append([path, values["out1"], 0])
            continue

        if not record.case_id.startswith("font-key-"):
            raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: unexpected record {record.case_id!r}")
        if pending_open is not None:
            raise ProtocolError(
                f"{REGISTRY_FONT_TEST_ID}: {pending_open} was listed as opened but has no category"
            )
        if record.case_id != f"font-key-{len(keys):04d}":
            raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: {record.case_id} is out of sequence")
        base = {"result", "out0", "out1", "out2", "out3", "out4", "detail"}
        raw = dict(record.values)
        extra = set(raw) - base
        if extra not in (set(), {"value_hex"}, {"name_hex"}):
            raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: {record.case_id} has invalid fields")
        values = _font_fields(record, base | extra)
        while stack and stack[-1][2] >= stack[-1][1]:
            stack.pop()
        if not stack:
            raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: {record.case_id} has no listing parent")
        parent_path, _count, index = stack[-1]
        if values["out2"] != index:
            raise ProtocolError(
                f"{REGISTRY_FONT_TEST_ID}: {record.case_id} breaks the enumeration order"
            )
        stack[-1][2] += 1
        disposition = values["out3"]
        path = str(values["detail"])
        if disposition == REGISTRY_FONT_KEY_NAME_UNSAFE:
            # name_hex is the whole listing slot; a usable name is NUL-terminated inside it.
            slot = bytes.fromhex(str(values.get("name_hex", "")))
            name = slot.split(b"\0", 1)[0] if b"\0" in slot else None
            if ("name_hex" not in values or len(slot) != REGISTRY_FONT_NAME_MAX + 1
                    or path != parent_path or values["result"] != REGISTRY_FONT_ERROR
                    or record.status != "FAIL"
                    or (name is not None
                        and _REGISTRY_FONT_NAME_RE.fullmatch(name.decode("latin-1")))):
                raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: {record.case_id} unsafe-name record")
            keys.append(RegistryFontKey(parent_path, index, REGISTRY_FONT_ERROR,
                                        REGISTRY_FONT_ERROR, disposition, None))
            continue
        if "name_hex" in values:
            raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: {record.case_id} has a stray name_hex")
        parts = _font_components(path)
        parent_parts = _font_components(parent_path)
        if parts[:-1] != parent_parts or not parts:
            raise ProtocolError(
                f"{REGISTRY_FONT_TEST_ID}: {record.case_id} is not a child of {parent_path}"
            )
        is_dir = values["out0"] == REGISTRY_FONT_TYPE_DIR
        in_values = _font_value_subtree(parent_parts)
        value = None
        if "value_hex" in values:
            if disposition != REGISTRY_FONT_KEY_VALUE or values["out4"] != 0 or not in_values:
                raise ProtocolError(
                    f"{REGISTRY_FONT_TEST_ID}: {record.case_id} carries a value it must not have"
                )
            value = bytes.fromhex(str(values["value_hex"]))
            if len(value) != values["out1"]:
                raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: {record.case_id} value size disagrees")
        ok = values["result"] == 0
        if (record.status == "PASS") != ok:
            raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: {record.case_id} status disagrees")
        if disposition == REGISTRY_FONT_KEY_INFO_FAILED:
            consistent = not ok
        elif not ok:
            consistent = False
        elif disposition == REGISTRY_FONT_KEY_METADATA:
            consistent = not is_dir and not in_values and values["out4"] == REGISTRY_FONT_ERROR
        elif disposition == REGISTRY_FONT_KEY_VALUE:
            consistent = (not is_dir and in_values
                          and 0 < values["out1"] <= REGISTRY_FONT_VALUE_MAX
                          and (values["out4"] == 0) == (value is not None))
        elif disposition == REGISTRY_FONT_KEY_VALUE_SIZE:
            consistent = (not is_dir and in_values and values["out4"] == REGISTRY_FONT_ERROR
                          and not 0 < values["out1"] <= REGISTRY_FONT_VALUE_MAX)
        elif disposition == REGISTRY_FONT_KEY_OPENED:
            consistent = (is_dir and _font_on_route(parts) and _font_reopenable(parts[-1])
                          and len(parts) <= REGISTRY_FONT_DEPTH_MAX)
            pending_open = path
        elif disposition == REGISTRY_FONT_KEY_OFF_ROUTE:
            consistent = is_dir and not _font_on_route(parts)
        elif disposition == REGISTRY_FONT_KEY_NAME_REFUSED:
            consistent = is_dir and _font_on_route(parts) and not _font_reopenable(parts[-1])
        elif disposition in (REGISTRY_FONT_KEY_DEPTH_LIMIT, REGISTRY_FONT_KEY_OPEN_LIMIT):
            consistent = is_dir and _font_on_route(parts)
        elif disposition == REGISTRY_FONT_KEY_ROOT_UNVERIFIED:
            consistent = is_dir and _font_on_route(parts) and len(parts) == 1
        else:
            consistent = False
        if not consistent:
            raise ProtocolError(
                f"{REGISTRY_FONT_TEST_ID}: {record.case_id} disposition {disposition} "
                "disagrees with its key"
            )
        keys.append(RegistryFontKey(path, index, values["out0"], values["out1"],
                                    disposition, value))

    if pending_open is not None:
        raise ProtocolError(
            f"{REGISTRY_FONT_TEST_ID}: {pending_open} was listed as opened but has no category"
        )
    anchor_listed = any(key.path == REGISTRY_FONT_ROOT_ANCHOR
                        and key.type == REGISTRY_FONT_TYPE_DIR for key in keys)
    root_opened = any(key.disposition == REGISTRY_FONT_KEY_OPENED
                      and key.path.count("/") == 1 for key in keys)
    root_refused = any(key.disposition == REGISTRY_FONT_KEY_ROOT_UNVERIFIED for key in keys)
    if root_opened and not anchor_listed:
        raise ProtocolError(
            f"{REGISTRY_FONT_TEST_ID}: a root category was opened although the listing did not "
            f"show {REGISTRY_FONT_ROOT_ANCHOR} as a directory"
        )
    root_verified = False
    if terminal:
        done = _font_fields(records[-1], {
            "result", "out0", "out1", "out2", "out3", "out4", "out5", "out6",
        })
        root_verified = done["out6"] == 1
        if (done["out6"] not in (0, 1)
                or (root_verified and (root_refused or not anchor_listed))
                or (not root_verified and root_opened)):
            raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: font-registry-done root check disagrees")
        while stack and stack[-1][2] >= stack[-1][1]:
            stack.pop()
        if stack:
            raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: listed keys are missing before done")
        refused = sum(1 for key in keys if key.disposition in _REGISTRY_FONT_REFUSALS)
        expected = {
            "out0": len(categories), "out1": len(keys), "out2": len(records) - 1,
            "out3": len(opened), "out4": refused, "out5": int(root_enumerated),
        }
        if any(done[field] != value for field, value in expected.items()):
            raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: font-registry-done counts disagree")
        if not registry_opened and done["result"] != REGISTRY_FONT_ERROR:
            raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: registry closed without being opened")
        if registry_opened and not categories:
            raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: the root listing record is missing")
    elif require_complete:
        raise ProtocolError(f"{REGISTRY_FONT_TEST_ID}: stream is missing font-registry-done")
    return RegistryFontReport(
        parsed=parsed,
        complete=terminal,
        registry_opened=registry_opened,
        root_enumerated=root_enumerated,
        root_verified=root_verified,
        categories=tuple(categories),
        opened=tuple(opened),
        keys=tuple(keys),
    )


def _s32(value: int) -> int:
    return value - (1 << 32) if value & 0x80000000 else value


# ---------------------------------------------------------------------------
# PSP-FONTFILES-001 read-only copy of flash0:/font (fixtures/psp_oracle/font_files.h)
#
# font-dir-open, then (when the listing opened) font-mkdir, font-entry-NNNN in
# listing order, font-dir-close, one font-copy-NNNN per copy candidate, and
# font-files-done. The parser checks the order, that only regular files with
# safe names inside the size bound were copied, and that a copy passes only
# when its three digests and byte counts agree. The copies and digests are
# private evidence; this parser never decides what a console should hold.
# ---------------------------------------------------------------------------

FONT_FILES_TEST_ID = "PSP-FONTFILES-001"
FONT_FILES_HOST0_LOG = "host0:/font_files_log.txt"
FONT_FILES_ENTRIES_MAX = 64
FONT_FILES_FILE_MAX = 16 * 1024 * 1024
FONT_FILES_MATCH_ALL = 7
_FONT_FILES_NAME_RE = re.compile(r"[A-Za-z0-9_-][A-Za-z0-9_.-]{0,62}")
_SHA256_TEXT_RE = re.compile(r"[0-9a-f]{64}")


@dataclass(frozen=True)
class FontFileEntry:
    name: str | None
    name_hex: str | None
    mode: int
    attr: int
    size: int
    disposition: int


@dataclass(frozen=True)
class FontFileCopy:
    name: str
    size: int
    sha256: str
    readback_sha256: str
    reread_sha256: str
    match: int
    verified: bool


@dataclass(frozen=True)
class FontFilesReport:
    parsed: object
    complete: bool
    listed: bool
    entries: tuple[FontFileEntry, ...]
    copies: tuple[FontFileCopy, ...]


def _io_fields(test_id: str, record, expected: set[str], text: set[str]) -> dict[str, object]:
    values = dict(record.values)
    if set(values) != expected:
        raise ProtocolError(
            f"{test_id}: {record.case_id} fields must be {sorted(expected)}, got {sorted(values)}"
        )
    out: dict[str, object] = {}
    for field, value in values.items():
        if field in text:
            out[field] = value
            continue
        try:
            out[field] = int(value, 0)
        except ValueError as exc:
            raise ProtocolError(f"{test_id}: {record.case_id} {field} is not an integer") from exc
    return out


def parse_font_files_output(text: str, *, require_complete: bool = True) -> FontFilesReport:
    """Validate the flash0:/font listing-and-copy stream."""

    tid = FONT_FILES_TEST_ID
    parsed = parse_output(text)
    records = list(parsed.results)
    if any(record.test_id != tid for record in records):
        raise ProtocolError(f"{tid}: foreign test_id in font-files stream")
    if records[0].case_id != "font-dir-open":
        raise ProtocolError(f"{tid}: stream must start with font-dir-open")
    opening = _io_fields(tid, records[0], {"result", "out0", "out1"}, set())
    listed = _s32(opening["result"]) >= 0
    if (records[0].status == "PASS") != listed:
        raise ProtocolError(f"{tid}: font-dir-open status disagrees")
    terminal = len(records) > 1 and records[-1].case_id == "font-files-done"
    body = records[1:-1] if terminal else records[1:]
    entries: list[FontFileEntry] = []
    copies: list[FontFileCopy] = []
    candidates: list[FontFileEntry] = []
    closed = False
    if not listed and body:
        raise ProtocolError(f"{tid}: records after a listing that did not open")
    if body:
        if body[0].case_id != "font-mkdir":
            raise ProtocolError(f"{tid}: font-mkdir must follow an opened listing")
        _io_fields(tid, body[0], {"result"}, set())
    for record in body[1:]:
        if record.case_id.startswith("font-entry-"):
            if closed or record.case_id != f"font-entry-{len(entries):04d}":
                raise ProtocolError(f"{tid}: {record.case_id} is out of sequence")
            raw = dict(record.values)
            name_field = {"detail"} if "detail" in raw else {"name_hex"}
            values = _io_fields(tid, record, {"result", "out0", "out1", "out2", "out3", "out4"}
                                | name_field, {"detail", "name_hex"})
            name = values.get("detail")
            name_hex = values.get("name_hex")
            if name is not None and not _FONT_FILES_NAME_RE.fullmatch(str(name)):
                raise ProtocolError(f"{tid}: {record.case_id} prints an unsafe name")
            if name_hex is not None and not re.fullmatch(r"(?:[0-9a-f]{2}){1,64}", str(name_hex)):
                raise ProtocolError(f"{tid}: {record.case_id} name_hex is not byte encoded")
            size = values["out2"] | (values["out3"] << 32)
            if size & (1 << 63):
                size -= 1 << 64
            regular = (values["out0"] & 0xF000) == 0x2000
            disposition = values["out4"]
            consistent = {
                0: regular and name is not None and 0 <= size <= FONT_FILES_FILE_MAX,
                1: not regular,
                2: regular and name is None,
                3: regular and name is not None and not 0 <= size <= FONT_FILES_FILE_MAX,
            }.get(disposition, False)
            if not consistent or values["result"] <= 0 or record.status != "PASS":
                raise ProtocolError(f"{tid}: {record.case_id} disposition {disposition} disagrees")
            entry = FontFileEntry(None if name is None else str(name),
                                  None if name_hex is None else str(name_hex),
                                  values["out0"], values["out1"], size, disposition)
            entries.append(entry)
            if disposition == 0:
                candidates.append(entry)
        elif record.case_id == "font-dir-close":
            if closed:
                raise ProtocolError(f"{tid}: font-dir-close repeats")
            values = _io_fields(tid, record, {"result", "out0", "out1"}, set())
            if values["out0"] != len(entries):
                raise ProtocolError(f"{tid}: font-dir-close entry count disagrees")
            if values["out1"] == 0xFFFFFFFF and len(entries) != FONT_FILES_ENTRIES_MAX:
                raise ProtocolError(f"{tid}: font-dir-close end result disagrees")
            if (record.status == "PASS") != (values["result"] == 0 and values["out1"] == 0):
                raise ProtocolError(f"{tid}: font-dir-close status disagrees")
            closed = True
        elif record.case_id.startswith("font-copy-"):
            index = len(copies)
            if not closed or record.case_id != f"font-copy-{index:04d}" or index >= len(candidates):
                raise ProtocolError(f"{tid}: {record.case_id} is out of sequence")
            values = _io_fields(
                tid, record,
                {"result", *(f"out{i}" for i in range(9)), "detail", "sha256",
                 "readback_sha256", "reread_sha256"},
                {"detail", "sha256", "readback_sha256", "reread_sha256"},
            )
            entry = candidates[index]
            if values["detail"] != entry.name:
                raise ProtocolError(f"{tid}: {record.case_id} copies a file it did not list")
            digests = [str(values[field]) for field in ("sha256", "readback_sha256", "reread_sha256")]
            if not all(_SHA256_TEXT_RE.fullmatch(digest) for digest in digests):
                raise ProtocolError(f"{tid}: {record.case_id} digests are malformed")
            match = values["out8"]
            expected = 0
            read_a = _s32(values["out1"])
            if read_a >= 0 and values["out3"] == values["out1"] and values["out5"] == values["out1"] \
                    and digests[0] == digests[1]:
                expected |= 1
            if read_a >= 0 and values["out7"] == values["out1"] and digests[0] == digests[2]:
                expected |= 2
            if read_a >= 0 and read_a == entry.size:
                expected |= 4
            # A failed write leaves fewer bytes written than read, so the counts decide bit 0 too.
            if match != expected:
                raise ProtocolError(f"{tid}: {record.case_id} match bits disagree with its counts")
            verified = match == FONT_FILES_MATCH_ALL
            if verified != (values["result"] == 0) or (record.status == "PASS") != verified:
                raise ProtocolError(f"{tid}: {record.case_id} status disagrees with its checks")
            copies.append(FontFileCopy(entry.name or "", entry.size, digests[0], digests[1],
                                       digests[2], match, verified))
        else:
            raise ProtocolError(f"{tid}: unexpected record {record.case_id!r}")
    if terminal:
        if listed and (not closed or len(copies) != len(candidates)):
            raise ProtocolError(f"{tid}: listing or copies incomplete before font-files-done")
        done = _io_fields(tid, records[-1], {"result", "out0", "out1", "out2", "out3"}, set())
        expected_done = {
            "out0": len(entries), "out1": len(candidates),
            "out2": sum(1 for copy in copies if copy.verified), "out3": len(records) - 1,
        }
        if any(done[field] != value for field, value in expected_done.items()):
            raise ProtocolError(f"{tid}: font-files-done counts disagree")
    elif require_complete:
        raise ProtocolError(f"{tid}: stream is missing font-files-done")
    return FontFilesReport(parsed, terminal, listed, tuple(entries), tuple(copies))


# ---------------------------------------------------------------------------
# PSP-FONTLIST-001 the system font list as the console's own font library reports
# it (fixtures/psp_oracle/font_list.h)
#
# font-list-newlib, then (when the library opened) font-list-count, and when the
# count is positive font-list-list, font-list-entry-NNNN for every requested
# index, font-info-NNNN for the same indices, then font-list-donelib and
# font-list-done. Entries carry the raw style bytes as raw_hex with words 0-7
# and 40-41 as out0-out9. The parser checks the order, the field sets, each
# record's own consistency and the counts; it never decides what fonts a console
# should hold. Names inside the entries are console strings and stay private.
# ---------------------------------------------------------------------------

FONT_LIST_TEST_ID = "PSP-FONTLIST-001"
FONT_LIST_HOST0_LOG = "host0:/font_list_log.txt"
FONT_LIST_ENTRIES_MAX = 64
FONT_LIST_STYLE_BYTES = 168
FONT_LIST_UNTOUCHED = 0xA5A5A5A5
_FONT_LIST_WORD_FIELDS = frozenset(f"out{i}" for i in range(10))
_FONT_LIST_RAW_RE = re.compile(r"(?:[0-9a-f]{2}){168}")


@dataclass(frozen=True)
class FontListEntry:
    index: int
    words: tuple[int, ...]
    raw: bytes
    ok: bool
    result: int


@dataclass(frozen=True)
class FontListReport:
    parsed: object
    complete: bool
    library: int
    library_ok: bool
    count: int | None
    requested: int
    entries: tuple[FontListEntry, ...]
    infos: tuple[FontListEntry, ...]
    failures: int | None


def _font_list_error_clean(error: int) -> bool:
    """An error word the library wrote as 0, or never wrote (the probe's untouched marker)."""
    return error in (0, FONT_LIST_UNTOUCHED)


def _font_list_entry(tid: str, record, prefix: str, index: int) -> FontListEntry:
    expected_id = f"{prefix}{index:04d}"
    if record.case_id != expected_id:
        raise ProtocolError(f"{tid}: {record.case_id} is out of sequence; expected {expected_id}")
    values = _io_fields(tid, record, {"result", "raw_hex", *_FONT_LIST_WORD_FIELDS}, {"raw_hex"})
    raw_hex = str(values["raw_hex"])
    if not _FONT_LIST_RAW_RE.fullmatch(raw_hex):
        raise ProtocolError(f"{tid}: {record.case_id} raw_hex is not {FONT_LIST_STYLE_BYTES} bytes")
    raw = bytes.fromhex(raw_hex)
    words = tuple(values[f"out{i}"] for i in range(10))
    expected = tuple(int.from_bytes(raw[4 * i:4 * i + 4], "little") for i in range(8)) + (
        int.from_bytes(raw[0xA0:0xA4], "little"), int.from_bytes(raw[0xA4:0xA8], "little"))
    if words != expected:
        raise ProtocolError(f"{tid}: {record.case_id} out words disagree with raw_hex")
    return FontListEntry(index, words, raw, record.status == "PASS", values["result"])


def parse_font_list_output(text: str, *, require_complete: bool = True) -> FontListReport:
    """Validate the font-library list stream."""

    tid = FONT_LIST_TEST_ID
    parsed = parse_output(text)
    records = list(parsed.results)
    if not records:
        raise ProtocolError(f"{tid}: empty font-list stream")
    if any(record.test_id != tid for record in records):
        raise ProtocolError(f"{tid}: foreign test_id in font-list stream")
    if records[0].case_id != "font-list-newlib":
        raise ProtocolError(f"{tid}: stream must start with font-list-newlib")
    newlib = _io_fields(tid, records[0], {"result", "out0", "out1", "out2", "out3", "out4"}, set())
    library = newlib["result"]
    library_ok = library != 0 and _font_list_error_clean(newlib["out0"])
    if (records[0].status == "PASS") != library_ok:
        raise ProtocolError(f"{tid}: font-list-newlib status disagrees")
    terminal = len(records) > 1 and records[-1].case_id == "font-list-done"
    body = records[1:-1] if terminal else records[1:]
    failures = 0 if library_ok else 1
    count: int | None = None
    requested = 0
    entries: list[FontListEntry] = []
    infos: list[FontListEntry] = []
    donelib_seen = False
    position = 0
    if library == 0:
        if body:
            raise ProtocolError(f"{tid}: records after a library that did not open")
    else:
        if position < len(body):
            record = body[position]
            position += 1
            if record.case_id != "font-list-count":
                raise ProtocolError(f"{tid}: font-list-count must follow font-list-newlib")
            values = _io_fields(tid, record, {"result", "out0", "out1"}, set())
            count = _s32(values["result"])
            requested = values["out1"]
            count_ok = 0 <= count <= FONT_LIST_ENTRIES_MAX and _font_list_error_clean(values["out0"])
            if (record.status == "PASS") != count_ok:
                raise ProtocolError(f"{tid}: font-list-count status disagrees")
            if requested != min(max(count, 0), FONT_LIST_ENTRIES_MAX):
                raise ProtocolError(f"{tid}: font-list-count requested count disagrees with the count")
            if not count_ok:
                failures += 1
        elif require_complete:
            raise ProtocolError(f"{tid}: stream is missing font-list-count")
        if requested > 0 and position < len(body):
            record = body[position]
            position += 1
            if record.case_id != "font-list-list":
                raise ProtocolError(f"{tid}: font-list-list must follow font-list-count")
            values = _io_fields(tid, record, {"result", "out0"}, set())
            if values["out0"] != requested:
                raise ProtocolError(f"{tid}: font-list-list requested count disagrees")
            list_ok = _s32(values["result"]) >= 0
            if (record.status == "PASS") != list_ok:
                raise ProtocolError(f"{tid}: font-list-list status disagrees")
            if not list_ok:
                failures += 1
            for index in range(requested):
                if position >= len(body):
                    break
                entry = _font_list_entry(tid, body[position], "font-list-entry-", index)
                position += 1
                if entry.ok != list_ok or entry.result != 0:
                    raise ProtocolError(f"{tid}: {body[position - 1].case_id} disagrees with font-list-list")
                entries.append(entry)
            for index in range(requested):
                if position >= len(body):
                    break
                info = _font_list_entry(tid, body[position], "font-info-", index)
                position += 1
                if info.ok != (info.result == 0):
                    raise ProtocolError(f"{tid}: {body[position - 1].case_id} status disagrees")
                if not info.ok:
                    failures += 1
                infos.append(info)
        if position < len(body):
            record = body[position]
            position += 1
            if record.case_id != "font-list-donelib":
                raise ProtocolError(f"{tid}: {record.case_id} is out of sequence; expected font-list-donelib")
            values = _io_fields(tid, record, {"result", "out0", "out1"}, set())
            donelib_ok = values["result"] == 0
            if (record.status == "PASS") != donelib_ok:
                raise ProtocolError(f"{tid}: font-list-donelib status disagrees")
            if not donelib_ok:
                failures += 1
            donelib_seen = True
        if position < len(body):
            raise ProtocolError(f"{tid}: unexpected record {body[position].case_id}")
    done_failures: int | None = None
    if terminal:
        values = _io_fields(tid, records[-1],
                            {"result", "out0", "out1", "out2", "out3", "out4", "out5", "out6"}, set())
        if library != 0 and not donelib_seen:
            raise ProtocolError(f"{tid}: font-list-done before font-list-donelib")
        if len(entries) != requested or len(infos) != requested:
            raise ProtocolError(f"{tid}: entries or infos incomplete before font-list-done")
        if values["out0"] != len(entries) or values["out1"] != sum(1 for info in infos if info.ok):
            raise ProtocolError(f"{tid}: font-list-done counts disagree")
        if values["out6"] != failures:
            raise ProtocolError(f"{tid}: font-list-done failure count disagrees")
        if (records[-1].status == "PASS") != (failures == 0):
            raise ProtocolError(f"{tid}: font-list-done status disagrees")
        done_failures = values["out6"]
    elif require_complete:
        raise ProtocolError(f"{tid}: stream is missing font-list-done")
    return FontListReport(parsed, terminal, library, library_ok, count, requested,
                          tuple(entries), tuple(infos), done_failures)


# ---------------------------------------------------------------------------
# PSP-UMDIO-001 bare-device and disc open results (fixtures/psp_oracle/umd_io.h)
#
# umd-state-before, io-open-NNNN for each fixed open, io-dopen-NNNN for each
# fixed dopen, umd-state-after, umdio-done. The path lists below mirror the
# probe source (a test keeps them equal); the parser records results and never
# decides which answer is correct.
# ---------------------------------------------------------------------------

UMDIO_TEST_ID = "PSP-UMDIO-001"
UMDIO_HOST0_LOG = "host0:/umd_io_log.txt"
UMDIO_OPENS = (
    ("umd0:", 0x1), ("umd1:", 0x1), ("umd:", 0x1), ("umd0:/", 0x1), ("umd1:/", 0x1),
    ("disc0:", 0x1), ("disc0:/", 0x1), ("disc0:/PSP_GAME", 0x1),
    ("disc0:/PSP_GAME/SYSDIR", 0x1), ("disc0:/UMD_DATA.BIN", 0x1),
    ("umd0:", 0xF0001), ("umd1:", 0xF0001), ("msstor0p1:/", 0x1),
)
UMDIO_DOPENS = ("umd0:", "umd1:", "umd:", "disc0:", "disc0:/", "disc0:/PSP_GAME")


@dataclass(frozen=True)
class UmdIoReport:
    parsed: object
    complete: bool
    before: tuple[int, int]
    after: tuple[int, int] | None
    opens: tuple[tuple[str, int, int, int], ...]
    dopens: tuple[tuple[str, int, int], ...]


def parse_umd_io_output(text: str, *, require_complete: bool = True) -> UmdIoReport:
    """Validate the fixed bare-device open/dopen stream."""

    tid = UMDIO_TEST_ID
    parsed = parse_output(text)
    records = list(parsed.results)
    if any(record.test_id != tid for record in records):
        raise ProtocolError(f"{tid}: foreign test_id in umd-io stream")
    expected = (["umd-state-before"]
                + [f"io-open-{i:04d}" for i in range(len(UMDIO_OPENS))]
                + [f"io-dopen-{i:04d}" for i in range(len(UMDIO_DOPENS))]
                + ["umd-state-after", "umdio-done"])
    observed = [record.case_id for record in records]
    if observed != expected[:len(observed)]:
        raise ProtocolError(f"{tid}: records out of order: {observed}")
    complete = len(observed) == len(expected)
    if not complete and require_complete:
        raise ProtocolError(f"{tid}: stream is missing umdio-done")
    before = after = None
    opens: list[tuple[str, int, int, int]] = []
    dopens: list[tuple[str, int, int]] = []
    for record in records:
        if record.status != "PASS":
            raise ProtocolError(f"{tid}: {record.case_id} must be PASS (results are recorded)")
        if record.case_id.startswith("umd-state-"):
            values = _io_fields(tid, record, {"result", "out0"}, set())
            state = (values["result"], values["out0"])
            if record.case_id == "umd-state-before":
                before = state
            else:
                after = state
        elif record.case_id.startswith("io-open-"):
            values = _io_fields(tid, record, {"result", "out0", "out1", "detail"}, {"detail"})
            path, flags = UMDIO_OPENS[len(opens)]
            if values["detail"] != path or values["out0"] != flags:
                raise ProtocolError(f"{tid}: {record.case_id} is not the source's open {path}")
            if (_s32(values["result"]) < 0) != (values["out1"] == 0xFFFFFFFF):
                raise ProtocolError(f"{tid}: {record.case_id} close result disagrees")
            opens.append((path, flags, values["result"], values["out1"]))
        elif record.case_id.startswith("io-dopen-"):
            values = _io_fields(tid, record, {"result", "out0", "detail"}, {"detail"})
            path = UMDIO_DOPENS[len(dopens)]
            if values["detail"] != path:
                raise ProtocolError(f"{tid}: {record.case_id} is not the source's dopen {path}")
            if (_s32(values["result"]) < 0) != (values["out0"] == 0xFFFFFFFF):
                raise ProtocolError(f"{tid}: {record.case_id} close result disagrees")
            dopens.append((path, values["result"], values["out0"]))
        elif record.case_id == "umdio-done":
            values = _io_fields(tid, record, {"result", "out0", "out1", "out2", "out3", "out4"},
                                set())
            counts = {
                "out0": len(UMDIO_OPENS),
                "out1": sum(1 for item in opens if _s32(item[2]) >= 0),
                "out2": len(UMDIO_DOPENS),
                "out3": sum(1 for item in dopens if _s32(item[1]) >= 0),
                "out4": len(records) - 1,
            }
            if any(values[field] != value for field, value in counts.items()):
                raise ProtocolError(f"{tid}: umdio-done counts disagree")
    return UmdIoReport(parsed, complete, before, after, tuple(opens), tuple(dopens))


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
