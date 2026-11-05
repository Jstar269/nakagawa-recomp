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
Path("seen_argv.json").write_text(json.dumps(sys.argv))
if mode == "exit3":
    sys.exit(3)
if mode == "noframe":
    sys.exit(0)
win = os.environ.get("SR_FBSNAP_WINDOWS")
if win:
    lo = int(win.split("-")[0])
    d = Path("." if mode == "rootdir" else "build/snapshots"); d.mkdir(parents=True, exist_ok=True)
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

    def test_corrupt_png_is_a_verdict_not_a_traceback(self):
        import struct, zlib
        good_idat = zlib.compress(b"\x00" + bytes(3))
        head = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)

        def chunk(tag, payload):
            return (struct.pack(">I", len(payload)) + tag + payload
                    + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))
        bad = self.dir / "bad.png"
        bad.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head)
                        + chunk(b"IDAT", good_idat[:-6]) + chunk(b"IEND", b""))
        with self.assertRaises(pg.PictureError):
            pg.load_picture(bad)
        ref = self.ppm("r.ppm", frame(w=1, h=1))
        self.assertEqual(pg.compare_files(ref, bad).status, pg.NOT_RUN)

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
        self.assertEqual(side["slice_us"], pg.DEFAULT_SLICE_US)
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

    def test_check_accept_stores_the_captured_frame(self):
        route = self.route()
        _, v = pg.run_route(route, 1, self.dir / "work", accept_frame=True)
        self.assertEqual(v.status, pg.ACCEPTED)
        self.assertIn("vblank=20", v.detail)
        ref = self.dir / "priv" / "ref.ppm"
        self.assertTrue(ref.is_file() and pg.sidecar_path(ref).is_file())
        self.assertEqual(self.check(route).status, pg.PASS)

    def test_accept_from_one_requested_run_still_checks_two(self):
        shared = self.dir / "counter.txt"
        flaky = self.dir / "flaky.py"
        flaky.write_text(
            "import sys\nfrom pathlib import Path\n"
            f"c = Path({str(shared)!r}); n = int(c.read_text()) if c.exists() else 0; c.write_text(str(n + 1))\n"
            + FAKE_RUNTIME.replace("int(sys.argv[2])", "int(sys.argv[2]) + n"), encoding="utf-8")
        route = self.route()
        data = json.loads(route.read_text())
        data["command"] = [sys.executable, "-I", str(flaky), "ok", "5"]
        route.write_text(json.dumps(data))
        _, v = pg.run_route(route, 1, self.dir / "work", accept_frame=True)
        self.assertEqual(v.status, pg.FAIL)
        self.assertTrue(v.detail.startswith("nondeterministic"))
        self.assertEqual(shared.read_text(), "2")  # it ran twice although runs=1 was asked
        self.assertFalse((self.dir / "priv" / "ref.ppm").exists())

    def test_check_accept_refuses_a_reference_inside_the_checkout(self):
        route = self.route()
        data = json.loads(route.read_text())
        data["reference"] = str(pg.ROOT / "tools" / "never-written.ppm")
        route.write_text(json.dumps(data))
        _, v = pg.run_route(route, 1, self.dir / "work", accept_frame=True)
        self.assertEqual(v.status, pg.NOT_RUN)
        self.assertIn("accept-refused", v.detail)
        self.assertFalse((pg.ROOT / "tools" / "never-written.ppm").exists())

    def test_frames_beside_the_working_directory_are_found(self):
        pg.accept(self.ppm("seed.ppm", frame(5)), self.dir / "priv" / "ref.ppm", commit="c")
        v = self.check(self.route("rootdir"))
        self.assertEqual((v.status, v.detail.split()[-1]), (pg.PASS, "vblank=20"))

    def test_first_frame_route_without_vblank(self):
        route = self.route(vblank=None)
        pg.accept(self.ppm("seed.ppm", frame(5)), self.dir / "priv" / "ref.ppm", commit="c")
        v = self.check(route)
        self.assertEqual(v.status, pg.PASS)
        self.assertIn("first-frame", v.detail)

    def test_missing_frame_is_not_run_but_a_crash_is_a_fail(self):
        pg.accept(self.ppm("seed.ppm", frame(5)), self.dir / "priv" / "ref.ppm", commit="c")
        crashed = self.check(self.route("exit3"))
        self.assertEqual((crashed.status, crashed.detail), (pg.FAIL, "runtime-exit-3"))
        silent = self.check(self.route("noframe"))
        self.assertEqual((silent.status, silent.detail), (pg.NOT_RUN, "no-frame-captured"))

    def test_a_timeout_is_a_fail(self):
        route = self.route()
        data = json.loads(route.read_text())
        data["command"] = [sys.executable, "-I", "-c", "import time; time.sleep(60)"]
        data["timeout"] = 1
        route.write_text(json.dumps(data))
        v = self.check(route)
        self.assertEqual((v.status, v.detail), (pg.FAIL, "timeout-1s"))

    def test_route_values_are_validated(self):
        route = self.route()
        base = json.loads(route.read_text())
        cases = [("threshold", "5"), ("threshold", -3), ("threshold", 500), ("threshold", True),
                 ("threshold", 1.5), ("max_percent", "1"), ("max_percent", 1000),
                 ("max_percent", -1), ("max_percent", True), ("timeout", 0),
                 ("timeout", "5"), ("timeout", False)]
        for key, value in cases:
            data = dict(base, **{key: value})
            route.write_text(json.dumps(data))
            with self.subTest(key=key, value=value):
                v = self.check(route)
                self.assertEqual((v.status, v.detail), (pg.NOT_RUN, f"route-{key}-invalid"))
        route.write_text(json.dumps(dict(base, threshold=0, max_percent=0, timeout=30)))
        self.assertEqual(self.check(route).status, pg.SKIP)  # valid values reach the compare

    def test_run_folder_placeholder_and_mkdirs(self):
        route = self.route(mode="ok", mkdirs=["ms", "fs/sub"], env={"SR_MEMSTICK": "{run}/ms"})
        data = json.loads(route.read_text())
        data["command"].append("{run}/arg")
        route.write_text(json.dumps(data))
        self.check(route)
        run = self.dir / "work" / "run0"
        seen = json.loads((run / "seen_env.json").read_text())
        self.assertEqual(Path(seen["SR_MEMSTICK"]), run / "ms")
        self.assertEqual(Path(json.loads((run / "seen_argv.json").read_text())[-1]), run / "arg")
        self.assertTrue((run / "ms").is_dir() and (run / "fs" / "sub").is_dir())
        for bad in (["/abs"], ["../up"]):
            data["mkdirs"] = bad
            route.write_text(json.dumps(data))
            self.assertEqual(self.check(route).detail, "route-mkdirs-invalid")

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
        # a gate that compares frames pins guest time to the guest (#2037) and publishes every
        # present of the window (as the showcase capture does), whatever the host environment says
        self.assertEqual(seen["SR_DETERMINISTIC_CLOCK"], "1")
        self.assertEqual(seen["SR_FPS_CAP"], "0")
        for gone in ("SR_FBSNAP", "SR_FLIGHT", "SR_DATAROOT"):
            self.assertNotIn(gone, seen)

    def test_the_clock_slice_is_recorded_with_the_reference(self):
        route = self.route(env={"SR_DETERMINISTIC_SLICE_US": "250"})
        self.assertEqual(self.check(route).status, pg.SKIP)
        self.assertEqual(pg.run_route(route, 2, self.dir / "work", accept_frame=True)[1].status, pg.ACCEPTED)
        side = json.loads(pg.sidecar_path(self.dir / "priv" / "ref.ppm").read_text())
        self.assertEqual(side["slice_us"], 250)

    def test_a_reference_taken_with_another_slice_says_so_when_it_differs(self):
        ref = self.dir / "priv" / "ref.ppm"
        pg.accept(self.ppm("seed.ppm", frame(9)), ref, commit="c", slice_us=50)
        v = self.check(self.route(fill=5))   # the default slice, 100
        self.assertEqual(v.status, pg.FAIL)
        self.assertIn("slice-differs(reference=50 run=100)", v.detail)
        v = pg.run_route(self.route(fill=5, env={"SR_DETERMINISTIC_SLICE_US": "50"}), 1,
                         self.dir / "work2")[1]
        self.assertEqual(v.status, pg.FAIL)
        self.assertNotIn("slice-differs", v.detail)

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

    def test_a_crash_is_a_nonzero_exit_without_strict(self):
        fake = self.dir / "fake_runtime.py"
        fake.write_text(FAKE_RUNTIME, encoding="utf-8")
        route = self.dir / "route.json"
        route.write_text(json.dumps({"command": [sys.executable, "-I", str(fake), "exit3", "5"],
                                     "reference": "ref.ppm", "vblank": 20}))
        self.assertEqual(self.run_cli("check", str(route))[0], 1)
        route.write_text(json.dumps({"command": [sys.executable, "-I", str(fake), "noframe", "5"],
                                     "reference": "ref.ppm", "vblank": 20}))
        self.assertEqual(self.run_cli("check", str(route))[0], 0)
        self.assertEqual(self.run_cli("check", str(route), "--strict")[0], 3)

    def test_display_smoke_never_deletes_a_folder_it_did_not_create(self):
        keep = self.dir / "mine"
        keep.mkdir()
        (keep / "notes.txt").write_text("unrelated", encoding="utf-8")
        code, _ = self.run_cli("display-smoke", "--build-dir", str(self.dir), "--work-dir", str(keep))
        self.assertEqual(code, 2)
        self.assertEqual((keep / "notes.txt").read_text(encoding="utf-8"), "unrelated")
        self.assertFalse((keep / pg.WORK_MARKER).exists())

    def test_work_dir_claim_rules(self):
        fresh = self.dir / "new" / "deeper"
        self.assertTrue(pg.claim_work_dir(fresh))
        (fresh / "run1").mkdir()
        (fresh / "run1" / "x.ppm").write_bytes(b"x")
        (fresh / "loose.txt").write_text("a")
        self.assertTrue(pg.claim_work_dir(fresh))  # marked by us, so it is cleared
        self.assertEqual(sorted(p.name for p in fresh.iterdir()), [pg.WORK_MARKER])
        empty = self.dir / "empty"
        empty.mkdir()
        self.assertTrue(pg.claim_work_dir(empty))
        a_file = self.dir / "file.txt"
        a_file.write_text("x")
        self.assertFalse(pg.claim_work_dir(a_file))
        self.assertEqual(a_file.read_text(), "x")

    def test_display_smoke_work_dir_must_be_outside_tracked_tree(self):
        code, _ = self.run_cli("display-smoke", "--build-dir", str(self.dir),
                               "--work-dir", str(pg.ROOT / "tools" / "scratch"))
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
