/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * Compressed disc images expanded once at import (src/core/nk_disc_expand.c,
 * src/core/nk_disc_chd.c).
 *
 * Every fixture is generated here from a synthetic ISO9660 image built in
 * memory (a PSP_GAME folder, a PARAM.SFO naming disc TEST00042, and a data
 * file of zero, pseudo-random, text and counting sectors). No disc image of
 * any game is involved. Writers in this file produce:
 *
 *   - CSO v1: DEFLATE blocks (fixed-Huffman with back-references, and stored
 *     DEFLATE blocks) plus stored CSO blocks, with index alignment 0 and 2;
 *   - CSO v2: LZ4-flagged blocks, DEFLATE blocks and blocks stored by size;
 *   - ZSO: LZ4 blocks with literals and overlapping matches, plus stored blocks;
 *   - CHD v5 DVD images: an uncompressed map (with an all-zero hunk) and a
 *     compressed map (a Huffman-coded type table) whose hunks use the CHD
 *     "zlib" codec (fixed-Huffman DEFLATE) or are stored uncompressed.
 *
 * CHD coverage: libchdr's v5 header and metadata reader, the uncompressed and
 * compressed v5 map decoders, the hunk CRC check, and the "zlib" codec path
 * (miniz's inflater over fixed-Huffman DEFLATE with back-references). The
 * lzma, huff, flac and zstd codecs, the CD codecs and self/parent hunk
 * references are covered only by libchdr upstream: there is no chdman here to
 * make them. The DVD/CD/hard-disk decision is tested through the metadata tag
 * each image carries.
 */

#if !defined(_WIN32) && !defined(_WIN64)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "native_test_isolation.h"
#include "nk_disc_chd.h"
#include "nk_disc_expand.h"
#include "nk_iso.h"
#include "nk_platform.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2048u
#define ISO_SECTORS 64u
#define ISO_BYTES (SECTOR * ISO_SECTORS)
#define DATA_FIRST 21u

static uint8_t g_iso[ISO_BYTES];
static uint8_t g_other_iso[ISO_BYTES]; /* an ISO9660 image with no PSP_GAME folder */
static char g_dir[NATIVE_TEST_PATH_MAX];
static int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

/* ---- small helpers ----------------------------------------------------- */

static void path_in(char *out, size_t size, const char *name) {
    int n = snprintf(out, size, "%s%c%s", g_dir, nk_platform_path_separator(), name);
    assert(n > 0 && (size_t)n < size);
}

static void write_bytes(const char *path, const uint8_t *data, size_t size) {
    FILE *f = nk_fopen_utf8(path, "wb");
    assert(f != NULL);
    assert(fwrite(data, 1, size, f) == size);
    assert(fclose(f) == 0);
}

static uint8_t *read_all(const char *path, size_t *size) {
    FILE *f = nk_fopen_utf8(path, "rb");
    uint8_t *data;
    long end;
    if (!f) return NULL;
    assert(fseek(f, 0, SEEK_END) == 0);
    end = ftell(f);
    assert(end >= 0);
    assert(fseek(f, 0, SEEK_SET) == 0);
    data = (uint8_t *)malloc((size_t)end + 1u);
    assert(data != NULL);
    assert(fread(data, 1, (size_t)end, f) == (size_t)end);
    fclose(f);
    *size = (size_t)end;
    return data;
}

static bool file_equals(const char *path, const uint8_t *expected, size_t size) {
    size_t got = 0;
    uint8_t *data = read_all(path, &got);
    bool same = data && got == size && memcmp(data, expected, size) == 0;
    free(data);
    return same;
}

static void put_le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { put_le16(p, v); put_le16(p + 2, v >> 16); }
static void put_le64(uint8_t *p, uint64_t v) { put_le32(p, (uint32_t)v); put_le32(p + 4, (uint32_t)(v >> 32)); }
static void put_be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) { put_be16(p, v >> 16); put_be16(p + 2, v); }
static void put_be64(uint8_t *p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(p + 4, (uint32_t)v); }

/* A growable output buffer. */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    while (b->len + extra > b->cap) b->cap = b->cap ? b->cap * 2u : 4096u;
    b->data = (uint8_t *)realloc(b->data, b->cap);
    assert(b->data != NULL);
}

