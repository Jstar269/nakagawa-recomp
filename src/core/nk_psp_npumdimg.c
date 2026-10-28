/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2012- PPSSPP Project
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * NPUMDIMG reader: a PSN release of a UMD game back to its ISO (issue #295,
 * phase 2).
 *
 * The header key derivation, header decryption, block-table descrambling
 * and per-block decrypt/LZRC steps are ported from PPSSPP
 * (https://github.com/hrydgard/ppsspp) Core/FileSystems/BlockDevices.cpp,
 * NPDRMDemoBlockDevice, and the table_info layout from BlockDevices.h, at
 * revision b4386554795949491a26e51b7d74082bb9b2470f (GPL-2.0-or-later); this
 * file is conveyed as GPL-3.0-or-later.
 *
 * Modified for this project (see src/core/PROVENANCE.md):
 *   - C99 over a read-at callback, so a package of any size streams from
 *     disk; every DRM BB call threads an NkPspCtx and fails closed by name;
 *   - PPSSPP's bounds checks are kept (psarOffset, blockLBAs in 1..4096, the
 *     64-bit table size, the block index, table size against the block
 *     size) and tightened: the whole table and every block must lie inside
 *     the file, the image is at most 4 GiB, and a block a packaging tool
 *     marked (unk_1c) or a short last block fails instead of being taken as
 *     read;
 *   - the image is written whole, block by block, rather than served per
 *     sector, with progress and cancel callbacks and a temp-file-then-rename
 *     step; the PBP identification, the PSOne/firmware refusals and the
 *     ISO9660/PARAM.SFO result check are project-authored (ECMA-119 and the
 *     PBP/SFO layouts).
 */

#if !defined(_WIN32) && !defined(_WIN64)
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nk_psp_amctrl.h"
#include "nk_psp_builtin_keys.h"
#include "nk_psp_lzrc.h"
#include "nk_psp_npumdimg.h"

#define PBP_HEADER_SIZE 0x28u
#define SFO_MAX_SIZE (64u * 1024u)
#define NP_HEADER_SIZE 256u
#define TABLE_ENTRY_SIZE 32u
#define ISO_PVD_OFFSET (16u * NK_NPUMDIMG_SECTOR)
#define ISO_MAX_DIR_SIZE (1024u * 1024u)

/* One block-table row (BlockDevices.h table_info). */
struct table_info {
    u8 mac[16];
    u32 offset;
    int size;
    int flag;
    int unk_1c;
};

struct NkNpumdimg {
    const NkPsarSource *src;
    uint64_t psarOffset;
    u32 lbaSize_;
    u32 blockLBAs_;
    u32 blockSize_;
    u32 numBlocks_;
    u8 vkey[16];
    u8 hkey[16];
    struct table_info *table_;
    u8 *blockBuf_;
    u8 *tempBuf_;
};

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 rd16(const u8 *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static int src_read(NkPspCtx *ctx, const NkPsarSource *src, uint64_t offset,
                    void *buf, size_t len, const char *what)
{
    if (len == 0) return NK_PSP_OK;
    if (offset > src->size || (uint64_t)len > src->size - offset ||
        src->read_at(src->user, offset, buf, len) != 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                           "%s could not be read (the file is truncated or "
                           "unreadable)", what);
    }
    return NK_PSP_OK;
}

/* ------------------------------ PARAM.SFO ------------------------------ */

/* Copy one PARAM.SFO string value; 0 when found. */
static int sfo_string(const u8 *sfo, size_t size, const char *key, char *out,
                      size_t out_size)
{
    u32 key_table, data_table, count;
    if (out_size) out[0] = '\0';
    if (size < 20u || memcmp(sfo, "\0PSF", 4) != 0) return -1;
    key_table = rd32(sfo + 8);
    data_table = rd32(sfo + 12);
    count = rd32(sfo + 16);
    if (key_table > size || data_table > size || count > (size - 20u) / 16u)
        return -1;
    for (u32 i = 0; i < count; i++) {
        const u8 *e = sfo + 20u + i * 16u;
        u32 key_off = (u32)key_table + rd16(e);
        u16 fmt = rd16(e + 2);
        u32 len = rd32(e + 4);
        u32 data_off = rd32(e + 12);
        size_t klen;
        if (key_off >= size) return -1;
        klen = strnlen((const char *)sfo + key_off, size - key_off);
        if (klen == size - key_off) return -1;
        if (strcmp((const char *)sfo + key_off, key) != 0) continue;
        if (fmt != 0x0204u && fmt != 0x0004u) return -1;
        if ((uint64_t)data_table + data_off + len > size) return -1;
        {
            const char *v = (const char *)sfo + data_table + data_off;
            size_t n = strnlen(v, len);
            if (n >= out_size) n = out_size - 1u;
            memcpy(out, v, n);
            out[n] = '\0';
        }
        return 0;
    }
    return -1;
}

