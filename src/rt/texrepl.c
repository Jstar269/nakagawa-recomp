/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * texrepl.c - Texture replacement packs in PPSSPP's layout (#1880), phase 1. See texrepl.h.
 *
 * The quick texture hash and the replacement-pack key and lookup order are translated from PPSSPP
 * (GPL-2.0-or-later), GPU/Common/TextureDecoder.cpp (StableQuickTexHash, QuickTexHashNonSSE),
 * GPU/Common/TextureReplacer.cpp (textures.ini, hash ranges, key and wildcard lookup) and
 * GPU/Common/TextureCacheCommon.cpp (the palette hash) at 3bc60270eae3069f13dfbfdb7292a7e1375de6f6.
 * Copyright (c) 2012- PPSSPP Project. Modified for Nakagawa Recomp.
 *
 * The hashes must match PPSSPP's bit for bit, since a pack's file names depend on them; the
 * selftest checks them against fixed quick-hash vectors taken from PPSSPP's own code and published
 * xxHash vectors (XXH32 and XXH64 are the public algorithms). The ini parser, the PNG decode (stb),
 * the cache and the budget are this project's own.
 */
#include "texrepl.h"

#include "nk_platform.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS NK_TEXREPL_MAX_DIM
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wpedantic"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#endif
#include "../../third_party/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ===================================================================================== hashes */

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;   /* the PSP and every host this builds for are little-endian */
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

#define P32_1 2654435761u
#define P32_2 2246822519u
#define P32_3 3266489917u
#define P32_4 668265263u
#define P32_5 374761393u

uint32_t nk_texrepl_xxh32(const void *data, size_t len, uint32_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint32_t h;
    if (len >= 16) {
        uint32_t v1 = seed + P32_1 + P32_2, v2 = seed + P32_2, v3 = seed, v4 = seed - P32_1;
        const uint8_t *limit = end - 16;
        do {
            v1 = rotl32(v1 + rd32(p) * P32_2, 13) * P32_1;
            v2 = rotl32(v2 + rd32(p + 4) * P32_2, 13) * P32_1;
            v3 = rotl32(v3 + rd32(p + 8) * P32_2, 13) * P32_1;
            v4 = rotl32(v4 + rd32(p + 12) * P32_2, 13) * P32_1;
            p += 16;
        } while (p <= limit);
        h = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h = seed + P32_5;
    }
    h += (uint32_t)len;
    while (p + 4 <= end) {
        h = rotl32(h + rd32(p) * P32_3, 17) * P32_4;
        p += 4;
    }
    while (p < end) {
        h = rotl32(h + (uint32_t)(*p++) * P32_5, 11) * P32_1;
    }
    h ^= h >> 15;
    h *= P32_2;
    h ^= h >> 13;
    h *= P32_3;
    h ^= h >> 16;
    return h;
}

#define P64_1 11400714785074694791ull
#define P64_2 14029467366897019727ull
#define P64_3 1609587929392839161ull
#define P64_4 9650029242287828579ull
#define P64_5 2870177450012600261ull

static uint64_t xxh64_round(uint64_t acc, uint64_t input) {
    acc += input * P64_2;
    acc = rotl64(acc, 31);
    return acc * P64_1;
}
static uint64_t xxh64_merge(uint64_t acc, uint64_t val) {
    acc ^= xxh64_round(0, val);
    return acc * P64_1 + P64_4;
}

