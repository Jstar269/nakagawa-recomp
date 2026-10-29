// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// GE cells for the PSP-3000 oracle, one PRX and one launch per case:
//   CASE=ge-sample (PSP-GE-SAMPLE-001): which texel a through-mode (vertex type bit 23)
//     textured triangle pair and a through-mode sprite show at each pixel, under nearest
//     and bilinear filtering, for 1:1, +0.5-texel, 2x magnification, 20->12 horizontal
//     minification and 16-bit vertex cells.
//   CASE=ge-raster (PSP-GE-RASTER-001, #343): the raster semantics corpus. Each cell is one
//     raw display list over a 16x4 window of generated source and destination patterns:
//     blend factors and equations, alpha, colour, stencil and depth tests, texture
//     functions, texture formats and CLUT modes, wrap and clamp, triangle coverage, 16-bit
//     framebuffer formats, dithering, pixel masks, logic operations, clear mode and fog.
//     The cell table and list builder are the block shared with src/rt/hle_thread_selftest.c,
//     which runs the same cells through the software GE and compares each window's digest.
//
// ge-sample: each draw is one raw display list (no libpspgu) into a 64x32 8888 framebuffer
// at VRAM offset 0x100000, cleared by the CPU through the uncached VRAM mirror before the list is
// queued. The texture is a 32x32 8888 image in main RAM whose texel (u, v) is
// R = 8u, G = 8v, A = 0xff and, in texture "u", B = 0xff on odd columns (0 on even); in
// texture "v", B = 0xff on odd rows. R and G name the texel; B gives the blend weight of
// the odd column (or row) to 1/255. No blending, alpha test, depth test, fog, dither or
// colour test; texture function REPLACE with RGBA; clamp in u and v. After the GE is idle
// the 16x16 pixel window x 3..18, y 3..18 is read back through the uncached mirror and
// recorded raw, one record per row (out0 = x 3 ... out15 = x 18).
//
// ge-raster: each cell clears the same 8 KiB block, writes its destination pattern into the
// 16x4 window x 16..31, y 8..11 and queues its list; the depth cells also use a 4 KiB depth
// block at VRAM offset 0x110000, written only by the GE. The window is read back raw (16-bit
// formats zero-extended), one record per row (out0 = x 16 ... out15 = x 31).
//
// Record stream and log: the same writer, round trip and module lifecycle as
// probe_hle_issues.c (copied byte for byte): NAKAGAWA_PSP_META, a NAKAGAWA_PSP_STEP
// before each draw, a `<draw>-draw` record with the GE wait outcome (ge-raster adds the
// window digest), the row records (sixteen `<draw>-rNN` for ge-sample, four `<cell>-rN` for
// ge-raster), a `<case>-done` terminal record whose out0 counts the records, the host0
// round trip and a final NAKAGAWA_PSP_COMPLETE line. Every GE wait is bounded
// (sceGeDrawSync(1) peeks against a deadline); a list that does not finish is a TIMEOUT
// record and every queue is reset (sceGeBreak(1)) before the next draw. The probe writes
// only its own RAM, the 8 KiB framebuffer block at VRAM 0x100000, the 4 KiB depth block at
// 0x110000 (ge-raster) and host0 logs.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspge.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_HLE_MEASURE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

#ifndef PROBE_BUILD_COMMIT
#error PROBE_BUILD_COMMIT is required
#endif

#define GE_CASE_SAMPLE 109
#define GE_CASE_RASTER 118

#if PSP_ORACLE_CASE == GE_CASE_SAMPLE
#define HLE_CAMPAIGN_ID "ge-sample"
#define HLE_TEST_ID "PSP-GE-SAMPLE-001"
#define HLE_LOG "host0:/ge_sample_log.txt"
#elif PSP_ORACLE_CASE == GE_CASE_RASTER
#define HLE_CAMPAIGN_ID "ge-raster"
#define HLE_TEST_ID "PSP-GE-RASTER-001"
#define HLE_LOG "host0:/ge_raster_log.txt"
#else
#error probe_ge_sample.c has no case for this PSP_ORACLE_CASE
#endif
#define HLE_DONE_ID HLE_CAMPAIGN_ID "-done"

static int s_log_failed;
static uint32_t s_records;

/* Durable line writer: open, write everything, close. A later hang or fault
   keeps every line already written. `truncate` starts a fresh log. */
static void hle_write(const char *line, int truncate)
{
    const int flags = PSP_O_WRONLY | PSP_O_CREAT | (truncate ? PSP_O_TRUNC : PSP_O_APPEND);
    const SceUID fd = sceIoOpen(HLE_LOG, flags, 0777);
    if (fd < 0) {
        s_log_failed = 1;
        return;
    }
    const size_t length = strlen(line);
    size_t offset = 0;
    while (offset < length) {
        const int wrote = sceIoWrite(fd, line + offset, (SceSize)(length - offset));
        if (wrote <= 0) {
            s_log_failed = 1;
            break;
        }
        offset += (size_t)wrote;
    }
    if (sceIoClose(fd) < 0) {
        s_log_failed = 1;
    }
}

static void hle_begin(void)
{
    static const char meta[] =
        "NAKAGAWA_PSP_META schema=1 source=psp model=unknown firmware=unknown "
        "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
        "source_commit=" PROBE_BUILD_COMMIT " fixture=hle-measure\n";
    hle_write(meta, 1);
}

/* Progress marker written durably before a call that could hang or fault. The
   step name is the measurement case about to run, so a hang names its cell. */
static void hle_step(const char *step)
{
    static char line[200];
    snprintf(line, sizeof(line), "NAKAGAWA_PSP_STEP schema=1 case_id=%s step=%s\n",
             HLE_CAMPAIGN_ID, step);
    hle_write(line, 0);
}

static void hle_emit(const char *case_id, const char *status, uint32_t result,
                     const uint32_t *out, uint32_t count)
{
    static char line[700];
    int n = snprintf(line, sizeof(line),
                     "NAKAGAWA_PSP_TEST schema=1 test_id=%s case_id=%s status=%s result=0x%08x",
                     HLE_TEST_ID, case_id, status, (unsigned int)result);
    for (uint32_t i = 0; i < count && n > 0 && n < (int)sizeof(line) - 32; i++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " out%u=0x%08x", (unsigned int)i,
                      (unsigned int)out[i]);
    }
    if (n > 0 && n < (int)sizeof(line) - 2) {
        line[n] = '\n';
        line[n + 1] = '\0';
        hle_write(line, 0);
    }
}

static void hle_record(const char *case_id, const char *status, uint32_t result,
                       const uint32_t *out, uint32_t count)
{
    s_records++;
    hle_emit(case_id, status, result, out, count);
}

/* The host0 round trip probe.c's teardown performs (PROBE_HOST0_ROUNDTRIP_PATH and
   probe_host0_roundtrip there): 64 bytes, byte i = (0x5A ^ (i * 0x25 + (i >> 3))) & 0xFF,
   written with truncate, then read back and compared. The runner's
   _verify_host0_roundtrip reads the file after unload, so every family leaves it.
   probe.c is a separate translation unit, so the contract is repeated here and
   tools/test_psp_oracle.py pins both copies. */
