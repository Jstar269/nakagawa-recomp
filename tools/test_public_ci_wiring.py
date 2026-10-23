# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression checks for public/clean-checkout CI wiring."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "fixtures" / "showcase"))

import ci_required  # noqa: E402
import ci_paths  # noqa: E402
import ci_test_shards  # noqa: E402
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


def _makefile_public_safe_arms(makefile: str) -> tuple[str, str]:
    """The private (PUBLIC_SAFE=0) and public-safe (PUBLIC_SAFE=1) arms of the Makefile switch."""
    start = makefile.index("ifeq ($(PUBLIC_SAFE),0)\n")
    else_at = makefile.index("\nelse\n", start)
    end = makefile.index("\nendif\n", else_at)
    return makefile[start:else_at], makefile[else_at:end]


def _cmake_sources_and_definitions() -> tuple[dict[str, set[str]], dict[str, set[str]]]:
    """Each CMake target's compiled .c files and the compile definitions it sets.

    A light reader for the calls this build uses: set() and list(APPEND) build the source
    lists, and every branch counts, so a conditional source still binds the check.
    add_library, add_executable and target_compile_definitions name the targets.
    """
    text = re.sub(r"#.*", "", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
    variables: dict[str, list[str]] = {}
    for name, body in re.findall(r"\bset\((\w+)\s+([^()]*)\)", text):
        variables.setdefault(name, []).extend(body.split())
    for name, body in re.findall(r"\blist\(APPEND\s+(\w+)\s+([^()]*)\)", text):
        variables.setdefault(name, []).extend(body.split())

    def expand(tokens: list[str]) -> list[str]:
        expanded: list[str] = []
        for token in tokens:
            ref = re.fullmatch(r"\$\{(\w+)\}", token)
            expanded.extend(variables.get(ref.group(1), []) if ref else [token])
        return expanded

    sources: dict[str, set[str]] = {}
    for target, body in re.findall(r"\badd_(?:library|executable)\((\w+)([^()]*)\)", text):
        sources[target] = {token for token in expand(body.split()) if token.endswith(".c")}
    definitions: dict[str, set[str]] = {}
    for target, body in re.findall(r"\btarget_compile_definitions\((\w+)([^()]*)\)", text):
        words = [word for word in body.split() if word not in {"PUBLIC", "PRIVATE", "INTERFACE"}]
        definitions.setdefault(target, set()).update(words)
    return sources, definitions


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
        self.assertIn("829a65d769d935c4852f8159e964312c0957260a", ci)
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

    def test_ge_frontend_profile_variant_is_compiled_in_ci(self) -> None:
        """The opt-in GE instrumentation build must compile ge.c in isolation."""
        native_tools = _ci_job_blocks()["native_tools"]
        step_name = "Compile GE frontend profiling mode"
        step = _ci_step(native_tools, step_name)
        self.assertIsNotNone(step, "native_tools must compile the GE profile variant")
        assert step is not None
        text = "\n".join(step)
        self.assertIn('build_dir="$RUNNER_TEMP/ge-frontend-profile"', text)
        self.assertIn("GE_FRONTEND_PROFILE=1", text)
        self.assertIn('"$build_dir/ge.o"', text)
        self.assertLess(
            _ci_step_index(native_tools, "Install the pinned SDL3 into the Linux prefix"),
            _ci_step_index(native_tools, step_name),
        )

        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("GE_FRONTEND_PROFILE ?= 0", makefile)
        self.assertIn("$(RT_GE_O): src/rt/ge.c", makefile)
        self.assertIn("$(CC) $(GE_CFLAGS)", makefile)

    def test_ffmpeg_h264_media_selftest_is_wired_in_linux_ci(self) -> None:
        """The optional H.264 backend must build and execute its real media fixture in CI."""
        native_tools = _ci_job_blocks()["native_tools"]
        self.assertIn("    runs-on: ubuntu-24.04", "\n".join(native_tools))
        dependencies = _ci_step(native_tools, "Install native test dependencies")
        ffmpeg_test = _ci_step(native_tools, "Build and run the opt-in FFmpeg H.264 media selftest")
        self.assertIsNotNone(dependencies)
        self.assertIsNotNone(ffmpeg_test)
        assert dependencies is not None and ffmpeg_test is not None
        dependency_text = "\n".join(dependencies)
        self.assertIn("libavcodec-dev", dependency_text)
        self.assertIn("libavutil-dev", dependency_text)
        self.assertIn("pkg-config", dependency_text)
        test_text = "\n".join(ffmpeg_test)
        self.assertIn("H264_FFMPEG=1 BUILD_ROOT=\"$RUNNER_TEMP/ffmpeg-h264-build\"", test_text)
        self.assertIn("psmf-media-selftest", test_text)
        self.assertLess(
            _ci_step_index(native_tools, "Install the pinned SDL3 into the Linux prefix"),
            _ci_step_index(native_tools, "Build and run the opt-in FFmpeg H.264 media selftest"),
        )

    def test_failed_linux_showcase_smoke_uploads_its_runtime_log(self) -> None:
        """A failed smoke names runtime.log; the run must keep it to diagnose a flake."""
        native_tools = _ci_job_blocks()["native_tools"]
        smoke = _ci_step(native_tools, "Build and run the Linux showcase headlessly")
        upload = _ci_step(native_tools, "Upload the Linux showcase smoke evidence on failure")
        self.assertIsNotNone(smoke)
        self.assertIsNotNone(upload)
        assert smoke is not None and upload is not None
        self.assertIn("id: showcase", "\n".join(smoke))
        self.assertLess(
            _ci_step_index(native_tools, "Build and run the Linux showcase headlessly"),
            _ci_step_index(native_tools, "Upload the Linux showcase smoke evidence on failure"),
        )
        text = "\n".join(upload)
        self.assertIn("if: failure() && steps.showcase.outcome == 'failure'", text)
        self.assertRegex(text, r"uses: actions/upload-artifact@[0-9a-f]{40} # v\d")
        self.assertIn("${{ runner.temp }}/showcase-build/smoke/", text)
        self.assertIn("${{ runner.temp }}/showcase-demo/packages/*/snap_f*.ppm", text)

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
        # The stage run must redirect all four XDG roots under RUNNER_TEMP, so it never
        # writes config, cache or state under the runner's HOME.
        stage_lines = _ci_step(native_tools.splitlines(), "Smoke the CMake Linux player headlessly")
        self.assertIsNotNone(stage_lines, "Smoke the CMake Linux player headlessly is not pinned")
        assert stage_lines is not None
        stage_text = "\n".join(stage_lines)
        invocation = stage_text[
            stage_text.index("stage_output=$(") : stage_text.index("--stage-only)") + len("--stage-only)")
        ]
        for assignment in (
            'XDG_DATA_HOME="$RUNNER_TEMP/player-smoke-data"',
            'XDG_CONFIG_HOME="$RUNNER_TEMP/player-smoke-config"',
            'XDG_CACHE_HOME="$RUNNER_TEMP/player-smoke-cache"',
            'XDG_STATE_HOME="$RUNNER_TEMP/player-smoke-state"',
        ):
            self.assertIn(assignment, invocation)
        self.assertIn("SDL3_VERSION=3.4.18", native_tools)
        self.assertIn("SDL3_COMMIT=829a65d769d935c4852f8159e964312c0957260a", native_tools)

        required_start = ci.index("  ci_required:\n")
        required_job = ci[required_start:]
        self.assertIn("needs: [classify, hygiene, public_export, markdown, python_tools, native_tools,", required_job)
        self.assertIn("PUBLIC_EXPORT_RESULT: ${{ needs.public_export.result }}", required_job)
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

    def test_public_safe_define_agrees_between_cmake_and_makefile(self) -> None:
        """SR_PUBLIC_SAFE marks the public-backend build in both build files.

        The Makefile's PUBLIC_SAFE=1 arm selects the public backends and defines the macro for
        every object; the PUBLIC_SAFE=0 arm uses the private backends and must not define it.
        CMake has no private arm, so every target that compiles a public backend must define the
        macro, and no CMake target may compile a private backend. This test does not cover the
        guarded code itself: src/rt/driver.c and src/rt/gui.c are in no CMake target.
        """
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        private_arm, public_arm = _makefile_public_safe_arms(makefile)
        self.assertRegex(public_arm, r"(?m)^override CFLAGS \+= -DSR_PUBLIC_SAFE$")
        self.assertNotIn("SR_PUBLIC_SAFE", private_arm)
        public_backends = set(re.findall(r"(?m)^\w+_BACKEND_SRC := (\S+)$", public_arm))
        private_backends = set(re.findall(r"(?m)^\w+_BACKEND_SRC := (\S+)$", private_arm))
        self.assertIn("src/rt/pgf_public.c", public_backends)
        self.assertFalse(public_backends & private_backends)

        sources, definitions = _cmake_sources_and_definitions()
        public_targets = {target: files & public_backends
                          for target, files in sources.items() if files & public_backends}
        self.assertTrue(public_targets,
                        "CMakeLists.txt compiles no public backend; the reader or the build changed")
        for target, files in sorted(public_targets.items()):
            with self.subTest(target=target):
                self.assertIn("SR_PUBLIC_SAFE", definitions.get(target, set()),
                              f"{target} compiles {sorted(files)} without SR_PUBLIC_SAFE")
        for target, files in sorted(sources.items()):
            with self.subTest(target=target):
                self.assertFalse(files & private_backends,
                                 f"{target} compiles a private backend, and CMake has no private arm")

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
    "Smoke the Windows player staging headlessly": '"--iso=$smoke/disc/TEST00007.iso" --stage-only)',
    "Stage and run production smoke outside its build directory": "VULKAN_SDK=/ucrt64 production-smoke-staged",
    "Build and run the full platform ladder": "VULKAN_SDK=/ucrt64 platform-ladder",
    "Build and run profile-zero production routes": "VULKAN_SDK=/ucrt64 profile-zero-e2e",
    "Fetch the profile-zero fixture built from source": "name: profile-zero-fixture",
    "Build and run display presentation smoke": "VULKAN_SDK=/ucrt64 display-smoke-run",
    "Build and run AOT/interpreter cosimulation gate": "VULKAN_SDK=/ucrt64 cosim-selftest",
    "Prove the cosimulation comparator is load-bearing": "VULKAN_SDK=/ucrt64 cosim-mutants",
    "Build and run scheduler selftest": "CC=gcc VULKAN_SDK=/ucrt64 sched-selftest",
    "Build and run dispatch isolation selftest matrix": "CC=gcc VULKAN_SDK=/ucrt64 dispatch-isolation-selftest",
    "Build and run portable FPU/VFPU conversion selftest": "CC=gcc VULKAN_SDK=/ucrt64 fp-convert-selftest",
    "Build and run strbuf checked-append formatting selftest": "CC=gcc VULKAN_SDK=/ucrt64 strbuf-selftest",
    "Build and run HLE thread selftest": "CC=gcc VULKAN_SDK=/ucrt64 hle-thread-selftest",
    "Build and run flash0 font device selftest": "PYTHONPATH=tools python -m unittest -v test_flash0_font",
    "Build and run synthetic VFPU fuzzer (shard 0 of 2)": "VFPU_FUZZ_PREGENERATED=1 CC=gcc VULKAN_SDK=/ucrt64 -j4 vfpu_fuzz_build",
    "Build and run synthetic VFPU fuzzer (shard 1 of 2)": "VFPU_FUZZ_PREGENERATED=1 CC=gcc VULKAN_SDK=/ucrt64 -j4 vfpu_fuzz_build",
    "Prove the fuzz shards cover every case exactly once": "NK_VFPU_FUZZ_BIN=build/windows-runtime/vfpu_fuzz.exe python -m unittest -v test_public_ci_wiring.VfpuFuzzShardTests",
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


class WindowsPlayerStageSmokeTests(unittest.TestCase):
    """#308: the Windows player's headless staging smoke, the counterpart of the Linux gate.

    The Linux native_tools job stages a TEST00007 disc through the built player's
    --stage-only route. The Windows job builds the same player but never ran that route,
    so a Windows-only staging regression reached no hosted gate.
    """

    STEP = "Smoke the Windows player staging headlessly"

    @classmethod
    def setUpClass(cls) -> None:
        cls.block = _ci_job_blocks()["windows_runtime"]
        cls.step = _ci_step(cls.block, cls.STEP)
        cls.text = "\n".join(cls.step or [])

    def test_step_runs_in_the_smoke_part_after_the_player_is_built(self) -> None:
        self.assertIsNotNone(self.step, f"windows_runtime no longer runs {self.STEP!r}")
        self.assertIn("if: matrix.part == 'smoke'", self.text)
        self.assertIn("shell: msys2 {0}", self.text)
        index = _ci_step_index(self.block, self.STEP)
        for earlier in ("Build and link the native player",
                        "Run native player UI regressions headlessly on Windows"):
            self.assertLess(_ci_step_index(self.block, earlier), index)
        # It runs the executable the link step built; it does not build a second one.
        self.assertIn("build/nakagawa_player.exe", self.text)
        self.assertNotIn("mingw32-make", self.text)

    def test_step_stages_a_test00007_disc_through_the_stage_only_route(self) -> None:
        self.assertIn("python fixtures/showcase/showcase.py stage-iso --out-dir", self.text)
        self.assertIn('"--iso=$smoke/disc/TEST00007.iso" --stage-only)', self.text)
        self.assertIn('grep -Fq "STAGING_RESULT status=PASS"', self.text)
        self.assertIn("timeout 60s", self.text)
        self.assertIn("SDL_VIDEODRIVER=offscreen", self.text)

    def test_step_redirects_every_user_data_root_under_runner_temp(self) -> None:
        self.assertIn('smoke="$(cygpath -m "$RUNNER_TEMP")/windows-stage-smoke"', self.text)
        invocation = self.text[
            self.text.index("stage_output=$(") : self.text.index("--stage-only)")
        ]
        for assignment in (
            'LOCALAPPDATA="$smoke/localappdata"',
            'APPDATA="$smoke/appdata"',
            'USERPROFILE="$smoke/userprofile"',
        ):
            self.assertIn(assignment, invocation)
        for directory in ("disc", "localappdata", "appdata", "userprofile"):
            self.assertIn(f'"$smoke/{directory}"', self.text)

    def test_disc_writer_action_is_the_one_the_step_calls(self) -> None:
        showcase = (ROOT / "fixtures" / "showcase" / "showcase.py").read_text(encoding="utf-8")
        self.assertIn('choices=("build", "smoke", "stage-iso")', showcase)
        self.assertIn("def stage_iso(out_dir: Path) -> Path:", showcase)


class VfpuFuzzShardTests(unittest.TestCase):
    """The synthetic VFPU fuzzer's two shards admit every case exactly once.

    The source and workflow checks run everywhere. The binary checks run only when
    NK_VFPU_FUZZ_BIN names a built vfpu_fuzz binary (the Windows fuzz-0 part builds it and
    runs them); elsewhere they report SKIP, never a silent pass.
    """

    SOURCE = ROOT / "src" / "rt" / "vfpu_fuzz.c"
    PROGRESS = re.compile(r"^FUZZ_PROGRESS case=(\d+) ", re.MULTILINE)
    SUMMARY = re.compile(r"^vfpu_fuzz: (\d+)/(\d+) distinct words tested", re.MULTILINE)
    MALFORMED = (
        "", "1", "0/0", "2/2", "3/2", "-1/2", "0/-2", "a/2", "1/b", "0/2/3", " 0/2", "0/ 2",
        "+0/2", "0/1234567890", "99999999999/2",
        "5000/5001",  # well-formed, but admits no case of the corpus
    )

    def test_source_keeps_the_default_gate_and_refuses_bad_shards(self) -> None:
        source = self.SOURCE.read_text(encoding="utf-8")
        self.assertIn('#define FUZZ_SHARD_ENV "FUZZ_CASE_SHARD"', source)
        self.assertIn("if (shard_spec != NULL && !fuzz_parse_shard(shard_spec, &shard_k, &shard_n)) {", source)
        self.assertIn("return *n >= 1u && *k < *n;", source)
        self.assertIn("if (admitted == 0) {", source)
        self.assertIn("if ((unsigned)c % shard_n != shard_k) continue;", source)
        self.assertIn("return (bad_cases || skipped || tested != admitted) ? 1 : 0;", source)

    def test_ci_runs_one_shard_per_fuzz_part(self) -> None:
        block = _ci_job_blocks()["windows_runtime"]
        for part, index in (("fuzz-0", 0), ("fuzz-1", 1)):
            with self.subTest(part=part):
                lines = _ci_step(block, f"Build and run synthetic VFPU fuzzer (shard {index} of 2)")
                self.assertIsNotNone(lines, f"no fuzz step for {part}")
                text = "\n".join(lines)
                self.assertIn(f"if: matrix.part == '{part}'", text)
                self.assertIn(f'FUZZ_CASE_SHARD: "{index}/2"', text)
        proof = _ci_step(block, "Prove the fuzz shards cover every case exactly once")
        self.assertIsNotNone(proof, "the shard proof step is missing")
        self.assertIn("if: matrix.part == 'fuzz-0'", "\n".join(proof))

    def test_ci_required_aggregates_every_windows_part(self) -> None:
        required = "\n".join(_ci_job_blocks()["ci_required"])
        self.assertIn("needs: [classify, hygiene, public_export, markdown, python_tools, native_tools, windows_runtime, main_smoke]", required)
        self.assertIn("WINDOWS_RESULT: ${{ needs.windows_runtime.result }}", required)
        self.assertIn("RUN_WINDOWS: ${{ needs.classify.outputs.run_windows }}", required)

    def _fuzz(self, binary: Path, shard: str | None) -> subprocess.CompletedProcess[str]:
        env = {key: value for key, value in os.environ.items() if key not in ("FUZZ_CASE_SHARD", "FUZZ_SEED")}
        env["FUZZ_TRIALS"] = "1"  # case selection does not depend on the trial count
        if shard is not None:
            env["FUZZ_CASE_SHARD"] = shard
        return subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=900, check=False)

    @unittest.skipUnless(os.environ.get("NK_VFPU_FUZZ_BIN"),
                         "NK_VFPU_FUZZ_BIN does not name a built vfpu_fuzz binary")
    def test_binary_shards_cover_every_case_exactly_once(self) -> None:
        binary = Path(os.environ["NK_VFPU_FUZZ_BIN"]).resolve()
        self.assertTrue(binary.is_file(), f"{binary} is not built")
        full = self._fuzz(binary, None)
        self.assertIn(full.returncode, (0, 1), full.stderr[-2000:])
        summary = self.SUMMARY.search(full.stdout)
        self.assertIsNotNone(summary, "the default run printed no summary")
        total = int(summary.group(2))
        default_cases = sorted(int(case) for case in self.PROGRESS.findall(full.stdout))
        self.assertEqual(default_cases, list(range(total)), "the default run did not test every case")
        self.assertEqual(int(summary.group(1)), total)

        covered: list[int] = []
        for shard_index in (0, 1):
            with self.subTest(shard=shard_index):
                run = self._fuzz(binary, f"{shard_index}/2")
                self.assertIn(run.returncode, (0, 1), run.stderr[-2000:])
                shard = self.SUMMARY.search(run.stdout)
                self.assertIsNotNone(shard, "the shard run printed no summary")
                cases = [int(case) for case in self.PROGRESS.findall(run.stdout)]
                self.assertEqual(int(shard.group(1)), len(cases), "a shard tested fewer cases than it printed")
                self.assertEqual(int(shard.group(2)), len(cases), "a shard admitted a count it did not run")
                self.assertTrue(all(case % 2 == shard_index for case in cases),
                                "a shard ran a case it does not admit")
                covered.extend(cases)
        self.assertEqual(sorted(covered), default_cases,
                         "the shards must together run every case exactly once")

    @unittest.skipUnless(os.environ.get("NK_VFPU_FUZZ_BIN"),
                         "NK_VFPU_FUZZ_BIN does not name a built vfpu_fuzz binary")
    def test_binary_refuses_malformed_or_empty_shards_before_any_case(self) -> None:
        binary = Path(os.environ["NK_VFPU_FUZZ_BIN"]).resolve()
        self.assertTrue(binary.is_file(), f"{binary} is not built")
        for spec in self.MALFORMED:
            with self.subTest(spec=spec):
                run = self._fuzz(binary, spec)
                self.assertEqual(run.returncode, 2, run.stdout[-500:] + run.stderr[-500:])
                self.assertIn("FUZZ_CASE_SHARD", run.stderr)
                self.assertNotIn("FUZZ_PROGRESS", run.stdout)