static void buf_put(Buf *b, const void *src, size_t n) {
    buf_reserve(b, n);
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void buf_zero(Buf *b, size_t n) {
    buf_reserve(b, n);
    memset(b->data + b->len, 0, n);
    b->len += n;
}

/* ---- the synthetic ISO -------------------------------------------------- */

static void iso_both32(uint8_t *p, uint32_t v) {
    put_le32(p, v);
    put_be32(p + 4, v);
}

static size_t iso_record(uint8_t *dir, size_t offset, const char *name, size_t name_len,
                         uint32_t lba, uint32_t size, bool is_dir) {
    size_t len = 33u + name_len + ((33u + name_len) & 1u);
    memset(dir + offset, 0, len);
    dir[offset] = (uint8_t)len;
    iso_both32(dir + offset + 2, lba);
    iso_both32(dir + offset + 10, size);
    dir[offset + 25] = is_dir ? 2u : 0u;
    dir[offset + 28] = 1;
    dir[offset + 31] = 1;
    dir[offset + 32] = (uint8_t)name_len;
    memcpy(dir + offset + 33, name, name_len);
    return len;
}

static size_t build_sfo(uint8_t *out, const char *disc_id) {
    const char *keys[4] = { "CATEGORY", "DISC_ID", "DISC_VERSION", "TITLE" };
    const char *values[4] = { "UG", disc_id, "1.00", "Synthetic Disc" };
    size_t key_start = 20u + 4u * 16u;
    size_t key_len = 0;
    size_t data_start;
    size_t data_len = 0;
    for (int i = 0; i < 4; i++) key_len += strlen(keys[i]) + 1u;
    data_start = key_start + ((key_len + 3u) & ~(size_t)3u);
    memcpy(out, "\0PSF", 4);
    put_le32(out + 4, 0x0101u);
    put_le32(out + 8, (uint32_t)key_start);
    put_le32(out + 12, (uint32_t)data_start);
    put_le32(out + 16, 4u);
    key_len = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t *entry = out + 20 + i * 16;
        size_t vlen = strlen(values[i]) + 1u;
        size_t vmax = (vlen + 3u) & ~(size_t)3u;
        put_le16(entry, (uint32_t)key_len);
        put_le16(entry + 2, 0x0204u);
        put_le32(entry + 4, (uint32_t)vlen);
        put_le32(entry + 8, (uint32_t)vmax);
        put_le32(entry + 12, (uint32_t)data_len);
        memcpy(out + key_start + key_len, keys[i], strlen(keys[i]) + 1u);
        memcpy(out + data_start + data_len, values[i], vlen);
        key_len += strlen(keys[i]) + 1u;
        data_len += vmax;
    }
    return data_start + data_len;
}

static void build_iso(uint8_t *image, bool psp) {
    uint8_t *pvd = image + 16u * SECTOR;
    uint8_t *root = image + 18u * SECTOR;
    uint32_t state = 0x2545F491u;
    size_t off;
    memset(image, 0, ISO_BYTES);
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memcpy(pvd + 40, "SYNTHETIC_DISC                  ", 32);
    iso_both32(pvd + 80, ISO_SECTORS);
    iso_record(pvd, 156, "\0", 1, 18, SECTOR, true);
    image[17u * SECTOR] = 255;
    memcpy(image + 17u * SECTOR + 1, "CD001", 5);
    image[17u * SECTOR + 6] = 1;
    off = iso_record(root, 0, "\0", 1, 18, SECTOR, true);
    off += iso_record(root, off, "\1", 1, 18, SECTOR, true);
    if (psp) {
        uint8_t *game = image + 19u * SECTOR;
        size_t sfo_len = build_sfo(image + 20u * SECTOR, "TEST00042");
        size_t g = iso_record(game, 0, "\0", 1, 19, SECTOR, true);
        off += iso_record(root, off, "DATA.BIN;1", 10, DATA_FIRST,
                          (ISO_SECTORS - DATA_FIRST) * SECTOR, false);
        off += iso_record(root, off, "PSP_GAME", 8, 19, SECTOR, true);
        g += iso_record(game, g, "\1", 1, 18, SECTOR, true);
        iso_record(game, g, "PARAM.SFO;1", 11, 20, (uint32_t)sfo_len, false);
    } else {
        off += iso_record(root, off, "DATA.BIN;1", 10, DATA_FIRST,
                          (ISO_SECTORS - DATA_FIRST) * SECTOR, false);
    }
    (void)off;
    for (uint32_t s = DATA_FIRST; s < ISO_SECTORS; s++) {
        uint8_t *sector = image + (size_t)s * SECTOR;
        switch (s % 4u) {
        case 0: /* zeros */
            break;
        case 1: /* pseudo-random: does not compress */
            for (uint32_t i = 0; i < SECTOR; i++) {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                sector[i] = (uint8_t)state;
            }
            break;
        case 2: { /* repeating text */
            static const char text[] = "synthetic disc data, sector pattern two. ";
            for (uint32_t i = 0; i < SECTOR; i++) sector[i] = (uint8_t)text[i % (sizeof(text) - 1u)];
            break;
        }
        default: /* counting */
            for (uint32_t i = 0; i < SECTOR; i++) sector[i] = (uint8_t)(i * 7u + s);
            break;
        }
    }
}

/* ---- DEFLATE writers (stored and fixed-Huffman blocks) ------------------- */

typedef struct {
    Buf *out;
    uint32_t acc;
    int bits;
} BitWriter;

static void bw_put(BitWriter *w, uint32_t value, int count) {
    for (int i = 0; i < count; i++) {
        w->acc |= ((value >> i) & 1u) << w->bits;
        if (++w->bits == 8) {
            uint8_t byte = (uint8_t)w->acc;
            buf_put(w->out, &byte, 1);
            w->acc = 0;
            w->bits = 0;
        }
    }
}

/* Huffman codes go most significant bit first. */
static void bw_code(BitWriter *w, uint32_t code, int length) {
    for (int i = length - 1; i >= 0; i--) bw_put(w, (code >> i) & 1u, 1);
}

static void bw_flush(BitWriter *w) {
    if (w->bits) bw_put(w, 0, 8 - w->bits);
}

