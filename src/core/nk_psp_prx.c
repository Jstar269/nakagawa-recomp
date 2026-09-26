/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * ~PSP/PRX container decryption (issue #295).
 *
 * Ported from PrxDecrypter.cpp of John-K/pspdecrypt
 * (https://github.com/John-K/pspdecrypt), commit
 * c156627db7634d395c380c0a9589130f603307fc (no per-file header upstream).
 * That file in turn derives from PPSSPP's Core/ELF/PrxDecrypter.cpp:
 * Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
 * The pspdecrypt repository's LICENSE.TXT is the GNU GPL version 3 with no
 * later-version grant, so this port is labelled GPL-3.0-only.
 *
 * Modified for this project:
 *   - written as portable C99 (no C++ templates/arrays, explicit
 *     little-endian loads, packed header kept via the shared header);
 *   - EVERY embedded tag key table (the keysNNN/g_keyNNN arrays and the
 *     g_tagInfo/g_tagInfo2 dispatch tables) and the PSAR key constants are
 *     REMOVED; each tag recipe is fetched at run
 *     time from the local-only KeyStore entry "prx.tag.0xXXXXXXXX"
 *     (code/key/key144/seed/xor) -- see nk_psp_keystore.h;
 *   - all KIRK operations thread an NkPspCtx so a missing key entry fails
 *     closed with its exact name;
 *   - the caller-visible contract returns the decrypted payload length and
 *     fails closed with NK_PSP_ERR_* codes.
 *
 * No key material of any kind appears in this file.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "nk_psp_crypto.h"
#include "nk_psp_kirk.h"
#include "nk_psp_keystore.h"
#include "nk_psp_prx.h"
#include "nk_psp_sha1.h"

#define PSP_HEADER_SIZE 0x150
/* Reconstructed CMD1 header sits below the 0x150 header copy:
 * 0x150 - sizeof(KIRK_CMD1_HEADER 0x90) - prxHeader 0x80 = 0x40. */
#define TYPE_OFFSET 0x40
/* Internal: a tag recipe simply does not cover this container type. */
#define TYPE_SKIP (-1000)

/* ---------------------------- LE helpers ------------------------------ */

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) |
           ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static s32 rd32s(const u8 *p)
{
    return (s32)rd32(p);
}

/* --------------------------- tag recipe ------------------------------- */

typedef struct {
    const u8 *key144;    /* 144 bytes, type 0/1 (may be NULL) */
    const u8 *key;       /* 16 bytes, type 2/5/6 (may be NULL) */
    const u8 *recipe_seed; /* optional bonus seed (may be NULL) */
    const u8 *xorpad;    /* optional type-5 xor1 pad (may be NULL) */
    int code;            /* KIRK keyvault slot 0..127 */
    char name[24];       /* canonical KeyStore entry name */
} TagRecipe;

static int tag_recipe_load(NkPspCtx *ctx, const u8 *inbuf, TagRecipe *out)
{
    u32 tag = rd32(inbuf + 0xD0);
    NkPrxTagEntry ent;

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "prx.tag.0x%08X", (unsigned)tag);
    if (ctx->ks == NULL ||
        nk_keystore_get_prx_tag(ctx->ks, tag, &ent) != 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_MISSING_KEY, out->name,
                           "this executable needs key entry %s "
                           "(container tag 0x%08X); add it to the local key "
                           "file and retry",
                           out->name, (unsigned)tag);
    }
    if (!ent.have_code) {
        return nk_psp_fail(ctx, NK_PSP_ERR_KEYFILE, out->name,
                           "key entry %s must carry an integer \"code\" "
                           "(KIRK keyvault slot 0..127)",
                           out->name);
    }
    out->code = ent.code;
    out->key = ent.key;
    out->key144 = ent.key144;
    out->recipe_seed = ent.seed;
    out->xorpad = ent.xorpad;
    if (out->key == NULL && out->key144 == NULL) {
        /* The KeyStore schema already enforces this; keep the belt. */
        return nk_psp_fail(ctx, NK_PSP_ERR_KEYFILE, out->name,
                           "key entry %s needs a \"key\" or \"key144\" value",
                           out->name);
    }
    return NK_PSP_OK;
}

