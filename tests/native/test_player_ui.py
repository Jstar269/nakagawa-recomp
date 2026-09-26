# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Headless SDL regressions for the native player's real event/render loop."""

from __future__ import annotations

import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest


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
    ) -> dict[str, object]:
        with tempfile.TemporaryDirectory(prefix=".player-ui-test-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            appdata = scratch / "appdata"
            localappdata = scratch / "localappdata"
            userprofile = scratch / "profile"
            for directory in (appdata, localappdata, userprofile):
                directory.mkdir()
            profile = scratch / "controller-profile.json"
            if invalid_profile:
                profile.write_text("{ definitely not a valid controller profile", encoding="utf-8")
            runtime_root = scratch / "runtime"
            runtime_root.mkdir()

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

            args = [str(PLAYER_EXE), f"--view={view}"]
            event_script = list(events)
            if drop_invalid_iso:
                invalid_iso = scratch / "source-owned-invalid.iso"
                invalid_iso.write_bytes(b"synthetic invalid PSP disc image\n")
                event_script.insert(0, f"DROP_FILE={invalid_iso}")
            args.append("--ui-test-events=" + ";".join(event_script))
            screenshot = scratch / "last-frame.bmp"
            args.append(f"--ui-test-screenshot={screenshot}")
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

            completed = subprocess.run(
                args,
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                timeout=20,
                check=False,
            )
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
            self.assertEqual((bmp[0], bmp[1]), (1280, 720))
            return {
                "frames": frames,
                "bmp": bmp,
                "profile_exists": profile.is_file(),
                "stdout": completed.stdout,
            }

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

    def test_mouse_click_adds_disc_through_sdl_event_loop(self) -> None:
        run = self.run_player(
            "library", ("MOUSE_MOVE=400,360", "MOUSE_DOWN", "MOUSE_UP")
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[2]["picker"], "1")
        self.assertNotEqual(frames[0]["pixels"], frames[2]["pixels"])

    def test_library_card_states_include_experimental_and_ready(self) -> None:
        missing = self.run_player("library", ())
        missing_frame = missing["frames"][0]  # type: ignore[index]
        self.assertEqual(missing_frame["selected_prepared"], "0")
        self.assertEqual(missing_frame["selected_package_status"], "1")

        experimental = self.run_player("experimental-library", ())
        experimental_frame = experimental["frames"][0]  # type: ignore[index]
        self.assertEqual(experimental_frame["selected_experimental"], "1")

        ready = self.run_player("ready", (), runtime_ready=True)
        ready_frame = ready["frames"][0]  # type: ignore[index]
        self.assertEqual(ready_frame["selected_runtime"], "1")
        self.assertEqual(ready_frame["selected_prepared"], "0")
        self.assertNotEqual(missing_frame["pixels"], ready_frame["pixels"])

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

    def test_quit_from_library_runs_through_sdl_event_loop(self) -> None:
        run = self.run_player("library", ("KEY_ESCAPE",))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "library")
        self.assertEqual(frames[-1]["running"], "0")


if __name__ == "__main__":
    unittest.main()
