// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

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
 * Header-only so the inspector (nk_iso.c), the staged-ELF launch check (nk_launch.c) and
 * the runtime's ELF mode (src/rt/driver.c) read one definition. The Python mirror is
 * tools/nk_core/iso_inspect.py _elf32_psp_start_entry; tools/test_iso_parity.py keeps the
 * two in agreement. Project-authored; no uofw source is copied.
 */
#ifndef NK_PSP_START_ENTRY_H
#define NK_PSP_START_ENTRY_H

#include <stdbool.h>
#include <stdint.h>

#define NK_PSP_NID_MODULE_START 0xD632ACDBu
#define NK_PSP_NID_MODULE_BOOTSTART 0xD3744BE0u
#define NK_PSP_START_ENTRY_MAX_PROGRAM_HEADERS 128u
#define NK_PSP_START_ENTRY_MAX_LIBRARIES 256u
#define NK_PSP_START_ENTRY_MAX_EXPORTS 4096u

/* Read exactly `bytes` bytes at `offset` of the image; false when not fully readable. */
typedef bool (*NkPspImageRead)(void *context, uint64_t offset, void *dst, uint32_t bytes);

static inline uint32_t nk_psp_start_entry_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* File offset of guest (link-space) address `address` with `bytes` file-backed bytes, or
 * false when no PT_LOAD maps the whole range from the file. */
static inline bool nk_psp_start_entry_map(const uint8_t *program_headers, uint32_t count,
                                          uint32_t address, uint32_t bytes, uint64_t *offset) {
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *ph = program_headers + (size_t)i * 32u;
        if (nk_psp_start_entry_le32(ph) != 1u) continue;
        uint32_t p_offset = nk_psp_start_entry_le32(ph + 4);
        uint32_t p_vaddr = nk_psp_start_entry_le32(ph + 8);
        uint32_t p_filesz = nk_psp_start_entry_le32(ph + 16);
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
static inline const char *nk_psp_start_entry(NkPspImageRead read, void *context,
                                             uint64_t image_size, uint32_t *entry) {
    uint8_t header[52];
    if (image_size < sizeof header || !read(context, 0, header, sizeof header)) {
        return "header-truncated";
    }
    uint32_t e_entry = nk_psp_start_entry_le32(header + 24);
    uint32_t phoff = nk_psp_start_entry_le32(header + 28);
    uint32_t phentsize = (uint32_t)header[42] | ((uint32_t)header[43] << 8);
    uint32_t phnum = (uint32_t)header[44] | ((uint32_t)header[45] << 8);
    *entry = e_entry;
    if (phentsize != 32u || phnum == 0 || phnum > NK_PSP_START_ENTRY_MAX_PROGRAM_HEADERS ||
        (uint64_t)phoff + 32u * phnum > image_size) {
        return "program-table-bounds";
    }
    uint8_t program_headers[NK_PSP_START_ENTRY_MAX_PROGRAM_HEADERS * 32u];
    if (!read(context, phoff, program_headers, 32u * phnum)) return "image-read-failed";

    const uint8_t *first_load = NULL;
    for (uint32_t i = 0; i < phnum && first_load == NULL; i++) {
        if (nk_psp_start_entry_le32(program_headers + (size_t)i * 32u) == 1u) {
            first_load = program_headers + (size_t)i * 32u;
        }
    }
    if (first_load == NULL) return NULL;
    uint32_t p_vaddr = nk_psp_start_entry_le32(first_load + 8);
    uint32_t p_paddr = nk_psp_start_entry_le32(first_load + 12);
    if (p_paddr == p_vaddr) return NULL;  /* no module info: the ELF entry */

    uint64_t info_offset = p_paddr & 0x1FFFFFFFu;
    uint8_t info[52];
    if (info_offset + sizeof info > image_size || !read(context, info_offset, info, sizeof info)) {
        return "module-info-range";
    }
    uint32_t ent_top = nk_psp_start_entry_le32(info + 36);
    uint32_t ent_end = nk_psp_start_entry_le32(info + 40);
    if (ent_end < ent_top) return "module-exports-range";

    bool have_start = false, have_bootstart = false;
    uint32_t start = 0, bootstart = 0;
    uint32_t cursor = ent_top;
    for (uint32_t library = 0; cursor < ent_end; library++) {
        uint8_t record[16];
        uint64_t record_offset = 0;
        if (library >= NK_PSP_START_ENTRY_MAX_LIBRARIES || ent_end - cursor < sizeof record ||
            !nk_psp_start_entry_map(program_headers, phnum, cursor, sizeof record, &record_offset) ||
            !read(context, record_offset, record, sizeof record)) {
            return "module-exports-range";
        }
        uint32_t words = record[8];
        uint32_t variables = record[9];
        uint32_t functions = (uint32_t)record[10] | ((uint32_t)record[11] << 8);
        if (words < 4u) return "module-exports-range";
        if (nk_psp_start_entry_le32(record) == 0u && functions != 0u) {
            /* syslib: the module manager reads the first one it finds. */
            uint32_t total = functions + variables;
            uint64_t table_offset = 0;
            if (total > NK_PSP_START_ENTRY_MAX_EXPORTS ||
                !nk_psp_start_entry_map(program_headers, phnum,
                                        nk_psp_start_entry_le32(record + 12),
                                        8u * total, &table_offset)) {
                return "module-exports-range";
            }
            for (uint32_t i = 0; i < functions; i++) {
                uint8_t nid[4], address[4];
                if (!read(context, table_offset + 4u * i, nid, 4) ||
                    !read(context, table_offset + 4u * (total + i), address, 4)) {
                    return "image-read-failed";
                }
                uint32_t value = nk_psp_start_entry_le32(nid);
                if (value == NK_PSP_NID_MODULE_START && !have_start) {
                    have_start = true;
                    start = nk_psp_start_entry_le32(address);
                } else if (value == NK_PSP_NID_MODULE_BOOTSTART && !have_bootstart) {
                    have_bootstart = true;
                    bootstart = nk_psp_start_entry_le32(address);
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

#endif /* NK_PSP_START_ENTRY_H */
