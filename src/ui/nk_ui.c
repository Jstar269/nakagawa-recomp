/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui.h"

#include "nk_ui_draw.h"
#include "nk_ui_focus.h"
#include "nk_ui_input.h"
#include "nk_ui_text.h"
#include "nk_ui_theme.h"
#include "nk_ui_waves.h"
#include "nk_ui_widgets.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool active;
    NkUiRect rect;
    NkUiId id;
    NkUiWavesCommand command;
} NkUiPendingWaves;

/* Payload of one Clay custom element, painted when its command comes out of the
   layout so it keeps its place in draw order. */
typedef enum {
    NK_UI_CUSTOM_SLOT_PROMPT = 1,
    NK_UI_CUSTOM_SLOT_SHAPES = 2,
    NK_UI_CUSTOM_SLOT_IMAGE = 3
} NkUiCustomSlotKind;

typedef struct {
    NkUiCustomSlotKind kind;
    NkUiPromptSpec prompt;
    NkUiShapeArt art;
    NkUiImageArt image;
    float image_alpha;      /* how far the image has faded in, 0..1 */
} NkUiCustomSlot;

/* A picture's fade: art that reaches a place already on screen eases in over
   NK_UI_MOTION_SLOW. Kept per picture element while it is drawn. */
#define NK_UI_ART_FADES 64u
typedef struct {
    NkUiId id;
    const NkUiImage *cover;
    const NkUiImage *contain;
    double start_s;         /* when it began to fade in */
    bool fading;            /* it began to fade in; else it was there at once */
    bool seen;              /* drawn in this frame */
} NkUiArtFade;

#define NK_UI_CUSTOM_SLOTS 96u
/* A wheel notch scrolls three 16 px lines, Windows' default of three lines a
   notch. A held pointer that scrolls a container further than the slop is a
   drag, so its release is no click. */
#define NK_UI_WHEEL_NOTCH_PX 48.0f
#define NK_UI_DRAG_SLOP 8.0f
/* Floating layers: a modal card over the page, the focus ring over both. */
#define NK_UI_Z_MODAL 1
#define NK_UI_Z_RING 2
#define NK_UI_FRAME_TEXT_BYTES 16384u

struct NkUi {
    NkUiHostKind host;
    NkUiTheme theme;
    NkUiTextBackend text;
    NkUiViewport viewport;
    NkUiScale scale;
    float text_scale;           /* viewport.text_size_pct / 100: type only, over the density */
    NkUiPrefs prefs;
    NkUiDrawBuffer draw;
    void *clay;
    NkUiFocusGraph *focus_graph;
    NkUiEvent events[64];
    size_t event_count;
    NkUiEvent held_events[64];  /* input waiting one frame for a laid-out focus graph */
    size_t held_count;
    bool layout_pending;        /* a control registered this frame before its first layout */
    bool graph_unplaced;        /* the last frame's focus graph has such controls */
    bool input_held;            /* this frame's input was held */
    size_t focus_entry_count;
    NkUiId focused;
    NkUiId focus_request;
    bool focus_pending;
    /* The primary pointer button, and the control it went down on: a click
       activates a control only when its press and its release both land on it. */
    bool pointer_down;
    NkUiId pointer_press_id;
    /* Where the pointer is, in logical px, and whether it is over the window.
       Hover shows only while the pointer was the last input: a key or a pad
       button hides it until the pointer moves again. hover_id is the control
       under the pointer, found from the last layout as the frame begins. */
    float pointer_x;
    float pointer_y;
    bool pointer_inside;
    bool pointer_hover;
    bool pointer_sent;          /* this frame's events already told Clay */
    NkUiId hover_id;
    float press_travel;         /* how far a held pointer has scrolled since the press */
    /* Scroll containers: the ones this frame opens, the ones the last layout
       held (Clay's scrolling and the wheel work on those), the innermost one
       open now, the wheel notches this frame's events gave them, and whether
       one moved. */
    NkUiId scroll_ids[NK_UI_SCROLL_MAX];
    size_t scroll_count;
    NkUiId laid_ids[NK_UI_SCROLL_MAX];
    size_t laid_count;
    NkUiId open_scroller;
    NkUiId scroller_stack[64];
    NkUiId container_ids[64];   /* the open containers, outermost first */
    float wheel_notches;
    bool scroll_moving;
    bool transitioning;     /* a transition ran in the last frame's layout */
    uint32_t ring_generation; /* the focus ring's element; a new one cuts to a new page */
    NkUiArtFade art_fades[NK_UI_ART_FADES];
    size_t art_fade_count;
    bool art_fading;        /* a picture is still fading in this frame */
    /* The launcher's backdrop (nk_ui_backdrop): its picture, how far it has faded
       in (0-1, before easing), whether it is shown, and whether this frame drew it. */
    struct {
        NkUiId id;
        const NkUiImage *picture;
        uint64_t serial;
        NkUiRect rect;
        float fade;
        bool shown;
        bool asked;
    } backdrop;
    double now_s;
    double frame_delta_s;       /* seconds since the previous frame; 0 on the first */
    bool frame_clock_started;
    unsigned container_depth;
    uint32_t text_color_stack[64];
    uint32_t text_color;
    bool frame_open;
    bool frame_ok;
    NkUiPendingWaves waves;
    NkUiCustomSlot custom[NK_UI_CUSTOM_SLOTS];
    size_t custom_count;
    char frame_text[NK_UI_FRAME_TEXT_BYTES];
    size_t frame_text_used;
    size_t (*display_text)(const char *in, char *out, size_t out_size);
    const NkUiPromptImages *prompt_images;
};

void *nk_ui_clay_create(size_t arena_bytes, float width, float height,
                        const NkUiTextBackend *text,
                        void (*report_error)(void *user, const NkUiLayoutError *error),
                        void *report_user);
void nk_ui_clay_destroy(void *clay);
bool nk_ui_clay_set_dimensions(void *clay, float width, float height, float density,
                               float text_scale);
int nk_ui_clay_device_px(int logical_px, float density, float text_scale);
float nk_ui_clay_device_spacing(float spacing, float density, float text_scale);
float nk_ui_clay_layout_spacing(float letter_spacing);
float nk_ui_clay_line_height(float line_height, int size_px, float text_scale);
bool nk_ui_clay_begin(void *clay);
bool nk_ui_clay_open_floating(void *clay, NkUiId id, const NkUiLayout *layout,
                              const NkUiStyle *style, const NkUiFloating *floating);
bool nk_ui_clay_open_container(void *clay, NkUiId id,
                               const NkUiLayout *layout, const NkUiStyle *style);
bool nk_ui_clay_open_element(void *clay, NkUiId id, const NkUiLayout *layout,
                             const NkUiStyle *style, void *custom_data);
bool nk_ui_clay_text_styled(void *clay, NkUiId id, const char *text, size_t length,
                            const NkUiTextStyle *style);
void nk_ui_clay_close_container(void *clay);
bool nk_ui_clay_text(void *clay, NkUiId id, const char *text, size_t length,
                     int font, int size_px, uint32_t rgba);
bool nk_ui_clay_element_rect(void *clay, NkUiId id, NkUiRect *out);
void nk_ui_clay_set_pointer(void *clay, float x, float y, bool down);
void nk_ui_clay_scroll_to(void *clay, NkUiId id, float offset);
bool nk_ui_clay_scroll_state(void *clay, NkUiId id, NkUiScrollState *out);
bool nk_ui_clay_wheel_scrolls(void *clay);
bool nk_ui_clay_transitioning(void *clay);
/* Container `id`, open in this frame, was not in the last frame's layout. */
bool nk_ui_clay_appeared(void *clay, NkUiId id);
void nk_ui_clay_set_debug(void *clay, bool enabled);
float nk_ui_clay_update_scroll(void *clay, const NkUiId *ids, size_t count, float wheel_px,
                               float delta_s);
NkUiId nk_ui_clay_pointer_target(void *clay, bool (*accept)(void *user, NkUiId id), void *user);
bool nk_ui_clay_end(void *clay, NkUiDrawBuffer *draw, float density,
                    float delta_s, NkUiClayPaint paint, void *paint_user);
size_t nk_ui_dump_draw_list(const NkUiDrawList *list, char *out, size_t cap);

static void nk_ui_consume_event(NkUi *ui, size_t index) {
    if (ui == NULL || index >= ui->event_count) {
        return;
    }
    memmove(&ui->events[index], &ui->events[index + 1u],
            (ui->event_count - index - 1u) * sizeof(ui->events[0]));
    --ui->event_count;
}

/* A scroll container's offset and content in this frame's layout, for
   nk_ui_focus_place (which takes rects from nk_ui_clay_element_rect). */
static bool nk_ui_layout_scroll(void *clay, NkUiId id, float *scroll_y, float *content_h) {
    NkUiScrollState state;
    if (!nk_ui_clay_scroll_state(clay, id, &state)) {
        return false;
    }
    *scroll_y = state.offset;
    *content_h = state.max_offset + state.viewport;
    return true;
}

