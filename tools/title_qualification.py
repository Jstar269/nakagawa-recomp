#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Qualify public, source-owned titles: validate candidates and run the launch smoke.

``validate`` checks checked-in public title manifests, public input profiles and
the bring-up reports the smoke writes. Manifests go through ``tools/title_manifest.py``,
the normative validator that ``assets/title_manifest.schema.json`` documents. Input
profiles are checked against the vocabulary the runtime parses in
``src/core/nk_input_profile.c``. Nothing is built or launched.

``smoke`` launches a public synthetic title through the native player. The user
data root and LOCALAPPDATA live in a disposable sandbox, the public input profile
is staged into that sandbox, and the run writes a bring-up-schema report
(``assets/bringup_report.schema.json``). The report uses the bring-up stage names,
failure classes and presentation evidence, so ``tools/library_sweep.py`` reads a
public run with the same vocabulary it uses for a retail run.

Scope: public titles only. A retail manifest, or a manifest that declares a disc
image, is refused. A candidate that cannot be launched at all (no native
player, no public launch surface) is a refusal with exit status 2 and writes no
report. A missing staged build, or a title that launches and fails, writes a
schema-valid report naming the failed stage, and exits 1.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from typing import NoReturn

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import nk_cli  # noqa: E402
import title_manifest  # noqa: E402

MANIFEST_SCHEMA = ROOT / "assets" / "title_manifest.schema.json"
BRINGUP_SCHEMA = ROOT / "assets" / "bringup_report.schema.json"
TITLE_DIR = ROOT / "assets" / "titles"
INPUT_PROFILE_DIR = ROOT / "assets" / "input_profiles"
DEFAULT_INPUT_PROFILE = INPUT_PROFILE_DIR / "standard-gamepad.json"
PLAYER_NAME = "nakagawa_player.exe" if os.name == "nt" else "nakagawa_player"
REPORT_NAME = "bringup-report.json"
OUTPUT_LOG_NAME = "player-output.log"
SMOKE_TIMEOUT_SECONDS = 150
MAX_PUBLIC_JSON_BYTES = 256 * 1024
# Public titles only. Retail manifests are local-only and never checked in.
PUBLIC_KINDS = frozenset({"synthetic", "homebrew"})
# Bring-up issue numbers that the bring-up route attaches to a launch failure.
# Maintenance: edit by hand; each number must be in the bring-up schema's issue_numbers enum (tested).
LAUNCH_FAILURE_ISSUES = [308]
# A public title is launchable only through a sample entry the native player
# already bundles. Titles without an entry are refused by name, never guessed.
# Maintenance rule (docs/TITLE_TESTING.md, "Launch surfaces"): add the bundled player entry
# first, then one line here, then a test that runs it. Do not discover entries at runtime.
PUBLIC_LAUNCH_SURFACES = {
    "display-smoke-v1": {"launch_index": 1},
}

# Input-profile vocabulary, mirrored from src/core/nk_input_profile.c (the tables
# kPspButtons, kPspAxes, kHostButtons, kHostAxes and kNavActions). The test suite
# parses the C tables and fails if these tuples drift.
PSP_CONTROLS = (
    "select", "start", "up", "right", "down", "left", "ltrigger", "rtrigger",
    "triangle", "circle", "cross", "square", "home", "hold",
)
PSP_AXES = ("analog_x", "analog_y")
HOST_BUTTONS = (
    "south", "east", "west", "north", "back", "guide", "start", "left_stick",
    "right_stick", "left_shoulder", "right_shoulder", "dpad_up", "dpad_down",
    "dpad_left", "dpad_right", "misc1", "right_paddle1", "left_paddle1",
    "right_paddle2", "left_paddle2", "touchpad",
)
HOST_AXES = ("leftx", "lefty", "rightx", "righty", "left_trigger", "right_trigger")
TRIGGER_AXES = ("left_trigger", "right_trigger")
NAV_ACTIONS = (
    "confirm", "cancel", "up", "down", "left", "right", "page_prev", "page_next", "menu",
)
INPUT_PROFILE_SCHEMA_VERSION = 2
INPUT_GUID_MAX_LENGTH = 64
INPUT_NAME_HINT_MAX_LENGTH = 128
INT16_MIN = -32768
INT16_MAX = 32767
INPUT_PROFILE_KEYS = frozenset({
    "schema_version", "device", "calibration", "psp_bindings", "navigation_bindings",
})
AXIS_KEYS = frozenset({"host_axis", "deadzone_inner", "deadzone_outer", "inverted", "rest", "min_val", "max_val"})
CALIBRATION_KEYS = frozenset({"trigger_threshold", "trigger_rest", "trigger_extreme", *PSP_AXES})


