/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Synthetic type-2 ~PSP sealer for native tests (issue #295).
 *
 * The inverse of the production type-2 path: it wraps a test-owned payload in
 * a ~PSP container that nk_container_decrypt opens with the given tag recipe,
 * keyvault row and KIRK command-1 key. The fuzz harness passes constant fake
 * keys; the player test passes the built-in table's values for one tag, read
 * from the linked table at run time. Header-only so each test binary links
 * nothing extra. Mirrors tools/test_psp_decrypt.py's sealer. */

#ifndef NK_TEST_PSP_SEAL_FIXTURE_H
#define NK_TEST_PSP_SEAL_FIXTURE_H

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nk_psp_aes.h"
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

#endif /* NK_TEST_PSP_SEAL_FIXTURE_H */
