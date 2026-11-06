/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Homebrew in an EBOOT.PBP (issue #2143): the player turns a package whose
 * DATA.PSP is a plain, unencrypted user-mode PSP program into a small private
 * disc image and imports that exactly as a picked disc. The image holds
 * PSP_GAME/PARAM.SFO (the package's title and a placeholder DISC_ID),
 * PSP_GAME/SYSDIR/EBOOT.BIN (the DATA.PSP bytes) and, when the package has
 * one, PSP_GAME/ICON0.PNG. The placeholder is not a retail disc ID, so the
 * existing rule that keys a non-retail disc by its contents names the game:
 * the same program with the same title is the same game on every machine
 * (nk_non_retail_disc_key; tools/nk_core/iso_inspect.py homebrew_library_key
 * is the Python half).
 *
 * Refused by name, never converted: an encrypted DATA.PSP, a DATA.PSP that is
 * not a PSP program, kernel-mode homebrew and custom-firmware plugins, and a
 * program larger than the disc route reads. No package section other than
 * these is executed or extracted.
 */

#ifndef NK_PBP_HOMEBREW_H
#define NK_PBP_HOMEBREW_H

#include <stdbool.h>
#include <stdint.h>

#include "nk_psp_npumdimg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NkPbpHomebrewKind {
    NK_PBP_HB_PROGRAM = 0,   /* a plain user-mode program this route can import */
    NK_PBP_HB_ENCRYPTED,     /* DATA.PSP is a ~PSP container */
    NK_PBP_HB_NOT_A_PROGRAM, /* DATA.PSP is empty, or not an ELF32 MIPS PSP program */
    NK_PBP_HB_KERNEL_MODE,   /* the module declares kernel mode (attribute 0x1000) */
    NK_PBP_HB_TOO_LARGE      /* DATA.PSP is larger than the disc route reads */
} NkPbpHomebrewKind;

typedef struct NkPbpHomebrew {
    NkPbpHomebrewKind kind;
    /* The library key the converted disc will have: X, three letters, five digits. */
    char key[10];
    char title[128];
    uint32_t program_size;
    uint32_t icon_size;
} NkPbpHomebrew;

/* The largest DATA.PSP imported (the disc route's executable bound). */
#define NK_PBP_HOMEBREW_MAX_PROGRAM_BYTES (512u * 1024u * 1024u)

/* Classify a package nk_pbp_identify_file stopped at with
 * NK_PBP_PAYLOAD_OTHER (info is what it filled in). NK_PSP_OK and kind
 * NK_PBP_HB_PROGRAM: the package is importable, and out->key is the key its
 * converted disc will have. Otherwise NK_PSP_ERR_UNSUPPORTED with out->kind
 * naming the refusal, or NK_PSP_ERR_INPUT / NK_PSP_ERR_FORMAT when the file
 * cannot be read as a package. Reads the package; writes nothing. */
int nk_pbp_homebrew_inspect_file(NkPspCtx *ctx, const char *pbp_path,
                                 const NkPbpInfo *info, NkPbpHomebrew *out);

/* Write the private disc image for an importable package to iso_path: to
 * "<iso_path>.converting" first, checked by reading it back as a disc
 * (nk_iso_inspect must name out->key), then renamed into place. A failure or
 * a cancel removes the partial file. callbacks may be NULL. */
int nk_pbp_homebrew_convert_file(NkPspCtx *ctx, const char *pbp_path,
                                 const NkPbpInfo *info, const char *iso_path,
                                 const NkNpumdimgCallbacks *callbacks, NkPbpHomebrew *out);

#ifdef __cplusplus
}
#endif

#endif /* NK_PBP_HOMEBREW_H */