/* ------------------------- kirk wrappers ------------------------------ */

/* Map a KIRK/negative return code onto the shared error contract. */
static int map_kirk_rc(NkPspCtx *ctx, int rc)
{
    if (rc == 0) return NK_PSP_OK;
    if (rc < 0) return rc; /* already an NK_PSP_ERR_* recorded in ctx */
    if (ctx->missing_entry[0] != '\0') return NK_PSP_ERR_MISSING_KEY;
    switch (rc) {
    case KIRK_INVALID_SIZE:
    case KIRK_INVALID_MODE:
    case KIRK_DATA_SIZE_ZERO:
        return NK_PSP_ERR_FORMAT;
    case KIRK_HEADER_HASH_INVALID:
    case KIRK_DATA_HASH_INVALID:
    case KIRK_SIG_CHECK_INVALID:
    default:
        return NK_PSP_ERR_INTEGRITY;
    }
}

/*
 * expandSeed(): counter-pattern ("AES-CTR like") copy of the 16-byte tag key
 * run through the keyvault pass, optionally XORed with a bonus seed.
 */
static int expand_seed(NkPspCtx *ctx, u8 out[0x90], const u8 *seed16,
                       int code, const u8 *bonus)
{
    u32 i;
    for (i = 0; i < 0x90; i += 0x10) {
        memcpy(out + i, seed16, 0x10);
        out[i] = (u8)(i >> 4);
    }
    if (kirk7(ctx, out, out, 0x90, code) != KIRK_OPERATION_SUCCESS) {
        int rc = ctx->missing_entry[0] != '\0'
                     ? NK_PSP_ERR_MISSING_KEY
                     : NK_PSP_ERR_INTEGRITY;
        if (rc == NK_PSP_ERR_INTEGRITY && ctx->message[0] == '\0') {
            snprintf(ctx->message, sizeof(ctx->message),
                     "keyvault pass failed for slot %d", code);
        }
        return rc;
    }
    if (bonus != NULL) {
        for (i = 0; i < 0x90; i++) out[i] ^= bonus[i % 0x10];
    }
    return NK_PSP_OK;
}

/* Type 2/5/6 header scramble: XOR with xorbuf[0x10..], keyvault pass,
 * XOR with xorbuf[0x50..] (the iterator runs on across both passes). */
static int decrypt_kirk_header(NkPspCtx *ctx, u8 *outbuf, const u8 *inbuf,
                               const u8 *xorbuf, int code)
{
    u8 tmp[0x40];
    int i;
    for (i = 0; i < 0x40; i++) tmp[i] = inbuf[i] ^ xorbuf[i];
    if (kirk7(ctx, tmp, tmp, 0x40, code) != KIRK_OPERATION_SUCCESS) {
        return ctx->missing_entry[0] != '\0' ? NK_PSP_ERR_MISSING_KEY
                                             : NK_PSP_ERR_INTEGRITY;
    }
    for (i = 0; i < 0x40; i++) outbuf[i] = tmp[i] ^ xorbuf[0x40 + i];
    return NK_PSP_OK;
}

/* Type 0/1 header scramble over the pre-decrypted 144-byte buffer:
 * XOR with xorbuf[0x14..], keyvault pass, XOR with xorbuf[0x20..]. */
static int decrypt_kirk_header_type0(NkPspCtx *ctx, u8 *outbuf,
                                     const u8 *inbuf, const u8 *xorbuf,
                                     int code)
{
    u8 tmp[0x70];
    int i;
    for (i = 0; i < 0x70; i++) tmp[i] = inbuf[i] ^ xorbuf[i + 0x14];
    if (kirk7(ctx, tmp, tmp, 0x70, code) != KIRK_OPERATION_SUCCESS) {
        return ctx->missing_entry[0] != '\0' ? NK_PSP_ERR_MISSING_KEY
                                             : NK_PSP_ERR_INTEGRITY;
    }
    for (i = 0; i < 0x70; i++) outbuf[i] = tmp[i] ^ xorbuf[i + 0x20];
    return NK_PSP_OK;
}

