# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression checks for public/clean-checkout CI wiring."""

from __future__ import annotations

from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))


class PublicCiWiringTests(unittest.TestCase):
    def test_windows_vfpu_ci_uses_pregenerated_public_mode(self) -> None:
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        self.assertIn("VFPU_FUZZ_PREGENERATED=1", ci)
        self.assertNotIn("GAME_ELF=tools/vfpu_words.txt", ci)

    def test_makefile_pregenerated_mode_does_not_require_game_elf(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("VFPU_FUZZ_PREGENERATED ?= 0", makefile)
        self.assertIn("ifeq ($(VFPU_FUZZ_PREGENERATED),1)", makefile)
        self.assertIn("VFPU_FUZZ_TITLE_OBJ :=", makefile)
        self.assertIn("VFPU_FUZZ_CHUNK_OBJS :=", makefile)
        self.assertIn("$(MAKE) VFPU_FUZZ_PREGENERATED=1 vfpu_fuzz_build", makefile)
        self.assertIn("--require-synthetic", makefile)
    def test_synthetic_marker_binds_exact_header_bytes(self) -> None:
        from vfpu_fuzz_gen import require_synthetic_cases, write_synthetic_marker

        with tempfile.TemporaryDirectory(prefix="vfpu_fuzz_marker_") as tmp:
            header = Path(tmp) / "cases.h"
            header.write_bytes(b"source-owned synthetic cases\n")
            write_synthetic_marker(header)
            self.assertTrue(require_synthetic_cases(header))
            header.write_bytes(b"different cases\n")
            self.assertFalse(require_synthetic_cases(header))

    def test_windows_build_job_forwards_the_source_commit(self) -> None:
        """Every Windows build job must state the revision its binaries record.

        The Windows runtime compile gate runs its builds in an MSYS2 shell with
        no git on PATH, so the Makefile cannot resolve a revision there. It used
        to log `process_begin: CreateProcess(NULL, git rev-parse HEAD, ...) failed`
        and build every binary of the job with an EMPTY identity
        (flight recorder ``build.build_id``, issue #532). The job now forwards
        the revision the checkout contains, which actions/checkout resolves to
        ``github.sha``, so a Windows-built binary is identified without needing
        git at all.
        """
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        lines = ci.splitlines()
        job_starts = [
            index
            for index, line in enumerate(lines)
            if re.match(r"^ {2}[A-Za-z0-9_]+:\s*$", line)
        ]
        windows_jobs = []
        for position, start in enumerate(job_starts):
            end = job_starts[position + 1] if position + 1 < len(job_starts) else len(lines)
            block = lines[start:end]
            if any("runs-on: windows" in line for line in block):
                windows_jobs.append((lines[start].strip(), block))
        self.assertTrue(windows_jobs, "no Windows job found in ci.yml")
        for name, block in windows_jobs:
            with self.subTest(job=name):
                self.assertTrue(
                    any(line.strip() == "SR_SOURCE_COMMIT: ${{ github.sha }}" for line in block),
                    f"{name} builds on a host with no git on PATH and must forward "
                    "SR_SOURCE_COMMIT explicitly",
                )


if __name__ == "__main__":
    unittest.main()
