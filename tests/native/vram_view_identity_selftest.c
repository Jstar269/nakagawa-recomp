// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * The pause menu's VRAM viewer leaves the game as it found it (#314).
 *
 * The viewer decodes guest VRAM while the game is paused at a VBLANK: the displayed framebuffer,
 * a window the user steps to (here the render-to-texture source), the depth buffer and the bound
 * texture, with the same calls and arguments gui_pause_describe_vram and gui_pause_decode_vram
 * use. On the Vulkan backend those reads go through sr_guest_copy_in, which materialises
 * GPU-resident targets into guest VRAM and applies CPU stores the backend was still holding. A
 * software-GE test would pass without reaching any of that, so this one runs ge.c's lists on
 * ge_gpu.c's real targets, as gpu-coherence-selftest does.
 *
 * Each frame draws into target T1, stores into it from the CPU (whole pixels, 16-bit stores
 * into the middle of a pixel, a byte store), draws T1 as a texture into T2, presents T2 with a
 * capture armed, then stores into T2 from the CPU. A run writes a digest of every presented
 * frame, the lists' return codes, and at the end guest VRAM and RAM as a guest read sees them
 * (through sr_guest_copy_in) and the GE state.
 *
 *   --write PATH          run without the viewer and write the digest to PATH
 *   --check PATH          run again and require the same digest
 *   --view-at N           with --check: run the viewer's decodes at frame N's VBLANK, check what
 *                         they show, and require that they moved bytes into guest VRAM (so the
 *                         run is not trivially the same)
 *   --scale N             SR_GPU_SCALE, before the backend starts
 *
 * Built with -DNK_VRAM_VIEW_MUTANT, the viewer also stores one byte into T1 where nothing
 * redraws it; a --check run must then fail on the frames presented after it.
 *
 * Exit 0 pass, 1 fail, 2 usage or setup, 77 no Vulkan device.
 */

#define _POSIX_C_SOURCE 200809L /* setenv under -std=c11 */

#include "fbcap.h"
#include "ge_shared.h"
#include "gpu_sdl3vk/ge_gpu.h"
#include "gpu_sdl3vk/sdl3vk.h"
#include "recomp.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Standalone support required by recomp.h and the production GE object (as in
 * gpu_coherence_selftest.c). The test owns a synthetic guest arena and reads no game data. */
uint8_t *g_mem;
uint32_t g_sr_debug;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count;
int g_sr_metadata_watch;
int g_sr_heap_watch;
int g_hle_depth;
CpuState *s_cpu;
int g_sr_last_writer_enabled;
uint32_t g_sr_store_context_pc;
unsigned g_sr_store_context_count;
unsigned g_sr_store_context_limit;
uint32_t g_sr_mem_watch_context_pc;
unsigned g_sr_mem_watch_context_limit;
unsigned g_sr_mem_watch_context_count;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value;
int g_sr_store_context_mem_gpr;
uint32_t g_sr_store_context_mem_offset;
unsigned g_sr_store_context_mem_words;

void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)addr; (void)width; (void)value; (void)pc;
}
void sr_add_mem_watch(uint32_t start, uint32_t end, const char *label) {
    (void)start; (void)end; (void)label;
}
void sr_add_value_watch(uint32_t value, const char *label) {
    (void)value; (void)label;
}
void sr_debug_init_watches(void) {}
void sr_last_writer_reset(void) {}
int sr_find_last_writer(uint32_t addr, uint32_t width,
                        uint32_t *write_addr, uint32_t *write_width,
                        uint32_t *value, uint32_t *pc) {
    (void)addr; (void)width; (void)write_addr; (void)write_width;
    (void)value; (void)pc;
    return 0;
}
void sr_oor(uint32_t addr, uint32_t value, int store) {
    fprintf(stderr, "vram view identity: out-of-range %s addr=0x%08x value=0x%08x\n",
            store ? "write" : "read", addr, value);
}
void sr_heap_note_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)addr; (void)width; (void)value; (void)pc;
}
void sr_heap_note_bulk_write(uint32_t addr, uint32_t width, uint32_t pc) {
    (void)addr; (void)width; (void)pc;
}
uint32_t sched_current_uid(void) { return 0; }
uint32_t sr_get_ge_status(void) { return 0; }

