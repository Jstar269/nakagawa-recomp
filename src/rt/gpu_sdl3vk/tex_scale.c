/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * tex_scale.c - see tex_scale.h.
 *
 * Jobs live in a fixed table of NK_TEX_SCALE_QUEUE slots, each FREE, QUEUED, RUNNING or DONE,
 * with a sequence number that keeps them first in, first out. Two worker threads take QUEUED
 * slots; the renderer thread submits and takes. Nothing here waits for a worker except
 * nk_tex_scale_wait_idle and shutdown.
 */
#include "tex_scale.h"

#include "../../../third_party/xbrz/xbrz.h"

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- settings --------------------------------------------------------------------------- */

static NkTexScaleConfig s_cfg;   /* all off until configured */

static void set_why(char *why, size_t cap, const char *text) {
    if (why && cap) snprintf(why, cap, "%s", text);
}

int nk_tex_scale_parse(const char *scale, const char *smart2d, const char *budget_mb,
                       NkTexScaleConfig *out, char *why, size_t why_cap) {
    NkTexScaleConfig c = { 0, 0, (uint64_t)NK_TEX_SCALE_DEFAULT_BUDGET_MB << 20 };
    memset(out, 0, sizeof(*out));
    out->budget_bytes = c.budget_bytes;
    if (scale && scale[0] && strcmp(scale, "off") != 0) {
        if (scale[1] != '\0' || scale[0] < '2' || scale[0] > '0' + NK_TEX_SCALE_MAX) {
            set_why(why, why_cap, "SR_TEX_SCALE takes off, 2, 3, 4, 5 or 6");
            return 0;
        }
        c.factor = scale[0] - '0';
    }
    if (smart2d && smart2d[0]) {
        if (strcmp(smart2d, "on") == 0) c.smart2d = 1;
        else if (strcmp(smart2d, "off") != 0) {
            set_why(why, why_cap, "SR_SMART_2D takes off or on");
            return 0;
        }
    }
    if (budget_mb && budget_mb[0]) {
        char *end = NULL;
        unsigned long v = strtoul(budget_mb, &end, 10);
        if (!end || *end || budget_mb[0] < '0' || budget_mb[0] > '9' || v < 1ul || v > 8192ul) {
            set_why(why, why_cap, "SR_TEX_SCALE_BUDGET_MB takes 1 to 8192");
            return 0;
        }
        c.budget_bytes = (uint64_t)v << 20;
    }
    *out = c;
    return 1;
}

void nk_tex_scale_configure(const NkTexScaleConfig *cfg) {
    if (cfg) s_cfg = *cfg;
    else memset(&s_cfg, 0, sizeof(s_cfg));
}

const NkTexScaleConfig *nk_tex_scale_config(void) { return &s_cfg; }

size_t nk_tex_scale_admission_line(const NkTexScaleConfig *cfg, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!cfg || (!cfg->factor && !cfg->smart2d)) return 0;
    const int n = snprintf(out, cap,
                           "TEX_SCALE: guest-visible, on through SR_TEX_SCALE/SR_SMART_2D and not admitted under "
                           "contract section 11.2 (each present commits the frame to guest VRAM; see the contract's "
                           "\"Texture Scaling and the Smart 2D Filter\" entry)");
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

static char s_start_line[384];   /* what the last nk_tex_scale_init_from_env printed */

static void start_line(const char *text) {
    const size_t used = strlen(s_start_line);
    if (used + 1 < sizeof(s_start_line)) snprintf(s_start_line + used, sizeof(s_start_line) - used, "%s\n", text);
    fprintf(stderr, "%s\n", text);
}

const char *nk_tex_scale_start_lines(void) { return s_start_line; }

