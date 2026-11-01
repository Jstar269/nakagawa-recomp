// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * ge_gpu's self-tests (SR_GPU_COHERENCE_SELFTEST): the coherence, replay and corpus cases that
 * run ge_gpu.c's real paths against a headless Vulkan device.
 *
 * This is not a translation unit: ge_gpu.c includes it, inside its SR_GPU_COHERENCE_SELFTEST
 * guard and at the point where the code used to sit, because the tests reach the file-local
 * state (render targets, batches, readback bookkeeping) directly. Build it only through ge_gpu.c.
 */

typedef struct CoherenceCase {
    const char *name;
    uint32_t fmt, stride;
    uint32_t dirty_offset, dirty_bytes;
    int pending_batch;
    int pending_readback;
    int dirty_alias;
} CoherenceCase;

static void coherence_fill_raw(uint8_t *dst, uint32_t pixels, uint32_t bpp, uint32_t raw) {
    for (uint32_t i = 0; i < pixels; i++) memcpy(dst + i * bpp, &raw, bpp);
}

static void coherence_reset_targets(void) {
    if (s_nbatch) (void)submit_pending();
    s_cur = NULL;
    for (int i = 0; i < MAX_TGT; i++)
        if (s_tgts[i].used) target_destroy(&s_tgts[i]);
    s_tmpl_ok = 0;
}

static uint32_t coherence_g1(uint32_t fmt) {
    static const uint32_t value[4] = { 0x4a69u, 0xca69u, 0xa73cu, 0x50607080u };
    return value[fmt & 3];
}

static uint32_t coherence_g0(uint32_t fmt) {
    static const uint32_t value[4] = { 0x1357u, 0x9357u, 0x2468u, 0x10203040u };
    return value[fmt & 3];
}

static uint32_t coherence_gbatch(uint32_t fmt) {
    static const uint32_t value[4] = { 0x6e25u, 0xee25u, 0xbb55u, 0xaa55aa55u };
    return value[fmt & 3];
}

static int coherence_queue_g1_batch(uint32_t base, uint32_t stride, uint32_t fmt) {
    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = fmt;
    s_ge->zbp = 0x04080000u;
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535;
    s_ge->clear = 1;
    s_ge->clear_mode = 0x301;
    uint32_t g_batch = coherence_gbatch(fmt);
    uint32_t rgba = fb_unpack(g_batch, fmt);
    GeVtx p0 = { 10.0f, 10.0f, 1.0f, 1.0f, 0, 0, 1.0f,
                 rgba & 0xff, (rgba >> 8) & 0xff, (rgba >> 16) & 0xff, rgba >> 24 };
    GeVtx p1 = p0;
    p1.x = 20.0f;
    p1.y = 20.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_nbatch) return 0;
    return 1;
}

/* A CPU load after a completed GE list must materialize only the GPU-owned
 * surface it touches. This drives the production GeGpuHooks and recomp.h scalar
 * accessors; it does not call the explicit snapshot API. */
static int coherence_run_scalar_vram_read(uint32_t depth_base) {
    const uint32_t fba = 0x00100000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u;
    const uint32_t color_offset = (10u * stride + 10u) * 4u;
    const uint32_t depth_offset = (10u * stride + 10u) * 2u;
    /* The GE XORs bits 6 and 13 of the absolute address (PSP-measured absolute form). */
    const uint32_t depth_swizzled = (depth_base + depth_offset) ^ 0x2040u;
    const uint32_t depth_linear_alias = 0x04200000u +
                                        (depth_base - 0x04000000u) + depth_offset;
    coherence_reset_targets();
    g_sr_vram_read_pending = 0;
    for (int i = 0; i < MAX_DEP; i++) {
        if (s_deps[i].used) {
            s_deps[i].guest_dirty = 0;
            s_deps[i].cpu_dirty = 1;
        }
    }
    memset(SR_HOST(base), 0, stride * FB_H * 4u);
    memset(SR_HOST(depth_base), 0, 0x44000u + 0x2040u);

    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = 3u;
    s_ge->zbp = depth_base;
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535u;
    s_ge->ztest_enable = 1;
    s_ge->ztest = 1u;
    s_ge->zwrite_disable = 0;
    GeVtx p0 = { 10.0f, 10.0f, (float)0x1111u, 1.0f, 0.0f, 0.0f, 1.0f,
                 0x40, 0x50, 0x60, 0xff };
    GeVtx p1 = p0;
    p1.x = 12.0f;
    p1.y = 12.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_nbatch) {
        fprintf(stderr, "gpu coherence selftest: failed to queue scalar-read sprite\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_flush("listend");
    uint32_t before = 1u;
    memcpy(&before, SR_HOST(base + color_offset), sizeof before);
    if (before != 0u || !g_sr_vram_read_pending) {
        fprintf(stderr,
                "gpu coherence selftest: list end materialized color early or missed pending ownership\n");
        coherence_reset_targets();
        return 0;
    }

    uint64_t lazy_color_start = SDL_GetTicksNS();
    uint32_t color = MEM_R32(base + color_offset);
    uint64_t lazy_color_ns = SDL_GetTicksNS() - lazy_color_start;
    if (color != 0x00605040u) {
        fprintf(stderr,
                "gpu coherence selftest: scalar framebuffer load (depth base 0x%08x) returned 0x%08x, expected 0x00605040\n",
                depth_base, color);
        coherence_reset_targets();
        return 0;
    }
    uint64_t lazy_depth_start = SDL_GetTicksNS();
    uint16_t depth = MEM_R16(depth_swizzled);
    uint64_t lazy_depth_ns = SDL_GetTicksNS() - lazy_depth_start;
    uint16_t depth_linear = MEM_R16(depth_linear_alias);
    if (depth != 0x1111u || depth_linear != 0x1111u) {
        fprintf(stderr,
                "gpu coherence selftest: depth scalar loads returned base=%04x mirror=%04x, expected 1111\n",
                depth, depth_linear);
        coherence_reset_targets();
        return 0;
    }

    enum { ACCESSOR_BENCH_ITERATIONS = 500000u };
    volatile uint32_t accessor_sink = 0u;
    uint64_t check_start = SDL_GetTicksNS();
    for (uint32_t i = 0; i < ACCESSOR_BENCH_ITERATIONS; i++) {
        sr_guest_vram_read_prepare_bytes(0x08010000u + ((i & 1023u) * 4u), 4u);
        accessor_sink += i + 1u;
    }
    uint64_t check_ns = SDL_GetTicksNS() - check_start;
    uint64_t loop_start = SDL_GetTicksNS();
    for (uint32_t i = 0; i < ACCESSOR_BENCH_ITERATIONS; i++) accessor_sink += i + 1u;
    uint64_t loop_ns = SDL_GetTicksNS() - loop_start;
    uint64_t scalar_start = SDL_GetTicksNS();
    for (uint32_t i = 0; i < ACCESSOR_BENCH_ITERATIONS; i++)
        accessor_sink += MEM_R32(0x08010000u + ((i & 1023u) * 4u));
    uint64_t scalar_ns = SDL_GetTicksNS() - scalar_start;
    uint64_t raw_start = SDL_GetTicksNS();
    for (uint32_t i = 0; i < ACCESSOR_BENCH_ITERATIONS; i++) {
        uint32_t value;
        memcpy(&value, SR_HOST_LINEAR(0x08010000u + ((i & 1023u) * 4u)), sizeof value);
        accessor_sink += value;
    }
    uint64_t raw_ns = SDL_GetTicksNS() - raw_start;
    (void)accessor_sink;

    MEM_W16(depth_swizzled, 0xFFFFu);
    MEM_W32(base + color_offset, 0xFF112233u);
    s_ge->ztest = 4u;                    /* less: the CPU depth store must make this pass */
    s_ge->blend_enable = 1;
    s_ge->blend_mode = 3u | (10u << 4); /* source alpha zero, destination fixed B */
    s_ge->blend_fixb = 0x00FFFFFFu;
    GeVtx q0 = { 10.0f, 10.0f, (float)0x4444u, 1.0f, 0.0f, 0.0f, 1.0f,
                 0x01, 0x02, 0x03, 0xff };
    GeVtx q1 = q0;
    q1.x = 12.0f;
    q1.y = 12.0f;
    if (!hook_sprite(&q0, &q1, 0)) {
        fprintf(stderr, "gpu coherence selftest: CPU-written targets were not drawable\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t hybrid_submit_start = SDL_GetTicksNS();
    if (!submit_pending()) {
        fprintf(stderr, "gpu coherence selftest: CPU-written target batch did not submit\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t hybrid_submit_ns = SDL_GetTicksNS() - hybrid_submit_start;
    uint64_t hybrid_color_start = SDL_GetTicksNS();
    uint32_t color_after_cpu_write = MEM_R32(base + color_offset);
    uint64_t hybrid_color_ns = SDL_GetTicksNS() - hybrid_color_start;
    uint64_t hybrid_depth_start = SDL_GetTicksNS();
    uint16_t depth_after_cpu_write = MEM_R16(depth_swizzled);
    uint64_t hybrid_depth_ns = SDL_GetTicksNS() - hybrid_depth_start;
    if (color_after_cpu_write != 0xFF112233u || depth_after_cpu_write != 0x4444u) {
        fprintf(stderr,
                "gpu coherence selftest: post-store draw returned color=%08x depth=%04x, expected ff112233/4444\n",
                color_after_cpu_write, depth_after_cpu_write);
        coherence_reset_targets();
        return 0;
    }
    puts("gpu coherence selftest: PASS cpu_stores_consumed_by_next_gpu_draw");

    s_ge->ztest = 1u;
    s_ge->blend_enable = 0;
    GeVtx e0 = { 14.0f, 14.0f, (float)0x5555u, 1.0f, 0.0f, 0.0f, 1.0f,
                 0x70, 0x60, 0x50, 0xff };
    GeVtx e1 = e0;
    e1.x = 16.0f;
    e1.y = 16.0f;
    if (!hook_sprite(&e0, &e1, 0)) {
        fprintf(stderr, "gpu coherence selftest: could not queue eager-sync candidate draw\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t eager_submit_start = SDL_GetTicksNS();
    if (!submit_pending()) {
        fprintf(stderr, "gpu coherence selftest: eager-sync candidate did not submit\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t eager_submit_ns = SDL_GetTicksNS() - eager_submit_start;
    uint64_t eager_materialize_start = SDL_GetTicksNS();
    if (!target_readback(s_cur) || !depth_readback_to_guest(s_cur->dep)) {
        fprintf(stderr, "gpu coherence selftest: eager-sync candidate materialization failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t eager_materialize_ns = SDL_GetTicksNS() - eager_materialize_start;
    printf("gpu coherence selftest: VRAM timing ns lazy-first-touch color=%" PRIu64
           " depth=%" PRIu64 " hybrid-listend-submit=%" PRIu64
           " hybrid-first-touch color=%" PRIu64 " depth=%" PRIu64
           " eager-listend-submit=%" PRIu64 " eager-sync-materialize=%" PRIu64
           " pending-check-extra=%" PRIu64 "/%u calls scalar-load=%" PRIu64
           " raw-load=%" PRIu64 " scalar-extra=%" PRIu64 "\n",
           lazy_color_ns, lazy_depth_ns, hybrid_submit_ns,
           hybrid_color_ns, hybrid_depth_ns, eager_submit_ns,
           eager_materialize_ns,
           check_ns > loop_ns ? (check_ns - loop_ns) / ACCESSOR_BENCH_ITERATIONS : 0u,
           ACCESSOR_BENCH_ITERATIONS, scalar_ns / ACCESSOR_BENCH_ITERATIONS,
           raw_ns / ACCESSOR_BENCH_ITERATIONS,
           scalar_ns > raw_ns ? (scalar_ns - raw_ns) / ACCESSOR_BENCH_ITERATIONS : 0u);
    puts("gpu coherence selftest: PASS scalar_cpu_read color+depth after list-end");
    coherence_reset_targets();
    return 1;
}

/* Establish issue 145's split generations without duplicating coherence: production
 * target_upload() creates GPU G1, the harness restores stale guest G0, then a real CPU
 * byte write flows through sr_gpu_vram_dirty(), production reacquire, and readback. */
static int coherence_run_case(const CoherenceCase *tc) {
    const uint32_t fba = 0x00100000u;
    const uint32_t base = 0x04000000u | fba;
    uint32_t bpp = tc->fmt == 3 ? 4u : 2u;
    uint32_t pixels = tc->stride * FB_H;
    uint32_t target_bytes = pixels * bpp;
    if (!tc->dirty_bytes || tc->dirty_offset + tc->dirty_bytes > target_bytes) return 0;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    uint8_t *expected = (uint8_t *)malloc(target_bytes);
    uint8_t *payload = (uint8_t *)malloc(tc->dirty_bytes);
    if (!expected || !payload) { free(expected); free(payload); return 0; }
    uint32_t g1 = coherence_g1(tc->fmt);
    coherence_fill_raw(guest, pixels, bpp, g1);
    coherence_fill_raw(expected, pixels, bpp, g1);

    Target *t = target_color_acquire(fba, tc->stride, tc->fmt, 1);
    if (!t || !t->gpu_valid) {
        fprintf(stderr, "gpu coherence selftest [%s]: could not establish GPU G1\n", tc->name);
        free(expected); free(payload); return 0;
    }
    coherence_fill_raw(guest, pixels, bpp, coherence_g0(tc->fmt));
    t->clean_gen = 0;

    if (tc->pending_batch) {
        if (!coherence_queue_g1_batch(base, tc->stride, tc->fmt)) {
            fprintf(stderr, "gpu coherence selftest [%s]: could not queue production batch\n", tc->name);
            free(expected); free(payload); return 0;
        }
        uint32_t g_batch = coherence_gbatch(tc->fmt);
        for (uint32_t py = 10; py < 20; py++) {
            for (uint32_t px = 10; px < 20; px++) {
                uint32_t b_offset = (py * tc->stride + px) * bpp;
                memcpy(expected + b_offset, &g_batch, bpp);
            }
        }
    }

    ReadbackSlot *stale = NULL;
    if (tc->pending_readback) {
        if (!target_prepare_present(t)) {
            fprintf(stderr, "gpu coherence selftest [%s]: async readback setup failed\n", tc->name);
            free(expected); free(payload); return 0;
        }
        for (int i = 0; i < READBACK_FRAMES; i++)
            if (s_readback[i].pending && s_readback[i].image == t->img) {
                stale = &s_readback[i]; break;
            }
        if (!stale) {
            fprintf(stderr, "gpu coherence selftest [%s]: no pending async slot\n", tc->name);
            free(expected); free(payload); return 0;
        }
    }

    for (uint32_t i = 0; i < tc->dirty_bytes; i++) payload[i] = (uint8_t)(0xd1u + i * 37u);
    memcpy(guest + tc->dirty_offset, payload, tc->dirty_bytes);
    memcpy(expected + tc->dirty_offset, payload, tc->dirty_bytes);
    uint32_t dirty_addr = base + tc->dirty_offset;
    if (tc->dirty_alias) dirty_addr += 0x40000000u;
    sr_gpu_vram_dirty(dirty_addr, tc->dirty_bytes);

    if (!t->cpu_n) {
        fprintf(stderr, "gpu coherence selftest [%s]: dirty hook did not hold the write\n", tc->name);
        free(expected); free(payload); return 0;
    }
    if (stale) {
        if (stale->commit || readback_finish(stale, 1, 1) < 0 ||
            memcmp(guest + tc->dirty_offset, payload, tc->dirty_bytes) != 0) {
            fprintf(stderr, "gpu coherence selftest [%s]: stale async completion overwrote G2\n",
                    tc->name);
            free(expected); free(payload); return 0;
        }
    }

    t = target_color_acquire(fba, tc->stride, tc->fmt, 1);
    if (!t || !target_readback(t)) {
        fprintf(stderr, "gpu coherence selftest [%s]: production reacquire/readback failed\n",
                tc->name);
        free(expected); free(payload); return 0;
    }

    size_t mismatch = 0;
    while (mismatch < target_bytes && guest[mismatch] == expected[mismatch]) mismatch++;
    if (mismatch != target_bytes) {
        fprintf(stderr,
                "gpu coherence selftest [%s]: byte %zu expected=%02x actual=%02x\n",
                tc->name, mismatch, expected[mismatch], guest[mismatch]);
        free(expected); free(payload); return 0;
    }
    printf("gpu coherence selftest: PASS %-22s fmt=%u stride=%u scale=%d\n",
           tc->name, tc->fmt, tc->stride, s_scale);
    free(expected);
    free(payload);
    return 1;
}

static int coherence_run_clean_case(const char *name, uint32_t fmt, uint32_t stride,
                                     uint32_t dirty_offset, uint32_t dirty_bytes) {
    const uint32_t fba = 0x00120000u;
    const uint32_t base = 0x04000000u | fba;
    uint32_t bpp = fmt == 3 ? 4u : 2u;
    uint32_t pixels = stride * FB_H;
    uint32_t target_bytes = pixels * bpp;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    uint8_t *expected = (uint8_t *)malloc(target_bytes);
    uint8_t *payload = (uint8_t *)malloc(dirty_bytes);
    if (!expected || !payload) { free(expected); free(payload); return 0; }

    uint32_t g1 = coherence_g1(fmt);
    coherence_fill_raw(guest, pixels, bpp, g1);
    coherence_fill_raw(expected, pixels, bpp, g1);

    Target *t = target_color_acquire(fba, stride, fmt, 1);
    if (!t || !t->gpu_valid || t->clean_gen != t->render_gen) {
        fprintf(stderr, "gpu coherence selftest [%s]: clean_gen initial setup failed\n", name);
        free(expected); free(payload); return 0;
    }

    for (uint32_t i = 0; i < dirty_bytes; i++) payload[i] = (uint8_t)(0xb5u + i * 19u);
    memcpy(guest + dirty_offset, payload, dirty_bytes);
    memcpy(expected + dirty_offset, payload, dirty_bytes);

    sr_gpu_vram_dirty(base + dirty_offset, dirty_bytes);

    if (t->clean_gen != t->render_gen) {
        fprintf(stderr, "gpu coherence selftest [%s]: clean_gen did not advance to render_gen\n", name);
        free(expected); free(payload); return 0;
    }

    uint64_t pre_rb = s_cnt_readback;
    if (!target_readback(t)) {
        fprintf(stderr, "gpu coherence selftest [%s]: target_readback failed\n", name);
        free(expected); free(payload); return 0;
    }
    if (s_cnt_readback != pre_rb) {
        fprintf(stderr, "gpu coherence selftest [%s]: readback performed unnecessary copy for clean target\n", name);
        free(expected); free(payload); return 0;
    }

    size_t mismatch = 0;
    while (mismatch < target_bytes && guest[mismatch] == expected[mismatch]) mismatch++;
    if (mismatch != target_bytes) {
        fprintf(stderr, "gpu coherence selftest [%s]: guest VRAM mismatch at byte %zu\n", name, mismatch);
        free(expected); free(payload); return 0;
    }

    printf("gpu coherence selftest: PASS %-22s fmt=%u stride=%u scale=%d\n",
           name, fmt, stride, s_scale);
    free(expected); free(payload);
    return 1;
}

/* Many small CPU writes into a resident target between two GE uses are held and applied
 * together (target_note_cpu_write, target_sync_cpu_writes), not one image patch each. The GPU
 * holds G1 while guest VRAM holds G0, so a byte the image takes from the wrong side shows.
 * 1024 adjacent 4-byte stores fill two rows and merge into one span; 48 unaligned 2-byte
 * stores in other rows split pixels and outnumber the spans, so one group is applied early.
 * The readback must hold the written bytes, G1 everywhere else (the GPU side of every split
 * pixel included), with far fewer patches than writes. */
static int coherence_run_cpu_write_coalesce(void) {
    const char *name = "cpu-write-coalesce";
    const uint32_t fba = 0x00150000u, base = 0x04000000u | fba;
    const uint32_t fmt = 3u, stride = 512u, bpp = 4u, pixels = stride * FB_H;
    const uint32_t target_bytes = pixels * bpp;
    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    uint8_t *expected = (uint8_t *)malloc(target_bytes);
    if (!expected) return 0;
    coherence_fill_raw(guest, pixels, bpp, coherence_g1(fmt));
    Target *t = target_color_acquire(fba, stride, fmt, 1);
    if (!t || !t->gpu_valid) { free(expected); return 0; }
    memcpy(expected, guest, target_bytes);              /* the image: G1 */
    coherence_fill_raw(guest, pixels, bpp, coherence_g0(fmt));
    t->clean_gen = 0;                                   /* the image is newer than VRAM */

    const unsigned long long notes0 = s_replay_stats.cpu_write_notes;
    const unsigned long long syncs0 = s_replay_stats.cpu_write_syncs;
    const unsigned uploads0 = s_cnt_upload;
    for (uint32_t i = 0; i < 1024u; i++) {             /* rows 40 and 41, every pixel */
        const uint32_t off = (40u * stride) * bpp + i * 4u;
        for (uint32_t k = 0; k < 4u; k++) guest[off + k] = expected[off + k] = (uint8_t)(0x31u + i + k);
        sr_gpu_vram_dirty(base + off, 4u);
    }
    for (uint32_t i = 0; i < 48u; i++) {               /* one 2-byte store per row, mid-pixel */
        const uint32_t off = ((100u + i) * stride + 7u * i) * bpp + 1u;
        guest[off] = expected[off] = (uint8_t)(0xa0u + i);
        guest[off + 1u] = expected[off + 1u] = (uint8_t)(0x50u + i);
        sr_gpu_vram_dirty(base + off, 2u);
    }
    const unsigned long long notes = s_replay_stats.cpu_write_notes - notes0;
    if (!target_readback(t)) {
        fprintf(stderr, "gpu coherence selftest [%s]: readback failed\n", name);
        free(expected); return 0;
    }
    const unsigned long long syncs = s_replay_stats.cpu_write_syncs - syncs0;
    const unsigned uploads = s_cnt_upload - uploads0;
    size_t mismatch = 0;
    while (mismatch < target_bytes && guest[mismatch] == expected[mismatch]) mismatch++;
    if (mismatch != target_bytes) {
        fprintf(stderr, "gpu coherence selftest [%s]: byte %zu expected=%02x actual=%02x\n",
                name, mismatch, expected[mismatch], guest[mismatch]);
        free(expected); return 0;
    }
    if (notes != 1072u || syncs != 2u || uploads > 1u + 48u) {
        fprintf(stderr, "gpu coherence selftest [%s]: notes=%llu syncs=%llu uploads=%u\n",
                name, notes, syncs, uploads);
        free(expected); return 0;
    }
    printf("gpu coherence selftest: PASS %-22s writes=%llu groups=%llu patches=%u\n",
           name, notes, syncs, uploads);
    coherence_reset_targets();
    free(expected);
    return 1;
}

static int coherence_run_full_cover_case(void) {
    const char *name = "full-cover-stale";
    const uint32_t fba = 0x00130000u;
    const uint32_t base = 0x04000000u | fba;
    uint32_t fmt = 3, stride = 512, bpp = 4;
    uint32_t pixels = stride * FB_H;
    uint32_t target_bytes = pixels * bpp;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    uint8_t *expected = (uint8_t *)malloc(target_bytes);
    if (!expected) return 0;

    uint32_t g1 = coherence_g1(fmt);
    coherence_fill_raw(guest, pixels, bpp, g1);
    Target *t = target_color_acquire(fba, stride, fmt, 1);
    if (!t || !t->gpu_valid) { free(expected); return 0; }

    t->clean_gen = 0;

    for (uint32_t i = 0; i < target_bytes; i++) {
        guest[i] = (uint8_t)(0x77u + i * 13u);
        expected[i] = guest[i];
    }
    sr_gpu_vram_dirty(base, target_bytes);

    if (!target_sync_cpu_writes(t) || t->clean_gen != t->render_gen) {
        fprintf(stderr, "gpu coherence selftest [%s]: full cover write failed to advance clean_gen\n", name);
        free(expected); return 0;
    }

    if (!target_readback(t) || memcmp(guest, expected, target_bytes) != 0) {
        fprintf(stderr, "gpu coherence selftest [%s]: target readback verification failed\n", name);
        free(expected); return 0;
    }

    printf("gpu coherence selftest: PASS %-22s fmt=%u stride=%u scale=%d\n",
           name, fmt, stride, s_scale);
    free(expected);
    return 1;
}

static int coherence_run_overlap_case(void) {
    const uint32_t fba_a = 0x000f0000u, fba_b = 0x00110000u;
    const uint32_t base_a = 0x04000000u | fba_a, base_b = 0x04000000u | fba_b;
    const uint32_t stride = FB_W, bpp = 4, bytes = stride * FB_H * bpp;
    const uint32_t g1a = 0x31527394u, g1b = 0x426384a5u, g0 = 0x10203040u;
    const uint32_t dirty_addr = base_b + 123u;
    const uint8_t payload[7] = { 0xe1, 0x17, 0xc3, 0x49, 0xa5, 0x6b, 0x8d };
    coherence_reset_targets();
    uint8_t *ga = (uint8_t *)SR_HOST(base_a), *gb = (uint8_t *)SR_HOST(base_b);
    uint8_t *ea = (uint8_t *)malloc(bytes), *eb = (uint8_t *)malloc(bytes);
    if (!ea || !eb) { free(ea); free(eb); return 0; }

    coherence_fill_raw(ga, stride * FB_H, bpp, g1a);
    Target *a = target_color_acquire(fba_a, stride, 3, 1);
    coherence_fill_raw(gb, stride * FB_H, bpp, g1b);
    Target *b = target_color_acquire(fba_b, stride, 3, 1);
    if (!a || !b) { free(ea); free(eb); return 0; }
    coherence_fill_raw(ga, (bytes + (fba_b - fba_a)) / bpp, bpp, g0);
    coherence_fill_raw(ea, stride * FB_H, bpp, g1a);
    coherence_fill_raw(eb, stride * FB_H, bpp, g1b);
    a->clean_gen = b->clean_gen = 0;
    memcpy(SR_HOST(dirty_addr), payload, sizeof(payload));
    memcpy(ea + (dirty_addr - base_a), payload, sizeof(payload));
    memcpy(eb + (dirty_addr - base_b), payload, sizeof(payload));
    sr_gpu_vram_dirty(dirty_addr, sizeof(payload));

    int ok = a->gpu_valid && b->gpu_valid && target_readback(a) &&
             memcmp(SR_HOST(base_a), ea, bytes) == 0 && target_readback(b) &&
             memcmp(SR_HOST(base_b), eb, bytes) == 0;
    if (!ok) fprintf(stderr, "gpu coherence selftest [overlapping-targets]: preservation failed\n");
    else printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
                "overlapping-targets", stride, s_scale);
    free(ea); free(eb);
    return ok;
}

static int coherence_run_tflt_descriptor_capacity(void) {
    const uint32_t stride = FB_W;
    coherence_reset_targets();
    for (unsigned i = 0; i < MAX_TGT; i++) {
        uint32_t fba = 0x00020000u + i * 0x00010000u;
        Target *target = target_color_acquire(fba, stride, 3u, 1);
        if (!target) {
            fprintf(stderr,
                    "gpu coherence selftest [TFLT-descriptors]: target %u/%u exhausted the fixed sampler pool\n",
                    i + 1u, MAX_TGT);
            coherence_reset_targets();
            return 0;
        }
        for (unsigned filter = 0; filter < TEX_FILTER_VARIANTS; filter++) {
            if (!target->set[filter]) {
                fprintf(stderr,
                        "gpu coherence selftest [TFLT-descriptors]: target %u lacks sampler variant %u\n",
                        i, filter);
                coherence_reset_targets();
                return 0;
            }
        }
    }
    coherence_reset_targets();
    printf("gpu coherence selftest: PASS TFLT descriptor-capacity targets=%u variants=%u sets=%u\n",
           MAX_TGT, TEX_FILTER_VARIANTS, FIXED_FILTER_DESCRIPTOR_SETS);
    return 1;
}

typedef struct SpriteEndpoint {
    float x, y, u, v;
} SpriteEndpoint;

typedef struct SpriteExpected {
    const char *name;
    SpriteEndpoint p0, p1;
    int x, y, w, h;
    int u, v, du, dv;
} SpriteExpected;

typedef struct RawSpriteVtx {
    float u, v;
    float x, y, z;
} RawSpriteVtx;

/* Synthetic through-mode sprites.  The expected table is deliberately independent of
 * the interpolation code: it names the complete texel sequence from the screen-space
 * minimum corner. UV is linear between the vertex on each edge and every pixel samples
 * it at its centre (PSP-GE-SAMPLE-001), so a pixel shows texel floor(u at x + 0.5):
 * reverse-x-scaled's first pixel reads 8 - 0.5 * 2 = 7. */
static const SpriteExpected k_sprite_expected[] = {
    { "normal",               { 10.0f,  10.0f, 0.0f, 0.0f }, { 14.0f,  14.0f, 4.0f, 4.0f },
      10,  10, 4, 4, 0, 0,  1,  1 },
    { "reverse-x",            { 14.0f,  20.0f, 0.0f, 0.0f }, { 10.0f,  24.0f, 4.0f, 4.0f },
      10,  20, 4, 4, 3, 0, -1,  1 },
    { "reverse-y",            { 10.0f,  34.0f, 0.0f, 0.0f }, { 14.0f,  30.0f, 4.0f, 4.0f },
      10,  30, 4, 4, 0, 3,  1, -1 },
    { "reverse-xy",           { 14.0f,  44.0f, 0.0f, 0.0f }, { 10.0f,  40.0f, 4.0f, 4.0f },
      10,  40, 4, 4, 3, 3, -1, -1 },
    { "one-pixel",            { 20.0f,  50.0f, 2.0f, 2.0f }, { 21.0f,  51.0f, 3.0f, 3.0f },
      20,  50, 1, 1, 2, 2,  0,  0 },
    { "one-pixel-reverse-x",  { 21.0f,  52.0f, 2.0f, 2.0f }, { 20.0f,  53.0f, 3.0f, 3.0f },
      20,  52, 1, 1, 2, 2,  0,  0 },
    { "reverse-x-offset",     { 14.0f,  60.0f, 2.0f, 3.0f }, { 10.0f,  64.0f, 6.0f, 7.0f },
      10,  60, 4, 4, 5, 3, -1,  1 },
    { "reverse-x-scaled",     { 14.0f,  70.0f, 0.0f, 0.0f }, { 10.0f,  74.0f, 8.0f, 4.0f },
      10,  70, 4, 4, 7, 0, -2,  1 },
    { "reverse-x-subpixel",   { 14.75f, 80.75f, 0.0f, 0.0f }, { 10.75f, 84.75f, 4.0f, 4.0f },
      10,  80, 4, 4, 3, 0, -1,  1 },
    { "reverse-x-far-edge",   { 38.0f,  90.0f, 0.0f, 0.0f }, { 30.0f,  92.0f, 8.0f, 2.0f },
      30,  90, 8, 2, 7, 0, -1,  1 },
    { "one-high-reverse-y",   { 40.0f, 101.0f, 1.0f, 2.0f }, { 44.0f, 100.0f, 5.0f, 3.0f },
      40, 100, 4, 1, 1, 2,  1,  0 },
    { "one-pixel-reverse-xy", { 51.0f, 111.0f, 2.0f, 2.0f }, { 50.0f, 110.0f, 3.0f, 3.0f },
      50, 110, 1, 1, 2, 2,  0,  0 },
};

static uint32_t *s_sprite_gpu_pixels;
static size_t s_sprite_gpu_bytes;

static uint32_t coherence_sprite_texel(int u, int v) {
    uint32_t r = (uint32_t)(u + 1) * 25u;
    uint32_t g = (uint32_t)(v + 1) * 25u;
    /* Default GE alpha write state keeps the destination alpha byte at zero. */
    return 0x00640000u | (g << 8) | r;
}

static int coherence_verify_sprite_output(const char *path, const uint32_t *fb,
                                          uint32_t stride) {
    for (size_t i = 0; i < sizeof(k_sprite_expected) / sizeof(k_sprite_expected[0]); i++) {
        const SpriteExpected *tc = &k_sprite_expected[i];
        for (int dy = 0; dy < tc->h; dy++) {
            for (int dx = 0; dx < tc->w; dx++) {
                int u = tc->u + dx * tc->du;
                int v = tc->v + dy * tc->dv;
                uint32_t want = coherence_sprite_texel(u, v);
                uint32_t got = fb[(tc->y + dy) * stride + tc->x + dx];
                if (got != want) {
                    fprintf(stderr,
                            "gpu coherence selftest [sprite-%s/%s]: pixel=(%d,%d) "
                            "texel=(%d,%d) expected=%08x actual=%08x\n",
                            path, tc->name, tc->x + dx, tc->y + dy, u, v, want, got);
                    return 0;
                }
            }
        }
        /* Prove the rectangle is endpoint-exclusive on all four sides. */
        for (int dx = 0; dx < tc->w; dx++) {
            if (fb[(tc->y - 1) * stride + tc->x + dx] != 0 ||
                fb[(tc->y + tc->h) * stride + tc->x + dx] != 0) {
                fprintf(stderr, "gpu coherence selftest [sprite-%s/%s]: y edge leaked\n",
                        path, tc->name);
                return 0;
            }
        }
        for (int dy = 0; dy < tc->h; dy++) {
            if (fb[(tc->y + dy) * stride + tc->x - 1] != 0 ||
                fb[(tc->y + dy) * stride + tc->x + tc->w] != 0) {
                fprintf(stderr, "gpu coherence selftest [sprite-%s/%s]: x edge leaked\n",
                        path, tc->name);
                return 0;
            }
        }
    }
    return 1;
}

static uint32_t coherence_prepare_sprite_fixture(uint32_t base, uint32_t tex_base,
                                                 uint32_t stride) {
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);
    for (int v = 0; v < 8; v++) {
        for (int u = 0; u < 8; u++) {
            uint32_t r = (uint32_t)(u + 1) * 25u;
            uint32_t g = (uint32_t)(v + 1) * 25u;
            tex[v * 8 + u] = 0xff640000u | (g << 8) | r;
        }
    }
    sr_gpu_vram_dirty(tex_base, 8u * 8u * 4u);

    const uint32_t vaddr = 0x00101000u;
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(vaddr);
    uint32_t sprite_count = (uint32_t)(sizeof(k_sprite_expected) /
                                       sizeof(k_sprite_expected[0]));
    for (uint32_t i = 0; i < sprite_count; i++) {
        const SpriteExpected *tc = &k_sprite_expected[i];
        vtx[i * 2] = (RawSpriteVtx){ tc->p0.u, tc->p0.v, tc->p0.x, tc->p0.y, 0.0f };
        vtx[i * 2 + 1] =
            (RawSpriteVtx){ tc->p1.u, tc->p1.v, tc->p1.x, tc->p1.y, 0.0f };
    }

    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x9C, base & 0x00FFFFFFu);
    DL_EMIT(0x9D, stride | ((base & 0xFF000000u) >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, tex_base & 0x00FFFFFFu);
    DL_EMIT(0xA8, 8 | ((tex_base & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (3 << 8) | 3);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC6, 0);
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3 | (1 << 8));
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3 << 0) | (3 << 7) | (1 << 23));
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0x04, (6 << 16) | (sprite_count * 2u));
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return list_addr;
}

static int coherence_run_sprite_semantics(void) {
    const uint32_t fba = 0x00160000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);

    uint64_t spr_before = s_cnt_spr;
    size_t sprite_count = sizeof(k_sprite_expected) / sizeof(k_sprite_expected[0]);
    uint32_t list_addr = coherence_prepare_sprite_fixture(base, tex_base, stride);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list_addr, 0);

    if (s_cnt_spr != spr_before + sprite_count) {
        fprintf(stderr, "gpu coherence selftest [sprite-semantics]: hook_sprite call count mismatch\n");
        return 0;
    }

    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride, .width = 480, .height = 272 };
    if (!gegpu_sync_guest_fb(&desc)) {
        fprintf(stderr, "gpu coherence selftest [sprite-semantics]: gegpu_sync_guest_fb failed\n");
        return 0;
    }

    uint32_t *fb = (uint32_t *)guest;
    if (!coherence_verify_sprite_output("gpu", fb, stride)) return 0;

    s_sprite_gpu_bytes = stride * FB_H * sizeof(*fb);
    free(s_sprite_gpu_pixels);
    s_sprite_gpu_pixels = (uint32_t *)malloc(s_sprite_gpu_bytes);
    if (!s_sprite_gpu_pixels) {
        s_sprite_gpu_bytes = 0;
        return 0;
    }
    memcpy(s_sprite_gpu_pixels, fb, s_sprite_gpu_bytes);

    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
           "sprite-exact-gpu", stride, s_scale);
    return 1;
}

static int coherence_run_software_sprite_semantics(void) {
    const uint32_t fba = 0x00160000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;

    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);

    uint32_t list_addr = coherence_prepare_sprite_fixture(base, tex_base, stride);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list_addr, 0);
    ge_set_gpu_hooks(&k_hooks);

    uint32_t *fb = (uint32_t *)guest;
    if (!coherence_verify_sprite_output("software", fb, stride)) {
        free(s_sprite_gpu_pixels);
        s_sprite_gpu_pixels = NULL;
        s_sprite_gpu_bytes = 0;
        return 0;
    }
    size_t bytes = stride * FB_H * sizeof(*fb);
    if (!s_sprite_gpu_pixels || s_sprite_gpu_bytes != bytes) {
        fprintf(stderr, "gpu coherence selftest [sprite-parity]: missing GPU reference\n");
        free(s_sprite_gpu_pixels);
        s_sprite_gpu_pixels = NULL;
        s_sprite_gpu_bytes = 0;
        return 0;
    }
    size_t mismatch = 0;
    while (mismatch < bytes && ((const uint8_t *)s_sprite_gpu_pixels)[mismatch] == guest[mismatch])
        mismatch++;
    if (mismatch != bytes) {
        fprintf(stderr,
                "gpu coherence selftest [sprite-parity]: byte=%zu gpu=%02x software=%02x\n",
                mismatch, ((const uint8_t *)s_sprite_gpu_pixels)[mismatch], guest[mismatch]);
        free(s_sprite_gpu_pixels);
        s_sprite_gpu_pixels = NULL;
        s_sprite_gpu_bytes = 0;
        return 0;
    }
    free(s_sprite_gpu_pixels);
    s_sprite_gpu_pixels = NULL;
    s_sprite_gpu_bytes = 0;

    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
           "sprite-exact-software", stride, s_scale);
    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
           "sprite-sw-gpu-parity", stride, s_scale);
    return 1;
}

