#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The SR_GESTAT window contract, from the runtime that prints it to the gates that read it.

VCOUNT advances by every elapsed display period at the scheduler's source latch, while
the vblank tick that calls ``ge_set_frame`` runs once per serviced episode. When the host
falls behind, consecutive ticks therefore step over a multiple of 60 (59 -> 61) or repeat
a value (61, 61). The GE used to close a statistics window only when ``frame % 60 == 0``,
so on a loaded host the window ending at 60 was never printed (the Linux showcase smoke
then failed "did not reach its first frame checkpoint") and a repeated multiple printed
a second, empty window.

The C harness below is synthetic (generated into a temp dir, never tracked). It drives
the production ``ge_set_frame`` in ``src/rt/ge.c`` through exactly that coalesced VCOUNT
sequence, drawing one through-mode triangle inside chosen windows, and the tests require
one ``GESTAT`` line and one ``SR_FBSNAP`` capture per crossed boundary, each carrying the
counters of its own window. The parser tests pin ``tools/ge_stat_windows.py``, which the
showcase smoke and the profile-zero route use to read the window label instead of
assuming it.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ge_stat_windows  # noqa: E402
from test_ge_nonfinite_vertex import CC, GE_HARNESS_PRELUDE_C, compile_ge_harness  # noqa: E402

HARNESS_C = GE_HARNESS_PRELUDE_C + r"""
#define LIST 0x08010000u
#define VERTS 0x08020000u

static uint32_t s_pc;
static void W(uint32_t cmd, uint32_t data) {
    uint32_t w = (cmd << 24) | (data & 0xFFFFFFu);
    memcpy(SR_HOST(s_pc), &w, 4);
    s_pc += 4;
}
static void vtx(uint32_t addr, float x, float y) {
    float f[6] = { 0.0f, 0.0f, 0.0f, x, y, 0.0f };
    uint32_t col = 0xFFFFFFFFu;
    memcpy(&f[2], &col, 4);
    memcpy(SR_HOST(addr), f, sizeof(f));
}

/* One through-mode triangle into the 8888 framebuffer at 0x04044000. */
static void emit_list(void) {
    s_pc = LIST;
    W(0x14, 0);                        /* ORIGIN */
    W(0x10, 0);                        /* BASE */
    W(0x9C, 0x044000);                 /* FRAMEBUFPTR */
    W(0x9D, 512);                      /* FRAMEBUFWIDTH */
    W(0xD2, 3);                        /* PIXFORMAT 8888 */
    W(0xD4, 0);                        /* SCISSOR1 */
    W(0xD5, (271u << 10) | 479u);      /* SCISSOR2 */
    W(0x1E, 0);                        /* TEXTUREMAPENABLE */
    W(0x4C, 0); W(0x4D, 0);            /* OFFSETX/Y */
    W(0x12, 3u | (7u << 2) | (3u << 7) | (1u << 23));  /* tc/col/pos float, through */
    W(0x01, VERTS - LIST);             /* VADDR (ORIGIN-relative) */
    W(0x04, (3u << 16) | 3u);          /* PRIM: TRIANGLES, 3 vertices */
    W(0x0C, 0);                        /* END */
    vtx(VERTS + 0u, 160.0f, 60.0f);
    vtx(VERTS + 24u, 320.0f, 60.0f);
    vtx(VERTS + 48u, 200.0f, 200.0f);
}

static int draw(void) { return ge_run_list(LIST, 0) == 0; }

/* VCOUNT as a coalescing scheduler delivers it: 1..59 one period per tick, a late host
 * stepping 59 -> 61 and servicing that two-episode batch as 61, 61, then one period per
 * tick again, an exact landing on 120 serviced twice, a one-second stall jumping
 * 120 -> 185 (two boundaries in one latch) serviced twice, and one exact landing on 240.
 * A triangle is drawn before the tick at 61 and before the second tick at 61, so the
 * windows closing at 61 and at 120 each own exactly one triangle. */
int main(void) {
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) return 2;
    g_mem = arena + 0x08000000u;
    emit_list();
    for (uint32_t f = 1; f <= 59; f++) ge_set_frame(f);
    if (!draw()) { printf("FAIL: list did not END\n"); return 1; }
    ge_set_frame(61);
    if (!draw()) { printf("FAIL: list did not END\n"); return 1; }
    ge_set_frame(61);
    for (uint32_t f = 62; f <= 120; f++) ge_set_frame(f);
    ge_set_frame(120);
    ge_set_frame(185);
    ge_set_frame(185);
    for (uint32_t f = 186; f <= 240; f++) ge_set_frame(f);
    printf("DONE\n");
    free(arena);
    return 0;
}
"""


