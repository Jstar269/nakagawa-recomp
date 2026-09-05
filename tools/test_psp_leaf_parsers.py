# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Fail-closed tests for the PSP-IO-001, PSP-AUDIO-001 and PSP-CACHE-001 parsers.

Every mutation family here is generated from the parser's own ``StreamSpec``,
so the number of cases moves with the source rather than with a comment.  The
three probes share one strict-sequence engine, so each family is asserted
against all three specs instead of being written out once per probe.
"""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from psp_oracle.parse_audio_query import (
    SPEC as AUDIO_SPEC,
    parse_audio_query_output,
)
from psp_oracle.parse_cache_alias import (
    SPEC as CACHE_SPEC,
    parse_cache_alias_output,
)
from psp_oracle.parse_io_matrix import (
    SPEC as IO_SPEC,
    parse_io_matrix_output,
)
from psp_oracle.protocol import ProtocolError, StreamSpec

META = (
    "NAKAGAWA_PSP_META schema=1 source=psp model=unknown firmware=unknown "
    "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
    "source_commit=0000000000000000000000000000000000000000 fixture=test\n"
)

# (spec, parser) for every probe that rides the shared strict-sequence engine.
PROBES = (
    (IO_SPEC, parse_io_matrix_output),
    (AUDIO_SPEC, parse_audio_query_output),
    (CACHE_SPEC, parse_cache_alias_output),
)


def line(spec: StreamSpec, case_id: str, status: str = "PASS", *, out0: int | None = None) -> str:
    """One protocol record for ``case_id``, in the exact shape the probe emits."""

    if case_id == spec.terminal_case:
        value = spec.terminal_count if out0 is None else out0
        return (
            f"NAKAGAWA_PSP_TEST schema=1 test_id={spec.test_id} case_id={case_id} "
            f"status={status} result=0x00000000 out0=0x{value:08x}\n"
        )
    return (
        f"NAKAGAWA_PSP_TEST schema=1 test_id={spec.test_id} case_id={case_id} "
        f"status={status} result=0x00000000 out0=0x00000001 out1=0x00000002\n"
    )


def stream(spec: StreamSpec, cases, *, meta: str = META) -> str:
    return meta + "".join(line(spec, case) for case in cases)


def golden(spec: StreamSpec) -> str:
    return stream(spec, spec.ordered_cases)


class TestSpecsMatchProbeSource(unittest.TestCase):
    """The record contract, pinned to what the probe sources actually emit."""

    def test_io_contract(self) -> None:
        self.assertEqual(IO_SPEC.test_id, "PSP-IO-001")
        self.assertEqual(IO_SPEC.terminal_case, "io-done")
        self.assertEqual(IO_SPEC.terminal_count, 6)
        self.assertEqual(IO_SPEC.record_count, 7)
        self.assertEqual(
            IO_SPEC.semantic_cases,
            (
                "io-open-create",
                "io-write",
                "io-read-verify",
                "io-lseek",
                "io-append",
                "io-errors",
            ),
        )

    def test_audio_contract(self) -> None:
        self.assertEqual(AUDIO_SPEC.test_id, "PSP-AUDIO-001")
        self.assertEqual(AUDIO_SPEC.terminal_case, "audio-done")
        self.assertEqual(AUDIO_SPEC.terminal_count, 4)
        self.assertEqual(AUDIO_SPEC.record_count, 5)
        self.assertEqual(
            AUDIO_SPEC.semantic_cases,
            (
                "audio-ch-reserve",
                "audio-ch-release",
                "audio-out2-query",
                "audio-src-reserve",
            ),
        )

    def test_cache_contract(self) -> None:
        self.assertEqual(CACHE_SPEC.test_id, "PSP-CACHE-001")
        self.assertEqual(CACHE_SPEC.terminal_case, "cache-done")
        self.assertEqual(CACHE_SPEC.terminal_count, 4)
        self.assertEqual(CACHE_SPEC.record_count, 5)
        self.assertEqual(
            CACHE_SPEC.semantic_cases,
            (
                "cache-alias-init",
                "cache-writeback-contrast",
                "cache-inval-contrast",
                "cache-wball",
            ),
        )

    def test_terminal_is_never_counted_as_semantic(self) -> None:
        for spec, _ in PROBES:
            with self.subTest(spec.test_id):
                self.assertNotIn(spec.terminal_case, spec.semantic_cases)
                self.assertEqual(spec.record_count, spec.terminal_count + 1)


class TestGoldenStreams(unittest.TestCase):
    def test_golden_passes_in_both_modes(self) -> None:
        for spec, parse in PROBES:
            for strict in (True, False):
                with self.subTest(spec.test_id, strict=strict):
                    report = parse(golden(spec), require_complete=strict)
                    self.assertTrue(report.complete)
                    self.assertTrue(report.all_passed)
                    self.assertTrue(report.terminal_present)
                    self.assertEqual(report.record_count, spec.record_count)

    def test_observables_are_read_from_the_named_fields(self) -> None:
        io_report = parse_io_matrix_output(golden(IO_SPEC))
        self.assertEqual(io_report.open_fd, 1)
        audio_report = parse_audio_query_output(golden(AUDIO_SPEC))
        self.assertEqual(audio_report.channel_reserve_rc, 1)
        self.assertEqual(audio_report.channel_rest_len, 2)
        cache_report = parse_cache_alias_output(golden(CACHE_SPEC))
        self.assertEqual(cache_report.stale_cached_read, 1)
        self.assertEqual(cache_report.fresh_cached_read, 2)

    def test_a_single_fail_cell_is_complete_but_not_passed(self) -> None:
        for spec, parse in PROBES:
            for case in spec.semantic_cases:
                with self.subTest(spec.test_id, case=case):
                    text = META + "".join(
                        line(spec, c, "FAIL" if c == case else "PASS")
                        for c in spec.ordered_cases
                    )
                    report = parse(text)
                    self.assertTrue(report.complete)
                    self.assertFalse(report.all_passed)


class TestTruncationNeverPasses(unittest.TestCase):
    """No prefix of a stream may ever report a semantic pass."""

    def test_every_ordinary_truncation_boundary(self) -> None:
        for spec, parse in PROBES:
            for k in range(1, spec.record_count):
                with self.subTest(spec.test_id, k=k):
                    text = stream(spec, spec.ordered_cases[:k])
                    with self.assertRaises(ProtocolError):
                        parse(text, require_complete=True)
                    report = parse(text, require_complete=False)
                    self.assertEqual(report.record_count, k)
                    self.assertFalse(report.complete)
                    self.assertFalse(report.all_passed)

    def test_every_truncation_that_kept_its_terminal_record(self) -> None:
        """The transport-defect shape: an ordered prefix ending at the terminal."""

        for spec, parse in PROBES:
            for k in range(1, spec.terminal_count):
                with self.subTest(spec.test_id, k=k):
                    text = stream(
                        spec, list(spec.semantic_cases[:k]) + [spec.terminal_case]
                    )
                    with self.assertRaises(ProtocolError):
                        parse(text, require_complete=True)
                    report = parse(text, require_complete=False)
                    self.assertTrue(report.terminal_present)
                    self.assertFalse(report.complete)
                    self.assertFalse(report.all_passed)

    def test_terminal_present_with_one_missing_middle_record(self) -> None:
        for spec, parse in PROBES:
            for dropped in spec.semantic_cases:
                with self.subTest(spec.test_id, dropped=dropped):
                    text = stream(
                        spec, [c for c in spec.ordered_cases if c != dropped]
                    )
                    with self.assertRaises(ProtocolError):
                        parse(text, require_complete=True)
                    report = parse(text, require_complete=False)
                    self.assertTrue(report.terminal_present)
                    self.assertFalse(report.complete)
                    self.assertFalse(report.all_passed)

    def test_missing_terminal_is_never_complete(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                text = stream(spec, spec.semantic_cases)
                with self.assertRaises(ProtocolError):
                    parse(text, require_complete=True)
                report = parse(text, require_complete=False)
                self.assertFalse(report.terminal_present)
                self.assertFalse(report.complete)
                self.assertFalse(report.all_passed)


class TestStructuralMutations(unittest.TestCase):
    def test_duplicate_semantic_record_rejected_in_both_modes(self) -> None:
        for spec, parse in PROBES:
            for case in spec.semantic_cases:
                with self.subTest(spec.test_id, case=case):
                    text = stream(spec, list(spec.ordered_cases) + [case])
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError):
                            parse(text, require_complete=strict)

    def test_duplicate_terminal_rejected_in_both_modes(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                text = stream(spec, list(spec.ordered_cases) + [spec.terminal_case])
                for strict in (True, False):
                    with self.assertRaises(ProtocolError):
                        parse(text, require_complete=strict)

    def test_swapped_neighbouring_records_rejected(self) -> None:
        for spec, parse in PROBES:
            for i in range(spec.record_count - 1):
                with self.subTest(spec.test_id, i=i):
                    order = list(spec.ordered_cases)
                    order[i], order[i + 1] = order[i + 1], order[i]
                    text = stream(spec, order)
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError):
                            parse(text, require_complete=strict)

    def test_records_after_the_terminal_are_rejected(self) -> None:
        """A fragment appended past the completion record can never extend it."""

        for spec, parse in PROBES:
            for case in spec.semantic_cases:
                with self.subTest(spec.test_id, case=case):
                    order = [c for c in spec.ordered_cases if c != case] + [case]
                    text = stream(spec, order)
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError) as ctx:
                            parse(text, require_complete=strict)
                        self.assertIn("terminal", str(ctx.exception))

    def test_unknown_case_id_rejected_in_both_modes(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                text = META + "".join(
                    line(spec, c) for c in spec.semantic_cases[:1]
                ) + (
                    f"NAKAGAWA_PSP_TEST schema=1 test_id={spec.test_id} "
                    "case_id=alien-cell status=PASS result=0x00000000 out0=0x00000000\n"
                )
                for strict in (True, False):
                    with self.assertRaises(ProtocolError) as ctx:
                        parse(text, require_complete=strict)
                    self.assertIn("unknown case_id", str(ctx.exception))

    def test_wrong_test_id_rejected_in_both_modes(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                text = golden(spec).replace(spec.test_id, "PSP-WRONG-999", 1)
                for strict in (True, False):
                    with self.assertRaises(ProtocolError) as ctx:
                        parse(text, require_complete=strict)
                    self.assertIn("foreign test_id", str(ctx.exception))

    def test_a_probes_stream_is_rejected_by_every_other_probes_parser(self) -> None:
        for spec, _ in PROBES:
            for other_spec, other_parse in PROBES:
                if other_spec.test_id == spec.test_id:
                    continue
                with self.subTest(stream=spec.test_id, parser=other_spec.test_id):
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError):
                            other_parse(golden(spec), require_complete=strict)


class TestTerminalCountIsLoadBearing(unittest.TestCase):
    def test_wrong_terminal_count_rejected_in_both_modes(self) -> None:
        for spec, parse in PROBES:
            for claimed in (0, spec.terminal_count - 1, spec.terminal_count + 1, 99):
                with self.subTest(spec.test_id, claimed=claimed):
                    text = META + "".join(
                        line(spec, c, out0=claimed if c == spec.terminal_case else None)
                        for c in spec.ordered_cases
                    )
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError) as ctx:
                            parse(text, require_complete=strict)
                        self.assertIn("terminal count mismatch", str(ctx.exception))

    def test_terminal_without_out0_rejected(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                text = stream(spec, spec.semantic_cases) + (
                    f"NAKAGAWA_PSP_TEST schema=1 test_id={spec.test_id} "
                    f"case_id={spec.terminal_case} status=PASS result=0x00000000\n"
                )
                for strict in (True, False):
                    with self.assertRaises(ProtocolError) as ctx:
                        parse(text, require_complete=strict)
                    self.assertIn("out0", str(ctx.exception))


class TestStaleLogContaminationIsRejected(unittest.TestCase):
    """The four stale shapes an append-only device log can produce.

    Each must be rejected mechanically, in both strictness modes, without any
    host-side inspection of which run a record came from.
    """

    def test_two_complete_runs_concatenated(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                for strict in (True, False):
                    with self.assertRaises(ProtocolError):
                        parse(golden(spec) + golden(spec), require_complete=strict)

    def test_two_complete_runs_with_a_single_meta(self) -> None:
        """Even without the duplicate META, every case_id repeats."""

        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                text = golden(spec) + stream(spec, spec.ordered_cases, meta="")
                for strict in (True, False):
                    with self.assertRaises(ProtocolError) as ctx:
                        parse(text, require_complete=strict)
                    self.assertIn("duplicate", str(ctx.exception))

    def test_stale_prefix_then_a_complete_run(self) -> None:
        for spec, parse in PROBES:
            for k in range(1, spec.record_count):
                with self.subTest(spec.test_id, stale=k):
                    text = stream(spec, spec.ordered_cases[:k]) + stream(
                        spec, spec.ordered_cases, meta=""
                    )
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError):
                            parse(text, require_complete=strict)

    def test_complete_run_then_a_partial_new_run(self) -> None:
        for spec, parse in PROBES:
            for k in range(1, spec.record_count):
                with self.subTest(spec.test_id, fresh=k):
                    text = golden(spec) + stream(spec, spec.ordered_cases[:k], meta="")
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError):
                            parse(text, require_complete=strict)

    def test_stale_prefix_alone_is_not_complete(self) -> None:
        """A leftover prefix with no fresh run must never look like a pass."""

        for spec, parse in PROBES:
            for k in range(1, spec.record_count):
                with self.subTest(spec.test_id, k=k):
                    report = parse(
                        stream(spec, spec.ordered_cases[:k]), require_complete=False
                    )
                    self.assertFalse(report.complete)
                    self.assertFalse(report.all_passed)


class TestProvenanceIsReportedNeverUpgraded(unittest.TestCase):
    def test_placeholder_metadata_is_flagged_and_never_blocks_the_parse(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                report = parse(golden(spec))
                issues = " ".join(report.sequence.provenance)
                self.assertIn("binary_sha256", issues)
                self.assertIn("source_commit", issues)
                self.assertIn("model", issues)
                self.assertIn("firmware", issues)
                # Placeholder provenance is a reporting fact, not a semantic
                # verdict: the stream still parses and still passes.
                self.assertTrue(report.all_passed)

    def test_measured_metadata_raises_no_provenance_issue(self) -> None:
        measured = (
            "NAKAGAWA_PSP_META schema=1 source=psp model=PSP-3000 "
            "firmware=6.61-ARK-5.1.0 binary_sha256=" + "a" * 64 + " "
            "source_commit=d923abc795a7ec38c8e2f763b632909be73b26d2 fixture=test\n"
        )
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                report = parse(stream(spec, spec.ordered_cases, meta=measured))
                self.assertEqual(report.sequence.provenance, ())


if __name__ == "__main__":
    unittest.main()