uint64_t nk_texrepl_xxh64(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P64_1 + P64_2, v2 = seed + P64_2, v3 = seed, v4 = seed - P64_1;
        const uint8_t *limit = end - 32;
        do {
            v1 = xxh64_round(v1, rd64(p));
            v2 = xxh64_round(v2, rd64(p + 8));
            v3 = xxh64_round(v3, rd64(p + 16));
            v4 = xxh64_round(v4, rd64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = xxh64_merge(h, v1);
        h = xxh64_merge(h, v2);
        h = xxh64_merge(h, v3);
        h = xxh64_merge(h, v4);
    } else {
        h = seed + P64_5;
    }
    h += (uint64_t)len;
    while (p + 8 <= end) {
        h ^= xxh64_round(0, rd64(p));
        h = rotl64(h, 27) * P64_1 + P64_4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (uint64_t)rd32(p) * P64_1;
        h = rotl64(h, 23) * P64_2 + P64_3;
        p += 4;
    }
    while (p < end) {
        h ^= (uint64_t)(*p++) * P64_5;
        h = rotl64(h, 11) * P64_1;
    }
    h ^= h >> 33;
    h *= P64_2;
    h ^= h >> 29;
    h *= P64_3;
    h ^= h >> 32;
    return h;
}

uint32_t nk_texrepl_quick_hash(const void *data, uint32_t size, uint32_t guest_addr) {
    const uint8_t *b = (const uint8_t *)data;
    uint32_t check = 0;
    if ((guest_addr & 0xfu) == 0 && (size & 0x3fu) == 0) {
        /* Eight 16-bit lanes (viewed as four 32-bit ones for the xor and add steps). */
        static const uint16_t kInitial[8] = {0xc00b, 0x9bd9, 0x4b73, 0xb651, 0x4d9b, 0x4309, 0x0083, 0x0001};
        uint16_t cursor16[8] = {0};
        uint16_t cursor2[8];
        memcpy(cursor2, kInitial, sizeof(cursor2));
        for (uint32_t i = 0; i < size / 16u; i += 4) {
            const uint8_t *b0 = b + (size_t)(i + 0) * 16u, *b1 = b + (size_t)(i + 1) * 16u;
            const uint8_t *b2 = b + (size_t)(i + 2) * 16u, *b3 = b + (size_t)(i + 3) * 16u;
            for (int j = 0; j < 8; j++) {
                uint16_t w;
                memcpy(&w, b0 + 2 * j, 2);
                cursor16[j] = (uint16_t)(cursor16[j] + (uint16_t)(w * cursor2[j]));
            }
            for (int j = 0; j < 4; j++) {
                uint32_t c;
                memcpy(&c, (const uint8_t *)cursor16 + 4 * j, 4);
                c ^= rd32(b1 + 4 * j);
                c += rd32(b2 + 4 * j);
                memcpy((uint8_t *)cursor16 + 4 * j, &c, 4);
            }
            for (int j = 0; j < 8; j++) {
                uint16_t w;
                memcpy(&w, b3 + 2 * j, 2);
                cursor16[j] = (uint16_t)(cursor16[j] ^ (uint16_t)(w * cursor2[j]));
            }
            for (int j = 0; j < 8; j++) cursor2[j] = (uint16_t)(cursor2[j] + 0x2455u);
        }
        for (int j = 0; j < 4; j++) {
            uint32_t c, c2;
            memcpy(&c, (const uint8_t *)cursor16 + 4 * j, 4);
            memcpy(&c2, (const uint8_t *)cursor2 + 4 * j, 4);
            c += c2;
            check += c;
        }
    } else {
        for (uint32_t i = 0; i < size / 8u; i++) {
            check += rd32(b + (size_t)i * 8u);
            check ^= rd32(b + (size_t)i * 8u + 4u);
        }
    }
    return check;
}

uint32_t nk_texrepl_clut_hash(const void *clut, uint32_t bytes) {
    return nk_texrepl_xxh32(clut, bytes, 0xC0108888u);
}

uint64_t nk_texrepl_cache_key(uint32_t addr, uint32_t format, uint32_t dim, uint32_t clut_hash) {
    uint64_t key = ((uint64_t)(addr & 0x3FFFFFFFu) << 32) | (dim & 0xFFFFu);
    if (format & 4u) key ^= clut_hash;
    return key;
}

uint32_t nk_texrepl_bits_per_texel(uint32_t format) {
    static const uint8_t kBits[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};
    return kBits[format & 15u];
}

/* ======================================================================================= pack */

typedef struct {
    uint64_t cachekey;
    uint32_t hash;
    uint32_t order;        /* position in the file listing and ini, so a later line wins */
    char *file;            /* NULL: no file, a "leave alone" line */
    /* Decoded image, loaded on first use. */
    uint32_t *rgba;
    int w, h;
    uint8_t state;         /* 0 not loaded, 1 loaded, 2 cannot be loaded */
    uint32_t pins;
    uint64_t lru;
} Alias;

typedef struct { uint32_t addr; int w, h, new_w, new_h; } HashRange;
typedef struct { int w, h; float value; } ReduceRange;

struct NkTexRepl {
    char *dir;
    NkTexReplHash hash_kind;
    int ignore_address, reduce_hash;
    int skip_dxt1_128x64, skip_dxt1_128x128;
    float reduce_global;
    Alias *aliases;
    size_t alias_n, alias_cap;
    HashRange *ranges;
    size_t range_n, range_cap;
    ReduceRange *reduce;
    size_t reduce_n, reduce_cap;
    char *diag[NK_TEXREPL_MAX_DIAG + 1];
    size_t diag_n, diag_dropped;
    char diag_last[64];
    uint64_t budget, used, clock;
    NkTexReplStats stats;
};

static void diag_add(NkTexRepl *p, const char *fmt, ...) {
    if (p->diag_n >= NK_TEXREPL_MAX_DIAG) { p->diag_dropped++; return; }
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    char *copy = (char *)malloc(n + 1);
    if (!copy) { p->diag_dropped++; return; }
    memcpy(copy, buf, n + 1);
    p->diag[p->diag_n++] = copy;
}

size_t nk_texrepl_diag_count(const NkTexRepl *p) {
    return p ? p->diag_n + (p->diag_dropped ? 1u : 0u) : 0u;
}
const char *nk_texrepl_diag(const NkTexRepl *p, size_t i) {
    if (!p) return "";
    if (i < p->diag_n) return p->diag[i];
    if (i == p->diag_n && p->diag_dropped) {
        NkTexRepl *m = (NkTexRepl *)p;   /* the summary line is formatted on demand */
        snprintf(m->diag_last, sizeof(m->diag_last), "... and %zu more not listed", p->diag_dropped);
        return m->diag_last;
    }
    return "";
}

NkTexReplHash nk_texrepl_hash_kind(const NkTexRepl *p) { return p ? p->hash_kind : NK_TEXREPL_HASH_QUICK; }
size_t nk_texrepl_entry_count(const NkTexRepl *p) { return p ? p->alias_n : 0u; }

static int alias_push(NkTexRepl *p, uint64_t cachekey, uint32_t hash, const char *file) {
    if (p->alias_n == p->alias_cap) {
        size_t cap = p->alias_cap ? p->alias_cap * 2 : 256;
        Alias *grown = (Alias *)realloc(p->aliases, cap * sizeof(Alias));
        if (!grown) return 0;
        p->aliases = grown;
        p->alias_cap = cap;
    }
    Alias *a = &p->aliases[p->alias_n];
    memset(a, 0, sizeof(*a));
    a->cachekey = cachekey;
    a->hash = hash;
    a->order = (uint32_t)p->alias_n;
    if (file && *file) {
        size_t n = strlen(file);
        a->file = (char *)malloc(n + 1);
        if (!a->file) return 0;
        memcpy(a->file, file, n + 1);
        for (char *c = a->file; *c; c++) if (*c == '\\') *c = '/';
    }
    p->alias_n++;
    return 1;
}

static int alias_cmp(const void *x, const void *y) {
    const Alias *a = (const Alias *)x, *b = (const Alias *)y;
    if (a->cachekey != b->cachekey) return a->cachekey < b->cachekey ? -1 : 1;
    if (a->hash != b->hash) return a->hash < b->hash ? -1 : 1;
    if (a->order != b->order) return a->order < b->order ? -1 : 1;
    return 0;
}

/* Sort by key and keep the last line of each key (a later line, the ini's after the folder scan,
 * wins). */
static void alias_finish(NkTexRepl *p) {
    if (p->alias_n < 2) return;
    qsort(p->aliases, p->alias_n, sizeof(Alias), alias_cmp);
    size_t out = 0;
    for (size_t i = 0; i < p->alias_n; i++) {
        if (i + 1 < p->alias_n && p->aliases[i].cachekey == p->aliases[i + 1].cachekey &&
            p->aliases[i].hash == p->aliases[i + 1].hash) {
            free(p->aliases[i].file);
            continue;
        }
        p->aliases[out++] = p->aliases[i];
    }
    p->alias_n = out;
}

static Alias *alias_find(const NkTexRepl *p, uint64_t cachekey, uint32_t hash) {
    size_t lo = 0, hi = p->alias_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const Alias *a = &p->aliases[mid];
        if (a->cachekey == cachekey && a->hash == hash) return &p->aliases[mid];
        if (a->cachekey < cachekey || (a->cachekey == cachekey && a->hash < hash)) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------- ini */

static int ci_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    return s;
}

/* "0xADDR", "ADDR": hexadecimal. */
static int parse_hex32(const char *s, uint32_t *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return 0;
    char *end;
    unsigned long long v = strtoull(s, &end, 16);
    if (*end || v > 0xFFFFFFFFull) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int parse_int(const char *s, int *out) {
    if (!*s) return 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end || v < -1000000 || v > 1000000) return 0;
    *out = (int)v;
    return 1;
}

static int parse_bool(const char *s, int *out) {
    if (ci_equal(s, "true") || !strcmp(s, "1")) { *out = 1; return 1; }
    if (ci_equal(s, "false") || !strcmp(s, "0") || !*s) { *out = 0; return 1; }
    return 0;
}

/* A path that stays inside the pack folder: relative, no ".." component, no drive or stream. */
static int path_inside_pack(const char *rel) {
    if (!*rel || rel[0] == '/' || rel[0] == '\\' || strchr(rel, ':')) return 0;
    const char *s = rel;
    while (*s) {
        const char *e = s;
        while (*e && *e != '/' && *e != '\\') e++;
        if (e - s == 2 && s[0] == '.' && s[1] == '.') return 0;
        s = *e ? e + 1 : e;
    }
    return 1;
}

/* "<16 hex><8 hex>[_<level>]". */
static int parse_hash_name(const char *name, uint64_t *cachekey, uint32_t *hash, int *level) {
    size_t n = strlen(name);
    if (n < 24) return 0;
    for (size_t i = 0; i < 24; i++) if (!isxdigit((unsigned char)name[i])) return 0;
    char part[17];
    memcpy(part, name, 16);
    part[16] = '\0';
    *cachekey = strtoull(part, NULL, 16);
    memcpy(part, name + 16, 8);
    part[8] = '\0';
    *hash = (uint32_t)strtoul(part, NULL, 16);
    *level = 0;
    if (n > 24) {
        if (name[24] != '_') return 0;
        if (!parse_int(name + 25, level) || *level < 0) return 0;
    }
    return 1;
}

typedef enum { SEC_NONE, SEC_OPTIONS, SEC_HASHES, SEC_HASHRANGES, SEC_REDUCE, SEC_FILTERING,
               SEC_INDEXED, SEC_GAMES, SEC_UNKNOWN } Section;

static int range_push(NkTexRepl *p, HashRange r) {
    if (p->range_n == p->range_cap) {
        size_t cap = p->range_cap ? p->range_cap * 2 : 16;
        HashRange *g = (HashRange *)realloc(p->ranges, cap * sizeof(*g));
        if (!g) return 0;
        p->ranges = g;
        p->range_cap = cap;
    }
    p->ranges[p->range_n++] = r;
    return 1;
}
static int reduce_push(NkTexRepl *p, ReduceRange r) {
    if (p->reduce_n == p->reduce_cap) {
        size_t cap = p->reduce_cap ? p->reduce_cap * 2 : 16;
        ReduceRange *g = (ReduceRange *)realloc(p->reduce, cap * sizeof(*g));
        if (!g) return 0;
        p->reduce = g;
        p->reduce_cap = cap;
    }
    p->reduce[p->reduce_n++] = r;
    return 1;
}

/* Parses the whole ini into `p`. Returns 0 with `err` set when it is malformed. */
static int parse_ini(NkTexRepl *p, char *text, char *err, size_t err_cap) {
    Section sec = SEC_NONE;
    int have_hash = 0, line_no = 0;
    size_t filtering = 0, indexed = 0, games = 0;
    int mips_noted = 0;
    char *save = NULL;
    for (char *line = text; line; line = save) {
        char *nl = strchr(line, '\n');
        if (nl) { *nl = '\0'; save = nl + 1; } else save = NULL;
        line_no++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close || *trim(close + 1) != '\0') {
                snprintf(err, err_cap, "textures.ini line %d: unfinished section header '%s'", line_no, s);
                return 0;
            }
            *close = '\0';
            const char *name = trim(s + 1);
            if (ci_equal(name, "options")) sec = SEC_OPTIONS;
            else if (ci_equal(name, "hashes")) sec = SEC_HASHES;
            else if (ci_equal(name, "hashranges")) sec = SEC_HASHRANGES;
            else if (ci_equal(name, "reducehashranges")) sec = SEC_REDUCE;
            else if (ci_equal(name, "filtering")) sec = SEC_FILTERING;
            else if (ci_equal(name, "indexed")) sec = SEC_INDEXED;
            else if (ci_equal(name, "games")) sec = SEC_GAMES;
            else {
                sec = SEC_UNKNOWN;
                diag_add(p, "textures.ini: unknown section [%s] not read", name);
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            snprintf(err, err_cap, "textures.ini line %d: '%s' is not a section, a comment or key = value",
                     line_no, s);
            return 0;
        }
        *eq = '\0';
        char *key = trim(s), *value = trim(eq + 1);
        if (sec == SEC_NONE) {
            snprintf(err, err_cap, "textures.ini line %d: '%s' is outside any section", line_no, key);
            return 0;
        }
        switch (sec) {
        case SEC_OPTIONS:
            if (ci_equal(key, "hash")) {
                if (ci_equal(value, "quick")) p->hash_kind = NK_TEXREPL_HASH_QUICK;
                else if (ci_equal(value, "xxh32")) p->hash_kind = NK_TEXREPL_HASH_XXH32;
                else if (ci_equal(value, "xxh64")) p->hash_kind = NK_TEXREPL_HASH_XXH64;
                else {
                    snprintf(err, err_cap, "textures.ini: unsupported hash type '%s' (use quick, xxh32 or xxh64)",
                             value);
                    return 0;
                }
                have_hash = 1;
            } else if (ci_equal(key, "ignoreAddress") || ci_equal(key, "reduceHash") ||
                       ci_equal(key, "ignoreMipmap")) {
                int b;
                if (!parse_bool(value, &b)) {
                    snprintf(err, err_cap, "textures.ini line %d: %s must be true or false, not '%s'",
                             line_no, key, value);
                    return 0;
                }
                if (ci_equal(key, "ignoreAddress")) p->ignore_address = b;
                else if (ci_equal(key, "reduceHash")) p->reduce_hash = b;
                else if (b) diag_add(p, "textures.ini: ignoreMipmap not applied (mip levels are not replaced)");
            } else if (ci_equal(key, "skipLastDXT1Blocks128x64") || ci_equal(key, "skipLastDXT1Blocks128x128")) {
                int n;
                if (!parse_int(value, &n) || n < 0) {
                    snprintf(err, err_cap, "textures.ini line %d: %s must be a whole number, not '%s'",
                             line_no, key, value);
                    return 0;
                }
                if (ci_equal(key, "skipLastDXT1Blocks128x64")) p->skip_dxt1_128x64 = n;
                else p->skip_dxt1_128x128 = n;
            } else if (ci_equal(key, "version")) {
                int v;
                if (!parse_int(value, &v)) {
                    snprintf(err, err_cap, "textures.ini line %d: version must be a whole number", line_no);
                    return 0;
                }
                if (v > 1) diag_add(p, "textures.ini: version %d is newer than this build reads (1); reading it anyway", v);
            } else if (ci_equal(key, "saveIndexed")) {
                if (*value && !ci_equal(value, "false") && strcmp(value, "0") != 0)
                    diag_add(p, "textures.ini: saveIndexed not applied (indexed textures are not supported)");
            } else {
                diag_add(p, "textures.ini: unknown [options] key '%s' not read", key);
            }
            break;
        case SEC_HASHES: {
            uint64_t ck;
            uint32_t h;
            int level;
            if (!*key || !parse_hash_name(key, &ck, &h, &level)) {
                diag_add(p, "textures.ini: [hashes] line '%s' is not a 24-digit hash name, not read", key);
                break;
            }
            if (*value && !path_inside_pack(value)) {
                diag_add(p, "textures.ini: [hashes] '%s' names '%s', which is outside the pack folder; not read",
                         key, value);
                break;
            }
            if (level > 0) {
                if (!mips_noted) {
                    mips_noted = 1;
                    diag_add(p, "textures.ini: mip-level entries (name_1) are not read; only level 0 is replaced");
                }
                break;
            }
            if (!alias_push(p, ck, h, value)) { snprintf(err, err_cap, "out of memory"); return 0; }
            break;
        }
        case SEC_HASHRANGES: {
            /* addr,w,h = newW,newH */
            char *c1 = strchr(key, ','), *c2 = c1 ? strchr(c1 + 1, ',') : NULL;
            char *vc = strchr(value, ',');
            HashRange r;
            if (!c2 || !vc) { diag_add(p, "textures.ini: [hashranges] '%s = %s' is not addr,w,h = w,h; not read", key, value); break; }
            *c1 = *c2 = *vc = '\0';
            if (!parse_hex32(trim(key), &r.addr) || !parse_int(trim(c1 + 1), &r.w) || !parse_int(trim(c2 + 1), &r.h) ||
                !parse_int(trim(value), &r.new_w) || !parse_int(trim(vc + 1), &r.new_h) ||
                r.w <= 0 || r.h <= 0 || r.new_w <= 0 || r.new_h <= 0) {
                diag_add(p, "textures.ini: [hashranges] line for '%s' has a bad number; not read", key);
                break;
            }
            if (!range_push(p, r)) { snprintf(err, err_cap, "out of memory"); return 0; }
            break;
        }
        case SEC_REDUCE: {
            char *c1 = strchr(key, ',');
            ReduceRange r;
            char *end;
            if (!c1) { diag_add(p, "textures.ini: [reducehashranges] '%s' is not w,h; not read", key); break; }
            *c1 = '\0';
            r.value = strtof(value, &end);
            if (!parse_int(trim(key), &r.w) || !parse_int(trim(c1 + 1), &r.h) || *end || !*value) {
                diag_add(p, "textures.ini: [reducehashranges] line for '%s' has a bad number; not read", key);
                break;
            }
            if (!reduce_push(p, r)) { snprintf(err, err_cap, "out of memory"); return 0; }
            break;
        }
        case SEC_FILTERING: filtering++; break;
        case SEC_INDEXED: indexed++; break;
        case SEC_GAMES: games++; break;
        default: break;
        }
    }
    if (!have_hash) {
        snprintf(err, err_cap, "textures.ini: no hash type in [options] (use hash = quick, xxh32 or xxh64)");
        return 0;
    }
    if (filtering) diag_add(p, "textures.ini: %zu [filtering] entries not applied (the game's own filtering is kept)", filtering);
    if (indexed) diag_add(p, "textures.ini: %zu [indexed] entries not applied (indexed replacements are not supported)", indexed);
    if (games) diag_add(p, "textures.ini: %zu [games] entries not applied (per-game ini overrides are not supported)", games);
    if (p->reduce_hash && p->hash_kind == NK_TEXREPL_HASH_QUICK) {
        p->reduce_hash = 0;
        diag_add(p, "textures.ini: reduceHash needs hash = xxh32 or xxh64; ignored with the quick hash");
    }
    if (p->ignore_address && p->hash_kind == NK_TEXREPL_HASH_QUICK) {
        p->ignore_address = 0;
        diag_add(p, "textures.ini: ignoreAddress needs hash = xxh32 or xxh64; ignored with the quick hash");
    }
    return 1;
}

