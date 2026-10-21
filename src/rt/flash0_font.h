/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
#ifndef SR_FLASH0_FONT_H
#define SR_FLASH0_FONT_H

/* Read-only flash0: device for the font directory (FONT_PLAN section 5).
 *
 * Only flash0:/font/<served name> is served. Japanese and Korean keep their existing slot
 * resolution. Each Latin registry index maps to ltn0.pgf..ltn15.pgf and resolves to its exact
 * imported file, the nearest imported file in the same nominal-size group, the project Latin PGF,
 * or a legacy unindexed import only when the project PGF is absent. Indexed cache files are served
 * only when the manifest vouches for their size, SHA-256 and reader verdict. Every Latin fallback
 * is named in the user status and one startup log line per index. Every write, create,
 * remove, rename and attribute change is refused with EACCES. The HLE hooks in hle.c call
 * these functions; the roots are supplied by the caller.
 *
 * The served names, cache names and cache directory come from nk_font_slots.h, which is shared
 * with the import flow. The slot table in flash0_font.c records what a PSP-3000 measurement fixed
 * (the font list and its order) and what it did not (the registry's own key names and types, and
 * the path_name string); see the registry view below. */

#include <stdint.h>
#include <stdio.h>

#include "../core/nk_font_slots.h"
#include "asset_index.h"

#define SR_FLASH0_PATH_MAX 1024

/* PSP error codes: EACCES for a refused write, and not-found for anything the device does not
 * serve. Not-found is 0x80010002u (ENOENT), the code the rest of the VFS returns for a missing
 * object (sr_cd_psp_error, SR_CD_NOT_FOUND); 0x80010014u is ENOTDIR. The console's value for
 * this device is UNMEASURED. */
#define SR_FLASH0_ERR_ACCESS    0x8001000Du
#define SR_FLASH0_ERR_NOT_FOUND 0x80010002u

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

/* Resolves one slot the way the device serves it (the user-imported cache, vouched for by its
 * manifest, then the project font directory, with the same size probe) and returns the host path
 * of the file that would be served, with *source_out = "user-imported" or "project". Returns 0
 * with an empty path and *source_out = "none" when no source has the slot. The HLE sceFont shim
 * loads its fonts through this, so both font paths agree on which file a slot is. */
int sr_flash0_font_resolve_path(const Flash0Sources *sources, NkFontSlot slot, char *path_out,
                                size_t capacity, const char **source_out);

/* Resolves one Latin registry file and returns the host path and source. served_index_out is
 * 0..15 for an indexed cache file, -1 for the project font, or -2 for a legacy unindexed cache
 * file. Returns 0 when no source can serve the requested Latin index. */
int sr_flash0_font_resolve_latin_index_path(const Flash0Sources *sources, int index,
                                           char *path_out, size_t capacity,
                                           const char **source_out, int *served_index_out);

/* The slot's lower-case name for diagnostics ("japanese", "latin", "korean"). */
const char *sr_flash0_font_slot_label(NkFontSlot slot);

/* ---- Registry view of the font device (FONT_PLAN section 4) ----
 *
 * The guest's own font library asks the system registry for the font list before it opens a
 * font: /DATA/FONT holds path_name (where the fonts are served), num_fonts, and one
 * PROPERTY/INFO<n> record per font index. This view is what the device knows about each index.
 *
 * MEASURED (PSP-3000, firmware 6.6.1, 2026-10-15, the console's own font library asked for the
 * system font list): there are SR_FONT_REGISTRY_COUNT indices; index 0 is the Japanese font,
 * indices 1 to 16 are Latin fonts, and index 17 is the Korean font; every index's numeric codes
 * (family, style, sub-style, language, region, country, weight, attributes and expiry) are the
 * rows of the table in flash0_font.c.
 *
 * NOT MEASURED: the registry's own key names and types, the unit its size values are held in,
 * and the path_name string. The names follow FONT_PLAN 4.2 and the keys the guest library has
 * been seen to ask for; the size values hold the served file's own 26.6 header words (the same
 * unit the PGF header uses); path_name holds the device root. All three are project choices.
 *
 * Per-font values come from the served file's header at run time, never from a table: sizes and
 * resolutions (header 0x24 to 0x30) and the font name (0x35). An index whose slot has no served
 * font, or whose served header is unusable, is absent, with a named reason; nothing is invented.
 *
 * The guest library walks the INFO entries in index order and stops at the first one missing, so
 * a bold or italic Latin index is listed too. No project font is bold or italic (FONT_PLAN D10),
 * so such an index is served from the Latin slot's regular face and reports the Regular style
 * rather than the measured one (served_as_regular says so). Bold is never synthesized. */
#define SR_FONT_REGISTRY_COUNT 18u
#define SR_FONT_REGISTRY_NAME_BYTES 64u /* the header's font-name field, with room for the NUL */

typedef enum SrFontRegistryStatus {
    SR_FONT_REGISTRY_PRESENT = 0,
    SR_FONT_REGISTRY_NO_SOURCE,         /* the index's slot has no user-imported or project font */
    SR_FONT_REGISTRY_HEADER_TRUNCATED,  /* the served file is shorter than a PGF header */
    SR_FONT_REGISTRY_HEADER_SHAPE,      /* header offset, size or magic is not a PGF header */
    SR_FONT_REGISTRY_HEADER_SIZES,      /* a nominal size or resolution is not positive */
    SR_FONT_REGISTRY_HEADER_NAME,       /* the font name is empty or not printable ASCII */
    SR_FONT_REGISTRY_BAD_INDEX
} SrFontRegistryStatus;

typedef struct {
    NkFontSlot slot;
    const char *source;     /* "user-imported" or "project" */
    const char *file_name;  /* the served name under flash0:/font/ */
    /* From the served file's header (26.6 fixed point, as stored): */
    int32_t h_size, v_size, h_resolution, v_resolution;
    char font_name[SR_FONT_REGISTRY_NAME_BYTES]; /* NUL-terminated, at most 63 characters */
    /* Measured codes of the index (style is Regular when served_as_regular): */
    int served_as_regular;  /* the measured style is bold or italic and the Regular face is served */
    uint32_t weight, family_code, style, sub_style;
    uint32_t language_code, region_code, country_code;
    uint32_t extra_attributes, expire_date;
} SrFontRegistryInfo;

/* The registry's path_name value: the device directory the served names sit in, with no trailing
 * separator (the guest's library adds one when it joins a file name). */
const char *sr_flash0_font_registry_path_name(void);

/* Fills *info for font index `index` and returns SR_FONT_REGISTRY_PRESENT, or returns why the
 * index is absent (info is then left zeroed). Reads the served file's header; opens and closes
 * the file. */
SrFontRegistryStatus sr_flash0_font_registry_info(const Flash0Sources *sources, uint32_t index,
                                                  SrFontRegistryInfo *info);

/* The slot that serves font index `index`: returns 0 for an index past the list. */
int sr_flash0_font_registry_slot(uint32_t index, NkFontSlot *slot_out);

/* A short sentence for a status, for diagnostics. */
const char *sr_flash0_font_registry_status_text(SrFontRegistryStatus status);

#ifdef SR_HLE_THREAD_SELFTEST
/* Test seam: marks a slot measured (non-zero) or pending (zero). Compiled only into the HLE
 * selftest, which stands in for the console measurement. */
void sr_flash0_font_selftest_set_measured(NkFontSlot slot, int measured);
#endif

#endif /* SR_FLASH0_FONT_H */
