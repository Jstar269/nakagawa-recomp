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
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Every binary the target runs, in order, with its arguments. ``.exe`` is dropped so one list
# serves Windows and POSIX. The process test differs by platform, as it always did.
_RUNS_BEFORE_PROCESS_TEST = (
    "build/mygame/cpu_lle_selftest",
    "build/mygame/domain_mode_selftest",
    "build/mygame/ge_texture_ref_selftest",
    "build/mygame/fbcap_selftest build/mygame/fbcap_selftest",
    "build/mygame/osk_text_entry_selftest",
    "build/mygame/ge_float24_selftest",
    "build/mygame/ge_raster_ref_selftest",
    "./build/test_pgf_public",
    "./build/test_core_catalog",
    "./build/test_parsers_hostile",
    "./build/test_manifest_parser --check",
    "./build/test_launch_resolution",
    "./build/test_player_state",
    "./build/test_input_settings",
    "./build/test_package_builder",
    "./build/test_xb_parser",
    "./build/test_fuzz_parsers --iters 100",
    "build/mygame/psmf_producer_selftest --fuzz-iters 100",
    "./build/test_input_profile",
)
_PROCESS_TEST = "./build/test_win32_process" if os.name == "nt" else "./build/test_posix_process"
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
        [make, "--no-print-directory", "-n", "CC=gcc", *goals],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"make -n {' '.join(goals)} failed:\n{result.stderr}")
    return _commands(result.stdout.splitlines())


def _test_runs(commands: list[str]) -> list[str]:
    return [
        command.replace(".exe", "")
        for command in commands
        if command.startswith(("./build/", "build/mygame/")) and " -o " not in command
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
