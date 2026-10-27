// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge-cpu-profile-validate-selftest: ge_cpu_profile_validate() against counters produced
// by the production GE object.
//
// Every block expected to validate is written by ge.o itself: source-owned display lists
// run through ge_run_list() (the command walker, draw_prim, patch tessellation, block
// transfer and CLUT load) with the profiles switched through their environment variables
// and ge_cpu_profile_reset(), the way the replay and the runtime configure them.  This file
// changes counters only in copies made for fault injection.  It supplies guest memory, a
// deterministic monotonic clock (each read advances it, so every timed interval is
// non-empty and runs are reproducible), the runtime's diagnostic globals at their disabled
// values, and, for one scenario, a GPU backend stand-in registered through the real
// ge_set_gpu_hooks() seam.  Runtime entry points a GE list must not reach abort.
//
// Fault injection copies a production snapshot, applies one counter fault, and requires
// the failure mask to equal exactly the check(s) that fault breaks, so a fault cannot pass
// because an unrelated check fired.  Each relation, and each clause of a multi-clause
// relation, has a fault that breaks it alone.  The *_wraps faults are built so that a
// wrapping uint64_t sum would land back on the expected value; they fail only because
// those relations are compared exactly.
//
// The Makefile runs this twice, once with --strip-cache-disabled, because ge.c latches
// the strip-cache switch once per process.

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv/unsetenv under a strict -std */
#endif

#include "recomp.h"
#include "debug.h"
#include "ge_shared.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t ge_run_list(uint32_t addr, int resume);

/* ---- Runtime state at its disabled values, and entry points that must stay cold ---- */

uint8_t *g_mem;
CpuState *s_cpu;
uint32_t g_sr_debug;
int g_sr_heap_watch;
int g_sr_metadata_watch;
int g_sr_last_writer_enabled;
int g_sr_mem_watch_count;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
uint32_t g_sr_store_context_pc;
unsigned g_sr_store_context_count;
unsigned g_sr_store_context_limit;
uint32_t g_sr_mem_watch_context_pc;
unsigned g_sr_mem_watch_context_limit;
unsigned g_sr_mem_watch_context_count;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value;
int g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset;
unsigned g_sr_store_context_mem_words;

static void unexpected(const char *what, uint32_t addr) {
    fprintf(stderr, "ge_cpu_profile_validate_selftest: unexpected runtime call %s addr=0x%08" PRIx32
            " (the lists stay in range with every watch disabled)\n", what, addr);
    fflush(stderr);
    abort();
}

void sr_oor(uint32_t a, uint32_t v, int store) {
    (void)v; (void)store;
    unexpected("sr_oor", a);
}
void sr_heap_note_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)width; (void)value; (void)pc;
    unexpected("sr_heap_note_write", addr);
}
void sr_heap_note_bulk_write(uint32_t addr, uint32_t width, uint32_t pc) {
    (void)width; (void)pc;
    unexpected("sr_heap_note_bulk_write", addr);
}
void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)width; (void)value; (void)pc;
    unexpected("sr_note_mem_write", addr);
}
int sr_find_last_writer(uint32_t addr, uint32_t width, uint32_t *write_addr,
                        uint32_t *write_width, uint32_t *value, uint32_t *pc) {
    (void)width; (void)write_addr; (void)write_width; (void)value; (void)pc;
    unexpected("sr_find_last_writer", addr);
    return 0;
}
void sr_add_mem_watch(uint32_t start, uint32_t end, const char *label) {
    (void)end; (void)label;
    unexpected("sr_add_mem_watch", start);
}
uint32_t sr_get_ge_status(void) {
    unexpected("sr_get_ge_status", 0u);
    return 0u;
}

/* Deterministic monotonic clock: each read advances 7 ns. */
static uint64_t s_clock_ns = 1000000u;
uint64_t SDL_GetTicksNS(void) {
    s_clock_ns += 7u;
    return s_clock_ns;
}