class QualificationError(ValueError):
    """A named refusal: the candidate is invalid, or the route cannot run it."""


def fail(path: str, message: str) -> NoReturn:
    raise QualificationError(f"{path}: {message}")


def load_public_json(path: Path) -> object:
    """Read one public JSON document with the manifest parser's size and duplicate-key rules."""
    if path.is_symlink():
        raise QualificationError(f"{path}: symbolic links are not accepted")
    if not path.is_file():
        raise QualificationError(f"{path}: not a regular file")
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise QualificationError(f"{path}: unable to read: {exc}") from exc
    if len(raw) > MAX_PUBLIC_JSON_BYTES:
        raise QualificationError(f"{path}: exceeds the {MAX_PUBLIC_JSON_BYTES}-byte limit")
    try:
        return title_manifest.loads_manifest(raw.decode("utf-8"))
    except (title_manifest.TitleManifestError, UnicodeError) as exc:
        raise QualificationError(f"{path}: {exc}") from exc


# -----------------------------------------------------------------------------
# Manifests and staging
# -----------------------------------------------------------------------------

def manifest_schema_problems() -> list[str]:
    """The portable schema and the normative validator must agree on the root contract."""
    schema = load_public_json(MANIFEST_SCHEMA)
    problems = []
    if set(schema.get("properties", {})) != set(title_manifest.ROOT_KEYS):
        problems.append("title_manifest.schema.json properties differ from title_manifest.ROOT_KEYS")
    if not set(schema.get("required", [])) <= set(title_manifest.ROOT_KEYS):
        problems.append("title_manifest.schema.json requires a key the validator does not accept")
    kinds = set(schema.get("properties", {}).get("kind", {}).get("enum", []))
    if not PUBLIC_KINDS <= kinds:
        problems.append("title_manifest.schema.json does not admit every public manifest kind")
    return problems


def load_public_manifest(path: Path) -> dict:
    """Validate one candidate manifest with the normative validator and refuse non-public kinds."""
    try:
        manifest = title_manifest.validate_manifest(title_manifest.load_manifest(path))
    except (title_manifest.TitleManifestError, OSError, ValueError) as exc:
        raise QualificationError(f"{path}: {exc}") from exc
    if manifest["kind"] not in PUBLIC_KINDS:
        raise QualificationError(
            f"{path}: kind {manifest['kind']!r} is local-only; the public route "
            "qualifies synthetic or homebrew titles")
    return manifest


def declared_path_problems(manifest: dict) -> list[str]:
    """Declarations the public route cannot honour, independent of any build output."""
    problems = []
    filesystem = manifest["filesystem"]
    if "disc_image" in filesystem:
        problems.append("filesystem.disc_image is local-only; the public route takes no disc image")
    modules = manifest.get("modules", [])
    if any(module["role"] == "guest-prx" for module in modules) and "module_dir" not in filesystem:
        problems.append("a guest-prx module is declared without filesystem.module_dir")
    return problems


