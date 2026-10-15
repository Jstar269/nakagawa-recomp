/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Project-authored DEFLATE/zlib/gzip decoder for the built-in PSP
 * decryption boundary (issue #295).  Written from the public RFC 1950
 * (zlib), RFC 1951 (DEFLATE) and RFC 1952 (gzip) specifications; no
 * third-party inflate implementation was copied.
 *
 * Fail-closed contract: every malformed, truncated, checksum-mismatched
 * or over-capacity stream returns a negative code with a message; no
 * partial buffer is ever handed to the caller.
 */

#include "nk_psp_inflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NK_INFLATE_DEFAULT_CAP (64u * 1024u * 1024u)
#define NK_INFLATE_MAXBITS 15
#define NK_INFLATE_MAXLITS 288
#define NK_INFLATE_MAXDISTS 32
#define NK_INFLATE_MAXCODES 19

typedef struct Bits {
    const uint8_t *src;
    size_t len;
    size_t pos;  /* index of the next byte to load into acc */
    uint32_t acc; /* partial byte, consumed least-significant bit first */
    int have;     /* bits remaining in acc */
    int over;     /* ran past the end of input */
} Bits;

typedef struct Huffman {
    int count[NK_INFLATE_MAXBITS + 1]; /* codes of each length 1..15 */
    int symbol[NK_INFLATE_MAXLITS];    /* symbols ordered by code */
} Huffman;

typedef struct OutBuf {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t limit; /* hard fail-closed output ceiling */
} OutBuf;

static void set_err(char *err, size_t err_len, const char *msg)
{
    if (err != NULL && err_len > 0u) {
        (void)snprintf(err, err_len, "%s", msg);
    }
}

static int bit_next(Bits *b)
{
    if (b->have == 0) {
        if (b->pos >= b->len) {
            b->over = 1;
            return 0;
        }
        b->acc = b->src[b->pos++];
        b->have = 8;
    }
    {
        int v = (int)(b->acc & 1u);
        b->acc >>= 1;
        b->have--;
        return v;
    }
}

/* Read n (0 <= n <= 15) bits, least-significant bit first. */
static int bits_n(Bits *b, int n, uint32_t *v)
{
    uint32_t r = 0u;
    int i;
    for (i = 0; i < n; i++) {
        if (b->over) {
            return -1;
        }
        r |= (uint32_t)bit_next(b) << i;
    }
    if (b->over) {
        return -1;
    }
    *v = r;
    return 0;
}

static void bits_align(Bits *b)
{
    b->have = 0;
    b->acc = 0u;
}

static int huff_build(Huffman *h, const uint8_t *lens, int n, char *err,
                      size_t err_len)
{
    int offs[NK_INFLATE_MAXBITS + 1];
    int left = 1;
    int used = 0;
    int len;
    int i;

    for (len = 0; len <= NK_INFLATE_MAXBITS; len++) {
        h->count[len] = 0;
    }
    for (i = 0; i < n; i++) {
        if (lens[i] > NK_INFLATE_MAXBITS) {
            set_err(err, err_len, "inflate: code length exceeds 15 bits");
            return NK_INFLATE_ERR_FORMAT;
        }
        if (lens[i] != 0) {
            h->count[lens[i]]++;
            used++;
        }
    }
    /* Reject oversubscribed code sets (left underflows). */
    for (len = 1; len <= NK_INFLATE_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            set_err(err, err_len, "inflate: oversubscribed huffman code");
            return NK_INFLATE_ERR_FORMAT;
        }
    }
    /*
     * An incomplete set is tolerated only when it holds a single code
     * (the legal zero-distance / single-symbol case); anything else is
     * malformed and fails closed.
     */
    if (left > 0 && used > 1) {
        set_err(err, err_len, "inflate: incomplete huffman code");
        return NK_INFLATE_ERR_FORMAT;
    }
    offs[1] = 0;
    for (len = 1; len < NK_INFLATE_MAXBITS; len++) {
        offs[len + 1] = offs[len] + h->count[len];
    }
    for (i = 0; i < n; i++) {
        if (lens[i] != 0) {
            h->symbol[offs[lens[i]]++] = i;
        }
    }
    return NK_INFLATE_OK;
}

