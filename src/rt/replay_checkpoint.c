// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Replay checkpoints, recording and replay (#2146, #2147). The format and the rules are in
 * replay_checkpoint.h. */

#include "replay_checkpoint.h"
#include "osk_text_entry.h" /* SR_OSK_TEXT_PENDING */
#include "vfpu_tables.h"    /* sr_vfpu_sha256: the runtime's SHA-256 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> /* GetModuleFileNameW: the executable's identity */
#endif

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

/* ---- taking a checkpoint of the running guest ------------------------------------------ */

#define CKPT_CHUNK ((size_t)1u << 20)
#define CKPT_VRAM_BASE 0x04000000u
#define CKPT_VRAM_SIZE 0x00200000u
#define CKPT_RAM_SIZE 0x04000000u

static NkCheckpoint s_ckpt;
static int s_ckpt_taken;          /* s_ckpt holds this VBLANK's checkpoint */
static uint32_t s_ckpt_taken_vcount;
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

/* This VBLANK's checkpoint in s_ckpt, taken once however many consumers want it. Returns 0
 * when it overflowed (more threads than a checkpoint holds). */
static int ckpt_take(uint32_t vcount) {
    if (s_ckpt_taken && s_ckpt_taken_vcount == vcount) return !s_ckpt.overflow;
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
    s_ckpt_taken = 1;
    s_ckpt_taken_vcount = vcount;
    return !s_ckpt.overflow;
}

/* Each consumer takes a checkpoint at the first VBLANK of each window of `every`, so a tick
 * that steps over a multiple still takes one. */
typedef struct {
    uint32_t every;
    uint32_t last_window;
    int have_window;
} CkptCadence;

static int cadence_due(CkptCadence *c, uint32_t vcount) {
    const uint32_t window = vcount / c->every;
    if (c->have_window && window == c->last_window) return 0;
    c->have_window = 1;
    c->last_window = window;
    return 1;
}

typedef enum {
    RR_UNREAD = -1,
    RR_OFF = 0,
    RR_RECORD,
    RR_REPLAY,   /* serving the recording and checking its checkpoints */
    RR_COMPLETE, /* the recording is used up; the run is live from here */
} RrMode;

static RrMode s_rr = RR_UNREAD;

/* Stop the run with one line. The exit handler then writes nothing more: a recording keeps
 * its last checkpoint as its end, and a failed replay prints no second verdict. */
static void die(const char *line) {
    s_rr = RR_OFF;
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
    exit(1);
}

/* SR_REPLAY_CHECKPOINT_EVERY, 1..100000; another value stops the run. */
static uint32_t read_every(void) {
    const char *every = getenv("SR_REPLAY_CHECKPOINT_EVERY");
    if (!every || !every[0]) return 60u;
    char *end = NULL;
    unsigned long v = strtoul(every, &end, 10);
    if (*end != '\0' || v < 1ul || v > 100000ul) {
        fprintf(stderr, "SR_REPLAY_CHECKPOINT_EVERY: invalid value '%s'; must be 1..100000\n", every);
        fflush(stderr);
        exit(1);
    }
    return (uint32_t)v;
}

/* ---- SR_REPLAY_CHECKPOINTS: the plain checkpoint stream -------------------------------- */

static int s_stream_on = -1; /* -1 not read yet, 0 off, 1 on */
static FILE *s_stream_fp;
static CkptCadence s_stream_cadence;

static void stream_setup(void) {
    const char *path = getenv("SR_REPLAY_CHECKPOINTS");
    s_stream_on = 0;
    if (!path || !path[0]) return;
    s_stream_cadence.every = read_every();
    s_stream_fp = fopen(path, "wb");
    if (!s_stream_fp) {
        fprintf(stderr, "SR_REPLAY_CHECKPOINTS: cannot create '%s'\n", path);
        fflush(stderr);
        exit(1);
    }
    fprintf(s_stream_fp, "NKCKPT_RUN v=1 every=%" PRIu32 " clock=%s\n", s_stream_cadence.every,
            sched_clock_deterministic() ? "deterministic" : "host");
    fflush(s_stream_fp);
    s_stream_on = 1;
}

