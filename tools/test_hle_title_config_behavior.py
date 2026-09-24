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
import os
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
        self.assertEqual(config["codegen_profile"], "none")
        header = title_runtime_config.render_header(config).lower()
        self.assertIn("#define sr_title_config_diagnostics_profile 0", header)
        for address in ("002d132c", "00331b80", "00333138", "002bdf38", "000823f0"):
            self.assertNotIn(address, header)

    def test_configured_header_is_typed_and_disjoint(self):
        config = title_runtime_config.bindings_from_manifest(synthetic_manifest())
        self.assertEqual(config["codegen_profile"], "none")
        header = title_runtime_config.render_header(config).lower()
        self.assertIn("#define sr_title_config_diagnostics_profile 0", header)
        for value in SYNTH_DISPLAY.values():
            self.assertIn(f"{value:08x}", header)
        for value in (SYNTH_LIBFONT, SYNTH_FRAME):
            self.assertIn(f"{value:08x}", header)
        for address in (0x00000BCC, 0x0029A8BC, 0x0001DC00, 0x00331B80):
            self.assertNotIn(f"{address:08x}", header)

    def test_hst_profile_is_the_only_profile_that_emits_the_diagnostic_gate(self):
        manifest = synthetic_manifest()
        manifest["codegen_profile"] = "hst"
        config = title_runtime_config.bindings_from_manifest(manifest)
        self.assertEqual(config["codegen_profile"], "hst")
        header = title_runtime_config.render_header(config).lower()
        self.assertIn("#define sr_title_config_diagnostics_profile 1", header)
        self.assertNotEqual(
            title_runtime_config.config_digest(config),
            title_runtime_config.config_digest(
                title_runtime_config.bindings_from_manifest(synthetic_manifest())
            ),
            "the profile bit must participate in the configuration identity",
        )

    def test_configured_bindings_reach_production_hle(self):
        make = shutil.which("mingw32-make")
        if not make:
            raise unittest.SkipTest("mingw32-make is not available")
        with tempfile.TemporaryDirectory(prefix="nakagawa_hle_") as tmp:
            tmp_path = Path(tmp)
            manifest = tmp_path / "synthetic-positive.json"
            positive_manifest = synthetic_manifest()
            positive_manifest["codegen_profile"] = "hst"
            manifest.write_text(json.dumps(positive_manifest), encoding="utf-8")
            header = tmp_path / "sr_title_config.h"
            exe = tmp_path / "hle_title_production_selftest_synthetic-positive.exe"
            command = [
                make,
                "--no-print-directory",
                "hle-title-selftest-one",
                "HLE_TITLE_CONFIG=synthetic-positive",
                f"HLE_TITLE_MANIFEST={manifest.as_posix()}",
                f"BUILD_DIR={tmp_path.as_posix()}",
                f"HLE_TITLE_SELFTEST_DIR={tmp_path.as_posix()}",
                f"HLE_TITLE_SELFTEST_HEADER={header.as_posix()}",
                f"HLE_TITLE_SELFTEST_EXE={exe.as_posix()}",
            ]
            result = subprocess.run(
                command,
                cwd=ROOT,
                capture_output=True,
                text=True,
                env={
                    **os.environ,
                    "SR_HLE_DIAGNOSTICS": "1",
                    "SR_EXPECT_HLE_DIAGNOSTICS": "1",
                },
                timeout=180,
            )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("hle_title_production_selftest:", output)
        self.assertIn("0 failures", output)
        self.assertIn("replaying title display-driver init", output)
        self.assertIn("libfont compat", output)

    def test_manifest_module_load_honours_address_and_refuses_collision(self):
        make = shutil.which("mingw32-make")
        if not make:
            raise unittest.SkipTest("mingw32-make is not available")
        with tempfile.TemporaryDirectory(prefix="nakagawa_hle_module_") as tmp:
            tmp_path = Path(tmp)
            module_root = tmp_path / "modules"
            module_root.mkdir()
            manifest = tmp_path / "synthetic-module.json"
            configured = synthetic_manifest()
            configured["modules"] = [{
                "name": "c285-runtime.prx",
                "load_address": 0x09F00000,
                "required": True,
                "role": "guest-prx",
                "guest_path": "disc0:/PSP_GAME/USRDIR/c285-runtime.prx",
                "load_address_evidence": "provisional",
            }, *configured.get("modules", [])]
            manifest.write_text(json.dumps(configured), encoding="utf-8")
            header = tmp_path / "sr_title_config.h"
            exe = tmp_path / "hle_title_production_selftest_module.exe"
            command = [
                make,
                "--no-print-directory",
                "hle-title-selftest-one",
                "HLE_TITLE_CONFIG=synthetic-module",
                f"HLE_TITLE_MANIFEST={manifest.as_posix()}",
                f"BUILD_DIR={tmp_path.as_posix()}",
                f"HLE_TITLE_SELFTEST_DIR={tmp_path.as_posix()}",
                f"HLE_TITLE_SELFTEST_HEADER={header.as_posix()}",
                f"HLE_TITLE_SELFTEST_EXE={exe.as_posix()}",
            ]
            env = os.environ.copy()
            ucrt_bin = Path("C:/msys64/ucrt64/bin")
            if ucrt_bin.is_dir():
                env["PATH"] = str(ucrt_bin) + os.pathsep + env.get("PATH", "")
            env["SR_MODULE_DIR"] = str(module_root)
            env["SR_TEST_GUEST_MODULE_LOAD"] = "success"
            built = subprocess.run(
                command, cwd=ROOT, capture_output=True, text=True, env=env, timeout=180
            )
            build_output = built.stdout + built.stderr
            self.assertEqual(built.returncode, 0, build_output)
            self.assertIn("prx image: runtime -> [0x09f00000", build_output.lower())
            self.assertIn("0 failures", build_output)

            env["SR_TEST_GUEST_MODULE_LOAD"] = "collision"
            collided = subprocess.run(
                [str(exe), "--title-config"],
                cwd=ROOT, capture_output=True, text=True, env=env, timeout=60,
            )
            collision_output = collided.stdout + collided.stderr
            self.assertEqual(collided.returncode, 0, collision_output)
            self.assertIn("GUEST_MODULE_LOAD_ADDRESS_COLLISION", collision_output)
            self.assertIn("0 failures", collision_output)

            env["SR_TEST_GUEST_MODULE_LOAD"] = "missing"
            missing = subprocess.run(
                [str(exe), "--title-config"],
                cwd=ROOT, capture_output=True, text=True, env=env, timeout=60,
            )
            missing_output = missing.stdout + missing.stderr
            self.assertEqual(missing.returncode, 0, missing_output)
            self.assertIn("GUEST_MODULE_INPUT_MISSING", missing_output)
            self.assertIn("0 failures", missing_output)

    def test_generic_profile_rejects_diagnostics_even_when_requested(self):
        make = shutil.which("mingw32-make")
        if not make:
            raise unittest.SkipTest("mingw32-make is not available")
        with tempfile.TemporaryDirectory(prefix="nakagawa_hle_generic_") as tmp:
            tmp_path = Path(tmp)
            header = tmp_path / "sr_title_config.h"
            exe = tmp_path / "hle_title_production_selftest_generic-negative.exe"
            command = [
                make,
                "--no-print-directory",
                "hle-title-selftest-one",
                "HLE_TITLE_CONFIG=generic-negative",
                "HLE_TITLE_MANIFEST=",
                f"BUILD_DIR={tmp_path.as_posix()}",
                f"HLE_TITLE_SELFTEST_DIR={tmp_path.as_posix()}",
                f"HLE_TITLE_SELFTEST_HEADER={header.as_posix()}",
                f"HLE_TITLE_SELFTEST_EXE={exe.as_posix()}",
            ]
            result = subprocess.run(
                command,
                cwd=ROOT,
                capture_output=True,
                text=True,
                env={
                    **os.environ,
                    "SR_HLE_DIAGNOSTICS": "1",
                    "SR_EXPECT_HLE_DIAGNOSTICS": "0",
                },
                timeout=180,
            )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("hle_title_production_selftest:", output)
        self.assertIn("0 failures", output)

    def test_hst_profile_requires_explicit_diagnostics_opt_in(self):
        make = shutil.which("mingw32-make")
        if not make:
            raise unittest.SkipTest("mingw32-make is not available")
        with tempfile.TemporaryDirectory(prefix="nakagawa_hle_hst_nooptin_") as tmp:
            tmp_path = Path(tmp)
            manifest = tmp_path / "synthetic-hst-nooptin.json"
            no_optin_manifest = synthetic_manifest()
            no_optin_manifest["codegen_profile"] = "hst"
            manifest.write_text(json.dumps(no_optin_manifest), encoding="utf-8")
            header = tmp_path / "sr_title_config.h"
            exe = tmp_path / "hle_title_production_selftest_hst-nooptin.exe"
            command = [
                make,
                "--no-print-directory",
                "hle-title-selftest-one",
                "HLE_TITLE_CONFIG=hst-nooptin",
                f"HLE_TITLE_MANIFEST={manifest.as_posix()}",
                f"BUILD_DIR={tmp_path.as_posix()}",
                f"HLE_TITLE_SELFTEST_DIR={tmp_path.as_posix()}",
                f"HLE_TITLE_SELFTEST_HEADER={header.as_posix()}",
                f"HLE_TITLE_SELFTEST_EXE={exe.as_posix()}",
            ]
            env = {
                key: value
                for key, value in os.environ.items()
                if key not in {"SR_HLE_DIAGNOSTICS", "SR_EXPECT_HLE_DIAGNOSTICS"}
            }
            env["SR_EXPECT_HLE_DIAGNOSTICS"] = "0"
            result = subprocess.run(
                command,
                cwd=ROOT,
                capture_output=True,
                text=True,
                env=env,
                timeout=180,
            )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("hle_title_production_selftest:", output)
        self.assertIn("0 failures", output)


if __name__ == "__main__":
    unittest.main()
