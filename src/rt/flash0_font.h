/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
#ifndef SR_FLASH0_FONT_H
#define SR_FLASH0_FONT_H

/* Read-only flash0: device for the font directory (FONT_PLAN section 5).
 *
 * Only flash0:/font/<served name> is served, one file per font slot. A slot's file comes
 * from the per-user cache first, then the project's fonts, and otherwise the open is refused
 * by name. There is no cross-slot substitution and no synthetic font. Every write, create,
 * remove, rename and attribute change is refused with EACCES. The HLE hooks in hle.c call
 * these functions; the roots are supplied by the caller.
 *
 * The served names, cache names and cache directory come from nk_font_slots.h, which is shared
 * with the import flow. The slot table in flash0_font.c is pending and fails closed until the
 * console measurement fixes the lookup and the slot order. */

#include <stdint.h>
#include <stdio.h>

#include "../core/nk_font_slots.h"
#include "asset_index.h"

#define SR_FLASH0_PATH_MAX 1024

/* PSP error codes: EACCES for a refused write, and not-found for anything the device does not
 * serve. */
#define SR_FLASH0_ERR_ACCESS    0x8001000Du
#define SR_FLASH0_ERR_NOT_FOUND 0x80010014u

/* Font source roots, UTF-8. An empty string means the root is unknown and is skipped. */
typedef struct {
    char user_data_dir[SR_FLASH0_PATH_MAX];  /* per-user data root; the cache is under it */
    char project_dir[SR_FLASH0_PATH_MAX];    /* project font directory (SR_FONTDIR rule) */
} Flash0Sources;

/* Non-zero when the guest path names the flash0: device (case-insensitive prefix). */
int sr_flash0_font_is_device_path(const char *guest_path);

/* Opens a served font read-only. On success *host_out is an open FILE that the caller owns
 * and *size_out is its size. Any write intent in flags is refused. */
uint32_t sr_flash0_font_open(const char *guest_path, uint32_t flags,
                             const Flash0Sources *sources,
                             FILE **host_out, uint32_t *size_out);

/* Size and kind for sceIoGetstat. flash0:/font is a directory; a served font is a file. */
uint32_t sr_flash0_font_stat(const char *guest_path, const Flash0Sources *sources,
                             uint32_t *size_out, int *is_dir_out);

/* Lists the served fonts under flash0:/font, sorted by name. The caller initializes list. */
uint32_t sr_flash0_font_list_dir(const char *guest_path, const Flash0Sources *sources,
                                 SrVfsDirList *list);

/* Named refusal for a write-side operation on a flash0: path. Returns EACCES. */
uint32_t sr_flash0_font_refuse_write(const char *guest_path, const char *operation);

/* Resolves one slot the way the device serves it (the user-imported cache, then the project
 * font directory, with the same size probe) and returns the host path of the file that would
 * be served, with *source_out = "user-imported" or "project". Returns 0 with an empty path
 * and *source_out = "none" when no source has the slot. The HLE sceFont shim loads its fonts
 * through this, so both font paths agree on which file a slot is. */
int sr_flash0_font_resolve_path(const Flash0Sources *sources, NkFontSlot slot, char *path_out,
                                size_t capacity, const char **source_out);

#ifdef SR_HLE_THREAD_SELFTEST
/* Test seam: marks a slot measured (non-zero) or pending (zero). Compiled only into the HLE
 * selftest, which stands in for the console measurement. */
void sr_flash0_font_selftest_set_measured(NkFontSlot slot, int measured);
#endif

#endif /* SR_FLASH0_FONT_H */
