# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Offline tests for tools/picture_gate.py (synthetic images and a fake runtime only)."""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

import picture_gate as pg  # noqa: E402

W, H = 6, 4

# A stand-in for the runtime: it publishes the frame the route's env asks for, the way the
# capture service does, and records the environment it was given.
FAKE_RUNTIME = r"""
import json, os, sys
from pathlib import Path
mode = sys.argv[1]
w, h, fill = 6, 4, int(sys.argv[2])
px = bytes((fill + i) & 0xFF for i in range(w * h * 3))
ppm = b"P6\n%d %d\n255\n" % (w, h) + px
Path("seen_env.json").write_text(json.dumps({k: v for k, v in os.environ.items()
                                             if k.startswith(("SR_", "SDL_"))}))
if mode == "exit3":
    sys.exit(3)
if mode == "noframe":
    sys.exit(0)
win = os.environ.get("SR_FBSNAP_WINDOWS")
if win:
    lo = int(win.split("-")[0])
    d = Path("build/snapshots"); d.mkdir(parents=True)
    # a later frame first in name order must not win over the earliest one
    (d / ("frame_v%d.ppm" % (lo + 3))).write_bytes(ppm[:-1] + b"\x00")
    (d / ("frame_v%d.ppm" % lo)).write_bytes(ppm)
else:
    Path("frame_first.ppm").write_bytes(ppm)
"""


def pixels(fill=0, w=W, h=H):
    return bytes((fill + i) & 0xFF for i in range(w * h * 3))


def frame(fill=0, w=W, h=H):
    return pg.Frame(w, h, pixels(fill, w, h))


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)

    def ppm(self, name, fr):
        path = self.dir / name
        pg.write_ppm(path, fr)
        return path


class MetricTests(Base):
    def test_identical_frames_are_zero_percent(self):
        v = pg.judge(pg.compare_frames(frame(), frame()), 0.0)
        self.assertEqual((v.status, v.over, v.differing, v.max_delta), (pg.PASS, 0, 0, 0))
        self.assertEqual(v.detail.split()[0], "0.00%")

    def test_one_changed_pixel_is_reported_as_a_failure(self):
        got = pg.perturb_one_pixel(frame())
        v = pg.judge(pg.compare_frames(frame(), got), 0.0)
        self.assertEqual(v.status, pg.FAIL)
        self.assertEqual((v.over, v.differing, v.total), (1, 1, W * H))
        self.assertEqual(v.percent, 100.0 / (W * H))
        self.assertGreater(v.max_delta, 0)

    def test_threshold_is_euclidean_rgb_distance(self):
        base = pg.Frame(1, 1, bytes([10, 10, 10]))
        near = pg.Frame(1, 1, bytes([13, 14, 10]))  # distance 5
        self.assertEqual(pg.compare_frames(base, near, 5).over, 0)
        self.assertEqual(pg.compare_frames(base, near, 4).over, 1)
        self.assertEqual(pg.compare_frames(base, near, 5).differing, 1)
        self.assertEqual(pg.compare_frames(base, near, 5).max_delta, 5)

    def test_tolerance_allows_a_small_share_but_not_more(self):
        got = pg.perturb_one_pixel(frame())
        self.assertEqual(pg.judge(pg.compare_frames(frame(), got), 5.0).status, pg.PASS)
        self.assertEqual(pg.judge(pg.compare_frames(frame(), got), 1.0).status, pg.FAIL)

    def test_size_mismatch_fails_and_never_resizes(self):
        v = pg.compare_frames(frame(), frame(w=3, h=2))
        self.assertEqual(v.status, pg.FAIL)
        self.assertIn("size-mismatch", v.detail)
        self.assertIn("6x4", v.detail)
        self.assertEqual(pg.judge(v, 100.0).status, pg.FAIL)


