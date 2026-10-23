/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui_draw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool nk_ui_reserve_vertices(NkUiDrawBuffer *buffer, uint32_t needed) {
    uint32_t capacity;
    NkUiVertex *grown;
    if (needed <= buffer->vertex_capacity) {
        return true;
    }
    capacity = buffer->vertex_capacity == 0 ? 64u : buffer->vertex_capacity;
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
#if SIZE_MAX < UINT64_MAX
    if ((size_t)capacity > SIZE_MAX / sizeof(*grown)) return false;
#endif
    grown = (NkUiVertex *)realloc(buffer->vertices, (size_t)capacity * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }
    buffer->vertices = grown;
    buffer->vertex_capacity = capacity;
    return true;
}

static bool nk_ui_reserve_indices(NkUiDrawBuffer *buffer, uint32_t needed) {
    uint32_t capacity;
    uint32_t *grown;
    if (needed <= buffer->index_capacity) {
        return true;
    }
    capacity = buffer->index_capacity == 0 ? 96u : buffer->index_capacity;
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
#if SIZE_MAX < UINT64_MAX
    if ((size_t)capacity > SIZE_MAX / sizeof(*grown)) return false;
#endif
    grown = (uint32_t *)realloc(buffer->indices, (size_t)capacity * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }
    buffer->indices = grown;
    buffer->index_capacity = capacity;
    return true;
}

static bool nk_ui_reserve_commands(NkUiDrawBuffer *buffer, uint32_t needed) {
    uint32_t capacity;
    NkUiDrawCmd *grown;
    if (needed <= buffer->command_capacity) {
        return true;
    }
    capacity = buffer->command_capacity == 0 ? 32u : buffer->command_capacity;
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
#if SIZE_MAX < UINT64_MAX
    if ((size_t)capacity > SIZE_MAX / sizeof(*grown)) return false;
#endif
    grown = (NkUiDrawCmd *)realloc(buffer->commands, (size_t)capacity * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }
    buffer->commands = grown;
    buffer->command_capacity = capacity;
    return true;
}

static bool nk_ui_reserve_text_runs(NkUiDrawBuffer *buffer, uint32_t needed) {
    uint32_t capacity;
    NkUiTextRun **grown;
    if (needed <= buffer->text_run_capacity) {
        return true;
    }
    capacity = buffer->text_run_capacity == 0 ? 8u : buffer->text_run_capacity;
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
#if SIZE_MAX < UINT64_MAX
    if ((size_t)capacity > SIZE_MAX / sizeof(*grown)) return false;
#endif
    grown = (NkUiTextRun **)realloc(buffer->text_runs, (size_t)capacity * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }
    buffer->text_runs = grown;
    buffer->text_run_capacity = capacity;
    return true;
}

static uint32_t nk_ui_premultiply(uint32_t rgba) {
    uint32_t r = (rgba >> 24) & 0xFFu;
    uint32_t g = (rgba >> 16) & 0xFFu;
    uint32_t b = (rgba >> 8) & 0xFFu;
    uint32_t a = rgba & 0xFFu;
    r = (r * a + 127u) / 255u;
    g = (g * a + 127u) / 255u;
    b = (b * a + 127u) / 255u;
    return (r << 24) | (g << 16) | (b << 8) | a;
}

static void nk_ui_sync_view(NkUiDrawBuffer *buffer) {
    buffer->list.v = buffer->vertices;
    buffer->list.idx = buffer->indices;
    buffer->list.cmd = buffer->commands;
}

static bool nk_ui_append_command(NkUiDrawBuffer *buffer, NkUiDrawCmd command) {
    if (buffer->list.ncmd == UINT32_MAX ||
        !nk_ui_reserve_commands(buffer, buffer->list.ncmd + 1u)) {
        return false;
    }
    buffer->commands[buffer->list.ncmd++] = command;
    buffer->list.v = buffer->vertices;
    buffer->list.idx = buffer->indices;
    buffer->list.cmd = buffer->commands;
    return true;
}

static float nk_ui_round_float(float value) {
    return floorf(value + 0.5f);
}

static NkUiPixelRect nk_ui_rect_to_pixels(NkUiRect rect, float density) {
    return nk_ui_round_rect(rect, density);
}

