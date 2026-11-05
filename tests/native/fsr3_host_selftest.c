/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * fsr3_host_selftest.c - FSR 3's CPU side (fsr3_host.c, fsr3_jitter.c) against AMD's own host code.
 *
 * F-K: tests/native/fsr3_host_oracle.txt is the record of AMD's unmodified fsr3upscalerDispatch
 * (FidelityFX SDK v2.4.0, built with MSVC) run through a recording backend: per frame its inputs,
 * every clear, barrier, discard and dispatch with its group counts, the constant buffers it stages
 * and the physical resource and mip behind each binding of the project's SPIR-V. This cell replays
 * the recorded inputs through fsr3_host_dispatch and requires the same record, line for line. AMD's
 * transition barriers before its discards have no job here (the Vulkan discard is a transition from
 * UNDEFINED), so each barrier line must name a resource the same frame discards, and is not replayed.
 *
 * J: the record's jitter phase counts and offsets, AMD's f32-to-f16 conversion for every sign and
 * exponent, and the RCAS constants for a sweep of sharpness. With FSR3_HOST_EXHAUSTIVE=1 the cell
 * also hashes the f16 conversion of all 2^32 floats against the record's hash.
 *
 * The record's provenance: tests/native/fsr3_host_oracle.md. No game data.
 */
#include "fsr3_host.h"
#include "fsr3_jitter.h"

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
static uint32_t float_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* ---- F-K: the per-frame record ---------------------------------------------------------------- */

#define MAX_LINES 512
#define LINE_LEN 512

typedef struct {
    char line[MAX_LINES][LINE_LEN];
    unsigned count, next;
} Expect;

static Expect s_expect;

static void emit(const char *fmt, ...) {
    va_list ap;
    if (s_expect.count == MAX_LINES) {
        CHECK(0, "more than %d lines for one frame", MAX_LINES);
        return;
    }
    va_start(ap, fmt);
    vsnprintf(s_expect.line[s_expect.count++], LINE_LEN, fmt, ap);
    va_end(ap);
}

static void emit_words(const char *tag, const void *data, size_t bytes) {
    char text[LINE_LEN];
    size_t at = (size_t)snprintf(text, sizeof(text), "%s", tag);
    const unsigned char *p = data;
    for (size_t i = 0; i + 4 <= bytes; i += 4) {
        uint32_t w;
        memcpy(&w, p + i, 4);
        at += (size_t)snprintf(text + at, sizeof(text) - at, " %08x", w);
    }
    emit("%s", text);
}

/* The binding sets a case has printed, per pass and sharpening label, numbered in order. */
#define MAX_SETS 64
static struct {
    char label[64];
    char text[2048];
} s_sets[MAX_SETS];
static unsigned s_set_count;