class CodeqlWakeWiringTests(unittest.TestCase):
    """The trusted CodeQL trigger names the wake workflow, and nothing else may share its name."""

    WORKFLOWS = ROOT / ".github" / "workflows"

    @staticmethod
    def _top_name(text: str) -> str | None:
        match = re.search(r"(?m)^name:[ \t]*(.+?)[ \t]*$", text)
        return match.group(1) if match else None

    def test_trigger_names_the_wake_workflow_exactly(self) -> None:
        wake = (self.WORKFLOWS / "codeql-wake.yml").read_text(encoding="utf-8")
        codeql = (self.WORKFLOWS / "codeql.yml").read_text(encoding="utf-8")
        name = self._top_name(wake)
        self.assertEqual(name, "CodeQL wake")
        self.assertIn(f'workflows: ["{name}"]', codeql)

    def test_no_other_workflow_shares_the_trigger_name(self) -> None:
        name = self._top_name((self.WORKFLOWS / "codeql-wake.yml").read_text(encoding="utf-8"))
        for path in sorted(self.WORKFLOWS.glob("*.y*ml")):
            if path.name == "codeql-wake.yml":
                continue
            with self.subTest(workflow=path.name):
                self.assertNotEqual(self._top_name(path.read_text(encoding="utf-8")), name)

    def test_docs_describe_the_wake_trigger(self) -> None:
        docs = (ROOT / "docs" / "CI.md").read_text(encoding="utf-8")
        blocks = _markdown_blocks(docs)
        self.assertTrue(
            any(
                ".github/workflows/codeql-wake.yml" in block
                for block in blocks
            ),
            "docs/CI.md must name the maintained CodeQL wake workflow path",
        )
        self.assertTrue(
            any(
                all(term in block.lower() for term in ("workflow_run", "codeql wake", "default branch"))
                for block in blocks
            ),
            "docs/CI.md must state that the trusted workflow runs from the default branch",
        )
        self.assertTrue(
            any(
                all(term in block.lower() for term in ("ci itself is not a trigger", "does not wait"))
                for block in blocks
            ),
            "docs/CI.md must state that CodeQL does not wait for CI to finish",
        )


