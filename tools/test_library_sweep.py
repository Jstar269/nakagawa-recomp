# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regressions for the private, resumable library compatibility sweep."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import library_sweep  # noqa: E402
import nk_cli  # noqa: E402
from test_iso_parity import build_psp_container, create_test_iso_with_executables  # noqa: E402


SOURCE_COMMIT = "a" * 40


def _write_iso(root: Path, name: str, disc_id: str, title: str) -> Path:
    path = root / name
    create_test_iso_with_executables(
        path,
        build_psp_container(),
        disc_id=disc_id,
        title=title,
    )
    return path


def _bringup_report(*, failure: str = "UNSUPPORTED_IMPORT") -> dict:
    return {
        "failure_class": failure,
        "issue_numbers": [308] if failure != "NONE" else [],
        "stages": {
            "inspect": {"status": "PASS"},
            "prepare_import": {"status": "PASS"},
            "analyze": {"status": "PASS"},
            "codegen": {"status": "PASS"},
            "compile": {"status": "PASS"},
            "build_package": {"status": "PASS"},
            "launch": {"status": "FAIL" if failure != "NONE" else "PASS"},
        },
        "unsupported_imports": [{"library": "sceAudio", "nid_name": "sceAudioInit"}],
        "runtime_imports": [],
        "presentation": {"frame_submissions": 0},
    }


class LibrarySweepTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="library_sweep_")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.iso_dir = self.root / "isos"
        self.iso_dir.mkdir()
        self.private_dir = self.root / "private"
        self.public_output = self.root / "public" / "aggregate.json"

    def _run_kwargs(self) -> dict:
        return {
            "source_commit": SOURCE_COMMIT,
            "process_reader": lambda: set(),
            "sleeper": lambda _seconds: None,
            "poll_seconds": 1,
            "max_wait_seconds": 0,
        }

    def test_interrupted_run_resumes_only_unrecorded_titles(self) -> None:
        first = _write_iso(self.iso_dir, "one.iso", "UCUS99991", "Resume Sentinel One")
        _write_iso(self.iso_dir, "two.iso", "UCUS99992", "Resume Sentinel Two")
        first_calls: list[str] = []

        def interrupt_after_first(iso_path, _work, _report, _sidecar, _budget):
            first_calls.append(iso_path.name)
            if len(first_calls) == 2:
                raise KeyboardInterrupt
            return library_sweep.RouteOutcome(_bringup_report())

        with mock.patch.object(library_sweep, "_run_bringup", side_effect=interrupt_after_first):
            with self.assertRaises(KeyboardInterrupt):
                library_sweep.run_sweep(
                    self.iso_dir, self.private_dir, self.public_output, **self._run_kwargs()
                )

        saved = json.loads((self.private_dir / "library-sweep.json").read_text(encoding="utf-8"))
        self.assertEqual(len(saved["rows"]), 1)
        self.assertEqual(saved["rows"][0]["disc_id"], "UCUS99991")
        self.assertEqual(saved["rows"][0]["source_file"], first.name)

        resumed_calls: list[str] = []

        def complete(iso_path, _work, _report, _sidecar, _budget):
            resumed_calls.append(iso_path.name)
            return library_sweep.RouteOutcome(_bringup_report())

        with mock.patch.object(library_sweep, "_run_bringup", side_effect=complete):
            result = library_sweep.run_sweep(
                self.iso_dir, self.private_dir, self.public_output, **self._run_kwargs()
            )
        self.assertEqual(resumed_calls, ["two.iso"])
        self.assertEqual(result["resumed_this_invocation"], 1)
        self.assertEqual(result["ran_this_invocation"], 1)
        self.assertEqual(len(result["rows"]), 2)

    def test_route_enforces_the_per_title_time_budget(self) -> None:
        work_dir = self.private_dir / "work"
        work_dir.mkdir(parents=True)
        iso = _write_iso(self.iso_dir, "timeout.iso", "UCUS99994", "Timeout Sentinel")
        observed_timeouts: list[int] = []

        class TimedOutProcess:
            pid = 9001
            returncode = None

            def communicate(self, timeout=None):
                observed_timeouts.append(timeout)
                raise subprocess.TimeoutExpired("nk_cli bringup", timeout)

        process = TimedOutProcess()
        with mock.patch.object(library_sweep.subprocess, "Popen", return_value=process):
            with mock.patch.object(library_sweep, "_terminate_process_tree") as terminate:
                outcome = library_sweep._run_bringup(
                    iso,
                    work_dir,
                    work_dir / "bringup.json",
                    work_dir / "sweep-imports.json",
                    7,
                )
        self.assertTrue(outcome.timed_out)
        self.assertEqual(observed_timeouts, [7])
        terminate.assert_called_once_with(process)

    def test_public_aggregate_contains_only_aggregated_title_neutral_data(self) -> None:
        _write_iso(
            self.iso_dir,
            "PRIVATE_SOURCE_FILENAME.iso",
            "UCUS99993",
            "PRIVATE_TITLE_SENTINEL",
        )

        def run_with_private_nid(_iso, _work, _report, sidecar, _budget):
            sidecar.parent.mkdir(parents=True, exist_ok=True)
            sidecar.write_text(json.dumps({
                "schema_version": 1,
                "unsupported_imports": [{
                    "library": "sceAudio",
                    "nid": "0x12345678",
                    "nid_name": "sceAudioInit",
                }],
            }), encoding="utf-8")
            return library_sweep.RouteOutcome(_bringup_report())

        with mock.patch.object(library_sweep, "_run_bringup", side_effect=run_with_private_nid):
            library_sweep.run_sweep(
                self.iso_dir, self.private_dir, self.public_output, **self._run_kwargs()
            )

        public_text = self.public_output.read_text(encoding="utf-8")
        for private_value in ("PRIVATE_SOURCE_FILENAME", "PRIVATE_TITLE_SENTINEL", "UCUS99993", "0x12345678"):
            self.assertNotIn(private_value, public_text)
        aggregate = json.loads(public_text)
        self.assertEqual(aggregate["furthest_stage_histogram"]["launch"], 1)
        self.assertEqual(aggregate["input_responsiveness"], "NOT_MEASURED_BY_HEADLESS_BRINGUP")
        self.assertEqual(aggregate["nid_families_by_title_count"], [
            {"library_family": "sceAudio", "title_count": 1},
        ])
        private = json.loads((self.private_dir / "library-sweep.json").read_text(encoding="utf-8"))
        self.assertEqual(private["rows"][0]["disc_id"], "UCUS99993")
        self.assertEqual(private["rows"][0]["first_missing_nids"][0]["nid"], "0x12345678")

    def test_private_nid_sidecar_is_contained_and_whitelisted(self) -> None:
        work_dir = self.private_dir / "bringup"
        work_dir.mkdir(parents=True)
        sidecar = work_dir / "sweep-imports.json"
        imports = [{
            "library": "sceAudio",
            "nid": 0x12345678,
            "name": "sceAudioInit",
            "module": "synthetic.prx",
            "boundary": "private diagnostic detail",
        }, {
            "library": "sceAudio",
            "nid": "0x87654321",
            "name": "sceAudioTerm",
            "module": "synthetic.prx",
            "boundary": "private diagnostic detail",
        }]
        with self.assertRaises(ValueError):
            nk_cli._write_private_sweep_import_report(
                self.private_dir / "outside.json", work_dir, imports
            )
        nk_cli._write_private_sweep_import_report(sidecar, work_dir, imports)
        payload = json.loads(sidecar.read_text(encoding="utf-8"))
        self.assertEqual(payload, {
            "schema_version": 1,
            "unsupported_imports": [
                {
                    "library": "sceAudio",
                    "nid": "0x12345678",
                    "nid_name": "sceAudioInit",
                },
                {
                    "library": "sceAudio",
                    "nid": "0x87654321",
                    "nid_name": "sceAudioTerm",
                },
            ],
        })

    def test_empty_sidecar_recovers_nids_from_private_build_report(self) -> None:
        work_dir = self.private_dir / "work" / "synthetic"
        output_dir = work_dir / "package"
        output_dir.mkdir(parents=True)
        sidecar = work_dir / "sweep-imports.json"
        sidecar.write_text(json.dumps({
            "schema_version": 1,
            "unsupported_imports": [],
        }), encoding="utf-8")
        (output_dir / "build-report.json").write_text(json.dumps({
            "unsupported": {
                "imports": [{
                    "library": "sceAudio",
                    "nid": "0x12345678",
                    "name": "sceAudioInit",
                }],
            },
        }), encoding="utf-8")

        rows, status = library_sweep._read_private_nid_rows(
            sidecar,
            _bringup_report(),
        )

        self.assertEqual(status, "BUILD_REPORT")
        self.assertEqual(rows, [{
            "library": "sceAudio",
            "nid": "0x12345678",
            "nid_name": "sceAudioInit",
        }])

    def test_decrypted_inputs_are_staged_from_the_disc_id_folder(self) -> None:
        decrypted_root = self.root / "decrypted-titles"
        source_dir = decrypted_root / "ULUS99995" / "decrypted"
        source_dir.mkdir(parents=True)
        eboot = b"synthetic decrypted executable"
        module = b"synthetic decrypted module"
        (source_dir / "EBOOT.elf").write_bytes(eboot)
        (source_dir / "libsample.prx").write_bytes(module)
        (source_dir / "notes.txt").write_text("ignored", encoding="utf-8")

        copied, status = library_sweep._stage_decrypted_inputs(
            decrypted_root,
            "ULUS99995",
            self.private_dir / "work",
        )
        production_dir = (
            self.private_dir / "work" / "user-data" / "titles" / "ULUS99995" / "decrypted"
        )
        self.assertEqual(copied, 2)
        self.assertEqual(status, "DIRECT_FOLDER")
        self.assertEqual((production_dir / "EBOOT.elf").read_bytes(), eboot)
        self.assertEqual((production_dir / "libsample.prx").read_bytes(), module)
        self.assertFalse((production_dir / "notes.txt").exists())

    def test_unmatched_disc_id_stages_nothing(self) -> None:
        decrypted_root = self.root / "decrypted-titles"
        (decrypted_root / "ULUS99994" / "decrypted").mkdir(parents=True)

        copied, status = library_sweep._stage_decrypted_inputs(
            decrypted_root,
            "ULUS99995",
            self.private_dir / "work",
        )
        self.assertEqual((copied, status), (0, "NO_MATCH"))

if __name__ == "__main__":
    unittest.main()
