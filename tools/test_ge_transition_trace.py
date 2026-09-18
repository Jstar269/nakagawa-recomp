#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for the narrow GE transition trace (issue #69) and its offline diff.

The C harness below is synthetic (generated into a temp dir, never tracked): it
feeds bone/world/view/proj uploads, a weighted VTYPE, and PRIM draws through the
production GE list walker in ``src/rt/ge.c`` with ``SR_GE_TRANSITION_TRACE``
armed, then this test asserts the JSONL record fields. The diff-script test runs
``tools/ge_transition_diff.py`` over a hand-written 3-frame JSONL, also synthetic.
"""

import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RT = ROOT / "src" / "rt"
DIFF_SCRIPT = ROOT / "tools" / "ge_transition_diff.py"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")

# Synthetic harness: two frames of one weighted triangle through ge_run_list().
# Tracked C sources are untouched by this; the harness is emitted into tmp.
HARNESS_C = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp.h"
#include "debug.h"

uint8_t *g_mem;
CpuState *s_cpu = NULL;
int g_sr_heap_watch = 0;
int g_sr_metadata_watch = 0;
int g_hle_depth = 0;
int g_sr_last_writer_enabled = 0;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count = 0;
uint32_t g_sr_mem_watch_context_pc = 0;
unsigned g_sr_mem_watch_context_limit = 0;
unsigned g_sr_mem_watch_context_count = 0;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value = 0;
uint32_t g_sr_store_context_pc = 0;
unsigned g_sr_store_context_limit = 0;
unsigned g_sr_store_context_count = 0;
int g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset = 0;
unsigned g_sr_store_context_mem_words = 0;

static unsigned s_oor = 0;
void sr_oor(uint32_t a, uint32_t v, int store) { (void)a; (void)v; (void)store; s_oor++; }
void sr_heap_note_write(uint32_t a, uint32_t w, uint32_t v, uint32_t pc) {
    (void)a; (void)w; (void)v; (void)pc;
}
void sr_heap_note_bulk_write(uint32_t a, uint32_t w, uint32_t pc) {
    (void)a; (void)w; (void)pc;
}
void sr_add_mem_watch(uint32_t s, uint32_t e, const char *l) {
    (void)s; (void)e; (void)l;
}
void sr_note_mem_write(uint32_t a, uint32_t w, uint32_t v, uint32_t pc) {
    (void)a; (void)w; (void)v; (void)pc;
}
int sr_find_last_writer(uint32_t a, uint32_t w, uint32_t *pa, uint32_t *pw,
                        uint32_t *pv, uint32_t *pp) {
    (void)a; (void)w; (void)pa; (void)pw; (void)pv; (void)pp;
    return 0;
}
uint32_t sched_current_uid(void) { return 0; }
uint32_t sr_get_ge_status(void) { return 0; }
uint64_t SDL_GetTicksNS(void) { static uint64_t t = 0; return (t += 1000); }

extern void ge_set_frame(uint32_t frame);
extern uint32_t ge_run_list(uint32_t addr, int resume);

#define LIST1 0x08010000u
#define LIST2 0x08011000u
#define VERTS 0x08020000u

static uint32_t s_pc;
static uint32_t f24(float f) { uint32_t u; memcpy(&u, &f, 4); return (u >> 8) & 0xFFFFFFu; }
static void W(uint32_t cmd, uint32_t data) {
    uint32_t w = (cmd << 24) | (data & 0xFFFFFFu);
    memcpy(SR_HOST(s_pc), &w, 4);
    s_pc += 4;
}
static uint32_t emit_prim(int type, int count) {
    uint32_t a = s_pc;
    W(0x04, ((uint32_t)(type & 7) << 16) | (uint32_t)(count & 0xFFFF));
    return a;
}
static void mat_upload(uint32_t num_cmd, uint32_t data_cmd, const float *m, int n) {
    W(num_cmd, 0);
    for (int i = 0; i < n; i++) W(data_cmd, f24(m[i]));
}
static void emit_vtx(uint32_t addr, float w, float u, float v, uint32_t col,
                     float nx, float ny, float nz, float x, float y, float z) {
    float f[10];
    f[0] = w; f[1] = u; f[2] = v; memcpy(&f[3], &col, 4);
    f[4] = nx; f[5] = ny; f[6] = nz; f[7] = x; f[8] = y; f[9] = z;
    memcpy(SR_HOST(addr), f, sizeof(f));
}

/* tc=float col=8888 nrm=float pos=float wt=float wc=0(1 weight) morph=1 through=0 idx=none */
#define VT (3u | (7u << 2) | (3u << 5) | (3u << 7) | (3u << 9))

static const float kIdent43[12] = { 1,0,0, 0,1,0, 0,0,1, 0,0,0 };
static const float kIdent44[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

static void emit_common_head(uint32_t list) {
    s_pc = list;
    W(0x14, 0);                       /* ORIGIN: offset := this word */
    W(0x10, 0);                       /* BASE */
    W(0x9C, 0x044000);                /* FRAMEBUFPTR -> 0x04044000 */
    W(0x9D, 512);                     /* FRAMEBUFWIDTH stride 512 */
    W(0xD2, 3);                       /* PIXFORMAT 8888 */
    W(0xD4, 0);                       /* SCISSOR1 */
    W(0xD5, (271u << 10) | 479u);     /* SCISSOR2 */
    W(0x17, 0);                       /* LIGHTINGENABLE */
    W(0x1E, 0);                       /* TEXTUREMAPENABLE (still logs tex addr/fmt) */
    W(0xA0, 0x030000);                /* TEXADDR0 low24 */
    W(0xA8, (0x08u << 16) | 64u);     /* TEXBUFWIDTH0: high byte + stride -> 0x08030000/64 */
    W(0xB8, (6u << 8) | 6u);          /* TEXSIZE0 64x64 */
    W(0xC3, 3);                       /* TEXFORMAT 8888 */
    W(0x42, f24(240.0f)); W(0x43, f24(136.0f)); W(0x44, f24(1000.0f));
    W(0x45, f24(240.0f)); W(0x46, f24(136.0f)); W(0x47, f24(30000.0f));
    W(0x4C, 0); W(0x4D, 0);           /* OFFSETX/Y */
}

int main(int argc, char **argv) {
    static char envbuf[512];
    if (!(argc > 1 && strcmp(argv[1], "off") == 0)) {
        snprintf(envbuf, sizeof(envbuf), "SR_GE_TRANSITION_TRACE=%s", argv[1]);
        _putenv(envbuf);
    }
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) return 2;
    g_mem = arena + 0x08000000u;

    /* Frame 41: full upload, then two weighted PRIMs (second reads zero verts). */
    emit_common_head(LIST1);
    mat_upload(0x3A, 0x3B, kIdent43, 12);   /* WORLD */
    mat_upload(0x3C, 0x3D, kIdent43, 12);   /* VIEW */
    mat_upload(0x3E, 0x3F, kIdent44, 16);   /* PROJ */
    mat_upload(0x2A, 0x2B, kIdent43, 12);   /* BONE0 = identity */
    W(0x12, VT);
    W(0x01, VERTS - LIST1);                 /* VADDR (ORIGIN-relative) */
    uint32_t prim1 = emit_prim(3, 3);
    uint32_t prim2 = emit_prim(3, 3);
    W(0x0C, 0);                             /* END */
    emit_vtx(VERTS + 0u * 40u, 1.0f, 0, 0, 0xFFFFFFFFu, 0, 0, 1, -0.5f, -0.5f, 0);
    emit_vtx(VERTS + 1u * 40u, 1.0f, 1, 0, 0xFFFFFFFFu, 0, 0, 1, 0.5f, -0.5f, 0);
    emit_vtx(VERTS + 2u * 40u, 1.0f, 0, 1, 0xFFFFFFFFu, 0, 0, 1, 0.0f, 0.5f, 0);

    ge_set_frame(41);
    if (ge_run_list(LIST1, 0) != 0) { printf("list1 did not END\n"); return 1; }

    /* Frame 42: only bone0 re-uploaded, with tx = 7.5. */
    emit_common_head(LIST2);
    float bone2[12];
    memcpy(bone2, kIdent43, sizeof(bone2));
    bone2[9] = 7.5f;
    mat_upload(0x2A, 0x2B, bone2, 12);
    W(0x12, VT);
    W(0x01, VERTS - LIST2);
    uint32_t prim3 = emit_prim(3, 3);
    W(0x0C, 0);

    ge_set_frame(42);
    if (ge_run_list(LIST2, 0) != 0) { printf("list2 did not END\n"); return 1; }

    printf("LIST1=0x%08x PRIM1=0x%08x PRIM2=0x%08x VERTS=0x%08x\n",
           LIST1, prim1, prim2, VERTS);
    printf("LIST2=0x%08x PRIM3=0x%08x\n", LIST2, prim3);
    printf("OOR=%u\nDONE\n", s_oor);
    free(arena);
    return 0;
}
"""


