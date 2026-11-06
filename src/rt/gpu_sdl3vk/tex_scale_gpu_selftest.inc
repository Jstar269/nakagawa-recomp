/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * tex_scale_gpu_selftest.inc - texture scaling and the smart 2D filter on ge_gpu.c's production
 * path (included by ge_gpu.c in the SR_GPU_TEXSCALE_SELFTEST build; mk/selftests.mk
 * gpu-texscale-selftest). Synthetic textures only. The cells:
 *   scaled-draw      a 1:1 sprite draws the xBRZ copy, sampled on the copy's own lattice;
 *   large            a 512x512 texture at 4x (16 MiB) uploads in bands and draws the copy;
 *   not-ready        a texture whose scale has not finished draws unscaled, without waiting;
 *   rewrite          a texture the guest rewrites draws unscaled until its own copy lands, then
 *                    the new copy; never the old one (the build with
 *                    NK_TEX_SCALE_MUTANT_SKIP_CONTENT_CHECK binds any copy and must fail here,
 *                    for that reason);
 *   clut             a CLUT texture is scaled after the palette lookup and again for a new
 *                    palette;
 *   budget           with room for two copies, a third texture stays unscaled while both are
 *                    in use; once they are idle it frees them and its copy lands, and a freed
 *                    texture's copy comes back on a later draw;
 *   rtt              a render target sampled as a texture, bound directly or read back through
 *                    the cache, is never scaled, and its draws are byte-identical with
 *                    scaling on and off;
 *   onetoone         a 1:1 draw binds the unscaled texture even with a copy ready: without that
 *                    rule a cell of an atlas shows its neighbour at its edge (device pixels at a
 *                    render scale above 1), with it the draw is the unscaled one; a magnified
 *                    draw keeps the copy, identical with the rule on and off. The cells above
 *                    check copies through 1:1 sprites, so they run with the rule off;
 *   guest-bytes      after every cell, no guest VRAM byte outside the drawn frame and no CLUT
 *                    byte changes while the scales land;
 *   smart2d          at a render scale above 1, a 1:1 linear sprite samples nearest (every
 *                    device pixel is a whole texel), linear blurs without the filter, and a
 *                    perspective triangle keeps its filter.
 */

#define XST_BASE   (0x04000000u | 0x00160000u)
#define XST_RTT    (0x04000000u | 0x000C8000u)
#define XST_STRIDE 512u
#define XST_TEX    0x04040000u
#define XST_W      16
#define XST_H      16
#define XST_RGB    0x00FFFFFFu

static uint32_t xst_hash(uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
    return v;
}

/* A texture xBRZ changes: a field, two diagonals, a block and lone texels, in colours that
 * depend on `seed`. */
static void xst_pattern(uint32_t *t, uint32_t seed) {
    const uint32_t field = 0xFF000000u | (xst_hash(seed) & 0x7F7F7Fu);
    const uint32_t ink = 0xFF000000u | ((xst_hash(seed + 1u) & 0x7F7F7Fu) + 0x808080u);
    const uint32_t spot = 0xFF000000u | (xst_hash(seed + 2u) & 0xFFFFFFu);
    for (int y = 0; y < XST_H; y++)
        for (int x = 0; x < XST_W; x++) {
            uint32_t c = field;
            if (x == y || x == 2 * y - 6) c = ink;
            if (x >= 10 && x < 14 && y >= 2 && y < 6) c = ink;
            if ((x * 5 + y * 3) % 23 == 0) c = spot;
            t[y * XST_W + x] = c;
        }
}

static void xst_upload(const uint32_t *t) {
    memcpy(SR_HOST(XST_TEX), t, sizeof(uint32_t) * XST_W * XST_H);
    sr_gpu_vram_dirty(XST_TEX, sizeof(uint32_t) * XST_W * XST_H);
}

/* GE state for a 16x16 8888 VRAM texture drawn by REPLACE into the 8888 target at `base`. */
static void xst_state(uint32_t base, uint32_t tex_addr, uint32_t fmt, int linear, int clamp) {
    wrap_state(base, XST_STRIDE);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_addr;
    s_ge->tex_bufw = XST_W;
    s_ge->tex_fmt = fmt;
    s_ge->tex_w = XST_W;
    s_ge->tex_h = XST_H;
    s_ge->tex_func = 3;                      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = linear ? (1u | 0x100u) : 0u;
    s_ge->tex_wrap = clamp ? 0x101u : 0u;
    s_ge->vtype = 1u << 23;                  /* through mode */
    ge_state_note_external_write();
}

/* A 1:1 sprite of the whole texture at guest (x, y), read back to guest VRAM. */
static int xst_draw(float x, float y) {
    GeVtx p0, p1;
    wrap_vtx(&p0, x, y, 0xFFFFFFFFu); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, x + XST_W, y + XST_H, 0xFFFFFFFFu); p1.u = (float)XST_W; p1.v = (float)XST_H;
    return hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
}

/* The coordinate device pixel k samples on an axis of a 1:1 through-mode sprite that spans
 * coordinates c0 .. c0 + n: its own centre, c0 + (k + 0.5) / scale (hook_sprite, #2104), clamped
 * above 1x to the first and last guest pixel centres (uv_edge_axis). At 1x it is the pixel centre
 * c0 + k + 0.5, so every value below is the one the PSP samples there. */
static float xst_axis_u(float c0, int k, int n) {
    float u = ((float)k + 0.5f) / (float)s_scale;
    if (s_scale > 1) {
        if (u < 0.5f) u = 0.5f;
        if (u > (float)n - 0.5f) u = (float)n - 0.5f;
    }
    return c0 + u;
}

/* The texel of a factor-`f` copy (f = 1: the texture) that guest pixel i reads back: the one its
 * top-left device sample, device pixel i * scale, falls in. */
static size_t xst_readback_texel(float c0, int i, int n, int f) {
    return (size_t)floorf((float)f * xst_axis_u(c0, i * s_scale, n));
}

/* What a 1:1 nearest sprite of texture `t` reads back from a factor-`f` copy (f = 0: the
 * texture itself). */
static void xst_expect(const uint32_t *t, int f, int clamp, uint32_t *want) {
    if (f < 2) { memcpy(want, t, sizeof(uint32_t) * XST_W * XST_H); return; }
    uint32_t *big = (uint32_t *)malloc(sizeof(uint32_t) * XST_W * XST_H * (size_t)f * (size_t)f);
    if (!big || !nk_tex_scale_image(t, XST_W, XST_H, f, clamp, clamp, big)) {
        memset(want, 0, sizeof(uint32_t) * XST_W * XST_H);
        free(big);
        return;
    }
    for (int y = 0; y < XST_H; y++)
        for (int x = 0; x < XST_W; x++)
            want[y * XST_W + x] = big[xst_readback_texel(0.0f, y, XST_H, f) * XST_W * (size_t)f +
                                     xst_readback_texel(0.0f, x, XST_W, f)];
    free(big);
}

