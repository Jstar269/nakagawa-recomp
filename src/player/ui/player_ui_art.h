/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef PLAYER_UI_ART_H
#define PLAYER_UI_ART_H

#include "nk_ui_image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Disc art for the next UI: each library title's ICON0.PNG, and PIC1.PNG for
   the title being viewed, read from the user's own disc image and decoded on a
   background thread, one disc at a time. The images live in memory only: they
   are never written to disk, cached in a file, or sent anywhere. A disc
   without an image, an unreadable disc, or a PNG over the limits below simply
   has no image, and the card keeps its placeholder; nothing is retried in the
   same session. */

/* The PSP's own sizes are 144 x 80 (ICON0) and 480 x 272 (PIC1); anything far
   larger is refused before it is decoded. The encoded size is already bounded
   by the ISO reader (NK_ICON_MAX_BYTES). */
#define PLAYER_UI_ART_ICON_MAX_SIDE 512u
#define PLAYER_UI_ART_PICTURE_MAX_SIDE 1024u
#define PLAYER_UI_ART_MAX_BYTES (1024u * 1024u)
/* Discs whose art is kept; the least recently shown goes first. */
#define PLAYER_UI_ART_SLOTS 64u

enum {
    PLAYER_UI_ART_ICON = 1u << 0,
    PLAYER_UI_ART_PICTURE = 1u << 1
};

/* Reads the wanted images' PNG bytes from a disc image (bit set in `wanted`),
   returning malloc'd buffers the caller frees; a missing image is NULL / 0.
   The player uses the ISO reader; tests pass their own. Runs on the worker. */
typedef void (*PlayerUiArtReader)(const char *iso_path, unsigned wanted,
                                  uint8_t **icon, size_t *icon_size,
                                  uint8_t **picture, size_t *picture_size);

typedef struct PlayerUiArt PlayerUiArt;

/* NULL reader: the ISO reader (nk_iso_read_game_art). Returns NULL when out of
   memory. */
PlayerUiArt *player_ui_art_create(PlayerUiArtReader reader);
/* Waits for a running read, then frees every image. */
void player_ui_art_destroy(PlayerUiArt *art);

/* Once per frame, before the frame's player_ui_art_get calls: takes a
   finished read's images, then starts the next wanted read. Images returned
   by player_ui_art_get stay valid until the next pump. */
void player_ui_art_pump(PlayerUiArt *art);

/* The images for a disc image path, asking for them on first sight: always
   the icon, and the picture too when want_picture. Either out pointer gets
   NULL until its image is decoded, and for good when there is none. */
void player_ui_art_get(PlayerUiArt *art, const char *iso_path, bool want_picture,
                       const NkUiImage **icon, const NkUiImage **picture);

/* True while a read runs or one is waiting to start (tests wait on it). */
bool player_ui_art_busy(const PlayerUiArt *art);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_UI_ART_H */
