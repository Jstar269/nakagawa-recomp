# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for the Win32/portability inventory (tools/portability_inventory.py).

Guards the classification contract: PSP semantic core files must surface as
SEMANTIC_CORE_CONTAMINATION, host backends as BACKEND_EXPECTED, the manager as
PRIVATE_MANAGER_ONLY, and the scan must not produce false positives from
Vulkan object handles (VK_NULL_HANDLE) or produce nondeterministic output.
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import portability_inventory as pi

ROOT = Path(__file__).resolve().parents[1]


class ClassificationTests(unittest.TestCase):
    def test_core_files_are_semantic_core(self) -> None:
        self.assertEqual(pi.classify_file("src/rt/hle.c")[1], "SEMANTIC_CORE_CONTAMINATION")
        self.assertEqual(pi.classify_file("src/rt/sched.c")[1], "SEMANTIC_CORE_CONTAMINATION")
        self.assertEqual(pi.classify_file("src/rt/sr_coro.c")[1], "SEMANTIC_CORE_CONTAMINATION")
        self.assertEqual(pi.classify_file("src/rt/recomp.c")[1], "SEMANTIC_CORE_CONTAMINATION")

    def test_backend_files_are_backend(self) -> None:
        self.assertEqual(pi.classify_file("src/rt/gpu_sdl3vk/sdl3vk.c")[1], "BACKEND_EXPECTED")
        self.assertEqual(pi.classify_file("src/rt/gpu_sdl3vk/ge_gpu.c")[1], "BACKEND_EXPECTED")
        self.assertEqual(pi.classify_file("src/rt/h264_mf.c")[1], "BACKEND_EXPECTED")
        self.assertEqual(pi.classify_file("src/rt/osk_win.c")[1], "BACKEND_EXPECTED")
        self.assertEqual(pi.classify_file("src/rt/gui.c")[1], "BACKEND_EXPECTED")
        self.assertEqual(pi.classify_file("src/rt/atrac3p/libavcodec/atrac.c")[1], "BACKEND_EXPECTED")

    def test_manager_and_build(self) -> None:
        self.assertEqual(pi.classify_file("hst_manager.ps1")[1], "PRIVATE_MANAGER_ONLY")
        self.assertEqual(pi.classify_file("hst.ps1")[1], "PRIVATE_MANAGER_ONLY")
        self.assertEqual(pi.classify_file("Makefile")[1], "BUILD_TOOL_ONLY")
        self.assertEqual(pi.classify_file("tools/codegen.py")[1], "BUILD_TOOL_ONLY")

    def test_selftests_are_test_only(self) -> None:
        self.assertEqual(pi.classify_file("src/rt/sched_selftest.c")[1], "TEST_ONLY")
        self.assertEqual(pi.classify_file("src/rt/hle_thread_selftest.c")[1], "TEST_ONLY")


class ScanTests(unittest.TestCase):
    def test_vulkan_handles_are_not_false_positives(self) -> None:
        text = "VkPipeline p = VK_NULL_HANDLE;\nsr_x(s_dev, VK_NULL_HANDLE, 1, &pci, NULL, &p);\n"
        hits = pi.scan_text(text, pi.PATTERNS)
        self.assertEqual(hits, [], "VK_NULL_HANDLE must not be reported as a Win32 HANDLE")

    def test_real_handle_hit_is_found(self) -> None:
        text = "    HANDLE find;\n"
        hits = pi.scan_text(text, pi.PATTERNS)
        self.assertTrue(any(h["label"] == "handle_type" for h in hits))

    def test_live_inventory_has_expected_class_distribution(self) -> None:
        files = pi.scan()
        classes = {f["class"] for f in files}
        self.assertEqual(classes, {"semantic_core", "backend", "build", "tests", "manager"})
        by_path = {f["path"]: f for f in files}
        self.assertEqual(by_path["src/rt/hle.c"]["class"], "semantic_core")
        self.assertEqual(by_path["src/rt/gpu_sdl3vk/sdl3vk.c"]["class"], "backend")
        self.assertEqual(by_path["hst_manager.ps1"]["class"], "manager")

    def test_live_scan_is_deterministic(self) -> None:
        a = json.dumps(pi.scan(), sort_keys=True)
        b = json.dumps(pi.scan(), sort_keys=True)
        self.assertEqual(a, b)


if __name__ == "__main__":
    unittest.main()
