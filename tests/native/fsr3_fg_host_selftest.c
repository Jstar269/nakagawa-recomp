/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * fsr3_fg_host_selftest.c - FSR 3 frame generation's CPU side (fsr3_fg_host.c) against AMD's own
 * host code.
 *
 * G-K: tests/native/fsr3_fg_host_oracle.txt is the record of AMD's unmodified frame interpolation and
 * optical flow (FidelityFX SDK v2.4.0, ffx_frameinterpolation.cpp and ffx_opticalflow.cpp, built
 * with MSVC) run through a recording backend and driven the way AMD's frame generation provider
 * drives them. Per call it holds the inputs, then every constant buffer staged, every clear, barrier,
 * discard, copy and dispatch with its group counts, and the physical resource and mip behind each
 * binding of the project's SPIR-V. This cell replays the recorded calls through
 * fsr3_fg_host_configure, fsr3_fg_host_prepare and fsr3_fg_host_dispatch and requires the same
 * record, line for line, barriers included.
 *
 * Two kinds of word are compared more loosely, each for a stated reason:
 * - optical flow's SPD constants (cbOF_SPD) have three padding words AMD leaves uninitialised, so
 *   the record holds whatever was on its stack. The port writes 0, and the padding is not compared.
 *   No shader reads it: the one pass that declares cbOF_SPD reads only its first five words.
 * - three words of cbFI come from <math.h> float functions, compared as fsr3_host_selftest does.
 *
 * Then the refusals: flags the project's shaders were not built for, sizes of 0, render sizes above
 * the maximum, and generation rectangles outside the display, none of which AMD's record exercises.
 *
 * The record's provenance: tests/native/fsr3_fg_host_oracle.md. No game data.
 */
#include "fsr3_fg_host.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;
static unsigned s_line;

#define CHECK(cond, ...) do { \
        if (!(cond)) { s_fail++; fprintf(stderr, "FAIL line %u: ", s_line); \
                       fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
    } while (0)

static float bits_float(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* ---- the host's record of one call ------------------------------------------------------------ */

#define MAX_LINES 2048
#define LINE_LEN 512

static struct {
    char line[MAX_LINES][LINE_LEN];
    int stage_kind[MAX_LINES]; /* a stage line: its Fsr3FgStageKind; otherwise -1 */
    unsigned count, next;
} s_expect;

static void emit_kind(int stage_kind, const char *fmt, ...) {
    va_list ap;
    if (s_expect.count == MAX_LINES) {
        CHECK(0, "more than %d lines for one call", MAX_LINES);
        return;
    }
    s_expect.stage_kind[s_expect.count] = stage_kind;
    va_start(ap, fmt);
    vsnprintf(s_expect.line[s_expect.count++], LINE_LEN, fmt, ap);
    va_end(ap);
}
#define emit(...) emit_kind(-1, __VA_ARGS__)

/* The binding sets a case has printed, per pass, numbered in order. */
#define MAX_SETS 256
static struct {
    char label[64];
    char text[2048];
} s_sets[MAX_SETS];
static unsigned s_set_count;

static void emit_dispatch(const Fsr3FgJob *j) {
    const Fsr3FgPassLayout *l = &fsr3_fg_pass_layout[j->pass];
    emit("dispatch %s %u %u %u", l->label, j->groups[0], j->groups[1], j->groups[2]);
    char text[2048];
    size_t at = 0;
    text[0] = 0;
    /* AMD's order: constant buffers, SRV images, UAV images, SRV buffers, UAV buffers, each in the
     * order of the pass's bindings. */
    static const Fsr3FgBindKind order[] = { FSR3FG_BIND_CBV, FSR3FG_BIND_SRV, FSR3FG_BIND_UAV, FSR3FG_BIND_SRV_BUFFER,
                                            FSR3FG_BIND_UAV_BUFFER };
    for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
        for (uint32_t i = 0; i < l->count; i++) {
            const Fsr3FgBinding *b = &l->bindings[i];
            if (b->kind != order[k]) continue;
            switch (b->kind) {
            case FSR3FG_BIND_CBV:
                at += (size_t)snprintf(text + at, sizeof(text) - at, "  cb %s %d\n", b->name, (int)j->res[i]);
                break;
            case FSR3FG_BIND_SRV:
                at += (size_t)snprintf(text + at, sizeof(text) - at, "  srv %s %u\n", b->name, j->res[i]);
                break;
            case FSR3FG_BIND_UAV:
                at += (size_t)snprintf(text + at, sizeof(text) - at, "  uav %s %u %u\n", b->name, j->res[i], j->mip[i]);
                break;
            case FSR3FG_BIND_SRV_BUFFER:
                at += (size_t)snprintf(text + at, sizeof(text) - at, "  srvbuf %s %u\n", b->name, j->res[i]);
                break;
            case FSR3FG_BIND_UAV_BUFFER:
                at += (size_t)snprintf(text + at, sizeof(text) - at, "  uavbuf %s %u\n", b->name, j->res[i]);
                break;
            case FSR3FG_BIND_SAMPLER:
                break;
            }
        }
    }
    unsigned p, k = 0;
    for (p = 0; p < s_set_count; p++) {
        if (strcmp(s_sets[p].label, l->label) != 0) continue;
        if (strcmp(s_sets[p].text, text) == 0) break;
        k++;
    }
    emit("  set %u", k);
    if (p == s_set_count) {
        if (s_set_count == MAX_SETS) {
            CHECK(0, "more than %d binding sets in one case", MAX_SETS);
            return;
        }
        snprintf(s_sets[p].label, sizeof(s_sets[p].label), "%s", l->label);
        snprintf(s_sets[p].text, sizeof(s_sets[p].text), "%s", text);
        s_set_count++;
        for (char *line = text; *line;) {
            char *end = strchr(line, '\n');
            *end = 0;
            emit("%s", line);
            line = end + 1;
        }
    }
}

