// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Fail-closed loading, validation and once-only publication of the VFPU
 * transcendental lookup tables (issue #187).
 *
 * The table DATA is the PPSSPP-derived collection tracked under
 * assets/vfpu/PROVENANCE.json (upstream revision f0baf3ade7bcb6c86f0835962b36eb4e51559d8f).
 * This module is an independent loader: it never trusts file size alone.
 * Every table must match the committed byte length AND SHA-256, end exactly at
 * EOF, pass its element-alignment check, and (where table bytes become native
 * indices or search bounds) satisfy value-domain invariants before the
 * aggregate is published as the runtime's global pointers.
 *
 * Concurrency: sr_vfpu_load() serializes first use with an atomic spin lock
 * (the same pattern as hle.c/iso.c) and publishes with acquire/release
 * ordering, so concurrent first transcendental calls cannot observe partial
 * state or duplicate loads. A failed load releases every temporary allocation,
 * prints the cause, and aborts: that is the documented unrecoverable startup
 * policy -- without validated tables the transcendentals would silently
 * compute wrong math.
 */

#include "vfpu_tables.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The committed tables are little-endian typed arrays (see PROVENANCE.json
 * `endianness`), and this loader casts raw bytes to native typed pointers. On
 * a big-endian host those casts would silently reinterpret every element.
 * Enforce the little-endian host contract at compile time where the compiler
 * exposes the byte order; MSVC targets are little-endian by construction. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "vfpu_tables: tables are little-endian; a little-endian host is required");
#endif

/* ---- FIPS-180-4 SHA-256 ------------------------------------------------- */

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t block[64];
    size_t used;
} SrSha256;

