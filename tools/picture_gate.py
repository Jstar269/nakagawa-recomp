#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Per-title headless picture check against private reference frames (#1845).

A title route that proves a frame was presented, not black and not stale
(``tools/frame_capture_check.py``) cannot see a menu whose text or colours broke. This gate
compares one captured frame with a reference picture kept outside the repository, and
reports one verdict per title:

  PASS <percent>%      the share of pixels over the threshold is within the tolerance
  FAIL <percent>% ...  too many pixels differ, the sizes differ, or runs disagree
  SKIP no-reference    the title has no reference yet (run ``accept`` once)
  NOT_RUN <reason>     the frame could not be produced (no route, no input, no capture)

A SKIP or NOT_RUN is never a pass: it prints its own verdict and ``--strict`` makes it an
exit status. A picture match is a regression tripwire, not a correctness claim.

Pictures are game pixels. A reference lives in the title's private folder, never in git:
``accept`` refuses a reference path inside this checkout (``build/`` excepted, it is ignored).
Public tests and ``display-smoke`` use the source-owned synthetic fixture only.

The frame comes from the runtime's own capture path (``SR_VIDEO=offscreen`` with
``SR_FBSNAP_WINDOWS``), never from the desktop. Nothing here opens a window, and nothing
reads the screen.

Usage:
  python tools/picture_gate.py compare REFERENCE FRAME [--threshold N] [--max-percent P]
  python tools/picture_gate.py accept FRAME REFERENCE [--commit SHA]
  python tools/picture_gate.py check ROUTE.json [--runs N] [--accept] [--strict]
  python tools/picture_gate.py display-smoke --build-dir DIR [--work-dir DIR]

``check --accept`` captures at least twice. ``display-smoke`` clears only a work folder it
made (it holds a marker file) and refuses any other non-empty folder.

A route file is private JSON next to the title's other private inputs:
  {"title": "...", "command": ["runtime.exe", "..."], "env": {"K": "V"},
   "pad": "route.pad", "vblank": 600, "window": 8, "reference": "ref.ppm",
   "mkdirs": ["fs", "memstick"],
   "threshold": 0, "max_percent": 0.0, "timeout": 600}
Relative paths resolve against the route file's folder. The gate adds the offscreen
presenter, dummy SDL drivers and the capture window; ``{run}`` in the command and in env
values is the run's fresh scratch folder (``mkdirs`` creates folders in it); it removes inherited ``SR_FBSNAP*``,
``SR_PADSCRIPT``, ``SR_FLIGHT*`` and ``SR_DATAROOT`` so the route file alone decides them.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

PASS, FAIL, SKIP, NOT_RUN = "PASS", "FAIL", "SKIP", "NOT_RUN"
ACCEPTED = "ACCEPTED"
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_NOT_PROVEN = 0, 1, 2, 3
DEFAULT_WINDOW = 8
MIN_ACCEPT_RUNS = 2
MAX_RGB_DISTANCE = 442  # sqrt(3 * 255^2) rounded up
DEFAULT_TIMEOUT_SECONDS = 600
# Inherited selectors that would change what the run captures or where it reads input.
_STRIPPED_ENV = re.compile(r"^(SR_FBSNAP.*|SR_FBDUMP|SR_PADSCRIPT.*|SR_FLIGHT.*|SR_DATAROOT|"
                           r"SR_EXIT_AT_VBLANK|SR_FIRST_FRAME_DUMP)$")
_FRAME_V = re.compile(r"^frame_v(\d+)\.ppm$")


class PictureError(Exception):
    """A frame or reference that cannot be read as a picture."""


@dataclass(frozen=True)
class Frame:
    width: int
    height: int
    pixels: bytes  # width * height * 3, RGB


@dataclass(frozen=True)
class Verdict:
    status: str
    detail: str = ""
    percent: float | None = None
    over: int = 0
    differing: int = 0
    total: int = 0
    max_delta: int = 0

    def line(self, title: str) -> str:
        return f"PICTURE_GATE {title}: {self.status}" + (f" {self.detail}" if self.detail else "")