static void fixed_symbol(BitWriter *w, uint32_t sym) {
    if (sym < 144u) bw_code(w, 0x30u + sym, 8);
    else if (sym < 256u) bw_code(w, 0x190u + (sym - 144u), 9);
    else if (sym < 280u) bw_code(w, sym - 256u, 7);
    else bw_code(w, 0xC0u + (sym - 280u), 8);
}

static void fixed_length(BitWriter *w, uint32_t length) {
    static const uint16_t base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227,
                                       258 };
    static const uint8_t extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    int code = 28;
    while (base[code] > length) code--;
    fixed_symbol(w, 257u + (uint32_t)code);
    bw_put(w, length - base[code], extra[code]);
    bw_code(w, 0, 5); /* distance code 0: distance 1 */
}

/* Raw DEFLATE, one fixed-Huffman block: runs of a repeated byte become
   distance-1 matches, everything else literals. */
static void deflate_fixed(Buf *out, const uint8_t *src, size_t n) {
    BitWriter w = { out, 0, 0 };
    size_t i = 0;
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    while (i < n) {
        size_t run = 0;
        fixed_symbol(&w, src[i]);
        i++;
        while (i + run < n && src[i + run] == src[i - 1] && run < 258u) run++;
        if (run >= 3u) {
            fixed_length(&w, (uint32_t)run);
            i += run;
        }
    }
    fixed_symbol(&w, 256u);
    bw_flush(&w);
}

static void deflate_stored(Buf *out, const uint8_t *src, size_t n) {
    uint8_t head[5];
    assert(n <= 65535u);
    head[0] = 1;
    put_le16(head + 1, (uint32_t)n);
    put_le16(head + 3, (uint32_t)(~n & 0xFFFFu));
    buf_put(out, head, sizeof(head));
    buf_put(out, src, n);
}

/* ---- LZ4 block writer ------------------------------------------------------ */

static void lz4_length(Buf *out, size_t extra) {
    while (extra >= 255u) {
        uint8_t b = 255;
        buf_put(out, &b, 1);
        extra -= 255u;
    }
    {
        uint8_t b = (uint8_t)extra;
        buf_put(out, &b, 1);
    }
}

static void lz4_sequence(Buf *out, const uint8_t *lit, size_t lit_len, size_t offset,
                         size_t match_len) {
    uint8_t token = (uint8_t)((lit_len >= 15u ? 15u : lit_len) << 4);
    if (match_len) token |= (uint8_t)(match_len - 4u >= 15u ? 15u : match_len - 4u);
    buf_put(out, &token, 1);
    if (lit_len >= 15u) lz4_length(out, lit_len - 15u);
    buf_put(out, lit, lit_len);
    if (match_len) {
        uint8_t off[2];
        put_le16(off, (uint32_t)offset);
        buf_put(out, off, 2);
        if (match_len - 4u >= 15u) lz4_length(out, match_len - 19u);
    }
}

/* A greedy LZ4 block: offsets 1..64, the end-of-block rules kept (the last
   match starts 12 or more bytes before the end, the last 5 bytes are
   literals). */
static void lz4_compress(Buf *out, const uint8_t *src, size_t n) {
    size_t ip = 0;
    size_t anchor = 0;
    while (ip + 12u <= n) {
        size_t best_len = 0;
        size_t best_off = 0;
        size_t limit = n - 5u - ip;
        for (size_t off = 1; off <= 64u && off <= ip; off++) {
            size_t len = 0;
            while (len < limit && src[ip + len] == src[ip + len - off]) len++;
            if (len > best_len) {
                best_len = len;
                best_off = off;
            }
        }
        if (best_len >= 4u) {
            lz4_sequence(out, src + anchor, ip - anchor, best_off, best_len);
            ip += best_len;
            anchor = ip;
        } else {
            ip++;
        }
    }
    lz4_sequence(out, src + anchor, n - anchor, 0, 0);
}

/* ---- CSO / ZSO writer ----------------------------------------------------- */

typedef enum { BLOCK_STORED, BLOCK_DEFLATE_FIXED, BLOCK_DEFLATE_STORED, BLOCK_LZ4 } BlockKind;

static BlockKind cso_v1_kind(uint32_t block) {
    switch (block % 3u) {
    case 0: return BLOCK_DEFLATE_FIXED;
    case 1: return BLOCK_DEFLATE_STORED;
    default: return BLOCK_STORED;
    }
}

static BlockKind cso_v2_kind(uint32_t block) {
    switch (block % 3u) {
    case 0: return BLOCK_LZ4;
    case 1: return BLOCK_DEFLATE_FIXED;
    default: return BLOCK_STORED;
    }
}

static BlockKind zso_kind(uint32_t block) {
    return (block % 5u) == 4u ? BLOCK_STORED : BLOCK_LZ4;
}