static void nk_ui_set_bounds(NkUiDrawCmd *cmd, NkUiPixelRect rect) {
    cmd->bounds[0] = rect.x0;
    cmd->bounds[1] = rect.y0;
    cmd->bounds[2] = rect.x1;
    cmd->bounds[3] = rect.y1;
    cmd->clip[0] = rect.x0;
    cmd->clip[1] = rect.y0;
    cmd->clip[2] = rect.x1;
    cmd->clip[3] = rect.y1;
}

static NkUiVertex nk_ui_vertex(float x, float y, uint32_t rgba_pm) {
    NkUiVertex vertex;
    vertex.x = x;
    vertex.y = y;
    vertex.u = 0.0f;
    vertex.v = 0.0f;
    vertex.rgba_pm = rgba_pm;
    return vertex;
}

static void nk_ui_append_rect_geometry(NkUiDrawBuffer *buffer, NkUiPixelRect rect,
                                      uint32_t rgba_pm, uint32_t *first_vertex,
                                      uint32_t *first_index, uint32_t *vertex_count,
                                      uint32_t *index_count) {
    uint32_t base = buffer->list.nv;
    *first_vertex = base;
    *first_index = buffer->list.ni;
    *vertex_count = 4u;
    *index_count = 6u;
    buffer->vertices[base + 0u] = nk_ui_vertex((float)rect.x0, (float)rect.y0, rgba_pm);
    buffer->vertices[base + 1u] = nk_ui_vertex((float)rect.x1, (float)rect.y0, rgba_pm);
    buffer->vertices[base + 2u] = nk_ui_vertex((float)rect.x1, (float)rect.y1, rgba_pm);
    buffer->vertices[base + 3u] = nk_ui_vertex((float)rect.x0, (float)rect.y1, rgba_pm);
    buffer->indices[buffer->list.ni + 0u] = base + 0u;
    buffer->indices[buffer->list.ni + 1u] = base + 1u;
    buffer->indices[buffer->list.ni + 2u] = base + 2u;
    buffer->indices[buffer->list.ni + 3u] = base + 0u;
    buffer->indices[buffer->list.ni + 4u] = base + 2u;
    buffer->indices[buffer->list.ni + 5u] = base + 3u;
}

static bool nk_ui_draw_add_pixel_rect(NkUiDrawBuffer *buffer, NkUiPixelRect rect,
                                      uint32_t rgba_pm, uint32_t element_id,
                                      int kind) {
    NkUiDrawCmd command;
    uint32_t first_vertex;
    uint32_t first_index;
    uint32_t vertex_count;
    uint32_t index_count;
    if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) {
        return true;
    }
    if (buffer->list.nv > UINT32_MAX - 4u || buffer->list.ni > UINT32_MAX - 6u ||
        !nk_ui_reserve_vertices(buffer, buffer->list.nv + 4u) ||
        !nk_ui_reserve_indices(buffer, buffer->list.ni + 6u) ||
        !nk_ui_reserve_commands(buffer, buffer->list.ncmd + 1u)) {
        return false;
    }
    nk_ui_append_rect_geometry(buffer, rect, rgba_pm, &first_vertex, &first_index,
                               &vertex_count, &index_count);
    buffer->list.nv += vertex_count;
    buffer->list.ni += index_count;
    memset(&command, 0, sizeof(command));
    command.kind = kind;
    command.first = first_index;
    command.count = index_count;
    command.element_id = element_id;
    command.rgba_pm = rgba_pm;
    nk_ui_set_bounds(&command, rect);
    nk_ui_sync_view(buffer);
    return nk_ui_append_command(buffer, command);
}

