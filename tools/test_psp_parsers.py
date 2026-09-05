# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from psp_oracle.parse_fpu_vector import EXPECTED_CELLS as FPU_EXPECTED_CELLS, parse_fpu_vector_output
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


if __name__ == "__main__":
    unittest.main()