#define HLE_ROUNDTRIP_PATH "host0:/nakagawa_transport_write.bin"
#define HLE_ROUNDTRIP_BYTES 64u

static int hle_host0_roundtrip(void)
{
    uint8_t expected[HLE_ROUNDTRIP_BYTES];
    uint8_t observed[HLE_ROUNDTRIP_BYTES];
    for (size_t i = 0; i < sizeof(expected); i++) {
        expected[i] = (uint8_t)(0x5Au ^ (i * 0x25u + (i >> 3)));
    }
    memset(observed, 0, sizeof(observed));
    SceUID fd = sceIoOpen(HLE_ROUNDTRIP_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return 0;
    }
    const int written = sceIoWrite(fd, expected, (SceSize)sizeof(expected));
    const int write_close = sceIoClose(fd);
    if (written != (int)sizeof(expected) || write_close < 0) {
        return 0;
    }
    fd = sceIoOpen(HLE_ROUNDTRIP_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    const int read = sceIoRead(fd, observed, (SceSize)sizeof(observed));
    const int read_close = sceIoClose(fd);
    return read == (int)sizeof(observed) && read_close >= 0 &&
           memcmp(expected, observed, sizeof(expected)) == 0;
}

/* Shared end of run, after every measurement and its cleanup. The round trip
   is host I/O only, so it cannot change a cell already recorded. As in probe.c's
   teardown, a failed round trip makes the completion status FAIL. */
static void hle_finish(int ok)
{
    const uint32_t count = s_records;
    hle_emit(HLE_DONE_ID, "PASS", 0, &count, 1);
    const int roundtrip = hle_host0_roundtrip();
    static char line[96];
    snprintf(line, sizeof(line), "NAKAGAWA_PSP_COMPLETE schema=1 status=%s\n",
             (ok && !s_log_failed && roundtrip) ? "PASS" : "FAIL");
    hle_write(line, 0);
}

/* Module lifecycle, as in probe.c: main parks until module_stop asks it to end,
   then runs the CRT's de-initialisation and exits; module_stop deletes it. */
static volatile SceUID s_hle_main_thread = -1;
static volatile int s_hle_stop_requested;
extern void _fini(void);
extern void __libcglue_deinit(void);
#define HLE_MAIN_STOP_TIMEOUT_US 1000000u

static void hle_park_until_stop(void)
{
    while (!s_hle_stop_requested) {
        sceKernelSleepThread();
    }
    _fini();
    __libcglue_deinit();
    sceKernelExitThread(0);
}

int module_stop(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    const SceUID main_thread = s_hle_main_thread;
    if (main_thread < 0) {
        return 1;
    }
    s_hle_stop_requested = 1;
    (void)sceKernelWakeupThread(main_thread);
    SceUInt timeout = HLE_MAIN_STOP_TIMEOUT_US;
    if (sceKernelWaitThreadEnd(main_thread, &timeout) < 0) {
        return 1;
    }
    if (sceKernelDeleteThread(main_thread) < 0) {
        return 1;
    }
    s_hle_main_thread = -1;
    return 0;
}

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == GE_CASE_SAMPLE

#define GS_FB_OFFSET 0x00100000u    /* VRAM-relative framebuffer (stride 64, 32 rows) */
#define GS_ZB_OFFSET 0x00110000u    /* depth buffer pointer; depth test and write are off */
#define GS_FB_STRIDE 64u
#define GS_FB_ROWS 32u
#define GS_BG 0x00000000u
#define GS_TEX_LOG2 5u              /* 32x32 texels */
#define GS_TEX_SIDE 32u
#define GS_WIN_X0 3u                /* read-back window x 3..18, y 3..18 */
#define GS_WIN_Y0 3u
#define GS_WIN_SIDE 16u
#define GS_WAIT_POLLS 5000u         /* 5000 x 100 us: a half-second bound per draw */
#define GS_WAIT_POLL_US 100u
#define GS_VTYPE_FLOAT 0x00800183u  /* through | position float | texture float */
#define GS_VTYPE_S16 0x00800102u    /* through | position s16 | texture s16 */

/* One texture mapping. Positions and texel coordinates are the vertex values as given
   (through mode: pixels and texels). */
struct gs_map {
    const char *name;
    uint32_t s16;
    float x0, y0, x1, y1, u0, v0, u1, v1;
};

static const struct gs_map s_gs_maps[6] = {
    {"1to1", 0u, 4.0f, 4.0f, 12.0f, 12.0f, 8.0f, 8.0f, 16.0f, 16.0f},
    {"half", 0u, 4.0f, 4.0f, 12.0f, 12.0f, 8.5f, 8.5f, 16.5f, 16.5f},
    {"mag2", 0u, 4.0f, 4.0f, 12.0f, 12.0f, 8.0f, 8.0f, 12.0f, 12.0f},
    {"min20", 0u, 4.0f, 4.0f, 16.0f, 18.0f, 6.0f, 8.0f, 26.0f, 22.0f},
    {"s16", 1u, 4.0f, 4.0f, 12.0f, 12.0f, 8.0f, 8.0f, 16.0f, 16.0f},
    {"s16min", 1u, 4.0f, 4.0f, 16.0f, 18.0f, 6.0f, 8.0f, 26.0f, 22.0f},
};
static const char *const s_gs_prims[2] = {"tri", "spr"};
static const char *const s_gs_filters[2] = {"near", "lin"};
static const uint32_t s_gs_filter_words[2] = {0x000u, 0x101u};
static const char *const s_gs_texs[2] = {"u", "v"};

static uint32_t s_gs_tex[2][GS_TEX_SIDE * GS_TEX_SIDE] __attribute__((aligned(64)));
static uint32_t s_gs_list[64] __attribute__((aligned(64)));
static float s_gs_vf[6 * 5] __attribute__((aligned(64)));
static int16_t s_gs_vs[6 * 5] __attribute__((aligned(64)));
static uint32_t s_gs_win[GS_WIN_SIDE * GS_WIN_SIDE];

static uint32_t gs_phys(const void *p)
{
    return (uint32_t)(uintptr_t)p & 0x0fffffffu;
}

static volatile uint32_t *gs_fb(void)
{
    const uint32_t vram = (uint32_t)(uintptr_t)sceGeEdramGetAddr();
    return (volatile uint32_t *)(uintptr_t)((vram | 0x40000000u) + GS_FB_OFFSET);
}

static void gs_textures(void)
{
    for (unsigned v = 0; v < GS_TEX_SIDE; v++) {
        for (unsigned u = 0; u < GS_TEX_SIDE; u++) {
            const uint32_t base = 0xff000000u | ((v * 8u) << 8) | (u * 8u);
            s_gs_tex[0][v * GS_TEX_SIDE + u] = base | ((u & 1u) ? 0x00ff0000u : 0u);
            s_gs_tex[1][v * GS_TEX_SIDE + u] = base | ((v & 1u) ? 0x00ff0000u : 0u);
        }
    }
}

/* Vertex k of the draw: triangles TL TR BL, BL TR BR; a sprite TL BR. */
static void gs_vertices(const struct gs_map *m, unsigned prim)
{
    static const uint8_t tri[6] = {0u, 1u, 2u, 2u, 1u, 3u};
    static const uint8_t spr[2] = {0u, 3u};
    const unsigned count = prim ? 2u : 6u;
    for (unsigned k = 0; k < count; k++) {
        const unsigned corner = prim ? spr[k] : tri[k];
        const int right = (corner & 1u) != 0;
        const int bottom = (corner & 2u) != 0;
        const float u = right ? m->u1 : m->u0;
        const float v = bottom ? m->v1 : m->v0;
        const float x = right ? m->x1 : m->x0;
        const float y = bottom ? m->y1 : m->y0;
        s_gs_vf[k * 5u + 0u] = u;
        s_gs_vf[k * 5u + 1u] = v;
        s_gs_vf[k * 5u + 2u] = x;
        s_gs_vf[k * 5u + 3u] = y;
        s_gs_vf[k * 5u + 4u] = 0.0f;
        s_gs_vs[k * 5u + 0u] = (int16_t)u;
        s_gs_vs[k * 5u + 1u] = (int16_t)v;
        s_gs_vs[k * 5u + 2u] = (int16_t)x;
        s_gs_vs[k * 5u + 3u] = (int16_t)y;
        s_gs_vs[k * 5u + 4u] = 0;
    }
}

/* The whole display list for one draw; every state the draw depends on is set. */
static uint32_t gs_list(const struct gs_map *m, unsigned prim, unsigned filter, unsigned tex)
{
    const uint32_t t = gs_phys(s_gs_tex[tex]);
    const uint32_t vb = m->s16 ? gs_phys(s_gs_vs) : gs_phys(s_gs_vf);
    const uint32_t words[] = {
        0x9C000000u | (GS_FB_OFFSET & 0x00ffffffu), 0x9D000000u | GS_FB_STRIDE,
        0xD2000003u,                                /* framebuffer 8888 */
        0x9E000000u | (GS_ZB_OFFSET & 0x00ffffffu), 0x9F000000u | GS_FB_STRIDE,
        0x15000000u, 0x16000000u | (GS_FB_STRIDE - 1u) | ((GS_FB_ROWS - 1u) << 10),
        0xD4000000u, 0xD5000000u | (GS_FB_STRIDE - 1u) | ((GS_FB_ROWS - 1u) << 10),
        0x4C000000u, 0x4D000000u,                   /* screen offset 0 */
        0x17000000u, 0x1C000000u, 0x1D000000u, 0x1F000000u, 0x20000000u,
        0x21000000u, 0x22000000u, 0x23000000u, 0x24000000u, 0x25000000u,
        0x27000000u, 0x28000000u,                   /* every per-fragment unit off */
        0xD3000000u,                                /* clear mode off */
        0xE7000001u,                                /* depth write off */
        0xE8000000u, 0xE9000000u,                   /* write every colour and alpha bit */
        0x55ffffffu, 0x580000ffu,                   /* material colour white */
        0x1E000001u,                                /* texture on */
        0xC0000000u,                                /* UV mapping */
        0xC2000000u,                                /* linear layout, one level */
        0xC3000003u,                                /* texture 8888 */
        0xA0000000u | (t & 0x00ffffffu),
        0xA8000000u | ((t >> 8) & 0x000f0000u) | GS_TEX_SIDE,
        0xB8000000u | (GS_TEX_LOG2 << 8) | GS_TEX_LOG2,
        0xC6000000u | s_gs_filter_words[filter],
        0xC7000101u,                                /* clamp u and v */
        0xC8000000u,                                /* level mode auto, bias 0 */
        0xC9000103u,                                /* REPLACE, RGBA */
        0xCA000000u,
        0x483f8000u, 0x493f8000u, 0x4A000000u, 0x4B000000u,
        0xCB000000u,                                /* texture flush */
        0x12000000u | (m->s16 ? GS_VTYPE_S16 : GS_VTYPE_FLOAT),
        0x10000000u | ((vb >> 8) & 0x000f0000u),
        0x01000000u | (vb & 0x00ffffffu),
        0x04000000u | (prim ? (6u << 16) | 2u : (3u << 16) | 6u),
        0x0F000000u, 0x0C000000u,
    };
    const uint32_t n = (uint32_t)(sizeof(words) / sizeof(words[0]));
    for (unsigned i = 0; i < n; i++) {
        s_gs_list[i] = words[i];
    }
    return n;
}

/* One draw: clear, queue, bounded wait, read back the window, then the records. */
static void gs_draw(const struct gs_map *m, unsigned prim, unsigned filter, unsigned tex)
{
    static char name[48];
    static char row[56];
    snprintf(name, sizeof(name), "%s-%s-%s-%s", s_gs_prims[prim], s_gs_filters[filter],
             m->name, s_gs_texs[tex]);
    hle_step(name);
    volatile uint32_t *fb = gs_fb();
    for (unsigned i = 0; i < GS_FB_STRIDE * GS_FB_ROWS; i++) {
        fb[i] = GS_BG;
    }
    __asm__ volatile("sync" ::: "memory");
    gs_vertices(m, prim);
    const uint32_t words = gs_list(m, prim, filter, tex);
    sceKernelDcacheWritebackAll();
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    const int qid = sceGeListEnQueue(s_gs_list, NULL, -1, NULL);
    int list_state = qid;
    int draw_state = -1;
    int idle = 0;
    for (unsigned i = 0; i < GS_WAIT_POLLS; i++) {
        list_state = qid >= 0 ? sceGeListSync(qid, 1) : qid;
        draw_state = sceGeDrawSync(1);
        idle = draw_state == PSP_GE_LIST_DONE;
        if (idle) {
            break;
        }
        sceKernelDelayThread(GS_WAIT_POLL_US);
    }
    const uint32_t elapsed = sceKernelGetSystemTimeLow() - t0;
    int reset_rc = 0;
    if (!idle) {
        PspGeBreakParam param;
        memset(&param, 0, sizeof(param));
        reset_rc = sceGeBreak(1, &param);
        for (unsigned i = 0; i < GS_WAIT_POLLS; i++) {
            if (sceGeDrawSync(1) == PSP_GE_LIST_DONE) {
                break;
            }
            sceKernelDelayThread(GS_WAIT_POLL_US);
        }
    }
    for (unsigned y = 0; y < GS_WIN_SIDE; y++) {
        for (unsigned x = 0; x < GS_WIN_SIDE; x++) {
            s_gs_win[y * GS_WIN_SIDE + x] =
                fb[(GS_WIN_Y0 + y) * GS_FB_STRIDE + GS_WIN_X0 + x];
        }
    }
    const uint32_t draw[7] = {(uint32_t)qid, (uint32_t)list_state, (uint32_t)draw_state,
                              elapsed, (uint32_t)idle, (uint32_t)reset_rc, words};
    snprintf(row, sizeof(row), "%s-draw", name);
    hle_record(row, qid < 0 ? "FAIL" : idle ? "PASS" : "TIMEOUT", (uint32_t)qid, draw, 7);
    for (unsigned y = 0; y < GS_WIN_SIDE; y++) {
        snprintf(row, sizeof(row), "%s-r%02u", name, GS_WIN_Y0 + y);
        hle_record(row, "PASS", GS_WIN_Y0 + y, &s_gs_win[y * GS_WIN_SIDE], GS_WIN_SIDE);
    }
}

static int hle_run(void)
{
    gs_textures();
    for (unsigned m = 0; m < 6u; m++) {
        for (unsigned prim = 0; prim < 2u; prim++) {
            for (unsigned filter = 0; filter < 2u; filter++) {
                for (unsigned tex = 0; tex < 2u; tex++) {
                    gs_draw(&s_gs_maps[m], prim, filter, tex);
                }
            }
        }
    }
    return 1;
}

#endif /* GE_CASE_SAMPLE */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == GE_CASE_RASTER

/* ==== GE raster cells (PSP-GE-RASTER-001, #343) ====
 * This block is carried byte for byte by fixtures/psp_oracle/probe_ge_sample.c (the PSP
 * side, CASE=ge-raster) and src/rt/hle_thread_selftest.c (the software GE side);
 * tools/test_psp_oracle.py pins the copy. Each cell is one display list of project-written
 * command words over generated patterns (no game data). It draws into the 16x4 window
 * x 16..31, y 8..11 of a 64x32 colour buffer at VRAM offset GR_FB_OFFSET whose other pixels
 * are 0 and whose window holds gr_dst() before the list runs. The base state turns every
 * per-fragment unit off and selects a 16x4 texture drawn 1:1 by a through-mode sprite with
 * REPLACE/RGBA, so a sprite's source colour is the texel; a cell's words then change only
 * what it measures. The result is the window's 64 pixels (16-bit formats zero-extended). */

#define GR_FB_OFFSET 0x00100000u   /* VRAM-relative colour buffer, stride 64, 32 rows */
#define GR_ZB_OFFSET 0x00110000u   /* VRAM-relative depth buffer, stride 64 */
#define GR_STRIDE 64u
#define GR_ROWS 32u
#define GR_WX 16u                  /* window x 16..31, y 8..11 */
#define GR_WY 8u
#define GR_WW 16u
#define GR_WH 4u
#define GR_N 64u                   /* pixels in the window, and texels in each texture */
#define GR_LIST_WORDS 256u
#define GR_VTX_WORDS 800u
#define GR_TEX_BYTES 256u          /* the largest texture (16x4 8888) */
#define GR_CLUT_BYTES 1024u
#define GR_Z 32768.0f              /* depth of every tested through-mode primitive */
#define GR_RED 0xff0000ffu
#define GR_GREEN 0xff00ff00u

enum { GR_K_SPRITE, GR_K_ZSPRITE, GR_K_TRI, GR_K_WRAP, GR_K_CLEAR, GR_K_FOG };
enum { GR_T_8888, GR_T_5650, GR_T_5551, GR_T_4444, GR_T_CLUT8, GR_T_CLUT4, GR_T_COUNT };
enum { GR_D_PATTERN, GR_D_WHITE };

/* kind: what is drawn (GR_K_*); psm: framebuffer format; tex: source texture (GR_T_*);
   dst: window contents before the draw (GR_D_*); arg: the kind's table row; vcol: vertex
   colour (ABGR); w: command words applied after the base state, 0 ends them. */
struct gr_cell {
    const char *name;
    uint8_t kind, psm, tex, dst, arg;
    uint32_t vcol;
    uint32_t w[6];
};

#define GR_CELL_COUNT 166u
static const struct gr_cell gr_cells[GR_CELL_COUNT] = {
    {"b-sa-isa", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000032u}},
    {"b-fix", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf0000aau, 0xe040c0ffu, 0xe17f3f01u}},
    {"b-fix-ff", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf0000aau, 0xe0ffffffu, 0xe1ffffffu}},
    {"b-dc-sc", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000000u}},
    {"b-idc-isc", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000011u}},
    {"b-da-ida", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000054u}},
    {"b-2sa-2isa", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000076u}},
    {"b-2isa-2sa", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000067u}},
    {"b-2da-2ida", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000098u}},
    {"b-sub-sa-fix", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf0001a2u, 0xe1806040u}},
    {"b-rsub-sa-isa", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000232u}},
    {"b-min", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000332u}},
    {"b-max", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000432u}},
    {"b-abs", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000532u}},
    {"b-eq6", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000632u}},
    {"b-eq7", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000732u}},
    {"b-fac11-15", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf0000fbu, 0xe0102030u, 0xe1c0b0a0u}},
    {"b565-sa-isa", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000032u}},
    {"b5551-sa-isa", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000032u}},
    {"b4444-sa-isa", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000032u}},
    {"b565-da-ida", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x21000001u, 0xdf000054u}},
    {"at-f0", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8000u}},
    {"at-f1", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8001u}},
    {"at-f2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8002u}},
    {"at-f3", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8003u}},
    {"at-f4", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8004u}},
    {"at-f5", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8005u}},
    {"at-f6", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8006u}},
    {"at-f7", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff8007u}},
    {"at-mask", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdb3c3007u}},
    {"at-gt0", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x22000001u, 0xdbff0006u}},
    {"ct-never", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x27000001u, 0xd8000000u, 0xd9800000u, 0xda800000u}},
    {"ct-always", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x27000001u, 0xd8000001u, 0xd9800000u, 0xda800000u}},
    {"ct-eq", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x27000001u, 0xd8000002u, 0xd9800000u, 0xda800000u}},
    {"ct-ne", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x27000001u, 0xd8000003u, 0xd9800000u, 0xda800000u}},
    {"ct-eq-mix", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x27000001u, 0xd8000002u, 0xd9004080u, 0xda00c0c0u}},
    {"st-f0", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8000u, 0xdd040003u}},
    {"st-f1", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8001u, 0xdd040003u}},
    {"st-f2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8002u, 0xdd040003u}},
    {"st-f3", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8003u, 0xdd040003u}},
    {"st-f4", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8004u, 0xdd040003u}},
    {"st-f5", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8005u, 0xdd040003u}},
    {"st-f6", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8006u, 0xdd040003u}},
    {"st-f7", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8007u, 0xdd040003u}},
    {"st-op0", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd000000u}},
    {"st-op1", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd010000u}},
    {"st-op2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd020000u}},
    {"st-op3", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd030000u}},
    {"st-op4", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd040000u}},
    {"st-op5", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd050000u}},
    {"st-op6", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd060000u}},
    {"st-op7", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd070000u}},
    {"st-mask", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcf05002u, 0xdd050002u}},
    {"st4444-incr", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd040000u}},
    {"st4444-decr", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd050000u}},
    {"st4444-repl", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd020000u}},
    {"st4444-lt", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff8004u, 0xdd040003u}},
    {"st5551-incr", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd040000u}},
    {"st5551-decr", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd050000u}},
    {"st5551-repl7f", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff7f01u, 0xdd020000u}},
    {"st5551-inv", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd030000u}},
    {"st565-repl", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x24000001u, 0xdcff5a01u, 0xdd020000u}},
    {"z-f0", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000000u}},
    {"z-f1", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000001u}},
    {"z-f2", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000002u}},
    {"z-f3", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000003u}},
    {"z-f4", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000004u}},
    {"z-f5", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000005u}},
    {"z-f6", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000006u}},
    {"z-f7", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000007u}},
    {"z-lt-st", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000004u, 0x24000001u, 0xdcff0001u, 0xdd040300u}},
    {"z-gt-st", GR_K_ZSPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x23000001u, 0xde000006u, 0x24000001u, 0xdcff6006u, 0xdd040302u}},
    {"tf-mod-rgb", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000000u, 0xca30a0e0u}},
    {"tf-mod-rgb-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010000u, 0xca30a0e0u}},
    {"tf-mod-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000100u, 0xca30a0e0u}},
    {"tf-mod-rgba-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010100u, 0xca30a0e0u}},
    {"tf-dec-rgb", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000001u, 0xca30a0e0u}},
    {"tf-dec-rgb-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010001u, 0xca30a0e0u}},
    {"tf-dec-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000101u, 0xca30a0e0u}},
    {"tf-dec-rgba-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010101u, 0xca30a0e0u}},
    {"tf-bld-rgb", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000002u, 0xca30a0e0u}},
    {"tf-bld-rgb-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010002u, 0xca30a0e0u}},
    {"tf-bld-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000102u, 0xca30a0e0u}},
    {"tf-bld-rgba-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010102u, 0xca30a0e0u}},
    {"tf-rep-rgb", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000003u, 0xca30a0e0u}},
    {"tf-rep-rgb-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010003u, 0xca30a0e0u}},
    {"tf-rep-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000103u, 0xca30a0e0u}},
    {"tf-rep-rgba-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010103u, 0xca30a0e0u}},
    {"tf-add-rgb", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000004u, 0xca30a0e0u}},
    {"tf-add-rgb-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010004u, 0xca30a0e0u}},
    {"tf-add-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9000104u, 0xca30a0e0u}},
    {"tf-add-rgba-x2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc9010104u, 0xca30a0e0u}},
    {"tf-mod-rgba-w", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0xffffffffu, {0xc9000100u}},
    {"tf-mod-rgba-1", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x01010101u, {0xc9000100u}},
    {"tfa-mod-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_WHITE, 0u, 0x9aff7f20u, {0xc9000100u, 0xca30a0e0u, 0x21000001u, 0xdf00002au, 0xe0000000u}},
    {"tfa-dec-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_WHITE, 0u, 0x9aff7f20u, {0xc9000101u, 0xca30a0e0u, 0x21000001u, 0xdf00002au, 0xe0000000u}},
    {"tfa-bld-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_WHITE, 0u, 0x9aff7f20u, {0xc9000102u, 0xca30a0e0u, 0x21000001u, 0xdf00002au, 0xe0000000u}},
    {"tfa-rep-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_WHITE, 0u, 0x9aff7f20u, {0xc9000103u, 0xca30a0e0u, 0x21000001u, 0xdf00002au, 0xe0000000u}},
    {"tfa-add-rgba", GR_K_SPRITE, 3u, GR_T_8888, GR_D_WHITE, 0u, 0x9aff7f20u, {0xc9000104u, 0xca30a0e0u, 0x21000001u, 0xdf00002au, 0xe0000000u}},
    {"tfa-mod-rgb", GR_K_SPRITE, 3u, GR_T_8888, GR_D_WHITE, 0u, 0x9aff7f20u, {0xc9000000u, 0xca30a0e0u, 0x21000001u, 0xdf00002au, 0xe0000000u}},
    {"tx-5650", GR_K_SPRITE, 3u, GR_T_5650, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000000u}},
    {"tx-5551", GR_K_SPRITE, 3u, GR_T_5551, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000001u}},
    {"tx-4444", GR_K_SPRITE, 3u, GR_T_4444, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000002u}},
    {"tx-c8-8888", GR_K_SPRITE, 3u, GR_T_CLUT8, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000005u, 0xc500ff03u}},
    {"tx-c8-sh4", GR_K_SPRITE, 3u, GR_T_CLUT8, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000005u, 0xc5020f13u}},
    {"tx-c8-5551", GR_K_SPRITE, 3u, GR_T_CLUT8, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000005u, 0xc500ff01u}},
    {"tx-c4-4444", GR_K_SPRITE, 3u, GR_T_CLUT4, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000004u, 0xc500ff02u}},
    {"tx-c4-off3", GR_K_SPRITE, 3u, GR_T_CLUT4, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc3000004u, 0xc503ff03u}},
    {"wr-c-neg", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc7000101u}},
    {"wr-w-neg", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc7000000u}},
    {"wr-uw-vc", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc7000100u}},
    {"wr-c-big", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 1u, 0x9aff7f20u, {0xc7000101u}},
    {"wr-w-big", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 1u, 0x9aff7f20u, {0xc7000000u}},
    {"wr-w-min4", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 2u, 0x9aff7f20u, {0xc7000000u}},
    {"wr-w-neg-lin", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc7000000u, 0xc6000101u}},
    {"wr-c-neg-lin", GR_K_WRAP, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xc7000101u, 0xc6000101u}},
    {"e-tri0", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri1", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 1u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri2", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 2u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri3", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 3u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri4", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 4u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri5", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 5u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri6", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 6u, 0x9aff7f20u, {0x1e000000u}},
    {"e-tri7", GR_K_TRI, 3u, GR_T_8888, GR_D_PATTERN, 7u, 0x9aff7f20u, {0x1e000000u}},
    {"fmt-565", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0u}},
    {"fmt-5551", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0u}},
    {"fmt-4444", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0u}},
    {"dth-565", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x20000001u}},
    {"dth-5551", GR_K_SPRITE, 1u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x20000001u}},
    {"dth-4444", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x20000001u}},
    {"dth-8888", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x20000001u}},
    {"dth-565-b", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x20000001u, 0x21000001u, 0xdf000032u}},
    {"msk-8888", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xe80f55f0u, 0xe90000c3u}},
    {"msk-565", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xe8f0f0f0u}},
    {"msk-4444", GR_K_SPRITE, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xe8000f0fu, 0xe90000f0u}},
    {"msk-8888-st", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0xe9000f0fu, 0x24000001u, 0xdcff5a01u, 0xdd020000u}},
    {"lo-0", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000000u}},
    {"lo-1", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000001u}},
    {"lo-2", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000002u}},
    {"lo-3", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000003u}},
    {"lo-4", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000004u}},
    {"lo-5", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000005u}},
    {"lo-6", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000006u}},
    {"lo-7", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000007u}},
    {"lo-8", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000008u}},
    {"lo-9", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000009u}},
    {"lo-10", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe600000au}},
    {"lo-11", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe600000bu}},
    {"lo-12", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe600000cu}},
    {"lo-13", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe600000du}},
    {"lo-14", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe600000eu}},
    {"lo-15", GR_K_SPRITE, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe600000fu}},
    {"lo-565-xor", GR_K_SPRITE, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x28000001u, 0xe6000006u}},
    {"clr-c", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000101u}},
    {"clr-a", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000201u}},
    {"clr-ca", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000301u}},
    {"clr-ca-msk", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000301u, 0xe80f55f0u, 0xe90000c3u}},
    {"clr-ca-dth565", GR_K_CLEAR, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000301u, 0x20000001u}},
    {"clr-ca-dth8888", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000301u, 0x20000001u}},
    {"clr-c-565", GR_K_CLEAR, 0u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000101u}},
    {"clr-ca-blend", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000301u, 0x21000001u, 0xdf000032u}},
    {"clr-ca-atest", GR_K_CLEAR, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000301u, 0x22000001u, 0xdb000000u}},
    {"clr-a-4444", GR_K_CLEAR, 2u, GR_T_8888, GR_D_PATTERN, 0u, 0x5ac3a50fu, {0xd3000201u}},
    {"fog-a", GR_K_FOG, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x1e000000u, 0x1f000001u, 0xcf8040c0u, 0xcdc08000u, 0xce3c9249u}},
    {"fog-b", GR_K_FOG, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x1e000000u, 0x1f000001u, 0xcf0080ffu, 0xcd000000u, 0xce3c8080u}},
    {"fog-c", GR_K_FOG, 3u, GR_T_8888, GR_D_PATTERN, 0u, 0x9aff7f20u, {0x1e000000u, 0x1f000001u, 0xcfffffffu, 0xcd3f0000u, 0xce3c8000u}},
};

