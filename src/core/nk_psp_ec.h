/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Project-authored elliptic-curve operations for the decryption boundary
 * (issue #295): ECDSA verification and point multiplication over the
 * 160-bit prime curves the KIRK container commands use.  Written from the
 * public SEC 1 / FIPS 186 descriptions of ECDSA; upstream libkirk's
 * ec.c/bn.c are GPL-2.0-only and were deliberately NOT ported (they are
 * not clearly GPL-3.0-compatible).  Curve parameters and public points are
 * KeyStore entries, never constants in this file.  No signing or key
 * generation exists here: verification only.
 */

#ifndef NK_PSP_EC_H
#define NK_PSP_EC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_EC_BYTES 20 /* 160-bit field/coordinate width */

typedef struct NkEcParams {
    uint8_t p[NK_EC_BYTES];  /* field prime */
    uint8_t a[NK_EC_BYTES];  /* curve coefficient a */
    uint8_t b[NK_EC_BYTES];  /* curve coefficient b */
    uint8_t n[NK_EC_BYTES];  /* group order */
    uint8_t gx[NK_EC_BYTES]; /* generator x */
    uint8_t gy[NK_EC_BYTES]; /* generator y */
} NkEcParams;

/* ECDSA verify (SEC 1 §4.1.4).  0 = signature valid, -1 = invalid/error. */
int nk_ec_verify(const NkEcParams *params,
                 const uint8_t qx[NK_EC_BYTES], const uint8_t qy[NK_EC_BYTES],
                 const uint8_t hash[NK_EC_BYTES],
                 const uint8_t r[NK_EC_BYTES], const uint8_t s[NK_EC_BYTES]);

/* Scalar multiplication k*P (affine).  0 = success; -1 = invalid input
 * (point off the curve, scalar out of range, or result at infinity). */
int nk_ec_point_mul(const NkEcParams *params,
                    const uint8_t k[NK_EC_BYTES],
                    const uint8_t px[NK_EC_BYTES], const uint8_t py[NK_EC_BYTES],
                    uint8_t outx[NK_EC_BYTES], uint8_t outy[NK_EC_BYTES]);

/* out = x mod n in a fixed NK_EC_BYTES * 8 steps, whatever n is (a key file
 * supplies n).  0 = success; -1 = null argument or n == 0. */
int nk_ec_mod_n(const uint8_t x[NK_EC_BYTES], const uint8_t n[NK_EC_BYTES],
                uint8_t out[NK_EC_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* NK_PSP_EC_H */