/* ------------------------------ PBP identify --------------------------- */

const char *nk_pbp_payload_name(NkPbpPayload payload)
{
    switch (payload) {
    case NK_PBP_PAYLOAD_NPUMDIMG: return "npumdimg";
    case NK_PBP_PAYLOAD_PSONE: return "psone";
    case NK_PBP_PAYLOAD_FIRMWARE: return "firmware";
    case NK_PBP_PAYLOAD_OTHER: return "other";
    default: return "unknown";
    }
}

static const char *firmware_tag_name(u32 tag)
{
    for (size_t i = 0; i < nk_psp_firmware_tag_count; i++) {
        if (nk_psp_firmware_tags[i].tag == tag) return nk_psp_firmware_tags[i].what;
    }
    return NULL;
}

int nk_pbp_identify(NkPspCtx *ctx, const NkPsarSource *src, NkPbpInfo *out)
{
    u8 hdr[PBP_HEADER_SIZE];
    u8 psp_head[0xD4];
    u8 psar_head[16];
    u32 offs[8];
    int rc;

    if (ctx == NULL || src == NULL || src->read_at == NULL || out == NULL)
        return NK_PSP_ERR_INTERNAL;
    memset(out, 0, sizeof(*out));
    if (src->size < PBP_HEADER_SIZE) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "the file is too small to be a PBP package");
    }
    rc = src_read(ctx, src, 0, hdr, sizeof(hdr), "the package header");
    if (rc != NK_PSP_OK) return rc;
    if (memcmp(hdr, "\0PBP", 4) != 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "the file is not a PBP package");
    }
    for (int i = 0; i < 8; i++) {
        offs[i] = rd32(hdr + 8 + i * 4);
        if (offs[i] < PBP_HEADER_SIZE || (uint64_t)offs[i] > src->size ||
            (i > 0 && offs[i] < offs[i - 1])) {
            return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                               "the PBP package's section table is malformed");
        }
    }
    out->sfo_offset = offs[0];
    out->sfo_size = offs[1] - offs[0];
    out->psp_offset = offs[6];
    out->psar_offset = offs[7];

    if (out->sfo_size > 0u && out->sfo_size <= SFO_MAX_SIZE) {
        u8 *sfo = (u8 *)malloc(out->sfo_size);
        if (sfo == NULL) {
            return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL, "out of memory");
        }
        rc = src_read(ctx, src, out->sfo_offset, sfo, out->sfo_size,
                      "the package's PARAM.SFO");
        if (rc != NK_PSP_OK) {
            free(sfo);
            return rc;
        }
        (void)sfo_string(sfo, out->sfo_size, "DISC_ID", out->disc_id, sizeof(out->disc_id));
        (void)sfo_string(sfo, out->sfo_size, "DISC_VERSION", out->disc_version,
                         sizeof(out->disc_version));
        (void)sfo_string(sfo, out->sfo_size, "CATEGORY", out->category, sizeof(out->category));
        (void)sfo_string(sfo, out->sfo_size, "TITLE", out->title, sizeof(out->title));
        free(sfo);
    }

    /* Firmware first: an updater's PARAM.SFO category, its DATA.PSP tag, or
     * a PSAR archive in DATA.PSAR, before anything else is looked at. */
    if (strcmp(out->category, "MG") == 0) {
        out->payload = NK_PBP_PAYLOAD_FIRMWARE;
        return nk_psp_fail(ctx, NK_PSP_ERR_FIRMWARE, NULL,
                           "this package is a PSP system software update; "
                           "Nakagawa never decrypts firmware");
    }
    if ((uint64_t)out->psp_offset + sizeof(psp_head) <= (uint64_t)out->psar_offset) {
        rc = src_read(ctx, src, out->psp_offset, psp_head, sizeof(psp_head),
                      "the package's DATA.PSP");
        if (rc != NK_PSP_OK) return rc;
        if (memcmp(psp_head, "~PSP", 4) == 0) {
            const char *what = firmware_tag_name(rd32(psp_head + 0xD0));
            if (what != NULL) {
                out->payload = NK_PBP_PAYLOAD_FIRMWARE;
                return nk_psp_fail(ctx, NK_PSP_ERR_FIRMWARE, NULL,
                                   "this package carries a %s; Nakagawa never "
                                   "decrypts firmware", what);
            }
        }
    }
    memset(psar_head, 0, sizeof(psar_head));
    if ((uint64_t)out->psar_offset + sizeof(psar_head) <= src->size) {
        rc = src_read(ctx, src, out->psar_offset, psar_head, sizeof(psar_head),
                      "the package's DATA.PSAR");
        if (rc != NK_PSP_OK) return rc;
    }
    if (memcmp(psar_head, "PSAR", 4) == 0 || memcmp(psar_head, "\0PSAR", 5) == 0) {
        out->payload = NK_PBP_PAYLOAD_FIRMWARE;
        return nk_psp_fail(ctx, NK_PSP_ERR_FIRMWARE, NULL,
                           "this package's DATA.PSAR is a firmware updater "
                           "archive (PSAR); Nakagawa never decrypts firmware");
    }
    if (memcmp(psar_head, "PSISOIMG", 8) == 0 || memcmp(psar_head, "PSTITLEIMG", 10) == 0) {
        out->payload = NK_PBP_PAYLOAD_PSONE;
        return nk_psp_fail(ctx, NK_PSP_ERR_PSONE, NULL,
                           "this package is a PSOne Classic (a PlayStation "
                           "game in the PSP's PS1 emulator); Nakagawa "
                           "recompiles PSP games only");
    }
    if (memcmp(psar_head, "NPUMDIMG", 8) != 0) {
        out->payload = NK_PBP_PAYLOAD_OTHER;
        return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                           "this package does not hold a PSN UMD image "
                           "(NPUMDIMG); only PSN releases of UMD games are "
                           "converted");
    }
    out->payload = NK_PBP_PAYLOAD_NPUMDIMG;
    return NK_PSP_OK;
}