/* ------------------------------------------------------------------------------- files */

static unsigned char *read_whole(const char *path, size_t cap, size_t *size) {
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) return NULL;
    unsigned char *buf = NULL;
    size_t len = 0, room = 0;
    for (;;) {
        if (len == room) {
            room = room ? room * 2 : 65536;
            if (room > cap + 1) room = cap + 1;
            if (len >= room) { free(buf); fclose(f); return NULL; }
            unsigned char *g = (unsigned char *)realloc(buf, room);
            if (!g) { free(buf); fclose(f); return NULL; }
            buf = g;
        }
        size_t got = fread(buf + len, 1, room - len, f);
        len += got;
        if (got == 0) break;
        if (len > cap) { free(buf); fclose(f); return NULL; }
    }
    fclose(f);
    *size = len;
    return buf;
}

typedef struct { NkTexRepl *p; } ScanCtx;

static int ends_with_ci(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && ci_equal(s + a - b, suffix);
}

static bool scan_file(const char *name, void *ctx) {
    NkTexRepl *p = ((ScanCtx *)ctx)->p;
    if (!name || !*name || name[0] == '.') return true;
    const char *dot = strrchr(name, '.');
    if (!dot) return true;
    size_t stem = (size_t)(dot - name);
    if (!(stem == 24 || (stem >= 26 && stem <= 27 && name[24] == '_'))) return true;
    char base[40];
    memcpy(base, name, stem);
    base[stem] = '\0';
    uint64_t ck;
    uint32_t h;
    int level;
    if (!parse_hash_name(base, &ck, &h, &level)) return true;
    if (!ends_with_ci(name, ".png")) {
        if (ends_with_ci(name, ".dds") || ends_with_ci(name, ".ktx2") || ends_with_ci(name, ".zim"))
            diag_add(p, "%s: only PNG replacements are read in this version; not used", name);
        return true;
    }
    if (level > 0) {
        diag_add(p, "%s: mip-level files are not read; only level 0 is replaced", name);
        return true;
    }
    alias_push(p, ck, h, name);
    return true;
}