def fnv1a_draw_id(vtype: int, vbase: int, prim: int, count: int) -> str:
    h = 2166136261
    for word in (vtype, vbase, prim, count):
        for i in range(4):
            h ^= (word >> (8 * i)) & 0xFF
            h = (h * 16777619) & 0xFFFFFFFF
    return f"{h:08x}"


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestTransitionTraceC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        cls.tmp = Path(tempfile.mkdtemp(prefix="getransition_"))
        harness_c = cls.tmp / "ge_transition_harness.c"
        harness_c.write_text(HARNESS_C, encoding="utf-8")
        cls.exe = cls.tmp / "ge_transition_harness.exe"
        result = subprocess.run(
            [CC, "-std=c11", "-O1", "-Isrc/rt", "-DSR_SDL3VK",
             "-o", os.fspath(cls.exe),
             os.fspath(harness_c),
             os.fspath(RT / "ge.c"),
             os.fspath(RT / "ge_capture.c"), "-lm"],
            capture_output=True, text=True, cwd=ROOT,
        )
        if result.returncode != 0:
            raise AssertionError("transition-trace harness did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_harness(self, arg: str) -> subprocess.CompletedProcess:
        env = dict(os.environ)
        env.pop("SR_GE_TRANSITION_TRACE", None)
        env.pop("SR_GESTAT", None)
        env.pop("SR_RTRACE", None)
        return subprocess.run([os.fspath(self.exe), arg], capture_output=True, text=True,
                              cwd=self.tmp, env=env)

    def test_weighted_draw_records(self):
        trace = self.tmp / "transition.jsonl"
        result = self.run_harness(os.fspath(trace))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DONE", result.stdout)
        summary = dict(re.findall(r"(LIST1|PRIM1|PRIM2|VERTS|LIST2|PRIM3|OOR)=(0x[0-9a-fA-F]+|\d+)",
                                  result.stdout))
        self.assertEqual(summary.get("OOR"), "0", "fixture caused out-of-range guest access")
        records = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()
                   if line.strip()]
        self.assertEqual(len(records), 3, "two weighted draws in frame 41, one in frame 42")
        list1 = int(summary["LIST1"], 16)
        verts = int(summary["VERTS"], 16)

        first, second, third = records
        # --- frame 41, first draw: full field check ---
        self.assertEqual(first["frame"], 41)
        self.assertEqual(first["draw"], 0)
        self.assertEqual(first["prim_index"], 1)
        self.assertEqual(first["list"], f"0x{list1:08x}")
        self.assertEqual(first["cmd"], f"0x{int(summary['PRIM1'], 16):08x}")
        self.assertEqual(first["prim"], 3)
        self.assertEqual(first["count"], 3)
        self.assertEqual(first["vtype"], "0x0007ff")
        self.assertEqual(first["w_fmt"], 3)
        self.assertEqual(first["w_count"], 1, "weight count uses wc+1 semantics (wc=0 -> 1)")
        self.assertEqual(first["tc_fmt"], 3)
        self.assertEqual(first["col_fmt"], 7)
        self.assertEqual(first["nrm_fmt"], 3)
        self.assertEqual(first["pos_fmt"], 3)
        self.assertEqual(first["idx_fmt"], 0)
        self.assertEqual(first["morph_n"], 1)
        self.assertEqual(first["through"], 0)
        self.assertEqual(first["vbase"], f"0x{verts:08x}")
        self.assertEqual(first["ibase"], "0x00000000")
        self.assertEqual(first["fb_ptr"], "0x04044000")
        self.assertEqual(first["fb_stride"], 512)
        self.assertEqual(first["fb_fmt"], 3)
        self.assertEqual(first["tex_enable"], 0)
        self.assertEqual(first["tex_addr"], "0x08030000")
        self.assertEqual(first["tex_fmt"], 3)
        self.assertEqual(first["draw_id"], fnv1a_draw_id(0x7FF, verts, 3, 3))
        ident43 = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]
        ident44 = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                   0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
        self.assertEqual(first["bone_effective"][:12], ident43)
        self.assertEqual(first["bone_written"], first["bone_effective"])
        self.assertEqual(first["world_effective"], ident43)
        self.assertEqual(first["world_written"], first["world_effective"])
        self.assertEqual(first["view_effective"], ident43)
        self.assertEqual(first["view_written"], first["view_effective"])
        self.assertEqual(first["proj_effective"], ident44)
        self.assertEqual(first["proj_written"], first["proj_effective"])
        self.assertEqual(first["bone_cursor"], 12)
        self.assertEqual(first["world_cursor"], 12)
        self.assertEqual(first["view_cursor"], 12)
        self.assertEqual(first["proj_cursor"], 16)
        self.assertEqual(first["bone_dropped"], 0)
        self.assertEqual(first["world_dropped"], 0)
        self.assertEqual(first["view_dropped"], 0)
        self.assertEqual(first["proj_dropped"], 0)

        # --- frame 41, second draw: ordinal advances, vertex base advances ---
        self.assertEqual(second["frame"], 41)
        self.assertEqual(second["draw"], 1)
        self.assertEqual(second["cmd"], f"0x{int(summary['PRIM2'], 16):08x}")
        self.assertEqual(second["vbase"], f"0x{verts + 120:08x}")
        self.assertEqual(second["draw_id"], fnv1a_draw_id(0x7FF, verts + 120, 3, 3))
        self.assertNotEqual(second["draw_id"], first["draw_id"])

        # --- frame 42: ordinal resets, re-uploaded bone write is visible both sides ---
        self.assertEqual(third["frame"], 42)
        self.assertEqual(third["draw"], 0)
        self.assertEqual(third["list"], f"0x{int(summary['LIST2'], 16):08x}")
        self.assertEqual(third["cmd"], f"0x{int(summary['PRIM3'], 16):08x}")
        self.assertEqual(third["draw_id"], first["draw_id"], "same draw, new frame")
        self.assertEqual(third["bone_effective"][9], 7.5)
        self.assertEqual(third["bone_written"][9], 7.5)
        self.assertEqual(third["world_effective"], ident43, "untouched state persists")

    def test_trace_off_by_default(self):
        sentinel = self.tmp / "should_not_exist.jsonl"
        result = self.run_harness("off")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DONE", result.stdout)
        self.assertNotIn("GE_TRANSITION_TRACE: armed", result.stderr)
        self.assertFalse(sentinel.exists())


