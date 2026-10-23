/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_H
#define NK_UI_H

/* Host-facing C API for Nakagawa's SDL-free UI core. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NkUi NkUi;
typedef struct NkUiTheme NkUiTheme;
typedef struct NkUiDrawList NkUiDrawList;
typedef uint32_t NkUiId;

typedef enum {
    NK_UI_HOST_LAUNCHER = 0,
    NK_UI_HOST_OVERLAY = 1
} NkUiHostKind;

typedef struct {
    float x;
    float y;
    float w;
    float h;
} NkUiRect;

typedef enum {
    NK_UI_SIZE_FIXED,
    NK_UI_SIZE_FIT,
    NK_UI_SIZE_GROW,
    NK_UI_SIZE_PERCENT
} NkUiSizeKind;

typedef struct {
    NkUiSizeKind kind;
    float value;
    float min;
    float max;
} NkUiSizing;

typedef enum {
    NK_UI_LAYOUT_ROW,
    NK_UI_LAYOUT_COLUMN
} NkUiLayoutDirection;

/* Where a container places its children on one axis. Zero is the start (left or
   top); any value other than CENTER or END is read as START. */
typedef enum {
    NK_UI_ALIGN_START = 0,
    NK_UI_ALIGN_CENTER,
    NK_UI_ALIGN_END
} NkUiAlign;

/* Zero-initialise layouts and styles (memset or "= {0}") before setting fields:
   the fields after padding_bottom and corner_radius were added later and default
   to zero (start alignment, no clipping, every corner rounded). */
typedef struct {
    NkUiSizing width;
    NkUiSizing height;
    NkUiLayoutDirection direction;
    float gap;
    float padding_left;
    float padding_top;
    float padding_right;
    float padding_bottom;
    NkUiAlign align_x;
    NkUiAlign align_y;
    bool clip;          /* clip children to this container's bounds */
} NkUiLayout;

/* Corners that stay square even when corner_radius is set, for an art panel
   flush with the top of a rounded card. */
enum {
    NK_UI_CORNER_SQUARE_TL = 1u << 0,
    NK_UI_CORNER_SQUARE_TR = 1u << 1,
    NK_UI_CORNER_SQUARE_BR = 1u << 2,
    NK_UI_CORNER_SQUARE_BL = 1u << 3
};

typedef struct {
    uint32_t fill_rgba;
    uint32_t border_rgba;
    float border_width;
    float corner_radius;
    uint32_t square_corners; /* NK_UI_CORNER_SQUARE_* bits */
} NkUiStyle;

/* One run of text. rgba 0 inherits the container's text colour (paper, or ink on
   an accent fill). letter_spacing is extra logical px after every character (CSS
   letter-spacing); line_height 0 means one font size; wrap false keeps the run on
   one line. */
typedef struct {
    int font;
    int size_px;
    uint32_t rgba;
    float letter_spacing;
    float line_height;
    bool wrap;
} NkUiTextStyle;

typedef struct {
    float width;
    float height;
    float baseline;
} NkUiTextSize;

typedef struct {
    uint32_t glyph;
    uint32_t codepoint;
    float x;
    float y;
    float advance_x;
    float advance_y;
} NkUiGlyph;

typedef struct {
    const uint8_t *pixels;
    int width;
    int height;
    int pitch;
    int bearing_x;
    int bearing_y;
    float advance_x;
    float advance_y;
} NkUiGlyphBitmap;

typedef struct {
    void *user;
    NkUiTextSize (*measure)(void *user, int font, int px, const char *s, size_t n);
    int (*shape)(void *user, int font, int px, const char *s, size_t n,
                 NkUiGlyph *out, int max);
    bool (*raster)(void *user, int font, int px, uint32_t glyph,
                   NkUiGlyphBitmap *out);
} NkUiTextBackend;

typedef struct {
    NkUiHostKind host;
    size_t arena_bytes; /* 0 selects Clay_MinMemorySize(). */
    const NkUiTextBackend *text;
    const NkUiTheme *theme; /* NULL selects the N1 theme, same as nk_ui_theme_n1(). */
} NkUiConfig;

/* The only theme shipped today. The pointer is static, never NULL, and stays
   valid for the life of the process; nk_ui_create copies the colors. */
const NkUiTheme *nk_ui_theme_n1(void);

typedef struct {
    int pixel_w;
    int pixel_h;
    float display_scale;
    int ui_scale_pct;   /* 0 = automatic (follow display_scale); explicit values: 100, 125, 150, 200.
                           100 forces 1x on a high-DPI display. Anything else is treated as 100. */
    int text_size_pct;  /* 90, 100, 125 or 150. Anything else is treated as 100. */
} NkUiViewport;

