/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Local-only KeyStore for the PSP decryption boundary (issue #295).
 *
 * The KeyStore is a JSON file the user supplies at run time; it lives
 * outside the repository (user-chosen path, an explicit --key-file
 * argument, or the player's app-data folder).  Validation is fail-closed:
 * shape, names and value lengths are checked, and every consumer learns
 * the exact entry name that is missing for a given container.
 */

#ifndef NK_PSP_KEYSTORE_H
#define NK_PSP_KEYSTORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_KEYSTORE_FORMAT "nakagawa-psp-keystore-1"
#define NK_KEYSTORE_MAX_ENTRIES 512
#define NK_KEYSTORE_MAX_BYTES (64u * 1024u)

typedef struct NkKeystore NkKeystore;

/* Structured view of a "prx.tag.0xXXXXXXXX" recipe entry. */
typedef struct NkPrxTagEntry {
    int have_code;
    int code;                     /* KIRK keyvault slot, 0..127 */
    const uint8_t *key;           /* 16 bytes, or NULL */
    const uint8_t *key144;        /* 144 bytes (pre-expanded XOR buffer), or NULL */
    const uint8_t *seed;          /* 16 bytes, or NULL */
    const uint8_t *xorpad;        /* 16 bytes, or NULL */
} NkPrxTagEntry;

NkKeystore *nk_keystore_create(void);
void nk_keystore_free(NkKeystore *ks);

/* Parse and validate JSON text.  Returns 0 or NK_PSP_ERR_KEYFILE with err. */
int nk_keystore_load_json(NkKeystore *ks, const char *text, size_t len,
                          char *err, size_t err_len);
/* Read, parse and validate a key file.  Distinguishes absent vs invalid. */
int nk_keystore_load_file(NkKeystore *ks, const char *path,
                          char *err, size_t err_len);

/* Flat (hex-string) entry lookup by canonical name.  0 on success. */
int nk_keystore_get(const NkKeystore *ks, const char *name,
                    const uint8_t **out, size_t *out_len);
/* Structured tag-entry lookup.  0 on success, -1 when absent. */
int nk_keystore_get_prx_tag(const NkKeystore *ks, uint32_t tag,
                            NkPrxTagEntry *out);
/* Number of loaded entries (0 for an absent/empty file). */
size_t nk_keystore_count(const NkKeystore *ks);

#ifdef __cplusplus
}
#endif

#endif /* NK_PSP_KEYSTORE_H */