/* The most jobs and stages one call used, against the plan's fixed capacity. */
static uint32_t s_max_jobs, s_max_stages;

static void emit_call(const Fsr3FgHost *h, int rc) {
    s_expect.count = s_expect.next = 0;
    if (h->job_count > s_max_jobs) s_max_jobs = h->job_count;
    if (h->stage_count > s_max_stages) s_max_stages = h->stage_count;
    if (rc != 0) {
        emit("result %d", rc);
        return;
    }
    for (uint32_t i = 0; i < h->job_count; i++) {
        const Fsr3FgJob *j = &h->jobs[i];
        switch (j->kind) {
        case FSR3FG_JOB_STAGE: {
            const Fsr3FgStage *s = &h->stages[j->stage];
            char text[LINE_LEN];
            size_t at = (size_t)snprintf(text, sizeof(text), "stage %u", j->stage);
            for (uint32_t w = 0; w < s->count; w++) at += (size_t)snprintf(text + at, sizeof(text) - at, " %08x", s->words[w]);
            emit_kind((int)s->kind, "%s", text);
            break;
        }
        case FSR3FG_JOB_CLEAR:
            emit("clear %u %08x %08x %08x %08x", j->resource, j->value[0], j->value[1], j->value[2], j->value[3]);
            break;
        case FSR3FG_JOB_BARRIER:
            emit("barrier %u 0 %u", j->resource, j->state);
            break;
        case FSR3FG_JOB_DISCARD:
            emit("discard %u", j->resource);
            break;
        case FSR3FG_JOB_COPY:
            emit("copy %u %u 0 0 0", j->resource, j->dest);
            break;
        case FSR3FG_JOB_DISPATCH:
            emit_dispatch(j);
            break;
        }
    }
    emit("result 0");
}