const NkTexScaleConfig *nk_tex_scale_init_from_env(int enhanced) {
    const char *scale = getenv("SR_TEX_SCALE");
    const char *smart = getenv("SR_SMART_2D");
    const char *budget = getenv("SR_TEX_SCALE_BUDGET_MB");
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_start_line[0] = '\0';
    int asked = (scale && scale[0] && strcmp(scale, "off") != 0) ||
                (smart && smart[0] && strcmp(smart, "off") != 0);
#ifdef NK_TEX_SCALE_MUTANT_NO_ACE_GATE
    enhanced = 1;   /* tex-scale-gate-mutant: the Ace gate removed; the settings cell must fail */
#endif
    if (!enhanced) {
        /* Authentic: nothing is read further, and a request is named, not obeyed. */
        if (asked) start_line("TEX_SCALE: off (texture scaling and the smart 2D filter need Ace Mode)");
        return &s_cfg;
    }
    char why[96], line[256];
    NkTexScaleConfig c;
    if (!nk_tex_scale_parse(scale, smart, budget, &c, why, sizeof(why))) {
        snprintf(line, sizeof(line), "TEX_SCALE: refused (%s)", why);
        start_line(line);
        return &s_cfg;
    }
    s_cfg = c;
    if (c.factor || c.smart2d) {
        char f[8];
        if (c.factor) snprintf(f, sizeof(f), "%d", c.factor); else snprintf(f, sizeof(f), "off");
        snprintf(line, sizeof(line), "TEX_SCALE: on (xbrz=%s, smart2d=%s, budget_mb=%llu)", f,
                 c.smart2d ? "on" : "off", (unsigned long long)(c.budget_bytes >> 20));
        start_line(line);
        if (nk_tex_scale_admission_line(&c, line, sizeof(line))) start_line(line);
    }
    return &s_cfg;
}

int nk_tex_scale_factor_for(int want, int w, int h) {
    if (want < 2 || w <= 0 || h <= 0) return 0;
    int side = w > h ? w : h;
    int f = want > NK_TEX_SCALE_MAX ? NK_TEX_SCALE_MAX : want;
    while (f >= 2 && side * f > NK_TEX_SCALE_MAX_DIM) f--;
    return f >= 2 ? f : 0;
}

/* ---- budget ------------------------------------------------------------------------------ */

static uint64_t s_budget_used, s_budget_peak;

int nk_tex_scale_budget_take(uint64_t bytes) {
    if (s_budget_used + bytes > s_cfg.budget_bytes) return 0;
    s_budget_used += bytes;
    if (s_budget_used > s_budget_peak) s_budget_peak = s_budget_used;
    return 1;
}

void nk_tex_scale_budget_give(uint64_t bytes) {
    s_budget_used = bytes > s_budget_used ? 0 : s_budget_used - bytes;
}

uint64_t nk_tex_scale_budget_used(void) { return s_budget_used; }

/* ---- the pure scale ----------------------------------------------------------------------- */

#define PAD 2   /* xBRZ reads two texels past each output texel's own (its 4x4 kernels) */

