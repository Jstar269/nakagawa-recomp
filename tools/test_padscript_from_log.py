# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors

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

    def test_adjacent_identical_states_are_coalesced(self):
        transitions = [
            padscript.Transition(10, 0, 0x8),
            padscript.Transition(20, 0x8, 0x8),
            padscript.Transition(30, 0x8, 0),
        ]
        self.assertEqual(
            padscript.pulses_from_transitions(transitions),
            [padscript.Pulse(10, 0x8, 20)],
        )

    def test_duplicate_vcount_transitions_do_not_emit_zero_width_spans(self):
        # The zero-width 0x8 press is expanded to the minimum width, not dropped.
        transitions = [
            padscript.Transition(10, 0, 0x8),
            padscript.Transition(10, 0x8, 0x10),
            padscript.Transition(20, 0x10, 0),
        ]
        self.assertEqual(
            padscript.pulses_from_transitions(transitions),
            [padscript.Pulse(10, 0x8, 8), padscript.Pulse(10, 0x10, 10)],
        )

    def test_same_vcount_press_and_release_is_expanded_not_dropped(self):
        transitions = [
            padscript.Transition(5, 0, 0x8),
            padscript.Transition(5, 0x8, 0),
        ]
        self.assertEqual(
            padscript.pulses_from_transitions(transitions),
            [padscript.Pulse(5, 0x8, 8)],
        )

    def test_stick_only_move_becomes_a_row_with_stick_columns(self):
        transitions = padscript.parse_transitions(
            [
                "ctrl_latch: vcount=10 buttons 0x0000 -> 0x0000 lx=255 ly=128\n",
                "ctrl_latch: vcount=30 buttons 0x0000 -> 0x0000 lx=128 ly=128\n",
            ]
        )
        pulses = padscript.pulses_from_transitions(transitions)
        self.assertEqual(pulses, [padscript.Pulse(10, 0, 20, 255, 128)])
        self.assertEqual(padscript.render_padscript(pulses), "10 0x0000 20 255 128\n")

    def test_centred_stick_keeps_the_three_column_rows(self):
        transitions = padscript.parse_transitions(
            [
                "ctrl_latch: vcount=10 buttons 0x0000 -> 0x4000 lx=128 ly=128\n",
                "ctrl_latch: vcount=30 buttons 0x4000 -> 0x0000 lx=128 ly=128\n",
            ]
        )
        self.assertEqual(
            padscript.render_padscript(padscript.pulses_from_transitions(transitions)),
            "10 0x4000 20\n",
        )

    def test_stick_move_during_a_press_splits_it_without_lengthening_it(self):
        transitions = [
            padscript.Transition(10, 0, 0x4000),
            padscript.Transition(15, 0x4000, 0x4000, 0, 128),
            padscript.Transition(17, 0x4000, 0),
        ]
        pulses = padscript.pulses_from_transitions(transitions, minimum_width=8)
        self.assertEqual(
            pulses,
            [padscript.Pulse(10, 0x4000, 5), padscript.Pulse(15, 0x4000, 3, 0, 128)],
        )
        # The press as a whole lasts the minimum width, as an unsplit press would.
        self.assertEqual(max(p.frame + p.width for p in pulses), 18)

    def test_stick_sweep_keeps_the_positions_the_stick_stayed_at(self):
        lines = [
            f"ctrl_latch: vcount={10 + i} buttons 0x0000 -> 0x0000 lx={130 + i} ly=128\n"
            for i in range(6)
        ]
        lines.append("ctrl_latch: vcount=40 buttons 0x0000 -> 0x0000 lx=128 ly=128\n")
        pulses = padscript.pulses_from_transitions(padscript.parse_transitions(lines))
        self.assertEqual(
            pulses,
            [padscript.Pulse(10, 0, 4, 133, 128), padscript.Pulse(14, 0, 26, 135, 128)],
        )
        exact = padscript.pulses_from_transitions(
            padscript.parse_transitions(lines), stick_min_width=1
        )
        self.assertEqual(len(exact), 6)

    def test_stick_jitter_back_to_the_same_position_is_dropped(self):
        transitions = [
            padscript.Transition(10, 0, 0, 200, 128),
            padscript.Transition(20, 0, 0, 201, 128),
            padscript.Transition(21, 0, 0, 200, 128),
            padscript.Transition(40, 0, 0),
        ]
        self.assertEqual(
            padscript.pulses_from_transitions(transitions),
            [padscript.Pulse(10, 0, 30, 200, 128)],
        )

    def test_out_of_range_stick_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "stick position out of range"):
            padscript.parse_transitions(
                ["ctrl_latch: vcount=10 buttons 0x0000 -> 0x0000 lx=256 ly=128\n"]
            )

    def test_out_of_range_mask_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "out of range"):
            padscript.parse_transitions(
                ["ctrl_latch: vcount=10 buttons 0x0000 -> 0x100000000\n"]
            )


if __name__ == "__main__":
    unittest.main()
