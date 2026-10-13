#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Read the runtime's ``SR_GESTAT`` statistics windows out of a run log.

With ``SR_GESTAT=1`` the GE (``ge_set_frame`` in ``src/rt/ge.c``) prints one
``GESTAT f=<vblank> ...`` line each time VCOUNT crosses a multiple of
``WINDOW_VBLANKS``, carrying the counters accumulated since the previous line, and
``SR_FBSNAP`` writes that moment's framebuffer as ``snap_f<vblank>.ppm``.

The label is the vblank that closed the window, which is not always the multiple
itself. VCOUNT advances by every elapsed display period at the scheduler's source
latch, so when the host falls behind the delivered ticks step over the multiple
(59 -> 61) and the window closes at 61. A gate that waits for the literal
``GESTAT f=60`` line therefore fails on a slow or loaded host even though the guest
reached the window; read the label instead. A label below the first boundary, or two
lines inside one window, break the runtime contract and are rejected.

VCOUNT is host time in the default paced mode, so how much a guest has drawn by a
given window depends on how fast the host ran it. A gate that wants the guest's
first rendered frame therefore asks for the first window whose counters show that
work (``first_window(output, require=("tri3d", "px3d"))``) rather than for the first
window, and bounds the wait by the run's own vblank budget.
"""

from __future__ import annotations

from dataclasses import dataclass
import re

WINDOW_VBLANKS = 60

_WINDOW_LINE = re.compile(r"GESTAT f=(\d+) ([^\n]*)")
_FIELD = re.compile(r"(\w+)=(\d+)")


class GeStatWindowError(ValueError):
    """A run log whose GESTAT lines violate the one-line-per-crossing contract."""


@dataclass(frozen=True)
class GeStatWindow:
    """One closed statistics window: the vblank that closed it and its counters."""

    frame: int
    counters: dict[str, int]

    @property
    def snapshot_name(self) -> str:
        """The ``SR_FBSNAP`` capture written when this window closed."""
        return f"snap_f{self.frame:05d}.ppm"


def parse_windows(output: str) -> list[GeStatWindow]:
    """Every window ``output`` closed, in order, each checked against the contract."""
    windows: list[GeStatWindow] = []
    for match in _WINDOW_LINE.finditer(output):
        frame = int(match.group(1))
        index = frame // WINDOW_VBLANKS
        if index == 0:
            raise GeStatWindowError(
                f"GE statistics window closed at vblank {frame}, before the first "
                f"{WINDOW_VBLANKS}-vblank boundary"
            )
        if windows and index <= windows[-1].frame // WINDOW_VBLANKS:
            raise GeStatWindowError(
                f"GE statistics window closed at vblank {frame} does not cross a new "
                f"{WINDOW_VBLANKS}-vblank boundary after vblank {windows[-1].frame}"
            )
        counters = {key: int(value) for key, value in _FIELD.findall(match.group(2))}
        windows.append(GeStatWindow(frame, counters))
    return windows


def first_window(output: str, require: tuple[str, ...] = ()) -> GeStatWindow | None:
    """The first window ``output`` closed whose ``require`` counters are all non-zero.

    With no ``require`` this is simply the first window. None means no window in the
    log qualifies; the whole log is still checked against the contract first.
    """
    for window in parse_windows(output):
        if all(window.counters.get(name) for name in require):
            return window
    return None
