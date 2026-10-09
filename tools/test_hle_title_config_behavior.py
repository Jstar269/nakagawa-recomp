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
import title_manifest

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
SYNTH_FRAME = 0x08905004


def synthetic_manifest() -> dict:
    manifest = json.loads(FIXTURE.read_text(encoding="utf-8"))
    manifest["runtime_bindings"].update(
        {
            "display_bringup": dict(SYNTH_DISPLAY),
            "runtime_sync": copy.deepcopy(SYNTH_RUNTIME_SYNC),
            "frame_ready_latch_addr": SYNTH_FRAME,
            "expected_data_file_count": 12345,
        }
    )
    return manifest


def libfont_selftest_manifest(tmp_path: Path) -> Path:
    """A validated synthetic manifest whose only guest module is a libfont PRX and
    which configures no ready-flag binding, so every host readiness write the
    runtime could make would be visible in the run output."""
    (tmp_path / "modules").mkdir(exist_ok=True)
    manifest = json.loads(FIXTURE.read_text(encoding="utf-8"))
    manifest["modules"].insert(0, {
        "name": "libfont.prx",
        "load_address": 0x09F00000,
        "required": True,
        "role": "guest-prx",
        "guest_path": "disc0:/PSP_GAME/SYSDIR/libfont.prx",
        "load_address_evidence": "provisional",
    })
    assert "libfont_ready_flag_addr" not in manifest["runtime_bindings"]
    path = tmp_path / "synthetic-libfont.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def run_libfont_selftest(make: str, tmp_path: Path, manifest_path: Path,
                         hle_source: str | None = None) -> subprocess.CompletedProcess:
    """Build and run the production HLE selftest against the synthetic libfont
    manifest. ``hle_source`` replaces src/rt/hle.c through a private Makefile, so a
    mutation is executed in a temporary directory rather than described."""
    tag = "mutant" if hle_source is not None else "libfont"
    out_dir = tmp_path / tag
    out_dir.mkdir(exist_ok=True)
    makefile = ROOT / "Makefile"
    if hle_source is None:
        command_makefile = None
        makefile_arg = []
    else:
        command_makefile = tmp_path / f"Makefile.{tag}"
        makefile_text = makefile.read_text(encoding="utf-8")
        assert "src/rt/hle.c" in makefile_text
        mutant_source = tmp_path / f"hle_{tag}.c"
        mutant_source.write_text(hle_source, encoding="utf-8")
        command_makefile.write_text(
            makefile_text.replace("src/rt/hle.c", mutant_source.as_posix()),
            encoding="utf-8",
        )
        makefile_arg = ["-f", command_makefile.as_posix()]
    header = out_dir / "sr_title_config.h"
    exe = out_dir / f"hle_title_production_selftest_{tag}.exe"
    command = [
        make,
        *makefile_arg,
        "--no-print-directory",
        "hle-title-selftest-one",
        f"HLE_TITLE_CONFIG=synthetic-{tag}",
        f"HLE_TITLE_MANIFEST={manifest_path.as_posix()}",
        f"BUILD_DIR={out_dir.as_posix()}",
        f"HLE_TITLE_SELFTEST_DIR={out_dir.as_posix()}",
        f"HLE_TITLE_SELFTEST_HEADER={header.as_posix()}",
        f"HLE_TITLE_SELFTEST_EXE={exe.as_posix()}",
    ]
    env = os.environ.copy()
    ucrt_bin = Path("C:/msys64/ucrt64/bin")
    if ucrt_bin.is_dir():
        env["PATH"] = str(ucrt_bin) + os.pathsep + env.get("PATH", "")
    env["SR_MODULE_DIR"] = str(tmp_path / "modules")
    env["SR_TEST_GUEST_MODULE_LOAD"] = "libfont-startup"
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                          env=env, timeout=300)