/* ---- the scene ------------------------------------------------------------------------- */

#define FRAMES   6u
#define STRIDE   512u
#define PSP_W    480u
#define PSP_H    272u
#define T1       0x04000000u   /* drawn, CPU-stored, then sampled as a texture */
#define T2       0x04088000u   /* drawn from T1 and shown */
#define ZBUF     0x04110000u   /* shared by both lists */
#define LISTS    0x08100000u   /* two lists a frame, 0x1000 bytes each */
#define VERTS    0x08200000u   /* 0x1000 bytes a frame */
#define RAM_END  0x08210000u

/* Through-mode vertex types: colour 8888 + float position, and float UV + float position. */
#define VT_COLOR ((7u << 2) | (3u << 7) | (1u << 23))
#define VT_TEX   (3u | (3u << 7) | (1u << 23))

typedef struct { uint32_t color; float x, y, z; } ColorVtx;
typedef struct { float u, v, x, y, z; } TexVtx;

static uint32_t c_clear1(uint32_t f) { return 0xff102030u + f * 0x00010203u; }
static uint32_t c_sprite(uint32_t f) { return 0xff2080e0u ^ (f * 0x00071305u); }
static uint32_t c_tri(uint32_t f)    { return 0xff40c060u + f * 0x00020100u; }
static uint32_t c_clear2(uint32_t f) { return 0xff000000u | (0x30u + f); }
static uint32_t c_bar(uint32_t f)    { return 0xffe0e0e0u - f * 0x00010101u; }
static uint32_t z_sprite(uint32_t f) { return 0x4000u + f * 0x100u; }
static uint32_t z_rtt(uint32_t f)    { return 0x6000u + f * 0x100u; }

typedef struct { uint32_t *dl; uint32_t n; } List;

static void W(List *l, uint32_t cmd, uint32_t data) {
    l->dl[l->n++] = (cmd << 24) | (data & 0x00FFFFFFu);
}

static void target(List *l, uint32_t fb) {
    W(l, 0x10, (VERTS >> 8) & 0x000F0000u);             /* BASE: VADDR's high bits */
    W(l, 0x9C, fb & 0x00FFFFFFu);
    W(l, 0x9D, STRIDE | ((fb & 0xFF000000u) >> 8));
    W(l, 0x9E, ZBUF & 0x00FFFFFFu);
    W(l, 0x9F, STRIDE | ((ZBUF & 0xFF000000u) >> 8));
    W(l, 0xD2, 3);                                      /* 8888 */
    W(l, 0x15, 0);
    W(l, 0x16, ((PSP_H - 1u) << 10) | (PSP_W - 1u));
    W(l, 0xD4, 0);
    W(l, 0xD5, ((PSP_H - 1u) << 10) | (PSP_W - 1u));
    W(l, 0xD6, 0);
    W(l, 0xD7, 0xFFFF);
    W(l, 0x1D, 0);                                      /* no culling */
    W(l, 0x21, 0);                                      /* no blending */
    W(l, 0x22, 0);
    W(l, 0x24, 0);
    W(l, 0x27, 0);
    W(l, 0x28, 0);
    W(l, 0x23, 1);                                      /* depth test, always, writes on */
    W(l, 0xDE, 1);
    W(l, 0xE7, 0);
}

static void color_prim(List *l, ColorVtx **v, uint32_t type, const ColorVtx *src, uint32_t n) {
    const uint32_t addr = VERTS + (uint32_t)((uint8_t *)*v - SR_HOST(VERTS));
    memcpy(*v, src, n * sizeof(*src));
    *v += n;
    W(l, 0x12, VT_COLOR);
    W(l, 0x01, addr & 0x00FFFFFFu);
    W(l, 0x04, (type << 16) | n);
}

static void finish(List *l) {
    W(l, 0x0F, 0);
    W(l, 0x0C, 0);
}

/* List A: clear T1 and depth (all of it at frame 0, then rows 0-99, so the rest carries over
 * from frame to frame as a game's partly redrawn target does), a flat sprite and a flat
 * triangle that move with the frame. */