/* ---- linearly filtered sprite: nothing from outside the sampled region may bleed ----
 *
 * The sprite-exact fixtures above run with GE_TEXFILTER=0 (NEAREST), so they pin the
 * endpoint-exclusive rectangle but say nothing about the filter window. This fixture
 * samples texel rows 1..4 of a texture whose row 0 is pure white, and demands that a
 * linearly filtered 1:1 sprite reproduce rows 1..4 exactly: an integer texel coordinate
 * names that texel's centre, so the filter weight is 0 and the outside row cannot
 * contribute. A half-texel window error shows up here as a white 50/50 wash on the
 * sprite's first row and column -- the one-pixel line this fixture exists to catch.
 * Both backends run the same list and the results must also agree byte for byte. */
#define COH_LIN_RECT_X 100
#define COH_LIN_RECT_Y 100
#define COH_LIN_RECT_W 4
#define COH_LIN_RECT_H 4
#define COH_LIN_TEX_U0 1u
#define COH_LIN_TEX_V0 1u
#define COH_LIN_OUTSIDE_ROW 0xFFFFFFFFu   /* pure white: no sampled texel uses 255 */

static uint32_t coherence_linear_texel(unsigned u, unsigned v) {
    if (v == 0u) return COH_LIN_OUTSIDE_ROW;          /* outside the sampled region */
    if (v > 4u) return 0xFF000000u;                    /* below it */
    return 0xFF000000u | (0x40u << 16) | ((v * 40u) << 8) | (u * 40u);
}

/* The default GE alpha-write state leaves the destination alpha byte at zero (see
 * coherence_sprite_texel), so the RGB triple is what this fixture compares. */
#define COH_LIN_RGB 0x00FFFFFFu

static uint32_t coherence_prepare_linear_sprite_fixture(uint32_t tex_base) {
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);
    for (unsigned v = 0; v < 8u; v++)
        for (unsigned u = 0; u < 8u; u++)
            tex[v * 8u + u] = coherence_linear_texel(u, v);
    sr_gpu_vram_dirty(tex_base, 8u * 8u * 4u);

    const uint32_t vaddr = 0x00101000u;
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(vaddr);
    vtx[0] = (RawSpriteVtx){ (float)COH_LIN_TEX_U0, (float)COH_LIN_TEX_V0,
                             (float)COH_LIN_RECT_X, (float)COH_LIN_RECT_Y, 0.0f };
    vtx[1] = (RawSpriteVtx){ (float)(COH_LIN_TEX_U0 + COH_LIN_RECT_W),
                             (float)(COH_LIN_TEX_V0 + COH_LIN_RECT_H),
                             (float)(COH_LIN_RECT_X + COH_LIN_RECT_W),
                             (float)(COH_LIN_RECT_Y + COH_LIN_RECT_H), 0.0f };

    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x9C, 0x00160000u);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, tex_base & 0x00FFFFFFu);
    DL_EMIT(0xA8, 8 | ((tex_base & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (3 << 8) | 3);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC6, 1);   /* GE_TEXFILTER = LINEAR: the filter window is what is under test */
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3 | (1 << 8));
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3 << 0) | (3 << 7) | (1 << 23));
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0x04, (6 << 16) | 2u);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return list_addr;
}

static int coherence_verify_linear_sprite(const char *path, const uint32_t *fb,
                                          uint32_t stride) {
    for (int dy = 0; dy < COH_LIN_RECT_H; dy++) {
        for (int dx = 0; dx < COH_LIN_RECT_W; dx++) {
            uint32_t want = coherence_linear_texel(COH_LIN_TEX_U0 + (unsigned)dx,
                                                   COH_LIN_TEX_V0 + (unsigned)dy) & COH_LIN_RGB;
            uint32_t got = fb[(COH_LIN_RECT_Y + dy) * stride + COH_LIN_RECT_X + dx] & COH_LIN_RGB;
            if (got != want) {
                fprintf(stderr,
                        "gpu coherence selftest [linear-%s]: pixel=(%d,%d) texel=(%u,%u) "
                        "expected=%06x actual=%06x%s\n",
                        path, COH_LIN_RECT_X + dx, COH_LIN_RECT_Y + dy,
                        COH_LIN_TEX_U0 + (unsigned)dx, COH_LIN_TEX_V0 + (unsigned)dy,
                        want, got,
                        (got & COH_LIN_RGB) == (COH_LIN_OUTSIDE_ROW & COH_LIN_RGB)
                            ? " -- the texel row OUTSIDE the sampled region bled in" : "");
                return 0;
            }
        }
    }
    /* The rectangle stays endpoint-exclusive: a filter that reached outward would also
     * paint the neighbouring rows. */
    for (int dx = 0; dx < COH_LIN_RECT_W; dx++) {
        if (fb[(COH_LIN_RECT_Y - 1) * stride + COH_LIN_RECT_X + dx] != 0 ||
            fb[(COH_LIN_RECT_Y + COH_LIN_RECT_H) * stride + COH_LIN_RECT_X + dx] != 0) {
            fprintf(stderr, "gpu coherence selftest [linear-%s]: y edge leaked\n", path);
            return 0;
        }
    }
    return 1;
}

static int coherence_run_linear_sprite(void) {
    const uint32_t base = 0x04000000u | 0x00160000u;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);

    uint32_t list_addr = coherence_prepare_linear_sprite_fixture(tex_base);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list_addr, 0);

    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride, .width = 480, .height = 272 };
    if (!gegpu_sync_guest_fb(&desc)) {
        fprintf(stderr, "gpu coherence selftest [linear-gpu]: gegpu_sync_guest_fb failed\n");
        return 0;
    }
    uint32_t *fb = (uint32_t *)guest;
    if (!coherence_verify_linear_sprite("gpu", fb, stride)) return 0;

    size_t bytes = stride * FB_H * sizeof(*fb);
    uint8_t *reference = (uint8_t *)malloc(bytes);
    if (!reference) {
        fprintf(stderr, "gpu coherence selftest [linear-gpu]: no reference buffer\n");
        return 0;
    }
    memcpy(reference, guest, bytes);

    memset(guest, 0, stride * FB_H * 4u);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list_addr, 0);
    ge_set_gpu_hooks(&k_hooks);
    if (!coherence_verify_linear_sprite("software", fb, stride)) { free(reference); return 0; }
    size_t mismatch = 0;
    while (mismatch < bytes && reference[mismatch] == guest[mismatch]) mismatch++;
    if (mismatch != bytes) {
        fprintf(stderr, "gpu coherence selftest [linear-parity]: byte=%zu gpu=%02x software=%02x\n",
                mismatch, reference[mismatch], guest[mismatch]);
        free(reference);
        return 0;
    }
    free(reference);

    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
           "linear-no-region-bleed", stride, s_scale);
    return 1;
}

/* Distinct GE minification/magnification filters must survive the production sampler path.
 * The two cases deliberately render at 2x magnification and 2x minification; the selected
 * filter is NEAREST in each case, so an alternating texture must contain no blended colors. */
#define COH_TFLT_RECT_X 120
#define COH_TFLT_RECT_Y 120

static uint32_t coherence_tflt_texel(unsigned u, unsigned v) {
    (void)v;
    return (u & 1u) ? 0xFFFFFFFFu : 0xFF000000u;
}

static uint32_t coherence_prepare_tflt_fixture(uint32_t tex_base, uint32_t tflt, int primitive_type,
                                               int rect_w, int rect_h,
                                               float u0, float v0, float u_span, float v_span) {
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);
    for (unsigned v = 0; v < 8u; v++)
        for (unsigned u = 0; u < 8u; u++)
            tex[v * 8u + u] = coherence_tflt_texel(u, v);
    sr_gpu_vram_dirty(tex_base, 8u * 8u * 4u);

    const uint32_t vaddr = 0x00101000u;
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(vaddr);
    vtx[0] = (RawSpriteVtx){ u0, v0, (float)COH_TFLT_RECT_X,
                             (float)COH_TFLT_RECT_Y, 0.0f };
    vtx[1] = (RawSpriteVtx){ u0 + u_span, v0 + v_span,
                             (float)(COH_TFLT_RECT_X + rect_w),
                             (float)(COH_TFLT_RECT_Y + rect_h), 0.0f };
    if (primitive_type == 3) {
        vtx[2] = (RawSpriteVtx){ u0, v0 + v_span,
                                 (float)COH_TFLT_RECT_X,
                                 (float)(COH_TFLT_RECT_Y + rect_h), 0.0f };
    }

    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x9C, 0x00160000u);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, tex_base & 0x00FFFFFFu);
    DL_EMIT(0xA8, 8u | ((tex_base & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (3u << 8) | 3u);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC6, tflt);
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3u | (1u << 8));
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0x04, ((uint32_t)primitive_type << 16) | (primitive_type == 3 ? 3u : 2u));
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return list_addr;
}

static int coherence_verify_tflt(const char *path, const uint32_t *fb, uint32_t stride,
                                 int rect_w, int rect_h, int expect_linear) {
    int found_blended = 0;
    for (int y = 0; y < rect_h; y++) {
        for (int x = 0; x < rect_w; x++) {
            uint32_t rgb = fb[(COH_TFLT_RECT_Y + y) * stride + COH_TFLT_RECT_X + x] & 0x00FFFFFFu;
            if (rgb != 0x000000u && rgb != 0xFFFFFFu) found_blended = 1;
            if (!expect_linear && rgb != 0x000000u && rgb != 0xFFFFFFu) {
                fprintf(stderr,
                        "gpu coherence selftest [TFLT-%s]: pixel=(%d,%d) unexpectedly blended: %06x\n",
                        path, COH_TFLT_RECT_X + x, COH_TFLT_RECT_Y + y, rgb);
                return 0;
            }
        }
    }
    if (expect_linear && !found_blended) {
        fprintf(stderr, "gpu coherence selftest [TFLT-%s]: expected linear filtering, found only texels\n",
                path);
        return 0;
    }
    return 1;
}

static int coherence_run_tflt_case(const char *name, int primitive_type, uint32_t tflt,
                                   int rect_w, int rect_h,
                                   float u0, float v0, float u_span, float v_span,
                                   int expect_linear) {
    const uint32_t base = 0x04000000u | 0x00160000u;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;
    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    uint32_t list_addr = coherence_prepare_tflt_fixture(tex_base, tflt, primitive_type,
                                                        rect_w, rect_h,
                                                        u0, v0, u_span, v_span);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list_addr, 0);
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    if (!gegpu_sync_guest_fb(&desc)) {
        fprintf(stderr, "gpu coherence selftest [TFLT-%s]: GPU readback failed\n", name);
        coherence_reset_targets();
        return 0;
    }
    uint32_t *fb = (uint32_t *)guest;
    if (!coherence_verify_tflt("gpu", fb, stride, rect_w, rect_h, expect_linear)) {
        coherence_reset_targets();
        return 0;
    }
    size_t bytes = stride * FB_H * sizeof(*fb);
    uint8_t *reference = (uint8_t *)malloc(bytes);
    if (!reference) {
        fprintf(stderr, "gpu coherence selftest [TFLT-%s]: no reference buffer\n", name);
        coherence_reset_targets();
        return 0;
    }
    memcpy(reference, guest, bytes);
    memset(guest, 0, stride * FB_H * 4u);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list_addr, 0);
    ge_set_gpu_hooks(&k_hooks);
    if (!coherence_verify_tflt("software", fb, stride, rect_w, rect_h, expect_linear)) {
        free(reference);
        coherence_reset_targets();
        return 0;
    }
    size_t pixel_count = bytes / sizeof(*fb);
    size_t mismatch = pixel_count;
    const uint32_t *gpu = (const uint32_t *)reference;
    for (size_t pixel = 0; pixel < pixel_count; pixel++) {
        int differs = 0;
        if (!expect_linear) {
            differs = gpu[pixel] != fb[pixel];
        } else {
            /* Vulkan filtering and the integer software lerp can round by one U8 step. */
            for (unsigned shift = 0; shift < 32; shift += 8) {
                int gpu_channel = (int)((gpu[pixel] >> shift) & 0xFFu);
                int sw_channel = (int)((fb[pixel] >> shift) & 0xFFu);
                int delta = gpu_channel - sw_channel;
                if (delta < -1 || delta > 1) differs = 1;
            }
        }
        if (differs) { mismatch = pixel; break; }
    }
    if (mismatch != pixel_count) {
        fprintf(stderr,
                "gpu coherence selftest [TFLT-%s]: GPU/software mismatch at byte=%zu pixel=(%zu,%zu) gpu=%08x software=%08x\n",
                name, mismatch * sizeof(*fb), mismatch % stride, mismatch / stride,
                gpu[mismatch], fb[mismatch]);
        free(reference);
        coherence_reset_targets();
        return 0;
    }
    free(reference);
    coherence_reset_targets();
    printf("gpu coherence selftest: PASS TFLT %-22s mode=0x%03x\n", name, tflt);
    return 1;
}

static int coherence_run_tflt_software_line(const char *name, uint32_t tflt,
                                            int rect_w, int rect_h,
                                            float u0, float u_span, int expect_linear) {
    const uint32_t base = 0x04000000u | 0x00160000u;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;
    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    uint32_t list_addr = coherence_prepare_tflt_fixture(tex_base, tflt, 1,
                                                        rect_w, rect_h,
                                                        u0, 0.0f, u_span, 0.0f);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list_addr, 0);
    int ok = coherence_verify_tflt("software-line", (const uint32_t *)guest, stride,
                                   rect_w, rect_h, expect_linear);
    ge_set_gpu_hooks(&k_hooks);
    coherence_reset_targets();
    if (!ok) return 0;
    printf("gpu coherence selftest: PASS TFLT %-22s mode=0x%03x\n", name, tflt);
    return 1;
}

static int coherence_run_tflt_filters(void) {
    int ok = 1;
    /* min=linear, mag=nearest; this sprite is magnified, so nearest must win. */
    if (!coherence_run_tflt_case("mag-nearest", 6, 0x001u, 16, 16,
                                 0.0f, 0.0f, 8.0f, 8.0f, 0))
        ok = 0;
    /* min=nearest, mag=linear; this sprite is minified, so nearest must win. */
    if (!coherence_run_tflt_case("min-nearest", 6, 0x100u, 4, 4,
                                 0.25f, 0.25f, 8.0f, 8.0f, 0))
        ok = 0;
    /* The complementary cases prove linear filtering still wins when its field is selected. */
    if (!coherence_run_tflt_case("mag-linear", 6, 0x100u, 16, 16,
                                 0.0f, 0.0f, 8.0f, 8.0f, 1))
        ok = 0;
    if (!coherence_run_tflt_case("min-linear", 6, 0x001u, 4, 4,
                                 0.25f, 0.25f, 8.0f, 8.0f, 1))
        ok = 0;
    /* The triangle path uses the per-pixel texture planes rather than sprite steps. */
    if (!coherence_run_tflt_case("triangle-mag-nearest", 3, 0x001u, 16, 16,
                                 0.0f, 0.0f, 8.0f, 8.0f, 0))
        ok = 0;
    if (!coherence_run_tflt_case("triangle-min-nearest", 3, 0x100u, 4, 4,
                                 0.25f, 0.25f, 8.0f, 8.0f, 0))
        ok = 0;
    if (!coherence_run_tflt_case("triangle-mag-linear", 3, 0x100u, 16, 16,
                                 0.0f, 0.0f, 8.0f, 8.0f, 1))
        ok = 0;
    if (!coherence_run_tflt_case("triangle-min-linear", 3, 0x001u, 4, 4,
                                 0.25f, 0.25f, 8.0f, 8.0f, 1))
        ok = 0;
    /* Lines are software-rasterized for this fixture; exercise their DDA footprint too. */
    if (!coherence_run_tflt_software_line("line-mag-nearest", 0x001u, 16, 2,
                                          0.0f, 8.0f, 0))
        ok = 0;
    if (!coherence_run_tflt_software_line("line-min-nearest", 0x100u, 4, 2,
                                          0.25f, 8.0f, 0))
        ok = 0;
    if (!coherence_run_tflt_software_line("line-mag-linear", 0x100u, 16, 2,
                                          0.0f, 8.0f, 1))
        ok = 0;
    if (!coherence_run_tflt_software_line("line-min-linear", 0x001u, 4, 2,
                                          0.25f, 8.0f, 1))
        ok = 0;
    return ok;
}

