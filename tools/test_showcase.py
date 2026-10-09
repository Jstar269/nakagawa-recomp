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


def scene_log(*frames: int, tri3d: int = 12) -> str:
    """A runtime log whose SR_GESTAT windows close at `frames`."""
    return "".join(
        f"GESTAT f={frame} wall=10ms ge=4ms tri2d=0 tri3d={tri3d} px3d=4096 mw=1/1/1\n"
        for frame in frames
    ) + "ctrl_latch: vcount=12 buttons 0x0000 -> 0x4000\n"


class ShowcaseFirstFrameCheckpointTests(unittest.TestCase):
    """The checkpoint is the first window the run closed, at whatever vblank closed it."""

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
        (self.package / "snap_f00061.ppm").write_bytes(b"P6\n1 1\n255\n\0\0\0")
        checkpoint, ppm = showcase.first_frame_checkpoint(
            "TEST00007", scene_log(61, 120, 181), self.package, self.log)
        self.assertEqual(checkpoint.frame, 61)
        self.assertEqual(checkpoint.counters["tri3d"], 12)
        self.assertEqual(ppm, self.package / "snap_f00061.ppm")

    def test_no_window_is_a_missed_checkpoint(self) -> None:
        with self.assertRaisesRegex(showcase.ShowcaseError,
                                    "TEST00008 did not reach its first frame checkpoint"):
            showcase.first_frame_checkpoint("TEST00008", "BOOT_EVENT phase=init\n",
                                            self.package, self.log)

    def test_scene_without_geometry_in_the_first_window_fails(self) -> None:
        (self.package / "snap_f00060.ppm").write_bytes(b"P6\n1 1\n255\n\0\0\0")
        with self.assertRaisesRegex(showcase.ShowcaseError,
                                    r"no visible 3D geometry .*\(vblank 60\)"):
            showcase.first_frame_checkpoint("TEST00007", scene_log(60, 120, tri3d=0),
                                            self.package, self.log)

    def test_capture_must_be_the_checkpoint_window_s_own(self) -> None:
        # A capture from another window (or an earlier run) is not this checkpoint's frame.
        (self.package / "snap_f00120.ppm").write_bytes(b"P6\n1 1\n255\n\0\0\0")
        with self.assertRaisesRegex(showcase.ShowcaseError,
                                    r"no GE framebuffer capture .*\(vblank 61\)"):
            showcase.first_frame_checkpoint("TEST00007", scene_log(61, 120),
                                            self.package, self.log)

    def test_duplicate_window_breaks_the_runtime_contract(self) -> None:
        with self.assertRaisesRegex(showcase.ShowcaseError, "TEST00007 .*does not cross"):
            showcase.first_frame_checkpoint("TEST00007", scene_log(60, 60, 120),
                                            self.package, self.log)


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
