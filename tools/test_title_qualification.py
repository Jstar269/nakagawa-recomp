#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for tools/title_qualification.py: public manifests, input profiles, launch reports and the smoke."""

from __future__ import annotations

import contextlib
import copy
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import library_sweep  # noqa: E402
import nk_cli  # noqa: E402
import title_manifest  # noqa: E402
import title_qualification as tq  # noqa: E402

INPUT_RUNTIME_SOURCE = ROOT / "src" / "core" / "nk_input_profile.c"
PROFILE = ROOT / "assets" / "input_profiles" / "standard-gamepad.json"
DISPLAY_SMOKE = ROOT / "assets" / "titles" / "display-smoke.json"
TITLE_ID = "display-smoke-v1"
DISPLAY_NAME = "Nakagawa Display Smoke Fixture"


def _c_table_strings(source: str, table: str) -> tuple[str, ...]:
    """The string names of one ``static const struct`` table in the runtime source, in order."""
    match = re.search(re.escape(table) + r"\[\] = \{(.*?)\n\};", source, re.DOTALL)
    if not match:
        raise AssertionError(f"table {table} not found in {INPUT_RUNTIME_SOURCE.name}")
    return tuple(re.findall(r'"([a-z_0-9]+)"', match.group(1)))


def _pass_output(*, title: str = TITLE_ID, name: str = DISPLAY_NAME, backend: str = "offscreen",
                 first_frame: str = "nonzero_pixels=130560 total_pixels=130560",
                 submissions: int = 2, exit_code: int = 0) -> str:
    lines = [
        f"[PLAYER] Launch index 1 is a bundled SAMPLE demo, not in library.json: "
        f"PLAY NOW available for TEST00006 ({name}).",
        f"[PLAYER] Spawning runtime: C:\\nk\\build\\{title}\\{title}.exe (ISO: fixtures/display_smoke/generate.py)",
        f"BOOT_EVENT phase=window_ready backend={backend}",
        "BOOT_EVENT phase=guest_start mode=scheduler entry=0x08810000",
        f"BOOT_EVENT phase=first_frame source=cpu {first_frame}",
    ]
    for frame in range(1, submissions + 1):
        lines.append(f"BOOT_EVENT phase=frame_present backend=offscreen frame={frame}")
        lines.append(f"HOST_PRESENT_SUBMITTED f={frame} buf=0x04000000 fmt=3 stride=512")
    lines.append(f"[PLAYER] --launch-index child exited with code {exit_code}.")
    return "\n".join(lines) + "\n"


def _public_manifest() -> dict:
    return title_manifest.validate_manifest(title_manifest.load_manifest(DISPLAY_SMOKE))