class PublicExportJobTests(unittest.TestCase):
    """The public-export audit is its own job, and no hygiene command was dropped (#735 item 5).

    The all-files checks and the candidate export used to run serially in one job. Splitting
    them runs both in parallel. The split may move a command between jobs; it may never drop
    one, run one twice, or change its command or its condition.
    """

    HYGIENE_STEPS = (
        "Check out repository",
        "Set up Python",
        "Install pre-commit",
        "Run shared all-files checks",
        "Enforce tracked-tree reference ratchet",
        "Locate the pinned Betterleaks binary",
        "Scan reachable sanitized Git history with Betterleaks",
        "Run Betterleaks synthetic canaries",
    )
    EXPORT_STEP = "Exercise public-export generation and candidate audit"

    def setUp(self) -> None:
        self.blocks = _ci_job_blocks()

    @staticmethod
    def _step_names(block: list[str]) -> list[str]:
        return [line.strip()[len("- name: "):] for line in block if line.strip().startswith("- name: ")]

    def test_hygiene_keeps_every_check_it_ran_before(self) -> None:
        self.assertEqual(self._step_names(self.blocks["hygiene"]), list(self.HYGIENE_STEPS))
        self.assertIn("pre-commit run --all-files --show-diff-on-failure",
                      "\n".join(self.blocks["hygiene"]))

    def test_pre_commit_bootstrap_uses_a_hash_locked_closure(self) -> None:
        step = _ci_step(self.blocks["hygiene"], "Install pre-commit")
        self.assertIsNotNone(step)
        text = "\n".join(step or [])
        self.assertIn('--require-hashes', text)
        self.assertIn('pre-commit-requirements.txt', text)
        match = re.search(r"<<'LOCK'\n(?P<lock>.*?)\n\s*LOCK", text, re.DOTALL)
        self.assertIsNotNone(match)
        lock = match.group("lock") if match is not None else ""
        requirements = [line.strip().rstrip("\\").strip()
                        for line in lock.splitlines() if "==" in line]
        package_versions = {
            line.split("==", 1)[0].lower(): line.split("==", 1)[1]
            for line in requirements
        }
        self.assertEqual(package_versions, {
            "cfgv": "3.5.0",
            "distlib": "0.4.4",
            "filelock": "4.2.4",
            "identify": "2.6.20",
            "nodeenv": "1.11.0",
            "packaging": "26.3",
            "platformdirs": "4.13.5",
            "pre-commit": "4.6.2",
            "pyyaml": "6.0.3",
            "python-discovery": "1.6.4",
            "virtualenv": "21.14.11",
        })
        self.assertEqual(lock.count("--hash=sha256:"), len(package_versions))

    def test_python_tools_step_installs_only_the_hashed_lock(self) -> None:
        step = _ci_step(self.blocks["python_tools"], "Install Python tooling dependencies")
        self.assertIsNotNone(step)
        installs = [line for line in step or [] if "pip install" in line]
        self.assertEqual(len(installs), 1)
        self.assertIn("--require-hashes -r tools/requirements-lock.txt", installs[0])
        # Building the checkout would pull an unhashed setuptools through build isolation.
        self.assertNotIn("--no-deps .", "\n".join(step or []))

    def test_export_step_moved_out_of_hygiene_into_its_own_job(self) -> None:
        self.assertNotIn(self.EXPORT_STEP, self._step_names(self.blocks["hygiene"]))
        self.assertIn(self.EXPORT_STEP, self._step_names(self.blocks["public_export"]))

    def test_export_job_keeps_the_candidate_audit_condition_and_command(self) -> None:
        export = "\n".join(_ci_step(self.blocks["public_export"], self.EXPORT_STEP) or [])
        self.assertIn("security_publication == 'true' ||", export)
        self.assertIn("github.event_name == 'workflow_dispatch'", export)
        self.assertIn("python tools/build_public_export.py --export-dir", export)
        self.assertIn("--public-safe-profile 2>&1", export)
        self.assertIn("PROVENANCE_UNVERIFIED", export)
        job = "\n".join(self.blocks["public_export"])
        self.assertIn("fetch-depth: 0", job)
        self.assertIn('python-version: "3.14.9"', job)

    def test_public_export_job_is_ungated_by_path(self) -> None:
        block = self.blocks["public_export"]
        self.assertIn("    if: ${{ always() && needs.classify.result == 'success' }}", block)
        self.assertIn("    needs: classify", block)
        text = "\n".join(block)
        for gate in ("run_python", "run_native", "run_windows", "run_markdown", "run_main_smoke",
                     "docs_only", "allow_substantive"):
            self.assertNotIn(gate, text, "public_export must not be path-gated on %s" % gate)

    def test_every_platform_gate_waits_for_the_export_result(self) -> None:
        # main_smoke runs only on main pushes and is not a substantive gate: it keeps hygiene.
        block = "\n".join(self.blocks["main_smoke"])
        self.assertIn("needs: [classify, hygiene, public_export]", block)
        self.assertIn("needs.public_export.result == 'success'", block)
        for job in ("python_tools", "native_tools", "windows_runtime"):
            with self.subTest(job=job):
                block = "\n".join(self.blocks[job])
                self.assertRegex(
                    block, r"(?m)^    needs: \[classify, public_export(, profile_zero_fixture)?\]$"
                )
                needs_line = re.search(r"(?m)^    needs: (.+)$", block)
                self.assertIsNotNone(needs_line, f"{job} declares no needs line")
                self.assertNotIn("hygiene", needs_line.group(1))
                self.assertNotIn("needs.hygiene.result", block)
                self.assertIn("needs.public_export.result == 'success'", block)

    def test_profile_zero_fixture_is_built_from_source_for_its_consumers(self) -> None:
        """#1100: the fixture is built and digest-checked in CI, never committed."""
        fixture = "\n".join(self.blocks["profile_zero_fixture"])
        self.assertIn("sha256sum --check --strict SHA256SUMS", fixture)
        self.assertIn("name: profile-zero-fixture", fixture)
        self.assertIn("if-no-files-found: error", fixture)
        for job in ("python_tools", "windows_runtime"):
            with self.subTest(job=job):
                block = "\n".join(self.blocks[job])
                self.assertIn("needs.profile_zero_fixture.result == 'success'", block)
                self.assertIn("name: profile-zero-fixture", block)
                self.assertIn("path: fixtures/profile_zero/prebuilt", block)

    def test_ci_required_still_requires_hygiene_and_export(self) -> None:
        required = "\n".join(self.blocks["ci_required"])
        self.assertIn("needs: [classify, hygiene, public_export, markdown, python_tools, native_tools,", required)
        self.assertIn("HYGIENE_RESULT: ${{ needs.hygiene.result }}", required)
        self.assertIn("PUBLIC_EXPORT_RESULT: ${{ needs.public_export.result }}", required)


