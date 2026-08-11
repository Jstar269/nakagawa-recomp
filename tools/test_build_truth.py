# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression tests for compiler-profile and transitive-header build truth."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
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
if __name__ == "__main__":
    unittest.main()
