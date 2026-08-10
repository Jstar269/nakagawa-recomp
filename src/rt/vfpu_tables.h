/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Fail-closed loading, validation and once-only publication of the VFPU
 * transcendental lookup tables (issue #187). The table DATA originates from
 * PPSSPP (assets/vfpu/PROVENANCE.json records the exact upstream revision and
 * per-file SHA-256); this module is an independent loader/validator.
 *
 * Every table is validated before publication:
 *   - the resolved root path is checked for truncation;
 *   - each file must be exactly the manifest byte length, end exactly at EOF,
 *     and match the committed SHA-256 (so an arbitrary same-sized file is
 *     rejected, not trusted);
 *   - value-domain invariants are enforced where table bytes become array
 *     indices or search bounds (asin indices, sin interval deltas);
 *   - all temporary buffers are released on any failure and the global pointer
 *     set is only published after the whole aggregate validates.
 * sr_vfpu_load() additionally serializes concurrent first use and treats a
 * failed load as a documented unrecoverable startup failure (after full
 * cleanup): without the tables the transcendentals would silently compute
 * wrong math, so aborting is the fail-closed policy.
 */
#ifndef SR_VFPU_TABLES_H
#define SR_VFPU_TABLES_H

#include <stddef.h>
#include <stdint.h>

/* Semantic limits shared with the runtime hot paths (defense in depth). */
#define SR_VFPU_ASIN_DELTAS_ENTRIES 64681u   /* vfpu_asin_lut_deltas.dat / 8 */
#define SR_VFPU_SIN_EXCEPTIONS_BYTES 86938u  /* vfpu_sin_lut_exceptions.dat */

typedef struct {
    const char *name;     /* file name under the table root */
    size_t bytes;         /* exact byte length of the committed asset */
    size_t elem;          /* element size of the typed view (1/2/4/8) */
    const char *sha256;   /* 64 lowercase hex chars of the committed asset */
} SrVfpuTableSpec;

/* Typed aggregate of every table, filled by sr_vfpu_tables_load(). */
typedef struct {
    const int8_t   (*rcp)[2];
    const int8_t   (*sqrt)[2];
    const int8_t   (*rsqrt)[2];
    const uint32_t *sin8192;
    const int8_t   (*sin_delta)[2];
    const int16_t  *sin_interval_delta;
    const uint8_t  *sin_exceptions;
    const uint32_t *exp2_65536;
    const uint8_t  (*exp2)[2];
    const uint32_t *log2_65536;
    const uint32_t *log2_65536_quadratic;
    const uint8_t  (*log2)[131072][2];
    const int32_t  (*asin_65536)[3];
    const uint64_t *asin_deltas;
    const uint16_t *asin_indices;
} SrVfpuTables;

/* Manifest of every table the runtime requires. Returns the entry count. */
const SrVfpuTableSpec *sr_vfpu_table_manifest(size_t *count_out);

/* FIPS-180-4 SHA-256 over a buffer (exported for the selftest). */
void sr_vfpu_sha256(const uint8_t *data, size_t size, uint8_t digest[32]);

/* Value-domain validators (exported for the selftest; the loader calls them
 * before publication). Return 0 when the data satisfies the invariant.
 *   - asin indices: every uint16 must be a valid index into the deltas table.
 *   - sin interval: for every reachable interval the reconstructed lo/hi
 *     binary-search bounds must stay within the exceptions allocation
 *     (lo <= hi <= SR_VFPU_SIN_EXCEPTIONS_BYTES; m = (lo+hi)/2 < hi, so
 *     hi <= len keeps every read in bounds -- genuine data maxes at exactly
 *     hi == len for 22 of 65536 intervals). */
int sr_vfpu_validate_asin_indices(const uint16_t *indices, size_t count,
                                  size_t deltas_entries);
int sr_vfpu_validate_sin_interval(const int16_t *delta, size_t count,
                                  size_t exceptions_bytes);

/* Load, hash-validate and semantically validate every table under `root` into
 * `out` WITHOUT touching global state. Returns 0 on success. On failure all
 * temporary allocations are released, `out` is left untouched, and a
 * diagnostic is written into `err` (if non-NULL, errcap bytes). */
int sr_vfpu_tables_load(const char *root, SrVfpuTables *out,
                        char *err, size_t errcap);

/* Publish a validated aggregate as the runtime's global table pointers. */
void sr_vfpu_tables_publish(const SrVfpuTables *t);

/* Runtime globals consumed directly by the hot transcendental kernels in
 * recomp.c. NULL until the first successful load. */
extern const int8_t   (*sr_rcp_lut)[2];
extern const int8_t   (*sr_sqrt_lut)[2];
extern const int8_t   (*sr_rsqrt_lut)[2];
extern const uint32_t *sr_sin_lut8192;
extern const int8_t   (*sr_sin_lut_delta)[2];
extern const int16_t  *sr_sin_lut_interval_delta;
extern const uint8_t  *sr_sin_lut_exceptions;
extern const uint32_t *sr_exp2_lut65536;
extern const uint8_t  (*sr_exp2_lut)[2];
extern const uint32_t *sr_log2_lut65536;
extern const uint32_t *sr_log2_lut65536_quadratic;
extern const uint8_t  (*sr_log2_lut)[131072][2];
extern const int32_t  (*sr_asin_lut65536)[3];
extern const uint64_t *sr_asin_lut_deltas;
extern const uint16_t *sr_asin_lut_indices;

/* Once-only synchronized load: resolve the root (PSP_VFPU_TABLES override or
 * assets/vfpu), load+validate, publish atomically. Safe for concurrent first
 * use. On failure: cleanup, print diagnostics, abort (documented policy). */
void sr_vfpu_load(void);

/* Same, with an explicit root; NULL selects the environment/default root.
 * Primary consumers: the selftest (to race a specific tree) and embedders
 * that want to pin the table set. Production uses sr_vfpu_load(). */
void sr_vfpu_load_with_root(const char *root);

#endif /* SR_VFPU_TABLES_H */
