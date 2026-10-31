/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * Disc verification: a whole-image hash compared with the user's Redump list.
 *
 * A disc image is hashed whole, with SHA-1 and CRC-32 (the hashes Redump
 * lists), on the user's machine. A compressed image (CSO, ZSO, CHD) is hashed
 * after its one-time expansion (nk_disc_expand.h), so it hashes the same as the
 * .iso it holds. The hashes are compared with a Redump datafile the user
 * imports themselves (nk_disc_dat_import); nothing is uploaded or downloaded.
 *
 * Tiers (the same rules as tools/nk_core/disc_verify.py):
 *   VERIFIED_RETAIL  the SHA-1 matches a disc in the list, and the CRC-32 and
 *                    size agree;
 *   MODIFIED         no match, but the disc ID is a known retail disc ID: one
 *                    the list names in a <serial>, or one a verified image in
 *                    the same library carries (fan translations, patched or
 *                    trimmed images);
 *   UNKNOWN          neither;
 *   NOT_CHECKED      no list imported, or the image was never hashed.
 *
 * The datafile (Logiqx XML, as Redump publishes it) is read by a small bounded
 * scanner with no entity or DTD processing: each <game> element, the size,
 * crc and sha1 attributes of its <rom> elements, and any <serial> text. A rom
 * with a malformed hash or size is skipped and counted.
 */

#ifndef NK_DISC_VERIFY_H
#define NK_DISC_VERIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nk_disc_expand.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The imported list, under the user's data folder ("verification/redump.dat",
   the path tools/nk_core/disc_verify.py uses too). */
#define NK_DISC_DAT_DIR "verification"
#define NK_DISC_DAT_FILE "redump.dat"
/* The largest datafile read: Redump's biggest lists are a few MiB. */
#define NK_DISC_DAT_MAX_BYTES (64u * 1024u * 1024u)
#define NK_DISC_DAT_MAX_ROMS 200000u

#define NK_DISC_SHA1_HEX_CHARS 40u
#define NK_DISC_CRC32_HEX_CHARS 8u

/* Named boundary codes. */
#define NK_DISC_DAT_READ_FAILED    "DISC_DAT_READ_FAILED"
#define NK_DISC_DAT_WRITE_FAILED   "DISC_DAT_WRITE_FAILED"
#define NK_DISC_DAT_NOT_DATAFILE   "DISC_DAT_NOT_DATAFILE"
#define NK_DISC_DAT_EMPTY          "DISC_DAT_EMPTY"
#define NK_DISC_DAT_TOO_LARGE      "DISC_DAT_TOO_LARGE"

typedef enum {
    NK_DISC_TIER_NOT_CHECKED = 0,
    NK_DISC_TIER_VERIFIED_RETAIL,
    NK_DISC_TIER_MODIFIED,
    NK_DISC_TIER_UNKNOWN
} NkDiscTier;

/* "NOT_CHECKED", "VERIFIED_RETAIL", "MODIFIED", "UNKNOWN": the names
   library.json stores. Inline, so nk_library.c needs no other object. */
static inline const char *nk_disc_tier_name(NkDiscTier tier) {
    switch (tier) {
    case NK_DISC_TIER_VERIFIED_RETAIL: return "VERIFIED_RETAIL";
    case NK_DISC_TIER_MODIFIED: return "MODIFIED";
    case NK_DISC_TIER_UNKNOWN: return "UNKNOWN";
    case NK_DISC_TIER_NOT_CHECKED:
    default: return "NOT_CHECKED";
    }
}

/* The tier a stored name means; false (and NOT_CHECKED) for any other text. */
static inline bool nk_disc_tier_from_name(const char *name, NkDiscTier *out_tier) {
    static const NkDiscTier tiers[] = {
        NK_DISC_TIER_NOT_CHECKED, NK_DISC_TIER_VERIFIED_RETAIL, NK_DISC_TIER_MODIFIED,
        NK_DISC_TIER_UNKNOWN,
    };
    if (out_tier) *out_tier = NK_DISC_TIER_NOT_CHECKED;
    for (size_t i = 0; name && i < sizeof(tiers) / sizeof(tiers[0]); i++) {
        if (strcmp(name, nk_disc_tier_name(tiers[i])) == 0) {
            if (out_tier) *out_tier = tiers[i];
            return true;
        }
    }
    return false;
}

