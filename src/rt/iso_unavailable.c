// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * Public-safe ISO boundary.
 *
 * The title-facing ISO/VFS implementation is excluded from the first public
 * profile.  Returning errors here keeps a generic build linkable without
 * fabricating a disc, an extent, or file contents.
 */

#include <stdio.h>
#include "iso.h"

static int s_iso_warned = 0;

static void warn_public_safe_iso(const char *op, const char *path) {
    if (!s_iso_warned) {
        s_iso_warned = 1;
        if (path) {
            fprintf(stderr, "[rt:iso] %s: disc access for '%s' is unavailable in public-safe build (PUBLIC_SAFE=1)\n", op, path);
        } else {
            fprintf(stderr, "[rt:iso] %s: disc access is unavailable in public-safe build (PUBLIC_SAFE=1)\n", op);
        }
    }
}

int iso_init(void) {
    warn_public_safe_iso("iso_init", NULL);
    return -1;
}

int iso_lookup(const char *guest_path, uint32_t *out_lba, uint32_t *out_size) {
    (void)out_lba;
    (void)out_size;
    warn_public_safe_iso("iso_lookup", guest_path);
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
