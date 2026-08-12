# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regression tests for the stable CI aggregate status."""

from __future__ import annotations

from pathlib import Path
import unittest

try:
    from ci_required import evaluate_environment, required_gate_passes
except ModuleNotFoundError:  # Running as ``python -m unittest tools.test_ci_required``.
    from tools.ci_required import evaluate_environment, required_gate_passes


def _results(**overrides: str) -> dict[str, str]:
    values = {
        "classify": "success",
        "hygiene": "success",
        "markdown": "skipped",
        "python-tools": "skipped",
        "native-tools": "skipped",
        "dashboard": "skipped",
        "windows-runtime": "skipped",
        "main-smoke": "skipped",
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
                results = _results(**{"dashboard": outcome})
                self.assertFalse(required_gate_passes(results, {"dashboard": True}, allow_substantive=True))

    def test_classifier_and_hygiene_failures_cannot_be_hidden(self) -> None:
        for name in ("classify", "hygiene"):
            with self.subTest(name=name):
                self.assertFalse(required_gate_passes(_results(**{name: "failure"}), {}))

    def test_draft_substantive_skips_fail_closed(self) -> None:
        self.assertFalse(
            required_gate_passes(
                _results(),
                {"python-tools": True, "native-tools": True, "dashboard": True, "windows-runtime": True},
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
            "MARKDOWN_RESULT": "skipped",
            "PYTHON_RESULT": "skipped",
            "NATIVE_RESULT": "skipped",
            "DASHBOARD_RESULT": "skipped",
            "WINDOWS_RESULT": "skipped",
            "MAIN_SMOKE_RESULT": "skipped",
            "RUN_MARKDOWN": "false",
            "RUN_PYTHON": "true",
            "RUN_NATIVE": "true",
            "RUN_DASHBOARD": "false",
            "RUN_WINDOWS": "true",
            "RUN_MAIN_SMOKE": "false",
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

    def test_workflow_preserves_ready_transition_and_cancellation_contract(self) -> None:
        workflow = (Path(__file__).resolve().parents[1] / ".github" / "workflows" / "ci.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("ready_for_review", workflow)
        self.assertIn("cancel-in-progress: true", workflow)
        self.assertIn("DRAFT: ${{ needs.classify.outputs.draft }}", workflow)

    def test_main_smoke_remains_required_when_substantive_jobs_are_suppressed(self) -> None:
        results = _results(**{"main-smoke": "failure"})
        self.assertFalse(
            required_gate_passes(
                results,
                {"python-tools": True, "main-smoke": True},
                allow_substantive=False,
            )
        )

    def test_environment_mapping_matches_workflow_contract(self) -> None:
        env = {
            "CLASSIFY_RESULT": "success",
            "HYGIENE_RESULT": "success",
            "MARKDOWN_RESULT": "skipped",
            "PYTHON_RESULT": "success",
            "NATIVE_RESULT": "skipped",
            "DASHBOARD_RESULT": "skipped",
            "WINDOWS_RESULT": "skipped",
            "MAIN_SMOKE_RESULT": "skipped",
            "RUN_MARKDOWN": "false",
            "RUN_PYTHON": "true",
            "RUN_NATIVE": "false",
            "RUN_DASHBOARD": "false",
            "RUN_WINDOWS": "false",
            "RUN_MAIN_SMOKE": "false",
            "ALLOW_SUBSTANTIVE": "true",
            "DRAFT": "false",
        }
        self.assertTrue(evaluate_environment(env))
        env["PYTHON_RESULT"] = "cancelled"
        self.assertFalse(evaluate_environment(env))


class AllowSubstantiveParsingTests(unittest.TestCase):
    """Every control output must be an explicit, well-formed boolean.

    A malformed classifier output is a broken control state, not an
    instruction to skip a gate.
    """

    def _env(self, allow_substantive: str | None) -> dict[str, str]:
        env = {
            "CLASSIFY_RESULT": "success",
            "HYGIENE_RESULT": "success",
            "MARKDOWN_RESULT": "skipped",
            "PYTHON_RESULT": "skipped",
            "NATIVE_RESULT": "skipped",
            "DASHBOARD_RESULT": "skipped",
            "WINDOWS_RESULT": "skipped",
            "MAIN_SMOKE_RESULT": "skipped",
            "RUN_MARKDOWN": "false",
            "RUN_PYTHON": "false",
            # Applicable, but the job was skipped -- this must fail unless
            # suppression was explicitly requested.
            "RUN_NATIVE": "true",
            "RUN_DASHBOARD": "false",
            "RUN_WINDOWS": "false",
            "RUN_MAIN_SMOKE": "false",
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
            "MARKDOWN_RESULT": "success",
            "PYTHON_RESULT": "success",
            "NATIVE_RESULT": "success",
            "DASHBOARD_RESULT": "success",
            "WINDOWS_RESULT": "success",
            "MAIN_SMOKE_RESULT": "success",
            "RUN_MARKDOWN": "false",
            "RUN_PYTHON": "false",
            "RUN_NATIVE": "false",
            "RUN_DASHBOARD": "false",
            "RUN_WINDOWS": "false",
            "RUN_MAIN_SMOKE": "false",
            "ALLOW_SUBSTANTIVE": "true",
            "DRAFT": "false",
        }

    def test_every_run_variable_requires_a_boolean(self) -> None:
        for run_key in (
            "RUN_MARKDOWN",
            "RUN_PYTHON",
            "RUN_NATIVE",
            "RUN_DASHBOARD",
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
            "RUN_DASHBOARD",
            "RUN_WINDOWS",
            "RUN_MAIN_SMOKE",
        ):
            with self.subTest(run_key=run_key):
                env = self._env()
                env[run_key] = " TrUe "
                self.assertTrue(evaluate_environment(env))
                env[run_key] = " FaLsE "
                self.assertTrue(evaluate_environment(env))

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

    def test_classifier_and_hygiene_non_success_remain_unconditionally_red(self) -> None:
        for key in ("CLASSIFY_RESULT", "HYGIENE_RESULT"):
            for value in ("failure", "cancelled", "skipped", "", "garbage"):
                with self.subTest(key=key, value=value):
                    env = self._env()
                    env[key] = value
                    self.assertFalse(evaluate_environment(env))


if __name__ == "__main__":
    unittest.main()
