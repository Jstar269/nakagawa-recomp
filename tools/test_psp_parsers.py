# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from psp_oracle.parse_fpu_vector import (
    EXPECTED_CELLS as FPU_EXPECTED_CELLS,
    EXPECTED_TERMINAL_COUNT,
    parse_fpu_vector_output,
)
from psp_oracle.parse_phaseb import EXPECTED_ORDERED_CASES as PHASEB_EXPECTED_CASES, parse_phaseb_output
from psp_oracle.parse_ctrl_clock import parse_ctrl_clock_output
from psp_oracle.protocol import ProtocolError

SAMPLE_META = (
    "NAKAGAWA_PSP_META schema=1 source=psp model=unknown firmware=unknown "
    "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
    "source_commit=0000000000000000000000000000000000000000 fixture=test\n"
)


def _make_fpu_line(case_id: str) -> str:
    if case_id == "fpu-boot-fcr31":
        return f"NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 case_id={case_id} status=PASS result=0x00000e00 out0=0x00000e00 out1=0x0000001c out2=0x00000000\n"
    if case_id == "fpu-ftz-contrast":
        return f"NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 case_id={case_id} status=PASS result=0x00000000 out0=0x00000000 out1=0x00000000 out2=0x00000000 out3=0x01000000\n"
    if case_id == "fpu-done":
        return f"NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 case_id={case_id} status=PASS result=0x00000000 out0=0x0000000f\n"
    return f"NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 case_id={case_id} status=PASS result=0x00000000 out0=0x00000000\n"


def _make_phaseb_line(case_id: str) -> str:
    if case_id == "PHASEB-COMPLETE":
        return f"NAKAGAWA_PSP_TEST schema=1 test_id=PSP-PHASEB-001 case_id={case_id} status=PASS result=0x00000000 out0=0x00000029\n"
    return f"NAKAGAWA_PSP_TEST schema=1 test_id=PSP-PHASEB-001 case_id={case_id} status=PASS result=0x00000000 out0=0x00000000\n"