def format_percent(percent: float) -> str:
    # An exact match prints 0.00%; any nonzero share keeps enough digits to show one pixel.
    return "0.00%" if percent == 0 else f"{percent:.4f}%"


# ---- reading pictures -----------------------------------------------------------------------

def parse_ppm(data: bytes, label: str = "frame") -> Frame:
    """Decode the exact binary P6 the runtime's capture service publishes."""
    pos = 0
    tokens = []
    while len(tokens) < 4:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if pos < len(data) and data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        if start == pos:
            raise PictureError(f"{label}: truncated PPM header")
        tokens.append(data[start:pos])
    pos += 1  # exactly one whitespace byte separates the header from the pixels
    if tokens[0] != b"P6":
        raise PictureError(f"{label}: not a binary P6 PPM")
    try:
        width, height, maxval = (int(t) for t in tokens[1:])
    except ValueError:
        raise PictureError(f"{label}: malformed PPM header") from None
    if width <= 0 or height <= 0 or maxval != 255:
        raise PictureError(f"{label}: unsupported PPM geometry {width}x{height} maxval={maxval}")
    need = width * height * 3
    pixels = data[pos:pos + need]
    if len(pixels) != need or len(data) != pos + need:
        raise PictureError(f"{label}: PPM holds {len(data) - pos} pixel bytes, expected {need}")
    return Frame(width, height, pixels)


def load_picture(path: Path) -> Frame:
    path = Path(path)
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise PictureError(f"{path.name}: {exc.strerror or exc}") from None
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        import pngcmp  # the project's RGB PNG subset; rejects anything else loudly
        try:
            width, height, rows = pngcmp.decode(path)
        except SystemExit as exc:
            raise PictureError(str(exc)) from None
        except Exception as exc:  # a truncated or corrupt stream: struct, zlib, index errors
            raise PictureError(f"{path.name}: corrupt PNG ({type(exc).__name__})") from None
        return Frame(width, height, b"".join(rows))
    return parse_ppm(data, path.name)


# ---- the metric -----------------------------------------------------------------------------

def compare_frames(reference: Frame, frame: Frame, threshold: int = 0) -> Verdict:
    """Share of pixels whose RGB distance from the reference exceeds ``threshold``.

    A size mismatch is a FAIL, never a resize. ``differing`` counts every pixel that is not
    identical, so a change under the threshold is still visible in the report.
    """
    if (reference.width, reference.height) != (frame.width, frame.height):
        return Verdict(FAIL, f"size-mismatch reference={reference.width}x{reference.height} "
                             f"frame={frame.width}x{frame.height}")
    total = reference.width * reference.height
    limit = threshold * threshold
    over = differing = max_sq = 0
    ref, got = reference.pixels, frame.pixels
    if ref != got:
        for i in range(0, total * 3, 3):
            if ref[i:i + 3] == got[i:i + 3]:
                continue
            dr, dg, db = ref[i] - got[i], ref[i + 1] - got[i + 1], ref[i + 2] - got[i + 2]
            sq = dr * dr + dg * dg + db * db
            differing += 1
            max_sq = max(max_sq, sq)
            if sq > limit:
                over += 1
    return Verdict(PASS, percent=100.0 * over / total, over=over, differing=differing,
                   total=total, max_delta=math.isqrt(max_sq))


def judge(measured: Verdict, max_percent: float) -> Verdict:
    """Turn a measurement into PASS or FAIL against the tolerance."""
    if measured.status == FAIL or measured.percent is None:
        return measured
    detail = (f"{format_percent(measured.percent)} over={measured.over}/{measured.total} "
              f"differing={measured.differing} max_delta={measured.max_delta}")
    status = PASS if measured.percent <= max_percent else FAIL
    return Verdict(status, detail, measured.percent, measured.over, measured.differing,
                   measured.total, measured.max_delta)