static void env_set(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

/* ---- Source-owned display lists ---- */

#define LIST_MAIN     0x08100000u
#define LIST_SUB      0x08108000u
#define LIST_EMPTY    0x0810C000u
#define LIST_BAD_CLUT 0x0810E000u
#define LIST_VRAM_DEPTH 0x08110000u
#define LIST_VRAM_CPU_WRITE 0x08112000u
#define LIST_VRAM_AMBIGUOUS 0x08114000u
#define VERTS         0x08200000u
#define INDICES       0x08280000u
#define CLUT_SRC      0x08290000u
#define VRAM_TEST_VERTS 0x08300000u

/* col 8888, float normal, float position: 4 + 12 + 12 = 28 bytes per vertex. */
#define VT_TRANSFORM ((7u << 2) | (3u << 5) | (3u << 7))
#define VT_THROUGH   (VT_TRANSFORM | (1u << 23))
#define VT_INDEX8    (1u << 11)
#define VT_INDEX16   (2u << 11)
#define VSTRIDE      28u

/* View-space depths under the perspective projection below (near 1, far 100). */
#define FRONT  (-5.0f) /* w = 5: in front of the near plane */
#define BEHIND (1.0f)  /* w = -1: behind the camera */

static uint32_t s_pc;
static uint32_t s_vtx = VERTS;
static void expect(int ok, const char *scenario, const char *what);

static void W(uint32_t cmd, uint32_t data) {
    uint32_t word = (cmd << 24) | (data & 0xFFFFFFu);
    memcpy(SR_HOST(s_pc), &word, 4);
    s_pc += 4;
}

static uint32_t f24(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u >> 8;
}

static uint32_t vtx(float x, float y, float z) {
    uint32_t addr = s_vtx;
    uint32_t color = 0xFF80C040u;
    float normal[3] = { 0.0f, 0.0f, 1.0f };
    float pos[3] = { x, y, z };
    memcpy(SR_HOST(addr), &color, 4);
    memcpy(SR_HOST(addr + 4u), normal, sizeof normal);
    memcpy(SR_HOST(addr + 16u), pos, sizeof pos);
    s_vtx += VSTRIDE;
    return addr;
}

static void prim(uint32_t type, uint32_t count, uint32_t vaddr) {
    W(0x01, vaddr);                 /* VADDR (BASE supplies 0x08000000) */
    W(0x04, (type << 16) | count);  /* PRIM */
}

static void matrix(uint32_t select, uint32_t data, const float *m, int n) {
    W(select, 0);
    for (int i = 0; i < n; i++) W(data, f24(m[i]));
}

static void emit_setup(void) {
    static const float identity43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    /* Column-major perspective: w = -z, near 1, far 100. */
    static const float perspective[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, -101.0f / 99.0f, -1,
        0, 0, -200.0f / 99.0f, 0,
    };
    W(0x10, 0x080000);              /* BASE: upper address bits 0x08000000 */
    W(0x13, 0);                     /* OFFSETADDR */
    W(0x9C, 0x044000);              /* FRAMEBUFPTR -> 0x04044000 */
    W(0x9D, 512);                   /* FRAMEBUFWIDTH */
    W(0xD2, 3);                     /* PIXFORMAT 8888 */
    W(0xD4, 0);                     /* SCISSOR1 */
    W(0xD5, (271u << 10) | 479u);   /* SCISSOR2 */
    W(0x1E, 0);                     /* TEXTUREMAPENABLE */
    W(0x23, 0);                     /* ZTESTENABLE */
    W(0x1C, 1);                     /* CLIPENABLE: near-plane clipping instead of z drops */
    W(0x17, 0);                     /* LIGHTINGENABLE */
    W(0x42, f24(240.0f)); W(0x43, f24(-136.0f)); W(0x44, f24(1000.0f));
    W(0x45, f24(240.0f)); W(0x46, f24(136.0f)); W(0x47, f24(30000.0f));
    W(0x4C, 0); W(0x4D, 0);         /* OFFSETX/Y */
    matrix(0x3A, 0x3B, identity43, 12);   /* WORLD */
    matrix(0x3C, 0x3D, identity43, 12);   /* VIEW */
    matrix(0x3E, 0x3F, perspective, 16);  /* PROJ */
}

static void build_lists(void) {
    /* Transform-mode geometry. */
    uint32_t tris = vtx(-0.4f, -0.4f, FRONT);           /* triangle drawn */
    vtx(0.4f, -0.4f, FRONT); vtx(0.0f, 0.4f, FRONT);
    vtx(-0.4f, -0.4f, FRONT); vtx(0.4f, -0.4f, FRONT);   /* crosses the near plane */
    vtx(0.0f, 0.4f, BEHIND);
    vtx(-0.4f, -0.4f, BEHIND); vtx(0.4f, -0.4f, BEHIND); /* wholly behind the camera */
    vtx(0.0f, 0.4f, BEHIND);
    vtx(-15.0f, -0.4f, FRONT); vtx(0.4f, -0.4f, FRONT);  /* leaves the screen grid */
    vtx(0.0f, 0.4f, FRONT);
    vtx(0.2f, 0.2f, FRONT);                              /* leftover: no triangle */
    uint32_t strip = vtx(-0.4f, -0.4f, FRONT);
    vtx(-0.4f, 0.4f, FRONT); vtx(0.0f, -0.4f, FRONT); vtx(0.0f, 0.4f, BEHIND);
    vtx(0.4f, -0.4f, FRONT); vtx(0.4f, 0.4f, FRONT);
    uint32_t fan = vtx(0.0f, 0.0f, FRONT);
    vtx(-0.4f, -0.4f, FRONT); vtx(0.4f, -0.4f, FRONT); vtx(0.4f, 0.4f, BEHIND);
    vtx(-0.4f, 0.4f, FRONT);
    uint32_t points = vtx(-0.2f, -0.2f, FRONT);
    vtx(0.2f, 0.2f, BEHIND); vtx(-15.0f, 0.0f, FRONT); vtx(0.2f, -0.2f, FRONT);
    uint32_t sprites = vtx(-0.3f, -0.3f, BEHIND);        /* first corner dropped */
    vtx(0.3f, 0.3f, FRONT);
    vtx(-0.3f, -0.3f, FRONT); vtx(0.3f, 0.3f, BEHIND);   /* second corner dropped */
    vtx(-0.3f, -0.3f, FRONT); vtx(0.3f, 0.3f, FRONT);
    uint32_t lines = vtx(-0.4f, 0.0f, FRONT);
    vtx(0.4f, 0.0f, FRONT); vtx(0.0f, -0.4f, FRONT); vtx(0.0f, 0.4f, FRONT);
    vtx(0.1f, 0.1f, FRONT);
    uint32_t line_strip = vtx(-0.4f, 0.0f, FRONT);
    vtx(0.0f, 0.4f, FRONT); vtx(0.0f, 0.0f, BEHIND); vtx(0.4f, 0.0f, FRONT);
    uint32_t patch = s_vtx;                              /* 4x4 control grid */
    for (int v = 0; v < 4; v++)
        for (int u = 0; u < 4; u++)
            vtx(-0.3f + 0.2f * (float)u, -0.3f + 0.2f * (float)v, FRONT);

    /* Through-mode geometry: screen pixels. */
    uint32_t t_points = vtx(10.0f, 10.0f, 0.0f);
    vtx(20.0f, 10.0f, 0.0f); vtx(30.0f, 10.0f, 0.0f);
    uint32_t t_lines = vtx(10.0f, 20.0f, 0.0f);
    vtx(40.0f, 20.0f, 0.0f); vtx(10.0f, 25.0f, 0.0f); vtx(40.0f, 25.0f, 0.0f);
    vtx(10.0f, 30.0f, 0.0f);
    uint32_t t_line_strip = vtx(50.0f, 20.0f, 0.0f);
    vtx(60.0f, 30.0f, 0.0f); vtx(70.0f, 20.0f, 0.0f); vtx(80.0f, 30.0f, 0.0f);
    uint32_t t_tris = vtx(10.0f, 40.0f, 0.0f);
    vtx(30.0f, 40.0f, 0.0f); vtx(20.0f, 60.0f, 0.0f);
    vtx(40.0f, 40.0f, 0.0f); vtx(60.0f, 40.0f, 0.0f); vtx(50.0f, 60.0f, 0.0f);
    vtx(70.0f, 40.0f, 0.0f);
    uint32_t t_strip = vtx(10.0f, 70.0f, 0.0f);
    vtx(10.0f, 90.0f, 0.0f); vtx(30.0f, 70.0f, 0.0f); vtx(30.0f, 90.0f, 0.0f);
    vtx(50.0f, 70.0f, 0.0f); vtx(50.0f, 90.0f, 0.0f);
    uint32_t t_fan = vtx(80.0f, 80.0f, 0.0f);
    vtx(70.0f, 70.0f, 0.0f); vtx(90.0f, 70.0f, 0.0f); vtx(90.0f, 90.0f, 0.0f);
    vtx(70.0f, 90.0f, 0.0f);
    uint32_t t_sprites = vtx(100.0f, 10.0f, 0.0f);
    vtx(120.0f, 30.0f, 0.0f); vtx(130.0f, 10.0f, 0.0f); vtx(150.0f, 30.0f, 0.0f);
    vtx(160.0f, 10.0f, 0.0f);

    /* Index buffers: an 8-bit transform strip into the triangle vertices and a 16-bit
     * through strip into the through strip vertices. */
    static const uint8_t idx8[5] = { 0, 1, 2, 12, 0 };
    static const uint16_t idx16[4] = { 0, 1, 2, 3 };
    memcpy(SR_HOST(INDICES), idx8, sizeof idx8);
    memcpy(SR_HOST(INDICES + 16u), idx16, sizeof idx16);

    for (uint32_t i = 0; i < 16u; i++) {               /* CLUT source words */
        uint32_t word = 0xFF000000u | i;
        memcpy(SR_HOST(CLUT_SRC + i * 4u), &word, 4);
    }

    s_pc = LIST_SUB;                                     /* CALL target */
    W(0x12, VT_THROUGH);
    prim(3, 3, t_tris);
    W(0x0B, 0);                                          /* RET */

    s_pc = LIST_MAIN;
    emit_setup();
    W(0x12, VT_TRANSFORM);
    prim(3, 13, tris);
    prim(4, 6, strip);
    prim(5, 5, fan);
    prim(0, 4, points);
    prim(6, 6, sprites);
    prim(1, 5, lines);
    prim(2, 4, line_strip);
    W(0x17, 1);                                          /* LIGHTINGENABLE for one draw */
    W(0x12, VT_TRANSFORM | VT_INDEX8);
    W(0x02, INDICES);                                    /* IADDR */
    prim(4, 5, tris);
    W(0x17, 0);
    W(0x12, VT_TRANSFORM);
    prim(7, 3, tris);                                    /* reserved type: no primitives */
    prim(3, 0, tris);
    prim(4, 2, strip);                                   /* strip too short for a triangle */
    W(0x01, patch);
    W(0x36, (2u << 8) | 2u);                             /* PATCHDIVISION 2x2 */
    W(0x37, 0);                                          /* PATCHPRIMITIVE triangles */
    W(0x05, 0x0404);                                     /* BEZIER 4x4 */
    W(0x01, patch);
    W(0x36, (1u << 8) | 1u);
    W(0x37, 2);                                          /* PATCHPRIMITIVE points */
    W(0x06, 0x0404);                                     /* SPLINE 4x4 */
    W(0x12, VT_THROUGH);
    prim(0, 3, t_points);
    prim(1, 5, t_lines);
    prim(2, 4, t_line_strip);
    prim(3, 7, t_tris);
    prim(4, 6, t_strip);
    prim(5, 5, t_fan);
    prim(6, 5, t_sprites);
    prim(7, 2, t_points);
    W(0x12, VT_THROUGH | VT_INDEX16);
    W(0x02, INDICES + 16u);
    prim(4, 4, t_strip);
    W(0xB0, CLUT_SRC & 0xFFFFFFu);                       /* CLUTADDR */
    W(0xB1, (CLUT_SRC >> 8) & 0xFF0000u);                /* CLUTADDRUPPER */
    W(0xC4, 2);                                          /* LOADCLUT: 2 blocks of 32 bytes */
    W(0xB2, 0x100000); W(0xB3, (0x04u << 16) | 64u);     /* TRANSFERSRC 0x04100000/64 */
    W(0xB4, 0x140000); W(0xB5, (0x04u << 16) | 64u);     /* TRANSFERDST 0x04140000/64 */
    W(0xEB, 0); W(0xEC, 0);                              /* TRANSFERSRCPOS/DSTPOS */
    W(0xEE, (7u << 10) | 7u);                            /* TRANSFERSIZE 8x8 */
    W(0xEA, 0);                                          /* TRANSFERSTART 16-bit */
    W(0x0A, LIST_SUB & 0xFFFFFFu);                       /* CALL */
    W(0x0F, 0);                                          /* FINISH */
    W(0x0C, 0);                                          /* END */

    s_pc = LIST_EMPTY;
    W(0x0C, 0);

    /* A CLUT load whose source span leaves the guest arena: production ends the clut_load
     * phase without counting a load (and reports the span on stderr). */
    s_pc = LIST_BAD_CLUT;
    W(0x10, 0x080000);
    W(0xB0, 0xFFFFE0);
    W(0xB1, 0x0B0000);                                   /* 0x0BFFFFE0 */
    W(0xC4, 0x3F);
    W(0x0C, 0);
}

static uint32_t depth_test_vertex(uint32_t address, uint16_t x, uint16_t y,
                                  uint16_t z, uint32_t color) {
    MEM_W32(address, color);
    MEM_W16(address + 4u, x);
    MEM_W16(address + 6u, y);
    MEM_W16(address + 8u, z);
    return address + 12u;                 /* 4-byte color alignment pads the vertex stride */
}

static void build_vram_depth_list_at(uint32_t zbp) {
    static const uint32_t through16 = (7u << 2) | (2u << 7) | (1u << 23);
    uint32_t vertex = VRAM_TEST_VERTS;
    uint32_t clear_sprite = vertex;
    vertex = depth_test_vertex(vertex, 0u, 0u, 0u, 0xFF646464u);
    vertex = depth_test_vertex(vertex, 480u, 272u, 0u, 0xFF646464u);
    uint32_t sprite_a = vertex;
    vertex = depth_test_vertex(vertex, 0u, 0u, 0x1111u, 0xFF101010u);
    vertex = depth_test_vertex(vertex, 32u, 16u, 0x1111u, 0xFF101010u);
    uint32_t sprite_b = vertex;
    vertex = depth_test_vertex(vertex, 64u, 8u, 0x2222u, 0xFF202020u);
    vertex = depth_test_vertex(vertex, 96u, 12u, 0x2222u, 0xFF202020u);
    uint32_t sprite_c = vertex;
    vertex = depth_test_vertex(vertex, 200u, 100u, 0x3333u, 0xFF303030u);
    (void)depth_test_vertex(vertex, 208u, 101u, 0x3333u, 0xFF303030u);

    s_pc = LIST_VRAM_DEPTH;
    W(0x10, 0x080000);
    W(0x9C, 0x00100000u);                /* colour base: 0x04100000 */
    W(0x9D, 512u);
    W(0xD2, 3u);                         /* 8888 */
    W(0x9E, zbp);                        /* depth base */
    W(0x9F, 512u);
    W(0xD4, 0u);
    W(0xD5, (271u << 10) | 479u);
    W(0x23, 1u);                         /* depth test enabled */
    W(0xDE, 1u);                         /* always pass */
    W(0x12, through16);
    W(0xD3, 1u | 0x100u | 0x200u | 0x400u); /* clear colour, alpha, depth */
    prim(6u, 2u, clear_sprite);
    W(0xD3, 0u);
    prim(6u, 2u, sprite_a);
    prim(6u, 2u, sprite_b);
    prim(6u, 2u, sprite_c);
    W(0x0C, 0u);
}

static void build_vram_cpu_write_list(void) {
    static const uint32_t through16 = (7u << 2) | (2u << 7) | (1u << 23);
    uint32_t vertex = VRAM_TEST_VERTS + 0x100u;
    uint32_t depth_sprite = vertex;
    vertex = depth_test_vertex(vertex, 0u, 0u, 0x4444u, 0xFF010203u);
    vertex = depth_test_vertex(vertex, 1u, 1u, 0x4444u, 0xFF010203u);
    uint32_t color_sprite = vertex;
    (void)depth_test_vertex(vertex, 2u, 2u, 0u, 0xFF010203u);
    (void)depth_test_vertex(vertex + 12u, 3u, 3u, 0u, 0xFF010203u);

    s_pc = LIST_VRAM_CPU_WRITE;
    W(0x10, 0x080000u);
    W(0x9C, 0x00100000u);
    W(0x9D, 512u);
    W(0xD2, 3u);
    W(0x9E, 0x0CC000u);
    W(0x9F, 512u);
    W(0xD4, 0u);
    W(0xD5, (271u << 10) | 479u);
    W(0x12, through16);
    W(0x23, 1u);
    W(0xDE, 4u);                         /* less */
    prim(6u, 2u, depth_sprite);
    W(0x23, 0u);
    W(0x28, 1u);                         /* logic op enabled */
    W(0xE6, 6u);                         /* XOR */
    prim(6u, 2u, color_sprite);
    W(0x0C, 0u);
}

static void build_vram_ambiguous_depth_list(uint32_t zbp, uint32_t vertex) {
    static const uint32_t through16 = (7u << 2) | (2u << 7) | (1u << 23);
    (void)depth_test_vertex(vertex, 20u, 20u, 0x1111u, 0xFF010203u);
    (void)depth_test_vertex(vertex + 12u, 21u, 21u, 0x1111u, 0xFF010203u);
    s_pc = LIST_VRAM_AMBIGUOUS;
    W(0x10, 0x080000u);
    W(0x9C, 0x00100000u);
    W(0x9D, 512u);
    W(0xD2, 3u);
    W(0x9E, zbp);
    W(0x9F, 512u);
    W(0xD4, 0u);
    W(0xD5, (271u << 10) | 479u);
    W(0x23, 1u);
    W(0xDE, 1u);
    W(0x21, 0u);
    W(0x28, 0u);
    W(0xD3, 0u);
    W(0x12, through16);
    prim(6u, 2u, vertex);
    W(0x0C, 0u);
}

/* zbp 0x0ce000 (bit 13 set). The PSP XORs bits 6 and 13 of the absolute address (measured 2026-10-21):
 * pixel (x, y) depth is at (zbp + linear) ^ 0x2040, not zbp + (linear ^ 0x2040). The mirror at
 * 0x04200000 then reads it back linearly. */
static void test_ge_vram_depth_bit13_absolute_xor(void) {
    const uint32_t zbp = 0x0CE000u;
    const uint32_t zbase = 0x04000000u + zbp;
    build_vram_depth_list_at(zbp);
    expect(ge_run_list(LIST_VRAM_DEPTH, 0) == 0, "vram_depth_bit13",
           "bit-13 depth list did not reach END");
    static const struct {
        unsigned x, y;
        uint16_t z;
    } points[] = {
        { 0u, 0u, 0x1111u },
        { 64u, 8u, 0x2222u },
        { 200u, 100u, 0x3333u },
    };
    for (size_t i = 0; i < sizeof points / sizeof points[0]; i++) {
        uint32_t linear = (points[i].y * 512u + points[i].x) * 2u;
        uint32_t stored = (zbase + linear) ^ 0x2040u;
        uint32_t alias_linear = 0x04200000u + zbp + linear;
        char detail[128];
        snprintf(detail, sizeof detail, "absolute-XOR depth at x=%u y=%u", points[i].x, points[i].y);
        expect(MEM_R16(stored) == points[i].z, "vram_depth_bit13", detail);
        snprintf(detail, sizeof detail, "linear mirror depth at x=%u y=%u", points[i].x, points[i].y);
        expect(MEM_R16(alias_linear) == points[i].z, "vram_depth_bit13", detail);
    }
}

/* ---- Texture mip levels (#1373) ----
 * A through-mode sprite samples an 8x8 8888 texture whose levels 0-3 are one colour each,
 * so the colour drawn names the level the sampler chose. Level 1 stores its 4 texels per
 * row in a buffer 8 wide with magenta beyond them, which only a sampler ignoring that
 * level's own buffer width reaches. The PSP's nearest-level rounding, slope formula and
 * between-level weights are unmeasured (ge.c tex_lod); these cases pin the PSPSDK modes and
 * keep today's level-0 output wherever mip levels are off. */
#define MIP_LIST  0x08116000u
#define MIP_VERTS 0x08118000u
#define MIP_TEX   0x08320000u
#define MIP_FB    0x04150000u
#define MIP_X     16u
#define MIP_Y     16u
#define MIP_LEVEL(mode, bias16) ((uint32_t)(mode) | (((uint32_t)(bias16) & 0xFFu) << 16))

static const uint32_t k_mip_color[4] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu };

