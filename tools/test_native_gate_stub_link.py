# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Keep the headless codegen-gate link surface synchronized with the runtime."""

from __future__ import annotations

import os
from pathlib import Path
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
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


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


if __name__ == "__main__":
    unittest.main()