/* GR_K_WRAP texture rectangles: u0, v0 at the left/top edge, u1, v1 at the right/bottom. */
static const float gr_wrap_uv[3][4] = {
    {-8.0f, -2.0f, 8.0f, 2.0f}, {12.0f, 3.0f, 28.0f, 7.0f}, {-16.0f, -4.0f, 48.0f, 12.0f},
};

/* GR_K_TRI: two flat triangles (red, then green), x/y relative to the window origin. */
static const float gr_tris[8][12] = {
    {0.0f, 0.0f, 16.0f, 0.0f, 0.0f, 4.0f, 16.0f, 0.0f, 16.0f, 4.0f, 0.0f, 4.0f},
    {0.5f, 0.5f, 15.5f, 0.5f, 0.5f, 3.5f, 15.5f, 0.5f, 15.5f, 3.5f, 0.5f, 3.5f},
    {0.25f, 0.25f, 15.75f, 0.25f, 0.25f, 3.75f, 15.75f, 0.25f, 15.75f, 3.75f, 0.25f, 3.75f},
    {0.96875f, 0.96875f, 15.03125f, 0.03125f, 2.03125f, 3.03125f,
     8.53125f, 1.46875f, 15.96875f, 3.96875f, 4.46875f, 3.53125f},
    {0.0f, 0.5f, 16.0f, 1.0f, 0.0f, 1.5f, 0.0f, 2.25f, 16.0f, 2.0f, 16.0f, 3.75f},
    {0.0f, 0.0f, 7.5f, 0.0f, 7.5f, 4.0f, 7.5f, 0.0f, 16.0f, 4.0f, 7.5f, 4.0f},
    {0.0f, 2.0f, 8.0f, 0.0f, 16.0f, 2.0f, 0.0f, 2.0f, 16.0f, 2.0f, 8.0f, 4.0f},
    {0.0f, 0.0f, 0.0f, 4.0f, 16.0f, 0.0f, 16.0f, 0.0f, 0.0f, 4.0f, 16.0f, 4.0f},
};

