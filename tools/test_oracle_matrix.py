# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Host-side tests for the one-command PSP oracle matrix (tools/psp_oracle/oracle_matrix.py, issue #352).

A fake device stands in for the build shell, the PSPLink soft reset, the resident agent's recover and probe
commands and the hardware lock, so every decision is exercised without a PSP: one launch per fresh boot,
identity-bound acceptance, stale-result rejection, crash containment with a bounded budget, and an explicit
intervention state for a wedge.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import sys
import tempfile
import time
import unittest

from tools.psp_oracle import oracle_matrix as om

COMMIT = "1234567890abcdef1234567890abcdef12345678"
SESSION = "matrix-test"


def _utc(moment: datetime) -> str:
    return moment.strftime("%Y-%m-%dT%H:%M:%SZ")


class FakeDevice:
    """The PSP, its resident agent and the build shell, driven by argv markers."""

    def __init__(self, root: Path, lock_path: Path, behaviours: dict[str, str]) -> None:
        self.root = root
        self.lock_path = lock_path
        self.behaviours = behaviours
        self.boot = 70
        self.pending_reset = False
        self.reset_works = True
        self.recover_exit = 0
        self.recover_failures_after_reset = False
        self.calls: list[str] = []
        self.launched: list[tuple[str, int]] = []
        self.attest()

    def attest(self) -> None:
        lock = json.loads(self.lock_path.read_text(encoding="utf-8"))
        lock["device_state_attestation"] = {
            "schema": 1,
            "boot_number": self.boot,
            "qualification_result": "QUALIFIED",
            "qualified_at_utc": _utc(datetime.now(timezone.utc)),
            "session_id": SESSION,
            "watchdog_state": {"boot_number": self.boot, "healthy": True, "armed": False, "remaining_s": 0},
        }
        self.lock_path.write_text(json.dumps(lock), encoding="utf-8")

    def __call__(self, argv: list[str], timeout: float, stdin: str | None) -> tuple[int | None, str, str]:
        kind = argv[0]
        self.calls.append(kind)
        if kind == "fake-build":
            return self.build(stdin or "")
        if kind == "fake-reset":
            if self.reset_works:
                self.pending_reset = True
            return None, "", ""                      # the reset drops USB: its command times out
        if kind == "fake-recover":
            if self.pending_reset:
                self.pending_reset = False
                self.boot += 1
                if self.recover_failures_after_reset:
                    return 3, "", "needs-person"
            if self.recover_exit != 0:
                return self.recover_exit, "", "needs-person"
            self.attest()
            return 0, "{}", ""
        if kind == "fake-probe":
            return self.probe(argv)
        raise AssertionError(f"unexpected command {argv}")

    def build(self, script: str) -> tuple[int | None, str, str]:
        assert "git checkout -q --detach " + COMMIT in script
        assert "git status --porcelain --untracked-files=normal" in script
        out = []
        for line in script.splitlines():
            if line.startswith("build_case "):
                _, case_id, staged = shlex.split(line)
                data = f"prx for {case_id} at {COMMIT}".encode()
                Path(staged).write_bytes(data)
                out.append(f"NK_MATRIX_BUILT {case_id} {hashlib.sha256(data).hexdigest()}")
        out.append("NK_MATRIX_BUILD_DONE")
        return 0, "\n".join(out) + "\n", ""

    def probe(self, argv: list[str]) -> tuple[int | None, str, str]:
        out = Path(argv[argv.index("--out") + 1])
        cases = [argv[i + 1].split("=", 1) for i, word in enumerate(argv) if word == "--campaign-case"]
        assert cases[0][0] == "transport-write"
        case_id = cases[1][0]
        boot = self.boot
        self.launched.append((case_id, boot))
        behaviour = self.behaviours.get(case_id, "ok")
        if behaviour == "wedge":
            return 3, json.dumps({"verdict": "needs-person", "boots": [boot], "detail": "no channel answers"}), ""
        if behaviour == "crash":
            self.boot += 1                           # the resident agent reset the PSP during the launch
            self.attest()
            return 4, json.dumps({"verdict": "recovered", "boots": [boot, self.boot]}), ""
        if behaviour == "refused":
            return 2, json.dumps({"verdict": "refused", "boots": []}), ""
        boots = [boot]
        if behaviour == "reboot-ok":
            self.boot += 1                           # the runner's own L2 reset rebooted the PSP mid-launch
            self.attest()
            boots.append(self.boot)
        now = time.time_ns()
        envelopes = []
        for name, prx in cases:
            sha = hashlib.sha256(Path(prx).read_bytes()).hexdigest()
            envelopes.append({
                "CASE_ID": name,
                "SOURCE_COMMIT": COMMIT,
                "BINARY_SHA256": "0" * 64 if behaviour == "wrong-sha" and name == case_id else sha,
                "FW": "6.6.1",
                "HOST0_RUN_STARTED_NS": now - (3600 * 10**9 if behaviour == "stale" else 0),
                "ACCEPTANCE_ELIGIBLE": behaviour != "ineligible" or name != case_id,
                "ACCEPTANCE_BLOCKERS": ["one or more scalar result records did not pass"]
                if behaviour == "ineligible" and name == case_id else [],
            })
        if behaviour == "wrong-commit":
            envelopes[1]["SOURCE_COMMIT"] = "f" * 40
        out.write_text(json.dumps({"envelopes": envelopes, "terminal_reason": None}), encoding="utf-8")
        return 0, json.dumps({"verdict": "ok", "boots": boots}), ""


class MatrixTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        base = Path(self._tmp.name)
        self.repo = base / "repo"
        (self.repo / "fixtures" / "psp_oracle").mkdir(parents=True)
        self.private = base / "private"
        self.private.mkdir()
        self.lock_path = base / "HARDWARE_LOCK.json"
        acquired = datetime.now(timezone.utc) - timedelta(minutes=1)
        self.lock_path.write_text(json.dumps({
            "state": "HELD", "holder_session": SESSION, "acquired_utc": _utc(acquired), "probes_run": [],
        }), encoding="utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def plan_dict(self, cases: list[dict[str, object]], **extra: object) -> dict[str, object]:
        return {
            "schema": 1,
            "matrix_id": "m1",
            "session_id": SESSION,
            "source_commit": COMMIT,
            "console_model": "PSP-3000",
            "expected_firmware": "6.6.1",
            "host0_root": "build/host0-m1",
            "evidence_dir": "evidence",
            "cases": cases,
            "commands": {
                "build_shell_argv": ["fake-build"],
                "reset_argv": ["fake-reset"],
                "recover_argv": ["fake-recover", "--session", "{session_id}", "--host0-root", "{host0_root}"],
                "probe_argv": ["fake-probe", "--deadline", "{deadline}", "--log", "{runner_log}", "--"],
                "pspsh_argv": ["pspsh", "-e", "{remote_command}"],
                "usbhostfs_argv": ["usbhostfs_pc", "{host0_root_wsl}"],
            },
            **extra,
        }

    def write_plan(self, value: dict[str, object]) -> Path:
        path = self.private / "plan.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def load(self, cases: list[dict[str, object]], **extra: object) -> om.MatrixPlan:
        return om.load_plan(self.write_plan(self.plan_dict(cases, **extra)), repo_root=self.repo)

    def runner(self, plan: om.MatrixPlan, device: FakeDevice) -> om.MatrixRunner:
        return om.MatrixRunner(plan, command_runner=device, lock_path=self.lock_path,
                               source_tree_problem=lambda commit: None,
                               git_common_dir=lambda root: root / ".git")

    def device(self, **behaviours: str) -> FakeDevice:
        return FakeDevice(self.repo, self.lock_path, {k.replace("_", "-"): v for k, v in behaviours.items()})

    def lock(self) -> dict[str, object]:
        return json.loads(self.lock_path.read_text(encoding="utf-8"))


class PlanValidationTests(MatrixTestCase):
    def refuses(self, pattern: str, cases=None, **extra) -> None:
        with self.assertRaisesRegex(om.MatrixError, pattern):
            self.load(cases if cases is not None else [{"case_id": "delay-zero", "timeout_seconds": 60}], **extra)

    def test_valid_plan_resolves_private_and_scratch_paths(self) -> None:
        plan = self.load([{"case_id": "delay-zero", "timeout_seconds": 60}])
        self.assertEqual(plan.host0_root, (self.repo / "fixtures/psp_oracle/build/host0-m1").resolve())
        self.assertEqual(plan.evidence_dir, (self.private / "evidence").resolve())
        self.assertEqual(plan.max_contained_failures, 1)
        self.assertEqual(plan.cases[0].staged_name, "delay_zero.prx")

    def test_refuses_listing_the_preflight(self) -> None:
        self.refuses("transport-write runs before every case", [{"case_id": "transport-write", "timeout_seconds": 60}])

    def test_refuses_duplicates_retired_and_short_timeouts(self) -> None:
        case = {"case_id": "delay-zero", "timeout_seconds": 60}
        self.refuses("duplicate case id", [case, case])
        self.refuses("retired", [{"case_id": "hle-null-out-pointers", "timeout_seconds": 60}])
        self.refuses("needs timeout_seconds 180", [{"case_id": "ge-timing", "timeout_seconds": 60}])
        self.refuses("timeout_seconds must be", [{"case_id": "delay-zero", "timeout_seconds": 0}])
        self.refuses("timeout_seconds must be", [{"case_id": "delay-zero", "timeout_seconds": 7200}])
        self.refuses("at most 64", [{"case_id": f"c{i}", "timeout_seconds": 60} for i in range(65)])

    def test_refuses_host0_outside_the_fixture_scratch_and_evidence_outside_the_plan(self) -> None:
        self.refuses("host0_root must stay under", host0_root=str(self.private))
        plan = self.plan_dict([{"case_id": "delay-zero", "timeout_seconds": 60}])
        plan["evidence_dir"] = str(self.repo)
        with self.assertRaisesRegex(om.MatrixError, "evidence_dir must stay under"):
            om.load_plan(self.write_plan(plan), repo_root=self.repo)

    def test_prebuilt_prx_needs_a_pinned_digest_inside_the_plan_directory(self) -> None:
        self.refuses("needs its lowercase sha256 pinned", [{"case_id": "nk-hang", "timeout_seconds": 60,
                                                             "prebuilt_prx": "nk_test_hang.prx"}])
        self.refuses("must stay under", [{"case_id": "nk-hang", "timeout_seconds": 60,
                                          "prebuilt_prx": str(self.repo / "x.prx"), "sha256": "a" * 64}])
        self.refuses("sha256 pins a prebuilt_prx only", [{"case_id": "delay-zero", "timeout_seconds": 60,
                                                         "sha256": "a" * 64}])

    def test_refuses_unknown_placeholders_commands_and_bad_bounds(self) -> None:
        plan = self.plan_dict([{"case_id": "delay-zero", "timeout_seconds": 60}])
        plan["commands"]["reset_argv"] = ["fake-reset", "{deadline}"]
        with self.assertRaisesRegex(om.MatrixError, r"unknown placeholder \{deadline\}"):
            om.load_plan(self.write_plan(plan), repo_root=self.repo)
        plan = self.plan_dict([{"case_id": "delay-zero", "timeout_seconds": 60}])
        plan["commands"]["shell"] = ["sh"]
        with self.assertRaisesRegex(om.MatrixError, "unknown commands: shell"):
            om.load_plan(self.write_plan(plan), repo_root=self.repo)
        self.refuses("watchdog_deadline_seconds", watchdog_deadline_seconds=5)
        self.refuses("max_contained_failures", max_contained_failures=9)
        self.refuses("expected_firmware", expected_firmware="6.61")

    def test_render_refuses_a_placeholder_without_a_value(self) -> None:
        self.assertEqual(om.render_argv(["a{x}b"], {"x": "1"}), ["a1b"])
        with self.assertRaisesRegex(om.MatrixError, r"\{y\}"):
            om.render_argv(["{y}"], {"x": "1"})


class BuildTests(MatrixTestCase):
    def test_script_builds_each_case_from_a_clean_clone_of_the_commit(self) -> None:
        plan = self.load([{"case_id": "delay-zero", "timeout_seconds": 60}])
        script = om.build_script(plan, "/repo/.git", [om.MatrixCase("transport-write", 0.0), plan.cases[0]])
        self.assertIn("git clone -q --no-checkout --no-hardlinks /repo/.git", script)
        self.assertIn(f"git checkout -q --detach {COMMIT}", script)
        self.assertIn('[ -z "$(git status --porcelain --untracked-files=normal)" ] || fail dirty', script)
        self.assertIn('BUILD_DIR="build/$1" EBOOT.PBP', script)
        staged = re.findall(r"^build_case (\S+) (\S+)$", script, re.MULTILINE)
        self.assertEqual([name for name, _ in staged], ["transport-write", "delay-zero"])
        self.assertTrue(staged[1][1].endswith("host0-m1/delay_zero.prx"))

    def test_wsl_path_style_converts_staged_paths(self) -> None:
        plan = self.load([{"case_id": "delay-zero", "timeout_seconds": 60}])
        wsl = om.MatrixPlan(**{**plan.__dict__, "path_style": "wsl",
                               "host0_root": Path("C:/nk/wt/fixtures/psp_oracle/build/h")})
        self.assertIn("/mnt/c/nk/wt/fixtures/psp_oracle/build/h/delay_zero.prx",
                      om.build_script(wsl, "/mnt/c/nk/main/.git", [plan.cases[0]]))

    def test_build_script_reaches_the_shell_with_lf_line_ends(self) -> None:
        # Measured on Windows 2026-10-29: text-mode stdin turned every LF into CRLF and bash refused the script.
        code, out, _err = om.run_command(
            [sys.executable, "-c", "import sys; sys.stdout.write(sys.stdin.buffer.read().hex())"], 60,
            "set -u\nbuild_case() {\n}\n")
        self.assertEqual(code, 0)
        self.assertEqual(bytes.fromhex(out), b"set -u\nbuild_case() {\n}\n")

    def test_incomplete_or_failed_build_output_is_a_refusal(self) -> None:
        good = f"NK_MATRIX_BUILT a {'1' * 64}\nNK_MATRIX_BUILD_DONE\n"
        self.assertEqual(om.parse_build_output(good, ["a"]), {"a": "1" * 64})
        with self.assertRaisesRegex(om.MatrixError, "did not report every case"):
            om.parse_build_output(f"NK_MATRIX_BUILT a {'1' * 64}\n", ["a"])
        with self.assertRaisesRegex(om.MatrixError, "did not report every case"):
            om.parse_build_output(good, ["a", "b"])
        with self.assertRaisesRegex(om.MatrixError, "build failed: make b"):
            om.parse_build_output("NK_MATRIX_BUILD_FAILED make b\n", ["a", "b"])

    def test_staged_digest_must_match_the_reported_one(self) -> None:
        plan = self.load([{"case_id": "delay-zero", "timeout_seconds": 60}])
        device = self.device()
        real_build = device.build

        def tampering_build(script: str):
            code, out, err = real_build(script)
            (plan.host0_root / "delay_zero.prx").write_bytes(b"swapped after the build")
            return code, out, err

        device.build = tampering_build  # type: ignore[method-assign]
        with self.assertRaisesRegex(om.MatrixError, "differs from the one the build reported"):
            self.runner(plan, device).build()

    def test_prebuilt_prx_is_staged_only_with_its_pinned_digest(self) -> None:
        data = b"resident-agent containment test probe"
        (self.private / "nk_test_hang.prx").write_bytes(data)
        good = hashlib.sha256(data).hexdigest()
        plan = self.load([{"case_id": "nk-hang", "timeout_seconds": 60, "prebuilt_prx": "nk_test_hang.prx",
                           "sha256": good}])
        staged = self.runner(plan, self.device()).build()
        self.assertEqual(staged["nk-hang"], good)
        self.assertEqual(list(staged), ["transport-write", "nk-hang"])
        bad = self.load([{"case_id": "nk-hang", "timeout_seconds": 60, "prebuilt_prx": "nk_test_hang.prx",
                          "sha256": "b" * 64}])
        with self.assertRaisesRegex(om.MatrixError, "not the pinned one"):
            self.runner(bad, self.device()).build()


class MatrixExecutionTests(MatrixTestCase):
    CASES = [{"case_id": "delay-zero", "timeout_seconds": 60}, {"case_id": "ge-nan", "timeout_seconds": 60},
             {"case_id": "dma-cells", "timeout_seconds": 60}]

    def test_healthy_matrix_runs_each_case_on_its_own_fresh_boot(self) -> None:
        plan = self.load(self.CASES)
        device = self.device()
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 0, report)
        self.assertEqual(report["accepted"], ["delay-zero", "ge-nan", "dma-cells"])
        self.assertEqual(device.launched, [("delay-zero", 71), ("ge-nan", 72), ("dma-cells", 73)])
        self.assertEqual(device.calls.count("fake-reset"), 3)
        self.assertEqual(report["state"], "COMPLETE")
        notes = self.lock()["probes_run"]
        self.assertEqual(len(notes), 6)
        self.assertIn("boot 71, launching transport-write + delay-zero", notes[0])
        self.assertIn("delay-zero on boot 71: ACCEPTED", notes[1])
        saved = json.loads(plan.report_path.read_text(encoding="utf-8"))
        self.assertEqual(saved["accepted"], report["accepted"])

    def test_crashing_case_is_contained_and_never_accepted(self) -> None:
        plan = self.load(self.CASES)
        device = self.device(ge_nan="crash")
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)
        statuses = [item["status"] for item in report["outcomes"]]
        self.assertEqual(statuses, [om.ACCEPTED, om.CONTAINED, om.ACCEPTED])
        # The crash rebooted the PSP to boot 73; the next case still gets its own newer boot.
        self.assertEqual(device.launched, [("delay-zero", 71), ("ge-nan", 72), ("dma-cells", 74)])
        self.assertIn(73, report["used_boots"])
        self.assertIn("rebooted during the launch (boot 72 -> 73)", " ".join(report["outcomes"][1]["reasons"]))

    def test_reboot_during_an_otherwise_eligible_launch_is_contained(self) -> None:
        plan = self.load(self.CASES[:2])
        device = self.device(delay_zero="reboot-ok")
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)
        self.assertEqual([item["status"] for item in report["outcomes"]], [om.CONTAINED, om.ACCEPTED])
        self.assertEqual(report["outcomes"][0]["probe_exit"], 0)
        self.assertEqual(device.launched, [("delay-zero", 71), ("ge-nan", 73)])

    def test_contained_failure_budget_stops_the_matrix(self) -> None:
        plan = self.load(self.CASES, max_contained_failures=0)
        device = self.device(delay_zero="crash")
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)
        self.assertEqual(report["state"], "CONTAINED_FAILURE_BUDGET_EXHAUSTED")
        self.assertEqual([item["case_id"] for item in report["not_run"]], ["ge-nan", "dma-cells"])
        self.assertEqual(len(device.launched), 1)

    def test_wedge_ends_in_an_explicit_intervention_state(self) -> None:
        plan = self.load(self.CASES)
        device = self.device(ge_nan="wedge")
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 3)
        self.assertEqual(report["state"], om.INTERVENTION)
        self.assertEqual(report["outcomes"][1]["status"], om.INTERVENTION)
        self.assertEqual([item["case_id"] for item in report["not_run"]], ["dma-cells"])
        self.assertEqual(len(device.launched), 2)

    def test_failed_recovery_after_the_reset_stops_before_any_launch(self) -> None:
        plan = self.load(self.CASES)
        device = self.device()
        device.recover_failures_after_reset = True
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 3)
        self.assertEqual(device.launched, [])
        self.assertIn("recover command exited 3", report["outcomes"][0]["reasons"][0])

    def test_reset_that_yields_no_newer_boot_is_not_a_fresh_epoch(self) -> None:
        plan = self.load(self.CASES)
        device = self.device()
        device.reset_works = False
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 3)
        self.assertEqual(device.launched, [])
        self.assertIn("did not produce a newer qualified boot", report["outcomes"][0]["reasons"][0])

    def test_stale_wrong_fixture_and_wrong_runner_results_are_rejected(self) -> None:
        for behaviour, reason in (("stale", "does not belong to this launch"),
                                  ("wrong-sha", "not the staged fixture's"),
                                  ("wrong-commit", "wrong runner")):
            with self.subTest(behaviour=behaviour):
                self.tearDown()
                self.setUp()
                plan = self.load(self.CASES[:1])
                code, report = self.runner(plan, self.device(delay_zero=behaviour)).execute()
                self.assertEqual(code, 2)
                outcome = report["outcomes"][0]
                self.assertEqual(outcome["status"], om.REJECTED)
                self.assertIn(reason, " ".join(outcome["reasons"]))
                self.assertEqual(report["accepted"], [])

    def test_ineligible_and_refused_launches_are_reported_not_accepted(self) -> None:
        plan = self.load(self.CASES[:2])
        code, report = self.runner(plan, self.device(delay_zero="ineligible", ge_nan="refused")).execute()
        self.assertEqual(code, 2)
        first, second = report["outcomes"]
        self.assertEqual(first["status"], om.NOT_ELIGIBLE)
        self.assertIn("did not pass", " ".join(first["reasons"]))
        self.assertEqual(second["status"], om.NOT_ELIGIBLE)
        self.assertIn("wrote no report", " ".join(second["reasons"]))

    def test_lock_held_by_another_session_refuses_before_touching_the_device(self) -> None:
        plan = self.load(self.CASES)
        device = self.device()
        lock = self.lock()
        lock["holder_session"] = "someone-else"
        self.lock_path.write_text(json.dumps(lock), encoding="utf-8")
        with self.assertRaisesRegex(om.MatrixError, "HARDWARE_LOCK_SESSION_MISMATCH"):
            self.runner(plan, device).execute()
        self.assertNotIn("fake-reset", device.calls)
        self.assertNotIn("fake-recover", device.calls)
        self.assertEqual(device.launched, [])

    def test_resume_continues_after_an_intervention_without_retrying_the_case(self) -> None:
        plan = self.load(self.CASES)
        device = self.device(ge_nan="wedge")
        self.assertEqual(self.runner(plan, device).execute()[0], 3)
        device.behaviours["ge-nan"] = "ok"
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)                      # ge-nan stays an intervention, not a pass
        self.assertEqual([item["status"] for item in report["outcomes"]],
                         [om.ACCEPTED, om.INTERVENTION, om.ACCEPTED])
        self.assertEqual([case for case, _ in device.launched], ["delay-zero", "ge-nan", "dma-cells"])
        self.assertEqual(device.calls.count("fake-build"), 1)

    def test_resume_refuses_a_changed_fixture_or_plan(self) -> None:
        plan = self.load(self.CASES)
        device = self.device(ge_nan="wedge")
        self.runner(plan, device).execute()
        (plan.host0_root / "dma_cells.prx").write_bytes(b"rebuilt behind the checkpoint")
        with self.assertRaisesRegex(om.MatrixError, "changed since it was built"):
            self.runner(plan, device).execute()
        other = self.load(self.CASES[:2])
        with self.assertRaisesRegex(om.MatrixError, "different matrix"):
            self.runner(other, device).execute()

    def test_build_only_touches_no_device(self) -> None:
        plan = self.load(self.CASES)
        device = self.device()
        code, report = self.runner(plan, device).execute(build_only=True)
        self.assertEqual(code, 0)
        self.assertEqual(report["state"], "BUILT")
        self.assertEqual(device.calls, ["fake-build"])
        self.assertEqual(sorted(report["staged"]), sorted(["transport-write", "delay-zero", "ge-nan", "dma-cells"]))

    def test_dirty_source_checkout_is_refused_before_the_build(self) -> None:
        plan = self.load(self.CASES)
        device = self.device()
        runner = om.MatrixRunner(plan, command_runner=device, lock_path=self.lock_path,
                                 source_tree_problem=lambda commit: "source worktree is dirty",
                                 git_common_dir=lambda root: root / ".git")
        with self.assertRaisesRegex(om.MatrixError, "dirty"):
            runner.execute()
        self.assertEqual(device.calls, [])


