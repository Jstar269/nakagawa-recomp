# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regression tests for the stable CI aggregate status."""

from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import re
import unittest
from unittest import mock

try:
    import ci_required
    from ci_required import evaluate_environment, required_gate_passes
except ModuleNotFoundError:  # Running as ``python -m unittest tools.test_ci_required``.
    from tools import ci_required
    from tools.ci_required import evaluate_environment, required_gate_passes


def _results(**overrides: str) -> dict[str, str]:
    values = {
        "classify": "success",
        "hygiene": "success",
        "public-export": "success",
        "python-tools": "skipped",
        "native-tools": "skipped",
        "windows-runtime": "skipped",
    }
    values.update(overrides)
    return values


class CiRequiredTests(unittest.TestCase):
    def test_irrelevant_skips_pass(self) -> None:
        self.assertTrue(required_gate_passes(_results(), {}, allow_substantive=True))

    def test_applicable_success_passes(self) -> None:
        results = _results(**{"python-tools": "success", "native-tools": "success"})
        self.assertTrue(
            required_gate_passes(
                results,
                {"python-tools": True, "native-tools": True},
                allow_substantive=True,
            )
        )

    def test_applicable_failure_or_cancel_fails(self) -> None:
        for outcome in ("failure", "cancelled", "skipped"):
            with self.subTest(outcome=outcome):
                results = _results(**{"python-tools": outcome})
                self.assertFalse(required_gate_passes(results, {"python-tools": True}, allow_substantive=True))

    def test_applicable_native_player_gate_failure_or_skip_fails(self) -> None:
        for outcome in ("failure", "cancelled", "skipped"):
            with self.subTest(outcome=outcome):
                results = _results(**{"native-tools": outcome})
                self.assertFalse(
                    required_gate_passes(results, {"native-tools": True}, allow_substantive=True)
                )

    def test_classifier_and_hygiene_failures_cannot_be_hidden(self) -> None:
        for name in ("classify", "hygiene", "public-export"):
            with self.subTest(name=name):
                self.assertFalse(required_gate_passes(_results(**{name: "failure"}), {}))

    def test_draft_substantive_skips_fail_closed(self) -> None:
        self.assertFalse(
            required_gate_passes(
                _results(),
                {"python-tools": True, "native-tools": True, "windows-runtime": True},
                allow_substantive=False,
                draft=True,
            )
        )

    def test_main_push_substantive_suppression_without_draft_remains_allowed(self) -> None:
        self.assertTrue(
            required_gate_passes(
                _results(),
                {"python-tools": True, "native-tools": True},
                allow_substantive=False,
                draft=False,
            )
        )

    def test_draft_to_ready_transition_requires_a_new_green_run(self) -> None:
        draft_env = {
            "CLASSIFY_RESULT": "success",
            "HYGIENE_RESULT": "success",
            "PUBLIC_EXPORT_RESULT": "success",
            "PYTHON_RESULT": "skipped",
            "NATIVE_RESULT": "skipped",
            "WINDOWS_RESULT": "skipped",
            "RUN_MARKDOWN": "false",
            "RUN_MAIN_SMOKE": "false",
            "RUN_PYTHON": "true",
            "RUN_NATIVE": "true",
            "RUN_WINDOWS": "true",
            "ALLOW_SUBSTANTIVE": "false",
            "DRAFT": "true",
        }
        self.assertFalse(evaluate_environment(draft_env))

        ready_env = dict(draft_env)
        ready_env.update(
            {
                "PYTHON_RESULT": "success",
                "NATIVE_RESULT": "success",
                "WINDOWS_RESULT": "success",
                "ALLOW_SUBSTANTIVE": "true",
                "DRAFT": "false",
            }
        )
        self.assertTrue(evaluate_environment(ready_env))

    def test_workflow_triggers_on_new_heads_and_on_draft_transitions(self) -> None:
        """CI starts for a new head and for each Draft/Ready transition.

        A Draft runs only the quick jobs, so marking it Ready must start the full
        matrix on the same head (ready_for_review), and converting it back to a Draft
        must start a light run whose shared concurrency group cancels a full run in
        progress (converted_to_draft). Every head-changing event stays listed, so
        nothing that changes code can go unvalidated.
        """
        workflow = (Path(__file__).resolve().parents[1] / ".github" / "workflows" / "ci.yml").read_text(
            encoding="utf-8"
        )
        # Match the declared list, not the file text: the rationale comment
        # above it names the events that must NOT trigger.
        match = re.search(
            r"^\s*pull_request:\s*\n\s*types:\s*\[([^\]]*)\]", workflow, re.MULTILINE
        )
        self.assertIsNotNone(match, "ci.yml must list the pull_request event types explicitly")
        assert match is not None
        declared = {item.strip() for item in match.group(1).split(",")}
        self.assertEqual(
            declared, {"opened", "synchronize", "reopened", "ready_for_review", "converted_to_draft"}
        )
        self.assertIn("cancel-in-progress: true", workflow)
        self.assertIn("DRAFT: ${{ needs.classify.outputs.draft }}", workflow)

    def test_main_smoke_remains_required_when_substantive_jobs_are_suppressed(self) -> None:
        # The main smoke is a step of the hygiene job, so its failure is hygiene's.
        results = _results(hygiene="failure")
        self.assertFalse(
            required_gate_passes(
                results,
                {"python-tools": True},
                allow_substantive=False,
            )
        )

    def test_folded_jobs_are_no_separate_inputs(self) -> None:
        """Markdown, main smoke and the profile-zero fixture are steps of required jobs.

        They reach the aggregate through hygiene and public-export, which must succeed on
        every event; the workflow no longer passes a result or run flag of their own.
        """
        workflow = (Path(__file__).resolve().parents[1] / ".github" / "workflows" / "ci.yml").read_text(
            encoding="utf-8"
        )
        for variable in ("MARKDOWN_RESULT", "MAIN_SMOKE_RESULT"):
            with self.subTest(variable=variable):
                self.assertNotIn(variable, workflow)
        # Their run flags are still passed, so a missing classifier output fails closed.
        self.assertIn("RUN_MARKDOWN: ${{ needs.classify.outputs.run_markdown }}", workflow)
        self.assertIn("RUN_MAIN_SMOKE: ${{ needs.classify.outputs.run_main_smoke }}", workflow)
        for name in ("markdown", "main-smoke"):
            with self.subTest(name=name):
                self.assertFalse(required_gate_passes(_results(hygiene="failure", **{name: "success"}), {}))

    def test_environment_mapping_matches_workflow_contract(self) -> None:
        env = {
            "CLASSIFY_RESULT": "success",
            "HYGIENE_RESULT": "success",
            "PUBLIC_EXPORT_RESULT": "success",
            "PYTHON_RESULT": "success",
            "NATIVE_RESULT": "skipped",
            "WINDOWS_RESULT": "skipped",
            "RUN_MARKDOWN": "false",
            "RUN_MAIN_SMOKE": "false",
            "RUN_PYTHON": "true",
            "RUN_NATIVE": "false",
            "RUN_WINDOWS": "false",
            "ALLOW_SUBSTANTIVE": "true",
            "DRAFT": "false",
        }
        self.assertTrue(evaluate_environment(env))
        env["PYTHON_RESULT"] = "cancelled"
        self.assertFalse(evaluate_environment(env))


