# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""tools/library_build.py: a whole folder of discs into one data folder, never stopping on a title."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import library_build  # noqa: E402
from nk_core import decrypt_tool  # noqa: E402
from test_iso_parity import create_test_iso, create_test_iso_with_executables  # noqa: E402

# A stand-in for `nk_cli.py bringup`: what it does depends on the disc file's name.
STUB_BRINGUP = r'''
import json, sys, time
from pathlib import Path
iso, data, work = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
name = iso.stem
stages = {s: {"status": "NOT_RUN", "duration_ms": 0} for s in
          ("inspect", "prepare_import", "analyze", "codegen", "compile", "build_package", "launch")}
report = {"reached_stage": "inspect", "stages": stages, "failure_class": "NONE",
          "unsupported_imports": [], "runtime_imports": [], "counts": {
              "functions": 1, "instructions": 1, "modules": 2, "encrypted_modules": 1,
              "unsupported_opcodes": {}},
          "exit_classification": "NOT_RUN", "runtime_output_kind": "NOT_RUN",
          "process_exit_code": None, "presentation": {"status": "NOT_RUN", "frame_submissions": 0}}
log = work.parent / "calls.log"
with open(log, "a", encoding="utf-8") as stream:
    stream.write(name + "\n")
if name.startswith("slow"):
    progress = {"schema_version": 1, "stages": {"compile": {
        "status": "RUNNING", "started_at_unix_ms": 1, "finished_at_unix_ms": None,
        "duration_ms": 0}}}
    (work / "bringup-progress.json").write_text(json.dumps(progress), encoding="utf-8")
    time.sleep(60)
if name.startswith("crash"):
    sys.exit(3)
for stage in ("inspect", "prepare_import", "analyze", "codegen"):
    stages[stage]["status"] = "PASS"
if name.startswith("fail"):
    stages["compile"]["status"] = "FAIL"
    report.update(reached_stage="compile", failure_class="COMPILE_FAILED")
else:
    for stage in ("compile", "build_package", "launch"):
        stages[stage]["status"] = "PASS"
    report.update(reached_stage="launch", failure_class="NONE",
                  unsupported_imports=[{"library": "sceUsb", "nid_name": "sceUsbDeactivate"}],
                  exit_classification="EXITED_NONZERO", runtime_output_kind="UNIMPLEMENTED_IMPORT",
                  process_exit_code=7, presentation={"status": "NO_FRAME_SUBMISSIONS",
                                                     "frame_submissions": 0})
    package = data / "packages" / name.split("-")[1].upper()
    package.mkdir(parents=True, exist_ok=True)
    (package / "package.json").write_bytes(b"x" * 2048)
(work / "bringup.json").write_text(json.dumps(report), encoding="utf-8")
'''


class LibraryBuildTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="library-build-test-")
        self.root = Path(self._tmp.name)
        self.discs = self.root / "discs"
        self.data = self.root / "data"
        self.work = self.root / "work"
        self.reports = self.root / "reports"
        self.discs.mkdir()
        self.stub = self.root / "stub_bringup.py"
        self.stub.write_text(STUB_BRINGUP, encoding="utf-8")
        self.logs: list[str] = []

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def disc(self, relative: str, disc_id: str) -> Path:
        """A disc image named for the stub's behaviour, with its disc ID in PARAM.SFO."""
        path = self.discs / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        create_test_iso(path, disc_id=disc_id, title=f"Title {disc_id}")
        return path

    def command(self, iso: Path, data: Path, work: Path, _launch: int) -> list[str]:
        return [sys.executable, str(self.stub), str(iso), str(data), str(work)]

    def build(self, **overrides) -> library_build.LibraryBuild:
        options = dict(jobs=2, priority="normal", title_timeout_seconds=30,
                       launch_timeout_seconds=5, command=self.command, log=self.logs.append,
                       pause_poll_seconds=0.05)
        options.update(overrides)
        return library_build.LibraryBuild(self.discs, self.data, self.work, [self.reports],
                                          **options)

    def manifest(self, folder: Path | None = None) -> dict:
        return json.loads(((folder or self.reports) / "library-build.json").read_text(
            encoding="utf-8"))

    def by_source(self, manifest: dict) -> dict[str, dict]:
        return {record["source"]: record for record in manifest["titles"]}

    def calls(self) -> list[str]:
        log = self.work / "calls.log"
        return log.read_text(encoding="utf-8").split() if log.is_file() else []

    def test_every_disc_gets_a_record_and_one_failure_never_stops_the_batch(self) -> None:
        self.disc("ok-test70001.iso", "TEST70001")
        self.disc("A folder/fail-test70002.iso", "TEST70002")
        self.disc("crash-test70003.iso", "TEST70003")
        self.disc("ok-test70004.iso", "TEST70004")
        (self.discs / "ok-test70001.png").write_bytes(b"art")
        (self.discs / "A folder" / "DATA.EDAT").write_bytes(b"licence")
        (self.discs / "notes").write_bytes(b"")
        result = self.build().run()

        self.assertEqual(result["counts"], {"BUILD_FAILED": 1, "BUILT": 2, "NO_REPORT": 1})
        self.assertEqual(result["skipped_files"], {"(none)": 1, ".edat": 1, ".png": 1})
        records = self.by_source(result)
        built = records["ok-test70001.iso"]
        self.assertEqual((built["disc_id"], built["title"]), ("TEST70001", "Title TEST70001"))
        self.assertEqual(built["package_bytes"], 2048)
        self.assertEqual(built["unsupported_imports"], ["sceUsb:sceUsbDeactivate"])
        self.assertEqual(built["decrypt"], {"status": "PASS", "modules": 2, "encrypted_modules": 1})
        self.assertEqual(built["launch"]["exit_classification"], "EXITED_NONZERO")
        self.assertEqual(built["launch"]["runtime_output_kind"], "UNIMPLEMENTED_IMPORT")
        failed = records["A folder/fail-test70002.iso"]
        self.assertEqual((failed["outcome"], failed["failed_stage"], failed["failure_class"]),
                         ("BUILD_FAILED", "compile", "COMPILE_FAILED"))
        self.assertIsNone(failed["package_bytes"])
        self.assertEqual(records["crash-test70003.iso"]["exit_code"], 3)
        # The same record in the data folder, and a readable summary beside it.
        self.assertEqual(self.manifest(self.data)["counts"], result["counts"])
        markdown = (self.reports / "library-build.md").read_text(encoding="utf-8")
        self.assertIn("| BUILT | 2 |", markdown)
        self.assertIn("| compile | COMPILE_FAILED | 1 |", markdown)
        self.assertIn("`sceUsb:sceUsbDeactivate`", markdown)

    def test_a_disc_id_seen_before_is_a_duplicate_and_builds_once(self) -> None:
        self.disc("a/ok-test70010.iso", "TEST70010")
        self.disc("b/ok-test70010.iso", "TEST70010")
        result = self.build().run()
        records = self.by_source(result)
        self.assertEqual(records["a/ok-test70010.iso"]["outcome"], "BUILT")
        self.assertEqual(records["b/ok-test70010.iso"]["outcome"], "DUPLICATE")
        self.assertEqual(records["b/ok-test70010.iso"]["duplicate_of"], "a/ok-test70010.iso")
        self.assertEqual(self.calls(), ["ok-test70010"])

    def test_a_stopped_run_resumes_and_redoes_only_what_did_not_end(self) -> None:
        self.disc("ok-test70020.iso", "TEST70020")
        self.disc("fail-test70021.iso", "TEST70021")
        self.disc("ok-test70022.iso", "TEST70022")
        self.build(jobs=1).run()
        self.assertEqual(len(self.calls()), 3)

        # A run cut off mid-title left it RUNNING; a resume redoes that one only.
        manifest = self.manifest()
        for record in manifest["titles"]:
            if record["source"] == "ok-test70022.iso":
                record["outcome"] = "RUNNING"
        (self.reports / "library-build.json").write_text(json.dumps(manifest), encoding="utf-8")
        (self.data / "library-build.json").unlink()
        self.build(jobs=1).run()
        self.assertEqual(self.calls()[3:], ["ok-test70022"])

        # A changed disc builds again; --retry-failed builds the failure again too.
        disc = self.discs / "ok-test70020.iso"
        disc.write_bytes(disc.read_bytes() + b"\0" * 2048)
        self.build(jobs=1, retry_failed=True).run()
        self.assertEqual(sorted(self.calls()[4:]), ["fail-test70021", "ok-test70020"])
        self.assertEqual(self.manifest()["counts"], {"BUILD_FAILED": 1, "BUILT": 2})

    def test_a_title_over_its_time_budget_is_stopped_and_recorded(self) -> None:
        self.disc("slow-test70030.iso", "TEST70030")
        self.disc("ok-test70031.iso", "TEST70031")
        started = time.monotonic()
        result = self.build(title_timeout_seconds=3).run()
        self.assertLess(time.monotonic() - started, 50)
        records = self.by_source(result)
        slow = records["slow-test70030.iso"]
        self.assertEqual((slow["outcome"], slow["failed_stage"], slow["failure_class"]),
                         ("TIMED_OUT", "compile", "TITLE_TIME_BUDGET"))
        self.assertEqual(records["ok-test70031.iso"]["outcome"], "BUILT")

    def test_stop_ends_the_run_and_pause_holds_new_titles(self) -> None:
        for index in range(3):
            self.disc(f"ok-test7004{index}.iso", f"TEST7004{index}")
        self.work.mkdir()
        (self.work / "STOP").write_text("", encoding="utf-8")
        result = self.build(jobs=1).run()
        self.assertEqual(self.calls(), [])
        self.assertEqual(result["counts"], {"PENDING": 3})

        (self.work / "STOP").unlink()
        (self.work / "PAUSE").write_text("", encoding="utf-8")
        build = self.build(jobs=1)
        finished = []
        import threading
        runner = threading.Thread(target=lambda: finished.append(build.run()))
        runner.start()
        time.sleep(0.5)
        self.assertEqual(self.calls(), [], "nothing starts while PAUSE is there")
        (self.work / "PAUSE").unlink()
        runner.join(timeout=60)
        self.assertEqual(finished[0]["counts"], {"BUILT": 3})

    def test_the_disc_check_decrypts_before_the_bringup_and_never_stops_it(self) -> None:
        self.disc("ok-test70050.iso", "TEST70050")
        self.disc("ok-test70051.iso", "TEST70051")
        order: list[str] = []

        def decrypt(iso: Path, data: Path, disc_id: str) -> list[dict]:
            order.append(disc_id)
            self.assertEqual(data, self.data.resolve())
            if disc_id == "TEST70051":
                raise RuntimeError("synthetic engine failure")
            return [{"member": "PSP_GAME/SYSDIR/EBOOT.BIN", "name": "EBOOT.elf", "status": 0,
                     "message": ""},
                    {"member": "PSP_GAME/USRDIR/A.PRX", "name": "A.PRX", "status": 1,
                     "message": "refused"}]

        result = self.build(jobs=1, decrypt=decrypt).run()
        self.assertEqual(order, ["TEST70050", "TEST70051"])
        self.assertEqual(self.calls(), ["ok-test70050", "ok-test70051"],
                         "every bring-up runs after its disc check, even a failed one")
        records = self.by_source(result)
        self.assertEqual(records["ok-test70050.iso"]["decrypt_step"],
                         {"files": 2, "failed": ["A.PRX"], "first_failure": "refused"})
        self.assertIn("synthetic engine failure",
                      records["ok-test70051.iso"]["decrypt_step"]["first_failure"])
        self.assertEqual(result["counts"], {"BUILT": 2})

    def test_decrypt_disc_writes_the_executable_where_the_player_reads_it(self) -> None:
        encrypted = self.discs / "encrypted.iso"
        create_test_iso_with_executables(encrypted, b"~PSP" + b"\0" * 60, disc_id="TEST70060")
        plain = self.discs / "plain.iso"
        create_test_iso_with_executables(plain, b"\x7fELF" + b"\0" * 60, disc_id="TEST70061")
        seen: list[bytes] = []

        def runner(source: Path, destination: Path) -> tuple[int, str]:
            seen.append(source.read_bytes()[:4])
            destination.write_bytes(b"decrypted")
            return 0, "ok"

        rows = decrypt_tool.decrypt_disc(encrypted, self.data, "test70060", runner=runner)
        target = self.data / "titles" / "TEST70060" / "decrypted" / "EBOOT.elf"
        self.assertEqual([(row["member"], row["name"], row["status"]) for row in rows],
                         [("PSP_GAME/SYSDIR/EBOOT.BIN", "EBOOT.elf", 0)])
        self.assertEqual(seen, [b"~PSP"])
        self.assertEqual(target.read_bytes(), b"decrypted")
        again = decrypt_tool.decrypt_disc(encrypted, self.data, "TEST70060", runner=runner)
        self.assertEqual(len(seen), 1, "a decrypted copy already there is kept")
        self.assertEqual(again[0]["message"], "kept the decrypted copy already there")
        self.assertEqual(decrypt_tool.decrypt_disc(plain, self.data, "TEST70061", runner=runner),
                         [], "a plain executable needs nothing")

        # A module set the intake refuses keeps its boundary; the executable is still decrypted.
        from nk_core import iso_inspect
        refused = iso_inspect.IsoInspectionError("DUPLICATE_DISC_MODULE_BASENAME: synthetic")
        target.unlink()
        with mock.patch.object(iso_inspect, "list_disc_module_candidates", side_effect=refused):
            rows = decrypt_tool.decrypt_disc(encrypted, self.data, "TEST70060", runner=runner)
        self.assertEqual([(row["name"], row["status"]) for row in rows],
                         [("(guest modules)", -1), ("EBOOT.elf", 0)])
        self.assertIn("DUPLICATE_DISC_MODULE_BASENAME", rows[0]["message"])
        self.assertTrue(target.is_file())

    def test_the_manifest_names_where_each_first_launch_stopped(self) -> None:
        stages = {stage: {"status": "PASS", "duration_ms": 1} for stage in library_build.STAGES}
        stages["launch"]["status"] = "FAIL"
        record = library_build.summarize_report({
            "reached_stage": "launch", "stages": stages, "failure_class": "RUNTIME_SEMANTIC_BOUNDARY",
            "exit_classification": "EXITED_NONZERO", "runtime_output_kind": "SEMANTIC_BOUNDARY",
            "runtime_boundary": "target-not-executable", "process_exit_code": 1,
            "presentation": {"status": "NO_FRAME_SUBMISSIONS", "frame_submissions": 0},
        })
        self.assertEqual(record["outcome"], "BUILT")
        self.assertEqual(record["launch"]["runtime_boundary"], "target-not-executable")
        record.update(source="a/a.iso", disc_id="TEST70001", title="Synthetic")
        markdown = library_build.render_markdown({
            "titles": [record], "counts": {"BUILT": 1}, "started_at": "t0", "updated_at": "t1",
            "user_data_root": "data",
        })
        self.assertIn("## Where first launches stopped", markdown)
        self.assertIn("| RUNTIME_SEMANTIC_BOUNDARY | `target-not-executable` | 1 |", markdown)

    def test_the_default_job_count_follows_the_cpus(self) -> None:
        self.assertEqual(library_build.default_jobs(None), 1)
        self.assertEqual(library_build.default_jobs(8), 1)
        self.assertEqual(library_build.default_jobs(24), 3)
        self.assertEqual(library_build.default_jobs(128), 4)

    def test_the_real_bringup_command_builds_into_the_shared_folder(self) -> None:
        command = library_build.bringup_command(
            Path("disc.iso"), self.data, self.work / "TEST70001", 30)
        self.assertEqual(command[1:4], [str(library_build.NK_CLI), "bringup", "disc.iso"])
        self.assertEqual(command[command.index("--user-data-root") + 1], str(self.data))
        self.assertEqual(command[command.index("--launch-timeout") + 1], "30")


if __name__ == "__main__":
    unittest.main()
