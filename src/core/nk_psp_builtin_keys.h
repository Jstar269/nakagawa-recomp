/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2012- PPSSPP Project
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Built-in PSP game-decryption key tables (owner decision 2026-10-27,
 * issue #295): the ~PSP tag recipes and the KIRK values the decryption
 * boundary needs, so a user's own game disc decrypts without a key file.
 * Firmware containers are never decrypted: their tags are listed only so the
 * boundary can refuse them by name before any key lookup.
 */

#ifndef NK_PSP_BUILTIN_KEYS_H
#define NK_PSP_BUILTIN_KEYS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One ~PSP tag recipe, the fields nk_psp_keystore.h names. */
typedef struct NkPspBuiltinTag {
    uint32_t tag;            /* the u32 at file offset 0xD0 */
    uint8_t code;            /* KIRK keyvault slot */
    const uint8_t *key;      /* 16 bytes (types 2, 5, 6), or NULL */
    const uint32_t *key144;  /* 36 words (types 0, 1), or NULL */
    const uint8_t *xorpad;   /* 16-byte type-5 pad, or NULL */
} NkPspBuiltinTag;

/* One flat KeyStore value ("kirk.cmd1.key", "kirk.keyvault.66", ...). */
typedef struct NkPspBuiltinValue {
    const char *name;
    const uint8_t *bytes;
    size_t len;
} NkPspBuiltinValue;

/* A tag that only firmware containers carry, with what it is. */
typedef struct NkPspFirmwareTag {
    uint32_t tag;
    const char *what;
} NkPspFirmwareTag;

extern const NkPspBuiltinTag nk_psp_builtin_tags[];
extern const size_t nk_psp_builtin_tag_count;
extern const NkPspBuiltinValue nk_psp_builtin_values[];
extern const size_t nk_psp_builtin_value_count;
extern const NkPspFirmwareTag nk_psp_firmware_tags[];
extern const size_t nk_psp_firmware_tag_count;

#ifdef __cplusplus
}
#endif

#endif /* NK_PSP_BUILTIN_KEYS_H */
