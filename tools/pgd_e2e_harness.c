/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2025-2026 the psp-recomp authors
 *
 * End-to-end driver for the runtime PGD implementation, built against
 * src/rt/pgd.c by tools/test_pgd_malformed.py. Inputs are synthetic PGD files
 * forged with the Python reference (tools/pgd_decrypt.py); no game data is
 * involved. Each command prints staged diagnostics so the Python side can
 * assert the INTENDED rejection reason (magic vs DRM type vs MAC vs size
 * validation vs read failure), not merely "some error".
 *
 *   pgd_e2e_harness <file> <32-hex-vkey> probe
 *   pgd_e2e_harness <file> <32-hex-vkey> readall
 *   pgd_e2e_harness <file> <32-hex-vkey> read <index>
 *   pgd_e2e_harness <file> <32-hex-vkey> cache <good-index> <bad-index>
 */

#include "pgd.c"

#include <inttypes.h>

static int parse_vkey(const char *hex, uint8_t out[16]) {
    if (strlen(hex) != 32) return 0;
    for (int i = 0; i < 16; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return 0;
        out[i] = (uint8_t)v;
    }
    return 1;
}

static void print_hex(const char *tag, const uint8_t *buf, uint32_t len) {
    printf("%s ", tag);
    for (uint32_t i = 0; i < len; i++) printf("%02x", buf[i]);
    printf("\n");
}

/* Re-run each header acceptance stage independently so a NULL sr_pgd_open can
 * be attributed to exactly one check. Mirrors sr_pgd_open's order. */
static void probe(const uint8_t header[0x90], const uint8_t vkey[16]) {
    kirk_init();
    printf("MAGIC %s\n", memcmp(header, "\x00PGD", 4) == 0 ? "OK" : "BAD");
    uint32_t key_index = rd32(header + 4);
    uint32_t drm_type = rd32(header + 8);
    printf("DRM %" PRIu32 "\n", drm_type);
    int mac_type = key_index > 1 ? 3 : 1;
    printf("MAC80 %s\n",
           bbmac_verify(header, 0x80, header + 0x80, mac_type, DNAS_1A90) ? "OK" : "BAD");
    printf("MAC70 %s\n",
           bbmac_verify(header, 0x70, header + 0x70, mac_type, vkey) ? "OK" : "BAD");

    uint8_t params[0x30], hkey[16], tmp2[16];
    memcpy(params, header + 0x30, 0x30);
    for (int i = 0; i < 16; i++) hkey[i] = (uint8_t)(header[0x10 + i] ^ vkey[i]);
    bbcipher_tmp2(hkey, tmp2);
    bbcipher_apply(tmp2, 0, params, 0x30);
    uint32_t data_size = rd32(params + 0x14);
    uint32_t block_size = rd32(params + 0x18);
    uint32_t data_offset = rd32(params + 0x1C);
    printf("PARAMS data_size=%" PRIu32 " block_size=%" PRIu32 " data_offset=%" PRIu32 "\n",
           data_size, block_size, data_offset);
    uint32_t align = 0;
    printf("VALIDATE %s\n", pgd_validate_sizes(data_size, block_size, &align) ? "OK" : "BAD");

    SrPgd *p = sr_pgd_open(header, vkey);
    printf("OPEN %s\n", p ? "OK" : "NULL");
    sr_pgd_free(p);
}

static SrPgd *open_or_die(const uint8_t header[0x90], const uint8_t vkey[16]) {
    SrPgd *p = sr_pgd_open(header, vkey);
    if (!p) {
        printf("OPEN NULL\n");
        exit(2);
    }
    return p;
}

static void read_one(SrPgd *p, FILE *f, uint32_t index) {
    uint32_t len = sr_pgd_block_len(p, index);
    const uint8_t *blk = sr_pgd_block(p, f, index);
    if (!blk) {
        printf("BLOCK %" PRIu32 " NULL len=%" PRIu32 "\n", index, len);
        return;
    }
    printf("BLOCK %" PRIu32 " len=%" PRIu32 " ", index, len);
    for (uint32_t i = 0; i < len; i++) printf("%02x", blk[i]);
    printf("\n");
}