/* ------------------------- header views ------------------------------- */

/* Upstream PRXType0/1/2/5/6 overlays, laid out sequentially (u8 arrays, no
 * padding) so each view is exactly 0x150 bytes like the file header. */

typedef struct {
    u8 tag[4];          /* file 0xD0 */
    u8 sha1[0x14];      /* file 0xD4 */
    u8 unused[0x28];    /* file 0xE8 */
    u8 kirkBlock[0x90]; /* file 0x110 (0x40) + file 0x80 (0x50) */
    u8 prxHeader[0x80]; /* file 0x00 */
} PRXType0;

typedef struct {
    u8 tag[4];
    u8 sha1[0x14];
    u8 unused[0x28];
    u8 kirkBlock[0x90];
    u8 prxHeader[0x80];
} PRXType1;

typedef struct {
    u8 tag[4];            /* file 0xD0 */
    u8 empty[0x58];       /* zeros (file 0xD4 region is not hashed) */
    u8 id[0x10];          /* file 0x140 */
    u8 sha1[0x14];        /* file 0x12C */
    u8 kirkHeader[0x40];  /* file 0x80 (0x30) + file 0xC0 (0x10) */
    u8 kirkMetadata[0x10];/* file 0xB0 */
    u8 prxHeader[0x80];   /* file 0x00 */
} PRXType2;

typedef struct {
    u8 tag[4];
    u8 empty[0x58];
    u8 id[0x10];
    u8 sha1[0x14];
    u8 kirkHeader[0x40];
    u8 kirkMetadata[0x10];
    u8 prxHeader[0x80];
} PRXType5;

typedef struct {
    u8 tag[4];               /* file 0xD0 */
    u8 empty[0x38];          /* zeros */
    u8 ecdsaSignatureTail[0x20]; /* file 0x10C */
    u8 id[0x10];             /* file 0x140 */
    u8 sha1[0x14];           /* file 0x12C */
    u8 kirkHeader[0x40];
    u8 kirkMetadata[0x10];
    u8 prxHeader[0x80];
} PRXType6;

typedef char prx_type0_size_check[(sizeof(PRXType0) == 0x150) ? 1 : -1];
typedef char prx_type2_size_check[(sizeof(PRXType2) == 0x150) ? 1 : -1];
typedef char prx_type5_size_check[(sizeof(PRXType5) == 0x150) ? 1 : -1];
typedef char prx_type6_size_check[(sizeof(PRXType6) == 0x150) ? 1 : -1];

static void prx_type01_init(void *view, const u8 *prx, int type1)
{
    PRXType1 *t = (PRXType1 *)view;
    (void)type1;
    memcpy(t->tag, prx + 0xD0, sizeof(t->tag));
    memcpy(t->sha1, prx + 0xD4, sizeof(t->sha1));
    memcpy(t->unused, prx + 0xE8, sizeof(t->unused));
    memcpy(t->kirkBlock, prx + 0x110, 0x40);
    memcpy(t->kirkBlock + 0x40, prx + 0x80, sizeof(t->kirkBlock) - 0x40);
    memcpy(t->prxHeader, prx, sizeof(t->prxHeader));
}

static void prx_type25_init(void *view, const u8 *prx)
{
    PRXType2 *t = (PRXType2 *)view;
    memcpy(t->tag, prx + 0xD0, sizeof(t->tag));
    memset(t->empty, 0, sizeof(t->empty));
    memcpy(t->id, prx + 0x140, sizeof(t->id));
    memcpy(t->sha1, prx + 0x12C, sizeof(t->sha1));
    memcpy(t->kirkHeader, prx + 0x80, sizeof(t->kirkHeader) - 0x10);
    memcpy(t->kirkHeader + 0x30, prx + 0xC0, 0x10);
    memcpy(t->kirkMetadata, prx + 0xB0, sizeof(t->kirkMetadata));
    memcpy(t->prxHeader, prx, sizeof(t->prxHeader));
}