static void stream_vblank(uint32_t vcount) {
    if (s_stream_on < 0) stream_setup();
    if (!s_stream_on || !cadence_due(&s_stream_cadence, vcount)) return;
    if (!ckpt_take(vcount) || !nk_ckpt_format(&s_ckpt, s_ckpt_line, sizeof s_ckpt_line)) {
        /* Never a silent gap: the stream ends here, and a diff reports it as ended early. */
        fprintf(stderr, "SR_REPLAY_CHECKPOINTS: checkpoint at vcount %" PRIu32 " did not fit; stream stopped\n",
                vcount);
        fflush(stderr);
        fclose(s_stream_fp);
        s_stream_fp = NULL;
        s_stream_on = 0;
        return;
    }
    fputs(s_ckpt_line, s_stream_fp);
    fflush(s_stream_fp);
}

/* ---- SR_RECORD / SR_REPLAY ------------------------------------------------------------- */

#define REC_DISC_LEN 16u /* the nkstate disc ID field: 15 characters and a terminator */

/* A growable byte buffer. */
typedef struct {
    uint8_t *data;
    size_t len, cap;
} RecBuf;

static int rec_buf_put(RecBuf *b, const void *bytes, size_t n) {
    if (n > SIZE_MAX - b->len) return 0;
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap : 4096u;
        while (ncap < b->len + n) {
            if (ncap > SIZE_MAX / 2u) return 0;
            ncap *= 2u;
        }
        uint8_t *p = (uint8_t *)realloc(b->data, ncap);
        if (!p) return 0;
        b->data = p;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
    return 1;
}

typedef enum { ANSW_OK, ANSW_CANCEL, ANSW_ABANDONED, ANSW_OPEN } AnswResult;

static const char *const k_answ_result[] = {"ok", "cancel", "abandoned", "open"};

typedef struct {
    uint32_t request;
    uint32_t pending;
    AnswResult result;
    uint32_t nunits;
    wchar_t units[SR_REPLAY_OSK_UNITS];
} OskAnswer;

static CkptCadence s_rr_cadence;
static uint32_t s_rr_vcount;        /* the last VBLANK the hook saw */
static int s_rr_seen_vblank;

/* Recording. */
static FILE *s_rec_fp;

/* The keyboard request in progress, both modes: requests are numbered from 0 in call order,
 * and `pending` counts the polls this request has answered "still open". */
static uint32_t s_osk_request;
static uint32_t s_osk_pending;

/* Replay. */
static char *s_rep_text;
static char **s_rep_ckpt;           /* the recorded checkpoint lines, in order */
static size_t s_rep_ckpt_count;
static size_t s_rep_ckpt_next;
static OskAnswer *s_rep_answ;       /* indexed by request number */
static size_t s_rep_answ_count;
static uint64_t s_rep_end;          /* the recording's last frame */
static int s_rep_have_end_line;
static NkCheckpoint s_rep_expect;

/* The executable's identity: SHA-256 over its size (8 bytes, little-endian) followed by the
 * SHA-256 of each 1 MiB block of the file, so it is computed without holding the file. */
static int exe_identity(char hex[65]) {
    FILE *fp = NULL;
#if defined(_WIN32)
    wchar_t path[4096];
    DWORD n = GetModuleFileNameW(NULL, path, (DWORD)(sizeof path / sizeof path[0]));
    if (n == 0 || n >= sizeof path / sizeof path[0]) return 0;
    fp = _wfopen(path, L"rb");
#elif defined(__linux__)
    fp = fopen("/proc/self/exe", "rb");
#endif
    if (!fp) return 0;
    const size_t block = (size_t)1u << 20;
    uint8_t *buf = (uint8_t *)malloc(block);
    RecBuf digests = {0};
    uint8_t size_le[8] = {0};
    uint64_t total = 0;
    int ok = buf != NULL && rec_buf_put(&digests, size_le, sizeof size_le);
    while (ok) {
        size_t got = fread(buf, 1, block, fp);
        if (got == 0) break;
        uint8_t d[32];
        sr_vfpu_sha256(buf, got, d);
        ok = rec_buf_put(&digests, d, sizeof d);
        total += got;
    }
    if (ferror(fp)) ok = 0;
    fclose(fp);
    free(buf);
    if (ok) {
        for (unsigned i = 0; i < 8u; i++) digests.data[i] = (uint8_t)(total >> (8u * i));
        uint8_t d[32];
        sr_vfpu_sha256(digests.data, digests.len, d);
        for (unsigned i = 0; i < 32u; i++) snprintf(hex + 2u * i, 3u, "%02x", d[i]);
    }
    free(digests.data);
    return ok;
}