static int xst_matches(int x0, int y0, const uint32_t *want) {
    const uint32_t *fb = (const uint32_t *)SR_HOST(XST_BASE);
    for (int y = 0; y < XST_H; y++)
        for (int x = 0; x < XST_W; x++)
            if ((fb[(y0 + y) * XST_STRIDE + x0 + x] & XST_RGB) != (want[y * XST_W + x] & XST_RGB)) return 0;
    return 1;
}

static int xst_first_diff(int x0, int y0, const uint32_t *want, int *dx, int *dy, uint32_t *got) {
    const uint32_t *fb = (const uint32_t *)SR_HOST(XST_BASE);
    for (int y = 0; y < XST_H; y++)
        for (int x = 0; x < XST_W; x++) {
            uint32_t g = fb[(y0 + y) * XST_STRIDE + x0 + x] & XST_RGB;
            if (g != (want[y * XST_W + x] & XST_RGB)) { *dx = x; *dy = y; *got = g; return 1; }
        }
    return 0;
}

static int xst_report(const char *cell, int x0, int y0, const uint32_t *want, const char *what) {
    int dx = 0, dy = 0;
    uint32_t got = 0;
    if (!xst_first_diff(x0, y0, want, &dx, &dy, &got)) return 1;
    fprintf(stderr, "gpu texscale selftest: FAIL %s: %s (texel %d,%d shows %06x, want %06x)\n",
            cell, what, dx, dy, got, want[dy * XST_W + dx] & XST_RGB);
    return 0;
}

static void xst_clear_fb(void) {
    memset(SR_HOST(XST_BASE), 0, XST_STRIDE * FB_H * 4u);
    sr_gpu_vram_dirty(XST_BASE, XST_STRIDE * FB_H * 4u);
}

/* The scaled copies and their jobs, from a clean cache. */
static void xst_reset(void) {
    coherence_reset_targets();
    (void)submit_pending();
    (void)cmd_drain(GEGPU_BOUNDARY_LIFETIME);
    while (s_tex_n) {                       /* the whole cache, as tex_evict_lru frees an entry */
        TexEnt *e = &s_tex[--s_tex_n];
        tex_shadow_release(e);
        vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &e->set);
        vkDestroySampler(s_dev, e->smp, NULL);
        if (e->set_af) vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &e->set_af);
        if (e->smp_af) vkDestroySampler(s_dev, e->smp_af, NULL);
        vkDestroyImageView(s_dev, e->view, NULL);
        vkDestroyImage(s_dev, e->img, NULL);
        vkFreeMemory(s_dev, e->mem, NULL);
        tex_xs_free(e);
    }
    nk_tex_scale_set_hold(0);
    nk_tex_scale_wait_idle();
    NkTexScaleResult r;
    while (nk_tex_scale_take(&r)) nk_tex_scale_release(&r);
    xst_clear_fb();
    s_cnt_present_cpu++;   /* a new present: the per-present upload bytes start again */
}

/* Read a target's whole device-resolution image (RGBA8, R in the low byte). */
static int xst_read_device(Target *t, uint32_t *out) {
    if (s_nbatch && !submit_pending()) return 0;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *map = NULL;
    const VkDeviceSize bytes = (VkDeviceSize)SCL_W * SCL_H * 4u;
    if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf, &mem, &map)) return 0;
    int ok = cmd_begin();
    if (ok) {
        to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c;
        memset(&c, 0, sizeof(c));
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = SCL_W; c.imageExtent.height = SCL_H; c.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(s_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        ok = cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION);
    }
    if (ok) memcpy(out, map, (size_t)bytes);
    vkDestroyBuffer(s_dev, buf, NULL);
    vkFreeMemory(s_dev, mem, NULL);
    return ok;
}

/* Device pixels of the 1:1 sprite drawn at guest (x0, y0) that are not the nearest texel of
 * `t`, an image of the texture at `f` times its size (f = 1: the texture itself). Device pixel k
 * samples coordinate xst_axis_u(0, k, n), which is texel floor(f * u) of the copy. */
static int xst_device_blends(const uint32_t *dev, int x0, int y0, const uint32_t *t, int f) {
    const int s = s_scale, tw = XST_W * f, th = XST_H * f;
    int blends = 0;
    for (int ky = 0; ky < XST_H * s; ky++)
        for (int kx = 0; kx < XST_W * s; kx++) {
            int tu = (int)floorf((float)f * xst_axis_u(0.0f, kx, XST_W));
            int tv = (int)floorf((float)f * xst_axis_u(0.0f, ky, XST_H));
            if (tu > tw - 1) tu = tw - 1;
            if (tv > th - 1) tv = th - 1;
            uint32_t got = dev[((size_t)y0 * (size_t)s + (size_t)ky) * SCL_W + (size_t)x0 * (size_t)s + (size_t)kx] & XST_RGB;
            if (got != (t[(size_t)tv * tw + tu] & XST_RGB)) blends++;
        }
    return blends;
}

static int xst_cell_scaled_draw(int f) {
    uint32_t tex[XST_W * XST_H], want[XST_W * XST_H], plain[XST_W * XST_H];
    xst_reset();
    nk_tex_scale_set_sync(1);
    xst_pattern(tex, 11u);
    xst_upload(tex);
    xst_expect(tex, f, 1, want);
    xst_expect(tex, 0, 1, plain);
    /* At 1x the guest pixel samples the copy at its texel centre's lattice point (f*i + f/2). From 3x
     * xBRZ often keeps that point, so only the device image at a render scale above 1 can tell
     * the copy from the texture. */
    const int guest_tells = memcmp(want, plain, sizeof(want)) != 0;
    if (!guest_tells && s_scale < 2) {
        fprintf(stderr, "gpu texscale selftest: FAIL scaled-draw: at factor %d and render scale 1 the copy and "
                        "the texture draw alike (vacuous); run this factor at a render scale above 1\n", f);
        return 0;
    }
    xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
    /* The first draw decodes and (sync) scales; the copy is stored at the next lookup. */
    const uint64_t bound0 = s_cnt_xs_bound;
    int ok = xst_draw(20.0f, 20.0f);
    s_tmpl_ok = 0;   /* force a fresh state build, as the next draw of a new list would */
    ok = ok && xst_draw(60.0f, 20.0f);
    if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL scaled-draw: draw or readback failed\n"); return 0; }
    if (!xst_report("scaled-draw", 20, 20, plain, "the first draw, before the copy was stored, is not the texture"))
        return 0;
    if (!xst_report("scaled-draw", 60, 20, want, "the second draw is not the xBRZ copy on its own lattice"))
        return 0;
    if (s_cnt_xs_bound == bound0) {
        fprintf(stderr, "gpu texscale selftest: FAIL scaled-draw: no scaled bind was counted\n");
        return 0;
    }
    int device_diff = -1;
    if (s_scale > 1) {
        uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
        uint32_t *big = (uint32_t *)malloc(sizeof(uint32_t) * XST_W * XST_H * (size_t)f * (size_t)f);
        ok = dev && big && s_cur && xst_read_device(s_cur, dev) && nk_tex_scale_image(tex, XST_W, XST_H, f, 1, 1, big);
        const int first_plain = ok ? xst_device_blends(dev, 20, 20, tex, 1) : -1;
        const int second_copy = ok ? xst_device_blends(dev, 60, 20, big, f) : -1;
        device_diff = ok ? xst_device_blends(dev, 60, 20, tex, 1) : -1;
        free(dev);
        free(big);
        if (!ok || first_plain != 0 || second_copy != 0 || device_diff == 0) {
            fprintf(stderr, "gpu texscale selftest: FAIL scaled-draw: device pixels off the expected texel: %d in the "
                            "unscaled draw, %d in the scaled draw; %d differ from the texture (want 0, 0, some)\n",
                    first_plain, second_copy, device_diff);
            return 0;
        }
    }
    printf("gpu texscale selftest: PASS %-12s factor=%d render-scale=%d device-pixels-from-copy=%d\n", "scaled-draw",
           f, s_scale, device_diff);
    return 1;
}

