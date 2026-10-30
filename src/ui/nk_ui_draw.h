/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_DRAW_H
#define NK_UI_DRAW_H

#include "nk_ui.h"
#include "nk_ui_image.h"

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
    NK_UI_CUSTOM_WAVES = 1,
    NK_UI_CUSTOM_IMAGE = 2    /* custom: const NkUiImage *, rgba_pm: the tint */
};

typedef struct {
    float x;
    float y;
    float u;
    float v;
    uint32_t rgba_pm;
} NkUiVertex;

/* Scissors nested deeper than this make the frame fail. */
#define NK_UI_DRAW_CLIP_DEPTH 32u

typedef struct {
    int kind;
    uint32_t texture;
    /* Where the command may draw, in device px (x0, y0, x1, y1): its bounds
       inside every scissor open around it, empty (x1 <= x0 or y1 <= y0) when
       they hide it all. A SCISSOR_START's clip is its scissor inside the ones
       around it; a SCISSOR_END's is the scissor it closes. A backend clips
       each command to it, so every backend clips alike. */
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
    int clip_stack[NK_UI_DRAW_CLIP_DEPTH][4]; /* the scissors open now, nested */
    uint32_t clip_depth;
} NkUiDrawBuffer;

/* A floating container (a Clay floating element): laid out apart from the
   container it is declared in and drawn over it, sized like that container
   and attached to it. */
typedef struct {
    float expand;         /* grows its box this far on every side (logical px) */
    int16_t z;            /* drawn over everything with a lower z */
    bool centered;        /* centred on its container; else at its top left */
    bool passthrough;     /* the pointer reaches what is under it; else it takes it */
    bool clip_to_parent;  /* clipped like its container (inside a scroll container) */
    NkUiId draw_id;       /* the element id its draw commands carry */
} NkUiFloating;

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
/* An image stretched over bounds (device px), every texel multiplied by the
   tint (straight RGBA; 0xFFFFFFFF draws the image as it is). The image must
   outlive the list; backends key their texture by NkUiImage.serial. */
bool nk_ui_draw_add_image(NkUiDrawBuffer *buffer, const NkUiImage *image,
                          NkUiPixelRect bounds, uint32_t tint_rgba,
                          uint32_t element_id);
bool nk_ui_draw_add_scissor(NkUiDrawBuffer *buffer, bool start,
                            NkUiPixelRect bounds, uint32_t element_id);

#endif /* NK_UI_DRAW_H */
