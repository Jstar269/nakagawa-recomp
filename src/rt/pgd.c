/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2025-2026 the psp-recomp authors
 *
 * PSP PGD (amctrl) decryptor -- C port of tools/pgd_decrypt.py. The
 * PSP-specific BBMac/BBCipher/PGD flow is derived-translated from the public
 * amctrl/PGD implementation family; AES-128 and later defensive/runtime
 * hardening are independently expressed. The required KIRK/amctrl platform
 * data is supplied locally and is a separate provenance category. See
 * docs/provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md. sr_pgd_open only
 * succeeds when the header MAC is reproduced exactly.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include "pgd.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/types.h>
#endif

/* -------------------------------------------------------------------------
 * AES-128 from GF(2^8) (FIPS-197). S-box and Rcon are computed, not tabled.
 * ------------------------------------------------------------------------- */

static uint8_t s_sbox[256], s_inv_sbox[256];
static int s_aes_ready = 0;

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a = (uint8_t)(a << 1);
        if (hi) a ^= 0x1B;
        b >>= 1;
    }
    return p;
}

static uint8_t rotl8(uint8_t b, int n) {
    return (uint8_t)((b << n) | (b >> (8 - n)));
}

static void aes_init(void) {
    if (s_aes_ready) return;
    /* multiplicative inverse via exp/log over generator 3 */
    uint8_t exp_[256], log_[256];
    uint8_t x = 1;
    for (int i = 0; i < 255; i++) { exp_[i] = x; log_[x] = (uint8_t)i; x = gf_mul(x, 3); }
    exp_[255] = exp_[0];   /* exp is periodic mod 255; makes inv(1)=exp[255] valid */
    for (int b = 0; b < 256; b++) {
        uint8_t inv = b == 0 ? 0 : exp_[255 - log_[b]];
        uint8_t s = (uint8_t)(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^ rotl8(inv, 4) ^ 0x63);
        s_sbox[b] = s;
    }
    for (int i = 0; i < 256; i++) s_inv_sbox[s_sbox[i]] = (uint8_t)i;
    s_aes_ready = 1;
}

/* Round keys: 11 * 16 bytes. State is column-major (byte = row + 4*col). */
static void aes_expand(const uint8_t key[16], uint8_t rk[176]) {
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if (i % 4 == 0) {
            uint8_t tmp = t[0]; t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = tmp;  /* RotWord */
            for (int j = 0; j < 4; j++) t[j] = s_sbox[t[j]];                        /* SubWord */
            t[0] ^= rcon;
            rcon = gf_mul(rcon, 2);
        }
        for (int j = 0; j < 4; j++) rk[i * 4 + j] = (uint8_t)(rk[(i - 4) * 4 + j] ^ t[j]);
    }
}

static void aes_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = s_sbox[s[i]];               /* SubBytes */
        for (int r = 1; r < 4; r++) {                                   /* ShiftRows */
            uint8_t row[4];
            for (int c = 0; c < 4; c++) row[c] = s[r + 4 * c];
            for (int c = 0; c < 4; c++) s[r + 4 * c] = row[(c + r) & 3];
        }
        if (round != 10) {                                             /* MixColumns */
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + 4 * c, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (uint8_t)(gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3);
                col[1] = (uint8_t)(a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3);
                col[2] = (uint8_t)(a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3));
                col[3] = (uint8_t)(gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2));
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];       /* AddRoundKey */
    }
    memcpy(out, s, 16);
}

static void aes_decrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[10 * 16 + i];
    for (int round = 9; round >= 0; round--) {
        for (int r = 1; r < 4; r++) {                                  /* InvShiftRows */
            uint8_t row[4];
            for (int c = 0; c < 4; c++) row[c] = s[r + 4 * c];
            for (int c = 0; c < 4; c++) s[r + 4 * c] = row[(c - r) & 3];
        }
        for (int i = 0; i < 16; i++) s[i] = s_inv_sbox[s[i]];          /* InvSubBytes */
        for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];       /* AddRoundKey */
        if (round != 0) {                                             /* InvMixColumns */
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + 4 * c, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (uint8_t)(gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9));
                col[1] = (uint8_t)(gf_mul(a0, 9) ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13));
                col[2] = (uint8_t)(gf_mul(a0, 13) ^ gf_mul(a1, 9) ^ gf_mul(a2, 14) ^ gf_mul(a3, 11));
                col[3] = (uint8_t)(gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9) ^ gf_mul(a3, 14));
            }
        }
    }
    memcpy(out, s, 16);
}