/* Decode one Huffman symbol (codes packed most-significant bit first). */
static int huff_dec(Bits *b, const Huffman *h, int *out)
{
    int code = 0;
    int first = 0;
    int index = 0;
    int len;
    for (len = 1; len <= NK_INFLATE_MAXBITS; len++) {
        int cnt;
        if (b->over) {
            return -1;
        }
        code |= bit_next(b);
        if (b->over) {
            return -1;
        }
        cnt = h->count[len];
        if (code - first < cnt) {
            *out = h->symbol[index + (code - first)];
            return 0;
        }
        index += cnt;
        first = (first + cnt) << 1;
        code <<= 1;
    }
    return -1;
}

static int out_put(OutBuf *o, uint8_t v)
{
    if (o->len == o->cap) {
        size_t nc = (o->cap != 0u) ? o->cap * 2u : 4096u;
        uint8_t *nd;
        if (nc < 65536u) {
            nc = 65536u;
        }
        if (nc > o->limit) {
            nc = o->limit;
        }
        if (nc <= o->cap) {
            return NK_INFLATE_ERR_OVERFLOW;
        }
        nd = (uint8_t *)realloc(o->data, nc);
        if (nd == NULL) {
            return NK_INFLATE_ERR_MEMORY;
        }
        o->data = nd;
        o->cap = nc;
    }
    o->data[o->len++] = v;
    return NK_INFLATE_OK;
}

static int out_copy(OutBuf *o, size_t dist, size_t length)
{
    size_t i;
    if (dist == 0u || dist > o->len) {
        return NK_INFLATE_ERR_FORMAT;
    }
    for (i = 0; i < length; i++) {
        int rc = out_put(o, o->data[o->len - dist]);
        if (rc != NK_INFLATE_OK) {
            return rc;
        }
    }
    return NK_INFLATE_OK;
}