class InputProfileVocabularyTests(unittest.TestCase):
    """The public profile and its validator use exactly the runtime's vocabulary."""

    @classmethod
    def setUpClass(cls):
        cls.source = INPUT_RUNTIME_SOURCE.read_text(encoding="utf-8")
        cls.valid = tq.load_public_json(PROFILE)

    def test_public_profile_validates(self):
        tq.validate_input_profile(copy.deepcopy(self.valid))

    def test_psp_controls_match_runtime_table_in_order(self):
        self.assertEqual(tq.PSP_CONTROLS, _c_table_strings(self.source, "kPspButtons"))

    def test_psp_axes_match_runtime_table(self):
        self.assertEqual(tq.PSP_AXES, _c_table_strings(self.source, "kPspAxes"))

    def test_host_names_match_runtime_tables(self):
        self.assertEqual(set(tq.HOST_BUTTONS), set(_c_table_strings(self.source, "kHostButtons")))
        self.assertEqual(set(tq.HOST_AXES), set(_c_table_strings(self.source, "kHostAxes")))

    def test_navigation_actions_match_runtime_table(self):
        self.assertEqual(set(tq.NAV_ACTIONS), set(_c_table_strings(self.source, "kNavActions")))

    def _refused(self, document: dict, fragment: str):
        with self.assertRaises(tq.QualificationError) as raised:
            tq.validate_input_profile(document)
        self.assertIn(fragment, str(raised.exception))

    def test_refuses_missing_control_entry(self):
        document = copy.deepcopy(self.valid)
        document["psp_bindings"] = [e for e in document["psp_bindings"] if e["control"] != "hold"]
        self._refused(document, "no entry for 'hold'")

    def test_refuses_duplicate_control(self):
        document = copy.deepcopy(self.valid)
        document["psp_bindings"].append({"control": "cross", "primary": "west"})
        self._refused(document, "duplicate binding for 'cross'")

    def test_refuses_unknown_control(self):
        document = copy.deepcopy(self.valid)
        document["psp_bindings"][0]["control"] = "analog"
        self._refused(document, "unknown name 'analog'")

    def test_refuses_unknown_navigation_action(self):
        document = copy.deepcopy(self.valid)
        document["navigation_bindings"][0]["action"] = "submit"
        self._refused(document, "unknown name 'submit'")

    def test_refuses_unrecognized_host_input(self):
        document = copy.deepcopy(self.valid)
        document["psp_bindings"][10]["primary"] = "gamepad_x"
        self._refused(document, "unrecognized host input 'gamepad_x'")

    def test_refuses_unknown_prefixed_host_button(self):
        document = copy.deepcopy(self.valid)
        document["psp_bindings"][10]["primary"] = "button:bogus"
        self._refused(document, "unknown host button 'bogus'")

    def test_refuses_empty_binding_string(self):
        document = copy.deepcopy(self.valid)
        document["psp_bindings"][10]["primary"] = ""
        self._refused(document, "non-empty string")

    def test_refuses_binding_entry_naming_no_source(self):
        document = copy.deepcopy(self.valid)
        document["psp_bindings"][10] = {"control": "cross"}
        self._refused(document, 'names neither "primary" nor "secondary"')

    def test_refuses_one_host_source_driving_two_controls(self):
        document = copy.deepcopy(self.valid)
        document["psp_bindings"][10]["primary"] = "back"
        self._refused(document, "bound to both 'select' and 'cross'")

    def test_refuses_out_of_range_trigger_threshold(self):
        document = copy.deepcopy(self.valid)
        document["calibration"]["trigger_threshold"] = 40000
        self._refused(document, "trigger_threshold")

    def test_refuses_combined_deadzones_at_the_limit(self):
        document = copy.deepcopy(self.valid)
        document["calibration"]["analog_x"]["deadzone_inner"] = 32767
        self._refused(document, "combined deadzones")

    def test_refuses_unknown_analog_host_axis(self):
        document = copy.deepcopy(self.valid)
        document["calibration"]["analog_y"]["host_axis"] = "hat"
        self._refused(document, "analog_y.host_axis")

    def test_refuses_non_boolean_inversion(self):
        document = copy.deepcopy(self.valid)
        document["calibration"]["analog_x"]["inverted"] = "no"
        self._refused(document, "must be a boolean")

    def test_refuses_schema_version_one(self):
        document = copy.deepcopy(self.valid)
        document["schema_version"] = 1
        self._refused(document, "schema_version 2")

    def test_refuses_per_title_entries_in_public_profile(self):
        document = copy.deepcopy(self.valid)
        document["per_title"] = [{"disc_id": "TEST00006", "profile": {}}]
        self._refused(document, "is not a field of the public input profile")


class ManifestContractTests(unittest.TestCase):

    def test_checked_in_public_manifests_pass_the_route(self):
        paths = sorted(tq.TITLE_DIR.glob("*.json"))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(manifest=path.name):
                tq.load_public_manifest(path)

    def test_manifest_schema_agrees_with_the_normative_validator(self):
        self.assertEqual(tq.manifest_schema_problems(), [])

    def test_retail_manifest_is_refused_as_local_only(self):
        manifest = json.loads(DISPLAY_SMOKE.read_text(encoding="utf-8"))
        manifest["id"] = "retail-probe-v1"
        manifest["kind"] = "retail"
        manifest["disc"] = {"id": "ABCD12345", "region": "OTHER", "revision_policy": "exact-disc-id"}
        with tempfile.TemporaryDirectory(prefix="tq-retail-") as temp:
            path = Path(temp) / "retail.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaises(tq.QualificationError) as raised:
                tq.load_public_manifest(path)
        self.assertIn("local-only", str(raised.exception))

    def test_disc_image_declaration_is_refused(self):
        manifest = _public_manifest()
        manifest["filesystem"]["disc_image"] = "GAME.ISO"
        self.assertTrue(any("disc_image" in p for p in tq.declared_path_problems(manifest)))

    def test_missing_build_is_named_when_required(self):
        manifest = _public_manifest()
        with tempfile.TemporaryDirectory(prefix="tq-build-") as temp:
            problems = tq.staging_problems(
                manifest, runtime_root=ROOT, build_root=Path(temp), require_build=True)
        self.assertTrue(any("not built" in p and TITLE_ID in p for p in problems), problems)

    def test_missing_build_is_not_a_validation_failure_by_default(self):
        manifest = _public_manifest()
        with tempfile.TemporaryDirectory(prefix="tq-build-") as temp:
            problems = tq.staging_problems(
                manifest, runtime_root=ROOT, build_root=Path(temp), require_build=False)
        self.assertEqual(problems, [])