static uint32_t mip_level_addr(unsigned level) { return MIP_TEX + level * 0x1000u; }
static unsigned mip_level_bufw(unsigned level) { return level == 1u ? 8u : (8u >> level); }

static void mip_fill_texture(void) {
    for (unsigned level = 0; level < 4u; level++) {
        unsigned size = 8u >> level, bufw = mip_level_bufw(level);
        for (unsigned v = 0; v < size; v++)
            for (unsigned u = 0; u < bufw; u++)
                MEM_W32(mip_level_addr(level) + (v * bufw + u) * 4u,
                        u < size ? k_mip_color[level] : 0xFFFF00FFu);
    }
}

/* Draw the texture (UV 0-8) onto a size x size sprite; return the one RGB colour the sprite
 * holds (framebuffer alpha is the stencil, which these draws leave alone), or 0 when it is
 * not uniform or the list did not end. */
static uint32_t mip_draw(uint32_t max_level, uint32_t level_word, uint32_t min_filter,
                         unsigned size) {
    const float verts[10] = { 0.0f, 0.0f, (float)MIP_X, (float)MIP_Y, 0.0f,
                              8.0f, 8.0f, (float)(MIP_X + size), (float)(MIP_Y + size), 0.0f };
    memcpy(SR_HOST(MIP_VERTS), verts, sizeof verts);
    for (unsigned y = 0; y < 16u; y++)
        for (unsigned x = 0; x < 16u; x++)
            MEM_W32(MIP_FB + ((MIP_Y + y) * 512u + MIP_X + x) * 4u, 0u);
    s_pc = MIP_LIST;
    W(0x10, 0x080000u);                     /* BASE */
    W(0x9C, MIP_FB & 0x00FFFFFFu);          /* FRAMEBUFPTR */
    W(0x9D, 512u);
    W(0xD2, 3u);                            /* 8888 */
    W(0xD4, 0u);
    W(0xD5, (271u << 10) | 479u);
    W(0xD3, 0u);                            /* not clear mode */
    W(0x21, 0u); W(0x22, 0u); W(0x23, 0u); W(0x24, 0u); W(0x27, 0u); W(0x28, 0u);
    for (unsigned level = 0; level < 4u; level++) {
        uint32_t addr = mip_level_addr(level), size_log2 = 3u - level;
        W(0xA0 + level, addr & 0x00FFFFFFu);                         /* TEXADDRn */
        W(0xA8 + level, ((addr >> 8) & 0x00FF0000u) | mip_level_bufw(level));
        W(0xB8 + level, (size_log2 << 8) | size_log2);              /* TEXSIZEn */
    }
    W(0xC2, max_level << 16);               /* TEXMODE: highest level, unswizzled */
    W(0xC3, 3u);                            /* TEXFORMAT 8888 */
    W(0xC6, min_filter);                    /* TEXFILTER, magnification nearest */
    W(0xC7, 0u);                            /* TEXWRAP repeat */
    W(0xC8, level_word);                    /* TEXLEVEL */
    W(0xC9, 3u | 0x100u);                   /* TEXFUNC replace, RGBA */
    W(0x1E, 1u);                            /* TEXTUREMAPENABLE */
    W(0x12, 3u | (3u << 7) | (1u << 23));   /* float UV and position, through mode */
    prim(6u, 2u, MIP_VERTS);
    W(0x0C, 0u);
    if (ge_run_list(MIP_LIST, 0) != 0) return 0u;
    uint32_t first = MEM_R32(MIP_FB + (MIP_Y * 512u + MIP_X) * 4u) & 0x00FFFFFFu;
    for (unsigned y = 0; y < size; y++)
        for (unsigned x = 0; x < size; x++)
            if ((MEM_R32(MIP_FB + ((MIP_Y + y) * 512u + MIP_X + x) * 4u) & 0x00FFFFFFu) != first)
                return 0u;
    return first;
}

static void test_ge_texture_mip_levels(void) {
    enum { RED = 0, GREEN = 1, BLUE = 2 };
    static const struct {
        const char *name;
        uint32_t max_level, level_word, min_filter;
        unsigned size;
        int level;            /* expected level, or -1 for the blend below */
    } cases[] = {
        /* Mip levels off: level 0 exactly as before. */
        { "highest_level_0",  0u, MIP_LEVEL(1, 16),  4u, 8u,  RED },
        { "non_mip_filter",   3u, MIP_LEVEL(1, 32),  1u, 8u,  RED },
        { "constant_level_0", 3u, MIP_LEVEL(1, 0),   4u, 8u,  RED },
        /* Constant: the bias names the level, clamped to the highest level. */
        { "constant_level_1", 3u, MIP_LEVEL(1, 16),  4u, 8u,  GREEN },
        { "constant_level_2", 3u, MIP_LEVEL(1, 32),  4u, 8u,  BLUE },
        { "constant_clamped", 1u, MIP_LEVEL(1, 48),  4u, 8u,  GREEN },
        /* Automatic: log2 of the texel footprint plus the bias. */
        { "auto_magnified",   3u, MIP_LEVEL(0, 0),   4u, 16u, RED },
        { "auto_one_to_one",  3u, MIP_LEVEL(0, 0),   4u, 8u,  RED },
        { "auto_minified",    3u, MIP_LEVEL(0, 0),   4u, 2u,  BLUE },
        { "auto_biased",      3u, MIP_LEVEL(0, -16), 4u, 2u,  GREEN },
        { "auto_clamped",     1u, MIP_LEVEL(0, 0),   4u, 1u,  GREEN },
        /* Slope: no public formula, so level 0 (unmeasured). */
        { "slope_level_0",    3u, MIP_LEVEL(2, 16),  4u, 2u,  RED },
        /* GU_NEAREST_MIPMAP_LINEAR at lod 0.5: half of level 0 and half of level 1. */
        { "linear_between",   3u, MIP_LEVEL(1, 8),   6u, 8u,  -1 },
    };
    mip_fill_texture();
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t want = (cases[i].level >= 0 ? k_mip_color[cases[i].level] : 0x007F7Fu) & 0x00FFFFFFu;
        uint32_t got = mip_draw(cases[i].max_level, cases[i].level_word, cases[i].min_filter,
                                cases[i].size);
        char detail[128];
        snprintf(detail, sizeof detail, "sprite colour 0x%08" PRIx32 ", expected 0x%08" PRIx32,
                 got, want);
        expect(got == want, cases[i].name, detail);
    }
    printf("MIP cases=%u (software rasterizer)\n", (unsigned)(sizeof cases / sizeof cases[0]));
}

/* The depth-surface span is cached per (base, stride) because every CPU VRAM access asks for it
 * (#1296). Six surfaces cycled through a smaller cache must keep returning their own bounds, each
 * the exact XOR image of the surface's even addresses (widened by at most one byte per end), and an
 * unsupported stride must stay refused after a cached success for the same base. */
static void test_ge_zbuf_span_bounds_cache(void) {
    static const struct { uint32_t base, stride; } surfaces[] = {
        { 0x040CE000u, 512u }, { 0x04088000u, 480u }, { 0x04000000u, 512u },
        { 0x04044000u, 256u }, { 0x04110000u, 512u }, { 0x040CC000u, 64u },
    };
    enum { N = sizeof surfaces / sizeof surfaces[0] };
    uint32_t want_first[N], want_end[N];
    for (unsigned i = 0; i < N; i++) {
        uint32_t lo = UINT32_MAX, hi = 0u;
        for (uint32_t p = 0; p < surfaces[i].stride * 272u; p++) {
            uint32_t a = (surfaces[i].base + 2u * p) ^ 0x2040u;
            if (a < lo) lo = a;
            if (a > hi) hi = a;
        }
        uint32_t first = 0u, end = 0u;
        expect(ge_zbuf_guest_span_bounds(surfaces[i].base, surfaces[i].stride, &first, &end),
               "zbuf_span_cache", "a supported depth surface had no span");
        expect(first <= lo && lo - first <= 1u && end >= hi + 2u && end - (hi + 2u) <= 1u,
               "zbuf_span_cache", "the span is not the XOR image of the surface");
        want_first[i] = first;
        want_end[i] = end;
    }
    for (unsigned round = 0; round < 5u; round++) {
        for (unsigned k = 0; k < N; k++) {
            unsigned i = (k * 5u + round) % N;
            uint32_t first = 0u, end = 0u;
            int ok = ge_zbuf_guest_span_bounds(surfaces[i].base, surfaces[i].stride, &first, &end);
            expect(ok && first == want_first[i] && end == want_end[i], "zbuf_span_cache",
                   "a repeated query returned another surface's span");
            uint32_t ignored_first, ignored_end;
            expect(!ge_zbuf_guest_span_bounds(surfaces[i].base, 513u, &ignored_first, &ignored_end),
                   "zbuf_span_cache", "a stride above 512 was accepted after a cached query");
        }
    }
}