static uint32_t build_list_a(uint32_t f, ColorVtx **v) {
    const uint32_t addr = LISTS + f * 0x2000u;
    List l = { (uint32_t *)SR_HOST(addr), 0 };
    target(&l, T1);
    W(&l, 0x1E, 0);
    W(&l, 0x50, 0);                                     /* flat shading */
    W(&l, 0xD3, 1u | (7u << 8));                        /* clear colour, stencil, depth */
    const float clear_h1 = f ? 100.0f : (float)PSP_H;
    const ColorVtx clear[2] = { { c_clear1(f), 0, 0, 0x1000 }, { c_clear1(f), PSP_W, clear_h1, 0x1000 } };
    color_prim(&l, v, 6, clear, 2);
    W(&l, 0xD3, 0);
    const float x0 = 40.0f + 16.0f * (float)f;
    const ColorVtx spr[2] = { { c_sprite(f), x0, 30, (float)z_sprite(f) },
                              { c_sprite(f), x0 + 160.0f, 150, (float)z_sprite(f) } };
    color_prim(&l, v, 6, spr, 2);
    const ColorVtx tri[3] = { { c_tri(f), 300.0f + 8.0f * (float)f, 160, 0x5000 },
                              { c_tri(f), 460.0f, 170, 0x5000 },
                              { c_tri(f), 380.0f, 262, 0x5000 } };
    color_prim(&l, v, 3, tri, 3);
    finish(&l);
    return addr;
}

/* List B: clear T2 (all of it at frame 0, then rows 0-199), draw two 1:1 windows of T1 as a
 * texture (render-to-texture), then a bar. */
static uint32_t build_list_b(uint32_t f, ColorVtx **v) {
    const uint32_t addr = LISTS + f * 0x2000u + 0x1000u;
    List l = { (uint32_t *)SR_HOST(addr), 0 };
    target(&l, T2);
    W(&l, 0x1E, 0);
    W(&l, 0xD3, 1u | (7u << 8));
    const float clear_h2 = f ? 200.0f : (float)PSP_H;
    const ColorVtx clear[2] = { { c_clear2(f), 0, 0, 0x0800 }, { c_clear2(f), PSP_W, clear_h2, 0x0800 } };
    color_prim(&l, v, 6, clear, 2);
    W(&l, 0xD3, 0);
    W(&l, 0x1E, 1);
    W(&l, 0xA0, T1 & 0x00FFFFFFu);
    W(&l, 0xA8, STRIDE | ((T1 & 0xFF000000u) >> 8));
    W(&l, 0xB8, (8u << 8) | 9u);                        /* 512 x 256 */
    W(&l, 0xC0, 0);
    W(&l, 0xC2, 0);
    W(&l, 0xC3, 3);
    W(&l, 0xC6, 0);                                     /* nearest */
    W(&l, 0xC7, 1u | (1u << 8));                        /* clamp */
    W(&l, 0xC9, 3u | (1u << 8));                        /* replace, RGBA */
    W(&l, 0xCB, 0);                                     /* TEXFLUSH */
    const float z = (float)z_rtt(f);
    TexVtx *t = (TexVtx *)*v;
    const uint32_t taddr = VERTS + (uint32_t)((uint8_t *)t - SR_HOST(VERTS));
    t[0] = (TexVtx){ 0, 0, 16, 16, z };
    t[1] = (TexVtx){ 240, 128, 256, 144, z };
    t[2] = (TexVtx){ 280, 140, 230, 130, z };
    t[3] = (TexVtx){ 470, 252, 420, 242, z };
    *v = (ColorVtx *)(t + 4);
    W(&l, 0x12, VT_TEX);
    W(&l, 0x01, taddr & 0x00FFFFFFu);
    W(&l, 0x04, (6u << 16) | 4u);
    W(&l, 0x1E, 0);
    const ColorVtx bar[2] = { { c_bar(f), 0, 250, 0x7000 }, { c_bar(f), PSP_W, 266, 0x7000 } };
    color_prim(&l, v, 6, bar, 2);
    finish(&l);
    return addr;
}

