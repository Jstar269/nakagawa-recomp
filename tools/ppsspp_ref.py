#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Scripted PPSSPP reference capture and a graded comparison (#1882).

The picture gate (``tools/picture_gate.py``, #1845) compares a title's frame with a reference
from Nakagawa's own earlier run, so it catches regressions only. This tool takes references
from a second implementation instead: it drives a local PPSSPP from a script, frame-exact in
emulated time, and grades Nakagawa's headless captures of the same frames against them.

PPSSPP is not a hardware oracle (AGENTS.md section 6). A PASS here reads "agrees with
PPSSPP", never "matches the PSP". The references are game pixels: they live in the title's
private folder, and this tool refuses an output folder inside the checkout (``build/``
excepted, it is ignored). PPSSPP is never bundled; its path comes from ``$PPSSPP_EXE`` or
``--ppsspp``.

Verdicts, one per scene:

  PASS <percent>%      the share of pixels over the threshold is within the scene's tolerance
  FAIL <percent>% ...  too many pixels differ, the sizes differ, or PPSSPP runs disagreed
  NO_REFERENCE         no PPSSPP reference for the scene yet (run ``capture``)
  NOT_RUN <reason>     nothing to compare: no PPSSPP, no disc, no Nakagawa capture, or a
                       PPSSPP build without the debugger requests the driver needs

NO_REFERENCE and NOT_RUN are never a pass: each exits 3. Any FAIL exits 1.

Usage:
  python tools/ppsspp_ref.py plan SCENES.json
  python tools/ppsspp_ref.py capture SCENES.json --allow-ppsspp-registration [--runs N] [--replace]
  python tools/ppsspp_ref.py compare SCENES.json [--diff-dir DIR]

A scene file is private JSON next to the title's other private inputs:

  {"title": "...", "disc": "game.iso", "pad": "route.pad", "out": "ppsspp-ref",
   "rtc": 1325376000, "timeout": 900, "ini": {"Graphics": {"SoftwareRenderer": "True"}},
   "scenes": [{"name": "title-menu", "frame": 1500, "nakagawa": "nk/frame_v1500.ppm",
               "threshold": 36, "max_percent": 0.5}]}

Relative paths resolve against the scene file's folder. ``pad`` is the legacy
``SR_PADSCRIPT`` table (``<frame> <mask> <width> [<lx> <ly>]``); a route program waits on guest
state that PPSSPP cannot report, so it is refused. ``rtc`` (Unix seconds) pins PPSSPP's clock;
the default is the date Nakagawa's deterministic clock uses (2012-01-01 00:00:00 UTC).

How a frame number maps to PPSSPP. The driver resets the game with the CPU stopped at boot,
then runs with ``cpu.runUntilTime`` from one event to the next, so every stop is at an exact
emulated microsecond, independent of host speed. Frame ``n`` is the display period that starts
at vblank ``n`` (59.94 Hz, 1001/60 ms per period, counted from boot): a capture is taken half
a period after vblank ``n``, and a scripted press for frame ``n`` is set half a period before
it, so the controller sample at vblank ``n`` is the first to see it. Nakagawa's scripted input
also waits for the guest to read each press (docs/DEBUGGING.md); PPSSPP does not, so a route
whose presses only land because of that wait can disagree, and that disagreement is a FAIL,
not a tolerance.

The driver needs a PPSSPP build whose WebSocket debugger has ``cpu.runUntilTime`` and reports
its process id in ``version`` (both landed after v1.20.4). With an older build it reports
NOT_RUN and captures nothing.

PPSSPP's debugger web server reports this machine's local IP address and port to
report.ppsspp.org while it runs (its remote-disc discovery; PPSSPP behaviour, not
Nakagawa's). ``capture`` refuses to start PPSSPP until ``--allow-ppsspp-registration`` says
the person running it accepts that. The driver itself connects only to 127.0.0.1.
"""

from __future__ import annotations

import argparse
import base64
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))

import picture_gate  # noqa: E402  (the shared metric and picture readers)

ROOT = Path(__file__).resolve().parents[1]
TOOL_NAME = "nakagawa-ppsspp-ref"
TOOL_VERSION = "1"

PASS, FAIL, NO_REFERENCE, NOT_RUN = "PASS", "FAIL", "NO_REFERENCE", "NOT_RUN"
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_NOT_PROVEN = 0, 1, 2, 3

# One PSP display period: 59.94 Hz, 1001/60 ms (the NTSC-derived rate the PSP LCD runs at).
PERIOD_US_NUM, PERIOD_US_DEN = 1001 * 1000, 60
# How far a stop may fall short of the time asked for. PPSSPP's reported time truncates to whole
# microseconds each time it rebases its clock (Core/CoreTiming.cpp GetGlobalTimeUs, and again on a
# CPU clock change), so a run can read a microsecond or so behind the deadline it was given. Every
# action sits half a period (8341 us) from a vblank, so this allowance never moves an input or a
# capture across one.
CLOCK_SLACK_US = 1000
# The latest a stop may land: past the half period it would read the next vblank's state.
LATE_LIMIT_US = PERIOD_US_NUM // (2 * PERIOD_US_DEN) - CLOCK_SLACK_US
# The date Nakagawa's deterministic clock starts at (src/rt/hle.c rtc_now_tick, #2037).
DEFAULT_RTC = 1325376000
DEFAULT_TIMEOUT_SECONDS = 900
REQUEST_TIMEOUT_SECONDS = 30.0
MAX_MESSAGE_BYTES = 64 << 20
MAX_PAD_ROWS = 4096  # the runtime's legacy-table limit (src/rt/hle_route.c ROUTE_MAX_LEGACY)
LOOPBACK = "127.0.0.1"
DEBUGGER_PATH = "/debugger"
DEBUGGER_PROTOCOL = "debugger.ppsspp.org"
_WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")

# PSP controller bits (pspctrl.h) and the names PPSSPP's input.buttons.send takes for them.
# The first fourteen are the names a pad script may spell out (src/rt/hle_route.c s_route_btn).
BUTTONS: tuple[tuple[str | None, int, str], ...] = (
    ("SELECT", 0x00000001, "select"),
    ("START", 0x00000008, "start"),
    ("UP", 0x00000010, "up"),
    ("RIGHT", 0x00000020, "right"),
    ("DOWN", 0x00000040, "down"),
    ("LEFT", 0x00000080, "left"),
    ("L", 0x00000100, "ltrigger"),
    ("R", 0x00000200, "rtrigger"),
    ("TRIANGLE", 0x00001000, "triangle"),
    ("CIRCLE", 0x00002000, "circle"),
    ("CROSS", 0x00004000, "cross"),
    ("SQUARE", 0x00008000, "square"),
    ("HOME", 0x00010000, "home"),
    ("HOLD", 0x00020000, "hold"),
    (None, 0x00100000, "vol_up"),
    (None, 0x00200000, "vol_down"),
    (None, 0x00400000, "screen"),
    (None, 0x00800000, "note"),
)
_BIT_TO_PPSSPP = {bit: ppsspp for _, bit, ppsspp in BUTTONS}
_NAME_TO_BIT = {name: bit for name, bit, _ in BUTTONS if name}
KNOWN_MASK = sum(_BIT_TO_PPSSPP)
CENTRE = 128


class RefError(Exception):
    """A scene file, pad script or PPSSPP answer the tool refuses."""


class NotRun(Exception):
    """The capture could not run; ``reason`` is the NOT_RUN token."""

    def __init__(self, reason: str, detail: str = ""):
        super().__init__(f"{reason}{' (' + detail + ')' if detail else ''}")
        self.reason = reason
        self.detail = detail


class CaptureFailed(Exception):
    """PPSSPP ran but did not reach or report what the plan asked for."""


class DebuggerError(Exception):
    """PPSSPP answered a request with an error event."""


# ---- timing ---------------------------------------------------------------------------------

def vblank_us(frame: int) -> float:
    return frame * PERIOD_US_NUM / PERIOD_US_DEN


def capture_us(frame: int) -> int:
    """Half a display period after vblank ``frame``: inside the period that frame is shown."""
    return round((2 * frame + 1) * PERIOD_US_NUM / (2 * PERIOD_US_DEN))


def input_us(frame: int) -> int:
    """Half a period before vblank ``frame``, so that vblank's controller sample sees it."""
    if frame <= 0:
        return 0
    return round((2 * frame - 1) * PERIOD_US_NUM / (2 * PERIOD_US_DEN))


# ---- the pad script -------------------------------------------------------------------------

@dataclass(frozen=True)
class PadRow:
    frame: int
    mask: int
    width: int
    stick: tuple[int, int] | None  # (lx, ly), or None for a centred stick


def parse_mask(token: str) -> int:
    """The runtime's mask grammar: hex (optional 0x, 1-8 digits) or names joined by '+'."""
    digits = token[2:] if token[:2].lower() == "0x" else token
    if digits and all(c in "0123456789abcdefABCDEF" for c in digits):
        if len(digits) > 8:
            raise RefError(f"mask '{token}' is wider than 32 bits")
        return int(digits, 16)
    if token[:2].lower() == "0x":
        raise RefError(f"mask '{token}' is not hex")
    mask = 0
    for name in token.split("+"):
        bit = _NAME_TO_BIT.get(name.upper())
        if bit is None:
            raise RefError(f"'{name}' is not a button name ({' '.join(_NAME_TO_BIT)}, joinable with '+')")
        mask |= bit
    return mask


def _axis(token: str) -> int:
    if not token.isdigit() or int(token) > 255:
        raise RefError(f"stick value '{token}' is not 0-255")
    return int(token)


def parse_padscript(text: str, label: str = "pad") -> list[PadRow]:
    """Read the legacy ``SR_PADSCRIPT`` table; refuse a route program and any unreadable row."""
    rows: list[PadRow] = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        tokens = raw.split()
        if not tokens or tokens[0].startswith("#"):
            continue
        if not tokens[0][0].isdigit():
            raise RefError(f"{label}:{lineno}: '{tokens[0]}' starts a route program; PPSSPP cannot "
                           "report guest state, so only the legacy '<frame> <mask> <width>' table "
                           "is accepted")
        cut = next((i for i, t in enumerate(tokens) if t.startswith("#")), len(tokens))
        tokens = tokens[:cut]
        if len(tokens) not in (3, 5):
            raise RefError(f"{label}:{lineno}: expected '<frame> <mask> <width> [<lx> <ly>]'")
        if not tokens[0].isdigit() or not tokens[2].isdigit():
            raise RefError(f"{label}:{lineno}: frame and width are decimal vblank counts")
        try:
            mask = parse_mask(tokens[1])
            stick = (_axis(tokens[3]), _axis(tokens[4])) if len(tokens) == 5 else None
        except RefError as exc:
            raise RefError(f"{label}:{lineno}: {exc}") from None
        if mask & ~KNOWN_MASK:
            raise RefError(f"{label}:{lineno}: mask 0x{mask:x} has bits PPSSPP's debugger cannot "
                           f"press (0x{mask & ~KNOWN_MASK:x})")
        rows.append(PadRow(int(tokens[0]), mask, int(tokens[2]), stick))
        if len(rows) > MAX_PAD_ROWS:
            raise RefError(f"{label}: more than {MAX_PAD_ROWS} rows")
    return rows


@dataclass(frozen=True)
class PadState:
    frame: int
    mask: int
    stick: tuple[int, int]


def pad_states(rows: Sequence[PadRow]) -> list[PadState]:
    """The pad over time: one state per change, from frame 0.

    The same rule as the runtime's table (src/rt/scripted_input.h ``sr_input_from_rows``): a row
    holds its mask for vblanks ``frame .. frame + width - 1``; overlapping rows OR their masks,
    and the stick of the row that started last holds. A zero-width row presses nothing.
    """
    live = [r for r in rows if r.width > 0]
    cuts = sorted({0} | {r.frame for r in live} | {r.frame + r.width for r in live})
    states: list[PadState] = []
    for at in cuts:
        held = [r for r in live if r.frame <= at < r.frame + r.width]
        mask = 0
        for r in held:
            mask |= r.mask
        with_stick = [r for r in held if r.stick is not None]
        latest = max(with_stick, key=lambda r: r.frame).stick if with_stick else None
        stick = latest if latest is not None else (CENTRE, CENTRE)
        if states and states[-1].mask == mask and states[-1].stick == stick:
            continue
        states.append(PadState(at, mask, stick))
    return states


def analog_value(byte: int, invert: bool = False) -> float:
    """The float PPSSPP's input.analog.send turns back into ``byte``.

    PPSSPP stores ``ceil(x * 127.5 + 127.5)`` for x and ``ceil(-y * 127.5 + 127.5)`` for y, so
    ``byte - 0.25`` (scaled) lands inside the byte's ceiling interval with margin to spare; the
    ends clamp to the -1..1 range the request accepts.
    """
    if byte == CENTRE:
        return 0.0
    value = (byte - 127.75) / 127.5
    value = max(-1.0, min(1.0, value))
    return -value if invert else value


# ---- the scene file -------------------------------------------------------------------------

@dataclass(frozen=True)
class Scene:
    name: str
    frame: int
    nakagawa: Path | None
    threshold: int
    max_percent: float


@dataclass(frozen=True)
class SceneFile:
    path: Path
    title: str
    disc: Path | None
    pad: Path | None
    out: Path
    rtc: int
    timeout: float
    ini: dict[str, dict[str, str]]
    scenes: tuple[Scene, ...]

    def reference(self, scene: Scene) -> Path:
        return self.out / f"{scene.name}.png"


def _inside_checkout(path: Path) -> bool:
    path = Path(os.path.abspath(path))
    try:
        rel = path.relative_to(ROOT)
    except ValueError:
        return False
    return not (rel.parts and rel.parts[0] == "build")


def load_scene_file(path: Path) -> SceneFile:
    path = Path(path)
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise RefError(f"{path.name}: {exc.strerror or exc}") from None
    except json.JSONDecodeError as exc:
        raise RefError(f"{path.name}: not JSON ({exc.msg} at line {exc.lineno})") from None
    if not isinstance(doc, dict):
        raise RefError(f"{path.name}: the top level must be an object")
    known = {"title", "disc", "pad", "out", "rtc", "timeout", "ini", "scenes"}
    extra = sorted(set(doc) - known)
    if extra:
        raise RefError(f"{path.name}: unknown keys {extra}")
    base = path.resolve().parent

    def opt_path(key: str) -> Path | None:
        value = doc.get(key)
        if value is None:
            return None
        if not isinstance(value, str) or not value:
            raise RefError(f"{path.name}: '{key}' must be a path string")
        return (base / value).resolve()

    title = doc.get("title", path.stem)
    if not isinstance(title, str) or not title:
        raise RefError(f"{path.name}: 'title' must be a string")
    out = opt_path("out") or (base / "ppsspp-ref").resolve()
    if _inside_checkout(out):
        raise RefError(f"{path.name}: 'out' is inside the checkout; PPSSPP references are game "
                       "pixels and stay in the title's private folder")
    rtc = doc.get("rtc", DEFAULT_RTC)
    if not isinstance(rtc, int) or isinstance(rtc, bool) or not 0 <= rtc <= 0x7FFFFFFF:
        raise RefError(f"{path.name}: 'rtc' must be Unix seconds within 0..2^31-1")
    timeout = doc.get("timeout", DEFAULT_TIMEOUT_SECONDS)
    if not isinstance(timeout, (int, float)) or isinstance(timeout, bool) or timeout <= 0:
        raise RefError(f"{path.name}: 'timeout' must be a positive number of seconds")
    ini = doc.get("ini", {})
    if not isinstance(ini, dict) or not all(
            isinstance(s, str) and isinstance(kv, dict)
            and all(isinstance(k, str) and isinstance(v, (str, int, float, bool)) for k, v in kv.items())
            for s, kv in ini.items()):
        raise RefError(f"{path.name}: 'ini' must map section names to {{key: value}} objects")
    ini_norm = {s: {k: _ini_value(v) for k, v in kv.items()} for s, kv in ini.items()}
    raw_scenes = doc.get("scenes")
    if not isinstance(raw_scenes, list) or not raw_scenes:
        raise RefError(f"{path.name}: 'scenes' must be a non-empty list")
    scenes: list[Scene] = []
    for i, s in enumerate(raw_scenes):
        where = f"{path.name}: scenes[{i}]"
        if not isinstance(s, dict):
            raise RefError(f"{where} must be an object")
        extra = sorted(set(s) - {"name", "frame", "nakagawa", "threshold", "max_percent"})
        if extra:
            raise RefError(f"{where}: unknown keys {extra}")
        name, frame = s.get("name"), s.get("frame")
        if not isinstance(name, str) or not _NAME_RE.match(name):
            raise RefError(f"{where}: 'name' must be 1-64 of A-Z a-z 0-9 . _ - (it names a file)")
        if not isinstance(frame, int) or isinstance(frame, bool) or frame < 0:
            raise RefError(f"{where}: 'frame' must be a vblank number >= 0")
        threshold = s.get("threshold", 0)
        max_percent = s.get("max_percent", 0.0)
        if not isinstance(threshold, int) or isinstance(threshold, bool) or threshold < 0:
            raise RefError(f"{where}: 'threshold' must be an RGB distance >= 0")
        if (not isinstance(max_percent, (int, float)) or isinstance(max_percent, bool)
                or not 0 <= max_percent <= 100):
            raise RefError(f"{where}: 'max_percent' must be within 0..100")
        nk = s.get("nakagawa")
        if nk is not None and (not isinstance(nk, str) or not nk):
            raise RefError(f"{where}: 'nakagawa' must be a path string")
        scenes.append(Scene(name, frame, (base / nk).resolve() if nk else None, threshold,
                            float(max_percent)))
    names = [s.name for s in scenes]
    if len(set(names)) != len(names):
        raise RefError(f"{path.name}: scene names repeat")
    return SceneFile(path, title, opt_path("disc"), opt_path("pad"), out, rtc, float(timeout),
                     ini_norm, tuple(scenes))


def _ini_value(value: object) -> str:
    if isinstance(value, bool):
        return "True" if value else "False"
    text = str(value)
    if "\n" in text or "\r" in text:
        raise RefError("an 'ini' value holds a line break")
    return text


# ---- the PPSSPP launch ----------------------------------------------------------------------

# Settings every capture pins: the debugger on a known port, no first-run screens, no update
# check, no state loading, no cheats, the software renderer with no frame skipping. A scene
# file's "ini" may add to or override them, and the reference sidecar records the result.
BASE_INI: dict[str, dict[str, str]] = {
    "General": {
        "FirstRun": "False",
        "CheckForNewVersion": "False",
        "RemoteDebuggerOnStartup": "True",
        "AutoLoadSaveState": "0",
        "EnableCheats": "False",
        "RewindSnapshotInterval": "0",
    },
    "Graphics": {
        "SoftwareRenderer": "True",
        "FrameSkip": "0",
        "AutoFrameSkip": "False",
    },
}


def effective_ini(port: int, overrides: Mapping[str, Mapping[str, str]]) -> dict[str, dict[str, str]]:
    ini = {s: dict(kv) for s, kv in BASE_INI.items()}
    for section, kv in overrides.items():
        ini.setdefault(section, {}).update(kv)
    general = ini.setdefault("General", {})
    if general.get("RemoteDebuggerOnStartup") != "True":
        raise RefError("the scene file's 'ini' turns the debugger off; the driver needs it")
    general["RemoteISOPort"] = str(port)  # the driver's choice, never the scene file's
    return ini


def render_ini(ini: Mapping[str, Mapping[str, str]]) -> str:
    lines: list[str] = []
    for section in sorted(ini):
        lines.append(f"[{section}]")
        lines.extend(f"{k} = {v}" for k, v in sorted(ini[section].items()))
        lines.append("")
    return "\n".join(lines)


def build_command(exe: Path, ini_path: Path, disc: Path) -> list[str]:
    return [str(exe), f"--appendconfig={ini_path}", str(disc)]


def resolve_ppsspp(arg: str | None, env: Mapping[str, str]) -> Path:
    value = arg or env.get("PPSSPP_EXE", "")
    if not value:
        raise NotRun("no-ppsspp", "$PPSSPP_EXE is not set and --ppsspp was not given")
    exe = Path(value)
    if not exe.is_file():
        raise NotRun("no-ppsspp", f"{exe.name} does not exist")
    return exe


def free_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind((LOOPBACK, 0))
        port: int = s.getsockname()[1]
        return port


# ---- the plan -------------------------------------------------------------------------------

@dataclass(frozen=True)
class Action:
    us: int
    kind: str  # "capture" or "input"
    frame: int
    scene: str = ""
    buttons: tuple[tuple[str, bool], ...] = ()
    stick: tuple[int, int] | None = None  # set when the stick changes

    def describe(self) -> str:
        if self.kind == "capture":
            return f"{self.us:>12} us  v{self.frame:<6} capture {self.scene}"
        parts = [f"{k}={'on' if v else 'off'}" for k, v in self.buttons]
        if self.stick is not None:
            parts.append(f"stick={self.stick[0]},{self.stick[1]}")
        return f"{self.us:>12} us  v{self.frame:<6} input {' '.join(parts) or '(none)'}"


def build_plan(rows: Sequence[PadRow], scenes: Sequence[Scene]) -> list[Action]:
    """Inputs and captures in emulated-time order.

    Only the buttons the script uses are ever sent, all released at boot, so PPSSPP's own
    disc and memory-stick sensor bits are never touched. At an equal time a capture comes
    first: setting the pad does not change the frame already on screen.
    """
    used = 0
    for r in rows:
        used |= r.mask
    names = [_BIT_TO_PPSSPP[bit] for _, bit, _ in BUTTONS if used & bit]
    actions: list[Action] = []
    pressed: dict[str, bool] = {}
    stick = (CENTRE, CENTRE)
    for i, state in enumerate(pad_states(rows)):
        change = []
        for _, bit, ppsspp in BUTTONS:
            if ppsspp not in names:
                continue
            on = bool(state.mask & bit)
            if i == 0 or pressed.get(ppsspp) != on:
                change.append((ppsspp, on))
            pressed[ppsspp] = on
        new_stick = state.stick if (i == 0 or state.stick != stick) else None
        stick = state.stick
        if change or new_stick is not None:
            actions.append(Action(input_us(state.frame), "input", state.frame,
                                  buttons=tuple(change), stick=new_stick))
    for s in scenes:
        actions.append(Action(capture_us(s.frame), "capture", s.frame, scene=s.name))
    order = {"capture": 0, "input": 1}
    actions.sort(key=lambda a: (a.us, order[a.kind]))
    return actions


# ---- a minimal WebSocket client (RFC 6455), loopback only -----------------------------------

class WebSocket:
    """Text messages over one client connection. Fragmentation, ping and close handled."""

    def __init__(self, sock: socket.socket, leftover: bytes = b""):
        self.sock = sock
        self.buf = bytearray(leftover)
        self.closed = False

    @classmethod
    def connect(cls, port: int, timeout: float, path: str = DEBUGGER_PATH,
                protocol: str = DEBUGGER_PROTOCOL) -> "WebSocket":
        sock = socket.create_connection((LOOPBACK, port), timeout=timeout)
        try:
            return cls.handshake(sock, f"{LOOPBACK}:{port}", path, protocol)
        except Exception:
            sock.close()
            raise

    @classmethod
    def handshake(cls, sock: socket.socket, host: str, path: str, protocol: str) -> "WebSocket":
        key = base64.b64encode(os.urandom(16))
        request = (f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
                   f"Connection: Upgrade\r\nSec-WebSocket-Key: {key.decode()}\r\n"
                   f"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: {protocol}\r\n\r\n")
        sock.sendall(request.encode("ascii"))
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = sock.recv(4096)
            if not chunk:
                raise CaptureFailed("the debugger closed the connection during the handshake")
            data += chunk
            if len(data) > 65536:
                raise CaptureFailed("the debugger's handshake reply is too long")
        head, _, rest = data.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        if not re.match(r"^HTTP/1\.1 101\b", lines[0]):
            raise CaptureFailed(f"the debugger refused the WebSocket upgrade ({lines[0]!r})")
        headers = {}
        for line in lines[1:]:
            k, _, v = line.partition(":")
            headers[k.strip().lower()] = v.strip()
        want = base64.b64encode(hashlib.sha1(key + _WS_GUID).digest()).decode()
        if headers.get("sec-websocket-accept") != want:
            raise CaptureFailed("the debugger's Sec-WebSocket-Accept does not match the key")
        return cls(sock, rest)

    def _read(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(max(65536, n - len(self.buf)))
            if not chunk:
                self.closed = True
                raise CaptureFailed("the debugger closed the connection")
            self.buf += chunk
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        n = len(payload)
        if n < 126:
            head = struct.pack("!BB", 0x80 | opcode, 0x80 | n)
        elif n < 65536:
            head = struct.pack("!BBH", 0x80 | opcode, 0x80 | 126, n)
        else:
            head = struct.pack("!BBQ", 0x80 | opcode, 0x80 | 127, n)
        key = os.urandom(4)
        self.sock.sendall(head + key + mask_payload(payload, key))

    def send_text(self, text: str) -> None:
        self._send_frame(0x1, text.encode("utf-8"))

    def recv_text(self, timeout: float) -> str:
        self.sock.settimeout(timeout)
        parts: list[bytes] = []
        total = 0
        while True:
            b0, b1 = self._read(2)
            fin, opcode, masked, n = b0 & 0x80, b0 & 0x0F, b1 & 0x80, b1 & 0x7F
            if b0 & 0x70:
                raise CaptureFailed("the debugger sent a frame with reserved bits set")
            if masked:
                raise CaptureFailed("the debugger sent a masked frame (servers must not mask)")
            if n == 126:
                (n,) = struct.unpack("!H", self._read(2))
            elif n == 127:
                (n,) = struct.unpack("!Q", self._read(8))
            if opcode >= 0x8 and (n > 125 or not fin):
                raise CaptureFailed("the debugger sent a malformed control frame")
            if total + n > MAX_MESSAGE_BYTES:
                raise CaptureFailed(f"a debugger message is larger than {MAX_MESSAGE_BYTES} bytes")
            payload = self._read(n)
            if opcode == 0x9:
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:
                continue
            if opcode == 0x8:
                self.closed = True
                raise CaptureFailed("the debugger closed the connection")
            if opcode not in (0x0, 0x1, 0x2) or (opcode == 0x0) != bool(parts):
                raise CaptureFailed(f"the debugger sent an unexpected frame (opcode {opcode})")
            parts.append(payload)
            total += n
            if fin:
                try:
                    return b"".join(parts).decode("utf-8")
                except UnicodeDecodeError:
                    raise CaptureFailed("a debugger message is not UTF-8") from None

    def close(self) -> None:
        if not self.closed:
            try:
                self._send_frame(0x8, struct.pack("!H", 1000))
            except OSError:
                pass
        self.closed = True
        self.sock.close()


def mask_payload(payload: bytes, key: bytes) -> bytes:
    n = len(payload)
    if not n:
        return b""
    keystream = (key * (n // 4 + 1))[:n]
    return (int.from_bytes(payload, "big") ^ int.from_bytes(keystream, "big")).to_bytes(n, "big")


# ---- the debugger protocol ------------------------------------------------------------------

class Debugger:
    """Requests with tickets; any other message (a broadcast) is kept for ``wait_event``."""

    def __init__(self, transport, request_timeout: float = REQUEST_TIMEOUT_SECONDS):
        self.transport = transport
        self.request_timeout = request_timeout
        self.events: list[dict] = []
        self._ticket = 0

    def _recv(self, timeout: float) -> dict:
        text = self.transport.recv_text(timeout)
        try:
            msg = json.loads(text)
        except json.JSONDecodeError:
            raise CaptureFailed("the debugger sent a message that is not JSON") from None
        if not isinstance(msg, dict) or not isinstance(msg.get("event"), str):
            raise CaptureFailed("the debugger sent a message without an event name")
        return msg

    def request(self, event: str, timeout: float | None = None, **params) -> dict:
        self._ticket += 1
        ticket = f"nk{self._ticket}"
        self.transport.send_text(json.dumps({"event": event, "ticket": ticket, **params}))
        deadline = time.monotonic() + (timeout or self.request_timeout)
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise CaptureFailed(f"no answer to {event} within {timeout or self.request_timeout:g} s")
            msg = self._recv(left)
            if msg.get("ticket") == ticket or (msg["event"] == "error" and "ticket" not in msg):
                if msg["event"] == "error":
                    raise DebuggerError(str(msg.get("message", "error")))
                if msg["event"] != event:
                    raise CaptureFailed(f"{event} was answered by {msg['event']}")
                return msg
            self.events.append(msg)

    def clear_events(self) -> None:
        self.events.clear()

    def wait_event(self, event: str, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            for i, msg in enumerate(self.events):
                if msg["event"] == event:
                    return self.events.pop(i)
            left = deadline - time.monotonic()
            if left <= 0:
                raise CaptureFailed(f"no {event} event within {timeout:g} s")
            self.events.append(self._recv(left))


def _unknown_event(exc: DebuggerError) -> bool:
    return "unknown event" in str(exc).lower()


def decode_screenshot(msg: Mapping) -> picture_gate.Frame:
    """A ``gpu.buffer.screenshot`` base64 answer as RGB rows, top row first."""
    try:
        width, height = int(msg["width"]), int(msg["height"])
        fmt = str(msg["format"])
        data = base64.b64decode(msg["base64"], validate=True)
    except (KeyError, TypeError, ValueError):
        raise CaptureFailed("the screenshot answer lacks width, height, format or base64") from None
    layouts = {"R8G8B8A8_UNORM": (4, (0, 1, 2)), "B8G8R8A8_UNORM": (4, (2, 1, 0)),
               "R8G8B8_UNORM": (3, (0, 1, 2))}
    if fmt not in layouts:
        raise CaptureFailed(f"the screenshot format {fmt!r} is not one the driver reads")
    bpp, (ri, gi, bi) = layouts[fmt]
    if width <= 0 or height <= 0 or len(data) != width * height * bpp:
        raise CaptureFailed(f"the screenshot holds {len(data)} bytes, not {width}x{height}x{bpp}")
    rgb = bytearray(width * height * 3)
    rgb[0::3] = data[ri::bpp]
    rgb[1::3] = data[gi::bpp]
    rgb[2::3] = data[bi::bpp]
    if msg.get("flipped"):
        stride = width * 3
        rgb = bytearray(b"".join(bytes(rgb[y * stride:(y + 1) * stride]) for y in reversed(range(height))))
    return picture_gate.Frame(width, height, bytes(rgb))


def write_png(path: Path, frame: picture_gate.Frame) -> None:
    """8-bit RGB, one IDAT, filter 0 on every row: the subset tools/pngcmp.py decodes."""
    stride = frame.width * 3
    raw = b"".join(b"\x00" + frame.pixels[y * stride:(y + 1) * stride] for y in range(frame.height))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", frame.width, frame.height, 8, 2, 0, 0, 0)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
                           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


@dataclass
class Shot:
    scene: str
    frame: int
    us: int
    picture: picture_gate.Frame


@dataclass
class DriveResult:
    version: str
    base_us: int
    shots: list[Shot] = field(default_factory=list)
    max_early_us: int = 0  # the furthest a stop fell short of its planned time (clock rounding)
    max_late_us: int = 0   # the furthest a stop ran past it


def drive(dbg: Debugger, plan: Sequence[Action], pid: int | None, rtc: int, timeout: float,
          clock: Callable[[], float] = time.monotonic, pause: Callable[[float], None] = time.sleep
          ) -> DriveResult:
    """Run one capture against a connected PPSSPP debugger."""
    deadline = clock() + timeout

    def left() -> float:
        remaining = deadline - clock()
        if remaining <= 0:
            raise CaptureFailed(f"the capture did not finish within {timeout:g} s")
        return remaining

    info = dbg.request("version", name=TOOL_NAME, version=TOOL_VERSION)
    version = str(info.get("version", "?"))
    if "pid" not in info:
        raise NotRun("ppsspp-too-old", f"PPSSPP {version} does not report its process id; the "
                     "driver needs a build newer than v1.20.4")
    if pid is not None and int(info["pid"]) != pid:
        raise CaptureFailed(f"the debugger on this port is process {info['pid']}, not the "
                            f"PPSSPP the driver started ({pid})")
    while dbg.request("game.status").get("game") is None:
        left()
        pause(0.25)
    dbg.clear_events()
    dbg.request("game.reset", **{"break": True})
    while not dbg.request("cpu.status").get("stepping"):
        left()
        pause(0.05)
    try:
        dbg.request("replay.time.set", value=rtc)
    except DebuggerError as exc:
        raise CaptureFailed(f"PPSSPP refused to pin the clock ({exc})") from None
    status = dbg.request("cpu.status")
    if "us" not in status:
        raise NotRun("ppsspp-too-old", f"PPSSPP {version} reports no emulated time in cpu.status")
    base_us = int(status["us"])
    result = DriveResult(version, base_us)
    for action in plan:
        target = base_us + action.us
        dbg.clear_events()
        try:
            dbg.request("cpu.runUntilTime", us=target)
        except DebuggerError as exc:
            if _unknown_event(exc):
                raise NotRun("ppsspp-too-old", f"PPSSPP {version} has no cpu.runUntilTime") from None
            raise CaptureFailed(f"cpu.runUntilTime {target} refused ({exc})") from None
        dbg.wait_event("cpu.stepping", left())
        reached = int(dbg.request("cpu.status").get("us", -1))
        if reached < target - CLOCK_SLACK_US:
            raise CaptureFailed(f"PPSSPP stopped at {reached} us before the {target} us the plan "
                                f"asked for (a breakpoint or an exception?)")
        if reached > target + LATE_LIMIT_US:
            raise CaptureFailed(f"PPSSPP stopped at {reached} us, past the next vblank after the "
                                f"{target} us the plan asked for")
        result.max_early_us = max(result.max_early_us, target - reached)
        result.max_late_us = max(result.max_late_us, reached - target)
        if action.kind == "input":
            if action.buttons:
                dbg.request("input.buttons.send", buttons=dict(action.buttons))
            if action.stick is not None:
                dbg.request("input.analog.send", stick="left", x=analog_value(action.stick[0]),
                            y=analog_value(action.stick[1], invert=True))
        else:
            shot = dbg.request("gpu.buffer.screenshot", type="base64")
            result.shots.append(Shot(action.scene, action.frame, reached - base_us,
                                     decode_screenshot(shot)))
    return result


# ---- capture --------------------------------------------------------------------------------

def _launch_and_drive(exe: Path, sf: SceneFile, plan: Sequence[Action], work: Path,
                      ) -> tuple[DriveResult, dict[str, dict[str, str]]]:
    if sf.disc is None:
        raise NotRun("no-disc", "the scene file names no disc image")
    disc = sf.disc
    port = free_loopback_port()
    ini = effective_ini(port, sf.ini)
    ini_path = work / "nk-ppsspp-ref.ini"
    ini_path.write_text(render_ini(ini), encoding="utf-8")
    log = (work / "ppsspp.log").open("wb")
    try:
        proc = subprocess.Popen(build_command(exe, ini_path, disc), cwd=str(exe.parent),
                                stdout=log, stderr=subprocess.STDOUT)
    except OSError as exc:
        log.close()
        raise NotRun("ppsspp-did-not-start", exc.strerror or str(exc)) from None
    ws = None
    try:
        deadline = time.monotonic() + 60
        while ws is None:
            if proc.poll() is not None:
                raise CaptureFailed(f"PPSSPP exited ({proc.returncode}) before its debugger answered")
            try:
                ws = WebSocket.connect(port, timeout=5)
            except (OSError, CaptureFailed):
                if time.monotonic() > deadline:
                    raise CaptureFailed(f"no PPSSPP debugger on port {port} within 60 s") from None
                time.sleep(0.5)
        return drive(Debugger(ws), plan, proc.pid, sf.rtc, sf.timeout), ini
    finally:
        if ws is not None:
            ws.close()
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=15)
        log.close()


def capture(sf: SceneFile, exe: Path, runs: int, replace: bool, keep_work: bool = False,
            runner=_launch_and_drive) -> list[tuple[str, str]]:
    """Capture every scene ``runs`` times; store the references only when the runs agree."""
    if sf.disc is None or not sf.disc.exists():
        raise NotRun("no-disc", "the scene file names no disc image" if sf.disc is None
                     else f"{sf.disc.name} does not exist")
    if sf.pad and not sf.pad.is_file():
        raise NotRun("no-pad", f"{sf.pad.name} does not exist")
    rows = parse_padscript(sf.pad.read_text(encoding="utf-8"), sf.pad.name) if sf.pad else []
    existing = [s.name for s in sf.scenes if sf.reference(s).exists()]
    if existing and not replace:
        raise RefError(f"references exist for {existing}; pass --replace to capture over them")
    plan = build_plan(rows, sf.scenes)
    results: list[DriveResult] = []
    ini: dict = {}
    for run in range(runs):
        work = Path(tempfile.mkdtemp(prefix="nk-ppsspp-ref-"))
        try:
            result, ini = runner(exe, sf, plan, work)
            results.append(result)
            print(f"PPSSPP_REF run {run + 1}: every stop within {result.max_early_us} us early and "
                  f"{result.max_late_us} us late of its planned time", file=sys.stderr)
        finally:
            if keep_work:
                print(f"PPSSPP_REF run {run + 1}: work folder kept at {work}", file=sys.stderr)
            else:
                shutil.rmtree(work, ignore_errors=True)
    verdicts: list[tuple[str, str]] = []
    first = results[0]
    disagree = set()
    for other in results[1:]:
        theirs = {s.scene: s.picture for s in other.shots}
        for a in first.shots:
            if theirs.get(a.scene) != a.picture:
                disagree.add(a.scene)
    # The port is the run's own choice and differs every time; it is not a capture setting.
    settings = {s: {k: v for k, v in kv.items() if k != "RemoteISOPort"} for s, kv in ini.items()}
    sf.out.mkdir(parents=True, exist_ok=True)
    for shot in first.shots:
        if shot.scene in disagree:
            # A reference this run was asked to replace must not survive as if it still held.
            for stale in (sf.out / f"{shot.scene}.png", sf.out / f"{shot.scene}.png.json"):
                stale.unlink(missing_ok=True)
            verdicts.append((shot.scene, f"{FAIL} nondeterministic (PPSSPP runs differ; "
                                         "no reference stored)"))
            continue
        ref = sf.out / f"{shot.scene}.png"
        write_png(ref, shot.picture)
        meta = {"tool": TOOL_NAME, "tool_version": TOOL_VERSION, "ppsspp_version": first.version,
                "title": sf.title, "frame": shot.frame, "emulated_us": shot.us,
                "rtc": sf.rtc, "runs": runs, "width": shot.picture.width,
                "height": shot.picture.height,
                "sha256": hashlib.sha256(shot.picture.pixels).hexdigest(),
                "ini": settings}
        Path(str(ref) + ".json").write_text(json.dumps(meta, indent=2, sort_keys=True) + "\n",
                                            encoding="utf-8")
        verdicts.append((shot.scene, f"CAPTURED {shot.picture.width}x{shot.picture.height} "
                                     f"v{shot.frame} ({runs} run{'s' if runs > 1 else ''})"))
    return verdicts


# ---- compare --------------------------------------------------------------------------------

DIFF_MARK = (255, 0, 255)


def diff_image(reference: picture_gate.Frame, frame: picture_gate.Frame,
               threshold: int) -> picture_gate.Frame:
    """The capture dimmed to a third, with every pixel over the threshold in magenta."""
    limit = threshold * threshold
    out = bytearray(len(frame.pixels))
    ref, got = reference.pixels, frame.pixels
    for i in range(0, len(got), 3):
        dr, dg, db = ref[i] - got[i], ref[i + 1] - got[i + 1], ref[i + 2] - got[i + 2]
        if dr * dr + dg * dg + db * db > limit:
            out[i:i + 3] = bytes(DIFF_MARK)
        else:
            out[i], out[i + 1], out[i + 2] = got[i] // 3, got[i + 1] // 3, got[i + 2] // 3
    return picture_gate.Frame(frame.width, frame.height, bytes(out))


@dataclass(frozen=True)
class Grade:
    scene: str
    frame: int
    status: str
    detail: str = ""
    diff: Path | None = None

    def line(self) -> str:
        return (f"PPSSPP_REF {self.scene} v{self.frame}: {self.status}"
                + (f" {self.detail}" if self.detail else ""))


def grade_scene(sf: SceneFile, scene: Scene, diff_dir: Path | None) -> Grade:
    ref_path = sf.reference(scene)
    if not ref_path.exists():
        return Grade(scene.name, scene.frame, NO_REFERENCE)
    try:
        reference = picture_gate.load_picture(ref_path)
    except picture_gate.PictureError as exc:
        return Grade(scene.name, scene.frame, FAIL, f"unreadable-reference ({exc})")
    if scene.nakagawa is None:
        return Grade(scene.name, scene.frame, NOT_RUN, "no-capture (the scene names no Nakagawa frame)")
    if not scene.nakagawa.exists():
        return Grade(scene.name, scene.frame, NOT_RUN, f"no-capture ({scene.nakagawa.name} does not exist)")
    try:
        frame = picture_gate.load_picture(scene.nakagawa)
    except picture_gate.PictureError as exc:
        return Grade(scene.name, scene.frame, NOT_RUN, f"unreadable-capture ({exc})")
    measured = picture_gate.compare_frames(reference, frame, scene.threshold)
    verdict = picture_gate.judge(measured, scene.max_percent)
    if verdict.status == picture_gate.FAIL and verdict.percent is None:
        return Grade(scene.name, scene.frame, FAIL, verdict.detail)
    status = PASS if verdict.status == picture_gate.PASS else FAIL
    diff = None
    if diff_dir is not None and verdict.differing:
        diff_dir.mkdir(parents=True, exist_ok=True)
        diff = diff_dir / f"{scene.name}.diff.png"
        write_png(diff, diff_image(reference, frame, scene.threshold))
    return Grade(scene.name, scene.frame, status,
                 f"{verdict.detail} tolerance={picture_gate.format_percent(scene.max_percent)}", diff)


def compare(sf: SceneFile, diff_dir: Path | None) -> list[Grade]:
    if diff_dir is not None and _inside_checkout(diff_dir):
        raise RefError("the diff folder is inside the checkout; diff images are game pixels")
    return [grade_scene(sf, s, diff_dir) for s in sf.scenes]


def summary(grades: Sequence[Grade]) -> tuple[str, int]:
    counts = {k: sum(1 for g in grades if g.status == k) for k in (PASS, FAIL, NO_REFERENCE, NOT_RUN)}
    line = (f"PPSSPP_REF summary: pass={counts[PASS]} fail={counts[FAIL]} "
            f"no_reference={counts[NO_REFERENCE]} not_run={counts[NOT_RUN]} "
            "(agreement with PPSSPP, not a hardware oracle)")
    if counts[FAIL]:
        return line, EXIT_FAIL
    if counts[NO_REFERENCE] or counts[NOT_RUN]:
        return line, EXIT_NOT_PROVEN
    return line, EXIT_OK


# ---- command line ---------------------------------------------------------------------------

REGISTRATION_NOTE = ("PPSSPP's debugger web server reports this machine's local IP address and "
                     "port to report.ppsspp.org while it runs; pass --allow-ppsspp-registration "
                     "to accept that")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan", help="print the PPSSPP command line and the input/capture timeline")
    p.add_argument("scenes", type=Path)
    p.add_argument("--ppsspp", help="PPSSPP executable (default: $PPSSPP_EXE)")
    p = sub.add_parser("capture", help="drive PPSSPP and store one reference per scene")
    p.add_argument("scenes", type=Path)
    p.add_argument("--ppsspp", help="PPSSPP executable (default: $PPSSPP_EXE)")
    p.add_argument("--runs", type=int, default=2,
                   help="capture N times; scenes whose runs differ are a FAIL and are not stored "
                        "(default 2)")
    p.add_argument("--replace", action="store_true", help="capture over existing references")
    p.add_argument("--allow-ppsspp-registration", action="store_true",
                   help="accept that PPSSPP's debugger server registers with report.ppsspp.org")
    p.add_argument("--keep-work", action="store_true", help="keep each run's PPSSPP log and ini")
    p = sub.add_parser("compare", help="grade Nakagawa's captures against the PPSSPP references")
    p.add_argument("scenes", type=Path)
    p.add_argument("--diff-dir", type=Path,
                   help="write <scene>.diff.png for every scene that differs (default: <out>/diff)")
    p.add_argument("--no-diff", action="store_true", help="write no diff images")
    args = ap.parse_args(argv)

    try:
        sf = load_scene_file(args.scenes)
    except RefError as exc:
        print(f"PPSSPP_REF {args.cmd}: refused ({exc})", file=sys.stderr)
        return EXIT_USAGE

    if args.cmd == "plan":
        try:
            rows = parse_padscript(sf.pad.read_text(encoding="utf-8"), sf.pad.name) if sf.pad else []
        except (OSError, RefError) as exc:
            print(f"PPSSPP_REF plan: refused ({exc})", file=sys.stderr)
            return EXIT_USAGE
        exe = Path(args.ppsspp or os.environ.get("PPSSPP_EXE") or "PPSSPP")
        disc = sf.disc or Path("<no disc>")
        print("PPSSPP_REF command: " + " ".join(build_command(exe, Path("<work>/nk-ppsspp-ref.ini"), disc)))
        for action in build_plan(rows, sf.scenes):
            print("PPSSPP_REF plan " + action.describe())
        return EXIT_OK

    if args.cmd == "capture":
        if args.runs < 1:
            ap.error("--runs must be >= 1")
        try:
            exe = resolve_ppsspp(args.ppsspp, os.environ)
            if not args.allow_ppsspp_registration:
                raise NotRun("registration-not-accepted", REGISTRATION_NOTE)
            verdicts = capture(sf, exe, args.runs, args.replace, args.keep_work)
        except NotRun as exc:
            print(f"PPSSPP_REF capture: {NOT_RUN} {exc}")
            return EXIT_NOT_PROVEN
        except (RefError, OSError) as exc:
            print(f"PPSSPP_REF capture: refused ({exc})", file=sys.stderr)
            return EXIT_USAGE
        except (CaptureFailed, DebuggerError) as exc:
            print(f"PPSSPP_REF capture: {FAIL} {exc}")
            return EXIT_FAIL
        for scene, text in verdicts:
            print(f"PPSSPP_REF {scene}: {text}")
        return EXIT_FAIL if any(t.startswith(FAIL) for _, t in verdicts) else EXIT_OK

    diff_dir = None if args.no_diff else (args.diff_dir or sf.out / "diff")
    try:
        grades = compare(sf, diff_dir)
    except RefError as exc:
        print(f"PPSSPP_REF compare: refused ({exc})", file=sys.stderr)
        return EXIT_USAGE
    for g in grades:
        print(g.line() + (f" diff={g.diff.name}" if g.diff else ""))
    line, code = summary(grades)
    print(line)
    return code


if __name__ == "__main__":
    sys.exit(main())
