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


def _ci_job_blocks() -> dict[str, list[str]]:
    """Map each top-level ci.yml job name to its lines, header included."""
    lines = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8").splitlines()
    starts = [i for i, line in enumerate(lines) if re.match(r"^ {2}[A-Za-z0-9_]+:\s*$", line)]
    blocks = {}
    for position, start in enumerate(starts):
        end = starts[position + 1] if position + 1 < len(starts) else len(lines)
        blocks[lines[start].strip().rstrip(":")] = lines[start:end]
    return blocks


def _ci_step_index(block: list[str], name: str) -> int:
    for index, line in enumerate(block):
        if line.strip() == f"- name: {name}":
            return index
    raise AssertionError(f"step {name!r} not found")


def _ci_step(block: list[str], name: str) -> list[str] | None:
    """The lines of one step, from its `- name:` line to the next step."""
    try:
        start = _ci_step_index(block, name)
    except AssertionError:
        return None
    indent = len(block[start]) - len(block[start].lstrip())
    end = start + 1
    while end < len(block):
        line = block[end]
        if line.strip().startswith("- ") and len(line) - len(line.lstrip()) == indent:
            break
        end += 1
    return block[start:end]


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
        windows_jobs = [
            (f"{name}:", block)
            for name, block in _ci_job_blocks().items()
            if any("runs-on: windows" in line for line in block)
        ]
        self.assertTrue(windows_jobs, "no Windows job found in ci.yml")
        for name, block in windows_jobs:
            with self.subTest(job=name):
                self.assertTrue(
                    any(line.strip() == "SR_SOURCE_COMMIT: ${{ github.sha }}" for line in block),
                    f"{name} builds on a host with no git on PATH and must forward "
                    "SR_SOURCE_COMMIT explicitly",
                )

    def test_provenance_record_gap_check_is_wired_in_ci_and_make(self) -> None:
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        precommit = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")

        self.assertIn("python tools/provenance_record_gap.py --check", ci)
        self.assertIn("tools/provenance_record_gap.py --check", makefile)
        self.assertIn("python tools/provenance_record_gap.py --check", precommit)

    def test_player_ui_regressions_ci_wiring(self) -> None:
        jobs = _ci_job_blocks()
        for name in ("native_tools", "windows_runtime"):
            self.assertIn(name, jobs, f"ci.yml no longer has a {name} job")

        native_tools = jobs["native_tools"]
        native_step = _ci_step(native_tools, "Run native player UI regressions headlessly")
        self.assertIsNotNone(native_step, "native_tools no longer runs player-ui-regressions")
        self.assertIn("run: make CC=gcc player-ui-regressions", "\n".join(native_step))
        self.assertLess(
            _ci_step_index(native_tools, "Build pinned SDL3 for headless Linux runtime"),
            _ci_step_index(native_tools, "Run native player UI regressions headlessly"),
            "player-ui-regressions must run after the pinned SDL3 build",
        )

        windows_step = _ci_step(
            jobs["windows_runtime"], "Run native player UI regressions headlessly on Windows"
        )
        self.assertIsNotNone(windows_step, "windows_runtime no longer runs player-ui-regressions")
        windows_text = "\n".join(windows_step)
        self.assertIn(
            "mingw32-make --no-print-directory CC=gcc VULKAN_SDK=/ucrt64 player-ui-regressions",
            windows_text,
        )
        # Exactly one Windows matrix part runs the UI regressions.
        self.assertIn("if: matrix.part == 'smoke'", windows_text)

        # The harness, not the workflow, makes the gate headless: it forces the
        # drivers for every player it spawns.
        harness = (ROOT / "tests" / "native" / "test_player_ui.py").read_text(encoding="utf-8")
        self.assertIn('"SDL_VIDEODRIVER": "dummy"', harness)
        self.assertIn('"SDL_RENDER_DRIVER": "software"', harness)

        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("player-ui-regressions: $(PLAYER_UI_TEST_EXE)", makefile)
        docs = (ROOT / "docs" / "CI.md").read_text(encoding="utf-8")
        self.assertIn("player-ui-regressions", docs)