/* RGBA (R low) <-> ARGB (B low): the two orders differ only in bytes 0 and 2. */
static inline uint32_t swap_rb(uint32_t p) {
    return (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
}

static inline int edge(int i, int n, int clamp) {
    if (clamp) return i < 0 ? 0 : (i >= n ? n - 1 : i);
    i %= n;
    return i < 0 ? i + n : i;
}

int nk_tex_scale_image(const uint32_t *rgba, int w, int h, int factor, int clamp_u, int clamp_v,
                       uint32_t *out) {
    if (factor < 2 || factor > NK_TEX_SCALE_MAX || w <= 0 || h <= 0) return 0;
    const int pw = w + 2 * PAD, ph = h + 2 * PAD;
    uint32_t *src = (uint32_t *)malloc((size_t)pw * (size_t)ph * sizeof(uint32_t));
    uint32_t *big = (uint32_t *)malloc((size_t)pw * (size_t)ph * (size_t)(factor * factor) *
                                       sizeof(uint32_t));
    if (!src || !big) {
        free(src);
        free(big);
        return 0;
    }
    for (int y = 0; y < ph; y++) {
        const uint32_t *row = rgba + (size_t)edge(y - PAD, h, clamp_v) * (size_t)w;
        for (int x = 0; x < pw; x++)
            src[(size_t)y * pw + x] = swap_rb(row[edge(x - PAD, w, clamp_u)]);
    }
    int ok = nk_xbrz_scale(factor, src, big, pw, ph, NULL, 0, ph);
    if (ok) {
        const size_t bw = (size_t)pw * (size_t)factor, ow = (size_t)w * (size_t)factor;
        for (size_t y = 0; y < (size_t)h * (size_t)factor; y++) {
            const uint32_t *in = big + (y + (size_t)PAD * factor) * bw + (size_t)PAD * factor;
            uint32_t *o = out + y * ow;
            for (size_t x = 0; x < ow; x++) o[x] = swap_rb(in[x]);
        }
    }
    free(src);
    free(big);
    return ok;
}

/* ---- jobs ---------------------------------------------------------------------------------- */

enum { JOB_FREE, JOB_QUEUED, JOB_RUNNING, JOB_DONE };

typedef struct {
    int state;
    uint64_t seq;
    uint64_t serial, hash;
    int w, h, factor, clamp_u, clamp_v;
    uint32_t *src;   /* w x h, owned while QUEUED/RUNNING */
    uint32_t *dst;   /* the result, owned while DONE */
    int failed;
} Job;

#define WORKERS 2

static Job s_jobs[NK_TEX_SCALE_QUEUE];
static uint64_t s_seq;
static SDL_Mutex *s_mx;
static SDL_Condition *s_work_cv, *s_idle_cv;
static SDL_Thread *s_threads[WORKERS];
static int s_started, s_quit, s_hold, s_sync;
static NkTexScaleStats s_stats;
/* Host bytes the jobs not FREE hold or will hold (source plus result), under s_mx. */
static uint64_t s_host_bytes;

static uint64_t job_host_bytes(int w, int h, int factor) {
    return (uint64_t)w * (uint64_t)h * 4u * (1u + (uint64_t)factor * (uint64_t)factor);
}

static void run_job(Job *j) {
    uint64_t t0 = SDL_GetTicksNS();
    size_t n = (size_t)j->w * (size_t)j->h * (size_t)(j->factor * j->factor);
    j->dst = (uint32_t *)malloc(n * sizeof(uint32_t));
    j->failed = !j->dst ||
        !nk_tex_scale_image(j->src, j->w, j->h, j->factor, j->clamp_u, j->clamp_v, j->dst);
    if (j->failed) {
        free(j->dst);
        j->dst = NULL;
    }
    free(j->src);
    j->src = NULL;
    s_stats.scale_ns += SDL_GetTicksNS() - t0;   /* the sync path holds s_mx */
}

static Job *oldest(int state) {
    Job *best = NULL;
    for (int i = 0; i < NK_TEX_SCALE_QUEUE; i++)
        if (s_jobs[i].state == state && (!best || s_jobs[i].seq < best->seq)) best = &s_jobs[i];
    return best;
}

static int worker(void *arg) {
    (void)arg;
    SDL_LockMutex(s_mx);
    for (;;) {
        Job *j = NULL;
        while (!s_quit && (s_hold || !(j = oldest(JOB_QUEUED)))) SDL_WaitCondition(s_work_cv, s_mx);
        if (s_quit) break;
        j->state = JOB_RUNNING;
        SDL_UnlockMutex(s_mx);
        uint64_t t0 = SDL_GetTicksNS();
        size_t n = (size_t)j->w * (size_t)j->h * (size_t)(j->factor * j->factor);
        uint32_t *dst = (uint32_t *)malloc(n * sizeof(uint32_t));
        int ok = dst && nk_tex_scale_image(j->src, j->w, j->h, j->factor, j->clamp_u, j->clamp_v, dst);
        uint64_t spent = SDL_GetTicksNS() - t0;
        SDL_LockMutex(s_mx);
        free(j->src);
        j->src = NULL;
        if (!ok) { free(dst); dst = NULL; }
        j->dst = dst;
        j->failed = !ok;
        j->state = JOB_DONE;
        s_stats.scale_ns += spent;
        if (ok) s_stats.scaled++;
        SDL_BroadcastCondition(s_idle_cv);
    }
    SDL_UnlockMutex(s_mx);
    return 0;
}

static int ensure_started(void) {
    if (s_started) return 1;
    s_mx = SDL_CreateMutex();
    s_work_cv = SDL_CreateCondition();
    s_idle_cv = SDL_CreateCondition();
    if (!s_mx || !s_work_cv || !s_idle_cv) return 0;
    s_started = 1;
    s_quit = 0;
    for (int i = 0; i < WORKERS; i++) {
        s_threads[i] = SDL_CreateThread(worker, "nk-texscale", NULL);
        if (!s_threads[i]) {
            /* Without workers a queued job would never run: scale on the caller instead. */
            fprintf(stderr, "TEX_SCALE: worker thread unavailable (%s); scaling on the renderer thread\n",
                    SDL_GetError());
            s_sync = 1;
            break;
        }
    }
    return 1;
}

/* Room for a job holding `host` bytes, under s_mx: a free slot, and the jobs' host memory stays
 * within NK_TEX_SCALE_HOST_MB (one job is always accepted, so the largest copy can be made). The
 * one test for nk_tex_scale_can_submit and nk_tex_scale_submit, so a caller told there is room is
 * never refused (a refused retry would decode again on every hit). */
static Job *room_for(uint64_t host) {
    Job *j = oldest(JOB_FREE);
    if (!j || (s_host_bytes && s_host_bytes + host > ((uint64_t)NK_TEX_SCALE_HOST_MB << 20))) return NULL;
    return j;
}

int nk_tex_scale_can_submit(int w, int h, int factor) {
    if (factor < 2 || factor > NK_TEX_SCALE_MAX || w <= 0 || h <= 0) return 0;
    if (!ensure_started()) return 0;
    SDL_LockMutex(s_mx);
    const int room = room_for(job_host_bytes(w, h, factor)) != NULL;
    SDL_UnlockMutex(s_mx);
    return room;
}

int nk_tex_scale_submit(uint64_t serial, uint64_t hash, const uint32_t *rgba, int w, int h,
                        int factor, int clamp_u, int clamp_v) {
    if (factor < 2 || factor > NK_TEX_SCALE_MAX || w <= 0 || h <= 0) return 0;
    if (!ensure_started()) return 0;
    SDL_LockMutex(s_mx);
    const uint64_t host = job_host_bytes(w, h, factor);
    Job *j = room_for(host);
    if (!j) {
        s_stats.refused_full++;
        SDL_UnlockMutex(s_mx);
        return 0;
    }
    j->src = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!j->src) {
        SDL_UnlockMutex(s_mx);
        return 0;
    }
    s_host_bytes += host;
    memcpy(j->src, rgba, (size_t)w * (size_t)h * sizeof(uint32_t));
    j->serial = serial;
    j->hash = hash;
    j->w = w; j->h = h; j->factor = factor;
    j->clamp_u = clamp_u; j->clamp_v = clamp_v;
    j->dst = NULL;
    j->failed = 0;
    j->seq = ++s_seq;
    s_stats.submitted++;
    if (s_sync) {
        j->state = JOB_RUNNING;
        run_job(j);
        if (!j->failed) s_stats.scaled++;
        j->state = JOB_DONE;
    } else {
        j->state = JOB_QUEUED;
        SDL_SignalCondition(s_work_cv);
    }
    SDL_UnlockMutex(s_mx);
    return 1;
}