class HleTitleConfigBehaviorTests(unittest.TestCase):
    def test_legacy_libfont_ready_binding_has_a_named_migration_error(self):
        manifest = synthetic_manifest()
        manifest["runtime_bindings"]["libfont_ready_flag_addr"] = 0x08905000
        with self.assertRaisesRegex(
            ValueError,
            "LIBFONT_READY_FLAG_RETIRED: host readiness injection is not supported yet",
        ):
            title_manifest.validate_manifest(manifest)

    def test_libfont_registry_import_is_a_named_issue_299_boundary(self):
        compatibility = (ROOT / "docs" / "COMPATIBILITY.md").read_text(encoding="utf-8")
        heading = "### Issue #299: libfont registry import boundary"
        self.assertTrue(heading in compatibility, "missing the #299 registry-boundary section")
        issue_299 = compatibility.split(heading, 1)[1]
        self.assertIn("sceRegOpenRegistry", issue_299)
        self.assertIn("NID `0x92e41280`, library `sceReg`", issue_299)
        self.assertIn("read/write virtual system registry", issue_299)
        self.assertIn("Unknown keys still fail closed", issue_299)
        self.assertIn("measured on a PSP-3000 running 6.6.1", issue_299)
        self.assertIn("not hardware measured", issue_299)

    def test_stopped_module_restart_comment_is_labeled_as_inference(self):
        source = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        restart_comment = "\n".join(
            line for line in source.splitlines()
            if "stopped module" in line.lower() or "Inference, not" in line
        )
        self.assertIn("Inference, not hardware-measured:", restart_comment)
        self.assertIn("stopped module is restartable", restart_comment)

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
        self.assertIn(f"{SYNTH_FRAME:08x}", header)
        self.assertNotIn("libfont_ready_flag_addr", header)
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
                    "SR_REAL_MODULE_START": "0",
                },
                timeout=180,
            )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("hle_title_production_selftest:", output)
        self.assertIn("0 failures", output)
        self.assertIn("replaying title display-driver init", output)
        boundary_lines = [
            line for line in output.splitlines()
            if "LIBFONT_STARTUP_UNAVAILABLE:" in line
        ]
        self.assertTrue(boundary_lines, output)
        self.assertTrue(
            all("guest libfont startup is not supported yet on this route" in line.lower()
                for line in boundary_lines),
            output,
        )
        self.assertTrue(
            any("refusing to synthesize readiness" in line.lower()
                for line in boundary_lines),
            output,
        )
        self.assertTrue(all("#" not in line for line in boundary_lines), output)

    def test_libfont_guest_startup_routes_exports_without_ready_binding(self):
        make = shutil.which("mingw32-make")
        if not make:
            raise unittest.SkipTest("mingw32-make is not available")
        with tempfile.TemporaryDirectory(prefix="nakagawa_libfont_startup_") as tmp:
            tmp_path = Path(tmp)
            manifest_path = libfont_selftest_manifest(tmp_path)
            result = run_libfont_selftest(make, tmp_path, manifest_path)
            source_text = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
            policy = "ModuleStartPolicy policy = module_start_policy();"
            self.assertEqual(source_text.count(policy), 1)
            # Denying every entry is the pre-#652 shape: libfont startup never runs,
            # so only the guest module_start can write the readiness word.
            mutant = run_libfont_selftest(
                make, tmp_path, manifest_path,
                hle_source=source_text.replace(policy, "ModuleStartPolicy policy = MODULE_START_NONE;", 1),
            )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("prx image: libfont -> [0x09f00000", output.lower())
        self.assertIn("PRX link:", output)
        self.assertIn("0 failures", output)
        self.assertIn(
            "translated guest libfont startup is not supported yet on this route; "
            "refusing to synthesize readiness",
            output.lower(),
        )
        mutant_output = mutant.stdout + mutant.stderr
        self.assertNotEqual(mutant.returncode, 0, mutant_output)
        self.assertIn(
            "FAIL: guest module_start writes the readiness word exactly once",
            mutant_output,
        )

    def test_libfont_start_respects_the_real_module_start_gate(self):
        """SR_REAL_MODULE_START is a tri-state gate: "0" is the environmental kill
        switch and has to cover libfont, "1" and the unset default run the translated
        entry. The mutant restores the pre-#652 gate, where "0" did not cover libfont."""
        make = shutil.which("mingw32-make")
        if not make:
            raise unittest.SkipTest("mingw32-make is not available")
        with tempfile.TemporaryDirectory(prefix="nakagawa_libfont_gate_") as tmp:
            tmp_path = Path(tmp)
            manifest_path = libfont_selftest_manifest(tmp_path)
            result = run_libfont_selftest(make, tmp_path, manifest_path)
            source_text = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
            kill_switch = 'if (strcmp(e, "0") == 0) return MODULE_START_NONE;'
            self.assertEqual(source_text.count(kill_switch), 1)
            mutant = run_libfont_selftest(
                make, tmp_path, manifest_path,
                hle_source=source_text.replace(
                    kill_switch,
                    'if (strcmp(e, "0") == 0) return MODULE_START_LIBFONT_ONLY;', 1),
            )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("0 failures", output)
        self.assertIn("SR_REAL_MODULE_START=0 disabled guest startup", output)
        self.assertIn("entry untranslated", output)
        self.assertIn("already started, refusing re-entry", output)
        self.assertNotIn("module absent", output)
        mutant_output = mutant.stdout + mutant.stderr
        self.assertNotEqual(mutant.returncode, 0, mutant_output)
        self.assertIn(
            "FAIL: SR_REAL_MODULE_START=0 does not execute translated module_start",
            mutant_output,
        )

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

    def test_runtime_placed_modules_load_bind_and_unload(self):
        """Guest-placed modules (#704) through the production loader: placement by the
        user-partition allocator, relocation, translation binding, start/stop with the
        module's own $gp, unload and reuse, LoadModuleByID, and every named refusal."""
        make = shutil.which("mingw32-make")
        if not make:
            raise unittest.SkipTest("mingw32-make is not available")
        with tempfile.TemporaryDirectory(prefix="nakagawa_hle_runtime_module_") as tmp:
            tmp_path = Path(tmp)
            module_root = tmp_path / "modules"
            module_root.mkdir()
            manifest = tmp_path / "synthetic-runtime-module.json"
            configured = synthetic_manifest()

            def runtime(name: str, guest_path: str, required: bool = True) -> dict:
                return {"name": name, "required": required, "role": "guest-prx",
                        "placement": "runtime", "guest_path": guest_path}

            configured["modules"] = [
                runtime("rt-alpha.prx", "ms0:/NKRT/RT_ALPHA.PRX"),
                runtime("rt-beta.prx", "disc0:/PSP_GAME/USRDIR/rt-beta.prx", required=False),
                runtime("rt-gamma.prx", "disc0:/PSP_GAME/USRDIR/rt-gamma.prx"),
                runtime("rt-delta.prx", "disc0:/PSP_GAME/USRDIR/rt-delta.prx"),
                runtime("rt-fixed.prx", "disc0:/PSP_GAME/USRDIR/rt-fixed.prx"),
                runtime("rt-nostart.prx", "disc0:/PSP_GAME/USRDIR/rt-nostart.prx"),
                *configured.get("modules", []),
            ]
            manifest.write_text(json.dumps(configured), encoding="utf-8")
            header = tmp_path / "sr_title_config.h"
            exe = tmp_path / "hle_title_production_selftest_runtime_module.exe"
            command = [
                make,
                "--no-print-directory",
                "hle-title-selftest-one",
                "HLE_TITLE_CONFIG=synthetic-runtime-module",
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
            env["SR_TEST_GUEST_MODULE_LOAD"] = "runtime-placement"
            env.pop("SR_REAL_MODULE_START", None)
            built = subprocess.run(
                command, cwd=ROOT, capture_output=True, text=True, env=env, timeout=300
            )
            output = built.stdout + built.stderr
            self.assertEqual(built.returncode, 0, output)
            self.assertIn("0 failures", output)
            self.assertNotIn("FAIL: runtime placement", output)
            for marker in (
                "GUEST_MODULE_PLACED: rt-alpha.prx uid=0x",
                "GUEST_MODULE_UNLOADED: rt-alpha.prx uid=0x",
                "GUEST_MODULE_BOUNDARY: guest-module-second-instance module=rt-alpha.prx",
                "GUEST_MODULE_BOUNDARY: guest-module-untranslated module=rt-gamma.prx",
                "GUEST_MODULE_LOAD_NO_MEMORY: rt-alpha.prx",
            ):
                self.assertIn(marker, output)

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