/* The largest copy: a 512x512 texture at factor 4 is 2048x2048, 16 MiB, more than the upload ring
 * slot and the shared staging buffer hold (a title's 512x512 font page crashed the runtime here
 * before the copy went up in bands). Main RAM, so no target or depth buffer can overlap it: the
 * kernel partition, which the self-test owns and no game address names. */
#define XST_BIG_ADDR 0x08400000u
#define XST_BIG      512
static int xst_cell_large(int f) {
    (void)f;
    const int factor = 4;
    uint32_t *tex = (uint32_t *)SR_HOST(XST_BIG_ADDR);
    for (int y = 0; y < XST_BIG; y++)
        for (int x = 0; x < XST_BIG; x++) {
            uint32_t c = 0xFF000000u | (xst_hash(0x5000u + (uint32_t)((x / 3) * 977 + (y / 5))) & 0xFFFFFFu);
            if (x == y || x + y == XST_BIG - 1) c = 0xFFFFFFFFu;
            tex[y * XST_BIG + x] = c;
        }
    uint32_t *big = (uint32_t *)malloc(sizeof(uint32_t) * XST_BIG * XST_BIG * (size_t)factor * (size_t)factor);
    if (!big || !nk_tex_scale_image(tex, XST_BIG, XST_BIG, factor, 1, 1, big)) {
        free(big);
        fprintf(stderr, "gpu texscale selftest: FAIL large: reference scale failed\n");
        return 0;
    }
    const int saved = s_xs_factor;
    xst_reset();
    s_xs_factor = factor;
    nk_tex_scale_set_sync(1);
    xst_state(XST_BASE, XST_BIG_ADDR, 3u, 1, 1);
    s_ge->tex_bufw = XST_BIG;
    s_ge->tex_w = XST_BIG;
    s_ge->tex_h = XST_BIG;
    ge_state_note_external_write();
    /* 1:1 sprites of the top-left and bottom-right 16x16 corners: the bands' first and last. */
    const uint64_t uploads0 = s_cnt_xs_uploads;
    int ok = 1;
    for (int pass = 0; ok && pass < 2; pass++) {
        GeVtx p0, p1;
        const float u0 = pass ? (float)(XST_BIG - XST_W) : 0.0f, v0 = pass ? (float)(XST_BIG - XST_H) : 0.0f;
        wrap_vtx(&p0, 20.0f, 160.0f, 0xFFFFFFFFu); p0.u = u0; p0.v = v0;
        wrap_vtx(&p1, 20.0f + XST_W, 160.0f + XST_H, 0xFFFFFFFFu); p1.u = u0 + XST_W; p1.v = v0 + XST_H;
        s_tmpl_ok = 0;
        ok = hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);   /* decodes, scales, queues */
        s_tmpl_ok = 0;
        ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);   /* stores, binds */
        if (!ok) break;
        uint32_t want[XST_W * XST_H];
        for (int y = 0; y < XST_H; y++)
            for (int x = 0; x < XST_W; x++)
                want[y * XST_W + x] = big[xst_readback_texel(v0, y, XST_H, factor) * XST_BIG * (size_t)factor +
                                          xst_readback_texel(u0, x, XST_W, factor)];
        ok = xst_report("large", 20, 160, want, pass ? "the last band of the 16 MiB copy is wrong"
                                                     : "the first band of the 16 MiB copy is wrong");
    }
    free(big);
    const uint64_t uploads = s_cnt_xs_uploads - uploads0;
    s_xs_factor = saved;
    xst_reset();
    if (!ok) return 0;
    if (uploads != 1u) {
        fprintf(stderr, "gpu texscale selftest: FAIL large: %llu scaled uploads, want 1\n", (unsigned long long)uploads);
        return 0;
    }
    printf("gpu texscale selftest: PASS %-12s 512x512 at 4x (16 MiB) uploaded in bands and drawn\n", "large");
    return 1;
}

/* The store bound: at most XS_UPLOAD_BYTES_PER_PRESENT of copies are stored per present, the first
 * always. Three 512x512 textures at 2x (4 MiB copies) are scaled in one present: the first two
 * copies land, the third waits in the job table through further draws of that present and lands at
 * the next one. Without the bound the third lands at once. */