static void prx_type6_init(PRXType6 *t, const u8 *prx)
{
    memcpy(t->tag, prx + 0xD0, sizeof(t->tag));
    memset(t->empty, 0, sizeof(t->empty));
    memcpy(t->ecdsaSignatureTail, prx + 0x10C, sizeof(t->ecdsaSignatureTail));
    memcpy(t->id, prx + 0x140, sizeof(t->id));
    memcpy(t->sha1, prx + 0x12C, sizeof(t->sha1));
    memcpy(t->kirkHeader, prx + 0x80, sizeof(t->kirkHeader) - 0x10);
    memcpy(t->kirkHeader + 0x30, prx + 0xC0, 0x10);
    memcpy(t->kirkMetadata, prx + 0xB0, sizeof(t->kirkMetadata));
    memcpy(t->prxHeader, prx, sizeof(t->prxHeader));
}

/* -------------------------- SHA-1 helper ------------------------------ */

typedef struct {
    const u8 *p;
    size_t n;
} ShaPart;

static void sha1_parts(const ShaPart *parts, size_t count, u8 out[0x14])
{
    SHA_CTX shactx;
    size_t i;
    SHAInit(&shactx);
    for (i = 0; i < count; i++) {
        if (parts[i].n != 0) {
            SHAUpdate(&shactx, (BYTE *)(void *)parts[i].p, (int)parts[i].n);
        }
    }
    SHAFinal(out, &shactx);
}

/* ------------------------- type decoders ------------------------------ */

static int finish_cmd1(NkPspCtx *ctx, u8 *outbuf, u8 *header, u32 size,
                       s32 decrypt_size)
{
    int rc = sceUtilsBufferCopyWithRange(ctx, outbuf, (int)size, header,
                                         (int)(size - TYPE_OFFSET),
                                         KIRK_CMD_DECRYPT_PRIVATE);
    if (rc != 0) return map_kirk_rc(ctx, rc);
    if (decrypt_size <= 0 || (u32)decrypt_size > size) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "container declares an impossible payload size");
    }
    return (int)decrypt_size;
}

static int psp_decrypt_type0(NkPspCtx *ctx, const TagRecipe *r,
                             const u8 *inbuf, u8 *outbuf, u32 size)
{
    PRXType0 t;
    u8 digest[0x14];
    u8 *header;
    s32 decrypt_size;
    int rc;

    if (r->key144 == NULL) return TYPE_SKIP;
    decrypt_size = rd32s(inbuf + 0xB0);
    prx_type01_init(&t, inbuf, 0);

    /* Type 0: SHA-1 over the pre-decrypted 144-byte buffer + views. */
    {
        ShaPart parts[4] = {
            {r->key144, 0x14},
            {t.unused, sizeof(t.unused)},
            {t.kirkBlock, sizeof(t.kirkBlock)},
            {t.prxHeader, sizeof(t.prxHeader)},
        };
        sha1_parts(parts, 4, digest);
    }
    if (memcmp(digest, t.sha1, 0x14) != 0) return NK_PSP_ERR_INTEGRITY;
    if (decrypt_size <= 0 || (u32)decrypt_size > size) return NK_PSP_ERR_FORMAT;

    if (outbuf != inbuf) memcpy(outbuf, inbuf, size);
    header = outbuf + TYPE_OFFSET;
    memcpy(header, t.kirkBlock, sizeof(t.kirkBlock));
    memcpy(header + sizeof(t.kirkBlock), t.prxHeader, sizeof(t.prxHeader));
    rc = decrypt_kirk_header_type0(ctx, header, t.kirkBlock, r->key144,
                                   r->code);
    if (rc != NK_PSP_OK) return rc;
    return finish_cmd1(ctx, outbuf, header, size, decrypt_size);
}