def _ci_step_with(block: list[str], needle: str) -> list[str] | None:
    """The lines of the first step containing ``needle`` anywhere in its body.

    Unlike :func:`_ci_step` this also finds a step whose ``- name:`` is a folded
    scalar (``- name: >-`` followed by continuation lines), which the sharded
    Python test step uses.
    """
    for index, line in enumerate(block):
        if not line.strip().startswith("- name: "):
            continue
        indent = len(line) - len(line.lstrip())
        end = index + 1
        while end < len(block):
            candidate = block[end]
            if candidate.strip().startswith("- ") and len(candidate) - len(candidate.lstrip()) == indent:
                break
            end += 1
        step = block[index:end]
        if any(needle in step_line for step_line in step):
            return step
    return None


def _folded_run(step: list[str]) -> str:
    """A step's shell text with line continuations folded into single spaces."""
    folded = re.sub(r"\\\s*\n\s*", " ", "\n".join(step))
    return re.sub(r"\s+", " ", folded)


class CoverageWiringTests(unittest.TestCase):
    """Coverage is measured inside the existing gates and published informationally.

    The measurement may not weaken a gate: the Python shards run the exact module
    list they always ran under unittest, the unprofiled native core gate keeps its
    command, and both uploads are informational (``codecov.yml``,
    ``fail_ci_if_error: false``), so a Codecov outage cannot fail a required job.
    """

    PYTHON_TEST_NEEDLE = "Run Python unit tests"
    C_MEASURE_STEP = "Measure native core C statement coverage"
    C_GATE_STEP = "Run native core test suite (hostile parsers, catalog, launch resolution, POSIX process)"
    UPLOADS = {
        "python_tools": ("Upload Python coverage to Codecov", "python", "coverage-python.xml"),
        "native_tools": ("Upload C coverage to Codecov", "c", None),
    }

    def setUp(self) -> None:
        self.blocks = _ci_job_blocks()
        self.ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")

    def test_python_shards_run_their_unchanged_module_list_under_coverage(self) -> None:
        step = _ci_step_with(self.blocks["python_tools"], "coverage run")
        self.assertIsNotNone(step, "python_tools no longer measures its shards")
        text = _folded_run(step or [])
        # The measured run is the same planner output, the same sys.path and the
        # same unittest invocation; only the measurement wrapper is added.
        self.assertIn("python tools/ci_test_shards.py --shard", text)
        self.assertIn("shard $SHARD received no test modules", text)
        self.assertIn(
            'PYTHONPATH=tools python -m coverage run --parallel-mode --source=tools -m unittest -v "${mods[@]}"',
            text,
        )
        self.assertEqual(text.count("-m unittest -v"), 1, "a shard must run unittest exactly once")
        self.assertIn("python -m coverage combine", text)
        self.assertIn("python -m coverage xml -o coverage-python.xml", text)

    def test_native_coverage_is_a_separate_instrumented_build_beside_the_gate(self) -> None:
        native = self.blocks["native_tools"]
        gate = _ci_step(native, self.C_GATE_STEP)
        self.assertIsNotNone(gate, "the unprofiled native core gate was removed")
        self.assertIn("make CC=gcc native-core-tests", "\n".join(gate or []))
        measured = _ci_step(native, self.C_MEASURE_STEP)
        self.assertIsNotNone(measured, "native_tools no longer measures C coverage")
        self.assertLess(
            _ci_step_index(native, self.C_GATE_STEP),
            _ci_step_index(native, self.C_MEASURE_STEP),
            "the measured build runs after the gate it must not disturb",
        )
        text = _folded_run(measured or [])
        # Its own build directory, under the runner temp tree, so the gate's tree
        # and objects are never reused or overwritten.
        self.assertIn('coverage_build="$RUNNER_TEMP/native-core-coverage"', text)
        self.assertIn('BUILD_DIR="$coverage_build"', text)
        # The instrumentation is appended to the Makefile's own flags, so the
        # measured build keeps the default configuration.
        self.assertIn("compiler-info", text)
        self.assertIn('CFLAGS="$base_cflags --coverage"', text)
        self.assertIn('LDFLAGS="--coverage"', text)
        self.assertIn("native-core-tests", text)
        self.assertNotIn("gcovr", text)

    def test_native_gcov_reports_are_generated_from_the_repository_root(self) -> None:
        text = _folded_run(_ci_step(self.blocks["native_tools"], self.C_MEASURE_STEP) or [])
        # gcov runs in the step's working directory, the repository root, where the
        # objects' recorded src/... paths resolve. A cd into the build directory would
        # bring back the header-only reports Codecov rejected.
        self.assertNotIn('cd "$coverage_build"', text)
        # One gcov run per object: several objects in one run append duplicate line
        # records for a shared source into the same report.
        self.assertIn('for gcno in "$coverage_build"/*.gcno; do', text)
        self.assertIn('gcov -p -b -r "$gcno"', text)
        self.assertIn('gcov_dir="$RUNNER_TEMP/native-core-gcov"', text)
        # The selftests' own sources stay out of the number, and an empty set of
        # reports fails the step instead of uploading nothing.
        self.assertIn("_selftest\\.c$", text)
        self.assertIn('test "${#reports[@]}" -gt 0', text)

    def test_both_uploads_use_the_pinned_action_oidc_and_the_declared_flags(self) -> None:
        for job, (name, flag, report) in self.UPLOADS.items():
            with self.subTest(job=job):
                step = _ci_step(self.blocks[job], name)
                self.assertIsNotNone(step, f"{job} no longer uploads {report}")
                text = "\n".join(step or [])
                self.assertRegex(
                    text,
                    r"uses: codecov/codecov-action@[0-9a-f]{40} # v\d+\.\d+\.\d+",
                )
                self.assertIn("use_oidc: true", text)
                if report is None:
                    # The reports are generated in the measuring step; the action only
                    # uploads them. Its gcov plugin runs gcov from the build directory,
                    # where the objects' repository-relative sources cannot be opened.
                    self.assertIn("directory: ${{ runner.temp }}/native-core-gcov", text)
                    self.assertIn("plugins: noop", text)
                    self.assertNotIn("plugins: gcov", text)
                else:
                    self.assertIn(f"files: {report}", text)
                self.assertIn(f"flags: {flag}", text)
                self.assertIn("fail_ci_if_error: false", text)
        # Informational means informational: no step may be allowed to fail quietly.
        self.assertNotIn("continue-on-error", self.ci)

    def test_only_the_uploading_jobs_gain_id_token_write(self) -> None:
        expected = "    permissions:\n      contents: read\n      id-token: write\n"
        for job in self.UPLOADS:
            with self.subTest(job=job):
                self.assertIn(expected, "\n".join(self.blocks[job]) + "\n")
        for job, block in self.blocks.items():
            if job in self.UPLOADS:
                continue
            with self.subTest(job=job):
                self.assertNotIn("id-token", "\n".join(block))
        # The workflow itself stays read-only and still names no write permission.
        self.assertIn("permissions:\n  contents: read\n", self.ci)
        self.assertNotIn("contents: write", self.ci)

    def test_every_action_use_in_ci_is_pinned_by_commit_sha_with_its_release(self) -> None:
        uses = [line.strip() for line in self.ci.splitlines() if line.strip().startswith("uses: ")]
        self.assertTrue(uses, "ci.yml declares no actions")
        for line in uses:
            with self.subTest(use=line):
                self.assertRegex(line, r"^uses: [\w.-]+/[\w.-]+@[0-9a-f]{40} # v\d+(?:\.\d+)*$")
        self.assertEqual(
            sum("codecov/codecov-action@" in line for line in uses), 2,
            "exactly the two coverage uploads use the Codecov action",
        )

    def test_coverage_configuration_omits_tests_and_stays_informational(self) -> None:
        import tomllib

        project = tomllib.loads((ROOT / "pyproject.toml").read_text(encoding="utf-8"))
        self.assertEqual(project["tool"]["coverage"]["run"]["omit"], ["tools/test_*.py"])

        codecov = (ROOT / "codecov.yml").read_text(encoding="utf-8")
        # Project and patch status may inform but never block a merge.
        self.assertRegex(
            codecov,
            r"(?m)^coverage:\n  status:\n    project:\n      default:\n        informational: true\n"
            r"    patch:\n      default:\n        informational: true\n",
        )
        self.assertNotIn("informational: false", codecov)
        for flag in ("python", "c"):
            with self.subTest(flag=flag):
                self.assertRegex(
                    codecov, rf"(?m)^    - name: {flag}\n      carryforward: true$"
                )

    def test_docs_describe_what_is_measured_and_that_it_is_informational(self) -> None:
        docs = (ROOT / "docs" / "CI.md").read_text(encoding="utf-8")
        coverage = _markdown_section(docs, "## Coverage")
        blocks = _markdown_blocks(coverage)
        for terms in (
            ("python_tools", "coverage-python.xml", "unittest"),
            ("codecov.yml", "informational"),
            ("native_tools", "native-core-tests"),
        ):
            with self.subTest(terms=terms):
                self.assertTrue(
                    any(all(term.lower() in block.lower() for term in terms) for block in blocks),
                    f"docs/CI.md coverage guidance must state the required terms together: {terms}",
                )