/* CBC with IV=0 (the KIRK cmd4/7 mode), in place-safe (out may equal in). */
static void aes_cbc_decrypt0(const uint8_t rk[176], uint8_t *buf, int n) {
    uint8_t prev[16] = {0}, ct[16];
    for (int i = 0; i < n; i += 16) {
        memcpy(ct, buf + i, 16);
        aes_decrypt_block(rk, buf + i, buf + i);
        for (int j = 0; j < 16; j++) buf[i + j] ^= prev[j];
        memcpy(prev, ct, 16);
    }
}

/* -------------------------------------------------------------------------
 * PSP KIRK / amctrl constants.
 *
 * These are NOT shipped with this project. They are PSP console decryption
 * values that the user supplies locally at runtime. Excluding the values is a
 * concrete boundary; it does not resolve the separate legal question of
 * distributing the implementation itself (NOTICE.md and issue #104).
 *
 * Format: a text file of `name = <32 hex chars>` lines, one per entry, `#` for
 * comments. Located via $SR_PGD_KEYS, else ./keys/pgd_keys.txt. The file is
 * gitignored. See docs/PGD_KEYS.md for the full schema and how to populate it.
 *
 * Absent or incomplete keys are not an error at load time: sr_pgd_open simply
 * returns NULL, which is the same contract it already had for a wrong key.
 * ------------------------------------------------------------------------- */

#define PGD_KEY_COUNT 7

static uint8_t KEY_38[16], KEY_39[16], KEY_63[16];
static uint8_t LOC_1CD4[16], LOC_1CE4[16], LOC_1CF4[16];
static uint8_t DNAS_1A90[16];

static const struct { const char *name; uint8_t *dst; } k_pgd_keys[PGD_KEY_COUNT] = {
    {"kirk_keyseed_38", KEY_38},
    {"kirk_keyseed_39", KEY_39},
    {"kirk_keyseed_63", KEY_63},
    {"amctrl_loc_1cd4", LOC_1CD4},
    {"amctrl_loc_1ce4", LOC_1CE4},
    {"amctrl_loc_1cf4", LOC_1CF4},
    {"dnas_1a90",       DNAS_1A90},
};

/* Round keys for the three fixed keys, expanded once. */
static uint8_t s_rk38[176], s_rk39[176], s_rk63[176];

static int s_keys_state = 0;   /* 0 = untried, 1 = loaded, -1 = unavailable */

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

const char *sr_pgd_keys_path(void) {
    const char *env = getenv("SR_PGD_KEYS");
    return (env && env[0]) ? env : "keys/pgd_keys.txt";
}

/* Parse `name = hex` lines into the table above. Returns the number of distinct
 * entries filled, so a truncated or partially-edited file fails closed. */
static int pgd_keys_parse(FILE *f) {
    char line[256];
    int seen[PGD_KEY_COUNT] = {0};
    int filled = 0;
    while (fgets(line, (int)sizeof line, f)) {
        size_t line_len = strlen(line);
        if (line_len == 0) continue;
        if (line[line_len - 1] != '\n' && line[line_len - 1] != '\r' && !feof(f)) {
            /* A physical line longer than the fixed buffer must not be
             * interpreted as a complete key/value record.  Drain the rest so
             * a suffix cannot be mistaken for a second valid entry. */
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) { }
            continue;
        }
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *name_end = eq;
        while (name_end > p && (name_end[-1] == ' ' || name_end[-1] == '\t')) name_end--;
        *name_end = '\0';
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        char *v_end = v + strcspn(v, "\r\n");
        *v_end = '\0';
        while (v_end > v && (v_end[-1] == ' ' || v_end[-1] == '\t')) *--v_end = '\0';
        for (int i = 0; i < PGD_KEY_COUNT; i++) {
            if (seen[i] || strcmp(p, k_pgd_keys[i].name) != 0) continue;
            if ((size_t)(v_end - v) != 32u) break;
            uint8_t tmp[16];
            int ok = 1;
            for (int b = 0; b < 16 && ok; b++) {
                int hi = hex_nibble((unsigned char)v[b * 2]);
                int lo = hi < 0 ? -1 : hex_nibble((unsigned char)v[b * 2 + 1]);
                if (lo < 0) ok = 0;
                else tmp[b] = (uint8_t)((hi << 4) | lo);
            }
            if (ok) {
                memcpy(k_pgd_keys[i].dst, tmp, 16);
                seen[i] = 1;
                filled++;
            }
            break;
        }
    }
    return filled;
}

/* Load the console constants and expand round keys. Returns 1 when the full set
 * is available. Tried at most once per process. */
