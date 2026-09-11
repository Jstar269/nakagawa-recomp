#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Run the host-backend native tests from the Python gate.

`mingw32-make native-core-tests` builds and runs these on a developer's own
machine, but hosted CI does not invoke that target, so on its own it gives the
Windows host's answer and nothing else. Two of the behaviours these tests cover
are precisely the ones that differ per host:

  * the launch resolver deriving `<executable>_image.bin` for an extensionless
    POSIX binary, not only for a name ending in `.exe`;
  * the POSIX process backend honouring a finite wait timeout, which the Win32
    backend has always done.

Driving them from here puts both on the Linux runner that the Python tooling
job already uses, which is where the POSIX half actually means something.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent

#: The host platform backend, chosen the same way the Makefile chooses it.
_WINDOWS = os.name == "nt"
PLATFORM_SRC = "src/core/nk_platform_win32.c" if _WINDOWS else "src/core/nk_platform_posix.c"
EXE_EXT = ".exe" if _WINDOWS else ""

CORE_SOURCES = [
    "src/core/nk_iso.c",
    "src/core/nk_library.c",
    "src/core/nk_launch.c",
    "src/core/nk_title_manifest.c",
    "src/core/generated/nk_title_catalog.c",
]


def _build_and_run(
    test_case: unittest.TestCase,
    test_source: str,
    exe_stem: str,
    extra_sources: tuple[str, ...] = (),
    extra_includes: tuple[str, ...] = (),
) -> str:
    if shutil.which("gcc") is None:
        raise unittest.SkipTest(
            "gcc is unavailable, so the native host-backend test cannot be built; "
            "reporting SKIP rather than passing without having run it"
        )

    out = ROOT / "build" / f"{exe_stem}{EXE_EXT}"
    out.parent.mkdir(parents=True, exist_ok=True)

    compile_cmd = [
        "gcc", "-std=c99", "-Wall", "-Wextra", "-Werror",
        "-Isrc/core", "-Isrc/core/generated", *extra_includes,
        *CORE_SOURCES,
        PLATFORM_SRC,
        *extra_sources,
        test_source,
        "-o", str(out),
    ]
    if _WINDOWS:
        compile_cmd.append("-lshell32")

    build = subprocess.run(compile_cmd, cwd=ROOT, capture_output=True, text=True)
    test_case.assertEqual(
        build.returncode, 0,
        f"compiling {test_source} failed:\n{build.stdout}\n{build.stderr}",
    )

    run = subprocess.run([str(out)], cwd=ROOT, capture_output=True, text=True, timeout=300)
    test_case.assertEqual(
        run.returncode, 0,
        f"{test_source} failed:\n{run.stdout}\n{run.stderr}",
    )
    return run.stdout


class NativeHostBackendTests(unittest.TestCase):
    def test_launch_resolution(self) -> None:
        """Sibling image discovery and writable save routing, on this host."""
        stdout = _build_and_run(
            self, "tests/native/test_launch_resolution.c", "test_launch_resolution"
        )
        self.assertIn("ALL LAUNCH RESOLUTION TESTS PASSED", stdout)

    def test_player_state(self) -> None:
        """Library entry lookup and honest add results, with no SDL involved."""
        stdout = _build_and_run(
            self, "tests/native/test_player_state.c", "test_player_state",
            extra_sources=("src/player/player_state.c",),
            extra_includes=("-Isrc/player",),
        )
        self.assertIn("ALL PLAYER STATE TESTS PASSED", stdout)

    @unittest.skipIf(_WINDOWS, "the POSIX process backend is not built on Windows")
    def test_posix_process_backend(self) -> None:
        """Finite wait timeouts and exit-status reporting on the POSIX backend."""
        stdout = _build_and_run(
            self, "tests/native/test_posix_process.c", "test_posix_process"
        )
        self.assertIn("ALL POSIX PROCESS TESTS PASSED", stdout)


if __name__ == "__main__":
    unittest.main()