class LaunchReportTests(unittest.TestCase):
    """Reports are bring-up-schema documents, and their failure classes are the sweep's blocker codes."""

    def _report(self, output: str = None, **kwargs) -> dict:
        options = {"returncode": 0, "timed_out": False, "duration_ms": 10}
        options.update(kwargs)
        return tq.launch_report(
            title=_public_manifest(),
            output=_pass_output() if output is None else output,
            **options,
        )

    def _assert_bringup_valid(self, report: dict):
        nk_cli.validate_bringup_report(report)

    def test_pass_is_a_schema_valid_first_presented_frame(self):
        report = self._report()
        self._assert_bringup_valid(report)
        self.assertEqual(report["failure_class"], "NONE")
        self.assertEqual(report["reached_stage"], "launch")
        self.assertEqual(report["stages"]["launch"]["status"], "PASS")
        self.assertEqual(report["presentation"]["status"], "FRAME_SUBMITTED")
        self.assertEqual(report["presentation"]["frame_submissions"], 2)
        self.assertEqual(report["presentation"]["backend"], "offscreen")
        self.assertEqual(report["exit_classification"], "EXITED_ZERO")
        self.assertEqual(report["process_exit_code"], 0)
        self.assertEqual(library_sweep._furthest_stage(report), "first presented frame")

    def test_missing_presented_frames_is_no_frame_submissions(self):
        output = "\n".join(
            line for line in _pass_output(submissions=0).splitlines()
            if not line.startswith(("HOST_PRESENT_SUBMITTED", "BOOT_EVENT phase=frame_present"))
        )
        report = self._report(output)
        self._assert_bringup_valid(report)
        self.assertEqual(report["failure_class"], "NO_FRAME_SUBMISSIONS")
        self.assertEqual(report["stages"]["launch"]["status"], "FAIL")
        self.assertEqual(library_sweep._furthest_stage(report), "launch")

    def test_absent_first_checkpoint_is_display_progress_unverified(self):
        output = "\n".join(
            line for line in _pass_output().splitlines() if "first_frame" not in line)
        report = self._report(output)
        self._assert_bringup_valid(report)
        self.assertEqual(report["failure_class"], "DISPLAY_PROGRESS_UNVERIFIED")

    def test_blank_first_checkpoint_is_display_progress_unverified(self):
        report = self._report(_pass_output(first_frame="nonzero_pixels=0 total_pixels=130560"))
        self.assertEqual(report["failure_class"], "DISPLAY_PROGRESS_UNVERIFIED")

    def test_windowed_backend_is_not_accepted_by_the_offscreen_route(self):
        report = self._report(_pass_output(backend="vulkan"))
        self._assert_bringup_valid(report)
        self.assertEqual(report["failure_class"], "DISPLAY_PROGRESS_UNVERIFIED")

    def test_another_runtime_is_refused_as_launch_failed(self):
        report = self._report(_pass_output(title="synthetic-title2-v1"))
        self._assert_bringup_valid(report)
        self.assertEqual(report["failure_class"], "LAUNCH_FAILED")

    def test_player_title_name_mismatch_is_launch_failed(self):
        report = self._report(_pass_output(name="Some Other Fixture"))
        self.assertEqual(report["failure_class"], "LAUNCH_FAILED")

    def test_nonzero_child_exit_is_launch_failed(self):
        report = self._report(_pass_output(exit_code=3), returncode=3)
        self._assert_bringup_valid(report)
        self.assertEqual(report["failure_class"], "LAUNCH_FAILED")
        self.assertEqual(report["exit_classification"], "EXITED_NONZERO")
        self.assertEqual(report["process_exit_code"], 3)

    def test_native_crash_report_is_named(self):
        output = _pass_output(exit_code=1) + "=== PSP RECOMPILER CRASH REPORT ===\n"
        report = self._report(output, returncode=1)
        self._assert_bringup_valid(report)
        self.assertEqual(report["runtime_output_kind"], "NATIVE_CRASH_REPORT")
        self.assertEqual(report["failure_class"], "NATIVE_RUNTIME_CRASH")

    def test_timeout_is_launch_timeout_with_no_exit_code(self):
        report = self._report("", returncode=None, timed_out=True, duration_ms=120000)
        self._assert_bringup_valid(report)
        self.assertEqual(report["failure_class"], "LAUNCH_TIMEOUT")
        self.assertEqual(report["exit_classification"], "TIMED_OUT")
        self.assertIsNone(report["process_exit_code"])
        self.assertEqual(report["stages"]["launch"]["status"], "TIMED_OUT")
        self.assertEqual(library_sweep._furthest_stage(report), "launch")

    def test_unbuilt_candidate_is_entry_not_compiled_at_compile_stage(self):
        report = tq.staging_failure_report()
        self._assert_bringup_valid(report)
        self.assertEqual(report["failure_class"], "ENTRY_NOT_COMPILED")
        self.assertEqual(report["reached_stage"], "compile")
        self.assertEqual(library_sweep._furthest_stage(report), "compile")

    def test_every_failure_class_is_in_the_sweep_vocabulary(self):
        outputs = [
            (_pass_output(submissions=0), 0, False),
            (_pass_output(), 3, False),
            ("", None, True),
            (_pass_output(backend="vulkan"), 0, False),
            (_pass_output(title="other-v1"), 0, False),
        ]
        classes = {self._report(output, returncode=code, timed_out=timed)["failure_class"]
                   for output, code, timed in outputs}
        classes.add(tq.staging_failure_report()["failure_class"])
        self.assertLessEqual(classes, library_sweep._PUBLIC_BLOCKER_CODES)

    def test_issue_numbers_are_the_schema_allowlist(self):
        report = self._report(_pass_output(submissions=0))
        self.assertLessEqual(set(report["issue_numbers"]), set(library_sweep._PUBLIC_ISSUE_NUMBERS))