static int psp_decrypt_type1(NkPspCtx *ctx, const TagRecipe *r,
                             const u8 *inbuf, u8 *outbuf, u32 size)
{
    PRXType1 t;
    u8 digest[0x14];
    u8 *header;
    s32 decrypt_size;
    int rc;

    if (r->key144 == NULL) return TYPE_SKIP;
    decrypt_size = rd32s(inbuf + 0xB0);
    prx_type01_init(&t, inbuf, 1);
    /* Type 1 first keyvault pass: 0xA0 bytes from sha1+0xC. */
    if (kirk7(ctx, t.sha1 + 0xC, t.sha1 + 0xC, 0xA0, r->code) !=
        KIRK_OPERATION_SUCCESS) {
        return ctx->missing_entry[0] != '\0' ? NK_PSP_ERR_MISSING_KEY
                                             : NK_PSP_ERR_INTEGRITY;
    }

    {
        ShaPart parts[4] = {
            {r->key144, 0x14},
            {t.unused, sizeof(t.unused)},
            {t.kirkBlock, sizeof(t.kirkBlock)},
            {t.prxHeader, sizeof(t.prxHeader)},
        };
        sha1_parts(parts, 4, digest);
    }
    if (memcmp(digest, t.sha1, 0x14) != 0) return NK_PSP_ERR_INTEGRITY;
    if (decrypt_size <= 0 || (u32)decrypt_size > size) return NK_PSP_ERR_FORMAT;

    if (outbuf != inbuf) memcpy(outbuf, inbuf, size);
    header = outbuf + TYPE_OFFSET;
    memcpy(header, t.kirkBlock, sizeof(t.kirkBlock));
    memcpy(header + sizeof(t.kirkBlock), t.prxHeader, sizeof(t.prxHeader));
    rc = decrypt_kirk_header_type0(ctx, header, t.kirkBlock, r->key144,
                                   r->code);
    if (rc != NK_PSP_OK) return rc;
    return finish_cmd1(ctx, outbuf, header, size, decrypt_size);
}

static int psp_decrypt_type2(NkPspCtx *ctx, const TagRecipe *r,
                             const u8 *inbuf, u8 *outbuf, u32 size)
{
    PRXType2 t;
    u8 xorbuf[0x90];
    u8 digest[0x14];
    u8 *header;
    s32 decrypt_size;
    int rc;

    if (r->key == NULL) return TYPE_SKIP;
    decrypt_size = rd32s(inbuf + 0xB0);
    rc = expand_seed(ctx, xorbuf, r->key, r->code, NULL);
    if (rc != NK_PSP_OK) return rc;

    prx_type25_init(&t, inbuf);
    /* The 0x60 keyvault pass spans id (0x10) + sha1 (0x14) + the first
     * 0x3C of kirkHeader, laid out contiguously in the view. */
    {
        u8 *span = (u8 *)&t + offsetof(PRXType2, id);
        if (kirk7(ctx, span, span, 0x60, r->code) != KIRK_OPERATION_SUCCESS) {
            return ctx->missing_entry[0] != '\0' ? NK_PSP_ERR_MISSING_KEY
                                                 : NK_PSP_ERR_INTEGRITY;
        }
    }

    {
        ShaPart parts[7] = {
            {t.tag, sizeof(t.tag)},
            {xorbuf, 0x10},
            {t.empty, sizeof(t.empty)},
            {t.id, sizeof(t.id)},
            {t.kirkHeader, sizeof(t.kirkHeader)},
            {t.kirkMetadata, sizeof(t.kirkMetadata)},
            {t.prxHeader, sizeof(t.prxHeader)},
        };
        sha1_parts(parts, 7, digest);
    }
    if (memcmp(digest, t.sha1, 0x14) != 0) return NK_PSP_ERR_INTEGRITY;
    if (decrypt_size <= 0 || (u32)decrypt_size > size) return NK_PSP_ERR_FORMAT;

    if (outbuf != inbuf) memcpy(outbuf, inbuf, size);
    header = outbuf + TYPE_OFFSET;
    memset(header, 0, sizeof(KIRK_CMD1_HEADER));
    memcpy(header + 0x70, t.kirkMetadata, sizeof(t.kirkMetadata));
    memcpy(header + sizeof(KIRK_CMD1_HEADER), t.prxHeader,
           sizeof(t.prxHeader));
    rc = decrypt_kirk_header(ctx, header, t.kirkHeader, xorbuf + 0x10, r->code);
    if (rc != NK_PSP_OK) return rc;
    ((KIRK_CMD1_HEADER *)header)->mode = KIRK_MODE_CMD1;
    return finish_cmd1(ctx, outbuf, header, size, decrypt_size);
}