/* Build a CISO/ZISO image of `iso`. version 1 or 2 for "CISO", 1 for "ZISO". */
static void build_ciso(Buf *out, const char *magic, uint8_t version, uint8_t align,
                       const uint8_t *iso, size_t iso_len, BlockKind (*kind)(uint32_t)) {
    uint32_t blocks = (uint32_t)(iso_len / SECTOR);
    size_t index_off = 24u;
    size_t pos;
    uint8_t header[24];
    memset(header, 0, sizeof(header));
    memcpy(header, magic, 4);
    put_le32(header + 4, 24u);
    put_le64(header + 8, iso_len);
    put_le32(header + 16, SECTOR);
    header[20] = version;
    header[21] = align;
    out->len = 0;
    buf_put(out, header, sizeof(header));
    buf_zero(out, ((size_t)blocks + 1u) * 4u);
    pos = out->len;
    for (uint32_t i = 0; i <= blocks; i++) {
        uint32_t entry;
        BlockKind k = i < blocks ? kind(i) : BLOCK_STORED;
        size_t pad = ((size_t)1u << align) - 1u;
        /* Pad each block start to the index alignment. */
        while (out->len & pad) buf_zero(out, 1);
        pos = out->len;
        entry = (uint32_t)(pos >> align);
        if (i < blocks) {
            const uint8_t *src = iso + (size_t)i * SECTOR;
            bool plain_flag = false;
            bool v2 = magic[0] == 'C' && version == 2u;
            size_t start = out->len;
            if (k == BLOCK_DEFLATE_FIXED) {
                deflate_fixed(out, src, SECTOR);
            } else if (k == BLOCK_DEFLATE_STORED) {
                deflate_stored(out, src, SECTOR);
            } else if (k == BLOCK_LZ4) {
                lz4_compress(out, src, SECTOR);
                plain_flag = v2; /* v2: the flag marks LZ4 */
            }
            /* v2 marks a stored block by its size, so a block that did not
               shrink is written stored, as a v2 writer does. */
            if (k == BLOCK_STORED || (v2 && out->len - start >= SECTOR)) {
                out->len = start;
                buf_put(out, src, SECTOR);
                plain_flag = !v2;
            }
            if (plain_flag) entry |= 0x80000000u;
        }
        put_le32(out->data + index_off + (size_t)i * 4u, entry);
    }
}

/* ---- CHD v5 writer ---------------------------------------------------------- */

static uint16_t crc16_ccitt(const uint8_t *data, size_t n) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

typedef struct {
    Buf *out;
    uint32_t acc;
    int bits;
} MsbWriter;

static void msb_put(MsbWriter *w, uint32_t value, int count) {
    for (int i = count - 1; i >= 0; i--) {
        w->acc = (w->acc << 1) | ((value >> i) & 1u);
        if (++w->bits == 8) {
            uint8_t byte = (uint8_t)w->acc;
            buf_put(w->out, &byte, 1);
            w->acc = 0;
            w->bits = 0;
        }
    }
}

static void msb_flush(MsbWriter *w) {
    while (w->bits) msb_put(w, 0, 1);
}

#define CHD_HUNK 4096u
#define CHD_TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

typedef enum { CHD_MAP_UNCOMPRESSED, CHD_MAP_COMPRESSED } ChdMap;

