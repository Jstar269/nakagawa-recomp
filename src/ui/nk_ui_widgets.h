/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_WIDGETS_H
#define NK_UI_WIDGETS_H

/* The component set of the player's design system (docs/PLAYER_UI_DESIGN.md):
   buttons, game cards, status pills, badges, section headers, the top bar and
   the prompt bar, plus the vector-art element the brand mark and placeholder art
   are drawn with. Every component takes its colours and sizes from the theme
   tokens in nk_ui_theme.h; screens compose them and never pick raw colours. */

#include "nk_ui.h"
#include "nk_ui_input.h"
#include "nk_ui_theme.h"

/* ---- Core seam ------------------------------------------------------------ */
/* Focusable controls register their previous-frame Clay bounds so nk_ui can
   move focus before the next screen is built. */
void nk_ui_register_button(NkUi *ui, NkUiId id, uint32_t flags);
bool nk_ui_button_focused(const NkUi *ui, NkUiId id);
/* True once, on the frame an ACTIVATE press reaches the focused control `id`. */
bool nk_ui_take_activation(NkUi *ui, NkUiId id);
/* Label colour of a button with these flags. */
uint32_t nk_ui_button_text_rgba(const NkUi *ui, uint32_t flags);

/* A stable child id for the k-th part of component `id`. */
NkUiId nk_ui_child_id(NkUiId id, uint32_t k);

/* ---- Vector art ------------------------------------------------------------ */
#define NK_UI_SHAPE_MAX_POINTS 8u

typedef enum {
    NK_UI_SHAPE_RECT = 1,   /* v = x, y, w, h; radius rounds it */
    NK_UI_SHAPE_CIRCLE,     /* v = cx, cy, r */
    NK_UI_SHAPE_POLY        /* v = count convex points, x then y */
} NkUiShapeKind;

typedef struct {
    NkUiShapeKind kind;
    uint32_t rgba;
    float radius;
    unsigned count;
    float v[NK_UI_SHAPE_MAX_POINTS * 2u];
} NkUiShape;

/* Shapes in a view box, drawn "contain"-fitted and centred in the element. The
   shapes array must outlive the frame (static art does). */
typedef struct {
    float view_x;
    float view_y;
    float view_w;
    float view_h;
    const NkUiShape *shapes;
    size_t count;
} NkUiShapeArt;

bool nk_ui_shapes(NkUi *ui, NkUiId id, NkUiSizing width, NkUiSizing height,
                  const NkUiShapeArt *art);

/* Decoded images in one element: `cover` fills it (cropped to the element),
   then `contain` is fitted inside contain_scale (0..1; 0 means 1) of it,
   centred. Either may be NULL. The images must outlive the frame's list. */
typedef struct {
    const NkUiImage *cover;
    const NkUiImage *contain;
    float contain_scale;
} NkUiImageArt;

bool nk_ui_image_art(NkUi *ui, NkUiId id, NkUiSizing width, NkUiSizing height,
                     const NkUiImageArt *art);

/* Project-drawn art. The N1 mark is the brand's Block & Branch; the showcase
   pictures belong to the project's own demo programs. No game or console art. */
typedef enum {
    NK_UI_ART_NONE = 0,          /* a placeholder box with a label */
    NK_UI_ART_SHOWCASE_SCENE,    /* the lit cube of the 3D showcase demo */
    NK_UI_ART_SHOWCASE_BREAKOUT  /* bricks, ball and paddle of the Breakout demo */
} NkUiArtKind;

const NkUiShapeArt *nk_ui_art_n1_mark(void);
const NkUiShapeArt *nk_ui_art_picture(NkUiArtKind kind);
/* A dark tint for a title's art panel, picked from the title's id so every
   card keeps its colour between runs. */
uint32_t nk_ui_art_tint(const char *key);

/* The app icon: the N1 mark on a rounded ink tile, `size` px square (16-256),
   written as straight-alpha RGBA bytes, `stride` bytes per row, top row first.
   tools/make_app_icon.py draws the Windows .ico with the same rules. Returns
   false, writing nothing, for a size out of range or a short stride. */
#define NK_UI_APP_ICON_MIN 16
#define NK_UI_APP_ICON_MAX 256
bool nk_ui_app_icon_rgba(int size, unsigned char *rgba, size_t stride);

/* ---- Prompt glyphs ---------------------------------------------------------- */
/* One prompt glyph in the current prompt style, `height` px tall (24 in bars). */
bool nk_ui_prompt_glyph(NkUi *ui, NkUiId id, NkUiPromptAction action, float height);
/* One glyph in `style`, whatever the current one, for a binding shown in its own
   device's set (#1299). A non-NULL `label` is drawn as given in place of the action's
   own text (a key's legend on a keycap, a pad button's name on a pill) and keeps the
   project-drawn form; without one the style's image is used when the host set one. */