static int psp_decrypt_type5(NkPspCtx *ctx, const TagRecipe *r,
                             const u8 *inbuf, u8 *outbuf, u32 size,
                             const u8 *caller_seed)
{
    PRXType5 t;
    u8 xorbuf[0x90];
    u8 digest[0x14];
    u8 *header;
    const u8 *bonus = caller_seed != NULL ? caller_seed : r->recipe_seed;
    const u8 *xor1 = r->xorpad;
    u8 data[0x50];
    s32 decrypt_size;
    int i, rc;

    if (r->key == NULL) return TYPE_SKIP;
    decrypt_size = rd32s(inbuf + 0xB0);
    rc = expand_seed(ctx, xorbuf, r->key, r->code, bonus);
    if (rc != NK_PSP_OK) return rc;

    prx_type25_init(&t, inbuf);

    /* Step 1: scramble + keyvault pass over kirkHeader + sha1 prefix. */
    memcpy(data, t.kirkHeader, 0x40);
    memcpy(data + 0x40, t.sha1, 0x10);
    for (i = 0; i < 0x50; i++) {
        if (xor1 != NULL) data[i] ^= xor1[i % 0x10];
        if (bonus != NULL) data[i] ^= bonus[i % 0x10];
    }
    if (kirk7(ctx, data, data, 0x50, r->code) != KIRK_OPERATION_SUCCESS) {
        return ctx->missing_entry[0] != '\0' ? NK_PSP_ERR_MISSING_KEY
                                             : NK_PSP_ERR_INTEGRITY;
    }
    memcpy(t.kirkHeader, data, 0x40);
    memcpy(t.sha1, data + 0x40, 0x10);

    /* Step 2: XOR then keyvault pass chaining id into the kirk header.
     * The span is id (0x10) + sha1 (0x14) + the first 0x3C of
     * kirkHeader, contiguous in the view. */
    {
        u8 *span = (u8 *)&t + offsetof(PRXType5, id);
        if (xor1 != NULL) {
            for (i = 0; i < 0x60; i++) span[i] ^= xor1[i % 0x10];
        }
        if (kirk7(ctx, span, span, 0x60, r->code) != KIRK_OPERATION_SUCCESS) {
            return ctx->missing_entry[0] != '\0' ? NK_PSP_ERR_MISSING_KEY
                                                 : NK_PSP_ERR_INTEGRITY;
        }
    }

    {
        ShaPart parts[7] = {
            {t.tag, sizeof(t.tag)},
            {xorbuf, 0x10},
            {t.empty, sizeof(t.empty)},
            {t.id, sizeof(t.id)},
            {t.kirkHeader, sizeof(t.kirkHeader)},
            {t.kirkMetadata, sizeof(t.kirkMetadata)},
            {t.prxHeader, sizeof(t.prxHeader)},
        };
        sha1_parts(parts, 7, digest);
    }
    if (memcmp(digest, t.sha1, 0x14) != 0) return NK_PSP_ERR_INTEGRITY;
    if (decrypt_size <= 0 || (u32)decrypt_size > size) return NK_PSP_ERR_FORMAT;

    if (outbuf != inbuf) memcpy(outbuf, inbuf, size);
    header = outbuf + TYPE_OFFSET;
    memset(header, 0, sizeof(KIRK_CMD1_HEADER));
    memcpy(header + 0x70, t.kirkMetadata, sizeof(t.kirkMetadata));
    memcpy(header + sizeof(KIRK_CMD1_HEADER), t.prxHeader,
           sizeof(t.prxHeader));
    rc = decrypt_kirk_header(ctx, header, t.kirkHeader, xorbuf + 0x10, r->code);
    if (rc != NK_PSP_OK) return rc;
    ((KIRK_CMD1_HEADER *)header)->mode = KIRK_MODE_CMD1;
    return finish_cmd1(ctx, outbuf, header, size, decrypt_size);
}