/* ---- texture mip levels (#1373) ----------------------------------------------------------
 *
 * The Vulkan backend samples TEXMODE/TEXLEVEL mip levels from one Vulkan mip chain per cached
 * texture and must choose what ge.c tex_detail16 and ge_tex_level_pick choose. Each case draws one through-mode sprite
 * with the Vulkan backend and then with the software GE, and compares the framebuffers:
 * exactly for nearest-level cases, and within the one-U8-step tolerance of the TFLT linear
 * cases for a blend of two levels. Every texel names its level and position, so a wrong
 * level, a level decoded without its own buffer width, or a level coordinate one texel off
 * changes a pixel. The UV start of 0.25 texel keeps every sample off a texel edge at every
 * level, and the footprints (1/2, 2 and 4 texels per pixel) give exact levels of detail.
 * Software/Vulkan agreement is not a PSP measurement; the level rules both paths share are
 * PSP-GE-MIP-001's (hle_thread_selftest.c test_ge_mip_cells_match_psp checks ge.c against
 * the PSP). The automatic and slope cases take the software rasterizer, so they check that
 * fallback. */
#define COH_MIP_TEX   0x04040000u          /* level L at COH_MIP_TEX + L * 0x1000 */
#define COH_MIP_ALT1  0x04045000u          /* a second level 1 (4x4) for the cache-key case */
#define COH_MIP_LIST  0x00100800u
#define COH_MIP_RESET 0x00100C00u
#define COH_MIP_VERTS 0x00101800u
#define COH_MIP_X     120
#define COH_MIP_Y     120

static uint32_t coherence_mip_texel(unsigned level, unsigned u, unsigned v) {
    uint32_t r = level * 64u + u * 8u + v, g = 255u - r, b = 40u * level + 30u;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* The second level 1 differs from the first in every colour bit. */
static uint32_t coherence_mip_alt_texel(unsigned u, unsigned v) {
    return coherence_mip_texel(1u, u, v) ^ 0x00FFFFFFu;
}

/* Levels 0-3 of an 8x8 8888 texture. Level 1 keeps its 4 texels per row in an 8-wide buffer
 * with magenta beyond them, which only a decode that ignores the level's own buffer width
 * reaches; the second level 1 is a plain 4x4. Levels 2 and 3 use buffer width 4: a linear
 * texture's rows are whole 16-byte lines (ge_tex_row_bytes). */
static unsigned coherence_mip_bufw(unsigned level) {
    return level <= 1u ? 8u : 4u;
}

static void coherence_mip_fill(void) {
    for (unsigned level = 0; level < 4u; level++) {
        unsigned size = 8u >> level, bufw = coherence_mip_bufw(level);
        uint32_t addr = COH_MIP_TEX + level * 0x1000u;
        uint32_t *tex = (uint32_t *)SR_HOST(addr);
        for (unsigned v = 0; v < size; v++)
            for (unsigned u = 0; u < bufw; u++)
                tex[v * bufw + u] = u < size ? coherence_mip_texel(level, u, v) : 0xFFFF00FFu;
        sr_gpu_vram_dirty(addr, size * bufw * 4u);
    }
    uint32_t *alt = (uint32_t *)SR_HOST(COH_MIP_ALT1);
    for (unsigned v = 0; v < 4u; v++)
        for (unsigned u = 0; u < 4u; u++) alt[v * 4u + u] = coherence_mip_alt_texel(u, v);
    sr_gpu_vram_dirty(COH_MIP_ALT1, 4u * 4u * 4u);
}

typedef struct CohMipCase {
    const char *name;
    uint32_t tflt;              /* TEXFILTER (magnification field in bits 8+) */
    uint32_t max_level;         /* TEXMODE highest level */
    uint32_t mode, bias16;      /* TEXLEVEL mode, and its bias in 1/16 (8-bit two's complement) */
    int rect;                   /* sprite side in pixels; the UV span is 8 texels */
    int tolerance;              /* per-channel U8 tolerance: 1 for a blend of two levels */
    int level;                  /* level the first pixel samples, or -1 for a blend */
    int alt_level1;             /* level 1 is the second (plain 4x4) level 1 */
    uint32_t level1_log2;       /* level 1 TEXSIZE log2 on both axes; 0 = 2, the halving chain */
} CohMipCase;

/* The GE list of one case: framebuffer 0x04160000, levels 0-3, then one sprite when `draw`
 * is set (otherwise the state alone, for direct hook draws). */
static uint32_t coherence_mip_list(const CohMipCase *c, int draw) {
    if (draw) {
        RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(COH_MIP_VERTS);
        vtx[0] = (RawSpriteVtx){ 0.25f, 0.25f, (float)COH_MIP_X, (float)COH_MIP_Y, 0.0f };
        vtx[1] = (RawSpriteVtx){ 8.25f, 8.25f, (float)(COH_MIP_X + c->rect),
                                 (float)(COH_MIP_Y + c->rect), 0.0f };
    }
    uint32_t *dl = (uint32_t *)SR_HOST(COH_MIP_LIST);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, 0x00160000u);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x4D, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    for (uint32_t level = 0; level < 4u; level++) {
        uint32_t addr = COH_MIP_TEX + level * 0x1000u, bufw = coherence_mip_bufw(level);
        uint32_t log2 = 3u - level;
        if (level == 1u) {
            addr = c->alt_level1 ? COH_MIP_ALT1 : addr;
            bufw = c->alt_level1 ? 4u : 8u;
            log2 = c->level1_log2 ? c->level1_log2 : 2u;
        }
        DL_EMIT(0xA0 + level, addr & 0x00FFFFFFu);                        /* TEXADDRn */
        DL_EMIT(0xA8 + level, bufw | ((addr & 0xFF000000u) >> 8));         /* TEXBUFWIDTHn */
        DL_EMIT(0xB8 + level, (log2 << 8) | log2);                         /* TEXSIZEn */
    }
    DL_EMIT(0xC2, c->max_level << 16);                  /* TEXMODE: highest level, unswizzled */
    DL_EMIT(0xC8, c->mode | ((c->bias16 & 0xFFu) << 16));                  /* TEXLEVEL */
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC6, c->tflt);
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3u | (1u << 8));                      /* replace, RGBA */
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));  /* float UV and position, through */
    if (draw) {
        DL_EMIT(0x01, COH_MIP_VERTS & 0x00FFFFFFu);
        DL_EMIT(0x04, (6u << 16) | 2u);                 /* one sprite */
    }
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return COH_MIP_LIST;
}

/* Texture sample points measured on a PSP-3000 (PSP-GE-SAMPLE-001, issue #282). A 32x32 8888
 * texture shows texel (u, v) as R = 8u, G = 8v, so a pixel's R and G name its texel and, under
 * bilinear filtering, its blend. Four through-mode cells from the record, each 8x8 pixels:
 *   A  triangle pair, bilinear, 1:1 at integer UV 8..16: every pixel is exactly its texel;
 *   B  the same with nearest filtering;
 *   C  triangle pair, nearest, UV 8.5..16.5: texel 9 + dx, 9 + dy;
 *   D  sprite, bilinear, 2x magnified (UV 8..12): the first pixel blends texel 7 at 4/16
 *      and texel 8 at 12/16 on each axis, the second texel 8 at 12/16 and texel 9 at 4/16.
 * Before the rule was measured, a 1:1 triangle blended half of the next texel under bilinear
 * filtering (a glyph's last row picked up the next glyph's first) and showed the next texel
 * under nearest. Both the Vulkan path and the software rasterizer are checked. */
#define COH_SAMPLE_TEX   0x04048000u
#define COH_SAMPLE_LIST  0x00102000u
#define COH_SAMPLE_VERTS 0x00102800u
static int coherence_sample_expect(int cell, int dx, int dy, uint32_t *want) {
    int r, g;
    if (cell == 0 || cell == 1) { r = 8 * (8 + dx); g = 8 * (8 + dy); }
    else if (cell == 2) { r = 8 * (9 + dx); g = 8 * (9 + dy); }
    else {
        static const int first_two[2] = { (56 * 4 + 64 * 12) / 16, (64 * 12 + 72 * 4) / 16 };
        if (dx > 1 || dy > 1) return 0;
        r = first_two[dx]; g = first_two[dy];
    }
    *want = 0xFF000000u | ((uint32_t)g << 8) | (uint32_t)r;
    return 1;
}

static int coherence_run_psp_sample_points(void) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u;
    const size_t bytes = (size_t)stride * FB_H * 4u;
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    uint32_t *tex = (uint32_t *)SR_HOST(COH_SAMPLE_TEX);
    for (unsigned v = 0; v < 32u; v++)
        for (unsigned u = 0; u < 32u; u++) tex[v * 32u + u] = 0xFF000000u | ((8u * v) << 8) | (8u * u);
    sr_gpu_vram_dirty(COH_SAMPLE_TEX, 32u * 32u * 4u);

    static const struct { int x, type; float u0, u1; uint32_t tflt; } cells[4] = {
        { 100, 3, 8.0f, 16.0f, 0x101u }, { 110, 3, 8.0f, 16.0f, 0x000u },
        { 120, 3, 8.5f, 16.5f, 0x000u }, { 130, 6, 8.0f, 12.0f, 0x101u },
    };
    const int y0 = 10;
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(COH_SAMPLE_VERTS);
    uint32_t *dl = (uint32_t *)SR_HOST(COH_SAMPLE_LIST);
    int p = 0, nv = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, 0x00160000u);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x4D, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, COH_SAMPLE_TEX & 0x00FFFFFFu);
    DL_EMIT(0xA8, 32u | ((COH_SAMPLE_TEX & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (5u << 8) | 5u);
    DL_EMIT(0xC2, 0);
    DL_EMIT(0xC8, 0);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC7, (1u << 8) | 1u);                         /* clamp u and v */
    DL_EMIT(0xC9, 3u | (1u << 8));                         /* replace, RGBA */
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));     /* float UV and position, through */
    for (int c = 0; c < 4; c++) {
        float x0 = (float)cells[c].x, x1 = x0 + 8.0f, ya = (float)y0, yb = ya + 8.0f;
        float u0 = cells[c].u0, u1 = cells[c].u1;
        uint32_t first = COH_SAMPLE_VERTS + (uint32_t)nv * (uint32_t)sizeof(RawSpriteVtx);
        DL_EMIT(0xC6, cells[c].tflt);
        DL_EMIT(0xCB, 0);                                  /* TEXFLUSH */
        if (cells[c].type == 3) {                          /* TL TR BL, BL TR BR */
            vtx[nv++] = (RawSpriteVtx){ u0, u0, x0, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u1, u0, x1, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u0, u1, x0, yb, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u0, u1, x0, yb, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u1, u0, x1, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u1, u1, x1, yb, 0.0f };
            DL_EMIT(0x01, first & 0x00FFFFFFu);
            DL_EMIT(0x04, (3u << 16) | 6u);
        } else {
            vtx[nv++] = (RawSpriteVtx){ u0, u0, x0, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u1, u1, x1, yb, 0.0f };
            DL_EMIT(0x01, first & 0x00FFFFFFu);
            DL_EMIT(0x04, (6u << 16) | 2u);
        }
    }
    DL_EMIT(0xC6, 0);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT

    uint32_t *gpu = (uint32_t *)malloc(bytes);
    if (!gpu) return 0;
    coherence_reset_targets();
    memset(fb, 0, bytes);
    sr_gpu_vram_dirty(base, (uint32_t)bytes);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(COH_SAMPLE_LIST, 0);
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    int ok = gegpu_sync_guest_fb(&desc);
    if (!ok) fprintf(stderr, "gpu coherence selftest [psp-sample-points]: GPU readback failed\n");
    if (ok) {
        memcpy(gpu, fb, bytes);
        memset(fb, 0, bytes);
        ge_set_gpu_hooks(NULL);
        ge_run_list(COH_SAMPLE_LIST, 0);
        ge_set_gpu_hooks(&k_hooks);
    }
    static const char *const names[4] = { "tri-linear-1to1", "tri-nearest-1to1",
                                          "tri-nearest-half", "spr-linear-mag2" };
    for (int c = 0; ok && c < 4; c++)
        for (int dy = 0; ok && dy < 8; dy++)
            for (int dx = 0; ok && dx < 8; dx++) {
                uint32_t want;
                if (!coherence_sample_expect(c, dx, dy, &want)) continue;
                size_t at = (size_t)(y0 + dy) * stride + (size_t)(cells[c].x + dx);
                uint32_t sw = fb[at] & 0x00FFFFFFu, gp = gpu[at] & 0x00FFFFFFu;
                want &= 0x00FFFFFFu;
                if (sw != want || gp != want) {
                    fprintf(stderr, "gpu coherence selftest [psp-sample-points/%s]: pixel=(%d,%d) "
                            "software=%06x gpu=%06x, want %06x (PSP-GE-SAMPLE-001)\n",
                            names[c], dx, dy, sw, gp, want);
                    ok = 0;
                }
            }
    free(gpu);
    coherence_reset_targets();
    if (ok) printf("gpu coherence selftest: PASS %-22s 4 cells (PSP-GE-SAMPLE-001)\n", "psp-sample-points");
    return ok;
}

/* Put TEXMODE, TEXLEVEL and TEXFILTER back to level 0 only through the list path, so the
 * later fixtures start from the state they assume and the state serial moves with it. */
static void coherence_mip_reset_state(void) {
    uint32_t *dl = (uint32_t *)SR_HOST(COH_MIP_RESET);
    dl[0] = 0xC2u << 24;
    dl[1] = 0xC8u << 24;
    dl[2] = 0xC6u << 24;
    dl[3] = 0x0Fu << 24;
    dl[4] = 0x0Cu << 24;
    ge_run_list(COH_MIP_RESET, 0);
}

/* The texel the first pixel shows: the sprite's UV runs 0.25..8.25 level-0 texels across
 * `rect` pixels, and the pixel samples it at its centre (PSP-GE-SAMPLE-001), so it reads
 * s = 0.25 + 0.5 * 8 / rect, which is s / 2^level in the level. Every case with an expected
 * level filters that level by the nearest rule, floor(s). */
static uint32_t coherence_mip_first_texel(const CohMipCase *c) {
    float s = (0.25f + 4.0f / (float)c->rect) / (float)(1u << (unsigned)c->level);
    unsigned size = 8u >> (unsigned)c->level, t = (unsigned)s;
    if (t >= size) t = size - 1u;
    return c->level == 1 && c->alt_level1 ? coherence_mip_alt_texel(t, t)
                                          : coherence_mip_texel((unsigned)c->level, t, t);
}

static int coherence_run_mip_case(const CohMipCase *c) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u;
    const size_t bytes = (size_t)stride * FB_H * 4u, pixels = bytes / 4u;
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    uint32_t *gpu = (uint32_t *)malloc(bytes);
    if (!gpu) {
        fprintf(stderr, "gpu coherence selftest [mip-%s]: no reference buffer\n", c->name);
        return 0;
    }
    uint32_t list = coherence_mip_list(c, 1);
    coherence_reset_targets();
    memset(fb, 0, bytes);
    sr_gpu_vram_dirty(base, (uint32_t)bytes);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list, 0);
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    int ok = gegpu_sync_guest_fb(&desc);
    if (!ok) fprintf(stderr, "gpu coherence selftest [mip-%s]: GPU readback failed\n", c->name);
    if (ok) {
        memcpy(gpu, fb, bytes);
        memset(fb, 0, bytes);
        ge_set_gpu_hooks(NULL);
        ge_run_list(list, 0);
        ge_set_gpu_hooks(&k_hooks);
    }
    uint32_t first = (uint32_t)(COH_MIP_Y * (int)stride + COH_MIP_X);
    if (ok && c->level >= 0) {
        uint32_t want = coherence_mip_first_texel(c) & 0x00FFFFFFu;
        if ((fb[first] & 0x00FFFFFFu) != want || (gpu[first] & 0x00FFFFFFu) != want) {
            fprintf(stderr,
                    "gpu coherence selftest [mip-%s]: first pixel gpu=%06x software=%06x, want "
                    "level %d texel %06x\n", c->name, gpu[first] & 0x00FFFFFFu,
                    fb[first] & 0x00FFFFFFu, c->level, want);
            ok = 0;
        }
    }
    for (size_t px = 0; ok && px < pixels; px++) {
        int differs = 0;
        for (unsigned shift = 0; shift < 32u; shift += 8u) {
            int delta = (int)((gpu[px] >> shift) & 0xFFu) - (int)((fb[px] >> shift) & 0xFFu);
            if (delta < -c->tolerance || delta > c->tolerance) differs = 1;
        }
        if (differs) {
            fprintf(stderr,
                    "gpu coherence selftest [mip-%s]: GPU/software mismatch at pixel=(%zu,%zu) "
                    "gpu=%08x software=%08x\n",
                    c->name, px % stride, px / stride, gpu[px], fb[px]);
            ok = 0;
        }
    }
    free(gpu);
    coherence_reset_targets();
    if (ok)
        printf("gpu coherence selftest: PASS mip %-22s mode=%u bias=%d/16 max=%u tflt=0x%03x\n",
               c->name, c->mode, (int)(int8_t)(uint8_t)c->bias16, c->max_level, c->tflt);
    return ok;
}

/* A notified CPU write into mip level 1 between two same-state draws must reach the second
 * draw: hook_vram_dirty has to see the level's own source range, not only level 0's, or the
 * state template keeps the old chain (the texture-dirty-same-state rule, for a level). */
static int coherence_run_mip_level_dirty_same_state(void) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u, rgb = 0x00FFFFFFu;
    const uint32_t level1 = COH_MIP_TEX + 0x1000u;
    const CohMipCase c = { "level-dirty", 0x004u, 3u, 1u, 16u, 8, 0, 1, 0, 0 };
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    uint32_t *tex = (uint32_t *)SR_HOST(level1);
    coherence_reset_targets();
    memset(fb, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(coherence_mip_list(&c, 0), 0);      /* the state, no draw */
    GeVtx p0 = { (float)COH_MIP_X, (float)COH_MIP_Y, 1.0f, 1.0f, 0.25f, 0.25f, 1.0f,
                 255, 255, 255, 255 };
    GeVtx p1 = { (float)(COH_MIP_X + 8), (float)(COH_MIP_Y + 8), 1.0f, 1.0f, 8.25f, 8.25f, 1.0f,
                 255, 255, 255, 255 };
    int ok = hook_sprite(&p0, &p1, 0);
    for (unsigned v = 0; v < 4u; v++)
        for (unsigned u = 0; u < 4u; u++) tex[v * 8u + u] = coherence_mip_alt_texel(u, v);
    sr_gpu_vram_dirty(level1, 4u * 8u * 4u);
    p0.x += 16.0f; p1.x += 16.0f;
    ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
    uint32_t left = fb[COH_MIP_Y * stride + COH_MIP_X] & rgb;
    uint32_t right = fb[COH_MIP_Y * stride + COH_MIP_X + 16] & rgb;
    uint32_t want_left = coherence_mip_texel(1u, 0u, 0u) & rgb;
    uint32_t want_right = coherence_mip_alt_texel(0u, 0u) & rgb;
    if (!ok || left != want_left || right != want_right) {
        fprintf(stderr,
                "gpu coherence selftest [mip-level-dirty]: draws=%d left=%06x (want %06x) "
                "right=%06x (want %06x)\n", ok, left, want_left, right, want_right);
        ok = 0;
    }
    coherence_mip_fill();                           /* level 1 back to its first contents */
    coherence_reset_targets();
    if (ok) printf("gpu coherence selftest: PASS mip %-22s old=%06x new=%06x\n",
                   "level-dirty-same-state", left, right);
    return ok;
}

static int coherence_run_mip_levels(void) {
    static const CohMipCase cases[] = {
        /* Constant mode: the bias is the level, clamped to the highest level; one half
         * rounds up to the next level (PSP-GE-MIP-001 c-b0f8). */
        { "const-level-1",       0x004u, 3u, 1u, 16u, 16, 0, 1, 0, 0 },
        { "const-level-2",       0x004u, 3u, 1u, 32u, 16, 0, 2, 0, 0 },
        { "const-clamped",       0x004u, 1u, 1u, 48u, 16, 0, 1, 0, 0 },
        { "const-half-rounds-up", 0x004u, 3u, 1u, 8u, 16, 0, 1, 0, 0 },
        { "const-mode-3",        0x004u, 3u, 3u, 32u, 16, 0, 2, 0, 0 },
        /* With mip levels on, the level of detail picks the filter within a level: a
         * magnified sprite at constant level 1 filters by the minification field
         * (nearest), and a minified one at constant level 0 by the magnification field. */
        { "const-mag-linear",    0x104u, 3u, 1u, 16u, 16, 0, 1, 0, 0 },
        { "const-0-mag-filter",  0x005u, 3u, 1u, 0u,   4, 0, 0, 0, 0 },
        /* GU_NEAREST_MIPMAP_LINEAR at constant level 0.5: half of level 0, half of level 1. */
        { "const-between",       0x006u, 3u, 1u, 8u,  16, 1, -1, 0, 0 },
        /* Automatic mode: log2 of the footprint plus the bias, clamped to [0, highest]
         * (software rasterizer). */
        { "auto-magnified",      0x004u, 3u, 0u, 0u,  16, 0, 0, 0, 0 },
        { "auto-level-1",        0x004u, 3u, 0u, 0u,   4, 0, 1, 0, 0 },
        { "auto-level-2",        0x004u, 3u, 0u, 0u,   2, 0, 2, 0, 0 },
        { "auto-biased",         0x004u, 3u, 0u, 0xF0u, 2, 0, 1, 0, 0 },
        { "auto-clamped",        0x004u, 1u, 0u, 0u,   2, 0, 1, 0, 0 },
        /* Bias -1.5 on a 2x minified sprite: level of detail -0.5, so level 0 through the
         * magnification field (nearest), not the minification field (linear). */
        { "auto-bias-mag-filter", 0x005u, 3u, 0u, 0xE8u, 4, 0, 0, 0, 0 },
        /* Slope mode with TEXLODSLOPE 0: its logarithm is -2032, so level 0 (software
         * rasterizer). */
        { "slope-zero",          0x004u, 3u, 2u, 16u,  4, 0, 0, 0, 0 },
        /* Level 1 declared 8x8 cannot sit in a halving chain: the software rasterizer draws. */
        { "chain-fallback",      0x004u, 3u, 1u, 16u, 16, 0, 1, 0, 3u },
        /* Only level 1's address changes after const-level-1: the cache must not reuse it. */
        { "level1-moved",        0x004u, 3u, 1u, 16u, 16, 0, 1, 1, 0 },
        { "level1-back",         0x004u, 3u, 1u, 16u, 16, 0, 1, 0, 0 },
    };
    int ok = 1;
    coherence_mip_fill();
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (!coherence_run_mip_case(&cases[i])) ok = 0;
    if (!coherence_run_mip_level_dirty_same_state()) ok = 0;
    coherence_mip_reset_state();
    coherence_reset_targets();
    return ok;
}

/* ---- ring wrap and blend colour parity (issue #697) -----------------------------------
 *
 * The per-slot vertex arena (VERT_ARENA_VERTS) and per-slot transfer ring are the Vulkan
 * rings a wrap could overrun while the GPU still reads them. A render that does not fit
 * the open recording moves to the next submit slot, and that slot's fence must retire
 * before its arena is written again. These fixtures drive the production hook and GE list
 * paths on synthetic guest memory and check analytic final pixels: an overwrite of
 * in-flight vertex data changes a pixel, and a stale slot cursor changes the last writer.
 *
 * The parity fixtures compare the Vulkan output with the software GE (ge.c, the arbiter)
 * for every blend factor pair, and also report the distance to the project's R6 colour
 * product (ge_raster_ref.c, SPEC_ASSUMPTION until the #343 oracle cells P8/P9 run). */
#include "../ge_raster_ref.h"

static uint32_t wrap_hash(uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
    return v;
}

static uint32_t wrap_pixel_rgba(uint32_t pass, uint32_t idx) {
    return 0xff000000u | (wrap_hash(idx * 2654435761u ^ pass * 0x9E3779B9u) & 0xFFFFFFu);
}

static void wrap_vtx(GeVtx *v, float x, float y, uint32_t c) {
    *v = (GeVtx){ x, y, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                  (int)(c & 0xFFu), (int)((c >> 8) & 0xFFu), (int)((c >> 16) & 0xFFu), (int)(c >> 24) };
}

static void wrap_state(uint32_t base, uint32_t stride) {
    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = 3;
    s_ge->zbp = 0x04080000u;
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535;
}

/* Many one-pixel sprites per pass over the whole frame. Each pass rewrites every pixel,
 * so the final frame is the last pass, and every submit slot is reused many times. */