bool nk_ui_prompt_glyph_in(NkUi *ui, NkUiId id, NkUiPromptStyle style, NkUiPromptAction action,
                           const char *label, float height);

/* ---- Text helpers ------------------------------------------------------------ */
/* ASCII letters upper-cased into frame storage (labels and chips are caps). */
const char *nk_ui_caps(NkUi *ui, const char *text);
void nk_ui_text_role(NkUi *ui, NkUiId id, const char *text, NkUiTypeRole role,
                     uint32_t rgba);
/* An eyebrow: the small tracked caps line that names a section. */
void nk_ui_eyebrow(NkUi *ui, NkUiId id, const char *text);

/* ---- Containers --------------------------------------------------------------- */
/* A card: raised surface, hairline edge, tile radius. Returns true when opened;
   close it with nk_ui_end_container. */
bool nk_ui_card_begin(NkUi *ui, NkUiId id, NkUiSizing width, NkUiSizing height,
                      float gap);
/* A 1 px hairline across (horizontal) or down the parent. */
void nk_ui_divider(NkUi *ui, NkUiId id, bool horizontal);

/* ---- Chips ---------------------------------------------------------------------- */
/* Status pill: marker and word in the status colour on a raised chip. The word is
   shown in caps. marker may be NULL. */
void nk_ui_status_pill(NkUi *ui, NkUiId id, const char *marker, const char *word,
                       uint32_t rgba);
/* Plain tag chip (muted text), e.g. YOUR DISC. */
void nk_ui_tag(NkUi *ui, NkUiId id, const char *text);
/* Badge over title art: micro caps on a translucent scrim. */
void nk_ui_badge(NkUi *ui, NkUiId id, const char *text);

/* ---- Bars ------------------------------------------------------------------------- */
typedef struct {
    const char *label;
    bool active;
    NkUiId id;      /* nonzero: the tab is focusable and can be activated */
} NkUiSection;

/* Top bar: brand mark and wordmark, section tabs with an accent underline on the
   active one, and the input chip on the right. Returns the index of the section
   the user chose this frame (a focused tab activated, or L / R from the active
   one), or -1. */
int nk_ui_top_bar(NkUi *ui, NkUiId id, const NkUiSection *sections, size_t count,
                  const char *input_label, bool input_active);

typedef struct {
    NkUiPromptAction action;
    const char *label;
} NkUiPromptItem;

/* Prompt bar: a one-line hint on the left and glyph + label prompts on the right. */
void nk_ui_prompt_bar(NkUi *ui, NkUiId id, const char *hint,
                      const NkUiPromptItem *items, size_t count);

/* The input chip text for the current prompt style. */
const char *nk_ui_input_label(NkUiPromptStyle style);

/* ---- Dialogs and progress ---------------------------------------------------------- */
/* A dialog card: raised surface, strong edge, dialog radius, 26/28/22 px
   padding, 14 px between blocks. Close it with nk_ui_end_container. */
bool nk_ui_dialog_begin(NkUi *ui, NkUiId id, float width);
/* A 6 px progress track `width` px wide: fraction 0..1 fills it in accent; a
   negative fraction is indeterminate, a sweeping segment driven by phase_s
   (pass 0 under reduced motion to hold it still). */
void nk_ui_progress_bar(NkUi *ui, NkUiId id, float width, float fraction, double phase_s);
/* Where the indeterminate segment starts, 0..travel px: it goes out and back
   every 2.4 s, slowing at both ends. A phase of 0 holds it at the start. */
float nk_ui_progress_sweep_offset(float travel, double phase_s);

/* ---- Game card ------------------------------------------------------------------- */
typedef struct {
    const char *title;
    const char *disc_id;
    const char *status_marker;
    const char *status_word;
    uint32_t status_rgba;
    const char *badge;        /* NULL: YOUR DISC */
    NkUiArtKind art;
    const char *art_key;      /* picks the art tint; NULL uses disc_id */
    const NkUiImage *icon;    /* the disc's ICON0.PNG, when read; NULL keeps the placeholder */
    float width;
    bool muted_title;         /* an unsupported entry */
} NkUiTileSpec;

/* A focusable game card: art panel with badge, two-line title, status and disc
   id. Returns true on the frame it is activated. */
bool nk_ui_tile(NkUi *ui, NkUiId id, const NkUiTileSpec *spec);
/* The dashed-feel "add" card at the end of the grid. */
bool nk_ui_add_tile(NkUi *ui, NkUiId id, const char *title, const char *detail,
                    float width, float height);

/* Title-page hero: the art panel at 480:272 with its caption row. With the
   disc's PIC1.PNG it fills the art area; with only its ICON0.PNG the icon sits
   centred; with neither, the placeholder. */
void nk_ui_hero(NkUi *ui, NkUiId id, NkUiArtKind art, const char *art_key,
                const NkUiImage *icon, const NkUiImage *picture,
                float width, const char *caption, const char *size_note);

#endif /* NK_UI_WIDGETS_H */