static const uint32_t SR_SHA_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t sr_sha_rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static void sr_sha_transform(SrSha256 *sha, const uint8_t block[64]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)block[i * 4u] << 24) |
               ((uint32_t)block[i * 4u + 1u] << 16) |
               ((uint32_t)block[i * 4u + 2u] << 8) |
               (uint32_t)block[i * 4u + 3u];
    }
    for (unsigned i = 16u; i < 64u; i++) {
        uint32_t s0 = sr_sha_rotr(w[i - 15u], 7u) ^ sr_sha_rotr(w[i - 15u], 18u) ^ (w[i - 15u] >> 3);
        uint32_t s1 = sr_sha_rotr(w[i - 2u], 17u) ^ sr_sha_rotr(w[i - 2u], 19u) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }
    uint32_t a = sha->h[0], b = sha->h[1], c = sha->h[2], d = sha->h[3];
    uint32_t e = sha->h[4], f = sha->h[5], g = sha->h[6], h = sha->h[7];
    for (unsigned i = 0; i < 64u; i++) {
        uint32_t s1 = sr_sha_rotr(e, 6u) ^ sr_sha_rotr(e, 11u) ^ sr_sha_rotr(e, 25u);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + SR_SHA_K[i] + w[i];
        uint32_t s0 = sr_sha_rotr(a, 2u) ^ sr_sha_rotr(a, 13u) ^ sr_sha_rotr(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    sha->h[0] += a; sha->h[1] += b; sha->h[2] += c; sha->h[3] += d;
    sha->h[4] += e; sha->h[5] += f; sha->h[6] += g; sha->h[7] += h;
}

void sr_vfpu_sha256(const uint8_t *data, size_t size, uint8_t digest[32]) {
    SrSha256 sha;
    memset(&sha, 0, sizeof sha);
    sha.h[0] = 0x6a09e667u; sha.h[1] = 0xbb67ae85u; sha.h[2] = 0x3c6ef372u;
    sha.h[3] = 0xa54ff53au; sha.h[4] = 0x510e527fu; sha.h[5] = 0x9b05688cu;
    sha.h[6] = 0x1f83d9abu; sha.h[7] = 0x5be0cd19u;

    sha.bits += (uint64_t)size * 8u;
    while (size) {
        size_t take = sizeof sha.block - sha.used;
        if (take > size) take = size;
        memcpy(sha.block + sha.used, data, take);
        sha.used += take;
        data += take;
        size -= take;
        if (sha.used == sizeof sha.block) {
            sr_sha_transform(&sha, sha.block);
            sha.used = 0;
        }
    }
    size_t used = sha.used;
    sha.block[used++] = 0x80u;
    if (used > 56u) {
        memset(sha.block + used, 0, 64u - used);
        sr_sha_transform(&sha, sha.block);
        used = 0;
    }
    memset(sha.block + used, 0, 56u - used);
    for (unsigned i = 0; i < 8u; i++)
        sha.block[56u + i] = (uint8_t)(sha.bits >> (56u - 8u * i));
    sr_sha_transform(&sha, sha.block);
    for (unsigned i = 0; i < 8u; i++) {
        digest[i * 4u] = (uint8_t)(sha.h[i] >> 24);
        digest[i * 4u + 1u] = (uint8_t)(sha.h[i] >> 16);
        digest[i * 4u + 2u] = (uint8_t)(sha.h[i] >> 8);
        digest[i * 4u + 3u] = (uint8_t)sha.h[i];
    }
}

static int sr_hex64_eq(const char *hex, const uint8_t digest[32]) {
    static const char SR_HEX[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32u; i++) {
        if (hex[i * 2u] != SR_HEX[digest[i] >> 4] ||
            hex[i * 2u + 1u] != SR_HEX[digest[i] & 0x0fu]) {
            return 0;
        }
    }
    return 1;
}

/* ---- manifest ----------------------------------------------------------- */
/* SHA-256 of each committed asset (assets/vfpu/PROVENANCE.json). Kept in sync
 * by tools/test_vfpu_table_manifest.py, which re-derives these from the files. */

static const SrVfpuTableSpec SR_VFPU_TABLES[] = {
    { "vfpu_rcp_lut.dat",                262144u, 1u, "0aee9fd249988073e4f364cd724cb6f54aa0c57ce0b515a1fe69a65444a92c55" },
    { "vfpu_sqrt_lut.dat",               262144u, 1u, "634e81488992fc2a0bfe5eaa3982f8895f7cdbb59595cb1dc6934bddafde86a6" },
    { "vfpu_rsqrt_lut.dat",              262144u, 1u, "9ebaa077ce0e70c6e6a706945bd905af681a8cc16d9fedd623b23d59d7794ced" },
    { "vfpu_sin_lut8192.dat",             4100u, 4u, "1017c38fcc37ced830dc68e5603725526ecadcc23bb0fdf99159622351f61676" },
    { "vfpu_sin_lut_delta.dat",          262144u, 1u, "24bddbb34b59b9a714e3c1f9a0335b3fbbfcedf8d04e71f95adf1b4295fbb13b" },
    { "vfpu_sin_lut_interval_delta.dat", 131074u, 2u, "1506831c116bb5def823fc89c80b8d4fcdd6c938b223855a67709fcfe3fb285b" },
    { "vfpu_sin_lut_exceptions.dat",      86938u, 1u, "46d585d103c9265393ed7c79bc4db7d047daf855c52358f4311a54d275e7fb74" },
    { "vfpu_exp2_lut65536.dat",             512u, 4u, "7a0c42d5652c0fa48f457072b02a87f949e747d3f8a04f25bfa861fc2b85b01d" },
    { "vfpu_exp2_lut.dat",               262144u, 1u, "fcd6b4f4bb088e70c0c3e0aa0466d8ae9a2bf6b9459ae0a660e7de37a914ea35" },
    { "vfpu_log2_lut65536.dat",             516u, 4u, "f7b0f2726808c5bb7207a2a5c297df437277e95e121b7e46dc8f5cf152bb0dd4" },
    { "vfpu_log2_lut65536_quadratic.dat",   512u, 4u, "25a0ddddbf33bab98a88f9c95a678fb93d54be17b1d08a1a90131161b42eeec5" },
    { "vfpu_log2_lut.dat",              2097152u, 1u, "2ce3277fc2eaa9188f7afa341dadfd041447c07482d7aea8c5dbd1a3c869181c" },
    { "vfpu_asin_lut65536.dat",            1536u, 4u, "7a046ed71cc700dba4f90ff42e66ebd4b292281d3d2899202c3e370aa7c26ef9" },
    { "vfpu_asin_lut_deltas.dat",        517448u, 8u, "6c6ba37ae631df90b731c6753f500e1bd4455b6435a56d50e3dd8f813b234027" },
    { "vfpu_asin_lut_indices.dat",       798916u, 2u, "418dd88590f2a2cfe4d45b27abe5e547a81ecf3ea00c90afec86740d6012d9c8" },
};

const SrVfpuTableSpec *sr_vfpu_table_manifest(size_t *count_out) {
    if (count_out) *count_out = sizeof SR_VFPU_TABLES / sizeof SR_VFPU_TABLES[0];
    return SR_VFPU_TABLES;
}

/* ---- value-domain validators ------------------------------------------- */

int sr_vfpu_validate_asin_indices(const uint16_t *indices, size_t count,
                                  size_t deltas_entries) {
    if (!indices || deltas_entries == 0u) return -1;
    for (size_t i = 0; i < count; i++) {
        if (indices[i] >= deltas_entries) return -1;
    }
    return 0;
}

int sr_vfpu_validate_sin_interval(const int16_t *delta, size_t count,
                                  size_t exceptions_bytes) {
    if (!delta || count < 2u || exceptions_bytes == 0u) return -1;
    /* The runtime computes, for k = arg >> 7 with arg <= 0x7FFFFF (the
     * mirrored significand range; 0x00800000 returns early):
     *   lo = ((169*k) >> 7) + delta[k] + 16384
     *   hi = ((169*(k+1)) >> 7) + delta[k+1] + 16384
     * and binary-searches m = (lo+hi)/2 while lo < hi, reading
     * exceptions[m]. m < hi, so hi <= exceptions_bytes (INCLUSIVE) keeps
     * every read in bounds; genuine data maxes at exactly hi == len (the
     * largest reachable m is 86937 of 86938 bytes). The invariant required
     * here is the full 0 <= lo <= hi <= exception_count: genuine data has
     * zero inverted intervals, so lo > hi is a corruption signal. The
     * unsigned comparisons also reject underflowed lo/hi (huge uint32). */
    for (size_t k = 0; k + 1u < count; k++) {
        uint32_t lo = ((169u * (uint32_t)k) >> 7) +
                      (uint32_t)(int32_t)delta[k] + 16384u;
        uint32_t hi = ((169u * ((uint32_t)k + 1u)) >> 7) +
                      (uint32_t)(int32_t)delta[k + 1u] + 16384u;
        if (lo > hi || hi > (uint32_t)exceptions_bytes)
            return -1;
    }
    return 0;
}

/* ---- index-provenance classification (issue #187) -----------------------
 *
 * Every native array index the transcendental kernels take is classified
 * below. Only two tables feed content-derived indices; everything else is
 * bounded by the guest float input (significand/exponent bit manipulation,
 * always < 2^24 before mirroring, or an exponent-derived row d in [0,7]).
 *
 * CONTENT-DERIVED (semantic validation + defense-in-depth consumer checks):
 *   vfpu_asin_lut_indices        uint16 value -> vfpu_asin_lut_deltas entry
 *   vfpu_sin_lut_interval_delta  int16 values -> lo/hi -> exceptions[m]
 *
 * GUEST-BOUNDED (index derives from float bits, validated by 24-bit masking):
 *   vfpu_rcp_lut        [i>>6]      i <= 0x7FFFFF -> <= 131071
 *   vfpu_sqrt_lut       [x>>6]      x <= 0x7FFFFF -> <= 131071
 *   vfpu_rsqrt_lut      [x>>6]      x <= 0x7FFFFF -> <= 131071
 *   vfpu_sin_lut8192    [arg>>13+0/1] arg <= 0x7FFFFF -> <= 1024 (1025 entries)
 *   vfpu_sin_lut_delta  [arg>>6]    arg <= 0x7FFFFF -> <= 131071
 *   vfpu_exp2_lut65536  [x>>16]     x <= 0x7FFFFF -> <= 127 (128 entries)
 *   vfpu_exp2_lut       [x>>6]      x <= 0x7FFFFF -> <= 131071
 *   vfpu_log2_lut65536  [x>>16+0/1] x <= 0x7FFFFF -> <= 128 (129 entries)
 *   vfpu_log2_lut65536_quadratic [x>>16] -> <= 127 (128 entries)
 *   vfpu_log2_lut       [d][i>>6]   d in [0,7], i <= 0x7FFFFF -> in 8x131072x2
 *   vfpu_asin_lut65536  [x>>16]     x <= 0x800000 -> <= 127 (512 entries)
 *
 * PURE COEFFICIENT (arithmetic only, never an index):
 *   vfpu_asin_lut_deltas  uint64 coefficients selected via validated indices
 *   vfpu_sin_lut_exceptions uint8 exception set selected via validated lo/hi
 *
 * All GUEST-BOUNDED indices rely on the caller-supplied masks in recomp.c;
 * the loader validates length + SHA-256 for those tables, and the committed
 * byte lengths above are asserted by the manifest. */

/* ---- fail-closed single-table read -------------------------------------- */

/* Resolve the table root. *overridden_out (optional) reports whether the
 * PSP_VFPU_TABLES environment override is actually in effect, so run evidence
 * can distinguish a noncanonical override set from the default validated one. */
static const char *sr_vfpu_root(int *overridden_out) {
    const char *override = getenv("PSP_VFPU_TABLES");
    if (override && override[0] != '\0') {
        if (overridden_out) *overridden_out = 1;
        return override;
    }
    if (overridden_out) *overridden_out = 0;
    return "assets/vfpu";
}

static void sr_vfpu_fmt_hex(const uint8_t digest[32], char out[65]) {
    static const char SR_HEX[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32u; i++) {
        out[i * 2u] = SR_HEX[digest[i] >> 4];
        out[i * 2u + 1u] = SR_HEX[digest[i] & 0x0fu];
    }
    out[64] = '\0';
}

/* Read one table into a fresh allocation. On success *out owns the buffer.
 * On failure *out is NULL and all resources are released. */
static int sr_vfpu_read_table(const char *root, const SrVfpuTableSpec *spec,
                              uint8_t **out, char *err, size_t errcap) {
    *out = NULL;
    char path[512];
    int written = snprintf(path, sizeof path, "%s/%s", root, spec->name);
    if (written < 0 || (size_t)written >= sizeof path) {
        snprintf(err, errcap, "table path truncated: root/%.32s", spec->name);
        return -1;
    }
    if (spec->bytes % spec->elem != 0u) {
        snprintf(err, errcap, "table %s: manifest length %zu not a multiple of elem %zu",
                 spec->name, spec->bytes, spec->elem);
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errcap, "cannot open %s", path);
        return -1;
    }
    uint8_t *buf = (uint8_t *)malloc(spec->bytes);
    if (!buf) {
        fclose(f);
        snprintf(err, errcap, "table %s: out of memory (%zu bytes)", spec->name, spec->bytes);
        return -1;
    }
    int ok = 0;
    if (fread(buf, 1, spec->bytes, f) != spec->bytes) {
        snprintf(err, errcap, "table %s: short read (want %zu bytes)", spec->name, spec->bytes);
    } else {
        /* Exact length plus EOF: reject a file that continues past the table. */
        int c = fgetc(f);
        if (c != EOF) {
            snprintf(err, errcap, "table %s: extra data after %zu bytes", spec->name, spec->bytes);
        } else if (ferror(f)) {
            snprintf(err, errcap, "table %s: read error at EOF", spec->name);
        } else {
            uint8_t digest[32];
            sr_vfpu_sha256(buf, spec->bytes, digest);
            if (!sr_hex64_eq(spec->sha256, digest)) {
                char got[65];
                sr_vfpu_fmt_hex(digest, got);
                snprintf(err, errcap,
                         "table %s: sha256 mismatch (want %s, got %s) -- not the "
                         "committed asset",
                         spec->name, spec->sha256, got);
            } else {
                ok = 1;
            }
        }
    }
    if (fclose(f) != 0 && ok) {
        snprintf(err, errcap, "table %s: close failed", spec->name);
        ok = 0;
    }
    if (!ok) {
        free(buf);
        return -1;
    }
    *out = buf;
    return 0;
}