/* SR_DISC_ID as a recording field: "-" when unset, else 1..15 of [A-Za-z0-9_-]. */
static int disc_field(char out[REC_DISC_LEN]) {
    const char *disc = getenv("SR_DISC_ID");
    if (!disc || !disc[0]) {
        strcpy(out, "-");
        return 1;
    }
    size_t n = strlen(disc);
    if (n >= REC_DISC_LEN) return 0;
    for (size_t i = 0; i < n; i++) {
        const char c = disc[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    memcpy(out, disc, n + 1u);
    return 1;
}

static void rr_exit(void);

static void record_setup(const char *path) {
    char exe[65], disc[REC_DISC_LEN];
    if (!exe_identity(exe)) die("SR_RECORD: this executable's identity could not be read; nothing is recorded");
    if (!disc_field(disc)) die("SR_RECORD: SR_DISC_ID is not 1..15 of [A-Za-z0-9_-]; nothing is recorded");
    s_rec_fp = fopen(path, "wb");
    if (!s_rec_fp) {
        fprintf(stderr, "SR_RECORD: cannot create '%s'\n", path);
        fflush(stderr);
        exit(1);
    }
    fprintf(s_rec_fp, "NKREC v=1 every=%" PRIu32 " clock=deterministic exe=%s disc=%s abi=%u cpustate=%u\n",
            s_rr_cadence.every, exe, disc, (unsigned)SR_CPUSTATE_ABI_VERSION, (unsigned)sizeof(CpuState));
    fflush(s_rec_fp);
}

static void refuse(const char *reason, size_t line) {
    s_rr = RR_OFF;
    fprintf(stderr, "REPLAY_REFUSED reason=%s line=%zu\n", reason, line);
    fflush(stderr);
    exit(1);
}

/* "key=" at *p: returns the value's start and moves *p past the value and one space. */
static const char *field(char **p, const char *key) {
    const size_t n = strlen(key);
    if (strncmp(*p, key, n) != 0) return NULL;
    char *v = *p + n;
    char *end = strchr(v, ' ');
    if (end) {
        *end = '\0';
        *p = end + 1;
    } else {
        *p = v + strlen(v);
    }
    return v;
}

static int parse_u64(const char *s, uint64_t *out) {
    if (!s || !*s) return 0;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        if (v > (UINT64_MAX - 9u) / 10u) return 0;
        v = v * 10u + (uint64_t)(*s - '0');
    }
    *out = v;
    return 1;
}

static int parse_u32(const char *s, uint32_t *out) {
    uint64_t v;
    if (!parse_u64(s, &v) || v > UINT32_MAX) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int push_ptr(char ***arr, size_t *count, size_t *cap, char *p) {
    if (*count == *cap) {
        size_t ncap = *cap ? *cap * 2u : 64u;
        char **n = (char **)realloc(*arr, ncap * sizeof **arr);
        if (!n) return 0;
        *arr = n;
        *cap = ncap;
    }
    (*arr)[(*count)++] = p;
    return 1;
}

static int parse_answer(char *p, OskAnswer *a) {
    const char *call = field(&p, "call=");
    const char *request = field(&p, "request=");
    const char *pending = field(&p, "pending=");
    const char *result = field(&p, "result=");
    const char *units = field(&p, "units=");
    if (!call || strcmp(call, "osk") != 0 || !parse_u32(request, &a->request) ||
        !parse_u32(pending, &a->pending) || !result || !units || *p)
        return 0;
    int r = -1;
    for (int i = 0; i < 4; i++)
        if (strcmp(result, k_answ_result[i]) == 0) r = i;
    if (r < 0) return 0;
    a->result = (AnswResult)r;
    a->nunits = 0;
    if (strcmp(units, "-") == 0) {
        a->units[0] = L'\0';
        return 1; /* no text: cancelled, abandoned, open, or confirmed empty */
    }
    if (a->result != ANSW_OK) return 0;
    const size_t len = strlen(units);
    if (len % 8u != 0 || len / 8u >= SR_REPLAY_OSK_UNITS) return 0;
    for (size_t i = 0; i < len / 8u; i++) {
        uint32_t v = 0;
        for (size_t k = 0; k < 8u; k++) {
            const int h = hexval(units[i * 8u + k]);
            if (h < 0 || (units[i * 8u + k] >= 'A' && units[i * 8u + k] <= 'F')) return 0;
            v = (v << 4) | (uint32_t)h;
        }
        if (v == 0u || v > (uint32_t)WCHAR_MAX) return 0;
        a->units[i] = (wchar_t)v;
    }
    a->nunits = (uint32_t)(len / 8u);
    a->units[a->nunits] = L'\0';
    return 1;
}

static void replay_setup(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        s_rr = RR_OFF;
        fprintf(stderr, "REPLAY_REFUSED reason=io-error path='%s'\n", path);
        fflush(stderr);
        exit(1);
    }
    RecBuf text = {0};
    uint8_t chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, fp)) > 0)
        if (!rec_buf_put(&text, chunk, got)) refuse("out-of-memory", 0);
    const int read_error = ferror(fp);
    fclose(fp);
    if (read_error || !rec_buf_put(&text, "", 1u)) refuse("io-error", 0);
    s_rep_text = (char *)text.data;
    if (strlen(s_rep_text) != text.len - 1u) refuse("malformed", 0); /* a NUL inside */

    char exe[65], disc[REC_DISC_LEN];
    size_t ckpt_cap = 0;
    size_t answ_cap = 0;
    size_t line_no = 0;
    char *line = s_rep_text;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (!nl) refuse("truncated", line_no + 1u); /* every line ends with its newline */
        *nl = '\0';
        line_no++;
        char *p = line;
        if (line_no == 1u) {
            if (strncmp(p, "NKREC v=1 ", 10) != 0) refuse("bad-magic", line_no);
            p += 10;
            const char *every = field(&p, "every=");
            const char *clock = field(&p, "clock=");
            const char *rec_exe = field(&p, "exe=");
            const char *rec_disc = field(&p, "disc=");
            const char *abi = field(&p, "abi=");
            const char *size = field(&p, "cpustate=");
            uint32_t v;
            if (!every || !clock || !rec_exe || !rec_disc || !abi || !size || *p) refuse("malformed", line_no);
            if (!parse_u32(every, &v) || v < 1u || v > 100000u) refuse("malformed", line_no);
            s_rr_cadence.every = v;
            if (strcmp(clock, "deterministic") != 0) refuse("clock-not-deterministic", line_no);
            if (!parse_u32(abi, &v) || v != SR_CPUSTATE_ABI_VERSION) refuse("abi-version", line_no);
            if (!parse_u32(size, &v) || v != sizeof(CpuState)) refuse("cpustate-size", line_no);
            if (!exe_identity(exe)) refuse("no-executable-identity", line_no);
            if (strcmp(rec_exe, exe) != 0) refuse("executable-identity", line_no);
            if (!disc_field(disc) || strcmp(rec_disc, disc) != 0) refuse("disc-id", line_no);
        } else if (strncmp(p, NK_CKPT_LINE_PREFIX, strlen(NK_CKPT_LINE_PREFIX)) == 0) {
            char detail[96];
            if (!nk_ckpt_parse(p, &s_rep_expect, detail, sizeof detail)) refuse("malformed-checkpoint", line_no);
            if (s_rep_ckpt_count && s_rep_expect.frame <= s_rep_end) refuse("checkpoint-order", line_no);
            if (s_rep_have_end_line) refuse("after-end", line_no);
            s_rep_end = s_rep_expect.frame;
            if (!push_ptr(&s_rep_ckpt, &s_rep_ckpt_count, &ckpt_cap, line)) refuse("out-of-memory", line_no);
        } else if (strncmp(p, "NKANSW v=1 ", 11) == 0) {
            static OskAnswer a;
            if (s_rep_have_end_line) refuse("after-end", line_no);
            if (!parse_answer(p + 11, &a)) refuse("malformed-answer", line_no);
            if (a.request + 1u == s_rep_answ_count && s_rep_answ[a.request].result == ANSW_OPEN &&
                a.pending >= s_rep_answ[a.request].pending) {
                s_rep_answ[a.request] = a; /* a later line for a request still open */
            } else if (a.request == s_rep_answ_count) {
                if (s_rep_answ_count == answ_cap) {
                    size_t ncap = answ_cap ? answ_cap * 2u : 8u;
                    OskAnswer *n = (OskAnswer *)realloc(s_rep_answ, ncap * sizeof *n);
                    if (!n) refuse("out-of-memory", line_no);
                    s_rep_answ = n;
                    answ_cap = ncap;
                }
                s_rep_answ[s_rep_answ_count++] = a;
            } else {
                refuse("answer-order", line_no);
            }
        } else if (strncmp(p, "NKREC_END ", 10) == 0) {
            p += 10;
            const char *frame = field(&p, "frame=");
            uint64_t f;
            if (s_rep_have_end_line || !parse_u64(frame, &f) || *p || f < s_rep_end) refuse("malformed", line_no);
            s_rep_end = f;
            s_rep_have_end_line = 1;
        } else {
            refuse("unknown-line", line_no);
        }
        line = nl + 1;
    }
    if (line_no == 0u) refuse("bad-magic", 0);
    if (s_rep_ckpt_count == 0u) refuse("no-checkpoints", line_no);
    /* A run stopped by _Exit (a capture or a vblank budget) leaves no end line; it then
     * replays to its last checkpoint, which is where its record is known to be complete. */
    if (!s_rep_have_end_line)
        fprintf(stderr, "replay: '%s' has no NKREC_END line; it replays to its last checkpoint, frame %llu\n",
                path, (unsigned long long)s_rep_end);
}