static int nk_ui_clamp_int(int value, int low, int high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static void nk_ui_rounded_points(NkUiPixelRect rect, int radius, float points[16][2]) {
    static const float starts[4] = {-90.0f, 0.0f, 90.0f, 180.0f};
    float centers[4][2];
    int corner;
    int segment;
    centers[0][0] = (float)(rect.x1 - radius);
    centers[0][1] = (float)(rect.y0 + radius);
    centers[1][0] = (float)(rect.x1 - radius);
    centers[1][1] = (float)(rect.y1 - radius);
    centers[2][0] = (float)(rect.x0 + radius);
    centers[2][1] = (float)(rect.y1 - radius);
    centers[3][0] = (float)(rect.x0 + radius);
    centers[3][1] = (float)(rect.y0 + radius);
    for (corner = 0; corner < 4; ++corner) {
        for (segment = 0; segment < 4; ++segment) {
            float angle = (starts[corner] + 30.0f * (float)segment) * 0.01745329251994329577f;
            unsigned index = (unsigned)(corner * 4 + segment);
            points[index][0] = nk_ui_round_float(centers[corner][0] + (float)radius * cosf(angle));
            points[index][1] = nk_ui_round_float(centers[corner][1] + (float)radius * sinf(angle));
        }
    }
}

static bool nk_ui_append_rounded_geometry(NkUiDrawBuffer *buffer, NkUiPixelRect rect,
                                          int radius, uint32_t rgba_pm,
                                          uint32_t *first_vertex, uint32_t *first_index,
                                          uint32_t *vertex_count, uint32_t *index_count) {
    float inner[16][2];
    float outer[16][2];
    NkUiPixelRect expanded;
    uint32_t base = buffer->list.nv;
    uint32_t index_base = buffer->list.ni;
    uint32_t i;
    int outer_radius;

    if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0 || radius <= 0) {
        return false;
    }
    expanded.x0 = rect.x0 - 1;
    expanded.y0 = rect.y0 - 1;
    expanded.x1 = rect.x1 + 1;
    expanded.y1 = rect.y1 + 1;
    outer_radius = radius + 1;
    if (outer_radius > (expanded.x1 - expanded.x0) / 2) {
        outer_radius = (expanded.x1 - expanded.x0) / 2;
    }
    if (outer_radius > (expanded.y1 - expanded.y0) / 2) {
        outer_radius = (expanded.y1 - expanded.y0) / 2;
    }
    nk_ui_rounded_points(rect, radius, inner);
    nk_ui_rounded_points(expanded, outer_radius, outer);

    *first_vertex = base;
    *first_index = index_base;
    *vertex_count = 33u;
    *index_count = 144u;
    buffer->vertices[base] = nk_ui_vertex(
        nk_ui_round_float(((float)rect.x0 + (float)rect.x1) * 0.5f),
        nk_ui_round_float(((float)rect.y0 + (float)rect.y1) * 0.5f), rgba_pm);
    for (i = 0; i < 16u; ++i) {
        buffer->vertices[base + 1u + i] = nk_ui_vertex(inner[i][0], inner[i][1], rgba_pm);
        buffer->vertices[base + 17u + i] = nk_ui_vertex(outer[i][0], outer[i][1], 0u);

        buffer->indices[index_base + i * 3u + 0u] = base;
        buffer->indices[index_base + i * 3u + 1u] = base + 1u + i;
        buffer->indices[index_base + i * 3u + 2u] = base + 1u + ((i + 1u) % 16u);

        buffer->indices[index_base + 48u + i * 6u + 0u] = base + 1u + i;
        buffer->indices[index_base + 48u + i * 6u + 1u] = base + 1u + ((i + 1u) % 16u);
        buffer->indices[index_base + 48u + i * 6u + 2u] = base + 17u + ((i + 1u) % 16u);
        buffer->indices[index_base + 48u + i * 6u + 3u] = base + 1u + i;
        buffer->indices[index_base + 48u + i * 6u + 4u] = base + 17u + ((i + 1u) % 16u);
        buffer->indices[index_base + 48u + i * 6u + 5u] = base + 17u + i;
    }
    return true;
}

void nk_ui_draw_buffer_reset(NkUiDrawBuffer *buffer) {
    uint32_t i;
    if (buffer == NULL) {
        return;
    }
    for (i = 0; i < buffer->text_run_count; ++i) {
        if (buffer->text_runs[i] != NULL) {
            free((void *)buffer->text_runs[i]->text);
            free(buffer->text_runs[i]);
        }
    }
    buffer->text_run_count = 0;
    buffer->list.nv = 0;
    buffer->list.ni = 0;
    buffer->list.ncmd = 0;
    buffer->list.atlas = NULL;
    buffer->list.v = buffer->vertices;
    buffer->list.idx = buffer->indices;
    buffer->list.cmd = buffer->commands;
}

