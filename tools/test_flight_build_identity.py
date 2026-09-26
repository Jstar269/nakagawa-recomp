#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Reproducible build identity of the flight recorder's ``build`` block.

``src/rt/flight_recorder.c`` used to stamp ``__DATE__``/``__TIME__`` into every
runtime binary, which made two builds of identical inputs differ in the compile
clock.  This module pins the replacement contract:

* two compilations of ``src/rt/flight_recorder.c`` with identical inputs,
  separated by at least one wall-clock second, produce byte-identical objects,
  so the compile clock cannot hide in the object;
* the written bundle records the reproducible identity the *build* forwarded --
  ``SOURCE_DATE_EPOCH`` as ``build.source_date_epoch`` (the
  reproducible-builds.org convention) and the source commit as
  ``build.build_id`` -- and never a wall clock;
* an ad-hoc compile that forwards no identity records nulls, and the sanitizer
  in ``tools/flight_diff.py`` refuses an identity-less bundle fail closed.

The identity is transported the way the Makefile already transports build
identity into the compile: a ``-D`` define alongside ``-DSR_BUILD_DIR``
(``SOURCE_DATE_EPOCH`` -> ``-DSR_SOURCE_DATE_EPOCH``, the source commit
``git rev-parse HEAD`` -> ``-DSR_BUILD_ID``).
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

try:
    import flight_diff
except ModuleNotFoundError:
    from tools import flight_diff

ROOT = Path(__file__).resolve().parent.parent
SRC_RT = ROOT / "src" / "rt"
RECORDER = SRC_RT / "flight_recorder.c"

# A fixed synthetic identity: a 40-hex source commit and a fixed epoch. No clock
# value is used anywhere in this module.
FAKE_COMMIT = "0123456789abcdef0123456789abcdef01234567"
FAKE_EPOCH = "1758742400"

HARNESS_MAIN = (
    '#include "flight_recorder.h"\n'
    "\n"
    "int main(void) {\n"
    "    uint64_t sequence;\n"
    "    sr_flight_init();\n"
    "    sequence = sr_flight_hle_import(0x1234u, 0u, 0u, 0u, 0u);\n"
    "    sr_flight_hle_arguments(sequence, 1u, 2u, 3u, 4u);\n"
    "    sr_flight_hle_return(sequence, 0u);\n"
    "    sr_flight_exit(0u);\n"
    "    return 0;\n"
    "}\n"
)


def _find_compiler() -> str:
    for candidate in (os.environ.get("CC", ""), "gcc", "cc", "clang"):
        word = candidate.split()[0] if candidate.strip() else ""
        if not word:
            continue
        resolved = shutil.which(word)
        if resolved:
            return resolved
    return ""


COMPILER = _find_compiler()
requires_compiler = unittest.skipUnless(COMPILER, "no C compiler on PATH")