int nk_tex_scale_take(NkTexScaleResult *out) {
    memset(out, 0, sizeof(*out));
    if (!s_started) return 0;
    SDL_LockMutex(s_mx);
    for (;;) {
        Job *j = oldest(JOB_DONE);
        if (!j) break;
        j->state = JOB_FREE;
        const uint64_t host = job_host_bytes(j->w, j->h, j->factor);
        s_host_bytes = host > s_host_bytes ? 0 : s_host_bytes - host;
        if (j->failed || !j->dst) {
            s_stats.discarded++;
            continue;
        }
        out->serial = j->serial;
        out->hash = j->hash;
        out->w = j->w; out->h = j->h; out->factor = j->factor;
        out->rgba = j->dst;
        j->dst = NULL;
        s_stats.taken++;
        SDL_UnlockMutex(s_mx);
        return 1;
    }
    SDL_UnlockMutex(s_mx);
    return 0;
}

void nk_tex_scale_release(NkTexScaleResult *res) {
    if (!res) return;
    free(res->rgba);
    res->rgba = NULL;
}

void nk_tex_scale_set_sync(int on) { s_sync = on ? 1 : 0; }

void nk_tex_scale_set_hold(int on) {
    if (!ensure_started()) return;
    SDL_LockMutex(s_mx);
    s_hold = on ? 1 : 0;
    SDL_BroadcastCondition(s_work_cv);
    SDL_UnlockMutex(s_mx);
}