static void rr_setup(void) {
    const char *rec = getenv("SR_RECORD");
    const char *rep = getenv("SR_REPLAY");
    const int recording = rec && rec[0];
    const int replaying = rep && rep[0];
    s_rr = RR_OFF;
    if (!recording && !replaying) return;
    if (recording && replaying) die("SR_RECORD and SR_REPLAY are both set; a run does one or the other");
    if (!sched_clock_deterministic())
        die(recording ? "SR_RECORD needs SR_DETERMINISTIC_CLOCK=1: a run paced by the host clock cannot be replayed"
                      : "REPLAY_REFUSED reason=clock-not-deterministic line=0 (set SR_DETERMINISTIC_CLOCK=1)");
    if (recording) {
        s_rr_cadence.every = read_every();
        record_setup(rec);
        s_rr = RR_RECORD;
    } else {
        replay_setup(rep);
        s_rr = RR_REPLAY;
    }
    if (atexit(rr_exit) != 0) die("replay: could not register the exit handler");
}

static void rr_complete(uint32_t vcount) {
    fprintf(stderr, "REPLAY_COMPLETE frame=%" PRIu32 " checkpoints=%zu answers=%zu\n", vcount, s_rep_ckpt_count,
            s_rep_answ_count);
    fflush(stderr);
    s_rr = RR_COMPLETE;
}

