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
} NkUiCustomSlot;

#define NK_UI_CUSTOM_SLOTS 96u
#define NK_UI_FRAME_TEXT_BYTES 16384u

struct NkUi {
    NkUiHostKind host;
    NkUiTheme theme;
    NkUiTextBackend text;
    NkUiViewport viewport;
    NkUiScale scale;
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
    double now_s;
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
    NkUiId ring_id;
    float ring_radius;
    const NkUiPromptImages *prompt_images;
};

void *nk_ui_clay_create(size_t arena_bytes, float width, float height,
                        const NkUiTextBackend *text);
void nk_ui_clay_destroy(void *clay);
bool nk_ui_clay_set_dimensions(void *clay, float width, float height, float density);
bool nk_ui_clay_begin(void *clay);
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
bool nk_ui_clay_end(void *clay, NkUiDrawBuffer *draw, float density,
                    NkUiClayPaint paint, void *paint_user);
size_t nk_ui_dump_draw_list(const NkUiDrawList *list, char *out, size_t cap);

static void nk_ui_consume_event(NkUi *ui, size_t index) {
    if (ui == NULL || index >= ui->event_count) {
        return;
    }
    memmove(&ui->events[index], &ui->events[index + 1u],
            (ui->event_count - index - 1u) * sizeof(ui->events[0]));
    --ui->event_count;
}

