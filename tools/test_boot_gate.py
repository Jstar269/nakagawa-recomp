# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import boot_gate


ORDERED = (
    "image_loaded",
    "runtime_registered",
    "window_ready",
    "guest_start",
    "display_flip",
)


def event(phase: str, **fields: object) -> str:
    suffix = " ".join(f"{key}={value}" for key, value in fields.items())
    return f"BOOT_EVENT phase={phase}{(' ' + suffix) if suffix else ''}\n"


class BootGateTests(unittest.TestCase):
    def parse(self, lines: list[str], *, allow_present_only: bool = False) -> dict:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "run.log"
            path.write_text("".join(lines), encoding="utf-8")
            return boot_gate.parse_log(str(path), allow_present_only=allow_present_only)

    def ordered_prefix(self) -> list[str]:
        return [event(phase) for phase in ORDERED]

    def test_cpu_nonblank_ordered_fault_free_run_passes(self) -> None:
        result = self.parse(self.ordered_prefix() + [event("first_frame", source="cpu", nonzero_pixels="0x20")])
        self.assertTrue(result["ok"])
        self.assertTrue(result["sequenceOk"])
        self.assertEqual(result["frameEvidence"], "content-validated")
        self.assertEqual(result["faultCount"], 0)
        self.assertEqual(result["disqualifyingReasons"], [])

    def test_gpu_nonblank_measurement_is_content_validated(self) -> None:
        result = self.parse(self.ordered_prefix() + [event("first_frame", source="gpu", nonzero_pixels=5)])
        self.assertTrue(result["ok"])
        self.assertEqual(result["frameEvidence"], "content-validated")

    def test_out_of_order_phases_fail_even_when_all_are_present(self) -> None:
        phases = ["image_loaded", "runtime_registered", "guest_start", "window_ready", "display_flip"]
        result = self.parse([event(phase) for phase in phases] + [event("first_frame", nonzero_pixels=1)])
        self.assertFalse(result["ok"])
        self.assertTrue(all(result["reached"].values()))
        self.assertFalse(result["sequenceOk"])
        self.assertIn("milestones-incomplete-or-out-of-order", result["disqualifyingReasons"])

    def test_runtime_fault_is_disqualifying(self) -> None:
        result = self.parse(
            self.ordered_prefix()
            + ["ERROR: dispatch failure\n", event("first_frame", source="cpu", nonzero_pixels=1)]
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["faultCount"], 1)
        self.assertIn("disqualifying-fault", result["disqualifyingReasons"])

    def test_stalled_phase_is_disqualifying(self) -> None:
        result = self.parse(
            self.ordered_prefix()
            + [event("stalled"), event("first_frame", source="cpu", nonzero_pixels=1)]
        )
        self.assertFalse(result["ok"])
        self.assertTrue(result["stalled"])
        self.assertIn("stalled", result["disqualifyingReasons"])

    def test_gpu_present_only_is_liveness_not_visual_success(self) -> None:
        lines = self.ordered_prefix() + [event("first_frame", source="gpu")]
        strict = self.parse(lines)
        self.assertFalse(strict["ok"])
        self.assertEqual(strict["frameEvidence"], "present-submitted")
        self.assertEqual(strict["requiredFrameEvidence"], "content-validated")

        liveness = self.parse(lines, allow_present_only=True)
        self.assertTrue(liveness["ok"])
        self.assertEqual(liveness["frameEvidence"], "present-submitted")
        self.assertEqual(liveness["requiredFrameEvidence"], "present-submitted")

    def test_invalid_nonzero_count_fails_without_crashing(self) -> None:
        result = self.parse(self.ordered_prefix() + [event("first_frame", source="cpu", nonzero_pixels="bogus")])
        self.assertFalse(result["ok"])
        self.assertEqual(result["nonzeroPixels"], 0)
        self.assertEqual(result["faultCount"], 1)
        self.assertIn("invalid nonzero_pixels", result["faults"][0])

    def test_malformed_host_path_is_disqualifying(self) -> None:
        result = self.parse(
            self.ordered_prefix()
            + ["Open(host0:)\n", event("first_frame", source="cpu", nonzero_pixels=1)]
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["malformedHostPaths"], 1)
        self.assertIn("malformed-host-path", result["disqualifyingReasons"])

    def test_missing_frame_fails_with_stable_fields(self) -> None:
        result = self.parse(self.ordered_prefix())
        self.assertFalse(result["ok"])
        self.assertIsNone(result["frameSource"])
        self.assertEqual(result["frameEvidence"], "none")
        self.assertEqual(result["lastPhase"], "display_flip")

    def test_allow_present_only_requires_real_boolean_in_library_api(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "run.log"
            path.write_text("", encoding="utf-8")
            with self.assertRaisesRegex(TypeError, "must be a boolean"):
                boot_gate.parse_log(str(path), allow_present_only=1)  # type: ignore[arg-type]

    def test_main_exit_codes_and_json_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            missing = root / "missing.log"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(boot_gate.main([str(missing), "--json"]), 2)

            valid = root / "valid.log"
            valid.write_text(
                "".join(self.ordered_prefix() + [event("first_frame", source="cpu", nonzero_pixels=1)]),
                encoding="utf-8",
            )
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(boot_gate.main([str(valid), "--json"]), 0)
            self.assertIn('"frameEvidence": "content-validated"', output.getvalue())


if __name__ == "__main__":
    unittest.main()