/* ------------------------------ NPUMDIMG ------------------------------- */

void nk_npumdimg_close(NkNpumdimg *img)
{
    if (img == NULL) return;
    free(img->table_);
    free(img->tempBuf_);
    free(img->blockBuf_);
    memset(img->vkey, 0, sizeof(img->vkey));
    memset(img->hkey, 0, sizeof(img->hkey));
    free(img);
}

uint32_t nk_npumdimg_lba_count(const NkNpumdimg *img) { return img ? img->lbaSize_ : 0u; }
uint32_t nk_npumdimg_block_count(const NkNpumdimg *img) { return img ? img->numBlocks_ : 0u; }
uint32_t nk_npumdimg_block_lbas(const NkNpumdimg *img) { return img ? img->blockLBAs_ : 0u; }

static int bad_header(NkPspCtx *ctx, const char *what)
{
    return nk_psp_fail(ctx, NK_PSP_ERR_INTEGRITY, NULL,
                       "the PSN image header does not decrypt to a valid "
                       "layout (%s); the package is damaged or was changed",
                       what);
}

int nk_npumdimg_open(NkPspCtx *ctx, const NkPsarSource *src,
                     const NkPbpInfo *pbp, NkNpumdimg **out)
{
    MAC_KEY mkey;
    CIPHER_KEY ckey;
    u8 np_header[NP_HEADER_SIZE];
    u32 lbaStart, lbaEnd, tableOffset_;
    uint64_t psar_room, tableSize64;
    NkNpumdimg *img;
    u8 *raw;
    int rc;

    if (ctx == NULL || src == NULL || pbp == NULL || out == NULL)
        return NK_PSP_ERR_INTERNAL;
    *out = NULL;
    if (pbp->payload != NK_PBP_PAYLOAD_NPUMDIMG) {
        return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                           "this package does not hold a PSN UMD image");
    }
    if ((uint64_t)pbp->psar_offset >= src->size ||
        src->size - pbp->psar_offset <= NP_HEADER_SIZE) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "the PSN image header is truncated");
    }
    psar_room = src->size - pbp->psar_offset;
    rc = src_read(ctx, src, pbp->psar_offset, np_header, sizeof(np_header),
                  "the PSN image header");
    if (rc != NK_PSP_OK) return rc;
    if (memcmp(np_header, "NPUMDIMG", 8) != 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                           "this package does not hold a PSN UMD image");
    }

    img = (NkNpumdimg *)calloc(1, sizeof(*img));
    if (img == NULL) return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL, "out of memory");
    img->src = src;
    img->psarOffset = pbp->psar_offset;

    /* The block size is in the clear: check it before any key is read. */
    img->blockLBAs_ = rd32(np_header + 0x0c); // block size in LBA
    if (img->blockLBAs_ == 0u || img->blockLBAs_ > NK_NPUMDIMG_MAX_BLOCK_LBAS) {
        nk_npumdimg_close(img);
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "the PSN image declares an impossible block size "
                           "(%u sectors; at most %u)",
                           (unsigned)rd32(np_header + 0x0c),
                           (unsigned)NK_NPUMDIMG_MAX_BLOCK_LBAS);
    }

    // getkey
    rc = sceDrmBBMacInit(ctx, &mkey, 3);
    if (rc == 0) rc = sceDrmBBMacUpdate(ctx, &mkey, np_header, 0xc0);
    if (rc == 0) rc = bbmac_getkey(ctx, &mkey, np_header+0xc0, img->vkey);

    // decrypt NP header
    memcpy(img->hkey, np_header+0xa0, 0x10);
    if (rc == 0) rc = sceDrmBBCipherInit(ctx, &ckey, 1, 2, img->hkey, img->vkey, 0);
    if (rc == 0) rc = sceDrmBBCipherUpdate(ctx, &ckey, np_header+0x40, 0x60);
    (void)sceDrmBBCipherFinal(&ckey);
    if (rc != 0) {
        nk_npumdimg_close(img);
        return rc;
    }

    lbaStart = rd32(np_header+0x54); // LBA start
    lbaEnd   = rd32(np_header+0x64); // LBA end
    if (lbaEnd < lbaStart) {
        nk_npumdimg_close(img);
        return bad_header(ctx, "the sector range is reversed");
    }
    if ((uint64_t)lbaEnd - lbaStart + 1u > NK_NPUMDIMG_MAX_LBAS) {
        nk_npumdimg_close(img);
        return bad_header(ctx, "the image would be larger than 4 GiB");
    }
    img->lbaSize_ = lbaEnd - lbaStart + 1u; // LBA size of ISO
    img->blockSize_ = img->blockLBAs_ * NK_NPUMDIMG_SECTOR;
    img->numBlocks_ = (img->lbaSize_ + img->blockLBAs_ - 1u) / img->blockLBAs_; // total blocks;

    tableOffset_ = rd32(np_header + 0x6c); // table offset
    tableSize64 = (uint64_t)img->numBlocks_ * TABLE_ENTRY_SIZE;
    if (tableOffset_ > psar_room || tableSize64 == 0u ||
        tableSize64 > psar_room - tableOffset_) {
        nk_npumdimg_close(img);
        return bad_header(ctx, "the block table lies outside the file");
    }

    img->table_ = (struct table_info *)calloc(img->numBlocks_, sizeof(struct table_info));
    raw = (u8 *)malloc((size_t)tableSize64);
    img->blockBuf_ = (u8 *)malloc(img->blockSize_);
    img->tempBuf_ = (u8 *)malloc(img->blockSize_);
    if (img->table_ == NULL || raw == NULL || img->blockBuf_ == NULL || img->tempBuf_ == NULL) {
        free(raw);
        nk_npumdimg_close(img);
        return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL, "out of memory");
    }
    rc = src_read(ctx, src, img->psarOffset + tableOffset_, raw, (size_t)tableSize64,
                  "the PSN image's block table");
    if (rc != NK_PSP_OK) {
        free(raw);
        nk_npumdimg_close(img);
        return rc;
    }

    {
        u8 *p = raw;
        u32 i, k0, k1, k2, k3;
        for (i = 0; i < img->numBlocks_; i++){
            u32 w[8];
            for (int j = 0; j < 8; j++) w[j] = rd32(p + j * 4);
            k0 = w[0]^w[1];
            k1 = w[1]^w[2];
            k2 = w[0]^w[3];
            k3 = w[2]^w[3];
            w[4] ^= k3;
            w[5] ^= k1;
            w[6] ^= k2;
            w[7] ^= k0;
            memcpy(img->table_[i].mac, p, 16);
            img->table_[i].offset = w[4];
            img->table_[i].size = (int)w[5];
            img->table_[i].flag = (int)w[6];
            img->table_[i].unk_1c = (int)w[7];
            p += TABLE_ENTRY_SIZE;
        }
    }
    free(raw);

    /* Check every row before any block is read, so a bad table writes
     * nothing. */
    for (u32 i = 0; i < img->numBlocks_; i++) {
        const struct table_info *t = &img->table_[i];
        if (t->unk_1c != 0) {
            nk_npumdimg_close(img);
            return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                               "block %u of the PSN image is marked as made by "
                               "a packaging tool, not the PlayStation Store",
                               (unsigned)i);
        }
        if (t->size <= 0 || (u32)t->size > img->blockSize_ ||
            (uint64_t)t->offset + (u32)t->size > psar_room) {
            nk_npumdimg_close(img);
            return bad_header(ctx, "a block-table entry points outside the file "
                                   "or past the block size");
        }
    }

    *out = img;
    return NK_PSP_OK;
}