def make_record(frame: int, draw: int, draw_id: str, **overrides) -> dict:
    record = {
        "frame": frame, "draw": draw, "prim_index": 1,
        "list": "0x08010000", "cmd": "0x080100a4",
        "prim": 4, "count": 6, "vtype": "0x0007ff",
        "w_fmt": 3, "w_count": 2, "tc_fmt": 3, "col_fmt": 7,
        "nrm_fmt": 3, "pos_fmt": 3, "idx_fmt": 0, "morph_n": 1, "through": 0,
        "vbase": "0x08020000", "ibase": "0x00000000",
        "fb_ptr": "0x04044000", "fb_stride": 512, "fb_fmt": 3,
        "tex_enable": 0, "tex_addr": "0x08030000", "tex_fmt": 3,
        "draw_id": draw_id,
        "bone_cursor": 12, "world_cursor": 12, "view_cursor": 12, "proj_cursor": 16,
        "bone_dropped": 0, "world_dropped": 0, "view_dropped": 0, "proj_dropped": 0,
        "bone_effective": [0.0, 1.0, 2.0, 3.0], "bone_written": [0.0, 1.0, 2.0, 3.0],
        "world_effective": [5.0], "world_written": [5.0],
        "view_effective": [0.0], "view_written": [0.0],
        "proj_effective": [1.0], "proj_written": [1.0],
    }
    record.update(overrides)
    return record


