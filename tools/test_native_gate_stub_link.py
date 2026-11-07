# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Keep the headless codegen-gate link surface synchronized with the runtime."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from makefile_text import read_makefile  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / "src" / "rt"
GATE_STUB = ROOT / "tools" / "gate_stub.c"
CI_WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"
MAKEFILE = ROOT / "Makefile"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")

_SRC_TOKEN = re.compile(r"\b(?:src|tests/native)/[\w./-]+\.(?:c|cpp)\b")

# Hosted-CI inline builds whose source lists duplicate a canonical Makefile
# target recipe. PR #118 shipped a runtime dependency (guest_interp.c) that the
# Makefile target picked up but the inline CI recipe silently omitted, failing
# hosted link gates when draft suppression was in place. Each mapping below is
# asserted source-list-equal so a new runtime dependency cannot be added to one
# surface and forgotten in the other.
_CI_BUILD_TO_MAKE_TARGET = {
    "vfpu_interp_selftest": "vfpu-interp-selftest",
    "fp_convert_selftest": "fp-convert-selftest",
    "vfpu_tables_selftest": "vfpu-tables-selftest",
    "watchpoints_file_selftest": "watchpoints-file-selftest",
}


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestGateStubLink(unittest.TestCase):
    def test_same_headless_runtime_link_inputs_include_stale_detector(self) -> None:
        assert CC is not None
        with tempfile.TemporaryDirectory(prefix="gate_stub_link_") as td:
            work = Path(td)
            generated = work / "gen.c"
            chunk = work / "gen_0.c"
            driver = work / ("driver.exe" if os.name == "nt" else "driver")
            generated.write_text(
                '#include "recomp.h"\n'
                "void sr_register_all(void) {}\n",
                encoding="ascii",
            )
            chunk.write_text(
                '#include "recomp.h"\n'
                "void sr_register_chunk_0(void) {}\n",
                encoding="ascii",
            )
            # driver.c takes its fallback entry from the generic title configuration;
            # the headless link therefore needs the configuration TU and a generated
            # generic (no-title) artifact for it to compile against.
            config_dir = work / "title-config"
            config_dir.mkdir(parents=True, exist_ok=True)
            config_header = config_dir / "sr_title_config.h"
            gen_res = subprocess.run(
                [sys.executable, str(ROOT / "tools" / "title_runtime_config.py"), "--output", str(config_header)],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertEqual(gen_res.returncode, 0, gen_res.stdout + gen_res.stderr)
            self.assertTrue(config_header.exists())
            self.assertIn("0 binding(s)", gen_res.stdout)

            command = [
                CC,
                "-O0",
                "-w",
                "-fno-var-tracking",
                "-D_CRT_SECURE_NO_WARNINGS",
                "-DSR_INSTRUCTION_TRACE",
                "-DSR_GATE_BUILD",
                "-I",
                str(RT),
                "-I",
                str(ROOT / "src" / "core"),
                "-I",
                str(config_dir),
                "-o",
                str(driver),
                str(generated),
                str(chunk),
                str(RT / "recomp.c"),
                # recomp.c's sr_lookup()/dispatch() consult the exec-span
                # registry and interpreter floor; this gate links the real
                # runtime, so the real guest_interp implementation belongs in
                # the link surface exactly as production requires.
                str(RT / "guest_interp.c"),
                # guest_interp.c calls the COP0/exception helpers; they link
                # from the real TU, not from gate_stub.c dead symbols.
                str(RT / "cpu_lle.c"),
                str(RT / "domain_mode.c"),
                # Both dispatch tiers call the TD-27 stale-block query; the
                # real detector links here exactly as production requires.
                # Gate off it is one cached branch, so the headless gate
                # stays on the AOT path with zero guest-memory touches.
                str(RT / "stale_code.c"),
                str(RT / "vfpu_tables.c"),
                str(RT / "driver.c"),
                str(ROOT / "src" / "core" / "nk_shared_helpers.c"),
                str(RT / "ace.c"),
                # driver.c reports how the run ended to the real run recorder.
                str(RT / "videorec.c"),
                str(RT / "title_config.c"),
                str(GATE_STUB),
                "-lm",
            ]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_headless_driver_link_fails_without_title_config(self) -> None:
        """Negative control: omitting title_config.c must fail at link time with an undefined reference."""
        assert CC is not None
        with tempfile.TemporaryDirectory(prefix="gate_stub_link_neg_") as td:
            work = Path(td)
            generated = work / "gen.c"
            chunk = work / "gen_0.c"
            driver = work / ("driver.exe" if os.name == "nt" else "driver")
            generated.write_text(
                '#include "recomp.h"\n'
                "void sr_register_all(void) {}\n",
                encoding="ascii",
            )
            chunk.write_text(
                '#include "recomp.h"\n'
                "void sr_register_chunk_0(void) {}\n",
                encoding="ascii",
            )
            config_dir = work / "title-config"
            config_dir.mkdir(parents=True, exist_ok=True)
            config_header = config_dir / "sr_title_config.h"
            subprocess.run(
                [sys.executable, str(ROOT / "tools" / "title_runtime_config.py"), "--output", str(config_header)],
                cwd=ROOT,
                check=True,
            )
            # Omit title_config.c
            command = [
                CC,
                "-O0",
                "-w",
                "-fno-var-tracking",
                "-D_CRT_SECURE_NO_WARNINGS",
                "-DSR_INSTRUCTION_TRACE",
                "-DSR_GATE_BUILD",
                "-I",
                str(RT),
                "-I",
                str(ROOT / "src" / "core"),
                "-I",
                str(config_dir),
                "-o",
                str(driver),
                str(generated),
                str(chunk),
                str(RT / "recomp.c"),
                str(RT / "guest_interp.c"),
                str(RT / "cpu_lle.c"),
                str(RT / "domain_mode.c"),
                str(RT / "stale_code.c"),
                str(RT / "vfpu_tables.c"),
                str(RT / "driver.c"),
                str(ROOT / "src" / "core" / "nk_shared_helpers.c"),
                str(RT / "ace.c"),
                str(GATE_STUB),
                "-lm",
            ]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, "link should have failed without title_config.c")
            self.assertIn("sr_title_config_fallback_entry", result.stderr + result.stdout)

    def test_headless_driver_link_fails_without_config_include(self) -> None:
        """Negative control: omitting config include dir must fail to compile title_config.c."""
        assert CC is not None
        with tempfile.TemporaryDirectory(prefix="gate_stub_link_neg_inc_") as td:
            work = Path(td)
            generated = work / "gen.c"
            chunk = work / "gen_0.c"
            driver = work / ("driver.exe" if os.name == "nt" else "driver")
            generated.write_text(
                '#include "recomp.h"\n'
                "void sr_register_all(void) {}\n",
                encoding="ascii",
            )
            chunk.write_text(
                '#include "recomp.h"\n'
                "void sr_register_chunk_0(void) {}\n",
                encoding="ascii",
            )
            # Omit -I config_dir
            command = [
                CC,
                "-O0",
                "-w",
                "-fno-var-tracking",
                "-D_CRT_SECURE_NO_WARNINGS",
                "-DSR_INSTRUCTION_TRACE",
                "-DSR_GATE_BUILD",
                "-I",
                str(RT),
                "-I",
                str(ROOT / "src" / "core"),
                "-o",
                str(driver),
                str(generated),
                str(chunk),
                str(RT / "recomp.c"),
                str(RT / "guest_interp.c"),
                str(RT / "cpu_lle.c"),
                str(RT / "domain_mode.c"),
                str(RT / "stale_code.c"),
                str(RT / "vfpu_tables.c"),
                str(RT / "driver.c"),
                str(ROOT / "src" / "core" / "nk_shared_helpers.c"),
                str(RT / "ace.c"),
                str(RT / "title_config.c"),
                str(GATE_STUB),
                "-lm",
            ]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, "compilation should have failed without sr_title_config.h")
            self.assertIn("sr_title_config.h", result.stderr + result.stdout)


