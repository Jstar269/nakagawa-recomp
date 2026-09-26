/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Project-authored ECDSA verification and point multiplication (issue #295),
 * written from the public SEC 1 / FIPS 186 specifications.  Upstream
 * libkirk's ec.c and bn.c are GPL-2.0-only and are NOT ported here.
 * All arithmetic is big-endian over fixed 160-bit values; curve parameters
 * arrive from the KeyStore.  Verification only -- there is no signing and
 * no key generation in this translation unit.
 */

#include <string.h>

#include "nk_psp_ec.h"

#define NK_EC_Len NK_EC_BYTES

static int ge(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < NK_EC_Len; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return 0;
}

static int is_zero(const uint8_t *a)
{
    for (int i = 0; i < NK_EC_Len; i++) {
        if (a[i]) return 0;
    }
    return 1;
}

/* out = a - b (20 bytes), returns borrow (assumes a >= b when borrow=0). */
static uint8_t sub_borrow(const uint8_t *a, const uint8_t *b, uint8_t *out)
{
    int borrow = 0;
    for (int i = NK_EC_Len - 1; i >= 0; i--) {
        int diff = (int)a[i] - (int)b[i] - borrow;
        if (diff < 0) { diff += 256; borrow = 1; } else { borrow = 0; }
        out[i] = (uint8_t)diff;
    }
    return (uint8_t)borrow;
}

static void add_carry(const uint8_t *a, const uint8_t *b, uint8_t *out, uint8_t *carry_out)
{
    int carry = 0;
    for (int i = NK_EC_Len - 1; i >= 0; i--) {
        int sum = (int)a[i] + (int)b[i] + carry;
        carry = sum >> 8;
        out[i] = (uint8_t)sum;
    }
    *carry_out = (uint8_t)carry;
}

/* out = (a + b) mod m (a, b < m). */
static void mod_add(const uint8_t *a, const uint8_t *b,
                    const uint8_t *m, uint8_t *out)
{
    uint8_t sum[NK_EC_Len];
    uint8_t carry = 0;
    add_carry(a, b, sum, &carry);
    if (carry || ge(sum, m)) {
        (void)sub_borrow(sum, m, out);
    } else {
        memcpy(out, sum, NK_EC_Len);
    }
}

/* out = (a - b) mod m (a, b < m). */
static void mod_sub(const uint8_t *a, const uint8_t *b,
                    const uint8_t *m, uint8_t *out)
{
    uint8_t diff[NK_EC_Len];
    uint8_t borrow = sub_borrow(a, b, diff);
    if (borrow) {
        uint8_t tmp[NK_EC_Len];
        uint8_t c2 = 0;
        add_carry(diff, m, tmp, &c2);
        memcpy(out, tmp, NK_EC_Len);
    } else {
        memcpy(out, diff, NK_EC_Len);
    }
}

/* Shift a 21-byte big-endian value left by one bit. */
static uint8_t shl21(uint8_t *v)
{
    uint8_t carry = 0;
    for (int i = NK_EC_Len; i >= 0; i--) {
        uint8_t next = (uint8_t)(v[i] >> 7);
        v[i] = (uint8_t)((v[i] << 1) | carry);
        carry = next;
    }
    return carry;
}

/*
 * out = (a * b) mod m via a 320-bit product reduced with binary
 * long division (shift-subtract).  m is a nonzero 160-bit modulus.
 */
static void mod_mul(const uint8_t *a, const uint8_t *b,
                    const uint8_t *m, uint8_t *out)
{
    uint8_t product[NK_EC_Len * 2];
    memset(product, 0, sizeof(product));
    for (int i = NK_EC_Len - 1; i >= 0; i--) {
        unsigned carry = 0;
        for (int j = NK_EC_Len - 1; j >= 0; j--) {
            unsigned v = (unsigned)product[i + j + 1] +
                         (unsigned)a[i] * (unsigned)b[j] + carry;
            product[i + j + 1] = (uint8_t)v;
            carry = v >> 8;
        }
        int k = i; /* propagate the row carry into higher-order bytes */
        while (carry != 0) {
            unsigned v = (unsigned)product[k] + carry;
            product[k] = (uint8_t)v;
            carry = v >> 8;
            k--;
        }
    }

    uint8_t acc[NK_EC_Len + 1];
    memset(acc, 0, sizeof(acc));
    for (int bit = 0; bit < (NK_EC_Len * 2) * 8; bit++) {
        (void)shl21(acc);
        acc[NK_EC_Len] |=
            (uint8_t)((product[bit / 8] >> (7 - (bit % 8))) & 1u);
        /* acc < 2m after the shift, so one conditional subtract fits it. */
        if (acc[0] != 0 || ge(acc + 1, m)) {
            uint8_t low[NK_EC_Len];
            uint8_t borrow = sub_borrow(acc + 1, m, low);
            memcpy(acc + 1, low, NK_EC_Len);
            acc[0] = (uint8_t)(acc[0] - borrow);
        }
    }
    memcpy(out, acc + 1, NK_EC_Len);
}

/* out = base^exp mod m (square and multiply). */
static void mod_pow(const uint8_t *base, const uint8_t *exp,
                    const uint8_t *m, uint8_t *out)
{
    uint8_t result[NK_EC_Len];
    uint8_t factor[NK_EC_Len];
    memset(result, 0, sizeof(result));
    result[NK_EC_Len - 1] = 1;
    memcpy(factor, base, NK_EC_Len);

    int started = 0;
    for (int i = 0; i < NK_EC_Len; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            if (started) {
                uint8_t sq[NK_EC_Len];
                mod_mul(result, result, m, sq);
                memcpy(result, sq, NK_EC_Len);
            }
            if ((exp[i] >> bit) & 1u) {
                uint8_t mulres[NK_EC_Len];
                mod_mul(result, factor, m, mulres);
                memcpy(result, mulres, NK_EC_Len);
                started = 1;
            }
        }
    }
    memcpy(out, result, NK_EC_Len);
}

