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

/* ---- minisign ----------------------------------------------------------------------
 *
 * The signature and public-key formats of minisign (github.com/jedisct1/minisign,
 * checked against release 0.12), so a file signed offline with the stock minisign
 * tool can be checked here. A public key is the line
 *
 *     base64("Ed" || key ID (8 bytes) || Ed25519 public key (32 bytes))
 *
 * optionally after an "untrusted comment: " line, as `minisign -G` writes it. A
 * signature file has four lines:
 *
 *     untrusted comment: <anything; not signed, never read>
 *     base64(algorithm (2 bytes) || key ID (8 bytes) || signature (64 bytes))
 *     trusted comment: <text>
 *     base64(global signature (64 bytes))
 *
 * The algorithm is "ED" (minisign's default: the signature covers the BLAKE2b-512
 * digest of the file) or "Ed" (the legacy form, `minisign -l`: it covers the file
 * itself). The global signature covers the 64 signature bytes followed by the
 * trusted comment's bytes, so the comment cannot be changed either. Lines end in
 * LF or CRLF (minisign on Windows writes CRLF). Parsing is strict: base64 must be
 * canonical and exactly the expected length, and a NUL or a stray CR fails. */

#define NK_MINISIGN_KEY_ID_BYTES 8u

typedef struct {
    uint8_t key_id[NK_MINISIGN_KEY_ID_BYTES];
    uint8_t public_key[NK_ED25519_PUBLIC_KEY_BYTES];
} NkMinisignKey;

typedef enum {
    NK_MINISIGN_VALID = 0,
    NK_MINISIGN_MALFORMED,       /* not a minisign signature file, or an unknown algorithm */
    NK_MINISIGN_UNKNOWN_KEY,     /* made with another key: its key ID is not this key's */
    NK_MINISIGN_BAD_SIGNATURE,   /* the signature over the file does not verify */
    NK_MINISIGN_BAD_COMMENT      /* the global signature over the trusted comment does not */
} NkMinisignResult;

/* Reads a minisign public key: a whole `minisign -G` .pub file, or only its base64
   line. Trailing line ends are allowed; anything else after the key line fails. */
bool nk_minisign_parse_key(const char *text, size_t length, NkMinisignKey *out);

/* Checks a minisign signature file (signature[0..signature_length)) over
   msg[0..msg_length) with key. Only NK_MINISIGN_VALID means the file and the
   trusted comment are exactly what the key's holder signed. On VALID, when the
   pointers are given, *trusted_comment points into signature at the comment text
   (without its prefix or line end) and *trusted_comment_length is its length;
   otherwise they are set to NULL and 0. */
NkMinisignResult nk_minisign_verify(const NkMinisignKey *key, const char *signature,
                                    size_t signature_length, const uint8_t *msg,
                                    size_t msg_length, const char **trusted_comment,
                                    size_t *trusted_comment_length);

#ifdef __cplusplus
}
#endif

#endif /* NK_SIG_H */