/* The game's CPU stores, through the production store path (recomp.h). */
static void store_t1(uint32_t f) {
    for (uint32_t x = 300; x < 364; x++)                /* row 180, under the triangle: whole pixels */
        sr_w32_pc_slow(T1 + (180u * STRIDE + x) * 4u, 0xff000000u | (x * 0x010203u) | f, 0);
    for (uint32_t k = 0; k < 8; k++)                    /* row 120, kept, partly under the sprite */
        sr_w16_pc_slow(T1 + (120u * STRIDE + 100u + 23u * k) * 4u + 2u, (uint16_t)(0x6c00u + 7u * k + f), 0);
    for (uint32_t k = 0; k < 10; k++)                   /* row 60, inside the sprite: mid-pixel */
        sr_w16_pc_slow(T1 + (60u * STRIDE + 140u + 3u * k) * 4u + 1u, (uint16_t)(0xa050u + k + f), 0);
    sr_w8_pc_slow(T1 + (70u * STRIDE + 150u) * 4u + 2u, (uint8_t)(0x11u + f), 0);
}

static void store_t2(uint32_t f) {
    for (uint32_t x = 10; x < 40; x++)                  /* row 230, kept: whole pixels */
        sr_w32_pc_slow(T2 + (230u * STRIDE + x) * 4u, 0xff00ff00u ^ (x << 16) ^ f, 0);
    for (uint32_t k = 0; k < 6; k++)                    /* row 100, inside the first window */
        sr_w16_pc_slow(T2 + (100u * STRIDE + 60u + 5u * k) * 4u + 3u, (uint16_t)(0x3c00u + k + f), 0);
}

/* ---- the digest ------------------------------------------------------------------------ */

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
#define FNV0 0xcbf29ce484222325ull

/* Guest memory as a guest read sees it: through sr_guest_copy_in, which materialises it. */
static int guest_hash(uint32_t addr, uint32_t size, uint64_t *out) {
    static uint8_t chunk[0x10000];
    uint64_t h = FNV0;
    for (uint32_t at = 0; at < size; at += sizeof(chunk)) {
        const uint32_t n = size - at < sizeof(chunk) ? size - at : (uint32_t)sizeof(chunk);
        if (!sr_guest_copy_in(chunk, addr + at, n)) return 0;
        h = fnv(h, chunk, n);
    }
    *out = h;
    return 1;
}

static int file_hash(const char *path, uint64_t *out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint8_t buf[0x4000];
    uint64_t h = FNV0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) h = fnv(h, buf, n);
    const int ok = !ferror(fp);
    fclose(fp);
    *out = h;
    return ok;
}

/* ---- the viewer ------------------------------------------------------------------------ */

static uint32_t s_px[GE_VRAM_VIEW_MAX_WIDTH * GE_VRAM_VIEW_MAX_HEIGHT];
static uint8_t s_before[ZBUF + STRIDE * PSP_H * 2u - T1];

static int expect_px(const char *what, uint32_t got, uint32_t want, uint32_t mask) {
    if ((got & mask) == (want & mask)) return 1;
    fprintf(stderr, "vram view identity: FAIL %s shows 0x%08x, want 0x%08x (mask 0x%08x)\n",
            what, got, want, mask);
    return 0;
}

/* What the page does at a VBLANK, for each source it offers: the arguments are the ones
 * gui_pause_describe_vram and gui_pause_decode_vram pass. Checks that each decode shows the
 * scene, and that the decodes moved bytes into guest VRAM. */