static void nk_ui_update_focus_from_events(NkUi *ui) {
    size_t i = 0;
    while (ui != NULL && i < ui->event_count) {
        NkUiEvent *event = &ui->events[i];
        NkUiId focused = 0;
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
            focused = press || release
                ? nk_ui_focus_hit_test(ui->focus_graph, event->x, event->y) : 0u;
            if (press) {
                ui->pointer_press_id = focused;
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
        } else if (event->type == NK_UI_EV_WHEEL) {
            /* Screens with an actual scroller consume wheel events themselves. */
            ++i;
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

NkUiScale nk_ui_scale_for_viewport(const NkUiViewport *viewport) {
    NkUiScale result = {1.0f, 0.0f, 0.0f};
    float ui_scale = 1.0f;
    float text_scale = 1.0f;
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
    if (viewport->text_size_pct == 90 || viewport->text_size_pct == 100 ||
        viewport->text_size_pct == 125 || viewport->text_size_pct == 150) {
        text_scale = (float)viewport->text_size_pct / 100.0f;
    }
    result.density = nk_ui_clamp_density(ui_scale * text_scale);
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
    ui->viewport.pixel_w = 1;
    ui->viewport.pixel_h = 1;
    ui->viewport.display_scale = 1.0f;
    ui->viewport.ui_scale_pct = 0;
    ui->viewport.text_size_pct = 100;
    ui->scale = nk_ui_scale_for_viewport(&ui->viewport);
    if (config != NULL) {
        arena_bytes = config->arena_bytes;
    }
    ui->clay = nk_ui_clay_create(arena_bytes, ui->scale.logical_w,
                                 ui->scale.logical_h, &ui->text);
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
    if (ui == NULL || viewport == NULL) {
        return;
    }
    ui->viewport = *viewport;
    scale = nk_ui_scale_for_viewport(viewport);
    if (scale.density != ui->scale.density || scale.logical_w != ui->scale.logical_w ||
        scale.logical_h != ui->scale.logical_h) {
        ui->scale = scale;
        if (!nk_ui_clay_set_dimensions(ui->clay, scale.logical_w, scale.logical_h,
                                       scale.density)) {
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
    nk_ui_update_focus_from_events(ui);
    ui->layout_pending = false;
    nk_ui_focus_begin_frame(ui->focus_graph);
    ui->focus_entry_count = 0u;
    nk_ui_draw_buffer_reset(&ui->draw);
    ui->now_s = isfinite(now_s) ? now_s : 37.0;
    ui->container_depth = 0;
    ui->text_color = ui->theme.paper;
    ui->frame_ok = true;
    memset(&ui->waves, 0, sizeof(ui->waves));
    ui->custom_count = 0u;
    ui->frame_text_used = 0u;
    ui->ring_id = 0u;
    ui->ring_radius = 0.0f;
    ui->frame_open = nk_ui_clay_begin(ui->clay);
    if (!ui->frame_open) {
        ui->frame_ok = false;
    }
}

bool nk_ui_begin_container(NkUi *ui, NkUiId id,
                           const NkUiLayout *layout, const NkUiStyle *style) {
    if (ui == NULL || !ui->frame_open || layout == NULL || ui->container_depth >= 64u ||
        !nk_ui_clay_open_container(ui->clay, id, layout, style)) {
        return false;
    }
    ui->text_color_stack[ui->container_depth] = ui->text_color;
    if (style != NULL && (style->fill_rgba == ui->theme.accent_dark ||
                          style->fill_rgba == ui->theme.accent_light)) {
        ui->text_color = ui->theme.ink;
    } else {
        ui->text_color = ui->theme.paper;
    }
    ++ui->container_depth;
    return true;
}

void nk_ui_end_container(NkUi *ui) {
    if (ui == NULL || !ui->frame_open || ui->container_depth == 0u) {
        return;
    }
    nk_ui_clay_close_container(ui->clay);
    --ui->container_depth;
    ui->text_color = ui->text_color_stack[ui->container_depth];
}

void nk_ui_button_configuration(const NkUi *ui, uint32_t flags,
                                NkUiLayout *layout, NkUiStyle *style) {
    float height = NK_UI_CONTROL_HEIGHT;
    float padding = 18.0f;
    bool disabled = (flags & NK_UI_BUTTON_DISABLED) != 0u;
    bool primary = (flags & NK_UI_BUTTON_PRIMARY) != 0u;
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
    memcpy(out, text, length + 1u);
    ui->frame_text_used += length + 1u;
    return out;
}

static float nk_ui_measure_logical(const NkUi *ui, const NkUiTextStyle *style,
                                   const char *text, size_t length) {
    int device_px = (int)floorf((float)style->size_px * ui->scale.density + 0.5f);
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
    spacing = floorf(style->letter_spacing * ui->scale.density + 0.5f);
    if (!isfinite(size.width) || size.width < 0.0f) size.width = 0.0f;
    return (size.width + (spacing > 0.0f ? spacing * (float)count : 0.0f)) /
           ui->scale.density;
}

const char *nk_ui_text_clamp(NkUi *ui, const char *text, const NkUiTextStyle *style,
                             float max_width, int max_lines) {
    static const char ellipsis[] = "\xE2\x80\xA6";
    char out[2048]; /* a details box can carry a few install paths */
    size_t length;
    size_t line_start = 0u;
    size_t cursor = 0u;
    size_t used = 0u;
    int line = 1;
    if (ui == NULL || text == NULL || style == NULL || max_lines < 1 ||
        !(max_width > 0.0f)) {
        return text != NULL ? text : "";
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
        return text;
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

void nk_ui_focus_ring(NkUi *ui, NkUiId id, float corner_radius) {
    if (ui == NULL || id == 0u) {
        return;
    }
    ui->ring_id = id;
    ui->ring_radius = isfinite(corner_radius) && corner_radius > 0.0f ? corner_radius : 0.0f;
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
    width = image != NULL
        ? (float)(int)(height * (float)image->width / (float)image->height + 0.5f)
        : nk_ui_prompt_label_width(style, action, label, height);
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
static bool nk_ui_paint_image(NkUiDrawBuffer *draw, const NkUiImageArt *art, NkUiRect rect,
                              float density, uint32_t id) {
    NkUiPixelRect box = nk_ui_round_rect(rect, density);
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
                                  0xFFFFFFFFu, id) ||
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
                                  0xFFFFFFFFu, id)) {
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
        return nk_ui_paint_image(&ui->draw, &slot->image, rect, density, element_id);
    }
    return true;
}

/* The focus ring sits outside the element: 3 px accent, 3 px gap, radius =
   element radius + 6. High contrast: a 4 px white ring between 2 px black
   keylines, so it reads on any colour. */
static bool nk_ui_draw_focus_ring(NkUi *ui) {
    NkUiRect rect;
    float gap = NK_UI_FOCUS_RING_GAP;
    float width = NK_UI_FOCUS_RING_WIDTH;
    if (ui->ring_id == 0u || ui->ring_id != ui->focused ||
        !nk_ui_clay_element_rect(ui->clay, ui->ring_id, &rect) ||
        rect.w <= 0.0f || rect.h <= 0.0f) {
        return true;
    }
    if (ui->prefs.high_contrast_focus) {
        float outer = NK_UI_FOCUS_HC_KEYLINE * 2.0f + NK_UI_FOCUS_HC_RING_WIDTH;
        float r0 = ui->ring_radius + outer;
        float r1 = ui->ring_radius + outer - NK_UI_FOCUS_HC_KEYLINE;
        const float black[4] = {r0, r0, r0, r0};
        const float white[4] = {r1, r1, r1, r1};
        NkUiRect a = {rect.x - outer, rect.y - outer, rect.w + outer * 2.0f, rect.h + outer * 2.0f};
        float inset = NK_UI_FOCUS_HC_KEYLINE;
        NkUiRect b = {a.x + inset, a.y + inset, a.w - inset * 2.0f, a.h - inset * 2.0f};
        return nk_ui_draw_add_border_corners(&ui->draw, a, ui->scale.density, outer,
                                             black, 0x000000FFu, ui->ring_id) &&
               nk_ui_draw_add_border_corners(&ui->draw, b, ui->scale.density,
                                             NK_UI_FOCUS_HC_RING_WIDTH, white,
                                             0xFFFFFFFFu, ui->ring_id);
    }
    {
        float grow = gap + width;
        float r = ui->ring_radius + grow;
        const float radii[4] = {r, r, r, r};
        NkUiRect ring = {rect.x - grow, rect.y - grow, rect.w + grow * 2.0f, rect.h + grow * 2.0f};
        return nk_ui_draw_add_border_corners(&ui->draw, ring, ui->scale.density, width,
                                             radii, ui->theme.focus, ui->ring_id);
    }
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
        }
    }
    if (ui->waves.active) {
        NkUiPixelRect bounds = nk_ui_round_rect(ui->waves.rect, ui->scale.density);
        if (!nk_ui_draw_add_custom(&ui->draw, NK_UI_CUSTOM_WAVES,
                                   &ui->waves.command, bounds, ui->waves.id)) {
            ui->frame_ok = false;
        }
    }
    if (!nk_ui_clay_end(ui->clay, &ui->draw, ui->scale.density,
                        nk_ui_paint_custom, ui)) {
        ui->frame_ok = false;
    }
    if (!nk_ui_draw_focus_ring(ui)) {
        ui->frame_ok = false;
    }
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
    entry.scroller = 0u;
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

size_t nk_ui_dump(const NkUi *ui, char *out, size_t cap) {
    if (ui == NULL) {
        if (cap > 0u && out != NULL) {
            out[0] = '\0';
        }
        return 0;
    }
    return nk_ui_dump_draw_list(&ui->draw.list, out, cap);
}
