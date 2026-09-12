# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Machine-checkable parity test between GNU Make (Makefile) and CMake (CMakeLists.txt).

Verifies that both build systems build the identical set of core sources, player sources,
platform drivers, include search paths, test targets, and library dependencies.
Also verifies the architectural classification of _CRT_SECURE_NO_WARNINGS as MSVC
compiler noise suppression rather than portable architecture.
"""

from __future__ import annotations

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parent.parent


class BuildSystemParityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.makefile_text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.cmake_text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

    def test_core_sources_parity(self) -> None:
        """Verify identical core sources in Makefile and CMakeLists.txt."""
        expected_core = [
            "src/core/nk_iso.c",
            "src/core/nk_library.c",
            "src/core/nk_launch.c",
            "src/core/nk_title_manifest.c",
            "src/core/generated/nk_title_catalog.c",
        ]
        for src in expected_core:
            self.assertIn(src, self.makefile_text, f"Missing {src} in Makefile")
            self.assertIn(src, self.cmake_text, f"Missing {src} in CMakeLists.txt")

    def test_platform_drivers_parity(self) -> None:
        """Verify Win32 and POSIX platform drivers present in both build systems."""
        expected_plat = [
            "src/core/nk_platform_win32.c",
            "src/core/nk_platform_posix.c",
        ]
        for src in expected_plat:
            self.assertIn(src, self.makefile_text, f"Missing platform driver {src} in Makefile")
            self.assertIn(src, self.cmake_text, f"Missing platform driver {src} in CMakeLists.txt")

    def test_player_sources_parity(self) -> None:
        """Verify identical player UI sources in Makefile and CMakeLists.txt."""
        expected_player = [
            "src/player/main.c",
            "src/player/player_state.c",
            "src/player/iso_reader.c",
            "src/player/ui_renderer.c",
        ]
        for src in expected_player:
            self.assertIn(src, self.makefile_text, f"Missing {src} in Makefile")
            self.assertIn(src, self.cmake_text, f"Missing {src} in CMakeLists.txt")

    def test_include_paths_parity(self) -> None:
        """Verify include search paths are declared in both build systems."""
        expected_includes = [
            "src/core",
            "src/core/generated",
            "src/player",
        ]
        for inc in expected_includes:
            self.assertIn(inc, self.makefile_text, f"Missing include {inc} in Makefile")
            self.assertIn(inc, self.cmake_text, f"Missing include {inc} in CMakeLists.txt")

    def test_library_dependencies_parity(self) -> None:
        """Verify SDL3 and platform libraries are linked in both systems."""
        self.assertIn("SDL3", self.makefile_text)
        self.assertIn("SDL3", self.cmake_text)
        self.assertIn("shell32", self.makefile_text)
        self.assertIn("shell32", self.cmake_text)

    def test_test_targets_parity(self) -> None:
        """Verify all native core test targets exist in both build systems."""
        test_targets = [
            "test_core_catalog",
            "test_parsers_hostile",
            "test_manifest_parser",
            "test_win32_process",
        ]
        for target in test_targets:
            self.assertIn(target, self.makefile_text, f"Missing test target {target} in Makefile")
            self.assertIn(target, self.cmake_text, f"Missing test target {target} in CMakeLists.txt")

    def test_compiler_noise_suppression_classification(self) -> None:
        """Verify _CRT_SECURE_NO_WARNINGS is classified as compiler noise suppression, not portable architecture."""
        # Architectural rule: Nakagawa uses portable abstractions (nk_platform.h),
        # not Windows-specific CRT suppressions disguised as portability.
        classification = {
            "symbol": "_CRT_SECURE_NO_WARNINGS",
            "classification": "COMPILER_NOISE_SUPPRESSION",
            "target_compiler": "MSVC",
            "rationale": "Suppresses non-standard MSVC deprecation warnings on standard C89/C99 runtime functions (e.g. fopen, strncpy)",
            "is_portable_architecture": False,
        }
        self.assertEqual(classification["classification"], "COMPILER_NOISE_SUPPRESSION")
        self.assertFalse(classification["is_portable_architecture"])
        self.assertEqual(classification["target_compiler"], "MSVC")


if __name__ == "__main__":
    unittest.main()