/* ---- the looser words ---------------------------------------------------------------------------
 * cbFI's deviceToViewDepth[2] and [3] (cosf, sinf) and fTanHalfFOV (tanf, atanf) come from <math.h>
 * float functions whose last bit can differ between C libraries. AMD's record was made with MSVC's
 * UCRT, and a UCRT build (MSVC, MSYS2 UCRT64) must match it exactly. Elsewhere each is compared
 * within a bound worked out for that dispatch's own inputs, on the assumption that each float function
 * call may differ by one unit in the last place between two C libraries; that difference is carried
 * through the slope of what follows, and the operations after it may each round one more unit away.
 * - deviceToViewDepth[2] and [3] are aspect * tan(fov / 2) and tan(fov / 2), from cosf and sinf of
 *   fov / 2: a unit in either is a relative error of ulp(c) / c + ulp(s) / s in the result.
 * - fTanHalfFOV is tanf(atanf(tanf(fov / 2) * aspect)). A unit in the inner tanf and the rounding of the
 *   product reach atanf's input; atanf divides that by 1 + u^2 and adds its own unit; the outer tanf
 *   multiplies the angle's difference by 1 + T^2 and adds its own unit. Near 90 degrees that slope is
 *   about 4, so one unit from atanf becomes about four.
 * Every other word must match exactly. */
#if defined(_UCRT)
#define LIBM_EXACT 1
#else
#define LIBM_EXACT 0
#endif

typedef struct {
    const char *name;
    int word;
    uint32_t bound; /* this dispatch's bound away from UCRT */
    uint32_t max_seen, max_bound;
} LibmField;

static LibmField s_libm[] = {
    { "deviceToViewDepth[2] (cosf, sinf)", 14, 0, 0, 0 },
    { "deviceToViewDepth[3] (cosf, sinf)", 15, 0, 0, 0 },
    { "fTanHalfFOV (tanf, atanf, tanf)", 38, 0, 0, 0 },
};

static double ulp_of(double x) {
    const float a = fabsf((float)x);
    return (double)nextafterf(a, INFINITY) - (double)a;
}

/* A bound in units, rounded up; 0 (an exact match) where the inputs give no finite bound. */
static uint32_t units(double x) {
    return (isfinite(x) && x >= 0.0 && x < 1e6) ? (uint32_t)ceil(x) : 0u;
}

/* The bounds for a dispatch that uses prepared inputs p (fsr3_fg_host.c's fi_dispatch). */
static void libm_bounds(const Fsr3FgHostPrepare *p) {
    const double aspect = (double)((float)p->render_width / (float)p->render_height);
    const double half = (double)(0.5f * p->camera_fov_angle_vertical);
    const double c = cos(half), s = sin(half), t = s / c;
    const double relative = ulp_of(c) / fabs(c) + ulp_of(s) / fabs(s);
    const double v[2] = { aspect * t, t };
    /* cosf and sinf, then the division and the reciprocals (and the aspect's multiply) after them. */
    for (int k = 0; k < 2; k++) s_libm[k].bound = units(fabs(v[k]) * relative / ulp_of(v[k])) + 2u;
    const double ti = tan((double)p->camera_fov_angle_vertical / 2.0), u = ti * aspect;
    const double theta = atan(u), big_t = tan(theta);
    const double d_theta = ulp_of(theta) + (aspect * ulp_of(ti) + ulp_of(u)) / (1.0 + u * u);
    s_libm[2].bound = units((1.0 + big_t * big_t) * d_theta / ulp_of(big_t)) + 1u;
    for (int k = 0; k < 3; k++)
        if (s_libm[k].bound > s_libm[k].max_bound) s_libm[k].max_bound = s_libm[k].bound;
}

static uint32_t libm_bound(const LibmField *f) { return LIBM_EXACT ? 0u : f->bound; }

/* The padding of Fsr3OfSpdConstants, words 5 to 7, and how many differed from the record. */
#define OF_SPD_PAD_FIRST 5
#define OF_SPD_PAD_LAST 7
static unsigned s_pad_words;

static uint32_t ulp_distance(uint32_t a, uint32_t b) {
    const int64_t ka = a < 0x80000000u ? (int64_t)a : (int64_t)0x80000000u - (int64_t)a;
    const int64_t kb = b < 0x80000000u ? (int64_t)b : (int64_t)0x80000000u - (int64_t)b;
    return (uint32_t)(ka > kb ? ka - kb : kb - ka);
}

