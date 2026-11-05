# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Offline tests for tools/ge_draw_identity.py on synthetic SR_GEWATCH logs."""

import io
import json
import pathlib
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

from tools import ge_draw_identity as gdi

FB_A = 0x04000000
FB_B = 0x04088000
RTT = 0x04110000


def xform(vblank, type_=3, count=36, vtype=0x11a, idx=0, vaddr=0x08800000, iaddr=0):
    return (f"GEWATCH frame={vblank} xform type={type_} count={count} vtype=0x{vtype:06x} idx={idx} "
            f"vaddr=0x{vaddr:08x} iaddr=0x{iaddr:08x}")


def glist(vblank, fbp, addr=0x09000000, prims=1):
    return f"GELIST f={vblank} list=0x{addr:08x} fbp=0x{fbp:08x} prims={prims} writes=0 nonblack=0 clearpx=0"


def scene(vblank, objects, fbp=FB_A):
    """One display list drawing each (vaddr, count) object, in order."""
    lines = [xform(vblank, count=c, vaddr=va) for va, c in objects]
    return lines + [glist(vblank, fbp)]


STATIC = [(0x08800000 + 0x1000 * i, 36 + 6 * i) for i in range(10)]


class ParseTests(unittest.TestCase):
    def test_lines_attach_to_the_list_that_closes_them_and_noise_is_ignored(self):
        log = ["boot noise", xform(5), "GE3D f=5 type=3", xform(5, count=6), glist(5, FB_A),
               "PRESENT f=5 buf=0x04000000 fmt=3 stride=512", xform(6)]
        t = gdi.parse(log)
        self.assertEqual(len(t.lists), 1)
        self.assertEqual([p.count for p in t.lists[0].prims], [36, 6])
        self.assertEqual(t.presents, [(5, FB_A)])
        self.assertEqual(len(t.trailing), 1)
        self.assertEqual((t.last_xform_vblank, t.last_list_vblank), (6, 5))

    def test_unknown_grouping_is_refused(self):
        with self.assertRaises(ValueError):
            gdi.group_frames(gdi.parse([]), "list")

    def test_cut_and_garbled_trace_lines_are_counted_and_a_sprite_line_is_not(self):
        log = [xform(5), xform(5, count=6)[:30], glist(5, FB_A), "GELIST f=6 list=0x0900",
               "PRESENT f=6 buf=", "GEWATCH frame=6 prim=3 spr idx=0 va=0x08800000/0x00000000 area=4",
               "GEWATCH frame=6 xform type=3 count=4 vtype=0xzz", xform(6), glist(6, FB_B)]
        t = gdi.parse(log)
        self.assertEqual(t.malformed, 4)
        self.assertEqual(t.recognised, 5)
        self.assertEqual([len(dl.prims) for dl in t.lists], [1, 1])
        self.assertEqual(gdi.summarize(t, "vblank")["malformed_lines"], 4)

    def test_prims_is_read_but_is_no_bound(self):
        # A list streamed through stall updates logs PRIMs from earlier runs than the one that
        # ends it, so more transform lines than prims= is normal and is not reported.
        log = [xform(5), xform(5), xform(5), glist(5, FB_A, prims=1), xform(6), glist(6, FB_B, prims=7)]
        t = gdi.parse(log)
        self.assertEqual([dl.prim_count for dl in t.lists], [1, 7])
        self.assertEqual(t.malformed, 0)
        self.assertEqual(gdi.summarize(t, "vblank")["frames"], 2)

    def test_an_empty_log_has_no_trace_lines(self):
        t = gdi.parse([])
        self.assertEqual((t.recognised, t.malformed, t.lists, t.trailing), (0, 0, [], []))
        self.assertEqual(gdi.summarize(t, "fbp")["frames"], 0)


