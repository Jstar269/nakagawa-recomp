#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from __future__ import annotations

import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import hst_doctor  # noqa: E402
import hst_doctor_checks  # noqa: E402
import hst_doctor_core  # noqa: E402
from hst_test_fixtures import write_elf, write_iso, write_psp_header  # noqa: E402


def write_pe(path: Path, *, pe_offset: int = 0x80) -> None:
    data = bytearray(max(0x100, pe_offset + 6))
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, pe_offset)
    data[pe_offset : pe_offset + 4] = b"PE\0\0"
    struct.pack_into("<H", data, pe_offset + 4, 0x8664)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write_synthetic_elf(
    path: Path,
    segments: list[tuple[int, int, int]],
    *,
    machine: int = 8,
    file_padding: int = 0,
) -> None:
    phoff = 52
    phentsize = 32
    data_offset = phoff + phentsize * len(segments)
    header = bytearray(52)
    header[:4] = b"\x7fELF"
    header[4] = 1  # ELF32
    header[5] = 1  # little-endian
    header[6] = 1  # version 1
    struct.pack_into("<HHI", header, 16, 2, machine, 1)
    struct.pack_into("<III", header, 24, 0, phoff, 0)
    struct.pack_into("<I", header, 36, 0)
    struct.pack_into("<HHHHHH", header, 40, 52, phentsize, len(segments), 40, 0, 0)
    phdrs = bytearray()
    curr_offset = data_offset
    for index, (p_type, filesz, memsz) in enumerate(segments):
        phdrs.extend(
            struct.pack(
                "<8I",
                p_type,
                curr_offset,
                index * 0x1000,
                index * 0x1000,
                filesz,
                memsz,
                5,
                0x10,
            )
        )
        curr_offset += filesz
    payload = b"\0" * (curr_offset - data_offset + file_padding)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(header) + bytes(phdrs) + payload)


