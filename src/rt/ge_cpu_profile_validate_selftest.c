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
// the failure mask to equal exactly the intended check(s), so a fault cannot pass because
// an unrelated check fired.  Faults whose wrapping uint64_t sum would land back on the
// expected value prove the comparisons are exact.
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
int g_sr_heap_watch;
int g_sr_last_writer_enabled;
int g_sr_mem_watch_count;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
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
#define VERTS         0x08200000u
#define INDICES       0x08280000u
#define CLUT_SRC      0x08290000u

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
static void m_clut_load_unended(GeCpuProfileStats *s) {
    s->clut_loads = s->phase[GE_CPU_CLUT_LOAD].calls + 1u;
}
static void m_command_undercount(GeCpuProfileStats *s) {
    s->commands = s->primitive_commands + s->block_transfers + s->clut_loads + s->flushes - 1u;
}
static void m_patch_without_command(GeCpuProfileStats *s) {
    s->primitive_profile_patch_commands++;
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
static void m_through_candidate_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_through_triangle_candidates++;
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
static void m_decode_mode_misattributed(GeCpuProfileStats *s) {
    /* Move decodes from transform to through: more skipped transform uses than rejects. */
    uint64_t skipped = s->primitive_profile_transform_vertex_uses -
                       s->primitive_profile_actual_transformed_vertices;
    uint64_t k = s->primitive_profile_vertex_rejects - skipped + 1u;
    s->primitive_profile_actual_transformed_vertices -= k;
    s->primitive_profile_actual_through_vertices += k;
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
    { "clut_load_unended", m_clut_load_unended, BIT(PHASE_CALLS) },
    { "command_undercount", m_command_undercount, BIT(COMMAND_BOUND) },
    { "patch_without_command", m_patch_without_command, BIT(PRIMITIVE_TYPES) },
    { "type_slot_overcount", m_type_slot_overcount, BIT(PRIMITIVE_TYPES) | BIT(PROFILE_AGREEMENT) },
    { "decode_use_drift", m_decode_use_drift, BIT(PROFILE_AGREEMENT) },
    { "strip_reuse_drift", m_strip_reuse_drift, BIT(PROFILE_AGREEMENT) },
    { "submitted_overcount", m_submitted_overcount, BIT(SUBMITTED_SPLIT) },
    { "submitted_sum_wraps", m_submitted_sum_wraps, BIT(SUBMITTED_SPLIT) },
    { "through_candidate_overcount", m_through_candidate_overcount, BIT(TRIANGLE_MODE_SPLIT) },
    { "triangle_reference_overcount", m_triangle_reference_overcount, BIT(REFERENCE_SPLIT) },
    { "reference_split_wraps", m_reference_split_wraps, BIT(REFERENCE_SPLIT) },
    { "triangle_use_overcount", m_triangle_use_overcount, BIT(USE_SPLIT) },
    { "transform_vertices_drift", m_transform_vertices_drift, BIT(USE_SPLIT) },
    { "references_undercount", m_references_undercount, BIT(REFERENCES_COVER_USES) },
    { "reject_overcount", m_reject_overcount, BIT(REJECT_BOUND) },
    { "decoded_total_drift", m_decoded_total_drift, BIT(DECODE_SPLIT) },
    { "through_decode_overcount", m_through_decode_overcount, BIT(DECODE_COVERAGE) },
    { "transform_decode_overcount", m_transform_decode_overcount, BIT(DECODE_COVERAGE) },
    { "decode_mode_misattributed", m_decode_mode_misattributed, BIT(DECODE_COVERAGE) },
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
    expect(memcmp(&rerun, &full, sizeof full) == 0, "reset", "rerun after reset differs");

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

    printf("ge_cpu_profile_validate_selftest: strip_cache=%s scenarios=%u faults=%u failures=%d\n",
           strip_cache ? "on" : "off", s_scenarios, s_faults, s_failures);
    free(arena);
    return s_failures ? 1 : 0;
}
