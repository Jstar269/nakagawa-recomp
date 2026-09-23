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

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
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

/* Mutate buffer in-place with bit flips, truncations, inserts, deletions,
   and length/offset/integer boundary value corruptions. */
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
    printf("[FUZZ] Testing PRX loader and ~PSP container (%u iterations)...\n", iters);
    fflush(stdout);

    uint8_t seed_prx[1024];
    size_t seed_prx_size = build_fuzz_prx(seed_prx, sizeof(seed_prx));

    uint8_t seed_psp[256];
    size_t seed_psp_size = build_fuzz_psp_container(seed_psp, sizeof(seed_psp));

    /* Verify clean load of synthetic PRX */
    SrPrxImage base_img;
    char err[256];
    int rc = sr_prx_load_from_memory(seed_prx, seed_prx_size, 0x08800000u,
                                     &base_img, err, sizeof(err));
    assert(rc == 0);
    assert(strcmp(base_img.modname, "fuzz_module") == 0);
    sr_prx_image_free(&base_img);

    /* Verify clean rejection of ~PSP container */
    rc = sr_prx_load_from_memory(seed_psp, seed_psp_size, 0x08800000u,
                                 &base_img, err, sizeof(err));
    assert(rc != 0);
    assert(strstr(err, "~PSP") != NULL);

    uint8_t mutated[2048];
    unsigned accepted = 0;
    for (unsigned i = 0; i < iters; i++) {
        uint8_t *chosen_seed = (i & 1) ? seed_prx : seed_psp;
        size_t chosen_size = (i & 1) ? seed_prx_size : seed_psp_size;

        memcpy(mutated, chosen_seed, chosen_size);
        size_t cur_size = chosen_size;
        mutate_buffer(mutated, &cur_size, sizeof(mutated));

        SrPrxImage img;
        if (sr_prx_load_from_memory(mutated, cur_size, 0x08800000u, &img,
                                    err, sizeof(err)) == 0) {
            accepted++;
            sr_prx_image_free(&img);
        }
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
 * Main Entry Point
 * -------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    unsigned iters = 100;

    const char *env_iters = getenv("FUZZ_ITERS");
    if (env_iters && *env_iters) {
        long val = strtol(env_iters, NULL, 10);
        if (val > 0) iters = (unsigned)val;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            long val = strtol(argv[++i], NULL, 10);
            if (val > 0) iters = (unsigned)val;
        }
    }

    assert(nk_platform_mkdir_p("build"));

    printf("=================================================================\n");
    printf("Starting Nakagawa deterministic parser fuzz suite (iters=%u)\n", iters);
    printf("=================================================================\n");
    fflush(stdout);

    test_fuzz_param_sfo(iters);
    test_fuzz_iso(iters > 1000 ? 1000 : iters);
    test_fuzz_xb(iters);
    test_fuzz_prx(iters);
    test_fuzz_manifest(iters);
    test_fuzz_library(iters > 500 ? 500 : iters);

    printf("=================================================================\n");
    printf("ALL DETERMINISTIC PARSER FUZZ HARNESSES PASSED SUCCESSFULLY!\n");
    printf("=================================================================\n");
    return 0;
}
