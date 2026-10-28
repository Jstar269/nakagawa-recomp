/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Container-level probe and decrypt orchestration for the built-in PSP
 * decryption boundary (issue #295).  Project-authored from the format
 * rules documented for this project; no key material here.
 *
 * Structural validation runs BEFORE any key lookup so a malformed
 * container reports CONTAINER_MALFORMED rather than a key diagnostic,
 * and a well-formed one reports exactly the key entry it needs.
 *
 * Firmware is never decrypted (owner rule, 2026-10-27): a firmware updater
 * archive (PSAR), a raw KIRK block in the IPL form, and a module whose tag
 * only firmware containers carry are refused by name here, before any key
 * lookup or decryption step runs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nk_psp_builtin_keys.h"
#include "nk_psp_container.h"
#include "nk_psp_inflate.h"
#include "nk_psp_kle.h"
#include "nk_psp_prx.h"

#define NK_PSP_HEADER_SIZE 0x150u
#define NK_MAX_WRAPPER_DEPTH 8
#define NK_DECOMP_CAP (64u * 1024u * 1024u)
#define NK_ELF_MIN_SIZE 0x34u

typedef struct View {
    const uint8_t *data;
    size_t size;
    int wrappers;
} View;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int has_magic(const uint8_t *d, size_t size, const char *magic,
                     size_t magic_len)
{
    return size >= magic_len && memcmp(d, magic, magic_len) == 0;
}

/* ------------------------- firmware refusal --------------------------- */

#define NK_KIRK_CMD1_MODE_OFFSET 0x60u

static int refuse_firmware_tag(NkPspCtx *ctx, uint32_t tag)
{
    for (size_t i = 0; i < nk_psp_firmware_tag_count; i++) {
        if (nk_psp_firmware_tags[i].tag == tag) {
            return nk_psp_fail(ctx, NK_PSP_ERR_FIRMWARE, NULL,
                               "container tag 0x%08X marks a %s; Nakagawa "
                               "never decrypts firmware",
                               (unsigned)tag, nk_psp_firmware_tags[i].what);
        }
    }
    return NK_PSP_OK;
}

/* Firmware forms that carry no ~PSP tag: an updater archive, and a bare KIRK
 * command-1 block, which is how the IPL is stored. */
static int refuse_firmware_form(NkPspCtx *ctx, const uint8_t *d, size_t size)
{
    if (has_magic(d, size, "PSAR", 4) || has_magic(d, size, "\0PSAR", 5)) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FIRMWARE, NULL,
                           "this is a firmware updater archive (PSAR); "
                           "Nakagawa never decrypts firmware");
    }
    if (size >= NK_KIRK_CMD1_MODE_OFFSET + 4u &&
        rd32(d + NK_KIRK_CMD1_MODE_OFFSET) == 1u &&
        !has_magic(d, size, "~PSP", 4) && !has_magic(d, size, "~SCE", 4) &&
        !has_magic(d, size, "\0PBP", 4) && !has_magic(d, size, "\x7f" "ELF", 4) &&
        !has_magic(d, size, "\x1f\x8b", 2)) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FIRMWARE, NULL,
                           "this is a bare KIRK block, the form the boot "
                           "loader (IPL) is stored in; Nakagawa never "
                           "decrypts firmware");
    }
    return NK_PSP_OK;
}

/* ------------------------- validation -------------------------------- */

/* ~PSP header fields read in this file: 0x06 compression attribute
 * (bit 0 = compressed), 0x27 segment count, 0x28 decompressed ELF size, 0x2C total
 * container size, 0x54 up to four segment sizes, 0xD0 tag. */
