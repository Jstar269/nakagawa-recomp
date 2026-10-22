# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for tools/regen_all.py, the one command that regenerates committed generated files."""

from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import regen_all  # noqa: E402

ROOT = regen_all.ROOT


class StepInventoryTests(unittest.TestCase):
    def test_outputs_are_tracked_files_owned_by_one_step(self) -> None:
        seen: dict[str, str] = {}
        for step in (*regen_all.STEPS, regen_all.PROVENANCE):
            for output in step.outputs:
                with self.subTest(output=output):
                    self.assertTrue((ROOT / output).is_file(), f"{step.name} names a missing output")
                    self.assertNotIn(output, seen, f"{output} belongs to {seen.get(output)} and {step.name}")
                    seen[output] = step.name

    def test_every_step_either_runs_or_says_why_not(self) -> None:
        for step in regen_all.STEPS:
            with self.subTest(step=step.name):
                self.assertNotEqual(step.render is None, not step.skip, "a step runs, or it is skipped with a reason")

    def test_staleness_messages_name_the_one_fix_command(self) -> None:
        # Each committed-output check prints regen_all as its fix, not its own generator.
        for rel in (
            "tools/recomp_helper_inventory.py",
            "tools/test_recomp_helper_inventory.py",
            "tools/waits_census.py",
            "tools/test_waits_census.py",
            "tools/gen_nidnames.py",
            "tools/test_gen_nidnames.py",
            "tools/test_hle_manifest.py",
            "tools/import_audit_gate.py",
            "tools/title_catalog_codegen.py",
            "tools/test_pgf_validate_parity.py",
            "tools/publish_audit.py",
        ):
            with self.subTest(path=rel):
                self.assertIn(regen_all.FIX_COMMAND, (ROOT / rel).read_text(encoding="utf-8"))


class RenderTests(unittest.TestCase):
    def test_each_public_generator_reproduces_its_committed_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for step in regen_all.STEPS:
                if step.skip:
                    continue
                with self.subTest(step=step.name):
                    scratch = Path(tmp) / step.name.replace(" ", "_")
                    scratch.mkdir()
                    rendered = step.render(scratch)
                    self.assertEqual(sorted(rendered), sorted(step.outputs))
                    for path, data in rendered.items():
                        self.assertEqual(data, (ROOT / path).read_bytes(), f"{path} is stale")

    def test_baseline_refresh_refuses_a_coverage_downgrade(self) -> None:
        committed = json.loads((ROOT / "tools" / "import_audit_baseline.json").read_text(encoding="ascii"))
        nid, entry = next((k, v) for k, v in committed.items() if v["classification"] == "dedicated")
        degraded = dict(committed)
        degraded[nid] = dict(entry, classification="fake_success")
        with tempfile.TemporaryDirectory() as tmp:
            fresh = Path(tmp) / "baseline.json"
            fresh.write_text(json.dumps(degraded), encoding="ascii")
            with self.assertRaises(regen_all.StepError) as ctx:
                regen_all._refuse_baseline_downgrades(fresh)
            self.assertIn(nid, str(ctx.exception))
            fresh.write_text(json.dumps(committed), encoding="ascii")
            regen_all._refuse_baseline_downgrades(fresh)


class AdmissionBlockTests(unittest.TestCase):
    def test_only_the_admission_block_is_ignored(self) -> None:
        ledger = {"schema_version": 1, "entries": [{"path": "a", "sha256": "0" * 64}]}
        with_admission = dict(ledger, admission={"workflow": "admit-new-reviewed"})
        self.assertEqual(
            regen_all._without_admission(json.dumps(with_admission).encode()),
            regen_all._without_admission(json.dumps(ledger).encode()),
        )
        rehashed = {"schema_version": 1, "entries": [{"path": "a", "sha256": "1" * 64}]}
        self.assertNotEqual(
            regen_all._without_admission(json.dumps(with_admission).encode()),
            regen_all._without_admission(json.dumps(rehashed).encode()),
        )


class CheckModeTests(unittest.TestCase):
    def _run_check(self, steps, provenance=("OK", [], "")) -> tuple[int, str]:
        out = io.StringIO()
        with mock.patch.object(regen_all, "STEPS", steps), \
                mock.patch.object(regen_all, "_provenance", return_value=provenance), \
                contextlib.redirect_stdout(out):
            rc = regen_all.run(check=True, base=None)
        return rc, out.getvalue()

    def test_a_stale_output_fails_check_with_the_fix_command(self) -> None:
        path = "docs/PSP_INTR_WAITS_CENSUS.md"
        stale = regen_all.Step("fake", "fake", (path,), lambda tmp: {path: b"not the committed bytes"})
        rc, out = self._run_check((stale,))
        self.assertEqual(rc, 1)
        self.assertIn(f"STALE fake: {path}", out)
        self.assertIn(f"Fix with: {regen_all.FIX_COMMAND}", out)

    def test_current_outputs_and_skips_pass_check_without_writing(self) -> None:
        path = "docs/PSP_INTR_WAITS_CENSUS.md"
        current = regen_all.Step("fake", "fake", (path,), lambda tmp: {path: (ROOT / path).read_bytes()})
        skipped = regen_all.Step("private", "private", (), skip="needs private inputs")
        rc, out = self._run_check((current, skipped))
        self.assertEqual(rc, 0, out)
        self.assertIn("SKIP  private: needs private inputs", out)

    def test_stale_provenance_controls_fail_check(self) -> None:
        rc, out = self._run_check((), provenance=("STALE", ["assets/public_provenance_ledger.json"], ""))
        self.assertEqual(rc, 1)
        self.assertIn(regen_all.FIX_COMMAND, out)

    def test_a_generator_failure_fails_check(self) -> None:
        def broken(tmp):
            raise regen_all.StepError("boom")

        rc, out = self._run_check((regen_all.Step("broken", "broken", (), broken),))
        self.assertEqual(rc, 1)
        self.assertIn("FAIL  broken: boom", out)


if __name__ == "__main__":
    unittest.main()
