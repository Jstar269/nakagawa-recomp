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

/* ---- Element ids: one allocation scheme --------------------------------------
   An id names one element for one frame, and two elements of one frame must not
   share one: Clay lays the second out empty. Every id comes from one of three
   places, which can never collide:
   - 0x80000000 and up: nk_ui_child_id, a widget's own parts. Never by hand.
   - A screen's own ids: NK_UI_ID(screen, offset), the screen's byte from the table
     below in the top 8 bits and a 24-bit offset under it. Each screen's file makes
     its ids through one macro of its own (`#define PAD_ID(offset) NK_UI_ID(...)`),
     so a screen's ids are allocated in one place.
   - Ids that several screens, the router or tests name (PLAYER_UI_ID_* in
     player_ui_nk.h): screen 0x00.
   A run of ids made by adding to a base (a row per control, a part per row) is a
   family: the base's `<NAME>_SPAN` says how many ids it holds, and no other id may
   fall inside it. test_player_ui.py's PlayerUiIdSourceTests checks the table, the
   ranges and the spans; the screen tests fail on any duplicate id Clay reports. */
#define NK_UI_ID(screen, offset) ((NkUiId)(((uint32_t)(screen) << 24) + (uint32_t)(offset)))
#define NK_UI_ID_SCREEN_SHARED 0x00u        /* PLAYER_UI_ID_*: named across screens */
#define NK_UI_ID_SCREEN_LIBRARY 0x10u
#define NK_UI_ID_SCREEN_TITLE 0x20u
#define NK_UI_ID_SCREEN_ABOUT 0x50u
#define NK_UI_ID_SCREEN_FLOW 0x60u          /* building, errors and the other one-card flows */
#define NK_UI_ID_SCREEN_WIZARD 0x61u
#define NK_UI_ID_SCREEN_DISC 0x62u          /* a disc found: the game-found card */
#define NK_UI_ID_SCREEN_SETTINGS 0x63u
#define NK_UI_ID_SCREEN_CONTROLS 0x64u
#define NK_UI_ID_SCREEN_DEVELOPER_LOG 0x65u
#define NK_UI_ID_SCREEN_OVERLAY 0x70u       /* the in-game overlay's pages (src/rt/overlay) */

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
   to zero (start alignment, no scrolling, every corner rounded). */
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
    /* A vertical scroll container (a Clay clip container): its children are
       clipped to its bounds and scroll with the wheel, a drag or touch, a
       control inside it taking focus, and nk_ui_scroll_to. */
    bool scroll;
} NkUiLayout;

/* Scroll containers one frame may hold; a frame that opens more is dropped. */
#define NK_UI_SCROLL_MAX 16u

/* A scroll container's state, in logical px: where it is now, and its last layout. */
typedef struct {
    float offset;       /* how far the content has moved up, 0 at the top */
    float max_offset;   /* the offset that shows the end of the content */
    float viewport;     /* the container's height */
} NkUiScrollState;

/* Corners that stay square even when corner_radius is set, for an art panel
   flush with the top of a rounded card. */
enum {
    NK_UI_CORNER_SQUARE_TL = 1u << 0,
    NK_UI_CORNER_SQUARE_TR = 1u << 1,
    NK_UI_CORNER_SQUARE_BR = 1u << 2,
    NK_UI_CORNER_SQUARE_BL = 1u << 3
};

/* What a container eases instead of cutting when it changes between frames
   (NkUiStyle.transition). */
enum {
    NK_UI_TRANSITION_COLOR = 1u << 0, /* its fill and border colours */
    NK_UI_TRANSITION_BOX = 1u << 1    /* its position and size */
};