static int loose_word(int kind, int word, uint32_t amd, uint32_t host) {
    if (kind == FSR3FG_STAGE_OF_SPD && word >= OF_SPD_PAD_FIRST && word <= OF_SPD_PAD_LAST) {
        s_pad_words++;
        return 1;
    }
    if (kind != FSR3FG_STAGE_FI) return 0;
    for (size_t i = 0; i < sizeof(s_libm) / sizeof(s_libm[0]); i++) {
        LibmField *f = &s_libm[i];
        if (f->word != word) continue;
        const uint32_t d = ulp_distance(amd, host);
        if (d > f->max_seen) f->max_seen = d;
        return d <= libm_bound(f);
    }
    return 0;
}

/* A record line against the host's: equal, or a staged buffer whose only differences are loose words. */
static int same_line(const char *amd, const char *host, int kind) {
    if (strcmp(amd, host) == 0) return 1;
    int sa, sh, na, nh;
    if (kind < 0 || sscanf(amd, "stage %d%n", &sa, &na) != 1 || sscanf(host, "stage %d%n", &sh, &nh) != 1 || sa != sh)
        return 0;
    const char *pa = amd + na, *ph = host + nh;
    for (int word = 0;; word++) {
        char *ea, *eh;
        const unsigned long a = strtoul(pa, &ea, 16), h = strtoul(ph, &eh, 16);
        if (ea == pa || eh == ph) return ea == pa && eh == ph;
        if (a != h && !loose_word(kind, word, (uint32_t)a, (uint32_t)h)) return 0;
        pa = ea;
        ph = eh;
    }
}

/* ---- refusals ------------------------------------------------------------------------------------ */

static void refusals(void) {
    static Fsr3FgHost h;
    const Fsr3FgHostDesc good = { FSR3FG_HOST_ENABLE_DEPTH_INVERTED, 480, 272, 960, 544 };
    Fsr3FgHostDesc d = good;
    CHECK(fsr3_fg_host_init(&h, &d) == 0, "refusals: the good description was refused");
    /* AMD's FFX_FRAMEINTERPOLATION_ENABLE_* bits the shaders were not built for: texture1D, display-size
     * motion vectors, jittered motion vectors, async, debug. And no inverted depth. */
    static const uint32_t bad_flags[] = { 1u << 2, 1u << 4, 1u << 5, 1u << 6, 1u << 7, 1u << 8, 1u << 31 };
    for (size_t i = 0; i < sizeof(bad_flags) / sizeof(bad_flags[0]); i++) {
        d = good;
        d.flags |= bad_flags[i];
        CHECK(fsr3_fg_host_init(&h, &d) == -1, "refusals: flag %08x accepted", bad_flags[i]);
    }
    d = good;
    d.flags = 0;
    CHECK(fsr3_fg_host_init(&h, &d) == -1, "refusals: depth that is not inverted accepted");
    for (int k = 0; k < 4; k++) {
        d = good;
        uint32_t *size[] = { &d.max_render_width, &d.max_render_height, &d.display_width, &d.display_height };
        *size[k] = 0;
        CHECK(fsr3_fg_host_init(&h, &d) == -1, "refusals: size %d of 0 accepted", k);
    }

    d = good;
    CHECK(fsr3_fg_host_init(&h, &d) == 0, "refusals: init");
    Fsr3FgHostPrepare p;
    memset(&p, 0, sizeof(p));
    p.frame_id = 1;
    static const uint32_t render[][2] = { { 0, 272 }, { 480, 0 }, { 481, 272 }, { 480, 273 } };
    for (size_t i = 0; i < sizeof(render) / sizeof(render[0]); i++) {
        p.render_width = render[i][0];
        p.render_height = render[i][1];
        CHECK(fsr3_fg_host_prepare(&h, &p) == -1, "refusals: render size %ux%u accepted", render[i][0], render[i][1]);
    }
    p.render_width = 480;
    p.render_height = 272;
    CHECK(fsr3_fg_host_prepare(&h, &p) == 0, "refusals: the maximum render size was refused");

    Fsr3FgHostDispatch q;
    memset(&q, 0, sizeof(q));
    q.frame_id = 1;
    fsr3_fg_host_configure(&h, 1, 1);
    static const uint32_t bad_rect[][4] = {
        { 0, 0, 961, 544 }, { 0, 0, 960, 545 }, { 1, 0, 960, 544 }, { 0, 1, 960, 544 }, { 961, 0, 1, 1 },
        { 0, 545, 1, 1 }, { 0, 0, 0, 544 }, { 0, 0, 960, 0 }, { 0xffffffffu, 0, 2, 1 }, { 0, 0xffffffffu, 1, 2 },
    };
    for (size_t i = 0; i < sizeof(bad_rect) / sizeof(bad_rect[0]); i++) {
        memcpy(q.generation_rect, bad_rect[i], sizeof(q.generation_rect));
        CHECK(fsr3_fg_host_dispatch(&h, &q) == -1, "refusals: rectangle %u %u %u %u accepted", bad_rect[i][0],
              bad_rect[i][1], bad_rect[i][2], bad_rect[i][3]);
    }
    static const uint32_t good_rect[][4] = { { 0, 0, 0, 0 }, { 0, 0, 960, 544 }, { 959, 543, 1, 1 }, { 10, 20, 950, 524 } };
    for (size_t i = 0; i < sizeof(good_rect) / sizeof(good_rect[0]); i++) {
        memcpy(q.generation_rect, good_rect[i], sizeof(q.generation_rect));
        CHECK(fsr3_fg_host_dispatch(&h, &q) == 0, "refusals: rectangle %u %u %u %u refused", good_rect[i][0],
              good_rect[i][1], good_rect[i][2], good_rect[i][3]);
    }
    for (int i = 0; i < FSR3FG_PASS_COUNT; i++)
        CHECK(fsr3_fg_pass_layout[i].count <= FSR3FG_MAX_BINDINGS, "pass %d has %u bindings", i,
              fsr3_fg_pass_layout[i].count);
}

