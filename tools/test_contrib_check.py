# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""`make contrib-check` must route, and must never turn a skip into a pass.

The local fast path is only worth having if it selects the same gates the
hosted workflow would, and only if a gate that could not run is reported as
such.  These tests pin both properties, plus the one place the script
interprets an audit result: the findings only a maintainer can clear, by
regenerating the controls from the private trusted ledger.
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

import contrib_check  # noqa: E402

MAINTAINER_LINE = "PROVENANCE_CONTENT_MISMATCH: tools/thing.py: audited bytes differ"
CONTRIBUTOR_LINE = "POLICY_UNCLASSIFIED: tools/new_thing.py: no explicit disposition"
SUMMARY_LINE = "publication audit: FAIL (2 findings across 748 tracked files, worktree content)"


class _Completed:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = "") -> None:
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


class TestRouting(unittest.TestCase):
    def test_a_shared_tool_pulls_in_its_shared_test_modules(self) -> None:
        modules = contrib_check.python_test_modules(["tools/public_export.py"])
        self.assertIn("tools.test_public_export", modules)
        self.assertIn("tools.test_publish_audit", modules)

    def test_a_changed_test_module_runs_itself(self) -> None:
        self.assertEqual(
            contrib_check.python_test_modules(["tools/test_sched_invariants.py"]),
            ["tools.test_sched_invariants"],
        )

    def test_a_tool_with_a_companion_test_runs_that_test(self) -> None:
        modules = contrib_check.python_test_modules(["tools/policy_sync.py"])
        self.assertIn("tools.test_publication_policy_gate", modules)

    def test_documentation_and_build_files_select_no_python_module(self) -> None:
        self.assertEqual(
            contrib_check.python_test_modules(["CONTRIBUTING.md", "Makefile", "mk/build_common.mk"]),
            [],
        )

    def test_routing_uses_the_hosted_classifier(self) -> None:
        """The local selection must come from tools/ci_paths, not a second copy."""
        source = (ROOT / "tools" / "contrib_check.py").read_text(encoding="utf-8")
        self.assertIn("ci_paths.classify", source)
        self.assertNotIn("def _is_docs", source)
        self.assertNotIn("def _is_native", source)


class TestGateStatusVocabulary(unittest.TestCase):
    def test_an_untouched_surface_is_not_run_not_pass(self) -> None:
        self.assertEqual(contrib_check.c_gate(["docs/a.md"]).status, contrib_check.NOT_RUN)
        self.assertEqual(contrib_check.markdown_gate(["tools/a.py"]).status, contrib_check.NOT_RUN)
        self.assertEqual(contrib_check.ruff_gate([]).status, contrib_check.NOT_RUN)
        self.assertEqual(contrib_check.purity_gate(["docs/a.md"]).status, contrib_check.NOT_RUN)

    def test_a_runtime_change_runs_the_core_purity_gate(self) -> None:
        gate = contrib_check.purity_gate(["src/rt/hle.c"])
        self.assertEqual(gate.name, "core-purity")
        self.assertEqual(gate.status, contrib_check.PASS, gate.detail)

    def test_a_missing_tool_is_skipped_rather_than_passed(self) -> None:
        with mock.patch.object(contrib_check.shutil, "which", return_value=None):
            gate = contrib_check.markdown_gate(["docs/a.md"])
        self.assertEqual(gate.status, contrib_check.SKIP)

    def test_a_failing_command_fails_with_its_own_output(self) -> None:
        with mock.patch.object(
            contrib_check.subprocess, "run",
            return_value=_Completed(1, "", "boom: something is wrong"),
        ):
            gate = contrib_check.ruff_gate(["tools/a.py"])
        self.assertEqual(gate.status, contrib_check.FAIL)
        self.assertIn("boom", gate.detail)

    def test_a_timeout_is_a_failure_not_a_skip(self) -> None:
        with mock.patch.object(
            contrib_check.subprocess, "run",
            side_effect=subprocess.TimeoutExpired(cmd="ruff", timeout=1),
        ):
            gate = contrib_check.ruff_gate(["tools/a.py"])
        self.assertEqual(gate.status, contrib_check.FAIL)
        self.assertIn("timed out", gate.detail)