void nk_tex_scale_wait_idle(void) {
    if (!s_started) return;
    SDL_LockMutex(s_mx);
    while (oldest(JOB_RUNNING) || (!s_hold && oldest(JOB_QUEUED))) SDL_WaitCondition(s_idle_cv, s_mx);
    SDL_UnlockMutex(s_mx);
}

void nk_tex_scale_shutdown(void) {
    if (!s_started) return;
    SDL_LockMutex(s_mx);
    s_quit = 1;
    SDL_BroadcastCondition(s_work_cv);
    SDL_UnlockMutex(s_mx);
    for (int i = 0; i < WORKERS; i++) {
        if (s_threads[i]) SDL_WaitThread(s_threads[i], NULL);
        s_threads[i] = NULL;
    }
    for (int i = 0; i < NK_TEX_SCALE_QUEUE; i++) {
        free(s_jobs[i].src);
        free(s_jobs[i].dst);
        memset(&s_jobs[i], 0, sizeof(s_jobs[i]));
    }
    SDL_DestroyCondition(s_work_cv);
    SDL_DestroyCondition(s_idle_cv);
    SDL_DestroyMutex(s_mx);
    s_work_cv = s_idle_cv = NULL;
    s_mx = NULL;
    s_started = 0;
    s_hold = 0;
    s_host_bytes = 0;
}

void nk_tex_scale_stats(NkTexScaleStats *out) {
    if (s_started) SDL_LockMutex(s_mx);
    *out = s_stats;
    if (s_started) SDL_UnlockMutex(s_mx);
    out->budget_used = s_budget_used;
    out->budget_peak = s_budget_peak;
}

void nk_tex_scale_report(void) {
    if (!s_cfg.factor && !s_cfg.smart2d) return;
    NkTexScaleStats st;
    nk_tex_scale_stats(&st);
    fprintf(stderr, "TEX_SCALE: exit (scaled=%llu, queue_full=%llu, discarded=%llu, budget_peak_mb=%llu, "
                    "worker_ms=%llu)\n",
            (unsigned long long)st.scaled, (unsigned long long)st.refused_full,
            (unsigned long long)st.discarded, (unsigned long long)(st.budget_peak >> 20),
            (unsigned long long)(st.scale_ns / 1000000ull));
}

/* ---- smart 2D filter ------------------------------------------------------------------- */

/* Texture coordinates are floats the guest wrote as integers, so a 1:1 draw may sit a little off
 * the texel lattice. The two filters agree at 1x only where the coordinate a pixel centre samples
 * is at or just above a texel centre: the linear rule (psp.frag) floors u - 0.5 to a 1/16 step,
 * so a centre even 1/4096 below takes 1/16 of the texel before, while nearest takes the texel
 * itself. The shader first rounds the coordinate to 1/4096, which lifts a value up to 1/8192
 * below a centre onto it. So every pixel centre's coordinate must lie in
 * [centre - S2D_BELOW, centre + S2D_ABOVE]: S2D_BELOW is half that rounding margin, left for a
 * host's interpolation error, and S2D_ABOVE stays well inside the 1/16 step. The map may also
 * drift at most S2D_EPS over the whole span: a bound on the slope alone would let a wide draw
 * drift by texels (480 pixels over 487 texels). The coordinate is linear across the draw, so the
 * first and last pixel centres bound every one between them. */