int nk_npumdimg_read_block(NkPspCtx *ctx, NkNpumdimg *img, uint32_t block,
                           const uint8_t **data)
{
    const struct table_info *t;
    u8 *readBuf;
    int rc;

    if (ctx == NULL || img == NULL || data == NULL) return NK_PSP_ERR_INTERNAL;
    *data = NULL;
    // The caller's block number is bounds-checked before indexing.
    if (block >= img->numBlocks_) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                           "block %u is past the image", (unsigned)block);
    }
    t = &img->table_[block];

    if ((u32)t->size < img->blockSize_)
        readBuf = img->tempBuf_;
    else
        readBuf = img->blockBuf_;

    rc = src_read(ctx, img->src, img->psarOffset + t->offset, readBuf, (size_t)t->size,
                  "a block of the PSN image");
    if (rc != NK_PSP_OK) return rc;

    if ((t->flag & 4) == 0) {
        CIPHER_KEY ckey;
        rc = sceDrmBBCipherInit(ctx, &ckey, 1, 2, img->hkey, img->vkey, t->offset>>4);
        if (rc == 0) rc = sceDrmBBCipherUpdate(ctx, &ckey, readBuf, t->size);
        (void)sceDrmBBCipherFinal(&ckey);
        if (rc != 0) return rc;
    }

    if ((u32)t->size < img->blockSize_) {
        // The decompressed block is always blockSize_ bytes; blockBuf_ is exactly
        // that big, and the decoder cannot write past it.
        int lzsize = nk_psp_lzrc_decompress(img->blockBuf_, img->blockSize_,
                                            readBuf, (size_t)t->size);
        if (lzsize < 0 || (u32)lzsize != img->blockSize_) {
            return nk_psp_fail(ctx, NK_PSP_ERR_DECOMPRESS, NULL,
                               "block %u of the PSN image did not expand to "
                               "%u bytes; the package is damaged or was changed",
                               (unsigned)block, (unsigned)img->blockSize_);
        }
    }

    *data = img->blockBuf_;
    return NK_PSP_OK;
}

