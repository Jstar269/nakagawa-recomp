# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Headless SDL regressions for the native player's real event/render loop."""

from __future__ import annotations

import os
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from typing import Sequence
import zlib


ROOT = Path(__file__).resolve().parents[2]
PLAYER_EXE = Path(
    os.environ.get("NAKAGAWA_PLAYER_UI_TEST_EXE", "build/nakagawa_player_ui_test.exe")
)
if not PLAYER_EXE.is_absolute():
    PLAYER_EXE = ROOT / PLAYER_EXE


def read_bmp(path: Path) -> tuple[int, int, int, bytes]:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise AssertionError("SDL did not save a Windows BMP frame")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40:
        raise AssertionError(f"unsupported BMP DIB header size {dib_size}")
    width, signed_height = struct.unpack_from("<ii", data, 18)
    bits_per_pixel = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if (
        width <= 0
        or signed_height == 0
        or bits_per_pixel not in (24, 32)
        or compression not in (0, 3)
        or (compression == 3 and bits_per_pixel != 32)
    ):
        raise AssertionError("SDL BMP has an unsupported pixel layout")
    if compression == 3:
        red_mask, green_mask, blue_mask, _alpha_mask = struct.unpack_from("<IIII", data, 54)
        if (red_mask, green_mask, blue_mask) != (0x00FF0000, 0x0000FF00, 0x000000FF):
            raise AssertionError("SDL BMP uses unexpected channel masks")
    return width, abs(signed_height), bits_per_pixel, data[pixel_offset:]


def pixel_rgb(
    bmp: tuple[int, int, int, bytes], x: int, y: int, top_down: bool = False
) -> tuple[int, int, int]:
    width, height, bpp, pixels = bmp
    bytes_per_pixel = bpp // 8
    stride = (width * bytes_per_pixel + 3) & ~3
    row = y if top_down else height - y - 1
    offset = row * stride + x * bytes_per_pixel
    blue, green, red = pixels[offset : offset + 3]
    return red, green, blue


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))


def png_pixel(red: int, green: int, blue: int) -> bytes:
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(b"\x00" + bytes((red, green, blue, 255))))
        + png_chunk(b"IEND", b"")
    )


def undecodable_png() -> bytes:
    """PNG-shaped bytes the header reader accepts but the decoder rejects.

    The signature, IHDR and IEND are well formed, so the image is an ordinary
    ISO entry that reads back intact; the IDAT carries bytes that are not a
    zlib stream, so SDL_LoadPNG_IO rejects it. This exercises decoder failure
    separately from missing ISO data and from a build with no PNG decoder.
    """
    ihdr = png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
    return (
        b"\x89PNG\r\n\x1a\n"
        + ihdr
        + png_chunk(b"IDAT", b"not a zlib stream")
        + png_chunk(b"IEND", b"")
    )


def corrupt_png_header() -> bytes:
    """Bytes that the ISO reader rejects before a decoder is involved."""
    return b"not a PNG image"


def synthetic_art_iso(
    path: Path, *, include_picture: bool = True, icon_bytes: bytes | None = None
) -> None:
    sector_size = 2048
    icon_image = png_pixel(50, 190, 120) if icon_bytes is None else icon_bytes
    assert len(icon_image) <= sector_size, "the synthetic icon must fit one sector"
    image = bytearray(22 * sector_size)

    def both_endian(value: int) -> bytes:
        return struct.pack("<I", value) + struct.pack(">I", value)

    def record(name: bytes, lba: int, size: int, directory: bool) -> bytes:
        record_size = 33 + len(name)
        if record_size & 1:
            record_size += 1
        result = bytearray(record_size)
        result[0] = record_size
        result[2:10] = both_endian(lba)
        result[10:18] = both_endian(size)
        result[25] = 2 if directory else 0
        result[28] = 1
        result[31] = 1
        result[32] = len(name)
        result[33 : 33 + len(name)] = name
        return bytes(result)

    pvd = memoryview(image)[16 * sector_size : 17 * sector_size]
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[158:166] = both_endian(17)
    pvd[166:174] = both_endian(sector_size)
    pvd[156:190] = record(b"\x00", 17, sector_size, True)

    root_records = (
        record(b"\x00", 17, sector_size, True)
        + record(b"\x01", 17, sector_size, True)
        + record(b"PSP_GAME", 18, sector_size, True)
    )
    game_records = (
        record(b"\x00", 18, sector_size, True)
        + record(b"\x01", 17, sector_size, True)
        + record(b"ICON0.PNG;1", 19, len(icon_image), False)
    )
    picture_size = 0
    if include_picture:
        game_records += record(b"PIC1.PNG;1", 20, len(png_pixel(50, 100, 190)), False)
        picture_size = len(png_pixel(50, 100, 190))
    image[17 * sector_size : 17 * sector_size + len(root_records)] = root_records
    image[18 * sector_size : 18 * sector_size + len(game_records)] = game_records
    image[19 * sector_size : 19 * sector_size + len(icon_image)] = icon_image
    if picture_size:
        image[20 * sector_size : 20 * sector_size + picture_size] = png_pixel(50, 100, 190)
    path.write_bytes(image)


def synthetic_stale_package(runtime_root: Path) -> None:
    package = runtime_root / "packages" / "TEST00006"
    package.mkdir(parents=True)
    package_json = {
        "format": "nakagawa-aot-package",
        "schema_version": 1,
        "title": {
            "id": "stale-synthetic-title",
            "display_name": "Synthetic stale package",
            "kind": "retail",
            "manifest_sha256": "0" * 64,
            "protected_digest": "0" * 64,
        },
        "inputs": {},
        "runtime": {},
        "executable": {"path": "stale-synthetic.exe"},
        "cache": {},
        "generated_objects": [],
        "required_local_assets": [],
        "build_report": "build-report.json",
    }
    (package / "package.json").write_text(json.dumps(package_json), encoding="utf-8")
    (package / "build-report.json").write_text("{}", encoding="utf-8")
    (package / "stale-synthetic.exe").write_bytes(b"synthetic stale executable")