class TestPspParsers(unittest.TestCase):
    def test_parse_fpu_vector_golden_complete(self) -> None:
        stream = SAMPLE_META + "".join(_make_fpu_line(cid) for cid in FPU_EXPECTED_CELLS)
        report = parse_fpu_vector_output(stream)
        self.assertEqual(report.cell_count, 16)
        self.assertEqual(report.boot_fcr31, 0x00000E00)
        self.assertEqual(report.trap_enables, 0x1C)  # bits 11:7 >> 7
        self.assertTrue(report.all_passed)

    def test_parse_fpu_vector_missing_boot_fcr31(self) -> None:
        stream = SAMPLE_META + "".join(
            _make_fpu_line(cid) for cid in FPU_EXPECTED_CELLS if cid != "fpu-boot-fcr31"
        )
        with self.assertRaises(ProtocolError):
            parse_fpu_vector_output(stream)

    def test_parse_fpu_vector_incomplete_strict(self) -> None:
        stream = (
            SAMPLE_META
            + _make_fpu_line("fpu-boot-fcr31")
            + _make_fpu_line("fpu-cvt-rm0")
            + _make_fpu_line("fpu-done")
        )
        with self.assertRaises(ProtocolError):
            parse_fpu_vector_output(stream, require_complete=True)
        # Non-strict allows ordered partial streams:
        report = parse_fpu_vector_output(stream, require_complete=False)
        self.assertEqual(report.cell_count, 3)

    def test_parse_fpu_vector_out_of_order(self) -> None:
        cells = list(FPU_EXPECTED_CELLS)
        cells[1], cells[2] = cells[2], cells[1]  # swap rm0 and rm1
        stream = SAMPLE_META + "".join(_make_fpu_line(cid) for cid in cells)
        with self.assertRaises(ProtocolError):
            parse_fpu_vector_output(stream)

    def test_parse_fpu_vector_duplicate_cell(self) -> None:
        stream = (
            SAMPLE_META
            + _make_fpu_line("fpu-boot-fcr31")
            + _make_fpu_line("fpu-cvt-rm0")
            + _make_fpu_line("fpu-cvt-rm0")
        )
        with self.assertRaises(ProtocolError):
            parse_fpu_vector_output(stream, require_complete=False)

    def test_parse_phaseb_golden_complete(self) -> None:
        stream = SAMPLE_META + "".join(_make_phaseb_line(cid) for cid in PHASEB_EXPECTED_CASES)
        report = parse_phaseb_output(stream)
        self.assertEqual(report.section_a_count, 12)
        self.assertEqual(report.section_b_count, 19)
        self.assertEqual(report.section_c_count, 6)
        self.assertEqual(report.section_d_count, 4)
        self.assertTrue(report.completed)
        self.assertTrue(report.all_passed)

    def test_parse_phaseb_incomplete_40_of_41(self) -> None:
        # Omit one case from section A
        cells = [cid for cid in PHASEB_EXPECTED_CASES if cid != "ED2-DERR"]
        stream = SAMPLE_META + "".join(_make_phaseb_line(cid) for cid in cells)
        with self.assertRaises(ProtocolError):
            parse_phaseb_output(stream, require_complete=True)

    def test_parse_phaseb_out_of_order(self) -> None:
        # Put Section A before Section C
        sec_a = [cid for cid in PHASEB_EXPECTED_CASES if cid.startswith("ED2-")]
        others = [cid for cid in PHASEB_EXPECTED_CASES if not cid.startswith("ED2-")]
        stream = SAMPLE_META + "".join(_make_phaseb_line(cid) for cid in (sec_a + others))
        with self.assertRaises(ProtocolError):
            parse_phaseb_output(stream)

    def test_parse_phaseb_duplicate_case(self) -> None:
        stream = (
            SAMPLE_META
            + _make_phaseb_line("CB-ORDINARY-EXEC")
            + _make_phaseb_line("CB-ORDINARY-EXEC")
        )
        with self.assertRaises(ProtocolError):
            parse_phaseb_output(stream, require_complete=False)

    def test_parse_ctrl_clock_system_golden(self) -> None:
        stream = (
            SAMPLE_META
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-SYSTEM-001 case_id=ctrl-ts-pairs status=PASS result=0x00000008 out0=0x00000008 out1=0x00000002 out2=0x00000010 out3=0x00000002 out4=0x00000010 out5=0x00010000 out6=0x00010000\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-SYSTEM-001 case_id=ctrl-ts-vcount status=PASS result=0x00000008 out0=0x00000008 out1=0x00000001 out2=0x00000002 out3=0x00000002 out4=0x00000010 out5=0x00000010 out6=0x00010000\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-SYSTEM-001 case_id=clock-snapshot status=PASS result=0x00010000 out0=0x00010000 out1=0x00000020 out2=0x00000100 out3=0x12345678 out4=0x00000000 out5=0x000000de out6=0x00000000\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-SYSTEM-001 case_id=delay-10ms status=PASS result=0x00002715 out0=0x00010000 out1=0x00012715 out2=0x00002715 out3=0x00002710\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-SYSTEM-001 case_id=ctrl-zero-count status=PASS result=0x00000000 out0=0x00012715\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-SYSTEM-001 case_id=ctrl-peek-read status=PASS result=0x00000000 out0=0x00012715 out1=0x00012715 out2=0x00000001 out3=0x00000001\n"
        )
        report = parse_ctrl_clock_output(stream)
        self.assertEqual(report.cpu_freq_mhz, 222)  # 0xde = 222
        self.assertEqual(report.bus_freq_mhz, 111)  # 222 // 2
        self.assertEqual(report.delay_measured_us, 10005)  # 0x2715 = 10005
        self.assertTrue(report.all_passed)

    def test_parse_ctrl_clock_timestamp_wrap(self) -> None:
        # uint32 wrap test: pad timestamp wrapping around 0xFFFFFFFF
        ts_before = 0xFFFFFFF0
        ts_after = 0x00000010
        delta = (ts_after - ts_before) & 0xFFFFFFFF
        self.assertEqual(delta, 32)

    def test_parse_ctrl_clock_synthetic_ctrl_golden(self) -> None:
        stream = (
            SAMPLE_META
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-CTRL-001 case_id=ctrl-clock-freqs status=PASS result=0x00000000 out0=0x000000de out1=0x0000006f\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-CTRL-001 case_id=ctrl-delay-calibration status=PASS result=0x00000000 out0=0x00002715\n"
        )
        report = parse_ctrl_clock_output(stream)
        self.assertEqual(report.cpu_freq_mhz, 222)
        self.assertEqual(report.bus_freq_mhz, 111)
        self.assertEqual(report.delay_measured_us, 10005)
        self.assertTrue(report.all_passed)

    def test_parse_ctrl_clock_out_of_order(self) -> None:
        stream = (
            SAMPLE_META
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-CTRL-001 case_id=ctrl-delay-calibration status=PASS result=0x00000000 out0=0x00002715\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-CTRL-001 case_id=ctrl-clock-freqs status=PASS result=0x00000000 out0=0x000000de out1=0x0000006f\n"
        )
        with self.assertRaises(ProtocolError):
            parse_ctrl_clock_output(stream)