static int nk_deflate(Bits *b, OutBuf *o, char *err, size_t err_len)
{
    static const uint16_t len_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t len_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dist_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577};
    static const uint8_t dist_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    static const uint8_t cl_order[NK_INFLATE_MAXCODES] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    int final = 0;

    while (!final) {
        uint32_t v = 0u;
        int btype;
        Huffman lh;
        Huffman dh;
        uint8_t lens[NK_INFLATE_MAXLITS + NK_INFLATE_MAXDISTS];

        if (b->over) {
            set_err(err, err_len, "inflate: truncated deflate stream");
            return NK_INFLATE_ERR_TRUNCATED;
        }
        final = bit_next(b);
        if (b->over) {
            set_err(err, err_len, "inflate: truncated deflate stream");
            return NK_INFLATE_ERR_TRUNCATED;
        }
        if (bits_n(b, 2, &v) != 0) {
            set_err(err, err_len, "inflate: truncated deflate stream");
            return NK_INFLATE_ERR_TRUNCATED;
        }
        btype = (int)v;

        if (btype == 0) {
            /* Stored (copy) block. */
            uint32_t len16 = 0u;
            uint32_t nlen16 = 0u;
            uint32_t nb = 0u;
            uint32_t i;
            bits_align(b);
            for (i = 0; i < 4u; i++) {
                if (b->pos >= b->len) {
                    set_err(err, err_len,
                            "inflate: truncated stored block header");
                    return NK_INFLATE_ERR_TRUNCATED;
                }
                if (i < 2u) {
                    len16 |= (uint32_t)b->src[b->pos] << (8u * i);
                } else {
                    nlen16 |= (uint32_t)b->src[b->pos] << (8u * (i - 2u));
                }
                b->pos++;
            }
            if ((nlen16 ^ 0xFFFFu) != len16) {
                set_err(err, err_len, "inflate: stored block length mismatch");
                return NK_INFLATE_ERR_FORMAT;
            }
            if (b->len - b->pos < (size_t)len16) {
                set_err(err, err_len, "inflate: truncated stored block");
                return NK_INFLATE_ERR_TRUNCATED;
            }
            for (nb = 0; nb < len16; nb++) {
                int rc = out_put(o, b->src[b->pos++]);
                if (rc == NK_INFLATE_ERR_OVERFLOW) {
                    set_err(err, err_len, "inflate: output exceeds limit");
                    return rc;
                }
                if (rc != NK_INFLATE_OK) {
                    set_err(err, err_len, "inflate: out of memory");
                    return rc;
                }
            }
        } else if (btype == 1 || btype == 2) {
            int nlit = 288;
            int ndist = 32;
            int nclen = 4;
            int i;

            if (btype == 1) {
                /* Fixed Huffman codes. */
                uint8_t flens[NK_INFLATE_MAXLITS];
                uint8_t dlens[NK_INFLATE_MAXDISTS];
                for (i = 0; i < 144; i++) {
                    flens[i] = 8;
                }
                for (; i < 256; i++) {
                    flens[i] = 9;
                }
                for (; i < 280; i++) {
                    flens[i] = 7;
                }
                for (; i < 288; i++) {
                    flens[i] = 8;
                }
                for (i = 0; i < NK_INFLATE_MAXDISTS; i++) {
                    dlens[i] = 5;
                }
                if (huff_build(&lh, flens, 288, err, err_len) !=
                    NK_INFLATE_OK) {
                    return NK_INFLATE_ERR_FORMAT;
                }
                if (huff_build(&dh, dlens, NK_INFLATE_MAXDISTS, err,
                               err_len) != NK_INFLATE_OK) {
                    return NK_INFLATE_ERR_FORMAT;
                }
            } else {
                /* Dynamic Huffman block header. */
                uint8_t cl_lens[NK_INFLATE_MAXCODES];
                Huffman clh;
                int total;
                int pos = 0;
                int prev = 0;

                if (bits_n(b, 5, &v) != 0) {
                    set_err(err, err_len,
                            "inflate: truncated dynamic block header");
                    return NK_INFLATE_ERR_TRUNCATED;
                }
                nlit = (int)v + 257;
                if (bits_n(b, 5, &v) != 0) {
                    set_err(err, err_len,
                            "inflate: truncated dynamic block header");
                    return NK_INFLATE_ERR_TRUNCATED;
                }
                ndist = (int)v + 1;
                if (bits_n(b, 4, &v) != 0) {
                    set_err(err, err_len,
                            "inflate: truncated dynamic block header");
                    return NK_INFLATE_ERR_TRUNCATED;
                }
                nclen = (int)v + 4;
                if (nlit > 286 || ndist > NK_INFLATE_MAXDISTS) {
                    set_err(err, err_len, "inflate: bad dynamic code counts");
                    return NK_INFLATE_ERR_FORMAT;
                }
                memset(cl_lens, 0, sizeof(cl_lens));
                for (i = 0; i < nclen; i++) {
                    if (bits_n(b, 3, &v) != 0) {
                        set_err(err, err_len,
                                "inflate: truncated code-length table");
                        return NK_INFLATE_ERR_TRUNCATED;
                    }
                    cl_lens[cl_order[i]] = (uint8_t)v;
                }
                if (huff_build(&clh, cl_lens, NK_INFLATE_MAXCODES, err,
                               err_len) != NK_INFLATE_OK) {
                    return NK_INFLATE_ERR_FORMAT;
                }

                total = nlit + ndist;
                memset(lens, 0, (size_t)total);
                while (pos < total) {
                    int sym;
                    int rep;
                    uint32_t extra = 0u;
                    if (huff_dec(b, &clh, &sym) != 0) {
                        if (b->over) {
                            set_err(err, err_len,
                                    "inflate: truncated code-length data");
                            return NK_INFLATE_ERR_TRUNCATED;
                        }
                        set_err(err, err_len,
                                "inflate: invalid code-length symbol");
                        return NK_INFLATE_ERR_FORMAT;
                    }
                    if (sym < 16) {
                        lens[pos++] = (uint8_t)sym;
                        prev = sym;
                        continue;
                    }
                    if (sym == 16) {
                        if (pos == 0) {
                            set_err(err, err_len,
                                    "inflate: repeat with no previous length");
                            return NK_INFLATE_ERR_FORMAT;
                        }
                        if (bits_n(b, 2, &extra) != 0) {
                            set_err(err, err_len,
                                    "inflate: truncated repeat count");
                            return NK_INFLATE_ERR_TRUNCATED;
                        }
                        rep = 3 + (int)extra;
                    } else if (sym == 17) {
                        if (bits_n(b, 3, &extra) != 0) {
                            set_err(err, err_len,
                                    "inflate: truncated zero run");
                            return NK_INFLATE_ERR_TRUNCATED;
                        }
                        rep = 3 + (int)extra;
                        prev = 0;
                    } else {
                        if (bits_n(b, 7, &extra) != 0) {
                            set_err(err, err_len,
                                    "inflate: truncated zero run");
                            return NK_INFLATE_ERR_TRUNCATED;
                        }
                        rep = 11 + (int)extra;
                        prev = 0;
                    }
                    if (pos + rep > total) {
                        set_err(err, err_len,
                                "inflate: code-length run overruns table");
                        return NK_INFLATE_ERR_FORMAT;
                    }
                    for (i = 0; i < rep; i++) {
                        lens[pos++] = (uint8_t)prev;
                    }
                }
                if (lens[256] == 0) {
                    set_err(err, err_len,
                            "inflate: block has no end-of-block code");
                    return NK_INFLATE_ERR_FORMAT;
                }
                if (huff_build(&lh, lens, nlit, err, err_len) !=
                    NK_INFLATE_OK) {
                    return NK_INFLATE_ERR_FORMAT;
                }
                if (huff_build(&dh, lens + nlit, ndist, err, err_len) !=
                    NK_INFLATE_OK) {
                    return NK_INFLATE_ERR_FORMAT;
                }
            }
        } else {
            set_err(err, err_len, "inflate: invalid deflate block type");
            return NK_INFLATE_ERR_FORMAT;
        }

        /* Decode the block body. */
        for (;;) {
            int sym;
            if (huff_dec(b, &lh, &sym) != 0) {
                if (b->over) {
                    set_err(err, err_len, "inflate: truncated deflate stream");
                    return NK_INFLATE_ERR_TRUNCATED;
                }
                set_err(err, err_len, "inflate: invalid literal/length code");
                return NK_INFLATE_ERR_FORMAT;
            }
            if (sym < 256) {
                int rc = out_put(o, (uint8_t)sym);
                if (rc == NK_INFLATE_ERR_OVERFLOW) {
                    set_err(err, err_len, "inflate: output exceeds limit");
                    return rc;
                }
                if (rc != NK_INFLATE_OK) {
                    set_err(err, err_len, "inflate: out of memory");
                    return rc;
                }
                continue;
            }
            if (sym == 256) {
                break;
            }
            {
                int li;
                uint32_t extra = 0u;
                size_t length;
                size_t dist;
                int dsym;
                int rc;

                if (sym < 257 || sym > 285) {
                    set_err(err, err_len, "inflate: invalid length code");
                    return NK_INFLATE_ERR_FORMAT;
                }
                li = sym - 257;
                if (bits_n(b, len_extra[li], &extra) != 0) {
                    set_err(err, err_len, "inflate: truncated length bits");
                    return NK_INFLATE_ERR_TRUNCATED;
                }
                length = (size_t)len_base[li] + (size_t)extra;

                if (huff_dec(b, &dh, &dsym) != 0) {
                    if (b->over) {
                        set_err(err, err_len,
                                "inflate: truncated distance code");
                        return NK_INFLATE_ERR_TRUNCATED;
                    }
                    set_err(err, err_len, "inflate: invalid distance code");
                    return NK_INFLATE_ERR_FORMAT;
                }
                if (dsym < 0 || dsym >= 30) {
                    set_err(err, err_len, "inflate: invalid distance symbol");
                    return NK_INFLATE_ERR_FORMAT;
                }
                if (bits_n(b, dist_extra[dsym], &extra) != 0) {
                    set_err(err, err_len, "inflate: truncated distance bits");
                    return NK_INFLATE_ERR_TRUNCATED;
                }
                dist = (size_t)dist_base[dsym] + (size_t)extra;

                if (dist > o->len) {
                    set_err(err, err_len, "inflate: distance too far back");
                    return NK_INFLATE_ERR_FORMAT;
                }
                rc = out_copy(o, dist, length);
                if (rc == NK_INFLATE_ERR_OVERFLOW) {
                    set_err(err, err_len, "inflate: output exceeds limit");
                    return rc;
                }
                if (rc == NK_INFLATE_ERR_MEMORY) {
                    set_err(err, err_len, "inflate: out of memory");
                    return rc;
                }
                if (rc != NK_INFLATE_OK) {
                    set_err(err, err_len, "inflate: invalid back-reference");
                    return NK_INFLATE_ERR_FORMAT;
                }
            }
        }
    }

    return NK_INFLATE_OK;
}