/* ------------------------------ ISO check ------------------------------ */

static int iso_bad(NkPspCtx *ctx, const char *what)
{
    return nk_psp_fail(ctx, NK_PSP_ERR_INTEGRITY, NULL,
                       "the converted disc image is not valid: %s", what);
}

/* Find name (case-insensitive, ";1" ignored) in a directory extent. */
static int iso_find(NkPspCtx *ctx, const NkPsarSource *iso, u32 dir_lba, u32 dir_size,
                    const char *name, int want_dir, u32 *lba, u32 *size)
{
    u8 *dir;
    size_t name_len = strlen(name);
    int rc;
    if (dir_size == 0u || dir_size > ISO_MAX_DIR_SIZE) return iso_bad(ctx, "a directory has an impossible size");
    dir = (u8 *)malloc(dir_size);
    if (dir == NULL) return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL, "out of memory");
    rc = src_read(ctx, iso, (uint64_t)dir_lba * NK_NPUMDIMG_SECTOR, dir, dir_size,
                  "a directory of the converted disc image");
    if (rc != NK_PSP_OK) {
        free(dir);
        return rc;
    }
    for (u32 pos = 0; pos < dir_size;) {
        u32 rec_len = dir[pos];
        if (rec_len == 0u) {
            pos = (pos / NK_NPUMDIMG_SECTOR + 1u) * NK_NPUMDIMG_SECTOR;
            continue;
        }
        if (rec_len < 34u || pos + rec_len > dir_size) break;
        {
            const u8 *r = dir + pos;
            u32 nlen = r[32];
            int is_dir = (r[25] & 0x02u) != 0;
            if (33u + nlen <= rec_len) {
                size_t cmp_len = nlen;
                const char *n = (const char *)r + 33;
                const char *semi = memchr(n, ';', nlen);
                if (semi != NULL) cmp_len = (size_t)(semi - n);
                if (cmp_len == name_len && is_dir == want_dir) {
                    size_t k;
                    for (k = 0; k < cmp_len; k++) {
                        char a = n[k], b = name[k];
                        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
                        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
                        if (a != b) break;
                    }
                    if (k == cmp_len) {
                        *lba = rd32(r + 2);
                        *size = rd32(r + 10);
                        free(dir);
                        return NK_PSP_OK;
                    }
                }
            }
        }
        pos += rec_len;
    }
    free(dir);
    return iso_bad(ctx, want_dir ? "the PSP_GAME folder is missing" : "PSP_GAME/PARAM.SFO is missing");
}