def _draft_env(**overrides: str) -> dict[str, str]:
    """A Draft pull request's light run: the quick jobs passed, the substantive ones skipped."""

    env = {
        "CLASSIFY_RESULT": "success",
        "HYGIENE_RESULT": "success",
        "PUBLIC_EXPORT_RESULT": "success",
        "PYTHON_RESULT": "skipped",
        "NATIVE_RESULT": "skipped",
        "WINDOWS_RESULT": "skipped",
        "RUN_MARKDOWN": "true",
        "RUN_MAIN_SMOKE": "false",
        "RUN_PYTHON": "false",
        "RUN_NATIVE": "false",
        "RUN_WINDOWS": "false",
        "ALLOW_SUBSTANTIVE": "false",
        "DRAFT": "true",
    }
    env.update(overrides)
    return env


class DraftLightRunTests(unittest.TestCase):
    """A Draft pull request's run is never green, and says why (DRAFT_LIGHT_CI)."""

    def test_a_docs_only_draft_with_every_quick_job_green_is_not_green(self) -> None:
        # No substantive gate applies, so only the Draft rule can turn this red.
        self.assertFalse(evaluate_environment(_draft_env()))
        self.assertFalse(required_gate_passes(_results(), {}, allow_substantive=False, draft=True))

    def test_a_draft_is_not_green_even_if_every_substantive_job_passed(self) -> None:
        env = _draft_env(
            PYTHON_RESULT="success", NATIVE_RESULT="success", WINDOWS_RESULT="success",
            RUN_PYTHON="true", RUN_NATIVE="true", RUN_WINDOWS="true", ALLOW_SUBSTANTIVE="true",
        )
        self.assertFalse(evaluate_environment(env))
        results = _results(**{"python-tools": "success", "native-tools": "success", "windows-runtime": "success"})
        applicable = {"python-tools": True, "native-tools": True, "windows-runtime": True}
        self.assertFalse(required_gate_passes(results, applicable, allow_substantive=True, draft=True))
        # The same results for a pull request that is not a Draft pass, exactly as before.
        self.assertTrue(required_gate_passes(results, applicable, allow_substantive=True, draft=False))
        self.assertTrue(evaluate_environment(dict(env, DRAFT="false")))

    def test_main_names_the_reason_and_fails(self) -> None:
        stdout, stderr = io.StringIO(), io.StringIO()
        with (
            mock.patch.dict(os.environ, _draft_env(), clear=True),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            code = ci_required.main()
        self.assertEqual(code, 1)
        self.assertEqual(ci_required.DRAFT_LIGHT_CI, "DRAFT_LIGHT_CI")
        self.assertIn("::error title=DRAFT_LIGHT_CI::", stdout.getvalue())
        self.assertIn("DRAFT_LIGHT_CI: this head belongs to a Draft pull request", stderr.getvalue())
        self.assertIn("classify success, hygiene success, public-export success", stderr.getvalue())
        self.assertIn("Ready for review", stderr.getvalue())

    def test_a_failed_quick_job_in_a_draft_is_reported_with_its_result(self) -> None:
        stderr = io.StringIO()
        with (
            mock.patch.dict(os.environ, _draft_env(HYGIENE_RESULT="failure"), clear=True),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(ci_required.main(), 1)
        self.assertIn("hygiene failure", stderr.getvalue())

    def test_a_non_draft_run_keeps_the_former_messages(self) -> None:
        ready = _draft_env(DRAFT="false", ALLOW_SUBSTANTIVE="true")
        stdout = io.StringIO()
        with mock.patch.dict(os.environ, ready, clear=True), contextlib.redirect_stdout(stdout):
            self.assertEqual(ci_required.main(), 0)
        self.assertIn("All applicable CI gates passed", stdout.getvalue())
        stderr = io.StringIO()
        failed = dict(ready, RUN_PYTHON="true", PYTHON_RESULT="failure")
        with (
            mock.patch.dict(os.environ, failed, clear=True),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(ci_required.main(), 1)
        self.assertNotIn("DRAFT_LIGHT_CI", stderr.getvalue())
        self.assertIn("An applicable CI gate failed", stderr.getvalue())


class AllowSubstantiveParsingTests(unittest.TestCase):
    """Every control output must be an explicit, well-formed boolean.

    A malformed classifier output is a broken control state, not an
    instruction to skip a gate.
    """

    def _env(self, allow_substantive: str | None) -> dict[str, str]:
        env = {
            "CLASSIFY_RESULT": "success",
            "HYGIENE_RESULT": "success",
            "PUBLIC_EXPORT_RESULT": "success",
            "PYTHON_RESULT": "skipped",
            "NATIVE_RESULT": "skipped",
            "WINDOWS_RESULT": "skipped",
            "RUN_MARKDOWN": "false",
            "RUN_MAIN_SMOKE": "false",
            "RUN_PYTHON": "false",
            # Applicable, but the job was skipped -- this must fail unless
            # suppression was explicitly requested.
            "RUN_NATIVE": "true",
            "RUN_WINDOWS": "false",
            # This fixture models the intentional main-push suppression, not a
            # draft PR. Draft-specific suppression is tested separately below.
            "DRAFT": "false",
        }
        if allow_substantive is not None:
            env["ALLOW_SUBSTANTIVE"] = allow_substantive
        return env

    def test_explicit_false_suppresses(self) -> None:
        for value in ("false", "False", "FALSE", " false "):
            with self.subTest(value=value):
                self.assertTrue(evaluate_environment(self._env(value)))

    def test_draft_suppression_is_not_green(self) -> None:
        env = self._env("false")
        env["DRAFT"] = "true"
        self.assertFalse(evaluate_environment(env))

    def test_absent_empty_or_malformed_keeps_gates_required(self) -> None:
        for value in (None, "", "   ", "xyz", "0", "no", "null", "true"):
            with self.subTest(value=value):
                self.assertFalse(
                    evaluate_environment(self._env(value)),
                    f"ALLOW_SUBSTANTIVE={value!r} must not silently suppress an applicable gate",
                )


class ApplicabilityParsingTests(unittest.TestCase):
    def _env(self) -> dict[str, str]:
        return {
            "CLASSIFY_RESULT": "success",
            "HYGIENE_RESULT": "success",
            "PUBLIC_EXPORT_RESULT": "success",
            "PYTHON_RESULT": "success",
            "NATIVE_RESULT": "success",
            "WINDOWS_RESULT": "success",
            "RUN_MARKDOWN": "false",
            "RUN_MAIN_SMOKE": "false",
            "RUN_PYTHON": "false",
            "RUN_NATIVE": "false",
            "RUN_WINDOWS": "false",
            "ALLOW_SUBSTANTIVE": "true",
            "DRAFT": "false",
        }

    def test_every_run_variable_requires_a_boolean(self) -> None:
        for run_key in (
            "RUN_MARKDOWN",
            "RUN_PYTHON",
            "RUN_NATIVE",
            "RUN_WINDOWS",
            "RUN_MAIN_SMOKE",
        ):
            for value in (None, "", "   ", "garbage", "0", "no"):
                with self.subTest(run_key=run_key, value=value):
                    env = self._env()
                    if value is None:
                        env.pop(run_key)
                    else:
                        env[run_key] = value
                    self.assertFalse(evaluate_environment(env))

    def test_run_variables_accept_case_and_surrounding_whitespace(self) -> None:
        for run_key in (
            "RUN_MARKDOWN",
            "RUN_PYTHON",
            "RUN_NATIVE",
            "RUN_WINDOWS",
            "RUN_MAIN_SMOKE",
        ):
            with self.subTest(run_key=run_key):
                env = self._env()
                env[run_key] = " TrUe "
                self.assertTrue(evaluate_environment(env))
                env[run_key] = " FaLsE "
                self.assertTrue(evaluate_environment(env))

    def test_missing_folded_step_flag_fails_closed(self) -> None:
        """Markdown and main smoke run as hygiene steps gated on these flags.

        A dropped or misnamed classifier output would skip the step and leave hygiene
        green, so the aggregate must fail when either flag is absent, even with every
        job successful.
        """
        for run_key in ("RUN_MARKDOWN", "RUN_MAIN_SMOKE"):
            with self.subTest(run_key=run_key):
                env = self._env()
                self.assertTrue(evaluate_environment(env))
                del env[run_key]
                self.assertFalse(evaluate_environment(env))

    def test_malformed_applicability_cannot_be_masked_by_successful_jobs(self) -> None:
        env = self._env()
        env["RUN_NATIVE"] = "unexpected"
        self.assertTrue(all(value == "success" for key, value in env.items() if key.endswith("_RESULT")))
        self.assertFalse(evaluate_environment(env))

    def test_allow_substantive_requires_a_boolean(self) -> None:
        for value in (None, "", "   ", "garbage", "0", "no", "null"):
            with self.subTest(value=value):
                env = self._env()
                if value is None:
                    env.pop("ALLOW_SUBSTANTIVE")
                else:
                    env["ALLOW_SUBSTANTIVE"] = value
                self.assertFalse(evaluate_environment(env))

    def test_draft_requires_a_boolean(self) -> None:
        for value in (None, "", "   ", "garbage", "0", "no", "null"):
            with self.subTest(value=value):
                env = self._env()
                if value is None:
                    env.pop("DRAFT")
                else:
                    env["DRAFT"] = value
                self.assertFalse(evaluate_environment(env))

    def test_public_export_result_is_required_on_every_event(self) -> None:
        """The export audit is its own required result (#735 item 5).

        It runs ungated on every event, so an unset or non-success result is never green,
        even when no substantive gate applies (the docs-only and main-push shapes).
        """
        for value in ("failure", "cancelled", "skipped", ""):
            with self.subTest(value=value):
                env = self._env()
                env["PUBLIC_EXPORT_RESULT"] = value
                self.assertFalse(evaluate_environment(env))
        env = self._env()
        del env["PUBLIC_EXPORT_RESULT"]
        self.assertFalse(evaluate_environment(env))
        env = self._env()
        env["PUBLIC_EXPORT_RESULT"] = "success"
        self.assertTrue(evaluate_environment(env))

    def test_classifier_and_hygiene_non_success_remain_unconditionally_red(self) -> None:
        for key in ("CLASSIFY_RESULT", "HYGIENE_RESULT", "PUBLIC_EXPORT_RESULT"):
            for value in ("failure", "cancelled", "skipped", "", "garbage"):
                with self.subTest(key=key, value=value):
                    env = self._env()
                    env[key] = value
                    self.assertFalse(evaluate_environment(env))


if __name__ == "__main__":
    unittest.main()