static int validate_psp(NkPspCtx *ctx, const uint8_t *d, size_t size)
{
    uint32_t psp_size;
    if (size < NK_PSP_HEADER_SIZE) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "container is too small for a ~PSP header "
                           "(%u bytes, need %u); the file is truncated or "
                           "not a module container",
                           (unsigned)size, (unsigned)NK_PSP_HEADER_SIZE);
    }
    psp_size = rd32(d + 0x2C);
    if (psp_size < NK_PSP_HEADER_SIZE) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "container declares an impossible total size "
                           "0x%08X",
                           (unsigned)psp_size);
    }
    if ((size_t)psp_size > size) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "container is truncated (declares 0x%08X bytes, "
                           "file carries %u)",
                           (unsigned)psp_size, (unsigned)size);
    }
    if (d[0x27] == 0u || d[0x27] > 4u) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "container declares an impossible segment count "
                           "(%u)",
                           (unsigned)d[0x27]);
    }
    {
        uint32_t total_segment_bytes = 0;
        uint8_t segment_count = d[0x27];
        for (uint8_t i = 0; i < segment_count; i++) {
            uint32_t segment_size = rd32(d + 0x54 + i * 4u);
            if (segment_size > (256u * 1024u * 1024u)) {
                return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                                   "container segment %u declares an impossible size "
                                   "0x%08X",
                                   (unsigned)i, (unsigned)segment_size);
            }
            if (total_segment_bytes > (256u * 1024u * 1024u) - segment_size) {
                return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                                   "container segment sizes overflow aggregate extent");
            }
            total_segment_bytes += segment_size;
        }
    }
    return NK_PSP_OK;
}

/* Extract the DATA.PSP slice out of a PBP package (fail closed). */
static int pbp_slice(NkPspCtx *ctx, const uint8_t *d, size_t size,
                     const uint8_t **slice, size_t *slice_size)
{
    uint32_t off_psp;
    uint32_t off_psar;
    size_t end;
    if (size < 0x28u) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "PBP package header is truncated (%u bytes)",
                           (unsigned)size);
    }
    off_psp = rd32(d + 0x20);
    off_psar = rd32(d + 0x24);
    if (off_psp < 0x28u || (size_t)off_psp >= size) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "PBP package has an impossible DATA.PSP offset "
                           "0x%08X",
                           (unsigned)off_psp);
    }
    end = size;
    if ((size_t)off_psar > (size_t)off_psp && (size_t)off_psar <= size) {
        end = (size_t)off_psar;
    }
    *slice = d + off_psp;
    *slice_size = end - (size_t)off_psp;
    return NK_PSP_OK;
}

/* Strip PBP and ~SCE outer layers until an inner module form remains. */
static int view_unwrap(NkPspCtx *ctx, View *v)
{
    int depth;
    for (depth = 0; depth < NK_MAX_WRAPPER_DEPTH; depth++) {
        int rc = refuse_firmware_form(ctx, v->data, v->size);
        if (rc != NK_PSP_OK) return rc;
        if (has_magic(v->data, v->size, "\0PBP", 4)) {
            const uint8_t *slice = NULL;
            size_t slice_size = 0;
            rc = pbp_slice(ctx, v->data, v->size, &slice, &slice_size);
            if (rc != NK_PSP_OK) return rc;
            v->data = slice;
            v->size = slice_size;
            v->wrappers++;
            continue;
        }
        if (has_magic(v->data, v->size, "~SCE", 4)) {
            uint32_t header_size;
            if (v->size < 8u) {
                return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                                   "~SCE wrapper header is truncated");
            }
            header_size = rd32(v->data + 4);
            if (header_size < 8u || (size_t)header_size >= v->size) {
                return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                                   "~SCE wrapper declares an impossible "
                                   "header size 0x%08X",
                                   (unsigned)header_size);
            }
            v->data += header_size;
            v->size -= header_size;
            v->wrappers++;
            continue;
        }
        break;
    }
    if (depth == NK_MAX_WRAPPER_DEPTH) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "container nesting is too deep");
    }
    return NK_PSP_OK;
}

static NkContainerKind classify(const uint8_t *d, size_t size)
{
    if (has_magic(d, size, "~PSP", 4)) return NK_CTR_PSP;
    if (has_magic(d, size, "\x7f"
                           "ELF", 4)) return NK_CTR_PLAIN_ELF;
    if (has_magic(d, size, "\x1f"
                           "\x8b", 2)) return NK_CTR_GZIP;
    if (has_magic(d, size, "~SCE", 4)) return NK_CTR_SCE_WRAPPER;
    if (has_magic(d, size, "\0PBP", 4)) return NK_CTR_PBP;
    return NK_CTR_UNKNOWN;
}

/* ---------------------------- probe ---------------------------------- */

