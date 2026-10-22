/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_BACKEND_SDLR_H
#define NK_UI_BACKEND_SDLR_H

#include "nk_ui_draw.h"

/* This header names no SDL type, so SDL-free code can include it. The SDL
   renderer is forwarded as `struct SDL_Renderer`, which is the tag SDL's own
   `SDL_Renderer` typedef uses.

   Both halves are compiled in only when the build defines NK_UI_WITH_SDL
   (renderer backend) and NK_UI_WITH_SDL_TTF (text engine). Without them each
   function is a stub: *_available() returns false, constructors return NULL and
   nk_ui_sdlr_render() returns false. That keeps `nk-ui-core-selftest` and the
   hosted Linux native-core-tests free of SDL. */

typedef struct NkUiSdlr NkUiSdlr;
typedef struct NkUiTtf NkUiTtf;
struct SDL_Renderer;

/* ---- SDL_Renderer backend (src/ui/backend/nk_ui_sdlr.c) ---- */

/* True when this build includes the SDL renderer backend. */
bool nk_ui_sdlr_available(void);

/* Creates a backend that draws into `renderer`, which the caller keeps owning and
   must keep alive until nk_ui_sdlr_destroy. `text` supplies shape and raster for
   NK_UI_DRAW_TEXT commands (nk_ui_ttf_backend() or any NkUiTextBackend); its
   `user` pointer must outlive the backend. A NULL `text`, or one without shape
   and raster, draws no text. Returns NULL on failure. The renderer must target
   real device pixels: draw-list coordinates are device pixels already. */
NkUiSdlr *nk_ui_sdlr_create(struct SDL_Renderer *renderer, const NkUiTextBackend *text);
void nk_ui_sdlr_destroy(NkUiSdlr *renderer);

/* Tells the backend the device scale (NkUiScale.density) the next lists use. A
   change drops every atlas page, because glyphs are rasterised per pixel size;
   pages are rebuilt lazily by the next render. Calling it with an unchanged
   scale does nothing. The host also calls it when the text-size preference
   changes, since that is part of the density. */
void nk_ui_sdlr_set_scale(NkUiSdlr *renderer, float density);

/* Draws one frame's list from nk_ui_end(). Returns true when the list was
   drawn, and false for a NULL argument or a malformed list (nothing is then
   guaranteed about partial output). Clip, blend and draw-colour state of the
   renderer are restored before returning.
   Custom commands the backend cannot draw are skipped and counted in
   NkUiSdlrStats.custom_skipped; today that is every NK_UI_CUSTOM_WAVES
   command, because nk_ui_waves_render_rgba() takes a palette and a clock rather
   than the NkUiWavesCommand carried by the list. */
bool nk_ui_sdlr_render(NkUiSdlr *renderer, const NkUiDrawList *list);

typedef struct {
    uint32_t atlas_pages;     /* pages alive now */
    uint32_t atlas_glyphs;    /* glyph entries cached now, including empty ones */
    uint32_t atlas_rebuilds;  /* times a populated atlas was dropped */
    uint32_t atlas_uploads;   /* glyph bitmaps copied into a page, ever */
    uint32_t glyphs_drawn;    /* glyph quads submitted, ever */
    uint32_t text_runs_drawn; /* text commands drawn, ever */
    uint32_t custom_skipped;  /* custom commands skipped, ever */
} NkUiSdlrStats;

void nk_ui_sdlr_get_stats(const NkUiSdlr *renderer, NkUiSdlrStats *out);

/* ---- SDL3_ttf text backend (src/ui/nk_ui_ttf.c) ---- */

/* True when this build includes the SDL3_ttf headers. The shared library itself
   is loaded at run time by nk_ui_ttf_create, as the player does today, so a
   missing library is a create failure, not a link failure. */
bool nk_ui_ttf_available(void);

/* Loads SDL3_ttf and starts a text engine. `library_paths` lists candidate
   shared-library paths tried in order, then the platform default name. NULL and
   0 try only the default. Returns NULL on failure; nk_ui_ttf_last_error() then
   names the reason (build without SDL3_ttf, library not found, symbol missing,
   TTF_Init failed). */
NkUiTtf *nk_ui_ttf_create(const char *const *library_paths, size_t count);
void nk_ui_ttf_destroy(NkUiTtf *ttf);
const char *nk_ui_ttf_last_error(void);

/* Registers font `font` (0 to 7, the id nk_ui widgets pass) from a TrueType or
   OpenType file, read once into memory. Re-registering an id is refused. */
bool nk_ui_ttf_add_font(NkUiTtf *ttf, int font, const char *path);

/* Appends `fallback` to the chain `font` consults, per glyph, when its own face
   lacks one (at most three). The fallback must be registered. */
bool nk_ui_ttf_set_fallback(NkUiTtf *ttf, int font, int fallback);

/* The text backend: measure and shape use SDL3_ttf with HarfBuzz shaping and
   kerning; raster returns an 8-bit coverage bitmap. Conventions (also in
   nk_ui_ttf.c):
   - px is the em size in device pixels, rasterised with light hinting.
   - NkUiGlyph.glyph is (fallback depth << 24) | glyph index, so a glyph found in a
     fallback face stays addressable; depth 0 is the font itself.
   - NkUiGlyph x and y are the bottom-left corner of the glyph's bitmap, whole
     pixels, from the text origin: x to the right, y down from the baseline.
     NkUiGlyphBitmap.bearing_x is 0 and bearing_y is the bitmap height, so
     "pen + (bearing_x, -bearing_y)" is the bitmap's top-left in either reading.
   - advance_x is the distance to the next glyph's x; the last glyph's runs to
     the end of the string.
   - measure() height is the face's line height and baseline its ascent.
   The returned struct points at `ttf`, which must outlive it. */
NkUiTextBackend nk_ui_ttf_backend(NkUiTtf *ttf);

#endif /* NK_UI_BACKEND_SDLR_H */
