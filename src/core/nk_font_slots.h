/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
#ifndef NK_FONT_SLOTS_H
#define NK_FONT_SLOTS_H

/* Font slot ids shared by the flash0: device (src/rt/flash0_font.c) and the import
 * path that writes the per-user cache (FONT_PLAN sections 5.2 and 7.1). This is the
 * one definition: include it, do not copy the enum or the cache layout. */

typedef enum {
    NK_FONT_SLOT_LATIN = 0,
    NK_FONT_SLOT_JAPANESE = 1,
    NK_FONT_SLOT_KOREAN = 2,
    NK_FONT_SLOT_COUNT = 3
} NkFontSlot;

/* Per-user cache: <user data>/fonts/v2/<stem>.pgf, one file per slot. The project build
 * output uses the same <stem>.pgf names in its own directory. */
#define NK_FONT_CACHE_SUBDIR "fonts/v2"

/* File stem of a slot, without the extension. NULL for an unknown slot. */
static inline const char *nk_font_slot_file_stem(NkFontSlot slot) {
    switch (slot) {
    case NK_FONT_SLOT_LATIN:    return "latin";
    case NK_FONT_SLOT_JAPANESE: return "japanese";
    case NK_FONT_SLOT_KOREAN:   return "korean";
    default:                    return NULL;
    }
}

#endif /* NK_FONT_SLOTS_H */