static void test_ge_vram_cpu_visibility(void) {
    build_vram_depth_list_at(0x0CC000u);
    expect(ge_run_list(LIST_VRAM_DEPTH, 0) == 0, "vram_depth", "depth list did not reach END");

    static const struct {
        unsigned x, y;
        uint16_t z;
    } points[] = {
        { 0u, 0u, 0x1111u },
        { 64u, 8u, 0x2222u },
        { 200u, 100u, 0x3333u },
    };
    const uint32_t zbase = 0x040CC000u;
    for (size_t i = 0; i < sizeof points / sizeof points[0]; i++) {
        uint32_t linear = (points[i].y * 512u + points[i].x) * 2u;
        uint32_t stored = zbase + (linear ^ 0x2040u);
        uint32_t alias_linear = 0x04200000u + (zbase - 0x04000000u) + linear;
        char detail[112];
        snprintf(detail, sizeof detail, "swizzled depth at x=%u y=%u",
                 points[i].x, points[i].y);
        expect(MEM_R16(stored) == points[i].z, "vram_depth", detail);
        snprintf(detail, sizeof detail, "linear mirror depth at x=%u y=%u",
                 points[i].x, points[i].y);
        expect(MEM_R16(alias_linear) == points[i].z, "vram_depth", detail);
    }
    uint32_t clear_linear = (200u * 512u + 300u) * 2u;
    expect(MEM_R16(zbase + (clear_linear ^ 0x2040u)) == 0u &&
           MEM_R16(0x04200000u + (zbase - 0x04000000u) + clear_linear) == 0u,
           "vram_depth", "GE depth clear is visible in the base and linear mirror views");
    expect(MEM_R32(0x04100000u + (200u * 512u + 300u) * 4u) == 0xFF646464u,
           "vram_color", "CPU load after the software GE clear sees the linear colour row");

    MEM_W16(zbase + 0x2040u, 0xFFFFu);
    uint32_t color_addr = 0x04100000u + (2u * 512u + 2u) * 4u;
    MEM_W32(color_addr, 0xFF112233u);
    build_vram_cpu_write_list();
    expect(ge_run_list(LIST_VRAM_CPU_WRITE, 0) == 0,
           "vram_cpu_write", "post-store GE list did not reach END");
    expect(MEM_R16(zbase + 0x2040u) == 0x4444u,
           "vram_cpu_write", "next GE depth test observes the CPU-written depth value");
    expect(MEM_R32(color_addr) == 0xFF102030u,
           "vram_cpu_write", "next GE logic-op draw observes the CPU-written color value");

    /* zbp 0x0cc040 (bit 6 set) is still refused: the PSP wrote no depth there (2026-10-21). zbp 0x0ce000
     * (bit 13 set) is measured and accepted, so the depth-tested draw runs. */
    static const uint32_t ambiguous_zbp[] = { 0x0CC040u, 0x0CE000u };
    for (size_t i = 0; i < sizeof ambiguous_zbp / sizeof ambiguous_zbp[0]; i++) {
        uint32_t color_pixel = 0x04100000u + (20u * 512u + 20u) * 4u;
        MEM_W32(color_pixel, 0xFFABCDEFu);
        build_vram_ambiguous_depth_list(ambiguous_zbp[i],
                                        VRAM_TEST_VERTS + 0x300u + (uint32_t)i * 32u);
        expect(ge_run_list(LIST_VRAM_AMBIGUOUS, 0) == 0,
               "vram_depth_boundary", "ambiguous depth list did not reach END");
        if (ambiguous_zbp[i] == 0x0CC040u)
            expect(MEM_R32(color_pixel) == 0xFFABCDEFu,
                   "vram_depth_boundary", "bit-6 depth placement rejects the whole depth-tested primitive");
        else
            expect(MEM_R32(color_pixel) == 0xFF010203u,
                   "vram_depth_boundary", "bit-13 depth placement runs the depth-tested primitive (PSP-measured)");
    }
}

/* ---- GPU backend stand-in (the hook contract src/rt/gpu_sdl3vk/ge_gpu.c fills) ---- */

static unsigned s_hook_prims, s_hook_flushes;
static int hook_tri(const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp) {
    (void)a; (void)b; (void)c; (void)persp;
    s_hook_prims++;
    return 1;
}
static int hook_sprite(const GeVtx *a, const GeVtx *b, int persp) {
    (void)a; (void)b; (void)persp;
    s_hook_prims++;
    return 1;
}
static int hook_line(const GeVtx *a, const GeVtx *b, int persp) {
    (void)a; (void)b; (void)persp;
    s_hook_prims++;
    return 1;
}
static int hook_point(const GeVtx *a, int persp) {
    (void)a; (void)persp;
    s_hook_prims++;
    return 1;
}
static void hook_flush(const char *reason) {
    (void)reason;
    s_hook_flushes++;
}
static void hook_vram_dirty(uint32_t addr, uint32_t bytes) {
    (void)addr; (void)bytes;
}

/* ---- Checks ---- */

static int s_failures;
static unsigned s_scenarios, s_faults;

static void expect(int ok, const char *scenario, const char *what) {
    if (!ok) {
        s_failures++;
        fprintf(stderr, "FAIL %s: %s\n", scenario, what);
    }
}

static void print_mask(FILE *out, uint32_t mask) {
    if (!mask) { fputs("none", out); return; }
    const char *sep = "";
    for (unsigned i = 0; i < GE_PROFILE_CHECK_COUNT; i++)
        if (mask & GE_PROFILE_CHECK_MASK(i)) {
            fprintf(out, "%s%s", sep, ge_profile_check_name((GeProfileCheck)i));
            sep = ",";
        }
}

static GeCpuProfileStats snapshot(void) {
    GeCpuProfileStats s;
    memset(&s, 0, sizeof s);
    ge_cpu_profile_get(&s);
    return s;
}

static void configure(const char *cpu, const char *prim_profile, const char *stride,
                      const char *calibration) {
    env_set("SR_GPU_CPU_PROFILE", NULL);
    env_set("SR_GE_CPU_PROFILE", cpu);
    env_set("SR_GE_PRIM_PROFILE", prim_profile);
    env_set("SR_GE_PRIM_PROFILE_STRIDE", stride);
    env_set("SR_GE_PRIM_CALIBRATION", calibration);
    ge_cpu_profile_reset();
}

static void run_list(uint32_t list, const char *scenario) {
    expect(ge_run_list(list, 0) == 0, scenario, "list did not reach END");
}

static void run_scene(const char *scenario) {
    run_list(LIST_MAIN, scenario);
}

static void expect_consistent(const char *scenario, const GeCpuProfileStats *s) {
    char detail[256];
    uint32_t mask = ge_cpu_profile_validate(s, detail, sizeof detail);
    s_scenarios++;
    if (mask) {
        s_failures++;
        fprintf(stderr, "FAIL %s: production profile rejected (", scenario);
        print_mask(stderr, mask);
        fprintf(stderr, "): %s\n", detail);
    }
    expect(detail[0] == '\0', scenario, "a consistent block left a detail message");
    printf("SCENARIO %s mask=0x%" PRIx32 " commands=%" PRIu64 " primitive_commands=%" PRIu64
           " triangle_candidates=%" PRIu64 " drawn/clipped/rejected=%" PRIu64 "/%" PRIu64 "/%" PRIu64
           " decoded=%" PRIu64 " rejects=%" PRIu64 " list_ns=%" PRIu64 " gpu_hook_ns=%" PRIu64 "\n",
           scenario, mask, s->commands, s->primitive_commands,
           s->primitive_profile_triangle_candidates,
           s->primitive_profile_transform_triangles_drawn,
           s->primitive_profile_transform_triangles_clipped,
           s->primitive_profile_transform_triangles_rejected,
           s->primitive_profile_actual_decoded_vertices, s->primitive_profile_vertex_rejects,
           s->phase[GE_CPU_LIST_TOTAL].ns, s->phase[GE_CPU_GPU_HOOK].ns);
}

static int coarse_counters_zero(const GeCpuProfileStats *s) {
    static const GeCpuProfileStats zero;
    size_t from = offsetof(GeCpuProfileStats, phase);
    size_t to = offsetof(GeCpuProfileStats, primitive_profile_stride);
    return memcmp((const char *)s + from, (const char *)&zero + from, to - from) == 0;
}

static int sparse_counters_zero(const GeCpuProfileStats *s) {
    static const GeCpuProfileStats zero;
    size_t from = offsetof(GeCpuProfileStats, primitive_profile_draw_setup);
    return memcmp((const char *)s + from, (const char *)&zero + from, sizeof *s - from) == 0;
}

/* Every field, compared by the padding-free uint64_t runs so padding bytes never matter. */
static int same_profile(const GeCpuProfileStats *a, const GeCpuProfileStats *b) {
    size_t coarse = offsetof(GeCpuProfileStats, phase);
    size_t coarse_end = offsetof(GeCpuProfileStats, primitive_profile_stride);
    size_t sparse = offsetof(GeCpuProfileStats, primitive_profile_timer_pair_ns);
    return a->enabled == b->enabled &&
           a->primitive_profile_enabled == b->primitive_profile_enabled &&
           a->primitive_profile_calibration_enabled == b->primitive_profile_calibration_enabled &&
           a->primitive_profile_stride == b->primitive_profile_stride &&
           memcmp((const char *)a + coarse, (const char *)b + coarse, coarse_end - coarse) == 0 &&
           memcmp((const char *)a + sparse, (const char *)b + sparse, sizeof *a - sparse) == 0;
}

/* The populations each relation constrains must actually be produced by the scene, so a
 * relation cannot hold only because both of its sides are zero. */