def staged_executable(build_root: Path, title_id: str) -> Path:
    """The path nk_launch.c probes for a launch: build/<id>/<id>.exe (Windows) or build/<id>/<id>."""
    base = build_root / title_id / title_id
    candidates = [base.with_name(base.name + ".exe"), base] if os.name == "nt" else [base]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return candidates[0]


def staging_problems(manifest: dict, *, runtime_root: Path, build_root: Path, require_build: bool) -> list[str]:
    """Check declared inputs exist inside the checkout, and the staged build when required."""
    problems = declared_path_problems(manifest)
    filesystem = manifest["filesystem"]
    for key in ("data_root", "module_dir", "psp_header", "executable"):
        value = filesystem.get(key)
        if not value:
            continue
        target = runtime_root / value
        if key == "psp_header" or key == "executable":
            if require_build and not target.is_file():
                problems.append(f"filesystem.{key} {value!r} is missing")
        elif require_build and not target.is_dir():
            problems.append(f"filesystem.{key} {value!r} is not a directory in the checkout")
    if require_build:
        executable = staged_executable(build_root, manifest["id"])
        if not executable.is_file():
            problems.append(
                f"not built: {manifest['id']}/{executable.name} is missing from the build root "
                f"(run the title's build target first)")
    return problems


# -----------------------------------------------------------------------------
# Input profiles
# -----------------------------------------------------------------------------

def _object(value: object, path: str) -> dict:
    if not isinstance(value, dict):
        fail(path, "must be an object")
    return value


def _list(value: object, path: str) -> list:
    if not isinstance(value, list):
        fail(path, "must be an array")
    return value


def _only_keys(mapping: dict, path: str, allowed: frozenset[str] | set[str]) -> None:
    unknown = sorted(key for key in mapping if key not in allowed)
    if unknown:
        fail(f"{path}.{unknown[0]}", "is not a field of the public input profile")


def _int_in(value: object, path: str, low: int, high: int) -> int:
    if type(value) is not int or not low <= value <= high:
        fail(path, f"must be an integer in [{low}, {high}]")
    return value


def _binding_source(value: object, path: str) -> None:
    """Accept the grammar parse_binding_source_str accepts in the runtime."""
    if not isinstance(value, str) or not value:
        fail(path, 'must be a non-empty string; write "none" to leave a control unbound')
    if value.casefold() == "none":
        return
    if value.startswith("button:"):
        if value[len("button:"):].casefold() not in HOST_BUTTONS:
            fail(path, f"unknown host button {value[len('button:'):]!r}")
        return
    if value.startswith("trigger:"):
        if value[len("trigger:"):].casefold() not in HOST_AXES:
            fail(path, f"unknown host axis {value[len('trigger:'):]!r}")
        return
    if value[0] in "+-":
        if value[1:].casefold() not in HOST_AXES:
            fail(path, f"unknown host axis {value[1:]!r}")
        return
    if value.casefold() in HOST_BUTTONS:
        return
    if value.casefold() in TRIGGER_AXES:
        return
    fail(path, f"unrecognized host input {value!r}")


def _binding_entry(entry: dict, path: str) -> dict[str, str]:
    if "primary" not in entry and "secondary" not in entry:
        fail(path, 'names neither "primary" nor "secondary"; write "none" to leave a control unbound')
    sources: dict[str, str] = {}
    for key in ("primary", "secondary"):
        if key in entry:
            _binding_source(entry[key], f"{path}.{key}")
            sources[key] = entry[key]
    return sources


def _bindings(value: object, path: str, key: str, names: tuple[str, ...]) -> dict[str, dict[str, str]]:
    """Every name appears exactly once, so an unbound control is written as "none", never omitted."""
    seen: dict[str, dict[str, str]] = {}
    for index, raw in enumerate(_list(value, path)):
        item = f"{path}[{index}]"
        entry = _object(raw, item)
        _only_keys(entry, item, {key, "primary", "secondary"})
        name = entry.get(key)
        if not isinstance(name, str) or name not in names:
            fail(f"{item}.{key}", f"unknown name {name!r}")
        if name in seen:
            fail(f"{item}.{key}", f"duplicate binding for {name!r}")
        seen[name] = _binding_entry(entry, item)
    missing = [name for name in names if name not in seen]
    if missing:
        fail(path, f"no entry for {missing[0]!r}; write \"none\" to leave a control unbound")
    return seen


