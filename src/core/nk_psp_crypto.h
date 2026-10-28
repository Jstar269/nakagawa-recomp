/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Common types, error contract, and KeyStore interface for the built-in
 * PSP container decryption boundary (issue #295).
 *
 * This engine implements container formats and cryptographic algorithms
 * only.  Every key, seed, XOR pad and curve value arrives at run time
 * through a KeyStore: the built-in game tables (nk_psp_builtin_keys.c,
 * owner decision 2026-10-27), or a key file given to nk_decrypt for
 * synthetic tests.  A missing entry fails closed with its exact name.
 * Firmware containers are refused by name before any key lookup.
 */

#ifndef NK_PSP_CRYPTO_H
#define NK_PSP_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Small-width aliases used by the ported engine sources. */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;

/* Error contract shared by the CLI, the Python tooling, and the player. */
enum {
    NK_PSP_OK = 0,
    NK_PSP_ERR_INPUT = -1,       /* unreadable or truncated input */
    NK_PSP_ERR_FORMAT = -2,      /* malformed container (fail closed) */
    NK_PSP_ERR_MISSING_KEY = -3, /* ctx->missing_entry names the entry */
    NK_PSP_ERR_KEYFILE = -4,     /* key file missing or invalid */
    NK_PSP_ERR_INTEGRITY = -5,   /* CMAC/SHA-1/signature check failed */
    NK_PSP_ERR_UNSUPPORTED = -6, /* container form or mode not enabled */
    NK_PSP_ERR_DECOMPRESS = -7,  /* payload decompression failed */
    NK_PSP_ERR_INTERNAL = -8,
    NK_PSP_ERR_FIRMWARE = -9,    /* firmware container: never decrypted */
    NK_PSP_ERR_CONTEXT = -10,    /* needs the user's console (fuse ID, licence) */
    NK_PSP_ERR_PSONE = -11,      /* PSOne Classic package: out of scope */
    NK_PSP_ERR_CANCELLED = -12   /* the caller cancelled; nothing was kept */
};

/* Operation context: the KeyStore plus fail-closed diagnostics. */
typedef struct NkPspCtx {
    const struct NkKeystore *ks; /* may be NULL only for probe-style calls */
    char missing_entry[80];      /* entry name when code == MISSING_KEY */
    char message[256];           /* human-readable, actionable, no tool names */
} NkPspCtx;

void nk_psp_ctx_init(NkPspCtx *ctx, const struct NkKeystore *ks);

/*
 * Look up a required KeyStore entry.  On success returns NK_PSP_OK and
 * fills out/out_len.  On absence records the entry name in the context
 * and returns NK_PSP_ERR_MISSING_KEY with a message naming the entry.
 */
int nk_psp_need(NkPspCtx *ctx, const char *name,
                const uint8_t **out, size_t *out_len);

/* Record a fail-closed message (and optional missing entry) in the context. */
int nk_psp_fail(NkPspCtx *ctx, int code, const char *entry, const char *fmt, ...);

/* The named result for a boundary return code, shared by the player and the
 * tools: VALIDATED_DECRYPTED for NK_PSP_OK, KEY_REQUIRED, MALFORMED_INPUT,
 * INTEGRITY_FAILURE, KEY_FILE_INVALID, UNSUPPORTED_SCHEME,
 * DECOMPRESS_FAILURE, IO_FAILURE, FIRMWARE_OUT_OF_SCOPE, CONTEXT_REQUIRED,
 * PSONE_CLASSIC_UNSUPPORTED, CANCELLED or INTERNAL_ERROR. */
const char *nk_psp_status_name(int code);

#ifdef __cplusplus
}
#endif

#endif /* NK_PSP_CRYPTO_H */