static int kirk_init(void) {
    if (s_keys_state) return s_keys_state > 0;
    s_keys_state = -1;

    const char *path = sr_pgd_keys_path();
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int filled = pgd_keys_parse(f);
    fclose(f);
    if (filled != PGD_KEY_COUNT) return 0;

    aes_init();
    aes_expand(KEY_38, s_rk38);
    aes_expand(KEY_39, s_rk39);
    aes_expand(KEY_63, s_rk63);
    s_keys_state = 1;
    return 1;
}

int sr_pgd_keys_available(void) { return kirk_init(); }

/* -------------------------------------------------------------------------
 * BBMac (AES-CMAC-derived) over a 16-aligned buffer, fixed-key (mac_type 1/3).
 * ------------------------------------------------------------------------- */

static void cmac_shift(const uint8_t in[16], uint8_t out[16]) {
    uint8_t carry = (in[0] & 0x80) ? 0x87 : 0;
    for (int i = 0; i < 15; i++) out[i] = (uint8_t)((in[i] << 1) | (in[i + 1] >> 7));
    out[15] = (uint8_t)((in[15] << 1) ^ carry);
}

/* CBC-MAC one 16-aligned run continued from `running`, key38 (mac_type 1/3).
 * Process one block at a time instead of allocating a temporary copy of the
 * entire run. This is equivalent to CBC with the incoming `running` value as
 * the previous ciphertext block and removes an allocation-failure surface. */
static void bbmac_cbc(uint8_t running[16], const uint8_t *block, size_t n) {
    uint8_t tmp[16];
    for (size_t i = 0; i < n; i += 16) {
        for (int j = 0; j < 16; j++) tmp[j] = (uint8_t)(block[i + (size_t)j] ^ running[j]);
        aes_encrypt_block(s_rk38, tmp, running);
    }
}

/* Compute the BBMac over `data` (len a multiple of 16); vkey may be NULL. */
static void bbmac(const uint8_t *data, int len, const uint8_t *vkey, uint8_t out[16]) {
    uint8_t running[16] = {0};
    int body = len - 16;
    if (body > 0) bbmac_cbc(running, data, body);
    uint8_t l[16], k1[16], zero[16] = {0};
    aes_encrypt_block(s_rk38, zero, l);
    cmac_shift(l, k1);
    uint8_t t[16];
    for (int i = 0; i < 16; i++) t[i] = (uint8_t)(data[len - 16 + i] ^ k1[i] ^ running[i]);
    uint8_t mac[16];
    aes_encrypt_block(s_rk38, t, mac);
    for (int i = 0; i < 16; i++) mac[i] ^= LOC_1CD4[i];
    if (vkey) {
        for (int i = 0; i < 16; i++) mac[i] ^= vkey[i];
        aes_encrypt_block(s_rk38, mac, mac);
    }
    memcpy(out, mac, 16);
}

/* Verify: reproduce the stored MAC. mac_type 3 first decrypts the stored MAC
 * with key63; type 1 compares directly. Returns 1 on match. */
static int bbmac_verify(const uint8_t *data, int len, const uint8_t stored[16],
                        int mac_type, const uint8_t *vkey) {
    uint8_t mac[16], check[16];
    bbmac(data, len, vkey, mac);
    memcpy(check, stored, 16);
    if (mac_type == 3) aes_cbc_decrypt0(s_rk63, check, 16);
    return memcmp(check, mac, 16) == 0;
}

/* -------------------------------------------------------------------------
 * BBCipher (fixed-key, cipher_type 1): decrypt `len` bytes (16-aligned) under
 * the amctrl stream cipher. header_key ^ vkey forms the cipher key.
 * ------------------------------------------------------------------------- */

static void bbcipher_tmp2(const uint8_t key[16], uint8_t tmp2[16]) {
    uint8_t kb[16];
    for (int i = 0; i < 16; i++) kb[i] = (uint8_t)(key[i] ^ LOC_1CF4[i]);
    aes_cbc_decrypt0(s_rk39, kb, 16);   /* kirk7 on one block = single-block decrypt */
    for (int i = 0; i < 16; i++) tmp2[i] = (uint8_t)(kb[i] ^ LOC_1CE4[i]);
}

/* Decrypt in place: data[0..len) ^= keystream(tmp2, seed). len<=0x800 per call
 * matches how the header (0x30) and each data block (block_size) are processed. */