/* ---- aggregate load + publish -------------------------------------------- */

static void sr_vfpu_assign(SrVfpuTables *t, uint8_t **bufs) {
    /* Order must match SR_VFPU_TABLES[] exactly (indices 0..14 below). */
    t->rcp = (const int8_t (*)[2])bufs[0];
    t->sqrt = (const int8_t (*)[2])bufs[1];
    t->rsqrt = (const int8_t (*)[2])bufs[2];
    t->sin8192 = (const uint32_t *)bufs[3];
    t->sin_delta = (const int8_t (*)[2])bufs[4];
    t->sin_interval_delta = (const int16_t *)bufs[5];
    t->sin_exceptions = (const uint8_t *)bufs[6];
    t->exp2_65536 = (const uint32_t *)bufs[7];
    t->exp2 = (const uint8_t (*)[2])bufs[8];
    t->log2_65536 = (const uint32_t *)bufs[9];
    t->log2_65536_quadratic = (const uint32_t *)bufs[10];
    t->log2 = (const uint8_t (*)[131072][2])bufs[11];
    t->asin_65536 = (const int32_t (*)[3])bufs[12];
    t->asin_deltas = (const uint64_t *)bufs[13];
    t->asin_indices = (const uint16_t *)bufs[14];
}

int sr_vfpu_tables_load(const char *root, SrVfpuTables *out,
                        char *err, size_t errcap) {
    size_t n = 0;
    const SrVfpuTableSpec *specs = sr_vfpu_table_manifest(&n);
    if (n != 15u) {
        snprintf(err, errcap, "internal manifest size mismatch (%zu)", n);
        return -1;
    }
    uint8_t *bufs[15];
    memset(bufs, 0, sizeof bufs);
    size_t loaded = 0;
    for (; loaded < n; loaded++) {
        if (sr_vfpu_read_table(root, &specs[loaded], &bufs[loaded], err, errcap) != 0) {
            /* err already holds the per-table diagnostic. */
            for (size_t i = 0; i <= loaded; i++) free(bufs[i]);
            return -1;
        }
    }
    /* Value-domain invariants before anything becomes globally visible. */
    int bad = 0;
    bad |= sr_vfpu_validate_asin_indices(
        (const uint16_t *)bufs[14],
        specs[14].bytes / specs[14].elem,
        specs[13].bytes / specs[13].elem);
    bad |= sr_vfpu_validate_sin_interval(
        (const int16_t *)bufs[5],
        specs[5].bytes / specs[5].elem,
        specs[6].bytes);
    if (bad != 0) {
        snprintf(err, errcap,
                 "table value-domain validation failed (asin indices or sin interval bounds)");
        for (size_t i = 0; i < n; i++) free(bufs[i]);
        return -1;
    }
    sr_vfpu_assign(out, bufs);
    return 0;
}

