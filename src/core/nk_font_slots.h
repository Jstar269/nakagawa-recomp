/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The per-user system-font layout, defined once for every consumer: the player, nk_cli
 * (tools/nk_core/fonts.py mirrors these values and a test pins them), and the flash0 font
 * device (WP-F), which serves the slot files under flash0:/font/.
 *
 * Two font sources are served per slot, in order: the user's own imported font, then the
 * project's font. A slot with neither is a named refusal. No key, decryption, or upload is
 * involved. Slot files are named by their served project names, so a slot's cache file and
 * its project file carry the same name. */

#ifndef NK_FONT_SLOTS_H
#define NK_FONT_SLOTS_H

/* Cache directory under the per-user data root: <user data>/fonts/v2 */
#define NK_FONT_CACHE_PARENT "fonts"
#define NK_FONT_CACHE_SUBDIR "v2"
#define NK_FONT_CACHE_SCHEMA_VERSION 2
#define NK_FONT_MANIFEST_NAME "manifest.json"
/* Every manifest entry records its source label; this flow writes only user entries. */
#define NK_FONT_MANIFEST_SOURCE_USER "user"
/* Version of the reader rules a manifest entry was checked under (the reader's verdict). */
#define NK_FONT_READER_VERSION 1
/* Project fonts sit where the runtime already resolves its font root: <asset root>/font */
#define NK_FONT_PROJECT_SUBDIR "font"
/* The reader's ceiling on an image (PGF_SPEC.md 3.1). A larger file is refused by name. */
#define NK_FONT_PGF_MAX_BYTES 16777216 /* 16 MiB */

typedef enum NkFontSlot {
    NK_FONT_SLOT_JAPANESE = 0,
    NK_FONT_SLOT_LATIN = 1,
    NK_FONT_SLOT_KOREAN = 2,
    NK_FONT_SLOT_COUNT = 3
} NkFontSlot;

#define NK_FONT_SLOT_JAPANESE_FILE "nkjpn.pgf"
#define NK_FONT_SLOT_LATIN_FILE "nkltn.pgf"
#define NK_FONT_SLOT_KOREAN_FILE "nkkr.pgf"
/* The project's Latin font comes in the console's two nominal sizes: NK_FONT_SLOT_LATIN_FILE for the
 * first size group and this file for the second. A project folder without it serves the first. */
#define NK_FONT_PROJECT_LATIN_SMALL_FILE "nkltns.pgf"
/* Each of the other fourteen Latin indices has a project face of its own style, named for its
 * ordinal: nkltn1.pgf to nkltn7.pgf and nkltn9.pgf to nkltn15.pgf (nk_font_project_latin_index_file).
 * The regular sans-serif faces of the two sizes keep the two names above. */

/* Firmware exposes sixteen separately named Latin PGFs. The first and second groups each
 * contain the same eight family/style combinations at a different nominal size. */
#define NK_FONT_LATIN_INDEX_COUNT 16
#define NK_FONT_LATIN_SIZE_GROUP_COUNT 8

/* Slot classification, from the reader's coverage probes (pgf_api.h, PgfVerdict): a file
 * that covers Hangul names the Korean slot, one that covers kana names the Japanese slot,
 * and one that covers Latin letters alone names the Latin slot. A file that covers both
 * Hangul and kana names no slot. The probe codes are U+AC00 or U+D55C, U+3042 or U+30A2,
 * and U+0041 with U+0061. */

#endif /* NK_FONT_SLOTS_H */
