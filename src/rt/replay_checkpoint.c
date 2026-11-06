// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Replay checkpoints (#2147). The format and the rules are in replay_checkpoint.h. */

#include "replay_checkpoint.h"
#include "vfpu_tables.h" /* sr_vfpu_sha256: the runtime's SHA-256 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int name_ok(const char *name) {
    size_t n = 0;
    if (!name) return 0;
    for (; name[n]; n++) {
        const char c = name[n];
        if (n + 1u >= NK_CKPT_NAME_LEN) return 0;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return n > 0u;
}

void nk_ckpt_begin(NkCheckpoint *ckpt, uint64_t frame) {
    ckpt->frame = frame;
    ckpt->count = 0u;
    ckpt->overflow = 0;
}

static NkCkptDigest *next_slot(NkCheckpoint *ckpt, const char *name) {
    if (ckpt->overflow || !name_ok(name) || ckpt->count >= NK_CKPT_MAX_DIGESTS) {
        ckpt->overflow = 1;
        return NULL;
    }
    NkCkptDigest *d = &ckpt->digests[ckpt->count++];
    memset(d->name, 0, sizeof d->name);
    memcpy(d->name, name, strlen(name));
    return d;
}

int nk_ckpt_add(NkCheckpoint *ckpt, const char *name, const void *bytes, size_t len) {
    NkCkptDigest *d = next_slot(ckpt, name);
    if (!d) return 0;
    sr_vfpu_sha256(len ? (const uint8_t *)bytes : (const uint8_t *)"", len, d->sha256);
    return 1;
}

int nk_ckpt_add_cpu(NkCheckpoint *ckpt, uint32_t uid, const CpuState *cpu) {
    uint8_t le[sizeof(CpuState)];
    const uint32_t *words = (const uint32_t *)(const void *)cpu;
    char name[NK_CKPT_NAME_LEN];
    for (size_t w = 0; w < sizeof(CpuState) / 4u; w++) {
        le[w * 4u] = (uint8_t)words[w];
        le[w * 4u + 1u] = (uint8_t)(words[w] >> 8);
        le[w * 4u + 2u] = (uint8_t)(words[w] >> 16);
        le[w * 4u + 3u] = (uint8_t)(words[w] >> 24);
    }
    snprintf(name, sizeof name, "cpu.%08" PRIx32, uid);
    return nk_ckpt_add(ckpt, name, le, sizeof le);
}

int nk_ckpt_add_chunks(NkCheckpoint *ckpt, const char *prefix, uint32_t guest_addr,
                       const uint8_t *host, size_t size, size_t chunk) {
    char name[NK_CKPT_NAME_LEN];
    if (chunk == 0u || !prefix || strlen(prefix) > NK_CKPT_NAME_LEN - 10u) {
        ckpt->overflow = 1;
        return 0;
    }
    for (size_t off = 0; off < size; off += chunk) {
        const size_t n = size - off < chunk ? size - off : chunk;
        if ((uint64_t)guest_addr + off > UINT32_MAX) {
            ckpt->overflow = 1;
            return 0;
        }
        snprintf(name, sizeof name, "%s.%08" PRIx32, prefix, (uint32_t)(guest_addr + off));
        if (!nk_ckpt_add(ckpt, name, host + off, n)) return 0;
    }
    return 1;
}