class FileAndVerdictTests(Base):
    def test_missing_reference_is_skip_not_pass(self):
        got = self.ppm("f.ppm", frame())
        v = pg.compare_files(self.dir / "none.ppm", got)
        self.assertEqual((v.status, v.detail), (pg.SKIP, "no-reference"))
        self.assertEqual(v.line("t"), "PICTURE_GATE t: SKIP no-reference")

    def test_unreadable_frame_is_not_run(self):
        ref = self.ppm("r.ppm", frame())
        bad = self.dir / "bad.ppm"
        bad.write_bytes(b"P6\n6 4\n255\nshort")
        self.assertEqual(pg.compare_files(ref, bad).status, pg.NOT_RUN)
        self.assertEqual(pg.compare_files(ref, self.dir / "absent.ppm").status, pg.NOT_RUN)

    def test_corrupt_reference_fails(self):
        ref = self.dir / "r.ppm"
        ref.write_bytes(b"garbage")
        v = pg.compare_files(ref, self.ppm("f.ppm", frame()))
        self.assertEqual(v.status, pg.FAIL)
        self.assertIn("unreadable-reference", v.detail)

    def test_verdict_grammar(self):
        ref = self.ppm("r.ppm", frame())
        same = pg.compare_files(ref, self.ppm("a.ppm", frame())).line("t")
        self.assertRegex(same, r"^PICTURE_GATE t: PASS 0\.00% over=0/24 differing=0 max_delta=0$")
        diff = pg.compare_files(ref, self.ppm("b.ppm", pg.perturb_one_pixel(frame()))).line("t")
        self.assertRegex(diff, r"^PICTURE_GATE t: FAIL \d+\.\d{4}% over=1/24 differing=1 ")

    def test_png_frames_are_read_too(self):
        import struct, zlib

        def chunk(tag, payload):
            return (struct.pack(">I", len(payload)) + tag + payload
                    + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))
        px = pixels()
        raw = b"".join(b"\x00" + px[y * W * 3:(y + 1) * W * 3] for y in range(H))
        png = self.dir / "f.png"
        png.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                        + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
        self.assertEqual(pg.compare_files(self.ppm("r.ppm", frame()), png).status, pg.PASS)

    def test_ppm_parser_rejects_malformed_input(self):
        for name, data in {"magic": b"P5\n1 1\n255\n\x00", "maxval": b"P6\n1 1\n65535\n\x00\x00",
                           "short": b"P6\n2 2\n255\n\x00", "long": b"P6\n1 1\n255\n\x00\x00\x00\x00",
                           "header": b"P6\n1 "}.items():
            with self.subTest(name), self.assertRaises(pg.PictureError):
                pg.parse_ppm(data)


class AcceptTests(Base):
    def test_accept_stores_frame_and_records_commit(self):
        src = self.ppm("f.ppm", frame(7))
        ref = self.dir / "priv" / "t.ppm"
        meta = pg.accept(src, ref, commit="abc123")
        self.assertEqual(ref.read_bytes(), src.read_bytes())
        side = json.loads(pg.sidecar_path(ref).read_text())
        self.assertEqual((side["commit"], side["width"], side["height"]), ("abc123", W, H))
        self.assertEqual(side["sha256"], meta["sha256"])
        self.assertEqual(pg.compare_files(ref, src).status, pg.PASS)

    def test_accept_replaces_only_when_called(self):
        ref = self.dir / "t.ppm"
        pg.accept(self.ppm("a.ppm", frame(1)), ref, commit="c1")
        before = ref.read_bytes()
        self.assertEqual(pg.compare_files(ref, self.ppm("b.ppm", frame(2))).status, pg.FAIL)
        self.assertEqual(ref.read_bytes(), before)  # a failing check never rewrites it
        pg.accept(self.ppm("b.ppm", frame(2)), ref, commit="c2")
        self.assertEqual(pg.compare_files(ref, self.dir / "b.ppm").status, pg.PASS)

    def test_accept_refuses_a_reference_inside_the_checkout(self):
        src = self.ppm("f.ppm", frame())
        inside = pg.ROOT / "tools" / "never-written-reference.ppm"
        with self.assertRaises(pg.PictureError) as ctx:
            pg.accept(src, inside, commit="x")
        self.assertIn("inside this checkout", str(ctx.exception))
        self.assertFalse(inside.exists())
        # build/ is ignored by git, so it is allowed as a scratch location.
        self.assertFalse(pg._inside_checkout(pg.ROOT / "build" / "x" / "r.ppm"))

    def test_accept_refuses_unreadable_frames_and_non_ppm_targets(self):
        bad = self.dir / "bad.ppm"
        bad.write_bytes(b"nope")
        with self.assertRaises(pg.PictureError):
            pg.accept(bad, self.dir / "r.ppm", commit="x")
        with self.assertRaises(pg.PictureError):
            pg.accept(self.ppm("f.ppm", frame()), self.dir / "r.bin", commit="x")
        self.assertFalse((self.dir / "r.ppm").exists())