static void expect_full_coverage(const char *scenario, const GeCpuProfileStats *s,
                                 int strip_cache, int hooked) {
    const GeCpuPhaseStats *p = s->phase;
    expect(s->enabled && s->primitive_profile_enabled && s->primitive_profile_calibration_enabled &&
           s->primitive_profile_stride == 1u, scenario, "profile configuration");
    expect(p[GE_CPU_LIST_TOTAL].ns > 0 && p[GE_CPU_COMMAND_DISPATCH].ns > 0 &&
           p[GE_CPU_PRIMITIVE].ns > 0 && p[GE_CPU_BLOCK_TRANSFER].ns > 0 &&
           p[GE_CPU_CLUT_LOAD].ns > 0, scenario, "coarse phases timed");
    expect(hooked ? (p[GE_CPU_GPU_HOOK].ns > 0 && s->flushes > 0)
                  : (p[GE_CPU_GPU_HOOK].calls == 0 && s->flushes == 0),
           scenario, "gpu hook and flush populations match the backend");
    expect(s->block_transfers > 0 && s->clut_loads > 0 && s->primitive_type[7] > 0 &&
           s->primitive_profile_patch_commands == 2u, scenario, "command kinds");
    expect(s->primitive_profile_through_triangle_candidates > 0 &&
           s->primitive_profile_transform_triangle_candidates > 0 &&
           s->primitive_profile_transform_triangles_drawn > 0 &&
           s->primitive_profile_transform_triangles_clipped > 0 &&
           s->primitive_profile_transform_triangles_rejected > 0,
           scenario, "triangle candidates and every acceptance verdict");
    expect(s->primitive_profile_non_triangle_primitives > 0 &&
           s->primitive_profile_vertex_rejects > 0 &&
           s->primitive_profile_actual_through_vertices > 0 &&
           s->primitive_profile_actual_transformed_vertices <
               s->primitive_profile_transform_vertex_uses,
           scenario, "rejects and a skipped sprite-corner decode");
    expect(strip_cache ? (s->primitive_profile_strip_cache_commands > 0 &&
                          s->strip_vertices_reused > 0 &&
                          s->primitive_profile_vertex_references > s->primitive_profile_vertices)
                       : (s->primitive_profile_strip_cache_commands == 0 &&
                          s->strip_vertices_reused == 0 &&
                          s->primitive_profile_vertex_references == s->primitive_profile_vertices),
           scenario, "strip-cache population matches the cache switch");
    for (unsigned i = 0; i < GE_PRIM_PROFILE_PHASE_COUNT; i++)
        expect(s->primitive_profile_phase[i].calls > 0 && s->primitive_profile_eligible[i] > 0,
               scenario, "every sparse phase sampled at stride 1");
    expect(s->primitive_profile_sampled_total.calls > 0 &&
           s->primitive_profile_empty_control.calls > 0, scenario, "calibration samples");
}

/* ---- Fault injection ---- */

typedef void (*Mutation)(GeCpuProfileStats *s);

static uint64_t nested_ns(const GeCpuProfileStats *s) {
    return s->phase[GE_CPU_COMMAND_DISPATCH].ns + s->phase[GE_CPU_PRIMITIVE].ns +
           s->phase[GE_CPU_BLOCK_TRANSFER].ns + s->phase[GE_CPU_CLUT_LOAD].ns +
           s->phase[GE_CPU_FLUSH].ns;
}

static void m_hook_outside_primitive(GeCpuProfileStats *s) {
    s->phase[GE_CPU_GPU_HOOK].ns = s->phase[GE_CPU_PRIMITIVE].ns + 1u;
}
static void m_dispatch_overrun(GeCpuProfileStats *s) {
    s->phase[GE_CPU_COMMAND_DISPATCH].ns += s->phase[GE_CPU_LIST_TOTAL].ns - nested_ns(s) + 1u;
}
static void m_nested_sum_wraps(GeCpuProfileStats *s) {
    /* A wrapping sum would come to exactly 0 <= list_total. */
    s->phase[GE_CPU_FLUSH].ns = UINT64_MAX - (nested_ns(s) - s->phase[GE_CPU_FLUSH].ns) + 1u;
}
static void m_primitive_phase_unpaired(GeCpuProfileStats *s) {
    s->phase[GE_CPU_PRIMITIVE].calls++;
}
static void m_list_calls_unpaired(GeCpuProfileStats *s) {
    s->phase[GE_CPU_LIST_TOTAL].calls++;
}
static void m_block_phase_unpaired(GeCpuProfileStats *s) {
    s->phase[GE_CPU_BLOCK_TRANSFER].calls++;
}
static void m_flush_phase_unpaired(GeCpuProfileStats *s) {
    s->phase[GE_CPU_FLUSH].calls++;
}
static void m_clut_load_unended(GeCpuProfileStats *s) {
    s->clut_loads = s->phase[GE_CPU_CLUT_LOAD].calls + 1u;
}
static void m_command_undercount(GeCpuProfileStats *s) {
    s->commands = s->primitive_commands + s->block_transfers + s->clut_loads + s->flushes - 1u;
}
static void m_command_bound_wraps(GeCpuProfileStats *s) {
    /* Block transfers stay paired with their phase calls; a wrapping sum of the command
     * kinds would fall back below commands. */
    s->block_transfers = UINT64_MAX;
    s->phase[GE_CPU_BLOCK_TRANSFER].calls = UINT64_MAX;
}
static void m_command_bound_wraps_last(GeCpuProfileStats *s) {
    /* The same with the overflow on the last term of the sum. */
    s->flushes = UINT64_MAX;
    s->phase[GE_CPU_FLUSH].calls = UINT64_MAX;
}
static void m_patch_without_command(GeCpuProfileStats *s) {
    s->primitive_profile_patch_commands++;
}
static void m_command_without_draw(GeCpuProfileStats *s) {
    /* One primitive command (with its phase call) that is neither a draw nor a patch: the
     * exact relation fails from below. */
    s->primitive_commands++;
    s->phase[GE_CPU_PRIMITIVE].calls++;
}
static void m_type_sum_wraps(GeCpuProfileStats *s) {
    /* Both profiles agree per type; a wrapping type sum would come back to
     * primitive_commands. */
    uint64_t type7 = s->primitive_type[7];
    s->primitive_type[7] = s->primitive_profile_commands[7] = UINT64_MAX;
    s->primitive_type[0] += type7 + 1u;
    s->primitive_profile_commands[0] += type7 + 1u;
}
static void m_type_slot_overcount(GeCpuProfileStats *s) {
    uint64_t types = 0;
    for (unsigned i = 0; i < 8; i++) types += s->primitive_type[i];
    s->primitive_type[3] += s->primitive_commands - types + 1u;
}
static void m_decode_use_drift(GeCpuProfileStats *s) {
    s->vertex_decode_uses++;
}
static void m_strip_reuse_drift(GeCpuProfileStats *s) {
    s->strip_vertices_reused++;
}
static void m_submitted_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_submitted[3]++;
}
static void m_submitted_sum_wraps(GeCpuProfileStats *s) {
    /* type 7 submits nothing; a wrapping sum would come back to the true total. */
    s->primitive_profile_submitted[7] = UINT64_MAX;
    s->primitive_profile_submitted[0]++;
}
static void m_submitted_sum_exceeds(GeCpuProfileStats *s) {
    /* The parts before the last already equal the expected total; the last pushes the sum
     * past UINT64_MAX, which must not compare equal. */
    s->primitive_profile_submitted[7] = UINT64_MAX;
}
static void m_through_candidate_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_through_triangle_candidates++;
}
static void m_mode_split_wraps(GeCpuProfileStats *s) {
    /* Transform candidates at UINT64_MAX with verdicts to match; a wrapping through +
     * transform sum would come back to triangle_candidates. */
    uint64_t transform = s->primitive_profile_transform_triangle_candidates;
    s->primitive_profile_transform_triangle_candidates = UINT64_MAX;
    s->primitive_profile_through_triangle_candidates += transform + 1u;
    s->primitive_profile_transform_triangles_drawn =
        UINT64_MAX - s->primitive_profile_transform_triangles_clipped -
        s->primitive_profile_transform_triangles_rejected;
}
static void m_triangle_reference_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_triangle_vertex_references++;
}
static void m_reference_split_wraps(GeCpuProfileStats *s) {
    uint64_t triangle = s->primitive_profile_triangle_vertex_references;
    s->primitive_profile_triangle_vertex_references = UINT64_MAX;
    s->primitive_profile_non_triangle_vertex_references += triangle + 1u;
}
static void m_triangle_use_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_triangle_vertex_uses++;
}
static void m_transform_vertices_drift(GeCpuProfileStats *s) {
    s->primitive_profile_transform_vertices++;
}
static void m_transform_use_overcount(GeCpuProfileStats *s) {
    /* Through + transform uses no longer sum to the uses; transform_vertices follows. */
    s->primitive_profile_transform_vertex_uses++;
    s->primitive_profile_transform_vertices++;
}
static void m_use_split_wraps(GeCpuProfileStats *s) {
    uint64_t triangle = s->primitive_profile_triangle_vertex_uses;
    s->primitive_profile_triangle_vertex_uses = UINT64_MAX;
    s->primitive_profile_non_triangle_vertex_uses += triangle + 1u;
}
static void m_references_undercount(GeCpuProfileStats *s) {
    /* Lower the total and its triangle split together, just below the uses. */
    uint64_t d = s->primitive_profile_vertex_references - s->primitive_profile_vertices + 1u;
    s->primitive_profile_triangle_vertex_references -= d;
    s->primitive_profile_vertex_references -= d;
}
static void m_reject_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_vertex_rejects = s->primitive_profile_non_triangle_vertex_references + 1u;
}
static void m_decoded_total_drift(GeCpuProfileStats *s) {
    s->primitive_profile_actual_decoded_vertices++;
}
static void m_through_decode_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_actual_through_vertices++;
    s->primitive_profile_actual_decoded_vertices++;
}
static void m_transform_decode_overcount(GeCpuProfileStats *s) {
    uint64_t extra = s->primitive_profile_transform_vertex_uses + 1u -
                     s->primitive_profile_actual_transformed_vertices;
    s->primitive_profile_actual_transformed_vertices += extra;
    s->primitive_profile_actual_decoded_vertices += extra;
}
static void m_skipped_decode_without_reject(GeCpuProfileStats *s) {
    /* One reject fewer than skipped transform decodes; the through decodes still match. */
    s->primitive_profile_vertex_rejects = s->primitive_profile_transform_vertex_uses -
                                          s->primitive_profile_actual_transformed_vertices - 1u;
}
static void m_calibration_bound_exceeds(GeCpuProfileStats *s) {
    /* Decodes just large enough that decoded + candidates exceeds UINT64_MAX, with more
     * control samples than decodes: a bound past UINT64_MAX holds for any count, so only
     * the through decodes (moved to keep the mode split) disagree with their uses. */
    uint64_t decoded = UINT64_MAX - s->primitive_profile_triangle_candidates + 1u;
    s->primitive_profile_actual_decoded_vertices = decoded;
    s->primitive_profile_actual_through_vertices =
        decoded - s->primitive_profile_actual_transformed_vertices;
    s->primitive_profile_empty_control.calls = decoded + 1u;
}
static void m_decode_split_wraps(GeCpuProfileStats *s) {
    /* A wrapping through + transformed sum would come back to decoded; both modes now also
     * disagree with their uses. */
    uint64_t through = s->primitive_profile_actual_through_vertices;
    s->primitive_profile_actual_through_vertices = UINT64_MAX;
    s->primitive_profile_actual_transformed_vertices += through + 1u;
}
static void m_decision_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_transform_triangles_drawn++;
}
static void m_decision_sum_wraps(GeCpuProfileStats *s) {
    uint64_t rejected = s->primitive_profile_transform_triangles_rejected;
    s->primitive_profile_transform_triangles_rejected = UINT64_MAX;
    s->primitive_profile_transform_triangles_clipped += rejected + 1u;
}
static void m_fetch_sample_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_phase[GE_PRIM_PROFILE_VERTEX_FETCH_DECODE].calls =
        s->primitive_profile_eligible[GE_PRIM_PROFILE_VERTEX_FETCH_DECODE] + 1u;
}
static void m_lighting_sample_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_phase[GE_PRIM_PROFILE_LIGHTING].calls =
        s->primitive_profile_eligible[GE_PRIM_PROFILE_LIGHTING] + 1u;
}
static void m_draw_setup_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_draw_setup.calls = s->primitive_profile_draw_setup_eligible + 1u;
}
static void m_sampled_total_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_sampled_total.calls = s->primitive_profile_triangle_candidates + 1u;
}
static void m_control_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_empty_control.calls = s->primitive_profile_actual_decoded_vertices +
                                               s->primitive_profile_triangle_candidates + 1u;
}
static void m_two_faults(GeCpuProfileStats *s) {
    m_hook_outside_primitive(s);
    m_decision_overcount(s);
}