def _workflow_text(name: str) -> str:
    return (ROOT / ".github" / "workflows" / name).read_text(encoding="utf-8")


def _markdown_blocks(text: str) -> list[str]:
    return [
        " ".join(line.strip() for line in block.splitlines() if line.strip())
        for block in re.split(r"\n\s*\n", text)
        if block.strip()
    ]


def _markdown_section(text: str, heading: str) -> str:
    lines = text.splitlines()
    start = next((index for index, line in enumerate(lines) if line.strip() == heading), None)
    if start is None:
        raise AssertionError(f"Markdown section is missing: {heading}")
    end = next(
        (index for index in range(start + 1, len(lines)) if re.match(r"^## ", lines[index])),
        len(lines),
    )
    return "\n".join(lines[start:end])


def _on_block(text: str) -> list[str]:
    """The top-level ``on:`` mapping: its lines, with comments and blank lines removed."""
    lines = text.splitlines()
    start = lines.index("on:")
    body: list[str] = []
    for line in lines[start + 1:]:
        if line and not line.startswith((" ", "#")):
            break
        stripped = line.strip()
        if stripped and not stripped.startswith("#"):
            body.append(line.rstrip())
    return body


class MergeQueueTriggerWiringTests(unittest.TestCase):
    """``merge_group`` is additive: it adds a trigger and changes no existing event's behaviour."""

    def setUp(self) -> None:
        self.ci = _workflow_text("ci.yml")

    def test_ci_on_block_adds_schedule_and_merge_group_beside_existing_triggers(self) -> None:
        self.assertEqual(
            _on_block(self.ci),
            [
                "  pull_request:",
                "    types: [opened, synchronize, reopened]",
                "  push:",
                "    branches: [main]",
                "  schedule:",
                '    - cron: "17 3 * * *"',
                "  workflow_dispatch:",
                "  merge_group:",
                "    types: [checks_requested]",
            ],
        )

    def test_event_conditions_only_select_the_export_and_python_tier(self) -> None:
        blocks = _ci_job_blocks()
        export = "\n".join(blocks["public_export"])
        python_tools = "\n".join(blocks["python_tools"])
        self.assertIn("github.event_name == 'workflow_dispatch'", export)
        self.assertIn("NK_TEST_TIER:", python_tools)
        self.assertIn("github.event_name == 'schedule'", python_tools)
        self.assertIn("github.event_name == 'workflow_dispatch'", python_tools)
        for job in ("classify", "hygiene", "markdown", "native_tools", "windows_runtime",
                    "main_smoke", "ci_required"):
            with self.subTest(job=job):
                self.assertNotIn("github.event_name", "\n".join(blocks[job]))

    def test_scheduled_and_manual_events_force_full_matrix_and_full_python_tier(self) -> None:
        for event_name in ("schedule", "workflow_dispatch"):
            with self.subTest(event=event_name):
                result = ci_paths.classify(["docs/CI.md"], event_name=event_name)
                for key in (
                    "python_tools", "run_python", "run_native", "run_windows",
                    "run_markdown", "run_main_smoke", "allow_substantive",
                ):
                    self.assertEqual(result[key], "true", key)
                self.assertEqual(result["python_scope"], "all")
                self.assertEqual(result["python_num_shards"], "4")

    def test_python_tools_skip_summary_counts_only_known_prefixes(self) -> None:
        output = "\n".join(
            (
                "test_a ... skipped 'NIGHTLY_TIER: slow case'",
                "test_b ... skipped 'WINDOWS_ONLY: mingw32-make unavailable'",
                "test_c ... skipped 'PRIVATE_INPUT: HST manifest absent'",
                "test_d ... skipped 'an unrelated optional compiler is missing'",
                "test_e ... skipped 'PRIVATE_INPUT: EBOOT absent'",
            )
        )
        self.assertEqual(
            ci_test_shards.skip_prefix_summary(output),
            "Python test skip prefixes: NIGHTLY_TIER=1 WINDOWS_ONLY=1 PRIVATE_INPUT=2",
        )

    def test_ci_stays_read_only_and_publishes_nothing_on_any_event(self) -> None:
        self.assertIn("permissions:\n  contents: read\n", self.ci)
        for forbidden in ("contents: write", "refs/heads/main", "gh release", "git push",
                          "action-gh-release", "upload-release"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.ci)