static void bbcipher_apply(const uint8_t tmp2[16], uint32_t seed, uint8_t *data, size_t len) {
    if (len == 0) return;

    uint32_t ckey_seed = seed + 1;
    uint8_t tmp1[16];
    if (ckey_seed == 1) {
        memset(tmp1, 0, 16);
    } else {
        memcpy(tmp1, tmp2, 12);
        uint32_t v = ckey_seed - 1;
        tmp1[12] = (uint8_t)v; tmp1[13] = (uint8_t)(v >> 8); tmp1[14] = (uint8_t)(v >> 16); tmp1[15] = (uint8_t)(v >> 24);
    }
    /* The historical implementation built the complete rounded-up keystream
     * in one allocation and then ran AES-CBC over it. Stream CBC decryption a
     * block at a time instead: plaintext_i = D(C_i) ^ C_(i-1), IV=0. This is
     * bit-identical, needs constant stack space, and makes len==0 harmless. */
    uint8_t prev[16] = {0};
    uint8_t ct[16], ks[16];
    size_t offset = 0;
    int first = 1;
    while (offset < len) {
        memcpy(ct, tmp2, 12);
        ct[12] = (uint8_t)ckey_seed; ct[13] = (uint8_t)(ckey_seed >> 8);
        ct[14] = (uint8_t)(ckey_seed >> 16); ct[15] = (uint8_t)(ckey_seed >> 24);
        ckey_seed++;

        aes_decrypt_block(s_rk63, ct, ks);
        for (int i = 0; i < 16; i++) ks[i] ^= prev[i];
        if (first) {
            for (int i = 0; i < 16; i++) ks[i] ^= tmp1[i];
            first = 0;
        }

        size_t chunk = len - offset;
        if (chunk > 16) chunk = 16;
        for (size_t i = 0; i < chunk; i++) data[offset + i] ^= ks[i];
        memcpy(prev, ct, 16);
        offset += chunk;
    }
}

/* -------------------------------------------------------------------------
 * PGD context.
 * ------------------------------------------------------------------------- */

struct SrPgd {
    uint8_t dkey[16];
    uint8_t data_tmp2[16];      /* precomputed BBCipher mixer for data blocks */
    uint32_t data_size, block_size, data_offset, align_size;
    uint32_t cached_index;
    int cache_valid;
    uint8_t *cache;             /* block_size bytes */
    uint8_t *cipher;            /* block_size bytes scratch */
};

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Validate decrypted PGD size parameters without 32-bit alignment wrap.
 * Runs before any block_size-proportional allocation: an oversized
 * block_size is rejected here (SR_PGD_MAX_BLOCK_SIZE, see pgd.h), never
 * handed to malloc in the hope that the allocator refuses it. */
static int pgd_validate_sizes(uint32_t data_size, uint32_t block_size, uint32_t *align_size) {
    if (!align_size || block_size == 0 || (block_size & 15u) != 0) return 0;
    if (block_size > SR_PGD_MAX_BLOCK_SIZE) return 0;
    uint64_t aligned = ((uint64_t)data_size + 15u) & ~(uint64_t)15u;
    if (aligned > UINT32_MAX) return 0;
    *align_size = (uint32_t)aligned;
    return 1;
}

static int pgd_seek_abs(FILE *host, uint64_t offset) {
    if (!host || offset > (uint64_t)INT64_MAX) return -1;
#if defined(_WIN32)
    return _fseeki64(host, (__int64)offset, SEEK_SET);
#else
    return fseeko(host, (off_t)offset, SEEK_SET);
#endif
}

