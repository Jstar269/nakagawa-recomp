/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_OVERLAY_HOST_H
#define NK_OVERLAY_HOST_H

/* The runtime's SDL side of the in-game overlay (#1159, C9c): the text engine and
   UI fonts, the nk_ui context, the SDL_Renderer backend and the input translation
   the pages need. The pause presenter (src/rt/gui.c) owns the window, the frozen
   frame and the renderer it draws into; this draws the pages over that image.

   SDL3_ttf is loaded at run time, as the player loads it. When it, or the bundled
   UI fonts, cannot be found, nk_overlay_host_create returns NULL with the reason,
   and the presenter shows the frozen frame without the pages (C9b's overlay). */

#include "nk_overlay.h"

#include <stdbool.h>
#include <stddef.h>

union SDL_Event;
struct SDL_Renderer;

typedef struct NkOverlayHost NkOverlayHost;

/* ui_root is the player's folder (SR_UI_ROOT, which holds assets/fonts/ui and may hold
   the SDL3_ttf library); NULL or "" tries the runtime's own folder and the working
   directory only. */
NkOverlayHost *nk_overlay_host_create(const char *ui_root, char *error, size_t error_size);
void nk_overlay_host_destroy(NkOverlayHost *host);

/* A pause opens the menu, or the controller dialog for NK_OVERLAY_OPEN_PAD_LOST;
   anything held from the game is forgotten. */
void nk_overlay_host_open(NkOverlayHost *host, NkOverlayOpenReason reason);
void nk_overlay_host_close(NkOverlayHost *host);
bool nk_overlay_host_is_open(const NkOverlayHost *host);

/* One input or window event (the presenter's input claim). Returns true when the
   image must be drawn again. The pad's Guide button is the presenter's, never the
   pages': it is not passed here. */
bool nk_overlay_host_event(NkOverlayHost *host, const union SDL_Event *event);
/* True when time alone needs a new frame: a held direction repeating, or the frame
   after a page change, which navigates by the page's own layout. */
bool nk_overlay_host_due(const NkOverlayHost *host, double now_s);

/* Draws one frame of the pages into `renderer`, whose output is the window's pixel
   size, at `display_scale`; `pixels_per_point` maps pointer events. The model's
   logical size is filled in here. Returns false when the frame could not be drawn;
   *action is the person's choice this frame. */
bool nk_overlay_host_draw(NkOverlayHost *host, struct SDL_Renderer *renderer,
                          float display_scale, float pixels_per_point, NkOverlayModel *model,
                          NkOverlayAction *action);
/* Lets go of the renderer the last frame drew into, before it is destroyed. */
void nk_overlay_host_unbind(NkOverlayHost *host);

/* The controller family the controls page names buttons for, and whether a pad is
   connected now. */
NkUiPadFamily nk_overlay_host_pad_family(const NkOverlayHost *host);
bool nk_overlay_host_pad_connected(const NkOverlayHost *host);

/* The page that is up (NK_OVERLAY_SCREEN_MENU while closed) and the VRAM viewer's window,
   which the presenter decodes when a frame returns NK_OVERLAY_ACT_VRAM_VIEW (#314). NULL
   for a NULL host. */
NkOverlayScreen nk_overlay_host_screen(const NkOverlayHost *host);
const NkOverlayVramView *nk_overlay_host_vram_view(const NkOverlayHost *host);

#endif /* NK_OVERLAY_HOST_H */
