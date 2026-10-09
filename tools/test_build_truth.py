# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

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
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
PROFILE_TOOL = ROOT / "tools" / "build_profile.py"
COMMON_MK = ROOT / "mk" / "build_common.mk"
sys.path.insert(0, str(ROOT / "tools"))

import build_profile
import codegen
import title_codegen_plan


_MTIME_MARGIN_NS = 2_000_000_000

#: Runs the repository's profile tool without ever spelling its path on a command
#: line, so a checkout under a path containing spaces still works. The path rides
#: in the environment, the only transport a command interpreter cannot split.
PROFILE_TOOL_ENV = "NK_PROFILE_TOOL"
PROFILE_TOOL_LAUNCH = (
    "import os, runpy, sys; p = os.environ['" + PROFILE_TOOL_ENV + "']; "
    "sys.argv[0] = p; runpy.run_path(p, run_name='__main__')"
)


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


def _make_safe_temp_dir(prefix: str) -> Path:
    """A new temporary directory whose path GNU Make can name (no whitespace).

    On a Windows profile whose name contains a space the temporary directory does
    too; its 8.3 short form is used then. Without one the calling test is skipped
    with the remedy instead of failing on the Makefile's space guard.
    """
    created = Path(tempfile.mkdtemp(prefix=prefix))
    if not any(char.isspace() for char in created.as_posix()):
        return created
    short = title_codegen_plan._windows_short_path(created)
    if short is not None and not any(char.isspace() for char in short.as_posix()):
        return short
    shutil.rmtree(created, ignore_errors=True)
    raise unittest.SkipTest(
        f"the temporary directory {created} contains whitespace GNU Make cannot name and "
        "has no 8.3 short form; point TMP/TEMP at a folder without spaces"
    )


def _make_safe_fixture_dir(name: str) -> Path:
    """A BUILD_DIR a lifecycle test may hand to Make.

    Not derived from the repository root on purpose. GNU Make splits a target or
    prerequisite name on whitespace, so a BUILD_DIR that inherits a space from the
    checkout path (`C:/some path/repo/build/...`) never reaches the recipe intact:
    Make binds the first fragment as the target and treats the rest as goals, so
    `clean` deletes a directory the caller never named and the parse-time mkdir
    creates a `spaces/...` tree in the repository root. The Makefile now refuses
    such a BUILD_DIR outright; these tests must therefore pass a path Make can
    represent, and a temporary directory is also the right place for a fixture.
    """
    return _make_safe_temp_dir(f"nakagawa-lifecycle-{name}-") / "build"


#: Make goals that delete files. A test may run one only against scratch roots.
DESTRUCTIVE_MAKE_GOALS = frozenset({"clean", "clean-fixtures", "distclean", "tidy", "clean-all"})

#: The ephemeral logs distclean and clean-all remove from LOG_DIR; any other log stays.
EPHEMERAL_LOG_NAMES = (
    "build_out_recomp.log", "build_err_recomp.log", "recomp_err.log", "obj_err.log",
    "link_err.log", "stdout_run.log", "stderr_run.log",
)


def _scratch_build_root(test: unittest.TestCase, name: str) -> Path:
    """A Make-safe scratch BUILD_ROOT, removed when ``test`` finishes.

    Any goal other than `help` writes beneath BUILD_ROOT while Make parses (the
    per-title BUILD_DIR, its profile stamps, the SDL3 discovery cache), and a
    changed profile stamp invalidates the objects already there. A probe that
    only wants a printed value therefore still runs against a scratch root, so
    it cannot rewrite or invalidate a developer's real build tree.
    """
    build_root = _make_safe_fixture_dir(name)
    test.addCleanup(shutil.rmtree, build_root.parent, True)
    return build_root


def _class_scratch_build_root(cls: type[unittest.TestCase], name: str) -> Path:
    """A scratch BUILD_ROOT shared by one test class, exported to its environment.

    The package planner (tools/title_codegen_plan.py) runs its own Make with the
    caller's environment, so a class that packages a title cannot pass BUILD_ROOT on a
    command line it never builds. Exporting it for the class (and restoring the
    environment afterwards) sends those runs to the scratch tree too. The directory is
    removed when the class finishes.
    """
    build_root = _make_safe_fixture_dir(name)
    cls.addClassCleanup(shutil.rmtree, build_root.parent, True)
    patcher = mock.patch.dict(os.environ, {"BUILD_ROOT": build_root.as_posix()})
    patcher.start()
    cls.addClassCleanup(patcher.stop)
    return build_root


def _reaches_checkout(path: Path) -> bool:
    """True when ``path`` is the repository checkout, lies inside it, or contains it."""
    resolved = path.resolve()
    return resolved.is_relative_to(ROOT) or ROOT.is_relative_to(resolved)


