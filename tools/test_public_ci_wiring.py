# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression checks for public/clean-checkout CI wiring."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "fixtures" / "showcase"))

import showcase as showcase_cli  # noqa: E402


class PublicCiWiringTests(unittest.TestCase):
    def test_showcase_guest_build_uses_pspdev_compiler(self) -> None:
        demo = {"id": "synthetic-test", "folder": "ge_scene", "target": "fixture_app"}
        with tempfile.TemporaryDirectory(prefix="showcase_cc_") as tmp:
            with mock.patch.object(showcase_cli, "_host_path", side_effect=lambda path: str(path)), \
                    mock.patch.object(showcase_cli.subprocess, "run",
                                      return_value=showcase_cli.subprocess.CompletedProcess([], 1)) as run:
                with self.assertRaises(showcase_cli.ShowcaseError):
                    showcase_cli._build_prx(demo, Path(tmp))
            shell_command = run.call_args.args[0][-1]
            self.assertIn("CC=psp-gcc", shell_command)

    def test_linux_showcase_ci_uses_pinned_runtime_dependencies(self) -> None:
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        lock = json.loads((ROOT / "assets" / "upstream" / "pspdev.lock.json").read_text())
        evidence = json.loads((ROOT / "assets" / "upstream" / "pspdev.evidence.json").read_text())
        self.assertIn("Build pinned SDL3 for headless Linux runtime", ci)
        self.assertIn("d9d5536704d585616d4db3c8ba3c4ff6fc2757e1", ci)
        self.assertIn("libvulkan-dev", ci)
        self.assertIn("SDL_UNIX_CONSOLE_BUILD=ON", ci)
        self.assertIn("pspdev.lock.json", ci)
        self.assertIn("pspdev.evidence.json", ci)
        self.assertIn(lock["distribution"]["archive_sha256"], evidence["archive"]["github_asset_digest"])
        self.assertIn("read -r PSPDEV_URL PSPDEV_SHA256", ci)
        self.assertIn("sha256sum --check --strict", ci)
        self.assertIn("make CC=gcc showcase-linux", ci)
        self.assertIn("NATIVE_RESULT: ${{ needs.native_tools.result }}", ci)
        docs = (ROOT / "docs" / "CI.md").read_text(encoding="utf-8")
        self.assertIn("showcase-linux", docs)

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