typedef struct {
    float density;
    float logical_w;
    float logical_h;
} NkUiScale;

typedef struct {
    int x0;
    int y0;
    int x1;
    int y1;
} NkUiPixelRect;

/* Which button-prompt glyph set the prompt bar draws. All four sets are drawn by
   this project and use no console maker's symbols. Zero is the default. */
typedef enum {
    NK_UI_PROMPT_NEUTRAL = 0,       /* neutral positions; PlayStation-family and unknown pads */
    NK_UI_PROMPT_LETTERS_A_SOUTH,   /* letters, with A on the south button */
    NK_UI_PROMPT_LETTERS_A_EAST,    /* letters, with A on the east button */
    NK_UI_PROMPT_KEYCAPS            /* keyboard keycaps */
} NkUiPromptStyle;

/* Held-direction repeat rate. The timings are owned by NkUiRepeatConfig
   (nk_ui_input.h); the preference only picks one. Zero is the default. */
typedef enum {
    NK_UI_REPEAT_NORMAL = 0,
    NK_UI_REPEAT_SLOW,
    NK_UI_REPEAT_FAST
} NkUiRepeatRate;

/* Detected controller family; it picks a default NkUiPromptStyle. */
typedef enum {
    NK_UI_PAD_NEUTRAL = 0,  /* PlayStation-family and unknown pads */
    NK_UI_PAD_LETTERS_A_SOUTH,
    NK_UI_PAD_LETTERS_A_EAST,
    NK_UI_PAD_KEYBOARD
} NkUiPadFamily;

/* Direction of an NK_UI_EV_NAV event. */
typedef enum {
    NK_UI_DIR_NONE = 0,
    NK_UI_DIR_UP,
    NK_UI_DIR_DOWN,
    NK_UI_DIR_LEFT,
    NK_UI_DIR_RIGHT
} NkUiNavDir;

typedef struct {
    bool high_contrast_focus;
    bool reduce_motion;
    NkUiPromptStyle prompt_style;
    NkUiRepeatRate repeat;
} NkUiPrefs;

typedef enum {
    NK_UI_EV_NAV,
    NK_UI_EV_ACTIVATE,
    NK_UI_EV_BACK,
    NK_UI_EV_OPTIONS,
    NK_UI_EV_DETAILS,
    NK_UI_EV_TAB,
    NK_UI_EV_MENU,
    NK_UI_EV_POINTER,
    NK_UI_EV_WHEEL,
    NK_UI_EV_TEXT,
    NK_UI_EV_DEVICE,
    NK_UI_EV_CYCLE  /* Tab / Shift+Tab: dir RIGHT = next, LEFT = previous in declaration order. */
} NkUiEventType;

/* The x and y of NK_UI_EV_POINTER and NK_UI_EV_WHEEL events are LOGICAL UI pixels,
   the same units as layout and NkUiScale.logical_w/h, not physical window pixels.
   A host converts by dividing physical pixels by NkUiScale.density. dir is read
   for NK_UI_EV_NAV; device and pad_family for NK_UI_EV_DEVICE; codepoint for
   NK_UI_EV_TEXT. */
typedef struct {
    NkUiEventType type;
    NkUiNavDir dir;
    float x;
    float y;
    bool down;
    bool repeat;
    int device;
    NkUiPadFamily pad_family;
    uint32_t codepoint;
} NkUiEvent;

enum {
    NK_UI_BUTTON_DISABLED = 1u << 0,
    NK_UI_BUTTON_PRIMARY = 1u << 1,
    NK_UI_BUTTON_DANGER = 1u << 2,
    NK_UI_BUTTON_LARGE = 1u << 3,   /* 52 px, the one main action on a screen */
    NK_UI_BUTTON_SMALL = 1u << 4,   /* 34 px, inline and pager actions */
    NK_UI_BUTTON_GROW = 1u << 5     /* fill the row instead of fitting the label */
};

/* Device scale and edge rounding are shared helpers for hosts and tests. */
NkUiScale nk_ui_scale_for_viewport(const NkUiViewport *viewport);
NkUiPixelRect nk_ui_round_rect(NkUiRect rect, float density);

NkUi *nk_ui_create(const NkUiConfig *config);
void nk_ui_destroy(NkUi *ui);
void nk_ui_set_viewport(NkUi *ui, const NkUiViewport *viewport);
void nk_ui_set_prefs(NkUi *ui, const NkUiPrefs *prefs);
void nk_ui_push_event(NkUi *ui, const NkUiEvent *event);
void nk_ui_begin(NkUi *ui, double now_s);

