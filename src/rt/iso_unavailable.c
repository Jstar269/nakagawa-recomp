// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * Public-safe ISO boundary.
 *
 * The title-facing ISO/VFS implementation is excluded from the first public
 * profile.  Returning errors here keeps a generic build linkable without
 * fabricating a disc, an extent, or file contents.
 */

#include "iso.h"

int iso_init(void) { return -1; }

int iso_lookup(const char *guest_path, uint32_t *out_lba, uint32_t *out_size) {
    (void)guest_path;
    (void)out_lba;
    (void)out_size;
    return -1;
}

int iso_read(uint32_t lba, uint32_t offset, void *dst, uint32_t bytes) {
    (void)lba;
    (void)offset;
    (void)dst;
    (void)bytes;
    return -1;
}

uint32_t iso_physical_lba(uint32_t lba_or_token) {
    (void)lba_or_token;
    return 0;
}

int iso_list(const char *guest_path, uint32_t index, IsoDirEntry *out) {
    (void)guest_path;
    (void)index;
    (void)out;
    return -1;
}
