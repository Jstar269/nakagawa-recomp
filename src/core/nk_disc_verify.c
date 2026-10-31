/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Disc verification (nk_disc_verify.h). SHA-1 follows FIPS 180-4 section 6.1;
   CRC-32 is the reflected 0xEDB88320 polynomial with ~0 in and out, the CRC
   Redump and zlib list. */

#include "nk_disc_verify.h"

#include "nk_platform.h"
#include "nk_types.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NK_DISC_HASH_CHUNK (1024u * 1024u)
#define NK_DISC_MAX_GAME_SERIALS 16u

static bool verify_fail(NkDiscExpandStatus *status, const char *code, const char *fmt, ...) {
    if (status) {
        va_list args;
        snprintf(status->code, sizeof(status->code), "%s", code);
        va_start(args, fmt);
        vsnprintf(status->message, sizeof(status->message), fmt, args);
        va_end(args);
    }
    return false;
}

static void verify_status_clear(NkDiscExpandStatus *status) {
    if (status) {
        status->code[0] = '\0';
        status->message[0] = '\0';
    }
}

/* ---- SHA-1 ------------------------------------------------------------- */

static uint32_t rotl32(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32u - n));
}

static void sha1_block(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (unsigned t = 0; t < 16u; t++) {
        w[t] = ((uint32_t)block[4u * t] << 24) | ((uint32_t)block[4u * t + 1u] << 16) |
               ((uint32_t)block[4u * t + 2u] << 8) | (uint32_t)block[4u * t + 3u];
    }
    for (unsigned t = 16u; t < 80u; t++) {
        w[t] = rotl32(w[t - 3u] ^ w[t - 8u] ^ w[t - 14u] ^ w[t - 16u], 1u);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (unsigned t = 0; t < 80u; t++) {
        uint32_t f, k;
        if (t < 20u) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (t < 40u) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (t < 60u) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t temp = rotl32(a, 5u) + f + e + k + w[t];
        e = d;
        d = c;
        c = rotl32(b, 30u);
        b = a;
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void nk_sha1_init(NkSha1 *ctx) {
    static const uint32_t initial[5] = {
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u,
    };
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->state, initial, sizeof(initial));
}

void nk_sha1_update(NkSha1 *ctx, const uint8_t *data, size_t len) {
    ctx->byte_count += len;
    if (ctx->block_len > 0) {
        size_t take = 64u - ctx->block_len < len ? 64u - ctx->block_len : len;
        memcpy(ctx->block + ctx->block_len, data, take);
        ctx->block_len += take;
        data += take;
        len -= take;
        if (ctx->block_len < 64u) return;
        sha1_block(ctx->state, ctx->block);
        ctx->block_len = 0;
    }
    while (len >= 64u) {
        sha1_block(ctx->state, data);
        data += 64u;
        len -= 64u;
    }
    if (len > 0) {
        memcpy(ctx->block, data, len);
        ctx->block_len = len;
    }
}

void nk_sha1_finish(NkSha1 *ctx, uint8_t digest[20]) {
    uint64_t bits = ctx->byte_count * 8u;
    ctx->block[ctx->block_len++] = 0x80u;
    if (ctx->block_len > 56u) {
        memset(ctx->block + ctx->block_len, 0, 64u - ctx->block_len);
        sha1_block(ctx->state, ctx->block);
        ctx->block_len = 0;
    }
    memset(ctx->block + ctx->block_len, 0, 56u - ctx->block_len);
    for (unsigned i = 0; i < 8u; i++) ctx->block[63u - i] = (uint8_t)(bits >> (8u * i));
    sha1_block(ctx->state, ctx->block);
    for (unsigned i = 0; i < 5u; i++) {
        digest[4u * i] = (uint8_t)(ctx->state[i] >> 24);
        digest[4u * i + 1u] = (uint8_t)(ctx->state[i] >> 16);
        digest[4u * i + 2u] = (uint8_t)(ctx->state[i] >> 8);
        digest[4u * i + 3u] = (uint8_t)ctx->state[i];
    }
    memset(ctx->block, 0, sizeof(ctx->block));
}

/* ---- CRC-32 ------------------------------------------------------------ */

/* Slicing-by-four tables, built once: table[0] is the classic byte table. */
static uint32_t g_crc_table[4][256];
static bool g_crc_ready = false;

static void crc32_build_tables(void) {
    for (uint32_t n = 0; n < 256u; n++) {
        uint32_t c = n;
        for (unsigned k = 0; k < 8u; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[0][n] = c;
    }
    for (uint32_t n = 0; n < 256u; n++) {
        for (unsigned t = 1; t < 4u; t++) {
            uint32_t prev = g_crc_table[t - 1u][n];
            g_crc_table[t][n] = g_crc_table[0][prev & 0xFFu] ^ (prev >> 8);
        }
    }
    g_crc_ready = true;
}

uint32_t nk_crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    if (!g_crc_ready) crc32_build_tables();
    uint32_t c = ~crc;
    while (len >= 4u) {
        c ^= (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
             ((uint32_t)data[3] << 24);
        c = g_crc_table[3][c & 0xFFu] ^ g_crc_table[2][(c >> 8) & 0xFFu] ^
            g_crc_table[1][(c >> 16) & 0xFFu] ^ g_crc_table[0][c >> 24];
        data += 4u;
        len -= 4u;
    }
    while (len-- > 0) c = g_crc_table[0][(c ^ *data++) & 0xFFu] ^ (c >> 8);
    return ~c;
}

/* ---- whole-image hash -------------------------------------------------- */

static void hex_encode(const uint8_t *bytes, size_t count, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < count; i++) {
        out[2u * i] = hex[bytes[i] >> 4];
        out[2u * i + 1u] = hex[bytes[i] & 0x0Fu];
    }
    out[2u * count] = '\0';
}

bool nk_disc_hash_file(const char *path, const NkDiscExpandCallbacks *callbacks,
                       NkDiscDigest *out, NkDiscExpandStatus *status) {
    verify_status_clear(status);
    if (out) memset(out, 0, sizeof(*out));
    if (!path || !path[0] || !out) {
        return verify_fail(status, NK_DISC_INTERNAL, "No disc image was given to hash.");
    }
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) {
        return verify_fail(status, NK_DISC_READ_FAILED,
                           "The disc image could not be opened for reading.");
    }
    int64_t total = nk_platform_get_file_size(path);
    uint8_t *buffer = (uint8_t *)malloc(NK_DISC_HASH_CHUNK);
    if (!buffer) {
        fclose(f);
        return verify_fail(status, NK_DISC_INTERNAL, "No memory was free to hash the disc image.");
    }
    NkSha1 sha1;
    nk_sha1_init(&sha1);
    uint32_t crc = 0;
    uint64_t done = 0;
    bool ok = true;
    for (;;) {
        if (callbacks && callbacks->is_cancelled && callbacks->is_cancelled(callbacks->user)) {
            ok = verify_fail(status, NK_DISC_CANCELLED, "Checking the disc image was cancelled.");
            break;
        }
        size_t got = fread(buffer, 1, NK_DISC_HASH_CHUNK, f);
        if (got > 0) {
            nk_sha1_update(&sha1, buffer, got);
            crc = nk_crc32_update(crc, buffer, got);
            done += got;
            if (callbacks && callbacks->on_progress) {
                callbacks->on_progress(done, total > 0 ? (uint64_t)total : done, callbacks->user);
            }
        }
        if (got < NK_DISC_HASH_CHUNK) {
            if (ferror(f)) {
                ok = verify_fail(status, NK_DISC_READ_FAILED,
                                 "The disc image could not be read to the end.");
            }
            break;
        }
    }
    fclose(f);
    free(buffer);
    /* The file changed size while it was read: the hash is of no one image. */
    if (ok && total >= 0 && (uint64_t)total != done) {
        ok = verify_fail(status, NK_DISC_READ_FAILED,
                         "The disc image changed while it was being checked.");
    }
    if (!ok) return false;
    uint8_t digest[20];
    nk_sha1_finish(&sha1, digest);
    hex_encode(digest, sizeof(digest), out->sha1);
    snprintf(out->crc32, sizeof(out->crc32), "%08x", (unsigned)crc);
    out->size_bytes = done;
    return true;
}

/* ---- datafile scanner -------------------------------------------------- */

typedef struct {
    const char *p;
    const char *end;
} Span;

/* The first occurrence of needle in [p, end), or NULL. */
static const char *span_find(const char *p, const char *end, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0 || (size_t)(end - p) < n) return NULL;
    for (const char *q = p; q + n <= end; q++) {
        if (*q == needle[0] && memcmp(q, needle, n) == 0) return q;
    }
    return NULL;
}