def compare_files(reference: Path, frame: Path, threshold: int = 0,
                  max_percent: float = 0.0) -> Verdict:
    try:
        ref = load_picture(reference)
    except PictureError as exc:
        if not Path(reference).exists():
            return Verdict(SKIP, "no-reference")
        return Verdict(FAIL, f"unreadable-reference ({exc})")
    try:
        got = load_picture(frame)
    except PictureError as exc:
        return Verdict(NOT_RUN, f"unreadable-frame ({exc})")
    return judge(compare_frames(ref, got, threshold), max_percent)


# ---- accept ---------------------------------------------------------------------------------

def _inside_checkout(path: Path) -> bool:
    path = Path(os.path.abspath(path))
    try:
        rel = path.relative_to(ROOT)
    except ValueError:
        return False
    return not (rel.parts and rel.parts[0] == "build")


def _head_commit() -> str:
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                             capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    return out.stdout.strip() if out.returncode == 0 and out.stdout.strip() else "unknown"


def sidecar_path(reference: Path) -> Path:
    return Path(str(reference) + ".json")


def accept(frame: Path, reference: Path, commit: str | None = None) -> dict:
    """Store ``frame`` as the reference. Only an explicit command ever replaces one."""
    reference = Path(reference)
    if _inside_checkout(reference):
        raise PictureError("reference path is inside this checkout; references are game "
                           "pixels and stay in the title's private folder")
    got = load_picture(frame)
    if reference.suffix.lower() != ".ppm":
        raise PictureError("reference must be a .ppm file")
    reference.parent.mkdir(parents=True, exist_ok=True)
    data = Path(frame).read_bytes()
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        data = b"P6\n%d %d\n255\n" % (got.width, got.height) + got.pixels
    tmp = reference.with_name(reference.name + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, reference)
    meta = {"commit": commit or _head_commit(), "width": got.width, "height": got.height,
            "sha256": hashlib.sha256(data).hexdigest()}
    sidecar_path(reference).write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    return meta


# ---- running a route ------------------------------------------------------------------------

def sandbox_env(base: dict, extra: dict, vblank: int | None, window: int,
                pad: Path | None) -> dict:
    env = {k: v for k, v in base.items() if not _STRIPPED_ENV.match(k)}
    env.update(extra)
    env.update({
        "SDL_VIDEO_DRIVER": "dummy", "SDL_VIDEODRIVER": "dummy",
        "SDL_AUDIO_DRIVER": "dummy", "SDL_AUDIODRIVER": "dummy",
        "SR_VIDEO": "offscreen",
    })
    if vblank is not None:
        env["SR_FBSNAP_WINDOWS"] = f"{vblank}-{vblank + window - 1}"
        env["SR_EXIT_AT_VBLANK"] = str(vblank + window)
    else:
        env["SR_FIRST_FRAME_DUMP"] = "1"
    if pad is not None:
        env["SR_PADSCRIPT"] = str(pad)
    return env


def collect_frame(run_dir: Path, vblank: int | None) -> tuple[Path | None, str]:
    """The earliest published frame of the window, or why there is none."""
    if vblank is None:
        first = run_dir / "frame_first.ppm"
        return (first, "first-frame") if first.is_file() else (None, "no-frame-captured")
    found = []
    # The capture service publishes under build/snapshots/ (it creates the folder); a runtime
    # built before that wrote the frame beside its working directory, so read both.
    for folder in (run_dir / "build" / "snapshots", run_dir):
        if folder.is_dir():
            for entry in folder.iterdir():
                match = _FRAME_V.match(entry.name)
                if match:
                    found.append((int(match.group(1)), entry))
        if found:
            break
    if not found:
        return None, "no-frame-captured"
    found.sort()
    return found[0][1], f"vblank={found[0][0]}"