static void record_answer(uint32_t request, uint32_t pending, AnswResult result, const wchar_t *text, int cap) {
    fprintf(s_rec_fp, "NKANSW v=1 call=osk request=%" PRIu32 " pending=%" PRIu32 " result=%s units=", request,
            pending, k_answ_result[result]);
    if (result == ANSW_OK) {
        int n = 0;
        for (; n < cap - 1 && text[n]; n++) fprintf(s_rec_fp, "%08" PRIx32, (uint32_t)text[n]);
        if (n == 0) fputc('-', s_rec_fp); /* confirmed with no text */
    } else {
        fputc('-', s_rec_fp);
    }
    fputc('\n', s_rec_fp);
    fflush(s_rec_fp);
}

static void rr_vblank(uint32_t vcount) {
    s_rr_vcount = vcount;
    s_rr_seen_vblank = 1;
    if (s_rr == RR_RECORD) {
        if (!cadence_due(&s_rr_cadence, vcount)) return;
        /* A keyboard request still open is written as far as it got, so a recording cut short
         * by _Exit still answers every poll before its last checkpoint. */
        if (s_osk_pending) record_answer(s_osk_request, s_osk_pending, ANSW_OPEN, NULL, 0);
        if (!ckpt_take(vcount) || !nk_ckpt_format(&s_ckpt, s_ckpt_line, sizeof s_ckpt_line))
            die("SR_RECORD: a checkpoint did not fit (too many threads); the recording stops here");
        fputs(s_ckpt_line, s_rec_fp);
        fflush(s_rec_fp);
        return;
    }
    if (s_rr != RR_REPLAY) return;
    if (cadence_due(&s_rr_cadence, vcount) && s_rep_ckpt_next < s_rep_ckpt_count) {
        char detail[96];
        NkCkptMismatch m;
        const size_t index = s_rep_ckpt_next;
        if (!nk_ckpt_parse(s_rep_ckpt[index], &s_rep_expect, detail, sizeof detail))
            refuse("malformed-checkpoint", 0);
        if (!ckpt_take(vcount)) die("REPLAY_MISMATCH reason=checkpoint-overflow");
        const NkCkptDiff diff = nk_ckpt_compare(&s_rep_expect, &s_ckpt, &m);
        if (diff != NK_CKPT_SAME) {
            fprintf(stderr, "REPLAY_MISMATCH frame=%" PRIu32 " index=%zu kind=%s digest=%s\n", vcount, index,
                    nk_ckpt_diff_name(diff), diff == NK_CKPT_FRAME ? "-" : m.name);
            fflush(stderr);
            s_rr = RR_OFF;
            exit(1);
        }
        s_rep_ckpt_next++;
    }
    if (s_rep_ckpt_next == s_rep_ckpt_count && vcount >= s_rep_end) rr_complete(vcount);
}