static int run_viewer(uint32_t f) {
    const GeState *ge = ge_state_ptr();
    const uint32_t rgb = 0x00FFFFFFu;
    int ok = 1;
    memcpy(s_before, SR_HOST(T1), sizeof(s_before));

    /* The displayed framebuffer: T2. Its first window shows T1 (150, 90) at (166, 106). */
    ok &= ge_vram_decode_surface(T2, PSP_W, PSP_H, STRIDE, 3u, s_px);
    ok &= expect_px("display (166,106)", s_px[106u * PSP_W + 166u], c_sprite(f), rgb);
    ok &= expect_px("display bar", s_px[258u * PSP_W + 5u], c_bar(f), rgb);
    /* The draw target the GE state names (T2 too, after list B). */
    ok &= ge_framebuffer() == T2 && ge->fbw == STRIDE;
    ok &= ge_vram_decode_surface(ge_framebuffer(), PSP_W, PSP_H, ge->fbw, ge->fbfmt & 3u, s_px);
    /* A window the user stepped to: T1, the texture source, with the CPU stores in it. */
    ok &= ge_vram_decode_surface(T1, PSP_W, PSP_H, STRIDE, 3u, s_px);
    ok &= expect_px("T1 sprite", s_px[90u * PSP_W + 150u], c_sprite(f), rgb);
    ok &= expect_px("T1 row 180", s_px[180u * PSP_W + 300u], 0xff000000u | (300u * 0x010203u) | f, ~0u);
    {
        const uint32_t k = 4u, x = 140u + 3u * k;
        const uint32_t v16 = 0xa050u + k + f;
        const uint32_t want = (c_sprite(f) & 0xff0000ffu) | (v16 << 8);
        ok &= expect_px("T1 split pixel", s_px[60u * PSP_W + x], want, rgb);
    }
    ok &= expect_px("T1 byte store", s_px[70u * PSP_W + 150u],
                    (c_sprite(f) & 0xff00ffffu) | ((0x11u + f) << 16), rgb);
    /* The depth buffer, as DEPTH16 grey: the RTT window's depth at (166, 106). */
    ok &= ge_vram_decode_surface(ZBUF, PSP_W, PSP_H, STRIDE, 11u, s_px);
    {
        const uint32_t g = z_rtt(f) >> 8;
        ok &= expect_px("depth", s_px[106u * PSP_W + 166u], 0xff000000u | g | (g << 8) | (g << 16), ~0u);
    }
    /* The bound texture: T1 through the texture decoder, 512 x 256. */
    ok &= ge->tex_addr == T1 && ge->tex_w == 512 && ge->tex_h == 256;
    ok &= ge_vram_decode_bound_texture(s_px, sizeof(s_px) / sizeof(s_px[0]));
    ok &= expect_px("texture", s_px[90u * 512u + 150u], c_sprite(f), rgb);

#ifdef NK_VRAM_VIEW_MUTANT
    sr_w8_pc_slow(T1 + (140u * STRIDE + 420u) * 4u, 0x77u, 0);   /* a viewer that writes */
#endif
    size_t moved = 0;
    const uint8_t *now = SR_HOST(T1);
    for (size_t i = 0; i < sizeof(s_before); i++) moved += now[i] != s_before[i];
    printf("vram view identity: viewer at frame %u moved %zu bytes into guest VRAM\n", f, moved);
    if (!moved) {
        fprintf(stderr, "vram view identity: FAIL the viewer moved nothing into guest VRAM, so this "
                        "run proves nothing\n");
        ok = 0;
    }
    if (!ok) fprintf(stderr, "vram view identity: FAIL the viewer's decodes at frame %u\n", f);
    return ok;
}

/* ---- the run --------------------------------------------------------------------------- */

#define MAX_LINES 32
static char s_lines[MAX_LINES][96];
static unsigned s_nlines;

static void line(const char *fmt, uint64_t a, uint64_t b) {
    if (s_nlines < MAX_LINES)
        snprintf(s_lines[s_nlines++], sizeof(s_lines[0]), fmt, a, b);
}

static int run(int view_at, const char *capture_path) {
    uint64_t codes = FNV0;
    for (uint32_t f = 0; f < FRAMES; f++) {
        ColorVtx *v = (ColorVtx *)SR_HOST(VERTS + f * 0x1000u);
        const uint32_t a = build_list_a(f, &v);
        const uint32_t b = build_list_b(f, &v);
        uint32_t rc = ge_run_list(a, 0);
        codes = fnv(codes, &rc, sizeof(rc));
        store_t1(f);
        rc = ge_run_list(b, 0);
        codes = fnv(codes, &rc, sizeof(rc));
        if (!sr_capture_arm(capture_path)) {
            fprintf(stderr, "vram view identity: FAIL capture not armed at frame %u\n", f);
            return 0;
        }
        const int shown = gegpu_present(T2, 3, STRIDE);
        uint64_t h = 0;
        if (shown != GEGPU_PRESENT_SHOWN || sr_capture_result() != 1 || !file_hash(capture_path, &h)) {
            fprintf(stderr, "vram view identity: FAIL frame %u present=%d capture=%d\n", f, shown,
                    sr_capture_result());
            return 0;
        }
        remove(capture_path);
        line("present %" PRIu64 " %016" PRIx64, f, h);
        store_t2(f);
        if ((int)f == view_at && !run_viewer(f)) return 0;   /* the paused VBLANK */
    }
    uint64_t vram = 0, ram = 0;
    if (!guest_hash(T1, ZBUF + STRIDE * PSP_H * 2u - T1, &vram) || !guest_hash(LISTS, RAM_END - LISTS, &ram)) {
        fprintf(stderr, "vram view identity: FAIL guest memory could not be read\n");
        return 0;
    }
    line("lists %" PRIu64 " %016" PRIx64, FRAMES * 2u, codes);
    line("vram %" PRIu64 " %016" PRIx64, ZBUF + STRIDE * PSP_H * 2u - T1, vram);
    line("ram %" PRIu64 " %016" PRIx64, RAM_END - LISTS, ram);
    line("ge %" PRIu64 " %016" PRIx64, sizeof(GeState), fnv(FNV0, ge_state_ptr(), sizeof(GeState)));
    return 1;
}