def capture_failure(how: str) -> Verdict:
    """Verdict for a run that produced no frame.

    A runtime that started and then exited nonzero or hit the timeout is a regression
    signal, so it is a FAIL (exit 1). NOT_RUN is for a run that could not start or that
    published nothing (no runtime, no pad script, no frame).
    """
    if how.startswith(("runtime-exit-", "timeout-")):
        return Verdict(FAIL, how)
    return Verdict(NOT_RUN, how)


def run_capture(command: list[str], env: dict, run_dir: Path,
                vblank: int | None, timeout: int,
                mkdirs: tuple[str, ...] = ()) -> tuple[Path | None, str]:
    """Run once in a fresh scratch folder; return the frame file or a NOT_RUN reason.

    ``{run}`` in the command and in env values becomes the scratch folder, so a route can
    point a memory stick or file-system root at a folder that is new for every run; ``mkdirs``
    lists the folders (relative to it) to create first.
    """
    if run_dir.exists():
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True)
    for name in mkdirs:
        (run_dir / name).mkdir(parents=True, exist_ok=True)
    here = str(run_dir)
    command = [c.replace("{run}", here) for c in command]
    env = {k: v.replace("{run}", here) for k, v in env.items()}
    # The runtime writes build/snapshots/ and frame_first.ppm relative to its working
    # directory, so the run happens in the scratch folder.
    try:
        done = subprocess.run(command, cwd=run_dir, env=env, stdin=subprocess.DEVNULL,
                              capture_output=True, timeout=timeout)
    except FileNotFoundError:
        return None, "runtime-not-found"
    except subprocess.TimeoutExpired:
        return None, f"timeout-{timeout}s"
    (run_dir / "stdout.log").write_bytes(done.stdout)
    (run_dir / "stderr.log").write_bytes(done.stderr)
    if done.returncode != 0:
        return None, f"runtime-exit-{done.returncode}"
    frame, how = collect_frame(run_dir, vblank)
    return frame, how