/* An element's opening tag "<name" followed by whitespace, "/" or ">". */
static const char *find_tag(const char *p, const char *end, const char *name) {
    size_t n = strlen(name);
    for (;;) {
        const char *q = span_find(p, end, "<");
        if (!q) return NULL;
        if ((size_t)(end - q) > n + 1u && memcmp(q + 1, name, n) == 0) {
            char next = q[1u + n];
            if (next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == '/' ||
                next == '>') {
                return q;
            }
        }
        p = q + 1;
    }
}

static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static unsigned hex_value(char c) {
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
    return (unsigned)(c - 'A' + 10);
}

/* The value of attribute `name` inside a tag [p, end), unquoted, or false. */
static bool tag_attribute(const char *p, const char *end, const char *name, Span *out) {
    size_t n = strlen(name);
    for (const char *q = p; q + n + 2u <= end; q++) {
        bool starts = q == p || q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r' || q[-1] == '\n';
        if (!starts || memcmp(q, name, n) != 0) continue;
        const char *r = q + n;
        while (r < end && (*r == ' ' || *r == '\t')) r++;
        if (r >= end || *r != '=') continue;
        r++;
        while (r < end && (*r == ' ' || *r == '\t')) r++;
        if (r >= end || *r != '"') continue;
        const char *value = r + 1;
        const char *close = value;
        while (close < end && *close != '"') close++;
        if (close >= end) return false;
        out->p = value;
        out->end = close;
        return true;
    }
    return false;
}