class RecorderObjectTests(unittest.TestCase):
    """Two compiles of identical inputs must produce identical objects."""

    def _compile(self, output: Path) -> None:
        result = subprocess.run(
            [
                COMPILER,
                "-std=c99",
                "-O0",
                "-I",
                str(SRC_RT),
                "-DSR_FLIGHT_RECORDER_LINKED",
                "-c",
                "src/rt/flight_recorder.c",
                "-o",
                str(output),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @requires_compiler
    def test_two_compilations_produce_identical_objects(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            first = tmp_path / "first.o"
            second = tmp_path / "second.o"
            self._compile(first)
            # Cross at least one wall-clock second so a __TIME__ stamp (or any
            # other clock stamp) would change the second object.
            time.sleep(1.1)
            self._compile(second)
            left = first.read_bytes()
            right = second.read_bytes()
            self.assertEqual(
                hashlib.sha256(left).hexdigest(),
                hashlib.sha256(right).hexdigest(),
                "two compiles of identical inputs produced different objects; "
                "flight_recorder.c still embeds the compile clock",
            )


@requires_compiler
class RecorderBundleTests(unittest.TestCase):
    """The bundle's build block records the build's reproducible identity."""

    def _build_and_dump(
        self, tmp_path: Path, name: str, defines: list[str]
    ) -> tuple[dict, str, str]:
        main_c = tmp_path / f"{name}.c"
        main_c.write_text(HARNESS_MAIN, encoding="utf-8")
        exe = tmp_path / f"{name}{'.exe' if os.name == 'nt' else ''}"
        bundle_path = tmp_path / f"{name}.json"
        compile_result = subprocess.run(
            [
                COMPILER,
                "-std=c99",
                "-O0",
                "-I",
                str(SRC_RT),
                "-DSR_FLIGHT_RECORDER_LINKED",
                *defines,
                str(main_c),
                "src/rt/flight_recorder.c",
                "-o",
                str(exe),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(
            compile_result.returncode, 0, compile_result.stdout + compile_result.stderr
        )
        env = dict(os.environ)
        env["SR_FLIGHT"] = "hle;4"
        env["SR_FLIGHT_OUTPUT"] = str(bundle_path)
        run_result = subprocess.run(
            [str(exe)],
            cwd=tmp_path,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(run_result.returncode, 0, run_result.stdout + run_result.stderr)
        raw = bundle_path.read_text(encoding="utf-8")
        return json.loads(raw), raw, run_result.stderr

    def test_source_date_epoch_and_build_id_are_recorded(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle, raw, _ = self._build_and_dump(
                Path(tmp),
                "both",
                [
                    f"-DSR_SOURCE_DATE_EPOCH={FAKE_EPOCH}",
                    f'-DSR_BUILD_ID="{FAKE_COMMIT}"',
                ],
            )
        build = bundle["build"]
        self.assertEqual(bundle["schema_version"], 3)
        self.assertEqual(build["source_date_epoch"], int(FAKE_EPOCH))
        self.assertEqual(build["build_id"], FAKE_COMMIT)
        self.assertNotIn("compiled_date", build)
        self.assertNotIn("compiled_time", build)
        self.assertNotIn("compiled_date", raw)
        self.assertNotIn("compiled_time", raw)
        flight_diff.validate_bundle(bundle)

    def test_build_id_alone_is_a_valid_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle, _, _ = self._build_and_dump(
                Path(tmp), "commit", [f'-DSR_BUILD_ID="{FAKE_COMMIT}"']
            )
        self.assertIsNone(bundle["build"]["source_date_epoch"])
        self.assertEqual(bundle["build"]["build_id"], FAKE_COMMIT)
        flight_diff.validate_bundle(bundle)

    def test_unidentified_build_records_nulls_and_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle, raw, _ = self._build_and_dump(Path(tmp), "bare", [])
        build = bundle["build"]
        self.assertIsNone(build["source_date_epoch"])
        self.assertIsNone(build["build_id"])
        self.assertNotIn("compiled_date", raw)
        self.assertNotIn("compiled_time", raw)
        with self.assertRaisesRegex(
            flight_diff.FlightDiffError, "reproducible build identity"
        ):
            flight_diff.validate_bundle(bundle)

    def test_non_numeric_source_date_epoch_fails_closed_to_null(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle, raw, stderr = self._build_and_dump(
                Path(tmp),
                "bad-epoch",
                ["-DSR_SOURCE_DATE_EPOCH=not-a-number", f'-DSR_BUILD_ID="{FAKE_COMMIT}"'],
            )
        self.assertIsNone(bundle["build"]["source_date_epoch"])
        self.assertEqual(bundle["build"]["build_id"], FAKE_COMMIT)
        self.assertNotIn("not-a-number", raw)
        self.assertIn("SR_FLIGHT", stderr)
        flight_diff.validate_bundle(bundle)

    def test_epoch_beyond_the_schema_maximum_fails_closed_to_null(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle, raw, stderr = self._build_and_dump(
                Path(tmp),
                "huge-epoch",
                ["-DSR_SOURCE_DATE_EPOCH=253402300800", f'-DSR_BUILD_ID="{FAKE_COMMIT}"'],
            )
        self.assertIsNone(bundle["build"]["source_date_epoch"])
        self.assertNotIn("253402300800", raw)
        self.assertIn("SR_FLIGHT", stderr)
        flight_diff.validate_bundle(bundle)

    def test_uppercase_build_id_is_recorded_lowercase(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle, _, _ = self._build_and_dump(
                Path(tmp), "upper", [f'-DSR_BUILD_ID="{FAKE_COMMIT.upper()}"']
            )
        self.assertEqual(bundle["build"]["build_id"], FAKE_COMMIT.lower())
        flight_diff.validate_bundle(bundle)


class MakeIdentityResolutionTests(unittest.TestCase):
    """How the Makefile resolves the commit that becomes ``build.build_id``.

    The default used to be a bare ``$(shell git rev-parse HEAD)``. Make spawns a
    missing program itself, so on a host without git on PATH -- the Windows
    runtime compile gate runs in an MSYS2 shell that has none -- the build log
    carried ``process_begin: CreateProcess(NULL, git rev-parse HEAD, ...) failed``
    and every binary of that job recorded an empty identity. These tests pin the
    replacement contract: a missing git yields an empty identity and NO error
    text, an identity the caller supplies wins outright, and no default reaches
    for git through make's own process spawn.
    """

    def _resolved(self, *overrides: str, env: dict[str, str] | None = None):
        """Resolve SR_SOURCE_COMMIT the way the Makefile does, in a dry run.

        ``clean-preview`` is a real target that skips every parse-time side
        effect (profile stamps, build directories, toolchain discovery), so the
        dump shows the resolved identity without the tree being touched. ``-p``
        makes GNU Make print its variable database.
        """
        make = shutil.which("mingw32-make") or shutil.which("make")
        self.assertTrue(make, "GNU Make is required for the build-identity check")
        environment = dict(os.environ)
        environment.update(env or {})
        result = subprocess.run(
            [make, "--no-print-directory", "-p", "-n", "clean-preview",
             f"BUILD_DIR={(ROOT / 'build' / 'identity-probe').as_posix()}", *overrides],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=300,
            env=environment,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # GNU Make prints the variable database twice and marks a value with `:=`
        # (file origin) or `=` (command line/environment origin); both spellings
        # carry the resolved identity, and both must agree.
        values: set[str] = set()
        for line in result.stdout.splitlines():
            match = re.match(r"^SR_SOURCE_COMMIT\s*(:?=)\s*(.*)$", line)
            if match:
                values.add(match.group(2).strip())
        return values, result.stdout + result.stderr

    def test_no_identity_default_spawns_git_through_make(self):
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("GIT ?= git", makefile)
        self.assertNotIn("$(shell git rev-parse HEAD)", makefile)
        # Every commit-stamped default reads the one guarded resolution.
        for name in ("SR_SOURCE_COMMIT", "PSP_ORACLE_SOURCE_COMMIT", "PSP_VFPU_ORACLE_COMMIT"):
            with self.subTest(variable=name):
                self.assertRegex(makefile, rf"(?m)^{name} \?= \$\(NK_GIT_REV_HEAD\)$")

    def test_missing_git_yields_no_identity_and_no_error_text(self):
        values, output = self._resolved("GIT=/nonexistent/nakagawa-no-such-git")
        self.assertEqual(values, {""}, output)
        for noise in ("process_begin", "CreateProcess", "No such file or directory"):
            self.assertNotIn(noise, output)

    def test_caller_supplied_identity_wins_over_git(self):
        for overrides, env in (
            ((f"SR_SOURCE_COMMIT={FAKE_COMMIT}",), None),
            ((), {"SR_SOURCE_COMMIT": FAKE_COMMIT}),
        ):
            with self.subTest(env=bool(env)):
                values, output = self._resolved(
                    *overrides, env=env or {}
                )
                self.assertEqual(values, {FAKE_COMMIT}, output)

    def test_caller_supplied_identity_wins_over_a_missing_git(self):
        """The Windows job's environment: no git, identity forwarded by CI."""
        values, output = self._resolved(
            "GIT=/nonexistent/nakagawa-no-such-git",
            env={"SR_SOURCE_COMMIT": FAKE_COMMIT},
        )
        self.assertEqual(values, {FAKE_COMMIT}, output)
        self.assertNotIn("process_begin", output)


if __name__ == "__main__":
    unittest.main()