static bool nk_ui_accepts_pointer(void *user, NkUiId id) {
    return nk_ui_focus_accepts_pointer((const NkUiFocusGraph *)user, id);
}

/* The control under the pointer: the innermost container Clay finds there in
   the last layout that the focus graph lets a pointer reach. */
static NkUiId nk_ui_pointer_target(NkUi *ui) {
    return nk_ui_clay_pointer_target(ui->clay, nk_ui_accepts_pointer, ui->focus_graph);
}

/* Off the window, Clay is told the pointer is at a point no layout reaches. */
static void nk_ui_send_pointer(NkUi *ui) {
    nk_ui_clay_set_pointer(ui->clay, ui->pointer_inside ? ui->pointer_x : -1.0f,
                           ui->pointer_inside ? ui->pointer_y : -1.0f, ui->pointer_down);
    ui->pointer_sent = true;
}

static void nk_ui_update_focus_from_events(NkUi *ui) {
    size_t i = 0;
    while (ui != NULL && i < ui->event_count) {
        NkUiEvent *event = &ui->events[i];
        NkUiId focused = 0;
        if (event->type != NK_UI_EV_POINTER && event->type != NK_UI_EV_WHEEL &&
            event->type != NK_UI_EV_DEVICE) {
            ui->pointer_hover = false;
        }
        if (event->type == NK_UI_EV_NAV &&
            (event->dir == NK_UI_DIR_LEFT || event->dir == NK_UI_DIR_RIGHT) &&
            nk_ui_focus_owns_horizontal(ui->focus_graph)) {
            /* Left and right belong to the focused control; the screen takes them
               with nk_ui_take_event(NK_UI_EV_NAV). */
            ++i;
        } else if (event->type == NK_UI_EV_NAV) {
            focused = nk_ui_focus_move(ui->focus_graph, event->dir);
            if (focused != 0u) {
                ui->focused = focused;
            }
            nk_ui_consume_event(ui, i);
        } else if (event->type == NK_UI_EV_CYCLE) {
            focused = nk_ui_focus_cycle(ui->focus_graph,
                                        event->dir == NK_UI_DIR_LEFT);
            if (focused != 0u) {
                ui->focused = focused;
            }
            nk_ui_consume_event(ui, i);
        } else if (event->type == NK_UI_EV_POINTER) {
            /* A press focuses the control under it; the release activates it
               only when it lands on the same control. A pointer that moves
               while held sends more down events: they are a drag, never a
               press, so a click whose first frame swapped the screen cannot
               press the control that took the old one's place. */
            bool press = event->down && !ui->pointer_down;
            bool release = !event->down && ui->pointer_down;
            ui->pointer_down = event->down;
            ui->pointer_x = event->x;
            ui->pointer_y = event->y;
            ui->pointer_inside = true;
            ui->pointer_hover = true;
            nk_ui_send_pointer(ui);
            focused = press || release ? nk_ui_pointer_target(ui) : 0u;
            if (press) {
                ui->pointer_press_id = focused;
                ui->press_travel = 0.0f;
                if (focused != 0u && nk_ui_focus_set(ui->focus_graph, focused)) {
                    ui->focused = focused;
                }
                nk_ui_consume_event(ui, i);
            } else if (release && focused != 0u && focused == ui->pointer_press_id &&
                       nk_ui_focus_set(ui->focus_graph, focused)) {
                ui->pointer_press_id = 0u;
                ui->focused = focused;
                event->type = NK_UI_EV_ACTIVATE;
                event->down = true;
                event->repeat = false;
                ++i;
            } else {
                /* Hover and drags stay separate from focus and activation. */
                if (release) ui->pointer_press_id = 0u;
                nk_ui_consume_event(ui, i);
            }
        } else if (event->type == NK_UI_EV_POINTER_LEAVE) {
            ui->pointer_inside = false;
            nk_ui_consume_event(ui, i);
        } else if (event->type == NK_UI_EV_WHEEL) {
            /* Over a scroll container with room to move, the wheel scrolls it;
               anywhere else it stays for the screen (the library turns its
               page with it). */
            if (!ui->pointer_inside || event->x != ui->pointer_x || event->y != ui->pointer_y) {
                ui->pointer_x = event->x;
                ui->pointer_y = event->y;
                ui->pointer_inside = true;
                nk_ui_send_pointer(ui);
            }
            if ((event->dir == NK_UI_DIR_UP || event->dir == NK_UI_DIR_DOWN) &&
                nk_ui_clay_wheel_scrolls(ui->clay)) {
                ui->wheel_notches += event->dir == NK_UI_DIR_UP ? 1.0f : -1.0f;
                nk_ui_consume_event(ui, i);
            } else {
                ++i;
            }
        } else {
            ++i;
        }
    }
}

static int nk_ui_clamp_edge(double value) {
    if (value <= (double)INT_MIN + 2.0) {
        return INT_MIN + 2;
    }
    if (value >= (double)INT_MAX - 2.0) {
        return INT_MAX - 2;
    }
    return (int)value;
}

static int nk_ui_round_edge(float position, float density) {
    double scaled;
    if (!isfinite(position) || !isfinite(density)) {
        return 0;
    }
    scaled = floor((double)position * (double)density + 0.5);
    return nk_ui_clamp_edge(scaled);
}

static float nk_ui_clamp_density(float scale) {
    if (isnan(scale) || scale < 1.0f) {
        return 1.0f;
    }
    if (scale >= 4.0f) {
        return 4.0f;
    }
    return scale;
}

/* The layout density comes from the UI scale alone. The text size is not part
   of it: it scales type on top of the density (nk_ui_text_scale_for_viewport),
   so the boxes, gaps and the window's logical size stay where they are and the
   layout reflows around larger text. */
NkUiScale nk_ui_scale_for_viewport(const NkUiViewport *viewport) {
    NkUiScale result = {1.0f, 0.0f, 0.0f};
    float ui_scale = 1.0f;
    float display_scale = 1.0f;
    if (viewport == NULL) {
        return result;
    }
    if (isfinite(viewport->display_scale) && viewport->display_scale > 0.0f) {
        display_scale = viewport->display_scale;
    }
    if (viewport->ui_scale_pct == 0) {
        ui_scale = display_scale;
    } else if (viewport->ui_scale_pct == 100 || viewport->ui_scale_pct == 125 ||
               viewport->ui_scale_pct == 150 || viewport->ui_scale_pct == 200) {
        ui_scale = (float)viewport->ui_scale_pct / 100.0f;
    }
    result.density = nk_ui_clamp_density(ui_scale);
    if (viewport->pixel_w > 0 && viewport->pixel_h > 0) {
        float fit = result.density;
        if (isfinite(viewport->min_logical_w) && viewport->min_logical_w > 0.0f) {
            float fit_w = (float)viewport->pixel_w / viewport->min_logical_w;
            if (fit_w < fit) fit = fit_w;
        }
        if (isfinite(viewport->min_logical_h) && viewport->min_logical_h > 0.0f) {
            float fit_h = (float)viewport->pixel_h / viewport->min_logical_h;
            if (fit_h < fit) fit = fit_h;
        }
        if (fit < result.density) {
            result.density = fit > NK_UI_MIN_FIT_DENSITY ? fit : NK_UI_MIN_FIT_DENSITY;
        }
    }
    if (viewport->pixel_w > 0) {
        result.logical_w = (float)viewport->pixel_w / result.density;
    }
    if (viewport->pixel_h > 0) {
        result.logical_h = (float)viewport->pixel_h / result.density;
    }
    return result;
}

float nk_ui_text_scale_for_viewport(const NkUiViewport *viewport) {
    if (viewport != NULL && (viewport->text_size_pct == 90 || viewport->text_size_pct == 100 ||
                             viewport->text_size_pct == 125 || viewport->text_size_pct == 150)) {
        return (float)viewport->text_size_pct / 100.0f;
    }
    return 1.0f;
}

NkUiPixelRect nk_ui_round_rect(NkUiRect rect, float density) {
    NkUiPixelRect result = {0, 0, 0, 0};
    float right = rect.x + rect.w;
    float bottom = rect.y + rect.h;
    if (!isfinite(density) || density <= 0.0f || !isfinite(rect.x) ||
        !isfinite(rect.y) || !isfinite(rect.w) || !isfinite(rect.h) ||
        !isfinite(right) || !isfinite(bottom) ||
        rect.w < 0.0f || rect.h < 0.0f) {
        return result;
    }
    result.x0 = nk_ui_round_edge(rect.x, density);
    result.y0 = nk_ui_round_edge(rect.y, density);
    result.x1 = nk_ui_round_edge(right, density);
    result.y1 = nk_ui_round_edge(bottom, density);
    return result;
}

