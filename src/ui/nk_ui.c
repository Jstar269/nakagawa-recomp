/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui.h"

#include "nk_ui_draw.h"
#include "nk_ui_text.h"
#include "nk_ui_theme.h"
#include "nk_ui_waves.h"

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

struct NkUi {
    NkUiHostKind host;
    NkUiTheme theme;
    NkUiTextBackend text;
    NkUiViewport viewport;
    NkUiScale scale;
    NkUiPrefs prefs;
    NkUiDrawBuffer draw;
    void *clay;
    NkUiEvent events[64];
    size_t event_count;
    NkUiId focused;
    double now_s;
    unsigned container_depth;
    uint32_t text_color_stack[64];
    uint32_t text_color;
    bool frame_open;
    bool frame_ok;
    NkUiPendingWaves waves;
};

void *nk_ui_clay_create(size_t arena_bytes, float width, float height,
                        const NkUiTextBackend *text);
void nk_ui_clay_destroy(void *clay);
bool nk_ui_clay_set_dimensions(void *clay, float width, float height, float density);
bool nk_ui_clay_begin(void *clay);
bool nk_ui_clay_open_container(void *clay, NkUiId id,
                               const NkUiLayout *layout, const NkUiStyle *style);
void nk_ui_clay_close_container(void *clay);
bool nk_ui_clay_text(void *clay, NkUiId id, const char *text, size_t length,
                     int font, int size_px, uint32_t rgba);
bool nk_ui_clay_end(void *clay, NkUiDrawBuffer *draw, float density);
size_t nk_ui_dump_draw_list(const NkUiDrawList *list, char *out, size_t cap);

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
    ui->host = config != NULL ? config->host : NK_UI_HOST_LAUNCHER;
    if (ui->host != NK_UI_HOST_LAUNCHER && ui->host != NK_UI_HOST_OVERLAY) {
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

void nk_ui_begin(NkUi *ui, double now_s) {
    if (ui == NULL) {
        return;
    }
    nk_ui_draw_buffer_reset(&ui->draw);
    ui->now_s = isfinite(now_s) ? now_s : 37.0;
    ui->container_depth = 0;
    ui->text_color = ui->theme.paper;
    ui->frame_ok = true;
    memset(&ui->waves, 0, sizeof(ui->waves));
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
    if (ui == NULL || layout == NULL || style == NULL) {
        return;
    }
    layout->width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout->height = (NkUiSizing){NK_UI_SIZE_FIXED, 44.0f, 0.0f, 0.0f};
    layout->direction = NK_UI_LAYOUT_ROW;
    layout->gap = 8.0f;
    layout->padding_left = 12.0f;
    layout->padding_top = 8.0f;
    layout->padding_right = 12.0f;
    layout->padding_bottom = 8.0f;
    style->fill_rgba = (flags & NK_UI_BUTTON_PRIMARY) != 0u ?
                       ui->theme.accent_dark : ui->theme.surface_raised;
    style->border_rgba = (flags & NK_UI_BUTTON_DANGER) != 0u ?
                         ui->theme.error : ui->theme.surface_high;
    style->border_width = 1.0f;
    style->corner_radius = 6.0f;
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
    nk_ui_waves_command(ui->prefs.reduce_motion ? 37.0 : ui->now_s,
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
    if (!nk_ui_clay_end(ui->clay, &ui->draw, ui->scale.density)) {
        ui->frame_ok = false;
    }
    ui->frame_open = false;
    ui->event_count = 0;
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
    }
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