/* Global pointer set published only after the aggregate validates. */
const int8_t   (*sr_rcp_lut)[2] = NULL;
const int8_t   (*sr_sqrt_lut)[2] = NULL;
const int8_t   (*sr_rsqrt_lut)[2] = NULL;
const uint32_t *sr_sin_lut8192 = NULL;
const int8_t   (*sr_sin_lut_delta)[2] = NULL;
const int16_t  *sr_sin_lut_interval_delta = NULL;
const uint8_t  *sr_sin_lut_exceptions = NULL;
const uint32_t *sr_exp2_lut65536 = NULL;
const uint8_t  (*sr_exp2_lut)[2] = NULL;
const uint32_t *sr_log2_lut65536 = NULL;
const uint32_t *sr_log2_lut65536_quadratic = NULL;
const uint8_t  (*sr_log2_lut)[131072][2] = NULL;
const int32_t  (*sr_asin_lut65536)[3] = NULL;
const uint64_t *sr_asin_lut_deltas = NULL;
const uint16_t *sr_asin_lut_indices = NULL;

void sr_vfpu_tables_publish(const SrVfpuTables *t) {
    sr_rcp_lut = t->rcp;
    sr_sqrt_lut = t->sqrt;
    sr_rsqrt_lut = t->rsqrt;
    sr_sin_lut8192 = t->sin8192;
    sr_sin_lut_delta = t->sin_delta;
    sr_sin_lut_interval_delta = t->sin_interval_delta;
    sr_sin_lut_exceptions = t->sin_exceptions;
    sr_exp2_lut65536 = t->exp2_65536;
    sr_exp2_lut = t->exp2;
    sr_log2_lut65536 = t->log2_65536;
    sr_log2_lut65536_quadratic = t->log2_65536_quadratic;
    sr_log2_lut = t->log2;
    sr_asin_lut65536 = t->asin_65536;
    sr_asin_lut_deltas = t->asin_deltas;
    sr_asin_lut_indices = t->asin_indices;
}