def run_route(route_path: Path, runs: int = 1, work_root: Path | None = None,
              accept_frame: bool = False) -> tuple[str, Verdict]:
    """Capture and compare one title route. Returns (title, verdict).

    ``accept_frame`` stores the captured frame as the reference instead of comparing; it
    captures at least ``MIN_ACCEPT_RUNS`` times and requires every run to agree, so a
    nondeterministic route is never accepted.
    """
    route_path = Path(route_path)
    title = route_path.stem
    try:
        route = json.loads(route_path.read_text(encoding="utf-8"))
    except OSError as exc:
        return title, Verdict(NOT_RUN, f"no-route ({exc.strerror or exc})")
    except ValueError:
        return title, Verdict(NOT_RUN, "route-not-json")
    if not isinstance(route, dict):
        return title, Verdict(NOT_RUN, "route-not-an-object")
    title = str(route.get("title") or title)
    base = route_path.resolve().parent

    def rel(value: str) -> Path:
        p = Path(value)
        return p if p.is_absolute() else base / p

    command = route.get("command")
    if (not isinstance(command, list) or not command
            or not all(isinstance(c, str) for c in command)):
        return title, Verdict(NOT_RUN, "route-has-no-command")
    command = [str(rel(command[0])) if (os.sep in command[0] or "/" in command[0])
               else command[0], *command[1:]]
    if not isinstance(route.get("reference"), str):
        return title, Verdict(NOT_RUN, "route-has-no-reference-path")
    reference = rel(route["reference"])
    pad = rel(route["pad"]) if route.get("pad") else None
    if pad is not None and not pad.is_file():
        return title, Verdict(NOT_RUN, "pad-script-missing")
    vblank = route.get("vblank")
    if vblank is not None and (not isinstance(vblank, int) or isinstance(vblank, bool)
                               or vblank < 1):
        return title, Verdict(NOT_RUN, "route-vblank-invalid")
    window = route.get("window", DEFAULT_WINDOW)
    if not isinstance(window, int) or isinstance(window, bool) or window < 1:
        return title, Verdict(NOT_RUN, "route-window-invalid")
    extra = route.get("env", {})
    if not isinstance(extra, dict) or not all(isinstance(v, str) for v in extra.values()):
        return title, Verdict(NOT_RUN, "route-env-invalid")
    mkdirs = route.get("mkdirs", [])
    if (not isinstance(mkdirs, list) or not all(isinstance(m, str) for m in mkdirs)
            or any(Path(m).anchor or m.startswith(("/", "\\")) or ".." in Path(m).parts for m in mkdirs)):
        return title, Verdict(NOT_RUN, "route-mkdirs-invalid")
    mkdirs = tuple(mkdirs)
    threshold, max_percent = route.get("threshold", 0), route.get("max_percent", 0.0)
    timeout = route.get("timeout", DEFAULT_TIMEOUT_SECONDS)
    if (not isinstance(threshold, int) or isinstance(threshold, bool)
            or not 0 <= threshold <= MAX_RGB_DISTANCE):
        return title, Verdict(NOT_RUN, "route-threshold-invalid")
    if (not isinstance(max_percent, (int, float)) or isinstance(max_percent, bool)
            or not 0 <= max_percent <= 100):
        return title, Verdict(NOT_RUN, "route-max_percent-invalid")
    if not isinstance(timeout, int) or isinstance(timeout, bool) or timeout < 1:
        return title, Verdict(NOT_RUN, "route-timeout-invalid")

    own_tmp = work_root is None
    work = Path(tempfile.mkdtemp(prefix="picture-gate-")) if own_tmp else Path(work_root)
    try:
        env = sandbox_env(os.environ, extra, vblank, window, pad)
        frames, how = [], ""
        if accept_frame:
            runs = max(runs, MIN_ACCEPT_RUNS)  # a reference is never taken from one run
        for n in range(max(1, runs)):
            frame, how = run_capture(command, env, work / f"run{n}", vblank, timeout, mkdirs)
            if frame is None:
                return title, capture_failure(how)
            frames.append(frame)
        if len(frames) > 1:
            first = frames[0].read_bytes()
            for other in frames[1:]:
                if other.read_bytes() != first:
                    try:
                        pct = judge(compare_frames(parse_ppm(first),
                                                   parse_ppm(other.read_bytes())), 0.0)
                    except PictureError as exc:
                        return title, Verdict(FAIL, f"nondeterministic unreadable-frame ({exc})")
                    return title, Verdict(FAIL, f"nondeterministic {pct.detail}", pct.percent,
                                          pct.over, pct.differing, pct.total, pct.max_delta)
        if accept_frame:
            try:
                meta = accept(frames[0], reference)
            except PictureError as exc:
                return title, Verdict(NOT_RUN, f"accept-refused ({exc})")
            return title, Verdict(ACCEPTED, f"{meta['width']}x{meta['height']} "
                                            f"commit={meta['commit']} {how}")
        verdict = compare_files(reference, frames[0], threshold, max_percent)
        if verdict.status in (PASS, FAIL):
            verdict = Verdict(verdict.status, f"{verdict.detail} {how}", verdict.percent,
                              verdict.over, verdict.differing, verdict.total, verdict.max_delta)
        return title, verdict
    finally:
        if own_tmp:
            shutil.rmtree(work, ignore_errors=True)


# ---- display-smoke: the public proof --------------------------------------------------------