int nk_npumdimg_check_iso(NkPspCtx *ctx, const NkPsarSource *iso,
                          const char *expected_disc_id)
{
    u8 pvd[NK_NPUMDIMG_SECTOR];
    u32 root_lba, root_size, game_lba, game_size, sfo_lba, sfo_size;
    char disc_id[16];
    u8 *sfo;
    int rc;

    if (ctx == NULL || iso == NULL || iso->read_at == NULL || expected_disc_id == NULL)
        return NK_PSP_ERR_INTERNAL;
    if (iso->size < (uint64_t)ISO_PVD_OFFSET + NK_NPUMDIMG_SECTOR)
        return iso_bad(ctx, "it is too small to hold a volume descriptor");
    rc = src_read(ctx, iso, ISO_PVD_OFFSET, pvd, sizeof(pvd),
                  "the converted disc image's volume descriptor");
    if (rc != NK_PSP_OK) return rc;
    if (pvd[0] != 1u || memcmp(pvd + 1, "CD001", 5) != 0 || pvd[6] != 1u)
        return iso_bad(ctx, "it has no ISO9660 primary volume descriptor");
    root_lba = rd32(pvd + 156 + 2);
    root_size = rd32(pvd + 156 + 10);
    rc = iso_find(ctx, iso, root_lba, root_size, "PSP_GAME", 1, &game_lba, &game_size);
    if (rc != NK_PSP_OK) return rc;
    rc = iso_find(ctx, iso, game_lba, game_size, "PARAM.SFO", 0, &sfo_lba, &sfo_size);
    if (rc != NK_PSP_OK) return rc;
    if (sfo_size == 0u || sfo_size > SFO_MAX_SIZE) return iso_bad(ctx, "PARAM.SFO has an impossible size");
    sfo = (u8 *)malloc(sfo_size);
    if (sfo == NULL) return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL, "out of memory");
    rc = src_read(ctx, iso, (uint64_t)sfo_lba * NK_NPUMDIMG_SECTOR, sfo, sfo_size,
                  "the converted disc image's PARAM.SFO");
    if (rc != NK_PSP_OK) {
        free(sfo);
        return rc;
    }
    if (sfo_string(sfo, sfo_size, "DISC_ID", disc_id, sizeof(disc_id)) != 0) {
        free(sfo);
        return iso_bad(ctx, "its PARAM.SFO has no DISC_ID");
    }
    free(sfo);
    if (strcmp(disc_id, expected_disc_id) != 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INTEGRITY, NULL,
                           "the converted disc image says it is %.15s, but the "
                           "package says %.15s", disc_id, expected_disc_id);
    }
    return NK_PSP_OK;
}