int nk_container_probe(NkPspCtx *ctx, const uint8_t *data, size_t size,
                       NkContainerInfo *out)
{
    View v;
    int rc;
    if (out == NULL) return NK_PSP_ERR_INTERNAL;
    memset(out, 0, sizeof(*out));
    if (data == NULL || size == 0u) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                           "input is empty");
    }
    v.data = data;
    v.size = size;
    v.wrappers = 0;
    rc = view_unwrap(ctx, &v);
    if (rc != NK_PSP_OK) return rc;
    out->wrappers_skipped = v.wrappers;
    out->kind = classify(v.data, v.size);
    switch (out->kind) {
    case NK_CTR_PSP:
        rc = validate_psp(ctx, v.data, v.size);
        if (rc != NK_PSP_OK) {
            out->kind = NK_CTR_UNKNOWN;
            return rc;
        }
        out->tag = rd32(v.data + 0xD0);
        out->compressed = (rd16(v.data + 0x06) & 1u) != 0u;
        return refuse_firmware_tag(ctx, out->tag);
    case NK_CTR_PLAIN_ELF:
    case NK_CTR_GZIP:
        return NK_PSP_OK;
    default:
        out->kind = NK_CTR_UNKNOWN;
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "unrecognized container magic "
                           "(expected ~PSP, ~SCE, PBP, gzip or ELF)");
    }
}

/* ------------------------ key-entry listing --------------------------- */

int nk_container_key_entries(const NkContainerInfo *info,
                             const NkKeystore *ks,
                             char entries[][80], int max_entries)
{
    int count = 0;
    if (info == NULL || entries == NULL || max_entries <= 0) return 0;
    if (info->kind != NK_CTR_PSP) return 0;

    snprintf(entries[count], (size_t)80, "prx.tag.0x%08X", info->tag);
    count++;
    if (count >= max_entries) return count;
    snprintf(entries[count], (size_t)80, "kirk.cmd1.key");
    count++;
    if (count >= max_entries) return count;
    if (ks != NULL) {
        NkPrxTagEntry recipe;
        if (nk_keystore_get_prx_tag(ks, info->tag, &recipe) == 0 &&
            recipe.have_code && recipe.code >= 0 && recipe.code <= 127) {
            snprintf(entries[count], (size_t)80, "kirk.keyvault.%d",
                     recipe.code);
            count++;
        }
    }
    return count;
}

/* --------------------------- decrypt ---------------------------------- */

/* Decompress a decrypted ~PSP payload (gzip or KL4E/KL3E by magic). */
static int decompress_payload(NkPspCtx *ctx, const uint8_t *payload,
                              size_t payload_size, int compressed,
                              uint32_t elf_size, uint8_t **out,
                              size_t *out_size)
{
    if (payload_size >= 2u && payload[0] == 0x1Fu && payload[1] == 0x8Bu) {
        char detail[128];
        uint8_t *d = NULL;
        size_t dlen = 0;
        int rc = nk_psp_inflate(payload, payload_size, &d, &dlen,
                                (size_t)elf_size, NK_DECOMP_CAP, detail,
                                sizeof(detail));
        if (rc != NK_INFLATE_OK) {
            return nk_psp_fail(ctx, NK_PSP_ERR_DECOMPRESS, NULL,
                               "gzip payload failed to decompress: %s",
                               detail[0] != '\0' ? detail : "malformed stream");
        }
        *out = d;
        *out_size = dlen;
        return NK_PSP_OK;
    }
    if (compressed) {
        int is_kl4e = 0;
        if (has_magic(payload, payload_size, "KL4E", 4)) {
            is_kl4e = 1;
        } else if (!has_magic(payload, payload_size, "KL3E", 4)) {
            return nk_psp_fail(ctx, NK_PSP_ERR_DECOMPRESS, NULL,
                               "compressed payload uses an unsupported "
                               "compression scheme");
        }
        if (elf_size < NK_ELF_MIN_SIZE || elf_size > NK_DECOMP_CAP) {
            return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                               "container declares an impossible compressed "
                               "size 0x%08X",
                               (unsigned)elf_size);
        }
        {
            uint8_t *d = (uint8_t *)malloc((size_t)elf_size);
            void *end = NULL;
            int rc;
            if (d == NULL) {
                return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                                   "out of memory decompressing payload");
            }
            /* The magic is 4 bytes; the stream may not read past the
             * decrypted payload we actually have. */
            rc = decompress_kle(d, (int)elf_size,
                                (uint8_t *)(void *)(payload + 4),
                                (int)(payload_size - 4u), &end, is_kl4e);
            if (rc <= 0) {
                free(d);
                return nk_psp_fail(ctx, NK_PSP_ERR_DECOMPRESS, NULL,
                                   "%s payload stream is malformed",
                                   is_kl4e ? "KL4E" : "KL3E");
            }
            *out = d;
            *out_size = (size_t)rc;
            return NK_PSP_OK;
        }
    }
    /* Uncompressed payload: take it as-is (caller owns a copy). */
    {
        uint8_t *d = (uint8_t *)malloc(payload_size);
        if (d == NULL) {
            return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                               "out of memory copying payload");
        }
        memcpy(d, payload, payload_size);
        *out = d;
        *out_size = payload_size;
        return NK_PSP_OK;
    }
}