static int write_ref(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) { fprintf(stderr, "vram view identity: FAIL cannot write %s\n", path); return 0; }
    for (unsigned i = 0; i < s_nlines; i++) fprintf(fp, "%s\n", s_lines[i]);
    return fclose(fp) == 0;
}

static int check_ref(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) { fprintf(stderr, "vram view identity: FAIL cannot read %s\n", path); return 0; }
    char buf[128];
    unsigned i = 0;
    int ok = 1;
    while (fgets(buf, sizeof(buf), fp)) {
        buf[strcspn(buf, "\r\n")] = 0;
        if (i >= s_nlines || strcmp(buf, s_lines[i]) != 0) {
            fprintf(stderr, "vram view identity: FAIL want \"%s\", got \"%s\"\n", buf,
                    i < s_nlines ? s_lines[i] : "(nothing)");
            ok = 0;
        }
        i++;
    }
    fclose(fp);
    if (i != s_nlines) {
        fprintf(stderr, "vram view identity: FAIL %u reference lines, %u in this run\n", i, s_nlines);
        ok = 0;
    }
    return ok;
}

static int set_env(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

int main(int argc, char **argv) {
    const char *write_path = NULL, *check_path = NULL;
    int view_at = -1;
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) goto usage;
        if (strcmp(argv[i], "--write") == 0) write_path = argv[++i];
        else if (strcmp(argv[i], "--check") == 0) check_path = argv[++i];
        else if (strcmp(argv[i], "--view-at") == 0) view_at = atoi(argv[++i]);
        else if (strcmp(argv[i], "--scale") == 0) { if (!set_env("SR_GPU_SCALE", argv[++i])) return 2; }
        else goto usage;
    }
    if (!write_path == !check_path || (write_path && view_at >= 0) || view_at >= (int)FRAMES ||
        view_at < -1)
        goto usage;

    char capture_path[1024];
    if (snprintf(capture_path, sizeof(capture_path), "%s.frame.ppm",
                 write_path ? write_path : check_path) >= (int)sizeof(capture_path))
        goto usage;
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) return 2;
    g_mem = arena + 0x08000000u;
    sr_perf_init();
    if (!sdl3vk_init("Nakagawa VRAM view identity selftest") || !gegpu_init()) {
        fprintf(stderr, "vram view identity: SKIP (Vulkan initialization unavailable)\n");
        sdl3vk_shutdown();
        free(arena);
        return 77;
    }
    int ok = run(view_at, capture_path);
    if (ok) ok = write_path ? write_ref(write_path) : check_ref(check_path);
    gegpu_shutdown();
    sdl3vk_shutdown();
    free(arena);
    if (!ok) return 1;
    if (view_at >= 0)
        printf("vram view identity: PASS same as the reference with the viewer at frame %d "
               "(%u frames, %u digest lines)\n", view_at, FRAMES, s_nlines);
    else
        printf("vram view identity: PASS %s (%u frames, %u digest lines)\n",
               write_path ? "reference written" : "same as the reference", FRAMES, s_nlines);
    return 0;
usage:
    fprintf(stderr, "usage: vram_view_identity_selftest (--write PATH | --check PATH [--view-at N]) "
                    "[--scale N]\n");
    return 2;
}