class MergeQueueDependencyReviewWiringTests(unittest.TestCase):
    """dependency-review runs on a merge group with both refs taken from the group payload."""

    def setUp(self) -> None:
        self.text = _workflow_text("dependency-review.yml")

    def test_trigger_adds_merge_group_beside_the_unchanged_pull_request_trigger(self) -> None:
        self.assertEqual(
            _on_block(self.text),
            [
                "  pull_request:",
                '    branches: [ "main" ]',
                "  merge_group:",
                "    types: [checks_requested]",
                '    branches: [ "main" ]',
            ],
        )

    def test_step_passes_both_refs_from_the_merge_group_payload(self) -> None:
        step = self.text.split("      - name: 'Dependency Review'", 1)[1]
        self.assertEqual(step.count("base-ref: ${{ github.event.merge_group.base_sha }}"), 1)
        self.assertEqual(step.count("head-ref: ${{ github.event.merge_group.head_sha }}"), 1)
        self.assertIn("with:\n          base-ref:", step)

    def test_required_job_name_action_pin_and_permissions_are_unchanged(self) -> None:
        self.assertIn("  dependency-review:\n    runs-on: ubuntu-26.04", self.text)
        self.assertIn(
            "uses: actions/dependency-review-action@a1d282b36b6f3519aa1f3fc636f609c47dddb294 # v5.0.0",
            self.text,
        )
        self.assertIn("permissions:\n  contents: read\n", self.text)