static bool parse_hex_bytes(const Span *value, uint8_t *out, size_t count) {
    if ((size_t)(value->end - value->p) != 2u * count) return false;
    for (size_t i = 0; i < count; i++) {
        char hi = value->p[2u * i], lo = value->p[2u * i + 1u];
        if (!is_hex_digit(hi) || !is_hex_digit(lo)) return false;
        out[i] = (uint8_t)((hex_value(hi) << 4) | hex_value(lo));
    }
    return true;
}

static bool parse_decimal(const Span *value, uint64_t *out) {
    size_t length = (size_t)(value->end - value->p);
    uint64_t result = 0;
    if (length == 0 || length > 20u) return false;
    for (size_t i = 0; i < length; i++) {
        char c = value->p[i];
        if (c < '0' || c > '9') return false;
        uint64_t digit = (uint64_t)(c - '0');
        if (result > (UINT64_MAX - digit) / 10u) return false;
        result = result * 10u + digit;
    }
    *out = result;
    return true;
}

size_t nk_disc_normalize_serials(const char *text, size_t length, char (*out_ids)[10],
                                 size_t max_ids) {
    size_t count = 0;
    size_t i = 0;
    while (i < length && count < max_ids) {
        /* One comma-separated part with spaces and dashes taken out. */
        char compact[32];
        size_t used = 0;
        while (i < length && text[i] != ',') {
            char c = text[i++];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '-') continue;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (used + 1u < sizeof(compact)) compact[used++] = c;
        }
        if (i < length) i++; /* the comma */
        compact[used] = '\0';
        bool retail = used >= 9u;
        for (size_t k = 0; retail && k < 9u; k++) {
            char c = compact[k];
            retail = k < 4u ? (c >= 'A' && c <= 'Z') : (c >= '0' && c <= '9');
        }
        if (!retail) continue;
        bool seen = false;
        for (size_t k = 0; k < count && !seen; k++) seen = memcmp(out_ids[k], compact, 9u) == 0;
        if (seen) continue;
        memcpy(out_ids[count], compact, 9u);
        out_ids[count][9] = '\0';
        count++;
    }
    return count;
}

/* The text of element `name` (e.g. "<name>...</name>") inside [p, end). */
static bool element_text(const char *p, const char *end, const char *name, Span *out) {
    char open[32], close[32];
    snprintf(open, sizeof(open), "<%s>", name);
    snprintf(close, sizeof(close), "</%s", name);
    const char *start = span_find(p, end, open);
    if (!start) return false;
    start += strlen(open);
    const char *stop = span_find(start, end, close);
    if (!stop) return false;
    out->p = start;
    out->end = stop;
    return true;
}

/* Copy element text, with the five predefined XML entities decoded. */
static void copy_text(const Span *text, char *out, size_t out_size) {
    static const struct { const char *entity; char c; } entities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''},
    };
    size_t used = 0;
    for (const char *q = text->p; q < text->end && used + 1u < out_size;) {
        bool decoded = false;
        for (size_t e = 0; *q == '&' && e < sizeof(entities) / sizeof(entities[0]); e++) {
            size_t n = strlen(entities[e].entity);
            if ((size_t)(text->end - q) >= n && memcmp(q, entities[e].entity, n) == 0) {
                out[used++] = entities[e].c;
                q += n;
                decoded = true;
                break;
            }
        }
        if (!decoded) out[used++] = *q++;
    }
    out[used] = '\0';
    while (used > 0 && (out[used - 1u] == ' ' || out[used - 1u] == '\n' || out[used - 1u] == '\r' ||
                        out[used - 1u] == '\t')) {
        out[--used] = '\0';
    }
}