/* Pattern byte `i` of stream `seed`: in every stream eight indices carry the edge values,
   the rest a fixed integer hash. */
static uint8_t gr_byte(uint32_t seed, uint32_t i)
{
    static const uint8_t edge[8] = {0x00u, 0xffu, 0x01u, 0xfeu, 0x7fu, 0x80u, 0x40u, 0xc0u};
    const uint32_t slot = (i * 5u + seed * 11u) & 63u;
    if (slot < 8u) {
        return edge[slot];
    }
    uint32_t x = seed * 0x9e3779b9u + i * 0x85ebca6bu + 0x165667b1u;
    x ^= x >> 15;
    x *= 0x2c1b3c6du;
    x ^= x >> 12;
    x *= 0x297a2d39u;
    x ^= x >> 15;
    return (uint8_t)(x >> 24);
}

/* An ABGR word from streams seed .. seed + 3 (R, G, B, A). */
static uint32_t gr_rgba(uint32_t seed, uint32_t i)
{
    return (uint32_t)gr_byte(seed, i) | ((uint32_t)gr_byte(seed + 1u, i) << 8) |
           ((uint32_t)gr_byte(seed + 2u, i) << 16) | ((uint32_t)gr_byte(seed + 3u, i) << 24);
}

/* Window pixel i before the draw, in the cell's framebuffer format. */
static uint32_t gr_dst(const struct gr_cell *c, uint32_t i)
{
    if (c->dst == GR_D_WHITE) {
        return c->psm == 3u ? 0xffffffffu : 0xffffu;
    }
    if (c->psm == 3u) {
        return gr_rgba(5u, i);
    }
    return (uint32_t)gr_byte(9u, i) | ((uint32_t)gr_byte(10u, i) << 8);
}