int sr_pgd_selftest(void) {
    /* FIPS-197 known-answer only: this proves the AES core, and must keep
     * working when the console constants are not installed. */
    aes_init();
    static const uint8_t key[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    static const uint8_t pt[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    static const uint8_t ct[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
    uint8_t rk[176], out[16];
    aes_expand(key, rk);
    aes_encrypt_block(rk, pt, out);
    if (memcmp(out, ct, 16) != 0) return 0;
    aes_decrypt_block(rk, ct, out);
    if (memcmp(out, pt, 16) != 0) return 0;
    return s_sbox[0x00] == 0x63 && s_sbox[0x53] == 0xed;
}

SrPgd *sr_pgd_open(const uint8_t header[0x90], const uint8_t vkey[16]) {
    if (!header || !vkey) return NULL;
    if (!kirk_init()) return NULL;   /* console constants not installed locally */
    if (memcmp(header, "\x00PGD", 4) != 0) return NULL;
    uint32_t key_index = rd32(header + 4);
    uint32_t drm_type = rd32(header + 8);
    if (drm_type != 1) return NULL;                 /* type 2 needs a per-console fuse key */
    int mac_type = key_index > 1 ? 3 : 1;

    if (!bbmac_verify(header + 0x00, 0x80, header + 0x80, mac_type, DNAS_1A90)) return NULL;
    if (!bbmac_verify(header + 0x00, 0x70, header + 0x70, mac_type, vkey)) return NULL;

    /* Decrypt the 0x30-byte parameter block at 0x30 with header_key ^ vkey. */
    uint8_t params[0x30], hkey[16], tmp2[16];
    memcpy(params, header + 0x30, 0x30);
    for (int i = 0; i < 16; i++) hkey[i] = (uint8_t)(header[0x10 + i] ^ vkey[i]);
    bbcipher_tmp2(hkey, tmp2);
    bbcipher_apply(tmp2, 0, params, 0x30);

    SrPgd *p = (SrPgd *)calloc(1, sizeof(SrPgd));
    if (!p) return NULL;
    memcpy(p->dkey, params + 0x00, 16);
    p->data_size = rd32(params + 0x14);
    p->block_size = rd32(params + 0x18);
    p->data_offset = rd32(params + 0x1C);
    if (!pgd_validate_sizes(p->data_size, p->block_size, &p->align_size)) { free(p); return NULL; }

    uint8_t dmix[16];
    for (int i = 0; i < 16; i++) dmix[i] = (uint8_t)(p->dkey[i] ^ vkey[i]);
    bbcipher_tmp2(dmix, p->data_tmp2);

    p->cached_index = 0;
    p->cache_valid = 0;
    p->cache = (uint8_t *)malloc(p->block_size);
    p->cipher = (uint8_t *)malloc(p->block_size);
    if (!p->cache || !p->cipher) { sr_pgd_free(p); return NULL; }
    return p;
}

uint32_t sr_pgd_data_size(const SrPgd *p) { return p ? p->data_size : 0; }
uint32_t sr_pgd_block_size(const SrPgd *p) { return p ? p->block_size : 0; }
uint32_t sr_pgd_data_offset(const SrPgd *p) { return p ? p->data_offset : 0; }

uint32_t sr_pgd_block_len(const SrPgd *p, uint32_t index) {
    if (!p) return 0;
    uint64_t start = (uint64_t)index * p->block_size;
    if (start >= p->align_size) return 0;
    uint64_t avail = p->align_size - start;
    return avail < p->block_size ? (uint32_t)avail : p->block_size;
}

const uint8_t *sr_pgd_block(SrPgd *p, FILE *host, uint32_t index) {
    if (!p || !host) return NULL;
    if (p->cache_valid && index == p->cached_index) return p->cache;
    uint32_t len = sr_pgd_block_len(p, index);
    if (len == 0) return NULL;
    uint64_t phys = (uint64_t)p->data_offset + (uint64_t)index * p->block_size;
    if (pgd_seek_abs(host, phys) != 0) return NULL;
    if (fread(p->cipher, 1, len, host) != len) return NULL;
    memcpy(p->cache, p->cipher, len);
    uint32_t seed = (uint32_t)(((uint64_t)index * p->block_size) >> 4);
    bbcipher_apply(p->data_tmp2, seed, p->cache, len);
    p->cached_index = index;
    p->cache_valid = 1;
    return p->cache;
}

void sr_pgd_free(SrPgd *p) {
    if (!p) return;
    free(p->cache);
    free(p->cipher);
    free(p);
}

#ifdef SR_PGD_TEST
/* Standalone verifier: `pgd_test <file> <32-hex-vkey>`. Prints params + a hex
 * digest of the first blocks so tools/test_pgd_c.py can diff against the Python
 * reference. */
#include <ctype.h>
int main(int argc, char **argv) {
    if (!sr_pgd_selftest()) { fprintf(stderr, "AES selftest FAILED\n"); return 1; }
    printf("AES FIPS-197 selftest: OK\n");
    if (argc < 3) return 0;
    uint8_t vkey[16];
    for (int i = 0; i < 16; i++) { unsigned v; sscanf(argv[2] + i * 2, "%2x", &v); vkey[i] = (uint8_t)v; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    uint8_t header[0x90];
    if (fread(header, 1, 0x90, f) != 0x90) { fprintf(stderr, "short header\n"); return 1; }
    SrPgd *p = sr_pgd_open(header, vkey);
    if (!p) { fprintf(stderr, "sr_pgd_open FAILED (MAC/key)\n"); return 2; }
    printf("open OK: data_size=%u block_size=%u data_offset=%u\n",
           sr_pgd_data_size(p), sr_pgd_block_size(p), sr_pgd_data_offset(p));
    for (uint32_t b = 0; b < 4; b++) {
        const uint8_t *blk = sr_pgd_block(p, f, b);
        if (!blk) break;
        printf("block %u first16:", b);
        for (int i = 0; i < 16; i++) printf(" %02x", blk[i]);
        printf("\n");
    }
    sr_pgd_free(p);
    fclose(f);
    return 0;
}
#endif
