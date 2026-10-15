# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Source-owned showcase art and disc-image regression checks."""

from __future__ import annotations

from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "fixtures" / "showcase"))
sys.path.insert(0, str(ROOT / "tools"))

import showcase
from nk_core.iso_inspect import inspect_iso


def minimal_elf() -> bytes:
    header = bytearray(52)
    header[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHIIIIIHHHHHH", header, 16,
                     2, 8, 1, 0x08804000, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    program = struct.pack("<IIIIIIII", 1, 0x100, 0x08804000, 0x08804000,
                          4, 0x100, 5, 0x1000)
    return bytes(header) + program + bytes(0x100 - 84) + b"\x13\x37\x00\x00"


SCENE = next(demo for demo in showcase.DEMOS if demo["disc_id"] == "TEST00007")
BREAKOUT = next(demo for demo in showcase.DEMOS if demo["disc_id"] == "TEST00008")
CAPTURE = b"P6\n1 1\n255\n\0\0\0"


def scene_log(*windows: tuple[int, int]) -> str:
    """A runtime log whose SR_GESTAT windows close at (vblank, tri3d) pairs."""
    return "".join(
        f"GESTAT f={frame} wall=10ms ge=4ms tri2d=0 tri3d={tri3d} spr2d=0 "
        f"px2d=130560 px3d={4096 if tri3d else 0} mw=1/1/1\n"
        for frame, tri3d in windows
    ) + "ctrl_latch: vcount=12 buttons 0x0000 -> 0x4000\n"


class ShowcaseFirstFrameCheckpointTests(unittest.TestCase):
    """The checkpoint is the guest's first drawn frame, at whatever vblank it landed."""

    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.package = Path(self._temp.name)
        self.log = self.package / "runtime.log"

    def tearDown(self) -> None:
        self._temp.cleanup()

    def test_window_closed_past_the_multiple_is_the_checkpoint(self) -> None:
        # A loaded host stepped VCOUNT 59 -> 61: the first window closes at 61 and its
        # capture is snap_f00061.ppm. The smoke used to demand the literal "GESTAT f=60"
        # and failed with "did not reach its first frame checkpoint" here.
        (self.package / "snap_f00061.ppm").write_bytes(CAPTURE)
        checkpoint, ppm = showcase.first_frame_checkpoint(
            SCENE, scene_log((61, 12), (120, 12), (181, 12)), self.package, self.log)
        self.assertEqual(checkpoint.frame, 61)
        self.assertEqual(checkpoint.counters["tri3d"], 12)
        self.assertEqual(ppm, self.package / "snap_f00061.ppm")

    def test_slow_boot_moves_the_checkpoint_to_the_first_drawn_window(self) -> None:
        # A starved host booted the guest late: its first flip came at vblank 55, so the
        # window closing at 64 holds only the clear and the cube first appears in the
        # window closing at 121. That window is the checkpoint, with its own capture.
        (self.package / "snap_f00064.ppm").write_bytes(CAPTURE)
        (self.package / "snap_f00121.ppm").write_bytes(CAPTURE)
        checkpoint, ppm = showcase.first_frame_checkpoint(
            SCENE, scene_log((64, 0), (121, 84), (180, 48)), self.package, self.log)
        self.assertEqual(checkpoint.frame, 121)
        self.assertEqual(ppm, self.package / "snap_f00121.ppm")

    def test_no_drawn_window_within_the_budget_is_a_missed_checkpoint(self) -> None:
        with self.assertRaisesRegex(
                showcase.ShowcaseError,
                r"TEST00007 did not reach its first frame checkpoint: no GE statistics window "
                r"before vblank 180 drew tri3d and px3d \(windows closed at vblank: 60, 120\)"):
            showcase.first_frame_checkpoint(SCENE, scene_log((60, 0), (120, 0)),
                                            self.package, self.log)
        with self.assertRaisesRegex(showcase.ShowcaseError,
                                    r"TEST00008 did not reach .*spr2d and px2d .*vblank: none"):
            showcase.first_frame_checkpoint(BREAKOUT, "BOOT_EVENT phase=init\n",
                                            self.package, self.log)

    def test_capture_must_be_the_checkpoint_window_s_own(self) -> None:
        # A capture from another window (or an earlier run) is not this checkpoint's frame.
        (self.package / "snap_f00120.ppm").write_bytes(CAPTURE)
        with self.assertRaisesRegex(showcase.ShowcaseError,
                                    r"no GE framebuffer capture .*\(vblank 61\)"):
            showcase.first_frame_checkpoint(SCENE, scene_log((61, 12), (120, 12)),
                                            self.package, self.log)

    def test_duplicate_window_breaks_the_runtime_contract(self) -> None:
        with self.assertRaisesRegex(showcase.ShowcaseError, "TEST00007 .*does not cross"):
            showcase.first_frame_checkpoint(SCENE, scene_log((60, 12), (60, 0), (120, 12)),
                                            self.package, self.log)

    def test_every_demo_names_the_counters_its_frame_draws(self) -> None:
        self.assertEqual(SCENE["frame_counters"], ("tri3d", "px3d"))
        self.assertEqual(BREAKOUT["frame_counters"], ("spr2d", "px2d"))


class ShowcaseScriptedPressTests(unittest.TestCase):
    """A failed check that depends on a scripted press says whether the guest read it."""

    def test_pad_script_rows_match_the_runtime_format(self) -> None:
        self.assertEqual(showcase._padscript(showcase.CROSS_PRESS, showcase.START_PRESS),
                         "12 4000 4\n240 0008 4\n")

    def test_a_press_the_guest_read_is_named_with_its_read(self) -> None:
        # Guest-time delivery holds the press until the guest reads it, so a late read is
        # normal on a loaded host; what the message needs is that the read happened.
        log = ("ctrl_latch: vcount=15 buttons 0x0000 -> 0x4000 lx=128 ly=128\n"
               "ctrl_read: vcount=57 guest read scripted 0x4000 (latched 43 samples since vblank 15)\n")
        self.assertEqual(
            showcase.scripted_press_note(log, showcase.CROSS_PRESS),
            " (the guest read the scripted press due at vblank 12 at vblank 57)")

    def test_a_press_the_guest_never_read_says_so(self) -> None:
        log = "ctrl_latch: vcount=120 buttons 0x0000 -> 0x0008 lx=128 ly=128\n"
        self.assertEqual(
            showcase.scripted_press_note(log, showcase.SAVE_START_PRESS),
            " (the guest never read the scripted press due at vblank 120)")

    def test_a_named_runtime_failure_reaches_the_smoke_message(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            cwd = Path(temp)
            log = ("ROUTE: pad script loaded\n"
                   "ROUTE_FAIL: pad script segment 2 of 4: the guest did not read the controller "
                   "while CROSS was held for 1800 vblanks\n")
            failed = subprocess.CompletedProcess(["runtime"], 86, stdout=log)
            with mock.patch.object(showcase.subprocess, "run", return_value=failed):
                with self.assertRaisesRegex(
                        showcase.ShowcaseError,
                        r"TEST00007 runtime exited 86 \(pad script segment 2 of 4: the guest did not "
                        r"read the controller while CROSS was held for 1800 vblanks\); see "):
                    showcase._run_runtime(["runtime"], {}, cwd, cwd / "runtime.log",
                                          "TEST00007 runtime")


class ShowcaseRuntimeRunTests(unittest.TestCase):
    def test_timeout_keeps_the_partial_log_and_names_it(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            cwd = Path(temp)
            log = cwd / "runtime.log"
            expired = subprocess.TimeoutExpired(["runtime"], 15, output=b"GESTAT f=60 tri3d=1\n")
            with mock.patch.object(showcase.subprocess, "run", side_effect=expired):
                with self.assertRaisesRegex(showcase.ShowcaseError,
                                            r"TEST00007 runtime exceeded the 15 second smoke window; see "):
                    showcase._run_runtime(["runtime"], {}, cwd, log, "TEST00007 runtime")
            self.assertEqual(log.read_text(encoding="utf-8"),
                             "GESTAT f=60 tri3d=1\n\nSHOWCASE_SMOKE_TIMEOUT\n")

    def test_captures_from_an_earlier_run_are_cleared_first(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            cwd = Path(temp)
            stale = cwd / "snap_f00060.ppm"
            stale.write_bytes(b"old")
            done = subprocess.CompletedProcess(["runtime"], 0, stdout="ok\n")
            with mock.patch.object(showcase.subprocess, "run", return_value=done) as run:
                output = showcase._run_runtime(["runtime"], {}, cwd, cwd / "runtime.log",
                                               "TEST00007 runtime")
            self.assertEqual(output, "ok\n")
            self.assertFalse(stale.exists())
            self.assertEqual(run.call_args.kwargs["timeout"], showcase.SMOKE_TIMEOUT_SECONDS)

    def test_exit_budget_is_three_statistics_windows(self) -> None:
        self.assertEqual(showcase.SMOKE_EXIT_VBLANK, 3 * showcase.WINDOW_VBLANKS)


class ShowcaseImageTests(unittest.TestCase):
    def test_framebuffer_metrics_require_visible_pixels(self) -> None:
        background = bytes((16, 24, 32))
        pixels = background + bytes((255, 0, 0)) + bytes((0, 255, 0)) + bytes((0, 0, 255))
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "frame.ppm"
            path.write_bytes(b"P6\n2 2\n255\n" + pixels)
            self.assertEqual(showcase._ppm_metrics(path, background), (4, 3))

    def test_ppm_size_rejects_truncated_or_non_p6_captures(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "frame.ppm"
            path.write_bytes(b"P6\n2 1\n255\n" + bytes(6))
            self.assertEqual(showcase._ppm_size(path), (2, 1))
            path.write_bytes(b"P6\n2 1\n255\n" + bytes(5))
            with self.assertRaises(showcase.ShowcaseError):
                showcase._ppm_size(path)
            path.write_bytes(b"P3\n2 1\n255\n" + bytes(6))
            with self.assertRaises(showcase.ShowcaseError):
                showcase._ppm_size(path)

    def test_headless_capture_reads_presented_and_published_vblanks_in_the_window(self) -> None:
        first, last = showcase.HEADLESS_CAPTURE_WINDOW
        output = "\n".join([
            f"HOST_PRESENT_SUBMITTED f={first - 1} buf=0x04000000 fmt=3 stride=512",
            f"HOST_PRESENT_SUBMITTED f={first} buf=0x04000000 fmt=3 stride=512",
            f"HOST_PRESENT_SUBMITTED f={last} buf=0x04000000 fmt=3 stride=512",
            f"FBSNAP f={first} swapchain capture -> frame_v{first}.ppm (result=1)",
            # A latched flip is reported at the next SetFrameBuf: the file name, not the
            # report's own f=, names the presented vblank.
            f"FBSNAP f={last + 1} swapchain capture -> frame_v{last}.ppm (result=1)",
            f"FBSNAP f={last + 2} swapchain capture -> frame_v{last + 2}.ppm (result=-1)",
        ])
        presented = showcase._window_vblanks(r"^HOST_PRESENT_SUBMITTED f=(\d+) ", output)
        published = showcase._window_vblanks(
            r"^FBSNAP f=\d+ swapchain capture -> frame_v(\d+)\.ppm \(result=1\)$", output
        )
        self.assertEqual(presented, {first, last})
        self.assertEqual(published, {first, last})

    def test_headless_capture_sets_only_the_capture_window(self) -> None:
        demo = showcase.DEMOS[0]
        with tempfile.TemporaryDirectory() as temp:
            package = Path(temp) / "packages" / str(demo["disc_id"])
            package.mkdir(parents=True)
            (package / "package.json").write_text(
                '{"executable": {"path": "game.exe"}, "runtime": {"run_entry": "0x08804000"}}',
                encoding="utf-8",
            )
            with mock.patch.object(showcase, "DEMO_ROOT", Path(temp)), \
                    mock.patch.dict("os.environ", {"SR_FBSNAP": "4", "SR_FBDUMP": "9"}):
                command, env, _ = showcase._headless_capture_command(demo, Path(temp) / "pad.txt")
        first, last = showcase.HEADLESS_CAPTURE_WINDOW
        self.assertEqual(command[-1], "--gui")
        self.assertEqual(env["SR_VIDEO"], "offscreen")
        self.assertEqual(env["SR_FBSNAP_WINDOWS"], f"{first}-{last}")
        # Host-speed independence: guest-driven vblanks and no wall-clock output cap.
        self.assertEqual(env["SR_NOVBPACE"], "1")
        self.assertEqual(env["SR_FPS_CAP"], "0")
        self.assertNotIn("SR_FBSNAP", env)
        self.assertNotIn("SR_FBDUMP", env)

    def test_icon_png_is_deterministic_144_by_80_rgba(self) -> None:
        palette = ((20, 40, 60), (80, 160, 200))
        first = showcase.make_icon_png(palette)
        second = showcase.make_icon_png(palette)
        self.assertEqual(first, second)
        self.assertTrue(first.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(struct.unpack_from(">II", first, 16), (144, 80))
        offset = 8
        raw = None
        while offset < len(first):
            size = struct.unpack_from(">I", first, offset)[0]
            kind = first[offset + 4:offset + 8]
            data = first[offset + 8:offset + 8 + size]
            if kind == b"IDAT":
                raw = zlib.decompress(data)
            offset += size + 12
        self.assertIsNotNone(raw)
        self.assertEqual(len(raw), 80 * (1 + 144 * 4))

    def test_generated_iso_has_psp_tree_and_stable_synthetic_identity(self) -> None:
        elf = minimal_elf()
        icon = showcase.make_icon_png(((10, 30, 50), (70, 150, 210)))
        notice = b"PSPSDK three-clause BSD license test notice\n"
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "showcase.iso"
            showcase.write_iso(path, "TEST00007", "Nakagawa 3D Showcase", icon, elf, notice)
            first = path.read_bytes()
            showcase.write_iso(path, "TEST00007", "Nakagawa 3D Showcase", icon, elf, notice)
            self.assertEqual(path.read_bytes(), first)
            metadata = inspect_iso(path)
        self.assertEqual(metadata.disc_id, "TEST00007")
        self.assertEqual(metadata.title, "Nakagawa 3D Showcase")

    def test_iso_builder_rejects_non_elf_eboot(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(showcase.ShowcaseError, "plaintext EBOOT"):
                showcase.write_iso(Path(temp) / "bad.iso", "TEST00007", "Bad",
                                   b"icon", b"not an ELF", b"notice")

    def test_already_based_elf_is_rejected_instead_of_packaged_as_prx(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "showcase_scene_app.prx"
            path.write_bytes(minimal_elf())
            with self.assertRaisesRegex(showcase.ShowcaseError,
                                        "not a final relocatable PSP PRX"):
                showcase.validate_psp_prx(path, 0x08804000)


if __name__ == "__main__":
    unittest.main()
