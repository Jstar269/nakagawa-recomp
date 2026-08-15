#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
MODULE_PATH = TOOLS / "hst_doctor.py"
sys.path.insert(0, str(TOOLS))

import hst_doctor  # noqa: E402
import hst_doctor_checks  # noqa: E402
import hst_doctor_core  # noqa: E402
import shader_embed  # noqa: E402
from hst_test_fixtures import write_elf, write_iso, write_psp_header  # noqa: E402


class ElfValidationTests(unittest.TestCase):
    def test_valid_mips_elf(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "module.elf"
            write_elf(path, load_segments=2)
            metadata, error = hst_doctor._parse_elf(path)
            self.assertIsNone(error)
            assert metadata is not None
            self.assertEqual(metadata["load_segments"], 2)
            self.assertEqual(metadata["machine"], 8)

    def test_rejects_non_mips_elf(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "module.elf"
            write_elf(path, machine=3)
            metadata, error = hst_doctor._parse_elf(path)
            self.assertIsNone(metadata)
            self.assertIn("not MIPS", error or "")

    def test_rejects_encrypted_module(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "module.prx"
            path.write_bytes(b"~SCE" + b"\0" * 128)
            metadata, error = hst_doctor._parse_elf(path)
            self.assertIsNone(metadata)
            self.assertIn("ELF", error or "")


class PrivateInputTests(unittest.TestCase):
    def make_valid_inputs(self, root: Path) -> None:
        write_elf(root / "place_game_here" / "EBOOT.elf")
        write_psp_header(root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "SYSDIR" / "EBOOT.BIN")
        decrypted = root / "place_game_here" / "EXTRACTED" / "decrypted"
        for name in ("libfont.prx", "scePsmf_library.prx", "scePsmfP_library.prx"):
            write_elf(decrypted / name)
        write_iso(root / "place_game_here" / "ISO" / "hst.iso")
        data_root = root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "USRDIR" / "xbdata_extracted"
        (data_root / "archive.xb.d" / "data").mkdir(parents=True)
        (data_root / "archive.xb.d" / "data" / "sample.bin.txt").write_text("synthetic", encoding="utf-8")

    def test_valid_input_layout_has_no_failures(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_valid_inputs(root)
            report = hst_doctor.Report(root, "inputs")
            hst_doctor.check_private_inputs(report, need_iso=True, need_assets=True)
            failures = [result for result in report.results if result.status == "FAIL"]
            self.assertEqual(failures, [])

    def test_multiple_isos_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_valid_inputs(root)
            write_iso(root / "place_game_here" / "ISO" / "second.iso")
            report = hst_doctor.Report(root, "inputs")
            hst_doctor.check_private_inputs(report, need_iso=True, need_assets=False)
            matches = [result for result in report.results if result.code == "INPUT_ISO"]
            self.assertEqual(matches[-1].status, "FAIL")
            self.assertIn("Multiple", matches[-1].summary)

    def test_empty_asset_tree_is_not_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_valid_inputs(root)
            data_root = root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "USRDIR" / "xbdata_extracted"
            for child in sorted(data_root.rglob("*"), reverse=True):
                if child.is_file():
                    child.unlink()
                else:
                    child.rmdir()
            report = hst_doctor.Report(root, "inputs")
            hst_doctor.check_private_inputs(report, need_iso=False, need_assets=True)
            asset = [result for result in report.results if result.code == "INPUT_XB_DATA"][-1]
            self.assertEqual(asset.status, "FAIL")


class RepositoryContractTests(unittest.TestCase):
    def make_docs(self, root: Path, *, package_license: str = "GPL-3.0-or-later") -> None:
        (root / "docs").mkdir(parents=True)
        (root / "interface").mkdir()
        (root / "assets").mkdir()
        (root / "LICENSE").write_text("GNU GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n", encoding="utf-8")
        notice = """GPL-3.0-or-later
This repository does not grant rights to the game.
The project ships no decryption keys of any kind.
This project is independent and is not endorsed.
Users must supply their own legally obtained inputs.
This remains subject to legal review.
"""
        (root / "NOTICE.md").write_text(notice, encoding="utf-8")
        (root / "README.md").write_text("GPL-3.0-or-later\n", encoding="utf-8")
        (root / "CONTRIBUTING.md").write_text("GPL-3.0-or-later\n", encoding="utf-8")
        (root / "SECURITY.md").write_text("security\n", encoding="utf-8")
        (root / "CODE_OF_CONDUCT.md").write_text("conduct\n", encoding="utf-8")
        (root / "docs" / "PUBLICATION_READINESS.md").write_text("publication\n", encoding="utf-8")
        (root / "interface" / "package.json").write_text(json.dumps({"license": package_license}), encoding="utf-8")
        (root / "assets" / "release_manifest.json").write_text(
            json.dumps({"license": "GPL-3.0-or-later"}), encoding="utf-8"
        )

    def test_license_metadata_mismatch_is_a_warning(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_docs(root, package_license="GPL-2.0-or-later")
            report = hst_doctor.Report(root, "repo")
            hst_doctor.check_repository_contract(report)
            warnings = [result for result in report.results if result.code == "LICENSE_METADATA" and result.status == "WARN"]
            self.assertEqual(len(warnings), 1)
            self.assertIn("package.json", warnings[0].path or "")

    def test_consistent_contract_has_no_failures(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_docs(root)
            report = hst_doctor.Report(root, "repo")
            hst_doctor.check_repository_contract(report)
            failures = [result for result in report.results if result.status == "FAIL"]
            self.assertEqual(failures, [])

    def test_live_repository_notice_passes_disclaimer_checks(self) -> None:
        report = hst_doctor.Report(ROOT, "repo")
        hst_doctor.check_repository_contract(report)
        failures = [result for result in report.results if result.code.startswith("NOTICE_") and result.status == "FAIL"]
        self.assertEqual(failures, [], f"Live NOTICE.md failed disclaimer checks: {failures}")


class DatarootAndSaveRootCheckTests(unittest.TestCase):
    def test_sr_dataroot_relative_path_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            report = hst_doctor.Report(root, "inputs")
            with mock.patch.dict(os.environ, {"SR_DATAROOT": "relative/path/to/assets"}):
                hst_doctor.check_private_inputs(report, need_iso=False, need_assets=True)
            failures = [result for result in report.results if result.code == "INPUT_SR_DATAROOT" and result.status == "FAIL"]
            self.assertTrue(failures)
            self.assertIn("not an absolute path", failures[0].summary)

    def test_sr_dataroot_nonexistent_directory_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            missing = root / "does_not_exist"
            report = hst_doctor.Report(root, "inputs")
            with mock.patch.dict(os.environ, {"SR_DATAROOT": str(missing.resolve())}):
                hst_doctor.check_private_inputs(report, need_iso=False, need_assets=True)
            failures = [result for result in report.results if result.code == "INPUT_SR_DATAROOT" and result.status == "FAIL"]
            self.assertTrue(failures)
            self.assertIn("not found", failures[0].summary)

    def test_sr_dataroot_empty_directory_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            empty_dir = root / "empty_assets"
            empty_dir.mkdir()
            report = hst_doctor.Report(root, "inputs")
            with mock.patch.dict(os.environ, {"SR_DATAROOT": str(empty_dir.resolve())}):
                hst_doctor.check_private_inputs(report, need_iso=False, need_assets=True)
            failures = [result for result in report.results if result.code == "INPUT_SR_DATAROOT" and result.status == "FAIL"]
            self.assertTrue(failures)
            self.assertIn("empty", failures[0].summary)

    def test_sr_dataroot_populated_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            assets_dir = root / "populated_assets"
            assets_dir.mkdir()
            (assets_dir / "file1.bin").write_bytes(b"data")
            report = hst_doctor.Report(root, "inputs")
            with mock.patch.dict(os.environ, {"SR_DATAROOT": str(assets_dir.resolve())}):
                hst_doctor.check_private_inputs(report, need_iso=False, need_assets=True)
            passes = [result for result in report.results if result.code == "INPUT_SR_DATAROOT" and result.status == "PASS"]
            self.assertTrue(passes)
            self.assertEqual(passes[0].metadata.get("files_scanned"), 1)

    def test_save_root_validation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            # Default memstick (created on demand or existing and writable)
            report = hst_doctor.Report(root, "inputs")
            hst_doctor_checks.check_save_root(report, root)
            res = next(r for r in report.results if r.code == "SAVE_ROOT")
            self.assertEqual(res.status, "PASS")

            # SR_MEMSTICK pointing to a regular file fails
            bad_file = root / "save_file.bin"
            bad_file.write_bytes(b"not a dir")
            report_bad = hst_doctor.Report(root, "inputs")
            with mock.patch.dict(os.environ, {"SR_MEMSTICK": str(bad_file.resolve())}):
                hst_doctor_checks.check_save_root(report_bad, root)
            res_bad = next(r for r in report_bad.results if r.code == "SAVE_ROOT")
            self.assertEqual(res_bad.status, "FAIL")

    def test_build_profile_diagnostics(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build_dir = root / "build" / "hst"
            build_dir.mkdir(parents=True)

            # Public safe profile
            (build_dir / "runtime_profile.json").write_text(
                json.dumps({"entries": {"CFLAGS": "-O0 -DSR_PUBLIC_SAFE"}}), encoding="utf-8"
            )
            report = hst_doctor.Report(root, "products")
            hst_doctor_checks.check_build_profile(report, root)
            res = next(r for r in report.results if r.code == "BUILD_PROFILE")
            self.assertEqual(res.status, "INFO")
            self.assertEqual(res.metadata.get("public_safe"), 1)


class CliTests(unittest.TestCase):
    def test_json_output_is_machine_readable(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            RepositoryContractTests().make_docs(root)
            proc = subprocess.run(
                [sys.executable, str(MODULE_PATH), "--root", str(root), "--scope", "repo", "--json"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            payload = json.loads(proc.stdout)
            self.assertEqual(payload["schema_version"], 1)
            self.assertEqual(payload["scope"], "repo")
            self.assertEqual(payload["counts"]["FAIL"], 0)


class EnvironmentContractTests(unittest.TestCase):
    def test_powerShell_accepts_current_core_line(self) -> None:
        with mock.patch.object(
            hst_doctor_checks,
            "_probe_powershell",
            return_value=(Path("pwsh"), "Core", "7.6.4", None),
        ):
            report = hst_doctor.Report(Path.cwd(), "build")
            hst_doctor_checks.check_powershell(report)
        result = next(item for item in report.results if item.code == "POWERSHELL_VERSION")
        self.assertEqual(result.status, "PASS")

    def test_powerShell_rejects_windows_powerShell_or_old_core(self) -> None:
        for edition, version in (("Desktop", "5.1.22621"), ("Core", "7.5.3")):
            with self.subTest(edition=edition, version=version), mock.patch.object(
                hst_doctor_checks,
                "_probe_powershell",
                return_value=(Path("pwsh"), edition, version, None),
            ):
                report = hst_doctor.Report(Path.cwd(), "build")
                hst_doctor_checks.check_powershell(report)
                result = next(item for item in report.results if item.code == "POWERSHELL_VERSION")
                self.assertEqual(result.status, "FAIL")

    def test_windows_11_requires_workstation_product_type_and_build_floor(self) -> None:
        cases = (
            (22000, 1, "PASS"),
            (19045, 1, "FAIL"),
            (26100, 3, "FAIL"),
        )
        report_root = Path.cwd()
        powershell_path = Path("pwsh")
        for build, product_type, expected in cases:
            with self.subTest(build=build, product_type=product_type), mock.patch.object(
                hst_doctor_checks.os, "name", "nt"
            ), mock.patch.object(
                hst_doctor_checks.sys,
                "getwindowsversion",
                return_value=type(
                    "WindowsVersion", (), {"build": build, "product_type": product_type}
                )(),
                create=True,
            ), mock.patch.object(
                hst_doctor_checks,
                "_probe_powershell",
                return_value=(powershell_path, "Core", "7.6.4", None),
            ):
                report = hst_doctor.Report(report_root, "build")
                hst_doctor_checks.check_platform(report)
                result = next(item for item in report.results if item.code == "HOST_WINDOWS_11")
                self.assertEqual(result.status, expected)

    def test_shader_mtime_does_not_require_glslc_when_provenance_is_valid(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            gpu_dir = root / "src" / "rt" / "gpu_sdl3vk"
            (gpu_dir / "shaders").mkdir(parents=True)
            shutil.copy2(shader_embed.MANIFEST, gpu_dir / "shader_manifest.json")
            for _, source, embedded in shader_embed.SHADERS:
                shutil.copy2(shader_embed.GPU_DIR / source, gpu_dir / source)
                shutil.copy2(shader_embed.GPU_DIR / embedded, gpu_dir / embedded)

            source = gpu_dir / "shaders" / "psp.vert"
            newer = max(path.stat().st_mtime for path in gpu_dir.rglob("*")) + 3600
            os.utime(source, (newer, newer))

            report = hst_doctor.Report(root, "build")
            with mock.patch.object(hst_doctor_checks, "_find_executable") as find_executable:
                hst_doctor_checks.check_shader_provenance(report, root, None)
            result = next(item for item in report.results if item.code == "GLSLC")
            self.assertEqual(result.status, "INFO")
            self.assertEqual(
                [item for item in report.results if item.status == "FAIL"],
                [],
            )
            find_executable.assert_not_called()

    def test_invalid_shader_provenance_is_attributed_and_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            gpu_dir = root / "src" / "rt" / "gpu_sdl3vk"
            (gpu_dir / "shaders").mkdir(parents=True)
            shutil.copy2(shader_embed.MANIFEST, gpu_dir / "shader_manifest.json")
            for _, source, embedded in shader_embed.SHADERS:
                shutil.copy2(shader_embed.GPU_DIR / source, gpu_dir / source)
                shutil.copy2(shader_embed.GPU_DIR / embedded, gpu_dir / embedded)
            source = gpu_dir / "shaders" / "psp.frag"
            source.write_text(source.read_text(encoding="utf-8") + "\n// stale test\n", encoding="utf-8")

            report = hst_doctor.Report(root, "build")
            with mock.patch.object(hst_doctor_checks, "_find_executable", return_value=None):
                hst_doctor_checks.check_shader_provenance(report, root, None)
            provenance = next(item for item in report.results if item.code == "SHADER_PROVENANCE")
            glslc = next(item for item in report.results if item.code == "GLSLC")
            self.assertEqual(provenance.status, "FAIL")
            self.assertIn("source SHA-256 mismatch", provenance.detail or "")
            self.assertEqual(glslc.status, "FAIL")


class SimpleFrontEndTests(unittest.TestCase):
    def setUp(self) -> None:
        self.frontend = (ROOT / "hst.ps1").read_text(encoding="utf-8-sig")
        self.manager = (ROOT / "hst_manager.ps1").read_text(encoding="utf-8-sig")
        self.makefile = (ROOT / "Makefile").read_text(encoding="utf-8")

    def test_frontend_exposes_small_supported_surface(self) -> None:
        for script in (
            ROOT / "hst.ps1",
            ROOT / "hst_manager.ps1",
            ROOT / "copy_build_assets.ps1",
            ROOT / "tools" / "hst_run_support.ps1",
            ROOT / "tools" / "test_visual_oracle.ps1",
            ROOT / "tools" / "title_manager_plan.ps1",
            ROOT / "tools" / "vulkan_sdk.ps1",
        ):
            self.assertIn("#requires -Version 7.6", script.read_text(encoding="utf-8-sig"), script.name)
        self.assertIn("pwsh -NoProfile", self.makefile)
        self.assertNotIn("powershell -NoProfile", self.makefile)
        for action in ("Doctor", "Build", "Rebuild", "Play", "Verify", "Manager"):
            self.assertIn(f'"{action}"', self.frontend)
        self.assertNotIn('VisualOracle', self.frontend)
        self.assertNotIn('DiffFunc', self.frontend)

    def test_play_validates_before_and_after_build(self) -> None:
        play = self.frontend[self.frontend.index('"Play" {') : self.frontend.index('"Verify" {')]
        self.assertLess(play.index('DoctorScope "inputs"'), play.index('ManagerAction "BuildFast"'))
        self.assertLess(play.index('ManagerAction "BuildFast"'), play.index('DoctorScope "run"'))
        self.assertLess(play.index('DoctorScope "run"'), play.index('ManagerAction "Run"'))

    def test_frontend_restores_caller_location(self) -> None:
        self.assertIn('Set-Location -LiteralPath $RepoRoot', self.frontend)
        self.assertIn('Set-Location -LiteralPath $OriginalLocation', self.frontend)

    def test_frontend_consumes_child_output_before_returning_status(self) -> None:
        self.assertIn('& python @arguments | Out-Host', self.frontend)
        self.assertIn('$arguments = @("-Action", $ManagerAction, "-MsysPath", $MsysPath)', self.frontend)
        self.assertIn('& $Manager @arguments | Out-Host', self.frontend)
        self.assertIn('$exitCode = [int]$LASTEXITCODE', self.frontend)
        self.assertIn('return ($exitCode -eq 0)', self.frontend)
        self.assertNotIn('return $?', self.frontend)

    def test_build_checks_products_after_manager_exit_status(self) -> None:
        for action in ('"Build" {', '"Rebuild" {'):
            block = self.frontend[self.frontend.index(action) : self.frontend.index('\n        }', self.frontend.index(action))]
            self.assertLess(block.index('ManagerAction -ManagerAction'), block.index('DoctorScope "products"'))
        self.assertNotIn('Invoke-ManagerBuild', self.frontend)
        self.assertNotIn('.pre-hst-launcher', self.frontend)



if __name__ == "__main__":
    unittest.main()
