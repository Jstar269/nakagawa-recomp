# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SAVEDATA_C = ROOT / "src" / "rt" / "savedata.c"

class TestSavedataSpanPreflight(unittest.TestCase):
    def test_savedata_uses_guest_span_validation(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        self.assertIn("sr_guest_span_readable", text, "savedata.c must use sr_guest_span_readable for preflight checks")
        self.assertIn("sr_guest_span_writable", text, "savedata.c must use sr_guest_span_writable for preflight checks")

    def test_preflight_checks_occur_before_host_file_operations(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        do_save_idx = text.find("static uint32_t do_save")
        self.assertNotEqual(do_save_idx, -1)
        do_save_code = text[do_save_idx:text.find("static int dir_exists", do_save_idx)]

        prepare_idx = do_save_code.find("s_storage.prepare")
        span_check_idx = do_save_code.find("sr_guest_span_readable")
        self.assertNotEqual(prepare_idx, -1)
        self.assertNotEqual(span_check_idx, -1)
        self.assertLess(span_check_idx, prepare_idx, "do_save must validate guest spans BEFORE calling s_storage.prepare")

    def test_do_load_validates_spans_before_mutating_datasize(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        do_load_idx = text.find("static uint32_t do_load")
        self.assertNotEqual(do_load_idx, -1)
        do_load_code = text[do_load_idx:text.find("static uint32_t do_delete", do_load_idx)]

        mutate_idx = do_load_code.find("MEM_W32(param + SDP_dataSize, 0)")
        span_check_idx = do_load_code.find("sr_guest_span_writable")
        self.assertNotEqual(mutate_idx, -1)
        self.assertNotEqual(span_check_idx, -1)
        self.assertLess(span_check_idx, mutate_idx, "do_load must validate guest spans BEFORE mutating SDP_dataSize")

    def test_null_buffer_and_oversized_spans_are_rejected_in_do_save_preflight(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        do_save_idx = text.find("static uint32_t do_save")
        self.assertNotEqual(do_save_idx, -1)
        do_save_code = text[do_save_idx:text.find("static int dir_exists", do_save_idx)]
        self.assertIn("!dataBuf", do_save_code, "do_save must reject dataBuf == 0 when dataSize > 0")
        self.assertIn("dataSize >= 0x04000000", do_save_code, "do_save must reject dataSize >= 64 MiB in preflight check")

    def test_zero_capacity_and_null_buffers_are_rejected_in_do_load_preflight(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        do_load_idx = text.find("static uint32_t do_load")
        self.assertNotEqual(do_load_idx, -1)
        do_load_code = text[do_load_idx:text.find("static uint32_t do_delete", do_load_idx)]
        self.assertIn("!dataBuf", do_load_code, "do_load must reject dataBuf == 0 when loading file data")
        self.assertIn("cap == 0", do_load_code, "do_load must reject cap == 0 when loading file data")
        self.assertIn("cap >= 0x04000000", do_load_code, "do_load must reject cap >= 64 MiB in preflight check")

    def test_load_debug_preview_is_bounded_by_bytes_read(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        do_load_idx = text.find("static uint32_t do_load")
        self.assertNotEqual(do_load_idx, -1)
        do_load_code = text[do_load_idx:text.find("static uint32_t do_delete", do_load_idx)]
        self.assertIn("rd > 0", do_load_code, "debug preview must bound p[0] access by actual bytes read")
        self.assertIn("rd > 3", do_load_code, "debug preview must bound p[3] access by actual bytes read")

    def test_load_file_size_clamping_uses_uint64_comparison(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        do_load_idx = text.find("static uint32_t do_load")
        self.assertNotEqual(do_load_idx, -1)
        do_load_code = text[do_load_idx:text.find("static uint32_t do_delete", do_load_idx)]
        self.assertIn("(uint64_t)cap", do_load_code, "do_load must compare file size against cap using 64-bit unsigned comparison to prevent LP64 narrowing truncation bypass")

    def test_sfo_reader_checks_index_extent_and_bounded_keys(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        sfo_idx = text.find("static void load_sfo_param")
        self.assertNotEqual(sfo_idx, -1)
        sfo_code = text[sfo_idx:text.find("/* ---- modes", sfo_idx)]
        self.assertIn("cnt > ((uint32_t)n - 20u) / 16u", sfo_code)
        self.assertIn("uint64_t keyPos", sfo_code)
        self.assertIn("uint64_t dataPos", sfo_code)
        self.assertIn("unterminated key", sfo_code)
        self.assertNotIn("keyStart + keyOff >=", sfo_code)
        self.assertNotIn("dataStart + dataOff + len >", sfo_code)

if __name__ == "__main__":
    unittest.main()