NkTexRepl *nk_texrepl_open(const char *dir, uint64_t budget_bytes, char *err, size_t err_cap) {
    char ebuf[256];
    if (!err || !err_cap) { err = ebuf; err_cap = sizeof(ebuf); }
    err[0] = '\0';
    if (!dir || !*dir || !nk_platform_dir_exists(dir)) {
        snprintf(err, err_cap, "texture pack folder '%s' does not exist", dir ? dir : "");
        return NULL;
    }
    NkTexRepl *p = (NkTexRepl *)calloc(1, sizeof(*p));
    if (!p) { snprintf(err, err_cap, "out of memory"); return NULL; }
    size_t dn = strlen(dir);
    p->dir = (char *)malloc(dn + 1);
    if (!p->dir) { free(p); snprintf(err, err_cap, "out of memory"); return NULL; }
    memcpy(p->dir, dir, dn + 1);
    while (dn > 1 && (p->dir[dn - 1] == '/' || p->dir[dn - 1] == '\\')) p->dir[--dn] = '\0';
    p->budget = budget_bytes ? budget_bytes : (uint64_t)NK_TEXREPL_DEFAULT_BUDGET_MB << 20;
    p->reduce_global = 0.5f;

    ScanCtx sc = { p };
    if (!nk_platform_list_files(p->dir, scan_file, &sc)) {
        snprintf(err, err_cap, "texture pack folder '%s' cannot be read", dir);
        nk_texrepl_close(p);
        return NULL;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/textures.ini", p->dir);
    if (nk_platform_file_exists(path)) {
        size_t size = 0;
        unsigned char *text = read_whole(path, 4u << 20, &size);
        if (!text) {
            snprintf(err, err_cap, "textures.ini cannot be read or is larger than 4 MiB");
            nk_texrepl_close(p);
            return NULL;
        }
        char *z = (char *)realloc(text, size + 1);
        if (!z) { free(text); snprintf(err, err_cap, "out of memory"); nk_texrepl_close(p); return NULL; }
        z[size] = '\0';
        char *body = z;
        if (size >= 3 && (unsigned char)z[0] == 0xEF && (unsigned char)z[1] == 0xBB && (unsigned char)z[2] == 0xBF)
            body += 3;   /* a UTF-8 byte order mark */
        for (char *c = body; *c; c++) if (*c == '\r') *c = ' ';
        int ok = parse_ini(p, body, err, err_cap);
        free(z);
        if (!ok) {
            nk_texrepl_close(p);
            return NULL;
        }
    }
    alias_finish(p);
    return p;
}

void nk_texrepl_close(NkTexRepl *p) {
    if (!p) return;
    for (size_t i = 0; i < p->alias_n; i++) {
        free(p->aliases[i].file);
        free(p->aliases[i].rgba);
    }
    for (size_t i = 0; i < p->diag_n; i++) free(p->diag[i]);
    free(p->aliases);
    free(p->ranges);
    free(p->reduce);
    free(p->dir);
    free(p);
}

/* ----------------------------------------------------------------------------- hashing */

static int find_range(const NkTexRepl *p, uint32_t addr, int w, int h, int *nw, int *nh) {
    for (size_t i = 0; i < p->range_n; i++) {
        const HashRange *r = &p->ranges[i];
        if (r->addr == addr && r->w == w && r->h == h) { *nw = r->new_w; *nh = r->new_h; return 1; }
    }
    *nw = w;
    *nh = h;
    return 0;
}

static float reduce_for(const NkTexRepl *p, int w, int h) {
    for (size_t i = 0; i < p->reduce_n; i++)
        if (p->reduce[i].w == w && p->reduce[i].h == h) return p->reduce[i].value;
    return p->reduce_global;
}

static uint32_t hash_bytes(const NkTexRepl *p, const uint8_t *d, uint32_t n, uint32_t guest_addr) {
    switch (p->hash_kind) {
    case NK_TEXREPL_HASH_QUICK: return nk_texrepl_quick_hash(d, n, guest_addr);
    case NK_TEXREPL_HASH_XXH32: return nk_texrepl_xxh32(d, n, 0xBACD7814u);
    case NK_TEXREPL_HASH_XXH64: return (uint32_t)nk_texrepl_xxh64(d, n, 0xBACD7814u);
    }
    return 0;
}

uint32_t nk_texrepl_hash_texture(const NkTexRepl *p, const uint8_t *data, size_t avail, uint32_t guest_addr,
                                 int bufw, int w, int h, uint32_t format, int max_seen_v) {
    if (!p || !data || w <= 0 || h <= 0 || bufw <= 0 || format > 10) return 0;
    if (!find_range(p, guest_addr, w, h, &w, &h)) {
        if (h == 512 && max_seen_v < 512 && max_seen_v != 0) h = max_seen_v;
    }
    float reduce = 1.0f;
    if (p->reduce_hash) {
        reduce = reduce_for(p, w, h);
        if (reduce < 0.0f) reduce = 0.0f;
        if (reduce > 1.0f) reduce = 1.0f;
    }
    const uint64_t bits = nk_texrepl_bits_per_texel(format);
    if (bufw <= w) {
        /* Contiguous: the pixels used. */
        const uint64_t total = (uint64_t)bufw * (uint64_t)h + (uint64_t)(w - bufw);
        uint32_t size = (uint32_t)((float)((bits * total) / 8u) * reduce);
        if (p->skip_dxt1_128x64 && format == 8 && w == 128 && h == 64) size -= 8u * (uint32_t)p->skip_dxt1_128x64;
        if (p->skip_dxt1_128x128 && format == 8 && w == 128 && h == 128) size -= 8u * (uint32_t)p->skip_dxt1_128x128;
        if (size > avail) return 0;
        return hash_bytes(p, data, size, guest_addr);
    }
    /* Rows with gaps: hash each row's texels and fold. */
    const uint32_t per_line = (uint32_t)((float)((bits * (uint64_t)w) / 8u) * reduce);
    const uint64_t stride = (bits * (uint64_t)bufw) / 8u;
    const uint64_t span = (uint64_t)(h - 1) * stride + per_line;
    if (span > avail) return 0;
    uint32_t result = 0;
    for (int y = 0; y < h; y++) {
        uint32_t row = hash_bytes(p, data + (uint64_t)y * stride, per_line, guest_addr);
        result = (result * 11u) ^ row;
    }
    return result;
}

/* ------------------------------------------------------------------------------ lookup */

static Alias *resolve(const NkTexRepl *p, const NkTexReplId *id) {
    uint64_t ck = nk_texrepl_cache_key(id->addr, id->format, id->dim, id->clut_hash);
    uint32_t hash = id->hash;
    if (p->ignore_address) ck &= 0xFFFFFFFFull;
    Alias *a;
    if ((a = alias_find(p, ck, hash))) return a;
    if ((a = alias_find(p, ck & 0xFFFFFFFFull, 0))) return a;               /* palette hash only */
    if (!p->ignore_address && (a = alias_find(p, ck, 0))) return a;          /* no data hash */
    if ((a = alias_find(p, ck & 0xFFFFFFFFull, hash))) return a;             /* no address */
    if (!p->ignore_address) {
        if ((a = alias_find(p, ck & ~0xFFFFFFFFull, hash))) return a;        /* address, not palette */
        if ((a = alias_find(p, ck & ~0xFFFFFFFFull, 0))) return a;           /* address only */
    }
    return NULL;
}

static uint64_t now_ns(void) {
    return (uint64_t)((double)clock() * (1e9 / (double)CLOCKS_PER_SEC));
}

static void evict_for(NkTexRepl *p, uint64_t need) {
    while (p->used + need > p->budget) {
        Alias *oldest = NULL;
        for (size_t i = 0; i < p->alias_n; i++) {
            Alias *a = &p->aliases[i];
            if (a->state == 1 && a->pins == 0 && (!oldest || a->lru < oldest->lru)) oldest = a;
        }
        if (!oldest) return;
        free(oldest->rgba);
        oldest->rgba = NULL;
        oldest->state = 0;
        p->used -= (uint64_t)oldest->w * (uint64_t)oldest->h * 4u;
        p->stats.evicted++;
    }
}

static int load_alias(NkTexRepl *p, Alias *a) {
    if (!a->file || !path_inside_pack(a->file)) { a->state = 2; return 0; }
    if (!ends_with_ci(a->file, ".png")) {
        diag_add(p, "%s: only PNG replacements are read in this version; not used", a->file);
        a->state = 2;
        p->stats.load_failed++;
        return 0;
    }
    char path[1400];
    snprintf(path, sizeof(path), "%s/%s", p->dir, a->file);
    uint64_t t0 = now_ns();
    size_t size = 0;
    unsigned char *file = read_whole(path, 64u << 20, &size);
    if (!file) {
        diag_add(p, "%s: file is missing, unreadable or larger than 64 MiB; texture left as the game draws it", a->file);
        a->state = 2;
        p->stats.load_failed++;
        return 0;
    }
    int w = 0, h = 0, ch = 0;
    unsigned char *px = NULL;
    if (stbi_info_from_memory(file, (int)size, &w, &h, &ch) && w > 0 && h > 0 && w <= NK_TEXREPL_MAX_DIM &&
        h <= NK_TEXREPL_MAX_DIM) {
        const uint64_t bytes = (uint64_t)w * (uint64_t)h * 4u;
        if (bytes > p->budget) {
            free(file);
            diag_add(p, "%s: %dx%d needs %llu KiB, more than the %llu KiB texture budget; not used", a->file, w, h,
                     (unsigned long long)(bytes >> 10), (unsigned long long)(p->budget >> 10));
            a->state = 2;
            p->stats.over_budget++;
            return 0;
        }
        px = stbi_load_from_memory(file, (int)size, &w, &h, &ch, 4);
    }
    free(file);
    if (!px) {
        diag_add(p, "%s: not a PNG this build can decode (or larger than %d texels a side); not used", a->file,
                 NK_TEXREPL_MAX_DIM);
        a->state = 2;
        p->stats.load_failed++;
        return 0;
    }
    const uint64_t bytes = (uint64_t)w * (uint64_t)h * 4u;
    evict_for(p, bytes);
    if (p->used + bytes > p->budget) {
        /* Everything held is in use: draw this texture unreplaced now, and try again on a later lookup. */
        stbi_image_free(px);
        p->stats.over_budget++;
        return 0;
    }
    a->rgba = (uint32_t *)malloc((size_t)bytes);
    if (!a->rgba) { stbi_image_free(px); a->state = 2; p->stats.load_failed++; return 0; }
    memcpy(a->rgba, px, (size_t)bytes);   /* R, G, B, A bytes: R in the low byte on a little-endian host */
    stbi_image_free(px);
    a->w = w;
    a->h = h;
    a->state = 1;
    p->used += bytes;
    if (p->used > p->stats.cache_peak) p->stats.cache_peak = p->used;
    p->stats.loaded++;
    p->stats.decode_ns += now_ns() - t0;
    return 1;
}

int nk_texrepl_lookup(NkTexRepl *p, const NkTexReplId *id, NkTexReplHit *out) {
    if (!p || !id || !out) return 0;
    memset(out, 0, sizeof(*out));
    p->stats.lookups++;
    Alias *a = resolve(p, id);
    if (!a) { p->stats.misses++; return 0; }
    if (!a->file) { p->stats.left_alone++; p->stats.misses++; return 0; }
    if (a->state == 0 && !load_alias(p, a)) { p->stats.misses++; return 0; }
    if (a->state != 1) { p->stats.misses++; return 0; }
    a->pins++;
    a->lru = ++p->clock;
    p->stats.hits++;
    out->rgba = a->rgba;
    out->w = a->w;
    out->h = a->h;
    out->pin = a;
    return 1;
}

void nk_texrepl_unpin(NkTexRepl *p, NkTexReplHit *hit) {
    (void)p;
    if (!hit || !hit->pin) return;
    Alias *a = (Alias *)hit->pin;
    if (a->pins) a->pins--;
    memset(hit, 0, sizeof(*hit));
}

int nk_texrepl_factor(const NkTexReplHit *hit, int w, int h) {
    if (!hit || !hit->rgba || w <= 0 || h <= 0) return 0;
    if (hit->w % w || hit->h % h) return 0;
    const int fx = hit->w / w, fy = hit->h / h;
    return fx == fy ? fx : 0;
}

void nk_texrepl_stats(const NkTexRepl *p, NkTexReplStats *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!p) return;
    *out = p->stats;
    out->cache_bytes = p->used;
}

void nk_texrepl_note_bad_factor(NkTexRepl *p) {
    if (p) p->stats.bad_factor++;
}