/* A CHD v5 image of `iso` with one metadata entry (`meta_tag`). */
static void build_chd(Buf *out, const uint8_t *iso, size_t iso_len, ChdMap map, uint32_t meta_tag) {
    uint32_t hunks = (uint32_t)((iso_len + CHD_HUNK - 1u) / CHD_HUNK);
    uint8_t header[124];
    size_t meta_off = 124u;
    size_t map_off;
    memset(header, 0, sizeof(header));
    memcpy(header, "MComprHD", 8);
    put_be32(header + 8, 124u);
    put_be32(header + 12, 5u);
    if (map == CHD_MAP_COMPRESSED) put_be32(header + 16, CHD_TAG('z', 'l', 'i', 'b'));
    put_be64(header + 32, iso_len);
    put_be64(header + 48, meta_off);
    put_be32(header + 56, CHD_HUNK);
    put_be32(header + 60, SECTOR);
    out->len = 0;
    buf_put(out, header, sizeof(header));
    {
        static const char text[] = "synthetic";
        uint8_t meta[16];
        put_be32(meta, meta_tag);
        put_be32(meta + 4, (uint32_t)sizeof(text));
        put_be64(meta + 8, 0);
        buf_put(out, meta, sizeof(meta));
        buf_put(out, text, sizeof(text));
    }
    map_off = out->len;
    put_be64(out->data + 40, map_off);

    if (map == CHD_MAP_UNCOMPRESSED) {
        /* Entries are hunk-aligned offsets / hunk size; 0 is an all-zero hunk. */
        size_t data_off;
        buf_zero(out, (size_t)hunks * 4u);
        while (out->len % CHD_HUNK) buf_zero(out, 1);
        data_off = out->len;
        for (uint32_t h = 0; h < hunks; h++) {
            const uint8_t *src = iso + (size_t)h * CHD_HUNK;
            bool zero = true;
            for (uint32_t i = 0; i < CHD_HUNK; i++) zero = zero && src[i] == 0;
            if (zero && h != 0u) continue; /* leave the entry 0 */
            put_be32(out->data + map_off + (size_t)h * 4u, (uint32_t)(out->len / CHD_HUNK));
            buf_put(out, src, CHD_HUNK);
        }
        (void)data_off;
        return;
    }

    /* Compressed map: hunk payloads first (in a side buffer), then the map. */
    {
        Buf payload = { NULL, 0, 0 };
        Buf bits = { NULL, 0, 0 };
        MsbWriter w = { &bits, 0, 0 };
        uint8_t *raw = (uint8_t *)calloc(hunks, 12u);
        uint32_t *lengths = (uint32_t *)calloc(hunks, sizeof(uint32_t));
        uint8_t *types = (uint8_t *)calloc(hunks, 1u);
        uint64_t first_offs;
        uint64_t cur;
        uint8_t maphdr[16];
        assert(raw && lengths && types);
        for (uint32_t h = 0; h < hunks; h++) {
            const uint8_t *src = iso + (size_t)h * CHD_HUNK;
            size_t before = payload.len;
            types[h] = 4; /* COMPRESSION_NONE */
            if (h % 2u == 0u) {
                deflate_fixed(&payload, src, CHD_HUNK);
                /* A compressed hunk must be smaller than a hunk; one that
                   did not shrink is stored, as chdman does. */
                if (payload.len - before < CHD_HUNK) {
                    types[h] = 0; /* COMPRESSION_TYPE_0: codec 0, zlib */
                } else {
                    payload.len = before;
                }
            }
            if (types[h] == 4) buf_put(&payload, src, CHD_HUNK);
            lengths[h] = (uint32_t)(payload.len - before);
        }
        /* Huffman tree for the 16 type symbols, 4 bits per entry: symbols 0
           and 4 get one-bit codes (0 and 1); a literal 1 is escaped as 1,1. */
        for (uint32_t s = 0; s < 16u; s++) {
            if (s == 0u || s == 4u) {
                msb_put(&w, 1, 4);
                msb_put(&w, 1, 4);
            } else {
                msb_put(&w, 0, 4);
            }
        }
        for (uint32_t h = 0; h < hunks; h++) msb_put(&w, types[h] == 0 ? 0u : 1u, 1);
        first_offs = 0; /* fixed below once the map size is known */
        for (uint32_t h = 0; h < hunks; h++) {
            uint16_t crc = crc16_ccitt(iso + (size_t)h * CHD_HUNK, CHD_HUNK);
            if (types[h] == 0) msb_put(&w, lengths[h], 16);
            msb_put(&w, crc, 16);
        }
        msb_flush(&w);
        first_offs = map_off + 16u + bits.len + 64u; /* a gap before the hunks */
        cur = first_offs;
        for (uint32_t h = 0; h < hunks; h++) {
            uint8_t *e = raw + (size_t)h * 12u;
            uint32_t len = lengths[h]; /* a NONE hunk records the hunk size */
            e[0] = types[h];
            e[1] = (uint8_t)(len >> 16);
            e[2] = (uint8_t)(len >> 8);
            e[3] = (uint8_t)len;
            for (int b = 0; b < 6; b++) e[4 + b] = (uint8_t)(cur >> (8 * (5 - b)));
            put_be16(e + 10, crc16_ccitt(iso + (size_t)h * CHD_HUNK, CHD_HUNK));
            cur += lengths[h];
        }
        put_be32(maphdr, (uint32_t)bits.len);
        for (int b = 0; b < 6; b++) maphdr[4 + b] = (uint8_t)(first_offs >> (8 * (5 - b)));
        put_be16(maphdr + 10, crc16_ccitt(raw, (size_t)hunks * 12u));
        maphdr[12] = 16; /* lengthbits */
        maphdr[13] = 0;  /* selfbits */
        maphdr[14] = 0;  /* parentbits */
        maphdr[15] = 0;
        buf_put(out, maphdr, sizeof(maphdr));
        buf_put(out, bits.data, bits.len);
        buf_zero(out, 64u);
        assert(out->len == first_offs);
        buf_put(out, payload.data, payload.len);
        free(payload.data);
        free(bits.data);
        free(raw);
        free(lengths);
        free(types);
    }
}

/* ---- callbacks ---------------------------------------------------------------- */

typedef struct {
    uint64_t last_done;
    uint64_t total;
    int calls;
    bool monotonic;
    int cancel_after;
} Progress;

static void on_progress(uint64_t done, uint64_t total, void *user) {
    Progress *p = (Progress *)user;
    if (done < p->last_done) p->monotonic = false;
    p->last_done = done;
    p->total = total;
    p->calls++;
}

static bool on_cancel(void *user) {
    Progress *p = (Progress *)user;
    return p->cancel_after >= 0 && p->calls > p->cancel_after;
}

/* ---- tests ---------------------------------------------------------------------- */

static void expect_expands(const char *label, const Buf *image, const uint8_t *iso, size_t iso_len) {
    char src[NATIVE_TEST_PATH_MAX];
    char dst[NATIVE_TEST_PATH_MAX];
    char part[NATIVE_TEST_PATH_MAX + 8];
    char name[64];
    NkDiscExpandStatus status;
    Progress progress = { 0, 0, 0, true, -1 };
    NkDiscExpandCallbacks callbacks = { on_progress, on_cancel, &progress };
    uint64_t bytes = 0;
    NkDiscFormat format;
    uint64_t probed = 0;
    snprintf(name, sizeof(name), "%s.img", label);
    path_in(src, sizeof(src), name);
    snprintf(name, sizeof(name), "%s.iso", label);
    path_in(dst, sizeof(dst), name);
    write_bytes(src, image->data, image->len);
    if (!nk_disc_expanded_size(src, &format, &probed, &status)) {
        fprintf(stderr, "%s: %s: %s\n", label, status.code, status.message);
        CHECK(false);
    }
    CHECK(probed == iso_len);
    if (!nk_disc_expand_file(src, dst, &callbacks, &bytes, &status)) {
        fprintf(stderr, "%s: %s: %s\n", label, status.code, status.message);
        CHECK(false);
    }
    CHECK(bytes == iso_len);
    CHECK(file_equals(dst, iso, iso_len));
    CHECK(progress.monotonic && progress.last_done == iso_len && progress.total == iso_len);
    snprintf(part, sizeof(part), "%s.part", dst);
    CHECK(nk_platform_path_missing(part));
    printf("PASS expand %s (%s, %zu -> %zu bytes)\n", label, nk_disc_format_extension(format),
           image->len, iso_len);
}