size_t nk_ckpt_format(const NkCheckpoint *ckpt, char *out, size_t cap) {
    static const char hex[] = "0123456789abcdef";
    size_t len;
    int n;
    if (ckpt->overflow || cap == 0u) return 0u;
    n = snprintf(out, cap, NK_CKPT_LINE_PREFIX "frame=%" PRIu64 " n=%" PRIu32, ckpt->frame, ckpt->count);
    if (n < 0 || (size_t)n >= cap) return 0u;
    len = (size_t)n;
    for (uint32_t i = 0; i < ckpt->count; i++) {
        const size_t name_len = strlen(ckpt->digests[i].name);
        if (cap - len < 1u + name_len + 1u + 64u + 2u) return 0u;
        out[len++] = ' ';
        memcpy(out + len, ckpt->digests[i].name, name_len);
        len += name_len;
        out[len++] = '=';
        for (unsigned b = 0; b < 32u; b++) {
            out[len++] = hex[ckpt->digests[i].sha256[b] >> 4];
            out[len++] = hex[ckpt->digests[i].sha256[b] & 15u];
        }
    }
    if (cap - len < 2u) return 0u;
    out[len++] = '\n';
    out[len] = '\0';
    return len;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int fail(char *detail, size_t cap, const char *why) {
    if (detail && cap) snprintf(detail, cap, "%s", why);
    return 0;
}

int nk_ckpt_parse(const char *line, NkCheckpoint *out, char *detail, size_t detail_cap) {
    const size_t prefix_len = strlen(NK_CKPT_LINE_PREFIX);
    const char *p;
    char *end;
    unsigned long long frame, count;

    nk_ckpt_begin(out, 0u);
    if (strncmp(line, NK_CKPT_LINE_PREFIX, prefix_len) != 0) return fail(detail, detail_cap, "not an NKCKPT v=1 line");
    p = line + prefix_len;
    if (strncmp(p, "frame=", 6) != 0) return fail(detail, detail_cap, "no frame=");
    p += 6;
    if (*p < '0' || *p > '9') return fail(detail, detail_cap, "bad frame");
    frame = strtoull(p, &end, 10);
    p = end;
    if (strncmp(p, " n=", 3) != 0) return fail(detail, detail_cap, "no n=");
    p += 3;
    if (*p < '0' || *p > '9') return fail(detail, detail_cap, "bad n");
    count = strtoull(p, &end, 10);
    p = end;
    if (count > NK_CKPT_MAX_DIGESTS) return fail(detail, detail_cap, "too many digests");
    out->frame = (uint64_t)frame;
    for (unsigned long long i = 0; i < count; i++) {
        char name[NK_CKPT_NAME_LEN];
        size_t n = 0;
        if (*p++ != ' ') return fail(detail, detail_cap, "digest list is short");
        while (*p && *p != '=' && n + 1u < NK_CKPT_NAME_LEN) name[n++] = *p++;
        name[n] = '\0';
        if (*p++ != '=' || !name_ok(name)) return fail(detail, detail_cap, "bad digest name");
        NkCkptDigest *d = next_slot(out, name);
        if (!d) return fail(detail, detail_cap, "bad digest name");
        for (unsigned b = 0; b < 32u; b++) {
            const int hi = hexval(p[0]), lo = hi < 0 ? -1 : hexval(p[1]);
            if (hi < 0 || lo < 0) return fail(detail, detail_cap, "bad digest hex");
            d->sha256[b] = (uint8_t)((hi << 4) | lo);
            p += 2;
        }
    }
    if (*p == '\n') p++;
    if (*p != '\0') return fail(detail, detail_cap, "trailing text after the digests");
    return 1;
}

NkCkptDiff nk_ckpt_compare(const NkCheckpoint *expected, const NkCheckpoint *actual, NkCkptMismatch *out) {
    NkCkptMismatch m;
    memset(&m, 0, sizeof m);
    m.frame = expected->frame;
    if (expected->frame != actual->frame) {
        m.kind = NK_CKPT_FRAME;
    } else {
        for (uint32_t i = 0; i < expected->count || i < actual->count; i++) {
            if (i >= expected->count || i >= actual->count ||
                strcmp(expected->digests[i].name, actual->digests[i].name) != 0) {
                m.kind = NK_CKPT_NAMES;
                if (i < expected->count) memcpy(m.name, expected->digests[i].name, NK_CKPT_NAME_LEN);
                else memcpy(m.name, actual->digests[i].name, NK_CKPT_NAME_LEN);
                break;
            }
            if (memcmp(expected->digests[i].sha256, actual->digests[i].sha256, 32u) != 0) {
                m.kind = NK_CKPT_DIGEST;
                memcpy(m.name, expected->digests[i].name, NK_CKPT_NAME_LEN);
                break;
            }
        }
    }
    if (out) *out = m;
    return m.kind;
}

/* ---- the runtime's checkpoint stream ---------------------------------------------------- */

#define CKPT_CHUNK ((size_t)1u << 20)
#define CKPT_VRAM_BASE 0x04000000u
#define CKPT_VRAM_SIZE 0x00200000u
#define CKPT_RAM_SIZE 0x04000000u

static int s_ckpt_on = -1; /* -1 not read yet, 0 off, 1 on */
static FILE *s_ckpt_fp;
static uint32_t s_ckpt_every = 60u;
static uint32_t s_ckpt_last_window;
static int s_ckpt_have_window;
static NkCheckpoint s_ckpt;
static char s_ckpt_line[NK_CKPT_MAX_DIGESTS * (NK_CKPT_NAME_LEN + 66u) + 64u];

typedef struct {
    NkCheckpoint *ckpt;
    uint8_t sched[16u + 16u * 256u];
    size_t sched_len;
    int sched_overflow;
} CkptVisit;

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void ckpt_visit_thread(void *ctx, uint32_t uid, int state, int priority, uint32_t wait_obj,
                              const CpuState *cpu) {
    CkptVisit *v = (CkptVisit *)ctx;
    nk_ckpt_add_cpu(v->ckpt, uid, cpu);
    if (v->sched_len + 16u > sizeof v->sched) {
        v->sched_overflow = 1;
        return;
    }
    put_le32(v->sched + v->sched_len, uid);
    put_le32(v->sched + v->sched_len + 4u, (uint32_t)state);
    put_le32(v->sched + v->sched_len + 8u, (uint32_t)priority);
    put_le32(v->sched + v->sched_len + 12u, wait_obj);
    v->sched_len += 16u;
}

static void ckpt_setup(void) {
    const char *path = getenv("SR_REPLAY_CHECKPOINTS");
    const char *every = getenv("SR_REPLAY_CHECKPOINT_EVERY");
    s_ckpt_on = 0;
    if (!path || !path[0]) return;
    if (every && every[0]) {
        char *end = NULL;
        unsigned long v = strtoul(every, &end, 10);
        if (*end != '\0' || v < 1ul || v > 100000ul) {
            fprintf(stderr, "SR_REPLAY_CHECKPOINT_EVERY: invalid value '%s'; must be 1..100000\n", every);
            fflush(stderr);
            exit(1);
        }
        s_ckpt_every = (uint32_t)v;
    }
    s_ckpt_fp = fopen(path, "wb");
    if (!s_ckpt_fp) {
        fprintf(stderr, "SR_REPLAY_CHECKPOINTS: cannot create '%s'\n", path);
        fflush(stderr);
        exit(1);
    }
    fprintf(s_ckpt_fp, "NKCKPT_RUN v=1 every=%" PRIu32 " clock=%s\n", s_ckpt_every,
            sched_clock_deterministic() ? "deterministic" : "host");
    fflush(s_ckpt_fp);
    s_ckpt_on = 1;
}

void sr_replay_checkpoint_vblank(uint32_t vcount) {
    if (s_ckpt_on == 0) return;
    if (s_ckpt_on < 0) {
        ckpt_setup();
        if (!s_ckpt_on) return;
    }
    /* The first VBLANK of each window, so a tick that steps over a multiple still takes one. */
    const uint32_t window = vcount / s_ckpt_every;
    if (s_ckpt_have_window && window == s_ckpt_last_window) return;
    s_ckpt_have_window = 1;
    s_ckpt_last_window = window;

    static CkptVisit visit;
    visit.ckpt = &s_ckpt;
    visit.sched_len = 16u;
    visit.sched_overflow = 0;
    nk_ckpt_begin(&s_ckpt, vcount);
    {
        const uint64_t vtime = sched_vtime_us();
        put_le32(visit.sched, (uint32_t)vtime);
        put_le32(visit.sched + 4u, (uint32_t)(vtime >> 32));
        put_le32(visit.sched + 8u, vcount);
        put_le32(visit.sched + 12u, 0u);
    }
    sched_for_each_thread_cpu(ckpt_visit_thread, &visit);
    if (visit.sched_overflow) s_ckpt.overflow = 1;
    nk_ckpt_add(&s_ckpt, "sched", visit.sched, visit.sched_len);
    nk_ckpt_add_chunks(&s_ckpt, "vram", CKPT_VRAM_BASE, SR_HOST_LINEAR(CKPT_VRAM_BASE), CKPT_VRAM_SIZE, CKPT_CHUNK);
    nk_ckpt_add_chunks(&s_ckpt, "ram", SR_RAM_BASE, g_mem, CKPT_RAM_SIZE, CKPT_CHUNK);
    if (!nk_ckpt_format(&s_ckpt, s_ckpt_line, sizeof s_ckpt_line)) {
        /* Never a silent gap: the stream ends here, and a diff reports it as ended early. */
        fprintf(stderr, "SR_REPLAY_CHECKPOINTS: checkpoint at vcount %" PRIu32 " did not fit; stream stopped\n",
                vcount);
        fflush(stderr);
        fclose(s_ckpt_fp);
        s_ckpt_fp = NULL;
        s_ckpt_on = 0;
        return;
    }
    fputs(s_ckpt_line, s_ckpt_fp);
    fflush(s_ckpt_fp);
}

const char *nk_ckpt_diff_name(NkCkptDiff diff) {
    switch (diff) {
    case NK_CKPT_SAME: return "same";
    case NK_CKPT_FRAME: return "frame";
    case NK_CKPT_NAMES: return "names";
    case NK_CKPT_DIGEST: return "digest";
    }
    return "unknown";
}
