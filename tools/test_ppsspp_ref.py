# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Offline tests for tools/ppsspp_ref.py (#1882).

No PPSSPP, no disc and no network: a fake debugger stands in for PPSSPP, the WebSocket client
talks to an in-process peer over a local socket pair, and every picture is synthetic.
"""

import base64
import contextlib
from fractions import Fraction
import hashlib
import io
import json
import math
import os
import socket
import struct
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

import picture_gate  # noqa: E402
import pngcmp  # noqa: E402
import ppsspp_ref as pr  # noqa: E402

PERIOD = Fraction(1001 * 1000, 60)  # microseconds per PSP display period
W, H = 4, 2


def solid(rgb, w=W, h=H):
    return picture_gate.Frame(w, h, bytes(rgb) * (w * h))


def write_ppm(path, frame):
    Path(path).write_bytes(b"P6\n%d %d\n255\n" % (frame.width, frame.height) + frame.pixels)


def vblank_of(us):
    """The vblank whose display period contains emulated time ``us``."""
    return math.floor(Fraction(us) / PERIOD)


class FakePPSSPP:
    """A PPSSPP debugger model: emulated time moves only through cpu.runUntilTime.

    Its screenshot shows the current vblank number in pixel 0 (R low byte, G high byte) and
    whether cross is held in pixel 1, as BGRA with the rows flipped, so the driver's channel
    order and flip are exercised.
    """

    def __init__(self, pid=4242, version_has_pid=True, has_run_until=True, has_us=True,
                 stop_short_by=0, boot_us=0, overshoot_by=0):
        self.pid, self.version_has_pid = pid, version_has_pid
        self.has_run_until, self.has_us, self.stop_short_by = has_run_until, has_us, stop_short_by
        self.overshoot_by = overshoot_by
        self.us = boot_us
        self.stepping = False
        self.buttons = {}
        self.stick = None
        self.rtc = None
        self.outbox = []
        self.log = []  # (event, us, params)

    # the transport interface
    def send_text(self, text):
        msg = json.loads(text)
        event, ticket = msg.pop("event"), msg.pop("ticket", None)
        self.log.append((event, self.us, msg))
        handler = getattr(self, "on_" + event.replace(".", "_"), None)
        if handler is None:
            self.reply({"event": "error", "message": "Bad message: unknown event"}, ticket)
            return
        handler(msg, ticket)

    def recv_text(self, timeout):
        if not self.outbox:
            raise pr.CaptureFailed("fake: nothing to receive")
        return json.dumps(self.outbox.pop(0))

    def reply(self, body, ticket):
        if ticket is not None:
            body = dict(body, ticket=ticket)
        self.outbox.append(body)

    # requests
    def on_version(self, msg, t):
        body = {"event": "version", "name": "PPSSPP", "version": "v-fake"}
        if self.version_has_pid:
            body["pid"] = self.pid
        self.reply(body, t)

    def on_game_status(self, msg, t):
        self.reply({"event": "game.status", "game": {"id": "SYNTH0001"}, "paused": False}, t)

    def on_game_reset(self, msg, t):
        self.stepping = bool(msg.get("break"))
        self.buttons, self.stick = {}, None
        self.reply({"event": "game.reset"}, t)

    def on_cpu_status(self, msg, t):
        body = {"event": "cpu.status", "stepping": self.stepping, "paused": False, "pc": 0,
                "ticks": self.us * 222}
        if self.has_us:
            body["us"] = self.us
        self.reply(body, t)

    def on_replay_time_set(self, msg, t):
        self.rtc = msg["value"]
        self.reply({"event": "replay.time.set"}, t)

    def on_cpu_runUntilTime(self, msg, t):
        if not self.has_run_until:
            self.reply({"event": "error", "message": "Bad message: unknown event"}, t)
            return
        target = int(msg["us"])
        before = self.us
        self.us = max(self.us, target - self.stop_short_by + self.overshoot_by)
        self.reply({"event": "cpu.runUntilTime", "targetUs": target, "us": before}, t)
        self.outbox.append({"event": "cpu.stepping", "pc": 0, "ticks": self.us * 222})

    def on_input_buttons_send(self, msg, t):
        self.buttons.update(msg["buttons"])
        self.reply({"event": "input.buttons.send"}, t)

    def on_input_analog_send(self, msg, t):
        self.stick = (msg["x"], msg["y"])
        self.reply({"event": "input.analog.send"}, t)

    def on_gpu_buffer_screenshot(self, msg, t):
        assert msg["type"] == "base64"
        v = vblank_of(self.us)
        top = [(v & 0xFF, v >> 8, 0), (255 if self.buttons.get("cross") else 0, 7, 9)]
        top += [(1, 2, 3)] * (W - 2)
        bottom = [(50, 60, 70)] * W
        rows = [bottom, top]  # flipped: the last row first
        data = b"".join(bytes((b, g, r, 255)) for row in rows for (r, g, b) in row)
        self.reply({"event": "gpu.buffer.screenshot", "width": W, "height": H, "flipped": True,
                    "format": "B8G8R8A8_UNORM", "base64": base64.b64encode(data).decode()}, t)


def no_wait(_seconds):
    pass


class TestTiming(unittest.TestCase):
    def test_capture_is_half_a_period_after_its_vblank(self):
        for frame in (0, 1, 299, 1500, 2200, 3000):
            us = pr.capture_us(frame)
            self.assertEqual(us, round((frame + Fraction(1, 2)) * PERIOD))
            self.assertEqual(vblank_of(us), frame)

    def test_input_lands_in_the_period_before_its_vblank(self):
        self.assertEqual(pr.input_us(0), 0)
        for frame in (1, 2, 600, 1499):
            us = pr.input_us(frame)
            self.assertEqual(vblank_of(us), frame - 1, frame)
            self.assertGreater(Fraction(frame) * PERIOD - us, PERIOD / 3)


class TestPadScript(unittest.TestCase):
    def test_table_grammar(self):
        rows = pr.parse_padscript("# heading\n\n10 4000 3\n20 0x8 1 # start\n30 CROSS+up 2 200 64\n")
        self.assertEqual(rows, [pr.PadRow(10, 0x4000, 3, None), pr.PadRow(20, 0x8, 1, None),
                                pr.PadRow(30, 0x4010, 2, (200, 64))])

    def test_refusals(self):
        for text, why in (("PRESS START 4\n", "route program"),
                          ("10 4000\n", "expected"),
                          ("10 4000 3 128\n", "expected"),
                          ("10 4000 3 300 1\n", "0-255"),
                          ("10 JUMP 3\n", "not a button name"),
                          ("10 0x 3\n", "not hex"),
                          ("10 123456789 3\n", "32 bits"),
                          ("10 1000000 3\n", "cannot press"),  # the UMD sensor bit
                          ("-1 4000 3\n", "route program"),
                          ("10 4000 x\n", "decimal")):
            with self.assertRaises(pr.RefError, msg=text) as cm:
                pr.parse_padscript(text)
            self.assertIn(why, str(cm.exception), text)

    def test_states_follow_the_runtime_rule(self):
        rows = [pr.PadRow(10, 0x4000, 5, None), pr.PadRow(12, 0x8, 1, (200, 100)),
                pr.PadRow(13, 0x10, 4, (10, 20)), pr.PadRow(40, 0x20, 0, None)]
        states = pr.pad_states(rows)
        c = (128, 128)
        self.assertEqual(states, [
            pr.PadState(0, 0, c),
            pr.PadState(10, 0x4000, c),
            pr.PadState(12, 0x4008, (200, 100)),
            pr.PadState(13, 0x4010, (10, 20)),  # the row that started last holds the stick
            pr.PadState(15, 0x10, (10, 20)),
            pr.PadState(17, 0, c),
        ])

    def test_analog_round_trips_through_ppsspps_conversion(self):
        def f32(v):
            return struct.unpack("<f", struct.pack("<f", v))[0]

        for byte in range(256):
            x, y = f32(pr.analog_value(byte)), f32(pr.analog_value(byte, invert=True))
            self.assertTrue(-1.0 <= x <= 1.0 and -1.0 <= y <= 1.0)
            self.assertEqual(min(255, max(0, math.ceil(f32(x * 127.5) + 127.5))), byte)
            self.assertEqual(min(255, max(0, math.ceil(f32(-y * 127.5) + 127.5))), byte)


class TestPlan(unittest.TestCase):
    def test_presses_releases_and_captures_in_time_order(self):
        rows = pr.parse_padscript("10 CROSS 3\n")
        scenes = [pr.Scene("late", 20, None, 0, 0.0), pr.Scene("early", 9, None, 0, 0.0)]
        plan = pr.build_plan(rows, scenes)
        self.assertEqual([(a.kind, a.frame, a.buttons, a.scene) for a in plan], [
            ("input", 0, (("cross", False),), ""),
            ("capture", 9, (), "early"),
            ("input", 10, (("cross", True),), ""),
            ("input", 13, (("cross", False),), ""),
            ("capture", 20, (), "late"),
        ])
        self.assertEqual(plan[0].stick, (128, 128))
        self.assertTrue(all(a.us == pr.input_us(a.frame) for a in plan if a.kind == "input"))
        self.assertTrue(all(a.us == pr.capture_us(a.frame) for a in plan if a.kind == "capture"))

    def test_only_the_scripts_buttons_are_sent(self):
        plan = pr.build_plan(pr.parse_padscript("5 START 1\n"), [])
        sent = {k for a in plan for k, _ in a.buttons}
        self.assertEqual(sent, {"start"})

    def test_a_capture_precedes_an_input_at_the_same_time(self):
        plan = pr.build_plan(pr.parse_padscript("8 CROSS 1\n"), [pr.Scene("s", 7, None, 0, 0.0)])
        same = [a for a in plan if a.us == pr.capture_us(7)]
        self.assertEqual([a.kind for a in same], ["capture", "input"])


class TestLaunch(unittest.TestCase):
    def test_ini_pins_the_debugger_port_and_renderer(self):
        ini = pr.effective_ini(5555, {"Graphics": {"SoftwareRenderer": "False"},
                                      "General": {"RemoteISOPort": "1"}})
        self.assertEqual(ini["General"]["RemoteISOPort"], "5555")
        self.assertEqual(ini["General"]["RemoteDebuggerOnStartup"], "True")
        self.assertEqual(ini["General"]["CheckForNewVersion"], "False")
        self.assertEqual(ini["Graphics"]["SoftwareRenderer"], "False")  # a scene may override
        text = pr.render_ini(ini)
        self.assertIn("[General]\n", text)
        self.assertIn("RemoteISOPort = 5555\n", text)
        with self.assertRaises(pr.RefError):
            pr.effective_ini(1, {"General": {"RemoteDebuggerOnStartup": "False"}})

    def test_command_line(self):
        cmd = pr.build_command(Path("C:/p/PPSSPPWindows64.exe"), Path("C:/w/a.ini"), Path("D:/g.iso"))
        self.assertEqual(cmd, [str(Path("C:/p/PPSSPPWindows64.exe")),
                               f"--appendconfig={Path('C:/w/a.ini')}", str(Path("D:/g.iso"))])

    def test_missing_ppsspp_is_not_run(self):
        with self.assertRaises(pr.NotRun) as cm:
            pr.resolve_ppsspp(None, {})
        self.assertEqual(cm.exception.reason, "no-ppsspp")
        with self.assertRaises(pr.NotRun) as cm:
            pr.resolve_ppsspp(None, {"PPSSPP_EXE": str(Path(tempfile.gettempdir()) / "nope.exe")})
        self.assertEqual(cm.exception.reason, "no-ppsspp")


class TestDrive(unittest.TestCase):
    def plan(self):
        rows = pr.parse_padscript("10 CROSS 3\n")
        return pr.build_plan(rows, [pr.Scene("before", 11, None, 0, 0.0),
                                    pr.Scene("held", 12, None, 0, 0.0),
                                    pr.Scene("after", 13, None, 0, 0.0)])

    def drive(self, fake, plan=None):
        return pr.drive(pr.Debugger(fake), plan or self.plan(), 4242, 1325376000, 60,
                        pause=no_wait)

    def test_frames_and_presses_land_where_the_plan_says(self):
        fake = FakePPSSPP(boot_us=500)
        result = self.drive(fake)
        self.assertEqual(fake.rtc, 1325376000)
        self.assertEqual(result.base_us, 500)
        got = {s.scene: s for s in result.shots}
        self.assertEqual(set(got), {"before", "held", "after"})
        for shot in result.shots:
            px = shot.picture.pixels
            # boot_us shifts the fake's clock; the driver plans from the boot stop.
            self.assertEqual(vblank_of(shot.us), shot.frame)
            self.assertEqual(px[W * 3:W * 3 + 3], bytes((50, 60, 70)))  # flip: bottom row last
        held = [s.picture.pixels[3] for s in (got["before"], got["held"], got["after"])]
        self.assertEqual(held, [255, 255, 0])  # cross held for vblanks 10..12
        sends = [(us, p["buttons"]) for e, us, p in fake.log if e == "input.buttons.send"]
        self.assertEqual([vblank_of(us - 500) + 1 for us, b in sends if b.get("cross")], [10])
        self.assertEqual([vblank_of(us - 500) + 1 for us, b in sends[1:] if not b.get("cross")], [13])

    def test_bgra_channels_are_read_in_order(self):
        fake = FakePPSSPP()
        shot = self.drive(fake, pr.build_plan([], [pr.Scene("v300", 300, None, 0, 0.0)])).shots[0]
        self.assertEqual(shot.picture.pixels[0:3], bytes((300 & 0xFF, 300 >> 8, 0)))
        self.assertEqual(shot.picture.pixels[3:6], bytes((0, 7, 9)))

    def test_an_old_ppsspp_is_not_run(self):
        for fake in (FakePPSSPP(version_has_pid=False), FakePPSSPP(has_run_until=False),
                     FakePPSSPP(has_us=False)):
            with self.assertRaises(pr.NotRun) as cm:
                self.drive(fake)
            self.assertEqual(cm.exception.reason, "ppsspp-too-old")

    def test_another_process_on_the_port_fails(self):
        with self.assertRaisesRegex(pr.CaptureFailed, "not the PPSSPP the driver started"):
            self.drive(FakePPSSPP(pid=1))

    def test_an_early_stop_fails(self):
        with self.assertRaisesRegex(pr.CaptureFailed, "before the"):
            self.drive(FakePPSSPP(stop_short_by=pr.CLOCK_SLACK_US + 1))

    def test_clock_rounding_short_of_the_deadline_is_accepted(self):
        # A real PPSSPP development build stopped 1 us short of a 15 s deadline: its reported time
        # truncates to whole microseconds when its clock rebases.
        fake = FakePPSSPP(stop_short_by=1)
        result = self.drive(fake)
        self.assertEqual(result.max_early_us, 1)
        self.assertEqual(result.max_late_us, 0)
        self.assertEqual([s.frame for s in result.shots], [s.frame for s in self.drive(FakePPSSPP()).shots])

    def test_a_stop_past_the_next_vblank_fails(self):
        result = self.drive(FakePPSSPP(overshoot_by=pr.LATE_LIMIT_US))
        self.assertEqual(result.max_late_us, pr.LATE_LIMIT_US)
        with self.assertRaisesRegex(pr.CaptureFailed, "past the next vblank"):
            self.drive(FakePPSSPP(overshoot_by=pr.LATE_LIMIT_US + 1))


class TestCaptureAndCompare(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / "game.iso").write_bytes(b"synthetic")
        (self.dir / "route.pad").write_text("10 CROSS 3\n")

    def tearDown(self):
        self.tmp.cleanup()

    def scene_file(self, **extra):
        doc = {"title": "SYNTH", "disc": "game.iso", "pad": "route.pad", "out": "ref",
               "scenes": [{"name": "menu", "frame": 12, "nakagawa": "nk_v12.ppm",
                           "threshold": 0, "max_percent": 0.0}]}
        doc.update(extra)
        path = self.dir / "scenes.json"
        path.write_text(json.dumps(doc))
        return path

    def fake_runner(self, pictures):
        calls = iter(pictures)

        def runner(exe, sf, plan, work):
            self.assertTrue(work.is_dir())
            picture = next(calls)
            shots = [pr.Shot(a.scene, a.frame, a.us, picture) for a in plan if a.kind == "capture"]
            return pr.DriveResult("v-fake", 0, shots), pr.effective_ini(1234, sf.ini)
        return runner

    def test_agreeing_runs_store_a_png_reference_with_a_sidecar(self):
        sf = pr.load_scene_file(self.scene_file())
        pic = solid((10, 20, 30))
        out = pr.capture(sf, Path("ppsspp.exe"), 2, False, runner=self.fake_runner([pic, pic]))
        self.assertEqual(out, [("menu", "CAPTURED 4x2 v12 (2 runs)")])
        ref = self.dir / "ref" / "menu.png"
        w, h, rows = pngcmp.decode(ref)
        self.assertEqual((w, h, b"".join(rows)), (W, H, pic.pixels))
        meta = json.loads((self.dir / "ref" / "menu.png.json").read_text())
        self.assertEqual(meta["sha256"], hashlib.sha256(pic.pixels).hexdigest())
        self.assertEqual((meta["frame"], meta["rtc"], meta["runs"]), (12, pr.DEFAULT_RTC, 2))
        self.assertNotIn("RemoteISOPort", meta["ini"]["General"])
        with self.assertRaisesRegex(pr.RefError, "--replace"):
            pr.capture(sf, Path("ppsspp.exe"), 1, False, runner=self.fake_runner([pic]))

    def test_disagreeing_runs_store_nothing(self):
        sf = pr.load_scene_file(self.scene_file())
        old = solid((1, 1, 1))
        (self.dir / "ref").mkdir()
        pr.write_png(self.dir / "ref" / "menu.png", old)
        out = pr.capture(sf, Path("ppsspp.exe"), 2, True,
                         runner=self.fake_runner([solid((1, 2, 3)), solid((1, 2, 4))]))
        self.assertTrue(out[0][1].startswith("FAIL nondeterministic"))
        self.assertFalse((self.dir / "ref" / "menu.png").exists())

    def test_missing_disc_or_pad_is_not_run(self):
        sf = pr.load_scene_file(self.scene_file(disc="missing.iso"))
        with self.assertRaises(pr.NotRun) as cm:
            pr.capture(sf, Path("ppsspp.exe"), 1, False, runner=self.fake_runner([]))
        self.assertEqual(cm.exception.reason, "no-disc")
        sf = pr.load_scene_file(self.scene_file(pad="missing.pad"))
        with self.assertRaises(pr.NotRun) as cm:
            pr.capture(sf, Path("ppsspp.exe"), 1, False, runner=self.fake_runner([]))
        self.assertEqual(cm.exception.reason, "no-pad")

    def compare_with(self, reference, capture, **scene):
        doc_scene = {"name": "menu", "frame": 12, "nakagawa": "nk_v12.ppm"}
        doc_scene.update(scene)
        sf = pr.load_scene_file(self.scene_file(scenes=[doc_scene]))
        (self.dir / "ref").mkdir(exist_ok=True)
        if reference is not None:
            pr.write_png(self.dir / "ref" / "menu.png", reference)
        if capture is not None:
            write_ppm(self.dir / "nk_v12.ppm", capture)
        return pr.compare(sf, self.dir / "ref" / "diff")[0]

    def test_pass_fail_no_reference_and_not_run(self):
        base = solid((100, 100, 100))
        g = self.compare_with(base, base)
        self.assertEqual((g.status, g.diff), (pr.PASS, None))
        self.assertIn("0.00% over=0/8", g.detail)

        changed = bytearray(base.pixels)
        changed[3:6] = bytes((100, 100, 200))
        g = self.compare_with(base, picture_gate.Frame(W, H, bytes(changed)), threshold=36)
        self.assertEqual(g.status, pr.FAIL)
        self.assertIn("12.5000% over=1/8", g.detail)
        w, h, rows = pngcmp.decode(g.diff)
        diff = b"".join(rows)
        self.assertEqual(diff[3:6], bytes(pr.DIFF_MARK))
        self.assertEqual(diff[0:3], bytes((33, 33, 33)))  # the capture dimmed to a third

        g = self.compare_with(base, picture_gate.Frame(W, H, bytes(changed)), threshold=36,
                              max_percent=12.5)
        self.assertEqual(g.status, pr.PASS)
        g = self.compare_with(base, picture_gate.Frame(W, H, bytes(changed)), threshold=200)
        self.assertEqual(g.status, pr.PASS)  # under the threshold: differing, not over
        self.assertIn("differing=1", g.detail)

        (self.dir / "ref" / "menu.png").unlink()
        self.assertEqual(self.compare_with(None, base).status, pr.NO_REFERENCE)
        (self.dir / "nk_v12.ppm").unlink()
        g = self.compare_with(base, None)
        self.assertEqual(g.status, pr.NOT_RUN)
        self.assertTrue(g.detail.startswith("no-capture"))

    def test_a_size_mismatch_fails(self):
        g = self.compare_with(solid((5, 5, 5)), solid((5, 5, 5), w=W + 1))
        self.assertEqual(g.status, pr.FAIL)
        self.assertIn("size-mismatch", g.detail)

    def test_summary_never_passes_a_missing_result(self):
        grades = [pr.Grade("a", 1, pr.PASS)]
        self.assertEqual(pr.summary(grades)[1], pr.EXIT_OK)
        self.assertEqual(pr.summary(grades + [pr.Grade("b", 1, pr.NO_REFERENCE)])[1], pr.EXIT_NOT_PROVEN)
        self.assertEqual(pr.summary(grades + [pr.Grade("b", 1, pr.NOT_RUN)])[1], pr.EXIT_NOT_PROVEN)
        self.assertEqual(pr.summary(grades + [pr.Grade("b", 1, pr.NOT_RUN), pr.Grade("c", 1, pr.FAIL)])[1],
                         pr.EXIT_FAIL)
        self.assertIn("not a hardware oracle", pr.summary(grades)[0])

    def test_the_command_line_reports_not_run(self):
        path = self.scene_file()
        out = io.StringIO()
        with patch.dict(os.environ, {"PPSSPP_EXE": ""}), contextlib.redirect_stdout(out):
            self.assertEqual(pr.main(["capture", str(path), "--allow-ppsspp-registration"]),
                             pr.EXIT_NOT_PROVEN)
        self.assertIn("NOT_RUN no-ppsspp", out.getvalue())
        exe = self.dir / "PPSSPPWindows64.exe"
        exe.write_bytes(b"")
        out = io.StringIO()
        with patch.dict(os.environ, {"PPSSPP_EXE": str(exe)}), contextlib.redirect_stdout(out):
            self.assertEqual(pr.main(["capture", str(path)]), pr.EXIT_NOT_PROVEN)
        self.assertIn("NOT_RUN registration-not-accepted", out.getvalue())
        self.assertIn("report.ppsspp.org", out.getvalue())
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(pr.main(["capture", str(self.scene_file(disc="gone.iso")), "--ppsspp",
                                      str(exe), "--allow-ppsspp-registration"]), pr.EXIT_NOT_PROVEN)
        self.assertIn("NOT_RUN no-disc", out.getvalue())
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(pr.main(["compare", str(self.scene_file())]), pr.EXIT_NOT_PROVEN)
        self.assertIn("PPSSPP_REF menu v12: NO_REFERENCE", out.getvalue())

    def test_plan_prints_the_timeline(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(pr.main(["plan", str(self.scene_file()), "--ppsspp", "PPSSPP.exe"]), 0)
        text = out.getvalue()
        self.assertIn("--appendconfig=", text)
        self.assertIn("input cross=on", text)
        self.assertIn("capture menu", text)

    def test_scene_file_refusals(self):
        bad = ({"scenes": []}, {"scenes": [{"name": "../x", "frame": 1}]},
               {"scenes": [{"name": "a", "frame": -1}]},
               {"scenes": [{"name": "a", "frame": 1}, {"name": "a", "frame": 2}]},
               {"scenes": [{"name": "a", "frame": 1, "max_percent": 101}]},
               {"rtc": -5}, {"surprise": 1}, {"ini": {"General": {"K": "a\nb"}}},
               {"out": str(pr.ROOT / "docs" / "refs")})
        for extra in bad:
            with self.assertRaises(pr.RefError, msg=extra):
                pr.load_scene_file(self.scene_file(**extra))
        # build/ is ignored, so a reference may sit there
        pr.load_scene_file(self.scene_file(out=str(pr.ROOT / "build" / "ppsspp-ref-test")))

    def test_a_diff_folder_inside_the_checkout_is_refused(self):
        sf = pr.load_scene_file(self.scene_file())
        with self.assertRaises(pr.RefError):
            pr.compare(sf, pr.ROOT / "docs")


class TestWebSocket(unittest.TestCase):
    """The client against an in-process peer over a local socket pair (no network)."""

    def serve(self, script):
        client, server = socket.socketpair()
        client.settimeout(10)
        server.settimeout(10)
        box = {}

        def run():
            try:
                box["result"] = script(server)
            except Exception as exc:  # surfaced by the test
                box["error"] = exc
            finally:
                server.close()
        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        return client, thread, box

    @staticmethod
    def read_request(server):
        data = b""
        while b"\r\n\r\n" not in data:
            data += server.recv(4096)
        head = data.split(b"\r\n\r\n")[0].decode()
        headers = {k.lower(): v.strip() for k, _, v in (l.partition(":") for l in head.split("\r\n")[1:])}
        return head.split("\r\n")[0], headers

    @staticmethod
    def accept_reply(key, accept=None):
        accept = accept or base64.b64encode(hashlib.sha1(key.encode() + pr._WS_GUID).digest()).decode()
        return ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\nSec-WebSocket-Protocol: debugger.ppsspp.org\r\n\r\n").encode()

    @staticmethod
    def frame(opcode, payload, fin=True, masked=False):
        n = len(payload)
        b0 = (0x80 if fin else 0) | opcode
        if n < 126:
            head = struct.pack("!BB", b0, (0x80 if masked else 0) | n)
        elif n < 65536:
            head = struct.pack("!BBH", b0, (0x80 if masked else 0) | 126, n)
        else:
            head = struct.pack("!BBQ", b0, (0x80 if masked else 0) | 127, n)
        if masked:
            key = b"\x01\x02\x03\x04"
            return head + key + pr.mask_payload(payload, key)
        return head + payload

    @staticmethod
    def read_client_frame(server):
        def need(n):
            buf = b""
            while len(buf) < n:
                buf += server.recv(n - len(buf))
            return buf
        b0, b1 = need(2)
        assert b1 & 0x80, "client frames must be masked"
        n = b1 & 0x7F
        if n == 126:
            (n,) = struct.unpack("!H", need(2))
        elif n == 127:
            (n,) = struct.unpack("!Q", need(8))
        key = need(4)
        return b0 & 0x0F, pr.mask_payload(need(n), key)

    def test_handshake_fragments_ping_and_large_messages(self):
        big = json.dumps({"event": "gpu.buffer.screenshot", "base64": "A" * 70000})

        def script(server):
            line, headers = self.read_request(server)
            assert line == "GET /debugger HTTP/1.1", line
            assert headers["sec-websocket-protocol"] == "debugger.ppsspp.org"
            server.sendall(self.accept_reply(headers["sec-websocket-key"]))
            data = big.encode()
            server.sendall(self.frame(0x1, data[:40000], fin=False) + self.frame(0x9, b"hi")
                           + self.frame(0x0, data[40000:]))
            pong = self.read_client_frame(server)
            sent = self.read_client_frame(server)
            return pong, sent

        client, thread, box = self.serve(script)
        ws = pr.WebSocket.handshake(client, "127.0.0.1:1", pr.DEBUGGER_PATH, pr.DEBUGGER_PROTOCOL)
        self.assertEqual(ws.recv_text(10), big)
        ws.send_text('{"event":"cpu.status","ticket":"nk1"}')
        thread.join(10)
        client.close()
        self.assertNotIn("error", box)
        pong, sent = box["result"]
        self.assertEqual(pong, (0xA, b"hi"))
        self.assertEqual(sent, (0x1, b'{"event":"cpu.status","ticket":"nk1"}'))

    def test_a_wrong_accept_key_is_refused(self):
        def script(server):
            _, headers = self.read_request(server)
            server.sendall(self.accept_reply(headers["sec-websocket-key"], accept="d3Jvbmc="))

        client, thread, _ = self.serve(script)
        with self.assertRaisesRegex(pr.CaptureFailed, "Accept"):
            pr.WebSocket.handshake(client, "127.0.0.1:1", pr.DEBUGGER_PATH, pr.DEBUGGER_PROTOCOL)
        thread.join(10)
        client.close()

    def test_a_masked_server_frame_is_refused(self):
        def script(server):
            _, headers = self.read_request(server)
            server.sendall(self.accept_reply(headers["sec-websocket-key"])
                           + self.frame(0x1, b"{}", masked=True))

        client, thread, _ = self.serve(script)
        ws = pr.WebSocket.handshake(client, "127.0.0.1:1", pr.DEBUGGER_PATH, pr.DEBUGGER_PROTOCOL)
        with self.assertRaisesRegex(pr.CaptureFailed, "masked"):
            ws.recv_text(10)
        thread.join(10)
        client.close()

    def test_the_client_only_dials_loopback(self):
        seen = []

        def fake_connect(address, timeout=None):
            seen.append(address)
            raise OSError("refused")

        with patch.object(pr.socket, "create_connection", fake_connect):
            with self.assertRaises(OSError):
                pr.WebSocket.connect(4321, timeout=1)
        self.assertEqual(seen, [("127.0.0.1", 4321)])


class TestDebuggerProtocol(unittest.TestCase):
    def test_tickets_match_answers_and_broadcasts_wait(self):
        class Scripted:
            def __init__(self):
                self.sent = []
                self.inbox = []

            def send_text(self, text):
                msg = json.loads(text)
                self.sent.append(msg)
                self.inbox += [{"event": "log", "message": "x"},
                               {"event": msg["event"], "ticket": msg["ticket"], "ok": 1}]

            def recv_text(self, timeout):
                return json.dumps(self.inbox.pop(0))

        t = Scripted()
        dbg = pr.Debugger(t)
        self.assertEqual(dbg.request("game.status")["ok"], 1)
        self.assertEqual([e["event"] for e in dbg.events], ["log"])
        self.assertEqual(dbg.wait_event("log", 1)["message"], "x")
        self.assertEqual(t.sent[0]["ticket"], "nk1")

    def test_an_error_answer_raises(self):
        fake = FakePPSSPP()
        with self.assertRaisesRegex(pr.DebuggerError, "unknown event"):
            pr.Debugger(fake).request("no.such.event")


if __name__ == "__main__":
    unittest.main()