#define BIT(check) GE_PROFILE_CHECK_MASK(GE_PROFILE_CHECK_##check)

typedef struct Fault {
    const char *name;
    Mutation mutate;
    uint32_t want;
} Fault;

/* Faults for a block with both profiles and calibration enabled. */
static const Fault s_full_faults[] = {
    { "hook_outside_primitive", m_hook_outside_primitive, BIT(PHASE_NESTING) },
    { "dispatch_overrun", m_dispatch_overrun, BIT(PHASE_NESTING) },
    { "nested_sum_wraps", m_nested_sum_wraps, BIT(PHASE_NESTING) },
    { "primitive_phase_unpaired", m_primitive_phase_unpaired, BIT(PHASE_CALLS) },
    { "list_calls_unpaired", m_list_calls_unpaired, BIT(PHASE_CALLS) },
    { "block_phase_unpaired", m_block_phase_unpaired, BIT(PHASE_CALLS) },
    { "flush_phase_unpaired", m_flush_phase_unpaired, BIT(PHASE_CALLS) },
    { "clut_load_unended", m_clut_load_unended, BIT(PHASE_CALLS) },
    { "command_undercount", m_command_undercount, BIT(COMMAND_BOUND) },
    { "command_bound_wraps", m_command_bound_wraps, BIT(COMMAND_BOUND) },
    { "command_bound_wraps_last", m_command_bound_wraps_last, BIT(COMMAND_BOUND) },
    { "patch_without_command", m_patch_without_command, BIT(PRIMITIVE_TYPES) },
    { "command_without_draw", m_command_without_draw, BIT(PRIMITIVE_TYPES) },
    { "type_sum_wraps", m_type_sum_wraps, BIT(PRIMITIVE_TYPES) },
    { "type_slot_overcount", m_type_slot_overcount, BIT(PRIMITIVE_TYPES) | BIT(PROFILE_AGREEMENT) },
    { "decode_use_drift", m_decode_use_drift, BIT(PROFILE_AGREEMENT) },
    { "strip_reuse_drift", m_strip_reuse_drift, BIT(PROFILE_AGREEMENT) },
    { "submitted_overcount", m_submitted_overcount, BIT(SUBMITTED_SPLIT) },
    { "submitted_sum_wraps", m_submitted_sum_wraps, BIT(SUBMITTED_SPLIT) },
    { "submitted_sum_exceeds", m_submitted_sum_exceeds, BIT(SUBMITTED_SPLIT) },
    { "through_candidate_overcount", m_through_candidate_overcount, BIT(TRIANGLE_MODE_SPLIT) },
    { "mode_split_wraps", m_mode_split_wraps, BIT(TRIANGLE_MODE_SPLIT) },
    { "triangle_reference_overcount", m_triangle_reference_overcount, BIT(REFERENCE_SPLIT) },
    { "reference_split_wraps", m_reference_split_wraps, BIT(REFERENCE_SPLIT) },
    { "triangle_use_overcount", m_triangle_use_overcount, BIT(USE_SPLIT) },
    { "transform_use_overcount", m_transform_use_overcount, BIT(USE_SPLIT) },
    { "transform_vertices_drift", m_transform_vertices_drift, BIT(USE_SPLIT) },
    { "use_split_wraps", m_use_split_wraps, BIT(USE_SPLIT) },
    { "references_undercount", m_references_undercount, BIT(REFERENCES_COVER_USES) },
    { "reject_overcount", m_reject_overcount, BIT(REJECT_BOUND) },
    { "decoded_total_drift", m_decoded_total_drift, BIT(DECODE_SPLIT) },
    { "decode_split_wraps", m_decode_split_wraps, BIT(DECODE_SPLIT) | BIT(DECODE_COVERAGE) },
    { "through_decode_overcount", m_through_decode_overcount, BIT(DECODE_COVERAGE) },
    { "transform_decode_overcount", m_transform_decode_overcount, BIT(DECODE_COVERAGE) },
    { "skipped_decode_without_reject", m_skipped_decode_without_reject, BIT(DECODE_COVERAGE) },
    { "calibration_bound_exceeds", m_calibration_bound_exceeds, BIT(DECODE_COVERAGE) },
    { "decision_overcount", m_decision_overcount, BIT(TRIANGLE_DECISIONS) },
    { "decision_sum_wraps", m_decision_sum_wraps, BIT(TRIANGLE_DECISIONS) },
    { "fetch_sample_overcount", m_fetch_sample_overcount, BIT(SAMPLE_BOUND) },
    { "lighting_sample_overcount", m_lighting_sample_overcount, BIT(SAMPLE_BOUND) },
    { "draw_setup_overcount", m_draw_setup_overcount, BIT(SAMPLE_BOUND) },
    { "sampled_total_overcount", m_sampled_total_overcount, BIT(CALIBRATION_BOUND) },
    { "control_overcount", m_control_overcount, BIT(CALIBRATION_BOUND) },
    { "two_faults", m_two_faults, BIT(PHASE_NESTING) | BIT(TRIANGLE_DECISIONS) },
};

static void expect_fault(const char *base, const GeCpuProfileStats *production,
                         const Fault *fault) {
    GeCpuProfileStats s;
    char detail[256];
    char label[128];
    memcpy(&s, production, sizeof s);
    fault->mutate(&s);
    uint32_t mask = ge_cpu_profile_validate(&s, detail, sizeof detail);
    snprintf(label, sizeof label, "%s/%s", base, fault->name);
    s_faults++;
    if (mask != fault->want) {
        s_failures++;
        fprintf(stderr, "FAIL %s: want ", label);
        print_mask(stderr, fault->want);
        fprintf(stderr, ", got ");
        print_mask(stderr, mask);
        fprintf(stderr, " (%s)\n", detail);
        return;
    }
    /* The detail names the lowest-numbered failed check. */
    unsigned lowest = 0;
    while (!(mask & GE_PROFILE_CHECK_MASK(lowest))) lowest++;
    const char *name = ge_profile_check_name((GeProfileCheck)lowest);
    expect(strncmp(detail, name, strlen(name)) == 0 && detail[strlen(name)] == ':',
           label, "detail does not name the lowest failed check");
}

static void expect_faults(const char *base, const GeCpuProfileStats *production) {
    for (size_t i = 0; i < sizeof s_full_faults / sizeof s_full_faults[0]; i++)
        expect_fault(base, production, &s_full_faults[i]);
}

static void expect_api_contract(void) {
    char detail[64];
    uint32_t all = GE_PROFILE_CHECK_MASK(GE_PROFILE_CHECK_COUNT) - 1u;
    expect(ge_cpu_profile_validate(NULL, detail, sizeof detail) == all &&
           strcmp(detail, "no profile snapshot") == 0, "api", "NULL snapshot fails closed");
    GeCpuProfileStats zero;
    memset(&zero, 0, sizeof zero);
    expect(ge_cpu_profile_validate(&zero, NULL, 0) == 0, "api", "all-zero block without detail");
    zero.enabled = zero.primitive_profile_enabled = zero.primitive_profile_calibration_enabled = 1;
    expect(ge_cpu_profile_validate(&zero, detail, sizeof detail) == 0, "api",
           "enabled block with zero samples");
    expect(strcmp(ge_profile_check_name(GE_PROFILE_CHECK_COUNT), "unknown") == 0, "api",
           "out-of-range check name");
    for (unsigned i = 0; i < GE_PROFILE_CHECK_COUNT; i++)
        for (unsigned j = i + 1; j < GE_PROFILE_CHECK_COUNT; j++)
            expect(strcmp(ge_profile_check_name((GeProfileCheck)i),
                          ge_profile_check_name((GeProfileCheck)j)) != 0, "api", "duplicate name");
}

static uint32_t vram_expected_cpu_address(uint32_t address) {
    uint32_t phys = SR_PHYS(address);
    uint32_t region = (phys >> 21) & 3u;
    uint32_t offset = phys & 0x001FFFFFu;
    if (region == 1u) {
        offset ^= 0x2040u;
    } else if (region == 3u) {
        uint32_t bank = (offset >> 5) & 0x1Fu;
        bank = ((bank << 1) | (bank >> 4)) & 0x1Fu;
        offset = (offset & ~0x3E0u) | (bank << 5);
        offset ^= 0x2040u;
    }
    return 0x04000000u + offset;
}