class TestStrictCompileIncludePath(unittest.TestCase):
    """A `tests/native` unit may include a `src/player` header by bare name.

    Those units sit outside `src/`, so the include resolves only when the
    strict-compile line carries `-Isrc/player`.  The flag was missing, so five
    tracked test units the change never touched failed the gate with a bare
    `fatal error: <player header>: No such file or directory`.
    """

    def _player_headers(self) -> set[str]:
        return {path.name for path in (ROOT / "src" / "player").glob("*.h")}

    def _unit_player_includes(self, relative: str) -> list[str]:
        """The `src/player` headers a `tests/native` unit includes by bare name."""
        source = (ROOT / relative).read_text(encoding="utf-8", errors="replace")
        names = {
            Path(name).name
            for name in re.findall(r'#\s*include\s+"([^"]+)"', source)
        }
        return sorted(names & self._player_headers())

    def _player_including_tests(self) -> list[str]:
        found: list[str] = []
        for path in sorted((ROOT / "tests" / "native").glob("*.c")):
            relative = path.relative_to(ROOT).as_posix()
            if self._unit_player_includes(relative):
                found.append(relative)
        return found

    def _needs_installed_sdk(self, relative: str) -> bool:
        """Does one of the unit's player headers pull a header outside the project?

        A slash inside an angle include means an SDK header (`<SDL3/SDL.h>`), and
        standard C headers never carry one.  `ui_clip.h` and `ui_renderer.h`
        pull SDL3, which the native and UI gates build and the Python-only shard
        that runs this file does not install.
        """
        for name in self._unit_player_includes(relative):
            header = (ROOT / "src" / "player" / name).read_text(encoding="utf-8")
            if re.search(r'#\s*include\s+<[^>]*/[^>]*>', header):
                return True
        return False

    def test_the_player_include_path_is_on_the_strict_compile_line(self) -> None:
        self.assertIn("-Isrc/player", contrib_check.STRICT_C_FLAGS)

    def test_every_unit_including_a_player_header_compiles(self) -> None:
        files = self._player_including_tests()
        self.assertTrue(
            files,
            "no tests/native unit includes a src/player header any more; "
            "retire this regression together with the last such unit",
        )
        for relative in files:
            with self.subTest(unit=relative):
                gate = contrib_check.c_gate([relative])
                if gate.status == contrib_check.SKIP:
                    self.skipTest(gate.detail)
                if gate.status == contrib_check.PASS:
                    continue
                # This regression owns the include path, not every dependency of
                # every unit.  A unit whose player header pulls an SDK header can
                # only reach a full compile where that SDK is installed, so on a
                # runner that installs none the SDK header may be what is
                # missing - a *player* header may not, which is the claim here.
                self.assertTrue(self._needs_installed_sdk(relative), gate.detail)
                for name in self._unit_player_includes(relative):
                    self.assertNotIn(f"{name}: No such file or directory",
                                     gate.detail)


def _makefile_text() -> str:
    """The Makefile and the fragments it includes, continuation lines joined."""
    parts = [(ROOT / "Makefile").read_text(encoding="utf-8")]
    parts.extend(path.read_text(encoding="utf-8") for path in sorted((ROOT / "mk").glob("*.mk")))
    return re.sub(r"\\\n[ \t]*", " ", "\n".join(parts))


def _assignments(text: str) -> dict[str, list[str]]:
    """Every value assigned to each variable, across all conditional branches.

    The drift checks ask whether a flag is on a recipe at all, so the union of
    the alternatives is the right reading: a flag present in any branch counts.
    """
    found: dict[str, list[str]] = {}
    pattern = re.compile(r"^(?:override\s+)?([A-Za-z0-9_.-]+)\s*(?::=|\?=|\+=|=)\s*(.*)$", re.M)
    for name, value in pattern.findall(text):
        found.setdefault(name, []).append(value)
    return found


def _expand(text: str, assignments: dict[str, list[str]], depth: int = 0) -> str:
    if depth > 10:
        return text
    return re.sub(
        r"\$\(([A-Za-z0-9_.-]+)\)",
        lambda match: _expand(" ".join(assignments.get(match.group(1), [])), assignments, depth + 1),
        text,
    )


def _rule_blocks(text: str, target: str) -> list[str]:
    """Header plus recipe of every rule whose target is exactly *target*."""
    lines = text.splitlines()
    blocks: list[str] = []
    for index, line in enumerate(lines):
        if not line.startswith(target + ":") or line.startswith(target + ":="):
            continue
        recipe = [line]
        for following in lines[index + 1:]:
            if not following.startswith("\t"):
                break
            recipe.append(following)
        blocks.append("\n".join(recipe))
    return blocks


def _has_flag(text: str, flag: str) -> bool:
    return re.search(re.escape(flag) + r"(?![A-Za-z0-9_./-])", text) is not None