#define XST_UP_ADDR(i) (XST_BIG_ADDR + (uint32_t)(i) * 0x00100000u)
static TexEnt *xst_entry_at(uint32_t addr) {
    for (int i = 0; i < s_tex_n; i++)
        if (s_tex[i].addr == addr) return &s_tex[i];
    return NULL;
}
static int xst_up_draw(uint32_t addr) {
    xst_state(XST_BASE, addr, 3u, 1, 1);
    s_ge->tex_bufw = XST_BIG;
    s_ge->tex_w = XST_BIG;
    s_ge->tex_h = XST_BIG;
    ge_state_note_external_write();
    GeVtx p0, p1;
    wrap_vtx(&p0, 20.0f, 160.0f, 0xFFFFFFFFu); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 20.0f + XST_W, 160.0f + XST_H, 0xFFFFFFFFu); p1.u = (float)XST_W; p1.v = (float)XST_H;
    s_tmpl_ok = 0;
    return hook_sprite(&p0, &p1, 0);
}
static int xst_up_ready(uint32_t addr) {
    const TexEnt *e = xst_entry_at(addr);
    return e && e->xs.state == XS_READY;
}
static int xst_cell_upload_cap(int f) {
    (void)f;
    const int factor = 2, saved = s_xs_factor;
    const uint64_t copy = (uint64_t)XST_BIG * XST_BIG * (uint64_t)factor * (uint64_t)factor * 4u;
    xst_reset();
    s_xs_factor = factor;
    nk_tex_scale_set_sync(1);
    for (uint32_t i = 0; i < 3u; i++) {
        uint32_t *t = (uint32_t *)SR_HOST(XST_UP_ADDR(i));
        for (int k = 0; k < XST_BIG * XST_BIG; k++)
            t[k] = 0xFF000000u | (xst_hash(0x7000u + i * 0x40000u + (uint32_t)(k / 7)) & 0xFFFFFFu);
    }
    const uint64_t up0 = s_cnt_xs_uploads, cap0 = s_cnt_xs_upload_capped;
    /* One present: each draw stores the copy the draw before it scaled (sync), then scales its own. */
    int ok = xst_up_draw(XST_UP_ADDR(0)) && xst_up_draw(XST_UP_ADDR(1)) && xst_up_draw(XST_UP_ADDR(2));
    ok = ok && xst_up_draw(XST_UP_ADDR(2)) && xst_up_draw(XST_UP_ADDR(1));   /* C's copy is done, and waits */
    const int a1 = xst_up_ready(XST_UP_ADDR(0)), b1 = xst_up_ready(XST_UP_ADDR(1)), c1 = xst_up_ready(XST_UP_ADDR(2));
    const uint64_t up1 = s_cnt_xs_uploads - up0, cap1 = s_cnt_xs_upload_capped - cap0;
    s_cnt_present_cpu++;   /* the next present */
    ok = ok && xst_up_draw(XST_UP_ADDR(2));
    const int c2 = xst_up_ready(XST_UP_ADDR(2));
    const uint64_t up2 = s_cnt_xs_uploads - up0;
    s_xs_factor = saved;
    xst_reset();
    if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL upload-cap: draw failed\n"); return 0; }
    if (!a1 || !b1 || c1 || up1 != 2u || cap1 != 1u || !c2 || up2 != 3u) {
        fprintf(stderr, "gpu texscale selftest: FAIL upload-cap: in one present A/B/C stored %d/%d/%d (want 1/1/0), "
                        "%llu copies stored (want 2), capped %llu (want 1); at the next present C stored %d, %llu "
                        "copies (want 1, 3)\n", a1, b1, c1, (unsigned long long)up1, (unsigned long long)cap1, c2,
                (unsigned long long)up2);
        return 0;
    }
    printf("gpu texscale selftest: PASS %-12s %llu MiB copies: two stored in one present, the third at the next\n",
           "upload-cap", (unsigned long long)(copy >> 20));
    return 1;
}

static int xst_cell_not_ready(int f) {
    uint32_t tex[XST_W * XST_H], want[XST_W * XST_H];
    xst_reset();
    nk_tex_scale_set_sync(0);
    nk_tex_scale_set_hold(1);
    xst_pattern(tex, 23u);
    xst_upload(tex);
    xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
    const uint64_t t0 = SDL_GetTicksNS();
    int ok = xst_draw(20.0f, 60.0f);
    s_tmpl_ok = 0;
    ok = ok && xst_draw(60.0f, 60.0f);
    const uint64_t spent_ms = (SDL_GetTicksNS() - t0) / 1000000u;
    xst_expect(tex, 0, 1, want);
    if (!ok || !xst_report("not-ready", 20, 60, want, "a texture with its scale queued is not drawn unscaled") ||
        !xst_report("not-ready", 60, 60, want, "a held scale was drawn"))
        return 0;
    /* Released, the copy lands at a later lookup. */
    nk_tex_scale_set_hold(0);
    nk_tex_scale_wait_idle();
    s_tmpl_ok = 0;
    ok = xst_draw(100.0f, 60.0f);
    xst_expect(tex, f, 1, want);
    if (!ok || !xst_report("not-ready", 100, 60, want, "the copy did not land once its scale finished")) return 0;
    printf("gpu texscale selftest: PASS %-12s held draws unscaled in %llu ms, then the copy\n", "not-ready",
           (unsigned long long)spent_ms);
    return 1;
}

static int xst_cell_rewrite(int f) {
    uint32_t a[XST_W * XST_H], b[XST_W * XST_H], want[XST_W * XST_H], stale[XST_W * XST_H];
    xst_reset();
    nk_tex_scale_set_sync(1);
    xst_pattern(a, 31u);
    xst_pattern(b, 47u);
    xst_upload(a);
    xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
    int ok = xst_draw(20.0f, 100.0f);
    s_tmpl_ok = 0;
    ok = ok && xst_draw(20.0f, 100.0f);
    xst_expect(a, f, 1, want);
    if (!ok || !xst_report("rewrite", 20, 100, want, "the first content's copy is not drawn")) return 0;
    /* The guest rewrites the texture; its scale is held, so only the content check stands
     * between the draw and the old copy. */
    nk_tex_scale_set_sync(0);
    nk_tex_scale_set_hold(1);
    xst_upload(b);
    s_tmpl_ok = 0;
    ok = xst_draw(60.0f, 100.0f);
    xst_expect(b, 0, 1, want);
    xst_expect(a, f, 1, stale);
    if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL rewrite: draw failed\n"); return 0; }
    if (xst_matches(60, 100, stale)) {
        fprintf(stderr, "gpu texscale selftest: FAIL rewrite: stale scaled copy of the previous content was drawn "
                        "after the guest rewrote the texture\n");
        return 0;
    }
    if (!xst_report("rewrite", 60, 100, want, "the rewritten texture is not drawn unscaled while its scale waits"))
        return 0;
    nk_tex_scale_set_hold(0);
    nk_tex_scale_wait_idle();
    s_tmpl_ok = 0;
    ok = xst_draw(100.0f, 100.0f);
    xst_expect(b, f, 1, want);
    if (!ok || !xst_report("rewrite", 100, 100, want, "the rewritten texture's own copy did not land")) return 0;
    printf("gpu texscale selftest: PASS %-12s rewritten texture rescaled, the old copy never drawn\n", "rewrite");
    return 1;
}