static int psp_decrypt_type6(NkPspCtx *ctx, const TagRecipe *r,
                             const u8 *inbuf, u8 *outbuf, u32 size)
{
    PRXType6 t;
    u8 xorbuf[0x90];
    u8 digest[0x14];
    u8 *header;
    s32 decrypt_size;
    int rc;

    if (r->key == NULL) return TYPE_SKIP;
    decrypt_size = rd32s(inbuf + 0xB0);
    rc = expand_seed(ctx, xorbuf, r->key, r->code, NULL);
    if (rc != NK_PSP_OK) return rc;

    prx_type6_init(&t, inbuf);
    {
        u8 *span = (u8 *)&t + offsetof(PRXType6, id);
        if (kirk7(ctx, span, span, 0x60, r->code) != KIRK_OPERATION_SUCCESS) {
            return ctx->missing_entry[0] != '\0' ? NK_PSP_ERR_MISSING_KEY
                                                 : NK_PSP_ERR_INTEGRITY;
        }
    }

    {
        ShaPart parts[8] = {
            {t.tag, sizeof(t.tag)},
            {xorbuf, 0x10},
            {t.empty, sizeof(t.empty)},
            {t.ecdsaSignatureTail, sizeof(t.ecdsaSignatureTail)},
            {t.id, sizeof(t.id)},
            {t.kirkHeader, sizeof(t.kirkHeader)},
            {t.kirkMetadata, sizeof(t.kirkMetadata)},
            {t.prxHeader, sizeof(t.prxHeader)},
        };
        sha1_parts(parts, 8, digest);
    }
    if (memcmp(digest, t.sha1, 0x14) != 0) return NK_PSP_ERR_INTEGRITY;
    if (decrypt_size <= 0 || (u32)decrypt_size > size) return NK_PSP_ERR_FORMAT;

    if (outbuf != inbuf) memcpy(outbuf, inbuf, size);
    header = outbuf + TYPE_OFFSET;
    memset(header, 0, sizeof(KIRK_CMD1_ECDSA_HEADER));
    memcpy(header + 0x40, t.ecdsaSignatureTail, sizeof(t.ecdsaSignatureTail));
    memcpy(header + 0x70, t.kirkMetadata, sizeof(t.kirkMetadata));
    memcpy(header + sizeof(KIRK_CMD1_ECDSA_HEADER), t.prxHeader,
           sizeof(t.prxHeader));
    rc = decrypt_kirk_header(ctx, header, t.kirkHeader, xorbuf + 0x10, r->code);
    if (rc != NK_PSP_OK) return rc;
    ((KIRK_CMD1_ECDSA_HEADER *)header)->mode = KIRK_MODE_CMD1;
    ((KIRK_CMD1_ECDSA_HEADER *)header)->ecdsa_hash = 1;
    return finish_cmd1(ctx, outbuf, header, size, decrypt_size);
}

/* --------------------------- entry point ------------------------------ */