/* GR_K_ZSPRITE: the depth window pixel i holds before the tested z = 0x8000 sprite. */
static uint32_t gr_depth(uint32_t i)
{
    static const uint16_t near_ref[8] = {0x8000u, 0x7fffu, 0x8001u, 0x0000u,
                                         0xffffu, 0x7ffeu, 0x8002u, 0x8000u};
    if ((i & 3u) == 0u) {
        return near_ref[(i >> 2) & 7u];
    }
    return ((uint32_t)gr_byte(40u, i) << 8) | gr_byte(41u, i);
}

/* Texture t's bytes (16x4 texels, buffer width 16); returns the byte count. */
static uint32_t gr_texture(uint32_t t, uint8_t *out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < GR_N; i++) {
        if (t == GR_T_8888) {
            const uint32_t v = gr_rgba(1u, i);
            for (uint32_t b = 0; b < 4u; b++) {
                out[n++] = (uint8_t)(v >> (8u * b));
            }
        } else if (t == GR_T_CLUT8) {
            out[n++] = gr_byte(13u, i);
        } else if (t == GR_T_CLUT4) {
            if (i < GR_N / 2u) {
                out[n++] = gr_byte(18u, i);
            }
        } else {
            out[n++] = gr_byte(21u + t, i);
            out[n++] = gr_byte(25u + t, i);
        }
    }
    return n;
}

