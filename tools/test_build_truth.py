# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression tests for compiler-profile and transitive-header build truth."""

from __future__ import annotations

import ast
import json
import os
from pathlib import Path
import re
import shlex
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
import codegen


_MTIME_MARGIN_NS = 2_000_000_000


def _set_mtime_after(path: Path, *references: Path) -> None:
    """Set ``path`` newer than the references without waiting for the clock."""
    latest = path.stat().st_mtime_ns
    if references:
        latest = max(latest, *(reference.stat().st_mtime_ns for reference in references))
    target = max(latest, time.time_ns()) + _MTIME_MARGIN_NS
    os.utime(path, ns=(target, target))


def _set_mtime_before(path: Path) -> None:
    """Move a fixture output into the past without waiting for the clock."""
    target = min(path.stat().st_mtime_ns, time.time_ns()) - _MTIME_MARGIN_NS
    os.utime(path, ns=(target, target))


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

        (self.root / "inner.h").write_text("#define INNER_TOKEN 2\n", encoding="ascii")
        _set_mtime_after(self.root / "inner.h", self.root / "build" / "dependent.o")
        self.assert_compiled(self.run_make("-O0"), "dependent.c")
        # The forced future timestamp proves the dependency edge.  Normalize
        # the fixture to the newly produced object before the unchanged-build
        # assertion; otherwise Make would quite correctly see the synthetic
        # future timestamp again on the next invocation.
        dependent_obj = self.root / "build" / "dependent.o"
        os.utime(self.root / "inner.h", ns=(dependent_obj.stat().st_mtime_ns,) * 2)
        self.assert_compiled(self.run_make("-O0"))

        self.assert_compiled(self.run_make("-O2"), "dependent.c", "unrelated.c")
        self.assert_compiled(self.run_make("-O2"))
        self.assert_compiled(self.run_make("-O0"), "dependent.c", "unrelated.c")

        (self.root / "inner.h").rename(self.root / "renamed.h")
        (self.root / "outer.h").write_text('#include "renamed.h"\n', encoding="ascii")
        _set_mtime_after(self.root / "outer.h", self.root / "build" / "dependent.o")
        self.assert_compiled(self.run_make("-O0"), "dependent.c")

        manifest = json.loads((self.root / "build" / "profile.json").read_text())
        self.assertEqual(manifest["sections"]["runtime"]["entries"], ["PROFILE_FLAGS=-O0"])

    def test_repository_rules_do_not_keep_manager_object_lists(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        manager = (ROOT / "nk_manager.ps1").read_text(encoding="utf-8")
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


class CpuStateAbiTests(unittest.TestCase):
    """The native and generated sides must agree on the versioned CpuState ABI."""

    def setUp(self) -> None:
        # An explicit CC may be a compound command (e.g. "ccache gcc"), as Make
        # accepts; a PATH-resolved compiler is a single path and is not split.
        configured = os.environ.get("CC")
        if configured:
            self.cc_command = shlex.split(configured)
        else:
            found = shutil.which("gcc") or shutil.which("cc")
            self.cc_command = [found] if found else []
        if not self.cc_command:
            self.skipTest("a C compiler is required")
        self.cc = " ".join(self.cc_command)
        self.temp = tempfile.TemporaryDirectory(prefix="nakagawa-cpustate-abi-")
        self.work = Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def _compile(self, source: Path, include_dir: Path, output: Path) -> subprocess.CompletedProcess:
        return subprocess.run(
            [*self.cc_command, "-std=c11", "-I", str(include_dir), "-c", str(source),
             "-o", str(output)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )

    def test_cpustate_v2_layout_and_generated_header_guard(self) -> None:
        layout_source = self.work / "layout.c"
        layout_source.write_text(
            textwrap.dedent(
                """
                #include <stddef.h>
                #include "recomp.h"

                _Static_assert(SR_CPUSTATE_ABI_VERSION == 2u, "ABI version");
                _Static_assert(offsetof(CpuState, cop0) == 852u, "cop0 offset");
                _Static_assert(offsetof(CpuState, next_pc) == 980u, "next_pc offset");
                _Static_assert(offsetof(CpuState, in_delay_slot) == 984u, "delay offset");
                _Static_assert(offsetof(CpuState, flow_kind) == 988u, "flow kind offset");
                _Static_assert(offsetof(CpuState, flow_target) == 992u, "flow target offset");
                _Static_assert(sizeof(CpuState) == 996u, "CpuState size");

                int abi_probe(void) {
                    CpuState state = {0};
                    *sr_cp0_status_ptr(&state) = 0x12345678u;
                    return state.cop0[SR_CP0_STATUS] != 0x12345678u;
                }
                """
            ).lstrip(),
            encoding="ascii",
        )
        layout = self._compile(layout_source, ROOT / "src" / "rt", self.work / "layout.o")
        self.assertEqual(
            layout.returncode,
            0,
            "the base runtime must compile the asserted CpuState ABI:\n"
            + layout.stdout
            + layout.stderr,
        )

        stale_dir = self.work / "stale"
        stale_dir.mkdir()
        generated_header = stale_dir / "generated_funcs.h"
        codegen.write_funcs_header(generated_header, [0x00001000])
        (stale_dir / "recomp.h").write_text(
            "#include <stdint.h>\n"
            "#define SR_CPUSTATE_ABI_VERSION 1u\n"
            "typedef struct CpuState CpuState;\n",
            encoding="ascii",
        )
        stale_source = stale_dir / "stale.c"
        stale_source.write_text('#include "generated_funcs.h"\n', encoding="ascii")
        stale = self._compile(stale_source, stale_dir, self.work / "stale.o")
        self.assertNotEqual(
            stale.returncode,
            0,
            "a generated header must reject a runtime with a mismatched ABI version",
        )
        self.assertRegex(stale.stdout + stale.stderr, r"SR_CPUSTATE_ABI_VERSION|CpuState ABI version")


class CpuStateAbiProfileTests(unittest.TestCase):
    """Profile hashing needs no compiler; keep it out of the compiler-gated suite."""

    def test_profile_hash_tracks_recomp_header_content(self) -> None:
        compiler = "cc-placeholder-for-profile-hash"
        with tempfile.TemporaryDirectory(prefix="nakagawa-cpustate-profile-") as temp:
            header = Path(temp) / "recomp.h"
            header.write_text("#define SR_CPUSTATE_ABI_VERSION 2u\n", encoding="ascii")
            first = build_profile.profile_hash(
                build_profile.profile_payload(compiler, ["RECOMP_FLAGS=-O0"], files=[str(header)])
            )
            header.write_text("#define SR_CPUSTATE_ABI_VERSION 3u\n", encoding="ascii")
            second = build_profile.profile_hash(
                build_profile.profile_payload(compiler, ["RECOMP_FLAGS=-O0"], files=[str(header)])
            )
            self.assertNotEqual(first, second)

            makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
            self.assertIn('CPU_STATE_ABI_HEADER := src/rt/recomp.h', makefile)
            self.assertIn('--file "$(CPU_STATE_ABI_HEADER)"', makefile)


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
        # Deliberately an assertion, not a skipTest. Skipping here would let the
        # guard retire itself: whoever removed the last sr_fbcap_* call would see
        # green, and the Makefile coupling this test protects would stop being
        # checked with nothing to say so. If the coupling really is gone, that is
        # a decision worth making explicitly -- delete this test in the same
        # commit that removes the calls.
        self.assertNotEqual(
            referenced,
            [],
            "sdl3vk.c no longer calls the fbcap policy, so this guard now "
            "protects nothing. Retire it deliberately (delete this test) rather "
            "than leaving it to pass vacuously.",
        )

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


NESTED_FRAMES_C = "src/rt/nested_frames.c"
# Anything that already carries nested_frames.c: naming one of these is as good
# as naming the file, and a recipe should prefer them.
NESTED_FRAMES_BUNDLES = (
    NESTED_FRAMES_C,
    "$(RT_SRCS)",
    "$(RT_OBJS)",
    "$(PORTABLE_CORE_SRCS)",
    "$(PORTABLE_CORE_OBJS)",
)


def _nested_frame_callers() -> list[str]:
    """Translation units that need nested_frames.c at link time.

    Direct callers are found by reference, not by a hand-kept list, so a new
    call site is covered the day it is written.  Indirect ones are the files
    that #include a direct caller's .c wholesale -- the selftest fixtures do
    that to get white-box access, and they inherit its undefined symbols.
    """
    src = ROOT / "src" / "rt"
    call = re.compile(r"\bsr_nested_frame_\w+\s*\(")
    direct = []
    texts = {}
    for path in sorted(src.rglob("*.c")):
        rel = path.relative_to(ROOT).as_posix()
        texts[rel] = path.read_text(encoding="utf-8", errors="replace")
        if rel == NESTED_FRAMES_C:
            continue
        if call.search(texts[rel]):
            direct.append(rel)
    indirect = []
    for rel, text in texts.items():
        if rel in direct or rel == NESTED_FRAMES_C:
            continue
        for caller in direct:
            if '#include "%s"' % Path(caller).name in text:
                indirect.append(rel)
                break
    return sorted(set(direct) | set(indirect))


def _is_link_statement(text: str) -> bool:
    """A link command, as opposed to a compile or a prerequisite list.

    Prerequisite lines name sources without ever running the linker, and the
    per-object rules compile with -c; neither needs the module's definitions.
    """
    if " -c " in text or text.rstrip().endswith(" -c"):
        return False
    return " -o " in text


class PortableCoreSourceSetTests(unittest.TestCase):
    """The hosted-Linux portable compile set is pinned and cannot silently shrink.

    ``make portable-core-objects`` (ci.yml, native_tools) is the only hosted
    proof that every PORTABLE_CORE_SRCS translation unit still compiles without
    Windows-only dependencies; the strict-C sweep contract in AGENTS.md sec. 9
    rides on the same set.  This test pins the set exactly: dropping (or
    silently reordering) an entry fails here, so no file can lose its Linux
    compile coverage without a deliberate, reviewed edit to this expectation.
    It also keeps the documented exclusions honest: sched.c, the gpu_sdl3vk
    backend and the *_selftest mains are deliberately NOT in the set (SDL3 or
    Vulkan host dependencies; selftest mains, not library objects).
    """

    EXPECTED_PORTABLE_SRCS = (
        "src/rt/recomp.c",
        "src/rt/flight_recorder.c",
        "src/rt/cpu_lle.c",
        "src/rt/domain_mode.c",
        "src/rt/nested_frames.c",
        "src/rt/stale_code.c",
        "src/rt/guest_interp.c",
        "src/rt/title_config.c",
        "src/rt/vfpu_tables.c",
        "src/rt/archive_vfs.c",
        "src/rt/debug.c",
        "src/rt/watchpoints_file.c",
        "src/rt/guest_printf.c",
        "src/rt/perf.c",
        "src/rt/vfpu_interp.c",
        "$(ISO_BACKEND_SRC)",
        "$(PGD_BACKEND_SRC)",
        "src/rt/mpeg.c",
        "$(PGF_BACKEND_SRC)",
        "src/rt/savedata.c",
        "src/rt/ge.c",
        "src/rt/h264_null.c",
        "src/rt/sr_coro.c",
    )

    def test_portable_core_srcs_is_pinned(self):
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        match = re.search(
            r"^PORTABLE_CORE_SRCS :=((?:.*\\\n)*.*)$", makefile, re.MULTILINE
        )
        self.assertIsNotNone(match, "PORTABLE_CORE_SRCS missing from the Makefile")
        entries = tuple(match.group(1).replace("\\\n", " ").split())
        self.assertEqual(entries, self.EXPECTED_PORTABLE_SRCS)


class NestedFramesLinkDependencyTests(unittest.TestCase):
    """hle.c, mpeg.c and sched.c call into nested_frames.c, so every recipe that
    links one of them must also supply it.  This is the same failure shape as
    the sdl3vk/fbcap guard above: the first version of the module updated
    RT_SRCS, PORTABLE_CORE_SRCS and the three hle.c selftest recipes but not
    sched-selftest, which failed to link on `undefined reference to
    sr_nested_frame_release_owner`.
    """

    def setUp(self) -> None:
        self.makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.lines = _logical_lines(self.makefile)

    def test_module_defines_the_symbols_its_callers_use(self) -> None:
        module = (ROOT / NESTED_FRAMES_C).read_text(encoding="utf-8")
        for symbol in ("sr_nested_frame_acquire", "sr_nested_frame_release",
                       "sr_nested_frame_release_owner", "sr_nested_frame_release_all"):
            definition = re.compile(rf"^\w[\w \t*]*\b{symbol}\s*\(", re.MULTILINE)
            self.assertRegex(module, definition, msg=symbol)

    def test_the_caller_set_is_discovered_not_assumed(self) -> None:
        callers = _nested_frame_callers()
        for expected in ("src/rt/hle.c", "src/rt/mpeg.c", "src/rt/sched.c"):
            self.assertIn(expected, callers)

    def test_every_link_of_a_caller_also_supplies_the_module(self) -> None:
        callers = _nested_frame_callers()
        offenders = []
        for number, text in self.lines:
            if not _is_link_statement(text):
                continue
            if not any(caller in text for caller in callers):
                continue
            if any(bundle in text for bundle in NESTED_FRAMES_BUNDLES):
                continue
            offenders.append(f"Makefile:{number}: {text.strip()}")
        self.assertEqual(
            offenders,
            [],
            "these Makefile link statements use a nested-frame caller without "
            f"{NESTED_FRAMES_C}; the link will fail with undefined references to "
            "sr_nested_frame_*:\n" + "\n".join(offenders),
        )

    def test_the_guard_rejects_a_recipe_that_drops_the_module(self) -> None:
        """Failing-before proof: the check must catch the sched-selftest shape."""
        continuation = chr(92)
        regressed = _logical_lines(
            "\n".join(
                [
                    "sched-selftest-one:",
                    "\t$(CC) $(CFLAGS) $(LDFLAGS) -o out.exe " + continuation,
                    "\t\tsrc/rt/sched_selftest.c src/rt/sr_coro.c $(LIBS)",
                ]
            )
            + "\n"
        )
        callers = _nested_frame_callers()
        offenders = [
            number
            for number, text in regressed
            if _is_link_statement(text)
            and any(caller in text for caller in callers)
            and not any(bundle in text for bundle in NESTED_FRAMES_BUNDLES)
        ]
        self.assertEqual(offenders, [2])

    def test_a_compile_only_rule_is_not_flagged(self) -> None:
        """The per-object rule for a caller compiles with -c and links nothing."""
        self.assertFalse(_is_link_statement(
            "\t$(CC) $(CFLAGS) $(DEPFLAGS) -c src/rt/hle.c -o $(BUILD_DIR)/hle.o"))
        self.assertFalse(_is_link_statement(
            "$(BUILD_DIR)/hle.o: src/rt/hle.c src/rt/asset_index.h"))


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

    def test_hst_name_does_not_change_generic_optimization_defaults(self) -> None:
        info = self.run_compiler_info("GAME_NAME=hst")
        self.assertEqual(info.get("RUNTIME_OPT"), "-O0")
        self.assertEqual(info.get("RECOMP_OPT"), "-O0")
        self.assertTrue(info.get("CFLAGS", "").startswith("-O0 "), info.get("CFLAGS"))
        self.assertTrue(info.get("RECOMP_FLAGS", "").startswith("-O0 "), info.get("RECOMP_FLAGS"))

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
            [
                self.make,
                "--no-print-directory",
                f"BUILD_DIR={hst_dir.as_posix()}",
                "GAME_NAME=hst",
                "RUNTIME_OPT=-O2",
                "RECOMP_OPT=-O1",
                "compiler-info",
            ],
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

    def test_private_hst_profile_values_are_supplied_by_the_manifest_adapter(self) -> None:
        adapter = (ROOT / "tools" / "title_manager_plan.ps1").read_text(encoding="utf-8")
        manager = (ROOT / "nk_manager.ps1").read_text(encoding="utf-8-sig")
        self.assertIn('"RUNTIME_OPT=-O2"', adapter)
        self.assertIn('"RECOMP_OPT=-O1"', adapter)
        self.assertNotIn("HST_EXTRA_SPANS", adapter)
        self.assertIn("$boundPlan.Environment.TITLE_EXTRA_SPANS", manager)

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
        self._build_dir("test_gi_m1")
        elf_rel = f"build/test_gi_m1/{self.SPLIT_NAME}"
        self._write_minimal_elf(ROOT / elf_rel)

        mutant = self._mutate_makefile(
            ("$(BUILD_DIR)/$(GAME_NAME)_image.bin: $(GAME_INPUT_PREREQ) tools/prxload.py\n"
             "\t$(PYTHON) tools/prxload.py --env-elf $(GAME_BASE)",
             "$(BUILD_DIR)/$(GAME_NAME)_image.bin: tools/prxload.py\n"
             "\t$(PYTHON) tools/prxload.py $(GAME_ELF) $(GAME_BASE)"),
        )
        blob = self._blob(self._make("test_gi_m1", elf_rel, makefile=mutant))
        self.assertTrue(
            self._injection_evidence(blob),
            "mutation did not reproduce the pre-fix command execution; this "
            "regression is no longer load-bearing. Expected one of "
            f"{self.INJECTION_SIGNATURES} in:\n" + blob)

    def test_M1b_sibling_inputs_are_transported_too(self) -> None:
        """GAME_PSP_HEADER shares the recipe, and so must share the transport."""
        self._build_dir("test_gi_hdr")
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
                # The input stamp is rewritten by Make, so also make the
                # existing output unambiguously old.  This avoids relying on
                # Windows' one-second timestamp resolution for the stamp and
                # keeps the assertion about the dependency edge deterministic.
                _set_mtime_before(image)
                _set_mtime_after(elf, image)
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

        _set_mtime_after(elf, image)
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
        self._build_dir(game)
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
        (build / f"{game}_recomp.c").write_text("void f_00304290(void *s) { (void)s; }\n",
                                                encoding="ascii")
        (build / f"{game}_recomp_funcs.h").write_text("", encoding="ascii")
        generated_inputs = tuple(p for p in build.iterdir() if p.is_file())
        _set_mtime_after(build / f"{game}_recomp.c", *generated_inputs)
        _set_mtime_after(build / f"{game}_recomp_funcs.h", build / f"{game}_recomp.c")

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

    def test_makefile_recipes_do_not_use_unix_mkdir_p(self) -> None:
        offenders = []
        for line_no, line in enumerate(self.makefile_text.splitlines(), start=1):
            if line.startswith("\t") and re.search(r"\bmkdir\s+-p\b", line):
                offenders.append(f"Makefile:{line_no}: {line.strip()}")
        self.assertEqual(
            offenders, [],
            "Makefile recipes contain 'mkdir -p' which fails under Windows cmd.exe:\n"
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
        self.assertEqual(
            set(phony_match.group(1).split()),
            {"$(PUBLIC_TARGETS)", "$(INTERNAL_TARGETS)"},
            "The .PHONY declaration must consume the single-source target catalogs",
        )
        catalog_match = re.search(
            r"(?ms)^PUBLIC_TARGETS := \\\n(?P<targets>.*?)(?=^INTERNAL_TARGETS :=)",
            self.makefile_text,
        )
        self.assertIsNotNone(catalog_match, "No PUBLIC_TARGETS catalog found in Makefile")
        public_targets = set(catalog_match.group("targets").replace("\\", "").split())
        for target in ("clean", "clean-fixtures", "tidy", "distclean", "clean-all"):
            self.assertIn(target, public_targets, f"Target {target} missing from PUBLIC_TARGETS")

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
        if os.name == "nt":
            self.assertIn("Include", proc.stdout)
        else:
            # Linux uses the system Vulkan loader and headers: an unset SDK must
            # not leave a dangling -I/Include in the compiler flags.
            self.assertNotIn("-I/Include", proc.stdout)

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

    def test_sdl3_discovery_in_makefile_records_identity_and_version(self) -> None:
        """Makefile compiler-info must discover and report SDL3_DIR, SDL3_PROVIDER, and SDL3_VERSION."""
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
        self.assertIn("SDL3_DIR=", proc.stdout)
        self.assertIn("SDL3_PROVIDER=", proc.stdout)
        self.assertIn("SDL3_VERSION=", proc.stdout)
        msys_prefix = os.environ.get("MSYS_PATH")
        if msys_prefix:
            msys_root = Path(msys_prefix).parent if Path(msys_prefix).name.lower() == "bin" else Path(msys_prefix)
        else:
            msys_root = Path(r"C:\msys64\ucrt64")
        if os.name == "nt" and (msys_root / "include" / "SDL3" / "SDL.h").is_file():
            self.assertIn("SDL3_PROVIDER=msys2_ucrt64", proc.stdout)
            self.assertIn(f"SDL3_DIR={msys_root.as_posix()}", proc.stdout)

    def test_sdl3_search_flags_precede_vulkan_sdk_in_makefile(self) -> None:
        """SDL3 include and library flags must precede Vulkan SDK flags in CFLAGS, PLAYER_INCLUDES, and LDFLAGS."""
        makefile_text = (ROOT / "Makefile").read_text(encoding="utf-8")
        cflags_match = re.search(r"CFLAGS\s*\?=\s*([^\r\n]+)", makefile_text)
        self.assertIsNotNone(cflags_match, "CFLAGS must be defined")
        cflags = cflags_match.group(1)
        self.assertIn("$(SDL3_INC_FLAGS)", cflags)
        sdl3_cflags_pos = cflags.index("$(SDL3_INC_FLAGS)")
        # CFLAGS carries the Vulkan include flags through VULKAN_INC_FLAGS, whose
        # Windows definition derives from $(VULKAN_SDK).
        self.assertRegex(makefile_text, r"VULKAN_INC_FLAGS\s*:=\s*-I\$\(VULKAN_SDK\)/Include")
        vulkan_cflags_pos = cflags.index("$(VULKAN_INC_FLAGS)")
        self.assertLess(sdl3_cflags_pos, vulkan_cflags_pos, "SDL3 includes must precede Vulkan SDK in CFLAGS")

        ldflags_match = re.search(r"LDFLAGS\s*\?=\s*([^\r\n]+)", makefile_text)
        self.assertIsNotNone(ldflags_match, "LDFLAGS must be defined")
        ldflags = ldflags_match.group(1)
        self.assertIn("$(SDL3_LDFLAGS)", ldflags)
        sdl3_ld_pos = ldflags.index("$(SDL3_LDFLAGS)")
        vulkan_ld_pos = ldflags.index("-L$(VULKAN_SDK)")
        self.assertLess(sdl3_ld_pos, vulkan_ld_pos, "SDL3 lib flags must precede Vulkan SDK in LDFLAGS")

        player_inc_match = re.search(r"PLAYER_INCLUDES\s*:=\s*([^\r\n]+)", makefile_text)
        self.assertIsNotNone(player_inc_match, "PLAYER_INCLUDES must be defined")
        player_inc = player_inc_match.group(1)
        self.assertIn("$(SDL3_INC_FLAGS)", player_inc)
        sdl3_pinc_pos = player_inc.index("$(SDL3_INC_FLAGS)")
        vulkan_pinc_pos = player_inc.index("$(PLAYER_VULKAN_INC)")
        self.assertLess(sdl3_pinc_pos, vulkan_pinc_pos, "SDL3 includes must precede Vulkan SDK in PLAYER_INCLUDES")

    def test_sdl3_discovery_fails_closed_with_actionable_remediation(self) -> None:
        """An invalid or absent SDL3 must stop the SDL3-linking targets with install instructions.

        The guard sits on the targets that link -lSDL3 (compile, player); portable runtime
        objects build without SDL3, so the guard itself is exercised here.
        """
        if not self.make:
            self.skipTest("GNU Make is required")
        makefile_text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertRegex(makefile_text, r"(?m)^compile:.*\| sdl3-check$")
        self.assertRegex(makefile_text, r"(?m)^\$\(PLAYER_EXE\): \| player-vulkan-check sdl3-check$")
        proc = subprocess.run(
            [self.make, "--no-print-directory", "sdl3-check", "SDL3_DIR=C:/nonexistent_sdl3_repro_test"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(proc.returncode, 0, "Build must fail closed when SDL3 is absent")
        combined_output = proc.stdout + proc.stderr
        self.assertIn("pacman -S mingw-w64-ucrt-x86_64-sdl3", combined_output)
        self.assertNotIn("fatal error: SDL3", combined_output, "Must fail at discovery phase before compiler execution")

    def test_copy_build_assets_supports_explicit_sdl3_dll_path(self) -> None:
        """copy_build_assets.ps1 must declare Sdl3DllPath parameter and Makefile must pass SDL3_DLL."""
        script_text = (ROOT / "copy_build_assets.ps1").read_text(encoding="utf-8")
        self.assertIn("[string]$Sdl3DllPath", script_text)
        self.assertIn("Copy-Item $Sdl3DllPath", script_text)

        makefile_text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("-Sdl3DllPath \"$(SDL3_DLL)\"", makefile_text)

    def test_runtime_profile_records_sdl3_identity(self) -> None:
        """runtime_profile.json must record SDL3_PROVIDER, SDL3_VERSION, and SDL3_DIR."""
        if not self.make:
            self.skipTest("GNU Make is required")
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_build = Path(tmpdir) / "build_test"
            proc = subprocess.run(
                [self.make, "--no-print-directory", f"BUILD_DIR={tmp_build.as_posix()}", "compiler-info"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            manifest_file = tmp_build / "runtime_profile.json"
            self.assertTrue(manifest_file.is_file(), "runtime_profile.json must be recorded")
            manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
            entries = manifest.get("sections", {}).get("runtime", {}).get("entries", [])
            self.assertTrue(any(e.startswith("SDL3_PROVIDER=") for e in entries), entries)
            self.assertTrue(any(e.startswith("SDL3_VERSION=") for e in entries), entries)
            self.assertTrue(any(e.startswith("SDL3_DIR=") for e in entries), entries)

    def test_sdl3_competing_provider_isolation_and_precedence(self) -> None:
        """nk_doctor_checks.discover_sdl3_provider isolates MSYS2 UCRT64 from incidental Vulkan SDK SDL3."""
        from unittest import mock
        import nk_doctor_checks
        from nk_doctor_checks import discover_sdl3_provider, Sdl3ProviderError

        # These are the Windows provider rules; every input is a synthetic directory,
        # so pin the platform decision instead of depending on the host running the test.
        with mock.patch.object(nk_doctor_checks.platform, "system", return_value="Windows"),              tempfile.TemporaryDirectory() as tmpdir:
            tmproot = Path(tmpdir)
            msys = tmproot / "msys64" / "ucrt64"
            (msys / "include" / "SDL3").mkdir(parents=True)
            (msys / "include" / "SDL3" / "SDL.h").write_text("#define SDL_MAJOR_VERSION 3\n#define SDL_MINOR_VERSION 4\n#define SDL_MICRO_VERSION 0\n", encoding="utf-8")
            (msys / "lib").mkdir(parents=True)
            (msys / "lib" / "libSDL3.dll.a").write_bytes(b"!<arch>\n")
            (msys / "bin").mkdir(parents=True)
            fake_pe = b"MZ" + b"\x00" * 58 + struct.pack("<I", 64) + b"PE\0\0" + struct.pack("<H", 0x8664)
            (msys / "bin" / "SDL3.dll").write_bytes(fake_pe)

            vulkan = tmproot / "VulkanSDK" / "1.4.357.0"
            (vulkan / "Include" / "SDL3").mkdir(parents=True)
            (vulkan / "Include" / "SDL3" / "SDL.h").write_text("#define SDL_MAJOR_VERSION 3\n", encoding="utf-8")
            (vulkan / "Lib").mkdir(parents=True)
            (vulkan / "Lib" / "SDL3.lib").write_bytes(b"!<arch>\n")
            (vulkan / "Bin").mkdir(parents=True)
            (vulkan / "Bin" / "SDL3.dll").write_bytes(b"MZ")

            prov = discover_sdl3_provider(msys_path=msys, vulkan_sdk=vulkan)
            self.assertEqual(prov.provider, "msys2_ucrt64")
            self.assertEqual(prov.root_dir, msys)
            self.assertEqual(prov.import_lib, msys / "lib" / "libSDL3.dll.a")

            empty_msys = tmproot / "empty_msys"
            (empty_msys / "bin").mkdir(parents=True)
            with self.assertRaises(Sdl3ProviderError) as ctx:
                discover_sdl3_provider(msys_path=empty_msys, vulkan_sdk=vulkan)
            err_msg = str(ctx.exception)
            self.assertIn("SDL3 dependency is missing from the supported MSYS2 UCRT64 toolchain", err_msg)
            self.assertIn("an incidental copy exists in Vulkan SDK", err_msg)
            self.assertIn("pacman -S mingw-w64-ucrt-x86_64-sdl3", err_msg)

            explicit_dir = tmproot / "custom_sdl3"
            (explicit_dir / "include" / "SDL3").mkdir(parents=True)
            (explicit_dir / "include" / "SDL3" / "SDL.h").write_text("#define SDL_MAJOR_VERSION 3\n", encoding="utf-8")
            (explicit_dir / "lib").mkdir(parents=True)
            (explicit_dir / "lib" / "libSDL3.dll.a").write_bytes(b"!<arch>\n")
            prov_exp = discover_sdl3_provider(explicit=str(explicit_dir), msys_path=msys, vulkan_sdk=vulkan)
            self.assertEqual(prov_exp.root_dir, explicit_dir)


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


class Sdl3MakeFragmentTests(unittest.TestCase):
    """The Makefile reads every SDL3 variable from one discovery pass."""

    def _fragment(self, provider, pkg_flags=None):
        sys.path.insert(0, str(ROOT / "tools"))
        import nk_doctor_checks as ndc
        from unittest import mock
        flags = pkg_flags or {}
        with mock.patch.object(ndc, "discover_sdl3_provider", return_value=provider),              mock.patch.object(ndc, "_pkg_config_flags", side_effect=lambda f: flags.get(f, "")):
            text = ndc.sdl3_make_fragment()
        return dict(line.split(" := ", 1) for line in text.splitlines())

    def _provider(self, kind, inc, lib):
        sys.path.insert(0, str(ROOT / "tools"))
        import nk_doctor_checks as ndc
        return ndc.Sdl3Provider(provider=kind, root_dir=Path(inc).parent,
                                include_dir=Path(inc), import_lib=Path(lib),
                                runtime_dll=None, version="3.4.0",
                                arch="x86_64", is_supported=True)

    def test_system_default_dirs_are_never_forced_onto_the_search_path(self) -> None:
        values = self._fragment(self._provider("system", "/usr/include",
                                               "/usr/lib/x86_64-linux-gnu/libSDL3.so"))
        self.assertEqual(values["SDL3_INC_FLAGS"], "")
        self.assertEqual(values["SDL3_LDFLAGS"], "")
        self.assertEqual(values["SDL3_ERROR"], "")

    def test_pkg_config_provider_uses_pkg_config_flags(self) -> None:
        values = self._fragment(
            self._provider("pkg_config", "/usr/include", "/usr/lib/libSDL3.so"),
            {"--cflags": "-I/usr/include/SDL3 -D_REENTRANT", "--libs-only-L": ""})
        self.assertEqual(values["SDL3_INC_FLAGS"], "-I/usr/include/SDL3 -D_REENTRANT")
        self.assertNotIn("-I/usr/include ", values["SDL3_INC_FLAGS"] + " ")

    def test_nonstandard_root_is_placed_on_the_search_path(self) -> None:
        values = self._fragment(self._provider("msys2_ucrt64", "C:/msys64/ucrt64/include",
                                               "C:/msys64/ucrt64/lib/libSDL3.dll.a"))
        self.assertEqual(values["SDL3_INC_FLAGS"], "-IC:/msys64/ucrt64/include")
        self.assertEqual(values["SDL3_LDFLAGS"], "-LC:/msys64/ucrt64/lib")

    def test_msys2_provider_with_a_foreign_compiler_fails_closed_without_flags(self) -> None:
        import nk_doctor_checks as ndc
        from unittest import mock
        provider = self._provider("msys2_ucrt64", "C:/msys64/ucrt64/include",
                                  "C:/msys64/ucrt64/lib/libSDL3.dll.a")
        with mock.patch.object(ndc, "discover_sdl3_provider", return_value=provider), \
             mock.patch.object(ndc.shutil, "which", return_value="C:/other/mingw64/bin/gcc.exe"):
            text = ndc.sdl3_make_fragment(compiler="gcc")
        values = dict(line.split(" := ", 1) for line in text.splitlines())
        self.assertEqual(values["SDL3_INC_FLAGS"], "")
        self.assertEqual(values["SDL3_LDFLAGS"], "")
        self.assertIn("sdl3 dependency is missing", values["SDL3_ERROR"].lower())
        self.assertIn("first on PATH", values["SDL3_ERROR"])

    def test_msys2_provider_with_its_own_compiler_emits_flags(self) -> None:
        import nk_doctor_checks as ndc
        from unittest import mock
        provider = self._provider("msys2_ucrt64", "C:/msys64/ucrt64/include",
                                  "C:/msys64/ucrt64/lib/libSDL3.dll.a")
        with mock.patch.object(ndc, "discover_sdl3_provider", return_value=provider), \
             mock.patch.object(ndc.shutil, "which", return_value="C:/msys64/ucrt64/bin/gcc.exe"):
            text = ndc.sdl3_make_fragment(compiler="gcc")
        values = dict(line.split(" := ", 1) for line in text.splitlines())
        self.assertEqual(values["SDL3_ERROR"], "")
        self.assertEqual(values["SDL3_LDFLAGS"], "-LC:/msys64/ucrt64/lib")

    def test_makefile_discovers_sdl3_once_per_parse(self) -> None:
        makefile_text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertEqual(sum("write_sdl3_make_fragment" in line for line in makefile_text.splitlines()), 1)
        self.assertNotIn("query_sdl3_info", makefile_text)


class ToolsModuleImportPathTests(unittest.TestCase):
    """Regression tests ensuring test modules under tools/ use path idioms for sibling imports."""

    def test_tools_test_modules_importing_siblings_use_path_idiom(self) -> None:
        """Every tools/test_*.py importing a tools sibling must insert tools into sys.path."""
        tools_dir = ROOT / "tools"
        sibling_modules = {p.stem for p in tools_dir.glob("*.py") if not p.name.startswith("test_")}

        missing: list[tuple[str, list[str]]] = []
        for path in sorted(tools_dir.glob("test_*.py")):
            text = path.read_text(encoding="utf-8")
            has_path_idiom = (
                "sys.path.insert" in text
                or "sys.path.append" in text
                or "except ModuleNotFoundError" in text
                or "except ImportError" in text
            )
            if has_path_idiom:
                continue

            tree = ast.parse(text)
            imported: list[str] = []
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    for alias in node.names:
                        if alias.name in sibling_modules:
                            imported.append(alias.name)
                elif isinstance(node, ast.ImportFrom):
                    if node.module and node.module in sibling_modules:
                        imported.append(node.module)
            if imported:
                missing.append((path.name, sorted(set(imported))))

        self.assertEqual(
            missing,
            [],
            f"Test modules under tools/ import siblings without sys.path idiom: {missing}",
        )


if __name__ == "__main__":
    unittest.main()
