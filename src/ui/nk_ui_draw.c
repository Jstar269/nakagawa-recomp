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

/* Rounded outline points, four per corner (0, 30, 60, 90 degrees), clockwise from
   the top-right corner. radius[] is per corner in TL, TR, BR, BL order; a zero
   radius puts all four points on the square corner. */
#define NK_UI_CORNER_POINTS 7u /* 15 degree steps: smooth at tile radii */
#define NK_UI_OUTLINE_POINTS (NK_UI_CORNER_POINTS * 4u)
#define NK_UI_ROUNDED_VERTICES (1u + NK_UI_OUTLINE_POINTS * 2u)
#define NK_UI_ROUNDED_INDICES (NK_UI_OUTLINE_POINTS * 9u)

/* Rounded outline points, NK_UI_CORNER_POINTS per corner, clockwise from the
   top-right corner, for the rect moved `inset` px inwards (negative: outwards).
   radius[] is per corner in TL, TR, BR, BL order. Points snap to half pixels. */
static float nk_ui_snap_half(float value) {
    return nk_ui_round_float(value * 2.0f) * 0.5f;
}

static void nk_ui_rounded_points(NkUiPixelRect rect, const int radius[4], float inset,
                                 float points[NK_UI_OUTLINE_POINTS][2]) {
    static const float starts[4] = {-90.0f, 0.0f, 90.0f, 180.0f};
    /* Corner order around the outline: TR, BR, BL, TL; dx, dy point inwards. */
    static const int order[4] = {1, 2, 3, 0};
    static const float dx[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
    static const float dy[4] = {1.0f, -1.0f, -1.0f, 1.0f};
    const float corner_x[4] = {(float)rect.x1, (float)rect.x1, (float)rect.x0, (float)rect.x0};
    const float corner_y[4] = {(float)rect.y0, (float)rect.y1, (float)rect.y1, (float)rect.y0};
    int corner;
    int segment;
    for (corner = 0; corner < 4; ++corner) {
        float r = (float)radius[order[corner]];
        float offset = r > inset ? r : inset;
        float arc = r - inset > 0.0f ? r - inset : 0.0f;
        float cx = corner_x[corner] + dx[corner] * offset;
        float cy = corner_y[corner] + dy[corner] * offset;
        for (segment = 0; segment < (int)NK_UI_CORNER_POINTS; ++segment) {
            float angle = (starts[corner] + 90.0f * (float)segment /
                           (float)(NK_UI_CORNER_POINTS - 1u)) * 0.01745329251994329577f;
            unsigned index = (unsigned)corner * NK_UI_CORNER_POINTS + (unsigned)segment;
            points[index][0] = nk_ui_snap_half(cx + arc * cosf(angle));
            points[index][1] = nk_ui_snap_half(cy + arc * sinf(angle));
        }
    }
}

/* A rounded fill: solid inside an outline half a pixel in from the rect, fading
   to nothing half a pixel outside it, so straight edges cover whole pixels
   exactly and curves are antialiased. */
static bool nk_ui_append_rounded_geometry(NkUiDrawBuffer *buffer, NkUiPixelRect rect,
                                          const int radius[4], uint32_t rgba_pm,
                                          uint32_t *first_vertex, uint32_t *first_index,
                                          uint32_t *vertex_count, uint32_t *index_count) {
    float inner[NK_UI_OUTLINE_POINTS][2];
    float outer[NK_UI_OUTLINE_POINTS][2];
    uint32_t base = buffer->list.nv;
    uint32_t index_base = buffer->list.ni;
    uint32_t i;

    if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) {
        return false;
    }
    nk_ui_rounded_points(rect, radius, 0.5f, inner);
    nk_ui_rounded_points(rect, radius, -0.5f, outer);

    *first_vertex = base;
    *first_index = index_base;
    *vertex_count = NK_UI_ROUNDED_VERTICES;
    *index_count = NK_UI_ROUNDED_INDICES;
    buffer->vertices[base] = nk_ui_vertex(
        nk_ui_snap_half(((float)rect.x0 + (float)rect.x1) * 0.5f),
        nk_ui_snap_half(((float)rect.y0 + (float)rect.y1) * 0.5f), rgba_pm);
    for (i = 0; i < NK_UI_OUTLINE_POINTS; ++i) {
        const uint32_t n = NK_UI_OUTLINE_POINTS;
        const uint32_t ring = index_base + n * 3u + i * 6u;
        buffer->vertices[base + 1u + i] = nk_ui_vertex(inner[i][0], inner[i][1], rgba_pm);
        buffer->vertices[base + 1u + n + i] = nk_ui_vertex(outer[i][0], outer[i][1], 0u);

        buffer->indices[index_base + i * 3u + 0u] = base;
        buffer->indices[index_base + i * 3u + 1u] = base + 1u + i;
        buffer->indices[index_base + i * 3u + 2u] = base + 1u + ((i + 1u) % n);

        buffer->indices[ring + 0u] = base + 1u + i;
        buffer->indices[ring + 1u] = base + 1u + ((i + 1u) % n);
        buffer->indices[ring + 2u] = base + 1u + n + ((i + 1u) % n);
        buffer->indices[ring + 3u] = base + 1u + i;
        buffer->indices[ring + 4u] = base + 1u + n + ((i + 1u) % n);
        buffer->indices[ring + 5u] = base + 1u + n + i;
    }
    return true;
}