static int rom_compare(const void *a, const void *b) {
    return memcmp(((const NkDiscDatRom *)a)->sha1, ((const NkDiscDatRom *)b)->sha1, 20u);
}

static int serial_compare(const void *a, const void *b) {
    return memcmp(a, b, 9u);
}

void nk_disc_dat_free(NkDiscDat *dat) {
    if (!dat) return;
    free(dat->roms);
    free(dat->serials);
    memset(dat, 0, sizeof(*dat));
}

static bool dat_push_rom(NkDiscDat *dat, size_t *capacity, const NkDiscDatRom *rom) {
    if (dat->rom_count == *capacity) {
        size_t grown = *capacity ? *capacity * 2u : 256u;
        NkDiscDatRom *roms = (NkDiscDatRom *)realloc(dat->roms, grown * sizeof(*roms));
        if (!roms) return false;
        dat->roms = roms;
        *capacity = grown;
    }
    dat->roms[dat->rom_count++] = *rom;
    return true;
}

static bool dat_push_serial(NkDiscDat *dat, size_t *capacity, const char id[10]) {
    if (dat->serial_count == *capacity) {
        size_t grown = *capacity ? *capacity * 2u : 256u;
        char (*serials)[10] = realloc(dat->serials, grown * sizeof(*serials));
        if (!serials) return false;
        dat->serials = serials;
        *capacity = grown;
    }
    memcpy(dat->serials[dat->serial_count++], id, 10u);
    return true;
}

bool nk_disc_dat_parse(const char *text, size_t length, NkDiscDat *out,
                       NkDiscExpandStatus *status) {
    verify_status_clear(status);
    if (!out) return verify_fail(status, NK_DISC_INTERNAL, "No datafile result was given.");
    memset(out, 0, sizeof(*out));
    if (!text) return verify_fail(status, NK_DISC_INTERNAL, "No datafile text was given.");
    const char *end = text + length;
    if (!find_tag(text, end, "datafile")) {
        return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                           "This file isn't a Redump datafile: it has no <datafile> element.");
    }
    const char *header = find_tag(text, end, "header");
    const char *header_end = header ? span_find(header, end, "</header") : NULL;
    if (header && header_end) {
        Span value;
        if (element_text(header, header_end, "name", &value)) {
            copy_text(&value, out->name, sizeof(out->name));
        }
        if (element_text(header, header_end, "version", &value)) {
            copy_text(&value, out->version, sizeof(out->version));
        }
    }
    size_t rom_capacity = 0, serial_capacity = 0;
    const char *p = text;
    for (;;) {
        const char *game = find_tag(p, end, "game");
        if (!game) break;
        const char *body = span_find(game, end, ">");
        const char *game_end = body ? span_find(body, end, "</game") : NULL;
        if (!game_end) break; /* an unterminated last element holds no complete rom */
        out->games++;
        char ids[NK_DISC_MAX_GAME_SERIALS][10];
        size_t id_count = 0;
        for (const char *s = body; id_count < NK_DISC_MAX_GAME_SERIALS;) {
            Span serial;
            if (!element_text(s, game_end, "serial", &serial)) break;
            id_count += nk_disc_normalize_serials(serial.p, (size_t)(serial.end - serial.p),
                                                  ids + id_count,
                                                  NK_DISC_MAX_GAME_SERIALS - id_count);
            s = serial.end;
        }
        for (size_t k = 0; k < id_count; k++) {
            if (!dat_push_serial(out, &serial_capacity, ids[k])) {
                nk_disc_dat_free(out);
                return verify_fail(status, NK_DISC_INTERNAL, "No memory was free to read the datafile.");
            }
        }
        for (const char *r = body;;) {
            const char *rom = find_tag(r, game_end, "rom");
            if (!rom) break;
            const char *rom_end = span_find(rom, game_end, ">");
            if (!rom_end) break;
            Span sha1, crc, size;
            NkDiscDatRom entry;
            uint8_t crc_bytes[4];
            memset(&entry, 0, sizeof(entry));
            if (tag_attribute(rom, rom_end, "sha1", &sha1) &&
                tag_attribute(rom, rom_end, "crc", &crc) &&
                tag_attribute(rom, rom_end, "size", &size) &&
                parse_hex_bytes(&sha1, entry.sha1, 20u) &&
                parse_hex_bytes(&crc, crc_bytes, 4u) &&
                parse_decimal(&size, &entry.size_bytes)) {
                if (out->rom_count >= NK_DISC_DAT_MAX_ROMS) {
                    nk_disc_dat_free(out);
                    return verify_fail(status, NK_DISC_DAT_TOO_LARGE,
                                       "The datafile lists more than %u discs.",
                                       (unsigned)NK_DISC_DAT_MAX_ROMS);
                }
                entry.crc32 = ((uint32_t)crc_bytes[0] << 24) | ((uint32_t)crc_bytes[1] << 16) |
                              ((uint32_t)crc_bytes[2] << 8) | (uint32_t)crc_bytes[3];
                if (!dat_push_rom(out, &rom_capacity, &entry)) {
                    nk_disc_dat_free(out);
                    return verify_fail(status, NK_DISC_INTERNAL,
                                       "No memory was free to read the datafile.");
                }
            } else {
                out->rejected_roms++;
            }
            r = rom_end + 1;
        }
        p = game_end + 1;
    }
    if (out->rom_count == 0) {
        nk_disc_dat_free(out);
        return verify_fail(status, NK_DISC_DAT_EMPTY, "The datafile lists no disc with a SHA-1 hash.");
    }
    qsort(out->roms, out->rom_count, sizeof(out->roms[0]), rom_compare);
    /* A SHA-1 listed twice keeps its first entry, as the Python reader does. */
    size_t kept = 0;
    for (size_t i = 0; i < out->rom_count; i++) {
        if (kept > 0 && memcmp(out->roms[kept - 1u].sha1, out->roms[i].sha1, 20u) == 0) continue;
        out->roms[kept++] = out->roms[i];
    }
    out->rom_count = kept;
    if (out->serial_count > 0) {
        qsort(out->serials, out->serial_count, sizeof(out->serials[0]), serial_compare);
        size_t unique = 0;
        for (size_t i = 0; i < out->serial_count; i++) {
            if (unique > 0 && memcmp(out->serials[unique - 1u], out->serials[i], 9u) == 0) continue;
            memcpy(out->serials[unique++], out->serials[i], 10u);
        }
        out->serial_count = unique;
    }
    return true;
}