static void expect_refused(const char *label, const uint8_t *data, size_t len, const char *code) {
    char src[NATIVE_TEST_PATH_MAX];
    char dst[NATIVE_TEST_PATH_MAX];
    char part[NATIVE_TEST_PATH_MAX + 8];
    char name[64];
    NkDiscExpandStatus status;
    uint64_t bytes = 7;
    snprintf(name, sizeof(name), "bad-%s.img", label);
    path_in(src, sizeof(src), name);
    snprintf(name, sizeof(name), "bad-%s.iso", label);
    path_in(dst, sizeof(dst), name);
    write_bytes(src, data, len);
    CHECK(!nk_disc_expand_file(src, dst, NULL, &bytes, &status));
    if (strcmp(status.code, code) != 0) {
        fprintf(stderr, "%s: expected %s, got %s: %s\n", label, code, status.code, status.message);
        CHECK(false);
    }
    CHECK(status.message[0] != '\0');
    CHECK(bytes == 0);
    CHECK(nk_platform_path_missing(dst));
    snprintf(part, sizeof(part), "%s.part", dst);
    CHECK(nk_platform_path_missing(part));
    printf("PASS refuse %s -> %s\n", label, code);
}

static void test_cso_zso_round_trips(void) {
    Buf image = { NULL, 0, 0 };
    build_ciso(&image, "CISO", 1, 0, g_iso, ISO_BYTES, cso_v1_kind);
    expect_expands("cso-v1", &image, g_iso, ISO_BYTES);
    build_ciso(&image, "CISO", 1, 2, g_iso, ISO_BYTES, cso_v1_kind);
    expect_expands("cso-v1-align2", &image, g_iso, ISO_BYTES);
    build_ciso(&image, "CISO", 2, 0, g_iso, ISO_BYTES, cso_v2_kind);
    expect_expands("cso-v2", &image, g_iso, ISO_BYTES);
    build_ciso(&image, "ZISO", 1, 0, g_iso, ISO_BYTES, zso_kind);
    expect_expands("zso", &image, g_iso, ISO_BYTES);
    build_ciso(&image, "ZISO", 1, 3, g_iso, ISO_BYTES, zso_kind);
    expect_expands("zso-align3", &image, g_iso, ISO_BYTES);
    free(image.data);
}

static void test_chd_round_trips(void) {
    Buf image = { NULL, 0, 0 };
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_UNCOMPRESSED, CHD_TAG('D', 'V', 'D', ' '));
    expect_expands("chd-uncompressed-map", &image, g_iso, ISO_BYTES);
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_COMPRESSED, CHD_TAG('D', 'V', 'D', ' '));
    expect_expands("chd-zlib", &image, g_iso, ISO_BYTES);
    free(image.data);
}