class FormatHardeningTests(unittest.TestCase):
    def test_rejects_pe_header_offset_outside_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.exe"
            data = bytearray(0x80)
            data[:2] = b"MZ"
            struct.pack_into("<I", data, 0x3C, 0x10000000)
            path.write_bytes(data)
            ok, error = hst_doctor._validate_pe_x64(path)
            self.assertFalse(ok)
            self.assertIn("outside", error)

    def test_accepts_synthetic_x64_pe(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "good.exe"
            write_pe(path)
            ok, detail = hst_doctor._validate_pe_x64(path)
            self.assertTrue(ok, detail)

    def test_rejects_non_primary_iso_descriptor(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.iso"
            write_iso(path, descriptor_type=2)
            metadata, error = hst_doctor._validate_iso(path)
            self.assertIsNone(metadata)
            self.assertIn("expected primary", error or "")

    def test_accepts_non_load_segment_with_filesz_greater_than_memsz(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "non_load.elf"
            # Segment 0: PT_LOAD (type 1) with filesz <= memsz
            # Segment 1: processor-specific non-PT_LOAD (type 0x700000a1) with filesz > memsz
            write_synthetic_elf(
                path,
                [(1, 16, 16), (0x700000A1, 32, 0)],
            )
            metadata, error = hst_doctor_core._parse_elf(path)
            self.assertIsNone(error)
            self.assertIsNotNone(metadata)
            assert metadata is not None
            self.assertEqual(metadata["load_segments"], 1)

    def test_rejects_pt_load_segment_with_filesz_greater_than_memsz(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad_load.elf"
            # Segment 0: PT_LOAD (type 1) with filesz > memsz
            write_synthetic_elf(path, [(1, 32, 16)])
            metadata, error = hst_doctor_core._parse_elf(path)
            self.assertIsNone(metadata)
            self.assertIn("has p_memsz < p_filesz", error or "")

    def test_rejects_segment_extending_beyond_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "overflow.elf"
            write_synthetic_elf(path, [(1, 64, 64)])
            data = path.read_bytes()
            path.write_bytes(data[:-10])
            metadata, error = hst_doctor_core._parse_elf(path)
            self.assertIsNone(metadata)
            self.assertIn("extends beyond the file", error or "")


class InputPairHardeningTests(unittest.TestCase):
    def test_mismatched_eboot_segment_counts_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_elf(root / "place_game_here" / "EBOOT.elf", load_segments=1)
            write_psp_header(
                root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "SYSDIR" / "EBOOT.BIN",
                segments=2,
            )
            decrypted = root / "place_game_here" / "EXTRACTED" / "decrypted"
            for name in ("libfont.prx", "scePsmf_library.prx", "scePsmfP_library.prx"):
                write_elf(decrypted / name)
            report = hst_doctor.Report(root, "inputs")
            hst_doctor.check_private_inputs(report, need_iso=False, need_assets=False)
            result = [item for item in report.results if item.code == "INPUT_EBOOT_PAIR"][-1]
            self.assertEqual(result.status, "FAIL")


class StrictModeHardeningTests(unittest.TestCase):
    def test_strict_warning_exit_is_two(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "docs").mkdir()
            (root / "interface").mkdir()
            (root / "assets").mkdir()
            (root / "LICENSE").write_text(
                "GNU GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n",
                encoding="utf-8",
            )
            notice = """GPL-3.0-or-later
This repository does not grant rights to the game.
The project ships no decryption keys of any kind.
This project is independent and is not endorsed.
Users must supply their own legally obtained inputs.
This remains subject to legal review.
"""
            for name, content in {
                "NOTICE.md": notice,
                "README.md": "GPL-3.0-or-later\n",
                "CONTRIBUTING.md": "GPL-3.0-or-later\n",
                "SECURITY.md": "security\n",
                "CODE_OF_CONDUCT.md": "conduct\n",
            }.items():
                (root / name).write_text(content, encoding="utf-8")
            (root / "docs" / "PUBLICATION_READINESS.md").write_text("publication\n", encoding="utf-8")
            (root / "interface" / "package.json").write_text(
                json.dumps({"license": "GPL-2.0-or-later"}),
                encoding="utf-8",
            )
            (root / "assets" / "release_manifest.json").write_text(
                json.dumps({"license": "GPL-3.0-or-later"}),
                encoding="utf-8",
            )
            proc = subprocess.run(
                [
                    sys.executable,
                    str(TOOLS / "hst_doctor.py"),
                    "--root",
                    str(root),
                    "--scope",
                    "repo",
                    "--json",
                    "--strict",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
            self.assertEqual(json.loads(proc.stdout)["exit_code"], 2)


class ManagerExitPropagationHardeningTests(unittest.TestCase):
    def test_parameterized_manager_actions_fail_closed(self) -> None:
        manager = (ROOT / "hst_manager.ps1").read_text(encoding="utf-8-sig")
        # Each failing action records a nonzero termination code and breaks out of the
        # switch; the single `exit` after the finally block applies it, so the caller
        # sees a nonzero status AND the caller's location is restored first.
        self.assertIn('"BuildFull" { if (-not (Invoke-HstBuild -Mode "Full")) { $script:ManagerExitCode = 1; break } }', manager)
        self.assertIn('"BuildFast" { if (-not (Invoke-HstBuild -Mode "Fast")) { $script:ManagerExitCode = 1; break } }', manager)
        self.assertIn('if (-not (Invoke-Selftest)) { $script:ManagerExitCode = 1; break }', manager)
        self.assertIn('$script:LastRunResult = $null', manager)
        self.assertIn('if ($null -eq $script:LastRunResult) { $script:ManagerExitCode = 1; break }', manager)
        self.assertIn('if ($Action -and $script:ManagerExitCode -ne 0) {\n    exit $script:ManagerExitCode', manager)

    def test_frontend_does_not_mask_manager_failure(self) -> None:
        frontend = (ROOT / "hst.ps1").read_text(encoding="utf-8-sig")
        self.assertNotIn("Invoke-ManagerBuild", frontend)
        self.assertNotIn("Get-HstProductBackupPath", frontend)
        self.assertIn("$LASTEXITCODE = 0", frontend)
        self.assertIn("$exitCode = [int]$LASTEXITCODE", frontend)
        self.assertIn('DoctorScope "products"', frontend)


class SetupMatrixScenariosTests(unittest.TestCase):
    """Test the fresh-clone setup matrix cases (A through G) using synthetic fixtures.

    Synthetic fixtures prove structural preflight acceptance (format headers, segment
    counts, descriptor types, and basic non-emptiness). They do NOT represent a retail
    extraction (e.g. HST's 56,672 files) or proof of an in-game retail launch.
    """

    def _setup_synthetic_preflight_workspace(self, root: Path) -> None:
        write_elf(root / "place_game_here" / "EBOOT.elf")
        write_psp_header(root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "SYSDIR" / "EBOOT.BIN")
        decrypted = root / "place_game_here" / "EXTRACTED" / "decrypted"
        for name in ("libfont.prx", "scePsmf_library.prx", "scePsmfP_library.prx"):
            write_elf(decrypted / name)
        write_iso(root / "place_game_here" / "ISO" / "synthetic.iso")
        data_root = root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "USRDIR" / "xbdata_extracted"
        (data_root / "archive.xb.d" / "data").mkdir(parents=True)
        (data_root / "archive.xb.d" / "data" / "sample.bin.txt").write_text("synthetic", encoding="utf-8")

    def test_case_a_no_private_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            report = hst_doctor.Report(root, "inputs")
            hst_doctor_checks.check_private_inputs(report, need_iso=True, need_assets=True)
            failed_codes = {res.code for res in report.results if res.status == "FAIL"}
            self.assertIn("INPUT_EBOOT_ELF", failed_codes)
            self.assertIn("INPUT_EBOOT_BIN", failed_codes)
            self.assertIn("INPUT_PRX_LIBFONT_PRX", failed_codes)
            self.assertIn("INPUT_PRX_SCEPSMF_LIBRARY_PRX", failed_codes)
            self.assertIn("INPUT_PRX_SCEPSMFP_LIBRARY_PRX", failed_codes)
            self.assertIn("INPUT_ISO", failed_codes)
            self.assertIn("INPUT_XB_DATA", failed_codes)

    def test_case_b_extraction_present_iso_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._setup_synthetic_preflight_workspace(root)
            (root / "place_game_here" / "ISO" / "synthetic.iso").unlink()
            report = hst_doctor.Report(root, "inputs")
            hst_doctor_checks.check_private_inputs(report, need_iso=True, need_assets=True)
            failed_codes = {res.code for res in report.results if res.status == "FAIL"}
            self.assertEqual(failed_codes, {"INPUT_ISO"})

    def test_case_c_iso_present_extraction_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._setup_synthetic_preflight_workspace(root)
            data_root = root / "place_game_here" / "EXTRACTED" / "PSP_GAME" / "USRDIR" / "xbdata_extracted"
            for child in sorted(data_root.rglob("*"), reverse=True):
                if child.is_file():
                    child.unlink()
                else:
                    child.rmdir()
            data_root.rmdir()
            report = hst_doctor.Report(root, "inputs")
            hst_doctor_checks.check_private_inputs(report, need_iso=True, need_assets=True)
            failed_codes = {res.code for res in report.results if res.status == "FAIL"}
            self.assertEqual(failed_codes, {"INPUT_XB_DATA"})

    def test_case_d_explicitly_wrong_configured_data_root(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._setup_synthetic_preflight_workspace(root)
            import os
            from unittest import mock
            # Relative path rejected
            with mock.patch.dict(os.environ, {"SR_DATAROOT": "relative/xbdata"}):
                report = hst_doctor.Report(root, "inputs")
                hst_doctor_checks.check_private_inputs(report, need_iso=True, need_assets=True)
                failed_codes = {res.code for res in report.results if res.status == "FAIL"}
                self.assertIn("INPUT_SR_DATAROOT", failed_codes)

            # Nonexistent path rejected
            missing = root / "nonexistent"
            with mock.patch.dict(os.environ, {"SR_DATAROOT": str(missing.resolve())}):
                report = hst_doctor.Report(root, "inputs")
                hst_doctor_checks.check_private_inputs(report, need_iso=True, need_assets=True)
                failed_codes = {res.code for res in report.results if res.status == "FAIL"}
                self.assertIn("INPUT_SR_DATAROOT", failed_codes)

    def test_case_e_valid_public_safe_candidate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build_dir = root / "build" / "hst"
            build_dir.mkdir(parents=True)
            # Structured sections format
            (build_dir / "runtime_profile.json").write_text(
                json.dumps({
                    "schema_version": 1,
                    "sections": {
                        "runtime": {
                            "entries": ["CFLAGS=-O0 -DSR_PUBLIC_SAFE", "GE_CFLAGS=-O2"]
                        }
                    }
                }),
                encoding="utf-8",
            )
            report = hst_doctor.Report(root, "products")
            hst_doctor_checks.check_build_profile(report, root)
            res = next(r for r in report.results if r.code == "BUILD_PROFILE")
            self.assertEqual(res.status, "INFO")
            self.assertEqual(res.metadata.get("public_safe"), 1)
            self.assertIn("public-safe", res.summary)

            # Full profile
            (build_dir / "runtime_profile.json").write_text(
                json.dumps({
                    "schema_version": 1,
                    "sections": {
                        "runtime": {
                            "entries": ["CFLAGS=-O0", "GE_CFLAGS=-O2"]
                        }
                    }
                }),
                encoding="utf-8",
            )
            report_full = hst_doctor.Report(root, "products")
            hst_doctor_checks.check_build_profile(report_full, root)
            res_full = next(r for r in report_full.results if r.code == "BUILD_PROFILE")
            self.assertEqual(res_full.status, "INFO")
            self.assertEqual(res_full.metadata.get("public_safe"), 0)
            self.assertIn("full", res_full.summary)

    def test_case_f_malformed_save_root(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bad_save = root / "save_file.bin"
            bad_save.write_bytes(b"not a folder")
            import os
            from unittest import mock
            with mock.patch.dict(os.environ, {"SR_MEMSTICK": str(bad_save.resolve())}):
                report = hst_doctor.Report(root, "inputs")
                hst_doctor_checks.check_save_root(report, root)
                failed = [res for res in report.results if res.code == "SAVE_ROOT" and res.status == "FAIL"]
                self.assertTrue(failed)

    def test_case_f_unwritable_save_root(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            save_dir = root / "memstick"
            save_dir.mkdir(parents=True)
            from unittest import mock
            with mock.patch.object(Path, "write_bytes", side_effect=PermissionError("access denied")):
                report = hst_doctor.Report(root, "inputs")
                hst_doctor_checks.check_save_root(report, root)
                failed = [res for res in report.results if res.code == "SAVE_ROOT" and res.status == "FAIL"]
                self.assertTrue(failed)
                self.assertIn("not writable", failed[0].summary)

    def test_case_g_synthetic_structurally_valid_preflight_inputs(self) -> None:
        """Synthetic preflight fixtures pass structural checks (SYNTHETIC/PREFLIGHT-VALID only)."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._setup_synthetic_preflight_workspace(root)
            report = hst_doctor.Report(root, "inputs")
            hst_doctor_checks.check_private_inputs(report, need_iso=True, need_assets=True)
            hst_doctor_checks.check_save_root(report, root)
            failures = [res for res in report.results if res.status == "FAIL"]
            self.assertEqual(failures, [])

    def test_case_h_lawful_private_preflight_if_available(self) -> None:
        """Preflight validation of live private inputs if present (preflight only, not retail acceptance)."""
        live_root = ROOT
        eboot_elf = live_root / "place_game_here" / "EBOOT.elf"
        if not eboot_elf.is_file():
            self.skipTest("Lawful private EBOOT.elf not available in workspace")
        report = hst_doctor.Report(live_root, "inputs")
        hst_doctor_checks.check_private_inputs(report, need_iso=False, need_assets=False)
        eboot_elf_res = next((r for r in report.results if r.code == "INPUT_EBOOT_ELF"), None)
        self.assertIsNotNone(eboot_elf_res)
        self.assertEqual(eboot_elf_res.status, "PASS")


class AgentIdentityChecks(unittest.TestCase):
    """A repository-local commit identity must be reported, whichever key carries it.

    The check exists because automated sessions have left an identity behind in
    this workspace, silently re-authoring later commits. Two ways of failing it
    are easy and were both found on review: looking only at `user.*` when Git
    also honours `author.*` and `committer.*`, and reporting only the
    highest-precedence scope so the remediation leaves a lower one effective.
    """

    def _repo(self, tmp: str, *config: tuple[str, ...]) -> Path:
        root = Path(tmp) / "repo"
        subprocess.run(["git", "init", "-q", str(root)], check=True, capture_output=True)
        for args in config:
            subprocess.run(["git", "-C", str(root), "config", *args],
                           check=True, capture_output=True)
        return root

    def _run(self, root: Path):
        report = hst_doctor.Report(root, "identity")
        hst_doctor_checks.check_agent_identity(report)
        results = [r for r in report.results if r.code == "GIT_IDENTITY"]
        self.assertEqual(len(results), 1, "the check must report exactly once")
        return results[0]

    def test_clean_checkout_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self._run(self._repo(tmp)).status, "PASS")

    def test_unrelated_user_key_is_not_an_identity(self) -> None:
        """user.signingkey is not an identity, and must not be reported as one."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._repo(tmp, ("user.signingkey", "ABCD1234"))
            self.assertEqual(self._run(root).status, "PASS")

    def test_local_user_identity_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = self._repo(tmp, ("user.name", "opencode"),
                              ("user.email", "opencode@nakagawa.local"))
            result = self._run(root)
            self.assertEqual(result.status, "WARN")
            self.assertIn("opencode@nakagawa.local", result.summary)

    def test_author_and_committer_keys_are_reported(self) -> None:
        """Git honours author.*/committer.* over user.*, so both must be inspected.

        Reproduced against Git 2.43: a repository-local author.email authors
        every commit in the checkout while user.email is unset entirely.
        """
        for key in ("author.name", "author.email", "committer.name", "committer.email"):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as tmp:
                root = self._repo(tmp, (key, "bot@x.invalid"))
                result = self._run(root)
                self.assertEqual(result.status, "WARN")
                self.assertIn(key, result.summary)

    def test_empty_value_is_still_an_override(self) -> None:
        """An empty [user] entry breaks commits; it must not read as unset."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._repo(tmp, ("user.email", ""))
            result = self._run(root)
            self.assertEqual(result.status, "WARN")
            self.assertIn("(empty)", result.summary)

    def test_worktree_scope_is_not_double_reported_when_extension_is_off(self) -> None:
        """Without extensions.worktreeConfig, --worktree *is* --local in Git.

        Reading it as a separate scope reports every .git/config value twice and
        emits a `--worktree --unset` remedy that Git refuses.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = self._repo(tmp, ("user.name", "localguy"))
            summary = self._run(root).summary
            self.assertNotIn("worktree:", summary)
            self.assertNotIn("--worktree", summary)

    def test_remediation_clears_every_populated_scope(self) -> None:
        """The emitted commands must leave no identity behind, in either scope.

        Clearing only the higher-precedence worktree value leaves the common
        .git/config identity immediately effective, which is the failure this
        asserts against: the remedy is run verbatim and the check must then pass.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = self._repo(
                tmp,
                ("extensions.worktreeConfig", "true"),
                ("user.name", "localguy"), ("user.email", "l@x.invalid"),
                ("committer.name", "cbot"),
                ("--worktree", "user.name", "wtguy"),
                ("--worktree", "author.email", "w@x.invalid"),
            )
            summary = self._run(root).summary
            self.assertIn("worktree:user.name=wtguy", summary)
            self.assertIn("local:user.name=localguy", summary)
            commands = re.search(r"clear it with '(.+)'\.$", summary)
            self.assertIsNotNone(commands, "the warning must carry a remediation")
            for command in commands.group(1).split(" && "):
                self.assertTrue(command.startswith("git config "), command)
                proc = subprocess.run(["git", "-C", str(root), *command.split()[1:]],
                                      capture_output=True, text=True)
                self.assertEqual(proc.returncode, 0, f"{command}: {proc.stderr}")
            self.assertEqual(self._run(root).status, "PASS")

    def test_uninspectable_config_warns_rather_than_passing(self) -> None:
        """A failed lookup must never read as a clean checkout."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "not-a-repo"
            root.mkdir()
            result = self._run(root)
            self.assertEqual(result.status, "WARN")
            self.assertIn("cannot be ruled out", result.summary)


if __name__ == "__main__":
    unittest.main()