/* ---- the replay ----------------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: fsr3_fg_host_selftest <fsr3_fg_host_oracle.txt>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "FAIL cannot open %s\n", argv[1]);
        return 1;
    }
    static Fsr3FgHost host;
    static char line[8192];
    unsigned cases = 0, prepares = 0, dispatches = 0, configures = 0, compared = 0, barriers = 0;
    int in_call = 0;
    uint32_t debug_bar_index = 0;
    while (fgets(line, sizeof(line), f)) {
        s_line++;
        line[strcspn(line, "\r\n")] = 0;
        if (in_call) {
            if (!strncmp(line, "barrier ", 8)) barriers++;
            if (s_expect.next == s_expect.count) {
                CHECK(0, "AMD has more: %s", line);
            } else {
                const unsigned n = s_expect.next++;
                CHECK(same_line(line, s_expect.line[n], s_expect.stage_kind[n]), "\n  AMD:  %s\n  host: %s", line,
                      s_expect.line[n]);
                compared++;
            }
            if (!strncmp(line, "result ", 7)) {
                CHECK(s_expect.next == s_expect.count, "host has more: %s", s_expect.line[s_expect.next]);
                in_call = 0;
            }
        } else if (!strncmp(line, "case ", 5)) {
            char name[64];
            Fsr3FgHostDesc desc;
            if (sscanf(line, "case %63s flags %u max %u %u display %u %u", name, &desc.flags, &desc.max_render_width,
                       &desc.max_render_height, &desc.display_width, &desc.display_height) != 6) {
                CHECK(0, "unreadable: %s", line);
                break;
            }
            /* AMD's debug-bar colour index is one function-static for the whole process, and the
             * record is one process: it carries from case to case. */
            debug_bar_index = host.debug_bar_index;
            CHECK(fsr3_fg_host_init(&host, &desc) == 0, "init refused: %s", line);
            host.debug_bar_index = debug_bar_index;
            s_set_count = 0;
            cases++;
        } else if (!strncmp(line, "configure ", 10)) {
            unsigned long long id;
            int hudless;
            if (sscanf(line, "configure %llu %d", &id, &hudless) != 2) {
                CHECK(0, "unreadable: %s", line);
                break;
            }
            fsr3_fg_host_configure(&host, id, hudless);
            configures++;
        } else if (!strncmp(line, "prepare ", 8)) {
            unsigned long long id;
            unsigned u[9];
            int reset;
            Fsr3FgHostPrepare p;
            memset(&p, 0, sizeof(p));
            if (sscanf(line, "prepare %llu %u %u %x %x %x %x %x %x %x %x %x %d", &id, &p.render_width, &p.render_height,
                       &u[0], &u[1], &u[2], &u[3], &u[4], &u[5], &u[6], &u[7], &u[8], &reset) != 13) {
                CHECK(0, "unreadable: %s", line);
                break;
            }
            p.frame_id = id;
            p.jitter_x = bits_float(u[0]);
            p.jitter_y = bits_float(u[1]);
            p.motion_vector_scale_x = bits_float(u[2]);
            p.motion_vector_scale_y = bits_float(u[3]);
            p.frame_time_delta = bits_float(u[4]);
            p.camera_near = bits_float(u[5]);
            p.camera_far = bits_float(u[6]);
            p.camera_fov_angle_vertical = bits_float(u[7]);
            p.view_space_to_meters_factor = bits_float(u[8]);
            p.reset = reset;
            emit_call(&host, fsr3_fg_host_prepare(&host, &p));
            in_call = 1;
            prepares++;
        } else if (!strncmp(line, "dispatch ", 9)) {
            unsigned long long id;
            unsigned u[2];
            int reset;
            Fsr3FgHostDispatch d;
            memset(&d, 0, sizeof(d));
            if (sscanf(line, "dispatch %llu %d %u %u %u %u %u %x %x", &id, &reset, &d.generation_rect[0],
                       &d.generation_rect[1], &d.generation_rect[2], &d.generation_rect[3],
                       &d.backbuffer_transfer_function, &u[0], &u[1]) != 9) {
                CHECK(0, "unreadable: %s", line);
                break;
            }
            d.frame_id = id;
            d.reset = reset;
            d.min_max_luminance[0] = bits_float(u[0]);
            d.min_max_luminance[1] = bits_float(u[1]);
            const int rc = fsr3_fg_host_dispatch(&host, &d);
            /* The cbFI words this sets stay in the persistent constants, so the bounds hold for the
             * prepares that stage them next, too. */
            libm_bounds(&host.prepared[d.frame_id % (sizeof(host.prepared) / sizeof(host.prepared[0]))]);
            emit_call(&host, rc);
            in_call = 1;
            dispatches++;
        } else {
            CHECK(0, "unknown record: %s", line);
        }
        if (s_fail > 20) {
            fprintf(stderr, "stopping after %d failures\n", s_fail);
            break;
        }
    }
    fclose(f);
    CHECK(!in_call, "the record ends inside a call");
    CHECK(cases >= 17 && prepares >= 100 && dispatches >= 100 && barriers >= 1000,
          "the record is short: %u cases, %u prepares, %u dispatches, %u barriers", cases, prepares, dispatches, barriers);
    refusals();
    for (size_t i = 0; i < sizeof(s_libm) / sizeof(s_libm[0]); i++)
        printf("libm field %s: largest difference from AMD %u (%s; largest bound %u)\n", s_libm[i].name,
               s_libm[i].max_seen, LIBM_EXACT ? "UCRT, exact" : "per-dispatch bound", s_libm[i].max_bound);
    /* A plan that does not fit returns -1, which the record would show as a mismatch; this is the
     * headroom left, which a change to the plans must keep above 0. */
    CHECK(s_max_jobs < FSR3FG_MAX_JOBS && s_max_stages < FSR3FG_MAX_STAGES, "no headroom: %u jobs, %u stages", s_max_jobs,
          s_max_stages);
    printf("largest plan: %u of %u jobs, %u of %u stages\n", s_max_jobs, FSR3FG_MAX_JOBS, s_max_stages, FSR3FG_MAX_STAGES);
    printf("cbOF_SPD padding words that differ from the record (AMD's uninitialised stack): %u\n", s_pad_words);
    printf("fsr3_fg_host_selftest: %u cases, %u configures, %u prepares, %u dispatches, %u lines matched (%u barriers): %s\n",
           cases, configures, prepares, dispatches, compared, barriers, s_fail ? "FAIL" : "ok");
    return s_fail ? 1 : 0;
}