# Every gate the serial windows_runtime job ran before it was split into parallel
# parts (#694), with the command that makes it that gate. The split may move a gate
# between parts; it may never drop one, run one twice, or change its command.
_WINDOWS_SERIAL_GATES = {
    "Build and run full production pipeline smoke": "VULKAN_SDK=/ucrt64 production-smoke",
    "Build and run AOT-gap dispatch-seam smoke": "VULKAN_SDK=/ucrt64 production-smoke-gap",
    "Run native core test suite on Windows": "mingw32-make --no-print-directory CC=gcc native-core-tests",
    "Build and link the native player": "VULKAN_SDK=/ucrt64 player-ui-tests",
    "Run native player UI regressions headlessly on Windows": "VULKAN_SDK=/ucrt64 player-ui-regressions",
    "Stage and run production smoke outside its build directory": "VULKAN_SDK=/ucrt64 production-smoke-staged",
    "Build and run the full platform ladder": "VULKAN_SDK=/ucrt64 platform-ladder",
    "Build and run profile-zero production routes": "VULKAN_SDK=/ucrt64 profile-zero-e2e",
    "Build and run display presentation smoke": "VULKAN_SDK=/ucrt64 display-smoke-run",
    "Build and run AOT/interpreter cosimulation gate": "VULKAN_SDK=/ucrt64 cosim-selftest",
    "Prove the cosimulation comparator is load-bearing": "VULKAN_SDK=/ucrt64 cosim-mutants",
    "Build and run scheduler selftest": "CC=gcc VULKAN_SDK=/ucrt64 sched-selftest",
    "Build and run dispatch isolation selftest matrix": "CC=gcc VULKAN_SDK=/ucrt64 dispatch-isolation-selftest",
    "Build and run portable FPU/VFPU conversion selftest": "CC=gcc VULKAN_SDK=/ucrt64 fp-convert-selftest",
    "Build and run strbuf checked-append formatting selftest": "CC=gcc VULKAN_SDK=/ucrt64 strbuf-selftest",
    "Build and run HLE thread selftest": "CC=gcc VULKAN_SDK=/ucrt64 hle-thread-selftest",
    "Build and run synthetic VFPU fuzzer": "VFPU_FUZZ_PREGENERATED=1 CC=gcc VULKAN_SDK=/ucrt64 -j4 vfpu_fuzz_build",
}
# Steps every part repeats: the toolchain setup and the runtime-object compile gate.
_WINDOWS_EVERY_PART = (
    "Check out repository",
    "Set up MSYS2 UCRT64",
    "Pin the Windows Python toolchain (MSYS2 UCRT64 CPython)",
    "Verify default and override compiler resolution",
    "Compile Windows runtime objects through the default compiler path",
)


class WindowsRuntimePartitionTests(unittest.TestCase):
    """#694: splitting windows_runtime into parallel parts must not weaken it."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.block = _ci_job_blocks()["windows_runtime"]
        text = "\n".join(cls.block)
        matrix = re.search(r"(?m)^\s+part:\s*\[([^\]]*)\]\s*$", text)
        if matrix is None:
            raise AssertionError("windows_runtime must declare its matrix parts explicitly")
        cls.parts = [part.strip() for part in matrix.group(1).split(",")]
        step_indent = "      - name: "
        cls.steps = {
            line[len(step_indent):]: _ci_step(cls.block, line[len(step_indent):])
            for line in cls.block
            if line.startswith(step_indent)
        }

    def part_of(self, name: str) -> str | None:
        conditions = [line.strip() for line in self.steps[name] if line.strip().startswith("if:")]
        if not conditions:
            return None
        self.assertEqual(len(conditions), 1, name)
        match = re.fullmatch(r"if: matrix\.part == '([A-Za-z0-9_-]+)'", conditions[0])
        self.assertIsNotNone(match, f"{name}: a part condition must name exactly one part")
        return match.group(1)

    def test_every_serial_gate_runs_in_exactly_one_declared_part_with_its_command(self) -> None:
        self.assertEqual(len(self.parts), len(set(self.parts)))
        for name, command in _WINDOWS_SERIAL_GATES.items():
            with self.subTest(step=name):
                self.assertIn(name, self.steps, "a serial Windows gate was dropped")
                self.assertIn(command, " ".join(line.strip() for line in self.steps[name]))
                self.assertIn(self.part_of(name), self.parts)

    def test_shared_steps_run_in_every_part_and_nothing_else_is_unconditional(self) -> None:
        for name in self.steps:
            with self.subTest(step=name):
                if name in _WINDOWS_EVERY_PART:
                    self.assertIsNone(self.part_of(name), f"{name} must run in every part")
                else:
                    self.assertIn(name, _WINDOWS_SERIAL_GATES, f"unclassified Windows step {name!r}")
        compile_step = " ".join(self.steps["Compile Windows runtime objects through the default compiler path"])
        self.assertIn("runtime-objects", compile_step)

    def test_every_part_runs_a_gate_and_dependent_steps_share_a_part(self) -> None:
        used = {self.part_of(name) for name in _WINDOWS_SERIAL_GATES}
        self.assertEqual(used, set(self.parts), "a declared part runs no gate, or a gate names no part")
        # The staged smoke re-runs generate/verify/run over the production smoke's build.
        self.assertEqual(
            self.part_of("Stage and run production smoke outside its build directory"),
            self.part_of("Build and run full production pipeline smoke"),
        )
        self.assertLess(
            _ci_step_index(self.block, "Build and run full production pipeline smoke"),
            _ci_step_index(self.block, "Stage and run production smoke outside its build directory"),
        )


if __name__ == "__main__":
    unittest.main()