static void test_vram_mirror_cpu_stores(void) {
    static const uint32_t mirrors[] = {
        0x04000000u, 0x04200000u, 0x04400000u, 0x04600000u,
        0x44000000u, 0x44200000u, 0x44400000u, 0x44600000u,
    };
    const uint32_t offset = 0x00010100u;
    const uint32_t value = 0x78563412u;
    for (size_t mirror = 0; mirror < sizeof mirrors / sizeof mirrors[0]; mirror++) {
        uint32_t address = mirrors[mirror] + offset;
        MEM_W32(address, value);
        for (unsigned byte = 0; byte < 4u; byte++) {
            uint32_t source = address + byte;
            uint32_t physical = vram_expected_cpu_address(source);
            uint8_t actual = MEM_R8(physical);
            uint8_t expected = (uint8_t)(value >> (byte * 8u));
            char detail[128];
            snprintf(detail, sizeof detail,
                     "mirror %08" PRIx32 " byte %u maps to %08" PRIx32,
                     mirrors[mirror], byte, physical);
            expect(actual == expected, "vram_mirror_store", detail);
        }
        MEM_W32(0x04000000u + offset, value);
        expect(MEM_R32(address) == value, "vram_mirror_read",
               "CPU load through each cached and uncached mirror returns the physical bytes");
    }
    static const uint32_t boundaries[] = { 0x1Fu, 0x3Fu, 0x1FFFu };
    static const size_t swizzled_mirrors[] = { 1u, 3u, 5u, 7u };
    for (size_t m = 0; m < sizeof swizzled_mirrors / sizeof swizzled_mirrors[0]; m++) {
        for (size_t b = 0; b < sizeof boundaries / sizeof boundaries[0]; b++) {
            uint32_t address = mirrors[swizzled_mirrors[m]] + boundaries[b];
            MEM_W32(address, value);
            for (unsigned byte = 0; byte < 4u; byte++) {
                uint32_t physical = vram_expected_cpu_address(address + byte);
                expect(MEM_R8(physical) == (uint8_t)(value >> (byte * 8u)),
                       "vram_mirror_boundary",
                       "scalar word stores translate every byte across 32-byte, 64-byte, and 8 KiB boundaries");
            }
        }
    }
    expect(!sr_guest_span_readable(0x0420003Fu, 2u) &&
           !sr_guest_span_writable(0x0460001Fu, 2u),
           "vram_mirror_bulk", "swizzled mirrors reject non-contiguous bulk spans");
    expect(sr_guest_span_readable(0x04200100u, 4u) &&
           sr_guest_span_readable(0x04400100u, 4u),
           "vram_mirror_bulk", "contiguous swizzled blocks and the linear mirror stay accessible");
}

/* ---- BOUNDINGBOX / BJUMP conditional rendering (issue #1372) ----
 *
 * The sceGuBeginObject/sceGuEndObject shape inside a CALLed sublist: OFFSETADDR (VADDR and the
 * BJUMP target are offset-relative, like JUMP's), BASE and VADDR, BOUNDINGBOX, BASE and a BJUMP
 * past the object, then the object itself (a red sprite at (10,10) and a FRAMEBUFPTR switch)
 * and RET.  After the RET the main list JUMPs to a tail that draws a green sprite at (30,10);
 * the JUMP target and the sprite's VADDR need the caller's offset, so the list reaches END and
 * draws green only if the PC, the call stack and the offset came back intact.  A skipped object leaves no red pixel and no framebuffer switch, so green
 * lands in framebuffer A; an object that runs draws red and moves green to framebuffer B.
 * Each case runs on the software rasterizer and again through the GPU hook seam the Vulkan
 * backend fills, and both must execute the same PRIM commands. */

extern unsigned long g_ge_prim_count;

#define LIST_BBOX       0x08118000u
#define LIST_BBOX_SUB   0x0811A000u
#define LIST_BBOX_TAIL  0x0811C000u
#define BBOX_VERTS      0x08320000u
#define BBOX_INDICES    0x08328000u
#define BBOX_SPRITES    0x08329000u
#define BBOX_FB_A       0x041A0000u
#define BBOX_FB_B       0x041C0000u
#define BBOX_SUB_OFFSET 0x00010000u          /* OFFSETADDR 0x000100 in the sublist */
#define BBOX_SENTINEL   0xFF5A5A5Au
#define BBOX_RED        0xFF0000FFu
#define BBOX_GREEN      0xFF00FF00u
#define BBOX_THROUGH16  ((7u << 2) | (2u << 7) | (1u << 23))

typedef struct {
    const char *name;
    uint32_t vtype;     /* VERTEXTYPE in force at BOUNDINGBOX */
    uint32_t vaddr;     /* box vertices */
    int count;          /* BOUNDINGBOX count; -1 emits no BOUNDINGBOX at all */
    int small_scissor;  /* scissor (0,0)-(99,271): the box is beyond its right edge only */
    int stall;          /* stall the list on the BJUMP word, then resume it */
    int skip;           /* expected: BJUMP takes its branch */
} BboxCase;

/* BASE supplies address bits 24-27 and OFFSETADDR is added, as sceGuBeginObject relies on. */
static uint32_t bbox_sub_base(uint32_t addr) {
    return (addr >> 8) & 0x0F0000u;
}
static uint32_t bbox_sub_rel(uint32_t addr) {
    return (addr - BBOX_SUB_OFFSET) & 0x00FFFFFFu;
}

static uint32_t bbox_px(uint32_t fb, unsigned x, unsigned y) {
    return fb + (y * 512u + x) * 4u;
}

/* Returns the address of the BJUMP word. */
static uint32_t build_bbox_list(const BboxCase *c, uint32_t sprite_a, uint32_t sprite_b) {
    s_pc = LIST_BBOX_SUB;
    W(0x13, BBOX_SUB_OFFSET >> 8);                     /* OFFSETADDR */
    W(0x10, bbox_sub_base(c->vaddr));                  /* BASE */
    W(0x01, bbox_sub_rel(c->vaddr));                   /* VADDR */
    if (c->count >= 0) W(0x07, (uint32_t)c->count);    /* BOUNDINGBOX */
    W(0x10, bbox_sub_base(LIST_BBOX_SUB));             /* BASE for the BJUMP target */
    uint32_t bjump = s_pc;
    W(0x09, 0);                                        /* BJUMP, patched below */
    W(0x12, BBOX_THROUGH16);                           /* the object: state and a draw */
    W(0x01, bbox_sub_rel(sprite_a));
    W(0x04, (6u << 16) | 2u);                          /* PRIM: sprite A */
    W(0x9C, BBOX_FB_B & 0xFFFFFFu);                    /* FRAMEBUFPTR -> B */
    uint32_t after_object = s_pc;
    W(0x0B, 0);                                        /* RET */
    uint32_t end_pc = s_pc;
    s_pc = bjump;                                      /* as sceGuEndObject patches it */
    W(0x09, bbox_sub_rel(after_object));
    s_pc = end_pc;

    s_pc = LIST_BBOX_TAIL;
    W(0x12, BBOX_THROUGH16);
    prim(6u, 2u, sprite_b);                            /* sprite B */
    W(0x0F, 0);                                        /* FINISH */
    W(0x0C, 0);                                        /* END */

    s_pc = LIST_BBOX;
    emit_setup();
    W(0x9C, BBOX_FB_A & 0xFFFFFFu); W(0x9D, 512u); W(0xD2, 3u);
    W(0x1D, 0); W(0x1F, 0); W(0x20, 0); W(0x21, 0); W(0x22, 0);   /* cull, fog, dither, */
    W(0x24, 0); W(0x27, 0); W(0x28, 0); W(0xD3, 0);               /* blend, tests, logic op, */
    W(0xE8, 0); W(0xE9, 0);                                       /* clear mode and masks off */
    if (c->small_scissor) W(0xD5, (271u << 10) | 99u);
    W(0x12, c->vtype);
    W(0x02, BBOX_INDICES & 0xFFFFFFu);                 /* IADDR */
    W(0x0A, LIST_BBOX_SUB & 0xFFFFFFu);                /* CALL */
    W(0x08, LIST_BBOX_TAIL & 0xFFFFFFu);               /* JUMP */
    return bjump;
}

static void test_ge_bounding_box_branch(void) {
    /* Transform-mode boxes under emit_setup's perspective (w = -z) and 480x272 viewport. */
    s_vtx = BBOX_VERTS;
    uint32_t box_on = s_vtx;                           /* centre of the screen */
    for (int i = 0; i < 8; i++)
        vtx((i & 1) ? 0.4f : -0.4f, (i & 2) ? 0.4f : -0.4f, (i & 4) ? -6.0f : FRONT);
    uint32_t box_right = s_vtx;                        /* right after box_on: x >= 520 */
    for (int i = 0; i < 8; i++)
        vtx((i & 1) ? 9.0f : 7.0f, (i & 2) ? 0.4f : -0.4f, (i & 4) ? -6.0f : FRONT);
    uint32_t box_above = s_vtx;                        /* y < -20 */
    for (int i = 0; i < 8; i++)
        vtx((i & 1) ? 0.4f : -0.4f, (i & 2) ? 9.0f : 7.0f, (i & 4) ? -6.0f : FRONT);
    uint32_t box_spans = s_vtx;                        /* beyond the left AND right edges */
    for (int i = 0; i < 8; i++)
        vtx((i & 1) ? 9.0f : -9.0f, (i & 2) ? 0.4f : -0.4f, (i & 4) ? -6.0f : FRONT);
    /* box_right with one corner behind the eye (w = -1) whose projection also lands right of
     * the screen (x -9 / w -1): only the w test keeps this box visible. */
    uint32_t box_behind = s_vtx;
    for (int i = 0; i < 8; i++)
        vtx(i == 7 ? -9.0f : ((i & 1) ? 9.0f : 7.0f), (i & 2) ? 0.4f : -0.4f,
            i == 7 ? BEHIND : FRONT);
    for (uint8_t i = 0; i < 8u; i++) {                 /* 8-bit indices of box_right's corners */
        uint8_t index = (uint8_t)(8u + i);
        memcpy(SR_HOST(BBOX_INDICES + i), &index, 1);
    }
    uint32_t sprite_a = BBOX_SPRITES;
    uint32_t v = depth_test_vertex(sprite_a, 10u, 10u, 0u, BBOX_RED);
    v = depth_test_vertex(v, 14u, 14u, 0u, BBOX_RED);
    uint32_t sprite_b = v;
    v = depth_test_vertex(v, 30u, 10u, 0u, BBOX_GREEN);
    v = depth_test_vertex(v, 34u, 14u, 0u, BBOX_GREEN);
    uint32_t box_through = v;                          /* through-mode corners at x = 600 */
    v = depth_test_vertex(v, 600u, 10u, 0u, BBOX_RED);
    (void)depth_test_vertex(v, 610u, 20u, 0u, BBOX_RED);

    const BboxCase cases[] = {
        /* Documented: a box with a vertex in view runs the object; a box wholly outside the
         * scissor region skips it. */
        { "onscreen",  VT_TRANSFORM, box_on,     8, 0, 0, 0 },
        { "right",     VT_TRANSFORM, box_right,  8, 0, 0, 1 },
        /* A fresh list without BOUNDINGBOX does not inherit the previous list's result. */
        { "no_box",    VT_TRANSFORM, box_on,    -1, 0, 0, 0 },
        { "above",     VT_TRANSFORM, box_above,  8, 0, 0, 1 },
        { "scissor",   VT_TRANSFORM, box_on,     8, 1, 0, 1 },
        { "partial",   VT_TRANSFORM, box_on,    16, 0, 0, 0 },   /* box_on then box_right */
        { "indexed",   VT_TRANSFORM | VT_INDEX8, box_on, 8, 0, 0, 1 },
        { "stalled",   VT_TRANSFORM, box_right,  8, 0, 1, 1 },
        /* UNMEASURED on the PSP, so the object runs (the behaviour before BJUMP was
         * emulated): no single edge, a corner behind the eye, through mode, zero count. */
        { "spans",     VT_TRANSFORM, box_spans,  8, 0, 0, 0 },
        { "behind",    VT_TRANSFORM, box_behind, 8, 0, 0, 0 },
        { "through",   BBOX_THROUGH16, box_through, 2, 0, 0, 0 },
        { "zero",      VT_TRANSFORM, box_right,  0, 0, 0, 0 },
        /* Vertices past the end of guest memory: no test is possible, so the object runs. */
        { "outside",   VT_TRANSFORM, 0x0BFFFFF0u, 8, 0, 0, 0 },
    };

    GeGpuHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.tri = hook_tri; hooks.sprite = hook_sprite; hooks.line = hook_line;
    hooks.point = hook_point; hooks.flush = hook_flush; hooks.vram_dirty = hook_vram_dirty;
    for (int hooked = 0; hooked < 2; hooked++) {
        ge_set_gpu_hooks(hooked ? &hooks : NULL);
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            const BboxCase *c = &cases[i];
            char scenario[64];
            snprintf(scenario, sizeof scenario, "bbox_%s_%s", c->name,
                     hooked ? "gpu_hooks" : "software");
            uint32_t bjump = build_bbox_list(c, sprite_a, sprite_b);
            static const unsigned xs[2] = { 11u, 31u };
            for (unsigned k = 0; k < 2u; k++) {
                MEM_W32(bbox_px(BBOX_FB_A, xs[k], 11u), BBOX_SENTINEL);
                MEM_W32(bbox_px(BBOX_FB_B, xs[k], 11u), BBOX_SENTINEL);
            }
            unsigned hook_prims = s_hook_prims;
            uint32_t next = LIST_BBOX;
            int resume = 0;
            if (c->stall) {
                g_ge_stall_addr = bjump;
                next = ge_run_list(next, 0);
                g_ge_stall_addr = 0;
                expect(next == bjump, scenario, "list did not stall on the BJUMP word");
                resume = 1;
            }
            expect(ge_run_list(next, resume) == 0, scenario,
                   "list did not reach END (PC, call stack or offset lost)");
            unsigned long prims = c->skip ? 1ul : 2ul;
            expect(g_ge_prim_count == prims, scenario,
                   c->skip ? "BJUMP did not skip the object's PRIM"
                           : "the object's PRIM did not run");
            if (hooked) {
                expect(s_hook_prims - hook_prims == (unsigned)prims, scenario,
                       "GPU hooks did not receive the same primitives");
            } else if (c->skip) {
                expect(MEM_R32(bbox_px(BBOX_FB_A, 11u, 11u)) == BBOX_SENTINEL, scenario,
                       "skipped sprite A was drawn");
                expect(MEM_R32(bbox_px(BBOX_FB_A, 31u, 11u)) == BBOX_GREEN &&
                       MEM_R32(bbox_px(BBOX_FB_B, 31u, 11u)) == BBOX_SENTINEL, scenario,
                       "skipped FRAMEBUFPTR changed the target of sprite B");
            } else {
                expect(MEM_R32(bbox_px(BBOX_FB_A, 11u, 11u)) == BBOX_RED, scenario,
                       "sprite A was not drawn");
                expect(MEM_R32(bbox_px(BBOX_FB_B, 31u, 11u)) == BBOX_GREEN &&
                       MEM_R32(bbox_px(BBOX_FB_A, 31u, 11u)) == BBOX_SENTINEL, scenario,
                       "FRAMEBUFPTR in the object did not move sprite B");
            }
        }
    }
    ge_set_gpu_hooks(NULL);
    printf("BBOX cases=%u (software and GPU hooks)\n", (unsigned)(sizeof cases / sizeof cases[0]));
}

