# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import padscript_from_log as padscript


class TestPadscriptFromLog(unittest.TestCase):
    def test_short_press_is_expanded(self):
        transitions = padscript.parse_transitions(
            [
                "ctrl_latch: vcount=3191 buttons 0x0000 -> 0x4000 lx=128 ly=128\n",
                "ctrl_latch: vcount=3192 buttons 0x4000 -> 0x0000 lx=128 ly=128\n",
            ]
        )
        self.assertEqual(
            padscript.pulses_from_transitions(transitions),
            [padscript.Pulse(3191, 0x4000, 8)],
        )

    def test_long_press_keeps_recorded_width(self):
        transitions = [
            padscript.Transition(10, 0, 0x8),
            padscript.Transition(30, 0x8, 0),
        ]
        self.assertEqual(
            padscript.pulses_from_transitions(transitions, minimum_width=4),
            [padscript.Pulse(10, 0x8, 20)],
        )

    def test_mask_change_splits_spans(self):
        transitions = [
            padscript.Transition(10, 0, 0x10),
            padscript.Transition(12, 0x10, 0x50),
            padscript.Transition(15, 0x50, 0),
        ]
        self.assertEqual(
            padscript.pulses_from_transitions(transitions, minimum_width=1),
            [
                padscript.Pulse(10, 0x10, 2),
                padscript.Pulse(12, 0x50, 3),
            ],
        )

    def test_backwards_vcount_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "moved backwards"):
            padscript.parse_transitions(
                [
                    "ctrl_latch: vcount=20 buttons 0x0000 -> 0x0008\n",
                    "ctrl_latch: vcount=10 buttons 0x0008 -> 0x0000\n",
                ]
            )

    def test_render_is_accepted_comment_free_format(self):
        self.assertEqual(
            padscript.render_padscript([padscript.Pulse(42, 0x4000, 12)]),
            "42 0x4000 12\n",
        )


if __name__ == "__main__":
    unittest.main()