/* Corner radius in device pixels, clamped to half the shorter side. */
static int nk_ui_radius_px(NkUiPixelRect px, float radius, float density) {
    int max_radius = ((px.x1 - px.x0) < (px.y1 - px.y0) ?
                      (px.x1 - px.x0) : (px.y1 - px.y0)) / 2;
    float scaled_radius;
    int radius_px;
    if (!isfinite(radius) || radius <= 0.0f) {
        return 0;
    }
    scaled_radius = radius * density;
    if (!isfinite(scaled_radius) || scaled_radius >= (float)max_radius) {
        radius_px = max_radius;
    } else {
        radius_px = (int)nk_ui_round_float(scaled_radius);
    }
    return nk_ui_clamp_int(radius_px, 0, max_radius);
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

bool nk_ui_draw_add_rect_corners(NkUiDrawBuffer *buffer, NkUiRect rect, float density,
                                 const float radius[4], uint32_t rgba,
                                 uint32_t element_id) {
    NkUiPixelRect px;
    NkUiDrawCmd command;
    uint32_t first_vertex;
    uint32_t first_index;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t rgba_pm;
    int radius_px[4];
    bool rounded = false;
    int corner;
    if (buffer == NULL || radius == NULL || density <= 0.0f || !isfinite(density)) {
        return false;
    }
    px = nk_ui_rect_to_pixels(rect, density);
    if (px.x1 <= px.x0 || px.y1 <= px.y0) {
        return true;
    }
    if (buffer->list.nv > UINT32_MAX - NK_UI_ROUNDED_VERTICES ||
        buffer->list.ni > UINT32_MAX - NK_UI_ROUNDED_INDICES) {
        return false;
    }
    for (corner = 0; corner < 4; ++corner) {
        radius_px[corner] = nk_ui_radius_px(px, radius[corner], density);
        rounded = rounded || radius_px[corner] > 0;
    }
    rgba_pm = nk_ui_premultiply(rgba);
    vertex_count = rounded ? NK_UI_ROUNDED_VERTICES : 4u;
    index_count = rounded ? NK_UI_ROUNDED_INDICES : 6u;
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

bool nk_ui_draw_add_rect(NkUiDrawBuffer *buffer, NkUiRect rect, float density,
                         float radius, uint32_t rgba, uint32_t element_id) {
    const float radii[4] = {radius, radius, radius, radius};
    return nk_ui_draw_add_rect_corners(buffer, rect, density, radii, rgba, element_id);
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

/* Geometry-only command: the caller has already appended `vertex_count`
   vertices and `index_count` indices. */
static bool nk_ui_finish_geometry(NkUiDrawBuffer *buffer, uint32_t first_index,
                                  uint32_t vertex_count, uint32_t index_count,
                                  NkUiPixelRect bounds, uint32_t rgba_pm,
                                  uint32_t element_id, int kind) {
    NkUiDrawCmd command;
    buffer->list.nv += vertex_count;
    buffer->list.ni += index_count;
    memset(&command, 0, sizeof(command));
    command.kind = kind;
    command.first = first_index;
    command.count = index_count;
    command.element_id = element_id;
    command.rgba_pm = rgba_pm;
    nk_ui_set_bounds(&command, bounds);
    nk_ui_sync_view(buffer);
    return nk_ui_append_command(buffer, command);
}

/* One quarter of a ring: the corner of a rounded border. Corner 0..3 is TL, TR,
   BR, BL. outer is the corner radius in device pixels, thickness the border
   width; both edges get a one-pixel transparent fringe, like rounded fills. */
#define NK_UI_ARC_SEGMENTS 6u
static bool nk_ui_draw_add_corner_ring(NkUiDrawBuffer *buffer, NkUiPixelRect px,
                                       int corner, int outer, int thickness,
                                       uint32_t rgba_pm, uint32_t element_id) {
    static const float starts[4] = {180.0f, 270.0f, 0.0f, 90.0f};
    const uint32_t points = NK_UI_ARC_SEGMENTS + 1u;
    const uint32_t vertex_count = points * 4u;
    const uint32_t index_count = NK_UI_ARC_SEGMENTS * 3u * 6u;
    float cx;
    float cy;
    float radii[4];
    int inner = outer - thickness;
    uint32_t base;
    uint32_t index_base;
    uint32_t i;
    uint32_t band;
    NkUiPixelRect bounds;
    if (inner < 0) inner = 0;
    switch (corner) {
    case 0: cx = (float)(px.x0 + outer); cy = (float)(px.y0 + outer);
        bounds = (NkUiPixelRect){px.x0, px.y0, px.x0 + outer, px.y0 + outer}; break;
    case 1: cx = (float)(px.x1 - outer); cy = (float)(px.y0 + outer);
        bounds = (NkUiPixelRect){px.x1 - outer, px.y0, px.x1, px.y0 + outer}; break;
    case 2: cx = (float)(px.x1 - outer); cy = (float)(px.y1 - outer);
        bounds = (NkUiPixelRect){px.x1 - outer, px.y1 - outer, px.x1, px.y1}; break;
    default: cx = (float)(px.x0 + outer); cy = (float)(px.y1 - outer);
        bounds = (NkUiPixelRect){px.x0, px.y1 - outer, px.x0 + outer, px.y1}; break;
    }
    if (buffer->list.nv > UINT32_MAX - vertex_count ||
        buffer->list.ni > UINT32_MAX - index_count ||
        !nk_ui_reserve_vertices(buffer, buffer->list.nv + vertex_count) ||
        !nk_ui_reserve_indices(buffer, buffer->list.ni + index_count) ||
        !nk_ui_reserve_commands(buffer, buffer->list.ncmd + 1u)) {
        return false;
    }
    /* Edges sit on pixel centres, half a pixel inside each side of the band,
       with the transparent fringe half a pixel outside: the arc then has the
       same weight as the straight runs, which cover whole pixels exactly. */
    radii[0] = (float)outer + 0.5f;
    radii[1] = (float)outer - 0.5f;
    radii[2] = (float)inner + 0.5f;
    radii[3] = inner > 0 ? (float)inner - 0.5f : 0.0f;
    if (radii[2] > radii[1]) radii[2] = radii[1];
    base = buffer->list.nv;
    index_base = buffer->list.ni;
    for (i = 0; i < points; ++i) {
        float angle = (starts[corner] + 90.0f * (float)i / (float)NK_UI_ARC_SEGMENTS) *
                      0.01745329251994329577f;
        float c = cosf(angle);
        float s = sinf(angle);
        for (band = 0; band < 4u; ++band) {
            uint32_t color = band == 1u || band == 2u ? rgba_pm : 0u;
            if (band == 3u && inner == 0) color = rgba_pm;
            buffer->vertices[base + i * 4u + band] = nk_ui_vertex(
                nk_ui_round_float((cx + radii[band] * c) * 2.0f) * 0.5f,
                nk_ui_round_float((cy + radii[band] * s) * 2.0f) * 0.5f, color);
        }
    }
    for (i = 0; i < NK_UI_ARC_SEGMENTS; ++i) {
        for (band = 0; band < 3u; ++band) {
            uint32_t a = base + i * 4u + band;
            uint32_t b = base + (i + 1u) * 4u + band;
            uint32_t *out = &buffer->indices[index_base + (i * 3u + band) * 6u];
            out[0] = a; out[1] = b; out[2] = b + 1u;
            out[3] = a; out[4] = b + 1u; out[5] = a + 1u;
        }
    }
    return nk_ui_finish_geometry(buffer, index_base, vertex_count, index_count, bounds,
                                 rgba_pm, element_id, NK_UI_DRAW_BORDER);
}

bool nk_ui_draw_add_border_corners(NkUiDrawBuffer *buffer, NkUiRect rect, float density,
                                   float width, const float radius[4], uint32_t rgba,
                                   uint32_t element_id) {
    NkUiPixelRect px;
    int thickness;
    int max_thickness;
    int r[4];
    int ext[4];
    int corner;
    bool rounded = false;
    uint32_t rgba_pm;
    NkUiPixelRect strips[4];
    if (buffer == NULL || radius == NULL || density <= 0.0f || !isfinite(density) ||
        !isfinite(width) || width <= 0.0f) {
        return false;
    }
    px = nk_ui_rect_to_pixels(rect, density);
    if (px.x1 <= px.x0 || px.y1 <= px.y0) {
        return true;
    }
    for (corner = 0; corner < 4; ++corner) {
        r[corner] = nk_ui_radius_px(px, radius[corner], density);
        rounded = rounded || r[corner] > 0;
    }
    if (!rounded) {
        return nk_ui_draw_add_border(buffer, rect, density, width, rgba, element_id);
    }
    max_thickness = (px.x1 - px.x0 < px.y1 - px.y0 ? px.x1 - px.x0 : px.y1 - px.y0) / 2;
    if (max_thickness < 1) max_thickness = 1;
    thickness = (int)nk_ui_round_float(width * density);
    if (thickness < 1) thickness = 1;
    if (thickness > max_thickness) thickness = max_thickness;
    rgba_pm = nk_ui_premultiply(rgba);
    for (corner = 0; corner < 4; ++corner) {
        if (r[corner] > 0 && r[corner] < thickness) r[corner] = thickness;
        ext[corner] = r[corner] > 0 ? r[corner] : thickness;
    }
    /* Straight runs between the corners, then the corners themselves. */
    strips[0] = (NkUiPixelRect){px.x0 + ext[0], px.y0, px.x1 - ext[1], px.y0 + thickness};
    strips[1] = (NkUiPixelRect){px.x1 - thickness, px.y0 + ext[1], px.x1, px.y1 - ext[2]};
    strips[2] = (NkUiPixelRect){px.x0 + ext[3], px.y1 - thickness, px.x1 - ext[2], px.y1};
    strips[3] = (NkUiPixelRect){px.x0, px.y0 + ext[0], px.x0 + thickness, px.y1 - ext[3]};
    for (corner = 0; corner < 4; ++corner) {
        if (!nk_ui_draw_add_pixel_rect(buffer, strips[corner], rgba_pm, element_id,
                                       NK_UI_DRAW_BORDER)) {
            return false;
        }
    }
    for (corner = 0; corner < 4; ++corner) {
        if (r[corner] > 0) {
            if (!nk_ui_draw_add_corner_ring(buffer, px, corner, r[corner], thickness,
                                            rgba_pm, element_id)) {
                return false;
            }
        } else {
            NkUiPixelRect square;
            square.x0 = corner == 0 || corner == 3 ? px.x0 : px.x1 - thickness;
            square.x1 = square.x0 + thickness;
            square.y0 = corner == 0 || corner == 1 ? px.y0 : px.y1 - thickness;
            square.y1 = square.y0 + thickness;
            if (!nk_ui_draw_add_pixel_rect(buffer, square, rgba_pm, element_id,
                                           NK_UI_DRAW_BORDER)) {
                return false;
            }
        }
    }
    return true;
}

bool nk_ui_draw_add_convex(NkUiDrawBuffer *buffer, const float *xy, uint32_t count,
                           uint32_t rgba, uint32_t element_id) {
    const uint32_t vertex_count = count * 2u;
    const uint32_t index_count = (count - 2u) * 3u + count * 6u;
    float area = 0.0f;
    float orient;
    uint32_t base;
    uint32_t index_base;
    uint32_t i;
    uint32_t rgba_pm;
    NkUiPixelRect bounds = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    if (buffer == NULL || xy == NULL || count < 3u || count > 256u) {
        return false;
    }
    for (i = 0; i < count; ++i) {
        const float *p = &xy[i * 2u];
        const float *q = &xy[((i + 1u) % count) * 2u];
        if (!isfinite(p[0]) || !isfinite(p[1])) return false;
        area += p[0] * q[1] - q[0] * p[1];
    }
    if (fabsf(area) < 0.5f) {
        return true; /* degenerate: nothing visible */
    }
    orient = area > 0.0f ? 1.0f : -1.0f;
    if (buffer->list.nv > UINT32_MAX - vertex_count ||
        buffer->list.ni > UINT32_MAX - index_count ||
        !nk_ui_reserve_vertices(buffer, buffer->list.nv + vertex_count) ||
        !nk_ui_reserve_indices(buffer, buffer->list.ni + index_count) ||
        !nk_ui_reserve_commands(buffer, buffer->list.ncmd + 1u)) {
        return false;
    }
    rgba_pm = nk_ui_premultiply(rgba);
    base = buffer->list.nv;
    index_base = buffer->list.ni;
    for (i = 0; i < count; ++i) {
        const float *prev = &xy[((i + count - 1u) % count) * 2u];
        const float *cur = &xy[i * 2u];
        const float *next = &xy[((i + 1u) % count) * 2u];
        float e0x = cur[0] - prev[0], e0y = cur[1] - prev[1];
        float e1x = next[0] - cur[0], e1y = next[1] - cur[1];
        float l0 = sqrtf(e0x * e0x + e0y * e0y);
        float l1 = sqrtf(e1x * e1x + e1y * e1y);
        float nx = 0.0f;
        float ny = 0.0f;
        float nl;
        float x = cur[0];
        float y = cur[1];
        /* Outward normals of both edges (y grows down), averaged. */
        if (l0 > 0.0f) { nx += orient * e0y / l0; ny -= orient * e0x / l0; }
        if (l1 > 0.0f) { nx += orient * e1y / l1; ny -= orient * e1x / l1; }
        nl = sqrtf(nx * nx + ny * ny);
        if (nl > 0.0f) { nx /= nl; ny /= nl; }
        /* Solid to the edge and clear one pixel outside it: neighbouring faces
           (the showcase cube) overlap by their fringe instead of leaving a seam. */
        buffer->vertices[base + i] = nk_ui_vertex(x, y, rgba_pm);
        buffer->vertices[base + count + i] = nk_ui_vertex(x + nx * 0.75f, y + ny * 0.75f, 0u);
        if ((int)x < bounds.x0) bounds.x0 = (int)x;
        if ((int)y < bounds.y0) bounds.y0 = (int)y;
        if ((int)x > bounds.x1) bounds.x1 = (int)x;
        if ((int)y > bounds.y1) bounds.y1 = (int)y;
    }
    for (i = 0; i + 2u < count; ++i) {
        buffer->indices[index_base + i * 3u + 0u] = base;
        buffer->indices[index_base + i * 3u + 1u] = base + i + 1u;
        buffer->indices[index_base + i * 3u + 2u] = base + i + 2u;
    }
    for (i = 0; i < count; ++i) {
        uint32_t *out = &buffer->indices[index_base + (count - 2u) * 3u + i * 6u];
        uint32_t j = (i + 1u) % count;
        out[0] = base + i; out[1] = base + j; out[2] = base + count + j;
        out[3] = base + i; out[4] = base + count + j; out[5] = base + count + i;
    }
    return nk_ui_finish_geometry(buffer, index_base, vertex_count, index_count, bounds,
                                 rgba_pm, element_id, NK_UI_DRAW_RECTANGLE);
}

bool nk_ui_draw_add_text(NkUiDrawBuffer *buffer, NkUiPixelRect bounds,
                         const char *text, size_t length, int font,
                         int size_px, uint32_t rgba, uint32_t element_id) {
    return nk_ui_draw_add_text_spaced(buffer, bounds, text, length, font, size_px,
                                      0.0f, rgba, element_id);
}

bool nk_ui_draw_add_text_spaced(NkUiDrawBuffer *buffer, NkUiPixelRect bounds,
                                const char *text, size_t length, int font,
                                int size_px, float letter_spacing_px, uint32_t rgba,
                                uint32_t element_id) {
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
    run->letter_spacing = isfinite(letter_spacing_px) && letter_spacing_px > 0.0f
        ? letter_spacing_px : 0.0f;
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

bool nk_ui_draw_add_text_centered(NkUiDrawBuffer *buffer, NkUiPixelRect bounds,
                                  const char *text, size_t length, int font,
                                  int size_px, uint32_t rgba, uint32_t element_id) {
    if (!nk_ui_draw_add_text_spaced(buffer, bounds, text, length, font, size_px, 0.0f,
                                    rgba, element_id)) {
        return false;
    }
    buffer->text_runs[buffer->text_run_count - 1u]->center = true;
    return true;
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