void nk_ui_draw_buffer_destroy(NkUiDrawBuffer *buffer) {
    if (buffer == NULL) {
        return;
    }
    nk_ui_draw_buffer_reset(buffer);
    free(buffer->vertices);
    free(buffer->indices);
    free(buffer->commands);
    free(buffer->text_runs);
    memset(buffer, 0, sizeof(*buffer));
}

bool nk_ui_draw_add_rect(NkUiDrawBuffer *buffer, NkUiRect rect, float density,
                         float radius, uint32_t rgba, uint32_t element_id) {
    NkUiPixelRect px;
    NkUiDrawCmd command;
    uint32_t first_vertex;
    uint32_t first_index;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t rgba_pm;
    int radius_px;
    bool rounded;
    if (buffer == NULL || density <= 0.0f || !isfinite(density)) {
        return false;
    }
    px = nk_ui_rect_to_pixels(rect, density);
    if (px.x1 <= px.x0 || px.y1 <= px.y0) {
        return true;
    }
    if (buffer->list.nv > UINT32_MAX - 33u || buffer->list.ni > UINT32_MAX - 144u) {
        return false;
    }
    radius_px = 0;
    if (isfinite(radius) && radius > 0.0f) {
        int max_radius = ((px.x1 - px.x0) < (px.y1 - px.y0) ?
                          (px.x1 - px.x0) : (px.y1 - px.y0)) / 2;
        float scaled_radius = radius * density;
        if (!isfinite(scaled_radius) || scaled_radius >= (float)max_radius) {
            radius_px = max_radius;
        } else {
            radius_px = (int)nk_ui_round_float(scaled_radius);
        }
        radius_px = nk_ui_clamp_int(radius_px, 0, max_radius);
    }
    rgba_pm = nk_ui_premultiply(rgba);
    rounded = radius_px > 0;
    vertex_count = rounded ? 33u : 4u;
    index_count = rounded ? 144u : 6u;
    if (!nk_ui_reserve_vertices(buffer, buffer->list.nv + vertex_count) ||
        !nk_ui_reserve_indices(buffer, buffer->list.ni + index_count) ||
        !nk_ui_reserve_commands(buffer, buffer->list.ncmd + 1u)) {
        return false;
    }
    if (rounded) {
        if (!nk_ui_append_rounded_geometry(buffer, px, radius_px, rgba_pm,
                                           &first_vertex, &first_index,
                                           &vertex_count, &index_count)) {
            return false;
        }
    } else {
        nk_ui_append_rect_geometry(buffer, px, rgba_pm, &first_vertex, &first_index,
                                   &vertex_count, &index_count);
    }
    buffer->list.nv += vertex_count;
    buffer->list.ni += index_count;
    memset(&command, 0, sizeof(command));
    command.kind = NK_UI_DRAW_RECTANGLE;
    command.first = first_index;
    command.count = index_count;
    command.element_id = element_id;
    command.rgba_pm = rgba_pm;
    nk_ui_set_bounds(&command, px);
    nk_ui_sync_view(buffer);
    return nk_ui_append_command(buffer, command);
}

