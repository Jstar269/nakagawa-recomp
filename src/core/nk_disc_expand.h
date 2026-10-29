/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * Compressed disc images (CSO, ZSO, CHD), expanded once at import.
 *
 * A compressed disc image is turned into a plain .iso in the private per-user
 * data folder the first time it is added, and everything downstream (disc
 * inspection, decryption, code generation, the runtime's PSP_ISO reads) reads
 * that .iso exactly as it reads a user's own .iso. Nothing is decompressed
 * into guest or runtime memory: this is an import step only.
 *
 *   <data>/discs/<DISC_ID>/<DISC_ID>.<cso|zso|chd>.iso   the expanded image
 *   <data>/discs/<DISC_ID>/<DISC_ID>.<ext>.iso.source    its source fingerprint
 *   <data>/discs/expanded/<fingerprint>.ref              fingerprint -> disc ID
 *
 * The fingerprint keys on the source's size, write time and a hash of its
 * first 64 KiB (the header and the start of the block index), so adding the
 * same unchanged file again reuses the expansion instead of writing it again.
 *
 * CSO (CISO v1 and v2) and ZSO (ZISO) are read here: the block index, raw
 * DEFLATE blocks through nk_psp_inflate_raw and LZ4 blocks through a small
 * block decoder in nk_disc_expand.c. CHD is read through a backend the program
 * registers (nk_disc_chd.c, over the vendored libchdr); a program built without
 * it refuses CHD by name. Every header field is checked before it is used and
 * every refusal carries a named boundary code (NK_DISC_* below).
 */

#ifndef NK_DISC_EXPAND_H
#define NK_DISC_EXPAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "nk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NK_DISC_SECTOR_BYTES 2048u
/* No UMD holds more than 1.8 GB; anything past this bound is a damaged or
   hostile header, refused before any allocation or loop uses it. */
#define NK_DISC_MAX_EXPANDED_BYTES (4ull * 1024ull * 1024ull * 1024ull)
#define NK_DISC_FINGERPRINT_CHARS 32u

/* Named boundary codes. */
#define NK_DISC_TRUNCATED              "DISC_IMAGE_TRUNCATED"
#define NK_DISC_HEADER_INVALID         "DISC_IMAGE_HEADER_INVALID"
#define NK_DISC_VERSION_UNSUPPORTED    "DISC_IMAGE_VERSION_UNSUPPORTED"
#define NK_DISC_SIZE_INVALID           "DISC_IMAGE_SIZE_INVALID"
#define NK_DISC_INDEX_INVALID          "DISC_IMAGE_INDEX_INVALID"
#define NK_DISC_BLOCK_CORRUPT          "DISC_IMAGE_BLOCK_CORRUPT"
#define NK_DISC_READ_FAILED            "DISC_IMAGE_READ_FAILED"
#define NK_DISC_WRITE_FAILED           "DISC_IMAGE_WRITE_FAILED"
#define NK_DISC_NOT_COMPRESSED         "DISC_IMAGE_NOT_COMPRESSED"
#define NK_DISC_ID_MISSING             "DISC_IMAGE_ID_MISSING"
#define NK_DISC_NOT_PSP                "DISC_NOT_PSP"
#define NK_DISC_CHD_UNAVAILABLE        "DISC_IMAGE_CHD_UNAVAILABLE"
#define NK_DISC_CHD_INVALID            "DISC_IMAGE_CHD_INVALID"
#define NK_DISC_CHD_CD_UNSUPPORTED     "DISC_IMAGE_CHD_CD_UNSUPPORTED"
#define NK_DISC_CHD_UNSUPPORTED        "DISC_IMAGE_CHD_UNSUPPORTED"
#define NK_DISC_CANCELLED              "CANCELLED"
#define NK_DISC_INTERNAL               "INTERNAL_ERROR"

typedef enum {
    NK_DISC_FORMAT_NONE = 0, /* not a compressed disc image (a plain .iso or anything else) */
    NK_DISC_FORMAT_CSO,
    NK_DISC_FORMAT_ZSO,
    NK_DISC_FORMAT_CHD
} NkDiscFormat;

typedef struct {
    void (*on_progress)(uint64_t done_bytes, uint64_t total_bytes, void *user);
    bool (*is_cancelled)(void *user);
    void *user;
} NkDiscExpandCallbacks;

typedef struct {
    char code[48];     /* a NK_DISC_* name; empty on success */
    char message[384]; /* one plain sentence for the user */
} NkDiscExpandStatus;

/* The CHD reader the program links. open checks that the image is a DVD-layout
   CHD of 2048-byte sectors and reports its size; read_hunk reads hunk `index`
   (hunk_bytes bytes) into dst. Every failure fills status. */
typedef struct {
    bool (*open)(const char *path, void **handle, uint64_t *logical_bytes,
                 uint32_t *hunk_bytes, NkDiscExpandStatus *status);
    bool (*read_hunk)(void *handle, uint32_t index, uint8_t *dst, NkDiscExpandStatus *status);
    void (*close)(void *handle);
} NkDiscChdBackend;

/* Install the CHD backend (NULL removes it). Call once at start-up, before
   any expansion runs. */
void nk_disc_set_chd_backend(const NkDiscChdBackend *backend);

/* The format named by a file's leading bytes ("CISO", "ZISO", "MComprHD"). */
NkDiscFormat nk_disc_format_from_magic(const uint8_t *bytes, size_t length);
/* The same, read from a file. NONE when the file cannot be read. */
NkDiscFormat nk_disc_image_format(const char *path);
/* "cso", "zso", "chd", or "" for NONE. */
const char *nk_disc_format_extension(NkDiscFormat format);

/* Check the header of a compressed image and report the size of the plain
   image it expands to, without expanding it. */
bool nk_disc_expanded_size(const char *source, NkDiscFormat *out_format,
                           uint64_t *out_expanded_bytes, NkDiscExpandStatus *status);

/* Expand source into dest_iso: written to dest_iso + ".part", its size checked,
   then renamed into place. A failure or a cancel removes the partial file and
   leaves any existing dest_iso untouched. */
bool nk_disc_expand_file(const char *source, const char *dest_iso,
                         const NkDiscExpandCallbacks *callbacks,
                         uint64_t *out_expanded_bytes, NkDiscExpandStatus *status);

typedef struct {
    NkDiscFormat format;
    uint64_t source_bytes;
    uint64_t expanded_bytes;
    char fingerprint[NK_DISC_FINGERPRINT_CHARS + 1];
    char disc_id[16];
    char iso_path[NK_MAX_PATH];
    /* A matching expansion was already in place and nothing was written. */
    bool reused;
} NkDiscImport;

/* Identify a compressed image, check its header and fingerprint it, and look
   for an expansion of the same unchanged file under data_root. On success
   out->reused says whether out->iso_path is ready. */
bool nk_disc_import_probe(const char *source, const char *data_root, NkDiscImport *out,
                          NkDiscExpandStatus *status);

/* Probe, then expand when no matching expansion exists: the image is expanded
   into <data_root>/discs/expanded/, checked to be a PSP disc with a disc ID,
   and moved into its per-title folder. */
bool nk_disc_import(const char *source, const char *data_root,
                    const NkDiscExpandCallbacks *callbacks, NkDiscImport *out,
                    NkDiscExpandStatus *status);

#ifdef __cplusplus
}
#endif

#endif /* NK_DISC_EXPAND_H */
