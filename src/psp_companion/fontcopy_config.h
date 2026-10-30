// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* What Nakagawa Font Copy (src/psp_companion/fontcopy.c) copies and where it writes. These
 * choices live in a header so the host harness (fixtures/psp_oracle/io_cases_host.c, mode
 * fontcopy) runs exactly what the app runs; tools/test_psp_oracle.py checks them.
 *
 *  - Source: flash0:/font, listed and opened read-only through font_files.h. Only names ending
 *    in .pgf are copied: the console's system fonts in the PlayStation Glyph Format, plain
 *    unencrypted files that the player imports (docs/SETUP.md, "System fonts"). Nothing is
 *    decrypted, and no other flash0 folder is touched.
 *  - If the console refuses to list flash0:/font to the app, the eighteen firmware font names
 *    below (the files the player's Japanese, Korean and sixteen Latin font slots stand for) are
 *    opened read-only one by one instead.
 *  - Destination: the app's own folder, NAKAGAWA/fonts, on the storage the app was started
 *    from: ef0: (the PSP Go's internal storage) when argv[0] names it, otherwise ms0: (the
 *    Memory Stick). Only that folder and its NAKAGAWA parent are created; nothing else is
 *    written.
 *
 * The includer includes font_files.h first. */

#ifndef NAKAGAWA_FONTCOPY_CONFIG_H
#define NAKAGAWA_FONTCOPY_CONFIG_H

#include <stdio.h>

#define FONTCOPY_PARENT "/NAKAGAWA"
#define FONTCOPY_FOLDER "/NAKAGAWA/fonts"
#define FONTCOPY_KNOWN_COUNT 18u

static const char *const s_fontcopy_known[FONTCOPY_KNOWN_COUNT] = {
    "jpn0.pgf", "kr0.pgf",
    "ltn0.pgf", "ltn1.pgf", "ltn2.pgf", "ltn3.pgf", "ltn4.pgf", "ltn5.pgf",
    "ltn6.pgf", "ltn7.pgf", "ltn8.pgf", "ltn9.pgf", "ltn10.pgf", "ltn11.pgf",
    "ltn12.pgf", "ltn13.pgf", "ltn14.pgf", "ltn15.pgf",
};

typedef struct FontCopyTarget {
    char device[8];   /* "ms0:" or "ef0:" */
    char parent[24];  /* <device>/NAKAGAWA */
    char folder[24];  /* <device>/NAKAGAWA/fonts */
} FontCopyTarget;

/* The storage the app started from: ef0: when argv[0] begins with it (in any case), else ms0:. */
static void fontcopy_target(FontCopyTarget *target, const char *argv0) {
    const char *device = "ms0:";
    if (argv0 != NULL && (argv0[0] | 0x20) == 'e' && (argv0[1] | 0x20) == 'f' &&
        argv0[2] == '0' && argv0[3] == ':') {
        device = "ef0:";
    }
    (void)snprintf(target->device, sizeof(target->device), "%s", device);
    (void)snprintf(target->parent, sizeof(target->parent), "%s" FONTCOPY_PARENT, device);
    (void)snprintf(target->folder, sizeof(target->folder), "%s" FONTCOPY_FOLDER, device);
}

static void fontcopy_configure(FontFiles *files, FontCopyTarget *target, const char *argv0) {
    fontcopy_target(target, argv0);
    files->dest_parent = target->parent;
    files->dest_dir = target->folder;
    files->pgf_only = 1u;
    files->known_names = s_fontcopy_known;
    files->known_count = FONTCOPY_KNOWN_COUNT;
}

#endif /* NAKAGAWA_FONTCOPY_CONFIG_H */