typedef struct {
    uint32_t fill_rgba;
    uint32_t border_rgba;
    float border_width;
    float corner_radius;
    uint32_t square_corners; /* NK_UI_CORNER_SQUARE_* bits */
    /* NK_UI_TRANSITION_* bits, eased over transition_s seconds (an
       NK_UI_MOTION_* token) with Clay's ease-out. Reduce motion cuts them all,
       and so does 0. */
    uint32_t transition;
    float transition_s;
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

/* A layout error Clay reported. Most leave the layout incomplete (an element,
   word or byte that did not fit, an internal error): those are fatal and
   nk_ui_end drops the frame. A duplicate id, a floating element without its
   parent, or a percentage over 100% still lays out a whole frame, so it is
   drawn and the error is only reported. */
typedef struct {
    const char *kind;   /* Clay's error type without its prefix, e.g. "DUPLICATE_ID" */
    const char *text;   /* Clay's explanation: text_length bytes, not NUL-terminated */
    size_t text_length;
    bool fatal;
} NkUiLayoutError;

typedef struct {
    NkUiHostKind host;
    size_t arena_bytes; /* 0 selects Clay_MinMemorySize(). */
    const NkUiTextBackend *text;
    const NkUiTheme *theme; /* NULL selects the N1 theme, same as nk_ui_theme_n1(). */
    /* The host's display rules, applied to every string before it is measured,
       clamped or drawn: the launcher shows the profile folder as "~" so that
       screenshots carry no user name. The rewrite must never grow its input;
       it writes at most out_size bytes, NUL included. NULL draws text as given. */
    size_t (*display_text)(const char *in, char *out, size_t out_size);
    /* Receives each layout error when it starts: an error that repeats every
       frame is reported again only after a frame without it. NULL ignores
       them; a fatal one still drops its frame. */
    void (*report_error)(void *user, const NkUiLayoutError *error);
    void *report_user;
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
    /* 90, 100, 125 or 150. Anything else is treated as 100. Type only: the text
       is measured, spaced, given line heights and drawn this much larger, while
       the density, boxes, gaps and logical size follow ui_scale_pct alone. */
    int text_size_pct;
    /* The smallest layout the host's screens are designed for, in layout units
       (0 = none). A window smaller than this at the chosen density lowers the
       density, down to NK_UI_MIN_FIT_DENSITY, so the layout keeps this size and
       the whole UI is drawn smaller instead of running off the window. */
    float min_logical_w;
    float min_logical_h;
} NkUiViewport;

/* The lowest density a minimum layout size may lower the UI to. */
#define NK_UI_MIN_FIT_DENSITY 0.25f

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

/* Which button-prompt glyph set the prompt bar draws. Every set has a
   project-drawn geometric form; a host may also hand nk_ui Kenney's CC0 prompt
   glyphs (nk_ui_set_prompt_images), drawn instead for every set but NEUTRAL.
   Zero is the default. */
typedef enum {
    NK_UI_PROMPT_NEUTRAL = 0,       /* neutral positions; unknown pads */
    NK_UI_PROMPT_LETTERS_A_SOUTH,   /* letters, with A on the south button */
    NK_UI_PROMPT_LETTERS_A_EAST,    /* letters, with A on the east button */
    NK_UI_PROMPT_KEYCAPS,           /* keyboard keycaps */
    NK_UI_PROMPT_SHAPES,            /* cross, circle, square and triangle face buttons */
    NK_UI_PROMPT_STYLE_COUNT
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
    NK_UI_PAD_NEUTRAL = 0,  /* unknown pads */
    NK_UI_PAD_LETTERS_A_SOUTH,
    NK_UI_PAD_LETTERS_A_EAST,
    NK_UI_PAD_KEYBOARD,
    NK_UI_PAD_SHAPES        /* a cross on the south button */
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
    NK_UI_EV_CYCLE, /* Tab / Shift+Tab: dir RIGHT = next, LEFT = previous in declaration order. */
    NK_UI_EV_PAGE,  /* Page Down / Up and the pad's triggers: dir RIGHT = next page, LEFT = previous. */
    NK_UI_EV_POINTER_LEAVE, /* the pointer left the window: nothing is under it */
    NK_UI_EV_EDGE   /* Home / End: dir UP = the first item of a list, DOWN = the last. */
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
    NK_UI_BUTTON_GROW = 1u << 5,    /* fill the row instead of fitting the label */
    NK_UI_BUTTON_NOT_DEFAULT = 1u << 6, /* never the screen's default focus (top-bar tabs) */
    NK_UI_BUTTON_ROW = 1u << 7,         /* left/right stay in its row of buttons */
    /* Left and right change the control instead of moving focus (a segmented
       control, a slider): while it is focused they stay queued as NK_UI_EV_NAV for
       nk_ui_take_event. For nk_ui_register_button. */
    NK_UI_BUTTON_OWNS_HORIZONTAL = 1u << 8
};

/* Device scale and edge rounding are shared helpers for hosts and tests. The
   density comes from the UI scale alone; the text size is a separate factor on
   type (1.0 for anything but 90, 125 or 150). */
NkUiScale nk_ui_scale_for_viewport(const NkUiViewport *viewport);
float nk_ui_text_scale_for_viewport(const NkUiViewport *viewport);
NkUiPixelRect nk_ui_round_rect(NkUiRect rect, float density);

NkUi *nk_ui_create(const NkUiConfig *config);
void nk_ui_destroy(NkUi *ui);
void nk_ui_set_viewport(NkUi *ui, const NkUiViewport *viewport);
void nk_ui_set_prefs(NkUi *ui, const NkUiPrefs *prefs);
void nk_ui_push_event(NkUi *ui, const NkUiEvent *event);
/* A pointer press activates nothing by itself: the release does, when it lands
   on the control the press landed on. A host calls this when its screen changes,
   so the rest of a click that began on the old screen activates nothing on the
   new one, even a control with the same id in the same place. */
void nk_ui_cancel_press(NkUi *ui);
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
/* The text size the viewport set (1.0 at 100 %). Layout units are unchanged by
   it: only type grows, so a screen that reserves room for a run of text in
   logical px reserves it from the text's own measurements, not a constant. */
float nk_ui_text_scale(const NkUi *ui);
/* The width the layout has (NkUiScale.logical_w for the last viewport set). */
float nk_ui_layout_width(const NkUi *ui);
/* How wide `text` lays out in `style` on one line, in layout units, at the
   current text size and after the host's display rules. */
float nk_ui_text_width(const NkUi *ui, const char *text, const NkUiTextStyle *style);
/* The logical height one line of `style` lays out in at the current text size,
   or at `text_scale` (nk_ui_text_scale_for_viewport) for a host that works out a
   layout before it has a frame. */
float nk_ui_line_height(const NkUi *ui, const NkUiTextStyle *style);
float nk_ui_line_height_at(const NkUiTextStyle *style, float text_scale);
/* The height for a component designed `height` px tall around one line of
   `style` (a button, chip, badge, tab, bar or row): `height` while the line
   fits with a quarter of a line free above and below, else that much. At 100 %
   text and smaller it is always `height`. */
float nk_ui_line_box(const NkUi *ui, float height, const NkUiTextStyle *style);
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
/* Glyph images for the prompt bar (an NkUiPromptImages from nk_ui_input.h,
   which must outlive every frame drawn with it); NULL returns to the
   geometric glyphs. */
struct NkUiPromptImages;
void nk_ui_set_prompt_images(NkUi *ui, const struct NkUiPromptImages *images);

/* Launcher-only custom element. Returns true when the waves were queued for the
   frame. Returns false, and draws nothing, on an overlay host (rejected by
   design, not an error), with no open frame, with an empty or non-finite rect,
   or with an unknown tint_index. rect is in logical UI pixels. */
bool nk_ui_waves_element(NkUi *ui, NkUiId id, NkUiRect rect, unsigned tint_index);

/* Launcher-only backdrop: `picture` cover-fitted over rect (logical px), drawn over
   the waves and under the layout, beneath a scrim of the page colour that keeps
   every text colour at the contrast the waves allow (nk_ui_backdrop_scrim). While
   `shown` it fades in over NK_UI_MOTION_SLOW, and once not shown it fades out; a
   different picture starts from nothing. A frame that does not call it drops it.
   Returns true while it is drawn. Returns false, and draws nothing, on an overlay
   host, with no open frame, an unusable picture or an empty rect, and under Reduce
   motion, where the backdrop is left out altogether: it is decoration. */
struct NkUiImage;
bool nk_ui_backdrop(NkUi *ui, NkUiId id, NkUiRect rect, const struct NkUiImage *picture,
                    bool shown);
/* The scrim alpha the backdrop is drawn under at `fade` (0-1, eased): enough that a
   white picture, over waves at their brightest, stays as dark as the waves' limit
   (NK_UI_WAVES_LUMINANCE_MAX), so contrast holds through the fade too. */
float nk_ui_backdrop_scrim(float fade);

/* Finishes the frame and returns its draw list, or NULL when the frame failed.
   The list is owned by ui and is valid until the next nk_ui_begin or
   nk_ui_destroy. Render it with nk_ui_sdlr_render (backend/nk_ui_sdlr.h), or
   with nk_ui_vk_prepare outside a render pass and nk_ui_vk_render inside one
   (backend/nk_ui_vk.h); NkUiDrawList itself is defined in nk_ui_draw.h and is
   opaque to hosts that only forward it. */
const NkUiDrawList *nk_ui_end(NkUi *ui);
bool nk_ui_next_wake(const NkUi *ui, double *at_s);
/* How long a host may wait for input before it draws the next frame, at most
   idle_s: one display frame (NK_UI_MOTION_FRAME) while something moves
   (nk_ui_in_motion), until the waves' next step (nk_ui_next_wake) while they
   play, else idle_s. now_s is on the clock the frames are drawn with. */
double nk_ui_wait_s(const NkUi *ui, double now_s, double idle_s);
/* Scroll containers. nk_ui_scroll_to moves container id so the content shows
   from `offset` down (clamped to the content), from this frame on when called
   before the container opens; a container that has not opened yet starts
   there. nk_ui_scroll_state gives where the container is now, after this
   frame's scrolling, and its content and height from the last layout (false
   before it was laid out). nk_ui_scroll_in_motion says a container moved this frame, by
   wheel, drag or momentum: a host that waits for input between frames draws
   the next one soon instead. */
void nk_ui_scroll_to(NkUi *ui, NkUiId id, float offset);
bool nk_ui_scroll_state(const NkUi *ui, NkUiId id, NkUiScrollState *out);
bool nk_ui_scroll_in_motion(const NkUi *ui);
/* Something on screen is still moving: a scroll container, or a transition
   (NkUiStyle.transition) that has not reached its end. A host draws the next
   frame soon, at its display rate, instead of waiting for input. */
bool nk_ui_in_motion(const NkUi *ui);
/* Clay's layout inspector (Clay_SetDebugModeEnabled), for developer mode: a
   panel down the window's right side that lists the layout and outlines the
   element under the pointer. Clay narrows the layout by the panel's width, so
   a screen sized from the window's own width runs on under the panel. Set
   between frames. */
void nk_ui_set_layout_debug(NkUi *ui, bool enabled);
NkUiId nk_ui_focused(const NkUi *ui);
void nk_ui_focus(NkUi *ui, NkUiId id);
size_t nk_ui_dump(const NkUi *ui, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NK_UI_H */