static int xst_cell_clut(int f) {
    uint8_t *idx = (uint8_t *)SR_HOST(XST_TEX);
    uint32_t pat[XST_W * XST_H], lut[XST_W * XST_H], want[XST_W * XST_H];
    xst_reset();
    nk_tex_scale_set_sync(1);
    xst_pattern(pat, 5u);
    for (int i = 0; i < XST_W * XST_H; i++) idx[i] = (uint8_t)(pat[i] & 0x0Fu);   /* 16 indices */
    sr_gpu_vram_dirty(XST_TEX, XST_W * XST_H);
    for (int pass = 0; pass < 2; pass++) {
        xst_state(XST_BASE, XST_TEX, 5u, 1, 1);   /* CLUT8; the state reset clears the palette */
        for (uint32_t j = 0; j < 256u; j++) {
            uint32_t entry = 0xFF000000u | (xst_hash(0x900u + j * 3u + (uint32_t)pass * 977u) & 0xFFFFFFu);
            memcpy(&s_ge->clutram[j * 4u], &entry, 4);
        }
        s_ge->clut_fmt = 3u | (0xFFu << 8);       /* 8888, shift 0, mask 0xFF */
        ge_state_note_external_write();
        for (int i = 0; i < XST_W * XST_H; i++) memcpy(&lut[i], &s_ge->clutram[idx[i] * 4u], 4);
        const float y = 140.0f + 40.0f * (float)pass;
        int ok = xst_draw(20.0f, y);
        s_tmpl_ok = 0;
        ok = ok && xst_draw(60.0f, y);
        xst_expect(lut, f, 1, want);
        if (!ok || !xst_report(pass ? "clut-new-palette" : "clut", 60, (int)y, want,
                               "the copy is not the scale of the palette lookup"))
            return 0;
    }
    printf("gpu texscale selftest: PASS %-12s scaled after the lookup, rescaled for a new palette\n", "clut");
    return 1;
}

/* A render target sampled as a texture: drawn the same with scaling on and off, and no scaled
 * copy is made or bound for it. */
static int xst_rtt_pass(int factor, int swizzle, uint8_t *out) {
    uint32_t tex[XST_W * XST_H];
    xst_reset();
    nk_tex_scale_set_sync(1);
    s_xs_factor = factor;
    memset(SR_HOST(XST_RTT), 0, XST_STRIDE * FB_H * 4u);
    sr_gpu_vram_dirty(XST_RTT, XST_STRIDE * FB_H * 4u);
    xst_pattern(tex, 71u);
    xst_upload(tex);
    /* Render into the second target: it becomes a GPU-valid render target. */
    xst_state(XST_RTT, XST_TEX, 3u, 1, 1);
    GeVtx p0, p1;
    wrap_vtx(&p0, 0.0f, 0.0f, 0xFFFFFFFFu); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 64.0f, 64.0f, 0xFFFFFFFFu); p1.u = (float)XST_W; p1.v = (float)XST_H;
    s_xs_factor = 0;                         /* the same target content in both passes */
    int ok = hook_sprite(&p0, &p1, 0) && submit_pending();
    s_xs_factor = factor;
    /* Sample it from the first target: bound directly (8888, linear), or, swizzled, read back
     * and decoded through the texture cache. */
    xst_state(XST_BASE, XST_RTT, 3u, 1, 1);
    s_ge->tex_bufw = XST_STRIDE;
    s_ge->tex_swizzle = (uint32_t)swizzle;
    ge_state_note_external_write();
    ok = ok && xst_draw(20.0f, 20.0f) && xst_draw(20.0f, 60.0f);
    if (ok) memcpy(out, SR_HOST(XST_BASE), XST_STRIDE * FB_H * 4u);
    for (int i = 0; i < s_tex_n; i++)
        if (s_tex[i].addr == XST_RTT && (s_tex[i].xs.state != XS_NONE || s_tex[i].xs.factor)) {
            fprintf(stderr, "gpu texscale selftest: FAIL rtt: a render target has a scaled copy (state %d)\n",
                    s_tex[i].xs.state);
            ok = 0;
        }
    return ok;
}

/* With room for two copies: A and B are scaled and in use, so C stays unscaled and no copy in use
 * is freed. Once A and B have been idle XS_IDLE_PRESENTS presents, C's next draw frees them and its
 * copy lands; A, whose copy was freed, is scaled again on its next draws. */
#define XST_TEX_B (XST_TEX + 0x1000u)
#define XST_TEX_C (XST_TEX + 0x2000u)
static int xst_budget_draw(uint32_t addr, const uint32_t *t, float x, int f, const char *what) {
    uint32_t want[XST_W * XST_H];
    xst_state(XST_BASE, addr, 3u, 1, 1);
    s_tmpl_ok = 0;
    if (!xst_draw(x, 230.0f)) {
        fprintf(stderr, "gpu texscale selftest: FAIL budget: draw or readback failed\n");
        return 0;
    }
    xst_expect(t, f, 1, want);
    return xst_report("budget", (int)x, 230, want, what);
}