static void readall(SrPgd *p, FILE *f) {
    size_t total = 0;
    uint8_t *out = NULL;
    for (uint32_t i = 0;; i++) {
        uint32_t len = sr_pgd_block_len(p, i);
        if (len == 0) break;
        const uint8_t *blk = sr_pgd_block(p, f, i);
        if (!blk) {
            printf("BLOCK %" PRIu32 " NULL len=%" PRIu32 "\n", i, len);
            free(out);
            exit(3);
        }
        printf("BLOCKLEN %" PRIu32 " %" PRIu32 "\n", i, len);
        if ((size_t)len > SIZE_MAX - total) {   /* refuse overflow before realloc/memcpy */
            printf("READALL OVERFLOW\n");
            free(out);
            exit(4);
        }
        uint8_t *grown = (uint8_t *)realloc(out, total + len);
        if (!grown) { free(out); exit(4); }
        out = grown;
        memcpy(out + total, blk, len);
        total += len;
    }
    size_t logical = sr_pgd_data_size(p);
    if (logical > total) logical = total;
    print_hex("DATA", out ? out : (const uint8_t *)"", (uint32_t)logical);
    free(out);
}

/* Cache-state scenario: a decrypted block must survive later FAILED reads
 * untouched, and failures must not poison or fabricate cache entries. */
static void cache_scenario(SrPgd *p, FILE *f, uint32_t good, uint32_t bad) {
    uint32_t len = sr_pgd_block_len(p, good);
    const uint8_t *blk = sr_pgd_block(p, f, good);
    if (!blk || len == 0) { printf("CACHE FAIL initial\n"); exit(5); }
    uint8_t *snap = (uint8_t *)malloc(len);
    if (!snap) exit(4);
    memcpy(snap, blk, len);

    /* Repeated invalid reads: out-of-range index and (per the caller's file
     * construction) an in-range index whose ciphertext the file lacks. */
    for (int round = 0; round < 3; round++) {
        if (sr_pgd_block(p, f, UINT32_MAX) != NULL) { printf("CACHE FAIL oob\n"); exit(5); }
        if (sr_pgd_block(p, f, bad) != NULL) { printf("CACHE FAIL badread\n"); exit(5); }
    }

    const uint8_t *again = sr_pgd_block(p, f, good);
    if (!again) { printf("CACHE FAIL reread-null\n"); exit(5); }
    if (memcmp(again, snap, len) != 0) { printf("CACHE FAIL reread-diff\n"); exit(5); }
    free(snap);
    printf("CACHE OK\n");
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <file> <vkeyhex> <cmd> [args]\n", argv[0]); return 64; }
    if (!sr_pgd_selftest()) { fprintf(stderr, "AES selftest FAILED\n"); return 1; }
    uint8_t vkey[16];
    if (!parse_vkey(argv[2], vkey)) { fprintf(stderr, "bad vkey\n"); return 64; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 66; }

    uint8_t header[0x90];
    size_t got = fread(header, 1, sizeof(header), f);
    if (got < sizeof(header)) {
        printf("SHORT_HEADER %zu\n", got);
        fclose(f);
        return 0;
    }

    const char *cmd = argv[3];
    if (strcmp(cmd, "probe") == 0) {
        probe(header, vkey);
    } else if (strcmp(cmd, "readall") == 0) {
        SrPgd *p = open_or_die(header, vkey);
        readall(p, f);
        sr_pgd_free(p);
    } else if (strcmp(cmd, "read") == 0 && argc >= 5) {
        SrPgd *p = open_or_die(header, vkey);
        read_one(p, f, (uint32_t)strtoul(argv[4], NULL, 0));
        sr_pgd_free(p);
    } else if (strcmp(cmd, "cache") == 0 && argc >= 6) {
        SrPgd *p = open_or_die(header, vkey);
        cache_scenario(p, f, (uint32_t)strtoul(argv[4], NULL, 0),
                       (uint32_t)strtoul(argv[5], NULL, 0));
        sr_pgd_free(p);
    } else {
        fprintf(stderr, "unknown command %s\n", cmd);
        fclose(f);
        return 64;
    }
    fclose(f);
    return 0;
}
