/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
#ifndef SR_PRX_LOADER_H
#define SR_PRX_LOADER_H

/* Clean-room PSP module (PRX) loader interface.
 * Spec: docs/cleanroom/PRX_LOADER_SPEC.md, sections 2-3.
 * C11, only C standard library headers used here.
 */

#include <stdbool.h>
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

/*
 * Where the PSP starts an executable: the module manager's start entry.
 *
 * The module manager's StartModule enters a module at its syslib module_start export
 * (NID 0xD632ACDB) when the module exports one, otherwise at its module_bootstart export
 * (NID 0xD3744BE0), otherwise at the ELF entry; e_entry is only the last resort. Behaviour
 * as documented by uofw (github.com/uofw/uofw 28306bb5, src/kd/modulemgr/modulemgr.c
 * StartModule and _ProcessModuleExportEnt, src/kd/loadcore/loadelf.c): the module info
 * record sits at the file offset the first PT_LOAD's p_paddr names (masked with
 * 0x1FFFFFFF) when p_paddr differs from p_vaddr, and its export table lists, per library,
 * the function NIDs, then the variable NIDs, then the addresses in the same order.
 *
 * Static inline so the inspector (src/core/nk_iso.c), the staged-ELF launch check
 * (src/core/nk_launch.c) and the runtime's ELF mode (driver.c) read one definition. The
 * Python mirror is tools/nk_core/iso_inspect.py _elf32_psp_start_entry;
 * tools/test_iso_parity.py keeps the two in agreement. No uofw source is copied.
 */
#define SR_PRX_NID_MODULE_START 0xD632ACDBu
#define SR_PRX_NID_MODULE_BOOTSTART 0xD3744BE0u
#define SR_PRX_START_ENTRY_MAX_PROGRAM_HEADERS 128u
#define SR_PRX_START_ENTRY_MAX_LIBRARIES 256u
#define SR_PRX_START_ENTRY_MAX_EXPORTS 4096u

/* Read exactly `bytes` bytes at `offset` of the image; false when not fully readable. */
typedef bool (*SrPrxImageRead)(void *context, uint64_t offset, void *dst, uint32_t bytes);

static inline uint32_t sr_prx_start_entry_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* File offset of guest (link-space) address `address` with `bytes` file-backed bytes, or
 * false when no PT_LOAD maps the whole range from the file. */
static inline bool sr_prx_start_entry_map(const uint8_t *program_headers, uint32_t count,
                                          uint32_t address, uint32_t bytes, uint64_t *offset) {
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *ph = program_headers + (size_t)i * 32u;
        if (sr_prx_start_entry_le32(ph) != 1u) continue;
        uint32_t p_offset = sr_prx_start_entry_le32(ph + 4);
        uint32_t p_vaddr = sr_prx_start_entry_le32(ph + 8);
        uint32_t p_filesz = sr_prx_start_entry_le32(ph + 16);
        if (address >= p_vaddr && (uint64_t)address - p_vaddr + bytes <= p_filesz) {
            *offset = (uint64_t)p_offset + (address - p_vaddr);
            return true;
        }
    }
    return false;
}

/* The start entry in the image's own (link) address space. Returns NULL with *entry set,
 * or the name of the first rule the image breaks: an image whose module info or export
 * table does not lie in the file is refused rather than started at a guessed address. */