def _axis(value: object, path: str) -> None:
    axis = _object(value, path)
    _only_keys(axis, path, AXIS_KEYS)
    if axis.get("host_axis") not in HOST_AXES:
        fail(f"{path}.host_axis", f"must be one of {', '.join(HOST_AXES)}")
    _int_in(axis.get("deadzone_inner"), f"{path}.deadzone_inner", 0, 32767)
    _int_in(axis.get("deadzone_outer"), f"{path}.deadzone_outer", 0, 32767)
    if axis["deadzone_inner"] + axis["deadzone_outer"] >= 32767:
        fail(path, "combined deadzones must be below 32767")
    if type(axis.get("inverted")) is not bool:
        fail(f"{path}.inverted", "must be a boolean")
    for key in ("rest", "min_val", "max_val"):
        _int_in(axis.get(key), f"{path}.{key}", INT16_MIN, INT16_MAX)


def validate_input_profile(document: object) -> dict:
    """Validate one public input profile (schema 2, global mapping only) and return it."""
    root = _object(document, "$")
    _only_keys(root, "$", INPUT_PROFILE_KEYS)
    if type(root.get("schema_version")) is not int or root["schema_version"] != INPUT_PROFILE_SCHEMA_VERSION:
        fail("$.schema_version", f"public input profiles use schema_version {INPUT_PROFILE_SCHEMA_VERSION}")
    device = _object(root.get("device"), "$.device")
    _only_keys(device, "$.device", {"guid", "name_hint"})
    guid = device.get("guid")
    if not isinstance(guid, str) or not guid or len(guid) > INPUT_GUID_MAX_LENGTH:
        fail("$.device.guid", f"must be a non-empty string of at most {INPUT_GUID_MAX_LENGTH} characters")
    if "name_hint" in device and (
        not isinstance(device["name_hint"], str) or len(device["name_hint"]) > INPUT_NAME_HINT_MAX_LENGTH
    ):
        fail("$.device.name_hint", f"must be a string of at most {INPUT_NAME_HINT_MAX_LENGTH} characters")
    calibration = _object(root.get("calibration"), "$.calibration")
    _only_keys(calibration, "$.calibration", CALIBRATION_KEYS)
    _int_in(calibration.get("trigger_threshold"), "$.calibration.trigger_threshold", 0, 32767)
    for key in ("trigger_rest", "trigger_extreme"):
        if key in calibration:
            _int_in(calibration[key], f"$.calibration.{key}", INT16_MIN, INT16_MAX)
    for axis in PSP_AXES:
        if axis not in calibration:
            fail(f"$.calibration.{axis}", "is required")
        _axis(calibration[axis], f"$.calibration.{axis}")
    psp = _bindings(root.get("psp_bindings"), "$.psp_bindings", "control", PSP_CONTROLS)
    _reject_shared_host_sources(psp)
    _bindings(root.get("navigation_bindings"), "$.navigation_bindings", "action", NAV_ACTIONS)
    return root


def _reject_shared_host_sources(psp: dict[str, dict[str, str]]) -> None:
    """A host input may drive one PSP control. Two owners would be a conflict the runtime refuses."""
    owners: dict[str, str] = {}
    for control, sources in psp.items():
        for source in sources.values():
            if source.casefold() == "none":
                continue
            if source in owners and owners[source] != control:
                fail("$.psp_bindings", f"{source!r} is bound to both {owners[source]!r} and {control!r}")
            owners[source] = control


# -----------------------------------------------------------------------------
# Bring-up report
# -----------------------------------------------------------------------------

