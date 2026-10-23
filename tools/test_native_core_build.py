# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Coverage contract for ``native-core-tests`` (#735 items 3 and 7).

The target may share compiled core objects and expand its test-binary recipes as macros,
but it must still run the same test binaries, in the same order, with the same arguments.
These checks read the target's dry run, which lists every command Make would execute, so
the contract holds without building anything.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Every dry run names a scratch BUILD_ROOT: parsing the Makefile writes beneath BUILD_ROOT (the
# SDL3 discovery fragment, a named title's stamps), so the checkout's build/ is never touched.
# Commands are compared after that root is folded back to the literal `build/` spelling.
_SCRATCH_ROOT = tempfile.TemporaryDirectory(prefix="nk-native-core-dry-")
unittest.addModuleCleanup(_SCRATCH_ROOT.cleanup)
_SCRATCH = Path(_SCRATCH_ROOT.name).as_posix()

# Every binary the target runs, in order, with its arguments. ``.exe`` is dropped so one list
# serves Windows and POSIX. The process test differs by platform, as it always did.
_RUNS_BEFORE_PROCESS_TEST = (
    "build/mygame/cpu_lle_selftest",
    "build/mygame/domain_mode_selftest",
    "build/mygame/clay_layout_selftest",
    "build/test_nk_ui_core",
    "build/test_player_screens",
    "build/test_nk_ui_focus",
    "build/test_nk_ui_focus_sdl --sdl-input",
    "build/mygame/ge_texture_ref_selftest",
    "build/mygame/fbcap_selftest build/mygame/fbcap_selftest",
    "build/mygame/osk_text_entry_selftest",
    "build/mygame/osk_overlay_selftest",
    "build/mygame/ge_float24_selftest",
    "build/mygame/ge_cpu_profile_validate_selftest",
    "build/mygame/ge_cpu_profile_validate_selftest --strip-cache-disabled",
    "build/mygame/ge_raster_ref_selftest",
    "build/test_pgf_public",
    "build/test_core_catalog",
    "build/test_parsers_hostile",
    "build/test_manifest_parser --check",
    "build/test_launch_resolution",
    "build/test_player_state",
    "build/test_input_settings",
    "build/test_package_builder",
    "build/test_xb_parser",
    "build/test_fuzz_parsers --iters 100",
    "build/mygame/psmf_producer_selftest --fuzz-iters 100",
    "build/test_input_profile",
)
_PROCESS_TEST = "build/test_win32_process" if os.name == "nt" else "build/test_posix_process"
EXPECTED_RUNS = _RUNS_BEFORE_PROCESS_TEST + (_PROCESS_TEST,)


def _make_program() -> str | None:
    for name in ("mingw32-make", "make"):
        found = shutil.which(name)
        if found:
            return found
    return None


def _commands(lines: list[str]) -> list[str]:
    """Join backslash-continued lines so each entry is one shell command."""
    commands: list[str] = []
    pending = ""
    for line in lines:
        stripped = line.strip()
        if stripped.endswith("\\"):
            pending += stripped[:-1] + " "
            continue
        commands.append((pending + stripped).strip())
        pending = ""
    if pending.strip():
        commands.append(pending.strip())
    return commands


def _dry_run(*goals: str) -> list[str]:
    make = _make_program()
    if make is None:
        raise unittest.SkipTest("no make program on PATH")
    result = subprocess.run(
        [make, "--no-print-directory", "-n", "CC=gcc", f"BUILD_ROOT={_SCRATCH}", *goals],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"make -n {' '.join(goals)} failed:\n{result.stderr}")
    return [_fold_root(c) for c in _commands(result.stdout.splitlines())]


def _fold_root(command: str) -> str:
    """Spell the scratch BUILD_ROOT as `build` so expectations read like the default layout."""
    return command.replace(_SCRATCH + "/", "build/").replace("./build/", "build/")


def _test_runs(commands: list[str]) -> list[str]:
    return [
        # A run may carry a shell tail (the SDL leg maps exit 77 to SKIP); compare the binary part.
        command.split(";")[0].replace(".exe", "")
        for command in commands
        if command.startswith("build/") and " -o " not in command
    ]


def _link_outputs(commands: list[str]) -> set[str]:
    outputs = set()
    for command in commands:
        tokens = command.split()
        if tokens and tokens[0] == "gcc" and "-o" in tokens:
            target = tokens[tokens.index("-o") + 1]
            outputs.add(os.path.basename(target).replace(".exe", ""))
    return outputs


class NativeCoreTestsContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.commands = _dry_run("native-core-tests")

    def test_runs_the_same_test_binaries_in_the_same_order(self) -> None:
        self.assertEqual(_test_runs(self.commands), list(EXPECTED_RUNS))

    def test_nk_ui_core_is_compiled_and_run_by_native_core_tests(self) -> None:
        compile_commands = [command for command in self.commands
                            if command.startswith("gcc") and "-o build/test_nk_ui_core" in command]
        self.assertEqual(len(compile_commands), 1, "nk_ui core test compile command")
        self.assertIn("-Isrc/ui", compile_commands[0])
        self.assertIn("-Ithird_party/clay", compile_commands[0])
        self.assertIn("tests/native/test_nk_ui_core.c", compile_commands[0])
        self.assertIn("src/ui/nk_ui_clay.c", compile_commands[0])
        self.assertIn("build/test_nk_ui_core", _test_runs(self.commands))

    def test_nk_ui_focus_selftest_has_an_sdl_free_build_and_an_sdl_leg(self) -> None:
        plain = [c for c in self.commands if c.startswith("gcc") and " -o build/test_nk_ui_focus" in c
                 and "_sdl" not in c.split(" -o ", 1)[1]]
        sdl = [c for c in self.commands if c.startswith("gcc") and " -o build/test_nk_ui_focus_sdl" in c]
        self.assertEqual(len(plain), 1, "SDL-free focus test compile")
        self.assertEqual(len(sdl), 1, "SDL translation leg compile")
        self.assertNotIn("NK_UI_WITH_SDL3", plain[0])
        for command in (plain[0], sdl[0]):
            self.assertIn("tests/native/test_nk_ui_focus.c", command)
            self.assertIn("src/ui/nk_ui_sdl_input.c", command)
            self.assertIn("-DSR_SELFTEST_BUILD_ROOT=", command)
        run = [c for c in self.commands if c.startswith("build/test_nk_ui_focus_sdl")]
        self.assertEqual(len(run), 1)
        self.assertIn("--sdl-input", run[0])
        self.assertIn("77", run[0], "exit 77 must be reported as SKIP, not swallowed or failed")

    def test_core_sources_compile_once_per_flag_set(self) -> None:
        # Two shared object sets: the core flags, and the core flags plus -Isrc/player.
        compiles = [c for c in self.commands if " -c src/core/nk_font.c " in c]
        self.assertEqual(len(compiles), 2, "\n".join(compiles))

    def test_binaries_that_share_the_core_objects_link_them(self) -> None:
        linked = [c for c in self.commands
                  if c.startswith("gcc") and "build/native-core/" in c and " -o build/test_" in c]
        names = {os.path.basename(c.split(" -o ", 1)[1].split()[0]).replace(".exe", "")
                 for c in linked}
        for binary in ("test_core_catalog", "test_parsers_hostile", "test_manifest_parser",
                       "test_launch_resolution", "test_input_profile", "test_input_settings",
                       "test_package_builder"):
            with self.subTest(binary=binary):
                self.assertIn(binary, names)

    def test_every_expected_binary_is_linked(self) -> None:
        outputs = _link_outputs(self.commands)
        for run in EXPECTED_RUNS:
            binary = os.path.basename(run.split()[0])
            if binary.endswith("selftest") or binary.startswith("psmf"):
                continue  # built by the cpu-lle/domain/ge/psmf recipes, not a -o link here
            with self.subTest(binary=binary):
                self.assertIn(binary, outputs)

    def test_target_does_not_recurse_into_make(self) -> None:
        recursive = [c for c in self.commands if "--no-print-directory" in c]
        self.assertEqual(recursive, [], "every sub-make re-parses the whole Makefile")


class StandaloneTestBinTargetTests(unittest.TestCase):
    """The *-test-bin targets stay buildable on their own; other tests invoke them directly."""

    def test_input_settings_and_package_builder_build_from_the_shared_objects(self) -> None:
        for goal, binary in (("input-settings-test-bin", "test_input_settings"),
                             ("package-builder-test-bin", "test_package_builder")):
            with self.subTest(goal=goal):
                commands = _dry_run(goal)
                self.assertTrue(any("build/native-core/player/nk_font.o" in c for c in commands))
                self.assertIn(binary, _link_outputs(commands))

    def test_player_state_keeps_its_seam_variant(self) -> None:
        commands = _dry_run("player-state-test-bin")
        self.assertTrue(any("-DNK_TITLE_MANIFEST_TEST_SEAMS" in c and "src/core/nk_font.c" in c
                            for c in commands))


if __name__ == "__main__":
    unittest.main()
