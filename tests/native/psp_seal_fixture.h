/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Synthetic sealers for native tests (issue #295).
 *
 * psp_seal_type2 is the inverse of the production type-2 path: it wraps a
 * test-owned payload in a ~PSP container that nk_container_decrypt opens with
 * the given tag recipe, keyvault row and KIRK command-1 key. The fuzz harness
 * passes constant fake keys; the player test passes the built-in table's
 * values for one tag, read from the linked table at run time.
 *
 * psp_seal_npumdimg_pbp (phase 2) packs a test-owned ISO image as a PSN UMD
 * image (an EBOOT.PBP whose DATA.PSAR is NPUMDIMG), through the production
 * DRM BB calls and the KeyStore it is given: the cipher is an XOR keystream,
 * so encrypting is the same call, and the type-3 BB MAC is wrapped with KIRK
 * command 4 under rows 0x38 and 0x63. Blocks are stored uncompressed.
 *
 * Header-only so each test binary links nothing extra. Mirrors
 * tools/test_psp_decrypt.py's sealers. */

#ifndef NK_TEST_PSP_SEAL_FIXTURE_H
#define NK_TEST_PSP_SEAL_FIXTURE_H

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nk_psp_aes.h"
#include "nk_psp_amctrl.h"
#include "nk_psp_kirk.h"
#include "nk_psp_sha1.h"

static void psp_seal_put16le(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void psp_seal_put32le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* Seal payload under tag. tag_key is the recipe's 16-byte key, vault_key the
 * keyvault row its code names, cmd1_key the KIRK command-1 key. Returns the
 * container size (0x150 + payload rounded up to 16). */
static size_t psp_seal_type2(uint8_t *out, size_t cap, const uint8_t *payload,
                             size_t payload_len, int compressed, uint32_t elf_size,
                             uint32_t tag, const uint8_t tag_key[16],
                             const uint8_t vault_key[16], const uint8_t cmd1_key[16]) {
    uint8_t a_key[16], c_key[16];
    uint8_t ctr[0x90], xorbuf[0x90], clear[0x40], t_kh[0x40], chain[0x60];
    uint8_t hdr_tail[0x30], metadata[0x10], digest[0x14], sha_input[0x150];
    uint8_t *cmac_input;
    size_t data_offset = 0x80u;
    size_t chk_size = (payload_len + 15u) & ~(size_t)15u;
    size_t container_size = 0x150u + chk_size;
    size_t at;
    AES_ctx ctx;
    SHA_CTX sha;

    assert(cap >= container_size);
    for (unsigned i = 0; i < 16u; i++) {
        a_key[i] = (uint8_t)(tag_key[i] ^ 0xA5u);
        c_key[i] = (uint8_t)(tag_key[i] ^ 0x5Au);
    }

    /* expandSeed(): counter-styled pattern through the keyvault pass. */
    for (unsigned n = 0; n < 0x90u / 16u; n++) {
        memcpy(ctr + n * 16u, tag_key, 16u);
        ctr[n * 16u] = (uint8_t)n;
    }
    AES_set_key(&ctx, vault_key, 128);
    AES_cbc_decrypt(&ctx, ctr, xorbuf, (int)sizeof(xorbuf));

    /* The 0x80-byte ~PSP header copy (CMD1 pre-data and prxHeader). */
    memset(out, 0, container_size);
    memcpy(out, "~PSP", 4);
    psp_seal_put16le(out + 4, 0x0200u);
    psp_seal_put16le(out + 6, (uint16_t)(compressed ? 1u : 0u));
    memcpy(out + 0x0A, "synthetic", 9);
    out[0x26] = 1u;
    out[0x27] = 2u; /* nsegments */
    psp_seal_put32le(out + 0x28, elf_size);
    psp_seal_put32le(out + 0x2C, (uint32_t)container_size);
    psp_seal_put32le(out + 0x30, 0x08804000u + 0x54u);
    psp_seal_put32le(out + 0x38, 0x100u);
    psp_seal_put32le(out + 0x3C, 0x10u);
    psp_seal_put32le(out + 0x40, 0x10u);
    psp_seal_put32le(out + 0x44, 0x08804000u);
    psp_seal_put32le(out + 0x48, 0x08809000u);
    psp_seal_put32le(out + 0x54, elf_size);
    psp_seal_put32le(out + 0x58, 0x100u);
    psp_seal_put32le(out + 0x78, 0x06060000u);

    /* CMD1 header tail at 0x60: mode, ecdsa_hash, sizes and the padding. */
    memset(hdr_tail, 0, sizeof(hdr_tail));
    psp_seal_put32le(hdr_tail, 1u); /* KIRK_MODE_CMD1 */
    psp_seal_put32le(hdr_tail + 0x10, (uint32_t)payload_len);
    psp_seal_put32le(hdr_tail + 0x14, (uint32_t)data_offset);
    memset(metadata, 0, sizeof(metadata)); /* all 16 bytes enter the CMAC-covered header */
    psp_seal_put32le(metadata, (uint32_t)payload_len);
    psp_seal_put32le(metadata + 4u, (uint32_t)data_offset);

    /* Wrapped CMD1 pre-data: the payload under a_key, then the 0x40 block. */
    AES_set_key(&ctx, a_key, 128);
    AES_cbc_encrypt(&ctx, payload, out + 0x150u, (int)chk_size);
    cmac_input = (uint8_t *)malloc(0x30u + 0x80u + chk_size);
    assert(cmac_input != NULL);
    memcpy(cmac_input, hdr_tail, 0x30u);
    memcpy(cmac_input + 0x30u, out, 0x80u);
    memcpy(cmac_input + 0xB0u, out + 0x150u, chk_size);
    AES_set_key(&ctx, c_key, 128);
    AES_CMAC(&ctx, cmac_input, (int)(0x30u + 0x80u + chk_size), clear + 0x30u);
    AES_CMAC(&ctx, hdr_tail, (int)sizeof(hdr_tail), clear + 0x20u);
    free(cmac_input);
    {
        uint8_t wrap_in[0x20], wrap_out[0x20];
        memcpy(wrap_in, a_key, 16u);
        memcpy(wrap_in + 16u, c_key, 16u);
        AES_set_key(&ctx, cmd1_key, 128);
        AES_cbc_encrypt(&ctx, wrap_in, wrap_out, (int)sizeof(wrap_in));
        memcpy(clear, wrap_out, 0x20u);
    }

    /* Inverse of the production header transform: out = kirk7(in ^ xa) ^ xb. */
    for (unsigned i = 0; i < 0x40u; i++) {
        t_kh[i] = (uint8_t)(clear[i] ^ xorbuf[0x50u + i]);
    }
    AES_set_key(&ctx, vault_key, 128);
    AES_cbc_encrypt(&ctx, t_kh, t_kh, (int)sizeof(t_kh));
    for (unsigned i = 0; i < 0x40u; i++) {
        t_kh[i] = (uint8_t)(t_kh[i] ^ xorbuf[0x10u + i]);
    }

    /* Integrity digest over the type-2 view, in the production part order. */
    memset(sha_input, 0, sizeof(sha_input));
    at = 0u;
    psp_seal_put32le(sha_input + at, tag);
    at += 4u;
    memcpy(sha_input + at, xorbuf, 0x10u);
    at += 0x10u;
    at += 0x58u + 0x10u; /* the zeroed empty region and the zeroed id */
    memcpy(sha_input + at, t_kh, 0x40u);
    at += 0x40u;
    memcpy(sha_input + at, metadata, 0x10u);
    at += 0x10u;
    memcpy(sha_input + at, out, 0x80u);
    at += 0x80u;
    assert(at == 0x14Cu);
    SHAInit(&sha);
    SHAUpdate(&sha, sha_input, (int)at);
    SHAFinal(digest, &sha);

    /* The chained keyvault pass covers {id, sha1, kirkHeader[0:0x3C]}. */
    memset(chain, 0, sizeof(chain));
    memcpy(chain + 0x10u, digest, 0x14u);
    memcpy(chain + 0x24u, t_kh, 0x3Cu);
    AES_set_key(&ctx, vault_key, 128);
    AES_cbc_encrypt(&ctx, chain, chain, (int)sizeof(chain));

    memcpy(out + 0x80u, chain + 0x24u, 0x30u);
    memcpy(out + 0xB0u, metadata, 0x10u);
    memcpy(out + 0xC0u, chain + 0x54u, 0x0Cu);
    memcpy(out + 0xCCu, t_kh + 0x3Cu, 0x04u);
    psp_seal_put32le(out + 0xD0, tag);
    memcpy(out + 0x12Cu, chain + 0x10u, 0x14u);
    memcpy(out + 0x140u, chain, 0x10u);
    psp_seal_put32le(out + 0x2C, (uint32_t)container_size);
    return container_size;
}

/* ---- PSN UMD image (NPUMDIMG) inside a PBP ------------------------------ */

/* A PARAM.SFO with CATEGORY, DISC_ID and TITLE (sorted keys, string values). */
static size_t psp_seal_sfo(uint8_t *out, size_t cap, const char *category,
                           const char *disc_id, const char *title) {
    const char *keys[3] = { "CATEGORY", "DISC_ID", "TITLE" };
    const char *values[3];
    size_t key_bytes = 0, data_bytes = 0, key_start = 20u + 3u * 16u, data_start;
    size_t key_len[3], value_len[3];
    values[0] = category;
    values[1] = disc_id;
    values[2] = title;
    for (unsigned i = 0; i < 3u; i++) {
        key_len[i] = strlen(keys[i]) + 1u;
        value_len[i] = strlen(values[i]) + 1u;
        key_bytes += key_len[i];
    }
    data_start = (key_start + key_bytes + 3u) & ~(size_t)3u;
    for (unsigned i = 0; i < 3u; i++) data_bytes += (value_len[i] + 3u) & ~(size_t)3u;
    assert(data_start + data_bytes <= cap);
    memset(out, 0, data_start + data_bytes);
    memcpy(out, "\0PSF", 4);
    psp_seal_put32le(out + 4, 0x0101u);
    psp_seal_put32le(out + 8, (uint32_t)key_start);
    psp_seal_put32le(out + 12, (uint32_t)data_start);
    psp_seal_put32le(out + 16, 3u);
    {
        size_t k = 0, d = 0;
        for (unsigned i = 0; i < 3u; i++) {
            uint8_t *entry = out + 20u + i * 16u;
            psp_seal_put16le(entry, (uint16_t)k);
            psp_seal_put16le(entry + 2, 0x0204u);
            psp_seal_put32le(entry + 4, (uint32_t)value_len[i]);
            psp_seal_put32le(entry + 8, (uint32_t)((value_len[i] + 3u) & ~(size_t)3u));
            psp_seal_put32le(entry + 12, (uint32_t)d);
            memcpy(out + key_start + k, keys[i], key_len[i]);
            memcpy(out + data_start + d, values[i], value_len[i]);
            k += key_len[i];
            d += (value_len[i] + 3u) & ~(size_t)3u;
        }
    }
    return data_start + data_bytes;
}

/* A PBP package: PARAM.SFO, empty art sections, a stub DATA.PSP and psar. */
static uint8_t *psp_seal_pbp(const char *category, const char *disc_id,
                             const uint8_t *psar, size_t psar_len, size_t *out_len) {
    uint8_t sfo[512];
    size_t sfo_len = psp_seal_sfo(sfo, sizeof(sfo), category, disc_id,
                                  "Synthetic PSN package");
    size_t sfo_at = 0x28u;
    size_t psp_at = (sfo_at + sfo_len + 15u) & ~(size_t)15u;
    size_t psar_at = (psp_at + 0x200u + 15u) & ~(size_t)15u;
    uint8_t *pbp = (uint8_t *)calloc(1, psar_at + psar_len);
    assert(pbp != NULL);
    memcpy(pbp, "\0PBP", 4);
    psp_seal_put32le(pbp + 4, 0x00010001u);
    psp_seal_put32le(pbp + 8, (uint32_t)sfo_at);
    for (unsigned i = 1; i < 7u; i++) psp_seal_put32le(pbp + 8 + i * 4u, (uint32_t)psp_at);
    psp_seal_put32le(pbp + 0x24, (uint32_t)psar_at);
    memcpy(pbp + sfo_at, sfo, sfo_len);
    memcpy(pbp + psp_at, "NKSTUB", 6);
    memcpy(pbp + psar_at, psar, psar_len);
    *out_len = psar_at + psar_len;
    return pbp;
}

/* DATA.PSAR of a PSN UMD image holding iso (a whole number of 2048-byte
 * sectors), sealed with the DRM BB calls under ctx's KeyStore. */
static uint8_t *psp_seal_npumdimg(NkPspCtx *ctx, const uint8_t *iso, size_t iso_len,
                                  uint32_t block_lbas, size_t *out_len) {
    uint8_t version[16], header_key[16], mac[16];
    size_t lbas = iso_len / 2048u;
    size_t block_size = (size_t)block_lbas * 2048u;
    size_t blocks = (lbas + block_lbas - 1u) / block_lbas;
    size_t table_at = 0x100u;
    size_t data_at = (table_at + blocks * 32u + 15u) & ~(size_t)15u;
    size_t total = data_at + blocks * block_size;
    uint8_t *psar = (uint8_t *)calloc(1, total);
    MAC_KEY mkey;
    CIPHER_KEY ckey;
    int rc;
    assert(psar != NULL && iso_len % 2048u == 0u && lbas > 0u && block_lbas > 0u);
    for (unsigned i = 0; i < 16u; i++) {
        version[i] = (uint8_t)(0x31u + 7u * i);
        header_key[i] = (uint8_t)(0xC5u ^ (13u * i));
    }
    for (size_t b = 0; b < blocks; b++) {
        size_t offset = data_at + b * block_size;
        size_t start = b * block_size;
        size_t take = iso_len - start < block_size ? iso_len - start : block_size;
        uint32_t w[8];
        memcpy(psar + offset, iso + start, take);
        rc = sceDrmBBCipherInit(ctx, &ckey, 1, 2, header_key, version,
                                (uint32_t)(offset >> 4));
        assert(rc == 0);
        rc = sceDrmBBCipherUpdate(ctx, &ckey, psar + offset, (int)block_size);
        assert(rc == 0);
        sceDrmBBCipherFinal(&ckey);
        for (unsigned j = 0; j < 4u; j++) w[j] = 0x4E4B0000u + (uint32_t)(b * 4u + j);
        w[4] = (uint32_t)offset ^ (w[2] ^ w[3]);
        w[5] = (uint32_t)block_size ^ (w[1] ^ w[2]);
        w[6] = w[0] ^ w[3];
        w[7] = w[0] ^ w[1];
        for (unsigned j = 0; j < 8u; j++) psp_seal_put32le(psar + table_at + b * 32u + j * 4u, w[j]);
    }
    memcpy(psar, "NPUMDIMG", 8);
    psp_seal_put32le(psar + 0x08, 3u);
    psp_seal_put32le(psar + 0x0C, block_lbas);
    memcpy(psar + 0x10, "UP9999-TEST89999_00-SYNTHETIC0000000", 36);
    psp_seal_put32le(psar + 0x54, 0u);
    psp_seal_put32le(psar + 0x64, (uint32_t)(lbas - 1u));
    psp_seal_put32le(psar + 0x6C, (uint32_t)table_at);
    rc = sceDrmBBCipherInit(ctx, &ckey, 1, 2, header_key, version, 0u);
    assert(rc == 0);
    rc = sceDrmBBCipherUpdate(ctx, &ckey, psar + 0x40, 0x60);
    assert(rc == 0);
    sceDrmBBCipherFinal(&ckey);
    memcpy(psar + 0xA0, header_key, 16u);
    rc = sceDrmBBMacInit(ctx, &mkey, 3);
    assert(rc == 0);
    rc = sceDrmBBMacUpdate(ctx, &mkey, psar, 0xC0);
    assert(rc == 0);
    rc = sceDrmBBMacFinal(ctx, &mkey, mac, NULL);
    assert(rc == 0);
    for (unsigned i = 0; i < 16u; i++) mac[i] ^= version[i];
    rc = kirk4(ctx, mac, mac, 16u, 0x38);
    assert(rc == KIRK_OPERATION_SUCCESS);
    rc = kirk4(ctx, mac, mac, 16u, 0x63);
    assert(rc == KIRK_OPERATION_SUCCESS);
    (void)rc;
    memcpy(psar + 0xC0, mac, 16u);
    *out_len = total;
    return psar;
}

/* The whole EBOOT.PBP for iso, sealed under ctx's KeyStore. */
static uint8_t *psp_seal_npumdimg_pbp(NkPspCtx *ctx, const char *disc_id,
                                      const uint8_t *iso, size_t iso_len,
                                      uint32_t block_lbas, size_t *out_len) {
    size_t psar_len = 0;
    uint8_t *psar = psp_seal_npumdimg(ctx, iso, iso_len, block_lbas, &psar_len);
    uint8_t *pbp = psp_seal_pbp("EG", disc_id, psar, psar_len, out_len);
    free(psar);
    return pbp;
}

#endif /* NK_TEST_PSP_SEAL_FIXTURE_H */