class ClassifyLaunchTests(MatrixTestCase):
    def classify(self, path: Path, **overrides):
        values = dict(case=om.MatrixCase("delay-zero", 60.0), epoch_boot=80, probe_exit=0,
                      probe_stdout='{"boots": [80]}', runner_report_path=path, launch_started_ns=time.time_ns(),
                      staged={"transport-write": "a" * 64, "delay-zero": "b" * 64}, source_commit=COMMIT,
                      expected_firmware="6.6.1")
        values.update(overrides)
        return om.classify_launch(**values)

    def test_report_file_older_than_the_launch_is_stale(self) -> None:
        path = self.private / "out.json"
        path.write_text(json.dumps({"envelopes": []}), encoding="utf-8")
        old = time.time() - 3600
        os.utime(path, (old, old))
        outcome = self.classify(path)
        self.assertEqual(outcome.status, om.REJECTED)
        self.assertIn("predates this launch", outcome.reasons[0])

    def test_runner_stopping_after_the_preflight_is_not_eligible(self) -> None:
        path = self.private / "out.json"
        path.write_text(json.dumps({"terminal_reason": "HOST0_ROUNDTRIP_FAILED", "envelopes": [{
            "CASE_ID": "transport-write", "SOURCE_COMMIT": COMMIT, "BINARY_SHA256": "a" * 64, "FW": "6.6.1",
            "HOST0_RUN_STARTED_NS": time.time_ns(), "ACCEPTANCE_ELIGIBLE": False}]}), encoding="utf-8")
        outcome = self.classify(path)
        self.assertEqual(outcome.status, om.NOT_ELIGIBLE)
        self.assertIn("HOST0_ROUNDTRIP_FAILED", outcome.reasons[0])

    def test_unexpected_case_order_is_rejected(self) -> None:
        path = self.private / "out.json"
        path.write_text(json.dumps({"envelopes": [{"CASE_ID": "delay-zero"}]}), encoding="utf-8")
        self.assertEqual(self.classify(path).status, om.REJECTED)

    def test_probe_command_timeout_is_an_intervention(self) -> None:
        outcome = self.classify(self.private / "missing.json", probe_exit=None, probe_stdout="")
        self.assertEqual(outcome.status, om.INTERVENTION)


if __name__ == "__main__":
    unittest.main()