class MatchTests(unittest.TestCase):
    def frames(self, *objs):
        return [[gdi.Prim(0, 3, c, 0x11a, 0, va, 0) for va, c in o] for o in objs]

    def test_static_scene_pairs_everything_in_place(self):
        a, b = self.frames(STATIC, STATIC)
        self.assertEqual(gdi.match(a, b), {i: i for i in range(len(STATIC))})

    def test_spawn_shifts_ordinals_but_the_matcher_keeps_every_old_object(self):
        spawned = STATIC[:3] + [(0x08900000, 36)] + STATIC[3:]
        a, b = self.frames(STATIC, spawned)
        pairs = gdi.match(a, b)
        self.assertNotIn(3, pairs)
        self.assertEqual(pairs, {j: (j if j < 3 else j - 1) for j in range(len(spawned)) if j != 3})
        row = gdi.pair_rows([a, b])[0]
        self.assertLess(row["ordinal"], 0.5)  # every PRIM after the spawn is out of place
        self.assertAlmostEqual(row["matched"], 10 / 11)

    def test_moved_vertex_buffers_still_pair_by_signature_between_anchors(self):
        # A ring-built object gets a new vaddr each frame; its neighbours anchor it.
        ring_a = STATIC[:4] + [(0x08a00000, 24)] + STATIC[4:]
        ring_b = STATIC[:4] + [(0x08a40000, 24)] + STATIC[4:]
        a, b = self.frames(ring_a, ring_b)
        self.assertEqual(gdi.match(a, b)[4], 4)
        row = gdi.pair_rows([a, b])[0]
        self.assertAlmostEqual(row["exact"], 10 / 11)
        self.assertEqual(row["matched"], 1.0)

    def test_a_reordered_unique_object_is_not_an_anchor_out_of_order(self):
        swapped = [STATIC[5]] + STATIC[:5] + STATIC[6:]
        a, b = self.frames(STATIC, swapped)
        pairs = gdi.match(a, b)
        # Anchors keep A's order, so the object moved to the front is not anchored there.
        self.assertNotEqual(pairs.get(0), 5)
        self.assertEqual([pairs[j] for j in range(1, 10)], [0, 1, 2, 3, 4, 6, 7, 8, 9])

    def test_a_changed_vertex_format_is_another_draw(self):
        # The object at position 4 moves its vertices and changes vtype (say, gains a texture):
        # its signature differs, so it is not paired, unlike a ring-built buffer that only moves.
        a = [gdi.Prim(0, 3, c, 0x11a, 0, va, 0) for va, c in STATIC]
        b = list(a)
        b[4] = gdi.Prim(0, 3, a[4].count, 0x11b, 0, 0x08a40000, 0)
        self.assertNotIn(4, gdi.match(a, b))
        self.assertAlmostEqual(gdi.pair_rows([a, b])[0]["matched"], 9 / 10)

    def test_vmass_weights_by_vertex_count(self):
        a, b = self.frames([(0x1000, 300)], [(0x1000, 300), (0x2000, 4)])
        row = gdi.pair_rows([a, b])[0]
        self.assertAlmostEqual(row["matched"], 0.5)
        self.assertAlmostEqual(row["vmass"], 300 / 304)

    def test_period_two_reuse_is_seen_by_exact2(self):
        even = [(0x08800000, 36), (0x08801000, 48)]
        odd = [(0x08c00000, 36), (0x08c01000, 48)]
        rows = gdi.pair_rows(self.frames(even, odd, even))
        self.assertEqual(rows[1]["exact"], 0.0)
        self.assertEqual(rows[1]["exact2"], 1.0)
        self.assertEqual(rows[1]["matched"], 1.0)


