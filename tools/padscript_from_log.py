# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Convert ``SR_INLOG`` controller transitions into an ``SR_PADSCRIPT``.

The desktop automation layer emits very short keyboard pulses.  A one-vblank
press can legitimately fall between the game's controller reads, so the
converter expands short spans to a configurable minimum while preserving the
recorded start frame and button mask.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
from typing import Iterable


LATCH_RE = re.compile(
    r"ctrl_latch:\s+vcount=(\d+)\s+buttons\s+"
    r"0x([0-9a-fA-F]+)\s+->\s+0x([0-9a-fA-F]+)"
)


@dataclass(frozen=True)
class Transition:
    frame: int
    old_mask: int
    new_mask: int


@dataclass(frozen=True)
class Pulse:
    frame: int
    mask: int
    width: int


def parse_transitions(lines: Iterable[str]) -> list[Transition]:
    """Return controller transitions in log order.

    A backwards vcount normally means that logs from separate runs were
    concatenated.  Refuse that input so the generated script cannot silently
    combine unrelated routes.
    """

    transitions: list[Transition] = []
    previous_frame = -1
    for line_number, line in enumerate(lines, 1):
        match = LATCH_RE.search(line)
        if match is None:
            continue
        transition = Transition(
            frame=int(match.group(1)),
            old_mask=int(match.group(2), 16),
            new_mask=int(match.group(3), 16),
        )
        if transition.frame < previous_frame:
            raise ValueError(
                f"vcount moved backwards at input line {line_number}; "
                "provide a log from one runtime run"
            )
        transitions.append(transition)
        previous_frame = transition.frame
    return transitions


def pulses_from_transitions(
    transitions: Iterable[Transition], minimum_width: int = 8
) -> list[Pulse]:
    """Convert mask spans into replay pulses."""

    if minimum_width < 1:
        raise ValueError("minimum_width must be at least 1")

    pulses: list[Pulse] = []
    active_frame: int | None = None
    active_mask = 0

    for transition in transitions:
        if active_mask:
            recorded_width = max(1, transition.frame - (active_frame or 0))
            pulses.append(
                Pulse(
                    frame=active_frame or 0,
                    mask=active_mask,
                    width=max(recorded_width, minimum_width),
                )
            )
        active_frame = transition.frame if transition.new_mask else None
        active_mask = transition.new_mask

    if active_mask:
        pulses.append(
            Pulse(
                frame=active_frame or 0,
                mask=active_mask,
                width=minimum_width,
            )
        )
    return pulses


def render_padscript(pulses: Iterable[Pulse]) -> str:
    """Render comment-free rows accepted by the runtime's strict parser."""

    rows = [
        f"{pulse.frame} 0x{pulse.mask:04x} {pulse.width}"
        for pulse in pulses
    ]
    return "\n".join(rows) + ("\n" if rows else "")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert SR_INLOG ctrl_latch transitions to SR_PADSCRIPT rows."
    )
    parser.add_argument("input", type=Path, help="runtime stderr log")
    parser.add_argument(
        "-o", "--output", type=Path, help="write the pad script instead of stdout"
    )
    parser.add_argument(
        "--minimum-width",
        type=int,
        default=8,
        help="expand shorter presses to this many vblanks (default: 8)",
    )
    args = parser.parse_args()

    try:
        with args.input.open("r", encoding="utf-8", errors="replace") as stream:
            transitions = parse_transitions(stream)
        output = render_padscript(
            pulses_from_transitions(transitions, args.minimum_width)
        )
    except (OSError, ValueError) as exc:
        parser.error(str(exc))

    if args.output is None:
        print(output, end="")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
