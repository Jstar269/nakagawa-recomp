/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2012- PPSSPP Project
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * PSN releases of UMD games (issue #295, phase 2): an EBOOT.PBP whose
 * DATA.PSAR is an NPUMDIMG image holds the game's disc image, block by
 * block, encrypted with the DRM BB cipher and compressed with LZRC.  This
 * reader turns it back into the ISO it was made from, the way PPSSPP reads
 * it (Core/FileSystems/BlockDevices.cpp NPDRMDemoBlockDevice at
 * b4386554795949491a26e51b7d74082bb9b2470f, GPL-2.0-or-later; modified
 * for this project, see nk_psp_npumdimg.c).  The ISO then goes through the
 * phase-1 disc route unchanged.
 *
 * Refused by name before any key lookup: PSOne Classics
 * (PSONE_CLASSIC_UNSUPPORTED), updater packages and PSAR archives
 * (FIRMWARE_OUT_OF_SCOPE), and any other package form (UNSUPPORTED_SCHEME).
 */

#ifndef NK_PSP_NPUMDIMG_H
#define NK_PSP_NPUMDIMG_H

#include "nk_psp_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NK_NPUMDIMG_SECTOR 2048u
/* PPSSPP's bound on the block size, in sectors (8 MiB blocks). */
#define NK_NPUMDIMG_MAX_BLOCK_LBAS 4096u
/* The largest image accepted: 4 GiB, more than a dual-layer UMD holds. */
#define NK_NPUMDIMG_MAX_LBAS 0x200000u

/* Random access to a package or image: read exactly len bytes at offset,
 * returning 0, or nonzero when the bytes are not all there. */
typedef int (*NkPsarReadAt)(void *user, uint64_t offset, void *buf, size_t len);

typedef struct NkPsarSource {
    NkPsarReadAt read_at;
    void *user;
    uint64_t size; /* total bytes */
} NkPsarSource;

/* What a PBP package's DATA.PSAR (or DATA.PSP) says it carries. */
typedef enum NkPbpPayload {
    NK_PBP_PAYLOAD_UNKNOWN = 0,
    NK_PBP_PAYLOAD_NPUMDIMG, /* a PSN release of a UMD game */
    NK_PBP_PAYLOAD_PSONE,    /* a PSOne Classic (PSISOIMG, PSTITLEIMG) */
    NK_PBP_PAYLOAD_FIRMWARE, /* a system software update */
    NK_PBP_PAYLOAD_OTHER     /* any other package (homebrew, minis, ...) */
} NkPbpPayload;

typedef struct NkPbpInfo {
    NkPbpPayload payload;
    uint32_t sfo_offset;
    uint32_t sfo_size;
    uint32_t psp_offset;
    uint32_t psar_offset;
    char disc_id[16];      /* PARAM.SFO DISC_ID, "" when absent */
    char disc_version[16]; /* PARAM.SFO DISC_VERSION */
    char category[8];      /* PARAM.SFO CATEGORY */
    char title[128];       /* PARAM.SFO TITLE (UTF-8) */
} NkPbpInfo;

typedef struct NkNpumdimgCallbacks {
    /* Called after each block with the bytes written so far. */
    void (*on_progress)(void *user, uint64_t done_bytes, uint64_t total_bytes);
    /* Nonzero stops the conversion (CANCELLED); nothing is kept. */
    int (*is_cancelled)(void *user);
    void *user;
} NkNpumdimgCallbacks;

typedef struct NkNpumdimg NkNpumdimg;

/* Identify a PBP package from its header, PARAM.SFO, DATA.PSP tag and
 * DATA.PSAR magic, with no key lookup.  NK_PSP_OK only for an NPUMDIMG
 * image; otherwise NK_PSP_ERR_FIRMWARE, NK_PSP_ERR_PSONE,
 * NK_PSP_ERR_UNSUPPORTED (another package form), NK_PSP_ERR_FORMAT (not a
 * well-formed PBP) or NK_PSP_ERR_INPUT, with out filled as far as it got. */
int nk_pbp_identify(NkPspCtx *ctx, const NkPsarSource *src, NkPbpInfo *out);
const char *nk_pbp_payload_name(NkPbpPayload payload);

/* Derive the image key from the 256-byte NPUMDIMG header, decrypt it,
 * check its layout and descramble the block table.  The KeyStore in ctx
 * supplies kirk.keyvault.56/57/99 and amctrl.loc_1CD4/1CE4/1CF4. */
int nk_npumdimg_open(NkPspCtx *ctx, const NkPsarSource *src,
                     const NkPbpInfo *pbp, NkNpumdimg **out);
uint32_t nk_npumdimg_lba_count(const NkNpumdimg *img);
uint32_t nk_npumdimg_block_count(const NkNpumdimg *img);
uint32_t nk_npumdimg_block_lbas(const NkNpumdimg *img);
/* Decrypt and expand one block; *data points at block_lbas * 2048 bytes
 * owned by img, valid until the next call. */
int nk_npumdimg_read_block(NkPspCtx *ctx, NkNpumdimg *img, uint32_t block,
                           const uint8_t **data);
void nk_npumdimg_close(NkNpumdimg *img);

/* Check an ISO image: a valid ISO9660 primary volume descriptor and a
 * PSP_GAME/PARAM.SFO whose DISC_ID equals expected_disc_id. */
int nk_npumdimg_check_iso(NkPspCtx *ctx, const NkPsarSource *iso,
                          const char *expected_disc_id);

/* File forms (UTF-8 paths).  nk_npumdimg_convert_file writes the whole
 * inner ISO to "<iso_path>.converting", checks it with
 * nk_npumdimg_check_iso against the package's own DISC_ID, then renames it
 * over iso_path; a failure or a cancel removes the partial file. */
int nk_pbp_identify_file(NkPspCtx *ctx, const char *pbp_path, NkPbpInfo *out);
int nk_npumdimg_convert_file(NkPspCtx *ctx, const char *pbp_path,
                             const char *iso_path,
                             const NkNpumdimgCallbacks *callbacks,
                             NkPbpInfo *info);
int nk_npumdimg_check_iso_file(NkPspCtx *ctx, const char *iso_path,
                               const char *expected_disc_id);

#ifdef __cplusplus
}
#endif

#endif /* NK_PSP_NPUMDIMG_H */