static void emit_dispatch(const Fsr3Host *h, const Fsr3Job *j, int sharpening) {
    const Fsr3PassLayout *l = &fsr3_pass_layout[j->pass];
    const char *label = (j->pass == FSR3_PASS_ACCUMULATE && sharpening) ? "FSR3-ACCUM_SHARP" : l->label;
    emit("dispatch %s %u %u 1", label, j->groups_x, j->groups_y);
    char text[2048];
    size_t at = 0;
    text[0] = 0;
    /* AMD's order: constant buffers, then SRVs, then UAVs, each in binding order. Constant buffer
     * n is the n-th one staged (main, SPD, RCAS). */
    for (uint32_t kind = FSR3_BIND_CBV; kind <= FSR3_BIND_UAV; kind++) {
        if (kind == FSR3_BIND_SAMPLER) continue;
        for (uint32_t i = 0; i < l->count; i++) {
            const Fsr3Binding *b = &l->bindings[i];
            if ((uint32_t)b->kind != kind) continue;
            uint32_t res, mip;
            if (kind == FSR3_BIND_CBV) {
                at += (size_t)snprintf(text + at, sizeof(text) - at, "  cb %s %u\n", b->name, b->id);
                continue;
            }
            fsr3_host_resolve(h, b, &res, &mip);
            if (kind == FSR3_BIND_SRV)
                at += (size_t)snprintf(text + at, sizeof(text) - at, "  srv %s %u\n", b->name, res);
            else
                at += (size_t)snprintf(text + at, sizeof(text) - at, "  uav %s %u %u\n", b->name, res, mip);
        }
    }
    unsigned p, k = 0;
    for (p = 0; p < s_set_count; p++) {
        if (strcmp(s_sets[p].label, label) != 0) continue;
        if (strcmp(s_sets[p].text, text) == 0) break;
        k++;
    }
    emit("  set %u", k);
    if (p == s_set_count) {
        if (s_set_count == MAX_SETS) {
            CHECK(0, "more than %d binding sets in one case", MAX_SETS);
            return;
        }
        snprintf(s_sets[p].label, sizeof(s_sets[p].label), "%s", label);
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

static void emit_frame(const Fsr3Host *h, int rc, int sharpening) {
    s_expect.count = s_expect.next = 0;
    if (rc != 0) {
        emit("result %d", rc);
        return;
    }
    int staged = 0;
    for (uint32_t i = 0; i < h->job_count; i++) {
        const Fsr3Job *j = &h->jobs[i];
        /* AMD stages the constant buffers after its clears and before its discards. */
        if (!staged && j->kind != FSR3_JOB_CLEAR) {
            emit_words("stage 0", &h->constants, sizeof(h->constants));
            emit_words("stage 1", &h->spd, sizeof(h->spd));
            emit_words("stage 2", &h->rcas, sizeof(h->rcas));
            staged = 1;
        }
        switch (j->kind) {
        case FSR3_JOB_CLEAR:
            emit("clear %u %08x %08x %08x %08x", j->resource, float_bits(j->value[0]), float_bits(j->value[1]),
                 float_bits(j->value[2]), float_bits(j->value[3]));
            break;
        case FSR3_JOB_DISCARD:
            emit("discard %u", j->resource);
            break;
        case FSR3_JOB_DISPATCH:
            emit_dispatch(h, j, sharpening);
            break;
        }
    }
    emit("result 0");
}

static int frame_discards(const Fsr3Host *h, uint32_t resource) {
    for (uint32_t i = 0; i < h->job_count; i++)
        if (h->jobs[i].kind == FSR3_JOB_DISCARD && h->jobs[i].resource == resource) return 1;
    return 0;
}

/* ---- the C library's float functions ----------------------------------------------------------
 * Five values come from <math.h> float functions whose last bit can differ between C libraries.
 * AMD's record was made with MSVC's UCRT, and a UCRT build (MSVC, MSYS2 UCRT64) must match it
 * exactly. Elsewhere those five are compared within LIBM_ULP units in the last place: glibc 2.39
 * rounds tanHalfFOV one unit the other way in some frames. Every other word, and every value those
 * functions feed that is an integer (the phase count, the SPD mip count), must match exactly. The
 * largest difference seen is printed. */
#if defined(_UCRT)
#define LIBM_ULP 0u
#else
#define LIBM_ULP 1u
#endif

typedef struct {
    const char *name;
    int stage;  /* constant buffer staged n-th, or -1 for the RCAS helper lines */
    int word;
    int halves; /* two f16 halves in one word: compared per half */
    uint32_t max_seen;
} LibmField;

static LibmField s_libm[] = {
    { "deviceToViewDepth[2] (cosf, sinf)", 0, 14, 0, 0 },
    { "deviceToViewDepth[3] (cosf, sinf)", 0, 15, 0, 0 },
    { "tanHalfFOV (tanf, atanf)", 0, 26, 0, 0 },
    { "rcas con.x (exp2f)", 2, 0, 0, 0 },
    { "rcas con.y (exp2f, as halves)", 2, 1, 1, 0 },
};

static uint32_t ulp_distance(uint32_t a, uint32_t b) {
    const int64_t ka = a < 0x80000000u ? (int64_t)a : (int64_t)0x80000000u - (int64_t)a;
    const int64_t kb = b < 0x80000000u ? (int64_t)b : (int64_t)0x80000000u - (int64_t)b;
    return (uint32_t)(ka > kb ? ka - kb : kb - ka);
}

/* 1 when word `word` of staged buffer `stage` is within the libm bound of AMD's value. */
static int libm_close(int stage, int word, uint32_t amd, uint32_t host) {
    for (size_t i = 0; i < sizeof(s_libm) / sizeof(s_libm[0]); i++) {
        LibmField *f = &s_libm[i];
        if (f->stage != stage || f->word != word) continue;
        uint32_t d;
        if (f->halves) {
            const uint32_t lo = (amd & 0xffffu) > (host & 0xffffu) ? (amd & 0xffffu) - (host & 0xffffu)
                                                                   : (host & 0xffffu) - (amd & 0xffffu);
            const uint32_t hi = (amd >> 16) > (host >> 16) ? (amd >> 16) - (host >> 16) : (host >> 16) - (amd >> 16);
            d = lo > hi ? lo : hi;
        } else {
            d = ulp_distance(amd, host);
        }
        if (d > f->max_seen) f->max_seen = d;
        return d <= LIBM_ULP;
    }
    return 0;
}

/* A record line against the host's: equal, or a staged buffer whose only differences are libm fields. */
static int same_line(const char *amd, const char *host) {
    if (strcmp(amd, host) == 0) return 1;
    int sa, sh, na, nh;
    if (sscanf(amd, "stage %d%n", &sa, &na) != 1 || sscanf(host, "stage %d%n", &sh, &nh) != 1 || sa != sh) return 0;
    const char *pa = amd + na, *ph = host + nh;
    for (int word = 0;; word++) {
        char *ea, *eh;
        const unsigned long a = strtoul(pa, &ea, 16), h = strtoul(ph, &eh, 16);
        if (ea == pa || eh == ph) return ea == pa && eh == ph;
        if (a != h && !libm_close(sa, word, (uint32_t)a, (uint32_t)h)) return 0;
        pa = ea;
        ph = eh;
    }
}

/* ---- J: jitter, f16 and RCAS ------------------------------------------------------------------ */

static void check_jitter(const char *line) {
    int i, n, e;
    unsigned x_bits, y_bits;
    if (sscanf(line, "jitter %d %d %d %x %x", &i, &n, &e, &x_bits, &y_bits) != 5) {
        CHECK(0, "unreadable: %s", line);
        return;
    }
    float x = -7.0f, y = -7.0f;
    const int rc = fsr3_jitter_offset(&x, &y, i, n);
    CHECK((rc == 0) == (e == 0), "jitter %d of %d: rc %d, AMD %d", i, n, rc, e);
    if (rc == 0 && e == 0)
        CHECK(float_bits(x) == x_bits && float_bits(y) == y_bits, "jitter %d of %d: %08x %08x, AMD %08x %08x", i, n,
              float_bits(x), float_bits(y), x_bits, y_bits);
}

static void check_f16(const char *line) {
    unsigned e, want[3];
    static const uint32_t mant[3] = { 0x0u, 0x2000u, 0x7fffffu };
    if (sscanf(line, "f16 %x %x %x %x", &e, &want[0], &want[1], &want[2]) != 4) {
        CHECK(0, "unreadable: %s", line);
        return;
    }
    for (int k = 0; k < 3; k++) {
        const uint32_t got = fsr3_f32_to_f16(bits_float((e << 23) | mant[k]));
        CHECK(got == want[k], "f16 of %08x: %04x, AMD %04x", (unsigned)((e << 23) | mant[k]), (unsigned)got, want[k]);
    }
}

static void check_f16_all(const char *line) {
    unsigned long long want;
    if (sscanf(line, "f16all %llx", &want) != 1) {
        CHECK(0, "unreadable: %s", line);
        return;
    }
    const char *env = getenv("FSR3_HOST_EXHAUSTIVE");
    if (!env || strcmp(env, "1") != 0) {
        printf("NOTE f16 over all 2^32 floats not run (FSR3_HOST_EXHAUSTIVE=1 runs it); every sign and exponent was checked\n");
        return;
    }
    unsigned long long h = 14695981039346656037ull;
    for (unsigned long long u = 0; u <= 0xffffffffull; u++) {
        const uint32_t r = fsr3_f32_to_f16(bits_float((uint32_t)u));
        h = (h ^ (r & 0xffu)) * 1099511628211ull;
        h = (h ^ (r >> 8)) * 1099511628211ull;
    }
    CHECK(h == want, "f16 over all 2^32 floats: hash %016llx, AMD %016llx", h, want);
    printf("f16 over all 2^32 floats: hash %016llx matches\n", h);
}

static void check_rcas(const char *line) {
    unsigned s, want[4];
    if (sscanf(line, "rcas %x %x %x %x %x", &s, &want[0], &want[1], &want[2], &want[3]) != 5) {
        CHECK(0, "unreadable: %s", line);
        return;
    }
    uint32_t con[4];
    fsr3_rcas_con(con, (-2.0f * bits_float(s)) + 2.0f);
    for (int k = 0; k < 4; k++)
        CHECK(con[k] == want[k] || libm_close(2, k, want[k], con[k]), "rcas %08x word %d: %08x, AMD %08x", s, k, con[k],
              want[k]);
}

/* ---- the replay --------------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: fsr3_host_selftest <fsr3_host_oracle.txt>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "FAIL cannot open %s\n", argv[1]);
        return 1;
    }
    static Fsr3Host host;
    static char line[4096];
    unsigned cases = 0, frames = 0, compared = 0, barriers = 0, helpers = 0;
    int in_frame = 0, sharpening = 0;
    while (fgets(line, sizeof(line), f)) {
        s_line++;
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "case ", 5)) {
            char name[64];
            Fsr3HostDesc desc;
            CHECK(!in_frame, "case inside a frame");
            if (sscanf(line, "case %63s flags %u max %u %u %u %u", name, &desc.flags, &desc.max_render_width,
                       &desc.max_render_height, &desc.max_upscale_width, &desc.max_upscale_height) != 6) {
                CHECK(0, "unreadable: %s", line);
                break;
            }
            fsr3_host_init(&host, &desc);
            s_set_count = 0;
            cases++;
        } else if (!strncmp(line, "frame ", 6)) {
            int i;
            unsigned u[17];
            Fsr3HostDispatch d;
            memset(&d, 0, sizeof(d));
            CHECK(!in_frame, "frame inside a frame");
            if (sscanf(line, "frame %d in %u %u %u %u %x %x %x %x %u %x %x %x %u %x %x %x %x %u %u %u", &i, &d.render_width,
                       &d.render_height, &d.upscale_width, &d.upscale_height, &u[0], &u[1], &u[2], &u[3], &u[4], &u[5],
                       &u[6], &u[7], &u[8], &u[9], &u[10], &u[11], &u[12], &u[13], &u[14], &u[15]) != 21) {
                CHECK(0, "unreadable: %s", line);
                break;
            }
            d.jitter_x = bits_float(u[0]);
            d.jitter_y = bits_float(u[1]);
            d.motion_vector_scale_x = bits_float(u[2]);
            d.motion_vector_scale_y = bits_float(u[3]);
            d.enable_sharpening = (int)u[4];
            d.sharpness = bits_float(u[5]);
            d.frame_time_delta_ms = bits_float(u[6]);
            d.pre_exposure = bits_float(u[7]);
            d.reset = (int)u[8];
            d.camera_near = bits_float(u[9]);
            d.camera_far = bits_float(u[10]);
            d.camera_fov_angle_vertical = bits_float(u[11]);
            d.view_space_to_meters_factor = bits_float(u[12]);
            d.has_exposure = (int)u[13];
            d.has_reactive = (int)u[14];
            d.has_transparency_and_composition = (int)u[15];
            sharpening = d.enable_sharpening;
            emit_frame(&host, fsr3_host_dispatch(&host, &d), sharpening);
            in_frame = 1;
            frames++;
        } else if (in_frame) {
            unsigned r, type, state;
            if (sscanf(line, "barrier %u %u %u", &r, &type, &state) == 3) {
                /* FFX_BARRIER_TYPE_TRANSITION (0) to FFX_API_RESOURCE_STATE_UNORDERED_ACCESS (2). */
                CHECK(type == 0 && state == 2 && frame_discards(&host, r), "barrier %u not before a discard of it", r);
                barriers++;
                continue;
            }
            if (s_expect.next == s_expect.count) {
                CHECK(0, "AMD has more: %s", line);
            } else {
                const char *mine = s_expect.line[s_expect.next++];
                CHECK(same_line(line, mine), "\n  AMD:  %s\n  host: %s", line, mine);
                compared++;
            }
            if (!strncmp(line, "result ", 7)) {
                CHECK(s_expect.next == s_expect.count, "host has more: %s", s_expect.line[s_expect.next]);
                in_frame = 0;
            }
        } else if (!strncmp(line, "phase ", 6)) {
            int rw, dw, n;
            CHECK(sscanf(line, "phase %d %d %d", &rw, &dw, &n) == 3, "unreadable: %s", line);
            CHECK(fsr3_jitter_phase_count(rw, dw) == n, "phase count %d -> %d: %d, AMD %d", rw, dw,
                  fsr3_jitter_phase_count(rw, dw), n);
            helpers++;
        } else if (!strncmp(line, "jitter ", 7)) {
            check_jitter(line);
            helpers++;
        } else if (!strncmp(line, "jitter0 ", 8) || !strncmp(line, "jitterneg ", 10)) {
            int e;
            float x, y;
            CHECK(sscanf(strchr(line, ' '), "%d", &e) == 1 && e != 0, "AMD accepted: %s", line);
            CHECK(fsr3_jitter_offset(&x, &y, 0, line[6] == '0' ? 0 : -4) == -1, "accepted: %s", line);
            helpers++;
        } else if (!strncmp(line, "f16 ", 4)) {
            check_f16(line);
            helpers++;
        } else if (!strncmp(line, "f16all ", 7)) {
            check_f16_all(line);
            helpers++;
        } else if (!strncmp(line, "rcas ", 5)) {
            check_rcas(line);
            helpers++;
        } else {
            CHECK(0, "unknown record: %s", line);
        }
        if (s_fail > 20) {
            fprintf(stderr, "stopping after %d failures\n", s_fail);
            break;
        }
    }
    fclose(f);
    CHECK(!in_frame, "the record ends inside a frame");
    CHECK(cases >= 18 && frames >= 100 && helpers >= 1000, "the record is short: %u cases, %u frames, %u helper lines",
          cases, frames, helpers);
    for (size_t i = 0; i < sizeof(s_libm) / sizeof(s_libm[0]); i++)
        printf("libm field %s: largest difference from AMD %u (bound %u)\n", s_libm[i].name, s_libm[i].max_seen, LIBM_ULP);
    printf("fsr3_host_selftest: %u cases, %u frames, %u lines matched, %u barriers checked, %u helper lines: %s\n", cases,
           frames, compared, barriers, helpers, s_fail ? "FAIL" : "ok");
    return s_fail ? 1 : 0;
}