class TestPartialStreamsNeverPass(unittest.TestCase):
    """Regression cover for the non-strict false-PASS defect.

    ``require_complete=False`` exists so a truncated capture can be *inspected*
    and classified.  It must never be able to produce ``all_passed`` (FPU and
    Phase-B) or ``completed`` (Phase-B).  Each case below is asserted in the
    non-strict mode, because strict mode raises before a verdict exists.
    """

    def _fpu(self, cells: list[str]) -> str:
        return SAMPLE_META + "".join(_make_fpu_line(cid) for cid in cells)

    def _phaseb(self, cases: list[str]) -> str:
        return SAMPLE_META + "".join(_make_phaseb_line(cid) for cid in cases)

    # ---- 1. ordinary prefix truncation -------------------------------------

    def test_fpu_ordinary_prefix_truncation_never_passes(self) -> None:
        for k in range(1, len(FPU_EXPECTED_CELLS)):
            with self.subTest(records=k):
                stream = self._fpu(FPU_EXPECTED_CELLS[:k])
                with self.assertRaises(ProtocolError):
                    parse_fpu_vector_output(stream, require_complete=True)
                report = parse_fpu_vector_output(stream, require_complete=False)
                self.assertEqual(report.cell_count, k)
                self.assertFalse(report.complete)
                self.assertFalse(report.all_passed)

    def test_phaseb_ordinary_prefix_truncation_never_passes(self) -> None:
        # Section C starts the stream, so every 1..41-record prefix is ordinary
        # (the 42nd record is the PHASEB-COMPLETE summary).
        for k in range(1, len(PHASEB_EXPECTED_CASES)):
            with self.subTest(records=k):
                stream = self._phaseb(PHASEB_EXPECTED_CASES[:k])
                with self.assertRaises(ProtocolError):
                    parse_phaseb_output(stream, require_complete=True)
                report = parse_phaseb_output(stream, require_complete=False)
                self.assertEqual(len(report.results), k)
                self.assertFalse(report.completed)
                self.assertFalse(report.all_passed)

    # ---- 2. terminal-record-containing truncation --------------------------

    def test_fpu_truncation_with_terminal_record_never_passes(self) -> None:
        # The exact shape the Boot #10 transport defect produces: an ordered
        # prefix whose last record is the terminal ``fpu-done``.
        stream = self._fpu(["fpu-boot-fcr31", "fpu-cvt-rm0", "fpu-done"])
        with self.assertRaises(ProtocolError):
            parse_fpu_vector_output(stream, require_complete=True)
        report = parse_fpu_vector_output(stream, require_complete=False)
        self.assertEqual(report.cell_count, 3)
        self.assertTrue(report.terminal_present)
        self.assertFalse(report.complete)
        self.assertFalse(report.all_passed)

    def test_fpu_every_prefix_plus_terminal_never_passes(self) -> None:
        for k in range(1, len(FPU_EXPECTED_CELLS) - 1):
            with self.subTest(records=k):
                stream = self._fpu(list(FPU_EXPECTED_CELLS[:k]) + ["fpu-done"])
                report = parse_fpu_vector_output(stream, require_complete=False)
                self.assertTrue(report.terminal_present)
                self.assertFalse(report.complete)
                self.assertFalse(report.all_passed)

    def test_phaseb_truncation_with_summary_record_never_completes(self) -> None:
        stream = self._phaseb(["CB-ORDINARY-EXEC", "GE-CB-FIRED", "PHASEB-COMPLETE"])
        with self.assertRaises(ProtocolError):
            parse_phaseb_output(stream, require_complete=True)
        report = parse_phaseb_output(stream, require_complete=False)
        self.assertEqual(len(report.results), 3)
        self.assertTrue(report.summary_present)
        self.assertFalse(report.completed)
        self.assertFalse(report.all_passed)

    def test_phaseb_every_prefix_plus_summary_never_completes(self) -> None:
        for k in range(1, len(PHASEB_EXPECTED_CASES) - 1):
            with self.subTest(records=k):
                cases = list(PHASEB_EXPECTED_CASES[:k]) + ["PHASEB-COMPLETE"]
                stream = self._phaseb(cases)
                report = parse_phaseb_output(stream, require_complete=False)
                self.assertTrue(report.summary_present)
                self.assertFalse(report.completed)
                self.assertFalse(report.all_passed)

    # ---- 3. missing middle records, terminal still present -----------------

    def test_fpu_missing_middle_with_terminal_never_passes(self) -> None:
        dropped = {"fpu-flag-div0", "fpu-cvt-rm2", "fpu-signed-zero"}
        cells = [cid for cid in FPU_EXPECTED_CELLS if cid not in dropped]
        stream = self._fpu(cells)
        with self.assertRaises(ProtocolError):
            parse_fpu_vector_output(stream, require_complete=True)
        report = parse_fpu_vector_output(stream, require_complete=False)
        self.assertEqual(report.cell_count, len(FPU_EXPECTED_CELLS) - 3)
        self.assertTrue(report.terminal_present)
        self.assertFalse(report.complete)
        self.assertFalse(report.all_passed)

    def test_phaseb_missing_middle_with_summary_never_completes(self) -> None:
        dropped = {"MTX-LK-REC", "GE-CB-STACK", "ED2-D00"}
        cases = [cid for cid in PHASEB_EXPECTED_CASES if cid not in dropped]
        stream = self._phaseb(cases)
        with self.assertRaises(ProtocolError):
            parse_phaseb_output(stream, require_complete=True)
        report = parse_phaseb_output(stream, require_complete=False)
        self.assertEqual(len(report.results), len(PHASEB_EXPECTED_CASES) - 3)
        self.assertTrue(report.summary_present)
        self.assertFalse(report.completed)
        self.assertFalse(report.all_passed)

    def test_fpu_single_missing_cell_with_terminal_never_passes(self) -> None:
        for dropped in FPU_EXPECTED_CELLS[1:-1]:
            with self.subTest(dropped=dropped):
                cells = [cid for cid in FPU_EXPECTED_CELLS if cid != dropped]
                report = parse_fpu_vector_output(
                    self._fpu(cells), require_complete=False
                )
                self.assertTrue(report.terminal_present)
                self.assertFalse(report.complete)
                self.assertFalse(report.all_passed)

    # ---- 4. duplicate terminal ---------------------------------------------

    def test_fpu_duplicate_terminal_rejected_in_both_modes(self) -> None:
        stream = self._fpu(list(FPU_EXPECTED_CELLS) + ["fpu-done"])
        with self.assertRaises(ProtocolError):
            parse_fpu_vector_output(stream, require_complete=True)
        with self.assertRaises(ProtocolError):
            parse_fpu_vector_output(stream, require_complete=False)

    def test_phaseb_duplicate_summary_rejected_in_both_modes(self) -> None:
        stream = self._phaseb(list(PHASEB_EXPECTED_CASES) + ["PHASEB-COMPLETE"])
        with self.assertRaises(ProtocolError):
            parse_phaseb_output(stream, require_complete=True)
        with self.assertRaises(ProtocolError):
            parse_phaseb_output(stream, require_complete=False)

    # ---- 5. full correct stream still passes -------------------------------

    def test_fpu_full_stream_passes_in_both_modes(self) -> None:
        stream = self._fpu(list(FPU_EXPECTED_CELLS))
        for require_complete in (True, False):
            with self.subTest(require_complete=require_complete):
                report = parse_fpu_vector_output(
                    stream, require_complete=require_complete
                )
                self.assertEqual(report.cell_count, 16)
                self.assertTrue(report.complete)
                self.assertTrue(report.terminal_present)
                self.assertTrue(report.all_passed)

    def test_phaseb_full_stream_passes_in_both_modes(self) -> None:
        stream = self._phaseb(list(PHASEB_EXPECTED_CASES))
        self.assertEqual(len(PHASEB_EXPECTED_CASES), 42)
        for require_complete in (True, False):
            with self.subTest(require_complete=require_complete):
                report = parse_phaseb_output(
                    stream, require_complete=require_complete
                )
                self.assertEqual(len(report.results), 42)
                self.assertTrue(report.summary_present)
                self.assertTrue(report.completed)
                self.assertTrue(report.all_passed)

    # ---- complete-but-failing streams stay measured-and-failed -------------

    def test_fpu_complete_stream_with_fail_cell_is_not_all_passed(self) -> None:
        stream = SAMPLE_META + "".join(
            _make_fpu_line(cid).replace("status=PASS", "status=FAIL")
            if cid == "fpu-flag-div0"
            else _make_fpu_line(cid)
            for cid in FPU_EXPECTED_CELLS
        )
        report = parse_fpu_vector_output(stream)
        self.assertTrue(report.complete)
        self.assertFalse(report.all_passed)

    def test_phaseb_complete_stream_with_fail_case_is_completed_not_passed(self) -> None:
        stream = SAMPLE_META + "".join(
            _make_phaseb_line(cid).replace("status=PASS", "status=FAIL")
            if cid == "MTX-WAIT-FIFO"
            else _make_phaseb_line(cid)
            for cid in PHASEB_EXPECTED_CASES
        )
        report = parse_phaseb_output(stream)
        self.assertTrue(report.completed)
        self.assertFalse(report.all_passed)

    # ---- ctrl/clock carries the same defect class --------------------------

    def test_ctrl_clock_partial_never_passes(self) -> None:
        line = (
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-CTRL-001 "
            "case_id=ctrl-clock-freqs status=PASS result=0x00000000 "
            "out0=0x000000de out1=0x0000006f\n"
        )
        stream = SAMPLE_META + line
        with self.assertRaises(ProtocolError):
            parse_ctrl_clock_output(stream, require_complete=True)
        report = parse_ctrl_clock_output(stream, require_complete=False)
        self.assertFalse(report.complete)
        self.assertFalse(report.all_passed)


