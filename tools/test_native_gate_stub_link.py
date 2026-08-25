# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Keep the headless codegen-gate link surface synchronized with the runtime."""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import title_runtime_config  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / "src" / "rt"
GATE_STUB = ROOT / "tools" / "gate_stub.c"
CI_WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"
MAKEFILE = ROOT / "Makefile"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")

_SRC_TOKEN = re.compile(r"\bsrc/[\w./-]+\.(?:c|cpp)\b")

# Hosted-CI inline builds whose source lists duplicate a canonical Makefile
# target recipe. PR #118 shipped a runtime dependency (guest_interp.c) that the
# Makefile target picked up but the inline CI recipe silently omitted, failing
# hosted link gates only after Draft suppression lifted. Each mapping below is
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
    def test_same_headless_runtime_link_inputs_have_no_stale_symbols(self) -> None:
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
                str(RT / "vfpu_tables.c"),
                str(RT / "driver.c"),
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
                str(config_dir),
                "-o",
                str(driver),
                str(generated),
                str(chunk),
                str(RT / "recomp.c"),
                str(RT / "vfpu_tables.c"),
                str(RT / "driver.c"),
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
                "-o",
                str(driver),
                str(generated),
                str(chunk),
                str(RT / "recomp.c"),
                str(RT / "vfpu_tables.c"),
                str(RT / "driver.c"),
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
    lines = MAKEFILE.read_text(encoding="utf-8").splitlines()
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
    guest_interp.c behind Draft-skipped jobs.
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


if __name__ == "__main__":
    unittest.main()
