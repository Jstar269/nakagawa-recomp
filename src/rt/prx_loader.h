/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
#ifndef SR_PRX_LOADER_H
#define SR_PRX_LOADER_H

/* Clean-room PSP module (PRX) loader interface.
 * Spec: docs/cleanroom/PRX_LOADER_SPEC.md, sections 2-3.
 * C11, only C standard library headers used here.
 */

#include <stddef.h>
#include <stdint.h>

#define SR_PRX_MAX_SEGMENTS 4
#define SR_PRX_MODNAME_LEN 28
#define SR_PRX_LIBNAME_LEN 256

typedef struct SrPrxSegment {
    uint32_t file_offset;
    uint32_t guest_addr;
    uint32_t file_size;
    uint32_t mem_size;
} SrPrxSegment;

typedef struct SrPrxExport {
    char lib[SR_PRX_LIBNAME_LEN];
    uint32_t nid;
    uint32_t addr;
    int is_func;
} SrPrxExport;

typedef struct SrPrxImportStub {
    char lib[SR_PRX_LIBNAME_LEN];
    uint32_t nid;
    uint32_t stub_addr;
} SrPrxImportStub;

typedef struct SrPrxImage {
    char modname[SR_PRX_MODNAME_LEN];
    uint16_t attr;
    uint8_t version[2];
    uint32_t gp;
    uint32_t entry;
    uint32_t mod_start;
    int has_mod_start;
    uint32_t start;
    uint32_t end;
    uint32_t nseg;
    SrPrxSegment segs[SR_PRX_MAX_SEGMENTS];
    uint32_t modinfo_addr;
    uint32_t nexp;
    SrPrxExport *exps;
    uint32_t nimp;
    SrPrxImportStub *imps;
} SrPrxImage;

int sr_prx_load(const char *host_path, uint32_t base, SrPrxImage *out,
                char *err, size_t errlen);
int sr_prx_load_from_memory(const unsigned char *data, size_t size,
                            uint32_t base, SrPrxImage *out,
                            char *err, size_t errlen);
void sr_prx_image_free(SrPrxImage *out);

/* Defined by the caller (runtime or test harness). Copies n host bytes to
 * guest memory. Returns 0 on success, nonzero when the range is not
 * writable guest memory. This is the only way the loader touches guest
 * memory. Declared here so the loader can call it without a runtime header.
 */
int sr_prx_guest_write(uint32_t guest_addr, const void *src, uint32_t n);

#endif