class MergeQueueCodeqlWiringTests(unittest.TestCase):
    """Merge groups reach the trusted CodeQL route through the wake; the route itself is unchanged.

    codeql.yml has no merge_group trigger of its own. A group's wake (codeql-wake.yml) starts
    the default branch's copy of this workflow through workflow_run. Every route reads the
    same analysis configuration and writes the same per-language categories, and nothing
    checks out the candidate tree.
    """

    def setUp(self) -> None:
        self.text = _workflow_text("codeql.yml")

    def test_trigger_block_has_no_direct_merge_group_route(self) -> None:
        self.assertEqual(
            _on_block(self.text),
            [
                "  workflow_run:",
                '    workflows: ["CodeQL wake"]',
                "    types: [completed]",
                "  workflow_dispatch:",
                "    inputs:",
                "      pull_request_number:",
                "        description: Open pull request number to scan from the trusted default branch",
                "        required: true",
                "        type: number",
                "  push:",
                "    branches: [main]",
                "  schedule:",
                '    - cron: "37 3 * * 1"',
            ],
        )

    def test_pull_request_route_text_is_unchanged(self) -> None:
        for fragment in (
            "if: ${{ github.event_name != 'workflow_run' || github.event.workflow_run.event == 'pull_request' || github.event.workflow_run.event == 'merge_group' }}",
            'if (context.eventName === "workflow_run") {',
            'if (!run || run.event !== "pull_request") {',
            "const { data: matches } = await github.rest.pulls.list({",
            '"+refs/pull/${PR_NUMBER}/merge:refs/candidate/merge"',
            'if [ "${TARGET_KIND}" = "pull-request" ]; then',
            'core.setOutput("ref", `refs/pull/${number}/merge`);',
            'if (kind === "pull-request") {',
            "PR base, head, or merge ref changed during CodeQL analysis; refusing a stale upload",
        ):
            with self.subTest(fragment=fragment):
                self.assertEqual(self.text.count(fragment), 1, fragment)

    def test_every_route_shares_one_analysis_configuration_and_category_scheme(self) -> None:
        self.assertEqual(self.text.count('category: "/language:${{ matrix.language }}/"'), 1)
        self.assertEqual(self.text.count("upload: never"), 1)
        self.assertEqual(self.text.count("trap-caching: false"), 1)
        self.assertEqual(self.text.count("disable-default-queries: false"), 1)
        self.assertEqual(self.text.count("build-mode: none"), 1)

    def test_merge_group_wake_route_analyses_the_queue_commit_as_data(self) -> None:
        self.assertNotIn("ref: ${{ github.sha }}", self.text)
        self.assertNotIn("ref: ${{ github.event.pull_request.head.sha", self.text)
        self.assertEqual(
            self.text.count('if (context.eventName === "workflow_run" && context.payload.workflow_run?.event === "merge_group") {'),
            1,
        )
        self.assertIn('kind === "merge-group"', self.text)
        self.assertIn('elif [ "${TARGET_KIND}" = "merge-group" ]; then', self.text)
        self.assertIn('"+${TARGET_REF}:refs/candidate/merge"', self.text)
        # Upload is by the queue ref and the group SHA, with the same categories as every route.
        self.assertIn("commit_sha: targetSha,", self.text)
        self.assertIn("ref: targetRef,", self.text)

    def test_merge_group_wake_route_checks_the_queue_ref_before_and_after_analysis(self) -> None:
        # Before analysis: the queue ref must name the commit the wake ran on.
        self.assertIn("queued.object.sha !== run.head_sha", self.text)
        # Before upload: the same check against the group SHA carried into the upload job.
        self.assertIn("queued.object.sha !== targetSha", self.text)
        # {ref} percent-encodes the slashes of a queue name (heads%2Fgh-readonly-queue...),
        # so the queue ref is always read through the reserved {+ref} expansion.
        self.assertEqual(
            self.text.count('github.request("GET /repos/{owner}/{repo}/git/ref/{+ref}", {'), 2)
        self.assertNotIn("git/ref/{ref}", self.text)
        # Only this repository's own queue branches are accepted.
        self.assertIn("/^gh-readonly-queue\\/[A-Za-z0-9._\\/-]+$/", self.text)
        self.assertEqual(
            self.text.count("headRepository?.full_name?.toLowerCase() !== `${context.repo.owner}/${context.repo.repo}`.toLowerCase()"),
            1,
        )

    def test_reserved_provenance_context_is_not_named_here(self) -> None:
        self.assertNotIn("Trusted provenance attestation", self.text)


class MergeQueueWakeWiringTests(unittest.TestCase):
    """The CodeQL wake signals merge groups too. It stays signal-only and keys its runs by ref."""

    def setUp(self) -> None:
        self.wake = _workflow_text("codeql-wake.yml")

    def test_wake_triggers_on_pull_requests_and_merge_groups(self) -> None:
        self.assertEqual(
            _on_block(self.wake),
            [
                "  pull_request:",
                "    types: [opened, synchronize, reopened]",
                "  merge_group:",
                "    types: [checks_requested]",
            ],
        )

    def test_wake_keys_merge_group_runs_by_ref_not_by_an_empty_number(self) -> None:
        self.assertIn(
            "  group: codeql-wake-${{ github.event.pull_request.number || github.ref }}\n",
            self.wake,
        )
        self.assertNotIn("codeql-wake-${{ github.event.pull_request.number }}\n", self.wake)
        self.assertIn("cancel-in-progress: true", self.wake)

    def test_wake_stays_signal_only_and_keeps_its_exact_name(self) -> None:
        self.assertIn("name: CodeQL wake\n", self.wake)
        self.assertIn("permissions: {}\n", self.wake)
        self.assertNotIn("actions/checkout", self.wake)
        self.assertNotIn("actions/upload-artifact", self.wake)