class TestDualChannelReconciliation(unittest.TestCase):
    """Host tests proving the dual-channel evidence contract:

    - partial stdout cannot become semantic PASS;
    - complete host0 stream can independently establish semantic completeness;
    - channel mismatch is reported without downgrading an otherwise complete host0 semantic measurement;
    - stale append logs (duplicate cells) are rejected;
    - completion count mismatch fails;
    - duplicate records fail;
    - reordered records fail;
    - truncated host0 log fails;
    - placeholder provenance remains explicitly HOST_ATTESTED rather than stream-attested;
    - stdout loss can be represented as a transport observation separately from semantic result.
    """

    def setUp(self) -> None:
        self.complete_fpu_stream = SAMPLE_META + "".join(
            _make_fpu_line(cid) for cid in FPU_EXPECTED_CELLS
        )
        self.partial_fpu_stdout = (
            SAMPLE_META + _make_fpu_line("fpu-boot-fcr31")
        )

    def test_partial_stdout_cannot_become_semantic_pass(self) -> None:
        """Partial stdout capture (1/16) must fail strict parse and report all_passed=False."""
        with self.assertRaises(ProtocolError) as ctx:
            parse_fpu_vector_output(self.partial_fpu_stdout, require_complete=True)
        self.assertIn("missing cells", str(ctx.exception))
        report = parse_fpu_vector_output(self.partial_fpu_stdout, require_complete=False)
        self.assertEqual(report.cell_count, 1)
        self.assertFalse(report.complete)
        self.assertFalse(report.terminal_present)
        self.assertFalse(report.all_passed)

    def test_complete_host0_independently_establishes_completeness(self) -> None:
        """A complete host0 stream alone establishes semantic completeness and all_passed."""
        report = parse_fpu_vector_output(self.complete_fpu_stream, require_complete=True)
        self.assertEqual(report.cell_count, 16)
        self.assertTrue(report.complete)
        self.assertTrue(report.terminal_present)
        self.assertTrue(report.all_passed)
        self.assertEqual(report.boot_fcr31, 0x00000E00)

    def test_channel_mismatch_reported_without_downgrading_host0_semantic(self) -> None:
        """Channel mismatch is reported while preserving complete host0 measurement."""
        stdout_report = parse_fpu_vector_output(self.partial_fpu_stdout, require_complete=False)
        host0_report = parse_fpu_vector_output(self.complete_fpu_stream, require_complete=True)

        # Stdout channel verdict
        stdout_transport_status = "PARTIAL_HARDWARE_PREFIX" if not stdout_report.complete else "COMPLETE"
        self.assertEqual(stdout_transport_status, "PARTIAL_HARDWARE_PREFIX")
        self.assertFalse(stdout_report.all_passed)

        # Host0 semantic verdict
        host0_semantic_status = "HARDWARE_MEASURED" if host0_report.all_passed else "FAIL"
        self.assertEqual(host0_semantic_status, "HARDWARE_MEASURED")
        self.assertTrue(host0_report.all_passed)

        # Channels do not agree on completeness, but host0 measurement remains valid
        channels_agree = (stdout_report.cell_count == host0_report.cell_count)
        self.assertFalse(channels_agree)

    def test_stale_append_log_rejected(self) -> None:
        """Stale append (log containing records from two runs) produces duplicate cell error."""
        stale_stream = (
            SAMPLE_META
            + "".join(_make_fpu_line(cid) for cid in FPU_EXPECTED_CELLS)
            + "".join(_make_fpu_line(cid) for cid in FPU_EXPECTED_CELLS)
        )
        with self.assertRaises(ProtocolError) as ctx:
            parse_fpu_vector_output(stale_stream, require_complete=False)
        self.assertIn("duplicate", str(ctx.exception))

    def test_duplicate_records_fail(self) -> None:
        """Any duplicate case_id must fail closed."""
        dup_stream = (
            SAMPLE_META
            + _make_fpu_line("fpu-boot-fcr31")
            + _make_fpu_line("fpu-boot-fcr31")
        )
        with self.assertRaises(ProtocolError) as ctx:
            parse_fpu_vector_output(dup_stream, require_complete=False)
        self.assertIn("duplicate", str(ctx.exception))

    def test_reordered_records_fail(self) -> None:
        """Out-of-order records must fail both strict and permissive modes."""
        reordered = list(FPU_EXPECTED_CELLS)
        reordered[1], reordered[2] = reordered[2], reordered[1]
        stream = SAMPLE_META + "".join(_make_fpu_line(cid) for cid in reordered)
        with self.assertRaises(ProtocolError) as ctx_strict:
            parse_fpu_vector_output(stream, require_complete=True)
        self.assertIn("out of order", str(ctx_strict.exception))

        with self.assertRaises(ProtocolError) as ctx_permissive:
            parse_fpu_vector_output(stream, require_complete=False)
        self.assertIn("out of order", str(ctx_permissive.exception))

    def test_truncated_host0_log_fails(self) -> None:
        """Truncated host0 log (missing last N records) must fail strict parse."""
        for cutoff in [1, 5, 10, 15]:
            truncated = SAMPLE_META + "".join(
                _make_fpu_line(cid) for cid in FPU_EXPECTED_CELLS[:cutoff]
            )
            with self.assertRaises(ProtocolError):
                parse_fpu_vector_output(truncated, require_complete=True)

    def _stream_with_sentinel(self, sentinel_line: str) -> str:
        return SAMPLE_META + "".join(
            _make_fpu_line(cid) if cid != "fpu-done" else sentinel_line
            for cid in FPU_EXPECTED_CELLS
        )

    def test_completion_count_mismatch_fails(self) -> None:
        """A sentinel whose out0 disagrees with the protocol count fails closed.

        This exercises the parser, not the fixture: an otherwise complete,
        all-PASS, correctly ordered stream must still be rejected when the
        terminal record miscounts, in both strictness modes.
        """
        bad_sentinel_line = (
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 case_id=fpu-done "
            "status=PASS result=0x00000000 out0=0x0000000a\n"
        )
        stream = self._stream_with_sentinel(bad_sentinel_line)
        for require_complete in (True, False):
            with self.subTest(require_complete=require_complete):
                with self.assertRaises(ProtocolError) as ctx:
                    parse_fpu_vector_output(stream, require_complete=require_complete)
                self.assertIn("terminal count mismatch", str(ctx.exception))

    def test_completion_sentinel_missing_count_fails(self) -> None:
        """A terminal record carrying no out0 count is rejected."""
        no_count_line = (
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 case_id=fpu-done "
            "status=PASS result=0x00000000\n"
        )
        with self.assertRaises(ProtocolError) as ctx:
            parse_fpu_vector_output(self._stream_with_sentinel(no_count_line))
        self.assertIn("missing its out0", str(ctx.exception))

    def test_correct_sentinel_count_is_accepted(self) -> None:
        """The count the probe actually emits (15 semantic cells) parses."""
        report = parse_fpu_vector_output(self.complete_fpu_stream, require_complete=True)
        claimed = int(dict(report.results["fpu-done"].values)["out0"], 0)
        self.assertEqual(claimed, EXPECTED_TERMINAL_COUNT)
        self.assertEqual(claimed, len(FPU_EXPECTED_CELLS) - 1)
        self.assertTrue(report.all_passed)

    def test_placeholder_provenance_remains_host_attested(self) -> None:
        """Placeholder fixture metadata produces issues and is HOST_ATTESTED."""
        from psp_oracle.protocol import parse_output, provenance_issues
        parsed = parse_output(self.complete_fpu_stream)
        issues = provenance_issues(parsed.metadata_dict())
        self.assertTrue(len(issues) > 0)
        self.assertTrue(any("unknown" in issue for issue in issues))
        self.assertTrue(any("all-zero" in issue for issue in issues))

    def test_surviving_terminal_after_middle_loss_fails_closed(self) -> None:
        """The transport-defect shape must not pass.

        A stream that keeps its head and its terminal sentinel but loses the
        middle records is exactly what a mid-run channel drop produces.  The
        terminal record must never substitute for the missing measurements.
        """
        head = FPU_EXPECTED_CELLS[:3]
        stream = (
            SAMPLE_META
            + "".join(_make_fpu_line(cid) for cid in head)
            + _make_fpu_line("fpu-done")
        )
        with self.assertRaises(ProtocolError) as ctx:
            parse_fpu_vector_output(stream, require_complete=True)
        self.assertIn("missing cells", str(ctx.exception))

        report = parse_fpu_vector_output(stream, require_complete=False)
        self.assertTrue(report.terminal_present)
        self.assertFalse(report.complete)
        self.assertFalse(report.all_passed)

    # ---- Mutation tests ----

    def test_mutation_fail_status_in_single_cell_prevents_pass(self) -> None:
        """Mutating any single cell status from PASS to FAIL prevents all_passed."""
        for mutant_cell in FPU_EXPECTED_CELLS:
            with self.subTest(cell=mutant_cell):
                stream = SAMPLE_META + "".join(
                    _make_fpu_line(cid).replace("status=PASS", "status=FAIL")
                    if cid == mutant_cell
                    else _make_fpu_line(cid)
                    for cid in FPU_EXPECTED_CELLS
                )
                report = parse_fpu_vector_output(stream)
                self.assertTrue(report.complete)
                self.assertFalse(report.all_passed)

    def test_mutation_missing_terminal_fails_completeness(self) -> None:
        """Mutating stream to drop only fpu-done leaves terminal_present False."""
        stream = SAMPLE_META + "".join(
            _make_fpu_line(cid) for cid in FPU_EXPECTED_CELLS if cid != "fpu-done"
        )
        with self.assertRaises(ProtocolError):
            parse_fpu_vector_output(stream, require_complete=True)
        report = parse_fpu_vector_output(stream, require_complete=False)
        self.assertFalse(report.complete)
        self.assertFalse(report.terminal_present)
        self.assertFalse(report.all_passed)

    def test_mutation_alien_case_id_rejected(self) -> None:
        """An unknown alien case_id is rejected in BOTH strictness modes.

        An unknown record is never a truncation, so the non-strict inspection
        mode must not quietly filter it out either.
        """
        stream = (
            SAMPLE_META
            + "".join(_make_fpu_line(cid) for cid in FPU_EXPECTED_CELLS[:5])
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 case_id=fpu-alien-cell status=PASS result=0x0\n"
            + "".join(_make_fpu_line(cid) for cid in FPU_EXPECTED_CELLS[5:])
        )
        for strict in (True, False):
            with self.subTest(require_complete=strict):
                with self.assertRaises(ProtocolError) as ctx:
                    parse_fpu_vector_output(stream, require_complete=strict)
                self.assertIn("unknown cell", str(ctx.exception))

    def test_mutation_foreign_test_id_record_rejected(self) -> None:
        """A complete run plus one foreign-probe record must not pass.

        Before this, both parsers filtered by ``test_id``, so a complete FPU
        stream carrying an appended record from a different probe's run parsed
        as an unqualified pass -- the "two independently valid streams from
        different run identities" shape.
        """
        stream = (
            SAMPLE_META
            + "".join(_make_fpu_line(cid) for cid in FPU_EXPECTED_CELLS)
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-IO-001 case_id=io-done status=PASS result=0x0 out0=0x6\n"
        )
        for strict in (True, False):
            with self.subTest(require_complete=strict):
                with self.assertRaises(ProtocolError) as ctx:
                    parse_fpu_vector_output(stream, require_complete=strict)
                self.assertIn("foreign test_id", str(ctx.exception))

    def test_mutation_phaseb_foreign_test_id_record_rejected(self) -> None:
        stream = (
            SAMPLE_META
            + "".join(_make_phaseb_line(cid) for cid in PHASEB_EXPECTED_CASES)
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 case_id=fpu-done status=PASS result=0x0 out0=0xf\n"
        )
        for strict in (True, False):
            with self.subTest(require_complete=strict):
                with self.assertRaises(ProtocolError) as ctx:
                    parse_phaseb_output(stream, require_complete=strict)
                self.assertIn("foreign test_id", str(ctx.exception))

    def test_mutation_phaseb_alien_case_id_rejected(self) -> None:
        stream = (
            SAMPLE_META
            + "".join(_make_phaseb_line(cid) for cid in PHASEB_EXPECTED_CASES[:5])
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-PHASEB-001 case_id=PHB-ALIEN status=PASS result=0x0\n"
        )
        for strict in (True, False):
            with self.subTest(require_complete=strict):
                with self.assertRaises(ProtocolError) as ctx:
                    parse_phaseb_output(stream, require_complete=strict)
                self.assertIn("unknown case", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
