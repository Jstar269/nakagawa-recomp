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
MODULE_PATH = TOOLS / "nk_doctor.py"
sys.path.insert(0, str(TOOLS))

import nk_doctor  # noqa: E402
import nk_doctor_checks  # noqa: E402
import nk_doctor_core  # noqa: E402
import shader_embed  # noqa: E402
from hst_test_fixtures import write_elf, write_iso, write_psp_header  # noqa: E402

_CHECKS_MODULE = nk_doctor_checks


class ElfValidationTests(unittest.TestCase):
    def test_valid_mips_elf(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "module.elf"
            write_elf(path, load_segments=2)
            metadata, error = nk_doctor_core._parse_elf(path)
            self.assertIsNone(error)
            assert metadata is not None
            self.assertEqual(metadata["load_segments"], 2)
            self.assertEqual(metadata["machine"], 8)

    def test_rejects_non_mips_elf(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "module.elf"
            write_elf(path, machine=3)
            metadata, error = nk_doctor_core._parse_elf(path)
            self.assertIsNone(metadata)
            self.assertIn("not MIPS", error or "")

    def test_rejects_encrypted_module(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "module.prx"
            path.write_bytes(b"~SCE" + b"\0" * 128)
            metadata, error = nk_doctor_core._parse_elf(path)
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

    def retail_manifest(self, disc_id: str = "UCUS98701") -> dict[str, object]:
        return {
            "schema_version": 1,
            "id": "mygame-v1",
            "display_name": "My Game",
            "kind": "retail",
            "game_name": "mygame",
            "disc": {"id": disc_id, "region": "NA", "revision_policy": "exact-disc-id"},
            "filesystem": {
                "data_root": "place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata_extracted",
                "module_dir": "place_game_here/EXTRACTED/decrypted",
                "psp_header": "place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN",
                "disc_image": "place_game_here/ISO/hst.iso",
            },
            "executable": {
                "path": "place_game_here/EBOOT.elf",
                "bss_metadata_source": "psp-header",
            },
            "modules": [
                {"name": name, "role": "guest-prx", "required": True}
                for name in ("libfont.prx", "scePsmf_library.prx", "scePsmfP_library.prx")
            ],
        }

    def check_inputs(
        self,
        root: Path,
        report: object,
        *,
        need_iso: bool,
        need_assets: bool,
        title_manifest: object | None = None,
        context_manifest: object | None = None,
        expected_disc_id: str | None = None,
    ) -> None:
        context_source = context_manifest
        if context_source is None:
            context_source = title_manifest if title_manifest is not None else self.retail_manifest()
        context = nk_doctor_checks.title_diagnostic_context(root, context_source)
        nk_doctor_checks.check_private_inputs(
            report,
            need_iso=need_iso,
            need_assets=need_assets,
            title_manifest=title_manifest,
            expected_disc_id=expected_disc_id,
            title_context=context,
        )

    def test_valid_input_layout_has_no_failures(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_valid_inputs(root)
            report = nk_doctor.Report(root, "inputs")
            self.check_inputs(root, report, need_iso=True, need_assets=True)
            failures = [result for result in report.results if result.status == "FAIL"]
            self.assertEqual(failures, [])

    def test_manifest_declared_iso_wins_over_unbound_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_valid_inputs(root)
            write_iso(root / "place_game_here" / "ISO" / "second.iso")
            report = nk_doctor.Report(root, "inputs")
            self.check_inputs(root, report, need_iso=True, need_assets=False)
            matches = [result for result in report.results if result.code == "INPUT_ISO"]
            self.assertEqual(matches[-1].status, "PASS")
            self.assertIn("hst.iso", matches[-1].path or "")

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
            report = nk_doctor.Report(root, "inputs")
            self.check_inputs(root, report, need_iso=False, need_assets=True)
            asset = [result for result in report.results if result.code == "INPUT_XB_DATA"][-1]
            self.assertEqual(asset.status, "FAIL")

    def test_expected_disc_id_is_retired_from_doctor_core(self) -> None:
        self.assertFalse(
            hasattr(nk_doctor_core, "EXPECTED_DISC_ID"),
            "EXPECTED_DISC_ID must be retired from nk_doctor_core; disc identity is manifest-owned",
        )

    def test_check_private_inputs_without_manifest_skips_disc_id(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_valid_inputs(root)
            report = nk_doctor.Report(root, "inputs")
            self.check_inputs(
                root,
                report,
                need_iso=True,
                need_assets=False,
                context_manifest=self.retail_manifest(),
            )
            disc_results = [r for r in report.results if r.code == "INPUT_DISC_ID"]
            self.assertEqual(len(disc_results), 1)
            self.assertEqual(disc_results[0].status, "INFO")
            self.assertIn("No title manifest with disc ID supplied", disc_results[0].summary)

    def test_check_private_inputs_with_explicit_disc_id_matching(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_valid_inputs(root)
            report = nk_doctor.Report(root, "inputs")
            self.check_inputs(
                root,
                report,
                need_iso=True,
                need_assets=False,
                expected_disc_id="UCUS98701",
            )
            disc_results = [r for r in report.results if r.code == "INPUT_DISC_ID"]
            self.assertEqual(len(disc_results), 1)
            self.assertEqual(disc_results[0].status, "PASS")
            self.assertIn("UCUS98701", disc_results[0].summary)

    def test_check_private_inputs_with_explicit_disc_id_mismatch_warns(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_valid_inputs(root)
            report = nk_doctor.Report(root, "inputs")
            self.check_inputs(
                root,
                report,
                need_iso=True,
                need_assets=False,
                expected_disc_id="OTHER9999",
            )
            disc_results = [r for r in report.results if r.code == "INPUT_DISC_ID"]
            self.assertEqual(len(disc_results), 1)
            self.assertEqual(disc_results[0].status, "WARN")
            self.assertIn("OTHER9999", disc_results[0].summary)

    def test_check_private_inputs_with_title_manifest_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_valid_inputs(root)
            manifest_data = self.retail_manifest()
            manifest_file = root / "title.json"
            manifest_file.write_text(json.dumps(manifest_data), encoding="utf-8")
            report = nk_doctor.Report(root, "inputs")
            self.check_inputs(
                root,
                report,
                need_iso=True,
                need_assets=False,
                title_manifest=manifest_file,
                context_manifest=manifest_data,
            )
            disc_results = [r for r in report.results if r.code == "INPUT_DISC_ID"]
            self.assertEqual(len(disc_results), 1)
            self.assertEqual(disc_results[0].status, "PASS")

    def test_check_private_inputs_with_title_manifest_dict(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_valid_inputs(root)
            manifest_dict = self.retail_manifest()
            manifest_dict["id"] = "custom-v1"
            report = nk_doctor.Report(root, "inputs")
            self.check_inputs(
                root,
                report,
                need_iso=True,
                need_assets=False,
                title_manifest=manifest_dict,
            )
            disc_results = [r for r in report.results if r.code == "INPUT_DISC_ID"]
            self.assertEqual(len(disc_results), 1)
            self.assertEqual(disc_results[0].status, "PASS")

    def test_doctor_cli_title_manifest_flag(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_valid_inputs(root)
            manifest_file = root / "title.json"
            manifest_file.write_text(json.dumps(self.retail_manifest()), encoding="utf-8")
            code = nk_doctor.main(["--root", str(root), "--scope", "inputs", "--title-manifest", str(manifest_file)])
            self.assertEqual(code, 0)

    def test_doctor_cli_missing_title_manifest_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            missing = root / "does_not_exist.json"
            code = nk_doctor.main(["--root", str(root), "--scope", "inputs", "--title-manifest", str(missing)])
            self.assertEqual(code, 1)


class RepositoryContractTests(unittest.TestCase):
    def make_docs(self, root: Path, *, manifest_license: str = "GPL-3.0-or-later") -> None:
        (root / "docs").mkdir(parents=True)
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
        (root / "assets" / "release_manifest.json").write_text(
            json.dumps({"license": manifest_license}), encoding="utf-8"
        )

    def test_license_metadata_mismatch_is_a_warning(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_docs(root, manifest_license="GPL-2.0-or-later")
            report = nk_doctor.Report(root, "repo")
            nk_doctor.check_repository_contract(report)
            warnings = [result for result in report.results if result.code == "LICENSE_METADATA" and result.status == "WARN"]
            self.assertEqual(len(warnings), 1)
            self.assertIn("release_manifest.json", warnings[0].path or "")

    def test_consistent_contract_has_no_failures(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_docs(root)
            report = nk_doctor.Report(root, "repo")
            nk_doctor.check_repository_contract(report)
            failures = [result for result in report.results if result.status == "FAIL"]
            self.assertEqual(failures, [])

    def test_live_repository_notice_passes_disclaimer_checks(self) -> None:
        report = nk_doctor.Report(ROOT, "repo")
        nk_doctor.check_repository_contract(report)
        failures = [result for result in report.results if result.code.startswith("NOTICE_") and result.status == "FAIL"]
        self.assertEqual(failures, [], f"Live NOTICE.md failed disclaimer checks: {failures}")


class DatarootAndSaveRootCheckTests(unittest.TestCase):
    def test_sr_dataroot_relative_path_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            report = nk_doctor.Report(root, "inputs")
            with mock.patch.dict(os.environ, {"SR_DATAROOT": "relative/path/to/assets"}):
                context = nk_doctor_checks.title_diagnostic_context(
                    root,
                    PrivateInputTests().retail_manifest(),
                )
                nk_doctor_checks.check_private_inputs(
                    report,
                    need_iso=False,
                    need_assets=True,
                    title_context=context,
                )
            failures = [result for result in report.results if result.code == "INPUT_SR_DATAROOT" and result.status == "FAIL"]
            self.assertTrue(failures)
            self.assertIn("not an absolute path", failures[0].summary)

    def test_sr_dataroot_nonexistent_directory_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            missing = root / "does_not_exist"
            report = nk_doctor.Report(root, "inputs")
            with mock.patch.dict(os.environ, {"SR_DATAROOT": str(missing.resolve())}):
                context = nk_doctor_checks.title_diagnostic_context(
                    root,
                    PrivateInputTests().retail_manifest(),
                )
                nk_doctor_checks.check_private_inputs(
                    report,
                    need_iso=False,
                    need_assets=True,
                    title_context=context,
                )
            failures = [result for result in report.results if result.code == "INPUT_SR_DATAROOT" and result.status == "FAIL"]
            self.assertTrue(failures)
            self.assertIn("not found", failures[0].summary)

    def test_sr_dataroot_empty_directory_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            empty_dir = root / "empty_assets"
            empty_dir.mkdir()
            report = nk_doctor.Report(root, "inputs")
            with mock.patch.dict(os.environ, {"SR_DATAROOT": str(empty_dir.resolve())}):
                context = nk_doctor_checks.title_diagnostic_context(
                    root,
                    PrivateInputTests().retail_manifest(),
                )
                nk_doctor_checks.check_private_inputs(
                    report,
                    need_iso=False,
                    need_assets=True,
                    title_context=context,
                )
            failures = [result for result in report.results if result.code == "INPUT_SR_DATAROOT" and result.status == "FAIL"]
            self.assertTrue(failures)
            self.assertIn("empty", failures[0].summary)

    def test_sr_dataroot_populated_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            assets_dir = root / "populated_assets"
            assets_dir.mkdir()
            (assets_dir / "file1.bin").write_bytes(b"data")
            report = nk_doctor.Report(root, "inputs")
            with mock.patch.dict(os.environ, {"SR_DATAROOT": str(assets_dir.resolve())}):
                context = nk_doctor_checks.title_diagnostic_context(
                    root,
                    PrivateInputTests().retail_manifest(),
                )
                nk_doctor_checks.check_private_inputs(
                    report,
                    need_iso=False,
                    need_assets=True,
                    title_context=context,
                )
            passes = [result for result in report.results if result.code == "INPUT_SR_DATAROOT" and result.status == "PASS"]
            self.assertTrue(passes)
            self.assertEqual(passes[0].metadata.get("files_scanned"), 1)

    def test_save_root_validation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            # Default memstick (created on demand or existing and writable)
            report = nk_doctor.Report(root, "inputs")
            nk_doctor_checks.check_save_root(report, root)
            res = next(r for r in report.results if r.code == "SAVE_ROOT")
            self.assertEqual(res.status, "PASS")

            # SR_MEMSTICK pointing to a regular file fails
            bad_file = root / "save_file.bin"
            bad_file.write_bytes(b"not a dir")
            report_bad = nk_doctor.Report(root, "inputs")
            with mock.patch.dict(os.environ, {"SR_MEMSTICK": str(bad_file.resolve())}):
                nk_doctor_checks.check_save_root(report_bad, root)
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
            report = nk_doctor.Report(root, "products")
            nk_doctor_checks.check_build_profile(report, root, "hst")
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

    def test_json_uses_the_canonical_tool_identifier(self) -> None:
        report = nk_doctor.Report(Path.cwd(), "repo")
        payload = json.loads(nk_doctor.render_json(report, False))
        self.assertEqual(payload["tool"], "nk_doctor")

    def test_canonical_checks_module_is_patchable(self) -> None:
        self.assertIs(nk_doctor_checks, _CHECKS_MODULE)
        with mock.patch.object(nk_doctor_checks, "_probe_powershell", return_value=(None, None, None, "patched")):
            report = nk_doctor.Report(Path.cwd(), "build")
            nk_doctor_checks.check_powershell(report)
        result = next(item for item in report.results if item.code == "POWERSHELL_VERSION")
        self.assertIn("patched", result.detail or "")

    def test_canonical_manifest_does_not_probe_hst_layout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = {
                "schema_version": 1,
                "id": "widget-v1",
                "display_name": "Widget",
                "kind": "synthetic",
                "game_name": "widget",
                "filesystem": {"data_root": "fixtures/widget/data"},
                "executable": {"bss_metadata_source": "none"},
                "modules": [],
            }
            context = nk_doctor_checks.title_diagnostic_context(root, manifest)
            report = nk_doctor.Report(root, "inputs")
            nk_doctor_checks.check_private_inputs(
                report,
                need_iso=True,
                need_assets=True,
                title_manifest=manifest,
                title_context=context,
            )
            failed_codes = {result.code for result in report.results if result.status == "FAIL"}
            self.assertEqual(failed_codes, {"INPUT_EBOOT_ELF", "INPUT_XB_DATA"})
            self.assertNotIn("INPUT_EBOOT_BIN", failed_codes)
            self.assertNotIn("INPUT_PRX_LIBFONT_PRX", failed_codes)
            self.assertNotIn("INPUT_ISO", failed_codes)
            data_result = next(result for result in report.results if result.code == "INPUT_XB_DATA")
            self.assertEqual(data_result.path, "fixtures/widget/data")


class EnvironmentContractTests(unittest.TestCase):
    def test_powerShell_accepts_current_core_line(self) -> None:
        # Still-supported lines per the Microsoft lifecycle table cited in
        # docs/SETUP.md (7.4 LTS and 7.5 until 2026-11-10; 7.6 LTS beyond).
        for version in ("7.4.0", "7.4.20", "7.5.10", "7.6.6", "7.7.0"):
            with self.subTest(version=version), mock.patch.object(
                _CHECKS_MODULE,
                "_probe_powershell",
                return_value=(Path("pwsh"), "Core", version, None),
            ):
                report = nk_doctor.Report(Path.cwd(), "build")
                _CHECKS_MODULE.check_powershell(report)
                result = next(item for item in report.results if item.code == "POWERSHELL_VERSION")
                self.assertEqual(result.status, "PASS")

    def test_powerShell_accepts_future_major_core(self) -> None:
        for version in ("8.0.0", "8.1.2", "9.0.0"):
            with self.subTest(version=version), mock.patch.object(
                _CHECKS_MODULE,
                "_probe_powershell",
                return_value=(Path("pwsh"), "Core", version, None),
            ):
                report = nk_doctor.Report(Path.cwd(), "build")
                _CHECKS_MODULE.check_powershell(report)
                result = next(item for item in report.results if item.code == "POWERSHELL_VERSION")
                self.assertEqual(result.status, "PASS")

    def test_powerShell_rejects_windows_powerShell_or_old_core(self) -> None:
        for edition, version in (
            ("Desktop", "5.1.22621"),
            ("Desktop", "8.0.0"),
            ("Core", "7.3.0"),
            ("Core", "6.2.0"),
        ):
            with self.subTest(edition=edition, version=version), mock.patch.object(
                _CHECKS_MODULE,
                "_probe_powershell",
                return_value=(Path("pwsh"), edition, version, None),
            ):
                report = nk_doctor.Report(Path.cwd(), "build")
                _CHECKS_MODULE.check_powershell(report)
                result = next(item for item in report.results if item.code == "POWERSHELL_VERSION")
                self.assertEqual(result.status, "FAIL")

    def test_powerShell_fails_on_malformed_or_unreadable_probe(self) -> None:
        cases = (
            (Path("pwsh"), "Core", "malformed", None),
            (Path("pwsh"), "Core", "7", None),
            (Path("pwsh"), "Core", "-1.6", None),
            (None, None, None, "pwsh was not found on PATH"),
            (Path("pwsh"), None, None, "exit 1"),
        )
        for executable, edition, version, error in cases:
            with self.subTest(executable=executable, edition=edition, version=version, error=error), mock.patch.object(
                _CHECKS_MODULE,
                "_probe_powershell",
                return_value=(executable, edition, version, error),
            ):
                report = nk_doctor.Report(Path.cwd(), "build")
                _CHECKS_MODULE.check_powershell(report)
                result = next(item for item in report.results if item.code == "POWERSHELL_VERSION")
                self.assertEqual(result.status, "FAIL")

    def test_powershell_floor_is_pinned_to_the_documented_minimum(self) -> None:
        # Issue #337: one floor, four surfaces. The static feature inventory
        # proves no tracked .ps1 needs anything above $IsWindows (6.0); the
        # enforced value is the oldest still-supported line, pinned here so the
        # doctor check, docs, and #requires headers cannot drift apart.
        self.assertEqual(_CHECKS_MODULE.MINIMUM_POWERSHELL, (7, 4))
        floor = _CHECKS_MODULE.MINIMUM_POWERSHELL_TEXT
        for doc in (ROOT / "docs" / "SETUP.md", ROOT / "README.md"):
            text = doc.read_text(encoding="utf-8")
            self.assertIn(
                f"PowerShell {floor}+",
                text,
                f"{doc.relative_to(ROOT)} does not document the PowerShell {floor}+ floor",
            )
        for script in sorted((ROOT / name for name in (
            "copy_build_assets.ps1", "nk.ps1", "nk_manager.ps1", "tools/nk_safety.ps1",
            "tools/test_manager_safety.ps1", "tools/test_visual_oracle.ps1",
            "tools/title_manager_plan.ps1", "tools/vulkan_sdk.ps1",
        ))):
            self.assertIn(
                f"#requires -Version {floor}",
                script.read_text(encoding="utf-8-sig"),
                f"{script.relative_to(ROOT)} does not declare '#requires -Version {floor}'",
            )

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
                _CHECKS_MODULE.os, "name", "nt"
            ), mock.patch.object(
                _CHECKS_MODULE.sys,
                "getwindowsversion",
                return_value=type(
                    "WindowsVersion", (tuple,), {
                        "build": build,
                        "product_type": product_type,
                        "platform_version": (10, 0, build),
                        "service_pack_major": 0,
                    }
                )((10, 0, build, 2, "")),
                create=True,
            ), mock.patch.object(
                _CHECKS_MODULE,
                "_probe_powershell",
                return_value=(powershell_path, "Core", "7.6.4", None),
            ):
                report = nk_doctor.Report(report_root, "build")
                _CHECKS_MODULE.check_platform(report)
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

            report = nk_doctor.Report(root, "build")
            with mock.patch.object(_CHECKS_MODULE, "_find_executable") as find_executable:
                _CHECKS_MODULE.check_shader_provenance(report, root, None)
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

            report = nk_doctor.Report(root, "build")
            with mock.patch.object(_CHECKS_MODULE, "_find_executable", return_value=None):
                _CHECKS_MODULE.check_shader_provenance(report, root, None)
            provenance = next(item for item in report.results if item.code == "SHADER_PROVENANCE")
            glslc = next(item for item in report.results if item.code == "GLSLC")
            self.assertEqual(provenance.status, "FAIL")
            self.assertIn("source SHA-256 mismatch", provenance.detail or "")
            self.assertEqual(glslc.status, "FAIL")


class SimpleFrontEndTests(unittest.TestCase):
    def setUp(self) -> None:
        self.frontend = (ROOT / "nk.ps1").read_text(encoding="utf-8-sig")
        self.manager = (ROOT / "nk_manager.ps1").read_text(encoding="utf-8-sig")
        self.makefile = (ROOT / "Makefile").read_text(encoding="utf-8")

    def test_frontend_exposes_small_supported_surface(self) -> None:
        for script in (
            ROOT / "nk.ps1",
            ROOT / "nk_manager.ps1",
            ROOT / "copy_build_assets.ps1",
            ROOT / "tools" / "nk_safety.ps1",
            ROOT / "tools" / "test_manager_safety.ps1",
            ROOT / "tools" / "test_visual_oracle.ps1",
            ROOT / "tools" / "title_manager_plan.ps1",
            ROOT / "tools" / "vulkan_sdk.ps1",
        ):
            self.assertIn(
                f"#requires -Version {_CHECKS_MODULE.MINIMUM_POWERSHELL_TEXT}",
                script.read_text(encoding="utf-8-sig"),
                script.name,
            )
        self.assertIn("pwsh -NoProfile", self.makefile)
        self.assertNotIn("powershell -NoProfile", self.makefile)
        for action in ("Doctor", "Build", "Rebuild", "Play", "Verify", "Manager"):
            self.assertIn(f'"{action}"', self.frontend)
        self.assertIn('$TitleManifest', self.frontend)
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

    def test_all_tracked_powershell_scripts_declare_consistent_requires_version(self) -> None:
        expected_scripts = {
            ROOT / "copy_build_assets.ps1",
            ROOT / "nk.ps1",
            ROOT / "nk_manager.ps1",
            ROOT / "tools" / "nk_safety.ps1",
            ROOT / "tools" / "test_manager_safety.ps1",
            ROOT / "tools" / "test_visual_oracle.ps1",
            ROOT / "tools" / "title_manager_plan.ps1",
            ROOT / "tools" / "vulkan_sdk.ps1",
        }
        if shutil.which("git") is None:
            self.skipTest("git is required to enumerate the tracked PowerShell policy surface")
        try:
            listing = subprocess.run(
                ["git", "-C", str(ROOT), "ls-files", "-z", "--", ":(icase)*.ps1"],
                capture_output=True,
                text=True,
                check=True,
            ).stdout
        except subprocess.CalledProcessError as exc:
            self.fail(f"git ls-files failed to enumerate tracked PowerShell scripts: {exc}")

        discovered_scripts = {
            ROOT / entry
            for entry in listing.split("\0")
            if entry and (ROOT / entry).is_file()
        }
        self.assertEqual(discovered_scripts, expected_scripts)
        for script in discovered_scripts:
            content = script.read_text(encoding="utf-8-sig")
            expected_header = f"#requires -Version {_CHECKS_MODULE.MINIMUM_POWERSHELL_TEXT}"
            self.assertIn(
                expected_header,
                content,
                f"{script.relative_to(ROOT)} does not contain expected '{expected_header}' header",
            )



class LongPathDiagnosticTests(unittest.TestCase):
    def test_query_windows_long_paths_enabled_mock_registry(self) -> None:
        mock_winreg = mock.MagicMock()
        mock_winreg.HKEY_LOCAL_MACHINE = 1
        mock_winreg.KEY_READ = 2
        mock_key = mock.MagicMock()
        mock_winreg.OpenKey.return_value.__enter__.return_value = mock_key
        mock_winreg.QueryValueEx.return_value = (1, 4)

        with mock.patch.dict("sys.modules", {"winreg": mock_winreg}), \
             mock.patch.object(os, "name", "nt"):
            self.assertTrue(nk_doctor_checks.query_windows_long_paths_enabled())

        mock_winreg.QueryValueEx.return_value = (0, 4)
        with mock.patch.dict("sys.modules", {"winreg": mock_winreg}), \
             mock.patch.object(os, "name", "nt"):
            self.assertFalse(nk_doctor_checks.query_windows_long_paths_enabled())

        mock_winreg.OpenKey.side_effect = OSError("Access denied")
        with mock.patch.dict("sys.modules", {"winreg": mock_winreg}), \
             mock.patch.object(os, "name", "nt"):
            self.assertFalse(nk_doctor_checks.query_windows_long_paths_enabled())

    def test_long_paths_pass_when_enabled_and_under_260(self) -> None:
        report = nk_doctor.Report(Path(r"C:\work\repo"), "build")
        with mock.patch.object(nk_doctor_checks, "query_windows_long_paths_enabled", return_value=True), \
             mock.patch.object(nk_doctor_checks.platform, "system", return_value="Windows"):
            nk_doctor_checks.check_long_paths(report, Path(r"C:\work\repo"))
        results = [r for r in report.results if r.code == "LONG_PATHS"]
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].status, "PASS")
        self.assertLessEqual(results[0].metadata.get("total_len", 0), 260)

    def test_long_paths_warn_when_policy_disabled(self) -> None:
        report = nk_doctor.Report(Path(r"C:\work\repo"), "build")
        with mock.patch.object(nk_doctor_checks, "query_windows_long_paths_enabled", return_value=False), \
             mock.patch.object(nk_doctor_checks.platform, "system", return_value="Windows"):
            nk_doctor_checks.check_long_paths(report, Path(r"C:\work\repo"))
        results = [r for r in report.results if r.code == "LONG_PATHS"]
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].status, "WARN")
        self.assertNotEqual(results[0].status, "FAIL")
        self.assertIn("LongPathsEnabled", results[0].summary)

    def test_long_paths_warn_when_path_exceeds_260(self) -> None:
        very_long_root = Path("C:\\" + "a" * 250)
        report = nk_doctor.Report(very_long_root, "build")
        with mock.patch.object(nk_doctor_checks, "query_windows_long_paths_enabled", return_value=True), \
             mock.patch.object(nk_doctor_checks.platform, "system", return_value="Windows"):
            nk_doctor_checks.check_long_paths(report, very_long_root)
        results = [r for r in report.results if r.code == "LONG_PATHS"]
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].status, "WARN")
        self.assertNotEqual(results[0].status, "FAIL")
        self.assertIn("exceeds 260", results[0].summary)

    def test_long_paths_advisory_never_fails(self) -> None:
        very_long_root = Path("C:\\" + "a" * 250)
        report = nk_doctor.Report(very_long_root, "build")
        with mock.patch.object(nk_doctor_checks, "query_windows_long_paths_enabled", return_value=False), \
             mock.patch.object(nk_doctor_checks.platform, "system", return_value="Windows"):
            nk_doctor_checks.check_long_paths(report, very_long_root)
        results = [r for r in report.results if r.code == "LONG_PATHS"]
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].status, "WARN")
        self.assertNotEqual(results[0].status, "FAIL")


if __name__ == "__main__":
    unittest.main()