class SandboxAndStagingTests(unittest.TestCase):

    def test_child_environment_points_every_user_location_into_the_sandbox(self):
        base = {"PATH": "x", "SR_FLIGHT": "ge,present", "SR_DATAROOT": "C:/elsewhere",
                "LOCALAPPDATA": "C:/real/profile"}
        with tempfile.TemporaryDirectory(prefix="tq-env-") as temp:
            sandbox = Path(temp)
            env = tq.child_environment(base, sandbox)
            self.assertEqual(env["LOCALAPPDATA"], str(sandbox / "localappdata"))
            self.assertEqual(env["APPDATA"], str(sandbox / "appdata"))
            self.assertEqual(env["SR_VIDEO"], "offscreen")
            self.assertEqual(env["SDL_VIDEO_DRIVER"], "dummy")
            self.assertNotIn("SR_FLIGHT", env)
            self.assertNotIn("SR_DATAROOT", env)
            self.assertEqual(base["LOCALAPPDATA"], "C:/real/profile")

    def test_input_profile_is_staged_where_the_player_reads_it(self):
        with tempfile.TemporaryDirectory(prefix="tq-profile-") as temp:
            sandbox = Path(temp)
            staged = tq.stage_input_profile(sandbox, PROFILE)
            self.assertEqual(staged, sandbox / "localappdata" / "Nakagawa" / "config" / "input_profile.json")
            self.assertEqual(staged.read_bytes(), PROFILE.read_bytes())

    def test_launch_command_isolates_user_data_and_names_the_sample(self):
        with tempfile.TemporaryDirectory(prefix="tq-cmd-") as temp:
            command = tq.launch_command(Path("player.exe"), Path(temp), ROOT, 1)
        self.assertIn("--demo", command)
        self.assertIn("--launch-index=1", command)
        self.assertTrue(any(arg.startswith("--user-data-root=") for arg in command))