static void test_malformed_refused(void) {
    Buf image = { NULL, 0, 0 };
    uint8_t *copy;

    /* Truncated index: 64 blocks named, the file ends after 4 entries. */
    build_ciso(&image, "CISO", 1, 0, g_iso, ISO_BYTES, cso_v1_kind);
    expect_refused("cso-truncated-index", image.data, 24u + 16u, NK_DISC_TRUNCATED);
    expect_refused("cso-truncated-header", image.data, 20u, NK_DISC_TRUNCATED);

    /* A block offset past the end of the file. */
    copy = (uint8_t *)malloc(image.len);
    assert(copy);
    memcpy(copy, image.data, image.len);
    put_le32(copy + 24u + 10u * 4u, (uint32_t)image.len + 4096u);
    expect_refused("cso-offset-past-end", copy, image.len, NK_DISC_INDEX_INVALID);

    /* Offsets going backwards. */
    memcpy(copy, image.data, image.len);
    put_le32(copy + 24u + 11u * 4u, 30u);
    expect_refused("cso-offset-backwards", copy, image.len, NK_DISC_INDEX_INVALID);

    /* A file that ends inside its last block. */
    expect_refused("cso-truncated-data", image.data, image.len - 100u, NK_DISC_INDEX_INVALID);

    /* Size overflow and sizes no disc has. */
    memcpy(copy, image.data, image.len);
    put_le64(copy + 8, UINT64_MAX - 1023u);
    expect_refused("cso-size-overflow", copy, image.len, NK_DISC_SIZE_INVALID);
    put_le64(copy + 8, NK_DISC_MAX_EXPANDED_BYTES + SECTOR);
    expect_refused("cso-size-too-large", copy, image.len, NK_DISC_SIZE_INVALID);
    put_le64(copy + 8, ISO_BYTES - 1u);
    expect_refused("cso-size-not-sectors", copy, image.len, NK_DISC_SIZE_INVALID);
    put_le64(copy + 8, 0);
    expect_refused("cso-size-zero", copy, image.len, NK_DISC_SIZE_INVALID);

    /* Version, header size, block size and alignment. */
    memcpy(copy, image.data, image.len);
    copy[20] = 3;
    expect_refused("cso-version-3", copy, image.len, NK_DISC_VERSION_UNSUPPORTED);
    memcpy(copy, image.data, image.len);
    put_le32(copy + 4, 32u);
    expect_refused("cso-header-size", copy, image.len, NK_DISC_HEADER_INVALID);
    memcpy(copy, image.data, image.len);
    put_le32(copy + 16, 3000u);
    expect_refused("cso-block-size", copy, image.len, NK_DISC_HEADER_INVALID);
    memcpy(copy, image.data, image.len);
    copy[21] = 31;
    expect_refused("cso-align", copy, image.len, NK_DISC_HEADER_INVALID);

    /* A DEFLATE block whose data is damaged (block 0 is fixed-Huffman). */
    memcpy(copy, image.data, image.len);
    {
        uint32_t first = (uint32_t)(copy[24] | (copy[25] << 8) | (copy[26] << 16));
        copy[first] = 0x07; /* BFINAL=1, BTYPE=11: reserved */
    }
    expect_refused("cso-bad-deflate", copy, image.len, NK_DISC_BLOCK_CORRUPT);
    free(copy);

    /* ZSO: wrong version, and an LZ4 match pointing before the block. */
    build_ciso(&image, "ZISO", 1, 0, g_iso, ISO_BYTES, zso_kind);
    copy = (uint8_t *)malloc(image.len);
    assert(copy);
    memcpy(copy, image.data, image.len);
    copy[20] = 2;
    expect_refused("zso-version-2", copy, image.len, NK_DISC_VERSION_UNSUPPORTED);
    memcpy(copy, image.data, image.len);
    {
        uint32_t first = (uint32_t)(copy[24] | (copy[25] << 8) | (copy[26] << 16));
        copy[first] = 0x0F; /* no literals, then a match */
        copy[first + 1] = 0x05;
        copy[first + 2] = 0x00; /* offset 5 with nothing written yet */
    }
    expect_refused("zso-bad-offset", copy, image.len, NK_DISC_BLOCK_CORRUPT);
    free(copy);

    /* CHD: a CD-layout image, a header with nothing after it, and a build
       without the CHD reader. */
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_UNCOMPRESSED, CHD_TAG('C', 'H', 'T', '2'));
    expect_refused("chd-cd", image.data, image.len, NK_DISC_CHD_CD_UNSUPPORTED);
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_UNCOMPRESSED, CHD_TAG('G', 'D', 'D', 'D'));
    expect_refused("chd-hard-disk", image.data, image.len, NK_DISC_CHD_UNSUPPORTED);
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_UNCOMPRESSED, CHD_TAG('D', 'V', 'D', ' '));
    expect_refused("chd-truncated", image.data, 124u, NK_DISC_CHD_INVALID);
    nk_disc_set_chd_backend(NULL);
    expect_refused("chd-no-backend", image.data, image.len, NK_DISC_CHD_UNAVAILABLE);
    nk_disc_chd_register();

    /* A plain ISO is not a compressed image. */
    expect_refused("plain-iso", g_iso, ISO_BYTES, NK_DISC_NOT_COMPRESSED);
    free(image.data);
}

static void test_cancel_removes_partial(void) {
    Buf image = { NULL, 0, 0 };
    char src[NATIVE_TEST_PATH_MAX];
    char dst[NATIVE_TEST_PATH_MAX];
    char part[NATIVE_TEST_PATH_MAX + 8];
    NkDiscExpandStatus status;
    /* Progress reports every MiB; the image is smaller, so cancel on the
       first report after the start. */
    Progress progress = { 0, 0, 0, true, 1 };
    NkDiscExpandCallbacks callbacks = { on_progress, on_cancel, &progress };
    build_ciso(&image, "CISO", 1, 0, g_iso, ISO_BYTES, cso_v1_kind);
    path_in(src, sizeof(src), "cancel.cso");
    path_in(dst, sizeof(dst), "cancel.iso");
    write_bytes(src, image.data, image.len);
    CHECK(!nk_disc_expand_file(src, dst, &callbacks, NULL, &status));
    CHECK(strcmp(status.code, NK_DISC_CANCELLED) == 0);
    CHECK(nk_platform_path_missing(dst));
    snprintf(part, sizeof(part), "%s.part", dst);
    CHECK(nk_platform_path_missing(part));
    free(image.data);
    printf("PASS cancel removes the partial image\n");
}