def _run_scratch_make(
    make: str, *arguments: str, build_root: Path, log_dir: Path
) -> subprocess.CompletedProcess:
    """Run the REAL repository Makefile with BUILD_ROOT and LOG_DIR on a scratch tree.

    Every lifecycle goal deletes beneath those two roots, and every parse-time write
    a goal makes (the per-title BUILD_DIR, profile stamps, the SDL3 discovery cache)
    lands beneath BUILD_ROOT. Pointing both away from the checkout is what keeps a
    suite run from emptying a developer's real build/ (private title builds,
    packages) and logs/. A root that would reach the checkout is refused before
    Make starts.
    """
    for variable, root in (("BUILD_ROOT", build_root), ("LOG_DIR", log_dir)):
        if _reaches_checkout(root):
            raise AssertionError(
                f"{variable}={root} reaches the repository checkout; a test may only "
                "run a destructive Make goal against a scratch tree"
            )
    return subprocess.run(
        [make, "--no-print-directory", *arguments,
         f"BUILD_ROOT={build_root.as_posix()}", f"LOG_DIR={log_dir.as_posix()}"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


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
        # The real shared make fragment, copied in so the fixture `include`s a
        # relative name. GNU Make cannot name a file whose path contains a space
        # -- not as a target, not as a prerequisite, and not even in an
        # `include` (quotes are kept as literal filename bytes by Make 4.4) --
        # so a checkout under `C:/some path/repo` cannot be referenced from a
        # fixture at all. Copying the fragment keeps its DEPFLAGS authoritative
        # while leaving the fixture's Make-visible names space-free.
        shutil.copyfile(COMMON_MK, self.root / COMMON_MK.name)
        (self.root / "inner.h").write_text("#define INNER_TOKEN 1\n", encoding="ascii")
        (self.root / "outer.h").write_text('#include "inner.h"\n', encoding="ascii")
        (self.root / "dependent.c").write_text(
            '#include "outer.h"\nint dependent(void) { return INNER_TOKEN; }\n',
            encoding="ascii",
        )
        (self.root / "unrelated.c").write_text(
            "int unrelated(void) { return 7; }\n", encoding="ascii"
        )
        # The profile tool is addressed through the environment, never through a
        # Make-visible name or a command-line argument. A checkout whose own path
        # contains a space cannot be spelled on a command line at all: sh and
        # cmd.exe both split it, so the tool would be handed a truncated path.
        # The `-c` launcher is the same shape the repository Makefile already
        # uses for its own inline Python, so it behaves identically under sh and
        # under the cmd.exe fallback.
        #
        # The tool is therefore deliberately NOT a prerequisite here (Make cannot
        # name it), so the stamp's staleness rests on the profile identity alone.
        # The tool dependency edge this fixture used to model lives in the
        # repository Makefile as `$(RUNTIME_PROFILE_STAMP): $(BUILD_PROFILE_TOOL)`,
        # where the tool is a repository-relative name; that edge is asserted in
        # test_repository_rules_keep_the_profile_tool_dependency.
        profile_run = PROFILE_TOOL_LAUNCH
        (self.root / "Makefile").write_text(
            textwrap.dedent(
                f"""
                PYTHON ?= python
                CC ?= gcc
                PROFILE_FLAGS ?= -O0
                BUILD := build
                PROFILE_RUN := -c "{profile_run}"
                include {COMMON_MK.name}
                PROFILE_HASH := $(shell $(PYTHON) $(PROFILE_RUN) hash --compiler "$(CC)" --entry "PROFILE_FLAGS=$(PROFILE_FLAGS)")
                PROFILE_STAMP := $(BUILD)/.profile-$(PROFILE_HASH)
                PROFILE_MANIFEST := $(BUILD)/profile.json
                OBJS := $(BUILD)/dependent.o $(BUILD)/unrelated.o

                .PHONY: all
                all: $(OBJS)

                $(PROFILE_STAMP):
                \t$(PYTHON) $(PROFILE_RUN) record --output $(PROFILE_MANIFEST) --section runtime --compiler "$(CC)" --entry "PROFILE_FLAGS=$(PROFILE_FLAGS)" --stamp "$@" --stale-glob ".profile-*" $(foreach obj,$(OBJS),--invalidate "$(obj)")

                $(BUILD)/%.o: %.c $(PROFILE_STAMP)
                \t@echo COMPILE $<
                \t"$(CC)" $(PROFILE_FLAGS) $(DEPFLAGS) -c $< -o $@

                -include $(PROFILE_STAMP)
                -include $(OBJS:.o=.d)
                """
            ).lstrip(),
            encoding="utf-8",
            newline="\n",
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def run_make(self, flags: str) -> str:
        env = dict(os.environ)
        env[PROFILE_TOOL_ENV] = str(PROFILE_TOOL)
        proc = subprocess.run(
            [self.make, "--no-print-directory", f"CC={self.cc}", f"PROFILE_FLAGS={flags}"],
            cwd=self.root,
            env=env,
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

    def test_repository_rules_keep_the_profile_tool_dependency(self) -> None:
        """The profile stamps depend on the tool that writes them.

        The build-truth fixture cannot model this edge: GNU Make cannot name a
        file whose path contains a space, so a fixture rooted under a checkout
        such as `C:/some path/repo` can never put the tool in a prerequisite
        list. The repository Makefile has no such constraint -- it names the tool
        relatively -- so the edge is pinned here, where it is real.
        """
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("BUILD_PROFILE_TOOL := tools/build_profile.py", makefile)
        for stamp in ("$(RUNTIME_PROFILE_STAMP)", "$(RECOMP_PROFILE_STAMP)",
                      "$(CODEGEN_PROFILE_STAMP)", "$(TITLE_CONFIG_STAMP)"):
            self.assertIn(
                f"{stamp}: $(BUILD_PROFILE_TOOL)", makefile,
                f"{stamp} must depend on the tool that writes it",
            )

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
CAPTURE_C = "src/rt/fbcap.c"
SDL3VK_VAR = "$(SDL3VK_SRCS)"


def _supplies_capture(text: str) -> bool:
    """True when a Makefile statement links the capture policy AND service."""
    return SDL3VK_VAR in text or (FBCAP_C in text and CAPTURE_C in text)


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


#: A link that names a response file (`@$(BUILD_DIR)/x.rsp`) and the `$(file >...)` statement
#: that writes it.
_RESPONSE_FILE_REF = re.compile(r"@\$\(BUILD_DIR\)/([\w.-]+\.rsp)\b")
_RESPONSE_FILE_WRITE = re.compile(r"^\$\(file >\$\(BUILD_DIR\)/([\w.-]+\.rsp),(.*)\)\s*$")


def _with_response_file_inputs(logical: list[tuple[int, str]]) -> list[tuple[int, str]]:
    """Each statement with the words of any response file it names appended.

    A link may take its object list from `@$(BUILD_DIR)/x.rsp`, which make writes from an
    earlier `$(file >$(BUILD_DIR)/x.rsp,<words>)` statement: a long BUILD_ROOT would push the
    list past the shell's line limit. The link's inputs are those words, so these scans read
    them as part of the link statement. The checks themselves are unchanged.
    """
    written: dict[str, str] = {}
    for _, text in logical:
        match = _RESPONSE_FILE_WRITE.match(text.strip())
        if match:
            written[match.group(1)] = match.group(2)
    result: list[tuple[int, str]] = []
    for number, text in logical:
        words = [written[name] for name in _RESPONSE_FILE_REF.findall(text) if name in written]
        result.append((number, " ".join([text, *words])))
    return result


class Sdl3vkLinkDependencyTests(unittest.TestCase):
    """sdl3vk.c calls into fbcap_policy.c and the presenter-neutral capture
    service fbcap.c, so every recipe that compiles the backend must also supply
    both.  Issue #57 added the policy call sites and
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
        service = (ROOT / "src" / "rt" / "fbcap.c").read_text(encoding="utf-8")
        backend = (ROOT / "src" / "rt" / "gpu_sdl3vk" / "sdl3vk.c").read_text(encoding="utf-8")
        called = sorted(set(re.findall(r"\b(sr_capture_\w+)\s*\(", backend)))
        self.assertNotEqual(called, [], "sdl3vk.c no longer calls the capture service")
        for symbol in called:
            definition = re.compile(rf"^\w[\w \t*]*\b{symbol}\s*\(", re.MULTILINE)
            self.assertRegex(service, definition, msg=symbol)

    def test_sdl3vk_srcs_bundles_the_backend_with_its_policy(self) -> None:
        definitions = [
            text for _, text in self.lines if re.match(r"\s*SDL3VK_SRCS\s*:?=", text)
        ]
        self.assertEqual(len(definitions), 1, self.lines)
        self.assertIn(SDL3VK_C, definitions[0])
        self.assertIn(FBCAP_C, definitions[0])
        self.assertIn(CAPTURE_C, definitions[0])

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
            if uses_backend and not _supplies_capture(text):
                offenders.append(f"Makefile:{number}: {text.strip()}")
        self.assertEqual(
            offenders,
            [],
            "these Makefile statements compile "
            f"{SDL3VK_C} without {FBCAP_C} and {CAPTURE_C}; sdl3vk.c calls "
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
            if (SDL3VK_C in text or SDL3VK_VAR in text) and not _supplies_capture(text)
        ]
        self.assertEqual(offenders, [2])
        # Supplying the policy without the capture service is the same link failure.
        policy_only = _logical_lines(
            f"ge-replay:\n\t$(CC) -o out.exe {SDL3VK_C} {FBCAP_C} $(LIBS)\n"
        )
        self.assertFalse(_supplies_capture(policy_only[1][1]))


class FlightRecorderLinkDependencyTests(unittest.TestCase):
    """The synthetic media selftest includes mpeg.c, which inherits the
    production recorder declarations through recomp.h. The Makefile adds
    SR_FLIGHT_RECORDER_LINKED globally, so this host target must link the real
    recorder implementation just like the other runtime selftests do.
    """

    def test_psmf_media_selftest_links_the_recorder_implementation(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        logical = _with_response_file_inputs(_logical_lines(makefile))
        selftest = (ROOT / "src" / "rt" / "psmf_media_selftest.c").read_text(encoding="utf-8")
        mpeg = (ROOT / "src" / "rt" / "mpeg.c").read_text(encoding="utf-8")
        recomp = (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8")
        recorder = (ROOT / "src" / "rt" / "flight_recorder.c").read_text(encoding="utf-8")

        self.assertIn('#include "mpeg.c"', selftest)
        self.assertIn('#include "recomp.h"', mpeg)
        self.assertIn('#include "flight_recorder.h"', recomp)
        self.assertIn("override CFLAGS += -DSR_FLIGHT_RECORDER_LINKED", makefile)
        self.assertRegex(recorder, r"(?m)^int g_sr_watch_enabled;")
        self.assertRegex(recorder, r"(?m)^void sr_watch_store\s*\(")

        media_links = [
            (number, text)
            for number, text in logical
            if "psmf_media_selftest.exe" in text and _is_link_statement(text)
        ]
        self.assertEqual(len(media_links), 1, media_links)
        number, link = media_links[0]
        # The recipe either names the implementation or links the filtered
        # runtime object set, which carries flight_recorder.o unless the filter
        # removes it.
        supplied = "src/rt/flight_recorder.c" in link or (
            "$(PSMF_MEDIA_RUNTIME_OBJS)" in link
            and "flight_recorder.o" not in _psmf_media_filtered_out(makefile)
        )
        self.assertTrue(
            supplied,
            "Makefile:%d links psmf_media_selftest.exe with SR_FLIGHT_RECORDER_LINKED, "
            "but omits the implementation that defines sr_watch_store and "
            "g_sr_watch_enabled:\n%s" % (number, link.strip()),
        )


def _psmf_media_filtered_out(makefile: str) -> str:
    """The objects PSMF_MEDIA_RUNTIME_OBJS removes from RT_OBJS (its filter-out list)."""
    match = re.search(
        r"(?m)^PSMF_MEDIA_RUNTIME_OBJS\s*:=\s*\$\(filter-out\s+([^,]*),\s*\$\(RT_OBJS\)\)",
        makefile,
    )
    assert match is not None, "PSMF_MEDIA_RUNTIME_OBJS must stay a filter-out of RT_OBJS"
    return match.group(1)


HLE_C = "src/rt/hle.c"
H264_BACKENDS = ("src/rt/h264_mf.c", "src/rt/h264_null.c")
H264_BACKEND_OBJECTS = ("h264_mf.o", "h264_null.o")
HLE_RUNTIME_BUNDLES = ("$(RT_SRCS)", "$(RT_OBJS)", "$(PSMF_MEDIA_RUNTIME_OBJS)")


def _rt_runtime_sources(makefile: str) -> set[str]:
    """Return the source tokens in RT_SRCS, stopping before its derived object set."""
    match = re.search(r"(?ms)^RT_SRCS\s*:=\s*(.*?)^RT_OBJS\s*:=", makefile)
    assert match is not None, "RT_SRCS must remain the source of the runtime object set"
    return set(match.group(1).replace("\\\n", " ").split())


def _hle_link_supplies_h264_backends(link: str, makefile: str) -> bool:
    """Check direct sources or a runtime object bundle that retains both backends."""
    direct_sources = all(source in link for source in H264_BACKENDS)
    if direct_sources:
        return True

    bundles = [bundle for bundle in HLE_RUNTIME_BUNDLES if bundle in link]
    if not bundles:
        return False

    runtime_sources = _rt_runtime_sources(makefile)
    if not all(source in runtime_sources for source in H264_BACKENDS):
        return False

    if "$(PSMF_MEDIA_RUNTIME_OBJS)" in bundles:
        filtered_out = _psmf_media_filtered_out(makefile)
        if any(obj in filtered_out for obj in H264_BACKEND_OBJECTS):
            return False
    return True


def _hle_link_h264_offenders(makefile: str) -> list[str]:
    """Find Makefile link commands that compile HLE without its PSMF backends."""
    logical = _with_response_file_inputs(_logical_lines(makefile))
    hle_links = [
        (number, text)
        for number, text in logical
        if _is_link_statement(text) and HLE_C in text
    ]
    if not hle_links:
        return [f"Makefile has no link command containing {HLE_C}"]
    return [
        f"Makefile:{number}: {text.strip()}"
        for number, text in hle_links
        if not _hle_link_supplies_h264_backends(text, makefile)
    ]


class HlePsmfPlayerBackendLinkDependencyTests(unittest.TestCase):
    """HLE's registered PSMF player path calls the H.264 backend API."""

    def setUp(self) -> None:
        self.makefile = (ROOT / "Makefile").read_text(encoding="utf-8")

    def test_runtime_sources_keep_both_h264_backends(self) -> None:
        sources = _rt_runtime_sources(self.makefile)
        self.assertIn(HLE_C, sources)
        for backend in H264_BACKENDS:
            self.assertIn(backend, sources)

    def test_every_hle_link_supplies_both_h264_backends(self) -> None:
        hle = (ROOT / HLE_C).read_text(encoding="utf-8")
        self.assertIn("hle_register_psmf_player_handlers", hle)
        self.assertRegex(hle, r"\bsr_h264_create\s*\(")

        offenders = _hle_link_h264_offenders(self.makefile)
        self.assertEqual(
            offenders,
            [],
            "these Makefile link statements include the registered HLE player path "
            "without both H.264 backend sources (or a runtime bundle retaining them):\n"
            + "\n".join(offenders),
        )

    def test_guard_rejects_direct_and_filtered_out_backend_regressions(self) -> None:
        """The source guard catches both selftest link shapes that #616 repaired."""
        direct_pattern = "src/rt/h264_mf.c src/rt/h264_null.c"
        self.assertEqual(self.makefile.count(direct_pattern), 3)
        direct_regression = self.makefile.replace(direct_pattern, "")
        direct_offenders = _hle_link_h264_offenders(direct_regression)
        self.assertEqual(len(direct_offenders), 3, "all three direct HLE links must be guarded")

        filter_pattern = "$(BUILD_DIR)/psmf_producer.o,$(RT_OBJS)"
        self.assertEqual(self.makefile.count(filter_pattern), 1)
        filtered_regression = self.makefile.replace(
            filter_pattern,
            "$(BUILD_DIR)/psmf_producer.o $(BUILD_DIR)/h264_mf.o "
            "$(BUILD_DIR)/h264_null.o,$(RT_OBJS)",
        )
        filtered_offenders = _hle_link_h264_offenders(filtered_regression)
        self.assertEqual(len(filtered_offenders), 2, "both PSMF media links must be guarded")


NESTED_FRAMES_C = "src/rt/nested_frames.c"
# Anything that already carries nested_frames.c: naming one of these is as good
# as naming the file, and a recipe should prefer them.
NESTED_FRAMES_BUNDLES = (
    NESTED_FRAMES_C,
    "$(RT_SRCS)",
    "$(RT_OBJS)",
    "$(PORTABLE_CORE_SRCS)",
    "$(PORTABLE_CORE_OBJS)",
    # RT_OBJS minus a fixed filter-out list; test_psmf_runtime_objs_keep_nested_frames
    # fails if that list ever removes nested_frames.o.
    "$(PSMF_MEDIA_RUNTIME_OBJS)",
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
    if re.search(r"(?:^|\s)-c(?:\s|$)", text):
        return False
    return re.search(r"(?:^|\s)-o\s", text) is not None


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


def _link_defines_nested_frame_stubs(text: str) -> bool:
    """True when a source named on the link line defines the nested-frame API itself.

    A self-contained selftest (psmf_media_selftest.c) supplies its own stubs, and
    linking nested_frames.c as well would be a duplicate definition.
    """
    definition = re.compile(r"^\w[\w \t*]*\bsr_nested_frame_acquire\s*\(", re.MULTILINE)
    for token in re.findall(r"src/[\w/.-]+\.c", text):
        path = ROOT / token
        if token != NESTED_FRAMES_C and path.is_file() and definition.search(
                path.read_text(encoding="utf-8")):
            return True
    return False


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
        self.lines = _with_response_file_inputs(_logical_lines(self.makefile))

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

    def test_psmf_runtime_objs_keep_nested_frames(self) -> None:
        removed = _psmf_media_filtered_out(self.makefile)
        self.assertNotIn("nested_frames.o", removed)
        self.assertIn("src/rt/nested_frames.c", self.makefile.split("RT_SRCS    :=", 1)[1])

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
            if _link_defines_nested_frame_stubs(text):
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
                [self.make, "--no-print-directory", f"BUILD_DIR={build_dir.as_posix()}",
                 f"BUILD_ROOT={Path(temp_dir).as_posix()}", "atrac3p-objects"],
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
                [self.make, "--no-print-directory", f"BUILD_DIR={build_dir.as_posix()}",
                 f"BUILD_ROOT={Path(temp_dir).as_posix()}", "atrac3p-objects"],
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
                [self.make, "-j4", "--no-print-directory", f"BUILD_DIR={build_dir.as_posix()}",
                 f"BUILD_ROOT={Path(temp_dir).as_posix()}", "atrac3p-objects"],
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
            f"BUILD_ROOT={self.build_dir.as_posix()}",
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
                f"BUILD_ROOT={self.build_dir.as_posix()}",
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
            [self.make, "--no-print-directory", f"BUILD_DIR={generic_dir.as_posix()}",
             f"BUILD_ROOT={self.build_dir.as_posix()}", "GAME_NAME=mygame", "compiler-info"],
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
                f"BUILD_ROOT={self.build_dir.as_posix()}",
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


class CodegenProfileTransportTests(unittest.TestCase):
    """The codegen profile names every guest module (EXTRA_ELF_ARGS). A title with a
    hundred modules under a long private path must still get a profile: the entries
    travel in the environment, never on a command line a shell truncates."""

    def setUp(self) -> None:
        self.make = shutil.which("mingw32-make") or shutil.which("make")
        if not self.make:
            self.skipTest("GNU Make is required")
        self.temp = tempfile.TemporaryDirectory(prefix="nakagawa-codegen-profile-")
        self.addCleanup(self.temp.cleanup)

    def _profile_hash(self, modules: list[str], **extra: str) -> str:
        assignments = [f"{key}={value}" for key, value in extra.items()]
        completed = subprocess.run(
            [self.make, "--no-print-directory", "GAME_NAME=probe",
             f"BUILD_ROOT={Path(self.temp.name).as_posix()}",
            f"BUILD_DIR={Path(self.temp.name).as_posix()}",
             f"GAME_EXTRA_ELFS={' '.join(modules)}", *assignments,
             "--eval", "nkprobe: ; @echo CODEGEN_PROFILE_HASH=$(CODEGEN_PROFILE_HASH)",
             "nkprobe"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        line = next((text for text in completed.stdout.splitlines()
                     if text.startswith("CODEGEN_PROFILE_HASH=")), "")
        return line.partition("=")[2]

    def test_many_long_module_paths_still_produce_a_profile(self) -> None:
        modules = [
            "C:/path/to/a-rather-long-user-data-directory/work/0123456789abcdef/"
            f"user-data/modules/module_{index:03d}.prx@runtime"
            for index in range(200)
        ]
        self.assertGreater(len(" ".join(modules)), 16384)
        self.assertRegex(self._profile_hash(modules), r"^[0-9a-f]{20}$")
        self.assertNotEqual(self._profile_hash(modules), self._profile_hash(modules[:-1]))

    def test_the_transport_keeps_the_argv_hash(self) -> None:
        # The environment form must hash exactly as the former --entry form did, so
        # existing builds keep their codegen profile. The former form is evaluated in
        # the same Make environment (same compiler resolution) for the comparison.
        former = (
            '$(shell "$(PYTHON)" $(BUILD_PROFILE_TOOL) hash --compiler "$(PYTHON)" '
            '--entry "GAME_NAME=$(GAME_NAME)" --entry "GAME_BASE=$(GAME_BASE)" '
            '--entry "CODEGEN_PROFILE_ARG=$(CODEGEN_PROFILE_ARG)" '
            '--entry "EXTRA_ELF_ARGS=$(EXTRA_ELF_ARGS)" --entry "EXTRA_SPAN_ARG=$(EXTRA_SPAN_ARG)" '
            '--entry "FUNCS_PER_CHUNK=$(FUNCS_PER_CHUNK)" '
            '--entry "CODEGEN_USER_ARGS=$(CODEGEN_USER_ARGS)" '
            '--entry "CODEGEN_TOOL=$(CODEGEN_TOOL)" --file "$(CPU_STATE_ABI_HEADER)" '
            '--entry "CHUNK_TARGET_BYTES=$(CHUNK_TARGET_BYTES)")'
        )
        completed = subprocess.run(
            [self.make, "--no-print-directory", "GAME_NAME=probe",
             f"BUILD_ROOT={Path(self.temp.name).as_posix()}",
            f"BUILD_DIR={Path(self.temp.name).as_posix()}",
             "GAME_EXTRA_ELFS=fixtures/a.prx@runtime fixtures/b.prx@0x08900000",
             "CHUNK_TARGET_BYTES=65536",
             "--eval", f"nkprobe: ; @echo NEW=$(CODEGEN_PROFILE_HASH) OLD={former}",
             "nkprobe"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        line = next(text for text in completed.stdout.splitlines() if text.startswith("NEW="))
        new_hash, _, old_part = line.partition(" OLD=")
        self.assertRegex(old_part, r"^[0-9a-f]{20}$")
        self.assertEqual(new_hash.removeprefix("NEW="), old_part)


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
        self.root = _make_safe_temp_dir("nakagawa-guest-input-")
        self.addCleanup(shutil.rmtree, self.root, True)
        # The scratch BUILD_ROOT every build here lands under. Nothing is written to
        # the checkout's own build/, so a developer's title builds are never touched.
        self.build_root = self.root / "build"

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
        return self.build_root / name

    def _make(self, game_name: str, elf: str, *, makefile: Path | None = None,
              target: str | None = None, extra: tuple[str, ...] = ()) -> subprocess.CompletedProcess:
        tgt = target or f"{self._build_dir(game_name).as_posix()}/{game_name}_image.bin"
        cmd = [self.make, "--no-print-directory"]
        if makefile is not None:
            cmd += ["-f", str(makefile)]
        cmd += [tgt, f"BUILD_ROOT={self.build_root.as_posix()}", f"GAME_NAME={game_name}",
                f"GAME_ELF={elf}", "GAME_BASE=0x08804000", "GAME_ENTRY=0x08804000", *extra]
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
        elf = build / self.SPLIT_NAME
        self._write_minimal_elf(elf)

        proc = self._make("test_gi_split", elf.as_posix())
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
        elf = self._build_dir("test_gi_m1") / self.SPLIT_NAME
        self._write_minimal_elf(elf)

        mutant = self._mutate_makefile(
            ("$(BUILD_DIR)/$(GAME_NAME)_image.bin: $(GAME_INPUT_PREREQ) tools/prxload.py\n"
             "\t$(PYTHON) tools/prxload.py --env-elf $(GAME_BASE)",
             "$(BUILD_DIR)/$(GAME_NAME)_image.bin: tools/prxload.py\n"
             "\t$(PYTHON) tools/prxload.py $(GAME_ELF) $(GAME_BASE)"),
        )
        blob = self._blob(self._make("test_gi_m1", elf.as_posix(), makefile=mutant))
        self.assertTrue(
            self._injection_evidence(blob),
            "mutation did not reproduce the pre-fix command execution; this "
            "regression is no longer load-bearing. Expected one of "
            f"{self.INJECTION_SIGNATURES} in:\n" + blob)

    def test_M1b_sibling_inputs_are_transported_too(self) -> None:
        """GAME_PSP_HEADER shares the recipe, and so must share the transport."""
        build = self._build_dir("test_gi_hdr")
        elf = build / "plain.elf"
        self._write_minimal_elf(elf)
        hdr = build / "hdr&ver&tail.BIN"
        hdr.write_bytes(b"\x00" * 64)

        proc = self._make("test_gi_hdr", elf.as_posix(),
                          extra=(f"GAME_PSP_HEADER={hdr.as_posix()}",))
        blob = self._blob(proc)
        self.assertEqual(
            self._injection_evidence(blob), [],
            "GAME_PSP_HEADER still reaches a command interpreter:\n" + blob)

    # -- D: freshness is preserved, not dropped --------------------------

    def _first_build(self, game: str, elf: Path) -> Path:
        self._write_minimal_elf(elf)
        proc = self._make(game, elf.as_posix())
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
                elf = self._first_build(game, build / base)
                image = build / f"{game}_image.bin"
                self.assertTrue(image.is_file())

                before = image.stat().st_mtime_ns
                # The input stamp is rewritten by Make, so also make the
                # existing output unambiguously old.  This avoids relying on
                # Windows' one-second timestamp resolution for the stamp and
                # keeps the assertion about the dependency edge deterministic.
                _set_mtime_before(image)
                _set_mtime_after(elf, image)
                proc = self._make(game, elf.as_posix())
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
        elf = build / "plain.elf"
        mutant = self._mutate_makefile(
            ("$(BUILD_DIR)/$(GAME_NAME)_image.bin: $(GAME_INPUT_PREREQ) tools/prxload.py",
             "$(BUILD_DIR)/$(GAME_NAME)_image.bin: tools/prxload.py"),
        )
        self._write_minimal_elf(elf)
        proc = self._make(game, elf.as_posix(), makefile=mutant)
        self.assertEqual(proc.returncode, 0, self._blob(proc))
        image = build / f"{game}_image.bin"
        before = image.stat().st_mtime_ns

        _set_mtime_after(elf, image)
        self._make(game, elf.as_posix(), makefile=mutant)
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
        elf = self._first_build(game, build / "plain.elf")
        image = build / f"{game}_image.bin"
        self.assertTrue(image.is_file())

        # Confirm the target really is considered up to date before deleting.
        proc = self._make(game, elf.as_posix())
        self.assertEqual(proc.returncode, 0, self._blob(proc))

        elf.unlink()
        proc = self._make(game, elf.as_posix())
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
        self._first_build(game, build / "plain.elf")

        for label, value, expect in (
            ("empty", "", "empty or whitespace-only"),
            ("whitespace", "   ", "empty or whitespace-only"),
            ("missing", (self.build_root / "does" / "not" / "exist.elf").as_posix(), "does not exist"),
            ("directory", build.as_posix(), "is a directory"),
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
        base = [self.make, "--no-print-directory", f"GAME_NAME={game}",
                f"BUILD_ROOT={self.build_root.as_posix()}", f"BUILD_DIR={build.as_posix()}"]

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

        proc = subprocess.run(base + [f"{build.as_posix()}/{game}_recomp.c"], cwd=ROOT,
                              capture_output=True, text=True,
                              encoding="utf-8", errors="replace", check=False)
        blob = (proc.stdout or "") + (proc.stderr or "")
        self.assertEqual(proc.returncode, 0,
                         "a lane that declares no GAME_ELF was forced to supply one:\n" + blob)
        self.assertNotIn("GAME_ELF does not exist", blob, blob)

    def test_the_makefile_default_guest_elf_does_not_reach_recipe_environments(self) -> None:
        """The `?=` default must never masquerade as an operator-declared input.

        An unconditional `export GAME_ELF` put eboot.elf into every recipe's
        environment, so a make nested inside any repository recipe -- the suite
        under `make test` or `make contrib-check` -- saw origin GAME_ELF =
        environment, which GAME_INPUT_TRACKED reads as "the operator declared
        an input": the guest-input stamp was demanded for a lane that declares
        none. That is exactly how the public-lane test above failed whenever
        the suite ran through make, and why the same suite passed when run
        directly. A default without a file behind it must stay invisible to
        recipes; a declared value (command line, operator environment) still
        reaches them, and a default that exists on disk is exported by design.
        """
        if (ROOT / "eboot.elf").exists():
            self.skipTest("the fixed default exists on disk and is exported by design")
        probe = ("--eval=probe-guest-env: ; @$(PYTHON) -c "
                 "\"import os; print('GAME_ELF_IN_ENV=' + str(os.environ.get('GAME_ELF')))\"")
        proc = subprocess.run(
            [self.make, "--no-print-directory", f"BUILD_ROOT={self.build_root.as_posix()}",
             probe, "probe-guest-env"],
            cwd=ROOT, capture_output=True, text=True,
            encoding="utf-8", errors="replace", check=False)
        blob = self._blob(proc)
        if proc.returncode != 0 and "unrecognized option" in blob:
            self.skipTest("this GNU Make has no --eval support")
        self.assertEqual(proc.returncode, 0, blob)
        lines = [ln for ln in (proc.stdout or "").splitlines()
                 if ln.startswith("GAME_ELF_IN_ENV=")]
        self.assertEqual(len(lines), 1, "the environment probe did not run:\n" + blob)
        ambient = os.environ.get("GAME_ELF")
        if ambient is None:
            self.assertEqual(
                lines[0], "GAME_ELF_IN_ENV=None",
                "the Makefile's default GAME_ELF leaked into a recipe environment:\n" + blob)
        else:
            self.assertEqual(
                lines[0], "GAME_ELF_IN_ENV=" + ambient,
                "an ambient GAME_ELF was not passed through unchanged:\n" + blob)

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
        elf = build / "dol$lar.elf"
        self._write_minimal_elf(elf)

        proc = self._make(game, elf.as_posix())
        blob = self._blob(proc)
        self.assertNotEqual(proc.returncode, 0, "Make no longer eats `$`; re-derive this case")
        self.assertIn(f"does not exist: {build.as_posix()}/dolar.elf", blob,
                      "Make's `$` expansion changed shape:\n" + blob)

        escaped = self._make(game, elf.as_posix().replace("$", "$$"))
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
                        f"BUILD_ROOT={_scratch_build_root(self, 'matrix').as_posix()}",
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
    """Structural and functional tests for clean, clean-fixtures, distclean, tidy, and clean-all targets.

    Every destructive goal runs through `_run_scratch_make` against a per-test scratch
    BUILD_ROOT and LOG_DIR, never against the checkout's own build/ and logs/. A
    developer's checkout holds private title builds, packages and run logs there, and
    an earlier revision of these tests ran `clean-all` against the checkout itself, so
    every suite run emptied that build/ tree.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.make = shutil.which("mingw32-make") or shutil.which("make")
        cls.makefile_text = (ROOT / "Makefile").read_text(encoding="utf-8")

    def setUp(self) -> None:
        self.build_root = _scratch_build_root(self, "lifecycle")
        self.scratch = self.build_root.parent
        self.log_dir = self.scratch / "logs"

    def _lifecycle_make(self, *arguments: str) -> subprocess.CompletedProcess:
        if not self.make:
            self.skipTest("GNU Make is required")
        return _run_scratch_make(
            self.make, *arguments, build_root=self.build_root, log_dir=self.log_dir
        )

    @staticmethod
    def _plant(path: Path, text: str = "artifact") -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

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
        for target in sorted(DESTRUCTIVE_MAKE_GOALS):
            self.assertIn(target, public_targets, f"Target {target} missing from PUBLIC_TARGETS")

    def test_default_roots_are_the_checkout_build_and_logs_trees(self) -> None:
        """Without overrides the lifecycle roots are the checkout's own build/ and logs/."""
        self.assertRegex(self.makefile_text, r"(?m)^BUILD_ROOT \?= build$")
        self.assertRegex(self.makefile_text, r"(?m)^LOG_DIR +\?= logs$")
        self.assertRegex(self.makefile_text, r"(?m)^BUILD_DIR +\?= \$\(BUILD_ROOT\)/\$\(GAME_NAME\)$")

    def test_platform_ladder_fs_negative_waits_for_positive_image(self) -> None:
        match = re.search(
            r"^platform-ladder-fs-negative:\s*(?P<prerequisites>.*)$",
            self.makefile_text,
            re.MULTILINE,
        )
        self.assertIsNotNone(match, "Filesystem negative ladder target is missing")
        self.assertIn("platform-ladder-fs", match.group("prerequisites").split())

    def test_every_destructive_goal_deletes_only_beneath_the_named_roots(self) -> None:
        """Each lifecycle recipe must take every path it deletes from BUILD_ROOT, LOG_DIR or BUILD_DIR.

        A recipe that spells `build` or `logs/...` itself deletes the checkout's own
        tree whatever roots its caller names. That is how `clean-all` and
        `clean-fixtures` emptied a developer's real build/ and `tidy` removed their
        run logs whenever this suite ran. A dry run prints each recipe without
        executing it, so this pins the property for every goal at once.
        """
        build_dir = self.build_root / "title"
        prefix = self.scratch.as_posix()
        default_root = re.compile(r"(?<![\w./-])(?:build|logs)(?![\w.-])")
        for goal in sorted(DESTRUCTIVE_MAKE_GOALS):
            with self.subTest(goal=goal):
                proc = self._lifecycle_make("-n", goal, f"BUILD_DIR={build_dir.as_posix()}")
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                deletions = [line for line in proc.stdout.splitlines()
                             if "unlink" in line or "rmtree" in line]
                self.assertTrue(deletions, f"{goal} printed no deletion step:\n{proc.stdout}")
                for line in deletions:
                    self.assertIn(prefix, line, f"{goal} deletes outside the named roots: {line}")
                    stray = default_root.findall(line.replace(prefix, ""))
                    self.assertEqual(
                        stray, [],
                        f"{goal} names the checkout's own build/ or logs/ tree: {line}",
                    )

    def test_build_root_that_reaches_the_checkout_is_refused(self) -> None:
        """An overridden BUILD_ROOT may not be the checkout, an ancestor, or a non-build/ subtree.

        clean-all empties BUILD_ROOT, so a typo such as `BUILD_ROOT=.` or
        `BUILD_ROOT=src` would otherwise wipe sources or private inputs. The refusal
        happens at parse time, so it is probed with the information-only `help` goal:
        were the guard ever lost, the probe still deletes and creates nothing.
        """
        if not self.make:
            self.skipTest("GNU Make is required")
        refused = {
            ".": "the repository root or one of its ancestors",
            ROOT.parent.as_posix(): "the repository root or one of its ancestors",
            "src": "inside the checkout but outside its build tree",
            "place_game_here": "inside the checkout but outside its build tree",
        }
        for value, reason in refused.items():
            with self.subTest(build_root=value):
                proc = subprocess.run(
                    [self.make, "--no-print-directory", "help", f"BUILD_ROOT={value}"],
                    cwd=ROOT, capture_output=True, text=True, check=False,
                )
                self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertIn(reason, proc.stderr)
        for value in ("build/nested", self.build_root.as_posix()):
            with self.subTest(build_root=value):
                proc = subprocess.run(
                    [self.make, "--no-print-directory", "help", f"BUILD_ROOT={value}"],
                    cwd=ROOT, capture_output=True, text=True, check=False,
                )
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_build_dir_that_reaches_the_checkout_is_refused(self) -> None:
        """An overridden BUILD_DIR may not be the checkout, an ancestor, or a non-build/ subtree.

        `make clean BUILD_DIR=<x>` deletes <x>, so `BUILD_DIR=.` or `BUILD_DIR=src` would
        wipe sources or private inputs exactly as `BUILD_ROOT=.` would. The probe is
        `-n clean`: the refusal happens while Make parses, before the profile records
        are written, so a held guard writes nothing at all. Were it lost, the dry run
        would still delete nothing (it prints the deletion), though the parse would
        write profile entries into the named directory.
        """
        if not self.make:
            self.skipTest("GNU Make is required")
        refused = {
            ".": "the repository root or one of its ancestors",
            ROOT.as_posix(): "the repository root or one of its ancestors",
            "src": "inside the checkout but outside its build tree",
            "place_game_here": "inside the checkout but outside its build tree",
        }
        for value, reason in refused.items():
            with self.subTest(build_dir=value):
                proc = self._lifecycle_make("-n", "clean", f"BUILD_DIR={value}")
                self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertIn(f"BUILD_DIR '{value}' is {reason}", proc.stderr)
                self.assertNotIn("rmtree", proc.stdout)

    def test_build_dir_outside_the_checkout_is_accepted(self) -> None:
        """A BUILD_DIR outside the checkout passes the scope check and is the one named.

        Only a scratch path is probed. A BUILD_DIR accepted beneath the checkout's build/
        is not run here: the parse would write its profile entries into the checkout's
        build/ tree, which is exactly what this suite must not do.
        """
        if not self.make:
            self.skipTest("GNU Make is required")
        outside = self.build_root / "outside-title"
        proc = self._lifecycle_make("-n", "clean", f"BUILD_DIR={outside.as_posix()}")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(outside.as_posix(), proc.stdout)

    def test_build_root_with_a_space_fails_closed(self) -> None:
        if not self.make:
            self.skipTest("GNU Make is required")
        proc = subprocess.run(
            [self.make, "--no-print-directory", "help",
             f"BUILD_ROOT={self.build_root.as_posix()} extra"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("BUILD_ROOT", proc.stderr)
        self.assertIn("contains a space", proc.stderr)

    def test_checkout_scope_refusal_outranks_the_space_refusal(self) -> None:
        """A spaced root that reaches the checkout is refused for the checkout, not the space.

        A checkout may live under a path with spaces, so its parent -- the root a
        typo most plausibly names -- is spaced too. When the space refusal ran first
        it hid the safety reason, which is how the relocated-clone check failed. The
        spellings below carry a space wherever the checkout lives, so the order is
        pinned in every checkout; tools/test_relocated_clone.py repeats it from a
        genuinely spaced checkout. A spaced root inside build/ or outside the
        checkout is still refused for its space.
        """
        if not self.make:
            self.skipTest("GNU Make is required")
        cases = {
            "x y/..": "the repository root or one of its ancestors",
            "src/x y": "inside the checkout but outside its build tree",
            "build/x y": "contains a space",
            f"{self.scratch.as_posix()}/x y": "contains a space",
        }
        for value, reason in cases.items():
            with self.subTest(build_root=value):
                proc = subprocess.run(
                    [self.make, "--no-print-directory", "help", f"BUILD_ROOT={value}"],
                    cwd=ROOT, capture_output=True, text=True, check=False,
                )
                self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertIn(reason, proc.stderr)
                if reason != "contains a space":
                    self.assertNotIn("contains a space", proc.stderr)

    def test_clean_removes_specified_build_dir(self) -> None:
        """make clean BUILD_DIR=<target> must remove the specified directory without touching other paths."""
        target_dir = self.build_root / "clean-target"
        self._plant(target_dir / "sample_artifact.o", "dummy")
        sibling = self._plant(self.build_root / "other-title" / "keep.o")

        proc = self._lifecycle_make("clean", f"BUILD_DIR={target_dir.as_posix()}")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertFalse(target_dir.exists(), f"Target dir {target_dir} was not cleaned")
        self.assertTrue(sibling.is_file(), "clean removed another title's build tree")

    def test_clean_fixtures_removes_fixture_subdirs(self) -> None:
        """make clean-fixtures must remove the smoke, cosim, oracle and verify trees under BUILD_ROOT."""
        fixture_dirs = [
            self.build_root / name
            for name in ("production-smoke", "production-smoke-gap", "cosim",
                         "nakagawa_psp_oracle", "vfpu_oracle", "portable-core",
                         "verify", "link")
        ]
        for fdir in fixture_dirs:
            self._plant(fdir / "artifact.tmp", "tmp")
        title_build = self._plant(self.build_root / "private-title" / "title.exe")

        proc = self._lifecycle_make("clean-fixtures")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        for fdir in fixture_dirs:
            self.assertFalse(fdir.exists(), f"Fixture directory {fdir} was not cleaned")
        self.assertTrue(title_build.is_file(), "clean-fixtures removed a title build tree")

    def test_distclean_and_tidy_preserve_binaries_while_cleaning_objects_and_ephemeral_logs(self) -> None:
        """distclean and tidy must preserve .exe and .pdb while removing .o, .d, and ephemeral logs."""
        for goal in ("distclean", "tidy"):
            with self.subTest(goal=goal):
                test_dir = self.build_root / goal
                exe_file = self._plant(test_dir / "mygame.exe", "binary")
                pdb_file = self._plant(test_dir / "mygame.pdb", "symbols")
                obj_file = self._plant(test_dir / "mygame.o", "object")
                dep_file = self._plant(test_dir / "mygame.d", "deps")
                ephemeral_logs = [self._plant(self.log_dir / name, "ephemeral log")
                                  for name in EPHEMERAL_LOG_NAMES]
                evidence_log = self._plant(self.log_dir / "oracle_run.log", "evidence")

                proc = self._lifecycle_make(goal, f"BUILD_DIR={test_dir.as_posix()}")
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertTrue(exe_file.is_file(), f"{goal} deleted .exe")
                self.assertTrue(pdb_file.is_file(), f"{goal} deleted .pdb")
                self.assertFalse(obj_file.exists(), f"{goal} did not delete .o")
                self.assertFalse(dep_file.exists(), f"{goal} did not delete .d")
                for log in ephemeral_logs:
                    self.assertFalse(log.exists(), f"{goal} did not clean ephemeral log {log.name}")
                self.assertTrue(evidence_log.is_file(), f"{goal} deleted a non-ephemeral log")

    def test_clean_all_cleans_all_build_subdirs_and_ephemeral_logs(self) -> None:
        """clean-all must remove everything under BUILD_ROOT and the ephemeral build logs."""
        sub_a = self._plant(self.build_root / "test_sub_a" / "test.bin", "a").parent
        sub_b = self._plant(self.build_root / "test_sub_b" / "test.bin", "b").parent
        loose = self._plant(self.build_root / "loose.bin")
        recomp_log = self._plant(self.log_dir / "recomp_err.log", "err")

        proc = self._lifecycle_make("clean-all")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertFalse(sub_a.exists(), f"Subdir {sub_a} was not cleaned by clean-all")
        self.assertFalse(sub_b.exists(), f"Subdir {sub_b} was not cleaned by clean-all")
        self.assertFalse(loose.exists(), f"File {loose} was not cleaned by clean-all")
        self.assertEqual(list(self.build_root.iterdir()), [], "clean-all left BUILD_ROOT populated")
        self.assertFalse(recomp_log.exists(), f"Log {recomp_log} was not cleaned by clean-all")

    def test_build_dir_with_a_space_fails_closed_before_any_recipe_runs(self) -> None:
        """A BUILD_DIR Make cannot name must be refused, not silently fragmented.

        Before the guard, Make split the whitespace-bearing target name derived
        from BUILD_DIR into several targets: `clean` removed the FIRST fragment --
        a directory the caller never named -- and the parse-time mkdir created
        directories named after the remaining fragments in the repository root.
        """
        victim = self.build_root / "guard-victim"
        keep = self._plant(victim / "keep.o", "object")
        spaced = victim.as_posix() + " extra"
        proc = self._lifecycle_make("clean", f"BUILD_DIR={spaced}")
        self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("contains a space", proc.stdout + proc.stderr)
        self.assertIn("NK_BUILD_ROOT", proc.stdout + proc.stderr)
        self.assertTrue(keep.is_file(), "clean removed a path outside the named BUILD_DIR")
        # The refused run must not leave Make-fragment directories behind either.
        self.assertFalse(
            (ROOT / "spaces").exists(),
            "a refused BUILD_DIR created a spaces/ tree in the repository root",
        )

    def test_clean_targets_never_delete_protected_paths(self) -> None:
        protected_dirs = [
            ROOT / "assets",
            ROOT / "fixtures",
            ROOT / "src",
            ROOT / "tools",
            ROOT / "docs",
        ]
        for pdir in protected_dirs:
            self.assertTrue(pdir.is_dir(), f"Protected directory {pdir} must exist")

        # A non-ephemeral log beside the ephemeral ones must survive every clean target.
        evidence_log = self._plant(self.log_dir / "evidence_run_test.log", "evidence data")

        proc = self._lifecycle_make("clean-all")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

        self.assertTrue(evidence_log.is_file(), "clean-all deleted non-ephemeral evidence log")
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
            [self.make, "--no-print-directory", "compiler-info",
             f"BUILD_ROOT={_scratch_build_root(self, 'vulkan-info').as_posix()}"],
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
        script_text = (ROOT / "tools" / "copy_build_assets.ps1").read_text(encoding="utf-8")
        self.assertIn("Get-Command gcc", script_text)
        self.assertIn("SDL3.dll", script_text)

    def test_sdl3_discovery_in_makefile_records_identity_and_version(self) -> None:
        """Makefile compiler-info must discover and report SDL3_DIR, SDL3_PROVIDER, and SDL3_VERSION."""
        if not self.make:
            self.skipTest("GNU Make is required")
        proc = subprocess.run(
            [self.make, "--no-print-directory", "compiler-info",
             f"BUILD_ROOT={_scratch_build_root(self, 'sdl3-info').as_posix()}"],
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
        # Windows definition derives from $(VULKAN_SDK); the path stays quoted so
        # a spaced SDK root remains a single shell word (#368).
        self.assertRegex(
            makefile_text, r'VULKAN_INC_FLAGS\s*:=\s*-I"\$\(VULKAN_SDK\)/Include'
        )
        vulkan_cflags_pos = cflags.index("$(VULKAN_INC_FLAGS)")
        self.assertLess(sdl3_cflags_pos, vulkan_cflags_pos, "SDL3 includes must precede Vulkan SDK in CFLAGS")

        ldflags_match = re.search(r"LDFLAGS\s*\?=\s*([^\r\n]+)", makefile_text)
        self.assertIsNotNone(ldflags_match, "LDFLAGS must be defined")
        ldflags = ldflags_match.group(1)
        self.assertIn("$(SDL3_LDFLAGS)", ldflags)
        sdl3_ld_pos = ldflags.index("$(SDL3_LDFLAGS)")
        vulkan_ld_pos = ldflags.index('-L"$(VULKAN_SDK)')
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
            [self.make, "--no-print-directory", "sdl3-check", "SDL3_DIR=C:/nonexistent_sdl3_repro_test",
             f"BUILD_ROOT={_scratch_build_root(self, 'sdl3-absent').as_posix()}"],
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
        script_text = (ROOT / "tools" / "copy_build_assets.ps1").read_text(encoding="utf-8")
        self.assertIn("[string]$Sdl3DllPath", script_text)
        self.assertIn("Copy-Item $Sdl3DllPath", script_text)

        makefile_text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("-Sdl3DllPath \"$(SDL3_DLL)\"", makefile_text)

    def test_copy_build_assets_helper_is_a_tools_script_everywhere_it_is_named(self) -> None:
        """The post-link asset copy helper is tools/copy_build_assets.ps1.

        The Makefile step, the publication policy and the PowerShell debt
        inventory are the three places that resolve the helper by path. If any
        of them still names the repository root, `make compile` stops staging
        SDL3, the runtime DLLs and font/ without saying so.
        """
        helper = ROOT / "tools" / "copy_build_assets.ps1"
        self.assertTrue(helper.is_file(), "tools/copy_build_assets.ps1 is missing")
        self.assertFalse(
            (ROOT / "copy_build_assets.ps1").exists(),
            "the helper still exists at the repository root",
        )

        makefile_text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn(
            "$(POWERSHELL) -NoProfile -ExecutionPolicy Bypass -File tools/copy_build_assets.ps1",
            makefile_text,
        )
        self.assertNotIn(
            "-File copy_build_assets.ps1", makefile_text,
            "the Makefile asset copy step still names the repository root",
        )

        policy = json.loads(
            (ROOT / "assets" / "public_source_profile.json").read_text(encoding="utf-8"))
        self.assertIn("tools/copy_build_assets.ps1", policy["include_paths"])
        self.assertNotIn(
            "copy_build_assets.ps1", policy["include_paths"],
            "the public source profile still publishes the repository root path",
        )

        import publish_audit

        self.assertIn("tools/copy_build_assets.ps1",
                      publish_audit.POWERSHELL_SILENTLY_CONTINUE_INVENTORY)
        self.assertNotIn("copy_build_assets.ps1",
                         publish_audit.POWERSHELL_SILENTLY_CONTINUE_INVENTORY)


    def test_powershell_debt_inventory_is_enforced_per_script(self) -> None:
        """Each script keeps its own SilentlyContinue ceiling; the total is their sum."""
        import tempfile

        import publish_audit

        inventory = publish_audit.POWERSHELL_SILENTLY_CONTINUE_INVENTORY
        self.assertEqual(sum(inventory.values()),
                         publish_audit.DEBT_BUDGETS["powershell_silently_continue"])
        # The repo-wide case below is only meaningful if git actually lists the scripts;
        # _debt_budget_findings falls back to an empty path list when git fails.
        import subprocess
        tracked_scripts = subprocess.run(
            ["git", "ls-files", "*.ps1"], cwd=ROOT, capture_output=True, text=True, check=True,
        ).stdout.split()
        self.assertTrue(set(inventory) <= set(tracked_scripts),
                        "every inventoried script must be a tracked .ps1 the audit inspects")
        self.assertEqual(
            [f for f in publish_audit._debt_budget_findings() if "SilentlyContinue" in f.detail],
            [],
        )
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            (root / "tools").mkdir()
            (root / "tools" / "copy_build_assets.ps1").write_text(
                "$ErrorActionPreference = 'SilentlyContinue'\n" * 4, encoding="utf-8")
            (root / "unlisted.ps1").write_text(
                "Get-Item x -ErrorAction SilentlyContinue\n", encoding="utf-8")
            findings = publish_audit._debt_budget_findings(
                root, ["tools/copy_build_assets.ps1", "unlisted.ps1"])
        flagged = {f.path for f in findings if "SilentlyContinue" in f.detail}
        self.assertEqual(flagged, {"tools/copy_build_assets.ps1", "unlisted.ps1"})

    def test_no_tracked_file_names_the_root_level_copy_build_assets_path(self) -> None:
        """A relocation is complete only when nothing still names the old path.

        Every tracked mention of the helper must carry its `tools/` directory,
        so a reader and every tool that resolves the path land on the same
        file. The generated inventories are the only records allowed to lag;
        the maintainer regenerates those with provenance-refresh.
        """
        # A mention is qualified when its `tools` directory component sits
        # immediately before it, in plain path form (`tools/...`) or in the
        # `ROOT / "tools" / "..."` form the tree-wide Python tests use.
        mention = re.compile(r"copy_build_assets\.ps1")
        qualified = re.compile(r"tools[\"'\\ /]*$")
        # The generated inventories are rewritten by provenance-refresh, and this
        # module has to name the old path in order to assert that nothing else does.
        allowed = {
            "assets/public_provenance_ledger.json",
            "PUBLIC_EXPORT.json",
            "docs/provenance/MODIFIED_FILE_NOTICES.json",
            "tools/test_build_truth.py",
        }
        if shutil.which("git") is None:
            self.skipTest("git is required to enumerate the tracked tree")
        listing = subprocess.run(
            ["git", "-C", str(ROOT), "ls-files", "-z"],
            capture_output=True, text=True, check=True).stdout
        offenders = []
        for entry in sorted(filter(None, listing.split("\0"))):
            if entry in allowed:
                continue
            try:
                text = (ROOT / entry).read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            for match in mention.finditer(text):
                window = text[max(0, match.start() - 32):match.start()]
                if not qualified.search(window):
                    offenders.append(entry)
                    break
        self.assertEqual(
            offenders, [],
            "these tracked files still name the root-level copy_build_assets.ps1 path",
        )

    def test_windows_package_build_uses_builtin_windows_powershell(self) -> None:
        """A consumer build must not need a separately installed PowerShell 7."""
        makefile_text = (Path(__file__).resolve().parents[1] / "Makefile").read_text(encoding="utf-8")
        script_text = (Path(__file__).resolve().parents[1] / "tools" / "copy_build_assets.ps1").read_text(encoding="utf-8")
        self.assertIn("POWERSHELL ?= powershell.exe", makefile_text)
        self.assertIn("$(POWERSHELL) -NoProfile -ExecutionPolicy Bypass -File tools/copy_build_assets.ps1", makefile_text)
        self.assertIn("#requires -Version 5.1", script_text)

    def test_runtime_profile_records_sdl3_identity(self) -> None:
        """runtime_profile.json must record SDL3_PROVIDER, SDL3_VERSION, and SDL3_DIR."""
        if not self.make:
            self.skipTest("GNU Make is required")
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_build = Path(tmpdir) / "build_test"
            proc = subprocess.run(
                [self.make, "--no-print-directory", f"BUILD_DIR={tmp_build.as_posix()}",
                 f"BUILD_ROOT={Path(tmpdir).as_posix()}", "compiler-info"],
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
        self.assertEqual(values["SDL3_INC_FLAGS"], '-I"C:/msys64/ucrt64/include"')
        self.assertEqual(values["SDL3_LDFLAGS"], '-L"C:/msys64/ucrt64/lib"')

    def test_a_spaced_toolchain_root_stays_a_single_shell_word(self) -> None:
        """Spaced paths (#368): an unquoted spaced -I/-L splits into phantom compiler arguments."""
        values = self._fragment(self._provider(
            "msys2_ucrt64", "C:/Program Files/SDL3/include",
            "C:/Program Files/SDL3/lib/libSDL3.dll.a"))
        self.assertEqual(values["SDL3_INC_FLAGS"], '-I"C:/Program Files/SDL3/include"')
        self.assertEqual(values["SDL3_LDFLAGS"], '-L"C:/Program Files/SDL3/lib"')

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
        self.assertEqual(values["SDL3_LDFLAGS"], '-L"C:/msys64/ucrt64/lib"')

    def test_makefile_discovers_sdl3_once_per_parse(self) -> None:
        makefile_text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertEqual(sum("write_sdl3_make_fragment" in line for line in makefile_text.splitlines()), 1)
        self.assertNotIn("query_sdl3_info", makefile_text)


class ProfileEntriesEnvTransportTests(unittest.TestCase):
    """Runtime profile entries with quotes/spaces travel through the environment (#368).

    A flag list such as CFLAGS now carries quoted -I/-L paths; handing that text
    to `build_profile hash --entry` on a command line would let cmd.exe/sh split
    it into phantom arguments before Python ever sees it. The environment is the
    transport that survives, matching GAME_EXTRA_ELFS_ENV, and the digest must
    not depend on which transport was used.
    """

    ENV_VAR = "NK_TEST_PROFILE_ENTRIES"

    def _run(self, args: list[str], env) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(PROFILE_TOOL), *args],
            capture_output=True, text=True, encoding="utf-8",
            errors="replace", check=False, env=env,
        )

    @staticmethod
    def _compiler() -> str:
        return shutil.which("python") or shutil.which("python3") or sys.executable

    def test_entries_env_hash_matches_identical_argv_entries(self) -> None:
        entries = [
            'CFLAGS=-O0 -I"C:/a b/include" -DSR_BUILD_DIR=\\"build/x\\"',
            "SDL3_DIR=C:/a b/sdl3",
        ]
        argv_args = ["hash", "--compiler", self._compiler()]
        for entry in entries:
            argv_args += ["--entry", entry]
        argv_run = self._run(argv_args, dict(os.environ))
        env = dict(os.environ, **{self.ENV_VAR: "\n".join(entries)})
        env_run = self._run(
            ["hash", "--compiler", self._compiler(),
             "--entries-env", self.ENV_VAR],
            env,
        )
        self.assertEqual(argv_run.returncode, 0, argv_run.stderr)
        self.assertEqual(env_run.returncode, 0, env_run.stderr)
        self.assertEqual(argv_run.stdout.strip(), env_run.stdout.strip())

    def test_entries_env_and_argv_entries_conflict_fails_closed(self) -> None:
        env = dict(os.environ, **{self.ENV_VAR: "A=1"})
        proc = self._run(
            ["hash", "--compiler", self._compiler(),
             "--entry", "A=1", "--entries-env", self.ENV_VAR],
            env,
        )
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertIn("conflicting sources", proc.stderr)

    def test_entries_file_hash_matches_identical_env_entries(self) -> None:
        entries = [
            'CFLAGS=-O0 -I"C:/a b/include" -DSR_BUILD_DIR=\\"build/x\\"',
            "EXTRA_ELF_ARGS=--extra-elf=a.prx@runtime --extra-elf=b.prx@0x08900000",
        ]
        env = dict(os.environ, **{self.ENV_VAR: "\n".join(entries)})
        env_run = self._run(
            ["hash", "--compiler", self._compiler(), "--entries-env", self.ENV_VAR], env)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "entries"
            # Make's $(file ...) writes the value and a trailing newline.
            path.write_text("\n".join(entries) + "\n", encoding="utf-8", newline="\n")
            file_run = self._run(
                ["hash", "--compiler", self._compiler(), "--entries-file", str(path)],
                dict(os.environ))
            conflict = self._run(
                ["hash", "--compiler", self._compiler(), "--entries-file", str(path),
                 "--entry", "A=1"],
                dict(os.environ))
        self.assertEqual(env_run.returncode, 0, env_run.stderr)
        self.assertEqual(file_run.returncode, 0, file_run.stderr)
        self.assertEqual(env_run.stdout.strip(), file_run.stdout.strip())
        self.assertEqual(conflict.returncode, 2, conflict.stderr)
        self.assertIn("conflicting sources", conflict.stderr)
        missing = self._run(
            ["hash", "--compiler", self._compiler(), "--entries-file",
             str(Path(tempfile.gettempdir()) / "nakagawa-no-such-entries-file")],
            dict(os.environ))
        self.assertEqual(missing.returncode, 2, missing.stderr)
        self.assertIn("could not be read", missing.stderr)

    def test_unset_entries_env_fails_closed(self) -> None:
        env = dict(os.environ)
        env.pop(self.ENV_VAR, None)
        proc = self._run(
            ["hash", "--compiler", self._compiler(),
             "--entries-env", self.ENV_VAR],
            env,
        )
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertIn(self.ENV_VAR, proc.stderr)


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


#: Module-level functions that delete the path passed as their first argument.
_DELETING_FUNCTIONS = frozenset({
    ("shutil", "rmtree"), ("os", "remove"), ("os", "unlink"), ("os", "rmdir"), ("os", "removedirs"),
})
#: Path methods that delete the path they are called on.
_DELETING_METHODS = frozenset({"unlink", "rmdir"})
_MAKE_PROGRAMS = frozenset({"make", "mingw32-make", "gmake"})
#: A variable named for a make program (`make`, `make_name`, `self.make`).
_MAKE_PROGRAM_NAME = re.compile(r"(?:^|_)g?make(?:_|$)")
_SCRATCH_ROOT_VARIABLES = ("BUILD_ROOT", "LOG_DIR")
#: Calls that launch the process whose argv list is their first argument.
_PROCESS_LAUNCHERS = frozenset({"run", "Popen", "check_output", "check_call", "call"})
#: Make options that run another directory's Makefile, so the run builds in that tree.
_OTHER_TREE_MAKE_OPTIONS = frozenset({"-C", "--directory"})
#: A Make variable bound to a checkout build/ path literal, such as PLAYER_EXE=build/x.exe.
_BUILD_PATH_BINDING = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=(?:\./)?build[/\\]")
#: A Makefile line naming the checkout's build/ tree literally (not derived from BUILD_ROOT).
_LITERAL_BUILD_PATH = re.compile(r"(?<![\w$./\\-])build[/\\]|Path\(\s*['\"]build['\"]\s*\)")


def _suite_test_files() -> list[Path]:
    """Every Python test module the suite discovers under tools/ and tests/."""
    return sorted((ROOT / "tools").glob("test_*.py")) + sorted((ROOT / "tests").rglob("test_*.py"))


def _instance_attribute(node: ast.AST) -> str | None:
    """`self.x` / `cls.x` as `@x`, so instance state is tracked apart from locals."""
    if (isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name)
            and node.value.id in ("self", "cls")):
        return "@" + node.attr
    return None


#: Path methods and properties whose result is still a path at or under the receiver.
_PATH_DERIVING_ATTRIBUTES = frozenset({
    "absolute", "expanduser", "glob", "iterdir", "joinpath", "parent", "parents",
    "resolve", "rglob", "walk", "with_name", "with_stem", "with_suffix",
})
#: Callables that turn a path argument into a path (or its string spelling).
_PATH_CONSTRUCTORS = frozenset({
    "Path", "PurePath", "PosixPath", "WindowsPath", "str", "fspath",
    "join", "abspath", "realpath", "normpath", "dirname",
})


def _is_anchored(node: ast.AST, anchored: set[str]) -> bool:
    """True when ``node`` evaluates to a path derived from an anchored name.

    Only path-preserving operations carry the anchor (`/`, `.parent`, `.resolve()`,
    `.glob()`, `Path(...)`, `os.path.join(...)`, f-strings, containers), so data
    read from a checkout file (`json.loads((ROOT / "x.json").read_text())`) does
    not make everything computed from it look like a checkout path.
    """
    attribute = _instance_attribute(node)
    if attribute is not None:
        return attribute in anchored
    if isinstance(node, ast.Name):
        return node.id in anchored
    if isinstance(node, ast.Attribute):
        return node.attr in _PATH_DERIVING_ATTRIBUTES and _is_anchored(node.value, anchored)
    if isinstance(node, ast.Subscript):
        return _is_anchored(node.value, anchored)
    if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Div, ast.Add)):
        return _is_anchored(node.left, anchored)
    if isinstance(node, ast.Call):
        func = node.func
        if isinstance(func, ast.Attribute) and func.attr in _PATH_DERIVING_ATTRIBUTES:
            return _is_anchored(func.value, anchored)
        name = func.id if isinstance(func, ast.Name) else (
            func.attr if isinstance(func, ast.Attribute) else None)
        return name in _PATH_CONSTRUCTORS and any(
            _is_anchored(argument, anchored) for argument in node.args)
    if isinstance(node, ast.IfExp):
        return _is_anchored(node.body, anchored) or _is_anchored(node.orelse, anchored)
    if isinstance(node, (ast.List, ast.Tuple, ast.Set)):
        return any(_is_anchored(element, anchored) for element in node.elts)
    if isinstance(node, (ast.Starred, ast.FormattedValue, ast.NamedExpr)):
        return _is_anchored(node.value, anchored)
    if isinstance(node, ast.JoinedStr):
        return any(_is_anchored(value, anchored) for value in node.values)
    return False


def _bound_names(target: ast.AST) -> list[str]:
    """Names a binding target rebinds; item or attribute stores rebind nothing."""
    attribute = _instance_attribute(target)
    if attribute is not None:
        return [attribute]
    if isinstance(target, ast.Name):
        return [target.id]
    if isinstance(target, ast.Starred):
        return _bound_names(target.value)
    if isinstance(target, (ast.Tuple, ast.List)):
        return [name for element in target.elts for name in _bound_names(element)]
    return []


def _assignments(scope: ast.AST) -> list[tuple[list[str], ast.AST]]:
    """(bound names, value) for every assignment and for-loop binding in ``scope``."""
    bindings: list[tuple[list[str], ast.AST]] = []
    for node in ast.walk(scope):
        if isinstance(node, ast.Assign):
            targets, value = node.targets, node.value
        elif isinstance(node, (ast.AnnAssign, ast.AugAssign)) and node.value is not None:
            targets, value = [node.target], node.value
        elif isinstance(node, (ast.For, ast.AsyncFor, ast.comprehension)):
            targets, value = [node.target], node.iter
        elif isinstance(node, ast.withitem) and node.optional_vars is not None:
            targets, value = [node.optional_vars], node.context_expr
        else:
            continue
        names = [name for target in targets for name in _bound_names(target)]
        if names:
            bindings.append((names, value))
    return bindings


def _propagate(bindings: list[tuple[list[str], ast.AST]], anchored: set[str]) -> set[str]:
    """Close ``anchored`` over ``bindings``: a name bound from an anchored value is anchored."""
    anchored = set(anchored)
    changed = True
    while changed:
        changed = False
        for names, value in bindings:
            if not set(names) <= anchored and _is_anchored(value, anchored):
                anchored.update(names)
                changed = True
    return anchored


def _is_make_program(node: ast.AST) -> bool:
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return Path(node.value).stem.lower() in _MAKE_PROGRAMS
    if isinstance(node, ast.Name):
        return _MAKE_PROGRAM_NAME.search(node.id.lower()) is not None
    return isinstance(node, ast.Attribute) and _MAKE_PROGRAM_NAME.search(node.attr.lower()) is not None


def _literal_prefix(node: ast.AST) -> str:
    """The constant text an argv element starts with (an f-string's leading literal)."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.JoinedStr) and node.values:
        first = node.values[0]
        if isinstance(first, ast.Constant) and isinstance(first.value, str):
            return first.value
    return ""


def _literal_text(node: ast.AST) -> str | None:
    """The text of a constant argv element, or None when it is not a constant string."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    return None


def _is_goal_like(node: ast.AST) -> bool:
    """True for an argv element that may name a Make goal rather than an option or NAME=value."""
    if isinstance(node, ast.Starred):
        return True
    text = _literal_text(node)
    if text is not None:
        return bool(text) and not text.startswith("-") and "=" not in text
    prefix = _literal_prefix(node)
    return "=" not in prefix and not prefix.startswith("-")


def _call_name(call: ast.Call) -> str | None:
    if isinstance(call.func, ast.Name):
        return call.func.id
    if isinstance(call.func, ast.Attribute):
        return call.func.attr
    return None


def _argv_extensions(scope: ast.AST, name: str) -> list[ast.AST]:
    """Elements added to the argv list ``name`` by `name += [...]`, `.append(x)` or `.extend(xs)`."""
    added: list[ast.AST] = []
    for node in ast.walk(scope):
        if isinstance(node, ast.AugAssign) and isinstance(node.target, ast.Name) \
                and node.target.id == name:
            added.extend(node.value.elts if isinstance(node.value, ast.List) else [node.value])
        elif (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
              and node.func.attr in ("append", "extend") and isinstance(node.func.value, ast.Name)
              and node.func.value.id == name):
            for argument in node.args:
                if node.func.attr == "extend" and isinstance(argument, ast.List):
                    added.extend(argument.elts)
                else:
                    added.append(argument)
    return added


def _checkout_scopes(tree: ast.Module) -> list[tuple[ast.AST, set[str]]]:
    """Each function and module-level statement, with the names anchored to the checkout there.

    A path is *checkout-anchored* when it is computed from `__file__` (the usual
    `ROOT = Path(__file__).resolve().parents[1]`), from a module-level name derived
    from it (`ROOT`, `TOOLS`, `ROOT / "build"` ...), or from a local or `self`/`cls`
    attribute bound to such a value.
    """
    module_level = [node for node in tree.body if not isinstance(
        node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))]
    module_bindings: list[tuple[list[str], ast.AST]] = []
    for node in module_level:
        module_bindings.extend(_assignments(node))
    anchored_module = _propagate(module_bindings, {"__file__"})

    # Attributes a class binds from anchored values in any of its methods. Each
    # method's locals stay its own; only `self.x` / `cls.x` flow between methods.
    class_attrs: dict[ast.ClassDef, set[str]] = {}
    for cls in (n for n in ast.walk(tree) if isinstance(n, ast.ClassDef)):
        methods = [n for n in cls.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]
        attrs: set[str] = set()
        while True:
            found = set(attrs)
            for method in methods:
                found |= {name for name in _propagate(_assignments(method), anchored_module | attrs)
                          if name.startswith("@")}
            if found == attrs:
                break
            attrs = found
        class_attrs[cls] = attrs

    owner: dict[ast.AST, ast.ClassDef] = {}
    for cls in class_attrs:
        for child in ast.walk(cls):
            owner.setdefault(child, cls)

    functions = [n for n in ast.walk(tree) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]
    nested = {child for fn in functions for child in ast.walk(fn) if child is not fn
              and isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef))}
    scopes: list[tuple[ast.AST, set[str]]] = []
    for fn in functions:
        if fn in nested:
            continue  # scanned with its enclosing function, whose bindings it closes over
        anchored = set(anchored_module)
        cls = owner.get(fn)
        if cls is not None:
            anchored |= class_attrs[cls]
        scopes.append((fn, _propagate(_assignments(fn), anchored)))
    scopes.extend((node, anchored_module) for node in module_level)
    return scopes


def _scan_for_checkout_damage(source: str, label: str) -> list[str]:
    """Report deletions and Make runs that could damage the real checkout's build/ or logs/.

    * `shutil.rmtree`, `os.remove`/`unlink`/`rmdir`, `Path.unlink`/`rmdir` (called
      directly or registered with `addCleanup`) on a checkout-anchored path;
    * a destructive Make goal (clean, clean-fixtures, distclean, tidy, clean-all)
      without scratch `BUILD_ROOT=` and `LOG_DIR=` overrides that are not
      checkout-anchored;
    * any other Make run -- a list starting with make that is passed straight to a
      process launcher or assigned to a name -- for a goal other than `help` without
      a scratch `BUILD_ROOT=`. Parsing a Makefile writes beneath BUILD_ROOT even for
      a dry run: the SDL3 discovery fragment, and a named title's profile stamps and
      the objects they invalidate;
    * a Make run that binds a variable to a checkout build/ path literal, such as
      `PLAYER_EXE=build/x.exe`.

    A run is not checked when it names another tree with `-C`, or when its `cwd=`
    is given and is not checkout-anchored (it then builds in that tree). Lists that
    are assertion arguments, dictionary values or arguments to other helpers are
    not runs and are not followed. A goal held in a variable is invisible to the
    scan; `_run_scratch_make` refuses checkout roots at run time.
    """
    tree = ast.parse(source, label)
    parents = {child: parent for parent in ast.walk(tree)
               for child in ast.iter_child_nodes(parent)}
    findings: list[tuple[int, str]] = []

    def report(node: ast.AST, message: str) -> None:
        findings.append((node.lineno, message))

    def check_make_run(node: ast.List, scope: ast.AST, is_anchored) -> None:
        elements = list(node.elts)
        if not elements or not _is_make_program(elements[0]):
            return
        parent = parents.get(node)
        foreign_tree = False
        if isinstance(parent, (ast.Assign, ast.AnnAssign, ast.AugAssign)) and parent.value is node:
            # A run assembled in a variable: its later `name += [...]`, `name.append(x)`
            # and `name.extend(xs)` elements belong to the same argv.
            targets = parent.targets if isinstance(parent, ast.Assign) else [parent.target]
            for target in targets:
                if isinstance(target, ast.Name):
                    elements.extend(_argv_extensions(scope, target.id))
        elif (isinstance(parent, ast.Call) and parent.args[:1] == [node]
              and _call_name(parent) in _PROCESS_LAUNCHERS):
            cwd = next((kw.value for kw in parent.keywords if kw.arg == "cwd"), None)
            foreign_tree = cwd is not None and not is_anchored(cwd)
        else:
            return  # data, an assertion's expected argv, or an argument to another helper

        for element in elements:
            argument = _literal_prefix(element)
            if _BUILD_PATH_BINDING.match(argument):
                report(node, f"passes {argument.partition('=')[0]}= a checkout build/ path "
                             f"({argument}); the run writes its output into the real build/ tree")

        destructive = sorted({e.value for e in elements if _literal_text(e) in DESTRUCTIVE_MAKE_GOALS})
        if destructive:
            for variable in _SCRATCH_ROOT_VARIABLES:
                overrides = [e for e in elements if _literal_prefix(e).startswith(variable + "=")]
                if not overrides:
                    report(node, f"runs destructive Make goal(s) {destructive} without a scratch "
                                 f"{variable}=; use _run_scratch_make")
                elif any(is_anchored(e) for e in overrides):
                    report(node, f"runs destructive Make goal(s) {destructive} with {variable} "
                                 f"inside the repository checkout")
            return
        if foreign_tree or any(_literal_text(e) in _OTHER_TREE_MAKE_OPTIONS for e in elements):
            return
        goal_like = [e for e in elements[1:] if _is_goal_like(e)]
        if not goal_like or all(_literal_text(e) == "help" for e in goal_like):
            return
        goals = [e.value for e in goal_like if _literal_text(e)] or ["the default goal"]
        relocations = [e for e in elements if _literal_prefix(e).startswith("BUILD_ROOT=")]
        if not relocations:
            report(node, f"runs Make for {goals} without a scratch BUILD_ROOT=; parsing writes the "
                         "checkout's build/ (the SDL3 discovery fragment, and a named title's "
                         "profile stamps and objects)")
        elif all(is_anchored(e) for e in relocations):
            report(node, f"runs Make for {goals} with BUILD_ROOT inside the repository checkout")

    for scope, anchored in _checkout_scopes(tree):
        def is_anchored(expr: ast.AST, anchored: set[str] = anchored) -> bool:
            return _is_anchored(expr, anchored)

        for node in ast.walk(scope):
            if isinstance(node, ast.Call):
                func = node.func
                target = None
                if (isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name)
                        and (func.value.id, func.attr) in _DELETING_FUNCTIONS):
                    target = node.args[0] if node.args else None
                elif isinstance(func, ast.Name) and func.id == "rmtree":
                    target = node.args[0] if node.args else None
                elif isinstance(func, ast.Attribute) and func.attr in _DELETING_METHODS:
                    target = func.value
                elif (isinstance(func, ast.Attribute)
                      and func.attr in ("addCleanup", "addClassCleanup") and node.args):
                    callback = node.args[0]
                    if (isinstance(callback, ast.Attribute) and isinstance(callback.value, ast.Name)
                            and (callback.value.id, callback.attr) in _DELETING_FUNCTIONS) or (
                            isinstance(callback, ast.Name) and callback.id == "rmtree"):
                        target = node.args[1] if len(node.args) > 1 else None
                    elif isinstance(callback, ast.Attribute) and callback.attr in _DELETING_METHODS:
                        target = callback.value
                if target is not None and is_anchored(target):
                    report(node, f"deletes a path inside the repository checkout: "
                                 f"{ast.unparse(target)}")
            elif isinstance(node, ast.List):
                check_make_run(node, scope, is_anchored)
    findings.sort(key=lambda finding: finding[0])
    return [f"{label}:{line}: {message}" for line, message in findings]


def _makefile_literal_build_paths(text: str, label: str) -> list[str]:
    """Makefile lines that name the checkout's build/ tree literally.

    Every output a recipe or variable makes must derive from BUILD_ROOT, so a default
    run writes beneath build/ and an overridden run writes beneath the caller's root.
    Comments, help text and the error messages that describe the default are not
    recipes and are skipped; `fixtures/.../build/` is a source fixture, not an output.
    """
    findings: list[str] = []
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.lstrip()
        if (stripped.startswith("#") or stripped.startswith("HELP_DESCRIPTION_")
                or "$(error" in line or "$(warning" in line):
            continue
        if _LITERAL_BUILD_PATH.search(line):
            findings.append(f"{label}:{number}: {line.strip()}")
    return findings


#: sdl3vk.c holds product code and the GPU capture selftest in one file. Its test-only
#: section runs from the selftest banner to the first product function after it, so the
#: scan covers that span and nothing else.
_SDL3VK_C = "src/rt/gpu_sdl3vk/sdl3vk.c"
_SDL3VK_SELFTEST_BEGIN = "/* ---- capture selftest (issue #57)"
_SDL3VK_SELFTEST_END = "void sdl3vk_shutdown(void) {"
#: One C comment, string literal or character literal. Literals are matched whole, so a
#: `//` or `/*` inside a string is never taken for a comment.
_C_LEXEME = re.compile(r"//[^\n]*|/\*.*?\*/|\"(?:[^\"\\\n]|\\.)*\"|'(?:[^'\\\n]|\\.)*'", re.S)
#: A string literal, as its raw C spelling, that names the checkout's build/ tree: exactly
#: `build`, or a `build` segment that starts the literal or follows a space, `>`, `=`, `:`,
#: a slash or a backslash and is followed by a separator. That covers `build/x`, `build\\x`,
#: the cwd-relative `%s\\build\\name` form the HLE selftest used, and a shell `> build/x`.
#: A build segment glued to a format conversion (`%s%cbuild%c`) is not matched: those are
#: built under a temporary root by the tests that use them.
_C_BUILD_LITERAL = re.compile(r"^build$|(?:^|[\s>=:/\\])build(?:/|\\\\)")
#: The macro's own default is the one place the scan allows the bare build root.
_C_BUILD_ROOT_DEFAULT = re.compile(r"^\s*#\s*define\s+SR_SELFTEST_BUILD_ROOT\b")
#: Literal spellings a test keeps on purpose, each with its reason. Every entry must still
#: occur in its file, so a stale entry fails instead of quietly allowing a new write.
_C_KEPT_BUILD_LITERALS: dict[tuple[str, str], str] = {
    (_SDL3VK_C, "build/snapshots/frame_0012.ppm"):
        "product default compared against sr_fbcap_path(): a string check, nothing is written",
    (_SDL3VK_C, "build/snapshots"):
        "product FBSNAP directory, cwd-relative: sdl3vk_capture_selftest runs in a scratch "
        "working directory beneath SR_SELFTEST_BUILD_ROOT, so it resolves only there",
    (_SDL3VK_C, "build/snapshots/selftest_frame.ppm"):
        "FBSNAP publication target, cwd-relative inside the same scratch working directory",
    (_SDL3VK_C, "build"):
        "cwd-relative removal (cap_test_rmdir), acting only inside the same scratch working "
        "directory, never the checkout's build/",
    ("tests/native/test_launch_resolution.c", "build"):
        "strstr() over the product's resolved executable path: a string read, nothing is written",
}


def _test_only_c_sources() -> list[tuple[str, str, int]]:
    """(repo-relative label, source text, first line) for each test-only C source."""
    sources: list[tuple[str, str, int]] = []
    for path in (sorted((ROOT / "tests" / "native").glob("*.c"))
                 + sorted((ROOT / "src" / "rt").glob("*selftest*.c"))):
        sources.append((path.relative_to(ROOT).as_posix(), path.read_text(encoding="utf-8"), 1))
    sdl3vk = (ROOT / _SDL3VK_C).read_text(encoding="utf-8")
    begin = sdl3vk.index(_SDL3VK_SELFTEST_BEGIN)
    end = sdl3vk.index(_SDL3VK_SELFTEST_END, begin)
    sources.append((_SDL3VK_C, sdl3vk[begin:end], sdl3vk.count("\n", 0, begin) + 1))
    return sources


def _c_build_literal_hits(source: str, first_line: int = 1) -> list[tuple[int, str, str]]:
    """(line, raw spelling, physical line) for each string literal naming the build/ tree.

    Comments and character literals are skipped; the literal is taken as spelled in the
    source, so `"build\\\\x"` is reported as `build\\\\x`.
    """
    lines = source.splitlines()
    hits: list[tuple[int, str, str]] = []
    for match in _C_LEXEME.finditer(source):
        lexeme = match.group(0)
        if not lexeme.startswith('"'):
            continue
        raw = lexeme[1:-1]
        if not _C_BUILD_LITERAL.search(raw):
            continue
        offset = source.count("\n", 0, match.start())
        hits.append((offset + first_line, raw, lines[offset]))
    return hits


def _c_build_literal_findings(source: str, label: str, first_line: int = 1) -> list[str]:
    """Literals that write under the checkout's build/ tree, not counting reasoned keeps."""
    findings: list[str] = []
    for number, raw, line in _c_build_literal_hits(source, first_line):
        if _C_BUILD_ROOT_DEFAULT.match(line) or (label, raw) in _C_KEPT_BUILD_LITERALS:
            continue
        findings.append(f"{label}:{number}: {line.strip()}")
    return findings


class CheckoutDeletionGuardTests(unittest.TestCase):
    """No test may delete from, or build into, the real checkout's build/ or logs/ trees.

    A developer's checkout keeps private title builds and packages under build/ and
    run logs under logs/. A test that cleans those trees, or runs a Make probe that
    writes there, destroys or invalidates them on every suite run; tools/test_build_truth.py
    once ran `clean-all` against the checkout itself, and several probes parsed the
    Makefile with the checkout's BUILD_ROOT. These scans keep that class of test from
    returning, and keep the Makefile's own outputs under BUILD_ROOT.
    """

    def test_no_test_deletes_from_or_builds_into_the_real_checkout(self) -> None:
        findings: list[str] = []
        for path in _suite_test_files():
            findings.extend(_scan_for_checkout_damage(
                path.read_text(encoding="utf-8"), path.relative_to(ROOT).as_posix()))
        self.assertEqual(
            findings, [],
            "tests delete from or build into the real repository checkout; point them at a "
            "scratch tree (tempfile, _run_scratch_make, or BUILD_ROOT=<scratch> on every "
            "Make run):\n" + "\n".join(findings),
        )

    def test_makefile_writes_only_beneath_build_root(self) -> None:
        findings: list[str] = []
        for path in [ROOT / "Makefile", *sorted((ROOT / "mk").glob("*.mk"))]:
            findings.extend(_makefile_literal_build_paths(
                path.read_text(encoding="utf-8"), path.relative_to(ROOT).as_posix()))
        self.assertEqual(
            findings, [],
            "Makefile outputs name the checkout's build/ tree literally; derive them from "
            "$(BUILD_ROOT) so a scratch BUILD_ROOT redirects every write:\n" + "\n".join(findings),
        )

    def test_makefile_literal_scan_names_each_hardcoded_build_path(self) -> None:
        makefile = textwrap.dedent('''\
            BUILD_ROOT ?= build
            PLAYER_EXE ?= build/nakagawa_player$(EXE_EXT)
            ASSET := fixtures/psp_oracle/build/nakagawa_psp_oracle.elf
            OUT := $(BUILD_ROOT)/x
            # build/comment is documentation
            $(error the checkout's build/ tree is refused)
            mk: ; $(PYTHON) -c "from pathlib import Path; Path('build').mkdir()"
        ''')
        findings = _makefile_literal_build_paths(makefile, "case.mk")
        self.assertEqual([int(f.split(":")[1]) for f in findings], [2, 7], "\n".join(findings))

    def test_test_only_c_writes_only_beneath_selftest_build_root(self) -> None:
        """Test-only C writes go through SR_SELFTEST_BUILD_ROOT, never a literal build/.

        A scratch run (BUILD_ROOT=<scratch>) must not create, modify or delete anything in the
        checkout. The Makefile passes -DSR_SELFTEST_BUILD_ROOT=$(BUILD_ROOT) to each of these
        binaries; a literal build/ path would silently ignore that and write into the checkout.
        """
        findings: list[str] = []
        for label, source, first_line in _test_only_c_sources():
            findings.extend(_c_build_literal_findings(source, label, first_line))
        self.assertEqual(
            findings, [],
            "test-only C sources name the checkout's build/ tree literally; build each scratch "
            "path from SR_SELFTEST_BUILD_ROOT (for example SR_SELFTEST_BUILD_ROOT \"/name\"), or "
            "run a product default from a scratch working directory:\n" + "\n".join(findings),
        )

    def test_c_build_literal_allowlist_entries_still_occur(self) -> None:
        present = {
            (label, raw)
            for label, source, first_line in _test_only_c_sources()
            for _, raw, _ in _c_build_literal_hits(source, first_line)
        }
        stale = sorted(f"{label} {raw!r}" for label, raw in _C_KEPT_BUILD_LITERALS
                       if (label, raw) not in present)
        self.assertEqual(stale, [], "kept build/ literals no longer occur; drop them from "
                         "_C_KEPT_BUILD_LITERALS:\n" + "\n".join(stale))

    def test_test_only_compiles_pass_the_selftest_build_root(self) -> None:
        """Each Makefile compile of a test-only source passes -DSR_SELFTEST_BUILD_ROOT.

        The C scan proves no source names build/ literally; this proves the Makefile feeds
        the macro. Without the define a scratch run would silently fall back to "build".
        """
        source_names = ("vfpu_tables_selftest.c", "fbcap_selftest.c", "cpu_lle_selftest.c",
                        "gpu_coherence_selftest.c", "gpu_capture_selftest.c")
        native = re.compile(r"tests/native/test_(?!ui_clip\.c)\w+\.c")
        define = r'-DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\"'
        checked: list[str] = []
        missing: list[str] = []
        for number, command in _logical_lines(ROOT.joinpath("Makefile").read_text(encoding="utf-8")):
            if "$(CC)" not in command:
                continue
            if not (native.search(command) or any(name in command for name in source_names)):
                continue
            checked.append(command)
            if define not in command:
                missing.append(f"Makefile:{number}: {command.strip()}")
        # Vacuity guard: the scan must actually reach the selftest and native test compiles.
        self.assertTrue(any("vfpu_tables_selftest.c" in c for c in checked), "no vfpu compile seen")
        self.assertTrue(any("tests/native/test_xb_parser.c" in c for c in checked),
                        "no native test compile seen")
        self.assertEqual(missing, [], "test-only compile commands without " + define + ":\n"
                         + "\n".join(missing))

    def test_c_build_literal_scan_names_each_hardcoded_path(self) -> None:
        source = textwrap.dedent(r'''
            #ifndef SR_SELFTEST_BUILD_ROOT
            #define SR_SELFTEST_BUILD_ROOT "build"
            #endif
            /* "build/in a comment is documentation" */
            // "build/in a line comment"
            static const char *a = "build/x.json";
            static const char *b = "build\\argv_echo_helper.exe";
            static const char *c = "build";
            static const wchar_t *d = L"build/wide.bin";
            static const char *e = SR_SELFTEST_BUILD_ROOT "/fine.json";
            static const char *f = "builder/x";
            static const char *g = "mybuild/x";
            static const char h = '"';
            static const char *i = "%s\\build\\archive_vfs_%lu";
            static const char *j = "%s%cbuild%cdisplay-smoke";
        ''')
        findings = _c_build_literal_findings(source, "case.c")
        self.assertEqual(
            [int(finding.split(":")[1]) for finding in findings],
            [7, 8, 9, 10, 15],
            "\n".join(findings),
        )

    def test_scan_reports_each_destructive_shape(self) -> None:
        source = textwrap.dedent('''
            import os, shutil, subprocess
            from pathlib import Path
            ROOT = Path(__file__).resolve().parents[1]
            BUILD = ROOT / "build"

            class Case:
                def setUp(self):
                    self.logs = ROOT / "logs"

                def test_offenders(self):
                    subprocess.run([self.make, "--no-print-directory", "clean-all"], cwd=ROOT)
                    subprocess.run([self.make, "tidy", f"BUILD_ROOT={BUILD}", "LOG_DIR=/x"], cwd=ROOT)
                    shutil.rmtree(BUILD / "sub")
                    doomed = ROOT / "build" / "title"
                    shutil.rmtree(doomed, ignore_errors=True)
                    (self.logs / "recomp_err.log").unlink()
                    self.addCleanup(shutil.rmtree, doomed, True)
                    os.remove(self.logs / "stdout_run.log")
                    subprocess.run([self.make, "GAME_NAME=hst", "--eval", "v: ; @echo", "v"], cwd=ROOT)
        ''')
        findings = _scan_for_checkout_damage(source, "case.py")
        self.assertEqual(
            [int(finding.split(":")[1]) for finding in findings],
            [12, 12, 13, 14, 16, 17, 18, 19, 20],
            "\n".join(findings),
        )
        self.assertIn("without a scratch BUILD_ROOT=", findings[0])
        self.assertIn("without a scratch LOG_DIR=", findings[1])
        self.assertIn("BUILD_ROOT inside the repository checkout", findings[2])
        self.assertIn("without a scratch BUILD_ROOT=", findings[8])

    def test_scan_reports_each_unscratched_make_run(self) -> None:
        source = textwrap.dedent('''
            import subprocess, tempfile
            from pathlib import Path
            ROOT = Path(__file__).resolve().parents[1]

            class Case:
                def test_probes(self):
                    subprocess.run([self.make, "--no-print-directory", "player"], cwd=ROOT)
                    subprocess.run([self.make, "help"], cwd=ROOT)
                    subprocess.run([self.make, "-n", "player", f"BUILD_ROOT={ROOT / 'build'}"], cwd=ROOT)
                    scratch = Path(tempfile.mkdtemp())
                    subprocess.run([self.make, "player", f"BUILD_ROOT={scratch}"], cwd=ROOT)
                    subprocess.run([self.make, "player", f"BUILD_ROOT={scratch}", "PLAYER_EXE=build/x.exe"], cwd=ROOT)
                    subprocess.run([self.make, "-C", str(ROOT / "fixtures"), "all"], cwd=ROOT)
                    subprocess.run([self.make, "all"], cwd=scratch)
                    argv = [self.make, "--no-print-directory", "gpu-selftest-status"]
                    self.assertEqual(argv, [self.make, "all"])
        ''')
        findings = _scan_for_checkout_damage(source, "case.py")
        self.assertEqual(
            [int(finding.split(":")[1]) for finding in findings],
            [8, 10, 13, 16],
            "\n".join(findings),
        )
        self.assertIn("without a scratch BUILD_ROOT=", findings[0])
        self.assertIn("with BUILD_ROOT inside the repository checkout", findings[1])
        self.assertIn("passes PLAYER_EXE= a checkout build/ path", findings[2])

    def test_scan_accepts_scratch_trees_and_read_only_checkout_use(self) -> None:
        source = textwrap.dedent('''
            import shutil, subprocess, tempfile
            from pathlib import Path
            ROOT = Path(__file__).resolve().parents[1]

            class Case:
                def test_scratch(self):
                    scratch = Path(tempfile.mkdtemp())
                    self.addCleanup(shutil.rmtree, scratch, True)
                    subprocess.run([self.make, "clean-all", f"BUILD_ROOT={scratch / 'b'}",
                                    f"LOG_DIR={scratch / 'l'}"], cwd=ROOT)
                    (scratch / "x.o").unlink()
                    text = (ROOT / "Makefile").read_text()
                    targets = ("clean", "clean-all")
                    subprocess.run([self.make, "help"], cwd=ROOT)
                    subprocess.run([self.make, "GAME_NAME=hst", f"BUILD_ROOT={scratch}", "v"], cwd=ROOT)
                    subprocess.run([self.make, "--no-print-directory", "player"], cwd=self.clone)
        ''')
        self.assertEqual(_scan_for_checkout_damage(source, "case.py"), [])


if __name__ == "__main__":
    unittest.main()