static int coherence_run_ring_wrap(void) {
    const uint32_t fba = 0x00100000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u;
    const uint32_t pixels = (uint32_t)(FB_W * FB_H);
    const uint32_t passes = 3u;
    uint32_t *guest = (uint32_t *)SR_HOST(base);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    wrap_state(base, stride);
    ge_set_gpu_hooks(&k_hooks);

    GeGpuReplayStats before, after;
    gegpu_replay_stats_get(&before);
    uint64_t spr_before = s_cnt_spr;
    for (uint32_t pass = 0; pass < passes; pass++) {
        for (uint32_t idx = 0; idx < pixels; idx++) {
            GeVtx p0, p1;
            uint32_t c = wrap_pixel_rgba(pass, idx);
            wrap_vtx(&p0, (float)(idx % FB_W), (float)(idx / FB_W), c);
            wrap_vtx(&p1, (float)(idx % FB_W) + 1.0f, (float)(idx / FB_W) + 1.0f, c);
            if (!hook_sprite(&p0, &p1, 0)) {
                fprintf(stderr, "gpu coherence selftest [ring-wrap]: hook_sprite refused pass=%u idx=%u\n",
                        pass, idx);
                coherence_reset_targets();
                return 0;
            }
        }
    }
    Target *t = s_cur;
    if (!t || !target_readback(t)) {
        fprintf(stderr, "gpu coherence selftest [ring-wrap]: target readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_replay_stats_get(&after);

    for (uint32_t idx = 0; idx < pixels; idx++) {
        uint32_t want = wrap_pixel_rgba(passes - 1u, idx);
        /* Non-clear draws write RGB only: alpha is the PSP stencil plane (build_state
         * cmask), so it must keep the initial zero from the cleared frame. */
        if ((guest[idx] >> 24) != 0 || (guest[idx] & 0xFFFFFFu) != (want & 0xFFFFFFu)) {
            fprintf(stderr, "gpu coherence selftest [ring-wrap]: idx=%u (x=%u,y=%u) rgb got=%06x want=%06x "
                            "alpha=%02x\n", idx, idx % FB_W, idx / FB_W, guest[idx] & 0xFFFFFFu,
                    want & 0xFFFFFFu, guest[idx] >> 24);
            coherence_reset_targets();
            return 0;
        }
    }
    uint64_t sprites = s_cnt_spr - spr_before;
    unsigned long long submits = after.render_submits - before.render_submits;
    if (sprites != (uint64_t)pixels * passes) {
        fprintf(stderr, "gpu coherence selftest [ring-wrap]: sprite count %llu\n",
                (unsigned long long)sprites);
        coherence_reset_targets();
        return 0;
    }
    /* The vertex arena overflows once per VERT_ARENA_VERTS, so this fixture forces many
     * arena flushes; more than one full slot cycle proves a recycled, fenced slot. */
    if (submits <= SUBMIT_FRAMES) {
        fprintf(stderr, "gpu coherence selftest [ring-wrap]: only %llu render submits, slot ring did not recycle\n",
                submits);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s sprites=%llu verts=%llu render_submits=%llu\n",
           "ring-wrap", (unsigned long long)sprites,
           (unsigned long long)sprites * 6ull, submits);
    coherence_reset_targets();
    return 1;
}

/* Repeated uploads of different guest content into one target. Each upload is a batched
 * transfer-ring operation; the final GPU image must equal the last content with the one
 * sprite drawn on top, proving the staged copies were not overwritten before the copy ran. */
static int coherence_run_upload_sequence(void) {
    const uint32_t fba = 0x00120000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u;
    const uint32_t bytes = stride * FB_H * 4u;
    const uint32_t rounds = 12u;
    uint32_t *guest = (uint32_t *)SR_HOST(base);

    coherence_reset_targets();
    for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) guest[i] = wrap_pixel_rgba(0u, i);
    sr_gpu_vram_dirty(base, bytes);
    Target *t = target_color_acquire(fba, stride, 3, 1);
    if (!t || !t->gpu_valid) {
        fprintf(stderr, "gpu coherence selftest [upload-sequence]: could not establish target\n");
        coherence_reset_targets();
        return 0;
    }
    GeGpuReplayStats before, after;
    gegpu_replay_stats_get(&before);
    for (uint32_t k = 1; k <= rounds; k++) {
        for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) guest[i] = wrap_pixel_rgba(k, i);
        sr_gpu_vram_dirty(base, bytes);
        if (!target_upload(t)) {
            fprintf(stderr, "gpu coherence selftest [upload-sequence]: upload %u failed\n", k);
            coherence_reset_targets();
            return 0;
        }
    }
    wrap_state(base, stride);
    ge_set_gpu_hooks(&k_hooks);
    GeVtx p0, p1;
    const uint32_t c = 0xff102030u;
    wrap_vtx(&p0, 0.0f, 0.0f, c);
    wrap_vtx(&p1, 1.0f, 1.0f, c);
    if (!hook_sprite(&p0, &p1, 0) || !s_cur || !target_readback(s_cur)) {
        fprintf(stderr, "gpu coherence selftest [upload-sequence]: draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_replay_stats_get(&after);
    for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) {
        uint32_t want = i == 0 ? c : wrap_pixel_rgba(rounds, i);
        if (guest[i] != want) {
            fprintf(stderr, "gpu coherence selftest [upload-sequence]: idx=%u got=%08x want=%08x\n",
                    i, guest[i], want);
            coherence_reset_targets();
            return 0;
        }
    }
    printf("gpu coherence selftest: PASS %-22s uploads=%u reservations=%llu fallbacks=%llu\n",
           "upload-sequence", rounds,
           after.upload_ring_reservations - before.upload_ring_reservations,
           after.upload_ring_fallbacks - before.upload_ring_fallbacks);
    coherence_reset_targets();
    return 1;
}

/* ---- notified texture rewrite between two same-state draws ------------------------------
 *
 * A CPU or DMAC write into a cached texture reaches hook_vram_dirty. Invalidating the
 * texture entry alone is not enough: the cached state template still carries that
 * texture's descriptor, and a following draw whose GE registers are unchanged takes the
 * template fast path in state_get without consulting tex_get, so it samples the old
 * image. This fixture draws one sprite, rewrites the whole texture and notifies the
 * write, draws a second sprite with identical registers, and requires the first sprite
 * to keep the old texels and the second to show the new ones. */
static int coherence_run_texture_dirty_same_state(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;
    const uint32_t rgb = 0x00FFFFFFu;
    const uint32_t old_texel = 0xFF0000FFu, new_texel = 0xFF00FF00u;   /* ABGR red, green */
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    for (unsigned i = 0; i < 64u; i++) tex[i] = old_texel;
    sr_gpu_vram_dirty(tex_base, 64u * 4u);

    wrap_state(base, stride);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_base;
    s_ge->tex_bufw = 8;
    s_ge->tex_fmt = 3;
    s_ge->tex_w = 8;
    s_ge->tex_h = 8;
    s_ge->tex_func = 3;      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = 0;    /* NEAREST */
    ge_set_gpu_hooks(&k_hooks);

    GeVtx p0, p1;
    const uint32_t white = 0xFFFFFFFFu;
    wrap_vtx(&p0, 10.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 18.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    uint64_t invalidations_before = s_cnt_tex_invalidations;
    if (!hook_sprite(&p0, &p1, 0)) {
        fprintf(stderr, "gpu coherence selftest [texture-dirty-same-state]: first draw not accepted\n");
        coherence_reset_targets();
        return 0;
    }

    /* The notified write under test: same address, same size, new bytes, no GE
     * register change and no CPU-batch boundary before the next draw. */
    for (unsigned i = 0; i < 64u; i++) tex[i] = new_texel;
    sr_gpu_vram_dirty(tex_base, 64u * 4u);

    wrap_vtx(&p0, 30.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 38.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_cur || !target_readback(s_cur)) {
        fprintf(stderr, "gpu coherence selftest [texture-dirty-same-state]: second draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }

    uint32_t left = guest[14u * stride + 14u] & rgb;
    uint32_t right = guest[14u * stride + 34u] & rgb;
    if (left != (old_texel & rgb) || right != (new_texel & rgb)) {
        fprintf(stderr,
                "gpu coherence selftest [texture-dirty-same-state]: left=%06x (want %06x) "
                "right=%06x (want %06x)%s\n",
                left, old_texel & rgb, right, new_texel & rgb,
                right == (old_texel & rgb)
                    ? " -- the second draw sampled the stale cached texture" : "");
        coherence_reset_targets();
        return 0;
    }
    if (s_cnt_tex_invalidations != invalidations_before + 1u) {
        fprintf(stderr,
                "gpu coherence selftest [texture-dirty-same-state]: expected one texture "
                "invalidation, counted %llu\n",
                (unsigned long long)(s_cnt_tex_invalidations - invalidations_before));
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s invalidations=1 old=%06x new=%06x\n",
           "texture-dirty-same-state", old_texel & rgb, new_texel & rgb);
    coherence_reset_targets();
    return 1;
}

/* The clean-page index of hook_vram_dirty (#282). A notified store into a page no cached texture
 * reads marks the page, so a second store there skips the texture scan; a texture created later
 * on that page must clear the mark, or the store that next changes its texels is skipped and the
 * draw after it samples the stale cached copy. */
static int coherence_run_texture_clean_page_index(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_a = 0x04040000u, tex_b = 0x04060000u;   /* different 4 KiB pages */
    const uint32_t stride = 512u;
    const uint32_t rgb = 0x00FFFFFFu;
    const uint32_t red = 0xFF0000FFu, blue = 0xFFFF0000u, green = 0xFF00FF00u;   /* ABGR */
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    uint32_t *a = (uint32_t *)SR_HOST(tex_a);
    uint32_t *b = (uint32_t *)SR_HOST(tex_b);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    for (unsigned i = 0; i < 64u; i++) a[i] = red;
    sr_gpu_vram_dirty(tex_a, 64u * 4u);

    wrap_state(base, stride);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_a;
    s_ge->tex_bufw = 8;
    s_ge->tex_fmt = 3;
    s_ge->tex_w = 8;
    s_ge->tex_h = 8;
    s_ge->tex_func = 3;      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = 0;    /* NEAREST */
    ge_set_gpu_hooks(&k_hooks);

    GeVtx p0, p1;
    const uint32_t white = 0xFFFFFFFFu;
    wrap_vtx(&p0, 10.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 18.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0)) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: first draw not accepted\n");
        coherence_reset_targets();
        return 0;
    }

    /* Texture B's page holds no cached texture yet: the first store scans and marks it, the
     * second skips the scan. */
    uint64_t skips_before = s_cnt_tex_clean_skips;
    for (unsigned i = 0; i < 64u; i++) b[i] = blue;
    sr_gpu_vram_dirty(tex_b, 32u * 4u);
    sr_gpu_vram_dirty(tex_b + 32u * 4u, 32u * 4u);
    if (s_cnt_tex_clean_skips != skips_before + 1u) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: expected the second store "
                "to skip the scan, skipped %llu\n",
                (unsigned long long)(s_cnt_tex_clean_skips - skips_before));
        coherence_reset_targets();
        return 0;
    }

    s_ge->tex_addr = tex_b;
    ge_state_note_external_write();   /* a TEXADDR command would bump the state serial */
    wrap_vtx(&p0, 30.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 38.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0)) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: second draw not accepted\n");
        coherence_reset_targets();
        return 0;
    }

    /* The store under test: B's texels change with no GE register change before the draw. */
    uint64_t invalidations_before = s_cnt_tex_invalidations;
    for (unsigned i = 0; i < 64u; i++) b[i] = green;
    sr_gpu_vram_dirty(tex_b, 64u * 4u);
    /* B is now the only entry on its page and invalid: a second store there skips the scan. */
    uint64_t quiet_before = s_cnt_tex_quiet_skips;
    sr_gpu_vram_dirty(tex_b, 64u * 4u);
    if (s_cnt_tex_quiet_skips != quiet_before + 1u) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: expected a quiet-page skip, "
                "skipped %llu\n", (unsigned long long)(s_cnt_tex_quiet_skips - quiet_before));
        coherence_reset_targets();
        return 0;
    }
    wrap_vtx(&p0, 50.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 58.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_cur || !target_readback(s_cur)) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: third draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t left = guest[14u * stride + 14u] & rgb;
    uint32_t mid = guest[14u * stride + 34u] & rgb;
    uint32_t right = guest[14u * stride + 54u] & rgb;
    if (left != (red & rgb) || mid != (blue & rgb) || right != (green & rgb) ||
        s_cnt_tex_invalidations != invalidations_before + 1u) {
        fprintf(stderr,
                "gpu coherence selftest [texture-clean-page-index]: left=%06x mid=%06x right=%06x "
                "(want %06x %06x %06x), invalidations=%llu (want 1)%s\n",
                left, mid, right, red & rgb, blue & rgb, green & rgb,
                (unsigned long long)(s_cnt_tex_invalidations - invalidations_before),
                right == (blue & rgb) ? " -- the store after the texture was created was skipped" : "");
        coherence_reset_targets();
        return 0;
    }
    /* The third draw re-uploaded B, so its page is no longer quiet: the next store must
     * invalidate it again, and the draw after it must show the new texels. */
    const uint32_t yellow = 0xFF00FFFFu;
    quiet_before = s_cnt_tex_quiet_skips;
    invalidations_before = s_cnt_tex_invalidations;
    for (unsigned i = 0; i < 64u; i++) b[i] = yellow;
    sr_gpu_vram_dirty(tex_b, 64u * 4u);
    wrap_vtx(&p0, 70.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 78.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_cur || !target_readback(s_cur)) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: fourth draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t last = guest[14u * stride + 74u] & rgb;
    if (last != (yellow & rgb) || s_cnt_tex_quiet_skips != quiet_before ||
        s_cnt_tex_invalidations != invalidations_before + 1u) {
        fprintf(stderr,
                "gpu coherence selftest [texture-clean-page-index]: after re-upload last=%06x (want %06x), "
                "quiet skips=%llu (want 0), invalidations=%llu (want 1)%s\n",
                last, yellow & rgb, (unsigned long long)(s_cnt_tex_quiet_skips - quiet_before),
                (unsigned long long)(s_cnt_tex_invalidations - invalidations_before),
                last == (green & rgb) ? " -- a stale quiet mark skipped the store" : "");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s clean=1 quiet=1 invalidations=2\n", "texture-clean-page-index");
    coherence_reset_targets();
    return 1;
}

/* A texture in main RAM changes with no notification (guest CPU stores to main RAM are not
 * tracked), so the content hash is what finds it: one texel in the upper half of a hashed
 * 8-byte word turns from red to green between two submissions, and the second draw shows
 * it. */