@unittest.skipUnless(CC, "no C compiler on PATH")
class GeStatWindowEmissionTests(unittest.TestCase):
    """The production ge_set_frame closes one window per VCOUNT boundary crossing."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = Path(tempfile.mkdtemp(prefix="gestatwin_"))
        exe = compile_ge_harness(cls.tmp, HARNESS_C, "ge_stat_window_harness")
        env = dict(os.environ)
        env["SR_GESTAT"] = "1"
        env["SR_FBSNAP"] = "1"
        for name in ("SR_RTRACE", "SR_GE_TRANSITION_TRACE", "SR_NAN_TRAP", "SR_PIXWHO"):
            env.pop(name, None)
        cls.run_dir = cls.tmp / "run"
        cls.run_dir.mkdir()
        cls.result = subprocess.run([os.fspath(exe)], capture_output=True, text=True,
                                    env=env, cwd=cls.run_dir, check=False)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _windows(self) -> list[ge_stat_windows.GeStatWindow]:
        self.assertEqual(self.result.returncode, 0, self.result.stdout + self.result.stderr)
        self.assertIn("DONE", self.result.stdout)
        return ge_stat_windows.parse_windows(self.result.stderr)

    def test_each_crossed_boundary_closes_exactly_one_window(self) -> None:
        # 59 -> 61 closes the first window at 61 (an exact-modulus test printed nothing),
        # the repeated 61 and 120 print nothing more (an exact-modulus test printed a
        # second, empty 120), and the 120 -> 185 jump over two boundaries is one window.
        # Raw labels, not the parser, so a regression shows the exact lines printed.
        self.assertEqual(self.result.returncode, 0, self.result.stdout + self.result.stderr)
        frames = [int(frame) for frame in re.findall(r"GESTAT f=(\d+) ", self.result.stderr)]
        self.assertEqual(frames, [61, 120, 185, 240], self.result.stderr)

    def test_each_window_carries_its_own_counters(self) -> None:
        counters = {window.frame: window.counters for window in self._windows()}
        self.assertEqual(counters[61]["tri2d"], 1, "the triangle drawn before 61 belongs to it")
        self.assertGreater(counters[61]["px2d"], 0)
        self.assertEqual(counters[120]["tri2d"], 1, "the triangle drawn during the 61 batch")
        self.assertEqual(counters[185]["tri2d"], 0)
        self.assertEqual(counters[240]["tri2d"], 0)

    def test_framebuffer_capture_is_named_for_the_closing_vblank(self) -> None:
        windows = self._windows()
        captures = sorted(path.name for path in self.run_dir.glob("snap_f*.ppm"))
        self.assertEqual(captures, sorted(window.snapshot_name for window in windows))
        self.assertIn("snap_f00061.ppm", captures)


class VcountCadenceSourceTests(unittest.TestCase):
    """Every per-vblank cadence keyed on VCOUNT uses the shared crossing helper.

    The emission tests above prove the helper's behaviour through ge_set_frame. The vblank
    tick's own cadences (the GE log line and the periodic profile dump) share the defect
    class, and the only harness that links the tick (hle-thread-selftest) builds on
    Windows alone, so this pins the call sites on every host: an exact modulus on VCOUNT
    anywhere in the tick reintroduces the lost and doubled boundaries.
    """

    EXACT_MODULUS = re.compile(r"\((?:s_vcount|frame)\s*(?:%\s*\w+|&\s*0x[0-9a-fA-F]+)\)\s*==\s*0")

    @staticmethod
    def _body(path: Path, signature: str) -> str:
        source = path.read_text(encoding="utf-8")
        start = source.index(signature)
        depth = 0
        for index in range(start, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[start:index + 1]
        raise AssertionError(f"unbalanced body for {signature}")

    def test_vblank_tick_and_ge_window_have_no_exact_modulus(self) -> None:
        tick = self._body(ROOT / "src" / "rt" / "hle.c", "void sr_vblank_tick(void) {")
        frame = self._body(ROOT / "src" / "rt" / "ge.c", "void ge_set_frame(uint32_t frame) {")
        for name, body in (("sr_vblank_tick", tick), ("ge_set_frame", frame)):
            with self.subTest(function=name):
                self.assertIsNone(self.EXACT_MODULUS.search(body), name)
        self.assertEqual(tick.count("sr_vcount_window_crossed("), 2,
                         "the GE log cadence and the profile dump both cross boundaries")
        self.assertEqual(frame.count("sr_vcount_window_crossed("), 1)

    def test_helper_lives_beside_the_vcount_contract(self) -> None:
        header = (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8")
        declaration = header.index("void     sr_display_advance_vcount(uint32_t elapsed_periods);")
        helper = header.index("static inline int sr_vcount_window_crossed(")
        self.assertLess(declaration, helper)


class GeStatWindowParserTests(unittest.TestCase):
    """tools/ge_stat_windows.py reads the label and enforces the emission contract."""

    LOG = (
        "GESTAT f=61 wall=12ms ge=3ms tri2d=0 tri3d=12 px3d=4000 mw=1/2/3\n"
        "GESTAT+ f=61 blk3d=0 skin=0 gpuprim=0\n"
        "GESTAT* f=61 fog0=0\n"
        "ctrl_latch: vcount=12 buttons 0x0000 -> 0x4000\n"
        "partial stdout lineGESTAT f=120 wall=9ms ge=2ms tri2d=0 tri3d=24 px3d=8000\n"
    )

    def test_windows_are_read_with_their_labels_and_counters(self) -> None:
        windows = ge_stat_windows.parse_windows(self.LOG)
        self.assertEqual([window.frame for window in windows], [61, 120])
        self.assertEqual(windows[0].counters["tri3d"], 12)
        self.assertEqual(windows[0].counters["px3d"], 4000)
        self.assertNotIn("blk3d", windows[0].counters, "GESTAT+ lines are not windows")
        self.assertEqual(windows[1].counters["tri3d"], 24)

    def test_first_window_is_the_checkpoint_whatever_its_label(self) -> None:
        window = ge_stat_windows.first_window(self.LOG)
        self.assertIsNotNone(window)
        assert window is not None
        self.assertEqual(window.frame, 61)
        self.assertEqual(window.snapshot_name, "snap_f00061.ppm")
        self.assertIsNone(ge_stat_windows.first_window("BOOT_EVENT phase=init\n"))

    def test_two_lines_for_one_window_fail_closed(self) -> None:
        with self.assertRaisesRegex(ge_stat_windows.GeStatWindowError, "does not cross"):
            ge_stat_windows.parse_windows("GESTAT f=60 tri3d=1\nGESTAT f=60 tri3d=0\n")
        with self.assertRaisesRegex(ge_stat_windows.GeStatWindowError, "does not cross"):
            ge_stat_windows.parse_windows("GESTAT f=61 tri3d=1\nGESTAT f=119 tri3d=0\n")

    def test_window_before_the_first_boundary_fails_closed(self) -> None:
        with self.assertRaisesRegex(ge_stat_windows.GeStatWindowError, "before the first"):
            ge_stat_windows.parse_windows("GESTAT f=59 tri3d=1\n")


if __name__ == "__main__":
    unittest.main()