static void test_import_and_reuse(void) {
    Buf image = { NULL, 0, 0 };
    char src[NATIVE_TEST_PATH_MAX];
    char root[NATIVE_TEST_PATH_MAX];
    char expected[NATIVE_TEST_PATH_MAX + 64];
    char sep = nk_platform_path_separator();
    NkDiscImport result;
    NkDiscExpandStatus status;
    NkIsoMetadata meta;
    path_in(root, sizeof(root), "data-root");
    CHECK(nk_platform_mkdir_p(root));
    path_in(src, sizeof(src), "import.cso");
    build_ciso(&image, "CISO", 1, 0, g_iso, ISO_BYTES, cso_v1_kind);
    write_bytes(src, image.data, image.len);

    /* A compressed image is named by nk_iso_inspect, never read as ISO9660. */
    memset(&meta, 0, sizeof(meta));
    CHECK(nk_iso_inspect(src, &meta) != NK_OK);
    CHECK(strcmp(meta.boundary_code, "DISC_IMAGE_NOT_EXPANDED") == 0);

    CHECK(nk_disc_import_probe(src, root, &result, &status));
    CHECK(!result.reused && result.format == NK_DISC_FORMAT_CSO);
    CHECK(result.expanded_bytes == ISO_BYTES);
    CHECK(strlen(result.fingerprint) == NK_DISC_FINGERPRINT_CHARS);

    if (!nk_disc_import(src, root, NULL, &result, &status)) {
        fprintf(stderr, "import: %s: %s\n", status.code, status.message);
        CHECK(false);
    }
    snprintf(expected, sizeof(expected), "%s%cdiscs%cTEST00042%cTEST00042.cso.iso", root, sep,
             sep, sep);
    CHECK(!result.reused);
    CHECK(strcmp(result.disc_id, "TEST00042") == 0);
    CHECK(strcmp(result.iso_path, expected) == 0);
    CHECK(file_equals(result.iso_path, g_iso, ISO_BYTES));
    memset(&meta, 0, sizeof(meta));
    CHECK(nk_iso_inspect(result.iso_path, &meta) == NK_OK);
    CHECK(strcmp(meta.disc_id, "TEST00042") == 0 && meta.has_psp_game_folder);

    /* The same unchanged file again: reused, nothing written. */
    CHECK(nk_disc_import(src, root, NULL, &result, &status));
    CHECK(result.reused);
    CHECK(strcmp(result.iso_path, expected) == 0);

    /* An expansion whose image was damaged on the drive is not reused. */
    {
        FILE *f = nk_fopen_utf8(expected, "ab");
        assert(f);
        fputc(0, f);
        fclose(f);
    }
    CHECK(nk_disc_import_probe(src, root, &result, &status));
    CHECK(!result.reused);
    CHECK(nk_disc_import(src, root, NULL, &result, &status));
    CHECK(!result.reused && file_equals(result.iso_path, g_iso, ISO_BYTES));

    /* A different file (a ZSO of the same disc) gets its own expansion. */
    path_in(src, sizeof(src), "import.zso");
    build_ciso(&image, "ZISO", 1, 0, g_iso, ISO_BYTES, zso_kind);
    write_bytes(src, image.data, image.len);
    CHECK(nk_disc_import(src, root, NULL, &result, &status));
    CHECK(!result.reused && strstr(result.iso_path, "TEST00042.zso.iso") != NULL);
    CHECK(file_equals(result.iso_path, g_iso, ISO_BYTES));

    /* A CHD imports the same way. */
    path_in(src, sizeof(src), "import.chd");
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_COMPRESSED, CHD_TAG('D', 'V', 'D', ' '));
    write_bytes(src, image.data, image.len);
    CHECK(nk_disc_import(src, root, NULL, &result, &status));
    CHECK(!result.reused && strstr(result.iso_path, "TEST00042.chd.iso") != NULL);
    CHECK(file_equals(result.iso_path, g_iso, ISO_BYTES));
    CHECK(nk_disc_import(src, root, NULL, &result, &status) && result.reused);

    /* A compressed image of something that is not a PSP game is refused, and
       nothing of it is kept. */
    path_in(src, sizeof(src), "not-psp.cso");
    build_ciso(&image, "CISO", 1, 0, g_other_iso, ISO_BYTES, cso_v1_kind);
    write_bytes(src, image.data, image.len);
    CHECK(!nk_disc_import(src, root, NULL, &result, &status));
    CHECK(strcmp(status.code, NK_DISC_NOT_PSP) == 0);
    {
        char staged[NATIVE_TEST_PATH_MAX + 64];
        snprintf(staged, sizeof(staged), "%s%cdiscs%cexpanded%c%s.iso", root, sep, sep, sep,
                 result.fingerprint);
        CHECK(nk_platform_path_missing(staged));
    }
    free(image.data);
    printf("PASS import, reuse and per-title placement\n");
}

int main(void) {
    char name[64];
    native_test_create_root("disc-expand");
    native_test_isolate_user_data_roots();
    snprintf(name, sizeof(name), "fixtures");
    {
        int n = snprintf(g_dir, sizeof(g_dir), "%s%c%s", native_test_get_root(),
                         nk_platform_path_separator(), name);
        assert(n > 0 && (size_t)n < sizeof(g_dir));
    }
    assert(nk_platform_mkdir_p(g_dir));
    build_iso(g_iso, true);
    build_iso(g_other_iso, false);
    nk_disc_chd_register();

    test_cso_zso_round_trips();
    test_chd_round_trips();
    test_malformed_refused();
    test_cancel_removes_partial();
    test_import_and_reuse();
    printf("test_disc_expand: %d checks passed\n", g_checks);
    return 0;
}