def _logical_lines(text: str) -> list[str]:
    """Join backslash-newline continuations so one command is one line."""
    return text.replace("\\\n", " ").splitlines()


def _ci_inline_sources(build_name: str) -> set[str]:
    """Source paths in the hosted-CI inline build of build/<build_name>."""
    joined = _logical_lines(CI_WORKFLOW.read_text(encoding="utf-8"))
    # "-o build/<name>" must be its own argument, not a prefix of a longer
    # name (e.g. vfpu_interp_selftest vs vfpu_interp_selftest_san).
    output_arg = re.compile(rf"-o build/{re.escape(build_name)}(?:\s|$)")
    sources: set[str] = set()
    for line in joined:
        stripped = line.strip()
        if not stripped.startswith(("gcc ", "g++ ", "cc ", "clang ")):
            continue
        if output_arg.search(stripped):
            sources |= set(_SRC_TOKEN.findall(stripped))
    return sources


def _make_target_sources(target: str) -> set[str]:
    """Literal source paths in the canonical Makefile target's recipe."""
    lines = read_makefile(ROOT).splitlines()
    start = None
    for i, line in enumerate(lines):
        if re.match(rf"^{re.escape(target)}:(?:\s|$)", line):
            start = i + 1
            break
    assert start is not None, f"Makefile target not found: {target}"
    recipe: list[str] = []
    for line in lines[start:]:
        if not line.startswith("\t"):
            break
        recipe.append(line[1:])
    joined = "\n".join(recipe).replace("\\\n", " ")
    return set(_SRC_TOKEN.findall(joined))


