# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Convert ``SR_INLOG`` controller transitions into an ``SR_PADSCRIPT``.

The desktop automation layer emits very short keyboard pulses.  A one-vblank
press can legitimately fall between the game's controller reads, so the
converter expands short spans to a configurable minimum while preserving the
recorded start frame and button mask.

The left stick is recorded too. ``ctrl_latch`` lines carry the stick the guest
was handed, and a row whose stick is off centre gets two more columns,
``<lx> <ly>``, which the runtime holds for that row's width. A stick sweeps
through many positions in a few vblanks; positions held for fewer than
``--stick-min-width`` vblanks are folded into the next one, so the replay holds
the positions the stick stayed at instead of one row per vblank.
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
    r"(?:\s+lx=(\d+)\s+ly=(\d+))?"
)
CENTRE = 128


@dataclass(frozen=True)
class Transition:
    frame: int
    old_mask: int
    new_mask: int
    lx: int = CENTRE
    ly: int = CENTRE


@dataclass(frozen=True)
class Pulse:
    frame: int
    mask: int
    width: int
    lx: int = CENTRE
    ly: int = CENTRE


@dataclass(frozen=True)
class _Span:
    frame: int
    mask: int
    lx: int
    ly: int


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
            lx=int(match.group(4)) if match.group(4) is not None else CENTRE,
            ly=int(match.group(5)) if match.group(5) is not None else CENTRE,
        )
        if transition.old_mask > 0xFFFFFFFF or transition.new_mask > 0xFFFFFFFF:
            raise ValueError(
                f"button mask out of range at input line {line_number}"
            )
        if transition.lx > 255 or transition.ly > 255:
            raise ValueError(f"stick position out of range at input line {line_number}")
        if transition.frame < previous_frame:
            raise ValueError(
                f"vcount moved backwards at input line {line_number}; "
                "provide a log from one runtime run"
            )
        transitions.append(transition)
        previous_frame = transition.frame
    return transitions


def _spans(transitions: Iterable[Transition], stick_min_width: int) -> list[_Span]:
    """The pad states in order, one per change, starting released and centred.

    A stick position held for fewer than ``stick_min_width`` vblanks while the
    buttons stay the same is folded into the next position: the next one starts
    where it started.
    """

    spans: list[_Span] = []
    current = _Span(-1, 0, CENTRE, CENTRE)
    for transition in transitions:
        span = _Span(transition.frame, transition.new_mask, transition.lx, transition.ly)
        if (span.mask, span.lx, span.ly) == (current.mask, current.lx, current.ly):
            continue
        spans.append(span)
        current = span

    folded: list[_Span] = []
    carried: int | None = None
    for index, span in enumerate(spans):
        if carried is not None:
            span = _Span(carried, span.mask, span.lx, span.ly)
            carried = None
        following = spans[index + 1] if index + 1 < len(spans) else None
        if (
            following is not None
            and following.mask == span.mask
            and following.frame - span.frame < stick_min_width
        ):
            carried = span.frame
            continue
        if folded and (folded[-1].mask, folded[-1].lx, folded[-1].ly) == (span.mask, span.lx, span.ly):
            continue
        folded.append(span)
    return folded


def pulses_from_transitions(
    transitions: Iterable[Transition], minimum_width: int = 8, stick_min_width: int = 4
) -> list[Pulse]:
    """Convert pad-state spans into replay pulses.

    A press shorter than ``minimum_width`` is expanded to it. A press the stick
    moved during is split at each move, and the expansion applies to the press
    as a whole, on its last split, so a split press is never held longer than
    an unsplit one.
    """

    if minimum_width < 1:
        raise ValueError("minimum_width must be at least 1")
    if stick_min_width < 1:
        raise ValueError("stick_min_width must be at least 1")

    spans = _spans(transitions, stick_min_width)
    pulses: list[Pulse] = []
    press_start = 0
    for index, span in enumerate(spans):
        if index == 0 or spans[index - 1].mask != span.mask:
            press_start = span.frame
        if span.mask == 0 and (span.lx, span.ly) == (CENTRE, CENTRE):
            continue
        following = spans[index + 1] if index + 1 < len(spans) else None
        if following is None:
            width = max(1, minimum_width - (span.frame - press_start)) if span.mask else minimum_width
        else:
            # A press that starts and ends at the same vcount is still a press: expand it.
            width = max(1, following.frame - span.frame)
            if span.mask and following.mask != span.mask:
                width = max(width, minimum_width - (span.frame - press_start))
        pulses.append(Pulse(span.frame, span.mask, width, span.lx, span.ly))
    return pulses


def render_padscript(pulses: Iterable[Pulse]) -> str:
    """Render comment-free rows accepted by the runtime's strict parser."""

    rows = [
        f"{pulse.frame} 0x{pulse.mask:04x} {pulse.width}"
        + ("" if (pulse.lx, pulse.ly) == (CENTRE, CENTRE) else f" {pulse.lx} {pulse.ly}")
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
    parser.add_argument(
        "--stick-min-width",
        type=int,
        default=4,
        help="fold stick positions held for fewer vblanks into the next one (default: 4)",
    )
    args = parser.parse_args()

    try:
        with args.input.open("r", encoding="utf-8", errors="replace") as stream:
            transitions = parse_transitions(stream)
        output = render_padscript(
            pulses_from_transitions(transitions, args.minimum_width, args.stick_min_width)
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