static bool read_bounded_file(const char *path, char **out_text, size_t *out_length,
                              NkDiscExpandStatus *status) {
    *out_text = NULL;
    *out_length = 0;
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) return verify_fail(status, NK_DISC_DAT_READ_FAILED, "The datafile can't be opened.");
    char *buffer = (char *)malloc((size_t)NK_DISC_DAT_MAX_BYTES + 1u);
    if (!buffer) {
        fclose(f);
        return verify_fail(status, NK_DISC_INTERNAL, "No memory was free to read the datafile.");
    }
    size_t got = fread(buffer, 1, (size_t)NK_DISC_DAT_MAX_BYTES + 1u, f);
    bool failed = ferror(f) != 0;
    fclose(f);
    if (failed) {
        free(buffer);
        return verify_fail(status, NK_DISC_DAT_READ_FAILED, "The datafile can't be read.");
    }
    if (got > NK_DISC_DAT_MAX_BYTES) {
        free(buffer);
        return verify_fail(status, NK_DISC_DAT_TOO_LARGE, "The datafile is larger than %u bytes.",
                           (unsigned)NK_DISC_DAT_MAX_BYTES);
    }
    buffer[got] = '\0';
    *out_text = buffer;
    *out_length = got;
    return true;
}

bool nk_disc_dat_load(const char *path, NkDiscDat *out, NkDiscExpandStatus *status) {
    char *text = NULL;
    size_t length = 0;
    if (out) memset(out, 0, sizeof(*out));
    if (!path || !path[0] || !out) {
        return verify_fail(status, NK_DISC_INTERNAL, "No datafile was given.");
    }
    if (!read_bounded_file(path, &text, &length, status)) return false;
    /* A UTF-8 byte-order mark is allowed, as the Python reader allows it. */
    size_t skip = length >= 3u && memcmp(text, "\xEF\xBB\xBF", 3u) == 0 ? 3u : 0u;
    bool ok = nk_disc_dat_parse(text + skip, length - skip, out, status);
    free(text);
    return ok;
}