static int coherence_run_texture_ram_unnotified_change(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x08530000u;
    const uint32_t stride = 512u;
    const uint32_t rgb = 0x00FFFFFFu;
    const uint32_t red = 0xFF0000FFu, green = 0xFF00FF00u;   /* ABGR */
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    for (unsigned i = 0; i < 64u; i++) tex[i] = red;

    wrap_state(base, stride);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_base;
    s_ge->tex_bufw = 8;
    s_ge->tex_fmt = 3;
    s_ge->tex_w = 8;
    s_ge->tex_h = 8;
    s_ge->tex_func = 3;      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = 0;    /* NEAREST */
    ge_set_gpu_hooks(&k_hooks);

    GeVtx p0, p1;
    const uint32_t white = 0xFFFFFFFFu;
    wrap_vtx(&p0, 10.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 18.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    int ok = hook_sprite(&p0, &p1, 0);
    /* A submission (a frame's present, say) separates the store from the next draw, as in a
     * title; within one submission an unchanged draw state reuses its template, texture
     * included. */
    ok = ok && submit_pending();
    tex[1] = green;          /* texel (1, 0): bytes 4-7 of the first 8-byte word; no notification */
    wrap_vtx(&p0, 30.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 38.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [texture-ram-unnotified]: draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t first = guest[10u * stride + 11u] & rgb;
    uint32_t second = guest[10u * stride + 31u] & rgb;
    if (first != (red & rgb) || second != (green & rgb)) {
        fprintf(stderr,
                "gpu coherence selftest [texture-ram-unnotified]: first=%06x (want %06x) "
                "second=%06x (want %06x)%s\n",
                first, red & rgb, second, green & rgb,
                second == (red & rgb) ? " -- the second draw reused the stale decode" : "");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s first=%06x second=%06x\n",
           "texture-ram-unnotified", first, second);
    coherence_reset_targets();
    return 1;
}

/* One CLUT8 texture drawn twice through the same palette bytes with a different CLUT format
 * word: the palette identity in the texture cache key covers the format, so the second draw
 * decodes again (index >> 1 picks another entry) instead of reusing the first decode. The
 * case writes GeState directly, outside a list, so it notes each write as such a writer must:
 * otherwise state_get keeps the first draw's template, texture included, and never reaches
 * the key. clut-format-list sends the same change as a CLUTFORMAT command. */
static int coherence_run_clut_format_same_palette(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;
    const uint32_t rgb = 0x00FFFFFFu;
    const uint32_t index = 6u;
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    uint8_t *tex = (uint8_t *)SR_HOST(tex_base);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    memset(tex, (int)index, 16u * 16u);
    sr_gpu_vram_dirty(tex_base, 16u * 16u);

    wrap_state(base, stride);
    for (uint32_t j = 0; j < 256u; j++) {
        uint32_t entry = 0xFF000000u | wrap_hash(0x51u + j);
        memcpy(&s_ge->clutram[j * 4u], &entry, 4);
    }
    uint32_t want_first, want_second;
    memcpy(&want_first, &s_ge->clutram[index * 4u], 4);
    memcpy(&want_second, &s_ge->clutram[(index >> 1) * 4u], 4);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_base;
    s_ge->tex_bufw = 16;
    s_ge->tex_fmt = 5;       /* CLUT8 */
    s_ge->tex_w = 16;
    s_ge->tex_h = 16;
    s_ge->tex_func = 3;      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = 0;    /* NEAREST */
    s_ge->clut_fmt = 3u | (0xFFu << 8);                 /* 8888, shift 0, mask 0xFF */
    ge_state_note_external_write();
    ge_set_gpu_hooks(&k_hooks);

    GeVtx p0, p1;
    const uint32_t white = 0xFFFFFFFFu;
    wrap_vtx(&p0, 10.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 26.0f, 26.0f, white); p1.u = 16.0f; p1.v = 16.0f;
    int ok = hook_sprite(&p0, &p1, 0);
    s_ge->clut_fmt = 3u | (1u << 2) | (0xFFu << 8);     /* same bytes, shift 1 */
    ge_state_note_external_write();
    wrap_vtx(&p0, 40.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 56.0f, 26.0f, white); p1.u = 16.0f; p1.v = 16.0f;
    ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [clut-format-same-palette]: draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t first = guest[18u * stride + 18u] & rgb;
    uint32_t second = guest[18u * stride + 48u] & rgb;
    if (first != (want_first & rgb) || second != (want_second & rgb)) {
        fprintf(stderr,
                "gpu coherence selftest [clut-format-same-palette]: first=%06x (want %06x) "
                "second=%06x (want %06x)%s\n",
                first, want_first & rgb, second, want_second & rgb,
                second == (want_first & rgb)
                    ? " -- the second draw reused the first format's decode" : "");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s first=%06x second=%06x\n",
           "clut-format-same-palette", first, second);
    coherence_reset_targets();
    return 1;
}

/* The same palette change sent the way a title sends it: one list loads the palette, draws a
 * CLUT8 sprite, changes only CLUTFORMAT (shift 1), and draws again. Every command moves the
 * GE state serial, so the second sprite must rebuild its state and decode through index >> 1. */
static int coherence_run_clut_format_list(void) {
    const uint32_t base = 0x04000000u | 0x00160000u;
    const uint32_t tex_base = 0x04040000u, clut_base = 0x04041000u;
    const uint32_t stride = 512u, rgb = 0x00FFFFFFu, index = 6u;
    const uint32_t vaddr = 0x00101800u, list_addr = 0x00100800u;
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    memset(SR_HOST(tex_base), (int)index, 16u * 16u);
    sr_gpu_vram_dirty(tex_base, 16u * 16u);
    uint32_t *clut = (uint32_t *)SR_HOST(clut_base);
    for (uint32_t j = 0; j < 256u; j++) clut[j] = 0xFF000000u | wrap_hash(0x51u + j);
    sr_gpu_vram_dirty(clut_base, 256u * 4u);
    const uint32_t want_first = clut[index] & rgb, want_second = clut[index >> 1] & rgb;

    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(vaddr);
    vtx[0] = (RawSpriteVtx){ 0.0f, 0.0f, 10.0f, 10.0f, 0.0f };
    vtx[1] = (RawSpriteVtx){ 16.0f, 16.0f, 26.0f, 26.0f, 0.0f };
    vtx[2] = (RawSpriteVtx){ 0.0f, 0.0f, 40.0f, 10.0f, 0.0f };
    vtx[3] = (RawSpriteVtx){ 16.0f, 16.0f, 56.0f, 26.0f, 0.0f };
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, base & 0x00FFFFFFu);
    DL_EMIT(0x9D, stride | ((base & 0xFF000000u) >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, tex_base & 0x00FFFFFFu);
    DL_EMIT(0xA8, 16u | ((tex_base & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (4u << 8) | 4u);
    DL_EMIT(0xC2, 0);
    DL_EMIT(0xC3, 5);                                   /* CLUT8 */
    DL_EMIT(0xC6, 0);                                   /* NEAREST */
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3u | (1u << 8));                      /* REPLACE, RGBA */
    DL_EMIT(0xB0, clut_base & 0x00FFFFFFu);
    DL_EMIT(0xB1, (clut_base & 0xFF000000u) >> 8);
    DL_EMIT(0xC5, 3u | (0xFFu << 8));                   /* 8888, shift 0, mask 0xFF */
    DL_EMIT(0xC4, 32u);                                 /* 256 entries x 4 bytes */
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0x04, (6u << 16) | 2u);
    DL_EMIT(0xC5, 3u | (1u << 2) | (0xFFu << 8));       /* same palette bytes, shift 1 */
    DL_EMIT(0x01, (vaddr + 2u * (uint32_t)sizeof(RawSpriteVtx)) & 0x00FFFFFFu);
    DL_EMIT(0x04, (6u << 16) | 2u);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list_addr, 0);
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    if (!gegpu_sync_guest_fb(&desc)) {
        fprintf(stderr, "gpu coherence selftest [clut-format-list]: GPU readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t first = guest[18u * stride + 18u] & rgb;
    uint32_t second = guest[18u * stride + 48u] & rgb;
    coherence_reset_targets();
    if (first != want_first || second != want_second) {
        fprintf(stderr,
                "gpu coherence selftest [clut-format-list]: first=%06x (want %06x) "
                "second=%06x (want %06x)%s\n",
                first, want_first, second, want_second,
                second == want_first ? " -- the second draw reused the first format's decode" : "");
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s first=%06x second=%06x\n",
           "clut-format-list", first, second);
    return 1;
}

/* ---- blend parity: Vulkan vs ge.c (arbiter) vs the R6 colour product --------------------- */

#define PARITY_GRID 16u                  /* 16 x 16 sprites, 4x4 pixels each */
#define PARITY_ORIGIN 64u
#define PARITY_FF_BOUND 2                /* analytic bound for truncating vs rounding blends */

static uint32_t parity_src(uint32_t sprite) { return wrap_hash(0x51u + sprite * 2654435761u); }
static uint32_t parity_bg(uint32_t idx) { return wrap_hash(0xB6u + idx * 40503u); }

static int parity_clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* ge.c factor_component(): the blend factor for one channel, integer 0..255. */
static int parity_factor(int f, int src_side, int chan, const int s[3], int sa,
                         const int d[3], int da, uint32_t fixa, uint32_t fixb) {
    int sv = s[chan], dv = d[chan];
    int fixed_a = (int)((fixa >> (8 * chan)) & 0xFFu), fixed_b = (int)((fixb >> (8 * chan)) & 0xFFu);
    switch (f & 0xF) {
        case 0: return src_side ? dv : sv;
        case 1: return 255 - (src_side ? dv : sv);
        case 2: return sa;
        case 3: return 255 - sa;
        case 4: return da;
        case 5: return 255 - da;
        case 6: return sa * 2;          /* doubled alphas are not clamped (PSP-GE-RASTER-001) */
        case 7: return 255 - sa * 2;
        case 8: return da * 2;
        case 9: return 255 - da * 2;
        default: return src_side ? fixed_a : fixed_b;
    }
}

/* The same blend equation with the R6 colour product ((2x+1)(2s+1))>>10 for every
 * multiply (ge_raster_ref.c sr_ge_color_mul for factors 0..255), extended to the doubled
 * alphas' 0..510 with the factor's sign applied afterwards (PSP-GE-RASTER-001). Min, max and
 * absdiff ignore the factors and equations 6 and 7 pass the source through (as ge.c does). */
static int parity_r6_mul(int v, int f) {
    int m = ((2 * v + 1) * (2 * (f < 0 ? -f : f) + 1)) >> 10;
    return f < 0 ? -m : m;
}

static int parity_r6_chan(int s, int d, int sf, int df, int eq) {
    int sv = parity_r6_mul(s, sf);
    int dv = parity_r6_mul(d, df);
    switch (eq & 7) {
        case 1: return parity_clamp8(sv - dv);
        case 2: return parity_clamp8(dv - sv);
        case 3: return s < d ? s : d;
        case 4: return s > d ? s : d;
        case 5: return s > d ? s - d : d - s;
        case 6: case 7: return s;
        default: return parity_clamp8(sv + dv);
    }
}

/* FIX blend constants for every factor/equation cell: two distinct RGB triples whose
 * channels all differ, so a dual-FIX state (sfp and dfp both FIX, constants unequal) is
 * exercised and a channel swap or a constant mix-up changes the picture. */
#define PARITY_FIXA 0x3F7F9Fu
#define PARITY_FIXB 0x7F5F2Fu

static uint32_t parity_list(uint32_t base, uint32_t vaddr, uint32_t blend_mode, uint32_t fixa,
                            uint32_t fixb, uint32_t sprites) {
    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    uint32_t stride = 512u;
    int p = 0;
    #define PARITY_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((val) & 0x00FFFFFFu)
    PARITY_EMIT(0x10, 0);
    PARITY_EMIT(0x4C, 0);
    PARITY_EMIT(0x9C, base & 0x00FFFFFFu);
    PARITY_EMIT(0x9D, stride | ((base & 0xFF000000u) >> 8));
    PARITY_EMIT(0xD2, 3);
    PARITY_EMIT(0xD3, 0);
    PARITY_EMIT(0x1D, 0);
    PARITY_EMIT(0x1E, 0);
    PARITY_EMIT(0x1F, 0);
    PARITY_EMIT(0x20, 0);
    PARITY_EMIT(0x22, 0);
    PARITY_EMIT(0x23, 0);
    PARITY_EMIT(0xE7, 1);
    PARITY_EMIT(0xE8, 0);
    PARITY_EMIT(0xE9, 0);
    PARITY_EMIT(0x15, 0);
    PARITY_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    PARITY_EMIT(0xD4, 0);
    PARITY_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    PARITY_EMIT(0x21, 1);
    PARITY_EMIT(0xDF, blend_mode & 0xFFFu);
    PARITY_EMIT(0xE0, fixa);
    PARITY_EMIT(0xE1, fixb);
    /* 8888 colour, float position, transform bypass (through) */
    PARITY_EMIT(0x12, (7u << 2) | (3u << 7) | (1u << 23));
    PARITY_EMIT(0x01, vaddr & 0x00FFFFFFu);
    PARITY_EMIT(0x04, (6u << 16) | (sprites * 2u));
    PARITY_EMIT(0x0F, 0);
    PARITY_EMIT(0x0C, 0);
    #undef PARITY_EMIT
    return list_addr;
}

/* Guest vertex layout for VERTEXTYPE 8888 colour + float position: colour first. */
typedef struct ParityGuestVtx { uint32_t rgba; float x, y, z; } ParityGuestVtx;

static void parity_vertices(uint32_t vaddr, uint32_t sprites) {
    ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(vaddr);
    for (uint32_t i = 0; i < sprites; i++) {
        uint32_t gx = i % PARITY_GRID, gy = i / PARITY_GRID;
        float x0 = (float)(PARITY_ORIGIN + 4u * gx), y0 = (float)(PARITY_ORIGIN + 4u * gy);
        uint32_t c = parity_src(i);
        v[2 * i]     = (ParityGuestVtx){ c, x0, y0, 0.0f };
        v[2 * i + 1] = (ParityGuestVtx){ c, x0 + 4.0f, y0 + 4.0f, 0.0f };
    }
}

static void parity_fill_background(uint32_t base) {
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    for (uint32_t y = 0; y < FB_H; y++)
        for (uint32_t x = 0; x < FB_W; x++) guest[y * FB_W + x] = parity_bg(y * FB_W + x);
    sr_gpu_vram_dirty(base, FB_W * FB_H * 4u);
}

typedef struct ParityTally {
    unsigned combos, pixels_nonzero;
    unsigned max_gpu_vs_sw, max_gpu_vs_r6, max_sw_vs_r6;
} ParityTally;

static int coherence_run_blend_parity(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t vaddr = 0x00101000u;
    const uint32_t stride = 512u;
    const uint32_t sprites = PARITY_GRID * PARITY_GRID;
    const uint32_t bytes = FB_W * FB_H * 4u;
    uint32_t *gpu_out = (uint32_t *)malloc(bytes);
    if (!gpu_out) return 0;
    ParityTally shader = {0}, ff = {0};
    int ok = 1;

    parity_vertices(vaddr, sprites);
    for (uint32_t eq = 0; eq < 8 && ok; eq++) {
        for (uint32_t sfp = 0; sfp <= 10 && ok; sfp++) {
            for (uint32_t dfp = 0; dfp <= 10 && ok; dfp++) {
                const uint32_t fixa = PARITY_FIXA, fixb = PARITY_FIXB;
                const uint32_t mode = sfp | (dfp << 4) | (eq << 8);
                const uint32_t list = parity_list(base, vaddr, mode, fixa, fixb, sprites);
                /* the matrix draws onto an 8888 framebuffer with dithering off, so
                 * build_state's routing reduces to blend_needs_shader() */
                int shader_path = blend_needs_shader(sfp, dfp, eq, fixa, fixb);
                ParityTally *tally = shader_path ? &shader : &ff;

                /* Vulkan path */
                coherence_reset_targets();
                parity_fill_background(base);
                ge_set_gpu_hooks(&k_hooks);
                ge_run_list(list, 0);
                GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                                           .width = 480, .height = FB_H };
                if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) { ok = 0; break; }
                memcpy(gpu_out, SR_HOST(base), bytes);

                /* ge.c software path (the arbiter) */
                coherence_reset_targets();
                parity_fill_background(base);
                ge_set_gpu_hooks(NULL);
                ge_run_list(list, 0);
                ge_set_gpu_hooks(&k_hooks);
                const uint32_t *sw_out = (const uint32_t *)SR_HOST(base);

                unsigned max_gs = 0, max_gr = 0, max_sr = 0;
                for (uint32_t sprite = 0; sprite < sprites; sprite++) {
                    uint32_t sc = parity_src(sprite);
                    int s[3] = { (int)(sc & 0xFF), (int)((sc >> 8) & 0xFF), (int)((sc >> 16) & 0xFF) };
                    int sa = (int)(sc >> 24);
                    for (uint32_t py = 0; py < 4u; py++) {
                        for (uint32_t px = 0; px < 4u; px++) {
                            uint32_t idx = (PARITY_ORIGIN + 4u * (sprite / PARITY_GRID) + py) * FB_W +
                                           PARITY_ORIGIN + 4u * (sprite % PARITY_GRID) + px;
                            uint32_t dw = parity_bg(idx);
                            int d[3] = { (int)(dw & 0xFF), (int)((dw >> 8) & 0xFF), (int)((dw >> 16) & 0xFF) };
                            int da = (int)(dw >> 24);
                            uint32_t g = gpu_out[idx], w = sw_out[idx];
                            for (int ch = 0; ch < 4; ch++) {
                                int gv = (int)((g >> (8 * ch)) & 0xFF), wv = (int)((w >> (8 * ch)) & 0xFF);
                                if (abs(gv - wv) > (int)max_gs) max_gs = (unsigned)abs(gv - wv);
                                if (gv != wv) tally->pixels_nonzero++;
                            }
                            for (int ch = 0; ch < 3; ch++) {
                                int sf = parity_factor((int)sfp, 1, ch, s, sa, d, da, fixa, fixb);
                                int df = parity_factor((int)dfp, 0, ch, s, sa, d, da, fixa, fixb);
                                int ref = parity_r6_chan(s[ch], d[ch], sf, df, (int)eq);
                                int gv = (int)((g >> (8 * ch)) & 0xFF), wv = (int)((w >> (8 * ch)) & 0xFF);
                                if (abs(gv - ref) > (int)max_gr) max_gr = (unsigned)abs(gv - ref);
                                if (abs(wv - ref) > (int)max_sr) max_sr = (unsigned)abs(wv - ref);
                            }
                        }
                    }
                }
                tally->combos++;
                if (max_gs > tally->max_gpu_vs_sw) tally->max_gpu_vs_sw = max_gs;
                if (max_gr > tally->max_gpu_vs_r6) tally->max_gpu_vs_r6 = max_gr;
                if (max_sr > tally->max_sw_vs_r6) tally->max_sw_vs_r6 = max_sr;

                if ((shader_path && max_gs != 0) || (!shader_path && max_gs > PARITY_FF_BOUND)) {
                    /* first deviating pixel, with its inputs, for the failure report */
                    for (uint32_t i = 0; i < sprites * 16u; i++) {
                        uint32_t sprite = i / 16u, px = i % 4u, py = (i / 4u) % 4u;
                        uint32_t idx = (PARITY_ORIGIN + 4u * (sprite / PARITY_GRID) + py) * FB_W +
                                       PARITY_ORIGIN + 4u * (sprite % PARITY_GRID) + px;
                        if (gpu_out[idx] == sw_out[idx]) continue;
                        fprintf(stderr, "gpu coherence selftest [blend-parity]: sfp=%u dfp=%u eq=%u "
                                        "x=%u y=%u gpu=%08x ge_c=%08x src=%08x dst=%08x\n",
                                sfp, dfp, eq, idx % FB_W, idx / FB_W, gpu_out[idx], sw_out[idx],
                                parity_src(sprite), parity_bg(idx));
                        break;
                    }
                    fprintf(stderr, "gpu coherence selftest [blend-parity]: %s blend deviates from ge.c "
                                    "by %u (bound %u)\n", shader_path ? "shader" : "fixed-function",
                            max_gs, shader_path ? 0u : (unsigned)PARITY_FF_BOUND);
                    ok = 0;
                }
                coherence_reset_targets();
            }
        }
    }
    ge_set_gpu_hooks(&k_hooks);
    free(gpu_out);
    if (!ok) return 0;
    printf("gpu coherence selftest: PASS %-22s combos=%u max_delta_vs_ge_c=0 (shader blend, exact)\n",
           "blend-parity-shader", shader.combos);
    printf("gpu coherence selftest: PASS %-22s combos=%u max_delta_vs_ge_c=%u (fixed-function, bound %d)\n",
           "blend-parity-ff", ff.combos, ff.max_gpu_vs_sw, PARITY_FF_BOUND);
    printf("gpu coherence selftest: INFO r6-reference shader gpu_vs_r6=%u sw_vs_r6=%u "
           "ff gpu_vs_r6=%u sw_vs_r6=%u\n",
           shader.max_gpu_vs_r6, shader.max_sw_vs_r6, ff.max_gpu_vs_r6, ff.max_sw_vs_r6);
    return 1;
}

/* ---- submission granularity (#827, issue #323) --------------------------------------------
 *
 * SUBMIT_BATCH_MAX caps how many recorded GE-ring ops -- render batches, destination
 * snapshot copies and ring uploads -- share one vkQueueSubmit. This fixture proves the cap is
 * a submission granularity and not an ordering boundary. It builds one GE display list and
 * runs it through ge.c's interpreter and the production hooks, so nothing here reaches the
 * backend by a route the player does not take. The list runs once per cap, and every frame
 * is compared byte for byte with ge.c's software rasterizer, the reference. The list chains
 * GPU-only dependencies that would change pixels if a longer command buffer lost or
 * reordered one:
 *   - overlapping dual-FIX shader-blend sprites with a new FIX constant per draw: each draw
 *     submits the previous render op and records a destination snapshot copy, the
 *     render+snapshot pairs behind the "mixed" player-route submits in #827;
 *   - CLUT8 sprites with a palette reload per draw (a new cache entry and a ring upload into
 *     the open command buffer each), then a GE block transfer that rewrites the texels
 *     through ge.c's CPU copy and dirty notification, and draws that re-upload those cached
 *     images in place after earlier draws in the same command buffer sampled them;
 *   - render-to-texture: draws alternate between a second target and the main one, which
 *     samples the second target's newest pixels directly.
 * No CPU-visible boundary falls inside the list, so the cap is the only thing that splits
 * it: a run must cost exactly ops/cap submits and leave ops%cap ops open. That also pins
 * SR_GPU_SUBMIT_BATCH=8 to the legacy schedule, op for op. */
#define SB_FBA_A    0x00000000u            /* main target (VRAM offset) */
#define SB_FBA_B    0x00088000u            /* render-to-texture source, after A's rows */
#define SB_ZBA      0x00110000u
#define SB_LIST     0x08400000u            /* synthetic main RAM from here on */
#define SB_VTX      0x08480000u
#define SB_PAL      0x08500000u            /* 16-entry 8888 palettes, 64 bytes each */
#define SB_TEX      0x08510000u            /* 16x16 CLUT8 texels, one 256-byte copy per run */
#define SB_XFER_SRC 0x08520000u            /* the block transfer's replacement texels */
#define SB_FIXB     0x5F7F3Fu

typedef struct SubmitBatchShape {
    uint32_t blend_draws;                  /* overlapping shader-blend sprites */
    uint32_t spaced_draws;                 /* shader-blend sprites in disjoint cells */
    uint32_t tail_draws;                   /* shader-blend sprites over the CLUT sprites */
    uint32_t clut_draws;                   /* CLUT8 sprites, one palette reload each */
    uint32_t rewrite_draws;                /* CLUT8 sprites after the block-transfer rewrite */
    uint32_t rtt_rounds;                   /* draw into B, then sample B from A */
    uint32_t size;                         /* shader-blend and target-B sprite edge */
} SubmitBatchShape;

typedef struct SubmitBatchList { uint32_t *dl; uint32_t n; uint32_t voff; } SubmitBatchList;

static void sb_emit(SubmitBatchList *l, uint32_t cmd, uint32_t val) {
    l->dl[l->n++] = (cmd << 24) | (val & 0x00FFFFFFu);
}

static void sb_target(SubmitBatchList *l, uint32_t fba) {
    sb_emit(l, 0x9C, fba);
    sb_emit(l, 0x9D, 512u | (0x04000000u >> 8));
}

/* One through-mode sprite: its two vertices, then VADDR and PRIM. Untextured sprites use
 * the 8888-colour layout (ParityGuestVtx), textured ones float UV + position. */
static void sb_sprite(SubmitBatchList *l, int textured, float x, float y, float u, float v,
                      float size, uint32_t rgba) {
    uint32_t vaddr = SB_VTX + l->voff;
    if (textured) {
        RawSpriteVtx *q = (RawSpriteVtx *)SR_HOST(vaddr);
        q[0] = (RawSpriteVtx){ u, v, x, y, 0.0f };
        q[1] = (RawSpriteVtx){ u + size, v + size, x + size, y + size, 0.0f };
        l->voff += 2u * (uint32_t)sizeof(RawSpriteVtx);
    } else {
        ParityGuestVtx *q = (ParityGuestVtx *)SR_HOST(vaddr);
        q[0] = (ParityGuestVtx){ rgba, x, y, 0.0f };
        q[1] = (ParityGuestVtx){ rgba, x + size, y + size, 0.0f };
        l->voff += 2u * (uint32_t)sizeof(ParityGuestVtx);
    }
    sb_emit(l, 0x01, vaddr);
    sb_emit(l, 0x04, (6u << 16) | 2u);
}

static uint32_t sb_fixa(uint32_t i) {
    uint32_t a = wrap_hash(0x3Du + i * 2654435761u) & 0xFFFFFFu;
    return a == SB_FIXB ? a ^ 1u : a;      /* dual FIX needs two distinct constants */
}

/* Write fresh inputs (both targets, texels, palettes, transfer source) and the display
 * list; return the list address. `tex` lets each run own its texture cache entries. */
static uint32_t submit_batch_prepare(const SubmitBatchShape *sh, uint32_t tex) {
    const uint32_t bytes = FB_W * FB_H * 4u;
    uint32_t *a = (uint32_t *)SR_HOST(0x04000000u | SB_FBA_A);
    for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) a[i] = wrap_pixel_rgba(0xA5u, i);
    sr_gpu_vram_dirty(0x04000000u | SB_FBA_A, bytes);
    memset(SR_HOST(0x04000000u | SB_FBA_B), 0, bytes);
    sr_gpu_vram_dirty(0x04000000u | SB_FBA_B, bytes);
    uint8_t *texels = (uint8_t *)SR_HOST(tex), *repl = (uint8_t *)SR_HOST(SB_XFER_SRC);
    for (uint32_t v = 0; v < 16u; v++) {
        for (uint32_t u = 0; u < 16u; u++) {
            texels[v * 16u + u] = (uint8_t)((u * 7u + v * 3u) & 15u);
            repl[v * 16u + u] = (uint8_t)((u * 5u + v * 11u + 9u) & 15u);
        }
    }
    sr_gpu_vram_dirty(tex, 256u);
    uint32_t *pal = (uint32_t *)SR_HOST(SB_PAL);
    for (uint32_t k = 0; k < sh->clut_draws; k++)
        for (uint32_t j = 0; j < 16u; j++) pal[k * 16u + j] = wrap_pixel_rgba(0xC1u + k, j);

    const uint32_t colour = (7u << 2) | (3u << 7) | (1u << 23);   /* 8888, float pos, through */
    const uint32_t uv = (3u << 0) | (3u << 7) | (1u << 23);       /* float UV, float pos */
    const float size = (float)sh->size;
    const uint32_t span_x = (uint32_t)DRAW_W - sh->size - 32u, span_y = FB_H - sh->size - 32u;
    SubmitBatchList l = { (uint32_t *)SR_HOST(SB_LIST), 0u, 0u };
    sb_emit(&l, 0x10, (SB_VTX >> 8) & 0x000F0000u);   /* BASE: vertices live in main RAM */
    sb_emit(&l, 0x13, 0);
    sb_emit(&l, 0x4C, 0);
    sb_emit(&l, 0x4D, 0);
    sb_target(&l, SB_FBA_A);
    sb_emit(&l, 0x9E, SB_ZBA);
    sb_emit(&l, 0x9F, 512u | (0x04000000u >> 8));
    sb_emit(&l, 0xD2, 3);
    sb_emit(&l, 0xD3, 0);
    static const uint8_t k_off[] = { 0x1D, 0x1F, 0x20, 0x22, 0x23, 0x24, 0x27, 0x28, 0xE8, 0xE9 };
    for (size_t i = 0; i < sizeof(k_off); i++) sb_emit(&l, k_off[i], 0);
    sb_emit(&l, 0xE7, 1);
    sb_emit(&l, 0x15, 0);
    sb_emit(&l, 0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    sb_emit(&l, 0xD4, 0);
    sb_emit(&l, 0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    sb_emit(&l, 0xC0, 0);
    sb_emit(&l, 0xC2, 0);
    sb_emit(&l, 0xC5, 3u | (0xFFu << 8));            /* 8888 palette, mask 0xFF */
    sb_emit(&l, 0xC6, 0);
    sb_emit(&l, 0xC7, 0);
    sb_emit(&l, 0xC9, 3u | (1u << 8));               /* replace, texture alpha */

    /* Shader blend: FIXA*src + FIXB*dst with a new FIXA per draw, each sprite overlapping
     * the last, so every draw blends against the previous draw's result. */
    sb_emit(&l, 0x1E, 0);
    sb_emit(&l, 0x21, 1);
    sb_emit(&l, 0xDF, 10u | (10u << 4));
    sb_emit(&l, 0xE1, SB_FIXB);
    sb_emit(&l, 0x12, colour);
    for (uint32_t i = 0; i < sh->blend_draws; i++) {
        sb_emit(&l, 0xE0, sb_fixa(i));
        sb_sprite(&l, 0, (float)(16u + (i * 3u) % span_x), (float)(16u + (i * 2u) % span_y),
                  0.0f, 0.0f, size, wrap_pixel_rgba(0x5Au, i));
    }

    /* Shader-blend sprites in disjoint cells of a band below and to the right of the
     * overlapping ones: nothing drawn since the last snapshot copy overlaps any of them (the
     * drawn region is tracked as one bounding rectangle, so each cell also lies outside the
     * rectangle around everything before it), and each reads the existing snapshot instead
     * of copying the target again. */
    for (uint32_t k = 0; k < sh->spaced_draws; k++) {
        sb_emit(&l, 0xE0, sb_fixa(sh->blend_draws + k));
        sb_sprite(&l, 0, (float)(200u + k * 24u), 228.0f, 0.0f, 0.0f, 12.0f,
                  wrap_pixel_rgba(0x6Bu, k));
    }

    /* CLUT8 sprites, one palette per draw, overlapping one another. */
    sb_emit(&l, 0x21, 0);
    sb_emit(&l, 0x1E, 1);
    sb_emit(&l, 0xA0, tex);
    sb_emit(&l, 0xA8, 16u | ((tex & 0xFF000000u) >> 8));
    sb_emit(&l, 0xB8, 4u | (4u << 8));
    sb_emit(&l, 0xC3, 5);
    sb_emit(&l, 0xB1, (SB_PAL & 0xFF000000u) >> 8);
    sb_emit(&l, 0x12, uv);
    for (uint32_t k = 0; k < sh->clut_draws; k++) {
        sb_emit(&l, 0xB0, SB_PAL + k * 64u);
        sb_emit(&l, 0xC4, 2);
        sb_sprite(&l, 1, (float)(240u + (k * 5u) % 200u), (float)(140u + (k * 3u) % 100u),
                  0.0f, 0.0f, 16.0f, 0);
    }
    /* Rewrite the texels with a 16-bit block transfer (16 rows of 16 bytes), then draw with
     * earlier palettes: same cache keys, new contents, so the images update in place. */
    sb_emit(&l, 0xB2, SB_XFER_SRC & 0x00FFFFF0u);
    sb_emit(&l, 0xB3, 8u | ((SB_XFER_SRC & 0xFF000000u) >> 8));
    sb_emit(&l, 0xB4, tex & 0x00FFFFF0u);
    sb_emit(&l, 0xB5, 8u | ((tex & 0xFF000000u) >> 8));
    sb_emit(&l, 0xEB, 0);
    sb_emit(&l, 0xEC, 0);
    sb_emit(&l, 0xEE, 7u | (15u << 10));
    sb_emit(&l, 0xEA, 0);
    for (uint32_t j = 0; j < sh->rewrite_draws; j++) {
        sb_emit(&l, 0xB0, SB_PAL + (j % sh->clut_draws) * 64u);
        sb_emit(&l, 0xC4, 2);
        sb_sprite(&l, 1, (float)(244u + j * 6u), (float)(150u + j * 4u), 0.0f, 0.0f, 16.0f, 0);
    }

    /* Shader-blend sprites over the first CLUT sprites, which were drawn after the last
     * snapshot copy: each needs a fresh copy although no blended draw came between. */
    sb_emit(&l, 0x1E, 0);
    sb_emit(&l, 0x21, 1);
    sb_emit(&l, 0x12, colour);
    for (uint32_t t = 0; t < sh->tail_draws; t++) {
        sb_emit(&l, 0xE0, sb_fixa(sh->blend_draws + sh->spaced_draws + t));
        sb_sprite(&l, 0, (float)(236u + t * 9u), 136.0f + (float)t, 0.0f, 0.0f, 24.0f,
                  wrap_pixel_rgba(0x7Cu, t));
    }
    sb_emit(&l, 0x21, 0);

    /* Render-to-texture: draw into B, then sample B's newest sprite (and its border) from A. */
    for (uint32_t r = 0; r < sh->rtt_rounds; r++) {
        uint32_t bx = 8u + (r * 12u) % span_x, by = 8u + (r * 6u) % span_y;
        sb_target(&l, SB_FBA_B);
        sb_emit(&l, 0x1E, 0);
        sb_emit(&l, 0x12, colour);
        sb_sprite(&l, 0, (float)bx, (float)by, 0.0f, 0.0f, size, wrap_pixel_rgba(0xB0u, r));
        sb_target(&l, SB_FBA_A);
        sb_emit(&l, 0x1E, 1);
        sb_emit(&l, 0xA0, SB_FBA_B);
        sb_emit(&l, 0xA8, 512u | (0x04000000u >> 8));
        sb_emit(&l, 0xB8, 9u | (9u << 8));
        sb_emit(&l, 0xC3, 3);
        sb_emit(&l, 0x12, uv);
        sb_sprite(&l, 1, (float)(24u + (r * 20u) % span_x), (float)(160u + (r % 4u) * 8u),
                  (float)(bx - 4u), (float)(by - 4u), size + 8.0f, 0);
    }
    sb_emit(&l, 0x0F, 0);
    sb_emit(&l, 0x0C, 0);
    return SB_LIST;
}

typedef struct SubmitBatchRun {
    unsigned long long ops, submits, waits, open, snapshots, uploads, wraps, fallbacks;
    unsigned long long snapshot_px;        /* device pixels the snapshot copies moved */
    double submit_ms, wait_ms, wall_ms;
} SubmitBatchRun;

/* Run the list at `cap` through the production hooks, then settle both targets through the
 * explicit snapshot boundary, which leaves the GPU frames in guest VRAM. */
static int submit_batch_run(const SubmitBatchShape *sh, uint32_t tex, uint32_t cap,
                            SubmitBatchRun *out) {
    memset(out, 0, sizeof(*out));
    coherence_reset_targets();
    if (!cmd_drain(GEGPU_BOUNDARY_LIFETIME)) return 0;
    uint32_t saved_cap = s_submit_batch_ops;
    if (s_async_submit) s_submit_batch_ops = cap;  /* synchronous mode is one op per submit */
    uint32_t list = submit_batch_prepare(sh, tex);
    ge_set_gpu_hooks(&k_hooks);

    GeGpuReplayStats before, after;
    unsigned long renders = s_cnt_submit;
    gegpu_replay_stats_get(&before);
    uint64_t started = SDL_GetTicksNS();
    ge_run_list(list, 0);
    gegpu_replay_stats_get(&after);
    renders = s_cnt_submit - renders;        /* render ops recorded by the list itself */
    out->open = s_cmd_slot ? s_cmd_slot->recorded_ops : 0u;
    GeGpuFbDescriptor da = { 0x04000000u | SB_FBA_A, 3u, 512u, (uint32_t)DRAW_W, FB_H };
    GeGpuFbDescriptor db = { 0x04000000u | SB_FBA_B, 3u, 512u, (uint32_t)DRAW_W, FB_H };
    int ok = gegpu_sync_guest_fb(&da) == GEGPU_SYNC_OK && gegpu_sync_guest_fb(&db) == GEGPU_SYNC_OK;
    out->wall_ms = (double)(SDL_GetTicksNS() - started) / 1000000.0;
    s_submit_batch_ops = saved_cap;

    out->snapshots = after.snapshot_copies - before.snapshot_copies;
    out->snapshot_px = after.snapshot_copy_px - before.snapshot_copy_px;
    out->uploads = after.upload_ring_reservations - before.upload_ring_reservations;
    out->wraps = after.upload_ring_wraps - before.upload_ring_wraps;
    out->fallbacks = after.upload_ring_fallbacks - before.upload_ring_fallbacks;
    out->ops = (unsigned long long)renders + out->snapshots + out->uploads + out->fallbacks;
    out->submits = after.queue_submits - before.queue_submits;
    for (int i = 0; i < GEGPU_BOUNDARY_COUNT; i++) {
        out->waits += after.boundary[i].waits - before.boundary[i].waits;
        out->submit_ms += (double)(after.boundary[i].submit_ns - before.boundary[i].submit_ns) / 1e6;
        out->wait_ms += (double)(after.boundary[i].wait_ns - before.boundary[i].wait_ns) / 1e6;
    }
    if (!ok) fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u snapshot sync failed\n", cap);
    return ok;
}

static int submit_batch_same(const char *what, uint32_t cap, const uint32_t *gpu,
                             const uint32_t *ref) {
    for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) {
        if (gpu[i] != ref[i]) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u target %s x=%u y=%u "
                            "gpu=%08x ge_c=%08x\n", cap, what, i % FB_W, i / FB_W, gpu[i], ref[i]);
            return 0;
        }
    }
    return 1;
}

static int coherence_run_submit_batch(void) {
    const SubmitBatchShape sh = { 48u, 8u, 2u, 24u, 6u, 8u, 24u };
    const size_t bytes = (size_t)FB_W * FB_H * 4u;
    uint32_t *ref_a = (uint32_t *)malloc(bytes), *ref_b = (uint32_t *)malloc(bytes);
    if (!ref_a || !ref_b) {
        free(ref_a); free(ref_b);
        fprintf(stderr, "gpu coherence selftest [submit-batch]: out of memory\n");
        return 0;
    }
    /* ge.c's software rasterizer on the same list: the reference every cap must equal. */
    coherence_reset_targets();
    ge_set_gpu_hooks(NULL);
    ge_run_list(submit_batch_prepare(&sh, SB_TEX), 0);
    ge_set_gpu_hooks(&k_hooks);
    memcpy(ref_a, SR_HOST(0x04000000u | SB_FBA_A), bytes);
    memcpy(ref_b, SR_HOST(0x04000000u | SB_FBA_B), bytes);

    /* Legacy cap, the configured cap, and the maximum. A warm-up run first creates the
     * pipelines, depth image and targets so every measured run records the same ops. */
    const uint32_t wanted[3] = { 8u, s_submit_batch_ops, SUBMIT_BATCH_MAX };
    uint32_t caps[4] = { s_submit_batch_ops, 0u, 0u, 0u };   /* caps[0] is the warm-up */
    uint32_t ncaps = 0;
    for (uint32_t i = 0; i < (s_async_submit ? 3u : 1u); i++) {
        /* SR_GPU_SYNC_SUBMIT=1 fixes the cap at 1: one op per submit, nothing to compare */
        uint32_t cap = s_async_submit ? wanted[i] : s_submit_batch_ops;
        int dup = 0;
        for (uint32_t j = 1; j <= ncaps; j++) dup |= caps[j] == cap;
        if (!dup) caps[++ncaps] = cap;
    }
    int ok = 1;
    for (uint32_t i = 0; i <= ncaps && ok; i++) {
        SubmitBatchRun run;
        uint32_t cap = caps[i];
        ok = submit_batch_run(&sh, SB_TEX + 0x100u * (i + 1u), cap, &run) &&
             submit_batch_same("A", cap, (const uint32_t *)SR_HOST(0x04000000u | SB_FBA_A), ref_a) &&
             submit_batch_same("B", cap, (const uint32_t *)SR_HOST(0x04000000u | SB_FBA_B), ref_b);
        if (!ok || i == 0) continue;         /* the warm-up checks frames only */
        /* Non-vacuous: every overlapping and tail blend draw recorded its snapshot copy, no
         * spaced blend draw did, and every palette and rewrite draw uploaded through the GE
         * chain. */
        if (run.snapshots != sh.blend_draws + sh.tail_draws ||
            run.uploads + run.fallbacks < sh.clut_draws + sh.rewrite_draws) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u snapshots=%llu uploads=%llu "
                            "fallbacks=%llu, want %u snapshots and >= %u uploads\n", cap,
                    run.snapshots, run.uploads, run.fallbacks, sh.blend_draws + sh.tail_draws,
                    sh.clut_draws + sh.rewrite_draws);
            ok = 0;
            break;
        }
        /* The overlapping blend draws find the snapshot holding this target with only the
         * previous sprite dirty, so each copies that box, not the frame: the copies move less
         * than one full frame apiece. Their frames still equal ge.c above. */
        const unsigned long long full_px = (unsigned long long)SCL_W * SCL_H;
        if (run.snapshot_px == 0u || run.snapshot_px >= run.snapshots * full_px) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u snapshot copies moved "
                            "%llu px for %llu copies of %llu px: no dirty-box copy\n",
                    cap, run.snapshot_px, run.snapshots, full_px);
            ok = 0;
            break;
        }
        /* The cap is the only thing that splits this list. A ring that wraps or is disabled
         * (SR_GPU_XFER_RING_KB, large SR_GPU_SCALE) adds boundaries of its own, so the exact
         * count is checked only where none occurred. */
        const char *accounting = "unchecked(ring wrap/fallback)";
        if (!s_async_submit) {
            accounting = "exact";
            ok = run.submits == run.ops && run.open == 0;
        } else if (!run.wraps && !run.fallbacks) {
            accounting = "exact";
            ok = run.submits == run.ops / cap && run.open == run.ops % cap;
        }
        if (!ok) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u ops=%llu submits=%llu open=%llu, "
                            "want submits=%llu open=%llu\n", cap, run.ops, run.submits, run.open,
                    s_async_submit ? run.ops / cap : run.ops, s_async_submit ? run.ops % cap : 0ull);
            break;
        }
        printf("gpu coherence selftest: PASS %-22s cap=%u ops=%llu submits=%llu open=%llu waits=%llu "
               "submit_ms=%.3f wait_ms=%.3f wall_ms=%.3f accounting=%s frames=ge.c-exact\n",
               "submit-batch", cap, run.ops, run.submits, run.open, run.waits, run.submit_ms,
               run.wait_ms, run.wall_ms, accounting);
    }
    free(ref_a);
    free(ref_b);
    coherence_reset_targets();
    return ok;
}

