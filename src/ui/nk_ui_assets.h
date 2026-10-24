/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_ASSETS_H
#define NK_UI_ASSETS_H

#include <stddef.h>

/* Files embedded in the binary by tools/gen_ui_assets.py (nk_ui_assets.c):
   Kenney's CC0 prompt glyphs ("input-prompts/<file>.png") and interface sounds
   ("interface-sounds/<file>.wav"), from third_party/kenney/. The bytes are the
   vendored files exactly; nothing is decoded until a caller asks. */
typedef struct {
    const char *name;
    const unsigned char *bytes;
    size_t size;
} NkUiEmbeddedAsset;

extern const NkUiEmbeddedAsset nk_ui_embedded_assets[];
extern const size_t nk_ui_embedded_asset_count;

/* The asset with exactly this name, or NULL. */
const NkUiEmbeddedAsset *nk_ui_embedded_asset(const char *name);

#endif /* NK_UI_ASSETS_H */
