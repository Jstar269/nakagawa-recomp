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
import tempfile
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
    "src/core/nk_json.c",
    "src/core/nk_font.c",
    "src/core/nk_input_profile.c",
    "src/core/generated/nk_title_catalog.c",
]


def _compile_object(
    owner: type[unittest.TestCase],
    source: str,
    object_name: str,
    extra_includes: tuple[str, ...] = (),
) -> Path:
    out = owner._object_dir / object_name
    compile_cmd = [
        "gcc", "-std=c99", "-Wall", "-Wextra", "-Werror",
        "-Isrc/core", "-Isrc/core/generated", *extra_includes,
        "-c", source, "-o", str(out),
    ]
    build = subprocess.run(compile_cmd, cwd=ROOT, capture_output=True, text=True)
    if build.returncode:
        raise AssertionError(
            f"compiling {source} failed:\n{build.stdout}\n{build.stderr}"
        )
    return out


def _build_and_run(
    test_case: unittest.TestCase,
    test_source: str,
    exe_stem: str,
    extra_sources: tuple[str, ...] = (),
) -> str:
    owner = type(test_case)
    if shutil.which("gcc") is None:
        raise unittest.SkipTest(
            "gcc is unavailable, so the native host-backend test cannot be built; "
            "reporting SKIP rather than passing without having run it"
        )

    tmp = test_case.enterContext(tempfile.TemporaryDirectory(prefix=f"nk_backend_{exe_stem}_"))
    out = Path(tmp) / f"{exe_stem}{EXE_EXT}"
    link_cmd = ["gcc", "-o", str(out)]
    link_cmd.extend(str(owner._objects[source]) for source in CORE_SOURCES)
    link_cmd.append(str(owner._objects[PLATFORM_SRC]))
    link_cmd.extend(str(owner._objects[source]) for source in extra_sources)
    link_cmd.append(str(owner._objects[test_source]))
    if _WINDOWS:
        link_cmd.append("-lshell32")

    build = subprocess.run(link_cmd, cwd=ROOT, capture_output=True, text=True)
    test_case.assertEqual(
        build.returncode, 0,
        f"linking {test_source} failed:\n{build.stdout}\n{build.stderr}",
    )

    run = subprocess.run([str(out)], cwd=ROOT, capture_output=True, text=True, timeout=300)
    test_case.assertEqual(
        run.returncode, 0,
        f"{test_source} failed:\n{run.stdout}\n{run.stderr}",
    )
    return run.stdout


class NativeHostBackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        if shutil.which("gcc") is None:
            raise unittest.SkipTest(
                "gcc is unavailable, so the native host-backend test cannot be built; "
                "reporting SKIP rather than passing without having run it"
            )

        cls._object_dir = Path(tempfile.mkdtemp(prefix="nk_backend_objects_"))
        cls._objects: dict[str, Path] = {}
        common_sources = [*CORE_SOURCES, PLATFORM_SRC]
        for source in common_sources:
            cls._objects[source] = _compile_object(
                cls, source, f"{Path(source).stem}.o"
            )

        cls._objects["src/player/player_state.c"] = _compile_object(
            cls, "src/player/player_state.c", "player_state.o", ("-Isrc/player",)
        )
        cls._objects["src/player/input_settings.c"] = _compile_object(
            cls, "src/player/input_settings.c", "input_settings.o", ("-Isrc/player",)
        )
        cls._objects["src/player/iso_reader.c"] = _compile_object(
            cls, "src/player/iso_reader.c", "iso_reader.o", ("-Isrc/player",)
        )
        cls._objects["src/player/package_builder.c"] = _compile_object(
            cls, "src/player/package_builder.c", "package_builder.o", ("-Isrc/player",)
        )
        cls._objects["src/rt/pgf_public.c"] = _compile_object(
            cls, "src/rt/pgf_public.c", "pgf_public.o", ("-Isrc/rt",)
        )
        harnesses = {
            "tests/native/test_launch_resolution.c": (),
            "tests/native/test_player_state.c": ("-Isrc/player",),
            "tests/native/test_package_builder.c": ("-Isrc/player",),
            "tests/native/test_pgf_public.c": ("-Isrc/rt",),
        }
        if not _WINDOWS:
            harnesses["tests/native/test_posix_process.c"] = ()
        for source, includes in harnesses.items():
            cls._objects[source] = _compile_object(
                cls, source, f"{Path(source).stem}.o", includes
            )

    @classmethod
    def tearDownClass(cls) -> None:
        object_dir = getattr(cls, "_object_dir", None)
        if object_dir is not None:
            shutil.rmtree(object_dir, ignore_errors=True)
        super().tearDownClass()

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
            extra_sources=(
                "src/player/player_state.c",
                "src/player/input_settings.c",
                "src/player/iso_reader.c",
                "src/player/package_builder.c",
            ),
        )
        self.assertIn("ALL PLAYER STATE TESTS PASSED", stdout)

    def test_package_builder(self) -> None:
        """Package builder progress line parser and state machine."""
        stdout = _build_and_run(
            self, "tests/native/test_package_builder.c", "test_package_builder",
            extra_sources=("src/player/package_builder.c",),
        )
        self.assertIn("ALL PACKAGE BUILDER TESTS PASSED", stdout)

    def test_public_pgf_reader(self) -> None:
        """Synthetic PGF reader contract on the current host compiler."""
        stdout = _build_and_run(
            self, "tests/native/test_pgf_public.c", "test_pgf_public",
            extra_sources=("src/rt/pgf_public.c",),
        )
        self.assertIn("ALL PUBLIC PGF READER TESTS PASSED", stdout)

    def test_synthetic_pgf_passes_public_validator(self) -> None:
        """The C-side base fixture also passes the public structural validator."""
        from tools.nk_core.fonts import validate_pgf_data

        owner = type(self)
        tmp = self.enterContext(tempfile.TemporaryDirectory(prefix="nk_backend_pgf_fixture_"))
        executable = Path(tmp) / f"test_pgf_fixture{EXE_EXT}"
        fixture = Path(tmp) / "synthetic.pgf"
        build = subprocess.run(
            [
                "gcc", "-o", str(executable),
                str(owner._objects["src/rt/pgf_public.c"]),
                str(owner._objects["tests/native/test_pgf_public.c"]),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            build.returncode, 0,
            f"linking the synthetic PGF fixture writer failed:\n{build.stdout}\n{build.stderr}",
        )
        write = subprocess.run(
            [str(executable), "--write-base", str(fixture)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            write.returncode, 0,
            f"writing the synthetic PGF fixture failed:\n{write.stdout}\n{write.stderr}",
        )
        result = validate_pgf_data(fixture.read_bytes(), filename=fixture.name)
        self.assertGreater(result["size"], 392)
        self.assertEqual(len(result["sha256"]), 64)

    @unittest.skipIf(_WINDOWS, "the POSIX process backend is not built on Windows")
    def test_posix_process_backend(self) -> None:
        """Finite wait timeouts and exit-status reporting on the POSIX backend."""
        stdout = _build_and_run(
            self, "tests/native/test_posix_process.c", "test_posix_process"
        )
        self.assertIn("ALL POSIX PROCESS TESTS PASSED", stdout)

    def test_audio_host_backend(self) -> None:
        """Public SDL3 host audio output backend: dummy driver handoff and no-device safety."""
        check_sdl = subprocess.run(
            ["gcc", "-E", "-x", "c", "-", "-o", os.devnull],
            input="#include <SDL3/SDL.h>\n",
            capture_output=True,
            text=True,
        )
        if check_sdl.returncode != 0:
            raise unittest.SkipTest("SDL3 headers not available on this host")

        tmp = self.enterContext(tempfile.TemporaryDirectory(prefix="nk_backend_audio_"))
        out = Path(tmp) / f"audio_selftest{EXE_EXT}"
        cmd = [
            "gcc", "-std=c99", "-Wall", "-Wextra",
            "-Isrc/rt", "-DSR_AUDIO_SELFTEST",
            "src/rt/audio_unavailable.c",
            "-lSDL3", "-lm",
            "-o", str(out),
        ]
        build = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(
            build.returncode, 0,
            f"compiling audio selftest failed:\n{build.stdout}\n{build.stderr}",
        )

        run = subprocess.run([str(out)], cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(
            run.returncode, 0,
            f"audio selftest failed:\n{run.stdout}\n{run.stderr}",
        )
        self.assertIn("ALL AUDIO HOST TESTS PASSED", run.stdout)


if __name__ == "__main__":
    unittest.main()