class TestTransitionDiff(unittest.TestCase):
    def run_diff(self, trace: Path, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(["python", os.fspath(DIFF_SCRIPT), os.fspath(trace), *args],
                              capture_output=True, text=True, cwd=ROOT)

    def write_three_frame_trace(self) -> Path:
        tmp = Path(tempfile.mkdtemp(prefix="getransitiondiff_"))
        self.addCleanup(shutil.rmtree, tmp, True)
        trace = tmp / "three.jsonl"
        good_a = make_record(30, 0, "deadbeef")
        good_b = make_record(30, 1, "cafef00d")
        good_gone = make_record(30, 2, "0badf00d")
        bad_a = make_record(31, 0, "deadbeef",
                            bone_effective=[0.0, 7.5, 2.0, 3.0],
                            bone_written=[0.0, 7.5, 2.0, 3.0])
        bad_b = make_record(31, 1, "cafef00d", tex_fmt=0)
        bad_new = make_record(31, 3, "00005eed")
        rec_a = make_record(32, 0, "deadbeef")
        rec_b = make_record(32, 1, "cafef00d", tex_fmt=0)
        with trace.open("w", encoding="utf-8") as fp:
            for record in (good_a, good_b, good_gone, bad_a, bad_b, bad_new, rec_a, rec_b):
                fp.write(json.dumps(record) + "\n")
        return trace

    def test_last_good_first_bad_first_recovered(self):
        result = self.run_diff(self.write_three_frame_trace(), "--frames", "30:31")
        self.assertEqual(result.returncode, 0, result.stderr)
        out = result.stdout
        self.assertIn("LAST_GOOD frame=30", out)
        self.assertIn("FIRST_BAD frame=31", out)
        # Corrupted draw: bone change shown, recovery found in frame 32.
        self.assertIn("draw_id=deadbeef", out)
        self.assertIn("FIRST_RECOVERED frame=32", out)
        self.assertIn("bone_effective: 1/4 changed", out)
        self.assertIn("[1]: 1.0 -> 7.5", out)
        self.assertIn("[1]: 7.5 -> 1.0", out)  # bad->recovered leg
        # Unrecovered draw: texture format change, no recovery.
        self.assertIn("draw_id=cafef00d", out)
        self.assertIn("tex_fmt: 3 -> 0", out)
        self.assertIn("FIRST_RECOVERED none after 31", out)
        # Draw membership changes are reported, not silently dropped.
        self.assertIn("draw_id=0badf00d", out)
        self.assertIn("MISSING in FIRST_BAD frame=31", out)
        self.assertIn("draw_id=00005eed", out)
        self.assertIn("NEW in FIRST_BAD frame=31", out)

    def test_pinned_recovery_end_cap_and_errors(self):
        trace = self.write_three_frame_trace()
        pinned = self.run_diff(trace, "--frames", "30,31", "--recovered", "32")
        self.assertEqual(pinned.returncode, 0, pinned.stderr)
        self.assertIn("FIRST_RECOVERED frame=32", pinned.stdout)
        capped = self.run_diff(trace, "--frames", "30:31", "--end", "31")
        self.assertEqual(capped.returncode, 0, capped.stderr)
        self.assertIn("FIRST_RECOVERED none in (31, 31]", capped.stdout)
        missing = self.run_diff(trace, "--frames", "30:99")
        self.assertEqual(missing.returncode, 2)
        self.assertIn("FIRST_BAD frame=99 not in trace", missing.stderr)


if __name__ == "__main__":
    unittest.main()