/* SHA-1 (FIPS 180-4). */
typedef struct {
    uint32_t state[5];
    uint64_t byte_count;
    uint8_t block[64];
    size_t block_len;
} NkSha1;
void nk_sha1_init(NkSha1 *ctx);
void nk_sha1_update(NkSha1 *ctx, const uint8_t *data, size_t len);
void nk_sha1_finish(NkSha1 *ctx, uint8_t digest[20]);

/* CRC-32 as Redump and zlib compute it (reflected 0xEDB88320, ~0 in and out).
   Start from 0 and pass each result back in. */
uint32_t nk_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/* The whole-image hashes of one disc image: lowercase hex, and its size. */
typedef struct {
    char sha1[NK_DISC_SHA1_HEX_CHARS + 1];
    char crc32[NK_DISC_CRC32_HEX_CHARS + 1];
    uint64_t size_bytes;
} NkDiscDigest;

/* Hash a whole plain image, reading it once. callbacks (optional) report
   progress and can cancel; a cancel or read failure fills status (CANCELLED,
   DISC_IMAGE_READ_FAILED) and leaves out cleared. Run it on a worker: a disc
   is up to 1.8 GB. */
bool nk_disc_hash_file(const char *path, const NkDiscExpandCallbacks *callbacks,
                       NkDiscDigest *out, NkDiscExpandStatus *status);

typedef struct {
    uint8_t sha1[20];
    uint32_t crc32;
    uint64_t size_bytes;
} NkDiscDatRom;

/* The parts of one datafile verification uses. roms is sorted by SHA-1 and
   serials (nine-character disc IDs, "TEST71002") are sorted, both for binary
   search. Free with nk_disc_dat_free. */
typedef struct {
    NkDiscDatRom *roms;
    size_t rom_count;
    char (*serials)[10];
    size_t serial_count;
    size_t games;
    size_t rejected_roms;
    char name[128];
    char version[64];
} NkDiscDat;

void nk_disc_dat_free(NkDiscDat *dat);
/* Read a datafile from memory. Refuses a text with no <datafile> element
   (DISC_DAT_NOT_DATAFILE), no rom with a SHA-1 (DISC_DAT_EMPTY) or more than
   NK_DISC_DAT_MAX_ROMS roms (DISC_DAT_TOO_LARGE). */
bool nk_disc_dat_parse(const char *text, size_t length, NkDiscDat *out,
                       NkDiscExpandStatus *status);
/* The same, read from a file of at most NK_DISC_DAT_MAX_BYTES. */
bool nk_disc_dat_load(const char *path, NkDiscDat *out, NkDiscExpandStatus *status);
/* <data_root>/verification/redump.dat. */
bool nk_disc_dat_path(const char *data_root, char *out, size_t out_size);
/* Check a datafile the user chose and keep a copy in their data folder,
   replacing an earlier import whole (written beside it, then renamed). out
   (optional) receives the parsed list. */
bool nk_disc_dat_import(const char *source, const char *data_root, NkDiscDat *out,
                        NkDiscExpandStatus *status);

/* Whether the list names this disc ID in a <serial>. */
bool nk_disc_dat_has_serial(const NkDiscDat *dat, const char *disc_id);

/* The tier of one image. dat or digest NULL (or an empty digest) is
   NOT_CHECKED. disc_id_verified_elsewhere says a verified image in the same
   library carries this disc ID. */
NkDiscTier nk_disc_classify(const NkDiscDat *dat, const char *disc_id,
                            const NkDiscDigest *digest, bool disc_id_verified_elsewhere);

/* Disc IDs in a <serial> value ("TEST-71002, TEST-71002GA") as PARAM.SFO
   spells them: up to max_ids nine-character IDs, without repeats. Returns how
   many were written. */
size_t nk_disc_normalize_serials(const char *text, size_t length, char (*out_ids)[10],
                                 size_t max_ids);

#ifdef __cplusplus
}
#endif

#endif /* NK_DISC_VERIFY_H */
