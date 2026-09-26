/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include "nk_types.h"
#include "nk_iso.h"
#include "nk_xb.h"
#include "nk_title_manifest.h"
#include "nk_library.h"
#include "nk_platform.h"
#include "prx_loader.h"
#include "nk_psp_aes.h"
#include "nk_psp_container.h"
#include "nk_psp_inflate.h"
#include "nk_psp_keystore.h"
#include "nk_psp_kirk.h"
#include "nk_psp_kle.h"
#include "nk_psp_sha1.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bounded mock guest arena for PRX loader relocations and segment writes */
static uint8_t s_guest_arena[512 * 1024];

int sr_prx_guest_write(uint32_t guest_addr, const void *src, uint32_t n) {
    uint32_t base = 0x08800000u;
    if (guest_addr < base) return -1;
    uint64_t offset = (uint64_t)guest_addr - base;
    if (offset > sizeof(s_guest_arena) || (uint64_t)n > sizeof(s_guest_arena) - offset) {
        return -1;
    }
    if (src && n > 0) {
        memcpy(s_guest_arena + offset, src, n);
    }
    return 0;
}

/* Deterministic 64-bit PRNG (SplitMix64) */
static uint64_t s_rng_state = 0x5EEDF007CAFE1234ULL;

static inline uint64_t fuzz_rand64(void) {
    uint64_t z = (s_rng_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static inline uint32_t fuzz_rand32(void) {
    return (uint32_t)fuzz_rand64();
}

static void write_file_bytes(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (size > 0) {
        assert(fwrite(data, 1, size, f) == size);
    }
    fclose(f);
}

static void put16le(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t get32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The KL4E/KL3E header carries its copy length most-significant byte first. */
static void put32be(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void put32_iso_both(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
    p[4] = (uint8_t)(value >> 24);
    p[5] = (uint8_t)(value >> 16);
    p[6] = (uint8_t)(value >> 8);
    p[7] = (uint8_t)value;
}

static void reset_guest_arena(void) {
    memset(s_guest_arena, 0xA5, sizeof(s_guest_arena));
}

static int guest_arena_is_guard(void) {
    for (size_t i = 0; i < sizeof(s_guest_arena); i++) {
        if (s_guest_arena[i] != 0xA5) return 0;
    }
    return 1;
}

/* Mutate buffer in-place with bit flips, truncations, inserts, deletions,
   and length/offset/integer boundary value corruptions. */
static void mutate_elf_field(uint8_t *data, size_t size);

static unsigned parse_fuzz_iterations(const char *text, unsigned fallback) {
    if (!text || !*text) return fallback;
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > 100000ul) {
        return fallback;
    }
    return (unsigned)value;
}

static void mutate_buffer(uint8_t *data, size_t *size, size_t max_cap) {
    if (*size == 0) return;

    static const uint32_t interesting_u32[] = {
        0, 1, 2, 3, 4, 16, 32, 52, 64, 128, 255, 256, 512, 1024, 2048,
        0x7FFFu, 0x8000u, 0xFFFFu, 0x10000u,
        0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu,
        0x08800000u, 0x08804000u, 0x0C000000u,
        (uint32_t)sizeof(s_guest_arena), (uint32_t)sizeof(s_guest_arena) + 1u
    };
    size_t num_interesting = sizeof(interesting_u32) / sizeof(interesting_u32[0]);

    unsigned ops = (fuzz_rand32() % 6u) + 1u;
    for (unsigned op = 0; op < ops; op++) {
        unsigned kind = fuzz_rand32() % 7u;
        if (kind == 0 && *size > 0) {
            /* Bit flip */
            size_t idx = fuzz_rand32() % *size;
            data[idx] ^= (uint8_t)(1u << (fuzz_rand32() % 8u));
        } else if (kind == 1 && *size > 0) {
            /* Random byte set */
            size_t idx = fuzz_rand32() % *size;
            data[idx] = (uint8_t)fuzz_rand32();
        } else if (kind == 2 && *size + 4 <= max_cap) {
            /* Byte insertion (1-4 bytes) */
            size_t count = (fuzz_rand32() % 4u) + 1u;
            size_t idx = fuzz_rand32() % (*size + 1u);
            memmove(data + idx + count, data + idx, *size - idx);
            for (size_t c = 0; c < count; c++) {
                data[idx + c] = (uint8_t)fuzz_rand32();
            }
            *size += count;
        } else if (kind == 3 && *size > 4) {
            /* Byte deletion / truncation */
            if (fuzz_rand32() % 4u == 0) {
                /* Truncate buffer */
                *size = (fuzz_rand32() % (*size));
            } else {
                size_t count = (fuzz_rand32() % 4u) + 1u;
                if (count >= *size) count = 1;
                size_t idx = fuzz_rand32() % (*size - count + 1u);
                memmove(data + idx, data + idx + count, *size - (idx + count));
                *size -= count;
            }
        } else if (kind == 4 && *size >= 4) {
            /* 32-bit integer / offset / length boundary injection */
            size_t idx = fuzz_rand32() % (*size - 3u);
            uint32_t val = interesting_u32[fuzz_rand32() % num_interesting];
            if (fuzz_rand32() % 2u) {
                val ^= (uint32_t)*size;
            }
            data[idx + 0] = (uint8_t)(val & 0xFFu);
            data[idx + 1] = (uint8_t)((val >> 8) & 0xFFu);
            data[idx + 2] = (uint8_t)((val >> 16) & 0xFFu);
            data[idx + 3] = (uint8_t)((val >> 24) & 0xFFu);
        } else if (kind == 5 && *size >= 2) {
            /* 16-bit integer boundary injection */
            size_t idx = fuzz_rand32() % (*size - 1u);
            uint16_t val = (uint16_t)interesting_u32[fuzz_rand32() % num_interesting];
            data[idx + 0] = (uint8_t)(val & 0xFFu);
            data[idx + 1] = (uint8_t)((val >> 8) & 0xFFu);
        } else if (kind == 6 && *size > 0) {
            /* Zero-fill or 0xFF-fill a small run */
            size_t run_len = (fuzz_rand32() % 16u) + 1u;
            size_t idx = fuzz_rand32() % *size;
            if (idx + run_len > *size) run_len = *size - idx;
            uint8_t fill = (fuzz_rand32() % 2u) ? 0x00u : 0xFFu;
            memset(data + idx, fill, run_len);
        }
    }
}

/* -----------------------------------------------------------------------------
 * 1. PARAM.SFO Seed Builder & Harness
 * -------------------------------------------------------------------------- */
static size_t build_param_sfo(uint8_t *buf, size_t buf_cap) {
    assert(buf_cap >= 256);
    memset(buf, 0, buf_cap);

    /* PSF v1.1 Header (20 bytes) */
    buf[0] = 0x00; buf[1] = 'P'; buf[2] = 'S'; buf[3] = 'F';
    buf[4] = 0x01; buf[5] = 0x01; buf[6] = 0x00; buf[7] = 0x00;

    uint32_t key_table_off = 20u + 3u * 16u; /* 68 */
    const char *k0 = "DISC_ID";
    const char *k1 = "TITLE";
    const char *k2 = "DISC_VERSION";

    size_t k0_len = strlen(k0) + 1;
    size_t k1_len = strlen(k1) + 1;
    size_t k2_len = strlen(k2) + 1;

    uint32_t data_table_off = key_table_off + (uint32_t)(k0_len + k1_len + k2_len);
    /* Align data table to 4 bytes */
    data_table_off = (data_table_off + 3u) & ~3u;

    buf[8] = (uint8_t)key_table_off;
    buf[9] = (uint8_t)(key_table_off >> 8);
    buf[12] = (uint8_t)data_table_off;
    buf[13] = (uint8_t)(data_table_off >> 8);
    buf[16] = 3; /* entry count */

    const char *v0 = "TEST00001";
    const char *v1 = "Fuzz Test Title";
    const char *v2 = "1.00";

    uint32_t v0_len = (uint32_t)strlen(v0) + 1;
    uint32_t v1_len = (uint32_t)strlen(v1) + 1;
    uint32_t v2_len = (uint32_t)strlen(v2) + 1;

    /* Entry 0: DISC_ID */
    uint16_t key0_off = 0;
    buf[20] = (uint8_t)key0_off;
    buf[22] = 0x04; buf[23] = 0x02; /* fmt: UTF-8 string (0x0204) */
    buf[24] = (uint8_t)v0_len;
    buf[28] = 16; /* max len */
    buf[32] = 0;  /* data offset */

    /* Entry 1: TITLE */
    uint16_t key1_off = (uint16_t)k0_len;
    buf[36] = (uint8_t)key1_off;
    buf[38] = 0x04; buf[39] = 0x02;
    buf[40] = (uint8_t)v1_len;
    buf[44] = 64;
    buf[48] = (uint8_t)v0_len;

    /* Entry 2: DISC_VERSION */
    uint16_t key2_off = (uint16_t)(k0_len + k1_len);
    buf[52] = (uint8_t)key2_off;
    buf[54] = 0x04; buf[55] = 0x02;
    buf[56] = (uint8_t)v2_len;
    buf[60] = 16;
    buf[64] = (uint8_t)(v0_len + v1_len);

    /* Key table */
    memcpy(buf + key_table_off, k0, k0_len);
    memcpy(buf + key_table_off + k0_len, k1, k1_len);
    memcpy(buf + key_table_off + k0_len + k1_len, k2, k2_len);

    /* Data table */
    memcpy(buf + data_table_off, v0, v0_len);
    memcpy(buf + data_table_off + v0_len, v1, v1_len);
    memcpy(buf + data_table_off + v0_len + v1_len, v2, v2_len);

    return (size_t)data_table_off + v0_len + v1_len + v2_len;
}

static void test_fuzz_param_sfo(unsigned iters) {
    printf("[FUZZ] Testing PARAM.SFO in-memory parser (%u iterations)...\n", iters);
    fflush(stdout);

    uint8_t seed[512];
    size_t seed_size = build_param_sfo(seed, sizeof(seed));
    assert(seed_size > 0 && seed_size < sizeof(seed));

    /* Verify seed parses cleanly */
    NkIsoMetadata base_meta;
    memset(&base_meta, 0, sizeof(base_meta));
    assert(nk_iso_parse_sfo_buffer(seed, seed_size, &base_meta));
    assert(strcmp(base_meta.disc_id, "TEST00001") == 0);
    assert(strcmp(base_meta.title_name, "Fuzz Test Title") == 0);
    assert(strcmp(base_meta.disc_version, "1.00") == 0);

    uint8_t mutated[1024];
    unsigned accepted = 0;
    for (unsigned i = 0; i < iters; i++) {
        memcpy(mutated, seed, seed_size);
        size_t cur_size = seed_size;
        mutate_buffer(mutated, &cur_size, sizeof(mutated));

        NkIsoMetadata meta;
        memset(&meta, 0, sizeof(meta));
        if (nk_iso_parse_sfo_buffer(mutated, cur_size, &meta)) {
            accepted++;
        }
    }
    printf("[FUZZ] PARAM.SFO completed: %u/%u accepted, 0 crashes\n", accepted, iters);
    fflush(stdout);
}

/* -----------------------------------------------------------------------------
 * 2. ISO9660 Seed Builder & Harness
 * -------------------------------------------------------------------------- */
static size_t append_iso_record(uint8_t *record, const void *name, size_t name_len,
                                uint32_t lba, uint32_t size, bool is_dir) {
    size_t rec_len = 33 + name_len + ((name_len & 1u) == 0 ? 1u : 0u);
    memset(record, 0, rec_len);
    record[0] = (uint8_t)rec_len;
    /* LBA both-endian */
    record[2] = (uint8_t)lba;
    record[3] = (uint8_t)(lba >> 8);
    record[4] = (uint8_t)(lba >> 16);
    record[5] = (uint8_t)(lba >> 24);
    record[6] = (uint8_t)(lba >> 24);
    record[7] = (uint8_t)(lba >> 16);
    record[8] = (uint8_t)(lba >> 8);
    record[9] = (uint8_t)lba;
    /* Size both-endian */
    record[10] = (uint8_t)size;
    record[11] = (uint8_t)(size >> 8);
    record[12] = (uint8_t)(size >> 16);
    record[13] = (uint8_t)(size >> 24);
    record[14] = (uint8_t)(size >> 24);
    record[15] = (uint8_t)(size >> 16);
    record[16] = (uint8_t)(size >> 8);
    record[17] = (uint8_t)size;
    record[25] = is_dir ? 2u : 0u;
    record[28] = 1;
    record[31] = 1;
    record[32] = (uint8_t)name_len;
    memcpy(record + 33, name, name_len);
    return rec_len;
}

static size_t build_fuzz_iso(uint8_t *buf, size_t cap) {
    const size_t sector_size = 2048;
    const size_t total_sectors = 19;
    const size_t total_bytes = total_sectors * sector_size;
    assert(cap >= total_bytes);
    memset(buf, 0, total_bytes);

    /* Sector 16: Primary Volume Descriptor (PVD) */
    uint8_t *pvd = buf + 16u * sector_size;
    pvd[0] = 0x01;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 0x01;
    memcpy(pvd + 40, "FUZZ_ISO_VOLUME                 ", 32);

    /* Root directory extent is sector 17, size 2048 */
    pvd[158] = 17; pvd[159] = 0; pvd[160] = 0; pvd[161] = 0;
    pvd[162] = 0; pvd[163] = 0; pvd[164] = 0; pvd[165] = 17;
    pvd[166] = 0x00; pvd[167] = 0x08; pvd[168] = 0; pvd[169] = 0;
    pvd[170] = 0; pvd[171] = 0; pvd[172] = 0x08; pvd[173] = 0x00;

    /* Root directory record embedded in PVD at offset 156 */
    const uint8_t dot = 0;
    append_iso_record(pvd + 156, &dot, 1, 17, 2048, true);

    /* Sector 17: Root directory */
    uint8_t *root = buf + 17u * sector_size;
    const uint8_t dotdot = 1;
    size_t off = 0;
    off += append_iso_record(root + off, &dot, 1, 17, 2048, true);
    off += append_iso_record(root + off, &dotdot, 1, 17, 2048, true);
    off += append_iso_record(root + off, "PSP_GAME", 8, 17, 2048, true);

    /* SFO in sector 18 */
    uint8_t sfo_temp[512];
    size_t sfo_size = build_param_sfo(sfo_temp, sizeof(sfo_temp));
    append_iso_record(root + off, "PARAM.SFO;1", 11, 18, (uint32_t)sfo_size, false);

    /* Sector 18: PARAM.SFO content */
    memcpy(buf + 18u * sector_size, sfo_temp, sfo_size);

    return total_bytes;
}

static void test_fuzz_iso(unsigned iters) {
    printf("[FUZZ] Testing ISO9660 filesystem parser (%u iterations)...\n", iters);
    fflush(stdout);

    size_t cap = 20 * 2048;
    uint8_t *seed = (uint8_t *)malloc(cap);
    uint8_t *mutated = (uint8_t *)malloc(cap * 2);
    assert(seed && mutated);

    size_t seed_size = build_fuzz_iso(seed, cap);
    const char *iso_path = "build/fuzz_test_temp.iso";

    write_file_bytes(iso_path, seed, seed_size);

    /* Verify valid seed */
    NkIsoMetadata base_meta;
    memset(&base_meta, 0, sizeof(base_meta));
    assert(nk_iso_inspect(iso_path, &base_meta) == NK_OK);
    assert(strcmp(base_meta.disc_id, "TEST00001") == 0);

    NkIsoReader *reader = nk_iso_reader_open(iso_path);
    assert(reader != NULL);
    uint32_t lba = 0, sz = 0;
    bool is_dir = false;
    assert(nk_iso_reader_lookup(reader, "PARAM.SFO", &lba, &sz, &is_dir) == 0);
    assert(lba == 18);
    nk_iso_reader_close(reader);

    unsigned accepted = 0;
    for (unsigned i = 0; i < iters; i++) {
        memcpy(mutated, seed, seed_size);
        size_t cur_size = seed_size;
        mutate_buffer(mutated, &cur_size, cap * 2);

        write_file_bytes(iso_path, mutated, cur_size);

        NkIsoMetadata meta;
        memset(&meta, 0, sizeof(meta));
        if (nk_iso_inspect(iso_path, &meta) == NK_OK) {
            accepted++;
        }

        reader = nk_iso_reader_open(iso_path);
        if (reader) {
            uint32_t test_lba = 0, test_sz = 0;
            bool test_is_dir = false;
            nk_iso_reader_lookup(reader, "PARAM.SFO", &test_lba, &test_sz, &test_is_dir);
            nk_iso_reader_lookup(reader, "PSP_GAME/PARAM.SFO", &test_lba, &test_sz, &test_is_dir);
            nk_iso_reader_lookup(reader, "NONEXISTENT", &test_lba, &test_sz, &test_is_dir);

            NkIsoDirEntry de;
            nk_iso_reader_list(reader, "", 0, &de);
            nk_iso_reader_list(reader, "PSP_GAME", 0, &de);

            uint8_t read_buf[2048];
            (void)nk_iso_reader_read(reader, 16, 0, read_buf, sizeof(read_buf));
            nk_iso_reader_close(reader);
        }
    }

    remove(iso_path);
    free(seed);
    free(mutated);
    printf("[FUZZ] ISO9660 completed: %u/%u inspected OK, 0 crashes\n", accepted, iters);
    fflush(stdout);
}

static size_t build_fuzz_elf(uint8_t *buf, size_t cap) {
    assert(cap >= 256);
    memset(buf, 0, cap);
    memcpy(buf, "\x7F" "ELF", 4);
    buf[4] = 1;
    buf[5] = 1;
    buf[6] = 1;
    put16le(buf + 16, 1u);
    put16le(buf + 18, 8u);
    put32le(buf + 20, 1u);
    put32le(buf + 24, 0x08800000u);
    put32le(buf + 28, 52u);
    put32le(buf + 32, 160u);
    put16le(buf + 40, 52u);
    put16le(buf + 42, 32u);
    put16le(buf + 44, 1u);
    put16le(buf + 46, 40u);
    put16le(buf + 48, 1u);
    uint8_t *ph = buf + 52;
    put32le(ph + 0, 1u);
    put32le(ph + 4, 84u);
    put32le(ph + 8, 0x08800000u);
    put32le(ph + 12, 0x08800000u);
    put32le(ph + 16, 4u);
    put32le(ph + 20, 4u);
    put32le(ph + 24, 1u);
    put32le(ph + 28, 4u);
    return 200u;
}

static size_t build_fuzz_classifier_iso(uint8_t *buf, size_t cap,
                                         const uint8_t *elf, size_t elf_size) {
    const size_t sector_size = 2048u;
    const size_t total_bytes = 22u * sector_size;
    assert(cap >= total_bytes && elf_size > 0 && elf_size <= sector_size);
    memset(buf, 0, total_bytes);

    uint8_t *pvd = buf + 16u * sector_size;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memset(pvd + 40, ' ', 32);
    memcpy(pvd + 40, "FUZZ_CLASSIFIER_VOLUME", 20);
    pvd[158] = 17;
    pvd[162] = 0;
    pvd[166] = 0;
    pvd[167] = 8;
    pvd[170] = 0;
    pvd[172] = 8;
    const uint8_t dot = 0;
    append_iso_record(pvd + 156, &dot, 1, 17, 2048, true);

    uint8_t *root = buf + 17u * sector_size;
    const uint8_t dotdot = 1;
    size_t off = 0;
    off += append_iso_record(root + off, &dot, 1, 17, 2048, true);
    off += append_iso_record(root + off, &dotdot, 1, 17, 2048, true);
    off += append_iso_record(root + off, "PSP_GAME", 8, 18, 2048, true);

    uint8_t *game = buf + 18u * sector_size;
    off = 0;
    off += append_iso_record(game + off, &dot, 1, 18, 2048, true);
    off += append_iso_record(game + off, &dotdot, 1, 17, 2048, true);
    off += append_iso_record(game + off, "SYSDIR", 6, 19, 2048, true);

    uint8_t *sysdir = buf + 19u * sector_size;
    off = 0;
    off += append_iso_record(sysdir + off, &dot, 1, 19, 2048, true);
    off += append_iso_record(sysdir + off, &dotdot, 1, 18, 2048, true);
    off += append_iso_record(sysdir + off, "EBOOT.BIN", 9, 20, (uint32_t)elf_size, false);
    off += append_iso_record(sysdir + off, "BOOT.BIN", 8, 21, (uint32_t)elf_size, false);
    memcpy(buf + 20u * sector_size, elf, elf_size);
    memcpy(buf + 21u * sector_size, elf, elf_size);
    return total_bytes;
}

static void test_fuzz_elf_classifier(unsigned iters) {
    printf("[FUZZ] Testing ELF32/MIPS executable classifier (%u iterations)...\n", iters);
    fflush(stdout);

    uint8_t elf_seed[256];
    size_t elf_size = build_fuzz_elf(elf_seed, sizeof(elf_seed));
    size_t iso_size = 22u * 2048u;
    uint8_t *iso_seed = (uint8_t *)malloc(iso_size);
    uint8_t *iso_mutated = (uint8_t *)malloc(iso_size);
    uint8_t elf_mutated[512];
    assert(iso_seed && iso_mutated);

    build_fuzz_classifier_iso(iso_seed, iso_size, elf_seed, elf_size);
    const char *iso_path = "build/fuzz_classifier_temp.iso";
    write_file_bytes(iso_path, iso_seed, iso_size);
    NkIsoExecutableReport report;
    assert(nk_iso_classify_executables(iso_path, &report) == NK_OK);
    assert(report.selected == NK_ISO_EXEC_SELECTION_EBOOT);
    assert(report.eboot.kind == NK_ISO_EXEC_MIPS_ELF32);

    unsigned selected = 0;
    for (unsigned i = 0; i < iters; i++) {
        memcpy(elf_mutated, elf_seed, elf_size);
        size_t cur_size = elf_size;
        mutate_elf_field(elf_mutated, cur_size);
        mutate_buffer(elf_mutated, &cur_size, sizeof(elf_mutated));
        memcpy(iso_mutated, iso_seed, iso_size);
        uint32_t extent_size = cur_size < 2048u ? (uint32_t)cur_size : 2048u;
        memcpy(iso_mutated + 20u * 2048u, elf_mutated, extent_size);
        uint8_t *eboot_size = iso_mutated + 19u * 2048u + 68u + 10u;
        put32_iso_both(eboot_size, extent_size);
        write_file_bytes(iso_path, iso_mutated, iso_size);

        assert(nk_iso_classify_executables(iso_path, &report) == NK_OK);
        if (report.selected == NK_ISO_EXEC_SELECTION_EBOOT) {
            assert(report.eboot.kind == NK_ISO_EXEC_MIPS_ELF32 && !report.boot_fallback);
            assert(strcmp(report.selected_path, "EBOOT.BIN") == 0);
            selected++;
        } else if (report.selected == NK_ISO_EXEC_SELECTION_BOOT) {
            assert(report.eboot.kind == NK_ISO_EXEC_PSP_ENCRYPTED);
            assert(report.boot.kind == NK_ISO_EXEC_MIPS_ELF32 && report.boot_fallback);
            assert(strcmp(report.selected_path, "BOOT.BIN") == 0);
            selected++;
        } else {
            assert(report.selected == NK_ISO_EXEC_SELECTION_NONE);
        }
    }

    remove(iso_path);
    free(iso_seed);
    free(iso_mutated);
    printf("[FUZZ] ELF classifier completed: %u/%u selected, 0 crashes\n", selected, iters);
    fflush(stdout);
}

/* -----------------------------------------------------------------------------
 * 3. XB Archive Seed Builder & Harness
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} FuzzByteBuffer;

static void bb_reserve(FuzzByteBuffer *b, size_t extra) {
    if (b->size + extra <= b->cap) return;
    size_t new_cap = b->cap ? b->cap * 2 : 128;
    while (new_cap < b->size + extra) new_cap *= 2;
    b->data = (uint8_t *)realloc(b->data, new_cap);
    assert(b->data);
    b->cap = new_cap;
}

static void bb_append(FuzzByteBuffer *b, const void *p, size_t n) {
    bb_reserve(b, n);
    if (n) memcpy(b->data + b->size, p, n);
    b->size += n;
}

static void bb_u32(FuzzByteBuffer *b, uint32_t v, bool be) {
    uint8_t bytes[4] = {
        (uint8_t)(be ? v >> 24 : v),
        (uint8_t)(be ? v >> 16 : v >> 8),
        (uint8_t)(be ? v >> 8 : v >> 16),
        (uint8_t)(be ? v : v >> 24)
    };
    bb_append(b, bytes, 4);
}

static void bb_pad4(FuzzByteBuffer *b) {
    static const uint8_t zeros[4] = { 0 };
    size_t pad = (4u - (b->size & 3u)) & 3u;
    bb_append(b, zeros, pad);
}

static uint8_t sjis_hash_byte(const uint8_t *p, size_t n) {
    uint8_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v = (uint8_t)((((v & 0x7fu) << 1) | ((v & 0x80u) >> 7)) ^ p[i]);
    }
    return v;
}

static FuzzByteBuffer make_xb_archive(const char *rel_path, const uint8_t *payload,
                                      size_t payload_len, NkXbCompression comp,
                                      bool be) {
    FuzzByteBuffer names = { 0 };
    size_t path_len = strlen(rel_path);
    uint8_t len_byte = (uint8_t)path_len;
    uint8_t hash_byte = sjis_hash_byte((const uint8_t *)rel_path, path_len);
    bb_append(&names, &len_byte, 1);
    bb_append(&names, &hash_byte, 1);
    bb_append(&names, rel_path, path_len);
    uint8_t zero = 0;
    bb_append(&names, &zero, 1);

    FuzzByteBuffer string_sec = { 0 };
    bb_u32(&string_sec, (uint32_t)names.size, be);
    bb_u32(&string_sec, 0, be);
    bb_append(&string_sec, names.data, names.size);
    bb_pad4(&string_sec);
    free(names.data);

    FuzzByteBuffer body = { 0 };
    if (comp == NK_XB_COMPRESSION_NONE) {
        bb_append(&body, payload, payload_len);
        bb_pad4(&body);
    } else {
        /* Prefix 8-byte header: expanded_size, compressed_size */
        bb_u32(&body, (uint32_t)payload_len, be);
        bb_u32(&body, (uint32_t)payload_len, be);
        bb_append(&body, payload, payload_len);
        bb_pad4(&body);
    }

    size_t data_start = 8 + 1 * 8 + string_sec.size;
    assert((data_start & 3u) == 0);
    uint32_t packed = ((uint32_t)comp << 28) | (uint32_t)(data_start / 4);

    FuzzByteBuffer fst = { 0 };
    bb_u32(&fst, (uint32_t)payload_len, be);
    bb_u32(&fst, packed, be);

    FuzzByteBuffer out = { 0 };
    static const uint8_t sig[4] = { 0x78, 0x65, 0x00, 0x01 };
    bb_append(&out, sig, 4);
    bb_u32(&out, 1, be); /* entry count */
    bb_append(&out, fst.data, fst.size);
    bb_append(&out, string_sec.data, string_sec.size);
    bb_append(&out, body.data, body.size);

    free(fst.data);
    free(string_sec.data);
    free(body.data);
    return out;
}

static void test_fuzz_xb(unsigned iters) {
    printf("[FUZZ] Testing XB archive in-memory parser (%u iterations)...\n", iters);
    fflush(stdout);

    static const uint8_t item_data[] = "synthetic fuzz archive entry payload";
    FuzzByteBuffer seed = make_xb_archive("data/fuzz.bin", item_data,
                                          sizeof(item_data) - 1,
                                          NK_XB_COMPRESSION_NONE, false);

    /* Verify clean open */
    NkXbArchive base_arch;
    char err[256];
    assert(nk_xb_open_memory(seed.data, seed.size, "seed.xb", false, NULL,
                             &base_arch, err, sizeof(err)) == NK_OK);
    assert(base_arch.entry_count == 1);
    uint8_t out[128];
    size_t out_sz = 0;
    assert(nk_xb_read_entry(&base_arch, 0, out, sizeof(out), &out_sz, err, sizeof(err)) == NK_OK);
    assert(out_sz == sizeof(item_data) - 1);
    nk_xb_close(&base_arch);

    uint8_t mutated[2048];
    unsigned accepted = 0;
    for (unsigned i = 0; i < iters; i++) {
        assert(seed.size <= sizeof(mutated));
        memcpy(mutated, seed.data, seed.size);
        size_t cur_size = seed.size;
        mutate_buffer(mutated, &cur_size, sizeof(mutated));

        bool be = (i & 1) != 0;
        NkXbArchive archive;
        if (nk_xb_open_memory(mutated, cur_size, "mutated.xb", be, NULL,
                              &archive, err, sizeof(err)) == NK_OK) {
            accepted++;
            for (size_t e = 0; e < archive.entry_count; e++) {
                uint8_t buf[256];
                size_t sz = 0;
                (void)nk_xb_read_entry(&archive, e, buf, sizeof(buf), &sz, err, sizeof(err));
            }
            nk_xb_close(&archive);
        }
    }

    free(seed.data);
    printf("[FUZZ] XB archive completed: %u/%u accepted, 0 crashes\n", accepted, iters);
    fflush(stdout);
}

/* -----------------------------------------------------------------------------
 * 4. PRX Loader / ELF / ~PSP Seed Builder & Harness
 * -------------------------------------------------------------------------- */
static size_t build_fuzz_prx(uint8_t *buf, size_t cap) {
    assert(cap >= 512);
    memset(buf, 0, cap);

    /* ELF Header (52 bytes) */
    buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 1; /* ELF32 */
    buf[5] = 1; /* LE */
    buf[6] = 1; /* EV_CURRENT */
    /* e_type = ET_PSP (0xFFA0) */
    buf[16] = 0xA0; buf[17] = 0xFF;
    /* e_machine = EM_MIPS (8) */
    buf[18] = 8; buf[19] = 0;
    /* e_version = 1 */
    buf[20] = 1;
    /* e_entry = 0x08804000 */
    buf[24] = 0x00; buf[25] = 0x40; buf[26] = 0x80; buf[27] = 0x08;
    /* e_phoff = 52 */
    buf[28] = 52;
    /* e_ehsize = 52 */
    buf[40] = 52;
    /* e_phentsize = 32 */
    buf[42] = 32;
    /* e_phnum = 2 (PT_LOAD, PT_REL_A) */
    buf[44] = 2;

    /* Program Header 0: PT_LOAD at offset 52 */
    uint32_t seg_file_off = 52u + 64u; /* 116 */
    uint32_t seg_vaddr = 0;
    uint32_t modinfo_file_off = seg_file_off + 16u; /* p_paddr points here */
    uint32_t seg_filesz = 128;
    uint32_t seg_memsz = 128;

    uint8_t *ph0 = buf + 52;
    ph0[0] = 1; /* PT_LOAD */
    ph0[4] = (uint8_t)seg_file_off;
    ph0[8] = (uint8_t)seg_vaddr;
    ph0[12] = (uint8_t)modinfo_file_off; /* p_paddr */
    ph0[16] = (uint8_t)seg_filesz;
    ph0[20] = (uint8_t)seg_memsz;
    ph0[24] = 7; /* PF_R | PF_W | PF_X */
    ph0[28] = 4; /* p_align */

    /* Program Header 1: PT_REL_A at offset 84 */
    uint32_t rel_file_off = seg_file_off + seg_filesz; /* 244 */
    uint32_t rel_filesz = 16; /* 2 reloc records */

    uint8_t *ph1 = buf + 84;
    ph1[0] = 0xA0; ph1[1] = 0x00; ph1[2] = 0x00; ph1[3] = 0x70; /* PT_REL_A */
    ph1[4] = (uint8_t)rel_file_off;
    ph1[16] = (uint8_t)rel_filesz;
    ph1[20] = (uint8_t)rel_filesz;

    /* SceModuleInfo at modinfo_file_off (52 bytes) */
    uint8_t *modinfo = buf + modinfo_file_off;
    modinfo[2] = 1; modinfo[3] = 0; /* version 1.0 */
    memcpy(modinfo + 4, "fuzz_module", 12);
    /* exp_start = 0, exp_end = 0, imp_start = 0, imp_end = 0 */

    /* Relocation stream at rel_file_off: HI16 (kind 5) + LO16 (kind 6) */
    uint8_t *rel = buf + rel_file_off;
    /* Record 0: HI16 at offset 0 of seg 0 targeting seg 0 */
    rel[0] = 0; /* offset */
    rel[4] = 5; /* kind 5 */
    /* Record 1: LO16 at offset 4 of seg 0 targeting seg 0 */
    rel[8] = 4;
    rel[12] = 6; /* kind 6 */

    return rel_file_off + rel_filesz;
}

static size_t build_fuzz_prx_b(uint8_t *buf, size_t cap) {
    assert(cap >= 512);
    memset(buf, 0, cap);
    memcpy(buf, "\x7F" "ELF", 4);
    buf[4] = 1;
    buf[5] = 1;
    buf[6] = 1;
    put16le(buf + 16, 0xFFA0u);
    put16le(buf + 18, 8u);
    put32le(buf + 20, 1u);
    put32le(buf + 24, 52u);
    put32le(buf + 28, 52u);
    put16le(buf + 40, 52u);
    put16le(buf + 42, 32u);
    put16le(buf + 44, 2u);

    uint32_t seg_off = 116u;
    uint8_t *ph0 = buf + 52;
    put32le(ph0 + 0, 1u);
    put32le(ph0 + 4, seg_off);
    put32le(ph0 + 8, 0x1000u);
    put32le(ph0 + 12, seg_off);
    put32le(ph0 + 16, 64u);
    put32le(ph0 + 20, 64u);
    put32le(ph0 + 24, 7u);
    put32le(ph0 + 28, 4u);

    uint32_t rel_off = seg_off + 64u;
    uint32_t rel_size = 17u;
    uint8_t *ph1 = buf + 84;
    put32le(ph1 + 0, 0x700000A1u);
    put32le(ph1 + 4, rel_off);
    put32le(ph1 + 16, rel_size);
    put32le(ph1 + 20, rel_size);

    uint8_t *modinfo = buf + seg_off;
    modinfo[2] = 1;
    memcpy(modinfo + 4, "fuzz_b_mod", 10);
    put32le(buf + seg_off + 52u, 0x12345678u);

    uint8_t *rel = buf + rel_off;
    static const uint8_t header[] = {0, 0, 3, 2, 3, 4, 1, 2, 2};
    memcpy(rel, header, sizeof(header));
    put16le(rel + sizeof(header), (uint16_t)(1u | (52u << 3)));
    put32le(rel + sizeof(header) + 2u, 52u);
    put16le(rel + sizeof(header) + 6u, (uint16_t)(2u | (1u << 4)));
    return rel_off + rel_size;
}

static size_t build_fuzz_prx_sectioned(uint8_t *buf, size_t cap) {
    assert(cap >= 1024);
    memset(buf, 0, cap);
    memcpy(buf, "\x7F" "ELF", 4);
    buf[4] = 1;
    buf[5] = 1;
    buf[6] = 1;
    put16le(buf + 16, 0xFFA0u);
    put16le(buf + 18, 8u);
    put32le(buf + 20, 1u);
    put32le(buf + 24, 52u);
    put32le(buf + 28, 52u);
    put16le(buf + 40, 52u);
    put16le(buf + 42, 32u);
    put16le(buf + 44, 1u);
    put16le(buf + 46, 40u);
    put16le(buf + 48, 4u);
    put16le(buf + 50, 3u);

    uint32_t seg_off = 116u;
    uint8_t *ph = buf + 52;
    put32le(ph + 0, 1u);
    put32le(ph + 4, seg_off);
    put32le(ph + 8, 0x1000u);
    put32le(ph + 12, seg_off);
    put32le(ph + 16, 60u);
    put32le(ph + 20, 60u);
    put32le(ph + 24, 7u);
    put32le(ph + 28, 4u);

    uint8_t *modinfo = buf + seg_off;
    modinfo[2] = 1;
    memcpy(modinfo + 4, "fuzz_sectioned", 15);
    put32le(buf + seg_off + 52u, 0x11111111u);
    put32le(buf + seg_off + 56u, 0x22222222u);

    uint32_t rel_off = seg_off + 60u;
    uint8_t *rel = buf + rel_off;
    put32le(rel + 0, 52u);
    put32le(rel + 4, 5u);
    put32le(rel + 8, 56u);
    put32le(rel + 12, 6u);

    uint32_t shstr_off = rel_off + 16u;
    uint8_t *shstr = buf + shstr_off;
    uint32_t info_name = 1u;
    uint32_t reloc_name = info_name + 22u;
    uint32_t shstr_name = reloc_name + 7u;
    memcpy(shstr + info_name, ".rodata.sceModuleInfo", 21);
    memcpy(shstr + reloc_name, ".reloc", 6);
    memcpy(shstr + shstr_name, ".shstrtab", 9);
    uint32_t shstr_size = shstr_name + 10u;

    uint32_t shoff = shstr_off + shstr_size;
    put32le(buf + 32, shoff);
    uint8_t *sh = buf + shoff;
    put32le(sh + 40, 1u);
    put32le(sh + 44, 1u);
    put32le(sh + 52, 0x1000u);
    put32le(sh + 56, seg_off);
    put32le(sh + 60, 52u);
    put32le(sh + 80, reloc_name);
    put32le(sh + 84, 0x700000A0u);
    put32le(sh + 92, 0x1000u);
    put32le(sh + 96, rel_off);
    put32le(sh + 100, 16u);
    put32le(sh + 120, shstr_name);
    put32le(sh + 124, 3u);
    put32le(sh + 136, shstr_off);
    put32le(sh + 140, shstr_size);
    return shoff + 4u * 40u;
}

static void mutate_elf_field(uint8_t *data, size_t size) {
    static const size_t offsets[] = {
        16, 18, 20, 24, 28, 32, 40, 42, 44, 46, 48, 50,
        52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96
    };
    static const uint32_t values[] = {
        0, 1, 2, 4, 8, 32, 40, 52, 64, 128, 200, 256,
        0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu
    };
    size_t offset = offsets[fuzz_rand32() % (sizeof(offsets) / sizeof(offsets[0]))];
    uint32_t value = values[fuzz_rand32() % (sizeof(values) / sizeof(values[0]))];
    if (offset + 4 <= size) put32le(data + offset, value);
    else if (offset + 2 <= size) put16le(data + offset, (uint16_t)value);
}

static size_t build_fuzz_psp_container(uint8_t *buf, size_t cap) {
    assert(cap >= 128);
    memset(buf, 0, cap);
    memcpy(buf, "~PSP", 4);
    buf[4] = 0x01; buf[5] = 0x00;
    buf[0x27] = 1; /* 1 segment */
    /* BSS size at 0x38 = 0x100 */
    buf[0x38] = 0x00; buf[0x39] = 0x01;
    /* Segment 0 size at 0x54 = 0x1000 */
    buf[0x54] = 0x00; buf[0x55] = 0x10;
    return 128;
}

static void test_fuzz_prx(unsigned iters) {
    printf("[FUZZ] Testing PRX A/B/section walkers and ~PSP container (%u iterations)...\n", iters);
    fflush(stdout);

    uint8_t seed_a[1024], seed_b[1024], seed_section[1024], seed_psp[256];
    size_t size_a = build_fuzz_prx(seed_a, sizeof(seed_a));
    size_t size_b = build_fuzz_prx_b(seed_b, sizeof(seed_b));
    size_t size_section = build_fuzz_prx_sectioned(seed_section, sizeof(seed_section));
    size_t size_psp = build_fuzz_psp_container(seed_psp, sizeof(seed_psp));

    SrPrxImage base_img;
    char err[256];
    reset_guest_arena();
    int rc = sr_prx_load_from_memory(seed_a, size_a, 0x08800000u,
                                     &base_img, err, sizeof(err));
    assert(rc == 0 && strcmp(base_img.modname, "fuzz_module") == 0);
    sr_prx_image_free(&base_img);

    reset_guest_arena();
    rc = sr_prx_load_from_memory(seed_b, size_b, 0x08800000u,
                                 &base_img, err, sizeof(err));
    assert(rc == 0 && strcmp(base_img.modname, "fuzz_b_mod") == 0);
    assert(get32le(s_guest_arena + 0x1000u + 52u) == 0x12345678u + 0x08801000u);
    sr_prx_image_free(&base_img);

    reset_guest_arena();
    rc = sr_prx_load_from_memory(seed_section, size_section, 0x08800000u,
                                 &base_img, err, sizeof(err));
    if (rc != 0 || strcmp(base_img.modname, "fuzz_sectioned") != 0) {
        fprintf(stderr, "sectioned PRX seed failed: %s\n", err);
    }
    assert(rc == 0 && strcmp(base_img.modname, "fuzz_sectioned") == 0);
    sr_prx_image_free(&base_img);

    reset_guest_arena();
    rc = sr_prx_load_from_memory(seed_psp, size_psp, 0x08800000u,
                                 &base_img, err, sizeof(err));
    assert(rc != 0 && strstr(err, "~PSP") != NULL);
    sr_prx_image_free(&base_img);

    uint8_t bomb[1024];
    memcpy(bomb, seed_a, size_a);
    put32le(bomb + 72, UINT32_MAX);
    reset_guest_arena();
    rc = sr_prx_load_from_memory(bomb, size_a, 0x08800000u,
                                 &base_img, err, sizeof(err));
    assert(rc != 0);
    assert(strstr(err, "image exceeds 64 MiB") != NULL);
    assert(guest_arena_is_guard());
    sr_prx_image_free(&base_img);

    uint8_t mutated[2048];
    unsigned accepted = 0;
    for (unsigned i = 0; i < iters; i++) {
        const uint8_t *chosen_seed = seed_a;
        size_t chosen_size = size_a;
        switch (i % 4u) {
            case 1:
                chosen_seed = seed_b;
                chosen_size = size_b;
                break;
            case 2:
                chosen_seed = seed_section;
                chosen_size = size_section;
                break;
            default:
                if (i % 4u != 3u) {
                    chosen_seed = seed_a;
                    chosen_size = size_a;
                } else {
                    chosen_seed = seed_psp;
                    chosen_size = size_psp;
                }
                break;
        }

        memcpy(mutated, chosen_seed, chosen_size);
        size_t cur_size = chosen_size;
        mutate_elf_field(mutated, cur_size);
        mutate_buffer(mutated, &cur_size, sizeof(mutated));

        SrPrxImage img;
        reset_guest_arena();
        rc = sr_prx_load_from_memory(mutated, cur_size, 0x08800000u,
                                     &img, err, sizeof(err));
        if (rc == 0) accepted++;
        else if (strstr(err, "guest write failed") == NULL) assert(guest_arena_is_guard());
        sr_prx_image_free(&img);
    }

    printf("[FUZZ] PRX / ~PSP loader completed: %u/%u accepted, 0 crashes\n", accepted, iters);
    fflush(stdout);
}

/* -----------------------------------------------------------------------------
 * 5. Title Manifest JSON Seed Builder & Harness
 * -------------------------------------------------------------------------- */
static const char s_valid_manifest_json[] =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"id\": \"fuzz-test-title\",\n"
    "  \"display_name\": \"Fuzz Test Title\",\n"
    "  \"kind\": \"synthetic\",\n"
    "  \"executable\": {\"base\": 0, \"entry\": 142606336, \"bss_metadata_source\": \"elf\", \"extra_executable_spans\": []},\n"
    "  \"modules\": [{\"name\": \"main.prx\", \"load_address\": 146800640, \"required\": true, \"role\": \"guest-prx\"}],\n"
    "  \"filesystem\": {\"data_root\": \"data\", \"memory_stick_root\": \"ms\", \"device_prefixes\": [\"host0:\"]},\n"
    "  \"hle_profile\": \"standard\",\n"
    "  \"feature_requirements\": [\"allegrex\"],\n"
    "  \"verification_profile\": \"smoke\"\n"
    "}";

static void test_fuzz_manifest(unsigned iters) {
    printf("[FUZZ] Testing Title Manifest JSON parser (%u iterations)...\n", iters);
    fflush(stdout);

    size_t seed_size = strlen(s_valid_manifest_json);
    NkTitleEntry base_entry;
    char err[256];
    assert(nk_title_manifest_parse_buffer(s_valid_manifest_json, seed_size,
                                          true, &base_entry, err, sizeof(err)));
    assert(strcmp(base_entry.id, "fuzz-test-title") == 0);

    uint8_t mutated[4096];
    unsigned accepted = 0;
    for (unsigned i = 0; i < iters; i++) {
        memcpy(mutated, s_valid_manifest_json, seed_size);
        size_t cur_size = seed_size;
        mutate_buffer(mutated, &cur_size, sizeof(mutated));

        NkTitleEntry entry;
        if (nk_title_manifest_parse_buffer((const char *)mutated, cur_size,
                                           true, &entry, err, sizeof(err))) {
            accepted++;
        }
    }

    printf("[FUZZ] Title Manifest JSON completed: %u/%u accepted, 0 crashes\n", accepted, iters);
    fflush(stdout);
}

typedef struct {
    uint32_t state[8];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
} FuzzSha256;

static uint32_t fuzz_sha_rotr(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}

static void fuzz_sha_transform(FuzzSha256 *ctx, const uint8_t block[64]) {
    static const uint32_t round[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t words[64];
    for (size_t i = 0; i < 16; i++) {
        words[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) |
                   (uint32_t)block[i * 4 + 3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = fuzz_sha_rotr(words[i - 15], 7) ^ fuzz_sha_rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
        uint32_t s1 = fuzz_sha_rotr(words[i - 2], 17) ^ fuzz_sha_rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t s1 = fuzz_sha_rotr(e, 6) ^ fuzz_sha_rotr(e, 11) ^ fuzz_sha_rotr(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + choose + round[i] + words[i];
        uint32_t s0 = fuzz_sha_rotr(a, 2) ^ fuzz_sha_rotr(a, 13) ^ fuzz_sha_rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + majority;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void fuzz_sha_init(FuzzSha256 *ctx) {
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bits = 0;
    ctx->used = 0;
}

static void fuzz_sha_update(FuzzSha256 *ctx, const uint8_t *data, size_t length) {
    ctx->bits += (uint64_t)length * 8u;
    while (length > 0) {
        size_t room = sizeof(ctx->block) - ctx->used;
        size_t take = length < room ? length : room;
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take;
        data += take;
        length -= take;
        if (ctx->used == sizeof(ctx->block)) {
            fuzz_sha_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void fuzz_sha_finish(FuzzSha256 *ctx, char out[65]) {
    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        memset(ctx->block + ctx->used, 0, sizeof(ctx->block) - ctx->used);
        fuzz_sha_transform(ctx, ctx->block);
        ctx->used = 0;
    }
    memset(ctx->block + ctx->used, 0, 56 - ctx->used);
    for (size_t i = 0; i < 8; i++) {
        ctx->block[63 - i] = (uint8_t)(ctx->bits >> (8u * i));
    }
    fuzz_sha_transform(ctx, ctx->block);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(ctx->state) / sizeof(ctx->state[0]); i++) {
        for (size_t j = 0; j < 4; j++) {
            uint8_t byte = (uint8_t)(ctx->state[i] >> (24u - 8u * j));
            out[i * 8 + j * 2] = hex[byte >> 4];
            out[i * 8 + j * 2 + 1] = hex[byte & 0x0f];
        }
    }
    out[64] = '\0';
}

static void fuzz_sha_file(const char *path, char out[65]) {
    FuzzSha256 ctx;
    fuzz_sha_init(&ctx);
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    uint8_t buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        fuzz_sha_update(&ctx, buffer, count);
    }
    assert(!ferror(file));
    assert(fclose(file) == 0);
    fuzz_sha_finish(&ctx, out);
}

static void test_fuzz_package(unsigned iters) {
    static const char fixture_sha[] =
        "f16d05ec6b29248d2c61adb1e9263f78e4f7bace1b955014a2d17872cfe4064d";
    static const char disc_id[] = "TEST00001";
    static const char title_id[] = "synthetic-allegrex-v1";
    static const char executable_name[] = "synthetic-allegrex-v1.exe";
    const char *root = "build/fuzz_package_root";
    char package_dir[512], package_path[640], report_path[640];
    char executable_path[768], image_path[768], absolute_root[1024];
    char package_json[8192], report_json[8192], cache_json[4096], cache_key_json[3072];
    char completion_json[4096], completion_path[768];
    char reason[512];

    snprintf(package_dir, sizeof(package_dir), "%s%cpackages%c%s", root,
             nk_platform_path_separator(), nk_platform_path_separator(), disc_id);
    assert(nk_platform_mkdir_p(package_dir));
    snprintf(package_path, sizeof(package_path), "%s%cpackage.json", package_dir,
             nk_platform_path_separator());
    snprintf(report_path, sizeof(report_path), "%s%cbuild-report.json", package_dir,
             nk_platform_path_separator());
    snprintf(executable_path, sizeof(executable_path), "%s%c%s", package_dir,
             nk_platform_path_separator(), executable_name);
    snprintf(image_path, sizeof(image_path), "%s%csynthetic-allegrex-v1_image.bin",
             package_dir, nk_platform_path_separator());
    assert(nk_platform_absolute_path(root, absolute_root, sizeof(absolute_root)));

    static const uint8_t fixture[] = "fixture";
    write_file_bytes(executable_path, fixture, sizeof(fixture) - 1u);
    write_file_bytes(image_path, fixture, sizeof(fixture) - 1u);
    const char *modules_digest =
        "37517e5f3dc66819f61f5a7bb8ace1921282415f10551d2defa5c3eb0985b570";
    const char *codegen_options_digest =
        "ca3d163bab055381827226140568f3bef7eaac187cebd76878e0b63e9e442356";
    const char *aot_digest =
        "e9937d6f8ce6f61a6039e79e815f179ef468e9d8434ecf0ed3bd90b683f2ccba";
    const char *native_digest =
        "f43a16301ce14295ebe90b2c4ba7d605d681d310f98eeab008b782f4ffdbb492";
    int cache_key_length = snprintf(cache_key_json, sizeof(cache_key_json),
        "{\"schema_version\":1,\"aot\":{\"digest\":\"%s\",\"components\":{"
        "\"analyzer_codegen_epoch\":\"analyzer-codegen-v1\","
        "\"analyzer_sha256\":\"%064d\",\"codegen_options_sha256\":\"%s\","
        "\"codegen_sha256\":\"%064d\",\"executable_sha256\":\"%s\","
        "\"generated_code_abi_epoch\":1,\"manifest_sha256\":\"%064d\","
        "\"modules_sha256\":\"%s\",\"psp_header_sha256\":null,"
        "\"runtime_abi_epoch\":1}},\"native\":{\"digest\":\"%s\","
        "\"components\":{\"compile_flags\":\"\",\"compiler_identity\":\"gcc-fixture\","
        "\"compiler_target\":\"fixture-target\",\"generated_code_digest\":\"%064d\","
        "\"link_flags\":\"\",\"runtime_abi_epoch\":1,\"runtime_source_digest\":\"%064d\"}}}",
        aot_digest, 0, codegen_options_digest, 0, fixture_sha, 0, modules_digest, native_digest, 0, 0);
    assert(cache_key_length > 0 && (size_t)cache_key_length < sizeof(cache_key_json));
    int cache_length = snprintf(cache_json, sizeof(cache_json),
        "{\"format\":\"nakagawa-aot-cache\",\"schema_version\":1,\"key\":%s,"
        "\"codegen_options\":{},\"runtime_abi_compatibility\":{"
        "\"current_epoch\":1,\"generated_code_reusable\":true}}",
        cache_key_json);
    assert(cache_length > 0 && (size_t)cache_length < sizeof(cache_json));
    int report_length = snprintf(report_json, sizeof(report_json),
        "{\"format\":\"nakagawa-build-report\",\"schema_version\":1,"
        "\"title_id\":\"%s\",\"runtime_abi\":{\"name\":\"CpuState\",\"version\":2},"
        "\"cache\":%s,"
        "\"input_hashes\":{\"manifest\":{\"sha256\":\"%064d\"},"
        "\"executable\":{\"sha256\":\"%s\"},\"modules\":[],\"psp_header\":null},"
        "\"tools\":{},\"coverage\":{},\"unsupported\":{\"imports\":[],"
        "\"instructions\":[],\"regions\":[]},\"analysis_diagnostics\":[],\"artifacts\":{}}\n",
        title_id, cache_json, 0, fixture_sha);
    assert(report_length > 0 && (size_t)report_length < sizeof(report_json));
    int package_length = snprintf(package_json, sizeof(package_json),
        "{\"format\":\"nakagawa-aot-package\",\"schema_version\":1,\"cache\":%s,"
        "\"title\":{\"id\":\"%s\",\"display_name\":\"Synthetic fixture\","
        "\"kind\":\"retail\",\"manifest_sha256\":\"%064d\","
        "\"protected_digest\":\"%064d\"},"
        "\"inputs\":{\"manifest\":{\"sha256\":\"%064d\"},"
        "\"executable\":{\"sha256\":\"%s\"},\"modules\":[],\"psp_header\":null},"
        "\"runtime\":{\"abi\":\"CpuState\",\"abi_version\":2,"
        "\"abi_header_sha256\":\"%064d\",\"run_entry\":\"0x00000000\","
        "\"runtime_contract\":null,\"runtime_bindings\":{},"
        "\"required_runtime_bindings\":[]},"
        "\"executable\":{\"path\":\"%s\",\"sha256\":\"%s\","
        "\"guest_entry\":\"0x00000000\"},\"generated_objects\":[],"
        "\"required_local_assets\":[],\"build_report\":\"build-report.json\"}\n",
        cache_json, title_id, 0, 0, 0, fixture_sha, 0, executable_name, fixture_sha);
    assert(package_length > 0 && (size_t)package_length < sizeof(package_json));
    write_file_bytes(package_path, package_json, (size_t)package_length);
    write_file_bytes(report_path, report_json, (size_t)report_length);
    char package_hash[65], report_hash[65];
    fuzz_sha_file(package_path, package_hash);
    fuzz_sha_file(report_path, report_hash);
    snprintf(completion_path, sizeof(completion_path), "%s%ccompletion-manifest.json",
             package_dir, nk_platform_path_separator());
    int completion_length = snprintf(completion_json, sizeof(completion_json),
        "{\"format\":\"nakagawa-aot-cache-completion\",\"schema_version\":1,"
        "\"status\":\"complete\",\"cache_key\":%s,\"artifacts\":["
        "{\"path\":\"package.json\",\"sha256\":\"%s\"},"
        "{\"path\":\"build-report.json\",\"sha256\":\"%s\"},"
        "{\"path\":\"%s\",\"sha256\":\"%s\"},"
        "{\"path\":\"synthetic-allegrex-v1_image.bin\",\"sha256\":\"%s\"}]}\n",
        cache_key_json, package_hash, report_hash, executable_name, fixture_sha, fixture_sha);
    assert(completion_length > 0 && (size_t)completion_length < sizeof(completion_json));
    write_file_bytes(completion_path, completion_json, (size_t)completion_length);

    NkRuntimePackageInfo info;
    assert(nk_title_manifest_validate_aot_package(
               absolute_root, disc_id, title_id, false, "EBOOT.BIN", 2,
               &info, reason, sizeof(reason)) == NK_RUNTIME_PACKAGE_OK);
    assert(info.package_root[0] != '\0');

    uint8_t mutated[4096];
    unsigned accepted = 0;
    for (unsigned i = 0; i < iters; i++) {
        bool mutate_package = (i & 1u) == 0;
        const char *seed = mutate_package ? package_json : report_json;
        size_t seed_size = mutate_package ? (size_t)package_length : (size_t)report_length;
        const char *path = mutate_package ? package_path : report_path;
        const char *valid_json = mutate_package ? report_json : package_json;
        const char *valid_path = mutate_package ? report_path : package_path;
        size_t valid_size = mutate_package ? (size_t)report_length : (size_t)package_length;

        memcpy(mutated, seed, seed_size);
        size_t cur_size = seed_size;
        mutate_buffer(mutated, &cur_size, sizeof(mutated));
        write_file_bytes(path, mutated, cur_size);
        write_file_bytes(valid_path, valid_json, valid_size);

        memset(&info, 0xA5, sizeof(info));
        NkRuntimePackageStatus status = nk_title_manifest_validate_aot_package(
            absolute_root, disc_id, title_id, false, "EBOOT.BIN", 2,
            &info, reason, sizeof(reason));
        if (status == NK_RUNTIME_PACKAGE_OK) {
            assert(info.package_root[0] != '\0');
            accepted++;
        } else {
            assert(info.package_root[0] == '\0');
            assert(info.executable_path[0] == '\0');
            assert(info.image_path[0] == '\0');
        }
    }

    remove(package_path);
    remove(report_path);
    remove(completion_path);
    remove(executable_path);
    remove(image_path);
    printf("[FUZZ] Package v1 JSON completed: %u/%u accepted, 0 crashes\n", accepted, iters);
    fflush(stdout);
}

/* -----------------------------------------------------------------------------
 * 6. Library JSON Seed Builder & Harness
 * -------------------------------------------------------------------------- */
static const char s_valid_library_json[] =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"games\": [\n"
    "    {\n"
    "      \"disc_id\": \"TEST00001\",\n"
    "      \"title_name\": \"Fuzz Test Title\",\n"
    "      \"disc_version\": \"1.00\",\n"
    "      \"is_prepared\": true,\n"
    "      \"status\": 1\n"
    "    }\n"
    "  ]\n"
    "}";

static void test_fuzz_library(unsigned iters) {
    printf("[FUZZ] Testing Library JSON parser (%u iterations)...\n", iters);
    fflush(stdout);

    const char *lib_path = "build/fuzz_test_library.json";
    size_t seed_size = strlen(s_valid_library_json);
    write_file_bytes(lib_path, s_valid_library_json, seed_size);

    NkLibrary base_lib;
    assert(nk_library_load(&base_lib, lib_path) == NK_OK);
    assert(nk_library_count(&base_lib) == 1);

    uint8_t mutated[2048];
    unsigned accepted = 0;
    for (unsigned i = 0; i < iters; i++) {
        memcpy(mutated, s_valid_library_json, seed_size);
        size_t cur_size = seed_size;
        mutate_buffer(mutated, &cur_size, sizeof(mutated));

        write_file_bytes(lib_path, mutated, cur_size);

        NkLibrary lib;
        if (nk_library_load(&lib, lib_path) == NK_OK) {
            accepted++;
        }
    }

    remove(lib_path);
    char bak_path[512];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", lib_path);
    remove(bak_path);

    printf("[FUZZ] Library JSON completed: %u/%u accepted, 0 crashes\n", accepted, iters);
    fflush(stdout);
}

/* -----------------------------------------------------------------------------
 * 7. Built-in PSP decryption boundary (issue #295, fuzz coverage for #319)
 *
 * Every parser the decryption boundary exposes to user-supplied bytes gets a
 * seeded mutation harness here: the container probe/framing (~PSP, ~SCE,
 * PBP), the ~PSP header parse, the KeyStore JSON loader, the
 * DEFLATE/zlib/gzip inflater, the KL4E/KL3E decoder and the KIRK command
 * dispatch on attacker-controlled headers.
 *
 * ALL SEEDS ARE SYNTHETIC and built in this file: a tiny source-owned
 * ELF32/MIPS image, a type-2 ~PSP container sealed around it with constant
 * fake keys (never key material), a synthetic KeyStore document, and
 * gzip/zlib/raw-deflate plus KL4E/KL3E streams produced by the small writers
 * below.  No retail, firmware or key bytes are involved.
 *
 * Ceilings asserted by these harnesses (the boundary's own limits):
 *   wrapper depth 8; ~PSP psp_size in [0x150, file size] and segments <= 4;
 *   KeyStore 64 KiB and 512 entries; inflate output <= the cap passed in
 *   (64 MiB in production, 256 KiB here); KL output <= the declared
 *   elf_size; KIRK data_size <= size - header and 16-byte aligned.
 * -------------------------------------------------------------------------- */
#define FUZZ_PSP_TAG 0x5EED3191u /* obviously synthetic; no retail meaning */
#define FUZZ_PSP_CODE 67         /* synthetic KIRK keyvault slot */
#define FUZZ_KEYSTORE_FORMAT "nakagawa-psp-keystore-1"
#define FUZZ_INFLATE_CAP (256u * 1024u)
#define FUZZ_CANARY 0xC7u

/* Constant, obviously-fake key bytes.  Derived from an entry id so each
 * synthetic key differs; this is a test pattern, not key material. */
static void fuzz_fake_key(uint8_t *out, unsigned id) {
    for (unsigned i = 0; i < 16u; i++) {
        out[i] = (uint8_t)(0x11u + 0x23u * (id + i));
    }
}

static void fuzz_hex_encode(const uint8_t *bytes, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2u] = digits[(bytes[i] >> 4) & 0x0Fu];
        out[i * 2u + 1u] = digits[bytes[i] & 0x0Fu];
    }
    out[n * 2u] = '\0';
}

/* The complete synthetic key set the sealed ~PSP container needs. */
static size_t build_keystore_json(char *buf, size_t cap) {
    uint8_t cmd1[16], vault[16], tag_key[16];
    char cmd1_hex[33], vault_hex[33], tag_hex[33];
    fuzz_fake_key(cmd1, 1u);
    fuzz_fake_key(vault, 2u);
    fuzz_fake_key(tag_key, 3u);
    fuzz_hex_encode(cmd1, sizeof(cmd1), cmd1_hex);
    fuzz_hex_encode(vault, sizeof(vault), vault_hex);
    fuzz_hex_encode(tag_key, sizeof(tag_key), tag_hex);
    int n = snprintf(buf, cap,
                     "{\"format\":\"" FUZZ_KEYSTORE_FORMAT "\",\"entries\":{"
                     "\"kirk.cmd1.key\":\"%s\","
                     "\"kirk.keyvault.%d\":\"%s\","
                     "\"prx.tag.0x%08X\":{\"code\":%d,\"key\":\"%s\"}}}",
                     cmd1_hex, FUZZ_PSP_CODE, vault_hex,
                     (unsigned)FUZZ_PSP_TAG, FUZZ_PSP_CODE, tag_hex);
    assert(n > 0 && (size_t)n < cap);
    return (size_t)n;
}

/* Tiny source-owned ELF32/MIPS image (one PT_LOAD, entry inside it). */
static size_t build_tiny_elf(uint8_t *buf, size_t cap) {
    const uint32_t ehsize = 52u, phsize = 32u, text_off = 84u;
    assert(cap >= 128u);
    memset(buf, 0, cap);
    buf[0] = 0x7Fu; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 1u; buf[5] = 1u; buf[6] = 1u; /* 32-bit, LSB, current */
    put16le(buf + 16, 2u);   /* e_type ET_EXEC */
    put16le(buf + 18, 8u);   /* e_machine EM_MIPS */
    put32le(buf + 20, 1u);   /* e_version */
    put32le(buf + 24, 0x08804000u + text_off); /* e_entry */
    put32le(buf + 28, ehsize);
    put32le(buf + 32, 0u);   /* e_shoff */
    put32le(buf + 36, 0x60000000u);
    put16le(buf + 40, (uint16_t)ehsize);
    put16le(buf + 42, (uint16_t)phsize);
    put16le(buf + 44, 1u);   /* e_phnum */
    put16le(buf + 46, 40u);
    put32le(buf + 52, 1u);   /* p_type PT_LOAD */
    put32le(buf + 56, text_off);
    put32le(buf + 60, 0x08804000u);
    put32le(buf + 64, 0x08804000u);
    put32le(buf + 68, 32u);  /* p_filesz */
    put32le(buf + 72, 0x1000u); /* p_memsz: the entry lies inside */
    put32le(buf + 76, 5u);   /* p_flags RX */
    put32le(buf + 80, 4u);   /* p_align */
    memcpy(buf + text_off, "NK319", 5);
    return (size_t)text_off + 32u;
}

/* ---- gzip / zlib / raw DEFLATE stream writers (harness-local) ---------- */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    uint32_t acc;
    int have;
    int overflow;
} FuzzBits;

static void fuzz_put_bits(FuzzBits *w, uint32_t value, int count) {
    for (int i = 0; i < count; i++) {
        uint32_t bit = (value >> i) & 1u;
        w->acc |= (uint32_t)bit << w->have;
        if (++w->have == 8) {
            if (w->len >= w->cap) { w->overflow = 1; return; }
            w->buf[w->len++] = (uint8_t)w->acc;
            w->acc = 0u;
            w->have = 0;
        }
    }
}

/* DEFLATE packs Huffman codes most-significant bit first. */
static void fuzz_put_code(FuzzBits *w, uint32_t code, int nbits) {
    for (int i = nbits - 1; i >= 0; i--) {
        fuzz_put_bits(w, (code >> i) & 1u, 1);
    }
}

static void fuzz_put_literal(FuzzBits *w, uint8_t byte) {
    if (byte < 144u) fuzz_put_code(w, 0x30u + byte, 8);
    else fuzz_put_code(w, 0x190u + (uint32_t)(byte - 144u), 9);
}

static void fuzz_finish_bits(FuzzBits *w) {
    if (w->have != 0) {
        if (w->len < w->cap) w->buf[w->len++] = (uint8_t)w->acc;
        else w->overflow = 1;
        w->acc = 0u;
        w->have = 0;
    }
}

/* Fixed-Huffman block: literals, then `matches` back-references of the
 * longest length (code 285, distance 1), then end-of-block. */
static size_t build_deflate_fixed(uint8_t *out, size_t cap,
                                  const uint8_t *literals, size_t lit_count,
                                  unsigned matches) {
    FuzzBits w;
    w.buf = out; w.cap = cap; w.len = 0; w.acc = 0u; w.have = 0; w.overflow = 0;
    fuzz_put_bits(&w, 1u, 1);  /* final block */
    fuzz_put_bits(&w, 1u, 2);  /* fixed Huffman */
    for (size_t i = 0; i < lit_count; i++) fuzz_put_literal(&w, literals[i]);
    for (unsigned m = 0; m < matches && !w.overflow; m++) {
        fuzz_put_code(&w, 0xC5u, 8); /* length symbol 285 -> 258 bytes */
        fuzz_put_code(&w, 0u, 5);    /* distance symbol 0 -> distance 1 */
    }
    fuzz_put_code(&w, 0u, 7);        /* end of block */
    fuzz_finish_bits(&w);
    assert(!w.overflow);
    return w.len;
}

/* Single stored (uncompressed) block, as the firmware's own payloads use. */
static size_t build_deflate_stored(uint8_t *out, size_t cap,
                                   const uint8_t *data, size_t n) {
    assert(n <= 0xFFFFu && cap >= n + 5u);
    out[0] = 1u; /* final block, stored type */
    out[1] = (uint8_t)n;
    out[2] = (uint8_t)(n >> 8);
    out[3] = (uint8_t)(~out[1]);
    out[4] = (uint8_t)(~out[2]);
    memcpy(out + 5, data, n);
    return n + 5u;
}

static uint32_t fuzz_crc32(const uint8_t *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        int k;
        crc ^= p[i];
        for (k = 0; k < 8; k++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return ~crc;
}

static uint32_t fuzz_adler32(const uint8_t *p, size_t n) {
    uint32_t s1 = 1u, s2 = 0u;
    while (n > 0u) {
        size_t k = (n < 5552u) ? n : 5552u;
        n -= k;
        while (k-- > 0u) { s1 += *p++; s2 += s1; }
        s1 %= 65521u;
        s2 %= 65521u;
    }
    return (s2 << 16) | s1;
}

static size_t build_gzip(uint8_t *out, size_t cap, const uint8_t *deflated,
                         size_t deflated_len, const uint8_t *plain,
                         size_t plain_len) {
    assert(cap >= deflated_len + 18u);
    out[0] = 0x1Fu; out[1] = 0x8Bu; out[2] = 8u; out[3] = 0u;
    memset(out + 4, 0, 5);  /* MTIME */
    out[8] = 0u;            /* XFL */
    out[9] = 3u;            /* OS = Unix; no name, no comment, no hcrc */
    memcpy(out + 10, deflated, deflated_len);
    size_t at = 10u + deflated_len;
    put32le(out + at, fuzz_crc32(plain, plain_len));
    put32le(out + at + 4u, (uint32_t)plain_len);
    return at + 8u;
}

static size_t build_zlib(uint8_t *out, size_t cap, const uint8_t *deflated,
                         size_t deflated_len, const uint8_t *plain,
                         size_t plain_len) {
    assert(cap >= deflated_len + 6u);
    out[0] = 0x78u; out[1] = 0x9Cu; /* CM=8, FCHECK valid */
    memcpy(out + 2, deflated, deflated_len);
    size_t at = 2u + deflated_len;
    uint32_t adler = fuzz_adler32(plain, plain_len);
    out[at + 0u] = (uint8_t)(adler >> 24);
    out[at + 1u] = (uint8_t)(adler >> 16);
    out[at + 2u] = (uint8_t)(adler >> 8);
    out[at + 3u] = (uint8_t)adler;
    return at + 4u;
}

/* ---- synthetic type-2 ~PSP sealer (mirrors tools/test_psp_decrypt.py) --- */

#define FUZZ_KLE_SIZE 0x1000u

static size_t seal_type2_psp(uint8_t *out, size_t cap, const uint8_t *payload,
                             size_t payload_len, int compressed,
                             uint32_t elf_size) {
    uint8_t cmd1_key[16], vault_key[16], tag_key[16], a_key[16], c_key[16];
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
    fuzz_fake_key(cmd1_key, 1u);
    fuzz_fake_key(vault_key, 2u);
    fuzz_fake_key(tag_key, 3u);
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
    put16le(out + 4, 0x0200u);
    put16le(out + 6, (uint16_t)(compressed ? 1u : 0u));
    memcpy(out + 0x0A, "synthetic", 9);
    out[0x26] = 1u;
    out[0x27] = 2u; /* nsegments */
    put32le(out + 0x28, elf_size);
    put32le(out + 0x2C, (uint32_t)container_size);
    put32le(out + 0x30, 0x08804000u + 0x54u);
    put32le(out + 0x38, 0x100u);
    put32le(out + 0x3C, 0x10u);
    put32le(out + 0x40, 0x10u);
    put32le(out + 0x44, 0x08804000u);
    put32le(out + 0x48, 0x08809000u);
    put32le(out + 0x54, elf_size);
    put32le(out + 0x58, 0x100u);
    put32le(out + 0x78, 0x06060000u);

    /* CMD1 header tail at 0x60: mode, ecdsa_hash, sizes and the padding. */
    memset(hdr_tail, 0, sizeof(hdr_tail));
    put32le(hdr_tail, 1u); /* KIRK_MODE_CMD1 */
    put32le(hdr_tail + 0x10, (uint32_t)payload_len);
    put32le(hdr_tail + 0x14, (uint32_t)data_offset);
    put32le(metadata, (uint32_t)payload_len);
    put32le(metadata + 4u, (uint32_t)data_offset);

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
    put32le(sha_input + at, FUZZ_PSP_TAG);
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
    put32le(out + 0xD0, FUZZ_PSP_TAG);
    memcpy(out + 0x12Cu, chain + 0x10u, 0x14u);
    memcpy(out + 0x140u, chain, 0x10u);
    put32le(out + 0x2C, (uint32_t)container_size);
    return container_size;
}

static void test_fuzz_decrypt_boundary(unsigned iters) {
    printf("[FUZZ] Testing PSP decryption boundary: ~PSP/~SCE/PBP framing, "
           "KeyStore, inflate, KL4E/KL3E, KIRK dispatch (%u iterations)...\n",
           iters);
    fflush(stdout);

    char json[1024];
    size_t json_len = build_keystore_json(json, sizeof(json));
    NkKeystore *ks = nk_keystore_create();
    char err[256];
    assert(ks != NULL);
    assert(nk_keystore_load_json(ks, json, json_len, err, sizeof(err)) == NK_PSP_OK);
    assert(nk_keystore_count(ks) == 3u);

    uint8_t elf[256];
    size_t elf_len = build_tiny_elf(elf, sizeof(elf));
    uint8_t sealed[1024];
    size_t sealed_len = seal_type2_psp(sealed, sizeof(sealed), elf, elf_len, 0,
                                        (uint32_t)elf_len);

    /* Self-check: the synthetic container round-trips through production. */
    {
        NkPspCtx ctx;
        NkContainerInfo info;
        uint8_t *out = NULL;
        size_t out_size = 0;
        nk_psp_ctx_init(&ctx, ks);
        assert(nk_container_probe(&ctx, sealed, sealed_len, &info) == NK_PSP_OK);
        assert(info.kind == NK_CTR_PSP);
        assert(info.tag == FUZZ_PSP_TAG);
        assert(info.compressed == 0);
        nk_psp_ctx_init(&ctx, ks);
        assert(nk_container_decrypt(&ctx, sealed, sealed_len, &out, &out_size) ==
               NK_PSP_OK);
        assert(out != NULL && out_size == elf_len);
        assert(memcmp(out, elf, elf_len) == 0);
        free(out);
    }

    uint8_t wrapped[1200];
    uint8_t *mutated;
    unsigned accepted = 0;
    for (unsigned i = 0; i < iters; i++) {
        unsigned form = (unsigned)(fuzz_rand32() % 3u);
        size_t seed_len = sealed_len;
        const uint8_t *seed = sealed;
        if (form == 1u) { /* ~SCE outer wrapper */
            memcpy(wrapped, "~SCE", 4);
            put32le(wrapped + 4, 0x10u);
            memcpy(wrapped + 0x10u, sealed, sealed_len);
            seed = wrapped;
            seed_len = 0x10u + sealed_len;
        } else if (form == 2u) { /* PBP package carrying DATA.PSP */
            memset(wrapped, 0, 0x28u);
            memcpy(wrapped, "\0PBP", 4);
            put32le(wrapped + 0x20, 0x28u);
            memcpy(wrapped + 0x28u, sealed, sealed_len);
            seed = wrapped;
            seed_len = 0x28u + sealed_len;
        }
        /* Exact-size heap copy: any read past the caller's length traps. */
        mutated = (uint8_t *)malloc(seed_len);
        assert(mutated != NULL);
        memcpy(mutated, seed, seed_len);
        size_t cur_size = seed_len;
        mutate_buffer(mutated, &cur_size, seed_len);

        NkPspCtx ctx;
        NkContainerInfo info;
        char entries[4][80];
        int n_entries;
        nk_psp_ctx_init(&ctx, ks);
        (void)nk_container_probe(&ctx, mutated, cur_size, &info);
        nk_psp_ctx_init(&ctx, ks);
        n_entries = nk_container_key_entries(&info, ks, entries, 4);
        assert(n_entries >= 0 && n_entries <= 4);
        nk_psp_ctx_init(&ctx, ks);
        {
            uint8_t *out = NULL;
            size_t out_size = 0;
            int rc = nk_container_decrypt(&ctx, mutated, cur_size, &out, &out_size);
            if (rc == NK_PSP_OK) {
                assert(out != NULL);
                assert(out_size <= 64u * 1024u * 1024u);
                assert(out_size >= 4u && memcmp(out, "\x7f"
                                                        "ELF", 4) == 0);
                accepted++;
            }
            assert(out == NULL || rc == NK_PSP_OK);
            free(out);
        }
        free(mutated);
    }
    nk_keystore_free(ks);
    printf("[FUZZ] Decryption boundary completed: %u/%u decrypted, 0 crashes\n",
           accepted, iters);
    fflush(stdout);
}

/* ---- KeyStore JSON loader --------------------------------------------- */

static void test_fuzz_keystore(unsigned iters) {
    printf("[FUZZ] Testing KeyStore JSON loader (%u iterations)...\n", iters);
    fflush(stdout);

    char seed[1024];
    size_t seed_len = build_keystore_json(seed, sizeof(seed));
    const char *path = "build/fuzz_test_keystore.json";
    char err[256];
    {
        NkKeystore *ks = nk_keystore_create();
        assert(ks != NULL);
        assert(nk_keystore_load_json(ks, seed, seed_len, err, sizeof(err)) ==
               NK_PSP_OK);
        nk_keystore_free(ks);
    }

    /* Ceiling: the file is rejected outright past 64 KiB, never truncated. */
    {
        char *big = (char *)malloc(NK_KEYSTORE_MAX_BYTES + 2u);
        NkKeystore *ks = nk_keystore_create();
        assert(big != NULL && ks != NULL);
        memset(big, ' ', NK_KEYSTORE_MAX_BYTES + 1u);
        assert(nk_keystore_load_json(ks, big, NK_KEYSTORE_MAX_BYTES + 1u, err,
                                     sizeof(err)) != NK_PSP_OK);
        assert(nk_keystore_count(ks) == 0u);
        nk_keystore_free(ks);
        free(big);
    }

    uint8_t *mutated = (uint8_t *)malloc(4096u);
    assert(mutated != NULL);
    unsigned accepted = 0;
    for (unsigned i = 0; i < iters; i++) {
        size_t cur_size = seed_len;
        memcpy(mutated, seed, seed_len);
        mutate_buffer(mutated, &cur_size, 4096u);

        NkKeystore *ks = nk_keystore_create();
        assert(ks != NULL);
        if (nk_keystore_load_json(ks, (const char *)mutated, cur_size, err,
                                  sizeof(err)) == NK_PSP_OK) {
            const uint8_t *value = NULL;
            size_t value_len = 0;
            assert(nk_keystore_count(ks) <= NK_KEYSTORE_MAX_ENTRIES);
            if (nk_keystore_get(ks, "kirk.cmd1.key", &value, &value_len) == 0) {
                /* Entry lengths are validated at load: never anything else. */
                assert(value_len == 16u || value_len == 20u || value_len == 21u);
            }
            accepted++;
        }
        nk_keystore_free(ks);

        if ((i % 8u) == 0u) { /* the file route as well as the buffer route */
            write_file_bytes(path, mutated, cur_size);
            NkKeystore *fks = nk_keystore_create();
            assert(fks != NULL);
            (void)nk_keystore_load_file(fks, path, err, sizeof(err));
            nk_keystore_free(fks);
        }
    }
    remove(path);
    free(mutated);
    printf("[FUZZ] KeyStore JSON completed: %u/%u accepted, 0 crashes\n",
           accepted, iters);
    fflush(stdout);
}

/* ---- inflate (raw DEFLATE / zlib / gzip) ------------------------------ */

static void test_fuzz_inflate(unsigned iters) {
    printf("[FUZZ] Testing inflate: raw DEFLATE, zlib and gzip (%u iterations)...\n",
           iters);
    fflush(stdout);

    uint8_t plain[64];
    for (size_t i = 0; i < sizeof(plain); i++) plain[i] = (uint8_t)(i * 7u + 3u);

    uint8_t deflated[2048];
    size_t deflated_len = build_deflate_fixed(deflated, sizeof(deflated), plain,
                                             16u, 0u);
    uint8_t gzipped[2100];
    size_t gzipped_len = build_gzip(gzipped, sizeof(gzipped), deflated,
                                    deflated_len, plain, 16u);
    uint8_t zlibbed[2100];
    size_t zlibbed_len = build_zlib(zlibbed, sizeof(zlibbed), deflated,
                                    deflated_len, plain, 16u);
    uint8_t stored[128];
    size_t stored_len = build_deflate_stored(stored, sizeof(stored), plain, 16u);
    uint8_t gz_stored[160];
    size_t gz_stored_len = build_gzip(gz_stored, sizeof(gz_stored), stored,
                                      stored_len, plain, 16u);
    /* Expansion bomb: 1200 back-references, 302 KiB out of ~2 KiB. */
    uint8_t bomb[2048];
    size_t bomb_len = build_deflate_fixed(bomb, sizeof(bomb), plain, 1u, 1200u);

    struct { const uint8_t *data; size_t size; } seeds[5];
    seeds[0].data = deflated; seeds[0].size = deflated_len;
    seeds[1].data = gzipped;  seeds[1].size = gzipped_len;
    seeds[2].data = zlibbed;  seeds[2].size = zlibbed_len;
    seeds[3].data = gz_stored; seeds[3].size = gz_stored_len;
    seeds[4].data = bomb;     seeds[4].size = bomb_len;

    char err[128];
    /* Fixed expectations: a well-formed stream decodes, a bomb is refused. */
    {
        uint8_t *out = NULL;
        size_t out_len = 0;
        assert(nk_psp_inflate(gzipped, gzipped_len, &out, &out_len, 0u,
                              FUZZ_INFLATE_CAP, err, sizeof(err)) == NK_INFLATE_OK);
        assert(out_len == 16u);
        assert(memcmp(out, plain, 16u) == 0);
        free(out);
        out = NULL;
        out_len = 0;
        assert(nk_psp_inflate(bomb, bomb_len, &out, &out_len, 0u,
                              FUZZ_INFLATE_CAP, err, sizeof(err)) ==
               NK_INFLATE_ERR_OVERFLOW);
        assert(out == NULL && out_len == 0u);
    }
    /* A zlib-looking input shorter than a zlib stream is refused as
     * truncated; the header flags byte is never read past the input. */
    {
        static const uint8_t short_zlib[4] = {0x78u, 0x9Cu, 0x00u, 0x20u};
        uint8_t *out = NULL;
        size_t out_len = 0;
        assert(nk_psp_inflate(short_zlib, sizeof(short_zlib), &out, &out_len, 0u,
                              FUZZ_INFLATE_CAP, err, sizeof(err)) ==
               NK_INFLATE_ERR_TRUNCATED);
        assert(out == NULL && out_len == 0u);
    }

    unsigned accepted = 0;
    for (unsigned i = 0; i < iters; i++) {
        unsigned pick = (unsigned)(fuzz_rand32() % 5u);
        size_t seed_size = seeds[pick].size;
        uint8_t *mutated = (uint8_t *)malloc(seed_size);
        assert(mutated != NULL);
        memcpy(mutated, seeds[pick].data, seed_size);
        size_t cur_size = seed_size;
        mutate_buffer(mutated, &cur_size, seed_size);

        uint8_t *out = NULL;
        size_t out_len = 0;
        int rc = nk_psp_inflate(mutated, cur_size, &out, &out_len, 0u,
                                FUZZ_INFLATE_CAP, err, sizeof(err));
        if (rc == NK_INFLATE_OK) {
            assert(out != NULL);
            assert(out_len <= FUZZ_INFLATE_CAP); /* the declared ceiling */
            accepted++;
        } else {
            assert(out == NULL && out_len == 0u);
        }
        free(out);
        free(mutated);
    }
    printf("[FUZZ] Inflate completed: %u/%u decoded, 0 crashes\n", accepted, iters);
    fflush(stdout);
}

/* ---- KIRK command dispatch on attacker-controlled headers -------------- */

static void test_fuzz_kirk(unsigned iters) {
    printf("[FUZZ] Testing KIRK command dispatch on hostile headers "
           "(%u iterations)...\n", iters);
    fflush(stdout);

    char json[1024];
    size_t json_len = build_keystore_json(json, sizeof(json));
    NkKeystore *ks = nk_keystore_create();
    char err[256];
    assert(ks != NULL);
    assert(nk_keystore_load_json(ks, json, json_len, err, sizeof(err)) == NK_PSP_OK);

    /* Two synthetic header shapes the attacker controls: a CMD1 block
     * (0x90 header: mode, ecdsa_hash, data_size, data_offset) and an
     * AES128-CBC block (0x14 header: mode, keyseed, data_size), each
     * followed by a 0x80 body. */
    enum { FUZZ_KIRK_SIZE = 0x90u + 0x80u, FUZZ_KIRK_GUARD = 64 };
    uint8_t cmd1_seed[FUZZ_KIRK_SIZE];
    uint8_t cbc_seed[FUZZ_KIRK_SIZE];
    memset(cmd1_seed, 0, sizeof(cmd1_seed));
    memcpy(cmd1_seed, "CMAC", 4);       /* AES_key slot */
    put32le(cmd1_seed + 0x60, 1u);      /* KIRK_MODE_CMD1 */
    cmd1_seed[0x64] = 0u;               /* ecdsa_hash */
    put32le(cmd1_seed + 0x70, 0x80u);   /* data_size */
    put32le(cmd1_seed + 0x74, 0x10u);   /* data_offset */
    memset(cbc_seed, 0, sizeof(cbc_seed));
    put32le(cbc_seed, 4u);              /* KIRK_MODE_ENCRYPT_CBC */
    put32le(cbc_seed + 0x0C, FUZZ_PSP_CODE); /* keyvault slot */
    put32le(cbc_seed + 0x10, 0x80u);    /* data_size (16-byte aligned) */
    for (unsigned i = 0; i < 0x80u; i++) {
        cmd1_seed[0x90u + i] = (uint8_t)(i * 3u);
        cbc_seed[0x90u + i] = (uint8_t)(i * 5u);
    }

    static const int kirk_commands[] = {
        KIRK_CMD_DECRYPT_PRIVATE, KIRK_CMD_ENCRYPT_IV_0, KIRK_CMD_DECRYPT_IV_0,
        KIRK_CMD_PRIV_SIGN_CHECK, KIRK_CMD_SHA1_HASH, 0x7F};

    /* The unmutated CBC header is a well-formed command: the AES pass runs
     * and writes exactly data_size bytes at 0x14, and nothing past the
     * caller's declared output size. */
    {
        uint8_t *outbuf = (uint8_t *)malloc(FUZZ_KIRK_SIZE + FUZZ_KIRK_GUARD);
        assert(outbuf != NULL);
        memset(outbuf, FUZZ_CANARY, FUZZ_KIRK_SIZE + FUZZ_KIRK_GUARD);
        NkPspCtx ctx;
        nk_psp_ctx_init(&ctx, ks);
        assert(sceUtilsBufferCopyWithRange(&ctx, outbuf, FUZZ_KIRK_SIZE, cbc_seed,
                                           FUZZ_KIRK_SIZE,
                                           KIRK_CMD_ENCRYPT_IV_0) ==
               KIRK_OPERATION_SUCCESS);
        assert(outbuf[0x14] != FUZZ_CANARY);
        for (unsigned g = 0; g < FUZZ_KIRK_GUARD; g++) {
            assert(outbuf[FUZZ_KIRK_SIZE + g] == FUZZ_CANARY);
        }
        free(outbuf);
    }

    unsigned accepted = 0;
    for (unsigned i = 0; i < iters; i++) {
        int cmd = kirk_commands[fuzz_rand32() % 6u];
        const uint8_t *seed = ((fuzz_rand32() % 2u) != 0u) ? cmd1_seed : cbc_seed;
        size_t insize = FUZZ_KIRK_SIZE;
        size_t scratch;
        uint8_t *inbuf = (uint8_t *)malloc(insize);
        assert(inbuf != NULL);
        memcpy(inbuf, seed, insize);
        scratch = insize;
        mutate_buffer(inbuf, &scratch, insize);

        /* Exactly the caller's declared output size, plus a poisoned tail:
         * any write past the negotiated size is caught without a sanitizer. */
        uint8_t *outbuf = (uint8_t *)malloc(insize + FUZZ_KIRK_GUARD);
        assert(outbuf != NULL);
        memset(outbuf, FUZZ_CANARY, insize + FUZZ_KIRK_GUARD);
        NkPspCtx ctx;
        nk_psp_ctx_init(&ctx, ks);
        int rc = sceUtilsBufferCopyWithRange(&ctx, outbuf, (int)insize, inbuf,
                                             (int)insize, cmd);
        for (unsigned g = 0; g < FUZZ_KIRK_GUARD; g++) {
            assert(outbuf[insize + g] == FUZZ_CANARY);
        }
        if (rc == KIRK_OPERATION_SUCCESS) accepted++;
        free(outbuf);
        free(inbuf);

        /* The ECDSA commands take fixed, exactly-checked sizes. */
        if ((i % 4u) == 0u) {
            uint8_t cmd13_out[0x28];
            uint8_t *heap13 = (uint8_t *)malloc(0x3Cu);
            uint8_t *heap17 = (uint8_t *)malloc(0x64u);
            assert(heap13 != NULL && heap17 != NULL);
            memset(heap13, 0x11u, 0x3Cu);
            memset(heap17, 0x22u, 0x64u);
            scratch = 0x3Cu;
            mutate_buffer(heap13, &scratch, 0x3Cu);
            scratch = 0x64u;
            mutate_buffer(heap17, &scratch, 0x64u);
            memset(cmd13_out, 0, sizeof(cmd13_out));
            nk_psp_ctx_init(&ctx, ks);
            (void)sceUtilsBufferCopyWithRange(&ctx, cmd13_out,
                                              (int)sizeof(cmd13_out), heap13,
                                              (int)0x3Cu,
                                              KIRK_CMD_ECDSA_MULTIPLY_POINT);
            nk_psp_ctx_init(&ctx, ks);
            (void)sceUtilsBufferCopyWithRange(&ctx, NULL, 0, heap17,
                                              (int)0x64u,
                                              KIRK_CMD_ECDSA_VERIFY);
            nk_psp_ctx_init(&ctx, ks);
            (void)sceUtilsBufferCopyWithRange(&ctx, NULL, 0, heap17,
                                              (int)0x64u - 1,
                                              KIRK_CMD_ECDSA_VERIFY);
            free(heap13);
            free(heap17);
        }
    }
    nk_keystore_free(ks);
    printf("[FUZZ] KIRK dispatch completed: %u/%u accepted, 0 crashes\n",
           accepted, iters);
    fflush(stdout);
}

/* ---- KL4E / KL3E decoder ---------------------------------------------- */

static void test_fuzz_kle(unsigned iters) {
    printf("[FUZZ] Testing KL4E/KL3E decoder on hostile headers "
           "(%u iterations)...\n", iters);
    fflush(stdout);

    uint8_t payload[64];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 5u + 9u);

    /* A synthetic "uncompressed" KL stream: the firmware's direct-copy
     * form, which is the one form a host can produce without the range
     * encoder.  The arithmetic-coded form is reached only by mutating it. */
    uint8_t kl4e[128];
    size_t kl4e_len = 0;
    memcpy(kl4e, "KL4E", 4);
    kl4e[4] = 0x80u | 3u; /* direct copy, shift = 3 */
    put32be(kl4e + 5, (uint32_t)sizeof(payload));
    memcpy(kl4e + 9, payload, sizeof(payload));
    kl4e_len = 9u + sizeof(payload);
    uint8_t kl3e[128];
    memcpy(kl3e, "KL3E", 4);
    memcpy(kl3e + 4, kl4e + 4, kl4e_len - 4u);
    size_t kl3e_len = kl4e_len;

    uint8_t *out = (uint8_t *)malloc(FUZZ_KLE_SIZE);
    assert(out != NULL);
    memset(out, 0, FUZZ_KLE_SIZE);
    {
        void *end = NULL;
        int rc = decompress_kle(out, (int)FUZZ_KLE_SIZE, kl4e + 4,
                                (int)(kl4e_len - 4u), &end, 1);
        assert(rc == (int)sizeof(payload));
        assert(memcmp(out, payload, sizeof(payload)) == 0);
        assert(end != NULL);
        assert((uint8_t *)end >= kl4e + 4 && (uint8_t *)end <= kl4e + kl4e_len);
    }
    /* A stream that declares more bytes than it carries fails closed
     * instead of copying from beyond the input. */
    {
        uint8_t truncated[12];
        void *end = NULL;
        int rc;
        memcpy(truncated, kl4e, sizeof(truncated));
        put32be(truncated + 5, 0x400u); /* claims 1024 bytes, carries 7 */
        rc = decompress_kle(out, (int)FUZZ_KLE_SIZE, truncated + 4,
                            (int)(sizeof(truncated) - 4u), &end, 1);
        assert(rc < 0);
        assert(end == NULL);
    }
    /* Fewer than the five header bytes is refused outright. */
    {
        void *end = NULL;
        assert(decompress_kle(out, (int)FUZZ_KLE_SIZE, kl4e + 4, 4, &end, 1) < 0);
        assert(decompress_kle(out, (int)FUZZ_KLE_SIZE, kl4e + 4, 0, &end, 0) < 0);
    }

    unsigned decoded = 0;
    for (unsigned i = 0; i < iters; i++) {
        int is_kl4e = (fuzz_rand32() % 2u) != 0u;
        const uint8_t *seed = is_kl4e ? kl4e : kl3e;
        size_t seed_len = is_kl4e ? kl4e_len : kl3e_len;
        /* Exact-size heap copy: the decoder may never read past it. */
        uint8_t *inbuf = (uint8_t *)malloc(seed_len);
        assert(inbuf != NULL);
        memcpy(inbuf, seed, seed_len);
        size_t cur_size = seed_len;
        mutate_buffer(inbuf, &cur_size, seed_len);

        memset(out, 0, FUZZ_KLE_SIZE);
        void *end = NULL;
        int in_size = (cur_size > 4u) ? (int)(cur_size - 4u) : 0;
        int rc = decompress_kle(out, (int)FUZZ_KLE_SIZE, inbuf + 4, in_size,
                                &end, is_kl4e);
        assert(rc <= (int)FUZZ_KLE_SIZE);
        if (rc > 0) {
            assert((uint8_t *)end >= inbuf && (uint8_t *)end <= inbuf + cur_size);
            decoded++;
        }
        free(inbuf);
    }
    free(out);
    printf("[FUZZ] KL4E/KL3E completed: %u/%u decoded, 0 crashes\n", decoded, iters);
    fflush(stdout);
}


int main(int argc, char **argv) {
    unsigned iters = 100;

    const char *env_iters = getenv("FUZZ_ITERS");
    iters = parse_fuzz_iterations(env_iters, iters);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = parse_fuzz_iterations(argv[++i], iters);
        }
    }

    assert(nk_platform_mkdir_p("build"));

    printf("=================================================================\n");
    printf("Starting Nakagawa deterministic parser fuzz suite (iters=%u)\n", iters);
    printf("=================================================================\n");
    fflush(stdout);

    test_fuzz_param_sfo(iters);
    test_fuzz_iso(iters > 1000 ? 1000 : iters);
    test_fuzz_elf_classifier(iters > 1000 ? 1000 : iters);
    test_fuzz_xb(iters);
    test_fuzz_prx(iters);
    test_fuzz_manifest(iters);
    test_fuzz_package(iters > 1000 ? 1000 : iters);
    test_fuzz_library(iters > 500 ? 500 : iters);
    test_fuzz_decrypt_boundary(iters > 4000 ? 4000 : iters);
    test_fuzz_keystore(iters > 1000 ? 1000 : iters);
    test_fuzz_inflate(iters > 2000 ? 2000 : iters);
    test_fuzz_kirk(iters > 2000 ? 2000 : iters);
    test_fuzz_kle(iters > 2000 ? 2000 : iters);

    printf("=================================================================\n");
    printf("ALL DETERMINISTIC PARSER FUZZ HARNESSES PASSED SUCCESSFULLY!\n");
    printf("=================================================================\n");
    return 0;
}
