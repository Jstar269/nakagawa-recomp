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
