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
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import scratch_dir  # noqa: E402

#: The host platform backend, chosen the same way the Makefile chooses it.
_WINDOWS = os.name == "nt"
PLATFORM_SRC = "src/core/nk_platform_win32.c" if _WINDOWS else "src/core/nk_platform_posix.c"
ISOLATION_SRC = "tests/native/native_test_isolation.c"
EXE_EXT = ".exe" if _WINDOWS else ""

CORE_SOURCES = [
    "src/core/nk_iso.c",
    "src/core/nk_library.c",
    "src/core/nk_launch.c",
    "src/core/nk_title_manifest.c",
    "src/core/nk_xb.c",
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
        "-Isrc/core", "-Isrc/core/generated", "-Isrc/rt", *extra_includes,
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
    extra_runs: tuple[tuple[str, ...], ...] = (),
) -> str:
    owner = type(test_case)
    if shutil.which("gcc") is None:
        raise unittest.SkipTest(
            "gcc is unavailable, so the native host-backend test cannot be built; "
            "reporting SKIP rather than passing without having run it"
        )

    tmp = test_case.enterContext(scratch_dir.TemporaryDirectory(prefix=f"nk_backend_{exe_stem}_"))
    out = Path(tmp) / f"{exe_stem}{EXE_EXT}"
    link_cmd = ["gcc", "-o", str(out)]
    link_cmd.extend(str(owner._objects[source]) for source in CORE_SOURCES)
    link_cmd.append(str(owner._objects[PLATFORM_SRC]))
    if test_source != "tests/native/test_pgf_public.c":
        # This harness owns its guest-memory and dirty-span stubs. Linking the
        # shared isolation object would define the same GE-hook seam twice.
        link_cmd.append(str(owner._objects[ISOLATION_SRC]))
    link_cmd.extend(str(owner._objects[source]) for source in extra_sources)
    if "src/rt/pgf_public.c" not in extra_sources:
        # nk_font.c calls the reader's validator. A caller that supplies the reader itself (the
        # reader's own test defines the guest globals it needs) gets no second copy or host stub.
        link_cmd.append(str(owner._objects["src/rt/pgf_public.c"]))
        link_cmd.append(str(owner._objects["src/core/nk_pgf_host.c"]))
    link_cmd.append(str(owner._objects[test_source]))
    if _WINDOWS:
        # The same host libraries the player links (Makefile PLAYER_EXTRA_LIBS):
        # package_builder.c fetches verified prerequisites through WinHTTP/BCrypt.
        link_cmd.extend(("-lshell32", "-lole32", "-luuid", "-lwinhttp", "-lbcrypt"))

    build = subprocess.run(link_cmd, cwd=ROOT, capture_output=True, text=True)
    test_case.assertEqual(
        build.returncode, 0,
        f"linking {test_source} failed:\n{build.stdout}\n{build.stderr}",
    )

    # Each test isolates its own per-user roots, so it runs here against a decoy
    # profile that must stay free of application folders: anything that lands
    # in it would have landed in the developer's real profile.
    profile = Path(tmp) / "developer-profile"
    home = profile / "home"
    env = dict(os.environ)
    env.update({
        "LOCALAPPDATA": str(profile / "localappdata"),
        "APPDATA": str(profile / "appdata"),
        "USERPROFILE": str(home),
        "HOME": str(home),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_DATA_HOME": str(home / ".local" / "share"),
        "XDG_STATE_HOME": str(home / ".local" / "state"),
    })
    run = subprocess.run([str(out)], cwd=ROOT, env=env, capture_output=True, text=True,
                         timeout=300)
    test_case.assertEqual(
        run.returncode, 0,
        f"{test_source} failed:\n{run.stdout}\n{run.stderr}",
    )
    # Other command-line modes of the same binary run against the same profile.
    for args in extra_runs:
        mode = subprocess.run([str(out), *args], cwd=ROOT, env=env, capture_output=True,
                              text=True, timeout=300)
        test_case.assertEqual(
            mode.returncode, 0,
            f"{test_source} {args[0]} failed:\n{mode.stdout}\n{mode.stderr}",
        )
    leaked = sorted(str(path.relative_to(profile)) for path in profile.rglob("*")
                    if "nakagawa" in path.name.lower())
    test_case.assertEqual(
        leaked, [], f"{test_source} wrote into the developer's profile before isolating it")
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
        common_sources = [*CORE_SOURCES, PLATFORM_SRC, "src/rt/pgf_public.c", "src/core/nk_pgf_host.c"]
        for source in common_sources:
            cls._objects[source] = _compile_object(
                cls, source, f"{Path(source).stem}.o"
            )

        # Compile the shared per-run isolation helper used by every test except
        # the PGF reader, whose source-owned stubs collect dirty spans itself.
        cls._objects[ISOLATION_SRC] = _compile_object(
            cls, ISOLATION_SRC, "native_test_isolation.o", ("-Itests/native",)
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
        cls._objects["src/player/setup_staging.c"] = _compile_object(
            cls, "src/player/setup_staging.c", "setup_staging.o", ("-Isrc/player",)
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
        # The package-validator mode, which the production smoke and player package
        # route tests run, must leave the developer's profile alone too.
        runtime_root = self.enterContext(
            scratch_dir.TemporaryDirectory(prefix="nk_backend_runtime_"))
        stdout = _build_and_run(
            self, "tests/native/test_player_state.c", "test_player_state",
            extra_sources=(
                "src/player/player_state.c",
                "src/player/input_settings.c",
                "src/player/iso_reader.c",
                "src/player/package_builder.c",
                "src/player/setup_staging.c",
            ),
            extra_runs=(
                ("--validate-package", runtime_root, "ULUS99998", "SYNTH00001", "1",
                 "EBOOT.BIN"),
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
        tmp = self.enterContext(scratch_dir.TemporaryDirectory(prefix="nk_backend_pgf_fixture_"))
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

        tmp = self.enterContext(scratch_dir.TemporaryDirectory(prefix="nk_backend_audio_"))
        out = Path(tmp) / f"audio_selftest{EXE_EXT}"
        cmd = [
            "gcc", "-std=c99", "-Wall", "-Wextra",
            "-Isrc/rt", "-DSR_AUDIO_SELFTEST",
            "src/rt/audio_unavailable.c", "src/rt/perf.c",
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

    def test_vblank_exit_closes_the_audio_dump_outside_the_stat_gate(self) -> None:
        """SR_EXIT_AT_VBLANK leaves through _Exit, which skips atexit, so hle.c closes the
        SR_AUDIODUMP file itself on that path whether or not SR_AUDIOSTAT is set (#802). The
        close goes through hle_exit_close_audio_dump, the helper route_fail also calls. The
        audio selftest pins what sr_audio_dump_finish does; this pins that the exit calls the
        helper after the audio statistics rather than inside them, that the helper sits under the
        public-backend guard, and that the backend defines the function it calls. The statistics
        come from the block route_fail shares (#827), which keeps the SR_AUDIOSTAT gate."""
        hle = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        start = hle.index("BOOT_EVENT phase=exit_at_vblank")
        block = hle[start:hle.index("_Exit(0);", start)]
        stats = block.find("hle_exit_audio_stats();")
        self.assertNotEqual(stats, -1, "the vblank exit must print its audio statistics through the shared block")
        self.assertNotIn("audio_stat_on()", block, "the SR_AUDIOSTAT gate belongs to the shared block")
        call = block.find("hle_exit_close_audio_dump();")
        self.assertGreater(call, stats, "the dump close must run after the audio statistics")
        helper_start = hle.index("static void hle_exit_close_audio_dump(void) {")
        helper = hle[helper_start:hle.index("\n}\n", helper_start)]
        guard = "#if !defined(SR_HLE_THREAD_SELFTEST) && defined(SR_PUBLIC_SAFE)\n"
        self.assertIn(guard + "    extern void sr_audio_dump_finish(void);\n"
                              "    sr_audio_dump_finish();\n#endif", helper,
                      "the helper must call the dump close directly under the public-backend guard")
        backend = (ROOT / "src" / "rt" / "audio_unavailable.c").read_text(encoding="utf-8")
        self.assertIn("\nvoid sr_audio_dump_finish(void) {\n", backend)

    def test_route_fail_closes_the_audio_dump_before_exit(self) -> None:
        """route_fail leaves through _Exit(ROUTE_FAIL_EXIT), which skips atexit, so it closes the
        SR_AUDIODUMP file through the same helper as the SR_EXIT_AT_VBLANK exit (#827). The close
        sits inside the exit branch and before _Exit. Under SR_ROUTE_NO_EXIT the run carries on,
        so the close must not run there. Like the vblank pin, this reads the source: no native
        target runs a route failure with the public audio backend linked."""
        hle = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        start = hle.index("static void route_fail(const char *fmt, ...) {")
        body = hle[start:hle.index("\n}\n", start)]
        call = body.find("hle_exit_close_audio_dump();")
        self.assertNotEqual(call, -1, "route_fail exits without closing the SR_AUDIODUMP file")
        branch = body.index('if (!getenv("SR_ROUTE_NO_EXIT")) {')
        self.assertGreater(call, branch,
                           "the dump close must sit inside the exit branch, not under SR_ROUTE_NO_EXIT")
        self.assertLess(call, body.index("_Exit(ROUTE_FAIL_EXIT);"),
                        "the dump close must run before _Exit(ROUTE_FAIL_EXIT)")

    def test_route_fail_reports_the_audio_stats_the_vblank_exit_reports(self) -> None:
        """A failed route leaves through _Exit(ROUTE_FAIL_EXIT) and so loses whatever the SR_AUDIOSTAT
        block would have printed. route_fail must print the same audio statistics as the
        SR_EXIT_AT_VBLANK exit, inside the exit branch, before it finalises the SR_AUDIODUMP file
        (#827). Both exits call one helper so the two cannot drift, and the helper keeps the
        SR_AUDIOSTAT gate. Like the vblank pin, this reads the source: no native target runs a route
        failure with the public audio backend linked."""
        hle = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        start = hle.index("static void route_fail(const char *fmt, ...) {")
        body = hle[start:hle.index("\n}\n", start)]
        stats = body.find("hle_exit_audio_stats();")
        self.assertNotEqual(stats, -1,
                            "route_fail exits without the audio statistics the vblank exit prints")
        branch = body.index('if (!getenv("SR_ROUTE_NO_EXIT")) {')
        self.assertGreater(stats, branch,
                           "the audio statistics must print inside the exit branch")
        self.assertLess(stats, body.find("hle_exit_close_audio_dump();"),
                        "the audio statistics must print before the dump is finalised")
        self.assertEqual(hle.count("static void hle_exit_audio_stats(void) {"), 1,
                         "the vblank exit and route_fail must share one definition of the stats block")
        helper_start = hle.index("static void hle_exit_audio_stats(void) {")
        helper = hle[helper_start:hle.index("\n}\n", helper_start)]
        self.assertIn("    if (!audio_stat_on()) return;\n", helper,
                      "the shared stats block must keep the SR_AUDIOSTAT gate")
        self.assertIn("#ifndef SR_HLE_THREAD_SELFTEST\n", helper,
                      "the stats call must keep the selftest-harness guard the vblank exit uses")
        self.assertIn("    sr_audio_dump_stats();\n", helper)

    def test_every_guest_thread_hard_exit_writes_the_perf_summary_first(self) -> None:
        """sr_perf_shutdown writes the SR_PERF_JSON summary and the final CSV row from an atexit
        hook, and _Exit skips atexit. Every _Exit reached on the guest thread (hle.c and ge.c)
        calls sr_perf_shutdown itself, or a scripted route that ends through SR_EXIT_AT_VBLANK,
        a route failure, a capture exit or the watchdog loses its summary while its CSV stops at
        the last whole second. The call is idempotent and a no-op without SR_PERF."""
        for name in ("hle.c", "ge.c"):
            lines = (ROOT / "src" / "rt" / name).read_text(encoding="utf-8").splitlines()
            found = 0
            for index, line in enumerate(lines):
                code = line.split("//", 1)[0]
                if "_Exit(" not in code or code.lstrip().startswith("*"):
                    continue
                found += 1
                window = "\n".join(lines[max(0, index - 6):index + 1])
                self.assertIn("sr_perf_shutdown();", window,
                              f"{name}:{index + 1} exits without the perf shutdown: {line.strip()}")
            self.assertGreater(found, 0, f"{name} has no _Exit site; the pin is stale")


if __name__ == "__main__":
    unittest.main()