/* ------------------------------ files ---------------------------------- */

/* The package and image live under the caller's per-user data root, which
 * may hold non-ASCII characters the narrow CRT would re-read in the active
 * ANSI code page.  This translation unit is also built into nk_decrypt,
 * which has no platform layer (tools/nk_core/decrypt_tool.py), so the wide
 * conversion is local, as in nk_decrypt_main.c and nk_psp_keystore.c. */
#if defined(_WIN32) || defined(_WIN64)
static int npumdimg_wide(const char *path, wchar_t *out, int cap)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, cap) > 0;
}
#endif

static FILE *npumdimg_fopen(const char *path, const char *mode)
{
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wpath[32768];
    wchar_t wmode[8];
    if (path == NULL || mode == NULL) return NULL;
    if (!npumdimg_wide(path, wpath, 32768) || !npumdimg_wide(mode, wmode, 8)) return NULL;
    return _wfopen(wpath, wmode);
#else
    if (path == NULL || mode == NULL) return NULL;
    return fopen(path, mode);
#endif
}

static void npumdimg_remove(const char *path)
{
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wpath[32768];
    if (npumdimg_wide(path, wpath, 32768)) (void)DeleteFileW(wpath);
#else
    (void)remove(path);
#endif
}

static int npumdimg_replace(const char *from, const char *to)
{
#if defined(_WIN32) || defined(_WIN64)
    wchar_t wfrom[32768];
    wchar_t wto[32768];
    if (!npumdimg_wide(from, wfrom, 32768) || !npumdimg_wide(to, wto, 32768)) return -1;
    return MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

static int npumdimg_seek(FILE *f, uint64_t offset)
{
#if defined(_WIN32) || defined(_WIN64)
    return _fseeki64(f, (__int64)offset, SEEK_SET);
#else
    return fseeko(f, (off_t)offset, SEEK_SET);
#endif
}

static int file_size(FILE *f, uint64_t *size)
{
#if defined(_WIN32) || defined(_WIN64)
    __int64 end;
    if (_fseeki64(f, 0, SEEK_END) != 0) return -1;
    end = _ftelli64(f);
#else
    off_t end;
    if (fseeko(f, 0, SEEK_END) != 0) return -1;
    end = ftello(f);
#endif
    if (end < 0) return -1;
    *size = (uint64_t)end;
    return 0;
}

static int file_read_at(void *user, uint64_t offset, void *buf, size_t len)
{
    FILE *f = (FILE *)user;
    if (npumdimg_seek(f, offset) != 0) return -1;
    return fread(buf, 1, len, f) == len ? 0 : -1;
}

static int open_source(NkPspCtx *ctx, const char *path, FILE **f, NkPsarSource *src)
{
    *f = npumdimg_fopen(path, "rb");
    if (*f == NULL) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL, "the file could not be opened");
    }
    if (file_size(*f, &src->size) != 0) {
        fclose(*f);
        *f = NULL;
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL, "the file's size could not be read");
    }
    src->read_at = file_read_at;
    src->user = *f;
    return NK_PSP_OK;
}

int nk_pbp_identify_file(NkPspCtx *ctx, const char *pbp_path, NkPbpInfo *out)
{
    FILE *f = NULL;
    NkPsarSource src;
    int rc;
    if (ctx == NULL || pbp_path == NULL || out == NULL) return NK_PSP_ERR_INTERNAL;
    memset(out, 0, sizeof(*out));
    rc = open_source(ctx, pbp_path, &f, &src);
    if (rc != NK_PSP_OK) return rc;
    rc = nk_pbp_identify(ctx, &src, out);
    fclose(f);
    return rc;
}

