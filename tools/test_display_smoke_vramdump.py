# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Offline tests for the display-smoke VRAM-dump leg's capture check (#2185).

The leg asks the capture run for eight vblanks starting where the baseline run first presented.
The capture run's own first present can land a vblank later, so the check must accept a capture at
any requested vblank and still fail closed. The runtime's log lines are synthetic here; no build is
needed.
"""

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
_SPEC = importlib.util.spec_from_file_location(
    "display_smoke_generate", ROOT / "fixtures" / "display_smoke" / "generate.py")
assert _SPEC is not None and _SPEC.loader is not None
generate = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(generate)

SELECTION = list(range(2, 10))


def run_output(first_present: int, dumped: list[int], extra: str = "") -> str:
    """Stderr shaped like a capture run's: present traces, then one VRAMDUMP line per vblank."""
    lines = [f"HOST_PRESENT_SUBMITTED f={first_present} slot=0"]
    for vblank in SELECTION:
        if vblank in dumped:
            lines.append(f"VRAMDUMP vblank={vblank} PASS")
        elif vblank < first_present:
            lines.append(f"VRAMDUMP vblank={vblank} NOT_CAPTURED "
                         "(no frame was presented at that vblank)")
    return "\n".join(lines) + "\n" + extra


class TestSelectedVramCapture(unittest.TestCase):
    def check(self, output: str) -> int:
        vblank: int = generate._require_selected_vram_capture(SELECTION, output)
        return vblank

    def test_a_capture_at_the_baseline_first_present_passes(self):
        self.assertEqual(self.check(run_output(2, [2, 3, 4])), 2)

    def test_a_first_present_one_vblank_late_passes(self):
        # #2185: the baseline first presented at vblank 2, the capture run at vblank 3.
        self.assertEqual(self.check(run_output(3, [3, 4, 5])), 3)

    def test_a_first_present_at_the_last_selected_vblank_passes(self):
        self.assertEqual(self.check(run_output(9, [9])), 9)

    def test_no_capture_in_the_selection_fails(self):
        with self.assertRaisesRegex(RuntimeError, r"2\.\.9 produced no VRAM capture .*vblank 10"):
            self.check(run_output(10, []))

    def test_a_run_that_never_presented_fails(self):
        with self.assertRaisesRegex(RuntimeError, "first presented at vblank none"):
            self.check("VRAMDUMP vblank=2 NOT_CAPTURED (no frame was presented at that vblank)\n")

    def test_a_capture_failure_fails_even_beside_a_pass(self):
        output = run_output(2, [3], "VRAMDUMP vblank=2 FAIL (cannot open raw image)\n")
        with self.assertRaisesRegex(RuntimeError, "failed at vblank 2"):
            self.check(output)

    def test_a_capture_that_was_not_requested_fails(self):
        output = run_output(2, [2], "VRAMDUMP vblank=11 PASS\n")
        with self.assertRaisesRegex(RuntimeError, "vblank 11, which was not requested"):
            self.check(output)


if __name__ == "__main__":
    unittest.main()