static int coherence_queue_flat_sprite(uint32_t base, uint32_t stride, uint32_t fmt,
                                       float x0, float y0, float x1, float y1) {
    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = fmt;
    s_ge->zbp = 0x04080000u;
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535u;
    GeVtx p0 = { x0, y0, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0x40, 0x50, 0x60, 0xff };
    GeVtx p1 = p0;
    p1.x = x1;
    p1.y = y1;
    return hook_sprite(&p0, &p1, 0) && s_nbatch;
}

/* #1170: a title's own memmove/memset replaced by a native codegen stub
 * (sr_native_memmove/sr_native_memset) must keep the scalar accessors' VRAM
 * contract.  The HST HUD copies a rendered 5551 framebuffer rect into a RAM
 * texture right after the list ends; a stub that formed its host pointer
 * without the read prepare copied whatever stale bytes guest VRAM held.  A
 * fill that skipped the write commit was later overwritten by the stale GPU
 * target on the next readback. */
static int coherence_run_native_bulk_copy(void) {
    const uint32_t fba = 0x00100000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u, fmt = 1u;
    const uint32_t ram = 0x08020000u;
    const uint32_t rect = 8u;
    const uint16_t want = (uint16_t)(fb_pack(0x00605040u, fmt) & 0x7fffu);
    coherence_reset_targets();
    g_sr_vram_read_pending = 0;
    memset(SR_HOST(base), 0, stride * FB_H * 2u);
    memset(SR_HOST(ram), 0xEE, rect * rect * 2u);
    if (!coherence_queue_flat_sprite(base, stride, fmt, 10.0f, 10.0f,
                                     10.0f + (float)rect, 10.0f + (float)rect)) {
        fprintf(stderr, "gpu coherence selftest: native-bulk failed to queue sprite\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_flush("listend");
    if (!g_sr_vram_read_pending) {
        fprintf(stderr, "gpu coherence selftest: native-bulk list end missed pending ownership\n");
        coherence_reset_targets();
        return 0;
    }
    for (uint32_t row = 0; row < rect; row++)
        sr_native_memmove(ram + row * rect * 2u, base + ((10u + row) * stride + 10u) * 2u,
                          rect * 2u, 0u);
    for (uint32_t i = 0; i < rect * rect; i++) {
        uint16_t px;
        memcpy(&px, SR_HOST(ram + i * 2u), sizeof px);
        if ((px & 0x7fffu) != want) {
            fprintf(stderr,
                    "gpu coherence selftest: native-bulk memmove pixel %u copied 0x%04x, expected 0x%04x\n",
                    i, px, want);
            coherence_reset_targets();
            return 0;
        }
    }

    const uint32_t fill = base + (12u * stride + 12u) * 2u;
    sr_native_memset(fill, 0xABu, 4u, 0u);
    if (!coherence_queue_flat_sprite(base, stride, fmt, 100.0f, 100.0f, 104.0f, 104.0f)) {
        fprintf(stderr, "gpu coherence selftest: native-bulk failed to queue second sprite\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_flush("listend");
    uint32_t filled = MEM_R32(fill);
    if (filled != 0xABABABABu) {
        fprintf(stderr,
                "gpu coherence selftest: native-bulk memset lost to the GPU target (0x%08x, expected 0xabababab)\n",
                filled);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS native-bulk-copy       5551 memmove rows=%u + memset survives readback\n",
           rect);
    coherence_reset_targets();
    return 1;
}

/* Stencil parity: the GPU stencil path against ge.c, bit for bit, in every framebuffer
 * format. Each cell of the matrix is one state (test function, reference, mask, the three
 * ops, the alpha write mask, blending) drawing two overlapping sprites onto a hashed
 * background, so the second sprite reads what the first wrote: a state that reads its own
 * writes must split there (stencil_splits). The depth list fills a striped depth buffer in
 * clear mode, with the stencil test on, which clear mode ignores, then draws the states the
 * GPU path takes under the depth test (depth-fail and tested stencil-fail ops KEEP). No
 * primitive may take the software fallback. */
#define STP_CELLS_X 24u
#define STP_PITCH 10u
#define STP_ORIGIN 16u

typedef struct StpList { uint32_t *dl; int p; } StpList;

static void stp_emit(StpList *l, uint32_t cmd, uint32_t val) {
    l->dl[l->p++] = (cmd << 24) | (val & 0x00FFFFFFu);
}

static void stp_begin(StpList *l, uint32_t base, uint32_t fmt, int ztest) {
    l->dl = (uint32_t *)SR_HOST(0x08040000u);
    l->p = 0;
    stp_emit(l, 0x10, 0x080000u);                         /* BASE: vertices in main RAM */
    stp_emit(l, 0x9C, base & 0x00FFFFFFu);
    stp_emit(l, 0x9D, 512u | ((base & 0xFF000000u) >> 8));
    stp_emit(l, 0x9E, 0);                                  /* depth at VRAM offset 0 */
    stp_emit(l, 0x9F, 512u | 0x040000u);
    stp_emit(l, 0xD2, fmt);
    stp_emit(l, 0xD3, 0);
    stp_emit(l, 0x1D, 0); stp_emit(l, 0x1E, 0); stp_emit(l, 0x1F, 0);
    stp_emit(l, 0x20, 0); stp_emit(l, 0x22, 0);
    stp_emit(l, 0x23, ztest ? 1u : 0u);
    stp_emit(l, 0x24, 1);                                  /* stencil test on throughout */
    stp_emit(l, 0xE7, 1);
    stp_emit(l, 0xE8, 0); stp_emit(l, 0xE9, 0);
    stp_emit(l, 0x15, 0);
    stp_emit(l, 0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    stp_emit(l, 0xD4, 0);
    stp_emit(l, 0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    stp_emit(l, 0xDF, 2u | (3u << 4));                     /* src alpha, 1 - src alpha */
    stp_emit(l, 0x12, (7u << 2) | (3u << 7) | (1u << 23));
}

/* One sprite from (x0,y0) to (x1,y1) at depth z, its vertices at *vaddr. */
static void stp_sprite(StpList *l, uint32_t *vaddr, uint32_t rgba, float x0, float y0,
                       float x1, float y1, float z) {
    ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(*vaddr);
    v[0] = (ParityGuestVtx){ rgba, x0, y0, z };
    v[1] = (ParityGuestVtx){ rgba, x1, y1, z };
    stp_emit(l, 0x01, *vaddr & 0x00FFFFFFu);
    stp_emit(l, 0x04, (6u << 16) | 2u);
    *vaddr += 2u * (uint32_t)sizeof(ParityGuestVtx);
}

/* The two overlapping sprites of a cell, in one primitive command so they can share a batch. */
static void stp_cell_sprites(StpList *l, uint32_t *vaddr, uint32_t cell, float z) {
    float x = (float)(STP_ORIGIN + STP_PITCH * (cell % STP_CELLS_X));
    float y = (float)(STP_ORIGIN + STP_PITCH * (cell / STP_CELLS_X));
    uint32_t ca = wrap_hash(0x5713u + cell * 2u), cb = wrap_hash(0x5714u + cell * 2u);
    ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(*vaddr);
    v[0] = (ParityGuestVtx){ ca, x + 1.0f, y + 1.0f, z };
    v[1] = (ParityGuestVtx){ ca, x + 7.0f, y + 7.0f, z };
    v[2] = (ParityGuestVtx){ cb, x + 3.0f, y + 3.0f, z };
    v[3] = (ParityGuestVtx){ cb, x + 9.0f, y + 9.0f, z };
    stp_emit(l, 0x01, *vaddr & 0x00FFFFFFu);
    stp_emit(l, 0x04, (6u << 16) | 4u);
    *vaddr += 4u * (uint32_t)sizeof(ParityGuestVtx);
}

static void stp_cell_state(StpList *l, uint32_t cell, uint32_t func, uint32_t ops) {
    static const uint32_t smask[5] = { 0xFFu, 0xF0u, 0x0Fu, 0x80u, 0x81u };
    static const uint32_t amask[5] = { 0x00u, 0x00u, 0xF0u, 0x0Fu, 0x80u };
    uint32_t h = wrap_hash(0xA11u + cell);
    stp_emit(l, 0xDC, func | ((h & 0xFFu) << 8) | (smask[cell % 5u] << 16));
    stp_emit(l, 0xDD, ops);
    stp_emit(l, 0xE9, amask[(cell / 5u) % 5u]);
    stp_emit(l, 0x21, (cell & 1u) ? 1u : 0u);
}

static void stp_end(StpList *l) {
    stp_emit(l, 0x10, 0);                                  /* later lists expect BASE 0 */
    stp_emit(l, 0x0F, 0);
    stp_emit(l, 0x0C, 0);
}

/* Matrix list: every test function with every stencil-fail and depth-pass op, depth off. */
static uint32_t stp_matrix_list(uint32_t base, uint32_t fmt) {
    StpList l; stp_begin(&l, base, fmt, 0);
    uint32_t vaddr = 0x08060000u, cell = 0;
    for (uint32_t func = 0; func < 8u; func++)
        for (uint32_t sfail = 0; sfail < 6u; sfail++)
            for (uint32_t zpass = 0; zpass < 6u; zpass++, cell++) {
                stp_cell_state(&l, cell, func, sfail | (((cell / 7u) % 6u) << 8) | (zpass << 16));
                stp_cell_sprites(&l, &vaddr, cell, 0.0f);
            }
    stp_end(&l);
    return 0x08040000u;
}

/* Depth list: a striped depth buffer, then depth-tested cells. */
static uint32_t stp_depth_list(uint32_t base, uint32_t fmt) {
    static const uint32_t zfunc[4] = { 4u, 5u, 6u, 7u };   /* less, lequal, greater, gequal */
    StpList l; stp_begin(&l, base, fmt, 1);
    uint32_t vaddr = 0x08060000u;
    stp_emit(&l, 0xD3, 1u | 0x400u | 0x100u);              /* depth and colour, stencil kept */
    for (uint32_t x = 0; x < STP_CELLS_X * STP_PITCH; x += 2u)
        stp_sprite(&l, &vaddr, 0xFF808080u, (float)(STP_ORIGIN + x), (float)STP_ORIGIN,
                   (float)(STP_ORIGIN + x + 2u), (float)(STP_ORIGIN + 8u * STP_PITCH),
                   (x & 2u) ? 49152.0f : 16384.0f);
    stp_emit(&l, 0xD3, 0);
    uint32_t cell = 0;
    for (uint32_t func = 0; func < 8u; func++)
        for (uint32_t zpass = 0; zpass < 6u; zpass++, cell++) {
            stp_cell_state(&l, cell, func, zpass << 16);
            stp_emit(&l, 0xDE, zfunc[cell % 4u]);
            stp_emit(&l, 0xE7, (cell / 4u) & 1u);          /* depth writes on and off */
            stp_cell_sprites(&l, &vaddr, cell, 32768.0f);
        }
    for (uint32_t sfail = 1; sfail < 6u; sfail++, cell++) {
        stp_cell_state(&l, cell, 1u, sfail | (2u << 16));   /* always: the fail op never runs */
        stp_emit(&l, 0xDE, zfunc[cell % 4u]);
        stp_emit(&l, 0xE7, 0);
        stp_cell_sprites(&l, &vaddr, cell, 32768.0f);
    }
    stp_end(&l);
    return 0x08040000u;
}

static void stp_fill_background(uint32_t base, uint32_t fmt) {
    if (fmt == 3u) {
        uint32_t *px = (uint32_t *)SR_HOST(base);
        for (uint32_t i = 0; i < FB_W * FB_H; i++) px[i] = wrap_hash(0x3C1u + i * 40503u);
    } else {
        uint16_t *px = (uint16_t *)SR_HOST(base);
        for (uint32_t i = 0; i < FB_W * FB_H; i++)
            px[i] = (uint16_t)wrap_hash(0x3C1u + i * 40503u);
    }
    sr_gpu_vram_dirty(base, FB_W * FB_H * (fmt == 3u ? 4u : 2u));
}

static int coherence_run_stencil_parity(void) {
    const uint32_t base = 0x04000000u | 0x00140000u;
    const uint32_t max_bytes = FB_W * FB_H * 4u;
    uint8_t *gpu_out = (uint8_t *)malloc(max_bytes);
    if (!gpu_out) return 0;
    GeState saved = *s_ge;                 /* the lists leave the stencil test and masks on */
    int ok = 1;
    unsigned runs = 0, differing = 0;
    const unsigned long long splits0 = s_replay_stats.stencil_splits;
    for (uint32_t fmt = 0; fmt < 4u && ok; fmt++) {
        for (int depth = 0; depth < 2 && ok; depth++) {
            const uint32_t bytes = FB_W * FB_H * (fmt == 3u ? 4u : 2u);
            const uint32_t list = depth ? stp_depth_list(base, fmt) : stp_matrix_list(base, fmt);
            coherence_reset_targets();
            stp_fill_background(base, fmt);
            const unsigned long long fallback0 = s_replay_stats.fallback_prims;
            ge_set_gpu_hooks(&k_hooks);
            ge_run_list(list, 0);
            GeGpuFbDescriptor desc = { .addr = base, .format = fmt, .stride = 512u,
                                       .width = 480, .height = FB_H };
            if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) { ok = 0; break; }
            memcpy(gpu_out, SR_HOST(base), bytes);
            const unsigned long long fallbacks = s_replay_stats.fallback_prims - fallback0;

            coherence_reset_targets();
            stp_fill_background(base, fmt);
            ge_set_gpu_hooks(NULL);
            ge_run_list(list, 0);
            ge_set_gpu_hooks(&k_hooks);
            const uint8_t *sw_out = (const uint8_t *)SR_HOST(base);
            runs++;
            /* The list must draw: count what ge.c changed against a fresh background. */
            const uint32_t bpp = fmt == 3u ? 4u : 2u;
            unsigned drawn = 0;
            for (uint32_t i = 0; i < FB_W * FB_H; i++) {
                uint32_t bg = wrap_hash(0x3C1u + i * 40503u), w = 0;
                memcpy(&w, sw_out + i * bpp, bpp);
                if (fmt != 3u) bg &= 0xFFFFu;
                if (w != bg) drawn++;
            }
            if (drawn < 4096u) {
                fprintf(stderr, "gpu coherence selftest [stencil-parity]: fmt=%u %s list changed "
                                "only %u pixels\n", fmt, depth ? "depth" : "matrix", drawn);
                ok = 0;
            }
            if (fallbacks) {
                fprintf(stderr, "gpu coherence selftest [stencil-parity]: fmt=%u %s list took "
                                "the software fallback for %llu primitives\n",
                        fmt, depth ? "depth" : "matrix", fallbacks);
                ok = 0;
            }
            for (uint32_t i = 0; i < FB_W * FB_H; i++) {
                if (i % FB_W >= 480u || !memcmp(gpu_out + i * bpp, sw_out + i * bpp, bpp)) continue;
                if (!differing++) {
                    uint32_t g = 0, w = 0;
                    memcpy(&g, gpu_out + i * bpp, bpp); memcpy(&w, sw_out + i * bpp, bpp);
                    uint32_t x = i % FB_W, y = i / FB_W;
                    uint32_t cell = (y - STP_ORIGIN) / STP_PITCH * STP_CELLS_X + (x - STP_ORIGIN) / STP_PITCH;
                    fprintf(stderr, "gpu coherence selftest [stencil-parity]: fmt=%u %s cell=%u "
                                    "x=%u y=%u gpu=%08x ge_c=%08x\n",
                            fmt, depth ? "depth" : "matrix", cell, x, y, g, w);
                }
                ok = 0;
            }
            coherence_reset_targets();
        }
    }
    free(gpu_out);
    *s_ge = saved;
    const unsigned long long splits = s_replay_stats.stencil_splits - splits0;
    if (ok && !splits) {
        fprintf(stderr, "gpu coherence selftest [stencil-parity]: no state split its batch\n");
        ok = 0;
    }
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [stencil-parity]: %u pixels differ from ge.c\n",
                differing);
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s lists=%u splits=%llu max_delta_vs_ge_c=0\n",
           "stencil-parity", runs, splits);
    return 1;
}

/* The fallback classifier names the first unsupported state, in the order the support
 * check always tested them, and a supported state names none. */
static int coherence_run_fallback_reasons(void) {
    GeState saved = *s_ge;
    int ok = 1;
    struct { const char *what; int want; } cells[] = {
        {"stencil", GEGPU_FALLBACK_STENCIL}, {"color test", GEGPU_FALLBACK_COLOR_TEST},
        {"logic op", GEGPU_FALLBACK_LOGIC_OP}, {"ram fb", GEGPU_FALLBACK_RAM_FB},
        {"wide stride", GEGPU_FALLBACK_WIDE_STRIDE},
    };
    s_ge->stencil_enable = s_ge->color_test_enable = s_ge->logic_op_enable = 0;
    s_ge->clear = 0; s_ge->mask_rgb = 0;   /* an exact write mask */
    s_ge->ztest_enable = 1;                /* a depth-tested stencil-fail INCR: unsupported */
    s_ge->stencil_test = 2u; s_ge->stencil_op = 4u;
    s_ge->fbp = 0x04000000u; s_ge->fbw = 512u;
    s_ge->tex_enable = 0;
    if (gpu_state_unsupported_reason() != -1) ok = 0;
    for (unsigned i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
        s_ge->stencil_enable = i == 0;
        s_ge->color_test_enable = i == 1;
        s_ge->logic_op_enable = i == 2;
        s_ge->fbp = i == 3 ? 0x08000000u : 0x04000000u;   /* main RAM, not VRAM */
        s_ge->fbw = i == 4 ? 1024u : 512u;
        const int got = gpu_state_unsupported_reason();
        if (got != cells[i].want) {
            fprintf(stderr, "gpu coherence selftest [fallback-reasons]: %s gave %d, want %d\n",
                    cells[i].what, got, cells[i].want);
            ok = 0;
        }
    }
    s_ge->fbp = 0x04000000u; s_ge->fbw = 512u;
    s_ge->stencil_enable = 1; s_ge->color_test_enable = 1;   /* stencil is tested first */
    if (gpu_state_unsupported_reason() != GEGPU_FALLBACK_STENCIL) ok = 0;
    /* The stencil states the GPU path takes: clear mode (ge.c ignores the test there), no
     * depth test, or KEEP for the ops the depth test can leave to run. */
    struct { const char *what; int clear, ztest; uint32_t test, op; int want; } st[] = {
        {"stencil clear", 1, 1, 2u, 0x000004u, -1},
        {"stencil no depth", 0, 0, 2u, 0x040404u, -1},
        {"stencil always", 0, 1, 1u, 0x020004u, -1},
        {"stencil keep", 0, 1, 2u, 0x050000u, -1},
        {"stencil zfail op", 0, 1, 1u, 0x000100u, GEGPU_FALLBACK_STENCIL},
    };
    s_ge->color_test_enable = 0;
    for (unsigned i = 0; i < sizeof(st) / sizeof(st[0]); i++) {
        s_ge->clear = st[i].clear; s_ge->ztest_enable = st[i].ztest;
        s_ge->stencil_test = st[i].test; s_ge->stencil_op = st[i].op;
        const int got = gpu_state_unsupported_reason();
        if (got != st[i].want) {
            fprintf(stderr, "gpu coherence selftest [fallback-reasons]: %s gave %d, want %d\n",
                    st[i].what, got, st[i].want);
            ok = 0;
        }
    }
    *s_ge = saved;
    if (ok) printf("gpu coherence selftest: PASS %-22s 11 cells\n", "fallback-reasons");
    return ok;
}

/* GPU transform and lighting parity (#318). Each cell is one display list of project-written
 * command words and generated vertices (no game data) drawing transform-mode triangles into a
 * cleared 512x272 8888 target with a depth buffer. It runs three ways: through the backend with
 * GPU T&L on, through the backend with it off (ge.c's T&L, then hook_tri), and in software.
 * The first two must leave byte-identical colour and depth, and the T&L run must count every
 * transform PRIM as drawn by psp_tnl.vert, or, for a cell whose state ge.c declines (a
 * near-plane crossing, specular light, texgen, flat shading), as declined: a run that silently
 * took the other route would prove nothing. The software run is reported, not asserted: the
 * Vulkan rasterizer's own tolerances against ge.c are the blend and sample-point fixtures'. On a
 * device without fp32 subnormal preservation there is no T&L stage, and every cell must take
 * ge.c's route and match: the fallback is what that device can show. */
enum {
    TNLC_STATIC, TNLC_STRIP_CULL0, TNLC_STRIP_CULL1, TNLC_FAN_IDX16, TNLC_LIST_IDX8,
    TNLC_TEXTURED, TNLC_FOG_LINEAR, TNLC_FOG_INF, TNLC_DEPTH, TNLC_RAMP, TNLC_FIXED_FMT,
    TNLC_MORPH2, TNLC_SKIN1, TNLC_SKIN2_U8, TNLC_SKIN4_U16, TNLC_SKIN8_F, TNLC_LIGHT_DIR,
    TNLC_LIGHT_POINT, TNLC_LIGHT4_MATUPD, TNLC_LIGHT_SKIN, TNLC_OFFSET_VIEWPORT,
    TNLC_NEAR_CROSS, TNLC_SPECULAR, TNLC_FLAT, TNLC_TEXGEN, TNLC_LIGHT_SUBNORMAL,
};

typedef struct TnlCell {
    const char *name;
    int kind;
    int gpu;          /* 1: drawn by psp_tnl.vert, 0: declined to ge.c */
} TnlCell;

#define TNLP_FBA     0x00000000u                 /* VRAM-relative colour buffer */
#define TNLP_ZBA     0x00100000u                 /* VRAM-relative depth buffer */
#define TNLP_TEX     0x04180000u                 /* 8x8 8888 texture */
#define TNLP_LIST    0x00200000u                 /* the harness arena, as the other fixtures */
#define TNLP_VTX     0x00210000u
#define TNLP_IDX     0x00280000u
#define TNLP_STRIDE  512u

static uint32_t tnlp_f24(float f) { uint32_t u; memcpy(&u, &f, 4); return u >> 8; }

/* Vertex writer following decode_vtype's order and alignment: weights, texture coordinate,
 * colour (8888 only), normal, position; each field aligned to its own size, the vertex to the
 * largest. fmt values are the vertex type's field codes (0 none, 1 8-bit, 2 16-bit, 3 float). */
typedef struct { int wfmt, wn, tc, col, nrm, pos, morph; } TnlpFmt;

static uint32_t tnlp_vtype(const TnlpFmt *f, int idx) {
    uint32_t vt = (uint32_t)f->tc | ((uint32_t)(f->col ? 7 : 0) << 2) | ((uint32_t)f->nrm << 5) |
                  ((uint32_t)f->pos << 7) | ((uint32_t)idx << 11);
    if (f->wfmt) vt |= ((uint32_t)f->wfmt << 9) | ((uint32_t)(f->wn - 1) << 14);
    if (f->morph > 1) vt |= (uint32_t)(f->morph - 1) << 18;
    return vt;
}

static uint32_t tnlp_align(uint32_t off, uint32_t a) { return (off + a - 1u) & ~(a - 1u); }

static void tnlp_put(uint8_t *d, uint32_t off, int fmt, float v, int is_unsigned) {
    if (fmt == 3) { memcpy(d + off, &v, 4); return; }
    if (fmt == 2) {
        int32_t q = (int32_t)lrintf(v * 32768.0f);
        if (is_unsigned) { if (q < 0) q = 0; if (q > 65535) q = 65535; }
        else { if (q < -32768) q = -32768; if (q > 32767) q = 32767; }
        uint16_t s = (uint16_t)q; memcpy(d + off, &s, 2); return;
    }
    int32_t q = (int32_t)lrintf(v * 128.0f);
    if (is_unsigned) { if (q < 0) q = 0; if (q > 255) q = 255; }
    else { if (q < -128) q = -128; if (q > 127) q = 127; }
    d[off] = (uint8_t)q;
}

/* One morph target of one vertex at d; returns the target's stride. */
static uint32_t tnlp_vertex(uint8_t *d, const TnlpFmt *f, const float *w, float u, float v,
                            uint32_t col, const float *n, const float *p) {
    static const uint32_t sz[4] = { 0, 1, 2, 4 };
    uint32_t off = 0, maxa = 1;
    if (f->wfmt) {
        uint32_t a = sz[f->wfmt];
        off = tnlp_align(off, a);
        for (int i = 0; i < f->wn; i++) tnlp_put(d, off + a * (uint32_t)i, f->wfmt, w[i], 1);
        off += a * (uint32_t)f->wn;
        if (a > maxa) maxa = a;
    }
    if (f->tc) {
        uint32_t a = sz[f->tc];
        off = tnlp_align(off, a);
        tnlp_put(d, off, f->tc, u, 1); tnlp_put(d, off + a, f->tc, v, 1);
        off += 2u * a;
        if (a > maxa) maxa = a;
    }
    if (f->col) {
        off = tnlp_align(off, 4u);
        memcpy(d + off, &col, 4);
        off += 4u;
        if (maxa < 4u) maxa = 4u;
    }
    if (f->nrm) {
        uint32_t a = sz[f->nrm];
        off = tnlp_align(off, a);
        for (int i = 0; i < 3; i++) tnlp_put(d, off + a * (uint32_t)i, f->nrm, n[i], 0);
        off += 3u * a;
        if (a > maxa) maxa = a;
    }
    uint32_t a = sz[f->pos];
    off = tnlp_align(off, a);
    for (int i = 0; i < 3; i++) tnlp_put(d, off + a * (uint32_t)i, f->pos, p[i], 0);
    off += 3u * a;
    if (a > maxa) maxa = a;
    return tnlp_align(off, maxa);
}

typedef struct { uint32_t *dl; int n; } TnlpList;
#define TNLP_W(L, cmd, data) ((L)->dl[(L)->n++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(data) & 0x00FFFFFFu))

static void tnlp_matrix(TnlpList *L, uint32_t num_cmd, const float *m, int count) {
    TNLP_W(L, num_cmd, 0);
    for (int i = 0; i < count; i++) TNLP_W(L, num_cmd + 1u, tnlp_f24(m[i]));
}

/* A deterministic pseudo-random sequence: the same vertices on every run and host. */
static uint32_t s_tnlp_seed;
static float tnlp_rand(float lo, float hi) {
    s_tnlp_seed = s_tnlp_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(s_tnlp_seed >> 8) * (1.0f / 16777216.0f);
}

/* A rotation about the axis (ax, ay, az) by `ang` radians, translated by t, as a 4x3 matrix. */
static void tnlp_rot(float *m, float ax, float ay, float az, float ang, float tx, float ty, float tz) {
    float l = sqrtf(ax * ax + ay * ay + az * az);
    ax /= l; ay /= l; az /= l;
    float c = cosf(ang), s = sinf(ang), k = 1.0f - c;
    m[0] = c + ax * ax * k;      m[1] = ay * ax * k + az * s; m[2] = az * ax * k - ay * s;
    m[3] = ax * ay * k - az * s; m[4] = c + ay * ay * k;      m[5] = az * ay * k + ax * s;
    m[6] = ax * az * k + ay * s; m[7] = ay * az * k - ax * s; m[8] = c + az * az * k;
    m[9] = tx; m[10] = ty; m[11] = tz;
}

/* Build cell `c`'s list; returns the list address. */
static uint32_t tnlp_build(const TnlCell *c) {
    TnlpList L = { (uint32_t *)SR_HOST(TNLP_LIST), 0 };
    s_tnlp_seed = 0x1234567u + (uint32_t)c->kind * 7919u;
    const uint32_t fb = 0x04000000u | TNLP_FBA;
    TNLP_W(&L, 0x10, 0);
    TNLP_W(&L, 0x9C, fb & 0x00FFFFFFu);
    TNLP_W(&L, 0x9D, TNLP_STRIDE | ((fb & 0xFF000000u) >> 8));
    TNLP_W(&L, 0xD2, 3);
    TNLP_W(&L, 0x9E, TNLP_ZBA);
    TNLP_W(&L, 0x9F, TNLP_STRIDE);
    TNLP_W(&L, 0x15, 0);
    TNLP_W(&L, 0x16, ((FB_H - 1u) << 10) | (480u - 1u));
    TNLP_W(&L, 0xD4, 0);
    TNLP_W(&L, 0xD5, ((FB_H - 1u) << 10) | (480u - 1u));
    TNLP_W(&L, 0xD6, 0);
    TNLP_W(&L, 0xD7, 0xFFFF);
    for (uint32_t cmd = 0x17; cmd <= 0x28; cmd++) TNLP_W(&L, cmd, 0);   /* every unit off */
    TNLP_W(&L, 0xE7, 0);                  /* depth writes on */
    TNLP_W(&L, 0xDE, 1);                  /* depth test ALWAYS when enabled */
    TNLP_W(&L, 0x50, c->kind == TNLC_FLAT ? 0 : 1);
    TNLP_W(&L, 0xE8, 0);
    TNLP_W(&L, 0xE9, 0);

    /* Clear colour and depth with a through-mode clear sprite. */
    const uint32_t clear_vtx = TNLP_VTX + 0x60000u;
    {
        uint8_t *d = (uint8_t *)SR_HOST(clear_vtx);
        const uint32_t bg = 0xff302010u;
        const float cz = c->kind == TNLC_DEPTH ? 65535.0f : 0.0f;
        float p0[3] = { 0.0f, 0.0f, cz }, p1[3] = { 480.0f, 272.0f, cz };
        memcpy(d, &bg, 4); memcpy(d + 4, p0, 12);
        memcpy(d + 16, &bg, 4); memcpy(d + 20, p1, 12);
        TNLP_W(&L, 0xD3, 0x701);
        TNLP_W(&L, 0x12, (7u << 2) | (3u << 7) | (1u << 23));
        TNLP_W(&L, 0x10, (clear_vtx >> 8) & 0x000F0000u);
        TNLP_W(&L, 0x01, clear_vtx);
        TNLP_W(&L, 0x04, (6u << 16) | 2u);
        TNLP_W(&L, 0xD3, 0);
    }

    /* Matrices: a perspective camera looking at the origin from +z. */
    float world[12], view[12], proj[16];
    tnlp_rot(world, 0.3f, 1.0f, 0.2f, 0.6f, 0.0f, 0.0f, 0.0f);
    tnlp_rot(view, 1.0f, 0.0f, 0.0f, 0.15f, 0.05f, -0.1f, -6.0f);
    {
        const float fov = 1.0f, aspect = 480.0f / 272.0f, zn = 0.5f, zf = 100.0f;
        float f = 1.0f / tanf(fov * 0.5f);
        memset(proj, 0, sizeof(proj));
        proj[0] = f / aspect; proj[5] = f;
        proj[10] = (zf + zn) / (zn - zf); proj[11] = -1.0f;
        proj[14] = 2.0f * zf * zn / (zn - zf);
    }
    if (c->kind == TNLC_NEAR_CROSS) view[11] = -1.2f;   /* the mesh reaches through the near plane */
    if (c->kind == TNLC_LIGHT_SUBNORMAL) {
        /* World 3x3 scaled by 2^-30 and view 3x3 by 2^30: the same screen positions (powers of
         * two scale exactly), but world-space normals near 2^-66, whose squared length is
         * subnormal before light_vertex normalises them. */
        for (int i = 0; i < 9; i++) { world[i] = ldexpf(world[i], -30); view[i] = ldexpf(view[i], 30); }
    }
    tnlp_matrix(&L, 0x3A, world, 12);
    tnlp_matrix(&L, 0x3C, view, 12);
    tnlp_matrix(&L, 0x3E, proj, 16);
    const float vxs = c->kind == TNLC_OFFSET_VIEWPORT ? 230.0f : 240.0f;
    const float vys = c->kind == TNLC_OFFSET_VIEWPORT ? -130.0f : -136.0f;
    TNLP_W(&L, 0x42, tnlp_f24(vxs));
    TNLP_W(&L, 0x43, tnlp_f24(vys));
    TNLP_W(&L, 0x44, tnlp_f24(-32000.0f));
    TNLP_W(&L, 0x45, tnlp_f24(2048.0f));
    TNLP_W(&L, 0x46, tnlp_f24(2048.0f));
    TNLP_W(&L, 0x47, tnlp_f24(32767.5f));
    if (c->kind == TNLC_OFFSET_VIEWPORT) {
        TNLP_W(&L, 0x4C, ((2048u - 240u) << 4) + 5u);   /* a sub-pixel offset */
        TNLP_W(&L, 0x4D, ((2048u - 136u) << 4) + 11u);
    } else {
        TNLP_W(&L, 0x4C, (2048u - 240u) << 4);
        TNLP_W(&L, 0x4D, (2048u - 136u) << 4);
    }
    TNLP_W(&L, 0x48, tnlp_f24(1.0f));
    TNLP_W(&L, 0x49, tnlp_f24(1.0f));
    TNLP_W(&L, 0x4A, 0);
    TNLP_W(&L, 0x4B, 0);
    TNLP_W(&L, 0xC0, c->kind == TNLC_TEXGEN ? 1u : 0u);

    /* Texture for the textured cells. */
    if (c->kind == TNLC_TEXTURED || c->kind == TNLC_TEXGEN || c->kind == TNLC_FIXED_FMT) {
        uint32_t *tex = (uint32_t *)SR_HOST(TNLP_TEX);
        for (int v = 0; v < 8; v++)
            for (int u = 0; u < 8; u++)
                tex[v * 8 + u] = 0xff000000u | ((uint32_t)(v * 32) << 8) | (uint32_t)(u * 32) |
                                 (((u ^ v) & 1) ? 0x00c00000u : 0x00200000u);
        sr_gpu_vram_dirty(TNLP_TEX, 8u * 8u * 4u);
        TNLP_W(&L, 0x1E, 1);
        TNLP_W(&L, 0xA0, TNLP_TEX & 0x00FFFFFFu);
        TNLP_W(&L, 0xA8, 8u | ((TNLP_TEX & 0xFF000000u) >> 8));
        TNLP_W(&L, 0xB8, (3u << 8) | 3u);
        TNLP_W(&L, 0xC2, 0);
        TNLP_W(&L, 0xC3, 3);
        TNLP_W(&L, 0xC6, 0);
        TNLP_W(&L, 0xC7, 0);                   /* repeat */
        TNLP_W(&L, 0xC9, 0x100u | 0u);         /* MODULATE, RGBA */
        TNLP_W(&L, 0xCB, 0);
        if (c->kind == TNLC_TEXTURED) {
            TNLP_W(&L, 0x48, tnlp_f24(1.75f));
            TNLP_W(&L, 0x49, tnlp_f24(-0.5f));
            TNLP_W(&L, 0x4A, tnlp_f24(0.125f));
            TNLP_W(&L, 0x4B, tnlp_f24(0.3f));
        }
        if (c->kind == TNLC_TEXGEN) {
            float tg[12]; tnlp_rot(tg, 0.0f, 0.0f, 1.0f, 0.4f, 0.5f, 0.5f, 1.0f);
            tnlp_matrix(&L, 0x40, tg, 12);
            TNLP_W(&L, 0xC0, 1u | (0u << 8));
        }
    }

    if (c->kind == TNLC_FOG_LINEAR || c->kind == TNLC_FOG_INF) {
        TNLP_W(&L, 0x1F, 1);
        TNLP_W(&L, 0xCF, 0x00406080u);
        if (c->kind == TNLC_FOG_LINEAR) {
            TNLP_W(&L, 0xCD, tnlp_f24(7.5f));
            TNLP_W(&L, 0xCE, tnlp_f24(0.37f));
        } else {
            TNLP_W(&L, 0xCD, 0x7F8000u);           /* +infinity */
            TNLP_W(&L, 0xCE, tnlp_f24(1.0f));
        }
    }

    if (c->kind == TNLC_DEPTH || c->kind == TNLC_STATIC) {
        TNLP_W(&L, 0x23, 1);
        TNLP_W(&L, 0xDE, c->kind == TNLC_DEPTH ? 4u : 6u);   /* LESS / GREATER */
    }
    if (c->kind == TNLC_STRIP_CULL0 || c->kind == TNLC_STRIP_CULL1) {
        TNLP_W(&L, 0x1D, 1);
        TNLP_W(&L, 0x9B, c->kind == TNLC_STRIP_CULL1 ? 1u : 0u);
    }

    /* Bones: each a rotation with a translation. */
    int skin_w = c->kind == TNLC_SKIN1 ? 1 : c->kind == TNLC_SKIN2_U8 ? 2 :
                 c->kind == TNLC_SKIN4_U16 ? 4 : c->kind == TNLC_SKIN8_F ? 8 :
                 c->kind == TNLC_LIGHT_SKIN ? 3 : 0;
    if (skin_w) {
        TNLP_W(&L, 0x2A, 0);
        for (int b = 0; b < 8; b++) {
            float m[12];
            tnlp_rot(m, tnlp_rand(-1, 1), tnlp_rand(-1, 1), tnlp_rand(0.2f, 1), tnlp_rand(-0.5f, 0.5f),
                     tnlp_rand(-0.3f, 0.3f), tnlp_rand(-0.3f, 0.3f), tnlp_rand(-0.3f, 0.3f));
            for (int i = 0; i < 12; i++) TNLP_W(&L, 0x2B, tnlp_f24(m[i]));
        }
    }
    if (c->kind == TNLC_MORPH2) {
        TNLP_W(&L, 0x2C, tnlp_f24(0.625f));
        TNLP_W(&L, 0x2D, tnlp_f24(0.375f));
    }

    /* Lighting. */
    int lit = c->kind == TNLC_LIGHT_DIR || c->kind == TNLC_LIGHT_POINT ||
              c->kind == TNLC_LIGHT4_MATUPD || c->kind == TNLC_LIGHT_SKIN || c->kind == TNLC_SPECULAR ||
              c->kind == TNLC_LIGHT_SUBNORMAL;
    if (lit) {
        TNLP_W(&L, 0x17, 1);
        TNLP_W(&L, 0x53, c->kind == TNLC_LIGHT4_MATUPD ? 3u : 0u);
        TNLP_W(&L, 0x54, 0x00080402u);
        TNLP_W(&L, 0x55, 0x00405060u);
        TNLP_W(&L, 0x56, 0x00c0a0e0u);
        TNLP_W(&L, 0x57, 0x00ffffffu);
        TNLP_W(&L, 0x58, 0xb0);
        /* A positive power only where the cell means to decline (powf); the powered-diffuse
           light of the four-light cell then lights as plain diffuse. */
        TNLP_W(&L, 0x5B, tnlp_f24(c->kind == TNLC_SPECULAR ? 8.0f : 0.0f));
        TNLP_W(&L, 0x5C, 0x00303030u);
        TNLP_W(&L, 0x5D, 0xc0);
        int nl = c->kind == TNLC_LIGHT4_MATUPD ? 4 : 1;
        for (int i = 0; i < nl; i++) {
            int point = c->kind == TNLC_LIGHT_POINT || (c->kind == TNLC_LIGHT4_MATUPD && (i & 1));
            uint32_t comp = c->kind == TNLC_SPECULAR ? 1u : (c->kind == TNLC_LIGHT4_MATUPD && i == 2 ? 2u : 0u);
            TNLP_W(&L, 0x18 + (uint32_t)i, 1);
            TNLP_W(&L, 0x5F + (uint32_t)i, comp | ((uint32_t)(point ? 1 : 0) << 8));
            float pos[3] = { tnlp_rand(-3, 3), tnlp_rand(-3, 3), tnlp_rand(1, 4) };
            for (int k = 0; k < 3; k++) TNLP_W(&L, 0x63 + (uint32_t)(3 * i + k), tnlp_f24(pos[k]));
            float att[3] = { 0.6f, 0.15f, 0.02f };
            for (int k = 0; k < 3; k++) TNLP_W(&L, 0x7B + (uint32_t)(3 * i + k), tnlp_f24(att[k]));
            TNLP_W(&L, 0x8F + (uint32_t)(3 * i), 0x00201810u + (uint32_t)i * 0x00080402u);
            TNLP_W(&L, 0x90 + (uint32_t)(3 * i), 0x00c0d0e0u - (uint32_t)i * 0x00102030u);
            TNLP_W(&L, 0x91 + (uint32_t)(3 * i), 0x00ffffffu);
        }
    }

    /* The mesh: a grid of 12 x 8 quads over [-1.6, 1.6] x [-1.1, 1.1], displaced in z, with
     * colours, normals, texture coordinates and weights from the sequence. */
    TnlpFmt f = { 0, 0, 3, 1, 3, 3, 1 };
    if (skin_w) {
        f.wfmt = c->kind == TNLC_SKIN2_U8 ? 1 : c->kind == TNLC_SKIN4_U16 ? 2 : 3;
        f.wn = skin_w;
    }
    if (c->kind == TNLC_FIXED_FMT) { f.tc = 2; f.nrm = 1; f.pos = 2; }
    if (c->kind == TNLC_MORPH2) f.morph = 2;
    const int gw = 12, gh = 8;
    uint8_t *vd = (uint8_t *)SR_HOST(TNLP_VTX);
    uint32_t stride = 0, nverts = 0;
    for (int y = 0; y <= gh; y++) {
        for (int x = 0; x <= gw; x++) {
            float scale = c->kind == TNLC_FIXED_FMT ? 0.5f : 1.0f;
            float p[3] = { (-1.6f + 3.2f * (float)x / (float)gw) * scale,
                           (-1.1f + 2.2f * (float)y / (float)gh) * scale,
                           tnlp_rand(-0.3f, 0.3f) * scale };
            float n[3] = { tnlp_rand(-0.5f, 0.5f), tnlp_rand(-0.5f, 0.5f), 0.8f };
            if (c->kind == TNLC_LIGHT_SUBNORMAL)
                for (int i = 0; i < 3; i++)   /* every component at least 2^-40, as ge.c requires */
                    n[i] = ldexpf(n[i] < 0.0f ? n[i] - 0.1f : n[i] + 0.1f, -35);
            float w[8];
            float sum = 0.0f;
            for (int i = 0; i < 8; i++) { w[i] = i < skin_w ? tnlp_rand(0.0f, 1.0f) : 0.0f; sum += w[i]; }
            if (skin_w) for (int i = 0; i < skin_w; i++) w[i] = w[i] / sum;
            if (skin_w > 1) w[0] = 0.0f;   /* a zero weight: skipped */
            uint32_t col = 0xff000000u | ((uint32_t)(tnlp_rand(0, 255)) << 16) |
                           ((uint32_t)(tnlp_rand(0, 255)) << 8) | (uint32_t)(tnlp_rand(0, 255));
            if (c->kind == TNLC_RAMP) {
                uint32_t k = (uint32_t)(y * (gw + 1) + x) * 2u;
                col = 0xff000000u | ((k & 255u) << 16) | (((k * 3u) & 255u) << 8) | ((255u - k) & 255u);
            }
            float u = (float)x / (float)gw, v = (float)y / (float)gh;
            uint8_t *at = vd + (size_t)nverts * (stride ? stride : 0);
            stride = 0;
            for (int m = 0; m < f.morph; m++) {
                float pm[3] = { p[0] + 0.2f * (float)m, p[1] - 0.1f * (float)m, p[2] };
                stride += tnlp_vertex(at + stride, &f, w, u, v, col, n, pm);
            }
            nverts++;
        }
    }
    uint32_t vt_idx = 0;
    uint32_t prim = 0, count = 0;
    TNLP_W(&L, 0x10, (TNLP_VTX >> 8) & 0x000F0000u);
    if (c->kind == TNLC_FAN_IDX16 || c->kind == TNLC_LIST_IDX8) {
        /* An index buffer over the grid's vertices. */
        uint8_t *ib = (uint8_t *)SR_HOST(TNLP_IDX);
        uint32_t ni = 0;
        if (c->kind == TNLC_FAN_IDX16) {
            /* A fan around the grid's centre through a ring of vertices. */
            uint16_t centre = (uint16_t)((gh / 2) * (gw + 1) + gw / 2);
            memcpy(ib + 2u * ni++, &centre, 2);
            for (int k = 0; k <= 16; k++) {
                float a = 6.2831853f * (float)k / 16.0f;
                int gx = gw / 2 + (int)lrintf(5.0f * cosf(a)), gy = gh / 2 + (int)lrintf(3.0f * sinf(a));
                uint16_t ix = (uint16_t)(gy * (gw + 1) + gx);
                memcpy(ib + 2u * ni++, &ix, 2);
            }
            vt_idx = 2; prim = 5;
        } else {
            for (int y = 0; y < gh && ni + 6u <= 255u * 3u; y += 2) {
                for (int x = 0; x < gw; x += 2) {
                    uint8_t a = (uint8_t)(y * (gw + 1) + x), b = (uint8_t)(a + 1);
                    uint8_t c2 = (uint8_t)(a + gw + 1), d2 = (uint8_t)(c2 + 1);
                    ib[ni++] = a; ib[ni++] = b; ib[ni++] = c2;
                    ib[ni++] = b; ib[ni++] = d2; ib[ni++] = c2;
                }
            }
            vt_idx = 1; prim = 3;
        }
        count = ni;
        TNLP_W(&L, 0x12, tnlp_vtype(&f, (int)vt_idx));
        TNLP_W(&L, 0x02, TNLP_IDX & 0x00FFFFFFu);
        TNLP_W(&L, 0x01, TNLP_VTX & 0x00FFFFFFu);
        TNLP_W(&L, 0x04, (prim << 16) | count);
    } else if (c->kind == TNLC_STRIP_CULL0 || c->kind == TNLC_STRIP_CULL1 || c->kind == TNLC_FLAT ||
               c->kind == TNLC_MORPH2 || c->kind == TNLC_FIXED_FMT) {
        /* One strip per grid row pair, through indices: vertices (y, x), (y + 1, x), ... */
        uint8_t *ib = (uint8_t *)SR_HOST(TNLP_IDX);
        TNLP_W(&L, 0x12, tnlp_vtype(&f, 2));
        TNLP_W(&L, 0x01, TNLP_VTX & 0x00FFFFFFu);
        for (int y = 0; y < gh; y++) {
            uint32_t at = TNLP_IDX + (uint32_t)y * 0x100u;
            uint16_t *row = (uint16_t *)(ib + (size_t)y * 0x100u);
            for (int x = 0; x <= gw; x++) {
                row[2 * x] = (uint16_t)(y * (gw + 1) + x);
                row[2 * x + 1] = (uint16_t)((y + 1) * (gw + 1) + x);
            }
            TNLP_W(&L, 0x02, at & 0x00FFFFFFu);
            TNLP_W(&L, 0x04, (4u << 16) | (uint32_t)(2 * (gw + 1)));
        }
    } else {
        /* A triangle list over the grid, two triangles per quad, written out vertex by vertex. */
        uint8_t *ld = (uint8_t *)SR_HOST(TNLP_VTX + 0x20000u);
        uint32_t nl = 0;
        for (int y = 0; y < gh; y++) {
            for (int x = 0; x < gw; x++) {
                int q[6] = { y * (gw + 1) + x, y * (gw + 1) + x + 1, (y + 1) * (gw + 1) + x,
                             y * (gw + 1) + x + 1, (y + 1) * (gw + 1) + x + 1, (y + 1) * (gw + 1) + x };
                for (int k = 0; k < 6; k++)
                    memcpy(ld + (size_t)nl++ * stride, vd + (size_t)q[k] * stride, stride);
            }
        }
        TNLP_W(&L, 0x12, tnlp_vtype(&f, 0));
        TNLP_W(&L, 0x10, ((TNLP_VTX + 0x20000u) >> 8) & 0x000F0000u);
        TNLP_W(&L, 0x01, (TNLP_VTX + 0x20000u) & 0x00FFFFFFu);
        TNLP_W(&L, 0x04, (3u << 16) | nl);
    }
    TNLP_W(&L, 0x0F, 0);
    TNLP_W(&L, 0x0C, 0);
    return TNLP_LIST;
}

typedef struct { uint32_t *color; uint16_t *depth; } TnlpOut;

/* Run cell c's list one way (mode 0: software, 1: backend with ge.c's T&L, 2: backend with
 * GPU T&L) and copy the colour and depth buffers out. */
static int tnlp_run(const TnlCell *c, int mode, TnlpOut *out, unsigned long long *taken) {
    const uint32_t fb = 0x04000000u | TNLP_FBA, zb = 0x04000000u | TNLP_ZBA;
    coherence_reset_targets();
    memset(SR_HOST(fb), 0, TNLP_STRIDE * FB_H * 4u);
    memset(SR_HOST(zb), 0, TNLP_STRIDE * FB_H * 2u);
    sr_gpu_vram_dirty(fb, TNLP_STRIDE * FB_H * 4u);
    sr_gpu_vram_dirty(zb, TNLP_STRIDE * FB_H * 2u);
    uint32_t list = tnlp_build(c);
    ge_tnl_set_enabled(mode == 2);
    ge_tnl_counts(NULL, NULL, 1);
    ge_set_gpu_hooks(mode ? &k_hooks : NULL);
    ge_run_list(list, 0);
    ge_set_gpu_hooks(&k_hooks);
    ge_tnl_counts(taken, NULL, 0);
    ge_tnl_set_enabled(-1);
    if (mode) {
        GeGpuFbDescriptor desc = { .addr = fb, .format = 3, .stride = TNLP_STRIDE, .width = 480, .height = FB_H };
        if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) return 0;
        if (s_nbatch && !submit_pending()) return 0;
        /* Only this fixture's depth surface: ge.c has one depth mirror, and reading an earlier
         * fixture's surface back into it would overwrite this one's. */
        for (int i = 0; i < MAX_DEP; i++)
            if (s_deps[i].used && s_deps[i].zba == TNLP_ZBA && !depth_to_cpu(&s_deps[i])) return 0;
    }
    memcpy(out->color, SR_HOST(fb), TNLP_STRIDE * FB_H * 4u);
    /* ge.c's depth surface: the software GE's own, or the backend's depth image read back. */
    const uint16_t *z = ge_zbuf_ptr();
    for (uint32_t y = 0; y < FB_H; y++)
        memcpy(out->depth + (size_t)y * TNLP_STRIDE, z + (size_t)y * ge_zbuf_stride(), TNLP_STRIDE * 2u);
    return 1;
}

static int coherence_run_tnl_parity(void) {
    static const TnlCell cells[] = {
        { "static-list-ztest", TNLC_STATIC, 1 },
        { "strip-cull0", TNLC_STRIP_CULL0, 1 },
        { "strip-cull1", TNLC_STRIP_CULL1, 1 },
        { "fan-index16", TNLC_FAN_IDX16, 1 },
        { "list-index8", TNLC_LIST_IDX8, 1 },
        { "textured-scale-offset", TNLC_TEXTURED, 1 },
        { "fog-linear", TNLC_FOG_LINEAR, 1 },
        { "fog-infinite-end", TNLC_FOG_INF, 1 },
        { "depth-less", TNLC_DEPTH, 1 },
        { "colour-ramp", TNLC_RAMP, 1 },
        { "fixed-formats", TNLC_FIXED_FMT, 1 },
        { "morph-2", TNLC_MORPH2, 1 },
        { "skin-1-float", TNLC_SKIN1, 1 },
        { "skin-2-u8", TNLC_SKIN2_U8, 1 },
        { "skin-4-u16", TNLC_SKIN4_U16, 1 },
        { "skin-8-float", TNLC_SKIN8_F, 1 },
        { "light-directional", TNLC_LIGHT_DIR, 1 },
        { "light-point", TNLC_LIGHT_POINT, 1 },
        { "light-4-matupdate", TNLC_LIGHT4_MATUPD, 1 },
        { "light-skinned", TNLC_LIGHT_SKIN, 1 },
        { "offset-viewport", TNLC_OFFSET_VIEWPORT, 1 },
        { "near-crossing", TNLC_NEAR_CROSS, 0 },
        { "specular", TNLC_SPECULAR, 0 },
        { "flat", TNLC_FLAT, 0 },
        { "texgen", TNLC_TEXGEN, 0 },
        { "light-subnormal-length", TNLC_LIGHT_SUBNORMAL, 1 },
    };
    const size_t px = (size_t)TNLP_STRIDE * FB_H;
    TnlpOut sw = { (uint32_t *)malloc(px * 4u), (uint16_t *)malloc(px * 2u) };
    TnlpOut cpu = { (uint32_t *)malloc(px * 4u), (uint16_t *)malloc(px * 2u) };
    TnlpOut gpu = { (uint32_t *)malloc(px * 4u), (uint16_t *)malloc(px * 2u) };
    int ok = sw.color && sw.depth && cpu.color && cpu.depth && gpu.color && gpu.depth;
    unsigned taken_cells = 0;
    const int buffers_ok = ok;
    for (size_t k = 0; buffers_ok && k < sizeof(cells) / sizeof(cells[0]); k++) {
        const TnlCell *c = &cells[k];
        unsigned long long t_sw = 0, t_cpu = 0, t_gpu = 0;
        if (!tnlp_run(c, 0, &sw, &t_sw) || !tnlp_run(c, 1, &cpu, &t_cpu) ||
            !tnlp_run(c, 2, &gpu, &t_gpu)) {
            fprintf(stderr, "gpu coherence selftest [tnl-parity/%s]: a run failed\n", c->name);
            ok = 0;
            break;
        }
        size_t cdiff = 0, zdiff = 0, first = px, zfirst = px;
        for (size_t i = 0; i < px; i++) {
            if (cpu.color[i] != gpu.color[i]) { cdiff++; if (first == px) first = i; }
            if (cpu.depth[i] != gpu.depth[i]) { zdiff++; if (zfirst == px) zfirst = i; }
        }
        size_t swdiff = 0, drawn = 0;
        int swmax = 0;
        for (size_t i = 0; i < px; i++) {
            if (cpu.color[i] != 0xff302010u) drawn++;
            if (sw.color[i] == cpu.color[i]) continue;
            swdiff++;
            for (int ch = 0; ch < 32; ch += 8) {
                int d = abs((int)((sw.color[i] >> ch) & 255u) - (int)((cpu.color[i] >> ch) & 255u));
                if (d > swmax) swmax = d;
            }
        }
        const int want_gpu = c->gpu && s_vs_tnl != VK_NULL_HANDLE;
        const int route_ok = want_gpu ? t_gpu > 0 : t_gpu == 0;
        if (cdiff || zdiff || !route_ok || !drawn) {
            fprintf(stderr, "gpu coherence selftest [tnl-parity/%s]: FAIL colour_diff=%zu depth_diff=%zu "
                    "gpu_tnl_prims=%llu (want %s) drawn=%zu",
                    c->name, cdiff, zdiff, t_gpu, want_gpu ? ">0" : "0", drawn);
            if (first < px)
                fprintf(stderr, " first=(%zu,%zu) cpu=%08x gpu=%08x", first % TNLP_STRIDE,
                        first / TNLP_STRIDE, (unsigned)cpu.color[first], (unsigned)gpu.color[first]);
            if (zfirst < px)
                fprintf(stderr, " zfirst=(%zu,%zu) cpu=%04x gpu=%04x sw=%04x", zfirst % TNLP_STRIDE,
                        zfirst / TNLP_STRIDE, cpu.depth[zfirst], gpu.depth[zfirst], sw.depth[zfirst]);
            fprintf(stderr, "\n");
            ok = 0;
            continue;
        }
        if (want_gpu) taken_cells++;
        printf("gpu coherence selftest: PASS tnl-parity %-22s gpu_tnl_prims=%llu drawn=%zu "
               "software_diff=%zu max_delta=%d\n", c->name, t_gpu, drawn, swdiff, swmax);
    }
    free(sw.color); free(sw.depth); free(cpu.color); free(cpu.depth); free(gpu.color); free(gpu.depth);
    if (ok)
        printf("gpu coherence selftest: PASS %-22s cells=%zu gpu_tnl_cells=%u%s\n", "tnl-parity",
               sizeof(cells) / sizeof(cells[0]), taken_cells,
               s_vs_tnl ? "" : " (no fp32 subnormal preservation: every cell checked on ge.c's route)");
    return ok;
}

/* The whole device-size image, not only the top-left device pixel of each scale x scale cell
 * that a readback to guest VRAM keeps (write_guest_fb). Colour is left in TRANSFER_SRC, as
 * target_readback leaves it; `after` is the layout the image returns to. */
static int coherence_read_device(VkImage img, VkImageAspectFlags aspect, VkImageLayout *layout,
                                 VkImageLayout after, uint32_t bpp, void *out) {
    if (s_nbatch && !submit_pending()) return 0;
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, img, aspect, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = aspect;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = SCL_W; c.imageExtent.height = SCL_H; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_xfer, 1, &c);
    to_layout(s_cmd, img, aspect, layout, after);
    if (!cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION)) return 0;
    memcpy(out, s_xfer_map, (size_t)SCL_W * SCL_H * bpp);
    return 1;
}