NkUi *nk_ui_create(const NkUiConfig *config) {
    NkUi *ui = (NkUi *)calloc(1, sizeof(*ui));
    NkUiTextBackend fixed;
    const NkUiTheme *theme;
    size_t arena_bytes = 0;
    void (*report_error)(void *user, const NkUiLayoutError *error) = NULL;
    void *report_user = NULL;
    if (ui == NULL) {
        return NULL;
    }
    ui->focus_graph = nk_ui_focus_graph_create();
    if (ui->focus_graph == NULL) {
        free(ui);
        return NULL;
    }
    ui->host = config != NULL ? config->host : NK_UI_HOST_LAUNCHER;
    if (ui->host != NK_UI_HOST_LAUNCHER && ui->host != NK_UI_HOST_OVERLAY) {
        nk_ui_focus_graph_destroy(ui->focus_graph);
        free(ui);
        return NULL;
    }
    theme = config != NULL && config->theme != NULL ? config->theme : nk_ui_theme_n1();
    ui->theme = *theme;
    fixed = nk_ui_text_fixed();
    ui->text = config != NULL && config->text != NULL ? *config->text : fixed;
    if (ui->text.measure == NULL) {
        ui->text = fixed;
    }
    ui->display_text = config != NULL ? config->display_text : NULL;
    ui->viewport.pixel_w = 1;
    ui->viewport.pixel_h = 1;
    ui->viewport.display_scale = 1.0f;
    ui->viewport.ui_scale_pct = 0;
    ui->viewport.text_size_pct = 100;
    ui->scale = nk_ui_scale_for_viewport(&ui->viewport);
    ui->text_scale = 1.0f;
    if (config != NULL) {
        arena_bytes = config->arena_bytes;
        report_error = config->report_error;
        report_user = config->report_user;
    }
    ui->clay = nk_ui_clay_create(arena_bytes, ui->scale.logical_w,
                                 ui->scale.logical_h, &ui->text,
                                 report_error, report_user);
    if (ui->clay == NULL) {
        nk_ui_draw_buffer_destroy(&ui->draw);
        nk_ui_focus_graph_destroy(ui->focus_graph);
        free(ui);
        return NULL;
    }
    return ui;
}

void nk_ui_destroy(NkUi *ui) {
    if (ui == NULL) {
        return;
    }
    nk_ui_clay_destroy(ui->clay);
    nk_ui_focus_graph_destroy(ui->focus_graph);
    nk_ui_draw_buffer_destroy(&ui->draw);
    free(ui);
}

void nk_ui_set_viewport(NkUi *ui, const NkUiViewport *viewport) {
    NkUiScale scale;
    float text_scale;
    if (ui == NULL || viewport == NULL) {
        return;
    }
    ui->viewport = *viewport;
    scale = nk_ui_scale_for_viewport(viewport);
    text_scale = nk_ui_text_scale_for_viewport(viewport);
    if (scale.density != ui->scale.density || scale.logical_w != ui->scale.logical_w ||
        scale.logical_h != ui->scale.logical_h || text_scale != ui->text_scale) {
        ui->scale = scale;
        ui->text_scale = text_scale;
        if (!nk_ui_clay_set_dimensions(ui->clay, scale.logical_w, scale.logical_h,
                                       scale.density, text_scale)) {
            ui->frame_ok = false;
        }
    }
}

void nk_ui_set_prefs(NkUi *ui, const NkUiPrefs *prefs) {
    if (ui != NULL && prefs != NULL) {
        ui->prefs = *prefs;
    }
}

void nk_ui_cancel_press(NkUi *ui) {
    if (ui != NULL) ui->pointer_press_id = 0u;
}

void nk_ui_push_event(NkUi *ui, const NkUiEvent *event) {
    if (ui == NULL || event == NULL) {
        return;
    }
    if (ui->event_count == sizeof(ui->events) / sizeof(ui->events[0])) {
        memmove(ui->events, ui->events + 1,
                (ui->event_count - 1u) * sizeof(ui->events[0]));
        --ui->event_count;
    }
    ui->events[ui->event_count++] = *event;
}

/* Puts held input back in front of anything that arrived since, in order. */
static void nk_ui_restore_held_events(NkUi *ui) {
    size_t cap = sizeof(ui->events) / sizeof(ui->events[0]);
    size_t keep;
    if (ui->held_count == 0u) {
        return;
    }
    keep = ui->event_count;
    if (keep > cap - ui->held_count) {
        keep = cap - ui->held_count;
    }
    memmove(&ui->events[ui->held_count], ui->events, keep * sizeof(ui->events[0]));
    memcpy(ui->events, ui->held_events, ui->held_count * sizeof(ui->events[0]));
    ui->event_count = ui->held_count + keep;
    ui->held_count = 0u;
}

void nk_ui_begin(NkUi *ui, double now_s) {
    size_t first_spatial;
    size_t i;
    if (ui == NULL) {
        return;
    }
    nk_ui_restore_held_events(ui);
    /* A screen's first frame registers its controls before Clay has laid them
       out, so the focus graph it leaves has them all at the origin and a move
       or a pointer hit through it goes nowhere. From the first such event on,
       input waits one frame for the laid-out graph, in order; what comes before
       it (an activation of the focused control) runs now. Input is never held
       twice in a row. */
    first_spatial = ui->event_count;
    if (ui->graph_unplaced && !ui->input_held) {
        for (i = 0u; i < ui->event_count; ++i) {
            if (ui->events[i].type == NK_UI_EV_NAV || ui->events[i].type == NK_UI_EV_CYCLE ||
                ui->events[i].type == NK_UI_EV_POINTER) {
                first_spatial = i;
                break;
            }
        }
    }
    ui->input_held = first_spatial < ui->event_count;
    if (ui->input_held) {
        ui->held_count = ui->event_count - first_spatial;
        memcpy(ui->held_events, &ui->events[first_spatial],
               ui->held_count * sizeof(ui->events[0]));
        ui->event_count = first_spatial;
    }
    ui->pointer_sent = false;
    nk_ui_update_focus_from_events(ui);
    now_s = isfinite(now_s) ? now_s : 37.0;
    /* Clay's time-based layout and scrolling advance by the time since the last
       frame. The first frame, and a clock that stands still or steps back, add
       none. */
    ui->frame_delta_s = ui->frame_clock_started && now_s > ui->now_s ? now_s - ui->now_s : 0.0;
    ui->frame_clock_started = true;
    ui->now_s = now_s;
    /* Once a frame, as Clay's lifecycle asks: the layout under a still pointer
       may have moved. A frame whose events told Clay already keeps that state,
       so Clay sees a press or release as this frame's. Then Clay's scrolling,
       over the containers the last frame opened. */
    if (!ui->pointer_sent) {
        nk_ui_send_pointer(ui);
    }
    ui->hover_id = ui->pointer_hover && ui->pointer_inside ? nk_ui_pointer_target(ui) : 0u;
    {
        NkUiFocusScrollRequest request;
        float moved = nk_ui_clay_update_scroll(ui->clay, ui->laid_ids, ui->laid_count,
                                               ui->wheel_notches * NK_UI_WHEEL_NOTCH_PX,
                                               (float)ui->frame_delta_s);
        ui->scroll_moving = moved > 0.0f;
        ui->wheel_notches = 0.0f;
        if (ui->pointer_down) {
            ui->press_travel += moved;
            if (ui->press_travel > NK_UI_DRAG_SLOP) {
                ui->pointer_press_id = 0u;
            }
        }
        /* Focus that moved into a scroll container brings its control into view. */
        if (nk_ui_focus_take_scroll(ui->focus_graph, &request)) {
            nk_ui_clay_scroll_to(ui->clay, request.scroller, request.scroll_y);
        }
        ui->scroll_count = 0u;
        ui->open_scroller = 0u;
    }
    ui->layout_pending = false;
    nk_ui_focus_begin_frame(ui->focus_graph);
    ui->focus_entry_count = 0u;
    nk_ui_draw_buffer_reset(&ui->draw);
    ui->container_depth = 0;
    ui->text_color = ui->theme.paper;
    ui->frame_ok = true;
    memset(&ui->waves, 0, sizeof(ui->waves));
    ui->custom_count = 0u;
    ui->frame_text_used = 0u;
    ui->frame_open = nk_ui_clay_begin(ui->clay);
    if (!ui->frame_open) {
        ui->frame_ok = false;
    }
}

/* A scroll container joins the focus graph, so focus moving to a control
   inside it can bring the control into view. nk_ui_focus_place gives it this
   frame's layout once the frame is laid out; a container Clay has not laid
   out yet joins on its second frame. */
static void nk_ui_register_scroller(NkUi *ui, NkUiId id) {
    NkUiFocusScroller scroller;
    NkUiScrollState state;
    if (!nk_ui_clay_scroll_state(ui->clay, id, &state) ||
        !nk_ui_clay_element_rect(ui->clay, id, &scroller.viewport)) {
        return;
    }
    scroller.id = id;
    scroller.scroll_x = 0.0f;
    scroller.scroll_y = state.offset;
    scroller.content_w = scroller.viewport.w;
    scroller.content_h = state.max_offset + state.viewport;
    (void)nk_ui_focus_register_scroller(ui->focus_graph, &scroller);
}