class TestStrictCompileFlagsMirrorTheMakefile(unittest.TestCase):
    """Each source is compiled with the flags the build compiles it with.

    The gate compiled every changed source with one fixed line, so a selftest
    whose hooks are declared under its own define (`SR_HLE_THREAD_SELFTEST`,
    `SR_SCHED_LIVENESS_TEST`), a player screen that includes `nk_ui.h`, and the
    Vulkan backend all failed on unchanged `origin/main`, for every contributor
    who touched them.  The per-source table must stay the Makefile's own flags.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = _makefile_text()
        cls.assignments = _assignments(cls.makefile)

    def test_every_listed_flag_is_on_the_recipe_that_builds_the_source(self) -> None:
        for source, unit in contrib_check.STRICT_C_UNITS.items():
            with self.subTest(source=source):
                self.assertTrue((ROOT / source).is_file(), f"{source} is not in the tree")
                self.assertTrue(unit.recipes, f"{source} names no Makefile recipe")
                building: list[str] = []
                raw: list[str] = []
                for target in unit.recipes:
                    blocks = _rule_blocks(self.makefile, target)
                    self.assertTrue(blocks, f"no Makefile rule `{target}:` (named for {source})")
                    for block in blocks:
                        expanded = _expand(block, self.assignments)
                        if source in expanded:
                            building.append(expanded)
                            raw.append(block)
                self.assertTrue(
                    building, f"none of {unit.recipes} compiles {source}; the table names the wrong rule",
                )
                recipe = "\n".join(building)
                for define in unit.defines:
                    self.assertTrue(
                        _has_flag(recipe, f"-D{define}"),
                        f"-D{define} is listed for {source} but is not on {unit.recipes}",
                    )
                for include in unit.includes:
                    self.assertTrue(
                        _has_flag(recipe, f"-I{include}"),
                        f"-I{include} is listed for {source} but is not on {unit.recipes}",
                    )
                for warning in unit.warnings:
                    self.assertTrue(
                        _has_flag(recipe, warning),
                        f"{warning} is listed for {source} but is not on {unit.recipes}",
                    )
                for header in unit.generated:
                    self.assertTrue(
                        any(header.include in block for block in raw),
                        f"{header.include} ({header.name}) is not on {unit.recipes}",
                    )

    def test_every_common_include_directory_is_one_the_makefile_uses(self) -> None:
        for flag in contrib_check.STRICT_C_FLAGS:
            if not flag.startswith("-I"):
                continue
            with self.subTest(flag=flag):
                self.assertTrue((ROOT / flag[2:]).is_dir(), f"{flag} names no directory")
                self.assertTrue(_has_flag(self.makefile, flag), f"the Makefile never passes {flag}")

    def test_the_drift_check_catches_a_define_the_recipe_does_not_set(self) -> None:
        """Guard the guard: an invented define must not be found on the recipe."""
        blocks = _rule_blocks(self.makefile, "sched-selftest-one")
        recipe = "\n".join(_expand(block, self.assignments) for block in blocks)
        self.assertTrue(_has_flag(recipe, "-DSR_SCHED_LIVENESS_TEST"))
        self.assertFalse(_has_flag(recipe, "-DSR_SCHED_LIVENESS"))
        self.assertFalse(_has_flag(recipe, "-DSR_HLE_THREAD_SELFTEST"))

    def test_a_selftest_is_compiled_with_its_own_defines(self) -> None:
        command, reason = contrib_check.strict_compile_command("cc", "src/rt/hle_thread_selftest.c", [])
        self.assertEqual(reason, "")
        for define in ("SR_HLE_THREAD_SELFTEST", "SR_CORO_LIFECYCLE_TEST", "SR_SCHED_LIVENESS_TEST"):
            self.assertIn(f"-D{define}", command)
        self.assertEqual(command[-1], "src/rt/hle_thread_selftest.c")
        plain, _ = contrib_check.strict_compile_command("cc", "src/rt/hle.c", [])
        self.assertFalse([flag for flag in plain if flag.startswith("-D")])


class TestStrictCompileUnavailableInputs(unittest.TestCase):
    def _fail_if_compiled(self, *args, **kwargs):
        raise AssertionError("the compiler must not run for a file that cannot be compiled here")

    def test_a_missing_generated_header_is_not_run_rather_than_failed(self) -> None:
        with tempfile.TemporaryDirectory() as empty, \
                mock.patch.dict(contrib_check.os.environ, {"BUILD_ROOT": empty, "CC": "cc"}), \
                mock.patch.object(contrib_check, "vulkan_include_flags", return_value=([], "")), \
                mock.patch.object(contrib_check.subprocess, "run", side_effect=self._fail_if_compiled):
            gate = contrib_check.c_gate(["fixtures/cosim/cosim_selftest.c"])
        self.assertEqual(gate.status, contrib_check.NOT_RUN)
        self.assertIn("cosim_recomp_funcs.h", gate.detail)
        self.assertIn("make cosim-selftest", gate.detail)

    def test_a_present_generated_header_is_put_on_the_include_path(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            fixture = Path(root) / "cosim" / "fixture"
            fixture.mkdir(parents=True)
            (Path(root) / "cosim" / "cosim_recomp_funcs.h").write_text("", encoding="ascii")
            for name in ("cosim_cells.h", "cosim_fpu_corpus.h"):
                (fixture / name).write_text("", encoding="ascii")
            with mock.patch.dict(contrib_check.os.environ, {"BUILD_ROOT": root}):
                command, reason = contrib_check.strict_compile_command(
                    "cc", "fixtures/cosim/cosim_selftest.c", [],
                )
        self.assertEqual(reason, "")
        self.assertIn(f"-I{root}/cosim", command)
        self.assertIn(f"-I{root}/cosim/fixture", command)
        self.assertEqual(command.count(f"-I{root}/cosim/fixture"), 1)
        self.assertIn("-DSR_INSTRUCTION_TRACE", command)

    def test_a_missing_vulkan_sdk_is_a_skip_with_its_reason_never_a_pass(self) -> None:
        missing = _Completed(
            1, "", "src/rt/gpu_sdl3vk/ge_gpu.c:82:10: fatal error: vulkan/vulkan.h: "
            "No such file or directory\ncompilation terminated.",
        )
        with mock.patch.dict(contrib_check.os.environ, {"CC": "cc"}), \
                mock.patch.object(contrib_check, "discover_vulkan_sdk",
                                  side_effect=contrib_check.VulkanSdkError("No usable Vulkan SDK found.")), \
                mock.patch.object(contrib_check.subprocess, "run", return_value=missing):
            gate = contrib_check.c_gate(["src/rt/gpu_sdl3vk/ge_gpu.c"])
        self.assertEqual(gate.status, contrib_check.SKIP)
        self.assertIn("Vulkan SDK", gate.detail)
        self.assertIn("No usable Vulkan SDK found.", gate.detail)

    def test_a_discovered_vulkan_sdk_is_on_the_include_path(self) -> None:
        sdk = Path(tempfile.gettempdir()) / "VulkanSDK" / "1.4.0"
        with mock.patch.object(contrib_check, "discover_vulkan_sdk", return_value=sdk):
            flags, reason = contrib_check.vulkan_include_flags()
        self.assertEqual(reason, "")
        self.assertIn(f"-I{sdk.as_posix()}/include", flags)

    def test_a_real_failure_outranks_a_skip_and_a_not_run(self) -> None:
        def fake_run(command, **kwargs):
            if command[-1].endswith("ge_gpu.c"):
                return _Completed(1, "", "fatal error: vulkan/vulkan.h: No such file or directory")
            return _Completed(1, "", "src/rt/hle.c:1:1: error: boom")

        with tempfile.TemporaryDirectory() as empty, \
                mock.patch.dict(contrib_check.os.environ, {"BUILD_ROOT": empty, "CC": "cc"}), \
                mock.patch.object(contrib_check, "vulkan_include_flags", return_value=([], "none")), \
                mock.patch.object(contrib_check.subprocess, "run", side_effect=fake_run):
            gate = contrib_check.c_gate([
                "src/rt/gpu_sdl3vk/ge_gpu.c", "fixtures/cosim/cosim_selftest.c", "src/rt/hle.c",
            ])
        self.assertEqual(gate.status, contrib_check.FAIL)
        self.assertIn("src/rt/hle.c: ", gate.detail)
        self.assertIn("SKIP src/rt/gpu_sdl3vk/ge_gpu.c", gate.detail)
        self.assertIn("NOT_RUN fixtures/cosim/cosim_selftest.c", gate.detail)

    def test_a_deleted_source_is_not_compiled(self) -> None:
        with mock.patch.object(contrib_check.subprocess, "run", side_effect=self._fail_if_compiled):
            gate = contrib_check.c_gate(["src/rt/no_such_file_any_more.c"])
        self.assertEqual(gate.status, contrib_check.NOT_RUN)


class TestStrictCompileOnUnchangedMain(unittest.TestCase):
    """The sources that failed the gate on unchanged `origin/main` no longer do.

    Each one either compiles, or reports why it could not be compiled here
    (`SKIP` for an absent SDK, `NOT_RUN` for an absent generated header).
    """

    SOURCES = (
        "src/rt/hle_thread_selftest.c",
        "src/rt/sched_selftest.c",
        "src/player/ui/player_ui_nk.c",
        "src/player/ui/scr_title.c",
        "tests/native/test_player_screens.c",
        "src/rt/gpu_sdl3vk/ge_gpu.c",
        "fixtures/cosim/cosim_selftest.c",
    )

    def test_no_unchanged_source_fails_the_strict_compile(self) -> None:
        if not (contrib_check.os.environ.get("CC") or contrib_check.find_tool("gcc")
                or contrib_check.find_tool("cc")):
            self.skipTest("no C compiler on PATH")
        for source in self.SOURCES:
            with self.subTest(source=source):
                gate = contrib_check.c_gate([source])
                self.assertNotEqual(gate.status, contrib_check.FAIL, gate.detail)


class TestPublicationGateSplit(unittest.TestCase):
    """Only a maintainer can clear the generated-control findings."""

    def _run(self, audit_stdout: str, audit_returncode: int = 1):
        def fake_run(command, **kwargs):
            if "policy_sync.py" in " ".join(command):
                return _Completed(0, "policy sync: OK\n")
            return _Completed(audit_returncode, audit_stdout, "")

        with mock.patch.object(contrib_check.subprocess, "run", side_effect=fake_run):
            return contrib_check.publication_gate()

    def test_only_maintainer_side_findings_do_not_fail_a_contributor(self) -> None:
        policy, audit = self._run(f"{MAINTAINER_LINE}\n{SUMMARY_LINE}\n")
        self.assertEqual(policy.status, contrib_check.PASS)
        self.assertEqual(audit.status, contrib_check.MAINTAINER_SIDE)
        self.assertIn("PROVENANCE_CONTENT_MISMATCH", audit.detail)

    def test_a_contributor_fixable_finding_still_fails(self) -> None:
        _policy, audit = self._run(f"{MAINTAINER_LINE}\n{CONTRIBUTOR_LINE}\n{SUMMARY_LINE}\n")
        self.assertEqual(audit.status, contrib_check.FAIL)
        self.assertIn("POLICY_UNCLASSIFIED", audit.detail)
        self.assertNotIn("PROVENANCE_CONTENT_MISMATCH", audit.detail)

    def test_an_unclassified_new_path_is_a_contributor_problem(self) -> None:
        """Adding a file the policy does not classify is the author's own gap."""
        _policy, audit = self._run(f"{CONTRIBUTOR_LINE}\n{SUMMARY_LINE}\n")
        self.assertEqual(audit.status, contrib_check.FAIL)

    def test_a_clean_audit_passes(self) -> None:
        _policy, audit = self._run("publication audit: OK\n", audit_returncode=0)
        self.assertEqual(audit.status, contrib_check.PASS)

    def test_policy_drift_fails_before_the_audit_runs(self) -> None:
        def fake_run(command, **kwargs):
            if "policy_sync.py" in " ".join(command):
                return _Completed(1, "", "unclassified: tools/new.py")
            raise AssertionError("the audit must not run once the policy check failed")

        with mock.patch.object(contrib_check.subprocess, "run", side_effect=fake_run):
            policy, audit = contrib_check.publication_gate()
        self.assertEqual(policy.status, contrib_check.FAIL)
        self.assertEqual(audit.status, contrib_check.NOT_RUN)

    def test_the_maintainer_side_code_list_names_real_audit_codes(self) -> None:
        source = (ROOT / "tools" / "publish_audit.py").read_text(encoding="utf-8")
        for code in contrib_check.MAINTAINER_SIDE_CODES:
            self.assertIn(f'"{code}"', source, f"{code} is not a finding this audit can raise")


class TestMakeTargetWiring(unittest.TestCase):
    def test_the_makefile_exposes_contrib_check_through_the_help_catalog(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertRegex(makefile, r"(?m)^contrib-check:")
        self.assertIn("tools/contrib_check.py", makefile)
        self.assertRegex(makefile, r"(?m)^\tcontrib-check \\$")
        self.assertRegex(
            makefile,
            r"(?m)^HELP_DESCRIPTION_contrib-check := .+$",
        )

    def test_the_target_runs_on_both_make_frontends(self) -> None:
        """mingw32-make and make share the recipe; nothing in it is host-specific."""
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        recipe = makefile.split("\ncontrib-check:\n", 1)[1].split("\n\n", 1)[0]
        self.assertIn("$(PYTHON)", recipe)
        for host_specific in ("mingw32", "cmd /c", "pwsh", "powershell"):
            self.assertNotIn(host_specific, recipe)


if __name__ == "__main__":
    unittest.main()
