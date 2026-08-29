# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Production-path regressions for the migrated title-qualified HLE bindings.

The positive case generates a temporary validated synthetic manifest and runs
the real ``hle.c`` handlers through the existing executable HLE selftest. The
test does not copy handler logic or mock guest memory. Generic/public fixture
profiles are also run through that executable and must keep the migrated HLE
groups absent.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import title_runtime_config

FIXTURE = ROOT / "assets" / "titles" / "synthetic.json"

SYNTH_DISPLAY = {
    "malloc_entry": 0x08901000,
    "vblank_device_init_entry": 0x08901010,
    "render_context_init_entry": 0x08901020,
    "render_context_magic_addr": 0x08902000,
    "render_table_ready_flag_addr": 0x08902004,
    "render_context_word_addr": 0x08902008,
}
SYNTH_RUNTIME_SYNC = {
    "config_base": 0x08903000,
    "sema_name_ptr": 0x08903020,
    "wrappers": [
        {"mode": 0, "enter": 0x08904000, "leave": 0x08904004},
        {"mode": 1, "enter": 0x08904010, "leave": 0x08904014},
        {"mode": 2, "enter": 0x08904020, "leave": 0x08904024},
    ],
}
SYNTH_LIBFONT = 0x08905000
SYNTH_FRAME = 0x08905004


def synthetic_manifest() -> dict:
    manifest = json.loads(FIXTURE.read_text(encoding="utf-8"))
    manifest["runtime_bindings"].update(
        {
            "display_bringup": dict(SYNTH_DISPLAY),
            "runtime_sync": copy.deepcopy(SYNTH_RUNTIME_SYNC),
            "libfont_ready_flag_addr": SYNTH_LIBFONT,
            "frame_ready_latch_addr": SYNTH_FRAME,
            "expected_data_file_count": 12345,
        }
    )
    return manifest


class HleTitleConfigBehaviorTests(unittest.TestCase):
    def test_generic_header_has_no_migrated_bindings(self):
        config = title_runtime_config.bindings_from_manifest(None)
        self.assertEqual(config["source_id"], "none")
        header = title_runtime_config.render_header(config).lower()
        for address in ("002d132c", "00331b80", "00333138", "002bdf38", "000823f0"):
            self.assertNotIn(address, header)

    def test_configured_header_is_typed_and_disjoint(self):
        config = title_runtime_config.bindings_from_manifest(synthetic_manifest())
        header = title_runtime_config.render_header(config).lower()
        for value in SYNTH_DISPLAY.values():
            self.assertIn(f"{value:08x}", header)
        for value in (SYNTH_LIBFONT, SYNTH_FRAME):
            self.assertIn(f"{value:08x}", header)
        for address in (0x00000BCC, 0x0029A8BC, 0x0001DC00, 0x00331B80):
            self.assertNotIn(f"{address:08x}", header)

    def test_configured_bindings_reach_production_hle(self):
        make = shutil.which("mingw32-make")
        if not make:
            raise unittest.SkipTest("mingw32-make is not available")
        with tempfile.TemporaryDirectory(prefix="nakagawa_hle_") as tmp:
            manifest = Path(tmp) / "synthetic-positive.json"
            manifest.write_text(json.dumps(synthetic_manifest()), encoding="utf-8")
            command = [
                make,
                "--no-print-directory",
                "hle-title-selftest-one",
                "HLE_TITLE_CONFIG=synthetic-positive",
                f"HLE_TITLE_MANIFEST={manifest.as_posix()}",
            ]
            result = subprocess.run(
                command,
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=180,
            )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("hle_title_production_selftest:", output)
        self.assertIn("0 failures", output)
        self.assertIn("replaying title display-driver init", output)
        self.assertIn("libfont compat", output)


if __name__ == "__main__":
    unittest.main()