static int xst_cell_budget(int f) {
    uint32_t ta[XST_W * XST_H], tb[XST_W * XST_H], tc[XST_W * XST_H];
    xst_reset();
    nk_tex_scale_set_sync(1);
    const NkTexScaleConfig saved = *nk_tex_scale_config();
    NkTexScaleConfig two = saved;
    const uint64_t copy = (uint64_t)XST_W * XST_H * (uint64_t)f * (uint64_t)f * 4u;
    two.budget_bytes = 2u * copy;
    nk_tex_scale_configure(&two);
    xst_pattern(ta, 61u);
    xst_pattern(tb, 67u);
    xst_pattern(tc, 71u);
    const uint32_t addr[3] = { XST_TEX, XST_TEX_B, XST_TEX_C };
    const uint32_t *tex[3] = { ta, tb, tc };
    for (int i = 0; i < 3; i++) {
        memcpy(SR_HOST(addr[i]), tex[i], sizeof(uint32_t) * XST_W * XST_H);
        sr_gpu_vram_dirty(addr[i], sizeof(uint32_t) * XST_W * XST_H);
    }
    const uint64_t short0 = s_cnt_xs_budget_short, reclaimed0 = s_cnt_xs_reclaimed;
    /* A and B: decoded and scaled at the first draw, the copy at the second. */
    int ok = xst_budget_draw(XST_TEX, ta, 20.0f, 0, "A's first draw is not the texture") &&
             xst_budget_draw(XST_TEX, ta, 20.0f, f, "A's copy is not drawn") &&
             xst_budget_draw(XST_TEX_B, tb, 60.0f, 0, "B's first draw is not the texture") &&
             xst_budget_draw(XST_TEX_B, tb, 60.0f, f, "B's copy is not drawn");
    if (ok && nk_tex_scale_budget_used() != 2u * copy) {
        fprintf(stderr, "gpu texscale selftest: FAIL budget: %llu bytes held after two copies, want %llu\n",
                (unsigned long long)nk_tex_scale_budget_used(), (unsigned long long)(2u * copy));
        ok = 0;
    }
    /* C: the budget is full of copies in use, so C stays unscaled and A keeps its copy. */
    ok = ok && xst_budget_draw(XST_TEX_C, tc, 100.0f, 0, "C, past the budget, is not drawn unscaled") &&
         xst_budget_draw(XST_TEX_C, tc, 100.0f, 0, "C, past the budget, is not drawn unscaled the second time") &&
         xst_budget_draw(XST_TEX, ta, 140.0f, f, "A's copy, in use, was freed for C");
    const uint64_t short1 = s_cnt_xs_budget_short - short0, reclaimed1 = s_cnt_xs_reclaimed - reclaimed0;
    if (ok && (short1 != 1u || reclaimed1 != 0u)) {
        fprintf(stderr, "gpu texscale selftest: FAIL budget: with the copies in use, %llu budget waits (want 1, once "
                        "per present) and %llu copies freed (want 0)\n",
                (unsigned long long)short1, (unsigned long long)reclaimed1);
        ok = 0;
    }
    /* A and B go idle; C's next draw frees both and offers C, whose copy the draw after shows. */
    s_cnt_present_cpu += XS_IDLE_PRESENTS;
    ok = ok && xst_budget_draw(XST_TEX_C, tc, 180.0f, 0, "C is not drawn unscaled while its copy is made") &&
         xst_budget_draw(XST_TEX_C, tc, 220.0f, f, "C's copy did not land once the idle copies were freed");
    const uint64_t reclaimed2 = s_cnt_xs_reclaimed - reclaimed0;
    if (ok && (reclaimed2 != 2u || nk_tex_scale_budget_used() != copy)) {
        fprintf(stderr, "gpu texscale selftest: FAIL budget: %llu idle copies freed (want 2), %llu bytes held (want "
                        "%llu)\n", (unsigned long long)reclaimed2, (unsigned long long)nk_tex_scale_budget_used(),
                (unsigned long long)copy);
        ok = 0;
    }
    /* A, its copy freed, is offered again on its next draw and scaled. */
    ok = ok && xst_budget_draw(XST_TEX, ta, 260.0f, 0, "A, its copy freed, is not drawn unscaled") &&
         xst_budget_draw(XST_TEX, ta, 300.0f, f, "A's copy did not come back after it was freed");
    if (ok && nk_tex_scale_budget_used() != 2u * copy) {
        fprintf(stderr, "gpu texscale selftest: FAIL budget: %llu bytes held at the end, want %llu\n",
                (unsigned long long)nk_tex_scale_budget_used(), (unsigned long long)(2u * copy));
        ok = 0;
    }
    nk_tex_scale_configure(&saved);
    if (!ok) return 0;
    printf("gpu texscale selftest: PASS %-12s in-use copies kept, idle ones freed (%llu), a freed copy rescaled\n",
           "budget", (unsigned long long)reclaimed2);
    return 1;
}

static int xst_cell_rtt(int f) {
    const size_t bytes = XST_STRIDE * FB_H * 4u;
    uint8_t *on = (uint8_t *)malloc(bytes), *off = (uint8_t *)malloc(bytes);
    int ok = on && off;
    for (int swizzle = 0; ok && swizzle < 2; swizzle++) {
        const uint64_t bound0 = s_cnt_xs_bound;
        ok = xst_rtt_pass(f, swizzle, on);
        const uint64_t bound = s_cnt_xs_bound - bound0;
        ok = ok && xst_rtt_pass(0, swizzle, off);
        s_xs_factor = f;
        if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL rtt: draw failed (swizzle %d)\n", swizzle); break; }
        if (bound) {
            fprintf(stderr, "gpu texscale selftest: FAIL rtt: %llu scaled binds for a render target (swizzle %d)\n",
                    (unsigned long long)bound, swizzle);
            ok = 0;
            break;
        }
        if (memcmp(on, off, bytes) != 0) {
            fprintf(stderr, "gpu texscale selftest: FAIL rtt: a render-to-texture draw differs with scaling on "
                            "(swizzle %d)\n", swizzle);
            ok = 0;
        }
    }
    free(on);
    free(off);
    s_xs_factor = f;
    if (ok) printf("gpu texscale selftest: PASS %-12s direct and read back: never scaled, identical on/off\n", "rtt");
    return ok;
}

/* A two-cell atlas, like a font page: glyph A in columns 0-7 and, when `with_b`, glyph B in
 * columns 8-15 with a stroke on the shared edge (else the field). */
static void xst_atlas(uint32_t *t, int with_b) {
    const uint32_t field = 0xFF202020u, ink_a = 0xFFE0E0E0u, ink_b = 0xFF30C0F0u;
    for (int y = 0; y < XST_H; y++)
        for (int x = 0; x < XST_W; x++) {
            uint32_t c = field;
            if (x < 8 && (x == y / 2 || (y == 12 && x >= 1)))
                c = ink_a;
            if (with_b && x >= 8 && (x == 8 || x - 8 == 15 - y || (y >= 3 && y <= 5)))
                c = ink_b;
            t[y * XST_W + x] = c;
        }
}

/* A sprite of the 8x16 texel cell at (0, 0), `zoom` guest pixels per texel, at (x, y). */
static int xst_cell_sprite(float x, float y, int zoom) {
    GeVtx p0, p1;
    wrap_vtx(&p0, x, y, 0xFFFFFFFFu); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, x + 8.0f * (float)zoom, y + 16.0f * (float)zoom, 0xFFFFFFFFu); p1.u = 8.0f; p1.v = 16.0f;
    s_tmpl_ok = 0;
    return hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
}

/* Device pixels that differ between the w x h guest-pixel regions at (ax, ay) and (bx, by). */
static int xst_device_diff(const uint32_t *dev, int ax, int ay, int bx, int by, int w, int h) {
    const int s = s_scale;
    int n = 0;
    for (int ky = 0; ky < h * s; ky++)
        for (int kx = 0; kx < w * s; kx++)
            if ((dev[((size_t)ay * (size_t)s + (size_t)ky) * SCL_W + (size_t)ax * (size_t)s + (size_t)kx] & XST_RGB) !=
                (dev[((size_t)by * (size_t)s + (size_t)ky) * SCL_W + (size_t)bx * (size_t)s + (size_t)kx] & XST_RGB))
                n++;
    return n;
}

/* Draws of one atlas cell, `pass` 0 with the 1:1 rule off and 1 with it on: for each atlas
 * (with glyph B, then without), one draw that decodes and scales and one that finds the copy. */
static int xst_onetoone_pass(float x0, uint64_t *bound, uint64_t *plain) {
    uint32_t tex[XST_W * XST_H];
    int ok = 1;
    for (int with_b = 1; ok && with_b >= 0; with_b--) {
        xst_atlas(tex, with_b);
        xst_upload(tex);
        ge_state_note_external_write();
        const float x = x0 + (with_b ? 0.0f : 40.0f);
        ok = xst_cell_sprite(x, 20.0f, 1);
        const uint64_t b0 = s_cnt_xs_bound, p0 = s_cnt_xs_plain;
        ok = ok && xst_cell_sprite(x + 20.0f, 20.0f, 1);
        *bound += s_cnt_xs_bound - b0;
        *plain += s_cnt_xs_plain - p0;
    }
    return ok;
}

