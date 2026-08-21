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
            title_runtime_config.write_if_changed(
                work / "sr_title_config.h",
                title_runtime_config.render_header(
                    title_runtime_config.bindings_from_manifest(None)
                ),
            )
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
                str(work),
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


if __name__ == "__main__":
    unittest.main()