static bool nk_ui_open(NkUi *ui, NkUiId id, const NkUiLayout *layout, const NkUiStyle *style,
                       const NkUiFloating *floating) {
    NkUiStyle cut;
    /* Reduce motion: every change is a cut. */
    if (ui != NULL && ui->prefs.reduce_motion && style != NULL && style->transition != 0u) {
        cut = *style;
        cut.transition = 0u;
        style = &cut;
    }
    if (ui == NULL || !ui->frame_open || layout == NULL || ui->container_depth >= 64u ||
        !(floating != NULL ? nk_ui_clay_open_floating(ui->clay, id, layout, style, floating)
                           : nk_ui_clay_open_container(ui->clay, id, layout, style))) {
        return false;
    }
    ui->scroller_stack[ui->container_depth] = ui->open_scroller;
    ui->container_ids[ui->container_depth] = id;
    if (layout->scroll) {
        if (ui->scroll_count == NK_UI_SCROLL_MAX) {
            ui->frame_ok = false;
        } else {
            ui->scroll_ids[ui->scroll_count++] = id;
        }
        nk_ui_register_scroller(ui, id);
        ui->open_scroller = id;
    }
    ui->text_color_stack[ui->container_depth] = ui->text_color;
    if (style != NULL && (style->fill_rgba == ui->theme.accent_dark ||
                          style->fill_rgba == ui->theme.accent_hover ||
                          style->fill_rgba == ui->theme.accent_pressed ||
                          style->fill_rgba == ui->theme.accent_light)) {
        ui->text_color = ui->theme.ink;
    } else {
        ui->text_color = ui->theme.paper;
    }
    ++ui->container_depth;
    return true;
}

bool nk_ui_begin_container(NkUi *ui, NkUiId id,
                           const NkUiLayout *layout, const NkUiStyle *style) {
    return nk_ui_open(ui, id, layout, style, NULL);
}

bool nk_ui_begin_modal(NkUi *ui, NkUiId id, const NkUiLayout *layout, const NkUiStyle *style) {
    NkUiFloating modal;
    memset(&modal, 0, sizeof(modal));
    modal.z = NK_UI_Z_MODAL;
    modal.centered = true;
    return nk_ui_open(ui, id, layout, style, &modal);
}

void nk_ui_end_container(NkUi *ui) {
    if (ui == NULL || !ui->frame_open || ui->container_depth == 0u) {
        return;
    }
    nk_ui_clay_close_container(ui->clay);
    --ui->container_depth;
    ui->text_color = ui->text_color_stack[ui->container_depth];
    ui->open_scroller = ui->scroller_stack[ui->container_depth];
}

NkUiTypeRole nk_ui_button_role(uint32_t flags) {
    return (flags & NK_UI_BUTTON_LARGE) != 0u ? NK_UI_TYPE_LABEL_L :
           (flags & NK_UI_BUTTON_SMALL) != 0u ? NK_UI_TYPE_LABEL_S : NK_UI_TYPE_LABEL;
}

