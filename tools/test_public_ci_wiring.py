# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression checks for public/clean-checkout CI wiring."""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "fixtures" / "showcase"))

import showcase as showcase_cli  # noqa: E402


class PublicCiWiringTests(unittest.TestCase):
    def test_showcase_guest_build_uses_pspdev_compiler(self) -> None:
        demo = {"id": "synthetic-test", "folder": "ge_scene", "target": "fixture_app"}
        with tempfile.TemporaryDirectory(prefix="showcase_cc_") as tmp:
            with mock.patch.object(showcase_cli, "_host_path", side_effect=lambda path: str(path)), \
                    mock.patch.object(showcase_cli.subprocess, "run",
                                      return_value=showcase_cli.subprocess.CompletedProcess([], 1)) as run:
                with self.assertRaises(showcase_cli.ShowcaseError):
                    showcase_cli._build_prx(demo, Path(tmp))
            shell_command = run.call_args.args[0][-1]
            self.assertIn("CC=psp-gcc", shell_command)

    def test_linux_showcase_ci_uses_pinned_runtime_dependencies(self) -> None:
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        lock = json.loads((ROOT / "assets" / "upstream" / "pspdev.lock.json").read_text())
        evidence = json.loads((ROOT / "assets" / "upstream" / "pspdev.evidence.json").read_text())
        self.assertIn("Build pinned SDL3 for headless Linux runtime", ci)
        self.assertIn("fa2c02bb6e21974a89ea9824bc53c9932abe5f9c", ci)
        self.assertIn("libvulkan-dev", ci)
        self.assertIn("SDL_UNIX_CONSOLE_BUILD=ON", ci)
        self.assertIn("pspdev.lock.json", ci)
        self.assertIn("pspdev.evidence.json", ci)
        self.assertIn(lock["distribution"]["archive_sha256"], evidence["archive"]["github_asset_digest"])
        self.assertIn("read -r PSPDEV_URL PSPDEV_SHA256", ci)
        self.assertIn("sha256sum --check --strict", ci)
        self.assertIn("make CC=gcc showcase-linux", ci)
        self.assertIn("NATIVE_RESULT: ${{ needs.native_tools.result }}", ci)
        docs = (ROOT / "docs" / "CI.md").read_text(encoding="utf-8")
        self.assertIn("showcase-linux", docs)

    def test_linux_cmake_player_gate_reuses_pinned_sdl_and_is_required(self) -> None:
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        native_start = ci.index("  native_tools:\n")
        native_end = ci.index("\n  windows_runtime:\n", native_start)
        native_tools = ci[native_start:native_end]

        self.assertIn("Configure, build, and test CMake Linux player", native_tools)
        self.assertIn("Build and run the Linux showcase headlessly", native_tools)
        self.assertIn("Smoke the CMake Linux player headlessly", native_tools)
        sdl_step = native_tools.index("Build pinned SDL3 for headless Linux runtime")
        player_step = native_tools.index("Configure, build, and test CMake Linux player")
        showcase_step = native_tools.index("Build and run the Linux showcase headlessly")
        stage_step = native_tools.index("Smoke the CMake Linux player headlessly")
        self.assertLess(sdl_step, player_step)
        self.assertLess(showcase_step, stage_step)
        self.assertIn("-DBUILD_PLAYER=ON", native_tools)
        self.assertIn('cmake --build "$player_build" --parallel 2', native_tools)
        self.assertIn('ctest --test-dir "$player_build" --output-on-failure', native_tools)
        self.assertIn('"$player_build/nakagawa_player" --help', native_tools)
        self.assertIn('grep -Fq "Usage: nakagawa_player"', native_tools)
        self.assertIn("showcase-scene-v1/TEST00007.iso", native_tools)
        self.assertIn("timeout 60s", native_tools)
        self.assertIn('"$player_build/nakagawa_player" "--iso=$iso" --stage-only', native_tools)
        self.assertIn('grep -Fq "STAGING_RESULT status=PASS"', native_tools)
        self.assertIn("SDL3_VERSION=3.4.16", native_tools)
        self.assertIn("SDL3_COMMIT=fa2c02bb6e21974a89ea9824bc53c9932abe5f9c", native_tools)

        required_start = ci.index("  ci_required:\n")
        required_job = ci[required_start:]
        self.assertIn("needs: [classify, hygiene, markdown, python_tools, native_tools,", required_job)
        self.assertIn("NATIVE_RESULT: ${{ needs.native_tools.result }}", required_job)
        self.assertIn("RUN_NATIVE: ${{ needs.classify.outputs.run_native }}", required_job)
        required = (ROOT / "tools" / "ci_required.py").read_text(encoding="utf-8")
        self.assertIn('(\"native-tools\", \"NATIVE_RESULT\", \"RUN_NATIVE\")', required)

    def test_xb_parser_cmake_target_includes_runtime_headers(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        target = cmake.split("add_executable(test_xb_parser", 1)[1].split(
            "add_test(NAME xb_parser_test", 1
        )[0]
        self.assertIn("${CMAKE_CURRENT_SOURCE_DIR}/src/rt", target)
        self.assertIn("src/rt/archive_vfs.c", target)

    def test_cmake_ctest_creates_its_binary_tree_scratch_directory(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn(
            'file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/build")', cmake
        )
        # The test runs from the source tree (it reads a source-relative
        # fixture) but writes its scratch files into the binary tree, so an
        # out-of-source build never depends on <source>/build existing.
        self.assertIn("set_tests_properties(player_state_test PROPERTIES", cmake)
        self.assertIn("WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}", cmake)
        self.assertIn(
            'ENVIRONMENT "NK_TEST_SCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/build"', cmake
        )
        state_test = (ROOT / "tests" / "native" / "test_player_state.c").read_text(
            encoding="utf-8")
        self.assertIn('getenv("NK_TEST_SCRATCH_DIR")', state_test)
        self.assertNotIn('"build/test_launch_global_profile.json"', state_test)

    def test_sdl3_pin_is_consistent_across_ci_cmake_and_docs(self) -> None:
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        version = re.search(r"^\s*SDL3_VERSION=(\d+\.\d+\.\d+)$", ci, re.MULTILINE)
        commit = re.search(r"^\s*SDL3_COMMIT=([0-9a-f]{40})$", ci, re.MULTILINE)
        self.assertIsNotNone(version, "ci.yml must name the pinned SDL3 version")
        self.assertIsNotNone(commit, "ci.yml must pin SDL3 to a full commit id")
        pinned = version.group(1)
        self.assertIn('test "$(pkg-config --modversion sdl3)" = "$SDL3_VERSION"', ci)

        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        url = re.search(
            r"releases/download/release-(\d+\.\d+\.\d+)/SDL3-(\d+\.\d+\.\d+)\.tar\.gz", cmake)
        self.assertIsNotNone(url, "CMakeLists.txt must bootstrap an official SDL3 release")
        self.assertEqual({url.group(1), url.group(2)}, {pinned})
        self.assertRegex(cmake, r"URL_HASH SHA256=[0-9a-f]{64}\b")
        self.assertIn(f"pinned official release SDL3 {pinned}", cmake)

        for doc in ("CI.md", "SETUP.md", "LINUX_DEVELOPMENT.md"):
            text = (ROOT / "docs" / doc).read_text(encoding="utf-8")
            with self.subTest(doc=doc):
                named = set(re.findall(r"SDL3 (\d+\.\d+\.\d+)|pinned (\d+\.\d+\.\d+)", text))
                versions = {v for pair in named for v in pair if v}
                self.assertTrue(versions, f"docs/{doc} names no SDL3 version")
                self.assertEqual(versions, {pinned})

    def test_windows_vfpu_ci_uses_pregenerated_public_mode(self) -> None:
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        self.assertIn("VFPU_FUZZ_PREGENERATED=1", ci)
        self.assertNotIn("GAME_ELF=tools/vfpu_words.txt", ci)

    def test_makefile_pregenerated_mode_does_not_require_game_elf(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("VFPU_FUZZ_PREGENERATED ?= 0", makefile)
        self.assertIn("ifeq ($(VFPU_FUZZ_PREGENERATED),1)", makefile)
        self.assertIn("VFPU_FUZZ_TITLE_OBJ :=", makefile)
        self.assertIn("VFPU_FUZZ_CHUNK_OBJS :=", makefile)
        self.assertIn("$(MAKE) VFPU_FUZZ_PREGENERATED=1 vfpu_fuzz_build", makefile)
        self.assertIn("--require-synthetic", makefile)
    def test_synthetic_marker_binds_exact_header_bytes(self) -> None:
        from vfpu_fuzz_gen import require_synthetic_cases, write_synthetic_marker

        with tempfile.TemporaryDirectory(prefix="vfpu_fuzz_marker_") as tmp:
            header = Path(tmp) / "cases.h"
            header.write_bytes(b"source-owned synthetic cases\n")
            write_synthetic_marker(header)
            self.assertTrue(require_synthetic_cases(header))
            header.write_bytes(b"different cases\n")
            self.assertFalse(require_synthetic_cases(header))

    def test_windows_build_job_forwards_the_source_commit(self) -> None:
        """Every Windows build job must state the revision its binaries record.

        The Windows runtime compile gate runs its builds in an MSYS2 shell with
        no git on PATH, so the Makefile cannot resolve a revision there. It used
        to log `process_begin: CreateProcess(NULL, git rev-parse HEAD, ...) failed`
        and build every binary of the job with an EMPTY identity
        (flight recorder ``build.build_id``, issue #532). The job now forwards
        the revision the checkout contains, which actions/checkout resolves to
        ``github.sha``, so a Windows-built binary is identified without needing
        git at all.
        """
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        lines = ci.splitlines()
        job_starts = [
            index
            for index, line in enumerate(lines)
            if re.match(r"^ {2}[A-Za-z0-9_]+:\s*$", line)
        ]
        windows_jobs = []
        for position, start in enumerate(job_starts):
            end = job_starts[position + 1] if position + 1 < len(job_starts) else len(lines)
            block = lines[start:end]
            if any("runs-on: windows" in line for line in block):
                windows_jobs.append((lines[start].strip(), block))
        self.assertTrue(windows_jobs, "no Windows job found in ci.yml")
        for name, block in windows_jobs:
            with self.subTest(job=name):
                self.assertTrue(
                    any(line.strip() == "SR_SOURCE_COMMIT: ${{ github.sha }}" for line in block),
                    f"{name} builds on a host with no git on PATH and must forward "
                    "SR_SOURCE_COMMIT explicitly",
                )


if __name__ == "__main__":
    unittest.main()