/* The CLUT the CLUT8/CLUT4 cells load: 256 8888 entries (512 16-bit ones). */
static void gr_clut(uint8_t *out)
{
    for (uint32_t k = 0; k < GR_CLUT_BYTES / 4u; k++) {
        const uint32_t v = gr_rgba(30u, k);
        for (uint32_t b = 0; b < 4u; b++) {
            out[k * 4u + b] = (uint8_t)(v >> (8u * b));
        }
    }
}

static uint32_t gr_fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

/* One through-mode vertex: float u, v; 8888 colour; float x, y, z (vertex type 0x80019f). */
static uint32_t gr_tvtx(uint32_t *out, float u, float v, uint32_t col, float x, float y, float z)
{
    out[0] = gr_fbits(u);
    out[1] = gr_fbits(v);
    out[2] = col;
    out[3] = gr_fbits(x);
    out[4] = gr_fbits(y);
    out[5] = gr_fbits(z);
    return 6u;
}

/* The cell's vertex words; returns the count. */
static uint32_t gr_vertices(const struct gr_cell *c, uint32_t *out)
{
    const float x0 = (float)GR_WX, y0 = (float)GR_WY;
    const float x1 = x0 + (float)GR_WW, y1 = y0 + (float)GR_WH;
    uint32_t n = 0;
    if (c->kind == GR_K_ZSPRITE || c->kind == GR_K_FOG) {
        /* One 1x1 sprite per window pixel: a depth in clear mode, or a fogged colour. */
        for (uint32_t i = 0; i < GR_N; i++) {
            const float x = x0 + (float)(i % GR_WW), y = y0 + (float)(i / GR_WW);
            if (c->kind == GR_K_ZSPRITE) {
                const float z = (float)gr_depth(i);
                n += gr_tvtx(out + n, 0.0f, 0.0f, 0u, x, y, z);
                n += gr_tvtx(out + n, 0.0f, 0.0f, 0u, x + 1.0f, y + 1.0f, z);
            } else {
                /* Transform mode: colour, x, y, z (vertex type 0x19c) at view z = i. */
                for (uint32_t k = 0; k < 2u; k++) {
                    out[n++] = gr_rgba(1u, i);
                    out[n++] = gr_fbits(x + (float)k);
                    out[n++] = gr_fbits(y + (float)k);
                    out[n++] = gr_fbits((float)i);
                }
            }
        }
    }
    if (c->kind == GR_K_TRI) {
        for (uint32_t k = 0; k < 6u; k++) {
            n += gr_tvtx(out + n, 0.0f, 0.0f, k < 3u ? GR_RED : GR_GREEN,
                         x0 + gr_tris[c->arg][2u * k], y0 + gr_tris[c->arg][2u * k + 1u], GR_Z);
        }
    } else if (c->kind == GR_K_WRAP) {
        const float *uv = gr_wrap_uv[c->arg];
        n += gr_tvtx(out + n, uv[0], uv[1], c->vcol, x0, y0, GR_Z);
        n += gr_tvtx(out + n, uv[2], uv[3], c->vcol, x1, y1, GR_Z);
    } else if (c->kind != GR_K_FOG) {
        n += gr_tvtx(out + n, 0.0f, 0.0f, c->vcol, x0, y0, GR_Z);
        n += gr_tvtx(out + n, (float)GR_WW, (float)GR_WH, c->vcol, x1, y1, GR_Z);
    }
    return n;
}

