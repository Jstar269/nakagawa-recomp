# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from psp_oracle.parse_fpu_vector import parse_fpu_vector_output
from psp_oracle.parse_phaseb import parse_phaseb_output
from psp_oracle.parse_ctrl_clock import parse_ctrl_clock_output
from psp_oracle.protocol import ProtocolError

SAMPLE_META = (
    "NAKAGAWA_PSP_META schema=1 source=psp model=unknown firmware=unknown "
    "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
    "source_commit=0000000000000000000000000000000000000000 fixture=test\n"
)


class TestPspParsers(unittest.TestCase):
    def test_parse_fpu_vector_golden(self) -> None:
        stream = (
            SAMPLE_META
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 case_id=fpu-boot-fcr31 status=PASS result=0x00000e00 out0=0x00000e00 out1=0x0000001c out2=0x00000000\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 case_id=fpu-cvt-rm0 status=PASS result=0x00000000 out0=0x00000000\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 case_id=fpu-ftz-contrast status=PASS result=0x00000000 out0=0x00000000 out1=0x00000000 out2=0x00000000 out3=0x01000000\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 case_id=fpu-done status=PASS result=0x00000000 out0=0x0000000f\n"
        )
        report = parse_fpu_vector_output(stream)
        self.assertEqual(report.cell_count, 4)
        self.assertEqual(report.boot_fcr31, 0x00000E00)
        self.assertEqual(report.trap_enables, 0x1C)  # bits 11:7 >> 7
        self.assertTrue(report.all_passed)

    def test_parse_fpu_vector_missing_boot_fcr31(self) -> None:
        stream = (
            SAMPLE_META
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 case_id=fpu-cvt-rm0 status=PASS result=0x00000000\n"
        )
        with self.assertRaises(ProtocolError):
            parse_fpu_vector_output(stream)

    def test_parse_phaseb_golden(self) -> None:
        stream = (
            SAMPLE_META
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-PHASEB-001 case_id=ED2-R77 status=PASS result=0x00000000 out0=0x00000000 out1=0x00000077 out2=0x00000010 out3=0x00000000 out4=0x800201a3 out5=0x00000000\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-PHASEB-001 case_id=MTX-CR-INIT0 status=PASS result=0x04000001 out0=0x00000000 out1=0x00000000 out2=0x00000000 out3=0x00000000\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-PHASEB-001 case_id=CB-ORDINARY-EXEC status=PASS result=0x00000001 out0=0x00000001 out1=0x00000001 out2=0x03500001 out3=0x03500001 out4=0x00007777\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-PHASEB-001 case_id=GE-CB-FIRED status=PASS result=0x00000001 out0=0x00000001 out1=0x00000001 out2=0x00000001\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-PHASEB-001 case_id=PHASEB-COMPLETE status=PASS result=0x00000000 out0=0x00000029\n"
        )
        report = parse_phaseb_output(stream)
        self.assertEqual(report.section_a_count, 1)
        self.assertEqual(report.section_b_count, 1)
        self.assertEqual(report.section_c_count, 1)
        self.assertEqual(report.section_d_count, 1)
        self.assertTrue(report.completed)
        self.assertTrue(report.all_passed)

    def test_parse_ctrl_clock_golden(self) -> None:
        stream = (
            SAMPLE_META
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-CTRL-001 case_id=ctrl-clock-freqs status=PASS result=0x00000000 out0=0x000000de out1=0x0000006f\n"
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-CTRL-001 case_id=ctrl-delay-calibration status=PASS result=0x00000000 out0=0x00002715\n"
        )
        report = parse_ctrl_clock_output(stream)
        self.assertEqual(report.cpu_freq_mhz, 222)  # 0xde = 222
        self.assertEqual(report.bus_freq_mhz, 111)  # 0x6f = 111
        self.assertEqual(report.delay_measured_us, 10005)  # 0x2715 = 10005
        self.assertTrue(report.all_passed)


if __name__ == "__main__":
    unittest.main()