bool nk_ui_draw_add_border(NkUiDrawBuffer *buffer, NkUiRect rect, float density,
                           float width, uint32_t rgba, uint32_t element_id) {
    NkUiPixelRect px;
    int thickness;
    int max_thickness;
    NkUiPixelRect strips[4];
    uint32_t rgba_pm;
    int i;
    if (buffer == NULL || density <= 0.0f || !isfinite(density) ||
        !isfinite(width) || width <= 0.0f) {
        return false;
    }
    px = nk_ui_rect_to_pixels(rect, density);
    if (px.x1 <= px.x0 || px.y1 <= px.y0) {
        return true;
    }
    max_thickness = (px.x1 - px.x0 < px.y1 - px.y0 ? px.x1 - px.x0 : px.y1 - px.y0) / 2;
    if (max_thickness < 1) {
        max_thickness = 1;
    }
    if (width >= (float)max_thickness / density) {
        thickness = max_thickness;
    } else {
        thickness = (int)nk_ui_round_float(width * density);
        if (thickness < 1) thickness = 1;
        if (thickness > max_thickness) thickness = max_thickness;
    }
    if (thickness * 2 >= px.x1 - px.x0 || thickness * 2 >= px.y1 - px.y0) {
        return nk_ui_draw_add_pixel_rect(buffer, px, nk_ui_premultiply(rgba),
                                         element_id, NK_UI_DRAW_BORDER);
    }
    strips[0] = (NkUiPixelRect){px.x0, px.y0, px.x1, px.y0 + thickness};
    strips[1] = (NkUiPixelRect){px.x0, px.y1 - thickness, px.x1, px.y1};
    strips[2] = (NkUiPixelRect){px.x0, px.y0 + thickness,
                                px.x0 + thickness, px.y1 - thickness};
    strips[3] = (NkUiPixelRect){px.x1 - thickness, px.y0 + thickness,
                                px.x1, px.y1 - thickness};
    rgba_pm = nk_ui_premultiply(rgba);
    for (i = 0; i < 4; ++i) {
        if (!nk_ui_draw_add_pixel_rect(buffer, strips[i], rgba_pm,
                                       element_id, NK_UI_DRAW_BORDER)) {
            return false;
        }
    }
    return true;
}

bool nk_ui_draw_add_text(NkUiDrawBuffer *buffer, NkUiPixelRect bounds,
                         const char *text, size_t length, int font,
                         int size_px, uint32_t rgba, uint32_t element_id) {
    NkUiTextRun *run;
    char *copy;
    NkUiDrawCmd command;
    if (buffer == NULL || (text == NULL && length != 0) || length == SIZE_MAX) {
        return false;
    }
    if (!nk_ui_reserve_text_runs(buffer, buffer->text_run_count + 1u) ||
        !nk_ui_reserve_commands(buffer, buffer->list.ncmd + 1u)) {
        return false;
    }
    run = (NkUiTextRun *)calloc(1, sizeof(*run));
    copy = (char *)malloc(length + 1u);
    if (run == NULL || copy == NULL) {
        free(run);
        free(copy);
        return false;
    }
    if (length != 0) {
        memcpy(copy, text, length);
    }
    copy[length] = '\0';
    run->bounds = (NkUiRect){(float)bounds.x0, (float)bounds.y0,
                             (float)(bounds.x1 - bounds.x0),
                             (float)(bounds.y1 - bounds.y0)};
    run->text = copy;
    run->length = length;
    run->font = font;
    run->size_px = size_px;
    buffer->text_runs[buffer->text_run_count++] = run;
    memset(&command, 0, sizeof(command));
    command.kind = NK_UI_DRAW_TEXT;
    command.custom_kind = NK_UI_DRAW_TEXT;
    command.custom = run;
    command.element_id = element_id;
    command.rgba_pm = nk_ui_premultiply(rgba);
    nk_ui_set_bounds(&command, bounds);
    return nk_ui_append_command(buffer, command);
}

bool nk_ui_draw_add_custom(NkUiDrawBuffer *buffer, uint32_t custom_kind,
                           const void *custom, NkUiPixelRect bounds,
                           uint32_t element_id) {
    NkUiDrawCmd command;
    if (buffer == NULL || custom == NULL ||
        !nk_ui_reserve_commands(buffer, buffer->list.ncmd + 1u)) {
        return false;
    }
    memset(&command, 0, sizeof(command));
    command.kind = NK_UI_DRAW_CUSTOM;
    command.custom_kind = custom_kind;
    command.custom = custom;
    command.element_id = element_id;
    nk_ui_set_bounds(&command, bounds);
    return nk_ui_append_command(buffer, command);
}

bool nk_ui_draw_add_scissor(NkUiDrawBuffer *buffer, bool start,
                            NkUiPixelRect bounds, uint32_t element_id) {
    NkUiDrawCmd command;
    if (buffer == NULL || !nk_ui_reserve_commands(buffer, buffer->list.ncmd + 1u)) {
        return false;
    }
    memset(&command, 0, sizeof(command));
    command.kind = start ? NK_UI_DRAW_SCISSOR_START : NK_UI_DRAW_SCISSOR_END;
    command.element_id = element_id;
    nk_ui_set_bounds(&command, bounds);
    return nk_ui_append_command(buffer, command);
}
