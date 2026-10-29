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
identity into the compile: a ``-D`` define
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
import sys
import tempfile
import time
import unittest
from pathlib import Path

try:
    import flight_diff
except ModuleNotFoundError:
    from tools import flight_diff
try:
    import scratch_dir
except ModuleNotFoundError:
    from tools import scratch_dir
try:
    from test_build_truth import _scratch_build_root
except ModuleNotFoundError:
    from tools.test_build_truth import _scratch_build_root

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
        self, tmp_path: Path, name: str, defines: list[str], *, source: str = HARNESS_MAIN,
        classes: str = "hle", limit: int = 4,
    ) -> tuple[dict, str, str]:
        main_c = tmp_path / f"{name}.c"
        main_c.write_text(source, encoding="utf-8")
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
        env["SR_FLIGHT"] = f"{classes};{limit}"
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

    def test_named_refusals_from_the_c_recorder_are_events_until_the_real_terminal(self):
        """Schema 7 written by the production C recorder: a refusal is counted, then the end wins."""
        refusal_then_fatal = (
            '#include "flight_recorder.h"\n'
            "\n"
            "int main(void) {\n"
            "    uint64_t sequence;\n"
            "    sr_flight_init();\n"
            "    sequence = sr_flight_hle_import(0x1579a159u, 0u, 0u, 0x08900100u, 0u);\n"
            "    sr_flight_hle_arguments(sequence, 1u, 2u, 3u, 4u);\n"
            "    sr_flight_hle_return(sequence, 0x80110001u);\n"
            "    sr_flight_unsupported(0x1579a159u, 0x80110001u, 0u, 0x08900100u);\n"
            "    sr_flight_fatal(SR_FLIGHT_KIND_FATAL_RAW_SYSCALL, 0x08900200u, 0x0cu, 0u);\n"
            "    return 0;\n"
            "}\n"
        )
        refusal_then_exit = (
            '#include "flight_recorder.h"\n'
            "\n"
            "int main(void) {\n"
            "    sr_flight_init();\n"
            "    sr_flight_unsupported(0x1579a159u, 0x80110001u, 0u, 0x08900100u);\n"
            "    sr_flight_exit(3u);\n"
            "    return 0;\n"
            "}\n"
        )
        with scratch_dir.TemporaryDirectory() as tmp:
            identity = [f'-DSR_BUILD_ID="{FAKE_COMMIT}"']
            fatal_bundle, _, _ = self._build_and_dump(
                Path(tmp), "refusal_fatal", identity, source=refusal_then_fatal,
                classes="hle,unsupported,fatal", limit=8,
            )
            exit_bundle, _, _ = self._build_and_dump(
                Path(tmp), "refusal_exit", identity, source=refusal_then_exit,
                classes="unsupported", limit=8,
            )
        self.assertEqual(fatal_bundle["schema_version"], 7)
        # The recorder links without the scheduler here: no thread source, an empty block.
        self.assertEqual(fatal_bundle["threads"], [])
        self.assertEqual(fatal_bundle["terminal"]["reason"], "fatal")
        self.assertEqual(fatal_bundle["terminal"]["kind"], 12)
        self.assertEqual(fatal_bundle["refusals"]["count"], 1)
        self.assertEqual(fatal_bundle["refusals"]["first_nid"], 0x1579A159)
        self.assertEqual(fatal_bundle["refusals"]["first_pc"], 0x08900100)
        self.assertEqual(
            fatal_bundle["refusals"]["nids"], [{"nid": 0x1579A159, "count": 1}]
        )
        flight_diff.validate_bundle(fatal_bundle)
        self.assertEqual(exit_bundle["terminal"]["reason"], "exit")
        self.assertEqual(exit_bundle["terminal"]["arg0"], 3)
        self.assertEqual(exit_bundle["recorder"]["triggers"]["fired"], 0)
        self.assertEqual(exit_bundle["refusals"]["count"], 1)
        flight_diff.validate_bundle(exit_bundle)

    def test_source_date_epoch_and_build_id_are_recorded(self):
        with scratch_dir.TemporaryDirectory() as tmp:
            bundle, raw, _ = self._build_and_dump(
                Path(tmp),
                "both",
                [
                    f"-DSR_SOURCE_DATE_EPOCH={FAKE_EPOCH}",
                    f'-DSR_BUILD_ID="{FAKE_COMMIT}"',
                ],
            )
        build = bundle["build"]
        self.assertEqual(bundle["schema_version"], 7)
        self.assertEqual(bundle["threads"], [])
        self.assertEqual(build["source_date_epoch"], int(FAKE_EPOCH))
        self.assertEqual(build["build_id"], FAKE_COMMIT)
        self.assertNotIn("compiled_date", build)
        self.assertNotIn("compiled_time", build)
        self.assertNotIn("compiled_date", raw)
        self.assertNotIn("compiled_time", raw)
        flight_diff.validate_bundle(bundle)

    def test_build_id_alone_is_a_valid_identity(self):
        with scratch_dir.TemporaryDirectory() as tmp:
            bundle, _, _ = self._build_and_dump(
                Path(tmp), "commit", [f'-DSR_BUILD_ID="{FAKE_COMMIT}"']
            )
        self.assertIsNone(bundle["build"]["source_date_epoch"])
        self.assertEqual(bundle["build"]["build_id"], FAKE_COMMIT)
        flight_diff.validate_bundle(bundle)

    def test_unidentified_build_records_nulls_and_fails_closed(self):
        with scratch_dir.TemporaryDirectory() as tmp:
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
        with scratch_dir.TemporaryDirectory() as tmp:
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
        with scratch_dir.TemporaryDirectory() as tmp:
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
        with scratch_dir.TemporaryDirectory() as tmp:
            bundle, _, _ = self._build_and_dump(
                Path(tmp), "upper", [f'-DSR_BUILD_ID="{FAKE_COMMIT.upper()}"']
            )
        self.assertEqual(bundle["build"]["build_id"], FAKE_COMMIT.lower())
        flight_diff.validate_bundle(bundle)

    def test_flight_diff_localizes_first_change_in_c_recorder_bundles(self):
        """The production C writer and diff CLI agree on first divergence."""
        harness = r'''#include "flight_recorder.h"

#ifdef SR_TEST_INJECT_DIVERGENCE
#define TEST_NID 0x12345678u
#else
#define TEST_NID 0x50f0c1ecu
#endif

int main(void) {
    uint64_t sequence;
    sr_flight_init();
    sr_flight_record(SR_FLIGHT_CLASS_SCHED, SR_FLIGHT_KIND_SCHED_PICK,
                     0x10u, 0x20u, 0u, 0u);
    sequence = sr_flight_hle_import(TEST_NID, 0x55u, 0x66u, 0x100u, 0x104u);
    sr_flight_hle_arguments(sequence, 0x77u, 2u, 3u, 4u);
    sr_flight_hle_return(sequence, 0u);
    sr_flight_record(SR_FLIGHT_CLASS_SCHED, SR_FLIGHT_KIND_SCHED_BLOCK,
                     0x10u, 0x20u, 0x30u, 0u);
    sr_flight_exit(0u);
    return 0;
}
'''
        with scratch_dir.TemporaryDirectory() as tmp:
            temp_path = Path(tmp)
            source = temp_path / "flight_diff_recorder_fixture.c"
            source.write_text(harness, encoding="utf-8")
            bundles: dict[str, Path] = {}
            for name, extra_defines in (
                ("baseline", []),
                ("injected", ["-DSR_TEST_INJECT_DIVERGENCE"]),
            ):
                executable = temp_path / f"{name}{'.exe' if os.name == 'nt' else ''}"
                bundle_path = temp_path / f"{name}.json"
                compile_result = subprocess.run(
                    [
                        COMPILER,
                        "-std=c99",
                        "-O0",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-I",
                        str(SRC_RT),
                        "-DSR_FLIGHT_RECORDER_LINKED",
                        f'-DSR_BUILD_ID="{FAKE_COMMIT}"',
                        *extra_defines,
                        str(source),
                        "src/rt/flight_recorder.c",
                        "-o",
                        str(executable),
                    ],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(
                    compile_result.returncode,
                    0,
                    compile_result.stdout + compile_result.stderr,
                )
                env = dict(os.environ)
                env.pop("SR_WATCH", None)
                env["SR_FLIGHT"] = "hle,sched,prx;8"
                env["SR_FLIGHT_OUTPUT"] = str(bundle_path)
                run_result = subprocess.run(
                    [str(executable)],
                    cwd=temp_path,
                    env=env,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(run_result.returncode, 0, run_result.stderr)
                bundles[name] = bundle_path

            baseline = flight_diff.load_bundle(bundles["baseline"])
            injected = flight_diff.load_bundle(bundles["injected"])
            self.assertEqual(
                [event["class"] for event in baseline["events"]],
                ["sched", "hle", "prx", "sched"],
            )
            self.assertEqual(
                [event["class"] for event in injected["events"]],
                ["sched", "hle", "sched"],
            )
            self.assertEqual(baseline["events"][1]["arg0"], 0x50F0C1EC)
            self.assertEqual(injected["events"][1]["arg0"], 0x12345678)
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "flight_diff.py"),
                    str(bundles["baseline"]),
                    str(bundles["injected"]),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("DIVERGENCE: sequence 2", result.stdout)
        self.assertIn(f'"arg0":{0x50F0C1EC}', result.stdout)
        self.assertIn(f'"arg0":{0x12345678}', result.stdout)

    def test_flight_diff_localizes_first_media_change_in_mixed_recorder_bundles(self):
        """A source-owned media milestone diverges before the later symptom.

        The limit-4 capture slides both rings, so that pair is INCOMPARABLE
        under the #320 comparability contract: identical retained windows
        cannot vouch for the dropped events. The same programs re-captured
        under a limit that retains every event are comparable, and the CLI
        still names the media milestone as the first divergence.
        """
        harness = r'''#include "flight_recorder.h"

#ifdef SR_TEST_INJECT_MEDIA
#define MEDIA_MARK 0x2222u
#else
#define MEDIA_MARK 0x1111u
#endif

int main(void) {
    uint64_t sequence;
    sr_flight_init();
    sr_flight_record(SR_FLIGHT_CLASS_SCHED, SR_FLIGHT_KIND_SCHED_PICK,
                     0x10u, 0x20u, 0u, 0u);
    sequence = sr_flight_hle_import(0x50f0c1ecu, 0x55u, 0x66u, 0x100u, 0x104u);
    sr_flight_hle_arguments(sequence, 0x77u, 2u, 3u, 4u);
    sr_flight_hle_return(sequence, 0u);
    sr_flight_record(SR_FLIGHT_CLASS_MEDIA, SR_FLIGHT_KIND_MEDIA_TEST_MILESTONE,
                     SR_FLIGHT_MEDIA_TEST_AU_READY, MEDIA_MARK, 0u, 1u);
    sr_flight_record(SR_FLIGHT_CLASS_SCHED, SR_FLIGHT_KIND_SCHED_BLOCK,
                     0x10u, 0x20u, 0x30u, 0u);
    sr_flight_exit(0u);
    return 0;
}
'''
        with scratch_dir.TemporaryDirectory() as tmp:
            temp_path = Path(tmp)
            source = temp_path / "flight_media_recorder_fixture.c"
            source.write_text(harness, encoding="utf-8")
            bundles: dict[str, Path] = {}
            executables: dict[str, Path] = {}
            for name, extra_defines in (
                ("baseline", []),
                ("injected", ["-DSR_TEST_INJECT_MEDIA"]),
            ):
                executable = temp_path / f"{name}{'.exe' if os.name == 'nt' else ''}"
                bundle_path = temp_path / f"{name}.json"
                compile_result = subprocess.run(
                    [
                        COMPILER,
                        "-std=c99",
                        "-O0",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-I",
                        str(SRC_RT),
                        "-DSR_FLIGHT_RECORDER_LINKED",
                        f'-DSR_BUILD_ID="{FAKE_COMMIT}"',
                        *extra_defines,
                        str(source),
                        "src/rt/flight_recorder.c",
                        "-o",
                        str(executable),
                    ],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(
                    compile_result.returncode,
                    0,
                    compile_result.stdout + compile_result.stderr,
                )
                env = dict(os.environ)
                env.pop("SR_WATCH", None)
                env["SR_FLIGHT"] = "hle,sched,prx,media;4"
                env["SR_FLIGHT_OUTPUT"] = str(bundle_path)
                run_result = subprocess.run(
                    [str(executable)],
                    cwd=temp_path,
                    env=env,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(run_result.returncode, 0, run_result.stderr)
                executables[name] = executable
                bundles[name] = bundle_path

            baseline = flight_diff.load_bundle(bundles["baseline"])
            injected = flight_diff.load_bundle(bundles["injected"])
            self.assertEqual(
                [event["class"] for event in baseline["events"]],
                ["hle", "prx", "media", "sched"],
            )
            self.assertEqual(
                [event["sequence"] for event in baseline["events"]],
                [2, 3, 4, 5],
            )
            self.assertEqual(baseline["recorder"]["limit"], 4)
            self.assertEqual(baseline["recorder"]["recorded"], 5)
            self.assertEqual(baseline["recorder"]["dropped"], 1)
            self.assertEqual(baseline["events"][2]["arg1"], 0x1111)
            self.assertEqual(injected["events"][2]["arg1"], 0x2222)
            # Both limit-4 runs slid their ring: sequence 1 is gone on both
            # sides. Even though the retained windows agree, the comparator
            # must refuse the pair (#320): the dropped events could differ.
            with self.assertRaises(flight_diff.BundleIncomparable):
                flight_diff.diff_bundles(baseline, injected, "sequence")

            # Re-capture the same executables under a limit that retains every
            # event: no drops on either side, so the pair is comparable and
            # the media milestone is still the first reported divergence.
            bundles_full: dict[str, Path] = {}
            for name in ("baseline", "injected"):
                bundle_path = temp_path / f"{name}-full.json"
                env = dict(os.environ)
                env.pop("SR_WATCH", None)
                env["SR_FLIGHT"] = "hle,sched,prx,media;8"
                env["SR_FLIGHT_OUTPUT"] = str(bundle_path)
                run_result = subprocess.run(
                    [str(executables[name])],
                    cwd=temp_path,
                    env=env,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(run_result.returncode, 0, run_result.stderr)
                bundles_full[name] = bundle_path

            baseline_full = flight_diff.load_bundle(bundles_full["baseline"])
            injected_full = flight_diff.load_bundle(bundles_full["injected"])
            self.assertEqual(baseline_full["recorder"]["dropped"], 0)
            self.assertEqual(injected_full["recorder"]["dropped"], 0)
            self.assertEqual(
                [event["sequence"] for event in baseline_full["events"]],
                [1, 2, 3, 4, 5],
            )
            divergence = flight_diff.diff_bundles(baseline_full, injected_full, "sequence")
            self.assertIsNotNone(divergence)
            self.assertEqual(divergence[0], "sequence 4")
            self.assertEqual(divergence[1]["arg1"], 0x1111)
            self.assertEqual(divergence[2]["arg1"], 0x2222)
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "flight_diff.py"),
                    str(bundles_full["baseline"]),
                    str(bundles_full["injected"]),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("DIVERGENCE: sequence 4", result.stdout)
            self.assertIn('"arg1":4369', result.stdout)
            self.assertIn('"arg1":8738', result.stdout)


# ---- Schema 7 trigger modes and the watch class (#320, folded from #1433) ----
#
# One source-owned harness drives the production recorder through the same entry points the
# runtime calls (HLE import/arguments/return, sr_flight_record, sr_watch_store, sr_flight_fatal,
# sr_flight_exit). argv[1] picks the scenario; the environment arms SR_FLIGHT, SR_FLIGHT_TRIGGER
# and SR_WATCH exactly as a player run would.

TRIGGER_HARNESS = r'''#include "flight_recorder.h"

#include <string.h>

#define PC_B 0x08804010u
#define PC_A (PC_B - 8u) /* A's return address is PC_B: the pc mode must not match an ra */
#ifdef SR_TEST_INJECT_DIVERGENCE
#define FIRST_ARGUMENT 0x99u
#else
#define FIRST_ARGUMENT 0x01u
#endif

static void hle(uint32_t nid, uint32_t pc, uint32_t argument, uint32_t result) {
    uint64_t sequence = sr_flight_hle_import(nid, 0x10u, argument, pc, pc + 8u);
    sr_flight_hle_arguments(sequence, argument, 2u, 3u, 4u);
    sr_flight_hle_return(sequence, result);
}

static void pick(uint32_t uid) {
    sr_flight_record(SR_FLIGHT_CLASS_SCHED, SR_FLIGHT_KIND_SCHED_PICK, uid, uid, 0u, 0u);
}

static void block(uint32_t uid) {
    sr_flight_record(SR_FLIGHT_CLASS_SCHED, SR_FLIGHT_KIND_SCHED_BLOCK, uid, uid, 0u, 0u);
}

static void draw(uint32_t list, uint32_t vertices) {
    sr_flight_record(SR_FLIGHT_CLASS_GE, SR_FLIGHT_KIND_GE_DRAW, list, list + 4u * vertices, 3u,
                     vertices);
}

int main(int argc, char **argv) {
    const char *scenario = argc > 1 ? argv[1] : "";
    sr_flight_init();
    if (strcmp(scenario, "pc") == 0) {
        pick(0x10u);
        hle(0x12345678u, PC_A, FIRST_ARGUMENT, 0u);
        pick(0x11u);
        hle(0x23456789u, PC_B, 7u, 0x80000000u);
        hle(0x23456789u, PC_B, 8u, 0u);
        pick(0x12u);
    } else if (strcmp(scenario, "ge-draw") == 0) {
        sr_flight_record(SR_FLIGHT_CLASS_GE, SR_FLIGHT_KIND_GE_LIST_ENQUEUE, 0x08900000u, 0u, 1u, 0u);
        draw(0x08900000u, 3u);
        draw(0x08900000u, 4u);
        draw(0x08900000u, 5u);
        sr_flight_record(SR_FLIGHT_CLASS_PRESENT, SR_FLIGHT_KIND_PRESENT_SET_FRAMEBUF,
                         0x04000000u, 3u, 512u, 1u);
    } else if (strcmp(scenario, "watch") == 0) {
        sr_watch_store(0x08800100u, 0x08a00000u, 0x11223344u, 4u); /* first write */
        sr_watch_store(0x08800104u, 0x08a00000u, 0x11223344u, 4u); /* same bytes: nothing */
        sr_watch_store(0x08800108u, 0x08a00010u, 0x55u, 1u);       /* outside the range */
        sr_watch_store(0x0880010cu, 0x08a00002u, 0x99u, 1u);       /* 0x22 -> 0x99 */
        sr_watch_store(0x08800110u, 0x08a00006u, 0xaabbccddu, 4u); /* straddles the end */
    } else if (strcmp(scenario, "checkpoint") == 0) {
        for (uint32_t uid = 1u; uid <= 4u; ++uid) {
            pick(uid);
            block(uid);
        }
    } else if (strcmp(scenario, "checkpoint-fatal") == 0) {
        pick(1u);
        pick(2u);
        sr_flight_fatal(SR_FLIGHT_KIND_FATAL_RAW_SYSCALL, 0x08800200u, 0x0cu, 0u);
        pick(3u);
    } else {
        return 2;
    }
    sr_flight_exit(0u);
    return 0;
}
'''

TRIGGER_PC_A = 0x08804008
TRIGGER_PC_B = 0x08804010


@requires_compiler
class RecorderTriggerModeTests(unittest.TestCase):
    """Each SR_FLIGHT_TRIGGER mode fires exactly at its event in a C-recorder bundle."""

    @classmethod
    def setUpClass(cls):
        cls._scratch = scratch_dir.TemporaryDirectory()
        cls.tmp = Path(cls._scratch.name)
        cls.executables = {}
        for name, defines in (("baseline", []), ("injected", ["-DSR_TEST_INJECT_DIVERGENCE"])):
            source = cls.tmp / "flight_trigger_fixture.c"
            source.write_text(TRIGGER_HARNESS, encoding="utf-8")
            executable = cls.tmp / f"trigger_{name}{'.exe' if os.name == 'nt' else ''}"
            result = subprocess.run(
                [COMPILER, "-std=c99", "-O0", "-Wall", "-Wextra", "-Werror", "-I", str(SRC_RT),
                 "-DSR_FLIGHT_RECORDER_LINKED", f'-DSR_BUILD_ID="{FAKE_COMMIT}"', *defines,
                 str(source), "src/rt/flight_recorder.c", "-o", str(executable)],
                cwd=ROOT, capture_output=True, text=True, check=False,
            )
            if result.returncode != 0:
                cls._scratch.cleanup()
                raise AssertionError(result.stdout + result.stderr)
            cls.executables[name] = executable

    @classmethod
    def tearDownClass(cls):
        cls._scratch.cleanup()

    def _run(self, scenario, flight, trigger=None, watch=None, *, name="baseline",
             tag=None):
        bundle_path = self.tmp / f"{tag or scenario}-{name}.json"
        if bundle_path.exists():
            bundle_path.unlink()
        env = dict(os.environ)
        for key in ("SR_FLIGHT_TRIGGER", "SR_WATCH"):
            env.pop(key, None)
        env["SR_FLIGHT"] = flight
        env["SR_FLIGHT_OUTPUT"] = str(bundle_path)
        if trigger is not None:
            env["SR_FLIGHT_TRIGGER"] = trigger
        if watch is not None:
            env["SR_WATCH"] = watch
        result = subprocess.run([str(self.executables[name]), scenario], cwd=self.tmp, env=env,
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        bundle = (json.loads(bundle_path.read_text(encoding="utf-8"))
                  if bundle_path.exists() else None)
        return bundle, bundle_path, result.stderr

    def _assert_trigger(self, bundle, *, kind, arg0, sequence, recorded):
        flight_diff.validate_bundle(bundle)
        self.assertEqual(bundle["schema_version"], 7)
        self.assertEqual(bundle["terminal"],
                         {"reason": "trigger", "sequence": sequence, "kind": kind, "arg0": arg0})
        self.assertEqual(bundle["recorder"]["triggers"]["fired"], 1)
        self.assertEqual(bundle["recorder"]["recorded"], recorded)

    def test_untriggered_bundle_records_mode_none(self):
        bundle, _, _ = self._run("pc", "hle,sched;16")
        flight_diff.validate_bundle(bundle)
        triggers = bundle["recorder"]["triggers"]
        self.assertEqual((triggers["mode"], triggers["pc"], triggers["ge_draw"],
                          triggers["checkpoint"]), ("none", None, None, None))
        self.assertIsNone(bundle["recorder"]["watch_range"])
        self.assertEqual(bundle["terminal"]["reason"], "exit")
        self.assertEqual(bundle["recorder"]["recorded"], 6)

    def test_pc_mode_fires_at_the_first_event_at_that_pc_not_its_return_address(self):
        bundle, _, _ = self._run("pc", "hle,sched;16", f"pc:0x{TRIGGER_PC_B:08x}")
        self._assert_trigger(bundle, kind=1, arg0=TRIGGER_PC_B, sequence=4, recorded=4)
        self.assertEqual(bundle["recorder"]["triggers"]["pc"], TRIGGER_PC_B)
        matched = bundle["events"][-1]
        self.assertEqual((matched["arg2"], matched["arg3"]), (TRIGGER_PC_B, TRIGGER_PC_B + 8))
        # The freeze caught the import mid-call; the final rewrite carries its arguments and
        # its return value.
        self.assertEqual(matched["arguments"], [7, 2, 3, 4])
        self.assertEqual(matched["return_value"], 0x80000000)

    def test_ge_draw_mode_fires_at_the_nth_draw(self):
        bundle, _, _ = self._run("ge-draw", "ge,present;16", "ge-draw:2")
        self._assert_trigger(bundle, kind=25, arg0=2, sequence=3, recorded=3)
        self.assertEqual(bundle["events"][-1]["arg3"], 4)

    def test_watch_class_records_value_changes_only(self):
        bundle, _, stderr = self._run("watch", "watch;16", watch="0x08a00000:8")
        flight_diff.validate_bundle(bundle)
        self.assertEqual(bundle["recorder"]["watch_range"], {"address": 0x08A00000, "length": 8})
        stores = [(event["arg0"], event["arg1"], event["arg2"], event["arg3"])
                  for event in bundle["events"]]
        self.assertEqual(stores, [
            (0x08800100, 0x08A00000, 4, 0x11223344),
            (0x0880010C, 0x08A00002, 1 | flight_diff.WATCH_KNOWN_BIT, 0x99),
            (0x08800110, 0x08A00006, 4, 0xAABBCCDD),
        ])
        self.assertEqual(stderr.count("SR_WATCH: pc="), 4, "the stderr report is unchanged")

    def test_watch_mode_fires_at_the_first_change_and_checkpoint_at_a_later_one(self):
        first, _, _ = self._run("watch", "watch;16", "watch", "0x08a00000:8", tag="watch-first")
        self._assert_trigger(first, kind=31, arg0=0x08A00000, sequence=1, recorded=1)
        second, _, _ = self._run("watch", "watch;16", "checkpoint:31:2:0", "0x08a00000:8",
                                 tag="watch-second")
        self._assert_trigger(second, kind=31, arg0=2, sequence=2, recorded=2)
        self.assertTrue(second["events"][-1]["arg2"] & flight_diff.WATCH_KNOWN_BIT)

    def test_checkpoint_keeps_its_after_window_within_the_ring_limit(self):
        bundle, _, _ = self._run("checkpoint", "sched;4", "checkpoint:3:2:2")
        self._assert_trigger(bundle, kind=3, arg0=2, sequence=3, recorded=5)
        self.assertEqual(bundle["recorder"]["dropped"], 1)
        self.assertEqual([event["sequence"] for event in bundle["events"]], [2, 3, 4, 5])
        self.assertEqual(bundle["recorder"]["triggers"]["checkpoint"],
                         {"kind": 3, "occurrence": 2, "after": 2})

    def test_a_run_ending_inside_the_window_keeps_the_trigger_terminal(self):
        bundle, _, _ = self._run("checkpoint", "sched;8", "checkpoint:3:4:3", tag="short-window")
        self._assert_trigger(bundle, kind=3, arg0=4, sequence=7, recorded=8)

    def test_a_fatal_inside_the_window_wins_the_terminal(self):
        bundle, _, _ = self._run("checkpoint-fatal", "sched,fatal;8", "checkpoint:3:1:3")
        flight_diff.validate_bundle(bundle)
        self.assertEqual(bundle["terminal"]["reason"], "fatal")
        self.assertEqual(bundle["terminal"]["kind"], 12)
        self.assertEqual(bundle["recorder"]["recorded"], 3)

    def test_refused_specs_disable_the_recorder_and_say_why(self):
        cases = (
            ("pc", "hle;8", "pc:", None),
            ("pc", "hle;8", "pc:0x1g", None),
            ("pc", "hle;8", "pc:0x100000000", None),
            ("ge-draw", "ge;8", "ge-draw:0", None),
            ("ge-draw", "present;8", "ge-draw:2", None),
            ("watch", "hle;8", "watch", None),
            ("checkpoint", "sched,fatal;8", "checkpoint:13:1:0", None),
            ("checkpoint", "sched;4", "checkpoint:3:1:4", None),
            ("checkpoint", "sched;8", "checkpoint:99:1:0", None),
            ("checkpoint", "hle;8", "checkpoint:3:1:0", None),
            ("checkpoint", "sched;8", "checkpoint:3:0:0", None),
            ("pc", "hle;8", "bogus", None),
        )
        for index, (scenario, flight, trigger, watch) in enumerate(cases):
            with self.subTest(trigger=trigger, flight=flight):
                bundle, _, stderr = self._run(scenario, flight, trigger, watch, tag=f"refused{index}")
                self.assertIsNone(bundle, "a refused trigger spec must not record")
                self.assertIn("SR_FLIGHT_TRIGGER:", stderr)
                self.assertIn("recorder disabled", stderr)

    def test_watch_class_needs_a_small_armed_watch_range(self):
        for index, watch in enumerate((None, "0x08a00000:65")):
            with self.subTest(watch=watch):
                bundle, _, stderr = self._run("watch", "watch;8", None, watch, tag=f"range{index}")
                self.assertIsNone(bundle)
                self.assertIn("the watch class needs SR_WATCH=ADDR:LEN with LEN at most 64", stderr)

    def test_flight_diff_names_the_first_divergence_under_one_trigger_policy(self):
        trigger = f"pc:0x{TRIGGER_PC_B:08x}"
        _, baseline, _ = self._run("pc", "hle,sched;16", trigger, tag="diff-pc")
        _, injected, _ = self._run("pc", "hle,sched;16", trigger, name="injected", tag="diff-pc")
        _, other_policy, _ = self._run("pc", "hle,sched;16", f"pc:0x{TRIGGER_PC_A:08x}",
                                       tag="diff-other")
        tool = str(ROOT / "tools" / "flight_diff.py")
        result = subprocess.run([sys.executable, tool, str(baseline), str(injected)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, flight_diff.EXIT_DIVERGENCE, result.stdout + result.stderr)
        self.assertIn(f"trigger mode: pc=0x{TRIGGER_PC_B:08x}", result.stdout)
        self.assertIn("DIVERGENCE: sequence 2", result.stdout)
        result = subprocess.run([sys.executable, tool, str(baseline), str(other_policy)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, flight_diff.EXIT_INCOMPARABLE,
                         result.stdout + result.stderr)
        self.assertIn("trigger policies differ", result.stdout)


class RecorderTriggerHotPathTests(unittest.TestCase):
    """Checking a trigger mode stays arithmetic (#320 acceptance 5, #1433 item 3).

    tools/test_flight_diff.py RecorderTimingTests already refuses any sleep, yield or
    scheduler call in the recorder. This adds the trigger and watch paths: recording an event,
    checking a mode and noting a watched store do no I/O, no environment read and no heap
    work. The only I/O is the one bundle write at a freeze, a terminal or exit.
    """

    HOT_PATHS = ("static uint64_t record_locked(", "static uint64_t record_checked(",
                 "static void check_trigger(", "static int event_pc(",
                 "static void watch_flight_note(", "void sr_flight_record(",
                 "uint64_t sr_flight_hle_import(", "int sr_flight_class_enabled(")
    IO = re.compile(r"\b(?:fopen|fprintf|fputs|fputc|fwrite|fflush|getenv|malloc|free)\s*\(")

    def _source(self) -> str:
        return re.sub(r"/\*.*?\*/|//[^\n]*", "", RECORDER.read_text(encoding="utf-8"), flags=re.S)

    @staticmethod
    def _body(source: str, signature: str) -> str:
        start = source.index(signature)
        # Skip forward declarations: the definition is the occurrence whose "{" precedes any ";".
        while source.index(";", start) < source.index("{", start):
            start = source.index(signature, start + 1)
        brace = source.index("{", start)
        depth = 0
        for index in range(brace, len(source)):
            depth += {"{": 1, "}": -1}.get(source[index], 0)
            if depth == 0:
                return source[brace:index + 1]
        raise AssertionError(f"unterminated body for {signature}")

    def test_event_and_trigger_paths_do_no_io_or_allocation(self):
        source = self._source()
        for signature in self.HOT_PATHS:
            with self.subTest(path=signature):
                self.assertEqual(self.IO.findall(self._body(source, signature)), [],
                                 "recording or checking a trigger must not touch I/O or the heap")


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
        # A scratch BUILD_ROOT: even a dry run parses the Makefile, and that writes the
        # SDL3 discovery fragment beneath BUILD_ROOT, which must not be the checkout's
        # build/. The scratch path is Make-safe (no whitespace) by construction.
        build_root = _scratch_build_root(self, "identity-probe")
        result = subprocess.run(
            [make, "--no-print-directory", "-p", "-n", "clean-preview",
             f"BUILD_ROOT={build_root.as_posix()}", *overrides],
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

    def test_psmf_media_selftest_recipe_forwards_build_identity(self):
        """The direct media selftest compile emits identity-bound bundles."""
        make = shutil.which("mingw32-make") or shutil.which("make")
        self.assertTrue(make, "GNU Make is required for the build-identity check")
        environment = os.environ.copy()
        environment["SOURCE_DATE_EPOCH"] = FAKE_EPOCH
        result = subprocess.run(
            [
                make,
                "--no-print-directory",
                "--dry-run",
                f"BUILD_ROOT={_scratch_build_root(self, 'flight-media-identity').as_posix()}",
                f"SR_SOURCE_COMMIT={FAKE_COMMIT}",
                "psmf-media-selftest",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=300,
            env=environment,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        continuation_crlf = chr(92) + chr(13) + chr(10)
        continuation_lf = chr(92) + chr(10)
        flattened = result.stdout.replace(continuation_crlf, " ").replace(
            continuation_lf, " "
        )
        compile_line = next(
            line
            for line in flattened.splitlines()
            if "src/rt/psmf_media_selftest.c" in line
        )
        self.assertIn("-DSR_BUILD_ID=", compile_line)
        self.assertIn(FAKE_COMMIT, compile_line)
        self.assertIn(f"-DSR_SOURCE_DATE_EPOCH={FAKE_EPOCH}", compile_line)


if __name__ == "__main__":
    unittest.main()