int nk_npumdimg_check_iso_file(NkPspCtx *ctx, const char *iso_path,
                               const char *expected_disc_id)
{
    FILE *f = NULL;
    NkPsarSource src;
    int rc;
    if (ctx == NULL || iso_path == NULL) return NK_PSP_ERR_INTERNAL;
    rc = open_source(ctx, iso_path, &f, &src);
    if (rc != NK_PSP_OK) return rc;
    rc = nk_npumdimg_check_iso(ctx, &src, expected_disc_id);
    fclose(f);
    return rc;
}

int nk_npumdimg_convert_file(NkPspCtx *ctx, const char *pbp_path,
                             const char *iso_path,
                             const NkNpumdimgCallbacks *callbacks,
                             NkPbpInfo *info)
{
    FILE *in = NULL;
    FILE *out = NULL;
    NkPsarSource src;
    NkPbpInfo local;
    NkPbpInfo *pbp = info != NULL ? info : &local;
    NkNpumdimg *img = NULL;
    char *temp = NULL;
    size_t temp_len;
    uint64_t total, done = 0;
    int rc;

    if (ctx == NULL || pbp_path == NULL || iso_path == NULL) return NK_PSP_ERR_INTERNAL;
    memset(pbp, 0, sizeof(*pbp));
    rc = open_source(ctx, pbp_path, &in, &src);
    if (rc != NK_PSP_OK) return rc;
    rc = nk_pbp_identify(ctx, &src, pbp);
    if (rc == NK_PSP_OK && pbp->disc_id[0] == '\0') {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                         "the package's PARAM.SFO has no DISC_ID to check the "
                         "result against");
    }
    if (rc == NK_PSP_OK) rc = nk_npumdimg_open(ctx, &src, pbp, &img);
    if (rc != NK_PSP_OK) {
        fclose(in);
        return rc;
    }

    temp_len = strlen(iso_path) + sizeof(".converting");
    temp = (char *)malloc(temp_len);
    if (temp == NULL) {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL, "out of memory");
        goto done;
    }
    snprintf(temp, temp_len, "%s.converting", iso_path);
    npumdimg_remove(temp);
    out = npumdimg_fopen(temp, "wb");
    if (out == NULL) {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                         "the disc image could not be created");
        goto done;
    }

    total = (uint64_t)img->lbaSize_ * NK_NPUMDIMG_SECTOR;
    for (u32 block = 0; block < img->numBlocks_; block++) {
        const uint8_t *data = NULL;
        u32 first_lba = block * img->blockLBAs_;
        u32 lbas = img->lbaSize_ - first_lba;
        size_t bytes;
        if (lbas > img->blockLBAs_) lbas = img->blockLBAs_;
        if (callbacks != NULL && callbacks->is_cancelled != NULL &&
            callbacks->is_cancelled(callbacks->user)) {
            rc = nk_psp_fail(ctx, NK_PSP_ERR_CANCELLED, NULL,
                             "the conversion was cancelled; the partial disc "
                             "image was removed");
            goto done;
        }
        rc = nk_npumdimg_read_block(ctx, img, block, &data);
        if (rc != NK_PSP_OK) goto done;
        bytes = (size_t)lbas * NK_NPUMDIMG_SECTOR;
        if (fwrite(data, 1, bytes, out) != bytes) {
            rc = nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                             "the disc image could not be written (is the disk full?)");
            goto done;
        }
        done += bytes;
        if (callbacks != NULL && callbacks->on_progress != NULL)
            callbacks->on_progress(callbacks->user, done, total);
    }
    if (fclose(out) != 0) {
        out = NULL;
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                         "the disc image could not be written (is the disk full?)");
        goto done;
    }
    out = NULL;

    rc = nk_npumdimg_check_iso_file(ctx, temp, pbp->disc_id);
    if (rc != NK_PSP_OK) goto done;
    if (npumdimg_replace(temp, iso_path) != 0) {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                         "the disc image could not be moved into place");
        goto done;
    }

done:
    if (out != NULL) fclose(out);
    if (rc != NK_PSP_OK && temp != NULL) npumdimg_remove(temp);
    free(temp);
    nk_npumdimg_close(img);
    fclose(in);
    return rc;
}