_SPAWNED_RUNTIME = re.compile(r"^\[PLAYER\] Spawning runtime: (.+?)(?: \(ISO: .*\))?\s*$", re.MULTILINE)
_PLAY_NOW = re.compile(r"PLAY NOW available for \S+ \((.+?)\)\.?\s*$", re.MULTILINE)
_FIRST_FRAME = re.compile(
    r"^BOOT_EVENT phase=first_frame source=(\S+) nonzero_pixels=(\d+) total_pixels=(\d+)\s*$",
    re.MULTILINE,
)


def _spawned_runtime_title(output: str) -> str | None:
    """The staged title id the player spawned, taken from build/<id>/<id>[.exe]."""
    match = _SPAWNED_RUNTIME.search(output)
    if not match:
        return None
    parts = [part for part in re.split(r"[\\/]", match.group(1).strip()) if part]
    if len(parts) < 2:
        return None
    stem = parts[-1][:-4] if parts[-1].casefold().endswith(".exe") else parts[-1]
    return parts[-2] if parts[-2] == stem else None


def _first_checkpoint(output: str) -> dict | None:
    """The first presented frame the player captured, if it holds visible pixels."""
    match = _FIRST_FRAME.search(output)
    if not match:
        return None
    nonzero, total = int(match.group(2)), int(match.group(3))
    if total <= 0 or nonzero <= 0:
        return None
    return {"source": match.group(1), "nonzero_pixels": nonzero, "total_pixels": total}


def _stage_fail(report: dict, stage: str, failure: str, duration_ms: int) -> None:
    report["stages"][stage] = {"status": "FAIL", "duration_ms": duration_ms}
    report["failure_class"] = failure
    report["issue_numbers"] = list(LAUNCH_FAILURE_ISSUES)


def staging_failure_report(duration_ms: int = 0) -> dict:
    """A report for a candidate whose build is absent: the compile stage names the missing entry."""
    report = nk_cli.new_bringup_report()
    report["reached_stage"] = "compile"
    report["stages"]["compile"] = {"status": "FAIL", "duration_ms": duration_ms}
    report["failure_class"] = "ENTRY_NOT_COMPILED"
    report["issue_numbers"] = list(LAUNCH_FAILURE_ISSUES)
    report["runtime_output_kind"] = "ENTRY_NOT_COMPILED"
    report["exit_classification"] = "NOT_RUN"
    return report


def launch_report(
    *,
    title: dict,
    output: str,
    returncode: int | None,
    timed_out: bool,
    duration_ms: int,
) -> dict:
    """Derive the bring-up-schema report for one launch from the player's output.

    The launch stage passes only when the spawned runtime is the staged title, the
    player advertised the title's display name, the offscreen presenter opened, the
    first presented frame carries visible pixels, presentation evidence is ordered,
    and the child exited 0. Every other outcome names one failure class from the
    bring-up vocabulary.
    """
    report = nk_cli.new_bringup_report()
    report["reached_stage"] = "launch"
    report["runtime_output_kind"] = nk_cli.runtime_output_kind(output, [])
    presentation_ok = nk_cli.set_bringup_presentation(report, output)
    if timed_out:
        report["process_exit_code"] = None
        report["exit_classification"] = "TIMED_OUT"
        report["stages"]["launch"] = {"status": "TIMED_OUT", "duration_ms": duration_ms}
        report["failure_class"] = "LAUNCH_TIMEOUT"
        report["issue_numbers"] = list(LAUNCH_FAILURE_ISSUES)
        return report
    report["process_exit_code"] = returncode
    report["exit_classification"] = "EXITED_ZERO" if returncode == 0 else "EXITED_NONZERO"
    if returncode != 0:
        kind = report["runtime_output_kind"]
        failure = {
            "NATIVE_CRASH_REPORT": "NATIVE_RUNTIME_CRASH",
            "ENTRY_NOT_COMPILED": "ENTRY_NOT_COMPILED",
            "VIDEO_UNAVAILABLE": "HEADLESS_UNAVAILABLE",
        }.get(kind, "LAUNCH_FAILED")
        _stage_fail(report, "launch", failure, duration_ms)
        return report
    spawned = _spawned_runtime_title(output)
    play_now = _PLAY_NOW.search(output)
    if spawned != title["id"] or not play_now or play_now.group(1) != title["display_name"]:
        _stage_fail(report, "launch", "LAUNCH_FAILED", duration_ms)
        return report
    if "BOOT_EVENT phase=window_ready backend=offscreen" not in output or _first_checkpoint(output) is None:
        _stage_fail(report, "launch", "DISPLAY_PROGRESS_UNVERIFIED", duration_ms)
        return report
    if not presentation_ok:
        _stage_fail(report, "launch", "NO_FRAME_SUBMISSIONS", duration_ms)
        return report
    report["stages"]["launch"] = {"status": "PASS", "duration_ms": duration_ms}
    return report