static void rr_exit(void) {
    if (s_rr == RR_RECORD && s_rec_fp) {
        if (s_osk_pending) record_answer(s_osk_request, s_osk_pending, ANSW_OPEN, NULL, 0);
        if (s_rr_seen_vblank) fprintf(s_rec_fp, "NKREC_END frame=%" PRIu32 "\n", s_rr_vcount);
        fclose(s_rec_fp);
        s_rec_fp = NULL;
    } else if (s_rr == RR_REPLAY) {
        /* Not a pass: the run stopped before the recording was used up. */
        fprintf(stderr, "REPLAY_NOT_RUN reached=%" PRIu32 " end=%llu checkpoints=%zu/%zu\n", s_rr_vcount,
                (unsigned long long)s_rep_end, s_rep_ckpt_next, s_rep_ckpt_count);
        fflush(stderr);
    }
}

void sr_replay_checkpoint_vblank(uint32_t vcount) {
    s_ckpt_taken = 0; /* the guest ran since the last call */
    if (s_stream_on != 0) stream_vblank(vcount);
    if (s_rr == RR_UNREAD) rr_setup();
    if (s_rr != RR_OFF) rr_vblank(vcount);
}

static void missing_answer(const char *what) {
    s_rr = RR_OFF;
    fprintf(stderr, "REPLAY_MISSING_ANSWER call=osk request=%" PRIu32 " frame=%" PRIu32 " detail=%s\n",
            s_osk_request, s_rr_vcount, what);
    fflush(stderr);
    exit(1);
}

int sr_replay_osk_poll(const wchar_t *desc, const wchar_t *initial, wchar_t *out, int cap, uint32_t input_type,
                       SrReplayOskLive live) {
    /* The live contract's first answer, which opens no request: never counted or recorded. */
    if (!out || cap < 2) return 0;
    if (s_rr == RR_UNREAD) rr_setup();
    if (s_rr == RR_REPLAY) {
        if (s_osk_request >= s_rep_answ_count) missing_answer("no-recorded-request");
        const OskAnswer *a = &s_rep_answ[s_osk_request];
        if (s_osk_pending < a->pending) {
            s_osk_pending++;
            return SR_OSK_TEXT_PENDING;
        }
        if (a->result == ANSW_OK) {
            uint32_t n = a->nunits;
            if (cap > SR_REPLAY_OSK_UNITS) cap = SR_REPLAY_OSK_UNITS;
            if (n > (uint32_t)cap - 1u) missing_answer("answer-longer-than-field");
            memcpy(out, a->units, (size_t)n * sizeof *out);
            out[n] = L'\0';
        } else if (a->result != ANSW_CANCEL) {
            missing_answer(a->result == ANSW_OPEN ? "polled-past-recording" : "polled-after-abandon");
        }
        s_osk_request++;
        s_osk_pending = 0;
        return a->result == ANSW_OK;
    }
    const int answer = live(desc, initial, out, cap, input_type);
    if (s_rr != RR_RECORD) return answer;
    if (answer == SR_OSK_TEXT_PENDING) {
        s_osk_pending++;
        return answer;
    }
    record_answer(s_osk_request, s_osk_pending, answer == 1 ? ANSW_OK : ANSW_CANCEL, out, cap);
    s_osk_request++;
    s_osk_pending = 0;
    return answer;
}

void sr_replay_osk_abandon(void (*live_abandon)(void)) {
    if (s_rr == RR_UNREAD) rr_setup();
    if (s_rr == RR_REPLAY) {
        if (s_osk_pending == 0u) return; /* no request open: nothing to abandon, as live */
        if (s_osk_request >= s_rep_answ_count || s_rep_answ[s_osk_request].result != ANSW_ABANDONED ||
            s_rep_answ[s_osk_request].pending != s_osk_pending)
            missing_answer("abandoned-unlike-recording");
        s_osk_request++;
        s_osk_pending = 0;
        return;
    }
    if (s_rr == RR_RECORD && s_osk_pending) {
        record_answer(s_osk_request, s_osk_pending, ANSW_ABANDONED, NULL, 0);
        s_osk_request++;
        s_osk_pending = 0;
    }
    live_abandon();
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