static int coherence_check_device_color(const char *step, const uint32_t *dev, const uint8_t *guest,
                                        uint32_t stride, uint32_t fmt) {
    const uint32_t bpp = fmt == 3u ? 4u : 2u;
    for (uint32_t y = 0; y < SCL_H; y++) {
        for (uint32_t x = 0; x < SCL_W; x++) {
            const uint32_t gx = x / (uint32_t)s_scale, gy = y / (uint32_t)s_scale;
            uint32_t raw = 0, want = 0;
            if (gx < stride) {                     /* columns past the stride upload as 0 */
                memcpy(&raw, guest + (gy * stride + gx) * bpp, bpp);
                want = fb_unpack(raw, fmt);
            }
            if (dev[y * SCL_W + x] != want) {
                fprintf(stderr, "gpu coherence selftest [scaled-upload]: %s fmt=%u stride=%u scale=%d: "
                        "device pixel (%u,%u) = 0x%08x, want 0x%08x\n", step, fmt, stride, s_scale,
                        x, y, dev[y * SCL_W + x], want);
                return 0;
            }
        }
    }
    return 1;
}

/* Every device pixel of each scale x scale cell holds its guest pixel after a whole-target
 * upload, after a CPU-write patch across a row boundary, and after a depth upload (#1296):
 * a readback to guest VRAM keeps only one device pixel per cell, so a row or column of a
 * cell left unwritten would not show there. Every format runs at a full and at a short
 * stride, whose columns past the stride upload as 0. At scale 1 the cells are single pixels;
 * gpu-coherence-selftest also runs this case alone at render scales 2 and 4. */