void nk_ui_button_configuration(const NkUi *ui, uint32_t flags,
                                NkUiLayout *layout, NkUiStyle *style) {
    float height = NK_UI_CONTROL_HEIGHT;
    float padding = 18.0f;
    bool disabled = (flags & NK_UI_BUTTON_DISABLED) != 0u;
    bool primary = (flags & NK_UI_BUTTON_PRIMARY) != 0u;
    NkUiTextStyle label;
    if (ui == NULL || layout == NULL || style == NULL) {
        return;
    }
    if ((flags & NK_UI_BUTTON_LARGE) != 0u) {
        height = NK_UI_CONTROL_HEIGHT_LARGE;
        padding = primary ? 34.0f : 22.0f;
    } else if ((flags & NK_UI_BUTTON_SMALL) != 0u) {
        height = NK_UI_CONTROL_HEIGHT_SMALL;
        padding = NK_UI_SPACE_3;
    }
    /* The label's own size: a larger text size grows the button around it. */
    label = nk_ui_type(nk_ui_button_role(flags), 0u);
    height = nk_ui_line_box(ui, height, &label);
    memset(layout, 0, sizeof(*layout));
    layout->width = (flags & NK_UI_BUTTON_GROW) != 0u
        ? (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f}
        : (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
    layout->height = (NkUiSizing){NK_UI_SIZE_FIXED, height, 0.0f, 0.0f};
    layout->direction = NK_UI_LAYOUT_ROW;
    layout->gap = NK_UI_SPACE_2;
    layout->padding_left = padding;
    layout->padding_right = padding;
    layout->align_x = NK_UI_ALIGN_CENTER;
    layout->align_y = NK_UI_ALIGN_CENTER;
    memset(style, 0, sizeof(*style));
    style->border_width = NK_UI_BORDER_HAIRLINE;
    style->corner_radius = NK_UI_RADIUS_CONTROL;
    if (disabled) {
        style->fill_rgba = primary ? ui->theme.surface_high : 0x00000000u;
        style->border_rgba = ui->theme.line;
    } else if (primary) {
        style->fill_rgba = ui->theme.accent_dark;
        style->border_rgba = ui->theme.accent_dark;
    } else {
        style->fill_rgba = 0x00000000u;
        style->border_rgba = ui->theme.line_strong;
    }
}

uint32_t nk_ui_button_text_rgba(const NkUi *ui, uint32_t flags) {
    if (ui == NULL) {
        return 0u;
    }
    if ((flags & NK_UI_BUTTON_DISABLED) != 0u) {
        return ui->theme.dim;
    }
    if ((flags & NK_UI_BUTTON_PRIMARY) != 0u) {
        return ui->theme.ink;
    }
    if ((flags & NK_UI_BUTTON_DANGER) != 0u) {
        return ui->theme.error;
    }
    return ui->theme.paper;
}

bool nk_ui_take_activation(NkUi *ui, NkUiId id) {
    size_t i;
    if (ui == NULL || ui->focused != id) {
        return false;
    }
    for (i = 0; i < ui->event_count; ++i) {
        NkUiEvent *event = &ui->events[i];
        if (event->type == NK_UI_EV_ACTIVATE && event->down && !event->repeat) {
            memmove(event, event + 1, (ui->event_count - i - 1u) * sizeof(*event));
            --ui->event_count;
            return true;
        }
    }
    return false;
}

bool nk_ui_take_event(NkUi *ui, NkUiEventType type, NkUiNavDir *dir) {
    size_t i;
    if (ui == NULL) {
        return false;
    }
    for (i = 0; i < ui->event_count; ++i) {
        NkUiEvent *event = &ui->events[i];
        if (event->type == type && event->down && !event->repeat) {
            if (dir != NULL) {
                *dir = event->dir;
            }
            memmove(event, event + 1, (ui->event_count - i - 1u) * sizeof(*event));
            --ui->event_count;
            return true;
        }
    }
    return false;
}

void nk_ui_label(NkUi *ui, NkUiId id, const char *text, int font, int size_px) {
    size_t length;
    if (ui == NULL || !ui->frame_open || text == NULL || size_px <= 0) {
        return;
    }
    if (ui->display_text != NULL) {
        text = nk_ui_frame_text(ui, text);
    }
    length = strlen(text);
    if (!nk_ui_clay_text(ui->clay, id, text, length, font, size_px, ui->text_color)) {
        ui->frame_ok = false;
    }
}

const char *nk_ui_frame_text(NkUi *ui, const char *text) {
    size_t length;
    char *out;
    if (ui == NULL || !ui->frame_open || text == NULL) {
        return "";
    }
    length = strlen(text);
    if (length >= NK_UI_FRAME_TEXT_BYTES - ui->frame_text_used) {
        ui->frame_ok = false;
        return "";
    }
    out = &ui->frame_text[ui->frame_text_used];
    if (ui->display_text != NULL) {
        /* The rewrite never grows its input, so length + 1 bytes hold it. */
        length = ui->display_text(text, out, length + 1u);
    } else {
        memcpy(out, text, length + 1u);
    }
    ui->frame_text_used += length + 1u;
    return out;
}

/* A run's width in layout units as Clay lays it out: measured at its text size. */
static float nk_ui_measure_logical(const NkUi *ui, const NkUiTextStyle *style,
                                   const char *text, size_t length) {
    int device_px = nk_ui_clay_device_px(style->size_px, ui->scale.density, ui->text_scale);
    NkUiTextSize size;
    float spacing;
    size_t count = 0u;
    size_t i;
    if (length == 0u || ui->text.measure == NULL || device_px < 1) {
        return 0.0f;
    }
    size = ui->text.measure(ui->text.user, style->font, device_px, text, length);
    for (i = 0; i < length; ++i) {
        if (((unsigned char)text[i] & 0xC0u) != 0x80u) ++count;
    }
    spacing = nk_ui_clay_device_spacing(nk_ui_clay_layout_spacing(style->letter_spacing),
                                        ui->scale.density, ui->text_scale);
    if (!isfinite(size.width) || size.width < 0.0f) size.width = 0.0f;
    return (size.width + (spacing > 0.0f ? spacing * (float)count : 0.0f)) /
           ui->scale.density;
}

float nk_ui_text_scale(const NkUi *ui) {
    return ui != NULL ? ui->text_scale : 1.0f;
}

float nk_ui_layout_width(const NkUi *ui) {
    return ui != NULL ? ui->scale.logical_w : 0.0f;
}

float nk_ui_text_width(const NkUi *ui, const char *text, const NkUiTextStyle *style) {
    char shown[2048];
    if (ui == NULL || text == NULL || style == NULL || style->size_px <= 0) {
        return 0.0f;
    }
    /* As drawn: after the host's display rules. */
    if (ui->display_text != NULL) {
        (void)ui->display_text(text, shown, sizeof(shown));
        text = shown;
    }
    return nk_ui_measure_logical(ui, style, text, strlen(text));
}

float nk_ui_line_height_at(const NkUiTextStyle *style, float text_scale) {
    if (style == NULL || style->size_px <= 0) {
        return 0.0f;
    }
    return nk_ui_clay_line_height(style->line_height, style->size_px, text_scale);
}

float nk_ui_line_height(const NkUi *ui, const NkUiTextStyle *style) {
    return nk_ui_line_height_at(style, ui != NULL ? ui->text_scale : 1.0f);
}

/* A quarter of a line above and below the text: room enough that a one-line
   component never reads as clipped. Every component's design height leaves at
   least that much at 100 %, so only larger text ever changes a box. */
float nk_ui_line_box(const NkUi *ui, float height, const NkUiTextStyle *style) {
    float need;
    if (ui == NULL || style == NULL || !(ui->text_scale > 1.0f)) {
        return height;
    }
    need = ceilf(nk_ui_line_height(ui, style) * 1.5f);
    return need > height ? need : height;
}

const char *nk_ui_text_clamp(NkUi *ui, const char *text, const NkUiTextStyle *style,
                             float max_width, int max_lines) {
    static const char ellipsis[] = "\xE2\x80\xA6";
    char out[2048]; /* a details box can carry a few install paths */
    char shown[sizeof(out)];
    size_t length;
    size_t line_start = 0u;
    size_t cursor = 0u;
    size_t used = 0u;
    int line = 1;
    if (ui == NULL || text == NULL || style == NULL || max_lines < 1 ||
        !(max_width > 0.0f)) {
        return text != NULL ? text : "";
    }
    /* Measure the text as it will be drawn: a cut made before the display rules
       ran could keep part of the profile folder they would have replaced. */
    if (ui->display_text != NULL) {
        (void)ui->display_text(text, shown, sizeof(shown));
        text = shown;
    }
    length = strlen(text);
    if (length >= sizeof(out) - sizeof(ellipsis)) {
        length = sizeof(out) - sizeof(ellipsis) - 1u;
    }
    /* Greedy word wrap, as Clay wraps; stop when the last allowed line fills. */
    while (cursor < length) {
        size_t word_end = cursor;
        while (word_end < length && text[word_end] != ' ') ++word_end;
        if (nk_ui_measure_logical(ui, style, text + line_start, word_end - line_start) >
                max_width && cursor > line_start) {
            if (line == max_lines) {
                break;
            }
            ++line;
            line_start = cursor;
        }
        cursor = word_end < length ? word_end + 1u : word_end;
    }
    if (cursor >= length && (line < max_lines ||
        nk_ui_measure_logical(ui, style, text + line_start, length - line_start) <= max_width)) {
        return text == shown ? nk_ui_frame_text(ui, shown) : text;
    }
    /* Cut the last line back until it and the ellipsis fit. */
    used = cursor > line_start ? cursor : line_start;
    while (used > line_start) {
        size_t end = used;
        while (end > line_start && text[end - 1u] == ' ') --end;
        memcpy(out, text, end);
        memcpy(out + end, ellipsis, sizeof(ellipsis));
        if (nk_ui_measure_logical(ui, style, out + line_start,
                                  end - line_start + sizeof(ellipsis) - 1u) <= max_width) {
            return nk_ui_frame_text(ui, out);
        }
        --used;
        while (used > line_start && ((unsigned char)text[used] & 0xC0u) == 0x80u) --used;
    }
    memcpy(out, text, line_start);
    memcpy(out + line_start, ellipsis, sizeof(ellipsis));
    return nk_ui_frame_text(ui, out);
}

void nk_ui_text(NkUi *ui, NkUiId id, const char *text, const NkUiTextStyle *style) {
    NkUiTextStyle resolved;
    const char *stored;
    if (ui == NULL || !ui->frame_open || text == NULL || style == NULL ||
        style->size_px <= 0) {
        return;
    }
    resolved = *style;
    if (resolved.rgba == 0u) {
        resolved.rgba = ui->text_color;
    }
    stored = nk_ui_frame_text(ui, text);
    if (!nk_ui_clay_text_styled(ui->clay, id, stored, strlen(stored), &resolved)) {
        ui->frame_ok = false;
    }
}

void nk_ui_spacer(NkUi *ui, NkUiId id, NkUiSizing width, NkUiSizing height) {
    NkUiLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.width = width;
    layout.height = height;
    if (nk_ui_begin_container(ui, id, &layout, NULL)) {
        nk_ui_end_container(ui);
    }
}

/* One band of the ring: a floating border around the open container, `expand`
   outside it, over everything else, clipped as the container is, and taking
   no pointer. Its draw commands carry the control's id. The band is the same
   Clay element on every control, so when focus moves Clay sees it change
   parent and eases it from the old control to the new one. */
static void nk_ui_ring_band(NkUi *ui, NkUiId id, uint32_t part, float expand, float width,
                            float radius, uint32_t rgba) {
    NkUiLayout layout;
    NkUiStyle style;
    NkUiFloating ring;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    memset(&style, 0, sizeof(style));
    style.border_rgba = rgba;
    style.border_width = width;
    style.corner_radius = radius;
    style.transition = NK_UI_TRANSITION_BOX;
    style.transition_s = (float)NK_UI_MOTION_FAST;
    memset(&ring, 0, sizeof(ring));
    ring.expand = expand;
    ring.z = NK_UI_Z_RING;
    ring.passthrough = true;
    ring.clip_to_parent = true;
    ring.draw_id = id;
    if (nk_ui_open(ui, nk_ui_child_id((NkUiId)ui->ring_generation, part), &layout, &style,
                   &ring)) {
        nk_ui_end_container(ui);
    }
}

/* The focus ring sits outside the element: 3 px accent, 3 px gap, radius =
   element radius + 6. High contrast: a 4 px white ring between 2 px black
   keylines, so it reads on any colour. Each band is a Clay floating element
   on the focused control, the container open now. */
void nk_ui_focus_ring(NkUi *ui, NkUiId id, float corner_radius) {
    float radius;
    if (ui == NULL || !ui->frame_open || id == 0u || id != ui->focused ||
        ui->container_depth == 0u) {
        return;
    }
    radius = isfinite(corner_radius) && corner_radius > 0.0f ? corner_radius : 0.0f;
    /* A control that was not on screen in the last frame (a new page) gets a
       new ring, which Clay shows at once: it does not fly in from where the
       last page left it. */
    if (nk_ui_clay_appeared(ui->clay, id)) {
        ++ui->ring_generation;
    }
    if (ui->prefs.high_contrast_focus) {
        float outer = NK_UI_FOCUS_HC_KEYLINE * 2.0f + NK_UI_FOCUS_HC_RING_WIDTH;
        nk_ui_ring_band(ui, id, 0xF0u, outer, outer, radius + outer, 0x000000FFu);
        nk_ui_ring_band(ui, id, 0xF1u, outer - NK_UI_FOCUS_HC_KEYLINE,
                        NK_UI_FOCUS_HC_RING_WIDTH, radius + outer - NK_UI_FOCUS_HC_KEYLINE,
                        0xFFFFFFFFu);
    } else {
        float grow = NK_UI_FOCUS_RING_GAP + NK_UI_FOCUS_RING_WIDTH;
        nk_ui_ring_band(ui, id, 0xF0u, grow, NK_UI_FOCUS_RING_WIDTH, radius + grow,
                        ui->theme.focus);
    }
}

const NkUiTheme *nk_ui_theme(const NkUi *ui) {
    return ui != NULL ? &ui->theme : nk_ui_theme_n1();
}

NkUiPromptStyle nk_ui_prompt_style(const NkUi *ui) {
    return ui != NULL ? ui->prefs.prompt_style : NK_UI_PROMPT_NEUTRAL;
}

static NkUiCustomSlot *nk_ui_custom_slot(NkUi *ui) {
    if (ui == NULL || !ui->frame_open || ui->custom_count >= NK_UI_CUSTOM_SLOTS) {
        if (ui != NULL) ui->frame_ok = false;
        return NULL;
    }
    memset(&ui->custom[ui->custom_count], 0, sizeof(ui->custom[0]));
    return &ui->custom[ui->custom_count++];
}

void nk_ui_set_prompt_images(NkUi *ui, const NkUiPromptImages *images) {
    if (ui != NULL) {
        ui->prompt_images = images;
    }
}

/* The glyph image for a style, or NULL to draw the geometry. */
static const NkUiImage *nk_ui_prompt_image_for(const NkUi *ui, NkUiPromptStyle style,
                                               NkUiPromptAction action) {
    const NkUiImage *image;
    if (ui->prompt_images == NULL || (unsigned)style >= (unsigned)NK_UI_PROMPT_STYLE_COUNT ||
        (unsigned)action >= (unsigned)NK_UI_PROMPT_ACT_COUNT) {
        return NULL;
    }
    image = &ui->prompt_images->image[style][action];
    return image->rgba_pm != NULL && image->width > 0u && image->height > 0u ? image : NULL;
}

/* How wide a glyph `height` px tall is drawn: its image at its own aspect, or
   the project-drawn form for its label. */
static float nk_ui_prompt_glyph_width(const NkUiImage *image, NkUiPromptStyle style,
                                      NkUiPromptAction action, const char *label, float height) {
    return image != NULL
        ? (float)(int)(height * (float)image->width / (float)image->height + 0.5f)
        : nk_ui_prompt_label_width(style, action, label, height);
}

float nk_ui_prompt_glyph_size(const NkUi *ui, NkUiPromptAction action, float height) {
    NkUiPromptStyle style;
    if (ui == NULL || !isfinite(height) || height <= 0.0f) {
        return 0.0f;
    }
    style = ui->prefs.prompt_style;
    if ((unsigned)style >= (unsigned)NK_UI_PROMPT_STYLE_COUNT) {
        return 0.0f;
    }
    return nk_ui_prompt_glyph_width(nk_ui_prompt_image_for(ui, style, action), style, action,
                                    NULL, height);
}

bool nk_ui_prompt_glyph(NkUi *ui, NkUiId id, NkUiPromptAction action, float height) {
    return ui != NULL &&
           nk_ui_prompt_glyph_in(ui, id, ui->prefs.prompt_style, action, NULL, height);
}

bool nk_ui_prompt_glyph_in(NkUi *ui, NkUiId id, NkUiPromptStyle style, NkUiPromptAction action,
                           const char *label, float height) {
    NkUiCustomSlot *slot;
    NkUiLayout layout;
    const NkUiImage *image = NULL;
    float width;
    if (ui == NULL || !isfinite(height) || height <= 0.0f ||
        (unsigned)style >= (unsigned)NK_UI_PROMPT_STYLE_COUNT) {
        return false;
    }
    if (label != NULL) {
        /* The legend lives in frame storage until the glyph is painted. */
        label = nk_ui_frame_text(ui, label);
    } else {
        image = nk_ui_prompt_image_for(ui, style, action);
    }
    width = nk_ui_prompt_glyph_width(image, style, action, label, height);
    slot = nk_ui_custom_slot(ui);
    if (slot == NULL) {
        return false;
    }
    slot->kind = NK_UI_CUSTOM_SLOT_PROMPT;
    slot->prompt.style = style;
    slot->prompt.action = action;
    slot->prompt.label = label;
    slot->prompt.fg_rgba = ui->theme.paper;
    slot->prompt.dim_rgba = ui->theme.placeholder;
    slot->prompt.face_rgba = ui->theme.surface_high;
    slot->prompt.font = NK_UI_FONT_MONO_MEDIUM;
    slot->prompt.image = image;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, width, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, height, 0.0f, 0.0f};
    if (!nk_ui_clay_open_element(ui->clay, id, &layout, NULL, slot)) {
        ui->frame_ok = false;
        return false;
    }
    nk_ui_clay_close_container(ui->clay);
    return true;
}