def synthetic_ready_package(runtime_root: Path) -> None:
    """Write a source-owned package accepted by the real native validator."""
    disc_id = "TEST00006"
    title_id = "display-smoke-v1"
    package = runtime_root / "packages" / disc_id
    package.mkdir(parents=True)
    executable_name = f"{title_id}.exe"
    image_name = f"{title_id}_image.bin"
    executable_bytes = b"synthetic ready package executable\n"
    input_executable_hash = hashlib.sha256(b"synthetic input executable\n").hexdigest()
    executable_hash = hashlib.sha256(executable_bytes).hexdigest()
    zero_hash = "0" * 64

    def write_json(path: Path, value: object) -> None:
        path.write_text(json.dumps(value, separators=(",", ":")) + "\n", encoding="utf-8")

    def canonical_hash(value: object) -> str:
        canonical = json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"
        return hashlib.sha256(canonical.encode("utf-8")).hexdigest()

    identity = {
        "container": None,
        "disc": {"disc_version": None, "id": disc_id, "region": "TEST"},
        "format": "nakagawa-title-input-identity",
        "main_executable": {"name": "EBOOT.BIN", "sha256": input_executable_hash},
        "manifest": {"id": title_id, "schema_version": 1},
        "modules": [],
        "param_sfo": None,
        "psp_header": None,
        "schema_version": 1,
    }
    identity_text = json.dumps(identity, separators=(",", ":")) + "\n"
    identity_digest = canonical_hash(identity)
    identity_path = runtime_root / "title-input-identities" / disc_id / "title-input-identity.json"
    identity_path.parent.mkdir(parents=True)
    identity_path.write_text(identity_text, encoding="utf-8")

    aot_components = {
        "analyzer_codegen_epoch": "analyzer-codegen-v1",
        "analyzer_sha256": zero_hash,
        "codegen_options_sha256": canonical_hash({}),
        "codegen_sha256": zero_hash,
        "executable_sha256": input_executable_hash,
        "generated_code_abi_epoch": 1,
        "manifest_sha256": zero_hash,
        "modules_sha256": canonical_hash([]),
        "psp_header_sha256": None,
        "runtime_abi_epoch": 1,
        "title_input_identity_sha256": identity_digest,
    }
    native_components = {
        "compile_flags": "",
        "compiler_identity": "gcc-fixture",
        "compiler_target": "fixture-target",
        "generated_code_digest": zero_hash,
        "link_flags": "",
        "runtime_abi_epoch": 1,
        "runtime_source_digest": zero_hash,
    }
    cache_key = {
        "schema_version": 2,
        "aot": {"digest": canonical_hash(aot_components), "components": aot_components},
        "native": {"digest": canonical_hash(native_components), "components": native_components},
    }
    cache = {
        "format": "nakagawa-aot-cache",
        "schema_version": 2,
        "key": cache_key,
        "codegen_options": {},
        "runtime_abi_compatibility": {"current_epoch": 1, "generated_code_reusable": True},
    }
    inputs = {
        "manifest": {"sha256": zero_hash},
        "executable": {"sha256": input_executable_hash},
        "modules": [],
        "psp_header": None,
    }
    report = {
        "format": "nakagawa-build-report",
        "schema_version": 1,
        "title_id": title_id,
        "runtime_abi": {"name": "CpuState", "version": 2},
        "cache": cache,
        "input_hashes": inputs,
        "tools": {},
        "coverage": {},
        "unsupported": {"imports": [], "instructions": [], "regions": []},
        "analysis_diagnostics": [],
        "artifacts": {},
    }
    package_json = {
        "format": "nakagawa-aot-package",
        "schema_version": 2,
        "cache": cache,
        "title": {
            "id": title_id,
            "display_name": "Synthetic fixture",
            "kind": "retail",
            "manifest_sha256": zero_hash,
            "protected_digest": zero_hash,
        },
        "inputs": inputs,
        "title_input_identity": identity,
        "runtime": {
            "abi": "CpuState",
            "abi_version": 2,
            "abi_header_sha256": zero_hash,
            "run_entry": "0x00000000",
            "runtime_contract": None,
            "runtime_bindings": {},
            "required_runtime_bindings": [],
        },
        "executable": {
            "path": executable_name,
            "sha256": executable_hash,
            "guest_entry": "0x00000000",
        },
        "generated_objects": [],
        "required_local_assets": [],
        "build_report": "build-report.json",
    }

    executable = package / executable_name
    image = package / image_name
    executable.write_bytes(executable_bytes)
    image.write_bytes(b"synthetic runtime image\n")
    write_json(package / "build-report.json", report)
    write_json(package / "package.json", package_json)

    completion = {
        "format": "nakagawa-aot-cache-completion",
        "schema_version": 2,
        "status": "complete",
        "cache_key": cache_key,
        "title_input_identity": identity,
        "artifacts": [
            {"path": "package.json", "sha256": hashlib.sha256((package / "package.json").read_bytes()).hexdigest()},
            {"path": "build-report.json", "sha256": hashlib.sha256((package / "build-report.json").read_bytes()).hexdigest()},
            {"path": executable_name, "sha256": hashlib.sha256(executable.read_bytes()).hexdigest()},
            {"path": image_name, "sha256": hashlib.sha256(image.read_bytes()).hexdigest()},
        ],
    }
    write_json(package / "completion-manifest.json", completion)


def badge_rect(frame: dict[str, str]) -> tuple[int, int, int, int]:
    """The status badge rectangle the renderer reported for this frame."""
    x, y, w, h = (int(value) for value in frame["badge"].split(","))
    if w <= 0 or h <= 0:
        raise AssertionError(f"frame {frame.get('frame')} drew no status badge")
    return x, y, w, h


def has_amber_badge(bmp: tuple[int, int, int, bytes], rect: tuple[int, int, int, int]) -> bool:
    """True when the reported status badge is drawn in the amber accent."""
    width, height, _, _ = bmp
    x0, y0, w, h = rect
    right = min(width, x0 + w)
    bottom = min(height, y0 + h)
    return any(
        (lambda color: color[0] > 180 and 90 < color[1] < 205 and color[2] < 90)(
            pixel_rgb(bmp, x, y)
        )
        for y in range(max(0, y0), bottom)
        for x in range(max(0, x0), right)
    )


ICON_ART_RGB = (50, 190, 120)


def has_icon_art(bmp: tuple[int, int, int, bytes]) -> bool:
    width, height, _, _ = bmp
    return any(
        all(
            abs(channel - expected) <= 12
            for channel, expected in zip(
                pixel_rgb(bmp, x, y), ICON_ART_RGB, strict=True
            )
        )
        for y in range(112, min(196, height))
        for x in range(min(1120, width), min(1210, width))
    )


def parse_frames(output: str) -> list[dict[str, str]]:
    frames = []
    for line in output.splitlines():
        if not line.startswith("[PLAYER_UI_TEST] frame="):
            continue
        fields = dict(re.findall(r"([a-z_]+)=(\S+)", line))
        if "frame" not in fields or "view" not in fields:
            raise AssertionError(f"malformed player frame record: {line}")
        frames.append(fields)
    return frames


DEFAULT_BASE_TIMEOUT_SECONDS: float = 30.0
TIMEOUT_BUFFER_SECONDS: float = 15.0


def compute_effective_timeout(
    event_script: Sequence[str],
    explicit_timeout: float | None = None,
) -> float:
    """Compute adaptive subprocess timeout based on scripted UI test events.

    Scripted wait events parse as:
      - WAIT_MS=<duration_ms>: waits for explicit millisecond duration.
      - WAIT_VIEW=<view_mask>,<timeout_ms>: waits up to timeout_ms for view match.

    When explicit_timeout is None, sums all wait event bounds and adds
    TIMEOUT_BUFFER_SECONDS over DEFAULT_BASE_TIMEOUT_SECONDS to ensure slow
    or heavily loaded test runners do not trigger premature TimeoutExpired.
    """
    if explicit_timeout is not None:
        return explicit_timeout
    effective = DEFAULT_BASE_TIMEOUT_SECONDS
    for ev in event_script:
        if ev.startswith("WAIT_MS="):
            try:
                effective += int(ev.split("=", 1)[1]) / 1000.0
            except ValueError:
                pass
        elif ev.startswith("WAIT_VIEW=") and "," in ev:
            try:
                effective += int(ev.split(",", 1)[1]) / 1000.0
            except ValueError:
                pass
    return effective + TIMEOUT_BUFFER_SECONDS