/* The cell's display list for vertex words at `vtx`, texture t at tex[t] and the CLUT at
   `clut` (physical addresses); returns the word count. Every state the draw reads is set. */
static uint32_t gr_list(const struct gr_cell *c, uint32_t vtx, const uint32_t *tex, uint32_t clut,
                        uint32_t *out)
{
    static const uint8_t off_units[12] = {0x17u, 0x1cu, 0x1du, 0x1fu, 0x20u, 0x21u,
                                          0x22u, 0x23u, 0x24u, 0x25u, 0x27u, 0x28u};
    static const float ident43[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    /* Clip z = 0 and w = 1 for every vertex, so the view z only feeds fog. */
    static const float proj[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                   0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const uint32_t t = tex[c->tex];
    const uint32_t full = (GR_STRIDE - 1u) | ((GR_ROWS - 1u) << 10);
    uint32_t n = 0;
#define GR_W(cmd, data) (out[n++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(data) & 0x00ffffffu))
    GR_W(0x10, (vtx >> 8) & 0x000f0000u);
    GR_W(0x9c, GR_FB_OFFSET);
    GR_W(0x9d, GR_STRIDE);
    GR_W(0xd2, c->psm);
    GR_W(0x9e, GR_ZB_OFFSET);
    GR_W(0x9f, GR_STRIDE);
    GR_W(0x15, 0);
    GR_W(0x16, full);
    GR_W(0xd4, 0);
    GR_W(0xd5, full);
    GR_W(0x4c, 0);
    GR_W(0x4d, 0);
    GR_W(0xd6, 0);
    GR_W(0xd7, 0xffffu);
    for (uint32_t i = 0; i < 12u; i++) {
        GR_W(off_units[i], 0);
    }
    GR_W(0x50, 0);              /* flat shading */
    GR_W(0xd3, 0);              /* clear mode off */
    GR_W(0xe7, 1);              /* depth writes off */
    GR_W(0xe8, 0);
    GR_W(0xe9, 0);
    GR_W(0xdb, 0x00ff0001u);    /* alpha test ALWAYS */
    GR_W(0xd8, 1);              /* colour test ALWAYS */
    GR_W(0xd9, 0);
    GR_W(0xda, 0);
    GR_W(0xdc, 0x00ff0001u);    /* stencil test ALWAYS */
    GR_W(0xdd, 0);              /* KEEP, KEEP, KEEP */
    GR_W(0xde, 1);              /* depth test ALWAYS */
    GR_W(0xdf, 0x32u);
    GR_W(0xe0, 0);
    GR_W(0xe1, 0);
    GR_W(0xe6, 3);              /* logic op COPY */
    GR_W(0xe2, 0x1d0cu);        /* the PSP SDK default dither matrix */
    GR_W(0xe3, 0xf3e2u);
    GR_W(0xe4, 0x0c1du);
    GR_W(0xe5, 0xe2f3u);
    GR_W(0xcf, 0);
    GR_W(0xcd, 0);
    GR_W(0xce, 0);
    GR_W(0x55, 0xffffffu);
    GR_W(0x58, 0xffu);
    GR_W(0x1e, 1);              /* texture on: UV, linear, one level, 8888 */
    GR_W(0xc0, 0);
    GR_W(0xc2, 0);
    GR_W(0xc3, 3);
    GR_W(0xa0, t);
    GR_W(0xa8, ((t >> 8) & 0x000f0000u) | GR_WW);
    GR_W(0xb8, (2u << 8) | 4u); /* 16x4 */
    GR_W(0xc6, 0);              /* nearest */
    GR_W(0xc7, 0x101u);         /* clamp u and v */
    GR_W(0xc8, 0);
    GR_W(0xc9, 0x103u);         /* REPLACE, RGBA */
    GR_W(0xca, 0);
    GR_W(0x48, gr_fbits(1.0f) >> 8);
    GR_W(0x49, gr_fbits(1.0f) >> 8);
    GR_W(0x4a, 0);
    GR_W(0x4b, 0);
    if (c->tex == GR_T_CLUT8 || c->tex == GR_T_CLUT4) {
        GR_W(0xb0, clut);
        GR_W(0xb1, (clut >> 8) & 0x000f0000u);
        GR_W(0xc5, 0x00ff03u);
        GR_W(0xc4, GR_CLUT_BYTES / 32u);
    }
    GR_W(0xcb, 0);              /* texture flush */
    GR_W(0x12, 0x0080019fu);    /* through, float uv, 8888 colour, float position */
    GR_W(0x01, vtx);
    if (c->kind == GR_K_ZSPRITE) {
        GR_W(0xe7, 0);
        GR_W(0xd3, 0x401u);     /* clear mode, depth only: one 1x1 sprite per pixel */
        GR_W(0x04, (6u << 16) | (2u * GR_N));
        GR_W(0xd3, 0);
        GR_W(0xe7, 1);
        GR_W(0x01, vtx + 2u * GR_N * 24u);
    }
    for (uint32_t i = 0; i < 6u && c->w[i] != 0u; i++) {
        out[n++] = c->w[i];
    }
    if (c->kind == GR_K_FOG) {
        GR_W(0x3a, 0);
        for (uint32_t i = 0; i < 12u; i++) {
            GR_W(0x3b, gr_fbits(ident43[i]) >> 8);
        }
        GR_W(0x3c, 0);
        for (uint32_t i = 0; i < 12u; i++) {
            GR_W(0x3d, gr_fbits(ident43[i]) >> 8);
        }
        GR_W(0x3e, 0);
        for (uint32_t i = 0; i < 16u; i++) {
            GR_W(0x3f, gr_fbits(proj[i]) >> 8);
        }
        GR_W(0x42, gr_fbits(1.0f) >> 8);
        GR_W(0x43, gr_fbits(1.0f) >> 8);
        GR_W(0x44, 0);
        GR_W(0x45, gr_fbits(2048.0f) >> 8);
        GR_W(0x46, gr_fbits(2048.0f) >> 8);
        GR_W(0x47, gr_fbits(32768.0f) >> 8);
        GR_W(0x4c, 2048u << 4);
        GR_W(0x4d, 2048u << 4);
        GR_W(0x12, 0x0000019cu);  /* transform, 8888 colour, float position */
        GR_W(0x04, (6u << 16) | (2u * GR_N));
    } else if (c->kind == GR_K_TRI) {
        GR_W(0x04, (3u << 16) | 6u);
    } else {
        GR_W(0x04, (6u << 16) | 2u);
    }
    GR_W(0x0f, 0);
    GR_W(0x0c, 0);
#undef GR_W
    return n;
}

/* FNV-1a 64 over the window's 64 result words, each little-endian. */
static uint64_t gr_digest(const uint32_t *px)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < GR_N; i++) {
        for (uint32_t b = 0; b < 4u; b++) {
            h ^= (px[i] >> (8u * b)) & 0xffu;
            h *= 0x100000001b3ull;
        }
    }
    return h;
}
/* ==== end of the GE raster cells ==== */