bool nk_ui_shapes(NkUi *ui, NkUiId id, NkUiSizing width, NkUiSizing height,
                  const NkUiShapeArt *art) {
    NkUiCustomSlot *slot;
    NkUiLayout layout;
    if (ui == NULL || art == NULL || art->shapes == NULL || art->count == 0u ||
        !(art->view_w > 0.0f) || !(art->view_h > 0.0f)) {
        return false;
    }
    slot = nk_ui_custom_slot(ui);
    if (slot == NULL) {
        return false;
    }
    slot->kind = NK_UI_CUSTOM_SLOT_SHAPES;
    slot->art = *art;
    memset(&layout, 0, sizeof(layout));
    layout.width = width;
    layout.height = height;
    if (!nk_ui_clay_open_element(ui->clay, id, &layout, NULL, slot)) {
        ui->frame_ok = false;
        return false;
    }
    nk_ui_clay_close_container(ui->clay);
    return true;
}

/* How far picture `id` has faded in. Art that reaches a container already on
   screen in the last frame (a disc's picture read after its card was drawn)
   or that changes in place eases in over NK_UI_MOTION_SLOW; art that comes
   with its container, a new page's, is there at once, and so is any under
   Reduce motion. */
static float nk_ui_art_alpha(NkUi *ui, NkUiId id, const NkUiImageArt *art) {
    NkUiArtFade *fade = NULL;
    double t;
    size_t i;
    for (i = 0u; i < ui->art_fade_count; ++i) {
        if (ui->art_fades[i].id == id) {
            fade = &ui->art_fades[i];
            break;
        }
    }
    if (fade == NULL) {
        if (ui->art_fade_count == NK_UI_ART_FADES) {
            return 1.0f;
        }
        fade = &ui->art_fades[ui->art_fade_count++];
        memset(fade, 0, sizeof(*fade));
        fade->id = id;
        fade->cover = art->cover;
        fade->contain = art->contain;
        fade->fading = ui->container_depth > 0u &&
                       !nk_ui_clay_appeared(ui->clay, ui->container_ids[ui->container_depth - 1u]);
        fade->start_s = ui->now_s;
    } else if (fade->cover != art->cover || fade->contain != art->contain) {
        fade->cover = art->cover;
        fade->contain = art->contain;
        fade->fading = true;
        fade->start_s = ui->now_s;
    }
    fade->seen = true;
    if (!fade->fading || ui->prefs.reduce_motion) {
        return 1.0f;
    }
    t = (ui->now_s - fade->start_s) / NK_UI_MOTION_SLOW;
    if (!(t < 1.0)) {
        fade->fading = false;
        return 1.0f;
    }
    ui->art_fading = true;
    t = t > 0.0 ? 1.0 - t : 1.0;
    /* Clay's ease-out, as the other transitions use. */
    return (float)(1.0 - t * t * t);
}

bool nk_ui_image_art(NkUi *ui, NkUiId id, NkUiSizing width, NkUiSizing height,
                     const NkUiImageArt *art) {
    NkUiCustomSlot *slot;
    NkUiLayout layout;
    if (ui == NULL || art == NULL || (art->cover == NULL && art->contain == NULL)) {
        return false;
    }
    slot = nk_ui_custom_slot(ui);
    if (slot == NULL) {
        return false;
    }
    slot->kind = NK_UI_CUSTOM_SLOT_IMAGE;
    slot->image = *art;
    slot->image_alpha = nk_ui_art_alpha(ui, id, art);
    memset(&layout, 0, sizeof(layout));
    layout.width = width;
    layout.height = height;
    if (!nk_ui_clay_open_element(ui->clay, id, &layout, NULL, slot)) {
        ui->frame_ok = false;
        return false;
    }
    nk_ui_clay_close_container(ui->clay);
    return true;
}

static bool nk_ui_image_usable(const NkUiImage *image) {
    return image != NULL && image->rgba_pm != NULL && image->width > 0u && image->height > 0u;
}

/* The image scaled to fill (cover) or fit inside (contain) a w x h box,
   centred on (cx, cy); device pixels, rounded to whole pixels. */
static NkUiPixelRect nk_ui_image_fit(const NkUiImage *image, float cx, float cy, float w,
                                     float h, bool cover) {
    NkUiPixelRect px;
    float scale_x = w / (float)image->width;
    float scale_y = h / (float)image->height;
    float scale = cover ? (scale_x > scale_y ? scale_x : scale_y)
                        : (scale_x < scale_y ? scale_x : scale_y);
    float drawn_w = (float)image->width * scale;
    float drawn_h = (float)image->height * scale;
    px.x0 = (int)floorf(cx - drawn_w * 0.5f + 0.5f);
    px.y0 = (int)floorf(cy - drawn_h * 0.5f + 0.5f);
    px.x1 = px.x0 + (int)floorf(drawn_w + 0.5f);
    px.y1 = px.y0 + (int)floorf(drawn_h + 0.5f);
    return px;
}

/* Cover: the picture fills the element, cropped by a scissor. Contain: the
   icon fits inside contain_scale of the element, centred. */