/* The 1:1 rule. With it off, a 1:1 draw of one cell of an atlas from the scaled copy shows the
 * neighbouring cell at its edge: the cell draws differently beside glyph B than beside the field
 * (the marks under LittleBigPlanet's text). With it on, the same draw is the unscaled texture's,
 * beside either neighbour. A magnified draw keeps the copy and is identical with the rule on or
 * off. Nearest sampling, so the unscaled texture itself never reads past the cell. */
static int xst_cell_onetoone(int f) {
    (void)f;
    const int smart = s_xs_smart2d;
    xst_reset();
    s_xs_smart2d = 0;                 /* the copy alone; smart 2D has its own cell */
    nk_tex_scale_set_sync(1);
    xst_state(XST_BASE, XST_TEX, 3u, 0, 1);
    uint64_t off_bound = 0, off_plain = 0, on_bound = 0, on_plain = 0;
    s_xs_onetoone_plain = 0;
    int ok = xst_onetoone_pass(20.0f, &off_bound, &off_plain);      /* rule off: x 20..100 */
    s_xs_onetoone_plain = 1;
    ok = ok && xst_onetoone_pass(120.0f, &on_bound, &on_plain);      /* rule on: x 120..200 */
    /* Magnified 2x from the copy of the field-only atlas, the rule on and then off. */
    const uint64_t mag_p0 = s_cnt_xs_plain, mag_b0 = s_cnt_xs_bound;
    ok = ok && xst_cell_sprite(20.0f, 60.0f, 2);
    s_xs_onetoone_plain = 0;
    ok = ok && xst_cell_sprite(60.0f, 60.0f, 2);
    s_xs_onetoone_plain = 1;
    const uint64_t mag_plain = s_cnt_xs_plain - mag_p0, mag_bound = s_cnt_xs_bound - mag_b0;
    s_xs_smart2d = smart;
    if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL onetoone: draw or readback failed\n"); return 0; }
    if (off_bound != 2u || off_plain != 0u || on_bound != 2u || on_plain != 2u || mag_bound != 2u || mag_plain != 0u) {
        fprintf(stderr, "gpu texscale selftest: FAIL onetoone: copies found %llu/%llu/%llu times (want 2/2/2), "
                        "drawn unscaled %llu/%llu/%llu (want 0/2/0) with the rule off, on, magnified\n",
                (unsigned long long)off_bound, (unsigned long long)on_bound, (unsigned long long)mag_bound,
                (unsigned long long)off_plain, (unsigned long long)on_plain, (unsigned long long)mag_plain);
        return 0;
    }
    uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
    if (!dev || !s_cur || !xst_read_device(s_cur, dev)) {
        free(dev);
        fprintf(stderr, "gpu texscale selftest: FAIL onetoone: device readback failed\n");
        return 0;
    }
    /* Positions: rule off at 20 (unscaled, with B), 40 (copy, with B), 60 (unscaled), 80 (copy);
     * rule on at 120, 140, 160, 180 likewise; magnified at (20, 60) on and (60, 60) off. The
     * neighbour is measured over guest columns 0-6: above 1x the last guest column's right half
     * samples past u = 8 (hook_sprite's pixel-centre shift), so even the unscaled texture reads
     * column 8 there. Over columns 0-6 only a copy can carry the neighbour in. */
    const int bleed_off = xst_device_diff(dev, 40, 20, 80, 20, 7, 16);
    const int copy_differs = xst_device_diff(dev, 40, 20, 20, 20, 8, 16);
    const int bleed_on = xst_device_diff(dev, 140, 20, 180, 20, 7, 16);
    const int on_vs_plain = xst_device_diff(dev, 140, 20, 20, 20, 8, 16) + xst_device_diff(dev, 180, 20, 60, 20, 8, 16);
    const int mag_diff = xst_device_diff(dev, 20, 60, 60, 60, 16, 32);
    free(dev);
    if (bleed_on != 0 || on_vs_plain != 0 || mag_diff != 0) {
        fprintf(stderr, "gpu texscale selftest: FAIL onetoone: with the rule on, %d device pixels of the cell depend on "
                        "its neighbour and %d differ from the unscaled draw; the magnified draw differs in %d with the "
                        "rule on and off (want 0, 0, 0)\n", bleed_on, on_vs_plain, mag_diff);
        return 0;
    }
    if (s_scale > 1 && bleed_off == 0) {
        fprintf(stderr, "gpu texscale selftest: FAIL onetoone: the copy shows no neighbour at the cell's edge at render "
                        "scale %d (vacuous: %d device pixels differ from the texture)\n", s_scale, copy_differs);
        return 0;
    }
    printf("gpu texscale selftest: PASS %-12s 1:1 draws unscaled (%d device pixels of neighbour bleed without the "
           "rule); magnified draws keep the copy\n", "onetoone", bleed_off);
    return 1;
}