def _load_display_smoke():
    path = ROOT / "fixtures" / "display_smoke" / "generate.py"
    spec = importlib.util.spec_from_file_location("display_smoke_generate", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def display_smoke_command(build_dir: Path) -> list[str]:
    gen = _load_display_smoke()
    manifest = gen._read_manifest(build_dir / "fixture")
    expected = gen.final_frame_first_pixel(int(manifest["frames"]))
    exe = (build_dir / f"{gen.ARTIFACT_STEM}.exe").resolve()
    if not exe.exists():
        exe = (build_dir / gen.ARTIFACT_STEM).resolve()
    image = (build_dir / f"{gen.ARTIFACT_STEM}_image.bin").resolve()
    return [str(exe), "--image", str(image), f"0x{gen.BASE:08x}", f"0x{gen.ENTRY:08x}",
            "none", "none", "--sched", "--gui",
            f"--expect-u32=0x{gen.FRAMEBUFFER:08x}:0x{expected:08x}"]


def perturb_one_pixel(frame: Frame) -> Frame:
    """The reference with its first pixel's channels inverted."""
    px = bytearray(frame.pixels)
    for c in range(3):
        px[c] ^= 0xFF
    return Frame(frame.width, frame.height, bytes(px))


def write_ppm(path: Path, frame: Frame) -> None:
    path.write_bytes(b"P6\n%d %d\n255\n" % (frame.width, frame.height) + frame.pixels)


WORK_MARKER = ".picture-gate-work"


def claim_work_dir(work: Path) -> bool:
    """Make ``work`` an empty folder this gate owns. Never deletes what it did not create.

    A folder with the marker file was made by an earlier run and is cleared; a missing or
    empty folder is taken; any other non-empty folder is refused untouched.
    """
    work = Path(work)
    if work.exists():
        if not work.is_dir():
            return False
        if (work / WORK_MARKER).is_file():
            for child in work.iterdir():
                if child.is_dir() and not child.is_symlink():
                    shutil.rmtree(child)
                else:
                    child.unlink()
        elif any(work.iterdir()):
            return False
    work.mkdir(parents=True, exist_ok=True)
    (work / WORK_MARKER).write_text("created by tools/picture_gate.py display-smoke\n",
                                    encoding="utf-8")
    return True


def display_smoke(build_dir: Path, work_dir: Path | None = None) -> int:
    """Headless twice: SKIP with no reference, PASS 0.00% after accept, FAIL after one pixel."""
    build_dir = Path(build_dir).resolve()
    work = Path(work_dir).resolve() if work_dir else build_dir / "picture-gate"
    if not _inside_build(work):
        print(f"display-smoke picture gate: work dir {work} must be under build/", file=sys.stderr)
        return EXIT_USAGE
    if not claim_work_dir(work):
        print(f"display-smoke picture gate: {work} is not empty and was not created by this "
              "gate; give an empty or new folder", file=sys.stderr)
        return EXIT_USAGE
    try:
        command = display_smoke_command(build_dir)
    except (OSError, KeyError, ValueError) as exc:
        print(f"PICTURE_GATE display-smoke: NOT_RUN fixture-not-built ({exc})")
        return EXIT_NOT_PROVEN
    if not Path(command[0]).exists():
        print("PICTURE_GATE display-smoke: NOT_RUN fixture-not-built (run display-smoke first)")
        return EXIT_NOT_PROVEN
    env = sandbox_env(os.environ, {"SR_PRESENT_TRACE": "1"}, None, 1, None)
    reference = work / "reference" / "display-smoke.ppm"
    results = []

    def step(label: str, verdict: Verdict, want: str) -> None:
        print(f"{verdict.line('display-smoke')}  [{label}]")
        results.append(verdict.status == want)

    # 1. No reference: SKIP, with the frame itself not required.
    first, how = run_capture(command, env, work / "run1", None, DEFAULT_TIMEOUT_SECONDS)
    if first is None:
        failed = capture_failure(how)
        print(failed.line("display-smoke"))
        return _exit_for(failed, True)
    step("no reference", compare_files(reference, first), SKIP)
    # 2. accept, then a second run must match the reference exactly.
    accept(first, work / "reference" / "display-smoke.ppm", commit="display-smoke")
    second, how = run_capture(command, env, work / "run2", None, DEFAULT_TIMEOUT_SECONDS)
    if second is None:
        failed = capture_failure(how)
        print(failed.line("display-smoke"))
        return _exit_for(failed, True)
    exact = compare_files(reference, second)
    step("second run against accepted reference", exact, PASS)
    results.append(exact.percent == 0 and exact.differing == 0)
    # 3. A one-pixel change to the reference is reported, not tolerated.
    bad = work / "reference" / "display-smoke-one-pixel.ppm"
    write_ppm(bad, perturb_one_pixel(load_picture(reference)))
    changed = compare_files(bad, second)
    step("one-pixel change", changed, FAIL)
    results.append(changed.differing == 1 and changed.over == 1)
    return EXIT_OK if all(results) else EXIT_FAIL


def _inside_build(path: Path) -> bool:
    try:
        rel = Path(os.path.abspath(path)).relative_to(ROOT)
    except ValueError:
        return True  # outside the checkout: nothing a commit could pick up
    return bool(rel.parts) and rel.parts[0] == "build"


# ---- command line ---------------------------------------------------------------------------

def _exit_for(verdict: Verdict, strict: bool) -> int:
    if verdict.status == FAIL:
        return EXIT_FAIL
    if verdict.status in (SKIP, NOT_RUN):
        return EXIT_NOT_PROVEN if strict else EXIT_OK
    return EXIT_OK


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("compare", help="compare a frame with a reference picture")
    p.add_argument("reference", type=Path)
    p.add_argument("frame", type=Path)
    p.add_argument("--threshold", type=int, default=0,
                   help="RGB distance a pixel may differ by and still count as equal (default 0)")
    p.add_argument("--max-percent", type=float, default=0.0,
                   help="share of pixels over the threshold still accepted (default 0)")
    p.add_argument("--strict", action="store_true", help="SKIP and NOT_RUN exit 3")
    p = sub.add_parser("accept", help="store a frame as the title's reference")
    p.add_argument("frame", type=Path)
    p.add_argument("reference", type=Path)
    p.add_argument("--commit", help="commit the frame came from (default: this checkout's HEAD)")
    p = sub.add_parser("check", help="run a private title route and compare its frame")
    p.add_argument("route", type=Path)
    p.add_argument("--runs", type=int, default=1,
                   help="capture N times; runs that differ are a FAIL (nondeterministic)")
    p.add_argument("--accept", action="store_true",
                   help="store the captured frame as the title's reference (explicit step)")
    p.add_argument("--strict", action="store_true", help="SKIP and NOT_RUN exit 3")
    p = sub.add_parser("display-smoke", help="the public synthetic proof (no game needed)")
    p.add_argument("--build-dir", type=Path, required=True)
    p.add_argument("--work-dir", type=Path)
    args = ap.parse_args(argv)

    if args.cmd == "compare":
        if args.threshold < 0 or not 0 <= args.max_percent <= 100:
            ap.error("--threshold must be >= 0 and --max-percent within 0..100")
        verdict = compare_files(args.reference, args.frame, args.threshold, args.max_percent)
        print(verdict.line(args.frame.stem))
        return _exit_for(verdict, args.strict)
    if args.cmd == "accept":
        try:
            meta = accept(args.frame, args.reference, args.commit)
        except PictureError as exc:
            print(f"PICTURE_GATE accept: refused ({exc})", file=sys.stderr)
            return EXIT_FAIL
        print(f"PICTURE_GATE accept: stored {meta['width']}x{meta['height']} "
              f"commit={meta['commit']}")
        return EXIT_OK
    if args.cmd == "check":
        if args.runs < 1:
            ap.error("--runs must be >= 1")
        title, verdict = run_route(args.route, args.runs, accept_frame=args.accept)
        print(verdict.line(title))
        return _exit_for(verdict, args.strict)
    return display_smoke(args.build_dir, args.work_dir)


if __name__ == "__main__":
    sys.exit(main())