/* Record the resolved manifest in run/build evidence so differential and
 * visual results are reproducible against a specific table set. */
static void sr_vfpu_evidence(const char *root, int overridden) {
    size_t n = 0;
    const SrVfpuTableSpec *specs = sr_vfpu_table_manifest(&n);
    fprintf(stderr, "[vfpu_tables] root=%s override=%s\n",
            root, overridden ? "PSP_VFPU_TABLES" : "default-assets/vfpu");
    for (size_t i = 0; i < n; i++) {
        fprintf(stderr, "[vfpu_tables] %s bytes=%zu sha256=%s\n",
                specs[i].name, specs[i].bytes, specs[i].sha256);
    }
    fflush(stderr);
}

/* 0=unloaded 2=ready 3=failed(abort policy); the spin lock is the loading
 * marker, so no separate "loading" state is needed. Zero-initialized
 * (ATOMIC_VAR_INIT is removed in C23, which the default GNU dialect now uses). */
static atomic_int g_vfpu_state = 0;
static atomic_flag g_vfpu_lock = ATOMIC_FLAG_INIT;

void sr_vfpu_load_with_root(const char *root) {
    if (atomic_load_explicit(&g_vfpu_state, memory_order_acquire) == 2)
        return;  /* fast path: already published */
    while (atomic_flag_test_and_set_explicit(&g_vfpu_lock, memory_order_acquire))
        ;  /* brief spin; the critical section runs exactly once */
    if (atomic_load_explicit(&g_vfpu_state, memory_order_relaxed) == 2) {
        atomic_flag_clear_explicit(&g_vfpu_lock, memory_order_release);
        return;
    }
    if (atomic_load_explicit(&g_vfpu_state, memory_order_relaxed) == 3) {
        atomic_flag_clear_explicit(&g_vfpu_lock, memory_order_release);
        fprintf(stderr, "[vfpu_tables] previous load failed; refusing to retry\n");
        abort();
    }
    int env_overridden = 0;
    const char *env_root = sr_vfpu_root(&env_overridden);
    const char *effective = root ? root : env_root;
    SrVfpuTables agg;
    memset(&agg, 0, sizeof agg);
    char err[512];
    if (sr_vfpu_tables_load(effective, &agg, err, sizeof err) != 0) {
        atomic_store_explicit(&g_vfpu_state, 3, memory_order_relaxed);
        atomic_flag_clear_explicit(&g_vfpu_lock, memory_order_release);
        /* Controlled failure with complete cleanup already happened inside the
         * loader; aborting here is the documented unrecoverable startup policy
         * (validated tables are required for correct transcendental math). */
        fprintf(stderr, "[vfpu_tables] FATAL: %s\n", err);
        abort();
    }
    sr_vfpu_tables_publish(&agg);
    atomic_store_explicit(&g_vfpu_state, 2, memory_order_release);
    atomic_flag_clear_explicit(&g_vfpu_lock, memory_order_release);
    /* The override flag must reflect whether PSP_VFPU_TABLES is really set:
     * a default run must not look like an override run (and vice versa). */
    sr_vfpu_evidence(effective, env_overridden);
}

void sr_vfpu_load(void) {
    sr_vfpu_load_with_root(NULL);
}