int nk_container_decrypt(NkPspCtx *ctx, const uint8_t *data, size_t size,
                         uint8_t **out, size_t *out_size)
{
    View v;
    NkContainerInfo info;
    int rc;

    if (out == NULL || out_size == NULL) return NK_PSP_ERR_INTERNAL;
    *out = NULL;
    *out_size = 0;
    if (data == NULL || size == 0u) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL, "input is empty");
    }
    if (ctx == NULL) return NK_PSP_ERR_INTERNAL;

    v.data = data;
    v.size = size;
    v.wrappers = 0;
    rc = view_unwrap(ctx, &v);
    if (rc != NK_PSP_OK) return rc;

    rc = nk_container_probe(ctx, v.data, v.size, &info);
    if (rc != NK_PSP_OK) return rc;

    if (info.kind == NK_CTR_PLAIN_ELF) {
        uint8_t *copy = (uint8_t *)malloc(v.size);
        if (copy == NULL) {
            return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                               "out of memory copying ELF image");
        }
        memcpy(copy, v.data, v.size);
        *out = copy;
        *out_size = v.size;
        return NK_PSP_OK;
    }
    if (info.kind == NK_CTR_GZIP) {
        char detail[128];
        uint8_t *d = NULL;
        size_t dlen = 0;
        rc = nk_psp_inflate(v.data, v.size, &d, &dlen, 0u, NK_DECOMP_CAP,
                            detail, sizeof(detail));
        if (rc != NK_INFLATE_OK) {
            return nk_psp_fail(ctx, NK_PSP_ERR_DECOMPRESS, NULL,
                               "gzip input failed to decompress: %s",
                               detail[0] != '\0' ? detail : "malformed stream");
        }
        *out = d;
        *out_size = dlen;
        return NK_PSP_OK;
    }
    if (info.kind != NK_CTR_PSP) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "unrecognized container magic "
                           "(expected ~PSP, ~SCE, PBP, gzip or ELF)");
    }

    /* Encrypted (or plain) ~PSP module: validate, then decrypt. */
    {
        uint32_t psp_size = rd32(v.data + 0x2C);
        uint32_t elf_size = rd32(v.data + 0x28);
        int compressed = info.compressed;
        uint8_t *work;
        int decrypted_len;
        uint8_t *result = NULL;
        size_t result_size = 0;

        work = (uint8_t *)malloc((size_t)psp_size);
        if (work == NULL) {
            return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                               "out of memory decrypting container");
        }
        decrypted_len = pspDecryptPRX(ctx, v.data, work, psp_size, NULL);
        if (decrypted_len < 0) {
            free(work);
            return decrypted_len;
        }
        rc = decompress_payload(ctx, work, (size_t)decrypted_len, compressed,
                                elf_size, &result, &result_size);
        free(work);
        if (rc != NK_PSP_OK) return rc;

        if (result_size < 4u || memcmp(result, "\x7f"
                                                "ELF", 4) != 0) {
            free(result);
            return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                               "decrypted payload is not an ELF image");
        }
        *out = result;
        *out_size = result_size;
        return NK_PSP_OK;
    }
}
