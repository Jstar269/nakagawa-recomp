/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Container-level probe and decrypt orchestration for the built-in PSP
 * decryption boundary (issue #295).  Project-authored from the format
 * rules the pipeline meets (see docs/SETUP.md); no key material here.
 *
 * Forms handled:
 *   - "~PSP" executables (EBOOT.BIN/PRX), encrypted or plain;
 *   - "~SCE" outer wrappers (u32 header size at +4, inner module follows);
 *   - PBP packages (DATA.PSP extracted at offset 0x20);
 *   - gzip-compressed payloads after decryption (and KL4E/KL3E streams);
 *   - plain ELF32/MIPS images (pass-through).
 *
 * Every key, seed or XOR pad arrives through the run-time KeyStore; a
 * missing entry fails closed naming the exact entry name required.
 */

#ifndef NK_PSP_CONTAINER_H
#define NK_PSP_CONTAINER_H

#include "nk_psp_crypto.h"
#include "nk_psp_keystore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NkContainerKind {
    NK_CTR_UNKNOWN = 0,
    NK_CTR_PLAIN_ELF,   /* 7f 45 4c 46 */
    NK_CTR_PSP,         /* "~PSP" executable/PRX */
    NK_CTR_SCE_WRAPPER, /* "~SCE" outer wrapper */
    NK_CTR_PBP,         /* "\0PBP" package */
    NK_CTR_GZIP         /* 1f 8b gzip stream */
} NkContainerKind;

typedef struct NkContainerInfo {
    NkContainerKind kind; /* innermost recognizable form */
    unsigned tag;         /* ~PSP tag at 0xD0 (0 when not a ~PSP) */
    int compressed;       /* ~PSP comp_attribute bit 0 */
    int wrappers_skipped; /* how many PBP/~SCE layers were unwrapped */
} NkContainerInfo;

/*
 * Identify a container WITHOUT needing any keys: unwraps PBP/~SCE
 * layers and validates structure.  Returns NK_PSP_OK, or a fail-closed
 * code (NK_PSP_ERR_FORMAT for a structurally malformed container) with
 * ctx->message filled when ctx is non-NULL.
 */
int nk_container_probe(NkPspCtx *ctx, const uint8_t *data, size_t size,
                       NkContainerInfo *out);

/*
 * Append the canonical key-entry names this container will need into
 * entries (one NUL-terminated name per slot, at most max_entries).
 * Always names the ~PSP tag entry and "kirk.cmd1.key"; when the KeyStore
 * already holds the tag recipe the "code" also resolves the matching
 * "kirk.keyvault.<code>" entry.  Returns the number of names written.
 */
int nk_container_key_entries(const NkContainerInfo *info,
                             const NkKeystore *ks,
                             char entries[][80], int max_entries);

/*
 * Full production boundary: unwrap PBP/~SCE, validate and decrypt the
 * ~PSP container, decompress (gzip or KL4E/KL3E) and verify the result
 * is an ELF32 image.  On success returns NK_PSP_OK and hands the caller
 * a freshly malloc'd buffer (*out, *out_size; caller frees).  On
 * failure returns a negative NK_PSP_ERR_* code with ctx filled in
 * (missing entry name and message) and *out untouched.
 */
int nk_container_decrypt(NkPspCtx *ctx, const uint8_t *data, size_t size,
                         uint8_t **out, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* NK_PSP_CONTAINER_H */
