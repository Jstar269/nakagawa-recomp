/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_DRAW_H
#define NK_UI_DRAW_H

#include "nk_ui.h"

typedef enum {
    NK_UI_DRAW_RECTANGLE = 1,
    NK_UI_DRAW_BORDER,
    NK_UI_DRAW_TEXT,
    NK_UI_DRAW_SCISSOR_START,
    NK_UI_DRAW_SCISSOR_END,
    NK_UI_DRAW_CUSTOM
} NkUiDrawKind;

enum {
    NK_UI_CUSTOM_NONE = 0,
    NK_UI_CUSTOM_WAVES = 1
};

typedef struct {
    float x;
    float y;
    float u;
    float v;
    uint32_t rgba_pm;
} NkUiVertex;

typedef struct {
    int kind;
    uint32_t texture;
    int clip[4];
    uint32_t first;
    uint32_t count;
    uint32_t custom_kind;
    const void *custom;
    uint32_t element_id;
    int bounds[4];
    uint32_t rgba_pm;
} NkUiDrawCmd;

struct NkUiDrawList {
    const NkUiVertex *v;
    uint32_t nv;
    const uint32_t *idx;
    uint32_t ni;
    const NkUiDrawCmd *cmd;
    uint32_t ncmd;
    const void *atlas;
};

typedef struct {
    NkUiRect bounds;
    const char *text;
    size_t length;
    int font;
    int size_px;
    float letter_spacing; /* extra device px after each glyph (CSS letter-spacing) */
    bool center;          /* centre the run across its bounds (glyph labels) */
} NkUiTextRun;

typedef struct {
    NkUiDrawList list;
    NkUiVertex *vertices;
    uint32_t vertex_capacity;
    uint32_t *indices;
    uint32_t index_capacity;
    NkUiDrawCmd *commands;
    uint32_t command_capacity;
    NkUiTextRun **text_runs;
    uint32_t text_run_count;
    uint32_t text_run_capacity;
} NkUiDrawBuffer;

/* Paints one Clay custom element into the draw list at its laid-out rect
   (logical px). data is the element's custom payload. */
typedef bool (*NkUiClayPaint)(void *user, void *data, NkUiRect rect, float density,
                              uint32_t element_id);

void nk_ui_draw_buffer_reset(NkUiDrawBuffer *buffer);
void nk_ui_draw_buffer_destroy(NkUiDrawBuffer *buffer);
bool nk_ui_draw_add_rect(NkUiDrawBuffer *buffer, NkUiRect rect, float density,
                         float radius, uint32_t rgba, uint32_t element_id);
bool nk_ui_draw_add_border(NkUiDrawBuffer *buffer, NkUiRect rect, float density,
                           float width, uint32_t rgba, uint32_t element_id);
bool nk_ui_draw_add_text(NkUiDrawBuffer *buffer, NkUiPixelRect bounds,
                         const char *text, size_t length, int font,
                         int size_px, uint32_t rgba, uint32_t element_id);
/* Per-corner variants. radius[] is TL, TR, BR, BL in logical px; a rounded
   border draws straight strips plus one quarter ring per rounded corner. */
bool nk_ui_draw_add_rect_corners(NkUiDrawBuffer *buffer, NkUiRect rect, float density,
                                 const float radius[4], uint32_t rgba,
                                 uint32_t element_id);
bool nk_ui_draw_add_border_corners(NkUiDrawBuffer *buffer, NkUiRect rect, float density,
                                   float width, const float radius[4], uint32_t rgba,
                                   uint32_t element_id);
/* A filled convex polygon, xy in device pixels (x0, y0, x1, y1, ...), 3 to 256
   points in either winding, with a one-pixel transparent edge fringe. */
bool nk_ui_draw_add_convex(NkUiDrawBuffer *buffer, const float *xy, uint32_t count,
                           uint32_t rgba, uint32_t element_id);
bool nk_ui_draw_add_text_spaced(NkUiDrawBuffer *buffer, NkUiPixelRect bounds,
                                const char *text, size_t length, int font,
                                int size_px, float letter_spacing_px, uint32_t rgba,
                                uint32_t element_id);
/* As nk_ui_draw_add_text_spaced, with the run centred across its bounds. */
bool nk_ui_draw_add_text_centered(NkUiDrawBuffer *buffer, NkUiPixelRect bounds,
                                  const char *text, size_t length, int font,
                                  int size_px, uint32_t rgba, uint32_t element_id);
bool nk_ui_draw_add_custom(NkUiDrawBuffer *buffer, uint32_t custom_kind,
                           const void *custom, NkUiPixelRect bounds,
                           uint32_t element_id);
bool nk_ui_draw_add_scissor(NkUiDrawBuffer *buffer, bool start,
                            NkUiPixelRect bounds, uint32_t element_id);

#endif /* NK_UI_DRAW_H */