class GroupingTests(unittest.TestCase):
    def thirty_hz_log(self, frames=6):
        """A double-buffered 30 Hz title: each guest frame's lists span two vblanks."""
        log = []
        for k in range(frames):
            fbp = FB_A if k % 2 == 0 else FB_B
            v = 100 + 2 * k
            log += [xform(v, count=12, vaddr=0x08f00000), glist(v, RTT)]          # shadow pass
            log += scene(v, STATIC[:5], fbp) + scene(v + 1, STATIC[5:], fbp)
        return log

    def test_fbp_grouping_keeps_a_guest_frame_whole_and_vblank_grouping_splits_it(self):
        t = gdi.parse(self.thirty_hz_log())
        by_fbp, notes = gdi.frame_groups(t, "fbp")
        # The first guest frame is not counted: the trace cannot show that it starts with it.
        self.assertEqual([len(f) for f in by_fbp], [11] * 5)
        self.assertIn("first", notes)
        self.assertEqual(by_fbp[0][0].count, 12)  # the render-target pass opens its own frame
        self.assertEqual(len(gdi.group_frames(t, "vblank")), 12)
        s = gdi.summarize(t, "fbp")
        self.assertEqual(s["matched"]["min"], 1.0)
        self.assertEqual(s["exact"]["min"], 1.0)
        split = gdi.summarize(t, "vblank")
        self.assertLess(split["ordinal"]["min"], 1.0)

    def test_present_lines_name_the_display_buffers(self):
        # Without PRESENT lines the two busiest framebuffers would be the render target and FB_A.
        log = []
        for k in range(4):
            v = 10 + k
            log += [xform(v, count=600, vaddr=0x08f00000), xform(v, count=600, vaddr=0x08f10000),
                    glist(v, RTT)]
            log += scene(v, STATIC[:2], FB_A if k % 2 == 0 else FB_B)
            log.append(f"PRESENT f={v} buf=0x{FB_A if k % 2 == 0 else FB_B:08x} fmt=3 stride=512")
        t = gdi.parse(log)
        self.assertEqual(gdi.display_buffers(t), {FB_A, FB_B})
        self.assertEqual([len(f) for f in gdi.group_frames(t, "fbp")], [4, 4, 4])
        no_present = gdi.parse([line for line in log if not line.startswith("PRESENT")])
        self.assertIn(RTT, gdi.display_buffers(no_present))

    def test_a_trace_cut_by_the_line_limit_is_reported_not_counted_as_empty(self):
        log = []
        for k in range(4):
            log += scene(50 + k, STATIC, FB_A if k % 2 == 0 else FB_B)
        log += [glist(60 + k, FB_A) for k in range(5)]  # lists go on; transform lines stopped
        s = gdi.summarize(gdi.parse(log), "fbp")
        self.assertEqual(s["truncated_after_vblank"], 53)
        self.assertEqual(s["frames"], 3)  # 50 is the first frame; 53 was closed by the list at 60
        self.assertNotIn("dropped_last_frame", s)
        self.assertEqual(gdi.summarize(gdi.parse(log), "vblank")["frames"], 4)

    def cut_log(self, *tail):
        """Four whole frames of 10 PRIMs, then 4 PRIMs of a fifth and whatever tail follows."""
        log = []
        for k in range(4):
            log += scene(50 + k, STATIC, FB_A if k % 2 == 0 else FB_B)
        return log + [xform(54, count=c, vaddr=va) for va, c in STATIC[:4]] + list(tail)

    def test_a_cut_inside_the_last_frame_drops_that_frame_with_a_warning(self):
        # The line limit stopped the transform lines inside frame 54; its lists went on.
        log = self.cut_log(glist(54, FB_A, prims=10), glist(54, FB_A, prims=6))
        for mode, whole in (("vblank", 4), ("fbp", 3)):
            s = gdi.summarize(gdi.parse(log), mode)
            self.assertEqual(s["frames"], whole, mode)
            self.assertIn("logged no transform line", s["dropped_last_frame"])
            self.assertEqual(s["prims_per_frame"]["min"], 10)
        # Without the second list nothing shows the cut, and the frame counts.
        s = gdi.summarize(gdi.parse(self.cut_log(glist(54, FB_A, prims=4))), "vblank")
        self.assertEqual(s["frames"], 5)
        self.assertNotIn("dropped_last_frame", s)

    def test_a_cut_frame_is_dropped_even_when_the_next_frame_closes_it(self):
        log = self.cut_log(glist(54, FB_A, prims=10), glist(54, FB_A, prims=6), glist(55, FB_B, prims=9))
        s = gdi.summarize(gdi.parse(log), "fbp")
        self.assertEqual(s["frames"], 3)
        self.assertIn("dropped_last_frame", s)

    def test_a_log_that_ends_inside_a_list_drops_the_frame_it_was_in(self):
        log = self.cut_log(glist(54, FB_A, prims=4), xform(54), xform(54))
        for mode in ("vblank", "fbp"):
            s = gdi.summarize(gdi.parse(log), mode)
            self.assertEqual(s["unclosed_prims"], 2)
            self.assertIn("no GELIST line closes", s["dropped_last_frame"])
        self.assertEqual(gdi.summarize(gdi.parse(log), "vblank")["frames"], 4)