bool nk_disc_dat_path(const char *data_root, char *out, size_t out_size) {
    if (!data_root || !data_root[0] || !out || out_size == 0) return false;
    char sep = nk_platform_path_separator();
    int written = snprintf(out, out_size, "%s%c%s%c%s", data_root, sep, NK_DISC_DAT_DIR, sep,
                           NK_DISC_DAT_FILE);
    return written > 0 && (size_t)written < out_size;
}

bool nk_disc_dat_import(const char *source, const char *data_root, NkDiscDat *out,
                        NkDiscExpandStatus *status) {
    char *text = NULL;
    size_t length = 0;
    char target[NK_MAX_PATH];
    char dir[NK_MAX_PATH];
    char tmp[NK_MAX_PATH + 8];
    NkDiscDat parsed;
    verify_status_clear(status);
    if (out) memset(out, 0, sizeof(*out));
    if (!source || !source[0] || !nk_disc_dat_path(data_root, target, sizeof(target))) {
        return verify_fail(status, NK_DISC_INTERNAL, "No datafile or data folder was given.");
    }
    if (!read_bounded_file(source, &text, &length, status)) return false;
    size_t skip = length >= 3u && memcmp(text, "\xEF\xBB\xBF", 3u) == 0 ? 3u : 0u;
    if (!nk_disc_dat_parse(text + skip, length - skip, &parsed, status)) {
        free(text);
        return false;
    }
    int n = snprintf(dir, sizeof(dir), "%s%c%s", data_root, nk_platform_path_separator(),
                     NK_DISC_DAT_DIR);
    int m = snprintf(tmp, sizeof(tmp), "%s.tmp", target);
    bool ok = n > 0 && (size_t)n < sizeof(dir) && m > 0 && (size_t)m < sizeof(tmp) &&
              nk_platform_mkdir_p_private(dir);
    if (ok) {
        FILE *f = nk_platform_fopen_private(tmp, "wb");
        ok = f != NULL;
        if (ok) {
            ok = fwrite(text, 1, length, f) == length;
            ok = (fclose(f) == 0) && ok;
        }
        if (ok) ok = nk_rename_utf8(tmp, target) == 0;
        if (!ok) (void)nk_remove_utf8(tmp);
    }
    free(text);
    if (!ok) {
        nk_disc_dat_free(&parsed);
        return verify_fail(status, NK_DISC_DAT_WRITE_FAILED,
                           "The datafile can't be saved in the data folder.");
    }
    if (out) {
        *out = parsed;
    } else {
        nk_disc_dat_free(&parsed);
    }
    return true;
}

bool nk_disc_dat_has_serial(const NkDiscDat *dat, const char *disc_id) {
    if (!dat || !dat->serials || !disc_id || strlen(disc_id) != 9u) return false;
    return bsearch(disc_id, dat->serials, dat->serial_count, sizeof(dat->serials[0]),
                   serial_compare) != NULL;
}

NkDiscTier nk_disc_classify(const NkDiscDat *dat, const char *disc_id,
                            const NkDiscDigest *digest, bool disc_id_verified_elsewhere) {
    if (!dat || !dat->roms || !digest || !digest->sha1[0]) return NK_DISC_TIER_NOT_CHECKED;
    NkDiscDatRom key;
    Span sha1 = {digest->sha1, digest->sha1 + strlen(digest->sha1)};
    Span crc = {digest->crc32, digest->crc32 + strlen(digest->crc32)};
    uint8_t crc_bytes[4];
    memset(&key, 0, sizeof(key));
    if (parse_hex_bytes(&sha1, key.sha1, 20u) && parse_hex_bytes(&crc, crc_bytes, 4u)) {
        uint32_t crc32 = ((uint32_t)crc_bytes[0] << 24) | ((uint32_t)crc_bytes[1] << 16) |
                         ((uint32_t)crc_bytes[2] << 8) | (uint32_t)crc_bytes[3];
        const NkDiscDatRom *rom = (const NkDiscDatRom *)bsearch(
            &key, dat->roms, dat->rom_count, sizeof(dat->roms[0]), rom_compare);
        if (rom && rom->crc32 == crc32 && rom->size_bytes == digest->size_bytes) {
            return NK_DISC_TIER_VERIFIED_RETAIL;
        }
    }
    if (disc_id && disc_id[0] && (disc_id_verified_elsewhere || nk_disc_dat_has_serial(dat, disc_id))) {
        return NK_DISC_TIER_MODIFIED;
    }
    return NK_DISC_TIER_UNKNOWN;
}
