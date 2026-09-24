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
import shutil
import subprocess
import tempfile
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
            "src/player/input_settings.c",
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

    def test_verification_targets_present(self) -> None:
        """Verify make test and make check exist in Makefile (Issue #188 O-05)."""
        self.assertTrue(
            bool(re.search(r"^test:", self.makefile_text, re.MULTILINE)),
            "Missing test target in Makefile",
        )
        self.assertTrue(
            bool(re.search(r"^check:", self.makefile_text, re.MULTILINE)),
            "Missing check target in Makefile",
        )

    def test_public_target_catalog_covers_phony_targets(self) -> None:
        """`make help` must expose every public target exactly once (Issue #188 O-16)."""
        catalog = re.search(
            r"(?ms)^PUBLIC_TARGETS := \\\n(.*?)(?=^INTERNAL_TARGETS :=)",
            self.makefile_text,
        )
        self.assertIsNotNone(catalog, "Makefile must define the public target catalog")
        targets = re.findall(
            r"(?m)^\s*([A-Za-z0-9_.%/+?-]+)\s*\\?$",
            catalog.group(1),
        )
        self.assertEqual(len(targets), len(set(targets)), "public target catalog contains a duplicate")
        self.assertIn("help", targets)
        self.assertIn("psp-oracle", targets)
        self.assertIn("psp-oracle-nakagawa", targets)
        self.assertIn(".PHONY: $(PUBLIC_TARGETS) $(INTERNAL_TARGETS)", self.makefile_text)

        defined_targets = set(re.findall(
            r"(?m)^([A-Za-z0-9_.%/+?-]+):",
            self.makefile_text,
        ))
        defined_targets.difference_update({".PHONY", ".SECONDARY"})
        internal = set(
            self.makefile_text.split("INTERNAL_TARGETS :=", 1)[1]
            .splitlines()[0].split()
        )
        self.assertEqual(
            defined_targets,
            set(targets) | internal,
            "public and internal target catalogs must cover every named target",
        )

        descriptions = dict(re.findall(
            r"(?m)^HELP_DESCRIPTION_([A-Za-z0-9_.%/+?-]+)\s*:=\s*(.+)$",
            self.makefile_text,
        ))
        self.assertEqual(set(targets), set(descriptions), "every public target needs one description")
        self.assertTrue(all(description.strip() for description in descriptions.values()))

    def test_provenance_refresh_wrapper_keeps_authority_explicit(self) -> None:
        self.assertRegex(self.makefile_text, r"(?m)^provenance-refresh:")
        self.assertIn("tools/provenance_refresh.py", self.makefile_text)
        self.assertIn("NK_TRUSTED_LEDGER", self.makefile_text)
        self.assertIn("PROVENANCE_REFRESH_APPLY_POLICY", self.makefile_text)

    def test_audited_public_controls_stay_diff_visible(self) -> None:
        """The provenance controls are audited bytes, so PR diffs must show them.

        GitHub collapses diffs of paths marked ``linguist-generated``; marking the
        ledger or export that way would hide exactly the bytes a reviewer must see.
        """
        attributes = (ROOT / ".gitattributes").read_text(encoding="utf-8")
        for path in (
            "/PUBLIC_EXPORT.json",
            "/assets/public_provenance_ledger.json",
            "/assets/public_source_profile.json",
        ):
            with self.subTest(path=path):
                self.assertNotRegex(
                    attributes,
                    rf"(?m)^{re.escape(path)}\s+.*linguist-generated",
                )

    def test_help_skips_parse_time_side_effects(self) -> None:
        """`make help` must not create or invalidate profile stamps or build dirs."""
        self.assertRegex(self.makefile_text, r"(?m)^NK_INFO_ONLY_GOALS := help$")
        self.assertIn(
            "ifeq ($(strip $(filter clean distclean,$(MAKECMDGOALS))$(NK_INFO_ONLY)),)",
            self.makefile_text,
        )
        make = shutil.which("mingw32-make") or shutil.which("make")
        python = shutil.which("python") or shutil.which("python3")
        if not make or not python:
            self.skipTest("GNU Make and Python are required for the behavioural check")
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp) / "help-build"
            proc = subprocess.run(
                [make, "--no-print-directory", "help", f"BUILD_DIR={build_dir.as_posix()}"],
                cwd=ROOT, capture_output=True, text=True, timeout=120,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertIn("help - list every public Make target", proc.stdout)
            self.assertFalse(
                build_dir.exists(),
                "make help created build state: "
                + ", ".join(sorted(p.name for p in build_dir.rglob("*"))),
            )

if __name__ == "__main__":
    unittest.main()