def write_report(report: dict, path: Path) -> None:
    """Refuse to write a report the bring-up schema rejects, then write it atomically."""
    nk_cli.validate_bringup_report(report)
    path.parent.mkdir(parents=True, exist_ok=True)
    nk_cli.write_bringup_file(report, path)


# -----------------------------------------------------------------------------
# Launch
# -----------------------------------------------------------------------------

_SANDBOX_DROP = ("SR_DATAROOT", "SR_FLIGHT", "SR_FLIGHT_OUTPUT", "SR_FLIGHT_RECORDER")


def child_environment(base: dict[str, str], sandbox: Path) -> dict[str, str]:
    """Every user-visible location points into the sandbox, and video/audio run offscreen."""
    env = {key: value for key, value in base.items() if not key.startswith(_SANDBOX_DROP)}
    env.update({
        "LOCALAPPDATA": str(sandbox / "localappdata"),
        "APPDATA": str(sandbox / "appdata"),
        # POSIX players resolve HOME and the XDG_* bases (src/core/nk_platform_posix.c); without
        # these a Linux smoke writes ~/.cache/nakagawa-recomp and reads ~/.config/nakagawa-recomp.
        "HOME": str(sandbox / "home"),
        "XDG_CONFIG_HOME": str(sandbox / "xdg" / "config"),
        "XDG_DATA_HOME": str(sandbox / "xdg" / "data"),
        "XDG_CACHE_HOME": str(sandbox / "xdg" / "cache"),
        "XDG_STATE_HOME": str(sandbox / "xdg" / "state"),
        "SDL_VIDEODRIVER": "dummy",
        "SDL_AUDIODRIVER": "dummy",
        "SR_VIDEO": "offscreen",
        "SR_PRESENT_TRACE": "1",
    })
    return env


def stage_input_profile(sandbox: Path, profile: Path) -> Path:
    """Copy the public input profile to the sandbox config folder the player reads."""
    target = sandbox / "localappdata" / "Nakagawa" / "config" / "input_profile.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(profile, target)
    return target


def launch_command(player: Path, sandbox: Path, runtime_root: Path, launch_index: int) -> list[str]:
    return [
        str(player),
        f"--user-data-root={sandbox / 'user-data'}",
        "--demo",
        f"--runtime-root={runtime_root}",
        f"--launch-index={launch_index}",
    ]


def _stop_process_tree(process: subprocess.Popen) -> None:
    """Stop the player and the runtime it spawned, so no orphan keeps the output pipe open.

    Windows ends the tree with taskkill. POSIX children are started as session leaders
    (run_player), so the player's process group also holds the runtime: SIGKILL that group.
    If the group cannot be found or signalled, the player itself is killed instead.
    """
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                       capture_output=True, check=False)
        return
    try:
        group = os.getpgid(process.pid)
    except OSError:
        group = None
    # Signal only a group this child leads. Never signal the group the runner itself is in.
    if group == process.pid:
        try:
            os.killpg(group, signal.SIGKILL)
            return
        except OSError:
            pass
    process.kill()