class RouteTests(Base):
    def setUp(self):
        super().setUp()
        self.fake = self.dir / "fake_runtime.py"
        self.fake.write_text(FAKE_RUNTIME, encoding="utf-8")

    def route(self, mode="ok", fill=5, vblank=20, **extra):
        data = {"title": "synthetic", "command": [sys.executable, "-I", str(self.fake), mode, str(fill)],
                "reference": "priv/ref.ppm", "vblank": vblank, **extra}
        if vblank is None:
            del data["vblank"]
        path = self.dir / "route.json"
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def check(self, route, runs=1):
        title, verdict = pg.run_route(route, runs, self.dir / "work")
        return verdict

    def test_no_reference_is_skip_then_accept_then_pass_then_fail(self):
        route = self.route()
        self.assertEqual(self.check(route).line("x"), "PICTURE_GATE x: SKIP no-reference")
        # accept the frame the route captures
        captured, _ = pg.run_capture(json.loads(route.read_text())["command"],
                                     pg.sandbox_env({}, {}, 20, 8, None),
                                     self.dir / "acc", 20, 60)
        pg.accept(captured, self.dir / "priv" / "ref.ppm", commit="c")
        passed = self.check(route)
        self.assertEqual(passed.status, pg.PASS)
        self.assertTrue(passed.detail.startswith("0.00% "))
        self.assertIn("vblank=20", passed.detail)  # the earliest frame of the window
        # a different picture from the same route is a FAIL
        failed = self.check(self.route(fill=90))
        self.assertEqual(failed.status, pg.FAIL)
        self.assertGreater(failed.percent, 0)

    def test_first_frame_route_without_vblank(self):
        route = self.route(vblank=None)
        pg.accept(self.ppm("seed.ppm", frame(5)), self.dir / "priv" / "ref.ppm", commit="c")
        v = self.check(route)
        self.assertEqual(v.status, pg.PASS)
        self.assertIn("first-frame", v.detail)

    def test_runtime_failure_or_missing_frame_is_not_run(self):
        pg.accept(self.ppm("seed.ppm", frame(5)), self.dir / "priv" / "ref.ppm", commit="c")
        self.assertEqual(self.check(self.route("exit3")).detail, "runtime-exit-3")
        self.assertEqual(self.check(self.route("noframe")).detail, "no-frame-captured")
        for v in (self.check(self.route("exit3")), self.check(self.route("noframe"))):
            self.assertEqual(v.status, pg.NOT_RUN)

    def test_bad_routes_are_not_run(self):
        missing = pg.run_route(self.dir / "absent.json")[1]
        self.assertEqual(missing.status, pg.NOT_RUN)
        for body, reason in [("{", "route-not-json"), ("[]", "route-not-an-object"),
                             ('{"reference": "r.ppm"}', "route-has-no-command"),
                             ('{"command": ["x"]}', "route-has-no-reference-path"),
                             ('{"command": ["x"], "reference": "r", "vblank": 0}', "route-vblank-invalid"),
                             ('{"command": ["x"], "reference": "r", "pad": "nope.pad"}', "pad-script-missing")]:
            path = self.dir / "bad.json"
            path.write_text(body, encoding="utf-8")
            with self.subTest(reason):
                v = pg.run_route(path)[1]
                self.assertEqual((v.status, v.detail), (pg.NOT_RUN, reason))

    def test_env_is_sandboxed(self):
        pad = self.dir / "r.pad"
        pad.write_text("0 0 1\n")
        route = self.route(pad="r.pad", env={"SR_NOVBPACE": "1"})
        inherited = {"SR_FBSNAP": "5", "SR_FLIGHT": "ge", "SR_DATAROOT": "C:/x",
                     "SR_PADSCRIPT": "other.pad", "SR_VIDEO": "gdi", "SR_KEEP": "yes"}
        with patch.dict(os.environ, inherited):
            self.check(route)
        seen = json.loads((self.dir / "work" / "run0" / "seen_env.json").read_text())
        self.assertEqual(seen["SR_VIDEO"], "offscreen")
        self.assertEqual(seen["SDL_VIDEO_DRIVER"], "dummy")
        self.assertEqual(seen["SR_FBSNAP_WINDOWS"], "20-27")
        self.assertEqual(seen["SR_EXIT_AT_VBLANK"], "28")
        self.assertEqual(Path(seen["SR_PADSCRIPT"]), pad)
        self.assertEqual(seen["SR_NOVBPACE"], "1")
        self.assertEqual(seen["SR_KEEP"], "yes")
        for gone in ("SR_FBSNAP", "SR_FLIGHT", "SR_DATAROOT"):
            self.assertNotIn(gone, seen)

    def test_repeated_runs_that_differ_are_reported_not_masked(self):
        flaky = self.dir / "flaky.py"
        # each run starts in a fresh scratch folder, so make the difference depend on a shared file
        shared = self.dir / "counter.txt"
        flaky.write_text(
            "import sys\nfrom pathlib import Path\n"
            f"c = Path({str(shared)!r}); n = int(c.read_text()) if c.exists() else 0; c.write_text(str(n + 1))\n"
            + FAKE_RUNTIME.replace("int(sys.argv[2])", "int(sys.argv[2]) + n"), encoding="utf-8")
        route = self.route()
        data = json.loads(route.read_text())
        data["command"] = [sys.executable, "-I", str(flaky), "ok", "5"]
        route.write_text(json.dumps(data))
        v = self.check(route, runs=2)
        self.assertEqual(v.status, pg.FAIL)
        self.assertTrue(v.detail.startswith("nondeterministic"))


