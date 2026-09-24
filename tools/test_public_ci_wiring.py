# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression checks for public/clean-checkout CI wiring."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


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


if __name__ == "__main__":
    unittest.main()