def run_player(player: Path, sandbox: Path, runtime_root: Path, launch_index: int,
               timeout_seconds: int) -> tuple[int | None, str, bool, int]:
    """Launch the title through the native player. Returns (exit code, output, timed out, ms)."""
    command = launch_command(player, sandbox, runtime_root, launch_index)
    started = time.perf_counter()
    process = subprocess.Popen(
        command, cwd=ROOT, env=child_environment(os.environ, sandbox),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace",
        # POSIX: the player leads its own session, so _stop_process_tree can signal the whole group.
        start_new_session=os.name != "nt",
    )
    try:
        output, _ = process.communicate(timeout=timeout_seconds)
        timed_out = False
        returncode: int | None = process.returncode
    except subprocess.TimeoutExpired:
        _stop_process_tree(process)
        output, _ = process.communicate()
        timed_out = True
        returncode = None
    except KeyboardInterrupt:
        # On POSIX the child has its own session and misses the terminal's Ctrl+C, so stop the
        # tree here, before the sandbox is deleted underneath a running child.
        _stop_process_tree(process)
        process.wait()
        raise
    duration_ms = int((time.perf_counter() - started) * 1000)
    return returncode, output or "", timed_out, duration_ms


def run_smoke(
    manifest_path: Path,
    *,
    build_root: Path,
    input_profile: Path,
    player: Path,
    timeout_seconds: int = SMOKE_TIMEOUT_SECONDS,
) -> tuple[int, Path | None, dict | None]:
    """Run the public smoke for one manifest. Returns (exit status, report path, report)."""
    manifest = load_public_manifest(manifest_path)
    title_id = manifest["id"]
    surface = PUBLIC_LAUNCH_SURFACES.get(title_id)
    if surface is None:
        raise QualificationError(
            f"{title_id}: no public launch surface; the native player bundles no sample "
            "entry for this title, so the route refuses it rather than guessing")
    validate_input_profile(load_public_json(input_profile))
    problems = declared_path_problems(manifest)
    if problems:
        raise QualificationError(f"{title_id}: {problems[0]}")
    output_dir = build_root / title_id / "title-qualification"
    report_path = output_dir / REPORT_NAME
    if not staged_executable(build_root, title_id).is_file():
        report = staging_failure_report()
        write_report(report, report_path)
        return 1, report_path, report
    if not player.is_file():
        raise QualificationError(f"{player}: native player is not built (run make player)")
    runtime_root = build_root.parent
    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
            prefix="title-qualification-", dir=output_dir, ignore_cleanup_errors=True) as temp:
        sandbox = Path(temp)
        stage_input_profile(sandbox, input_profile)
        returncode, output, timed_out, duration_ms = run_player(
            player, sandbox, runtime_root, surface["launch_index"], timeout_seconds)
    (output_dir / OUTPUT_LOG_NAME).write_text(output, encoding="utf-8")
    report = launch_report(
        title=manifest, output=output, returncode=returncode,
        timed_out=timed_out, duration_ms=duration_ms,
    )
    write_report(report, report_path)
    return (0 if report["failure_class"] == "NONE" else 1), report_path, report


# -----------------------------------------------------------------------------
# Commands
# -----------------------------------------------------------------------------

def _default_manifests() -> list[Path]:
    return sorted(TITLE_DIR.glob("*.json"))


def _default_profiles() -> list[Path]:
    return sorted(INPUT_PROFILE_DIR.glob("*.json")) if INPUT_PROFILE_DIR.is_dir() else []