static inline const char *sr_prx_start_entry(SrPrxImageRead read, void *context,
                                             uint64_t image_size, uint32_t *entry) {
    uint8_t header[52];
    if (image_size < sizeof header || !read(context, 0, header, sizeof header)) {
        return "header-truncated";
    }
    uint32_t e_entry = sr_prx_start_entry_le32(header + 24);
    uint32_t phoff = sr_prx_start_entry_le32(header + 28);
    uint32_t phentsize = (uint32_t)header[42] | ((uint32_t)header[43] << 8);
    uint32_t phnum = (uint32_t)header[44] | ((uint32_t)header[45] << 8);
    *entry = e_entry;
    if (phentsize != 32u || phnum == 0 || phnum > SR_PRX_START_ENTRY_MAX_PROGRAM_HEADERS ||
        (uint64_t)phoff + 32u * phnum > image_size) {
        return "program-table-bounds";
    }
    uint8_t program_headers[SR_PRX_START_ENTRY_MAX_PROGRAM_HEADERS * 32u];
    if (!read(context, phoff, program_headers, 32u * phnum)) return "image-read-failed";

    const uint8_t *first_load = NULL;
    for (uint32_t i = 0; i < phnum && first_load == NULL; i++) {
        if (sr_prx_start_entry_le32(program_headers + (size_t)i * 32u) == 1u) {
            first_load = program_headers + (size_t)i * 32u;
        }
    }
    if (first_load == NULL) return NULL;
    uint32_t p_vaddr = sr_prx_start_entry_le32(first_load + 8);
    uint32_t p_paddr = sr_prx_start_entry_le32(first_load + 12);
    if (p_paddr == p_vaddr) return NULL;  /* no module info: the ELF entry */

    uint64_t info_offset = p_paddr & 0x1FFFFFFFu;
    uint8_t info[52];
    if (info_offset + sizeof info > image_size || !read(context, info_offset, info, sizeof info)) {
        return "module-info-range";
    }
    uint32_t ent_top = sr_prx_start_entry_le32(info + 36);
    uint32_t ent_end = sr_prx_start_entry_le32(info + 40);
    if (ent_end < ent_top) return "module-exports-range";

    bool have_start = false, have_bootstart = false;
    uint32_t start = 0, bootstart = 0;
    uint32_t cursor = ent_top;
    for (uint32_t library = 0; cursor < ent_end; library++) {
        uint8_t record[16];
        uint64_t record_offset = 0;
        if (library >= SR_PRX_START_ENTRY_MAX_LIBRARIES || ent_end - cursor < sizeof record ||
            !sr_prx_start_entry_map(program_headers, phnum, cursor, sizeof record, &record_offset) ||
            !read(context, record_offset, record, sizeof record)) {
            return "module-exports-range";
        }
        uint32_t words = record[8];
        uint32_t variables = record[9];
        uint32_t functions = (uint32_t)record[10] | ((uint32_t)record[11] << 8);
        if (words < 4u) return "module-exports-range";
        if (sr_prx_start_entry_le32(record) == 0u && functions != 0u) {
            /* syslib: the module manager reads the first one it finds. */
            uint32_t total = functions + variables;
            uint64_t table_offset = 0;
            if (total > SR_PRX_START_ENTRY_MAX_EXPORTS ||
                !sr_prx_start_entry_map(program_headers, phnum,
                                        sr_prx_start_entry_le32(record + 12),
                                        8u * total, &table_offset)) {
                return "module-exports-range";
            }
            for (uint32_t i = 0; i < functions; i++) {
                uint8_t nid[4], address[4];
                if (!read(context, table_offset + 4u * i, nid, 4) ||
                    !read(context, table_offset + 4u * (total + i), address, 4)) {
                    return "image-read-failed";
                }
                uint32_t value = sr_prx_start_entry_le32(nid);
                if (value == SR_PRX_NID_MODULE_START && !have_start) {
                    have_start = true;
                    start = sr_prx_start_entry_le32(address);
                } else if (value == SR_PRX_NID_MODULE_BOOTSTART && !have_bootstart) {
                    have_bootstart = true;
                    bootstart = sr_prx_start_entry_le32(address);
                }
            }
            break;
        }
        if ((uint64_t)cursor + 4u * words > ent_end) return "module-exports-range";
        cursor += 4u * words;
    }
    if (have_start) {
        *entry = start;
    } else if (have_bootstart) {
        *entry = bootstart;
    }
    return NULL;
}

#endif