class LaunchProcessTests(unittest.TestCase):

    def test_a_hung_launch_is_stopped_at_the_timeout(self):
        hang = [sys.executable, "-c", "import time; time.sleep(60)"]
        with tempfile.TemporaryDirectory(prefix="tq-hang-") as temp:
            with mock.patch.object(tq, "launch_command", return_value=hang):
                returncode, _output, timed_out, duration_ms = tq.run_player(
                    Path(sys.executable), Path(temp), ROOT, 1, timeout_seconds=2)
        self.assertTrue(timed_out)
        self.assertIsNone(returncode)
        self.assertLess(duration_ms, 30000)


class SmokeRouteRefusalTests(unittest.TestCase):

    def test_titles_without_a_public_launch_surface_are_refused_by_name(self):
        with tempfile.TemporaryDirectory(prefix="tq-refuse-") as temp:
            with self.assertRaises(tq.QualificationError) as raised:
                tq.run_smoke(
                    ROOT / "assets" / "titles" / "synthetic.json",
                    build_root=Path(temp), input_profile=PROFILE, player=Path(temp) / "p.exe")
        self.assertIn("no public launch surface", str(raised.exception))

    def test_retail_titles_are_refused_before_any_launch(self):
        manifest = json.loads(DISPLAY_SMOKE.read_text(encoding="utf-8"))
        manifest["id"] = "display-smoke-v1"
        manifest["kind"] = "retail"
        manifest["disc"] = {"id": "ABCD12345", "region": "OTHER", "revision_policy": "exact-disc-id"}
        with tempfile.TemporaryDirectory(prefix="tq-retail-smoke-") as temp:
            path = Path(temp) / "retail.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaises(tq.QualificationError):
                tq.run_smoke(path, build_root=Path(temp), input_profile=PROFILE, player=Path(temp) / "p.exe")

    def test_unbuilt_fixture_writes_a_schema_valid_entry_not_compiled_report(self):
        with tempfile.TemporaryDirectory(prefix="tq-unbuilt-") as temp:
            build_root = Path(temp)
            status, report_path, report = tq.run_smoke(
                DISPLAY_SMOKE, build_root=build_root, input_profile=PROFILE,
                player=build_root / "no-such-player.exe")
            written = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(status, 1)
        self.assertEqual(report["failure_class"], "ENTRY_NOT_COMPILED")
        nk_cli.validate_bringup_report(written)
        self.assertEqual(written, report)

    def test_cli_validates_the_checked_in_assets(self):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            status = tq.main(["validate"])
        self.assertEqual(status, 0, output.getvalue())


_BUILT_FIXTURE = any(
    candidate.is_file()
    for candidate in (ROOT / "build" / TITLE_ID / f"{TITLE_ID}.exe", ROOT / "build" / TITLE_ID / TITLE_ID)
)


@unittest.skipUnless(
    _BUILT_FIXTURE and (ROOT / "build" / tq.PLAYER_NAME).is_file(),
    "the public smoke needs the display fixture and native player: run `make display-smoke player` first",
)
class PublicSmokeTests(unittest.TestCase):
    """Load, stage and run the public display fixture through the native player, in a sandbox."""

    def test_display_fixture_presents_a_first_checkpoint_and_exits_cleanly(self):
        output_dir = ROOT / "build" / TITLE_ID / "title-qualification"
        with contextlib.redirect_stdout(io.StringIO()):
            code = tq.main(["smoke", "--manifest", str(DISPLAY_SMOKE)])
        report_path = output_dir / tq.REPORT_NAME
        report = json.loads(report_path.read_text(encoding="utf-8"))
        nk_cli.validate_bringup_report(report)
        self.assertEqual(code, 0, report)
        self.assertEqual(report["failure_class"], "NONE")
        self.assertEqual(report["presentation"]["status"], "FRAME_SUBMITTED")
        self.assertGreater(report["presentation"]["frame_submissions"], 0)
        self.assertEqual(report["exit_classification"], "EXITED_ZERO")
        self.assertEqual(library_sweep._furthest_stage(report), "first presented frame")
        log = (output_dir / tq.OUTPUT_LOG_NAME).read_text(encoding="utf-8")
        self.assertIn("phase=first_frame source=cpu", log)
        leftovers = [p.name for p in output_dir.iterdir() if p.name.startswith("title-qualification-")]
        self.assertEqual(leftovers, [], "the sandbox must be removed after the run")


if __name__ == "__main__":
    unittest.main()