int pspDecryptPRX(NkPspCtx *ctx, const u8 *inbuf, u8 *outbuf,
                  u32 size, const u8 *seed)
{
    TagRecipe recipe;
    int have_missing = 0, have_integrity = 0, have_format = 0;
    int krc, rc;

    if (ctx == NULL || inbuf == NULL || outbuf == NULL) {
        return NK_PSP_ERR_INTERNAL;
    }
    ctx->missing_entry[0] = '\0';
    ctx->message[0] = '\0';
    if (size < PSP_HEADER_SIZE) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                           "container is too small for a ~PSP header");
    }

    /* The tag recipe lookup comes first so an absent key file reports the
     * exact entry this executable needs. */
    rc = tag_recipe_load(ctx, inbuf, &recipe);
    if (rc != NK_PSP_OK) return rc;

    krc = kirk_init(ctx);
    if (krc != KIRK_OPERATION_SUCCESS) {
        if (krc == KIRK_NOT_INITIALIZED) return NK_PSP_ERR_MISSING_KEY;
        if (krc < 0) return krc;
        return NK_PSP_ERR_KEYFILE;
    }

    /* The decoder does not know the container's tag/type mapping, so it
     * tries every type the recipe carries material for (upstream order:
     * 0, 1, 2, 5, 6); the per-type SHA-1 check selects the right one. */
    if (recipe.key144 != NULL) {
        rc = psp_decrypt_type0(ctx, &recipe, inbuf, outbuf, size);
        if (rc >= 0) return rc;
        if (rc == NK_PSP_ERR_MISSING_KEY) have_missing = 1;
        else if (rc == NK_PSP_ERR_INTEGRITY) have_integrity = 1;
        else if (rc == NK_PSP_ERR_FORMAT) have_format = 1;

        rc = psp_decrypt_type1(ctx, &recipe, inbuf, outbuf, size);
        if (rc >= 0) return rc;
        if (rc == NK_PSP_ERR_MISSING_KEY) have_missing = 1;
        else if (rc == NK_PSP_ERR_INTEGRITY) have_integrity = 1;
        else if (rc == NK_PSP_ERR_FORMAT) have_format = 1;
    }
    if (recipe.key != NULL) {
        rc = psp_decrypt_type2(ctx, &recipe, inbuf, outbuf, size);
        if (rc >= 0) return rc;
        if (rc == NK_PSP_ERR_MISSING_KEY) have_missing = 1;
        else if (rc == NK_PSP_ERR_INTEGRITY) have_integrity = 1;
        else if (rc == NK_PSP_ERR_FORMAT) have_format = 1;

        rc = psp_decrypt_type5(ctx, &recipe, inbuf, outbuf, size, seed);
        if (rc >= 0) return rc;
        if (rc == NK_PSP_ERR_MISSING_KEY) have_missing = 1;
        else if (rc == NK_PSP_ERR_INTEGRITY) have_integrity = 1;
        else if (rc == NK_PSP_ERR_FORMAT) have_format = 1;

        rc = psp_decrypt_type6(ctx, &recipe, inbuf, outbuf, size);
        if (rc >= 0) return rc;
        if (rc == NK_PSP_ERR_MISSING_KEY) have_missing = 1;
        else if (rc == NK_PSP_ERR_INTEGRITY) have_integrity = 1;
        else if (rc == NK_PSP_ERR_FORMAT) have_format = 1;
    }

    /* Fail-closed aggregation, most specific diagnosis first.  ctx already
     * carries the exact missing entry / the first recorded message. */
    if (have_missing) return NK_PSP_ERR_MISSING_KEY;
    if (have_integrity) {
        if (ctx->message[0] == '\0') {
            snprintf(ctx->message, sizeof(ctx->message),
                     "integrity checks failed for key entry %s "
                     "(wrong key entry values, or a modified container)",
                     recipe.name);
        }
        return NK_PSP_ERR_INTEGRITY;
    }
    if (have_format) {
        if (ctx->message[0] == '\0') {
            snprintf(ctx->message, sizeof(ctx->message),
                     "container fields are inconsistent with key entry %s",
                     recipe.name);
        }
        return NK_PSP_ERR_FORMAT;
    }
    return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, recipe.name,
                       "no enabled decryption path matched key entry %s",
                       recipe.name);
}