def cmd_validate(args: argparse.Namespace) -> int:
    build_root = args.build_root
    results: list[tuple[str, list[str]]] = [("manifest schema contract", manifest_schema_problems())]
    for path in args.manifest or _default_manifests():
        try:
            manifest = load_public_manifest(path)
            problems = staging_problems(
                manifest, runtime_root=build_root.parent, build_root=build_root,
                require_build=args.require_build)
        except QualificationError as exc:
            problems = [str(exc)]
        results.append((f"manifest {path.name}", problems))
    for path in args.input_profile or _default_profiles():
        try:
            validate_input_profile(load_public_json(path))
            problems = []
        except QualificationError as exc:
            problems = [str(exc)]
        results.append((f"input profile {path.name}", problems))
    for path in args.report or []:
        try:
            nk_cli.validate_bringup_report(load_public_json(path))
            problems = []
        except (QualificationError, ValueError) as exc:
            problems = [str(exc)]
        results.append((f"report {path.name}", problems))
    failed = 0
    for label, problems in results:
        if problems:
            failed += 1
            print(f"FAIL {label}: {problems[0]}")
            for problem in problems[1:]:
                print(f"     {problem}")
        else:
            print(f"OK   {label}")
    print(f"TITLE_QUALIFICATION_VALIDATE checked={len(results)} failed={failed}")
    return 0 if failed == 0 else 2


def cmd_smoke(args: argparse.Namespace) -> int:
    try:
        status, report_path, report = run_smoke(
            args.manifest,
            build_root=args.build_root,
            input_profile=args.input_profile,
            player=args.player,
            timeout_seconds=args.timeout,
        )
    except QualificationError as exc:
        print(f"TITLE_QUALIFICATION status=REFUSED reason={exc}")
        return 2
    rel = report_path.relative_to(ROOT).as_posix() if report_path.is_relative_to(ROOT) else str(report_path)
    checkpoint = "yes" if report["presentation"]["frame_submissions"] > 0 else "no"
    print(
        f"TITLE_QUALIFICATION status={'PASS' if status == 0 else 'FAIL'} "
        f"reached_stage={report['reached_stage']} failure_class={report['failure_class']} "
        f"frame_submissions={report['presentation']['frame_submissions']} "
        f"first_checkpoint={checkpoint} exit={report['process_exit_code']} report={rel}"
    )
    return status


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--build-root", type=Path, default=ROOT / "build",
                        help="build tree holding build/<title id>/ (default: %(default)s)")
    sub = parser.add_subparsers(dest="command", required=True)

    validate = sub.add_parser(
        "validate", help="validate public manifests, input profiles and reports",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Exit status: 0 when every candidate passes; 2 when at least one candidate\n"
               "fails validation (the first problem of each failing candidate is printed).\n"
               "Nothing is built or launched.")
    validate.add_argument("--manifest", type=Path, action="append",
                          help="manifest to validate (default: every assets/titles/*.json)")
    validate.add_argument("--input-profile", type=Path, action="append",
                          help="input profile to validate (default: assets/input_profiles/*.json)")
    validate.add_argument("--report", type=Path, action="append",
                          help="bring-up-schema report to validate")
    validate.add_argument("--require-build", action="store_true",
                          help="also require the staged build and declared directories to exist")
    validate.set_defaults(func=cmd_validate)

    smoke = sub.add_parser(
        "smoke", help="launch a public title through the native player",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Exit status: 0 when the launch passes; 1 when the title launches and fails, or its\n"
               "staged build is missing (a report naming the failed stage is written); 2 when the\n"
               "candidate is refused before any launch (no report is written).")
    smoke.add_argument("--manifest", type=Path, required=True)
    smoke.add_argument("--input-profile", type=Path, default=DEFAULT_INPUT_PROFILE)
    smoke.add_argument("--player", type=Path, default=ROOT / "build" / PLAYER_NAME)
    smoke.add_argument("--timeout", type=int, default=SMOKE_TIMEOUT_SECONDS,
                       help="seconds before the launch is reported as LAUNCH_TIMEOUT")
    smoke.set_defaults(func=cmd_smoke)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    args.build_root = args.build_root.resolve()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