# Read-only snapshot of the live `main-minimal` ruleset (id 20662430, last updated
# 2026-10-13T01:50:04-04:00), taken 2026-10-15 with
# `gh api repos/Jstar269/nakagawa-recomp/rulesets/20662430`. Refresh it only by reading the
# ruleset again; nothing in the repository writes to GitHub. Each value is the integration id
# the ruleset pins for that context (15368 is GitHub Actions).
MAIN_RULESET_SNAPSHOT: dict[str, object] = {
    "strict_required_status_checks_policy": True,
    "required_status_checks": {
        "CI required": 15368,
        "OSV Vulnerability Scan": 15368,
        "dependency-review": 15368,
        "Hygiene and security": 15368,
        "CodeQL": 57789,
    },
}

# Not a required check until its producer identity is bound (docs/PROVENANCE_MERGE_GATE.md,
# Maintainer configuration, item 3). Adding it requires that proof, not only this edit.
TRUSTED_PROVENANCE_CONTEXT = "Trusted provenance attestation"


def _prose(text: str) -> str:
    """Collapse line breaks and runs of spaces so a sentence can be matched as one string."""
    return re.sub(r"\s+", " ", text)


def _backticked(fragment: str) -> set[str]:
    return set(re.findall(r"`([^`]+)`", fragment))


class PolicyDeltaLookupWiringTests(unittest.TestCase):
    """Both trusted attestation routes prefer exact nested deltas and retain flat fallback."""

    WORKFLOWS = (
        "provenance-attestation.yml",
        "provenance-attestation-merge-group.yml",
    )

    def test_nested_lookup_precedes_legacy_flat_fallback(self) -> None:
        for name in self.WORKFLOWS:
            with self.subTest(workflow=name):
                text = _workflow_text(name)
                nested_assignment = text.index(
                    'nested_policy_dir="docs/provenance/policy-deltas/${candidate_policy_sha}/${base_policy_sha}"'
                )
                nested_candidate = text.index('${nested_policy_dir}/candidate-policy.json?ref=${authority_sha}')
                nested_authority = text.index('${nested_policy_dir}/policy-delta-authority.json?ref=${authority_sha}')
                nested_candidate_nonempty = text.index(
                    'test -s "${RUNNER_TEMP}/trusted/candidate-policy.json"', nested_authority,
                )
                nested_authority_nonempty = text.index(
                    'test -s "${RUNNER_TEMP}/trusted/policy-delta-authority.json"', nested_authority,
                )
                fallback_assignment = text.index('policy_dir="${flat_policy_dir}"')
                flat_candidate = text.index('${policy_dir}/candidate-policy.json?ref=${authority_sha}',
                                            fallback_assignment)
                flat_authority = text.index('${policy_dir}/policy-delta-authority.json?ref=${authority_sha}',
                                            fallback_assignment)
                self.assertLess(nested_assignment, nested_candidate)
                self.assertLess(nested_candidate, nested_authority)
                self.assertLess(nested_authority, nested_candidate_nonempty)
                self.assertLess(nested_candidate_nonempty, nested_authority_nonempty)
                self.assertLess(nested_authority_nonempty, fallback_assignment)
                self.assertLess(fallback_assignment, flat_candidate)
                self.assertLess(flat_candidate, flat_authority)
                self.assertIn('policy_dir="${nested_policy_dir}"', text)
                self.assertIn('policy_dir="${flat_policy_dir}"', text)
                self.assertIn('test -s "${RUNNER_TEMP}/trusted/candidate-policy.json"', text)
                self.assertIn('test -s "${RUNNER_TEMP}/trusted/policy-delta-authority.json"', text)


class MainRulesetClaimsTests(unittest.TestCase):
    """The written claims about main's required checks, and the aggregate behind them, must match the snapshot."""

    def setUp(self) -> None:
        self.required = set(MAIN_RULESET_SNAPSHOT["required_status_checks"])
        self.strict = MAIN_RULESET_SNAPSHOT["strict_required_status_checks_policy"]

    def test_ci_doc_names_exactly_the_snapshot_required_checks(self) -> None:
        doc = (ROOT / "docs" / "CI.md").read_text(encoding="utf-8")
        candidates = [
            block for block in _markdown_blocks(doc)
            if all(term in block.lower() for term in ("`main`", "ruleset", "requir", "exact pull-request heads"))
        ]
        self.assertTrue(candidates, "docs/CI.md must state required checks for exact main pull-request heads")
        block = candidates[0]
        start = block.lower().find("requir")
        end = block.lower().find("exact pull-request heads", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        self.assertEqual(_backticked(block[start:end]), self.required)

    def test_provenance_gate_doc_names_exactly_the_snapshot_required_checks(self) -> None:
        doc = (ROOT / "docs" / "PROVENANCE_MERGE_GATE.md").read_text(encoding="utf-8")
        candidates = [
            block for block in _markdown_blocks(doc)
            if all(term in block.lower() for term in ("main-minimal", "ruleset", "five checks"))
        ]
        self.assertTrue(
            candidates,
            "docs/PROVENANCE_MERGE_GATE.md must state the required main ruleset checks",
        )
        block = candidates[0]
        start = block.lower().find("five checks")
        left = block.find("(", start)
        right = block.find(")", left + 1)
        self.assertGreaterEqual(left, start)
        self.assertGreater(right, left)
        self.assertEqual(_backticked(block[left + 1:right]), self.required)

    def test_provenance_workflow_comment_names_exactly_the_snapshot_required_checks(self) -> None:
        comment = _prose(re.sub(r"(?m)^\s*#", "", _workflow_text("provenance-attestation.yml")))
        match = re.search(r"which are: ([^.]*)\.", comment)
        self.assertIsNotNone(match, "provenance-attestation.yml no longer lists the ruleset's checks")
        assert match is not None
        self.assertEqual({name.strip() for name in match.group(1).split(",")}, self.required)

    def test_strict_policy_claims_match_the_snapshot(self) -> None:
        doc = _prose((ROOT / "docs" / "PROVENANCE_MERGE_GATE.md").read_text(encoding="utf-8"))
        claims = re.findall(
            r"`strict_required_status_checks_policy` is (?:already )?\*{0,2}`(true|false)`", doc
        )
        self.assertTrue(claims, "docs/PROVENANCE_MERGE_GATE.md no longer records the strict-policy value")
        self.assertEqual(set(claims), {"true" if self.strict else "false"})

    def test_trusted_provenance_context_is_not_in_the_required_snapshot(self) -> None:
        self.assertNotIn(
            TRUSTED_PROVENANCE_CONTEXT,
            self.required,
            "Trusted provenance attestation may be required only after its producer identity is bound "
            "(docs/PROVENANCE_MERGE_GATE.md, Maintainer configuration, item 3).",
        )

    def test_ci_required_aggregates_exactly_the_results_its_script_reads(self) -> None:
        block = "\n".join(_ci_job_blocks()["ci_required"])
        needs = re.search(r"(?m)^    needs: \[([^\]]*)\]$", block)
        self.assertIsNotNone(needs)
        assert needs is not None
        needed = {job.strip() for job in needs.group(1).split(",")}
        wired = dict(
            re.findall(r"(?m)^      ([A-Z_]+_RESULT): \$\{\{ needs\.([a-z_]+)\.result \}\}$", block)
        )
        read = {result for _name, result, _run in ci_required._GATES} | {
            "CLASSIFY_RESULT",
            "HYGIENE_RESULT",
            "PUBLIC_EXPORT_RESULT",
        }
        self.assertEqual(set(wired), read)
        self.assertEqual({wired[key] for key in read}, needed)


if __name__ == "__main__":
    unittest.main()
