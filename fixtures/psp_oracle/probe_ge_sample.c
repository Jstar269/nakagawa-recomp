// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// GE texture sample-point cells for the PSP-3000 oracle, one PRX and one launch:
//   CASE=ge-sample (PSP-GE-SAMPLE-001): which texel a through-mode (vertex type bit 23)
//     textured triangle pair and a through-mode sprite show at each pixel, under nearest
//     and bilinear filtering, for 1:1, +0.5-texel, 2x magnification, 20->12 horizontal
//     minification and 16-bit vertex cells.
//
// Each draw is one raw display list (no libpspgu) into a 64x32 8888 framebuffer at VRAM
// offset 0x100000, cleared by the CPU through the uncached VRAM mirror before the list is
// queued. The texture is a 32x32 8888 image in main RAM whose texel (u, v) is
// R = 8u, G = 8v, A = 0xff and, in texture "u", B = 0xff on odd columns (0 on even); in
// texture "v", B = 0xff on odd rows. R and G name the texel; B gives the blend weight of
// the odd column (or row) to 1/255. No blending, alpha test, depth test, fog, dither or
// colour test; texture function REPLACE with RGBA; clamp in u and v. After the GE is idle
// the 16x16 pixel window x 3..18, y 3..18 is read back through the uncached mirror and
// recorded raw, one record per row (out0 = x 3 ... out15 = x 18).
//
// Record stream and log: the same writer, round trip and module lifecycle as
// probe_hle_issues.c (copied byte for byte): NAKAGAWA_PSP_META, a NAKAGAWA_PSP_STEP
// before each draw, a `<draw>-draw` record with the GE wait outcome, sixteen
// `<draw>-rNN` row records, a `ge-sample-done` terminal record whose out0 counts the
// records, the host0 round trip and a final NAKAGAWA_PSP_COMPLETE line. Every GE wait is
// bounded (sceGeDrawSync(1) peeks against a deadline); a list that does not finish is a
// TIMEOUT record and every queue is reset (sceGeBreak(1)) before the next draw. The probe
// writes only its own RAM, the 8 KiB framebuffer block at VRAM 0x100000 and host0 logs.

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

#if PSP_ORACLE_CASE == GE_CASE_SAMPLE
#define HLE_CAMPAIGN_ID "ge-sample"
#define HLE_TEST_ID "PSP-GE-SAMPLE-001"
#define HLE_LOG "host0:/ge_sample_log.txt"
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
