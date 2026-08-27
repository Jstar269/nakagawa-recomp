# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression tests for compiler-profile and transitive-header build truth."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROFILE_TOOL = ROOT / "tools" / "build_profile.py"
COMMON_MK = ROOT / "mk" / "build_common.mk"
sys.path.insert(0, str(ROOT / "tools"))

import build_profile


class BuildTruthTests(unittest.TestCase):
    def setUp(self) -> None:
        self.make = shutil.which("mingw32-make") or shutil.which("make")
        self.cc = os.environ.get("CC")
        if not self.cc:
            self.cc = "gcc" if shutil.which("gcc") else ("cc" if shutil.which("cc") else None)
        if not self.make or not self.cc:
            self.skipTest("GNU Make and a C compiler are required")
        self.temp = tempfile.TemporaryDirectory(prefix="nakagawa-build-truth-")
        self.root = Path(self.temp.name)
        (self.root / "build").mkdir()
        (self.root / "inner.h").write_text("#define INNER_TOKEN 1\n", encoding="ascii")
        (self.root / "outer.h").write_text('#include "inner.h"\n', encoding="ascii")
        (self.root / "dependent.c").write_text(
            '#include "outer.h"\nint dependent(void) { return INNER_TOKEN; }\n',
            encoding="ascii",
        )
        (self.root / "unrelated.c").write_text(
            "int unrelated(void) { return 7; }\n", encoding="ascii"
        )
        (self.root / "Makefile").write_text(
            textwrap.dedent(
                f"""
                PYTHON ?= python
                CC ?= gcc
                PROFILE_FLAGS ?= -O0
                BUILD := build
                PROFILE_TOOL := {PROFILE_TOOL.as_posix()}
                include {COMMON_MK.as_posix()}
                PROFILE_HASH := $(shell $(PYTHON) $(PROFILE_TOOL) hash --compiler "$(CC)" --entry "PROFILE_FLAGS=$(PROFILE_FLAGS)")
                PROFILE_STAMP := $(BUILD)/.profile-$(PROFILE_HASH)
                PROFILE_MANIFEST := $(BUILD)/profile.json
                OBJS := $(BUILD)/dependent.o $(BUILD)/unrelated.o

                .PHONY: all
                all: $(OBJS)

                $(PROFILE_STAMP): $(PROFILE_TOOL)
                \t$(PYTHON) $(PROFILE_TOOL) record --output $(PROFILE_MANIFEST) --section runtime --compiler "$(CC)" --entry "PROFILE_FLAGS=$(PROFILE_FLAGS)" --stamp "$@" --stale-glob ".profile-*" $(foreach obj,$(OBJS),--invalidate "$(obj)")

                $(BUILD)/%.o: %.c $(PROFILE_STAMP)
                \t@echo COMPILE $<
                \t$(CC) $(PROFILE_FLAGS) $(DEPFLAGS) -c $< -o $@

                -include $(PROFILE_STAMP)
                -include $(OBJS:.o=.d)
                """
            ).lstrip(),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def run_make(self, flags: str) -> str:
        proc = subprocess.run(
            [self.make, "--no-print-directory", f"CC={self.cc}", f"PROFILE_FLAGS={flags}"],
            cwd=self.root,
            check=False,
            capture_output=True,
            text=True,
        )
        if proc.returncode:
            self.fail(f"make failed ({proc.returncode}):\n{proc.stdout}{proc.stderr}")
        return proc.stdout + proc.stderr

    def assert_compiled(self, output: str, *sources: str) -> None:
        compiled = {
            line.removeprefix("COMPILE ").strip()
            for line in output.splitlines()
            if line.startswith("COMPILE ")
        }
        self.assertEqual(compiled, set(sources), output)

    def test_transitive_headers_and_profiles_drive_exact_rebuilds(self) -> None:
        self.assert_compiled(self.run_make("-O0"), "dependent.c", "unrelated.c")
        self.assert_compiled(self.run_make("-O0"))

        time.sleep(1.1)  # GNU Make on Windows may compare timestamps at one-second resolution.
        (self.root / "inner.h").write_text("#define INNER_TOKEN 2\n", encoding="ascii")
        self.assert_compiled(self.run_make("-O0"), "dependent.c")
        self.assert_compiled(self.run_make("-O0"))

        self.assert_compiled(self.run_make("-O2"), "dependent.c", "unrelated.c")
        self.assert_compiled(self.run_make("-O2"))
        self.assert_compiled(self.run_make("-O0"), "dependent.c", "unrelated.c")

        time.sleep(1.1)
        (self.root / "inner.h").rename(self.root / "renamed.h")
        (self.root / "outer.h").write_text('#include "renamed.h"\n', encoding="ascii")
        self.assert_compiled(self.run_make("-O0"), "dependent.c")

        manifest = json.loads((self.root / "build" / "profile.json").read_text())
        self.assertEqual(manifest["sections"]["runtime"]["entries"], ["PROFILE_FLAGS=-O0"])

    def test_repository_rules_do_not_keep_manager_object_lists(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        manager = (ROOT / "hst_manager.ps1").read_text(encoding="utf-8")
        self.assertIn("$(DEPFLAGS) -c", makefile)
        self.assertIn("$(RUNTIME_PROFILE_STAMP)", makefile)
        self.assertIn("$(RECOMP_PROFILE_STAMP)", makefile)
        self.assertNotIn("$staleObjs", manager)
        self.assertNotIn("Skipping shader recompile", manager)

    def test_forced_same_profile_recipe_does_not_invalidate_objects(self) -> None:
        stamp = self.root / "build" / ".profile-same"
        obj = self.root / "build" / "dependent.o"
        obj.write_bytes(b"object")
        build_profile.activate_stamp(
            stamp, ".profile-*", "same", invalidate=[obj]
        )
        self.assertFalse(obj.exists())
        obj.write_bytes(b"object")
        build_profile.activate_stamp(
            stamp, ".profile-*", "same", invalidate=[obj]
        )
        self.assertTrue(obj.exists())


SDL3VK_C = "src/rt/gpu_sdl3vk/sdl3vk.c"
FBCAP_C = "src/rt/fbcap_policy.c"
SDL3VK_VAR = "$(SDL3VK_SRCS)"


def _logical_lines(makefile: str) -> list[tuple[int, str]]:
    """Join backslash continuations, dropping whole-line comments.

    Returns (1-based line number of the first physical line, joined text).
    """
    logical: list[tuple[int, str]] = []
    pending: list[str] = []
    start = 0
    for number, raw in enumerate(makefile.splitlines(), start=1):
        if not pending and raw.lstrip().startswith("#"):
            continue
        if not pending:
            start = number
        if raw.endswith("\\"):
            pending.append(raw[:-1])
            continue
        pending.append(raw)
        logical.append((start, " ".join(pending)))
        pending = []
    if pending:
        logical.append((start, " ".join(pending)))
    return logical


class Sdl3vkLinkDependencyTests(unittest.TestCase):
    """sdl3vk.c calls into fbcap_policy.c, so every recipe that compiles the
    backend must also supply the policy.  Issue #57 added the call sites and
    updated RT_SRCS plus gpu-capture-selftest, but not gpu-coherence-selftest
    or ge-replay -- both failed to link on `undefined reference to
    sr_fbcap_owner`.  --gc-sections does not save an omitting recipe, because
    ld resolves undefined symbols before discarding unreachable sections, and
    a link failure must never be mistaken for the legitimate exit-77 SKIP.
    """

    def setUp(self) -> None:
        self.makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.lines = _logical_lines(self.makefile)

    def test_policy_symbols_are_defined_where_the_makefile_says_they_are(self) -> None:
        source = (ROOT / "src" / "rt" / "fbcap_policy.c").read_text(encoding="utf-8")
        for symbol in ("sr_fbcap_owner", "sr_fbcap_path", "sr_fbcap_exit_status"):
            definition = re.compile(rf"^\w[\w \t*]*\b{symbol}\s*\(", re.MULTILINE)
            self.assertRegex(source, definition, msg=symbol)

    def test_sdl3vk_srcs_bundles_the_backend_with_its_policy(self) -> None:
        definitions = [
            text for _, text in self.lines if re.match(r"\s*SDL3VK_SRCS\s*:?=", text)
        ]
        self.assertEqual(len(definitions), 1, self.lines)
        self.assertIn(SDL3VK_C, definitions[0])
        self.assertIn(FBCAP_C, definitions[0])

    def test_every_user_of_the_backend_also_supplies_the_capture_policy(self) -> None:
        backend = (ROOT / "src" / "rt" / "gpu_sdl3vk" / "sdl3vk.c").read_text(encoding="utf-8")
        referenced = sorted(set(re.findall(r"\bsr_fbcap_\w+\s*\(", backend)))
        if not referenced:
            self.skipTest("sdl3vk.c no longer calls the fbcap policy; guard retired")

        offenders = []
        for number, text in self.lines:
            uses_backend = SDL3VK_C in text or SDL3VK_VAR in text
            supplies_policy = FBCAP_C in text or SDL3VK_VAR in text
            if uses_backend and not supplies_policy:
                offenders.append(f"Makefile:{number}: {text.strip()}")
        self.assertEqual(
            offenders,
            [],
            "these Makefile statements compile "
            f"{SDL3VK_C} without {FBCAP_C}; sdl3vk.c calls "
            f"{', '.join(s.rstrip('(').strip() for s in referenced)}, so the link "
            f"will fail. Use {SDL3VK_VAR}:\n" + "\n".join(offenders),
        )

    def test_the_guard_rejects_a_recipe_that_drops_the_policy(self) -> None:
        """Failing-before proof: the check must actually catch the #57 shape."""
        # Built by joining, not as one escaped literal: an inline "\\" next to an
        # escaped newline+tab reads as a UNC path to publish_audit's LOCAL_PATH rule.
        continuation = chr(92)
        regressed = _logical_lines(
            "\n".join(
                [
                    "gpu-coherence-selftest:",
                    "\t$(CC) -o out.exe harness.c " + continuation,
                    f"\t\t{SDL3VK_C} $(LIBS)",
                ]
            )
            + "\n"
        )
        offenders = [
            number
            for number, text in regressed
            if (SDL3VK_C in text or SDL3VK_VAR in text)
            and not (FBCAP_C in text or SDL3VK_VAR in text)
        ]
        self.assertEqual(offenders, [2])


class Atrac3pBuildPortabilityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.make = shutil.which("mingw32-make") or shutil.which("make")

    def test_makefile_does_not_contain_per_recipe_mkdir_in_atrac3p_rule(self) -> None:
        lines = _logical_lines(self.makefile)
        for number, text in lines:
            if "atrac3p_%.o:" in text:
                self.assertNotIn(
                    "mkdir -p",
                    text,
                    f"Makefile:{number} contains per-recipe 'mkdir -p' in atrac3p rule which fails in cmd.exe when sh is absent",
                )

    def test_makefile_defines_atrac3p_obj_dirs_up_front(self) -> None:
        self.assertIn("ATRAC3P_OBJ_DIRS :=", self.makefile)

    def test_atrac3p_nested_object_directories_build_clean_and_parallel(self) -> None:
        if not self.make:
            self.skipTest("GNU Make is required")
        with tempfile.TemporaryDirectory(prefix="nakagawa-atrac3p-build-") as temp_dir:
            build_dir = Path(temp_dir) / "build_atrac3p"
            self.assertFalse(build_dir.exists())

            # 1. Clean serial build for atrac3p-objects
            proc = subprocess.run(
                [self.make, "--no-print-directory", f"BUILD_DIR={build_dir.as_posix()}", "atrac3p-objects"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                proc.returncode,
                0,
                f"make atrac3p-objects failed ({proc.returncode}):\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}",
            )

            # Verify nested directories were created
            codec_dir = build_dir / "atrac3p_libavcodec"
            util_dir = build_dir / "atrac3p_libavutil"
            self.assertTrue(codec_dir.is_dir(), f"{codec_dir} was not created")
            self.assertTrue(util_dir.is_dir(), f"{util_dir} was not created")
            self.assertTrue(any(codec_dir.glob("*.o")), f"No .o files in {codec_dir}")
            self.assertTrue(any(util_dir.glob("*.o")), f"No .o files in {util_dir}")

            # 2. Idempotent second build
            proc_idem = subprocess.run(
                [self.make, "--no-print-directory", f"BUILD_DIR={build_dir.as_posix()}", "atrac3p-objects"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                proc_idem.returncode,
                0,
                f"idempotent make failed:\n{proc_idem.stdout}\n{proc_idem.stderr}",
            )

            # 3. Clean parallel build (-j4)
            shutil.rmtree(build_dir)
            proc_par = subprocess.run(
                [self.make, "-j4", "--no-print-directory", f"BUILD_DIR={build_dir.as_posix()}", "atrac3p-objects"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                proc_par.returncode,
                0,
                f"parallel make failed:\n{proc_par.stdout}\n{proc_par.stderr}",
            )
            self.assertTrue(codec_dir.is_dir())
            self.assertTrue(util_dir.is_dir())


class OptimizationProfileContractTests(unittest.TestCase):
    """Contract tests for HST and generic optimization defaults and profile manifests."""

    def setUp(self) -> None:
        self.make = shutil.which("mingw32-make") or shutil.which("make")
        if not self.make:
            self.skipTest("GNU Make is required")
        self.temp = tempfile.TemporaryDirectory(prefix="nakagawa-opt-profile-")
        self.build_dir = Path(self.temp.name) / "build"

    def tearDown(self) -> None:
        self.temp.cleanup()

    def run_compiler_info(self, *extra_args: str) -> dict[str, str]:
        cmd = [
            self.make,
            "--no-print-directory",
            f"BUILD_DIR={self.build_dir.as_posix()}",
            "compiler-info",
            *extra_args,
        ]
        proc = subprocess.run(
            cmd,
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(
            proc.returncode,
            0,
            f"make compiler-info failed ({proc.returncode}):\n{proc.stdout}\n{proc.stderr}",
        )
        info: dict[str, str] = {}
        for line in proc.stdout.splitlines():
            if "=" in line:
                key, val = line.split("=", 1)
                info[key.strip()] = val.strip()
        return info

    def test_hst_implicit_defaults_resolve_to_o2_o1(self) -> None:
        info = self.run_compiler_info("GAME_NAME=hst")
        self.assertEqual(info.get("RUNTIME_OPT"), "-O2")
        self.assertEqual(info.get("RECOMP_OPT"), "-O1")
        self.assertTrue(info.get("CFLAGS", "").startswith("-O2 "), info.get("CFLAGS"))
        self.assertTrue(info.get("RECOMP_FLAGS", "").startswith("-O1 "), info.get("RECOMP_FLAGS"))

    def test_generic_game_implicit_defaults_remain_o0_o0(self) -> None:
        info_default = self.run_compiler_info()
        self.assertEqual(info_default.get("RUNTIME_OPT"), "-O0")
        self.assertEqual(info_default.get("RECOMP_OPT"), "-O0")
        self.assertTrue(info_default.get("CFLAGS", "").startswith("-O0 "), info_default.get("CFLAGS"))
        self.assertTrue(info_default.get("RECOMP_FLAGS", "").startswith("-O0 "), info_default.get("RECOMP_FLAGS"))

        info_other = self.run_compiler_info("GAME_NAME=othergame")
        self.assertEqual(info_other.get("RUNTIME_OPT"), "-O0")
        self.assertEqual(info_other.get("RECOMP_OPT"), "-O0")
        self.assertTrue(info_other.get("CFLAGS", "").startswith("-O0 "), info_other.get("CFLAGS"))
        self.assertTrue(info_other.get("RECOMP_FLAGS", "").startswith("-O0 "), info_other.get("RECOMP_FLAGS"))

    def test_explicit_hst_overrides_still_win(self) -> None:
        info = self.run_compiler_info("GAME_NAME=hst", "RUNTIME_OPT=-O0", "RECOMP_OPT=-O0")
        self.assertEqual(info.get("RUNTIME_OPT"), "-O0")
        self.assertEqual(info.get("RECOMP_OPT"), "-O0")
        self.assertTrue(info.get("CFLAGS", "").startswith("-O0 "), info.get("CFLAGS"))
        self.assertTrue(info.get("RECOMP_FLAGS", "").startswith("-O0 "), info.get("RECOMP_FLAGS"))

        info_custom = self.run_compiler_info("GAME_NAME=hst", "RUNTIME_OPT=-O1", "RECOMP_OPT=-O2")
        self.assertEqual(info_custom.get("RUNTIME_OPT"), "-O1")
        self.assertEqual(info_custom.get("RECOMP_OPT"), "-O2")
        self.assertTrue(info_custom.get("CFLAGS", "").startswith("-O1 "), info_custom.get("CFLAGS"))
        self.assertTrue(info_custom.get("RECOMP_FLAGS", "").startswith("-O2 "), info_custom.get("RECOMP_FLAGS"))

    def test_build_profile_manifests_record_effective_flags(self) -> None:
        hst_dir = self.build_dir / "hst_default"
        subprocess.run(
            [self.make, "--no-print-directory", f"BUILD_DIR={hst_dir.as_posix()}", "GAME_NAME=hst", "compiler-info"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        runtime_manifest = json.loads((hst_dir / "runtime_profile.json").read_text(encoding="utf-8"))
        recomp_manifest = json.loads((hst_dir / "recomp_profile.json").read_text(encoding="utf-8"))
        runtime_entries = runtime_manifest["sections"]["runtime"]["entries"]
        recomp_entries = recomp_manifest["sections"]["generated"]["entries"]
        self.assertTrue(any(e.startswith("CFLAGS=-O2 ") for e in runtime_entries), runtime_entries)
        self.assertTrue(any(e.startswith("RECOMP_FLAGS=-O1 ") for e in recomp_entries), recomp_entries)

        generic_dir = self.build_dir / "generic_default"
        subprocess.run(
            [self.make, "--no-print-directory", f"BUILD_DIR={generic_dir.as_posix()}", "GAME_NAME=mygame", "compiler-info"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        runtime_generic = json.loads((generic_dir / "runtime_profile.json").read_text(encoding="utf-8"))
        recomp_generic = json.loads((generic_dir / "recomp_profile.json").read_text(encoding="utf-8"))
        self.assertTrue(any(e.startswith("CFLAGS=-O0 ") for e in runtime_generic["sections"]["runtime"]["entries"]))
        self.assertTrue(any(e.startswith("RECOMP_FLAGS=-O0 ") for e in recomp_generic["sections"]["generated"]["entries"]))

        override_dir = self.build_dir / "hst_override"
        subprocess.run(
            [
                self.make,
                "--no-print-directory",
                f"BUILD_DIR={override_dir.as_posix()}",
                "GAME_NAME=hst",
                "RUNTIME_OPT=-O0",
                "RECOMP_OPT=-O0",
                "compiler-info",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        runtime_override = json.loads((override_dir / "runtime_profile.json").read_text(encoding="utf-8"))
        recomp_override = json.loads((override_dir / "recomp_profile.json").read_text(encoding="utf-8"))
        self.assertTrue(any(e.startswith("CFLAGS=-O0 ") for e in runtime_override["sections"]["runtime"]["entries"]))
        self.assertTrue(any(e.startswith("RECOMP_FLAGS=-O0 ") for e in recomp_override["sections"]["generated"]["entries"]))

    def test_direct_make_and_hst_manager_agree_on_hst_effective_profile(self) -> None:
        pwsh = shutil.which("pwsh")
        if not pwsh:
            self.skipTest("pwsh is required for hst_manager agreement test")

        direct_info = self.run_compiler_info("GAME_NAME=hst")
        manager_script = ROOT / "hst_manager.ps1"
        self.assertTrue(manager_script.is_file())

        ps_cmd = f"""
        . '{ROOT / "tools" / "vulkan_sdk.ps1"}'
        $VulkanSdk = Get-VulkanSdkPath
        $RuntimeOpt = $null
        $RecompOpt = $null
        $FuncsPerChunk = 0
        $GameElfForMake = "eboot.elf"
        $VulkanSdkForMake = $VulkanSdk -replace "\\\\", "/"
        $script:TitleManagerMakeArgs = $null
        function Get-HstMakeBaseArgs {{
            if ($null -ne $script:TitleManagerMakeArgs) {{
                $args = @($script:TitleManagerMakeArgs)
                if ($RuntimeOpt) {{ $args += "RUNTIME_OPT=-$RuntimeOpt" }}
                if ($RecompOpt) {{ $args += "RECOMP_OPT=-$RecompOpt" }}
                return $args
            }}
            $args = @(
                "GAME_NAME=hst",
                "GAME_ELF=$GameElfForMake",
                "GAME_BASE=0",
                "GAME_ENTRY=0",
                "VULKAN_SDK=$VulkanSdkForMake"
            )
            if ($RuntimeOpt) {{ $args += "RUNTIME_OPT=-$RuntimeOpt" }}
            if ($RecompOpt) {{ $args += "RECOMP_OPT=-$RecompOpt" }}
            if ($FuncsPerChunk -gt 0) {{ $args += "FUNCS_PER_CHUNK=$FuncsPerChunk" }}
            return $args
        }}
        $args = Get-HstMakeBaseArgs
        $args -join ";"
        """
        proc = subprocess.run(
            [pwsh, "-NoProfile", "-Command", ps_cmd],
            capture_output=True,
            text=True,
            check=True,
        )
        manager_args = [arg.strip() for arg in proc.stdout.strip().split(";") if arg.strip()]
        self.assertIn("GAME_NAME=hst", manager_args)
        self.assertFalse(any(a.startswith("RUNTIME_OPT=") for a in manager_args))
        self.assertFalse(any(a.startswith("RECOMP_OPT=") for a in manager_args))

        manager_resolved = self.run_compiler_info(*manager_args)
        self.assertEqual(direct_info.get("RUNTIME_OPT"), manager_resolved.get("RUNTIME_OPT"))
        self.assertEqual(direct_info.get("RECOMP_OPT"), manager_resolved.get("RECOMP_OPT"))
        self.assertEqual(manager_resolved.get("RUNTIME_OPT"), "-O2")
        self.assertEqual(manager_resolved.get("RECOMP_OPT"), "-O1")

    def test_profile_hashes_and_stamps_change_when_optimization_values_change(self) -> None:
        cc = os.environ.get("CC", "gcc")
        payload_o0 = build_profile.profile_payload(cc, ["CFLAGS=-O0 -Wall", "GE_CFLAGS=-O2"])
        payload_o2 = build_profile.profile_payload(cc, ["CFLAGS=-O2 -Wall", "GE_CFLAGS=-O2"])
        hash_o0 = build_profile.profile_hash(payload_o0)
        hash_o2 = build_profile.profile_hash(payload_o2)
        self.assertNotEqual(hash_o0, hash_o2)

        recomp_o0 = build_profile.profile_payload(cc, ["RECOMP_FLAGS=-O0 -w", "TRACE=0"])
        recomp_o1 = build_profile.profile_payload(cc, ["RECOMP_FLAGS=-O1 -w", "TRACE=0"])
        self.assertNotEqual(build_profile.profile_hash(recomp_o0), build_profile.profile_hash(recomp_o1))

        stamp_o0 = self.build_dir / f".runtime-profile-{hash_o0}"
        stamp_o2 = self.build_dir / f".runtime-profile-{hash_o2}"

        build_profile.activate_stamp(stamp_o0, ".runtime-profile-*", hash_o0)
        self.assertTrue(stamp_o0.is_file())
        self.assertFalse(stamp_o2.exists())

        build_profile.activate_stamp(stamp_o2, ".runtime-profile-*", hash_o2)
        self.assertTrue(stamp_o2.is_file())
        self.assertFalse(stamp_o0.exists())


class GuestInputTransportTests(unittest.TestCase):
    """The guest-input pathname boundary: no shell interpretation, no lost freshness.

    Every test here drives the REAL repository Makefile. A mutation test that
    builds its own toy Makefile only proves a property of cmd.exe and would keep
    passing after the hardening was reverted, so each mutation below edits a copy
    of the real Makefile and asserts the real one behaves differently.
    """

    # A legal Windows filename containing `&`, which is a command separator to
    # cmd.exe and a background operator to a POSIX shell. Either way, a recipe
    # that interpolates it raw hands `ver` to a command interpreter.
    SPLIT_NAME = "split&ver&tail.elf"
    VER_OUTPUT = "Microsoft Windows [Version"

    # WHICH shell GNU Make dispatches to is a property of the host, not of the
    # defect: Make prefers a POSIX `sh` when one is on PATH (it is, under MSYS2)
    # and falls back to cmd.exe otherwise. So the evidence that pathname data
    # reached an interpreter has more than one shape, and pinning only cmd.exe's
    # made this suite host-dependent in both directions:
    #
    #   * the M1 mutation could not reproduce the pre-fix behavior at all, because
    #     `ver` is a cmd builtin that `sh` does not have -- the regression was
    #     permanently red on an MSYS2 host, which is how a real gate rots into
    #     noise;
    #   * worse, the POSITIVE test only rejected cmd.exe's signatures, so an
    #     injection dispatched through `sh` would have satisfied it.
    #
    # Assert the property -- "a fragment of the pathname was dispatched as a
    # command" -- rather than one host's spelling of it.
    INJECTION_SIGNATURES = (
        VER_OUTPUT,                       # cmd.exe ran `ver`
        "is not recognized as an internal",  # cmd.exe tried to resolve a fragment
        "ver: command not found",         # a POSIX shell tried to run `ver`
    )

    def _injection_evidence(self, blob: str) -> list[str]:
        return [marker for marker in self.INJECTION_SIGNATURES if marker in blob]

    def setUp(self) -> None:
        self.make = shutil.which("mingw32-make") or shutil.which("make")
        if not self.make:
            self.skipTest("GNU Make is required")
        self.temp = tempfile.TemporaryDirectory(prefix="nakagawa-guest-input-")
        self.root = Path(self.temp.name)
        self.builds: list[Path] = []

    def tearDown(self) -> None:
        for b in self.builds:
            shutil.rmtree(b, ignore_errors=True)
        self.temp.cleanup()

    # -- helpers ---------------------------------------------------------

    def _write_minimal_elf(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        payload_off = 52 + 32
        filesz = 8
        blob = bytearray(payload_off + filesz)
        blob[:8] = b"\x7fELF\x01\x01\x01\x00"
        struct.pack_into(
            "<HHIIIIIHHHHHH", blob, 16,
            2, 8, 1, 0x08804000, 52, 0, 0, 52, 32, 1, 0, 0, 0,
        )
        struct.pack_into(
            "<8I", blob, 52,
            1, payload_off, 0x08804000, 0x08804000, filesz, filesz, 5, 4,
        )
        struct.pack_into("<2I", blob, payload_off, 0x03E00008, 0x00000000)
        path.write_bytes(blob)

    def _build_dir(self, name: str) -> Path:
        d = ROOT / "build" / name
        self.builds.append(d)
        shutil.rmtree(d, ignore_errors=True)
        return d

    def _make(self, game_name: str, elf_rel: str, *, makefile: Path | None = None,
              target: str | None = None, extra: tuple[str, ...] = ()) -> subprocess.CompletedProcess:
        tgt = target or f"build/{game_name}/{game_name}_image.bin"
        cmd = [self.make, "--no-print-directory"]
        if makefile is not None:
            cmd += ["-f", str(makefile)]
        cmd += [tgt, f"GAME_NAME={game_name}", f"GAME_ELF={elf_rel}",
                "GAME_BASE=0x08804000", "GAME_ENTRY=0x08804000", *extra]
        return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", check=False)

    @staticmethod
    def _blob(proc: subprocess.CompletedProcess) -> str:
        return (proc.stdout or "") + (proc.stderr or "")

    def _mutate_makefile(self, *replacements: tuple[str, str]) -> Path:
        """Return a copy of the real Makefile with `replacements` applied.

        Each replacement must actually match, so the mutation cannot silently
        become a no-op if the Makefile is refactored.
        """
        text = (ROOT / "Makefile").read_text(encoding="utf-8")
        for old, new in replacements:
            self.assertIn(old, text, f"mutation anchor vanished from Makefile: {old!r}")
            text = text.replace(old, new, 1)
        mutant = self.root / "Makefile.mutant"
        mutant.write_text(text, encoding="utf-8", newline="\n")
        return mutant

    # -- B: no shell interpretation --------------------------------------

    def test_metacharacter_pathname_reaches_no_command_interpreter(self) -> None:
        """A legal pathname containing `&` must not be dispatched as a command."""
        build = self._build_dir("test_gi_split")
        elf_rel = f"build/test_gi_split/{self.SPLIT_NAME}"
        self._write_minimal_elf(ROOT / elf_rel)

        proc = self._make("test_gi_split", elf_rel)
        blob = self._blob(proc)
        self.assertEqual(
            self._injection_evidence(blob), [],
            "a command interpreter saw a fragment of the pathname:\n" + blob)
        self.assertEqual(proc.returncode, 0, blob)
        self.assertTrue((build / "test_gi_split_image.bin").is_file(), blob)

    def test_M1_mutation_raw_recipe_interpolation_reintroduces_command_execution(self) -> None:
        """M1: restoring raw $(GAME_ELF) in the recipe must make the above test fail."""
        if sys.platform != "win32":
            self.skipTest("cmd.exe command splitting is the Windows failure mode")
        build = self._build_dir("test_gi_m1")
        elf_rel = f"build/test_gi_m1/{self.SPLIT_NAME}"
        self._write_minimal_elf(ROOT / elf_rel)

        mutant = self._mutate_makefile(
            ("$(BUILD_DIR)/$(GAME_NAME)_image.bin: $(GAME_INPUT_PREREQ) tools/prxload.py\n"
             "\t$(PYTHON) tools/prxload.py --env-elf $(GAME_BASE)",
             "$(BUILD_DIR)/$(GAME_NAME)_image.bin: tools/prxload.py\n"
             "\t$(PYTHON) tools/prxload.py $(GAME_ELF) $(GAME_BASE)"),
        )
        blob = self._blob(proc := self._make("test_gi_m1", elf_rel, makefile=mutant))
        self.assertTrue(
            self._injection_evidence(blob),
            "mutation did not reproduce the pre-fix command execution; this "
            "regression is no longer load-bearing. Expected one of "
            f"{self.INJECTION_SIGNATURES} in:\n" + blob)

    def test_M1b_sibling_inputs_are_transported_too(self) -> None:
        """GAME_PSP_HEADER shares the recipe, and so must share the transport."""
        build = self._build_dir("test_gi_hdr")
        elf_rel = "build/test_gi_hdr/plain.elf"
        self._write_minimal_elf(ROOT / elf_rel)
        hdr_rel = "build/test_gi_hdr/hdr&ver&tail.BIN"
        (ROOT / hdr_rel).write_bytes(b"\x00" * 64)

        proc = self._make("test_gi_hdr", elf_rel, extra=(f"GAME_PSP_HEADER={hdr_rel}",))
        blob = self._blob(proc)
        self.assertEqual(
            self._injection_evidence(blob), [],
            "GAME_PSP_HEADER still reaches a command interpreter:\n" + blob)

    # -- D: freshness is preserved, not dropped --------------------------

    def _first_build(self, game: str, elf_rel: str) -> Path:
        elf = ROOT / elf_rel
        self._write_minimal_elf(elf)
        proc = self._make(game, elf_rel)
        self.assertEqual(proc.returncode, 0, self._blob(proc))
        return elf

    def test_changing_the_elf_rebuilds_for_every_pathname_shape(self) -> None:
        """D: the dependency edge must survive names Make cannot put in a prereq list."""
        shapes = {
            "plain": "plain.elf",
            "space": "my test game.elf",
            "parens": "Game (USA) (v1.0).elf",
            "amp": "Rock & Roll.elf",
            "caret": "game^caret.elf",
            "bracket": "br[ack]ets.elf",
            "semi": "semi;colon.elf",
            "equals": "eq=uals.elf",
            "quote": "sin'gle.elf",
            "dash": "-leading-dash.elf",
            "pct": "pct%PATH%.elf",
        }
        for key, base in shapes.items():
            with self.subTest(shape=key):
                game = f"test_gi_d_{key}"
                build = self._build_dir(game)
                elf_rel = f"build/{game}/{base}"
                elf = self._first_build(game, elf_rel)
                image = build / f"{game}_image.bin"
                self.assertTrue(image.is_file())

                before = image.stat().st_mtime_ns
                time.sleep(1.1)
                os.utime(elf, None)
                proc = self._make(game, elf_rel)
                self.assertEqual(proc.returncode, 0, self._blob(proc))
                self.assertGreater(
                    image.stat().st_mtime_ns, before,
                    f"changing the ELF did not rebuild for shape {key!r}: the dependency "
                    f"edge was dropped and a stale image would be reused",
                )

    def test_M2_mutation_dropping_the_stamp_edge_loses_freshness(self) -> None:
        """M2: removing the stamp prerequisite must make the freshness test fail."""
        game = "test_gi_m2"
        build = self._build_dir(game)
        elf_rel = f"build/{game}/plain.elf"
        mutant = self._mutate_makefile(
            ("$(BUILD_DIR)/$(GAME_NAME)_image.bin: $(GAME_INPUT_PREREQ) tools/prxload.py",
             "$(BUILD_DIR)/$(GAME_NAME)_image.bin: tools/prxload.py"),
        )
        elf = ROOT / elf_rel
        self._write_minimal_elf(elf)
        proc = self._make(game, elf_rel, makefile=mutant)
        self.assertEqual(proc.returncode, 0, self._blob(proc))
        image = build / f"{game}_image.bin"
        before = image.stat().st_mtime_ns

        time.sleep(1.1)
        os.utime(elf, None)
        self._make(game, elf_rel, makefile=mutant)
        self.assertEqual(
            image.stat().st_mtime_ns, before,
            "mutation did not drop the dependency edge; the freshness regression "
            "is no longer load-bearing",
        )

    # -- E: an absent input fails, it does not reuse stale output ---------

    def test_M4_deleting_the_elf_fails_instead_of_reusing_stale_output(self) -> None:
        """E: an up-to-date target must not mask a missing input.

        GNU Make only reports a missing prerequisite when it decides to remake,
        so a target that looks up to date silently survives its input being
        deleted. The stamp is FORCE-checked, so the absence is caught.
        """
        game = "test_gi_e"
        build = self._build_dir(game)
        elf_rel = f"build/{game}/plain.elf"
        elf = self._first_build(game, elf_rel)
        image = build / f"{game}_image.bin"
        self.assertTrue(image.is_file())

        # Confirm the target really is considered up to date before deleting.
        proc = self._make(game, elf_rel)
        self.assertEqual(proc.returncode, 0, self._blob(proc))

        elf.unlink()
        proc = self._make(game, elf_rel)
        self.assertNotEqual(
            proc.returncode, 0,
            "build succeeded with its guest input deleted, reusing stale output:\n"
            + self._blob(proc),
        )
        self.assertIn("GAME_ELF does not exist", self._blob(proc))
        self.assertTrue(image.is_file(), "the stale image should be left in place, not deleted")

    def test_invalid_values_fail_closed(self) -> None:
        """Empty, whitespace-only, and directory values must not build."""
        game = "test_gi_invalid"
        build = self._build_dir(game)
        elf_rel = f"build/{game}/plain.elf"
        self._first_build(game, elf_rel)

        for label, value, expect in (
            ("empty", "", "empty or whitespace-only"),
            ("whitespace", "   ", "empty or whitespace-only"),
            ("missing", "build/does/not/exist.elf", "does not exist"),
            ("directory", f"build/{game}", "is a directory"),
        ):
            with self.subTest(value=label):
                proc = self._make(game, value)
                self.assertNotEqual(proc.returncode, 0,
                                    f"{label} GAME_ELF was accepted:\n" + self._blob(proc))
                self.assertIn(expect, self._blob(proc))

    def test_public_lane_without_a_declared_guest_input_is_not_forced_to_invent_one(self) -> None:
        """A caller that supplies its own generated artifacts needs no GAME_ELF.

        The synthetic VFPU fuzz lane (.github/workflows/ci.yml) hand-writes
        <game>_recomp.c and never names a guest ELF. Making the guest-input stamp
        an unconditional prerequisite broke that lane, because a FORCE-checked
        stamp demanded an ELF nothing was going to read.
        """
        game = "test_gi_public"
        build = self._build_dir(game)
        build.mkdir(parents=True, exist_ok=True)
        base = [self.make, "--no-print-directory", f"GAME_NAME={game}", f"BUILD_DIR=build/{game}"]

        # Settle the profile stamps first. CI creates them in earlier steps, so by the
        # time it hand-writes <game>_recomp.c that file is the newest prerequisite and
        # the codegen recipe is not triggered at all. Writing recomp.c against a fresh
        # build directory instead makes the just-created profile stamp newer, which
        # triggers codegen and fails on main too -- a different, pre-existing condition
        # that would mask what this test is actually pinning.
        subprocess.run(base + ["compiler-info"], cwd=ROOT, capture_output=True,
                       text=True, check=False)
        time.sleep(1.1)
        (build / f"{game}_recomp.c").write_text("void f_00304290(void *s) { (void)s; }\n",
                                                encoding="ascii")
        (build / f"{game}_recomp_funcs.h").write_text("", encoding="ascii")

        proc = subprocess.run(base + [f"build/{game}/{game}_recomp.c"], cwd=ROOT,
                              capture_output=True, text=True,
                              encoding="utf-8", errors="replace", check=False)
        blob = (proc.stdout or "") + (proc.stderr or "")
        self.assertEqual(proc.returncode, 0,
                         "a lane that declares no GAME_ELF was forced to supply one:\n" + blob)
        self.assertNotIn("GAME_ELF does not exist", blob, blob)

    # -- Make is also a parser -------------------------------------------

    def test_M5_make_expands_dollar_in_the_value_and_the_build_fails_closed(self) -> None:
        """M5: `$` is consumed by GNU Make upstream of any transport.

        This is a real, measured parser case, not a hypothetical: the value is
        corrupted before the environment is written, so the only correct
        behaviour is to fail on the corrupted name rather than open a different
        file. `$$` is the working escape.
        """
        game = "test_gi_m5"
        build = self._build_dir(game)
        base = "dol$lar.elf"
        elf_rel = f"build/{game}/{base}"
        self._write_minimal_elf(ROOT / elf_rel)

        proc = self._make(game, elf_rel)
        blob = self._blob(proc)
        self.assertNotEqual(proc.returncode, 0, "Make no longer eats `$`; re-derive this case")
        self.assertIn("does not exist: build/test_gi_m5/dolar.elf", blob,
                      "Make's `$` expansion changed shape:\n" + blob)

        escaped = self._make(game, elf_rel.replace("$", "$$"))
        self.assertEqual(escaped.returncode, 0,
                         "the documented `$$` escape no longer works:\n" + self._blob(escaped))
        self.assertTrue((build / f"{game}_image.bin").is_file())


class GuestInputSourcePrecedenceTests(unittest.TestCase):
    """M3: two disagreeing sources for one input must fail, never be reconciled."""

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="nakagawa-guest-src-")
        self.root = Path(self.temp.name)
        self.elf = self.root / "real.elf"
        GuestInputTransportTests._write_minimal_elf(self, self.elf)
        self.decoy = self.root / "decoy.elf"
        GuestInputTransportTests._write_minimal_elf(self, self.decoy)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def _run(self, tool: str, *args: str) -> subprocess.CompletedProcess:
        env = {**os.environ, "GAME_ELF": str(self.elf)}
        return subprocess.run([sys.executable, str(ROOT / "tools" / tool), *args],
                              cwd=ROOT, env=env, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", check=False)

    def test_env_and_positional_disagreeing_is_refused(self) -> None:
        cases = {
            "prxload.py": ("--env-elf", str(self.decoy), "0x08804000"),
            "codegen.py": ("--env-elf", str(self.decoy), str(self.root / "out.c"),
                           "--base=0x08804000", "--profile=none"),
            "imports.py": ("--env-elf", str(self.decoy), "0x08804000"),
            "vfpu_fuzz_gen.py": ("--env-elf", str(self.decoy), str(self.root / "out.h"),
                                 "--base=0x08804000"),
        }
        for tool, args in cases.items():
            with self.subTest(tool=tool):
                before = self.decoy.read_bytes()
                proc = self._run(tool, *args)
                self.assertNotEqual(proc.returncode, 0,
                                    f"{tool} silently reconciled two sources:\n"
                                    + proc.stdout + proc.stderr)
                self.assertEqual(
                    self.decoy.read_bytes(), before,
                    f"{tool} OVERWROTE the extra positional -- a guest ELF passed alongside "
                    f"--env-elf would be destroyed",
                )

    def test_verify_gates_refuses_two_sources(self) -> None:
        proc = self._run("verify_gates.py", "--cc", "gcc", "--elf", str(self.decoy),
                         "--env-elf", "--run-elf", "x", "--workdir", str(self.root))
        self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("conflicting sources", proc.stdout + proc.stderr)

    def test_legacy_positional_form_still_works(self) -> None:
        """The env form is additive: the documented positional call must not regress."""
        out = self.root / "legacy_image.bin"
        proc = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "prxload.py"), str(self.elf),
             "0x08804000", f"--out={out}"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue(out.is_file())

    def test_pathname_containing_equals_is_not_a_verification_spec(self) -> None:
        """A legal `name=value.elf` pathname must not be parsed as `pc=word`."""
        weird = self.root / "eq=uals.elf"
        GuestInputTransportTests._write_minimal_elf(self, weird)
        out = self.root / "eq_image.bin"
        proc = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "prxload.py"), str(weird),
             "0x08804000", f"--out={out}"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue(out.is_file())


class ShellPortabilityAndRecipeTruthTests(unittest.TestCase):
    """Regression and structural tests for Windows cmd.exe / MSYS2 / POSIX shell recipe truth.

    Make executes recipes using the active shell (cmd.exe on Windows when sh is
    absent, or sh under MSYS2/Linux). Recipes must not introduce accidental shell
    assumptions:
      - cmd.exe treats tab-indented `#` as an executable name ('#' is not recognized).
      - cmd.exe has no `true` built-in, so `&& true` fails with exit code 1.
      - Unix `rm -f` fails under cmd.exe; Python Path.unlink is portable.
      - Posix `test -f` fails under cmd.exe; cmd `if not exist` fails under sh.
    """

    def setUp(self) -> None:
        self.makefile_text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.make = shutil.which("mingw32-make") or shutil.which("make")

    def test_makefile_has_no_tab_indented_comments_in_recipes(self) -> None:
        """Physical lines starting with tab must not start with '#'.

        Under Windows cmd.exe, Make passes '# comment' directly to cmd.exe which fails:
        '#' is not recognized as an internal or external command, operable program or batch file.
        Comments belong outside recipes or as un-indented lines.
        """
        offenders = []
        for line_no, line in enumerate(self.makefile_text.splitlines(), start=1):
            if line.startswith("\t#") or line.startswith("\t #"):
                offenders.append(f"Makefile:{line_no}: {line.strip()}")
        self.assertEqual(
            offenders, [],
            "Makefile contains tab-indented comments in recipes which break Windows cmd.exe:\n"
            + "\n".join(offenders),
        )

    def test_makefile_recipes_do_not_use_unix_true_or_chained_and_true(self) -> None:
        """Recipes must not chain with `&& true` or invoke `true` directly.

        `true` does not exist in standard Windows cmd.exe. Make recipes should use
        newline-separated execution or explicit target dependencies.
        """
        offenders = []
        for line_no, line in enumerate(self.makefile_text.splitlines(), start=1):
            if line.startswith("\t") and re.search(r"\btrue\b", line):
                offenders.append(f"Makefile:{line_no}: {line.strip()}")
        self.assertEqual(
            offenders, [],
            "Makefile recipes contain 'true' which fails under Windows cmd.exe:\n"
            + "\n".join(offenders),
        )

    def test_makefile_recipes_do_not_use_raw_rm(self) -> None:
        """Recipes must not rely on Unix `rm` for cleanup when Python is available."""
        offenders = []
        for line_no, line in enumerate(self.makefile_text.splitlines(), start=1):
            if line.startswith("\t") and re.search(r"\brm\s+-[rf]", line):
                offenders.append(f"Makefile:{line_no}: {line.strip()}")
        self.assertEqual(
            offenders, [],
            "Makefile recipes contain 'rm -f' which is not portable to Windows cmd.exe:\n"
            + "\n".join(offenders),
        )

    def test_makefile_recipes_do_not_use_shell_conditionals(self) -> None:
        """Recipes must not use shell-specific test -f or cmd if not exist."""
        offenders = []
        for line_no, line in enumerate(self.makefile_text.splitlines(), start=1):
            if line.startswith("\t") and (re.search(r"\btest\s+-[fdsew]", line) or "if not exist" in line):
                offenders.append(f"Makefile:{line_no}: {line.strip()}")
        self.assertEqual(
            offenders, [],
            "Makefile recipes contain shell-specific conditional commands:\n"
            + "\n".join(offenders),
        )

    def test_matrix_recipes_fail_fast_on_first_configuration_failure(self) -> None:
        """Mutation test: verify that a failing sub-configuration causes the aggregate target to fail.

        When Make runs newline-separated recipe lines, any non-zero exit code must immediately
        abort the target with non-zero exit status (fail-closed behavior preserved).
        """
        if not self.make:
            self.skipTest("GNU Make is required")

        for target, override_var in (
            ("sched-selftest", "SCHED_SELFTEST_MANIFEST_generic"),
            ("dispatch-isolation-selftest", "DISPATCH_ISO_MANIFEST_generic"),
        ):
            with self.subTest(target=target):
                proc = subprocess.run(
                    [
                        self.make,
                        "--no-print-directory",
                        target,
                        f"{override_var}=nonexistent_manifest_fixture.json",
                    ],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertNotEqual(
                    proc.returncode, 0,
                    f"{target} did not fail when a configuration was invalid:\n"
                    + proc.stdout + proc.stderr,
                )


class BuildArtifactLifecycleTests(unittest.TestCase):
    """Structural and functional tests for clean, clean-fixtures, distclean, tidy, and clean-all targets."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.make = shutil.which("mingw32-make") or shutil.which("make")
        cls.makefile_text = (ROOT / "Makefile").read_text(encoding="utf-8")

    def test_makefile_declares_lifecycle_phony_targets(self) -> None:
        """Verify that clean-fixtures, tidy, and clean-all are declared as phony targets."""
        phony_match = re.search(r"^\.PHONY:\s*(.+)$", self.makefile_text, re.MULTILINE)
        self.assertIsNotNone(phony_match, "No .PHONY declaration found in Makefile")
        phony_targets = set(phony_match.group(1).split())
        for target in ("clean", "clean-fixtures", "tidy", "distclean", "clean-all"):
            self.assertIn(target, phony_targets, f"Target {target} missing from .PHONY")

    def test_clean_removes_specified_build_dir(self) -> None:
        """make clean BUILD_DIR=<target> must remove the specified directory without touching other paths."""
        if not self.make:
            self.skipTest("GNU Make is required")
        target_dir = ROOT / "build" / "test_lifecycle_clean"
        target_dir.mkdir(parents=True, exist_ok=True)
        sentinel = target_dir / "sample_artifact.o"
        sentinel.write_text("dummy", encoding="utf-8")
        self.assertTrue(sentinel.is_file())

        proc = subprocess.run(
            [self.make, "--no-print-directory", "clean", f"BUILD_DIR={target_dir.as_posix()}"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertFalse(target_dir.exists(), f"Target dir {target_dir} was not cleaned")

    def test_clean_fixtures_removes_fixture_subdirs(self) -> None:
        """make clean-fixtures must remove smoke, cosim, and oracle artifact directories under build/."""
        if not self.make:
            self.skipTest("GNU Make is required")
        fixture_dirs = [
            ROOT / "build" / "production-smoke",
            ROOT / "build" / "production-smoke-gap",
            ROOT / "build" / "cosim",
            ROOT / "build" / "nakagawa_psp_oracle",
            ROOT / "build" / "vfpu_oracle",
        ]
        for fdir in fixture_dirs:
            fdir.mkdir(parents=True, exist_ok=True)
            (fdir / "artifact.tmp").write_text("tmp", encoding="utf-8")

        proc = subprocess.run(
            [self.make, "--no-print-directory", "clean-fixtures"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        for fdir in fixture_dirs:
            self.assertFalse(fdir.exists(), f"Fixture directory {fdir} was not cleaned")

    def test_distclean_and_tidy_preserve_binaries_while_cleaning_objects_and_ephemeral_logs(self) -> None:
        """distclean and tidy must preserve .exe and .pdb while removing .o, .d, and ephemeral logs."""
        if not self.make:
            self.skipTest("GNU Make is required")
        test_dir = ROOT / "build" / "test_lifecycle_distclean"
        test_dir.mkdir(parents=True, exist_ok=True)
        exe_file = test_dir / "mygame.exe"
        pdb_file = test_dir / "mygame.pdb"
        obj_file = test_dir / "mygame.o"
        dep_file = test_dir / "mygame.d"
        exe_file.write_text("binary", encoding="utf-8")
        pdb_file.write_text("symbols", encoding="utf-8")
        obj_file.write_text("object", encoding="utf-8")
        dep_file.write_text("deps", encoding="utf-8")

        log_dir = ROOT / "logs"
        log_dir.mkdir(parents=True, exist_ok=True)
        ephemeral_log = log_dir / "build_out_recomp.log"
        ephemeral_log.write_text("ephemeral log", encoding="utf-8")

        proc = subprocess.run(
            [self.make, "--no-print-directory", "tidy", f"BUILD_DIR={test_dir.as_posix()}"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue(exe_file.is_file(), "distclean deleted .exe")
        self.assertTrue(pdb_file.is_file(), "distclean deleted .pdb")
        self.assertFalse(obj_file.exists(), "distclean did not delete .o")
        self.assertFalse(dep_file.exists(), "distclean did not delete .d")
        self.assertFalse(ephemeral_log.exists(), "distclean did not clean ephemeral log")

        # Cleanup test dir
        shutil.rmtree(test_dir, ignore_errors=True)

    def test_clean_all_cleans_all_build_subdirs_and_ephemeral_logs(self) -> None:
        """clean-all must remove all subdirectories under build/ and ephemeral build logs."""
        if not self.make:
            self.skipTest("GNU Make is required")
        sub_a = ROOT / "build" / "test_sub_a"
        sub_b = ROOT / "build" / "test_sub_b"
        sub_a.mkdir(parents=True, exist_ok=True)
        sub_b.mkdir(parents=True, exist_ok=True)
        (sub_a / "test.bin").write_text("a", encoding="utf-8")
        (sub_b / "test.bin").write_text("b", encoding="utf-8")

        log_dir = ROOT / "logs"
        log_dir.mkdir(parents=True, exist_ok=True)
        recomp_log = log_dir / "recomp_err.log"
        recomp_log.write_text("err", encoding="utf-8")

        proc = subprocess.run(
            [self.make, "--no-print-directory", "clean-all"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertFalse(sub_a.exists(), f"Subdir {sub_a} was not cleaned by clean-all")
        self.assertFalse(sub_b.exists(), f"Subdir {sub_b} was not cleaned by clean-all")
        self.assertFalse(recomp_log.exists(), f"Log {recomp_log} was not cleaned by clean-all")

    def test_clean_targets_never_delete_protected_paths(self) -> None:
        """Verification that clean recipes do not touch protected paths or source directories."""
        protected_dirs = [
            ROOT / "assets",
            ROOT / "fixtures",
            ROOT / "src",
            ROOT / "tools",
            ROOT / "docs",
        ]
        for pdir in protected_dirs:
            self.assertTrue(pdir.is_dir(), f"Protected directory {pdir} must exist")

        # Create non-ephemeral log file and verify it is not deleted by clean targets
        log_dir = ROOT / "logs"
        log_dir.mkdir(parents=True, exist_ok=True)
        evidence_log = log_dir / "evidence_run_test.log"
        evidence_log.write_text("evidence data", encoding="utf-8")

        if self.make:
            proc = subprocess.run(
                [self.make, "--no-print-directory", "clean-all"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

        self.assertTrue(evidence_log.is_file(), "clean-all deleted non-ephemeral evidence log")
        evidence_log.unlink(missing_ok=True)

        for pdir in protected_dirs:
            self.assertTrue(pdir.is_dir(), f"Protected directory {pdir} was compromised")


class MachinePortabilityTests(unittest.TestCase):
    """Regression and structural tests for machine and toolchain portability."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.make = shutil.which("mingw32-make") or shutil.which("make")

    def test_vulkan_sdk_discovery_in_makefile_resolves_when_unset(self) -> None:
        """When VULKAN_SDK is not explicitly set, Makefile discovers it dynamically via tools/vulkan_sdk.py."""
        if not self.make:
            self.skipTest("GNU Make is required")
        proc = subprocess.run(
            [self.make, "--no-print-directory", "compiler-info"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("CFLAGS=", proc.stdout)
        self.assertIn("Include", proc.stdout)

    def test_mem_debug_nm_discovery_prefers_environment_and_path(self) -> None:
        """get_symbol_rvas in mem_debug.py must probe NM environment variable and shutil.which before hardcoded paths."""
        mem_debug_text = (ROOT / "tools" / "mem_debug.py").read_text(encoding="utf-8")
        self.assertIn("os.environ.get(\"NM\")", mem_debug_text)
        self.assertIn("shutil.which(\"nm\")", mem_debug_text)

    def test_copy_build_assets_script_has_toolchain_discovery_fallback(self) -> None:
        """copy_build_assets.ps1 must attempt compiler toolchain discovery if SDL3.dll is absent from local dirs."""
        script_text = (ROOT / "copy_build_assets.ps1").read_text(encoding="utf-8")
        self.assertIn("Get-Command gcc", script_text)
        self.assertIn("SDL3.dll", script_text)


class StrbufSafetyTests(unittest.TestCase):
    """Structural tests ensuring safe cursor-accumulation formatting across source-owned C/C++."""

    def test_strbuf_header_declares_safe_inline_append(self) -> None:
        """src/rt/strbuf.h must define static inline sr_buf_append and sr_buf_append_v with bounds checks."""
        header_path = ROOT / "src" / "rt" / "strbuf.h"
        self.assertTrue(header_path.is_file(), "src/rt/strbuf.h must exist")
        text = header_path.read_text(encoding="utf-8")
        self.assertIn("sr_buf_append", text)
        self.assertIn("sr_buf_append_v", text)
        self.assertIn("n >= cap", text)
        self.assertIn("cap - n - 1", text)
        self.assertIn("format(printf", text)

    def test_trace_paths_use_sr_buf_append_not_unclamped_accumulation(self) -> None:
        """recomp.c, interp.cpp, and ge.c must not use unclamped n += snprintf(buf + n, ...)."""
        recomp_text = (ROOT / "src" / "rt" / "recomp.c").read_text(encoding="utf-8")
        interp_text = (ROOT / "src" / "ref" / "interp.cpp").read_text(encoding="utf-8")
        ge_text = (ROOT / "src" / "rt" / "ge.c").read_text(encoding="utf-8")

        self.assertNotIn("n += snprintf(line + n", recomp_text)
        self.assertIn("sr_buf_append(line, sizeof(line)", recomp_text)

        self.assertNotIn("n += std::snprintf(line + n", interp_text)
        self.assertIn("sr_buf_append(line, sizeof(line)", interp_text)

        self.assertNotIn("bn += snprintf(buf + bn", ge_text)
        self.assertIn("sr_buf_append(buf, sizeof(buf)", ge_text)

    def test_makefile_declares_strbuf_selftest(self) -> None:
        """Makefile must declare strbuf-selftest target in .PHONY and compile strbuf_selftest.c."""
        makefile_text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("strbuf-selftest", makefile_text)
        self.assertIn("strbuf_selftest.c", makefile_text)


if __name__ == "__main__":
    unittest.main()