class TestInlineCiSelftestLinkSync(unittest.TestCase):
    """The hosted-CI inline selftest builds must match their Makefile targets.

    These pairs duplicate the same link contract in two places. When they drift,
    the Makefile target keeps passing locally while the hosted substantive gate
    fails (or vice versa), which is exactly how PR #118 hid a missing
    guest_interp.c behind the earlier draft-suppressed jobs.
    """

    def test_inline_ci_selftest_source_lists_match_makefile_targets(self) -> None:
        for build_name, target in sorted(_CI_BUILD_TO_MAKE_TARGET.items()):
            # Some selftests ship both a plain and a sanitizer CI build, some
            # only one; require that every variant which exists matches the
            # canonical target, and that at least one variant exists.
            variants = [
                name
                for name in (build_name, f"{build_name}_san")
                if _ci_inline_sources(name)
            ]
            with self.subTest(make_target=target):
                self.assertTrue(
                    variants,
                    f"No inline CI gcc/g++ build found for build/{build_name}(_san)",
                )
            for name in variants:
                with self.subTest(ci_build=name, make_target=target):
                    self.assertEqual(_ci_inline_sources(name), _make_target_sources(target))



# A program that links the run recorder (it rides with the presenter, SDL3VK_SRCS) but no
# scheduler: it registers the clock it was given (or none) and asks to record.
_RECORDER_CLOCK_HARNESS = """\
#include "videorec.h"
#include <stdio.h>
#include <string.h>

static int paced_clock(void) { return 0; }
static int deterministic_clock(void) { return 1; }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (strcmp(argv[1], "paced") == 0) sr_videorec_set_clock(paced_clock);
    else if (strcmp(argv[1], "deterministic") == 0) sr_videorec_set_clock(deterministic_clock);
    else if (strcmp(argv[1], "none") != 0) return 2;
    int active = sr_videorec_active();
    printf("active=%d\\n", active);
    fflush(stdout);
    if (active) (void)sr_videorec_finish_status("the clock-seam harness ended the run", 0);
    return 0;
}
"""


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestRecorderClockSeam(unittest.TestCase):
    """The recorder records only on the clock the runtime's main() hands it (#2080).

    videorec.c links with the presenter, so programs that run no scheduler (the GPU
    selftests, ge-replay) link it too. With no clock set, a requested recording is
    refused for that reason; with one set, the recorder follows it. This builds the
    real videorec.c, not its standalone-selftest variant.
    """

    _tmp: tempfile.TemporaryDirectory[str]
    work: Path
    exe: Path
    build_error: str

    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory(prefix="videorec_clock_seam_")
        cls.work = Path(cls._tmp.name)
        src = cls.work / "harness.c"
        src.write_text(_RECORDER_CLOCK_HARNESS, encoding="ascii")
        cls.exe = cls.work / ("harness.exe" if os.name == "nt" else "harness")
        assert CC is not None
        built = subprocess.run(
            [CC, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", f"-I{RT}",
             str(src), str(RT / "videorec.c"), "-o", str(cls.exe)],
            capture_output=True, text=True,
        )
        cls.build_error = "" if built.returncode == 0 else built.stdout + built.stderr

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def _run(self, mode: str) -> tuple[subprocess.CompletedProcess[str], dict[str, object]]:
        self.assertEqual(self.build_error, "", self.build_error)
        out = self.work / mode
        env = dict(os.environ, SR_VIDEOREC_DIR=str(out), SR_VIDEOREC_CODEC="raw")
        # The clock comes from the registered function, never from this key.
        env["SR_DETERMINISTIC_CLOCK"] = "1"
        result = subprocess.run([str(self.exe), mode], env=env, capture_output=True, text=True,
                                timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        record = json.loads((out / "record.json").read_text(encoding="utf-8"))
        return result, record

    def _reason(self, record: dict[str, object]) -> str:
        reason = record["reason"]
        if not isinstance(reason, str):
            self.fail(f"record.json's reason is not a string: {record!r}")
        return reason

    def test_no_clock_refuses_because_no_scheduler_runs(self) -> None:
        result, record = self._run("none")
        self.assertIn("active=0", result.stdout)
        self.assertEqual(record["status"], "REFUSED")
        self.assertIn("runs no scheduler", self._reason(record))
        self.assertIn("VIDEOREC: not recording: this program runs no scheduler", result.stderr)

    def test_a_paced_clock_refuses_for_the_clock(self) -> None:
        result, record = self._run("paced")
        self.assertIn("active=0", result.stdout)
        self.assertEqual(record["status"], "REFUSED")
        self.assertIn("SR_DETERMINISTIC_CLOCK=1", self._reason(record))
        self.assertNotIn("runs no scheduler", self._reason(record))

    def test_a_deterministic_clock_starts_the_recording(self) -> None:
        result, record = self._run("deterministic")
        self.assertIn("active=1", result.stdout)
        self.assertNotEqual(record["status"], "REFUSED", record)


if __name__ == "__main__":
    unittest.main()