class CommandLineTests(Base):
    def run_cli(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = pg.main(list(argv))
        return code, out.getvalue()

    def test_exit_codes(self):
        ref = self.ppm("r.ppm", frame())
        same = self.ppm("a.ppm", frame())
        diff = self.ppm("b.ppm", pg.perturb_one_pixel(frame()))
        self.assertEqual(self.run_cli("compare", str(ref), str(same))[0], 0)
        code, text = self.run_cli("compare", str(ref), str(diff))
        self.assertEqual(code, 1)
        self.assertIn("FAIL", text)
        none = str(self.dir / "none.ppm")
        self.assertEqual(self.run_cli("compare", none, str(same))[0], 0)
        code, text = self.run_cli("compare", none, str(same), "--strict")
        self.assertEqual((code, text.strip()), (3, "PICTURE_GATE a: SKIP no-reference"))
        self.assertEqual(self.run_cli("compare", str(ref), str(diff), "--max-percent", "50")[0], 0)

    def test_accept_command_and_refusal(self):
        src = self.ppm("f.ppm", frame())
        code, text = self.run_cli("accept", str(src), str(self.dir / "p" / "t.ppm"), "--commit", "deadbeef")
        self.assertEqual(code, 0)
        self.assertIn("commit=deadbeef", text)
        code, _ = self.run_cli("accept", str(src), str(pg.ROOT / "tools" / "x.ppm"))
        self.assertEqual(code, 1)

    def test_display_smoke_without_a_build_is_not_run(self):
        code, text = self.run_cli("display-smoke", "--build-dir", str(self.dir / "nobuild"))
        self.assertEqual(code, 3)
        self.assertIn("NOT_RUN", text)

    def test_display_smoke_work_dir_must_be_outside_tracked_tree(self):
        code, _ = self.run_cli("display-smoke", "--build-dir", str(self.dir),
                               "--work-dir", str(pg.ROOT / "tools" / "scratch"))
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