/* Containers use device-independent layout units and are translated by Clay.
   Returns true when the container was opened; the caller must then call
   nk_ui_end_container exactly once. Returns false, and opens nothing, when
   ui or layout is NULL, no frame is open (nk_ui_begin not called or failed),
   nesting is already 64 deep, or the layout engine refused the element. After
   a false return the caller must NOT call nk_ui_end_container, and should skip
   the container's children. nk_ui_end closes containers left open but then
   reports the frame as failed (returns NULL). */
bool nk_ui_begin_container(NkUi *ui, NkUiId id,
                           const NkUiLayout *layout, const NkUiStyle *style);
void nk_ui_end_container(NkUi *ui);

/* Widgets append to the active Clay frame. IDs must be stable across frames.
   nk_ui_button returns true only on the frame the button is activated: it is
   focused, not NK_UI_BUTTON_DISABLED, and an ACTIVATE press (not a key repeat)
   was queued; the event is consumed. It returns false otherwise, including
   when the button could not be added. */
bool nk_ui_button(NkUi *ui, NkUiId id, const char *label, uint32_t flags);
void nk_ui_label(NkUi *ui, NkUiId id, const char *text, int font, int size_px);
/* A text run with an explicit style. The text is copied into the frame, so a
   temporary buffer is fine. */
void nk_ui_text(NkUi *ui, NkUiId id, const char *text, const NkUiTextStyle *style);
/* Copies text into storage that lives until the next nk_ui_begin. Returns ""
   when the frame arena is full or no frame is open. */
const char *nk_ui_frame_text(NkUi *ui, const char *text);
/* Word-wraps text in `style` at max_width logical px and keeps at most
   max_lines lines, ending the last kept line with an ellipsis when text was cut.
   Returns frame storage (or text itself when it already fits). Clay clips at
   most ten containers per layout, so long names are shortened, not clipped. */
const char *nk_ui_text_clamp(NkUi *ui, const char *text, const NkUiTextStyle *style,
                             float max_width, int max_lines);
/* An empty element that takes space (a spacer or a fixed gap). */
void nk_ui_spacer(NkUi *ui, NkUiId id, NkUiSizing width, NkUiSizing height);
/* Draws the focus ring around element `id` this frame, outside its bounds, with
   the ring radius following the element's own corner radius. The widgets call it
   for their focused control; a screen calls it for custom focusable containers. */
void nk_ui_focus_ring(NkUi *ui, NkUiId id, float corner_radius);
/* Consumes the first queued press of `type` (for example NK_UI_EV_TAB from the
   shoulder buttons or Q/E, NK_UI_EV_MENU, NK_UI_EV_OPTIONS) and returns true;
   dir, when not NULL, receives its direction. Screens call it for the actions
   their prompt bar names that are not a focused control's activation. */
bool nk_ui_take_event(NkUi *ui, NkUiEventType type, NkUiNavDir *dir);
/* Theme the frame was created with (a copy owned by ui). */
const NkUiTheme *nk_ui_theme(const NkUi *ui);
/* The prompt-glyph style in the current preferences. */
NkUiPromptStyle nk_ui_prompt_style(const NkUi *ui);

/* Launcher-only custom element. Returns true when the waves were queued for the
   frame. Returns false, and draws nothing, on an overlay host (rejected by
   design, not an error), with no open frame, with an empty or non-finite rect,
   or with an unknown tint_index. rect is in logical UI pixels. */
bool nk_ui_waves_element(NkUi *ui, NkUiId id, NkUiRect rect, unsigned tint_index);

/* Finishes the frame and returns its draw list, or NULL when the frame failed.
   The list is owned by ui and is valid until the next nk_ui_begin or
   nk_ui_destroy. Render it with nk_ui_vk_render (backend/nk_ui_vk.h) or
   nk_ui_sdlr_render (backend/nk_ui_sdlr.h); NkUiDrawList itself is defined in
   nk_ui_draw.h and is opaque to hosts that only forward it. */
const NkUiDrawList *nk_ui_end(NkUi *ui);
bool nk_ui_next_wake(const NkUi *ui, double *at_s);
NkUiId nk_ui_focused(const NkUi *ui);
void nk_ui_focus(NkUi *ui, NkUiId id);
size_t nk_ui_dump(const NkUi *ui, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NK_UI_H */
