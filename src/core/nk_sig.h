/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_SIG_H
#define NK_SIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_ED25519_SIGNATURE_BYTES 64u
#define NK_ED25519_PUBLIC_KEY_BYTES 32u

/* Checks an Ed25519 (RFC 8032, pure, no context) signature over msg[0..len).
 * Returns true only when the signature is valid for that public key; a NULL
 * signature or key, or a NULL msg with a nonzero len, is false. msg may be NULL
 * when len is 0. Verification only: this module holds no key, signs nothing and
 * makes no connection. The primitive is Monocypher's crypto_ed25519_check
 * (third_party/monocypher/), which rejects a non-canonical S (S >= L). */
bool nk_ed25519_verify(const uint8_t sig[NK_ED25519_SIGNATURE_BYTES],
                       const uint8_t pub[NK_ED25519_PUBLIC_KEY_BYTES],
                       const uint8_t *msg, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NK_SIG_H */