static int coherence_run_scaled_upload(void) {
    static const struct { uint32_t fmt, stride; } cells[] = {
        { 3u, 512u }, { 0u, 512u }, { 1u, 512u }, { 2u, 512u },
        { 3u, 480u }, { 0u, 480u }, { 1u, 448u }, { 2u, 496u },
    };
    const uint32_t fba = 0x00100000u, base = 0x04000000u | fba;
    uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
    if (!dev) return 0;
    int ok = 1;
    for (uint32_t c = 0; ok && c < sizeof(cells) / sizeof(cells[0]); c++) {
        const uint32_t fmt = cells[c].fmt, stride = cells[c].stride, bpp = fmt == 3u ? 4u : 2u;
        coherence_reset_targets();
        uint8_t *guest = (uint8_t *)SR_HOST(base);
        for (uint32_t i = 0; i < stride * FB_H; i++) {
            const uint32_t raw = (i + 1u) * 0x9e3779b1u ^ (fmt << 24);
            memcpy(guest + i * bpp, &raw, bpp);
        }
        const char *fail = NULL;           /* "" once the check has printed its own line */
        Target *t = target_color_acquire(fba, stride, fmt, 1);
        if (!t || !t->gpu_valid)
            fail = "the target was not established";
        else if (!coherence_read_device(t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 4u, dev))
            fail = "device read after the upload failed";
        else if (!coherence_check_device_color("whole-target upload", dev, guest, stride, fmt))
            fail = "";

        /* A CPU write from mid row 60 into row 61, held and applied as a patch. */
        if (!fail) {
            const uint32_t off = (60u * stride + 37u) * bpp, len = (stride + 50u) * bpp;
            for (uint32_t i = 0; i < len; i++) guest[off + i] = (uint8_t)(0x5au + i * 13u);
            const unsigned long long syncs0 = s_replay_stats.cpu_write_syncs;
            const unsigned long uploads0 = s_cnt_upload;
            sr_gpu_vram_dirty(base + off, len);
            const int held = t->cpu_n;
            t = target_color_acquire(fba, stride, fmt, 1);
            if (!t || !t->gpu_valid)
                fail = "the target was lost after the CPU write";
            else if (!held || s_replay_stats.cpu_write_syncs != syncs0 + 1u ||
                     s_cnt_upload != uploads0 + 1u) {   /* one held span, one patch */
                fprintf(stderr, "gpu coherence selftest [scaled-upload]: held=%d syncs=%llu uploads=%lu\n",
                        held, s_replay_stats.cpu_write_syncs - syncs0, s_cnt_upload - uploads0);
                fail = "the CPU write was not applied as a patch";
            }
            else if (!coherence_read_device(t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 4u, dev))
                fail = "device read after the patch failed";
            else if (!coherence_check_device_color("CPU-write patch", dev, guest, stride, fmt))
                fail = "";
        }
        if (fail) {
            if (*fail)
                fprintf(stderr, "gpu coherence selftest [scaled-upload]: fmt=%u stride=%u scale=%d: %s\n",
                        fmt, stride, s_scale, fail);
            ok = 0;
        }
    }

    /* Depth: ge.c's CPU surface, uploaded at device size. */
    if (ok) {
        coherence_reset_targets();
        memset(s_ge, 0, sizeof(*s_ge));
        s_ge->fbp = base; s_ge->fbw = 512u;
        s_ge->zbp = 0x04080000u; s_ge->zbw = 512u;
        DepthEnt *d = NULL;
        uint16_t *dev16 = (uint16_t *)dev;
        ok = ge_zbuf_prepare_current() && (s_zbuf = ge_zbuf_ptr()) != NULL;
        if (ok) {
            for (uint32_t y = 0; y < FB_H; y++)
                for (uint32_t x = 0; x < FB_W; x++)
                    s_zbuf[y * 512u + x] = (uint16_t)(0x1234u + y * 977u + x * 31u);
            ge_zbuf_note_external_write();
            d = depth_acquire(0x00080000u, 512u);
        }
        if (d) d->cpu_dirty = 1;
        ok = d && depth_from_cpu(d) &&
             coherence_read_device(d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 2u, dev16);
        for (uint32_t y = 0; ok && y < SCL_H; y++) {
            for (uint32_t x = 0; x < SCL_W; x++) {
                const uint16_t want = s_zbuf[(y / (uint32_t)s_scale) * 512u + x / (uint32_t)s_scale];
                if (dev16[y * SCL_W + x] != want) {
                    fprintf(stderr, "gpu coherence selftest [scaled-upload]: depth upload scale=%d: "
                            "device pixel (%u,%u) = 0x%04x, want 0x%04x\n", s_scale, x, y,
                            dev16[y * SCL_W + x], want);
                    ok = 0;
                    break;
                }
            }
        }
        if (!ok && !d) fprintf(stderr, "gpu coherence selftest [scaled-upload]: no depth surface\n");
    }
    free(dev);
    coherence_reset_targets();
    if (ok)
        printf("gpu coherence selftest: PASS %-22s cells=%u scale=%d\n", "scaled-upload",
               (unsigned)(sizeof(cells) / sizeof(cells[0])), s_scale);
    return ok;
}

int gegpu_coherence_selftest(void) {
    /* SR_GPU_COHERENCE_CASE=scaled-upload (the harness's --case) runs that case alone, as the
     * recipe does at render scales 2 and 4: the other fixtures expect 1x pixels (a filtered sample
     * lands on other device pixels at a higher scale). Any other value fails. */
    const char *only = getenv("SR_GPU_COHERENCE_CASE");
    if (only && *only) {
        if (strcmp(only, "scaled-upload") == 0) return coherence_run_scaled_upload();
        fprintf(stderr, "gpu coherence selftest: unknown SR_GPU_COHERENCE_CASE '%s'\n", only);
        return 0;
    }
    static const CoherenceCase cases[] = {
        { "8888-middle", 3, 512, (91u * 512u + 137u) * 4u, 4, 0, 0, 0 },
        { "5650-unaligned", 0, 512, (42u * 512u + 61u) * 2u + 1u, 3, 0, 0, 0 },
        { "5551-unaligned", 1, 512, (43u * 512u + 62u) * 2u + 1u, 3, 0, 0, 0 },
        { "4444-unaligned", 2, 512, (44u * 512u + 63u) * 2u + 1u, 3, 0, 0, 0 },
        { "8888-unaligned", 3, 512, (45u * 512u + 64u) * 4u + 1u, 5, 0, 0, 0 },
        { "row-start", 3, 512, 10u * 512u * 4u, 4, 0, 0, 0 },
        { "row-end", 3, 512, (11u * 512u + 511u) * 4u, 4, 0, 0, 0 },
        { "row-crossing", 3, 512, (12u * 512u + 511u) * 4u + 2u, 6, 0, 0, 0 },
        { "multi-row", 3, 512, (20u * 512u + 500u) * 4u, (12u + 512u + 17u) * 4u, 0, 0, 0 },
        { "stride-500", 3, 500, (30u * 500u + 499u) * 4u + 1u, 11, 0, 0, 0 },
        { "pending-batch", 3, 512, (70u * 512u + 90u) * 4u, 8, 1, 0, 0 },
        { "pending-async", 3, 512, (71u * 512u + 91u) * 4u, 8, 0, 1, 0 },
        { "alias-vram", 3, 512, (72u * 512u + 92u) * 4u, 8, 0, 0, 1 },
    };
    int ok = 1;
    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (!coherence_run_case(&cases[i])) ok = 0;
    if (!coherence_run_clean_case("clean-aligned", 3, 512, (50u * 512u + 20u) * 4u, 16)) ok = 0;
    if (!coherence_run_clean_case("clean-unaligned", 3, 512, (51u * 512u + 21u) * 4u + 1u, 5)) ok = 0;
    if (!coherence_run_full_cover_case()) ok = 0;
    if (!coherence_run_cpu_write_coalesce()) ok = 0;
    if (!coherence_run_overlap_case()) ok = 0;
    if (!coherence_run_tflt_descriptor_capacity()) ok = 0;
    if (!coherence_run_sprite_semantics()) ok = 0;
    if (!coherence_run_software_sprite_semantics()) ok = 0;
    if (!coherence_run_linear_sprite()) ok = 0;
    if (!coherence_run_tflt_filters()) ok = 0;
    if (!coherence_run_ring_wrap()) ok = 0;
    if (!coherence_run_submit_batch()) ok = 0;
    if (!coherence_run_upload_sequence()) ok = 0;
    if (!coherence_run_texture_dirty_same_state()) ok = 0;
    if (!coherence_run_texture_clean_page_index()) ok = 0;
    if (!coherence_run_texture_ram_unnotified_change()) ok = 0;
    if (!coherence_run_clut_format_same_palette()) ok = 0;
    if (!coherence_run_clut_format_list()) ok = 0;
    if (!coherence_run_blend_parity()) ok = 0;
    if (!coherence_run_scalar_vram_read(0x04080000u)) ok = 0;
    /* zbp 0x0a2000: bit 13 set, so the absolute XOR differs from the within-buffer form (0x0a6854 vs
     * 0x0a2854 for pixel 10,10). The 0x0ce000 placement used by the software selftest would overlap the
     * colour surface at 0x100000 here. */
    if (!coherence_run_scalar_vram_read(0x040A2000u)) ok = 0;
    if (!coherence_run_native_bulk_copy()) ok = 0;
    if (!coherence_run_fallback_reasons()) ok = 0;
    /* Last: its chain-fallback case draws through the software fallback, which hands the
     * current depth surface to ge.c, and the fixtures above start from the depth state the
     * ones before them leave. */
    if (!coherence_run_mip_levels()) ok = 0;
    if (!coherence_run_psp_sample_points()) ok = 0;
    /* After the mip fixtures: it rewrites the depth buffer they start from. */
    if (!coherence_run_stencil_parity()) ok = 0;
    if (!coherence_run_tnl_parity()) ok = 0;
    /* Last: it rewrites ge.c's depth surface. */
    if (!coherence_run_scaled_upload()) ok = 0;
    coherence_reset_targets();
    return ok;
}

/* Source-owned regression for the explicit snapshot boundary. It first proves that
 * ordinary presentation remains asynchronous (the pre-fix lag), then asks the same
 * target through gegpu_sync_guest_fb() and verifies that guest VRAM is current before
 * publication. Descriptor rejection and no-target answers are checked too. */
/* Ace Mode's texture filtering (ace.h), run with SR_ACE asking for 16x on a device that has it
 * (gpu-ace-selftest). Through-mode draws keep the guest's sampling exactly: the PSP-measured
 * sample points still hold and no anisotropic sampler is made for them. A transformed draw that
 * minifies linearly gets one anisotropic sampler, made once and reused; a nearest-filtered
 * texture, and the same entry drawn in through mode, keep the plain set. */
int gegpu_ace_af_selftest(void) {
    if (!s_ace_af) {
        fprintf(stderr, "gpu ace selftest: SKIP (this device has no anisotropic filtering)\n");
        return 77;
    }
    if (s_tex_pool_sets != 2u * MAX_TEX) {
        fprintf(stderr, "gpu ace selftest: FAIL texture pool has %u sets, expected %u\n",
                s_tex_pool_sets, 2u * MAX_TEX);
        return 0;
    }
    if (!coherence_run_psp_sample_points()) return 0;
    if (s_cnt_af_samplers != 0u) {
        fprintf(stderr, "gpu ace selftest: FAIL through-mode draws made %llu anisotropic samplers\n",
                (unsigned long long)s_cnt_af_samplers);
        return 0;
    }
    /* The list left the 32x32 texture bound. Draw it transformed, then nearest, then through. */
    GeState *g = s_ge;
    const uint32_t through = 1u << 23;
    g->vtype &= ~through;
    g->tex_filter = 0x101u;                                  /* linear min and mag */
    VkDescriptorSet aniso = tex_get();
    VkDescriptorSet again = tex_get();
    int ok = s_cnt_af_samplers == 1u && aniso == again && aniso != s_white_set;
    if (!ok) fprintf(stderr, "gpu ace selftest: FAIL transformed linear draw: samplers=%llu same=%d\n",
                     (unsigned long long)s_cnt_af_samplers, aniso == again);
    g->tex_filter = 0x100u;                                  /* nearest min, linear mag */
    VkDescriptorSet nearest = tex_get();
    if (ok && (s_cnt_af_samplers != 1u || nearest == aniso)) {
        fprintf(stderr, "gpu ace selftest: FAIL nearest minification got anisotropy\n");
        ok = 0;
    }
    g->tex_filter = 0x101u;
    g->vtype |= through;
    VkDescriptorSet plain = tex_get();
    if (ok && (s_cnt_af_samplers != 1u || plain == aniso)) {
        fprintf(stderr, "gpu ace selftest: FAIL a through-mode draw bound the anisotropic set\n");
        ok = 0;
    }
    if (ok) printf("gpu ace selftest: PASS af=%d through-mode exact, transformed linear filtered, "
                   "nearest kept\n", s_ace_af);
    return ok;
}

int gegpu_snapshot_sync_selftest(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u, fmt = 3u;
    const uint32_t bytes = stride * FB_H * 4u;
    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    coherence_fill_raw(guest, stride * FB_H, 4u, coherence_g1(fmt));
    Target *t = target_color_acquire(fba, stride, fmt, 1);
    if (!t || !t->gpu_valid) {
        fprintf(stderr, "gpu snapshot sync selftest: could not establish GPU target\n");
        coherence_reset_targets();
        return 0;
    }

    /* The image remains GPU G1, but guest VRAM is deliberately restored to stale G0. */
    coherence_fill_raw(guest, stride * FB_H, 4u, coherence_g0(fmt));
    t->clean_gen = 0;
    if (!target_prepare_present(t)) {
        fprintf(stderr, "gpu snapshot sync selftest: present readback setup failed\n");
        coherence_reset_targets();
        return 0;
    }

    ReadbackSlot *pending = NULL;
    for (int i = 0; i < READBACK_FRAMES; i++) {
        if (s_readback[i].pending && s_readback[i].image == t->img) {
            pending = &s_readback[i];
            break;
        }
    }
    int stale = pending && pending->commit && t->clean_gen != t->render_gen &&
                memcmp(guest, &(uint32_t){ coherence_g0(fmt) }, sizeof(uint32_t)) == 0;
    if (!stale) {
        fprintf(stderr, "gpu snapshot sync selftest: no stale pre-commit snapshot observed "
                        "(render_gen=%llu clean_gen=%llu pending=%d)\n",
                (unsigned long long)t->render_gen, (unsigned long long)t->clean_gen,
                pending != NULL);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: ordinary_present_lag render_gen=%llu "
           "clean_gen=%llu pending=1 bytes=%u\n",
           (unsigned long long)t->render_gen, (unsigned long long)t->clean_gen, bytes);

    GeGpuFbDescriptor d = { base, fmt, stride, 480u, FB_H };
    int rc = gegpu_sync_guest_fb(&d);
    if (rc != GEGPU_SYNC_OK || t->clean_gen != t->render_gen ||
        memcmp(guest, &(uint32_t){ coherence_g1(fmt) }, sizeof(uint32_t)) != 0) {
        fprintf(stderr, "gpu snapshot sync selftest: explicit sync failed rc=%d "
                        "clean_gen=%llu render_gen=%llu\n", rc,
                (unsigned long long)t->clean_gen, (unsigned long long)t->render_gen);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: explicit_sync_ok render_gen=%llu\n",
           (unsigned long long)t->render_gen);

    GeGpuFbDescriptor bad = d;
    bad.width = 0;
    if (gegpu_sync_guest_fb(&bad) != GEGPU_SYNC_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: invalid descriptor was accepted\n");
        coherence_reset_targets();
        return 0;
    }
    GeGpuFbDescriptor cross = d;
    cross.addr = 0x041f0000u;
    if (gegpu_sync_guest_fb(&cross) != GEGPU_SYNC_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: cross-aperture descriptor was accepted\n");
        coherence_reset_targets();
        return 0;
    }

    /* A caller that agrees on the address but disagrees on how the bytes are laid
     * out would misread the buffer it just synchronised, so the boundary refuses.
     * These two controls keep that refusal honest.  A real observer defect once
     * produced exactly this shape -- live target 5551, caller describing 8888 --
     * and the refusal is what surfaced it.  Any change that makes an observer
     * quieter must leave these failing closed. */
    GeGpuFbDescriptor wrong_fmt = d;
    wrong_fmt.format = fmt == 3u ? 1u : 3u;
    if (gegpu_sync_guest_fb(&wrong_fmt) != GEGPU_SYNC_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: pixel-format mismatch against a live "
                        "target was accepted (target fmt=%u, caller fmt=%u)\n",
                fmt, wrong_fmt.format);
        coherence_reset_targets();
        return 0;
    }
    GeGpuFbDescriptor wrong_stride = d;
    wrong_stride.stride = stride + 64u;
    if (gegpu_sync_guest_fb(&wrong_stride) != GEGPU_SYNC_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: stride mismatch against a live "
                        "target was accepted (target stride=%u, caller stride=%u)\n",
                stride, wrong_stride.stride);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: live_target_mismatch_refused fmt and stride\n");
    GeGpuFbDescriptor no_target = d;
    no_target.addr = 0x04010000u;
    if (gegpu_sync_guest_fb(&no_target) != GEGPU_SYNC_NO_TARGET) {
        fprintf(stderr, "gpu snapshot sync selftest: unmapped VRAM was not NO_TARGET\n");
        coherence_reset_targets();
        return 0;
    }
    GeGpuFbDescriptor ram = d;
    ram.addr = 0x08010000u;
    if (gegpu_sync_guest_fb(&ram) != GEGPU_SYNC_NO_TARGET) {
        fprintf(stderr, "gpu snapshot sync selftest: main RAM was not NO_TARGET\n");
        coherence_reset_targets();
        return 0;
    }
    /* Declines that involve no GPU-valid target keep -1, so gui_present does not sync guest
     * VRAM for them. Only a GPU-valid target that fails to prepare or present reports
     * GEGPU_PRESENT_TARGET_FAILED (issue #827). */
    if (gegpu_present(0x08010000u, (int)fmt, stride) != GEGPU_PRESENT_NOT_RESIDENT ||
        gegpu_present(base, (int)fmt, FB_W + 64u) != GEGPU_PRESENT_NOT_RESIDENT) {
        fprintf(stderr, "gpu snapshot sync selftest: a present decline without a GPU target was not -1\n");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: present_declines_without_target_keep_code\n");

    if (!coherence_queue_g1_batch(base, stride, fmt) || !submit_pending() ||
        !target_prepare_present(t) || t->clean_gen == t->render_gen) {
        fprintf(stderr, "gpu snapshot sync selftest: ordinary second present lost async lag\n");
        coherence_reset_targets();
        return 0;
    }
    if (gegpu_sync_guest_fb(&d) != GEGPU_SYNC_OK || t->clean_gen != t->render_gen) {
        fprintf(stderr, "gpu snapshot sync selftest: second explicit sync failed\n");
        coherence_reset_targets();
        return 0;
    }

    /* Present-side failure (issue #827): the target is GPU-valid and newer than guest VRAM.
     * The sdl3vk fault seam (selftest only) makes vkQueuePresentKHR report OUT_OF_DATE, which
     * present_common resolves as -1 after the target was prepared. gegpu_present() must report
     * GEGPU_PRESENT_TARGET_FAILED, and the sync gui_present runs before its fall-through must
     * make guest VRAM current with the newest generation. */
    coherence_fill_raw(guest, stride * FB_H, 4u, coherence_g0(fmt));
    if (!coherence_queue_g1_batch(base, stride, fmt) || !submit_pending() ||
        t->clean_gen == t->render_gen) {
        fprintf(stderr, "gpu snapshot sync selftest: could not stage a newer GPU target\n");
        coherence_reset_targets();
        return 0;
    }
    sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_DATE_KHR);
    int failed_present = gegpu_present(base, (int)fmt, stride);
    sdl3vk_present_fault_clear();
    if (failed_present != GEGPU_PRESENT_TARGET_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: failed present of a prepared target returned %d, "
                        "not GEGPU_PRESENT_TARGET_FAILED\n", failed_present);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: present_target_failure_reports_target_failed\n");
    if (gegpu_sync_guest_fb(&d) != GEGPU_SYNC_OK || t->clean_gen != t->render_gen ||
        memcmp(guest, &(uint32_t){ coherence_g1(fmt) }, sizeof(uint32_t)) != 0) {
        fprintf(stderr, "gpu snapshot sync selftest: guest VRAM was not current after a target failure\n");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: present_target_failure_synced_guest_vram\n");

    /* Controls: a present that is shown, and one that is shown with an enqueued swapchain
     * rebuild (VK_SUBOPTIMAL_KHR), both still report 1. Only -1 is remapped. */
    sdl3vk_present_fault_inject(VK_SUBOPTIMAL_KHR);
    int rebuilt_present = gegpu_present(base, (int)fmt, stride);
    sdl3vk_present_fault_clear();
    if (rebuilt_present != GEGPU_PRESENT_SHOWN ||
        gegpu_present(base, (int)fmt, stride) != GEGPU_PRESENT_SHOWN) {
        fprintf(stderr, "gpu snapshot sync selftest: a shown present did not stay 1 (rebuild=%d)\n",
                rebuilt_present);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: present_shown_stays_1\n");
    coherence_reset_targets();
    return 1;
}