static int xst_cell_smart2d(void) {
    uint32_t tex[XST_W * XST_H];
    const int factor = s_xs_factor;
    xst_reset();
    s_xs_factor = 0;   /* the filter alone, on the guest's own texture */
    xst_pattern(tex, 89u);
    xst_upload(tex);
    xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
    const uint64_t n0 = s_cnt_smart2d;
    int ok = xst_draw(20.0f, 20.0f);                 /* smart 2D on */
    const uint64_t on_count = s_cnt_smart2d - n0;
    s_xs_smart2d = 0;
    s_tmpl_ok = 0;
    ok = ok && xst_draw(60.0f, 20.0f);               /* off */
    s_xs_smart2d = 1;
    /* A perspective triangle keeps its filter. */
    s_tmpl_ok = 0;
    GeVtx a, b, c;
    wrap_vtx(&a, 100.0f, 20.0f, 0xFFFFFFFFu); a.rw = 1.0f;   a.u = 0.0f;          a.v = 0.0f;
    wrap_vtx(&b, 140.0f, 24.0f, 0xFFFFFFFFu); b.rw = 0.5f;   b.u = 16.0f * 0.5f;  b.v = 0.0f;
    wrap_vtx(&c, 104.0f, 60.0f, 0xFFFFFFFFu); c.rw = 0.25f;  c.u = 0.0f;          c.v = 16.0f * 0.25f;
    s_ge->vtype = 0;
    ge_state_note_external_write();
    const uint64_t n1 = s_cnt_smart2d;
    ok = ok && hook_tri(&a, &b, &c, 1);
    const uint64_t tri_count = s_cnt_smart2d - n1;
    s_xs_factor = factor;
    if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL smart2d: draw failed\n"); return 0; }
    if (on_count != 1u || tri_count != 0u) {
        fprintf(stderr, "gpu texscale selftest: FAIL smart2d: the 1:1 sprite was classified %llu times (want 1), "
                        "the perspective triangle %llu (want 0)\n",
                (unsigned long long)on_count, (unsigned long long)tri_count);
        return 0;
    }
    /* The sprite smart 2D samples nearest reads whole texels at every render scale. At the PSP's
     * resolution the linear one does too, so the guest frame is the texture with the filter on or
     * off. Above 1x each device pixel samples at its own position (hook_sprite, #2104), so the
     * linear sprite's guest pixels blend their neighbours; the device image below checks that. */
    uint32_t want[XST_W * XST_H];
    xst_expect(tex, 0, 1, want);
    if (!xst_report("smart2d", 20, 20, want, "the filtered sprite's guest pixels are not the texture") ||
        (s_scale == 1 && !xst_report("smart2d", 60, 20, want, "the linear sprite's guest pixels are not the texture")))
        return 0;
    if (s_scale > 1) {
        uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
        if (!dev || !s_cur || !xst_read_device(s_cur, dev)) {
            free(dev);
            fprintf(stderr, "gpu texscale selftest: FAIL smart2d: device readback failed\n");
            return 0;
        }
        const int nearest_blends = xst_device_blends(dev, 20, 20, tex, 1);
        const int linear_blends = xst_device_blends(dev, 60, 20, tex, 1);
        free(dev);
        if (nearest_blends != 0 || linear_blends == 0) {
            fprintf(stderr, "gpu texscale selftest: FAIL smart2d: %d blended device pixels with the filter (want 0), "
                            "%d without it (want some)\n", nearest_blends, linear_blends);
            return 0;
        }
        printf("gpu texscale selftest: PASS %-12s nearest at %dx (%d blended device pixels without it)\n",
               "smart2d", s_scale, linear_blends);
    } else {
        printf("gpu texscale selftest: PASS %-12s classified at 1x; frames identical\n", "smart2d");
    }
    return 1;
}

int gegpu_texscale_selftest(void) {
    const int f = s_xs_factor;
    if (!f || !s_xs_smart2d) {
        fprintf(stderr, "gpu texscale selftest: FAIL texture scaling or the smart 2D filter is off (factor %d, "
                        "smart2d %d): the Ace line or SR_TEX_SCALE/SR_SMART_2D did not take\n", f, s_xs_smart2d);
        return 0;
    }
    int ok = 1;
    static int (*const cells[])(int) = { xst_cell_scaled_draw, xst_cell_large, xst_cell_upload_cap,
                                         xst_cell_not_ready, xst_cell_rewrite, xst_cell_clut, xst_cell_budget,
                                         xst_cell_rtt, xst_cell_onetoone };
    static const char *const names[] = { "scaled-draw", "large", "upload-cap", "not-ready", "rewrite", "clut",
                                         "budget", "rtt", "onetoone" };
    /* The cells before onetoone check copies through 1:1 sprites, so they run with the 1:1 rule
     * off; onetoone turns it back on. */
    s_xs_onetoone_plain = 0;
    const size_t vram = 0x00200000u, fb0 = XST_BASE & 0x001FFFFFu, fb1 = fb0 + XST_STRIDE * FB_H * 4u;
    uint8_t *before = (uint8_t *)malloc(vram), clut[sizeof(s_ge->clutram)];
    if (!before) return 0;
    for (size_t i = 0; ok && i < sizeof(cells) / sizeof(cells[0]); i++) {
        ok = cells[i](f);
        if (!ok) break;
        /* After the cell: one more draw of its last texture while every pending scale lands, and
         * a VRAM read of the texture. Outside the frame that draw writes, guest VRAM and the
         * palette are the bytes the CPU left: no scaling step writes guest memory. */
        memcpy(before, SR_HOST(0x04000000u), vram);
        memcpy(clut, s_ge->clutram, sizeof(clut));
        s_tmpl_ok = 0;
        ok = xst_draw(200.0f, 200.0f);
        nk_tex_scale_wait_idle();
        s_tmpl_ok = 0;
        ok = ok && xst_draw(200.0f, 200.0f);
        (void)hook_vram_read(XST_TEX, 4096u);
        const uint8_t *now = (const uint8_t *)SR_HOST(0x04000000u);
        size_t bad = vram;
        for (size_t b = 0; b < vram; b++)
            if ((b < fb0 || b >= fb1) && now[b] != before[b]) { bad = b; break; }
        if (ok && (bad != vram || memcmp(clut, s_ge->clutram, sizeof(clut)) != 0)) {
            fprintf(stderr, "gpu texscale selftest: FAIL guest-bytes after %s: %s changed (VRAM offset 0x%zx)\n",
                    names[i], bad != vram ? "a guest VRAM byte" : "a CLUT byte", bad);
            ok = 0;
        }
    }
    free(before);
    s_xs_onetoone_plain = 1;
    if (ok) printf("gpu texscale selftest: PASS %-12s VRAM outside the frame and the CLUT unchanged\n", "guest-bytes");
    ok = ok && xst_cell_smart2d();
    xst_reset();
    nk_tex_scale_set_sync(0);
    /* Under --twin, the host twin (#2050) draws every cell a second time. xst_reset compared each
     * twin with its image before freeing them. With `--twin native`, the pixels must be equal, so a
     * scaled copy, its factor and clamp, the one-to-one swap and smart 2D all reach the twin pass.
     * As in gegpu_coherence_selftest, a twin found behind its image and re-seeded also fails, and a
     * requested twin must have run to the end: unavailable or turned off fails, never a quiet OK. */
    const char *twin_env = getenv("SR_GPU_TWIN_COLOR");
    if (twin_mode_parse(twin_env) > 0 && !s_twin) {
        fprintf(stderr, "gpu texscale selftest: FAIL twin-identity: the host twin was requested "
                        "(SR_GPU_TWIN_COLOR=%s) but %s\n", twin_env,
                s_twin_unavailable ? "this device refused it"
                : s_cnt_twin_alloc_fail ? "it turned off after a failed allocation" : "it is off");
        ok = 0;
    } else if (s_twin) {
        if (s_twin_identity_failures || s_cnt_twin_reseed) {
            fprintf(stderr, "gpu texscale selftest: FAIL twin-identity (%u identity failure(s), printed above; "
                            "%lu re-seed(s), last lag: %s)\n", s_twin_identity_failures, s_cnt_twin_reseed,
                    s_twin_lag_cause ? s_twin_lag_cause : "none");
            ok = 0;
        } else {
            printf("gpu texscale selftest: PASS %-12s every twin in step with its image%s\n", "twin-identity",
                   s_twin_color ? "" : ", pixel for pixel");
        }
    }
    return ok;
}