int main(int argc, char **argv) {
    int strip_cache = 1;
    if (argc == 2 && strcmp(argv[1], "--strip-cache-disabled") == 0) strip_cache = 0;
    else if (argc != 1) {
        fprintf(stderr, "usage: %s [--strip-cache-disabled]\n", argv[0]);
        return 2;
    }
    env_set("SR_GE_STRIP_CACHE_DISABLE", strip_cache ? NULL : "1");

    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) {
        fprintf(stderr, "ge_cpu_profile_validate_selftest: no guest arena\n");
        return 2;
    }
    g_mem = arena + 0x08000000u;
    test_vram_mirror_cpu_stores();
    test_ge_zbuf_span_bounds_cache();
    test_ge_texture_mip_levels();
    build_lists();
    expect_api_contract();

    /* Both profiles, every sample, calibration: the fault-injection base. */
    configure("1", "1", "1", "1");
    run_scene("full");
    run_list(LIST_BAD_CLUT, "full");
    GeCpuProfileStats full = snapshot();
    expect_consistent("full", &full);
    expect_full_coverage("full", &full, strip_cache, 0);
    expect(full.phase[GE_CPU_CLUT_LOAD].calls > full.clut_loads, "full",
           "an unloaded CLUT span still ends its phase");

    /* Profiles latch until a reset: a changed stride does not apply mid-session, and a
     * second pass accumulates onto the first. */
    env_set("SR_GE_PRIM_PROFILE_STRIDE", "5");
    run_scene("accumulated");
    GeCpuProfileStats accumulated = snapshot();
    expect_consistent("accumulated", &accumulated);
    expect(accumulated.primitive_profile_stride == 1u, "accumulated", "stride latched until reset");
    expect(accumulated.primitive_profile_triangle_candidates ==
               2u * full.primitive_profile_triangle_candidates &&
           accumulated.primitive_commands == 2u * full.primitive_commands,
           "accumulated", "second pass accumulates");

    /* A reset clears every counter and sampling ordinal: a fresh identical run reproduces
     * the first snapshot byte for byte. */
    configure("1", "1", "1", "1");
    GeCpuProfileStats cleared = snapshot();
    expect_consistent("reset", &cleared);
    expect(coarse_counters_zero(&cleared) && sparse_counters_zero(&cleared), "reset",
           "reset clears every counter");
    run_scene("reset");
    run_list(LIST_BAD_CLUT, "reset");
    GeCpuProfileStats rerun = snapshot();
    expect(same_profile(&rerun, &full), "reset", "rerun after reset differs");

    /* The GPU backend seam: hooks take every primitive and flush at list end. */
    GeGpuHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.tri = hook_tri; hooks.sprite = hook_sprite; hooks.line = hook_line;
    hooks.point = hook_point; hooks.flush = hook_flush; hooks.vram_dirty = hook_vram_dirty;
    ge_set_gpu_hooks(&hooks);
    configure("1", "1", "1", "1");
    run_scene("hooked");
    ge_set_gpu_hooks(NULL);
    GeCpuProfileStats hooked = snapshot();
    expect_consistent("hooked", &hooked);
    expect_full_coverage("hooked", &hooked, strip_cache, 1);
    expect(s_hook_prims > 0 && s_hook_flushes > 0 &&
           hooked.phase[GE_CPU_FLUSH].calls == hooked.flushes, "hooked", "hook traffic");

    /* Sparse defaults: stride 512, no calibration. */
    configure("1", "1", NULL, NULL);
    run_scene("sparse_default");
    GeCpuProfileStats sparse_default = snapshot();
    expect_consistent("sparse_default", &sparse_default);
    expect(sparse_default.primitive_profile_stride == 512u &&
           !sparse_default.primitive_profile_calibration_enabled &&
           sparse_default.primitive_profile_sampled_total.calls == 0 &&
           sparse_default.primitive_profile_empty_control.calls == 0 &&
           sparse_default.primitive_profile_phase[GE_PRIM_PROFILE_VERTEX_FETCH_DECODE].calls <
               sparse_default.primitive_profile_eligible[GE_PRIM_PROFILE_VERTEX_FETCH_DECODE],
           "sparse_default", "sparse sampling below eligibility");

    /* One profile at a time: the other's counters stay zero. */
    configure("1", NULL, NULL, NULL);
    run_scene("coarse_only");
    GeCpuProfileStats coarse_only = snapshot();
    expect_consistent("coarse_only", &coarse_only);
    expect(coarse_only.enabled && !coarse_only.primitive_profile_enabled &&
           coarse_only.primitive_commands > 0 && sparse_counters_zero(&coarse_only),
           "coarse_only", "only the coarse profile counts");
    configure(NULL, "1", "1", NULL);
    run_scene("sparse_only");
    GeCpuProfileStats sparse_only = snapshot();
    expect_consistent("sparse_only", &sparse_only);
    expect(!sparse_only.enabled && sparse_only.primitive_profile_enabled &&
           sparse_only.primitive_profile_triangle_candidates > 0 &&
           coarse_counters_zero(&sparse_only), "sparse_only", "only the sparse profile counts");
    configure(NULL, NULL, NULL, NULL);
    env_set("SR_GPU_CPU_PROFILE", "1");
    ge_cpu_profile_reset();
    run_scene("gpu_cpu_switch");
    GeCpuProfileStats gpu_switch = snapshot();
    expect_consistent("gpu_cpu_switch", &gpu_switch);
    expect(gpu_switch.enabled && gpu_switch.primitive_commands == coarse_only.primitive_commands,
           "gpu_cpu_switch", "SR_GPU_CPU_PROFILE enables the coarse profile");

    /* Profiling off: nothing is counted at all. */
    configure(NULL, NULL, NULL, NULL);
    run_scene("disabled");
    GeCpuProfileStats disabled = snapshot();
    expect_consistent("disabled", &disabled);
    expect(!disabled.enabled && !disabled.primitive_profile_enabled &&
           coarse_counters_zero(&disabled) && sparse_counters_zero(&disabled),
           "disabled", "a disabled profile counts nothing");

    /* A list with no draws: zero-sample populations. */
    configure("1", "1", "1", "1");
    run_list(LIST_EMPTY, "empty_list");
    GeCpuProfileStats empty = snapshot();
    expect_consistent("empty_list", &empty);
    expect(empty.phase[GE_CPU_LIST_TOTAL].calls == 1u && empty.commands == 1u &&
           empty.primitive_commands == 0u && sparse_counters_zero(&empty),
           "empty_list", "one END command and no primitives");

    expect_faults("full", &full);
    expect_faults("hooked", &hooked);
    static const Fault coarse_fault = {
        "type_slot_overcount", m_type_slot_overcount, BIT(PRIMITIVE_TYPES) };
    expect_fault("coarse_only", &coarse_only, &coarse_fault);
    static const Fault sparse_fault = {
        "through_candidate_overcount", m_through_candidate_overcount, BIT(TRIANGLE_MODE_SPLIT) };
    expect_fault("sparse_only", &sparse_only, &sparse_fault);

    test_ge_vram_cpu_visibility();
    test_ge_vram_depth_bit13_absolute_xor();
    test_ge_bounding_box_branch();

    printf("ge_cpu_profile_validate_selftest: strip_cache=%s scenarios=%u faults=%u failures=%d\n",
           strip_cache ? "on" : "off", s_scenarios, s_faults, s_failures);
    free(arena);
    return s_failures ? 1 : 0;
}