class CliTests(unittest.TestCase):
    def run_cli(self, lines, *args, text=None):
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d, "runtime.log")
            p.write_text(text if text is not None else "\n".join(lines) + "\n", encoding="utf-8")
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                rc = gdi.main([str(p), *args])
        self.err = err.getvalue()
        return rc, out.getvalue()

    def three_frames(self):
        log = []
        for k in range(3):
            log += scene(k, STATIC, FB_A if k % 2 == 0 else FB_B)
        return log

    def test_the_label_defaults_to_the_file_name_not_its_path(self):
        rc, out = self.run_cli(self.three_frames(), "--json")
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(out)["label"], "runtime.log")

    def test_a_missing_log_is_a_clear_error(self):
        out, err = io.StringIO(), io.StringIO()
        with tempfile.TemporaryDirectory() as d, redirect_stdout(out), redirect_stderr(err):
            rc = gdi.main([str(pathlib.Path(d, "absent.log"))])
        self.assertEqual(rc, 2)
        self.assertIn("cannot read the log", err.getvalue())
        self.assertNotIn("Traceback", err.getvalue())

    def test_a_log_without_trace_lines_says_so(self):
        for text in ("", "boot noise\nmore noise\n"):
            rc, out = self.run_cli([], text=text)
            self.assertEqual(rc, 1)
            self.assertIn("no GEWATCH, GELIST or PRESENT lines", self.err)

    def test_a_log_cut_mid_line_reports_the_cut_line_and_the_cut_frame(self):
        text = "\n".join(self.three_frames() + [xform(3)]) + "\n" + xform(3)[:25]
        rc, out = self.run_cli([], "--group", "vblank", text=text)
        self.assertEqual(rc, 0)
        self.assertIn("WARNING: 1 trace lines are cut or garbled", out)
        self.assertIn("note: 1 PRIMs after the last GELIST line were not counted", out)
        self.assertIn("frames 3, pairs 2", out)

    def test_a_cut_last_frame_is_a_warning_in_the_text_report(self):
        log = self.three_frames() + [xform(3), glist(3, FB_B, prims=8), glist(3, FB_B, prims=3)]
        rc, out = self.run_cli(log, "--group", "vblank")
        self.assertEqual(rc, 0)
        self.assertIn("WARNING: the last frame may be cut and is not counted", out)
        self.assertIn("frames 3, pairs 2", out)

    def test_json_report(self):
        rc, out = self.run_cli(self.three_frames(), "--json", "--label", "synthetic")
        self.assertEqual(rc, 0)
        doc = json.loads(out)
        self.assertEqual(doc["label"], "synthetic")
        self.assertEqual([s["grouping"] for s in doc["summaries"]], ["fbp", "vblank"])
        self.assertEqual(doc["summaries"][0]["vmass"]["median"], 1.0)

    def test_text_report_and_failure_without_pairs(self):
        rc, out = self.run_cli(scene(0, STATIC), "--group", "fbp")
        self.assertEqual(rc, 1)
        self.assertIn("too few frames", out)
        rc, out = self.run_cli(self.three_frames(), "--group", "fbp")
        self.assertEqual(rc, 0)
        self.assertIn("matched  median 1.000", out)
        self.assertIn("note: the first frame is not counted", out)


if __name__ == "__main__":
    unittest.main()