#define GR_WAIT_POLLS 5000u         /* 5000 x 100 us: a half-second bound per draw */
#define GR_WAIT_POLL_US 100u

static uint8_t s_gr_tex[GR_T_COUNT][GR_TEX_BYTES] __attribute__((aligned(64)));
static uint8_t s_gr_clut[GR_CLUT_BYTES] __attribute__((aligned(64)));
static uint32_t s_gr_list[GR_LIST_WORDS] __attribute__((aligned(64)));
static uint32_t s_gr_vtx[GR_VTX_WORDS] __attribute__((aligned(64)));
static uint32_t s_gr_win[GR_N];

static uint32_t gr_phys(const void *p)
{
    return (uint32_t)(uintptr_t)p & 0x0fffffffu;
}

/* The uncached mirror of the 8 KiB colour block at VRAM offset GR_FB_OFFSET. */
static uintptr_t gr_fb(void)
{
    const uint32_t vram = (uint32_t)(uintptr_t)sceGeEdramGetAddr();
    return (uintptr_t)((vram | 0x40000000u) + GR_FB_OFFSET);
}

/* One cell: clear the block, write the window's destination pattern, queue the list, wait
   (bounded), read the window back, then the records: `<cell>-draw` (the GE wait outcome and
   the window digest) and one `<cell>-rN` record per window row. */
static void gr_draw(const struct gr_cell *c, const uint32_t *tex, uint32_t clut)
{
    static char row[64];
    hle_step(c->name);
    volatile uint32_t *fb32 = (volatile uint32_t *)gr_fb();
    volatile uint16_t *fb16 = (volatile uint16_t *)gr_fb();
    for (unsigned i = 0; i < GR_STRIDE * GR_ROWS; i++) {
        fb32[i] = 0u;
    }
    for (uint32_t i = 0; i < GR_N; i++) {
        const uint32_t at = (GR_WY + i / GR_WW) * GR_STRIDE + GR_WX + i % GR_WW;
        if (c->psm == 3u) {
            fb32[at] = gr_dst(c, i);
        } else {
            fb16[at] = (uint16_t)gr_dst(c, i);
        }
    }
    __asm__ volatile("sync" ::: "memory");
    (void)gr_vertices(c, s_gr_vtx);
    const uint32_t words = gr_list(c, gr_phys(s_gr_vtx), tex, clut, s_gr_list);
    sceKernelDcacheWritebackAll();
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    const int qid = sceGeListEnQueue(s_gr_list, NULL, -1, NULL);
    int list_state = qid;
    int draw_state = -1;
    int idle = 0;
    for (unsigned i = 0; i < GR_WAIT_POLLS; i++) {
        list_state = qid >= 0 ? sceGeListSync(qid, 1) : qid;
        draw_state = sceGeDrawSync(1);
        idle = draw_state == PSP_GE_LIST_DONE;
        if (idle) {
            break;
        }
        sceKernelDelayThread(GR_WAIT_POLL_US);
    }
    const uint32_t elapsed = sceKernelGetSystemTimeLow() - t0;
    int reset_rc = 0;
    if (!idle) {
        PspGeBreakParam param;
        memset(&param, 0, sizeof(param));
        reset_rc = sceGeBreak(1, &param);
        for (unsigned i = 0; i < GR_WAIT_POLLS; i++) {
            if (sceGeDrawSync(1) == PSP_GE_LIST_DONE) {
                break;
            }
            sceKernelDelayThread(GR_WAIT_POLL_US);
        }
    }
    for (uint32_t i = 0; i < GR_N; i++) {
        const uint32_t at = (GR_WY + i / GR_WW) * GR_STRIDE + GR_WX + i % GR_WW;
        s_gr_win[i] = c->psm == 3u ? fb32[at] : (uint32_t)fb16[at];
    }
    const uint64_t digest = gr_digest(s_gr_win);
    const uint32_t draw[9] = {(uint32_t)qid, (uint32_t)list_state, (uint32_t)draw_state,
                              elapsed, (uint32_t)idle, (uint32_t)reset_rc, words,
                              (uint32_t)digest, (uint32_t)(digest >> 32)};
    snprintf(row, sizeof(row), "%s-draw", c->name);
    hle_record(row, qid < 0 ? "FAIL" : idle ? "PASS" : "TIMEOUT", (uint32_t)qid, draw, 9);
    for (uint32_t y = 0; y < GR_WH; y++) {
        snprintf(row, sizeof(row), "%s-r%u", c->name, (unsigned int)y);
        hle_record(row, "PASS", y, &s_gr_win[y * GR_WW], GR_WW);
    }
}

static int hle_run(void)
{
    uint32_t tex[GR_T_COUNT];
    for (uint32_t t = 0; t < GR_T_COUNT; t++) {
        (void)gr_texture(t, s_gr_tex[t]);
        tex[t] = gr_phys(s_gr_tex[t]);
    }
    gr_clut(s_gr_clut);
    for (uint32_t i = 0; i < GR_CELL_COUNT; i++) {
        gr_draw(&gr_cells[i], tex, gr_phys(s_gr_clut));
    }
    return 1;
}

#endif /* GE_CASE_RASTER */

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    s_hle_main_thread = sceKernelGetThreadId();
    hle_begin();
    const int ok = hle_run();
    hle_finish(ok);
    hle_park_until_stop();
    return 0;
}