class NativePlayerUiTests(unittest.TestCase):
    def run_player(
        self,
        view: str,
        events: tuple[str, ...] = (),
        *,
        error_code: str | None = None,
        runtime_ready: bool = False,
        invalid_profile: bool = False,
        drop_invalid_iso: bool = False,
        art_iso: str | None = None,
        stale_package: bool = False,
        wait_background: bool = False,
        catalog_reload_count: int = 0,
        build_ready_test: bool = False,
        post_build_ready: bool = False,
        downloaded_tool_payload: bool = False,
        legacy_data: bool = False,
        env_extra: dict[str, str] | None = None,
        width: int = 1280,
        height: int = 720,
        timeout: float | None = None,
    ) -> dict[str, object]:
        with tempfile.TemporaryDirectory(prefix=".player-ui-test-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            appdata = scratch / "appdata"
            localappdata = scratch / "localappdata"
            userprofile = scratch / "profile"
            for directory in (appdata, localappdata, userprofile):
                directory.mkdir()
            legacy_root = userprofile / "Nakagawa" / "data"
            if legacy_data:
                legacy_root.mkdir(parents=True)
            profile = scratch / "controller-profile.json"
            if invalid_profile:
                profile.write_text("{ definitely not a valid controller profile", encoding="utf-8")
            runtime_root = scratch / "runtime"
            runtime_root.mkdir()
            tool_marker: Path | None = None
            if downloaded_tool_payload:
                tool_marker = runtime_root / "prerequisites" / "synthetic-tool" / "marker.txt"
                tool_marker.parent.mkdir(parents=True)
                tool_marker.write_text("synthetic downloaded tool", encoding="utf-8")
            if build_ready_test:
                synthetic_ready_package(runtime_root)
            elif post_build_ready:
                synthetic_ready_package(runtime_root)
            catalog_overlay: Path | None = None
            if catalog_reload_count:
                catalog_overlay = scratch / "catalog-overlay.json"
                catalog_overlay.write_text(
                    json.dumps(
                        {
                            "schema_version": 1,
                            "id": "synthetic-ui-catalog-reload-v1",
                            "display_name": "Synthetic UI catalog reload fixture",
                            "kind": "retail",
                            "disc": {
                                "id": "TEST00999",
                                "region": "NA",
                                "revision_policy": "exact-disc-id",
                            },
                            "executable": {
                                "base": "0x08800000",
                                "entry": "0x08800000",
                                "bss_metadata_source": "elf",
                                "extra_executable_spans": [],
                            },
                            "modules": [],
                            "filesystem": {
                                "data_root": "data",
                                "memory_stick_root": "ms",
                                "device_prefixes": ["host0:"],
                            },
                            "hle_profile": "standard",
                            "feature_requirements": ["allegrex"],
                            "verification_profile": "smoke",
                        },
                        separators=(",", ":"),
                    ),
                    encoding="utf-8",
                )
            ui_iso_path: Path | None = None
            art_mount_source: Path | None = None
            if art_iso is not None:
                if art_iso == "saturated":
                    # The cache exerciser appends a per-slot suffix. Keep
                    # printf tokens in this caller-owned path to prove it is
                    # always passed as data to a fixed format string.
                    ui_iso_path = scratch / "art_%s_%p.iso"
                else:
                    ui_iso_path = scratch / "source-owned-synthetic.iso"
                    synthetic_art_iso(
                        ui_iso_path,
                        include_picture=art_iso != "icon-only",
                        icon_bytes=(
                            undecodable_png() if art_iso == "undecodable" else
                            corrupt_png_header() if art_iso == "corrupt" else None
                        ),
                    )
                    if art_iso == "unavailable":
                        ui_iso_path.rename(scratch / "source-owned-synthetic.iso.offline")
                    elif art_iso == "late":
                        art_mount_source = ui_iso_path
                        ui_iso_path = scratch / "late-mounted-synthetic.iso"
                    elif art_iso not in ("available", "icon-only", "undecodable", "corrupt"):
                        raise ValueError(f"unknown synthetic ISO mode: {art_iso}")
            if stale_package:
                synthetic_stale_package(runtime_root)

            env = os.environ.copy()
            env.update(
                {
                    "SDL_VIDEODRIVER": "dummy",
                    "SDL_RENDER_DRIVER": "software",
                    "APPDATA": str(appdata),
                    "LOCALAPPDATA": str(localappdata),
                    "USERPROFILE": str(userprofile),
                    "HOME": str(userprofile),
                    "XDG_CONFIG_HOME": str(userprofile / "config"),
                    "XDG_DATA_HOME": str(userprofile / "data"),
                    "XDG_CACHE_HOME": str(userprofile / "cache"),
                    "NK_INPUT_PROFILE": str(profile),
                }
            )
            if env_extra:
                env.update(env_extra)

            args = [str(PLAYER_EXE), f"--view={'build-ready' if build_ready_test else view}"]
            event_script = list(events)
            if drop_invalid_iso:
                invalid_iso = scratch / "source-owned-invalid.iso"
                invalid_iso.write_bytes(b"synthetic invalid PSP disc image\n")
                event_script.insert(0, f"DROP_FILE={invalid_iso}")
            args.append("--ui-test-events=" + ";".join(event_script))
            screenshot = scratch / "last-frame.bmp"
            args.append(f"--ui-test-screenshot={screenshot}")
            if ui_iso_path is not None:
                args.append(f"--ui-test-iso-path={ui_iso_path}")
            if wait_background:
                args.append("--ui-test-wait-background")
            if catalog_overlay is not None:
                args.extend(
                    (
                        f"--ui-test-catalog-reload-path={catalog_overlay}",
                        f"--ui-test-catalog-reload-count={catalog_reload_count}",
                    )
                )
            if art_mount_source is not None:
                args.append(f"--ui-test-art-mount-source={art_mount_source}")
            args.append(f"--width={width}")
            args.append(f"--height={height}")
            if error_code:
                args.append(f"--ui-test-error-code={error_code}")

            if runtime_ready:
                package = runtime_root / "build" / "display-smoke-v1"
                package.mkdir(parents=True)
                # The launcher resolves a developer package by path existence;
                # these empty files are synthetic sentinels, never executed.
                (package / "display-smoke-v1.exe").write_bytes(b"")
                (package / "display-smoke-v1").write_bytes(b"")
                (package / "display-smoke-v1_image.bin").write_bytes(b"")
            args.append(f"--runtime-root={runtime_root}")

            effective_timeout = compute_effective_timeout(event_script, timeout)

            started = time.perf_counter()
            completed = subprocess.run(
                args,
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                timeout=effective_timeout,
                check=False,
            )
            wall_seconds = time.perf_counter() - started
            if completed.returncode != 0:
                self.fail(
                    f"player returned {completed.returncode}\n"
                    f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
                )
            frames = parse_frames(completed.stdout)
            screenshot_line = next(
                (line for line in completed.stdout.splitlines() if line.startswith("[PLAYER_UI_TEST] screenshot=")),
                "",
            )
            self.assertIn("screenshot=PASS", screenshot_line, completed.stdout)
            self.assertGreaterEqual(len(frames), 2, completed.stdout)
            self.assertTrue(all(int(frame["pixels"], 16) != 0 for frame in frames))
            self.assertTrue(screenshot.is_file())
            bmp = read_bmp(screenshot)
            self.assertEqual((bmp[0], bmp[1]), (width, height))
            persisted_settings = None
            for settings_path in scratch.rglob("settings.json"):
                try:
                    candidate = json.loads(settings_path.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError):
                    continue
                if isinstance(candidate, dict) and "fps_cap" in candidate:
                    persisted_settings = candidate
                    break
            return {
                "frames": frames,
                "bmp": bmp,
                "profile_exists": profile.is_file(),
                "stdout": completed.stdout,
                "wall_seconds": wall_seconds,
                "stderr": completed.stderr,
                "legacy_root": legacy_root,
                "legacy_root_exists": legacy_root.is_dir(),
                "settings": persisted_settings,
                "downloaded_tool_marker_exists": (
                    tool_marker.is_file() if tool_marker is not None else None
                ),
            }

    def test_compute_effective_timeout(self) -> None:
        """Effective timeout scales with scripted wait events and respects explicit bounds."""
        self.assertEqual(compute_effective_timeout([], explicit_timeout=12.5), 12.5)
        self.assertEqual(
            compute_effective_timeout([]),
            DEFAULT_BASE_TIMEOUT_SECONDS + TIMEOUT_BUFFER_SECONDS,
        )
        events = ["WAIT_MS=2500", "WAIT_VIEW=ready_library,10000", "WAIT_VIEW=error,5000", "QUIT"]
        expected = DEFAULT_BASE_TIMEOUT_SECONDS + 2.5 + 10.0 + 5.0 + TIMEOUT_BUFFER_SECONDS
        self.assertEqual(compute_effective_timeout(events), expected)

    @unittest.skipUnless(sys.platform == "win32", "legacy %USERPROFILE%\\Nakagawa\\data migration warning is Windows-only")
    def test_legacy_data_root_is_reported_once_without_moving_it(self) -> None:
        run = self.run_player("empty", legacy_data=True)
        stderr = run["stderr"]
        legacy_root = run["legacy_root"]
        assert isinstance(stderr, str)
        assert isinstance(legacy_root, Path)
        expected_old_path = str(legacy_root.parent.parent) + "\\Nakagawa\\data"
        marker = f'LEGACY_DATA_DIR_FOUND: old_path="{expected_old_path}"'
        self.assertEqual(stderr.count(marker), 1, stderr)
        self.assertTrue(run["legacy_root_exists"])

    def test_empty_library_wizard_picker_and_font_confirmation(self) -> None:
        run = self.run_player("empty", ("KEY_RETURN", "KEY_RETURN", "KEY_RETURN"))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "library")
        self.assertEqual(frames[1]["view"], "wizard")
        self.assertEqual(frames[1]["wizard_step"], "welcome")
        self.assertEqual(frames[2]["wizard_step"], "select_game")
        self.assertEqual(frames[3]["picker"], "1")
        self.assertNotEqual(frames[0]["pixels"], frames[1]["pixels"])

        fonts = self.run_player("wizard4", ("KEY_RETURN",))
        font_frames = fonts["frames"]
        assert isinstance(font_frames, list)
        self.assertEqual(font_frames[1]["wizard_step"], "ready_launch")
        self.assertEqual(font_frames[1]["font_confirmed"], "1")

    def test_library_keyboard_gamepad_navigation_and_picker(self) -> None:
        run = self.run_player("library", ("KEY_RIGHT", "PAD_DPAD_RIGHT", "KEY_TAB", "KEY_RETURN"))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual([frames[i]["selected"] for i in range(1, 3)], ["1", "1"])
        self.assertNotEqual(frames[0]["pixels"], frames[1]["pixels"])
        self.assertEqual(frames[3]["focus"], "1")
        self.assertEqual(frames[4]["picker"], "1")

    def test_settings_screen_returns_to_library(self) -> None:
        run = self.run_player("settings", ("KEY_TAB", "KEY_ESCAPE"))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "settings")
        self.assertEqual(frames[1]["focus"], "1")
        self.assertEqual(frames[2]["view"], "library")
        self.assertNotEqual(frames[0]["pixels"], frames[2]["pixels"])

    def test_settings_presentation_choice_saves_with_native_default_schema(self) -> None:
        events = ("KEY_TAB",) * 4 + ("KEY_RETURN",) + ("KEY_TAB",) * 10 + ("KEY_RETURN",)
        run = self.run_player("settings", events)
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["focus_count"], "15")
        self.assertEqual(frames[4]["focus"], "4")
        self.assertEqual(frames[5]["focus"], "4")
        self.assertNotEqual(frames[4]["pixels"], frames[5]["pixels"])
        self.assertEqual(frames[-1]["view"], "library")
        settings = run["settings"]
        self.assertIsInstance(settings, dict)
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("schema_version"), 2)
        self.assertEqual(settings.get("fps_cap"), 0)
        renderer_source = (ROOT / "src" / "player" / "ui_renderer.c").read_text(
            encoding="utf-8"
        )
        self.assertIn('case -1: return "PSP TIMING";', renderer_source)
        self.assertIn('case 0:  return "NO EXTRA CAP";', renderer_source)
        self.assertNotIn("30 FPS CAP", renderer_source)
        self.assertNotIn("60 FPS CAP", renderer_source)

    def test_settings_about_opens_the_pinned_license_list(self) -> None:
        run = self.run_player("settings", ("KEY_TAB",) * 12 + ("KEY_RETURN",))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["focus_count"], "15")
        self.assertEqual(frames[12]["focus"], "12")
        self.assertEqual(frames[-1]["view"], "prereq_about")

    def test_settings_removal_requires_confirmation_and_keeps_other_app_data(self) -> None:
        events = ("KEY_TAB",) * 13 + ("KEY_RETURN", "KEY_RETURN")
        run = self.run_player("settings", events, downloaded_tool_payload=True)
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[13]["focus"], "13")
        self.assertEqual(frames[14]["view"], "confirm_remove_tools")
        self.assertEqual(frames[-1]["view"], "settings")
        self.assertFalse(run["downloaded_tool_marker_exists"])

    def test_settings_layout_avoids_launcher_save_overlap_at_client_widths(self) -> None:
        cases = (
            (1044, 900, "1"),
            (1187, 900, "1"),
            (1043, 900, "0"),
            (800, 720, "0"),
            (1188, 619, "0"),
            (1188, 620, "1"),
        )
        for width, height, expected_two_col in cases:
            with self.subTest(width=width, height=height):
                run = self.run_player("settings", width=width, height=height)
                frames = run["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["settings_two_col"], expected_two_col)

    def test_script_can_wait_for_a_view_and_a_minimum_duration(self) -> None:
        started = time.monotonic()
        run = self.run_player(
            "settings",
            (
                "KEY_ESCAPE",
                "WAIT_VIEW=library|settings,1000",
                "WAIT_MS=100",
                "ASSERT_VIEW=library",
                "KEY_S",
            ),
        )
        elapsed = time.monotonic() - started
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertGreaterEqual(elapsed, 0.08)
        self.assertIn("[PLAYER_UI_TEST] wait_view actual=library result=PASS", run["stdout"])
        self.assertIn("[PLAYER_UI_TEST] wait_ms result=PASS", run["stdout"])
        self.assertIn("[PLAYER_UI_TEST] assert_view result=PASS actual=library", run["stdout"])
        self.assertEqual(frames[-1]["view"], "settings")

    def test_mouse_click_adds_disc_through_sdl_event_loop(self) -> None:
        run = self.run_player(
            "library", ("MOUSE_MOVE=400,360", "MOUSE_DOWN", "MOUSE_UP")
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[2]["picker"], "1")
        self.assertNotEqual(frames[0]["pixels"], frames[2]["pixels"])

    def test_library_card_states_include_experimental_and_ready(self) -> None:
        # The ordinary library auto-discovers built showcase packages beside
        # the test executable. Use the in-memory staged-but-unbuilt card so the
        # missing-runtime state is stable whether or not `make showcase` ran.
        missing = self.run_player("ready-library", (), runtime_ready=False)
        missing_frame = missing["frames"][0]  # type: ignore[index]
        self.assertEqual(missing_frame["selected_prepared"], "0")
        self.assertEqual(missing_frame["selected_package_status"], "1")

        experimental = self.run_player("experimental-library", ())
        experimental_frame = experimental["frames"][0]  # type: ignore[index]
        self.assertEqual(experimental_frame["selected_experimental"], "1")

        ready = self.run_player(
            "ready", ("KEY_LEFT",) * 4, runtime_ready=True, wait_background=True
        )
        ready_frames = ready["frames"]
        assert isinstance(ready_frames, list)
        ready_frame = ready_frames[-1]
        self.assertEqual(ready_frame["selected_runtime"], "1")
        self.assertEqual(ready_frame["selected_prepared"], "0")
        self.assertNotEqual(missing_frame["pixels"], ready_frame["pixels"])

    def test_one_title_offline_art_and_stale_package_stay_off_frame_path(self) -> None:
        # The counters are the load-bearing gate: package validation and ISO
        # reads must never run on the UI thread, on any frame.
        # `package_validations` counts the UI-thread wrapper, so it is a proxy
        # for the whole validation path: a regression that inlined the core
        # call on the frame path would still leave it at zero, and only a
        # sanitizer or an interactive profile would see that. The wall-clock
        # bounds below are deliberately generous because the headless
        # software renderer, not the product's GPU path, sets the absolute
        # per-frame floor here; they only reject the maintainer-reported shape
        # (a first frame of seconds, steady frames of hundreds of ms).
        events = ("KEY_LEFT",) * 24
        offline = self.run_player(
            "ready",
            events,
            art_iso="unavailable",
            stale_package=True,
            wait_background=True,
        )
        frames = offline["frames"]
        assert isinstance(frames, list)
        self.assertGreaterEqual(len(frames), 20)
        validation_calls = [int(frame["package_validations"]) for frame in frames]
        self.assertEqual(validation_calls, [0] * len(frames))
        elapsed = [int(frame["render_ns"]) for frame in frames]
        self.assertLess(elapsed[0], 1_000_000_000)
        self.assertLess(max(elapsed[1:]), 250_000_000)
        self.assertLess(sorted(elapsed[1:])[len(elapsed[1:]) // 2], 60_000_000)
        # Coarse end-to-end guard over process start through the last frame:
        # the maintainer-reported 3 fps interactive loop would need ~9s here.
        self.assertLess(offline["wall_seconds"], 6.0)
        self.assertEqual(frames[-1]["selected_package_status"], "3")
        self.assertEqual(frames[-1]["selected_runtime"], "0")
        # An absent disc image must show the initials placeholder, never a
        # blank or a blocked frame waiting for the drive.
        self.assertFalse(has_icon_art(offline["bmp"]))

        available = self.run_player(
            "ready",
            ("KEY_LEFT",) * 8,
            art_iso="available",
            stale_package=True,
            wait_background=True,
        )
        art_frames = available["frames"]
        assert isinstance(art_frames, list)
        self.assertEqual(
            [int(frame["package_validations"]) for frame in art_frames],
            [0] * len(art_frames),
        )
        self.assertEqual(art_frames[-1]["selected_package_status"], "3")
        # Synthetic icon art arrives without blocking the UI frame.
        self.assertTrue(has_icon_art(available["bmp"]))

    def test_package_status_worker_and_catalog_reload_do_not_deadlock_or_corrupt_status(
        self,
    ) -> None:
        """Two threads share the title catalog: the worker validates a package
        while the UI thread reloads the overlay 128 times. The load-bearing
        claims are that both sides finish and that the status the worker read
        is the one it stored; a data race between them is only visible to a
        sanitizer, which hosted CI does not run."""
        run = self.run_player(
            "ready",
            wait_background=True,
            catalog_reload_count=128,
        )
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] catalog_reload result=PASS "
            r"reloads=128 status_checks=128 status=1",
        )

    def test_exit_path_joins_iso_art_workers_and_tears_down_textures(self) -> None:
        """ui_font_shutdown() is the only place the ISO-art workers are joined
        and the only font/texture teardown. Every exit has to reach it, so the
        launcher never destroys the SDL queue or renderer while a worker is
        still inside it."""
        run = self.run_player(
            "ready",
            ("WAIT_ART_DELAY_ACTIVE", "QUIT"),
            art_iso="available",
            env_extra={"PLAYER_UI_TEST_ART_DELAY_MS": "5000"},
        )
        self.assertIn(
            "[PLAYER_UI_TEST] wait_art_delay_active result=PASS", run["stdout"]
        )
        shutdown = re.findall(
            r"\[PLAYER_UI_TEST\] font_shutdown "
            r"art_jobs_active_at_shutdown=(\d+) art_jobs_drained=(\d+)",
            run["stdout"],
        )
        self.assertEqual(len(shutdown), 1, run["stdout"])
        self.assertEqual(shutdown[0], ("1", "1"), run["stdout"])

    def test_iso_art_failures_reach_a_terminal_state_after_bounded_retries(self) -> None:
        run = self.run_player(
            "ready",
            ("WAIT_ART_ATTEMPT=3", "WAIT_MS=2500"),
            art_iso="unavailable",
            env_extra={"PLAYER_UI_TEST_NO_PNG_DECODER": "1"},
        )
        attempts = re.findall(
            r"\[PLAYER_UI_TEST\] art_attempt index=(\d+) "
            r"completed_ns=(\d+) icon_loaded=(\d) pic1_loaded=(\d) wanted=(\d+) "
            r"icon_state=(\w+) pic1_state=(\w+)",
            run["stdout"],
        )
        self.assertEqual(len(attempts), 3, run["stdout"])
        # A missing image remains retryable, then ends specifically as absent;
        # it is not the same result as corrupt bytes or an unsupported decoder.
        self.assertEqual(attempts[-1][5:], ("absent", "absent"), run["stdout"])

    def test_corrupt_art_is_failed_after_bounded_retries(self) -> None:
        run = self.run_player(
            "ready",
            ("WAIT_ART_ATTEMPT=3", "WAIT_MS=2500"),
            art_iso="corrupt",
            wait_background=True,
        )
        attempts = re.findall(
            r"\[PLAYER_UI_TEST\] art_attempt index=(\d+) "
            r"completed_ns=(\d+) icon_loaded=(\d) pic1_loaded=(\d) wanted=(\d+) "
            r"icon_state=(\w+) pic1_state=(\w+)",
            run["stdout"],
        )
        self.assertEqual(len(attempts), 3, run["stdout"])
        self.assertEqual(attempts[-1][5:], ("failed", "loaded"), run["stdout"])

    def test_readable_art_without_png_decoder_is_unsupported(self) -> None:
        run = self.run_player(
            "ready",
            ("WAIT_ART_ATTEMPT=1", "WAIT_MS=2500"),
            art_iso="available",
            wait_background=True,
            env_extra={"PLAYER_UI_TEST_NO_PNG_DECODER": "1"},
        )
        attempts = re.findall(
            r"\[PLAYER_UI_TEST\] art_attempt index=(\d+) "
            r"completed_ns=(\d+) icon_loaded=(\d) pic1_loaded=(\d) wanted=(\d+) "
            r"icon_state=(\w+) pic1_state=(\w+)",
            run["stdout"],
        )
        self.assertEqual(len(attempts), 1, run["stdout"])
        self.assertEqual(
            attempts[0][2:],
            ("0", "0", "3", "unsupported", "unsupported"),
            run["stdout"],
        )
        self.assertFalse(has_icon_art(run["bmp"]), run["stdout"])

    def test_decoder_rejection_is_failed_after_bounded_retries(self) -> None:
        """A decoder rejection is a failed image, not a build capability result."""
        run = self.run_player(
            "ready",
            ("WAIT_ART_ATTEMPT=3", "WAIT_MS=2500"),
            art_iso="undecodable",
            wait_background=True,
        )
        attempts = re.findall(
            r"\[PLAYER_UI_TEST\] art_attempt index=(\d+) "
            r"completed_ns=(\d+) icon_loaded=(\d) pic1_loaded=(\d) wanted=(\d+) "
            r"icon_state=(\w+) pic1_state=(\w+)",
            run["stdout"],
        )
        self.assertEqual(len(attempts), 3, run["stdout"])
        self.assertEqual(
            attempts[0][2:],
            ("0", "1", "3", "pending", "loaded"),
            run["stdout"],
        )
        self.assertEqual(attempts[-1][5:], ("failed", "loaded"), run["stdout"])
        self.assertFalse(has_icon_art(run["bmp"]), run["stdout"])

    def test_iso_art_retry_rereads_only_the_image_that_is_still_missing(self) -> None:
        """A disc with ICON0.PNG and no PIC1.PNG must not re-read its icon
        (up to 1 MiB) on every retry: the attempt after the first asks only for
        the picture it is still missing."""
        run = self.run_player(
            "ready",
            ("WAIT_ART_ATTEMPT=1", "WAIT_MS=1500"),
            art_iso="icon-only",
            wait_background=True,
        )
        self.assertTrue(has_icon_art(run["bmp"]), run["stdout"])
        attempts = re.findall(
            r"\[PLAYER_UI_TEST\] art_attempt index=(\d+) "
            r"completed_ns=(\d+) icon_loaded=(\d) pic1_loaded=(\d) wanted=(\d)",
            run["stdout"],
        )
        self.assertGreaterEqual(len(attempts), 2, run["stdout"])
        self.assertEqual(attempts[0][2:], ("1", "0", "3"))
        for attempt in attempts[1:]:
            # 2 == NK_ISO_ART_PICTURE: the loaded icon is not read again.
            self.assertEqual(attempt[2:], ("1", "0", "2"), run["stdout"])

    def test_every_queued_title_claims_a_check_before_its_first_result(self) -> None:
        """A title whose package status is not known yet must be claimed as
        checking. Leaving it unclaimed shows "BUILD PACKAGE" for a title that
        may already be prepared."""
        run = self.run_player("library")
        frame = run["frames"][0]  # type: ignore[index]
        titles = int(frame["games"])
        self.assertGreater(titles, 1, run["stdout"])
        self.assertEqual(frame["titles_unclaimed"], "0", run["stdout"])
        self.assertEqual(frame["titles_pending"], str(titles), run["stdout"])

    def test_lost_worker_events_are_reclaimed_instead_of_stalling_the_card(self) -> None:
        """A worker whose completion event never reaches the queue must not
        leave its card pending for the session. With every push refused the
        launcher still finishes both jobs, so the harness reaches its quit
        instead of waiting on background work that can never finish."""
        run = self.run_player(
            "ready",
            ("WAIT_MS=1200",),
            art_iso="available",
            stale_package=True,
            wait_background=True,
            env_extra={"NK_UI_TEST_DROP_WORKER_EVENT": "1"},
        )
        self.assertIn("ISO art for", run["stderr"])
        self.assertIn("Package status for", run["stderr"])
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["running"], "0")
        # The package job was applied from the worker result, so no title is
        # left claiming a check that will never finish.
        self.assertEqual(frames[-1]["titles_pending"], "0", run["stdout"])
        self.assertEqual(frames[-1]["titles_unclaimed"], "0", run["stdout"])
        self.assertEqual(frames[-1]["selected_package_status"], "3", run["stdout"])
        # The art job was reclaimed, so the icon is retried rather than lost.
        self.assertFalse(has_icon_art(run["bmp"]), run["stdout"])

    def test_package_status_thread_creation_failure_is_unresolved_and_retryable(
        self,
    ) -> None:
        """A failed worker start stays unresolved until the user retries.

        The 2-second library rescan must not turn one create failure into a
        repeating thread-creation loop. The retry button then enters the same
        production queue and lets the real validator establish MISSING.
        """
        run = self.run_player(
            "ready",
            events=("WAIT_MS=2200", "KEY_RETURN", "WAIT_MS=100"),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "1"
            },
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        failed_frames = [frame for frame in frames
                         if frame["selected_package_check_failed"] == "1"]
        self.assertTrue(failed_frames, run["stdout"])
        self.assertTrue(all(frame["selected_package_status"] == "4"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(all(frame["package_check_failed_badge"] == "1"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(all(frame["titles_failed"] == "1" and
                            frame["titles_pending"] == "0" and
                            frame["titles_unclaimed"] == "0"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(any(int(frame["ticks_ms"]) >= 2000 and
                            frame["package_status_thread_attempts"] == "1"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(any(frame["package_status_thread_attempts"] == "2"
                            for frame in frames), run["stdout"])
        self.assertEqual(frames[-1]["package_status_thread_attempts"], "2")
        self.assertEqual(frames[-1]["selected_package_check_failed"], "0")
        self.assertEqual(frames[-1]["titles_failed"], "0")
        self.assertEqual(frames[-1]["titles_pending"], "0")
        self.assertEqual(frames[-1]["titles_unclaimed"], "0")
        self.assertEqual(frames[-1]["selected_package_status"], "1")
        self.assertEqual(frames[-1]["package_building"], "0")

    def test_failed_post_build_worker_start_survives_cache_invalidation_and_retries(self) -> None:
        run = self.run_player(
            "ready",
            events=(
                "WAIT_VIEW=library,5000",
                "INVALIDATE_PACKAGE_CACHE",
                "KEY_RETURN",
                "WAIT_MS=100",
                "WAIT_VIEW=ready_library,10000",
                "ASSERT_VIEW=ready_library",
            ),
            wait_background=True,
            build_ready_test=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "2"
            },
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertIn(
            "[PLAYER_UI_TEST] post_build_validation status=0", run["stdout"]
        )
        self.assertIn(
            "[PLAYER_UI_TEST] assert_view result=PASS actual=ready_library",
            run["stdout"],
        )
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] invalidate_package_cache generation=\d+",
        )
        failed_frames = [frame for frame in frames
                         if frame["selected_package_check_failed"] == "1"]
        self.assertTrue(failed_frames, run["stdout"])
        self.assertTrue(all(frame["package_check_failed_badge"] == "1"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(any(frame["package_status_thread_attempts"] == "2"
                            and frame["view"] == "library"
                            for frame in failed_frames), run["stdout"])
        self.assertEqual(frames[-1]["view"], "ready_library")
        self.assertGreaterEqual(
            int(frames[-1]["package_status_thread_attempts"]), 3
        )

    def test_worker_start_failure_automatically_retries_after_bounded_backoff(self) -> None:
        run = self.run_player(
            "ready",
            events=("WAIT_MS=6200",),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "1"
            },
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(frame["package_status_thread_attempts"] == "1" and
                int(frame["ticks_ms"]) < 5000
                for frame in frames),
            run["stdout"],
        )
        self.assertGreaterEqual(
            int(frames[-1]["package_status_thread_attempts"]), 2,
            run["stdout"],
        )
        self.assertEqual(frames[-1]["selected_package_check_failed"], "0")
        self.assertEqual(frames[-1]["selected_package_status"], "1")

    def test_completed_retry_clears_failure_when_cache_generation_changes(self) -> None:
        run = self.run_player(
            "ready",
            events=(
                "WAIT_MS=100",
                "KEY_RETURN",
                "WAIT_MS=100",
                "INVALIDATE_PACKAGE_CACHE",
                "WAIT_MS=1700",
            ),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "1",
                "NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS": "600",
            },
        )
        self.assertIn("[PLAYER_UI_TEST] invalidate_package_cache", run["stdout"])
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertGreaterEqual(
            int(frames[-1]["package_status_thread_attempts"]), 3,
            run["stdout"],
        )
        self.assertEqual(frames[-1]["selected_package_check_failed"], "0",
                         run["stdout"])
        self.assertEqual(frames[-1]["selected_package_status"], "1",
                         run["stdout"])

    def test_failed_post_build_check_does_not_suppress_later_title_build(self) -> None:
        run = self.run_player(
            "library",
            events=(
                "START_SYNTHETIC_BUILD=2",
                "WAIT_MS=2200",
                "START_SYNTHETIC_BUILD=1",
                "WAIT_VIEW=error,10000",
                "ASSERT_VIEW=error",
            ),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "2"
            },
        )
        self.assertIn(
            "[PLAYER_UI_TEST] synthetic_build result=PASS index=1 disc=TEST00006",
            run["stdout"],
        )
        self.assertIn(
            "[PLAYER_UI_TEST] synthetic_build result=PASS index=0 disc=TEST00005",
            run["stdout"],
        )
        self.assertIn(
            "Package status for TEST00006 is unresolved", run["stderr"]
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(int(frame["package_status_thread_attempts"]) >= 3
                for frame in frames),
            run["stdout"],
        )
        self.assertIn(
            "[PLAYER_UI_TEST] post_build_validation status=1", run["stdout"]
        )
        self.assertIn(
            "[PLAYER_UI_TEST] assert_view result=PASS actual=error",
            run["stdout"],
        )

    def test_failed_post_build_check_same_disc_cache_epoch_is_not_stale(self) -> None:
        run = self.run_player(
            "library",
            events=(
                "START_SYNTHETIC_BUILD=2",
                "WAIT_MS=2200",
                "INVALIDATE_AND_START_SYNTHETIC_BUILD=2",
                "WAIT_VIEW=error,10000",
                "ASSERT_VIEW=error",
            ),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "2"
            },
        )
        action_line = next(
            line for line in run["stdout"].splitlines()
            if line.startswith(
                "[PLAYER_UI_TEST] invalidate_cache_and_synthetic_build"
            )
        )
        action_match = re.search(
            r"result=PASS index=1 disc=TEST00006 generation=(\d+) "
            r"build_session_generation=(\d+)$",
            action_line,
        )
        self.assertIsNotNone(action_match, run["stdout"])
        assert action_match is not None
        self.assertGreater(int(action_match.group(1)), 0)
        self.assertGreater(int(action_match.group(2)), 0)
        post_match = re.search(
            r"\[PLAYER_UI_TEST\] post_build_validation status=1 .*?"
            r"build_session_generation=(\d+) "
            r"current_build_session_generation=(\d+)",
            run["stdout"],
        )
        self.assertIsNotNone(post_match, run["stdout"])
        assert post_match is not None
        self.assertEqual(post_match.group(1), post_match.group(2), run["stdout"])
        self.assertEqual(post_match.group(1), action_match.group(2), run["stdout"])
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(int(frame["package_status_thread_attempts"]) >= 3
                for frame in frames),
            run["stdout"],
        )
        self.assertIn(
            "[PLAYER_UI_TEST] assert_view result=PASS actual=error",
            run["stdout"],
        )

    def test_worker_start_failure_does_not_block_other_title_builds(self) -> None:
        """A's worker-start failure cannot strand the post-build latch for B.

        A fails before either B build starts, and B completes both builds. The
        failure selector is title-keyed so this does not depend on worker
        scheduling or a global create-attempt ordinal.
        """
        run = self.run_player(
            "library",
            events=(
                "START_SYNTHETIC_BUILD=1",
                "WAIT_PACKAGE_FAILURE=TEST00005,1",
                "START_SYNTHETIC_BUILD=2",
                "WAIT_VIEW=ready_library,10000",
                "START_SYNTHETIC_BUILD=2",
                "WAIT_VIEW=ready_library,10000",
                "ASSERT_VIEW=ready_library",
                "QUIT",
            ),
            runtime_ready=True,
            post_build_ready=True,
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_FOR": "TEST00005"
            },
        )
        post_build_lines = [
            line for line in run["stdout"].splitlines()
            if "post_build_transition" in line and "disc=TEST00006" in line
        ]
        self.assertEqual(len(post_build_lines), 2, run["stdout"])
        unresolved = [
            line for line in run["stderr"].splitlines()
            if "is unresolved because its worker could not start" in line
        ]
        self.assertGreaterEqual(len(unresolved), 1, run["stderr"])
        self.assertTrue(all("TEST00005" in line for line in unresolved), run["stderr"])
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(frame["selected_disc"] == "TEST00005" and
                frame["selected_package_check_failed"] == "1" and
                frame["selected_package_status"] == "4"
                for frame in frames),
            run["stdout"],
        )
        self.assertEqual(frames[-1]["selected_disc"], "TEST00006")
        self.assertEqual(frames[-1]["selected_package_check_failed"], "0")
        self.assertEqual(frames[-1]["selected_package_status"], "0")

    def test_repeated_post_build_failures_keep_prior_title_retryable(self) -> None:
        run = self.run_player(
            "library",
            events=(
                "START_SYNTHETIC_BUILD=2",
                "WAIT_PACKAGE_FAILURE=TEST00006,1",
                "START_SYNTHETIC_BUILD=1",
                "WAIT_PACKAGE_FAILURE=TEST00005,1",
                "KEY_RIGHT",
                "KEY_RETURN",
                "WAIT_PACKAGE_FAILURE=TEST00006,2",
                "QUIT",
            ),
            runtime_ready=True,
            post_build_ready=True,
            wait_background=True,
            env_extra={
                # B fails on its post-build check, A fails on the next build,
                # and B fails again on the explicit retry. The following
                # repeated failures must stay attributed to their titles.
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_FOR":
                    "TEST00005,TEST00006"
            },
        )
        self.assertIn(
            "Package status for TEST00006 is unresolved", run["stderr"]
        )
        self.assertIn(
            "Package status for TEST00005 is unresolved", run["stderr"]
        )
        unresolved = [
            line for line in run["stderr"].splitlines()
            if "is unresolved because its worker could not start" in line
        ]
        self.assertGreaterEqual(
            sum("TEST00005" in line for line in unresolved), 2, run["stderr"]
        )
        self.assertGreaterEqual(
            sum("TEST00006" in line for line in unresolved), 2, run["stderr"]
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(frame["selected_disc"] == "TEST00006" and
                frame["selected_package_check_failed"] == "1" and
                frame["selected_package_status"] == "4"
                for frame in frames),
            run["stdout"],
        )
        self.assertEqual(frames[-1]["selected_disc"], "TEST00006")
        self.assertEqual(frames[-1]["selected_package_check_failed"], "1")
        self.assertEqual(frames[-1]["selected_package_status"], "4")
        self.assertEqual(frames[-1]["titles_pending"], "0")

    def test_texture_cache_saturation_with_in_flight_jobs_does_not_double_free_or_format_percent_tokens(
        self,
    ) -> None:
        """When all 64 texture cache slots have in-flight art jobs, a new
        request must return no slot rather than evicting and freeing a job whose
        completion event is already in the SDL event queue. Evicting the in-flight
        job would cause a use-after-free and double-free when its completion
        event is subsequently delivered. The caller-owned path contains literal
        %s/%p tokens to catch its accidental use as a printf format string."""
        run = self.run_player(
            "ready",
            ("SATURATE_ART_CACHE",),
            art_iso="saturated",
        )
        self.assertIn(
            "[PLAYER_UI_TEST] saturate_art_cache result=PASS slots=64 delivered=64 overflow_refused=1",
            run["stdout"],
        )

    def test_async_build_validation_enters_ready_library(self) -> None:
        run = self.run_player(
            "ready",
            ("WAIT_VIEW=ready_library,10000", "ASSERT_VIEW=ready_library"),
            build_ready_test=True,
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "building_package")
        ready_frames = [frame for frame in frames if frame["view"] == "ready_library"]
        self.assertTrue(ready_frames)
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] post_build_validation status=0 reason=Runtime package v2 is valid",
        )
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] post_build_transition view=ready_library "
            r"disc=TEST00006 found=1 prepared=1 status=\d+",
        )

    def test_late_mounted_iso_art_retries_after_cooldown(self) -> None:
        run = self.run_player(
            "ready",
            ("WAIT_ART_ATTEMPT=1", "MOUNT_ART_ISO", "WAIT_ART_ATTEMPT=2"),
            art_iso="late",
            wait_background=True,
        )
        self.assertTrue(has_icon_art(run["bmp"]))
        attempts = re.findall(
            r"\[PLAYER_UI_TEST\] art_attempt index=(\d+) "
            r"completed_ns=(\d+) icon_loaded=(\d) pic1_loaded=(\d)",
            run["stdout"],
        )
        self.assertEqual(len(attempts), 2, run["stdout"])
        self.assertEqual(attempts[0][2:], ("0", "0"))
        self.assertEqual(attempts[1][2:], ("1", "1"))
        retry_delay_ns = int(attempts[1][1]) - int(attempts[0][1])
        self.assertGreaterEqual(retry_delay_ns, 1_000_000_000)
        self.assertLessEqual(retry_delay_ns, 2_500_000_000)

    def test_inspection_and_support_screens_render_and_return(self) -> None:
        for view in ("inspecting", "supported", "experimental", "unsupported", "preparing"):
            with self.subTest(view=view):
                run = self.run_player(view, ("KEY_ESCAPE",))
                frames = run["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["view"], view)
                self.assertEqual(frames[-1]["view"], "library")
                self.assertNotEqual(frames[0]["pixels"], frames[-1]["pixels"])

    def test_staged_card_exposes_runtime_required_status(self) -> None:
        run = self.run_player("ready")
        frame = run["frames"][0]  # type: ignore[index]
        self.assertEqual(frame["selected_staged"], "1")
        self.assertEqual(frame["selected_runtime"], "0")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(has_amber_badge(run["bmp"], badge_rect(frames[-1])))

    def test_staging_and_package_build_cancellation_render_progress(self) -> None:
        staging = self.run_player("wizard-staging", ("KEY_ESCAPE",))
        stage_frames = staging["frames"]
        assert isinstance(stage_frames, list)
        self.assertEqual(stage_frames[0]["extracting"], "1")
        self.assertEqual(stage_frames[0]["extraction_percent"], "67")
        self.assertEqual(stage_frames[1]["extraction_cancel"], "1")
        self.assertTrue(all(int(frame["pixels"], 16) != 0 for frame in stage_frames[:2]))

        building = self.run_player("building", ("KEY_ESCAPE",))
        build_frames = building["frames"]
        assert isinstance(build_frames, list)
        self.assertEqual(build_frames[0]["view"], "building_package")
        self.assertEqual(build_frames[0]["package_building"], "1")
        self.assertEqual(build_frames[1]["package_cancelled"], "1")
        self.assertEqual(build_frames[1]["view"], "library")

    def test_controller_bind_conflict_calibration_and_profile_save(self) -> None:
        bind = self.run_player("controller", ("KEY_TAB", "KEY_RETURN", "PAD_SOUTH"))
        bind_frames = bind["frames"]
        assert isinstance(bind_frames, list)
        self.assertEqual(bind_frames[2]["controller_capturing"], "1")
        self.assertEqual(bind_frames[3]["controller_capturing"], "0")
        self.assertEqual(bind_frames[3]["controller_conflicts"], "1")
        self.assertNotEqual(bind_frames[0]["start_binding"], bind_frames[3]["start_binding"])

        calibrate = self.run_player(
            "controller", tuple(["KEY_TAB"] * 18 + ["KEY_RETURN", "KEY_ESCAPE"])
        )
        cal_frames = calibrate["frames"]
        assert isinstance(cal_frames, list)
        self.assertEqual(cal_frames[19]["calibrating"], "1")
        self.assertEqual(cal_frames[20]["calibrating"], "0")
        self.assertEqual(cal_frames[20]["view"], "controller")

        invalid = self.run_player("controller", (), invalid_profile=True)
        invalid_frame = invalid["frames"][0]  # type: ignore[index]
        self.assertEqual(invalid_frame["profile_fallback"], "1")

        save = self.run_player("controller", tuple(["KEY_TAB"] * 19 + ["KEY_RETURN"]))
        save_frame = save["frames"][-2]  # type: ignore[index]
        self.assertEqual(save_frame["profile_save_notice"], "1")
        self.assertTrue(save["profile_exists"])

    def test_corrupt_disc_and_error_card_recovery(self) -> None:
        corrupt = self.run_player("library", ("KEY_RETURN",), drop_invalid_iso=True)
        error_frames = corrupt["frames"]
        assert isinstance(error_frames, list)
        self.assertEqual(error_frames[1]["view"], "error")
        self.assertEqual(error_frames[1]["error"], "ISO_CORRUPT")
        self.assertEqual(error_frames[2]["picker"], "1")

        missing_source = self.run_player("error", ("KEY_RETURN",))
        missing_source_frames = missing_source["frames"]
        assert isinstance(missing_source_frames, list)
        self.assertEqual(missing_source_frames[0]["error"], "SOURCE_NOT_FOUND")
        self.assertEqual(missing_source_frames[1]["picker"], "1")

        recovery_codes = (
            "EXPERIMENTAL_PROFILE_FAILED",
            "SOURCE_NOT_FOUND",
            "PROCESS_SPAWN_FAILED",
            "STAGED_EXECUTABLE_INVALID",
            "MANIFEST_MISMATCH",
            "RUNTIME_NOT_FOUND",
            "LIBRARY_WRITE_FAILED",
            "RUNTIME_PACKAGE_NOT_READY",
            "PYTHON_NOT_FOUND",
            "CLI_NOT_FOUND",
            "DATA_DIR_UNAVAILABLE",
            "SPAWN_FAILED",
            "PACKAGE_BUILD_FAILED",
            "RUNTIME_PREMATURE_EXIT",
            "RUNTIME_ERROR_EXIT",
        )
        for code in recovery_codes:
            with self.subTest(code=code):
                recovered = self.run_player("library", ("KEY_RETURN",), error_code=code)
                frames = recovered["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["view"], "error")
                self.assertEqual(frames[0]["error"], code)
                self.assertEqual(frames[1]["view"], "library")
                self.assertNotEqual(frames[0]["pixels"], frames[1]["pixels"])
                # Only a missing or corrupt source reopens the file picker.
                expected_picker = "1" if code in ("ISO_CORRUPT", "SOURCE_NOT_FOUND") else "0"
                self.assertEqual(frames[1]["picker"], expected_picker)

    def test_per_title_controller_mapping_choice(self) -> None:
        """A disc can get its own mapping, or go back to the global one."""
        # The library card carries the choice: three tabs reach it, because the
        # primary action, add and remove come first and paging never applies to
        # a one-title library.
        card = self.run_player("library", ("KEY_TAB", "KEY_TAB", "KEY_TAB", "KEY_RETURN"))
        card_frames = card["frames"]
        assert isinstance(card_frames, list)
        self.assertEqual(card_frames[0]["input_scope"], "global")
        self.assertEqual(card_frames[0]["input_titles"], "0")
        self.assertEqual(card_frames[4]["input_scope"], "TEST00005")
        self.assertEqual(card_frames[4]["input_titles"], "1")
        self.assertNotEqual(card_frames[0]["pixels"], card_frames[4]["pixels"])

        # The same control in Controller Settings, which says which mapping it
        # is editing, and which is the 23rd focus stop of that view.
        settings = self.run_player(
            "controller", tuple(["KEY_TAB"] * 22 + ["KEY_RETURN"])
        )
        settings_frames = settings["frames"]
        assert isinstance(settings_frames, list)
        self.assertEqual(settings_frames[0]["input_scope"], "global")
        self.assertEqual(settings_frames[0]["focus_count"], "23")
        self.assertEqual(settings_frames[23]["input_scope"], "TEST00005")
        self.assertNotEqual(settings_frames[0]["pixels"], settings_frames[23]["pixels"])

        # Toggling back returns the disc to the global mapping.
        back = self.run_player(
            "controller", tuple(["KEY_TAB"] * 22 + ["KEY_RETURN"] * 2)
        )
        back_frames = back["frames"]
        assert isinstance(back_frames, list)
        self.assertEqual(back_frames[23]["input_scope"], "TEST00005")
        self.assertEqual(back_frames[24]["input_scope"], "global")
        self.assertEqual(back_frames[24]["input_titles"], "0")

    def test_quit_from_library_runs_through_sdl_event_loop(self) -> None:
        run = self.run_player("library", ("KEY_ESCAPE",))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "library")
        self.assertEqual(frames[-1]["running"], "0")

    def test_failed_close_confirmation_needs_a_second_explicit_close(self) -> None:
        run = self.run_player(
            "library",
            ("CLOSE", "CLOSE"),
            env_extra={
                "NK_UI_TEST_GAME_RUNNING": "1",
                "NK_UI_TEST_MESSAGEBOX_FAIL": "1",
            },
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["game_running"], "1")
        self.assertEqual(frames[-1]["game_running"], "1")
        self.assertEqual(frames[-1]["running"], "0")
        self.assertIn("Repeat the close request", run["stderr"])
        self.assertIn("explicit force-quit", run["stderr"])

    def test_font_fallback_reason_is_reported(self) -> None:
        """Every frame names the active text path and, on fallback, why (#421)."""
        forced = self.run_player("library", (), env_extra={"NK_UI_NO_TTF": "1"})
        forced_frames = forced["frames"]
        assert isinstance(forced_frames, list)
        for frame in forced_frames:
            self.assertEqual(frame["font"], "bitmap")
            self.assertEqual(frame["font_reason"], "forced-off")
        self.assertIn("NK_UI_NO_TTF", str(forced["stderr"]))

        normal = self.run_player("library", ())
        normal_frames = normal["frames"]
        assert isinstance(normal_frames, list)
        for frame in normal_frames:
            self.assertIn(frame["font"], ("ttf", "bitmap"))
            if frame["font"] == "ttf":
                self.assertEqual(frame["font_reason"], "NONE")
            else:
                # A fallback without a named reason is exactly the silent
                # degradation this contract forbids.
                self.assertNotEqual(frame["font_reason"], "NONE")
                self.assertTrue(frame["font_reason"])


def library_disc_ids(scratch: Path) -> list[str]:
    """Disc IDs in every library.json this run wrote below its isolated LOCALAPPDATA."""
    ids: list[str] = []
    for path in scratch.rglob("library.json"):
        data = json.loads(path.read_text(encoding="utf-8"))
        ids.extend(entry["disc_id"] for entry in data.get("games", []))
    return ids


class FirstRunRouteTests(unittest.TestCase):
    """First-run routes: --iso= persistence and --launch-index on an empty library.

    An inspected disc is stored only when the user chooses ADD TO LIBRARY, as the
    file picker and setup wizard already do; --launch-now is the exception, since it
    must launch and a launch needs a library record. --launch-index names a library
    entry, so an empty library is an error and bundled demo entries are named SAMPLE.
    """

    def scratch_env(self, scratch: Path) -> dict[str, str]:
        profile = scratch / "profile"
        for name in ("appdata", "localappdata", "profile"):
            (scratch / name).mkdir(exist_ok=True)
        env = os.environ.copy()
        env.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "SDL_RENDER_DRIVER": "software",
                "SDL_AUDIODRIVER": "dummy",
                "APPDATA": str(scratch / "appdata"),
                "LOCALAPPDATA": str(scratch / "localappdata"),
                "USERPROFILE": str(profile),
                "HOME": str(profile),
                "XDG_CONFIG_HOME": str(profile / "config"),
                "XDG_DATA_HOME": str(profile / "data"),
                "XDG_CACHE_HOME": str(profile / "cache"),
            }
        )
        return env

    def run_route(self, scratch: Path, args: Sequence[str]) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(PLAYER_EXE), *args, f"--runtime-root={scratch / 'runtime'}"],
            cwd=ROOT, env=self.scratch_env(scratch), capture_output=True, text=True,
            timeout=120, check=False,
        )

    def test_iso_route_stores_nothing_until_add_to_library(self) -> None:
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import create_test_iso

        with tempfile.TemporaryDirectory(prefix=".first-run-iso-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "supported.iso"
            create_test_iso(iso, disc_id="TEST00002", title="Café® ™",
                            volume_id="TEST00001")
            completed = self.run_route(scratch, [
                f"--iso={iso}",
                "--ui-test-events=WAIT_VIEW=supported,10000;QUIT",
                f"--ui-test-screenshot={scratch / 'last-frame.bmp'}",
            ])
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            views = [frame["view"] for frame in parse_frames(completed.stdout)]
            self.assertIn("supported", views, completed.stdout)
            self.assertEqual(library_disc_ids(scratch), [],
                             "the card still offers ADD TO LIBRARY, so nothing may be stored yet")

    def test_add_to_library_on_the_iso_card_stores_the_disc(self) -> None:
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import create_test_iso

        with tempfile.TemporaryDirectory(prefix=".first-run-add-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "supported.iso"
            create_test_iso(iso, disc_id="TEST00002", title="Café® ™",
                            volume_id="TEST00001")
            completed = self.run_route(scratch, [
                f"--iso={iso}",
                "--ui-test-events=WAIT_VIEW=supported,10000;KEY_RETURN;WAIT_VIEW=library,10000;QUIT",
                f"--ui-test-screenshot={scratch / 'last-frame.bmp'}",
            ])
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertEqual(library_disc_ids(scratch), ["TEST00002"])

    def test_launch_now_on_experimental_iso_reports_the_unavailable_boundary(self) -> None:
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import build_plain_mips_elf, create_test_iso_with_executables

        with tempfile.TemporaryDirectory(prefix=".first-run-experimental-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "experimental.iso"
            executable = build_plain_mips_elf() + bytes(i % 251 for i in range(70000))
            create_test_iso_with_executables(
                iso, executable, disc_id="ULUS99998", title="Experimental Fixture"
            )
            completed = self.run_route(scratch, [
                f"--iso={iso}",
                "--launch-now",
                "--ui-test-events=WAIT_MS=100;QUIT",
                f"--ui-test-screenshot={scratch / 'last-frame.bmp'}",
            ])

        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        frames = parse_frames(completed.stdout)
        self.assertTrue(frames, completed.stdout)
        self.assertEqual(frames[0]["view"], "error")
        self.assertEqual(frames[0]["error"], "EXPERIMENTAL_LAUNCH_UNAVAILABLE")
        self.assertIn("in the works (#308)", completed.stderr)
        self.assertEqual(library_disc_ids(scratch), [])

    def test_launch_index_on_an_empty_library_fails_with_a_clear_message(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".first-run-empty-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            completed = self.run_route(scratch, ["--launch-index=0", "--headless-launch"])
        self.assertEqual(completed.returncode, 2, completed.stdout + completed.stderr)
        self.assertIn("library is empty", completed.stderr)
        self.assertNotIn("has no prepared runtime", completed.stderr)

    def test_demo_launch_index_names_its_bundled_entry_as_a_sample(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".first-run-demo-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            completed = self.run_route(scratch, ["--demo", "--launch-index=0", "--headless-launch"])
        # No runtime is built in the scratch root, so the bounded launch refuses.
        self.assertEqual(completed.returncode, 3, completed.stdout + completed.stderr)
        self.assertIn("SAMPLE", completed.stderr)


if __name__ == "__main__":
    unittest.main()