/* Fermat inverse: a^(m-2) mod m (m prime). */
static void mod_inv(const uint8_t *a, const uint8_t *m, uint8_t *out)
{
    uint8_t exp[NK_EC_Len];
    uint8_t two[NK_EC_Len];
    memset(two, 0, sizeof(two));
    two[NK_EC_Len - 1] = 2;
    (void)sub_borrow(m, two, exp);
    mod_pow(a, exp, m, out);
}

typedef struct { int infinity; uint8_t x[NK_EC_Len]; uint8_t y[NK_EC_Len]; } EcPoint;

static void point_double(const NkEcParams *params, const EcPoint *p, EcPoint *out)
{
    if (p->infinity || is_zero(p->y)) {
        out->infinity = 1;
        return;
    }
    /* lambda = (3x^2 + a) / (2y) */
    uint8_t xx[NK_EC_Len], three_x[NK_EC_Len], num[NK_EC_Len];
    uint8_t two_y[NK_EC_Len], inv[NK_EC_Len], lambda[NK_EC_Len];
    mod_mul(p->x, p->x, params->p, xx);
    mod_add(xx, xx, params->p, three_x);
    mod_add(three_x, xx, params->p, num);
    mod_add(num, params->a, params->p, num);
    mod_add(p->y, p->y, params->p, two_y);
    mod_inv(two_y, params->p, inv);
    mod_mul(num, inv, params->p, lambda);

    uint8_t lambda2[NK_EC_Len], x3[NK_EC_Len], y3[NK_EC_Len], t[NK_EC_Len];
    mod_mul(lambda, lambda, params->p, lambda2);
    mod_sub(lambda2, p->x, params->p, x3);
    mod_sub(x3, p->x, params->p, x3);
    mod_sub(p->x, x3, params->p, t);
    mod_mul(lambda, t, params->p, y3);
    mod_sub(y3, p->y, params->p, y3);

    out->infinity = 0;
    memcpy(out->x, x3, NK_EC_Len);
    memcpy(out->y, y3, NK_EC_Len);
}

static void point_add(const NkEcParams *params, const EcPoint *p,
                      const EcPoint *q, EcPoint *out)
{
    if (p->infinity) { *out = *q; return; }
    if (q->infinity) { *out = *p; return; }
    uint8_t dx[NK_EC_Len];
    uint8_t borrow = sub_borrow(p->x, q->x, dx);
    if (borrow == 0 && is_zero(dx)) {
        /* Same x: either doubles or y1 = -y2 (infinity). */
        uint8_t sumy[NK_EC_Len];
        mod_add(p->y, q->y, params->p, sumy);
        if (is_zero(sumy)) { out->infinity = 1; return; }
        point_double(params, p, out);
        return;
    }
    uint8_t dy[NK_EC_Len], inv[NK_EC_Len], lambda[NK_EC_Len];
    mod_sub(p->y, q->y, params->p, dy);
    mod_inv(dx, params->p, inv);
    mod_mul(dy, inv, params->p, lambda);

    uint8_t lambda2[NK_EC_Len], x3[NK_EC_Len], y3[NK_EC_Len], t[NK_EC_Len];
    mod_mul(lambda, lambda, params->p, lambda2);
    mod_sub(lambda2, p->x, params->p, x3);
    mod_sub(x3, q->x, params->p, x3);
    mod_sub(p->x, x3, params->p, t);
    mod_mul(lambda, t, params->p, y3);
    mod_sub(y3, p->y, params->p, y3);

    out->infinity = 0;
    memcpy(out->x, x3, NK_EC_Len);
    memcpy(out->y, y3, NK_EC_Len);
}

static int point_on_curve(const NkEcParams *params, const EcPoint *p)
{
    uint8_t yy[NK_EC_Len], xxx[NK_EC_Len], ax[NK_EC_Len], rhs[NK_EC_Len];
    mod_mul(p->y, p->y, params->p, yy);
    mod_mul(p->x, p->x, params->p, xxx);
    mod_mul(xxx, p->x, params->p, xxx);
    mod_mul(params->a, p->x, params->p, ax);
    mod_add(xxx, ax, params->p, rhs);
    mod_add(rhs, params->b, params->p, rhs);
    return memcmp(yy, rhs, NK_EC_Len) == 0;
}

