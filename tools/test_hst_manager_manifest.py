# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Hermetic opt-in HST manager/build planning and precedence tests."""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
MANAGER = ROOT / "hst_manager.ps1"
MANIFEST = ROOT / "assets" / "titles" / "hst-ucus98701.json"
HELPER = ROOT / "tools" / "title_manager_plan.ps1"
VULKAN_HELPER = ROOT / "tools" / "vulkan_sdk.ps1"


class HstManagerManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.shell = shutil.which("pwsh")
        cls.make = shutil.which("mingw32-make") or shutil.which("make")
        if cls.shell is None or cls.make is None:
            raise unittest.SkipTest("PowerShell 7.6+ (pwsh) and GNU Make are required")

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="nakagawa-issue197-manager-")
        self.root = Path(self.temp.name)
        fake_sdk = self.root / "fake-sdk"
        (fake_sdk / "Include" / "vulkan").mkdir(parents=True)
        (fake_sdk / "Include" / "vulkan" / "vulkan.h").write_text("// synthetic Vulkan header\n", encoding="ascii")
        (fake_sdk / "Lib").mkdir()
        (fake_sdk / "Lib" / "vulkan-1.lib").write_bytes(b"synthetic Vulkan import library")
        private = self.root / "place_game_here"
        (private / "EXTRACTED" / "decrypted").mkdir(parents=True)
        (private / "EXTRACTED" / "PSP_GAME" / "SYSDIR").mkdir(parents=True)
        (private / "EBOOT.elf").write_text("synthetic private binding\n", encoding="ascii")
        for name in ("libfont.prx", "scePsmf_library.prx", "scePsmfP_library.prx"):
            (private / "EXTRACTED" / "decrypted" / name).write_text("synthetic\n", encoding="ascii")
        (private / "EXTRACTED" / "PSP_GAME" / "SYSDIR" / "EBOOT.BIN").write_text(
            "synthetic\n", encoding="ascii"
        )
        # The manager now anchors every managed path to its own script location and fails
        # closed when the workspace identity anchors are missing (#183), so the harness
        # stages a complete fake workspace: the manager itself, its dot-sourced helpers
        # and the repository identity files.
        self.manager_copy = self.root / "hst_manager.ps1"
        shutil.copy2(MANAGER, self.manager_copy)
        tools_dir = self.root / "tools"
        tools_dir.mkdir(exist_ok=True)
        for helper in (
            "hst_safety.ps1",
            "hst_run_support.ps1",
            "vulkan_sdk.ps1",
            "title_manager_plan.ps1",
            "title_codegen_plan.py",
            "title_manifest.py",
        ):
            shutil.copy2(ROOT / "tools" / helper, tools_dir / helper)
        (self.root / "AGENTS.md").write_text("synthetic anchors\n", encoding="utf-8")
        (self.root / "src" / "rt").mkdir(parents=True)
        (self.root / "src" / "rt" / "recomp.c").write_text("synthetic\n", encoding="ascii")
        (tools_dir / "codegen.py").write_text("synthetic\n", encoding="ascii")
        (self.root / "capture.py").write_text(
            textwrap.dedent(
                """
                import json
                import os
                from pathlib import Path
                import sys

                target, game_name, game_elf, base, entry, extra, header, profile, build_dir, funcs, runtime, recomp, sdk = sys.argv[1:]
                record = {
                    "target": target,
                    "game_name": game_name,
                    "game_elf": game_elf,
                    "base": base,
                    "entry": entry,
                    "extra": extra,
                    "header": header,
                    "profile": profile,
                    "build_dir": build_dir,
                    "funcs": funcs,
                    "runtime": runtime,
                    "recomp": recomp,
                    "sdk": sdk,
                    "hst_extra_spans": os.environ.get("HST_EXTRA_SPANS"),
                }
                with (Path("capture.jsonl")).open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps(record, sort_keys=True) + "\\n")
                if target == "all":
                    output = Path(build_dir) / f"{game_name}.exe"
                    output.parent.mkdir(parents=True, exist_ok=True)
                    output.write_bytes(b"synthetic manager output")
                """
            ).strip()
            + "\n",
            encoding="utf-8",
        )
        (self.root / "Makefile").write_text(
            textwrap.dedent(
                """
                PYTHON ?= python
                GAME_NAME ?= mygame
                GAME_ELF ?= eboot.elf
                GAME_BASE ?= 0x08804000
                GAME_ENTRY ?= 0x08804000
                ifeq ($(GAME_NAME),hst)
                CODEGEN_PROFILE_ARG ?= --profile=hst
                GAME_EXTRA_ELFS ?= place_game_here/EXTRACTED/decrypted/libfont.prx@0x32200000 place_game_here/EXTRACTED/decrypted/scePsmf_library.prx@0x32280000 place_game_here/EXTRACTED/decrypted/scePsmfP_library.prx@0x322f8868
                GAME_PSP_HEADER ?= place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN
                endif
                CODEGEN_PROFILE_ARG ?=
                GAME_EXTRA_ELFS ?=
                GAME_PSP_HEADER ?=
                BUILD_DIR ?= build/$(GAME_NAME)
                FUNCS_PER_CHUNK ?= 2000
                RUNTIME_OPT ?= -O0
                RECOMP_OPT ?= -O0
                .PHONY: all clean selftest vfpu_fuzz
                vfpu_fuzz:
                __RECIPE_TAB__$(PYTHON) capture.py vfpu_fuzz "$(GAME_NAME)" "$(GAME_ELF)" "$(GAME_BASE)" "$(GAME_ENTRY)" "$(GAME_EXTRA_ELFS)" "$(GAME_PSP_HEADER)" "$(CODEGEN_PROFILE_ARG)" "$(BUILD_DIR)" "$(FUNCS_PER_CHUNK)" "$(RUNTIME_OPT)" "$(RECOMP_OPT)" "$(VULKAN_SDK)"
                all:
                __RECIPE_TAB__$(PYTHON) capture.py all "$(GAME_NAME)" "$(GAME_ELF)" "$(GAME_BASE)" "$(GAME_ENTRY)" "$(GAME_EXTRA_ELFS)" "$(GAME_PSP_HEADER)" "$(CODEGEN_PROFILE_ARG)" "$(BUILD_DIR)" "$(FUNCS_PER_CHUNK)" "$(RUNTIME_OPT)" "$(RECOMP_OPT)" "$(VULKAN_SDK)"
                clean:
                __RECIPE_TAB__$(PYTHON) capture.py clean "$(GAME_NAME)" "$(GAME_ELF)" "$(GAME_BASE)" "$(GAME_ENTRY)" "$(GAME_EXTRA_ELFS)" "$(GAME_PSP_HEADER)" "$(CODEGEN_PROFILE_ARG)" "$(BUILD_DIR)" "$(FUNCS_PER_CHUNK)" "$(RUNTIME_OPT)" "$(RECOMP_OPT)" "$(VULKAN_SDK)"
                selftest:
                __RECIPE_TAB__$(PYTHON) capture.py selftest "$(GAME_NAME)" "$(GAME_ELF)" "$(GAME_BASE)" "$(GAME_ENTRY)" "$(GAME_EXTRA_ELFS)" "$(GAME_PSP_HEADER)" "$(CODEGEN_PROFILE_ARG)" "$(BUILD_DIR)" "$(FUNCS_PER_CHUNK)" "$(RUNTIME_OPT)" "$(RECOMP_OPT)" "$(VULKAN_SDK)"
                """
            ).replace("__RECIPE_TAB__", "\t").lstrip(),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def run_manager(self, action: str, manifest: Path | None = None, **overrides) -> subprocess.CompletedProcess[str]:
        make_executable = overrides.pop("MakeExecutable", self.make)
        command = [
            self.shell,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(self.manager_copy),
            "-Action",
            action,
            "-MsysPath",
            str(Path(self.make).parent),
            "-MakeExecutable",
            str(make_executable),
            "-VulkanSdk",
            str(self.root / "fake-sdk"),
        ]
        if manifest is not None:
            command.extend(["-TitleManifest", str(manifest)])
        for key, value in overrides.items():
            command.extend([f"-{key}", str(value)])
        env = os.environ.copy()
        env.pop("HST_EXTRA_SPANS", None)
        return subprocess.run(
            command,
            cwd=self.root,
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout=120,
        )

    def run_in_one_shell(self, body: str, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
        """Run several manager invocations inside a single PowerShell process.

        Environment leakage is only observable when the manager shares a process with a
        later caller, which `-File` per-invocation runs can never demonstrate.
        """
        env = os.environ.copy()
        env.pop("HST_EXTRA_SPANS", None)
        return subprocess.run(
            [self.shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", body],
            cwd=str(cwd or self.root),
            env=env,
            capture_output=True,
            text=True,
            check=False,
            stdin=subprocess.DEVNULL,
            timeout=180,
        )

    def manager_call(self, action: str, manifest: Path | None = None) -> str:
        call = (
            f"& '{self.manager_copy}' -Action {action} "
            f"-MsysPath '{Path(self.make).parent}' -MakeExecutable '{self.make}' "
            f"-VulkanSdk '{self.root / 'fake-sdk'}'"
        )
        if manifest is not None:
            call += f" -TitleManifest '{manifest}'"
        return call

    def make_sdk(self, name: str, *, complete: bool = True) -> Path:
        sdk = self.root / name
        (sdk / "Include" / "vulkan").mkdir(parents=True, exist_ok=True)
        (sdk / "Include" / "vulkan" / "vulkan.h").write_text("// synthetic header\n", encoding="ascii")
        if complete:
            (sdk / "Lib").mkdir(exist_ok=True)
            (sdk / "Lib" / "vulkan-1.lib").write_bytes(b"synthetic import library")
        return sdk

    def resolve_sdk(self, *, explicit: Path | None = None, environment: Path | None = None) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        if environment is None:
            env.pop("VULKAN_SDK", None)
        else:
            env["VULKAN_SDK"] = str(environment)
        explicit_arg = str(explicit) if explicit else ""
        command = (
            f". '{VULKAN_HELPER}'; "
            f"try {{ Resolve-VulkanSdk -ExplicitPath '{explicit_arg}' -InstallRoot '{self.root}' }} "
            "catch { Write-Error $_; exit 1 }"
        )
        return subprocess.run(
            [self.shell, "-NoProfile", "-NonInteractive", "-Command", command],
            cwd=self.root,
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )

    SPAN = "0x00303194,0x00306e24"

    def test_manifest_span_is_scoped_and_never_leaks_into_a_later_legacy_run(self) -> None:
        proc = self.run_in_one_shell(
            "\n".join(
                [
                    self.manager_call("BuildFast", MANIFEST),
                    self.manager_call("BuildFast"),
                    "if (Test-Path -LiteralPath 'Env:HST_EXTRA_SPANS') "
                    "{ Write-Output \"FINAL=PRESENT:$env:HST_EXTRA_SPANS\" } "
                    "else { Write-Output 'FINAL=ABSENT' }",
                ]
            )
        )
        combined = proc.stdout + proc.stderr
        self.assertIn("FINAL=ABSENT", combined, combined)
        records = [record for record in self.records() if record["target"] == "all"]
        self.assertEqual(len(records), 2, combined)
        manifest_run, legacy_run = records
        # The manifest build's child still receives the span...
        self.assertEqual(manifest_run["hst_extra_spans"], self.SPAN)
        # ...and the legacy build that follows it in the same process does not.
        self.assertIsNone(legacy_run["hst_extra_spans"])

    def test_preexisting_caller_span_is_restored_exactly_after_the_manager_exits(self) -> None:
        caller = "0x00000010,0x00000020"
        proc = self.run_in_one_shell(
            "\n".join(
                [
                    f"$env:HST_EXTRA_SPANS = '{caller}'",
                    self.manager_call("BuildFast", MANIFEST),
                    'Write-Output "FINAL=$env:HST_EXTRA_SPANS"',
                ]
            )
        )
        combined = proc.stdout + proc.stderr
        self.assertIn(f"FINAL={caller}", combined, combined)
        record = [item for item in self.records() if item["target"] == "all"][-1]
        # Scoped to the build; the caller's unrelated value never reached the analyzer.
        self.assertEqual(record["hst_extra_spans"], self.SPAN)

    def test_scoped_span_unwinds_when_the_scoped_operation_throws(self) -> None:
        body = "\n".join(
            [
                f". '{HELPER}'",
                "function Invoke-Failing {",
                "  $state = Push-TitleAnalyzerEnvironment -Value 'SCOPED-VALUE'",
                "  try { throw 'synthetic spawn failure' }",
                "  finally { Pop-TitleAnalyzerEnvironment -State $state }",
                "}",
                "$env:HST_EXTRA_SPANS = 'ORIGINAL-VALUE'",
                "try { Invoke-Failing } catch { }",
                'Write-Output "RESTORED=$env:HST_EXTRA_SPANS"',
                "Remove-Item -LiteralPath 'Env:HST_EXTRA_SPANS' -Force",
                "try { Invoke-Failing } catch { }",
                "if (Test-Path -LiteralPath 'Env:HST_EXTRA_SPANS') "
                "{ Write-Output 'ABSENT=NO' } else { Write-Output 'ABSENT=YES' }",
            ]
        )
        proc = self.run_in_one_shell(body, cwd=ROOT)
        combined = proc.stdout + proc.stderr
        self.assertIn("RESTORED=ORIGINAL-VALUE", combined, combined)
        self.assertIn("ABSENT=YES", combined, combined)
        self.assert_no_make()

    def records(self) -> list[dict[str, str | None]]:
        path = self.root / "capture.jsonl"
        if not path.exists():
            return []
        return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]

    def write_manifest(self, name: str, mutate) -> Path:
        value = json.loads(MANIFEST.read_text(encoding="utf-8"))
        mutate(value)
        path = self.root / name
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def assert_manager_success(self, proc: subprocess.CompletedProcess[str]) -> None:
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def assert_no_make(self) -> None:
        self.assertEqual(self.records(), [])

    def test_buildfull_legacy_and_manifest_modes_have_equal_effective_hst_values(self) -> None:
        legacy = self.run_manager("BuildFull")
        self.assert_manager_success(legacy)
        manifest = self.run_manager("BuildFull", MANIFEST)
        self.assert_manager_success(manifest)
        alls = [record for record in self.records() if record["target"] == "all"]
        self.assertEqual(len(alls), 2)
        first, second = alls
        for field in (
            "game_name", "game_elf", "base", "entry", "extra", "header", "profile",
            "build_dir", "funcs", "runtime", "recomp", "sdk",
        ):
            self.assertEqual(first[field], second[field], field)
        self.assertEqual(first["hst_extra_spans"] or "0x00303194,0x00306e24", "0x00303194,0x00306e24")
        self.assertEqual(second["hst_extra_spans"], "0x00303194,0x00306e24")
        planner = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "title_codegen_plan.py"),
                str(MANIFEST),
                "--manager-plan",
                "--game-name=hst",
                "--game-elf=place_game_here/EBOOT.elf",
                "--build-dir=build/hst",
                "--module-dir=place_game_here/EXTRACTED/decrypted",
                "--psp-header=place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        direct_plan = json.loads(planner.stdout)
        self.assertEqual(second["game_name"], direct_plan["make"]["game_name"])
        self.assertEqual(second["base"], direct_plan["make"]["game_base"])
        self.assertEqual(second["entry"], direct_plan["make"]["game_entry"])
        self.assertEqual(second["profile"], direct_plan["make"]["codegen_profile_arg"])
        self.assertEqual(second["build_dir"], direct_plan["make"]["build_dir"])
        self.assertEqual(second["funcs"], str(direct_plan["make"]["funcs_per_chunk"]))
        self.assertEqual(second["hst_extra_spans"], direct_plan["environment"]["HST_EXTRA_SPANS"])
        self.assertIn("Using opt-in title manifest", manifest.stdout)

    def test_fast_and_test_routes_preserve_legacy_action_and_effective_values(self) -> None:
        # `all` reaches analyze.py and receives the scoped span; `selftest` does not run the
        # analyzer, so manifest mode leaves it exactly as legacy mode does - unset.
        for action, target, spans in (
            ("BuildFast", "all", self.SPAN),
            ("Test", "selftest", None),
        ):
            with self.subTest(action=action):
                legacy = self.run_manager(action)
                self.assert_manager_success(legacy)
                manifest = self.run_manager(action, MANIFEST)
                self.assert_manager_success(manifest)
                records = [record for record in self.records() if record["target"] == target]
                self.assertEqual(len(records), 2)
                first, second = records
                for field in (
                    "target", "game_name", "game_elf", "base", "entry", "extra", "header",
                    "profile", "build_dir", "funcs", "runtime", "recomp", "sdk",
                ):
                    self.assertEqual(first[field], second[field], field)
                self.assertIsNone(first["hst_extra_spans"])
                self.assertEqual(second["hst_extra_spans"], spans)

    def test_fuzz_receives_the_scoped_span_because_it_runs_the_pipeline(self) -> None:
        # `make vfpu_fuzz` invokes `$(MAKE) pipeline`, so it does reach analyze.py.
        legacy = self.run_manager("Fuzz")
        self.assert_manager_success(legacy)
        manifest = self.run_manager("Fuzz", MANIFEST)
        self.assert_manager_success(manifest)
        records = [record for record in self.records() if record["target"] == "vfpu_fuzz"]
        self.assertEqual(len(records), 2)
        self.assertIsNone(records[0]["hst_extra_spans"])
        self.assertEqual(records[1]["hst_extra_spans"], self.SPAN)

    def test_explicit_make_path_is_required_to_be_a_real_executable(self) -> None:
        missing = self.root / "does-not-exist-make"
        proc = self.run_manager("BuildFast", MakeExecutable=missing)
        self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("could not resolve make", (proc.stdout + proc.stderr).lower())
        self.assert_no_make()

    def test_operational_overrides_win_without_mutating_manifest(self) -> None:
        before = MANIFEST.read_bytes()
        proc = self.run_manager(
            "BuildFast",
            MANIFEST,
            RuntimeOpt="O2",
            RecompOpt="O1",
            FuncsPerChunk=64,
        )
        self.assert_manager_success(proc)
        record = [item for item in self.records() if item["target"] == "all"][-1]
        self.assertEqual(record["runtime"], "-O2")
        self.assertEqual(record["recomp"], "-O1")
        self.assertEqual(record["funcs"], "64")
        self.assertEqual(MANIFEST.read_bytes(), before)

    def test_manifest_validation_and_private_binding_fail_before_make(self) -> None:
        cases = [
            ("missing.json", None),
            ("malformed.json", "malformed"),
            ("duplicate.json", "duplicate"),
            ("unknown-field.json", "unknown-field"),
            ("bad-schema.json", lambda value: value.update(schema_version=2)),
            ("bad-identity.json", lambda value: value.update(id="other-title-v1")),
            ("bad-base.json", lambda value: value["executable"].update(base=1)),
            ("bad-entry.json", lambda value: value["executable"].update(entry=1)),
            ("bad-profile.json", lambda value: value.update(codegen_profile="none")),
            ("bad-bss.json", lambda value: value["executable"].update(bss_metadata_source="elf")),
            ("bad-span.json", lambda value: value["executable"].update(
                extra_executable_spans=[{"start": 1, "end": 2}]
            )),
            ("bad-disc.json", lambda value: value["disc"].update(id="UCUS98702")),
            ("bad-module.json", lambda value: value["modules"][0].update(name="other.prx")),
            ("bad-address.json", lambda value: value["modules"][0].update(load_address=1)),
        ]
        for name, mutation in cases:
            with self.subTest(name=name):
                if mutation == "malformed":
                    path = self.root / name
                    path.write_text("{", encoding="ascii")
                elif mutation == "duplicate":
                    path = self.root / name
                    path.write_text('{"schema_version":1,"schema_version":1}', encoding="ascii")
                elif mutation == "unknown-field":
                    path = self.root / name
                    path.write_text('{"schema_version":1,"unexpected":true}', encoding="ascii")
                elif mutation is None:
                    path = self.root / name
                else:
                    path = self.write_manifest(name, mutation)
                proc = self.run_manager("BuildFast", path)
                self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assert_no_make()

        missing_elf = self.root / "place_game_here" / "EBOOT.elf"
        missing_elf.unlink()
        proc = self.run_manager("BuildFast", MANIFEST)
        self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("missing required private binding", (proc.stdout + proc.stderr).lower())
        self.assert_no_make()

    def test_manager_plan_parser_rejects_invalid_json_version_unknown_fields_and_controls(self) -> None:
        plan_proc = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "title_codegen_plan.py"),
                str(MANIFEST),
                "--manager-plan",
                "--game-name=hst",
                "--game-elf=place_game_here/EBOOT.elf",
                "--build-dir=build/hst",
                "--module-dir=place_game_here/EXTRACTED/decrypted",
                "--psp-header=place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        base = json.loads(plan_proc.stdout)

        def assert_helper_rejects(value: str) -> None:
            path = self.root / "plan.json"
            path.write_text(value, encoding="utf-8")
            command = (
                f". '{HELPER}'; "
                f"$p=Get-Content -LiteralPath '{path}' -Raw; "
                "try { $o=$p | ConvertFrom-Json; Assert-TitleManagerPlan $o | Out-Null; exit 1 } "
                "catch { exit 0 }"
            )
            proc = subprocess.run(
                [self.shell, "-NoProfile", "-Command", command],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

        assert_helper_rejects("not-json")
        version = copy.deepcopy(base)
        version["plan_version"] = 99
        assert_helper_rejects(json.dumps(version))
        unknown = copy.deepcopy(base)
        unknown["unexpected"] = True
        assert_helper_rejects(json.dumps(unknown))
        control = copy.deepcopy(base)
        control["make"]["build_dir"] = "bad\npath"
        assert_helper_rejects(json.dumps(control))

    def test_nonzero_python_planner_is_rejected_without_make(self) -> None:
        failing = self.root / "failing-python.cmd"
        failing.write_text("@echo planner failed 1>&2\r\n@exit /b 7\r\n", encoding="ascii")
        command = (
            f". '{HELPER}'; "
            f"try {{ Invoke-TitleManagerPlan -PlannerScript 'missing.py' -ManifestPath '{MANIFEST}' "
            f"-GameName hst -GameElf eboot.elf -BuildDir build/hst -ModuleDir modules "
            f"-PspHeader header.bin -FuncsPerChunk 2000 -PythonCommand '{failing}'; exit 1 }} "
            "catch { exit 0 }"
        )
        proc = subprocess.run(
            [self.shell, "-NoProfile", "-Command", command],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assert_no_make()

    def test_vulkan_sdk_helper_precedence_and_validity(self) -> None:
        explicit = self.make_sdk("explicit-sdk")
        environment = self.make_sdk("environment-sdk")
        self.make_sdk("1.10.0.0", complete=False)
        scanned = self.make_sdk("1.9.0.0")
        self.make_sdk("not-a-version")

        explicit_result = self.resolve_sdk(explicit=explicit, environment=environment)
        self.assertEqual(explicit_result.returncode, 0, explicit_result.stdout + explicit_result.stderr)
        self.assertEqual(Path(explicit_result.stdout.strip()), explicit.resolve())

        environment_result = self.resolve_sdk(environment=environment)
        self.assertEqual(environment_result.returncode, 0, environment_result.stdout + environment_result.stderr)
        self.assertEqual(Path(environment_result.stdout.strip()), environment.resolve())

        scan_result = self.resolve_sdk()
        self.assertEqual(scan_result.returncode, 0, scan_result.stdout + scan_result.stderr)
        self.assertEqual(Path(scan_result.stdout.strip()), scanned.resolve())

        invalid_result = self.resolve_sdk(environment=self.root / "missing-sdk")
        self.assertNotEqual(invalid_result.returncode, 0)
        self.assertIn("unusable Vulkan SDK", invalid_result.stderr)


if __name__ == "__main__":
    unittest.main()
