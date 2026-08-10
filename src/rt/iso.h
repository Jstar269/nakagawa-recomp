// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)

#ifndef PSP_RECOMP_ISO_H
#define PSP_RECOMP_ISO_H

#include <stdint.h>

int iso_init(void);
/* Resolve a guest path ("disc0:/PSP_GAME/...") to its extent. Returns 0 on success. */
int iso_lookup(const char *guest_path, uint32_t *out_lba, uint32_t *out_size);
/* Read bytes from (lba*2048 + offset). Returns bytes read. */
int iso_read(uint32_t lba, uint32_t offset, void *dst, uint32_t bytes);

/* Return the first physical LBA for an iso_lookup token (multi-extent tokens are opaque). */
uint32_t iso_physical_lba(uint32_t lba_or_token);

typedef struct IsoDirEntry {
    char name[256];
    uint32_t lba;
    uint32_t size;
    int is_dir;
    int is_symlink;
} IsoDirEntry;

/* Return one cached child of a directory.  1=entry, 0=end, -1=invalid/not a directory. */
int iso_list(const char *guest_path, uint32_t index, IsoDirEntry *out);

#endif