static void point_mul_scalar(const NkEcParams *params, const uint8_t k[NK_EC_Len],
                             const EcPoint *p, EcPoint *out)
{
    EcPoint r;
    r.infinity = 1;
    int started = 0;
    for (int i = 0; i < NK_EC_Len; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            if (started) {
                EcPoint doubled;
                point_double(params, &r, &doubled);
                r = doubled;
            }
            if ((k[i] >> bit) & 1u) {
                EcPoint sum;
                if (!started) {
                    sum = *p;
                    started = 1;
                } else {
                    point_add(params, &r, p, &sum);
                }
                r = sum;
            }
        }
    }
    *out = r;
}

int nk_ec_point_mul(const NkEcParams *params,
                    const uint8_t k[NK_EC_BYTES],
                    const uint8_t px[NK_EC_BYTES], const uint8_t py[NK_EC_BYTES],
                    uint8_t outx[NK_EC_BYTES], uint8_t outy[NK_EC_BYTES])
{
    if (params == NULL || k == NULL || px == NULL || py == NULL) return -1;
    if (is_zero(k)) return -1;
    EcPoint p;
    p.infinity = 0;
    memcpy(p.x, px, NK_EC_Len);
    memcpy(p.y, py, NK_EC_Len);
    if (!point_on_curve(params, &p)) return -1;
    /* scalar must be < n for ECDSA use; point multiplication for KIRK
     * command 0x0D also carries a 160-bit scalar. */
    if (ge(k, params->n)) return -1;
    EcPoint out;
    point_mul_scalar(params, k, &p, &out);
    if (out.infinity) return -1;
    memcpy(outx, out.x, NK_EC_Len);
    memcpy(outy, out.y, NK_EC_Len);
    return 0;
}

/* out = x mod n by shift-and-subtract: exactly NK_EC_Len * 8 steps whatever
 * the curve order is. Repeated subtraction (the earlier form) ran for up to
 * x / n iterations, which a key file with a tiny order turns into a hang.
 * The running remainder r stays below n, so 2r + bit < 2n; a carry out of
 * the shift means 2r + bit >= 2^160 > n, and one subtraction modulo 2^160
 * lands on the true value in both cases. */
int nk_ec_mod_n(const uint8_t x[NK_EC_BYTES], const uint8_t n[NK_EC_BYTES],
                uint8_t out[NK_EC_BYTES])
{
    uint8_t r[NK_EC_Len];
    if (x == NULL || n == NULL || out == NULL || is_zero(n)) return -1;
    memset(r, 0, sizeof(r));
    for (int i = 0; i < NK_EC_Len; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            uint8_t carry = (uint8_t)(r[0] >> 7);
            for (int j = 0; j < NK_EC_Len - 1; j++) {
                r[j] = (uint8_t)((r[j] << 1) | (r[j + 1] >> 7));
            }
            r[NK_EC_Len - 1] = (uint8_t)((r[NK_EC_Len - 1] << 1) | ((x[i] >> bit) & 1u));
            if (carry || ge(r, n) || memcmp(r, n, NK_EC_Len) == 0) {
                (void)sub_borrow(r, n, r);
            }
        }
    }
    memcpy(out, r, NK_EC_Len);
    return 0;
}

int nk_ec_verify(const NkEcParams *params,
                 const uint8_t qx[NK_EC_BYTES], const uint8_t qy[NK_EC_BYTES],
                 const uint8_t hash[NK_EC_BYTES],
                 const uint8_t r[NK_EC_BYTES], const uint8_t s[NK_EC_BYTES])
{
    if (params == NULL || qx == NULL || qy == NULL ||
        hash == NULL || r == NULL || s == NULL) return -1;
    /* r, s in [1, n-1] */
    if (is_zero(r) || is_zero(s) || ge(r, params->n) || ge(s, params->n)) return -1;

    EcPoint q;
    q.infinity = 0;
    memcpy(q.x, qx, NK_EC_Len);
    memcpy(q.y, qy, NK_EC_Len);
    if (!point_on_curve(params, &q)) return -1;

    uint8_t s_inv[NK_EC_Len], u1[NK_EC_Len], u2[NK_EC_Len];
    mod_inv(s, params->n, s_inv);
    mod_mul(hash, s_inv, params->n, u1);
    mod_mul(r, s_inv, params->n, u2);

    EcPoint g;
    g.infinity = 0;
    memcpy(g.x, params->gx, NK_EC_Len);
    memcpy(g.y, params->gy, NK_EC_Len);
    if (!point_on_curve(params, &g)) return -1;

    EcPoint t1, t2, x;
    point_mul_scalar(params, u1, &g, &t1);
    point_mul_scalar(params, u2, &q, &t2);
    point_add(params, &t1, &t2, &x);
    if (x.infinity) return -1;

    uint8_t xr[NK_EC_Len];
    if (nk_ec_mod_n(x.x, params->n, xr) != 0) return -1;
    return memcmp(xr, r, NK_EC_Len) == 0 ? 0 : -1;
}