static bool nk_ui_paint_image(NkUiDrawBuffer *draw, const NkUiImageArt *art, float alpha,
                              NkUiRect rect, float density, uint32_t id) {
    NkUiPixelRect box = nk_ui_round_rect(rect, density);
    /* White at the fade's alpha: the backends draw an image tinted by it. */
    uint32_t tint = 0xFFFFFF00u | (uint32_t)(alpha * 255.0f + 0.5f);
    float cx = (float)(box.x0 + box.x1) * 0.5f;
    float cy = (float)(box.y0 + box.y1) * 0.5f;
    float box_w = (float)(box.x1 - box.x0);
    float box_h = (float)(box.y1 - box.y0);
    if (box_w <= 0.0f || box_h <= 0.0f) {
        return true;
    }
    if (nk_ui_image_usable(art->cover)) {
        if (!nk_ui_draw_add_scissor(draw, true, box, id) ||
            !nk_ui_draw_add_image(draw, art->cover,
                                  nk_ui_image_fit(art->cover, cx, cy, box_w, box_h, true),
                                  tint, id) ||
            !nk_ui_draw_add_scissor(draw, false, box, id)) {
            return false;
        }
    }
    if (nk_ui_image_usable(art->contain)) {
        float scale = art->contain_scale > 0.0f && art->contain_scale <= 1.0f
            ? art->contain_scale : 1.0f;
        if (!nk_ui_draw_add_image(draw, art->contain,
                                  nk_ui_image_fit(art->contain, cx, cy, box_w * scale,
                                                  box_h * scale, false),
                                  tint, id)) {
            return false;
        }
    }
    return true;
}

/* Fits the art's view box inside rect ("contain", centred) and draws its shapes
   in order. */
static bool nk_ui_paint_shapes(NkUiDrawBuffer *draw, const NkUiShapeArt *art,
                               NkUiRect rect, float density, uint32_t id) {
    float scale;
    float origin_x;
    float origin_y;
    size_t i;
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return true;
    }
    scale = rect.w / art->view_w < rect.h / art->view_h
        ? rect.w / art->view_w : rect.h / art->view_h;
    origin_x = rect.x + (rect.w - art->view_w * scale) * 0.5f - art->view_x * scale;
    origin_y = rect.y + (rect.h - art->view_h * scale) * 0.5f - art->view_y * scale;
    for (i = 0; i < art->count; ++i) {
        const NkUiShape *shape = &art->shapes[i];
        if (shape->kind == NK_UI_SHAPE_RECT) {
            NkUiRect r = {origin_x + shape->v[0] * scale, origin_y + shape->v[1] * scale,
                          shape->v[2] * scale, shape->v[3] * scale};
            if (!nk_ui_draw_add_rect(draw, r, density, shape->radius * scale,
                                     shape->rgba, id)) {
                return false;
            }
        } else if (shape->kind == NK_UI_SHAPE_CIRCLE) {
            float xy[64];
            unsigned k;
            float cx = (origin_x + shape->v[0] * scale) * density;
            float cy = (origin_y + shape->v[1] * scale) * density;
            float radius = shape->v[2] * scale * density;
            for (k = 0; k < 32u; ++k) {
                float angle = (float)k * 0.19634954084936207f; /* 2 pi / 32 */
                xy[k * 2u] = cx + radius * cosf(angle);
                xy[k * 2u + 1u] = cy + radius * sinf(angle);
            }
            if (!nk_ui_draw_add_convex(draw, xy, 32u, shape->rgba, id)) {
                return false;
            }
        } else if (shape->kind == NK_UI_SHAPE_POLY && shape->count >= 3u &&
                   shape->count <= NK_UI_SHAPE_MAX_POINTS) {
            float xy[NK_UI_SHAPE_MAX_POINTS * 2u];
            unsigned k;
            for (k = 0; k < shape->count; ++k) {
                xy[k * 2u] = (origin_x + shape->v[k * 2u] * scale) * density;
                xy[k * 2u + 1u] = (origin_y + shape->v[k * 2u + 1u] * scale) * density;
            }
            if (!nk_ui_draw_add_convex(draw, xy, shape->count, shape->rgba, id)) {
                return false;
            }
        }
    }
    return true;
}

static bool nk_ui_paint_custom(void *user, void *data, NkUiRect rect, float density,
                               uint32_t element_id) {
    NkUi *ui = (NkUi *)user;
    const NkUiCustomSlot *slot = (const NkUiCustomSlot *)data;
    if (ui == NULL || slot == NULL) {
        return true;
    }
    if (slot->kind == NK_UI_CUSTOM_SLOT_PROMPT) {
        return nk_ui_prompt_draw(&ui->draw, &slot->prompt, rect, density, element_id);
    }
    if (slot->kind == NK_UI_CUSTOM_SLOT_SHAPES) {
        return nk_ui_paint_shapes(&ui->draw, &slot->art, rect, density, element_id);
    }
    if (slot->kind == NK_UI_CUSTOM_SLOT_IMAGE) {
        return nk_ui_paint_image(&ui->draw, &slot->image, slot->image_alpha, rect, density,
                                 element_id);
    }
    return true;
}

bool nk_ui_waves_element(NkUi *ui, NkUiId id, NkUiRect rect, unsigned tint_index) {
    NkUiWavePalette palette;
    NkUiPixelRect bounds;
    if (ui == NULL || !ui->frame_open) {
        return false;
    }
    if (ui->host == NK_UI_HOST_OVERLAY) {
        return false;
    }
    if (!isfinite(rect.x) || !isfinite(rect.y) || !isfinite(rect.w) ||
        !isfinite(rect.h) || rect.w <= 0.0f || rect.h <= 0.0f ||
        !nk_ui_waves_palette(tint_index, &palette)) {
        return false;
    }
    bounds = nk_ui_round_rect(rect, ui->scale.density);
    if (bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0) {
        return false;
    }
    ui->waves.active = true;
    ui->waves.rect = rect;
    ui->waves.id = id;
    /* The waves step at 30 Hz (NK_UI_MOTION_WAVE_FRAME), so a backend can keep
       the last frame for every UI redraw in between. */
    nk_ui_waves_command(ui->prefs.reduce_motion ? 37.0 :
                            floor(ui->now_s / NK_UI_MOTION_WAVE_FRAME) * NK_UI_MOTION_WAVE_FRAME,
                        (float)(bounds.x1 - bounds.x0), (float)(bounds.y1 - bounds.y0),
                        &palette, &ui->waves.command);
    return true;
}

bool nk_ui_backdrop(NkUi *ui, NkUiId id, NkUiRect rect, const NkUiImage *picture, bool shown) {
    float step;
    if (ui == NULL || !ui->frame_open || ui->host == NK_UI_HOST_OVERLAY) {
        return false;
    }
    if (!isfinite(rect.x) || !isfinite(rect.y) || !isfinite(rect.w) || !isfinite(rect.h) ||
        rect.w <= 0.0f || rect.h <= 0.0f || !nk_ui_image_usable(picture) ||
        ui->prefs.reduce_motion) {
        return false;
    }
    if (ui->backdrop.picture == NULL || picture->serial != ui->backdrop.serial) {
        ui->backdrop.fade = 0.0f;   /* a new picture starts from nothing */
    }
    ui->backdrop.id = id;
    ui->backdrop.picture = picture;
    ui->backdrop.serial = picture->serial;
    ui->backdrop.rect = rect;
    ui->backdrop.shown = shown;
    ui->backdrop.asked = true;
    /* As a transition does: one starting now is given one display frame of the idle
       time before it, not the whole wait for input. */
    step = (float)((!ui->transitioning && ui->frame_delta_s > NK_UI_MOTION_FRAME
                        ? NK_UI_MOTION_FRAME : ui->frame_delta_s) / NK_UI_MOTION_SLOW);
    if (!(step > 0.0f)) step = 0.0f;
    ui->backdrop.fade += shown ? step : -step;
    if (ui->backdrop.fade < 0.0f) ui->backdrop.fade = 0.0f;
    if (ui->backdrop.fade > 1.0f) ui->backdrop.fade = 1.0f;
    /* Still fading: the host draws the next frame soon (nk_ui_in_motion). */
    if (shown ? ui->backdrop.fade < 1.0f : ui->backdrop.fade > 0.0f) ui->art_fading = true;
    return ui->backdrop.fade > 0.0f || shown;
}

/* The backdrop under its scrim, eased with Clay's ease-out as the other fades are. */
static bool nk_ui_paint_backdrop(NkUi *ui) {
    float t = 1.0f - ui->backdrop.fade;
    float eased = 1.0f - t * t * t;
    float scrim = nk_ui_backdrop_scrim(eased);
    NkUiImageArt art;
    memset(&art, 0, sizeof(art));
    art.cover = ui->backdrop.picture;
    return nk_ui_paint_image(&ui->draw, &art, eased, ui->backdrop.rect, ui->scale.density,
                             ui->backdrop.id) &&
           nk_ui_draw_add_rect(&ui->draw, ui->backdrop.rect, ui->scale.density, 0.0f,
                               (ui->theme.surface & 0xFFFFFF00u) |
                                   (uint32_t)(scrim * 255.0f + 0.999f),
                               ui->backdrop.id);
}