#define S2D_EPS   (1.0f / 64.0f)
#define S2D_BELOW (1.0f / 16384.0f)
#define S2D_ABOVE (1.0f / 32.0f)

static int on_centre(float u) {
    const float t = u - 0.5f, d = t - floorf(t + 0.5f);   /* signed offset from the nearest centre */
    return d >= -S2D_BELOW && d <= S2D_ABOVE;
}

int nk_smart2d_sprite(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    const float w = fabsf(x1 - x0), h = fabsf(y1 - y0);
    if (!(w >= 1.0f && h >= 1.0f)) return 0;
    /* one texel per pixel, either direction, over the whole span ... */
    if (fabsf(fabsf(u1 - u0) - w) > S2D_EPS || fabsf(fabsf(v1 - v0) - h) > S2D_EPS) return 0;
    /* ... and the first and last pixel centres (half a pixel in from each edge) on a texel centre */
    const float hu = (u1 - u0) * (0.5f / w), hv = (v1 - v0) * (0.5f / h);
    return on_centre(u0 + hu) && on_centre(u1 - hu) && on_centre(v0 + hv) && on_centre(v1 - hv);
}

int nk_smart2d_triangle(const float x[3], const float y[3], const float u[3], const float v[3],
                        const float rw[3]) {
    for (int i = 0; i < 3; i++)
        if (!(rw[i] != 0.0f) || fabsf(rw[i] - rw[0]) > 1e-6f * fabsf(rw[0])) return 0;
    float tu[3], tv[3];
    for (int i = 0; i < 3; i++) { tu[i] = u[i] / rw[i]; tv[i] = v[i] / rw[i]; }
    const float dx1 = x[1] - x[0], dx2 = x[2] - x[0];
    const float dy1 = y[1] - y[0], dy2 = y[2] - y[0];
    const float det = dx1 * dy2 - dx2 * dy1;
    if (fabsf(det) < 0.5f) return 0;               /* degenerate or under a pixel */
    const float du1 = tu[1] - tu[0], du2 = tu[2] - tu[0];
    const float dv1 = tv[1] - tv[0], dv2 = tv[2] - tv[0];
    const float dudx = (du1 * dy2 - du2 * dy1) / det, dudy = (dx1 * du2 - dx2 * du1) / det;
    const float dvdx = (dv1 * dy2 - dv2 * dy1) / det, dvdy = (dx1 * dv2 - dx2 * dv1) / det;
    /* The drift from a +/-1 map, summed over the bounding box, as for a sprite. */
    float xmin = x[0], xmax = x[0], ymin = y[0], ymax = y[0];
    for (int i = 1; i < 3; i++) {
        xmin = fminf(xmin, x[i]); xmax = fmaxf(xmax, x[i]);
        ymin = fminf(ymin, y[i]); ymax = fmaxf(ymax, y[i]);
    }
    const float bw = xmax - xmin, bh = ymax - ymin;
    if (fabsf(fabsf(dudx) - 1.0f) * bw + fabsf(dudy) * bh > S2D_EPS ||
        fabsf(dvdx) * bw + fabsf(fabsf(dvdy) - 1.0f) * bh > S2D_EPS)
        return 0;
    /* The pixel centres the box can hold span [cx0, cx1] x [cy0, cy1]; u and v are linear, so
     * their values at those four corners bound every centre the triangle covers. */
    const float cx0 = ceilf(xmin - 0.5f) + 0.5f, cx1 = floorf(xmax - 0.5f) + 0.5f;
    const float cy0 = ceilf(ymin - 0.5f) + 0.5f, cy1 = floorf(ymax - 0.5f) + 0.5f;
    if (cx1 < cx0 || cy1 < cy0) return 0;          /* covers no pixel centre */
    const float cx[2] = { cx0, cx1 }, cy[2] = { cy0, cy1 };
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            const float ex = cx[i] - x[0], ey = cy[j] - y[0];
            if (!on_centre(tu[0] + dudx * ex + dudy * ey) || !on_centre(tv[0] + dvdx * ex + dvdy * ey)) return 0;
        }
    return 1;
}