static uint32_t crc32_run(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < n; i++) {
        int k;
        crc ^= (uint32_t)p[i];
        for (k = 0; k < 8; k++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

static uint32_t adler32_run(const uint8_t *p, size_t n)
{
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    while (n > 0u) {
        size_t k = (n < 5552u) ? n : 5552u;
        n -= k;
        while (k-- > 0u) {
            s1 += (uint32_t)*p++;
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
    }
    return (s2 << 16) | s1;
}

/* Skip a NUL-terminated header string; returns 0 on success. */
static int gzip_skip_str(Bits *b, size_t trailer_start, const char **msg)
{
    while (b->pos < trailer_start) {
        if (b->src[b->pos] == 0u) {
            b->pos++;
            return 0;
        }
        b->pos++;
    }
    *msg = "inflate: gzip header string runs past input";
    return -1;
}

static int decode_and_check(Bits *b, OutBuf *o, char *err, size_t err_len)
{
    int rc = nk_deflate(b, o, err, err_len);
    if (rc != NK_INFLATE_OK) {
        free(o->data);
        o->data = NULL;
        o->cap = 0u;
        o->len = 0u;
    }
    return rc;
}

int nk_psp_inflate(const uint8_t *src, size_t srclen, uint8_t **out,
                   size_t *outlen, size_t expect, size_t cap, char *err,
                   size_t err_len)
{
    Bits b;
    OutBuf o;
    size_t init_cap;
    int mode; /* 0 = raw, 1 = zlib, 2 = gzip */
    int rc;

    if (out == NULL || outlen == NULL || (src == NULL && srclen != 0u)) {
        set_err(err, err_len, "inflate: invalid arguments");
        return NK_INFLATE_ERR_FORMAT;
    }
    *out = NULL;
    *outlen = 0u;
    if (err != NULL && err_len > 0u) {
        err[0] = '\0';
    }
    if (srclen == 0u) {
        set_err(err, err_len, "inflate: empty input");
        return NK_INFLATE_ERR_FORMAT;
    }
    if (cap == 0u) {
        cap = NK_INFLATE_DEFAULT_CAP;
    }

    memset(&b, 0, sizeof(b));
    memset(&o, 0, sizeof(o));
    o.limit = cap;

    init_cap = (expect != 0u) ? expect : srclen * 4u;
    if (init_cap < 16384u) {
        init_cap = 16384u;
    }
    if (init_cap > (1u << 20)) {
        init_cap = 1u << 20;
    }
    if (init_cap > cap) {
        init_cap = cap;
    }
    if (init_cap == 0u) {
        init_cap = 1u;
    }
    o.data = (uint8_t *)malloc(init_cap);
    if (o.data == NULL) {
        set_err(err, err_len, "inflate: out of memory");
        return NK_INFLATE_ERR_MEMORY;
    }
    o.cap = init_cap;

    /* Auto-detect the wrapper. */
    if (srclen >= 2u && src[0] == 0x1Fu && src[1] == 0x8Bu) {
        mode = 2;
    } else if (srclen >= 2u && (src[0] & 0x0Fu) == 8u &&
               (((uint32_t)src[0] << 8) | (uint32_t)src[1]) % 31u == 0u) {
        mode = 1;
    } else {
        mode = 0;
    }

    b.src = src;
    b.len = srclen;

    if (mode == 2) {
        /* gzip (RFC 1952). */
        uint8_t flg;
        size_t hdr_end;
        const char *hmsg = NULL;
        if (srclen < 10u + 8u) {
            set_err(err, err_len, "inflate: gzip input too short");
            free(o.data);
            return NK_INFLATE_ERR_TRUNCATED;
        }
        if (src[2] != 8u) {
            set_err(err, err_len, "inflate: unsupported gzip compression method");
            free(o.data);
            return NK_INFLATE_ERR_FORMAT;
        }
        flg = src[3];
        if ((flg & 0xE0u) != 0u) {
            set_err(err, err_len, "inflate: gzip header reserved bits set");
            free(o.data);
            return NK_INFLATE_ERR_FORMAT;
        }
        b.pos = 10u;
        if ((flg & 0x04u) != 0u) { /* FEXTRA */
            uint32_t xlen;
            if (b.len - b.pos < 2u) {
                set_err(err, err_len, "inflate: truncated gzip extra field");
                free(o.data);
                return NK_INFLATE_ERR_TRUNCATED;
            }
            xlen = (uint32_t)src[b.pos] | ((uint32_t)src[b.pos + 1u] << 8);
            b.pos += 2u;
            if (b.len - b.pos < (size_t)xlen + 8u) {
                set_err(err, err_len, "inflate: truncated gzip extra field");
                free(o.data);
                return NK_INFLATE_ERR_TRUNCATED;
            }
            b.pos += (size_t)xlen;
        }
        hdr_end = b.len;
        if ((flg & 0x08u) != 0u) { /* FNAME */
            if (gzip_skip_str(&b, hdr_end, &hmsg) != 0) {
                set_err(err, err_len, hmsg);
                free(o.data);
                return NK_INFLATE_ERR_TRUNCATED;
            }
        }
        if ((flg & 0x10u) != 0u) { /* FCOMMENT */
            if (gzip_skip_str(&b, hdr_end, &hmsg) != 0) {
                set_err(err, err_len, hmsg);
                free(o.data);
                return NK_INFLATE_ERR_TRUNCATED;
            }
        }
        if ((flg & 0x02u) != 0u) { /* FHCRC */
            uint32_t want;
            uint32_t got;
            if (b.len - b.pos < 2u) {
                set_err(err, err_len, "inflate: truncated gzip header crc");
                free(o.data);
                return NK_INFLATE_ERR_TRUNCATED;
            }
            want = (uint32_t)src[b.pos] | ((uint32_t)src[b.pos + 1u] << 8);
            got = crc32_run(src, b.pos) & 0xFFFFu;
            b.pos += 2u;
            if (want != got) {
                set_err(err, err_len, "inflate: gzip header crc mismatch");
                free(o.data);
                return NK_INFLATE_ERR_CHECKSUM;
            }
        }
        rc = decode_and_check(&b, &o, err, err_len);
        if (rc != NK_INFLATE_OK) {
            return rc;
        }
        {
            uint32_t want_crc;
            uint32_t want_len;
            uint32_t got_crc;
            if (b.len - b.pos < 8u) {
                set_err(err, err_len, "inflate: truncated gzip trailer");
                free(o.data);
                return NK_INFLATE_ERR_TRUNCATED;
            }
            want_crc = (uint32_t)src[b.pos] | ((uint32_t)src[b.pos + 1u] << 8) |
                       ((uint32_t)src[b.pos + 2u] << 16) |
                       ((uint32_t)src[b.pos + 3u] << 24);
            want_len = (uint32_t)src[b.pos + 4u] |
                       ((uint32_t)src[b.pos + 5u] << 8) |
                       ((uint32_t)src[b.pos + 6u] << 16) |
                       ((uint32_t)src[b.pos + 7u] << 24);
            got_crc = crc32_run(o.data, o.len);
            if (got_crc != want_crc ||
                (uint32_t)(o.len & 0xFFFFFFFFu) != want_len) {
                set_err(err, err_len, "inflate: gzip crc or size mismatch");
                free(o.data);
                return NK_INFLATE_ERR_CHECKSUM;
            }
        }
    } else if (mode == 1) {
        /* zlib (RFC 1950): a 2-byte header, the deflate stream, then a
         * 4-byte adler32 trailer.  Anything shorter cannot be a zlib
         * stream, so it is rejected before the header flags (src[3]) and
         * the trailer are read. */
        if (srclen < 6u) {
            set_err(err, err_len, "inflate: truncated zlib stream");
            free(o.data);
            return NK_INFLATE_ERR_TRUNCATED;
        }
        b.pos = 2u;
        if ((src[3] & 0x20u) != 0u) {
            set_err(err, err_len,
                    "inflate: zlib preset dictionary is not supported");
            free(o.data);
            return NK_INFLATE_ERR_FORMAT;
        }
        rc = decode_and_check(&b, &o, err, err_len);
        if (rc != NK_INFLATE_OK) {
            /*
             * The leading bytes may have been raw DEFLATE that merely
             * resembled a zlib header: retry as raw before failing.
             */
            Bits b2;
            OutBuf o2;
            memset(&b2, 0, sizeof(b2));
            memset(&o2, 0, sizeof(o2));
            o2.limit = cap;
            o2.cap = init_cap;
            o2.data = (uint8_t *)malloc(init_cap);
            b2.src = src;
            b2.len = srclen;
            if (o2.data == NULL) {
                set_err(err, err_len, "inflate: out of memory");
                return NK_INFLATE_ERR_MEMORY;
            }
            if (decode_and_check(&b2, &o2, err, err_len) == NK_INFLATE_OK) {
                *out = o2.data;
                *outlen = o2.len;
                return NK_INFLATE_OK;
            }
            free(o2.data);
            return rc;
        }
        {
            uint32_t want;
            uint32_t got;
            if (b.len - b.pos < 4u) {
                set_err(err, err_len, "inflate: truncated zlib adler32");
                free(o.data);
                return NK_INFLATE_ERR_TRUNCATED;
            }
            want = ((uint32_t)src[b.pos] << 24) |
                   ((uint32_t)src[b.pos + 1u] << 16) |
                   ((uint32_t)src[b.pos + 2u] << 8) |
                   (uint32_t)src[b.pos + 3u];
            got = adler32_run(o.data, o.len);
            if (got != want) {
                set_err(err, err_len, "inflate: zlib adler32 mismatch");
                free(o.data);
                return NK_INFLATE_ERR_CHECKSUM;
            }
        }
    } else {
        /* Raw DEFLATE. */
        b.pos = 0u;
        rc = decode_and_check(&b, &o, err, err_len);
        if (rc != NK_INFLATE_OK) {
            free(o.data);
            return rc;
        }
    }

    *out = o.data;
    *outlen = o.len;
    return NK_INFLATE_OK;
}