const NkUiDrawList *nk_ui_end(NkUi *ui) {
    if (ui == NULL || !ui->frame_open) {
        return NULL;
    }
    if (ui->container_depth != 0u) {
        ui->frame_ok = false;
        while (ui->container_depth != 0u) {
            nk_ui_clay_close_container(ui->clay);
            --ui->container_depth;
            ui->text_color = ui->text_color_stack[ui->container_depth];
            ui->open_scroller = ui->scroller_stack[ui->container_depth];
        }
    }
    if (ui->waves.active) {
        NkUiPixelRect bounds = nk_ui_round_rect(ui->waves.rect, ui->scale.density);
        if (!nk_ui_draw_add_custom(&ui->draw, NK_UI_CUSTOM_WAVES,
                                   &ui->waves.command, bounds, ui->waves.id)) {
            ui->frame_ok = false;
        }
    }
    if (ui->backdrop.asked && ui->backdrop.fade > 0.0f && !nk_ui_paint_backdrop(ui)) {
        ui->frame_ok = false;
    }
    if (!ui->backdrop.asked) {
        ui->backdrop.picture = NULL;   /* a frame without it ends it */
        ui->backdrop.fade = 0.0f;
    }
    ui->backdrop.asked = false;
    /* Clay adds the frame's time to a transition from the frame it starts on.
       The time since the last frame was idle when nothing was moving then, so
       a transition starting now gets one frame of it, not the whole wait for
       input, and begins its course where it should. */
    {
        double transition_delta = ui->frame_delta_s;
        if (!ui->transitioning && transition_delta > NK_UI_MOTION_FRAME) {
            transition_delta = NK_UI_MOTION_FRAME;
        }
        if (!nk_ui_clay_end(ui->clay, &ui->draw, ui->scale.density, (float)transition_delta,
                            nk_ui_paint_custom, ui)) {
            ui->frame_ok = false;
        }
    }
    ui->transitioning = nk_ui_clay_transitioning(ui->clay) || ui->art_fading;
    /* A picture not drawn this frame starts afresh when it comes back. */
    {
        size_t kept = 0u;
        size_t i;
        for (i = 0u; i < ui->art_fade_count; ++i) {
            if (ui->art_fades[i].seen) {
                ui->art_fades[kept] = ui->art_fades[i];
                ui->art_fades[kept].seen = false;
                ++kept;
            }
        }
        ui->art_fade_count = kept;
        ui->art_fading = false;
    }
    memcpy(ui->laid_ids, ui->scroll_ids, ui->scroll_count * sizeof(ui->scroll_ids[0]));
    ui->laid_count = ui->scroll_count;
    nk_ui_focus_place(ui->focus_graph, nk_ui_clay_element_rect, nk_ui_layout_scroll, ui->clay);
    if (ui->focus_pending) {
        if (ui->focus_request == 0u) {
            ui->focused = 0u;
        } else if (nk_ui_focus_set(ui->focus_graph, ui->focus_request)) {
            ui->focused = ui->focus_request;
        }
        ui->focus_pending = false;
        ui->focus_request = 0u;
    }
    nk_ui_focus_end_frame(ui->focus_graph);
    ui->focused = nk_ui_focus_current(ui->focus_graph);
    ui->frame_open = false;
    ui->event_count = 0;
    ui->graph_unplaced = ui->layout_pending;
    nk_ui_restore_held_events(ui);
    return ui->frame_ok ? &ui->draw.list : NULL;
}

bool nk_ui_next_wake(const NkUi *ui, double *at_s) {
    double frame;
    if (ui == NULL || !ui->waves.active || ui->prefs.reduce_motion) {
        return false;
    }
    if (!isfinite(ui->now_s * 30.0)) {
        return false;
    }
    frame = floor(ui->now_s * 30.0) + 1.0;
    if (!isfinite(frame)) {
        return false;
    }
    if (at_s != NULL) {
        *at_s = frame / 30.0;
    }
    return true;
}

double nk_ui_wait_s(const NkUi *ui, double now_s, double idle_s) {
    double wake;
    double wait = idle_s;
    if (ui == NULL || !isfinite(idle_s) || idle_s < 0.0) {
        return 0.0;
    }
    if (nk_ui_in_motion(ui)) {
        wait = NK_UI_MOTION_FRAME;
    } else if (isfinite(now_s) && nk_ui_next_wake(ui, &wake)) {
        wait = wake - now_s;
    }
    return wait < 0.0 ? 0.0 : wait < idle_s ? wait : idle_s;
}

NkUiId nk_ui_focused(const NkUi *ui) {
    return ui != NULL ? ui->focused : 0u;
}

void nk_ui_focus(NkUi *ui, NkUiId id) {
    if (ui != NULL) {
        ui->focused = id;
        ui->focus_request = id;
        ui->focus_pending = id != 0u;
        if (id != 0u && nk_ui_focus_set(ui->focus_graph, id)) {
            ui->focus_pending = false;
            ui->focus_request = 0u;
        }
    }
}

void nk_ui_register_button(NkUi *ui, NkUiId id, uint32_t flags) {
    NkUiFocusEntry entry;
    NkUiRect rect = {0.0f, 0.0f, 0.0f, 0.0f};
    if (ui == NULL || id == 0u || ui->focus_graph == NULL) {
        return;
    }
    if (!nk_ui_clay_element_rect(ui->clay, id, &rect)) {
        ui->layout_pending = true;
    }
    entry.id = id;
    entry.rect = rect;
    entry.flags = (flags & NK_UI_BUTTON_DISABLED) != 0u
        ? NK_UI_FOCUS_DISABLED : 0u;
    if ((flags & NK_UI_BUTTON_ROW) != 0u) {
        entry.flags |= NK_UI_FOCUS_ROW_MEMBER;
    }
    if ((flags & NK_UI_BUTTON_OWNS_HORIZONTAL) != 0u) {
        entry.flags |= NK_UI_FOCUS_OWNS_HORIZONTAL;
    }
    /* A destructive action is never the default focus (PLAYER_UI_DESIGN.md). */
    if ((flags & NK_UI_BUTTON_DANGER) != 0u) {
        entry.flags |= NK_UI_FOCUS_NO_DEFAULT;
    }
    /* The page's first enabled control that may take the default is its
       default, whatever was focused before: when the screen changes and the
       old focus is gone, focus lands here (the title page's PLAY or BUILD
       PACKAGE), not on the first control drawn (a top-bar tab). */
    if ((entry.flags & NK_UI_FOCUS_DISABLED) == 0u && ui->focus_entry_count == 0u &&
        (flags & (NK_UI_BUTTON_NOT_DEFAULT | NK_UI_BUTTON_DANGER)) == 0u) {
        entry.flags |= NK_UI_FOCUS_DEFAULT;
    }
    entry.scope = NK_UI_SCOPE_ROOT;
    entry.scroller = ui->open_scroller;
    (void)nk_ui_focus_register(ui->focus_graph, &entry);
    /* Controls that may not take default focus, disabled ones included, do
       not use up the default. */
    if ((entry.flags & NK_UI_FOCUS_DEFAULT) != 0u) {
        ++ui->focus_entry_count;
    }
}

bool nk_ui_button_focused(const NkUi *ui, NkUiId id) {
    return ui != NULL && ui->focused == id;
}

void nk_ui_set_layout_debug(NkUi *ui, bool enabled) {
    if (ui != NULL && !ui->frame_open) {
        nk_ui_clay_set_debug(ui->clay, enabled);
    }
}

void nk_ui_scroll_to(NkUi *ui, NkUiId id, float offset) {
    if (ui != NULL) {
        nk_ui_clay_scroll_to(ui->clay, id, offset);
    }
}

bool nk_ui_scroll_state(const NkUi *ui, NkUiId id, NkUiScrollState *out) {
    return ui != NULL && nk_ui_clay_scroll_state(ui->clay, id, out);
}

bool nk_ui_scroll_in_motion(const NkUi *ui) {
    return ui != NULL && ui->scroll_moving;
}

bool nk_ui_in_motion(const NkUi *ui) {
    return ui != NULL && (ui->scroll_moving || ui->transitioning);
}

bool nk_ui_hovered(const NkUi *ui, NkUiId id) {
    return ui != NULL && id != 0u && ui->hover_id == id;
}

bool nk_ui_pressed(const NkUi *ui, NkUiId id) {
    return nk_ui_hovered(ui, id) && ui->pointer_down && ui->pointer_press_id == id;
}

size_t nk_ui_dump(const NkUi *ui, char *out, size_t cap) {
    if (ui == NULL) {
        if (cap > 0u && out != NULL) {
            out[0] = '\0';
        }
        return 0;
    }
    return nk_ui_dump_draw_list(&ui->draw.list, out, cap);
}
