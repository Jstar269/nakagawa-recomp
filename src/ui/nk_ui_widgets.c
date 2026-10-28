/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The player's component set, drawn from the design-system tokens. Layout is
   plain Clay rows and columns; the focus ring is drawn by the core outside the
   focused element, so a control never changes size when focus moves. */

#include "nk_ui_widgets.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void nk_ui_button_configuration(const NkUi *ui, uint32_t flags,
                                NkUiLayout *layout, NkUiStyle *style);

static NkUiSizing nk_ui_fixed(float value) {
    NkUiSizing sizing = {NK_UI_SIZE_FIXED, value, 0.0f, 0.0f};
    return sizing;
}

static NkUiSizing nk_ui_fit(void) {
    NkUiSizing sizing = {NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
    return sizing;
}

static NkUiSizing nk_ui_grow(void) {
    NkUiSizing sizing = {NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    return sizing;
}

static NkUiLayout nk_ui_row(NkUiSizing width, NkUiSizing height, float gap) {
    NkUiLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.width = width;
    layout.height = height;
    layout.direction = NK_UI_LAYOUT_ROW;
    layout.gap = gap;
    layout.align_y = NK_UI_ALIGN_CENTER;
    return layout;
}

static NkUiLayout nk_ui_column(NkUiSizing width, NkUiSizing height, float gap) {
    NkUiLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.width = width;
    layout.height = height;
    layout.direction = NK_UI_LAYOUT_COLUMN;
    layout.gap = gap;
    return layout;
}

static void nk_ui_pad(NkUiLayout *layout, float top, float right, float bottom, float left) {
    layout->padding_top = top;
    layout->padding_right = right;
    layout->padding_bottom = bottom;
    layout->padding_left = left;
}

static NkUiStyle nk_ui_fill(uint32_t fill, uint32_t border, float radius) {
    NkUiStyle style;
    memset(&style, 0, sizeof(style));
    style.fill_rgba = fill;
    style.border_rgba = border;
    style.border_width = border != 0u ? NK_UI_BORDER_HAIRLINE : 0.0f;
    style.corner_radius = radius;
    return style;
}

NkUiId nk_ui_child_id(NkUiId id, uint32_t k) {
    uint32_t hash = (uint32_t)id * 2654435761u;
    hash ^= (k + 1u) * 0x9E3779B9u;
    hash ^= hash >> 15;
    return (NkUiId)(0x80000000u | (hash & 0x7FFFFFFFu));
}

/* ---- Text --------------------------------------------------------------------- */

const char *nk_ui_caps(NkUi *ui, const char *text) {
    char buffer[256];
    size_t i;
    if (text == NULL) {
        return "";
    }
    for (i = 0; text[i] != '\0' && i + 1u < sizeof(buffer); ++i) {
        unsigned char c = (unsigned char)text[i];
        buffer[i] = c < 0x80u ? (char)toupper(c) : (char)c;
    }
    buffer[i] = '\0';
    return nk_ui_frame_text(ui, buffer);
}

void nk_ui_text_role(NkUi *ui, NkUiId id, const char *text, NkUiTypeRole role,
                     uint32_t rgba) {
    NkUiTextStyle style = nk_ui_type(role, rgba);
    nk_ui_text(ui, id, text, &style);
}

void nk_ui_eyebrow(NkUi *ui, NkUiId id, const char *text) {
    nk_ui_text_role(ui, id, nk_ui_caps(ui, text), NK_UI_TYPE_EYEBROW,
                    nk_ui_theme(ui)->muted);
}

/* ---- Buttons ---------------------------------------------------------------------- */

bool nk_ui_button(NkUi *ui, NkUiId id, const char *label, uint32_t flags) {
    NkUiLayout layout;
    NkUiStyle style;
    NkUiTextStyle text;
    const NkUiTheme *theme;
    bool focused;
    bool activated;
    if (ui == NULL || label == NULL) {
        return false;
    }
    theme = nk_ui_theme(ui);
    nk_ui_register_button(ui, id, flags);
    nk_ui_button_configuration(ui, flags, &layout, &style);
    focused = nk_ui_button_focused(ui, id);
    if (focused && (flags & (NK_UI_BUTTON_PRIMARY | NK_UI_BUTTON_DISABLED)) == 0u) {
        style.fill_rgba = theme->surface_high;
    }
    if (!nk_ui_begin_container(ui, id, &layout, &style)) {
        return false;
    }
    if (focused) {
        nk_ui_focus_ring(ui, id, NK_UI_RADIUS_CONTROL);
    }
    activated = (flags & NK_UI_BUTTON_DISABLED) == 0u && nk_ui_take_activation(ui, id);
    text = nk_ui_type((flags & NK_UI_BUTTON_LARGE) != 0u ? NK_UI_TYPE_LABEL_L :
                      (flags & NK_UI_BUTTON_SMALL) != 0u ? NK_UI_TYPE_LABEL_S :
                      NK_UI_TYPE_LABEL,
                      nk_ui_button_text_rgba(ui, flags));
    nk_ui_text(ui, id, nk_ui_caps(ui, label), &text);
    nk_ui_end_container(ui);
    return activated;
}

/* ---- Containers --------------------------------------------------------------------- */

bool nk_ui_card_begin(NkUi *ui, NkUiId id, NkUiSizing width, NkUiSizing height,
                      float gap) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout layout = nk_ui_column(width, height, gap);
    NkUiStyle style = nk_ui_fill(theme->surface_raised, theme->line, NK_UI_RADIUS_TILE);
    nk_ui_pad(&layout, NK_UI_SPACE_4, NK_UI_SPACE_5, NK_UI_SPACE_4 + 2.0f, NK_UI_SPACE_5);
    return nk_ui_begin_container(ui, id, &layout, &style);
}

void nk_ui_divider(NkUi *ui, NkUiId id, bool horizontal) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout layout = horizontal ? nk_ui_row(nk_ui_grow(), nk_ui_fixed(1.0f), 0.0f)
                                   : nk_ui_row(nk_ui_fixed(1.0f), nk_ui_grow(), 0.0f);
    NkUiStyle style = nk_ui_fill(theme->line, 0u, 0.0f);
    if (nk_ui_begin_container(ui, id, &layout, &style)) {
        nk_ui_end_container(ui);
    }
}

/* ---- Chips ------------------------------------------------------------------------- */

void nk_ui_status_pill(NkUi *ui, NkUiId id, const char *marker, const char *word,
                       uint32_t rgba) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout layout = nk_ui_row(nk_ui_fit(), nk_ui_fixed(NK_UI_CHIP_HEIGHT), 6.0f);
    NkUiStyle style = nk_ui_fill(theme->surface_high, theme->line_strong, NK_UI_RADIUS_CHIP);
    nk_ui_pad(&layout, 0.0f, 10.0f, 0.0f, 10.0f);
    if (!nk_ui_begin_container(ui, id, &layout, &style)) {
        return;
    }
    if (marker != NULL && marker[0] != '\0') {
        nk_ui_text_role(ui, nk_ui_child_id(id, 1u), marker, NK_UI_TYPE_LABEL_S, rgba);
    }
    nk_ui_text_role(ui, nk_ui_child_id(id, 2u), nk_ui_caps(ui, word), NK_UI_TYPE_LABEL_S,
                    rgba);
    nk_ui_end_container(ui);
}

void nk_ui_tag(NkUi *ui, NkUiId id, const char *text) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout layout = nk_ui_row(nk_ui_fit(), nk_ui_fixed(NK_UI_CHIP_HEIGHT), 0.0f);
    NkUiStyle style = nk_ui_fill(theme->surface_high, theme->line_strong, NK_UI_RADIUS_CHIP);
    nk_ui_pad(&layout, 0.0f, 10.0f, 0.0f, 10.0f);
    if (nk_ui_begin_container(ui, id, &layout, &style)) {
        nk_ui_text_role(ui, nk_ui_child_id(id, 1u), nk_ui_caps(ui, text),
                        NK_UI_TYPE_LABEL_S, theme->muted);
        nk_ui_end_container(ui);
    }
}

void nk_ui_badge(NkUi *ui, NkUiId id, const char *text) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout layout = nk_ui_row(nk_ui_fit(), nk_ui_fixed(NK_UI_BADGE_HEIGHT), 0.0f);
    NkUiStyle style = nk_ui_fill(theme->art_scrim, 0u, NK_UI_RADIUS_CHIP);
    nk_ui_pad(&layout, 0.0f, 7.0f, 0.0f, 7.0f);
    if (nk_ui_begin_container(ui, id, &layout, &style)) {
        nk_ui_text_role(ui, nk_ui_child_id(id, 1u), nk_ui_caps(ui, text), NK_UI_TYPE_MICRO,
                        theme->paper);
        nk_ui_end_container(ui);
    }
}

/* ---- Art --------------------------------------------------------------------------- */

#define NK_UI_ACCENT_RGBA 0xD7F24AFFu
#define NK_UI_PAPER_RGBA 0xF5F7FAFFu

static const NkUiShape g_n1_mark[] = {
    {NK_UI_SHAPE_RECT, NK_UI_ACCENT_RGBA, 0.0f, 0u, {34.0f, 34.0f, 46.0f, 62.8f}},
    {NK_UI_SHAPE_RECT, NK_UI_ACCENT_RGBA, 0.0f, 0u, {34.0f, 104.0f, 46.0f, 29.2f}},
    {NK_UI_SHAPE_RECT, NK_UI_ACCENT_RGBA, 0.0f, 0u, {34.0f, 140.4f, 46.0f, 29.2f}},
    {NK_UI_SHAPE_RECT, NK_UI_ACCENT_RGBA, 0.0f, 0u, {34.0f, 176.8f, 46.0f, 29.2f}},
    {NK_UI_SHAPE_POLY, NK_UI_PAPER_RGBA, 0.0f, 5u,
     {34.0f, 34.0f, 88.0f, 34.0f, 206.0f, 195.1f, 206.0f, 206.0f, 160.0f, 206.0f}},
    {NK_UI_SHAPE_RECT, NK_UI_PAPER_RGBA, 0.0f, 0u, {160.0f, 34.0f, 46.0f, 172.0f}}
};

static const NkUiShapeArt g_n1_mark_art = {
    34.0f, 34.0f, 172.0f, 172.0f, g_n1_mark, sizeof(g_n1_mark) / sizeof(g_n1_mark[0])
};

/* The 3D showcase demo's lit cube, as an isometric block. */
static const NkUiShape g_showcase_scene[] = {
    {NK_UI_SHAPE_POLY, NK_UI_ACCENT_RGBA, 0.0f, 4u,
     {60.0f, 14.0f, 104.0f, 38.0f, 60.0f, 62.0f, 16.0f, 38.0f}},
    {NK_UI_SHAPE_POLY, 0x3DD6C6FFu, 0.0f, 4u,
     {16.0f, 38.0f, 60.0f, 62.0f, 60.0f, 108.0f, 16.0f, 84.0f}},
    {NK_UI_SHAPE_POLY, 0x8B7CFFFFu, 0.0f, 4u,
     {104.0f, 38.0f, 60.0f, 62.0f, 60.0f, 108.0f, 104.0f, 84.0f}}
};

#define NK_UI_BRICK(x, y, c) {NK_UI_SHAPE_RECT, c, 0.0f, 0u, {x, y, 40.0f, 12.0f}}
/* The Breakout showcase demo: three brick rows, the ball and the paddle. */
static const NkUiShape g_showcase_breakout[] = {
    NK_UI_BRICK(10.0f, 10.0f, 0xFF5A5FFFu), NK_UI_BRICK(55.0f, 10.0f, 0xFF5A5FFFu),
    NK_UI_BRICK(100.0f, 10.0f, 0xFF5A5FFFu), NK_UI_BRICK(145.0f, 10.0f, 0xFF5A5FFFu),
    NK_UI_BRICK(10.0f, 26.0f, NK_UI_ACCENT_RGBA), NK_UI_BRICK(55.0f, 26.0f, NK_UI_ACCENT_RGBA),
    NK_UI_BRICK(100.0f, 26.0f, NK_UI_ACCENT_RGBA), NK_UI_BRICK(145.0f, 26.0f, NK_UI_ACCENT_RGBA),
    NK_UI_BRICK(10.0f, 42.0f, 0x3DD6C6FFu), NK_UI_BRICK(55.0f, 42.0f, 0x3DD6C6FFu),
    NK_UI_BRICK(145.0f, 42.0f, 0x3DD6C6FFu),
    {NK_UI_SHAPE_CIRCLE, NK_UI_PAPER_RGBA, 0.0f, 0u, {122.0f, 78.0f, 5.0f}},
    {NK_UI_SHAPE_RECT, NK_UI_PAPER_RGBA, 3.5f, 0u, {88.0f, 104.0f, 48.0f, 7.0f}}
};
#undef NK_UI_BRICK

static const NkUiShapeArt g_showcase_scene_art = {
    0.0f, 0.0f, 120.0f, 120.0f, g_showcase_scene,
    sizeof(g_showcase_scene) / sizeof(g_showcase_scene[0])
};

static const NkUiShapeArt g_showcase_breakout_art = {
    0.0f, 0.0f, 200.0f, 120.0f, g_showcase_breakout,
    sizeof(g_showcase_breakout) / sizeof(g_showcase_breakout[0])
};

/* The add-a-disc plus sign, drawn inside a 44 px outlined square. */
static const NkUiShape g_plus[] = {
    {NK_UI_SHAPE_RECT, NK_UI_ACCENT_RGBA, 1.5f, 0u, {22.5f, 12.0f, 3.0f, 24.0f}},
    {NK_UI_SHAPE_RECT, NK_UI_ACCENT_RGBA, 1.5f, 0u, {12.0f, 22.5f, 24.0f, 3.0f}}
};

static const NkUiShapeArt g_plus_art = {
    0.0f, 0.0f, 48.0f, 48.0f, g_plus, sizeof(g_plus) / sizeof(g_plus[0])
};

const NkUiShapeArt *nk_ui_art_n1_mark(void) {
    return &g_n1_mark_art;
}

/* ---- App icon ---------------------------------------------------------------------- */
/* Keep these in step with TILE_RGB, CORNER_FRACTION, INSET_FRACTION and CORNER_SAMPLES in
   tools/make_app_icon.py; tools/test_app_icon.py compares them. */
#define NK_UI_APP_ICON_TILE_RGB 0x10151Cu
#define NK_UI_APP_ICON_CORNER 0.18
#define NK_UI_APP_ICON_INSET 0.16
#define NK_UI_APP_ICON_CORNER_SAMPLES 16

typedef struct {
    double x;
    double y;
} NkUiIconPoint;

/* Each clip against one pixel edge adds at most one vertex. */
#define NK_UI_ICON_CLIP_POINTS (NK_UI_SHAPE_MAX_POINTS + 4u)

static unsigned nk_ui_icon_clip(const NkUiIconPoint *in, unsigned count, int axis,
                                double bound, int keep_above, NkUiIconPoint *out) {
    unsigned n = 0u;
    for (unsigned i = 0u; i < count; ++i) {
        NkUiIconPoint a = in[i];
        NkUiIconPoint b = in[(i + 1u) % count];
        double av = axis == 0 ? a.x : a.y;
        double bv = axis == 0 ? b.x : b.y;
        int a_in = keep_above ? av >= bound : av <= bound;
        int b_in = keep_above ? bv >= bound : bv <= bound;
        if (a_in) out[n++] = a;
        if (a_in != b_in) {
            double t = (bound - av) / (bv - av);
            NkUiIconPoint p = {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)};
            out[n++] = p;
        }
    }
    return n;
}

/* Exact area of the convex polygon inside pixel (px, py). */
static double nk_ui_icon_coverage(const NkUiIconPoint *poly, unsigned count, int px, int py) {
    NkUiIconPoint a[NK_UI_ICON_CLIP_POINTS];
    NkUiIconPoint b[NK_UI_ICON_CLIP_POINTS];
    unsigned n = nk_ui_icon_clip(poly, count, 0, (double)px, 1, a);
    if (n != 0u) n = nk_ui_icon_clip(a, n, 0, (double)(px + 1), 0, b);
    if (n != 0u) n = nk_ui_icon_clip(b, n, 1, (double)py, 1, a);
    if (n != 0u) n = nk_ui_icon_clip(a, n, 1, (double)(py + 1), 0, b);
    if (n < 3u) return 0.0;
    double total = 0.0;
    for (unsigned i = 0u; i < n; ++i) {
        NkUiIconPoint p = b[i];
        NkUiIconPoint q = b[(i + 1u) % n];
        total += p.x * q.y - q.x * p.y;
    }
    return (total < 0.0 ? -total : total) * 0.5;
}

static double nk_ui_icon_tile_coverage(int size, int px, int py) {
    double radius = size * NK_UI_APP_ICON_CORNER;
    int near_x = px < radius || px + 1 > size - radius;
    int near_y = py < radius || py + 1 > size - radius;
    if (!(near_x && near_y)) return 1.0;
    double cx = px < radius ? radius : size - radius;
    double cy = py < radius ? radius : size - radius;
    int inside = 0;
    for (int sy = 0; sy < NK_UI_APP_ICON_CORNER_SAMPLES; ++sy) {
        double y = py + (sy + 0.5) / NK_UI_APP_ICON_CORNER_SAMPLES;
        for (int sx = 0; sx < NK_UI_APP_ICON_CORNER_SAMPLES; ++sx) {
            double x = px + (sx + 0.5) / NK_UI_APP_ICON_CORNER_SAMPLES;
            double dx = (x < radius || x > size - radius) ? x - cx : 0.0;
            double dy = (y < radius || y > size - radius) ? y - cy : 0.0;
            if (dx * dx + dy * dy <= radius * radius) ++inside;
        }
    }
    return (double)inside / (NK_UI_APP_ICON_CORNER_SAMPLES * NK_UI_APP_ICON_CORNER_SAMPLES);
}

static unsigned char nk_ui_icon_byte(double value) {
    int v = (int)(value * 255.0 + 0.5);
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

bool nk_ui_app_icon_rgba(int size, unsigned char *rgba, size_t stride) {
    if (rgba == NULL || size < NK_UI_APP_ICON_MIN || size > NK_UI_APP_ICON_MAX ||
        stride < (size_t)size * 4u) {
        return false;
    }
    const NkUiShapeArt *art = &g_n1_mark_art;
    NkUiIconPoint polys[sizeof(g_n1_mark) / sizeof(g_n1_mark[0])][NK_UI_SHAPE_MAX_POINTS];
    unsigned counts[sizeof(g_n1_mark) / sizeof(g_n1_mark[0])];
    double inset = size * NK_UI_APP_ICON_INSET;
    double extent = art->view_w > art->view_h ? art->view_w : art->view_h;
    double scale = (size - 2.0 * inset) / extent;
    for (size_t s = 0u; s < art->count; ++s) {
        const NkUiShape *shape = &art->shapes[s];
        NkUiIconPoint *poly = polys[s];
        if (shape->kind == NK_UI_SHAPE_RECT) {
            double x = shape->v[0], y = shape->v[1];
            double w = shape->v[2], h = shape->v[3];
            NkUiIconPoint corners[4] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
            memcpy(poly, corners, sizeof(corners));
            counts[s] = 4u;
        } else {
            for (unsigned i = 0u; i < shape->count; ++i) {
                poly[i].x = shape->v[2u * i];
                poly[i].y = shape->v[2u * i + 1u];
            }
            counts[s] = shape->count;
        }
        for (unsigned i = 0u; i < counts[s]; ++i) {
            poly[i].x = inset + (poly[i].x - art->view_x) * scale;
            poly[i].y = inset + (poly[i].y - art->view_y) * scale;
        }
    }
    const double tile[3] = {((NK_UI_APP_ICON_TILE_RGB >> 16) & 0xFFu) / 255.0,
                            ((NK_UI_APP_ICON_TILE_RGB >> 8) & 0xFFu) / 255.0,
                            (NK_UI_APP_ICON_TILE_RGB & 0xFFu) / 255.0};
    for (int py = 0; py < size; ++py) {
        unsigned char *row = rgba + (size_t)py * stride;
        for (int px = 0; px < size; ++px) {
            double alpha = nk_ui_icon_tile_coverage(size, px, py);
            double c[3] = {tile[0] * alpha, tile[1] * alpha, tile[2] * alpha};
            for (size_t s = 0u; s < art->count; ++s) {
                double cover = nk_ui_icon_coverage(polys[s], counts[s], px, py);
                if (cover <= 0.0) continue;
                uint32_t fill = art->shapes[s].rgba;
                for (int k = 0; k < 3; ++k) {
                    double channel = ((fill >> (24 - 8 * k)) & 0xFFu) / 255.0;
                    c[k] = channel * cover + c[k] * (1.0 - cover);
                }
                alpha = cover + alpha * (1.0 - cover);
            }
            for (int k = 0; k < 3; ++k) {
                row[px * 4 + k] = nk_ui_icon_byte(alpha > 0.0 ? c[k] / alpha : c[k]);
            }
            row[px * 4 + 3] = nk_ui_icon_byte(alpha);
        }
    }
    return true;
}

const NkUiShapeArt *nk_ui_art_picture(NkUiArtKind kind) {
    switch (kind) {
    case NK_UI_ART_SHOWCASE_SCENE: return &g_showcase_scene_art;
    case NK_UI_ART_SHOWCASE_BREAKOUT: return &g_showcase_breakout_art;
    case NK_UI_ART_NONE:
    default: return NULL;
    }
}

uint32_t nk_ui_art_tint(const char *key) {
    static const uint32_t tints[] = {0x161C24FFu, 0x16324AFFu, 0x1A1F2EFFu, 0x12202AFFu};
    uint32_t hash = 2166136261u;
    if (key == NULL) {
        return tints[0];
    }
    while (*key != '\0') {
        hash ^= (unsigned char)*key++;
        hash *= 16777619u;
    }
    return tints[hash % (sizeof(tints) / sizeof(tints[0]))];
}

static uint32_t nk_ui_art_fill(NkUiArtKind art, const char *key) {
    if (art == NK_UI_ART_SHOWCASE_SCENE) return 0x0E1A2BFFu;
    if (art == NK_UI_ART_SHOWCASE_BREAKOUT) return 0x14101FFFu;
    return nk_ui_art_tint(key);
}

/* The art itself, centred in a growing area: the disc's own picture or icon
   when the host read them, else the project picture at `scale` of the area's
   height, else a labelled placeholder box. */
static void nk_ui_art_content(NkUi *ui, NkUiId id, NkUiArtKind art, const NkUiImage *icon,
                              const NkUiImage *disc_picture, float area_h, float scale,
                              const char *placeholder) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const NkUiShapeArt *picture = nk_ui_art_picture(art);
    NkUiLayout layout = nk_ui_row(nk_ui_grow(), nk_ui_grow(), 0.0f);
    layout.align_x = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, id, &layout, NULL)) {
        return;
    }
    if ((icon != NULL && icon->rgba_pm != NULL) ||
        (disc_picture != NULL && disc_picture->rgba_pm != NULL)) {
        NkUiImageArt image;
        image.cover = disc_picture != NULL && disc_picture->rgba_pm != NULL ? disc_picture : NULL;
        image.contain = image.cover == NULL ? icon : NULL;
        image.contain_scale = scale;
        nk_ui_image_art(ui, nk_ui_child_id(id, 4u), nk_ui_grow(), nk_ui_grow(), &image);
    } else if (picture != NULL) {
        float h = area_h * scale;
        float w = h * picture->view_w / picture->view_h;
        nk_ui_shapes(ui, nk_ui_child_id(id, 1u), nk_ui_fixed(w), nk_ui_fixed(h), picture);
    } else {
        NkUiLayout box = nk_ui_row(nk_ui_fit(), nk_ui_fixed(54.0f), 0.0f);
        NkUiStyle style = nk_ui_fill(0u, theme->placeholder, 6.0f);
        nk_ui_pad(&box, 0.0f, 14.0f, 0.0f, 14.0f);
        if (nk_ui_begin_container(ui, nk_ui_child_id(id, 2u), &box, &style)) {
            nk_ui_text_role(ui, nk_ui_child_id(id, 3u), placeholder, NK_UI_TYPE_MICRO,
                            theme->dim);
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

/* ---- Game card ------------------------------------------------------------------- */

bool nk_ui_tile(NkUi *ui, NkUiId id, const NkUiTileSpec *spec) {
    const NkUiTheme *theme;
    NkUiLayout layout;
    NkUiStyle style;
    bool focused;
    bool activated;
    float art_h;
    char meta[96];
    if (ui == NULL || spec == NULL) {
        return false;
    }
    theme = nk_ui_theme(ui);
    nk_ui_register_button(ui, id, 0u);
    focused = nk_ui_button_focused(ui, id);
    art_h = (float)(int)(spec->width * NK_UI_TILE_ART_RATIO + 0.5f);
    layout = nk_ui_column(nk_ui_fixed(spec->width), nk_ui_fit(), 0.0f);
    style = nk_ui_fill(theme->surface_raised, focused ? theme->line_strong : theme->line,
                       NK_UI_RADIUS_TILE);
    if (!nk_ui_begin_container(ui, id, &layout, &style)) {
        return false;
    }
    if (focused) {
        nk_ui_focus_ring(ui, id, NK_UI_RADIUS_TILE);
    }
    activated = nk_ui_take_activation(ui, id);

    /* Art panel: flush with the card's top edge, square at the bottom. */
    layout = nk_ui_column(nk_ui_grow(), nk_ui_fixed(art_h), 0.0f);
    nk_ui_pad(&layout, NK_UI_SPACE_2, NK_UI_SPACE_2, NK_UI_SPACE_2, NK_UI_SPACE_2);
    style = nk_ui_fill(nk_ui_art_fill(spec->art, spec->art_key != NULL ? spec->art_key
                                                                       : spec->disc_id),
                       0u, NK_UI_RADIUS_TILE);
    style.square_corners = NK_UI_CORNER_SQUARE_BL | NK_UI_CORNER_SQUARE_BR;
    if (nk_ui_begin_container(ui, nk_ui_child_id(id, 1u), &layout, &style)) {
        nk_ui_badge(ui, nk_ui_child_id(id, 2u),
                    spec->badge != NULL && spec->badge[0] != '\0' ? spec->badge : "Your disc");
        nk_ui_art_content(ui, nk_ui_child_id(id, 3u), spec->art, spec->icon, NULL,
                          art_h - NK_UI_SPACE_2 * 2.0f - NK_UI_BADGE_HEIGHT, 0.82f,
                          "ART FROM YOUR DISC");
        nk_ui_end_container(ui);
    }

    /* Body: the name (two lines at most) and the meta row. */
    layout = nk_ui_column(nk_ui_grow(), nk_ui_fit(), NK_UI_SPACE_2);
    nk_ui_pad(&layout, NK_UI_SPACE_3, 14.0f, 14.0f, 14.0f);
    if (nk_ui_begin_container(ui, nk_ui_child_id(id, 4u), &layout, NULL)) {
        NkUiLayout name = nk_ui_column(nk_ui_grow(), nk_ui_fixed(40.0f), 0.0f);
        if (nk_ui_begin_container(ui, nk_ui_child_id(id, 5u), &name, NULL)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_TILE_TITLE,
                                            spec->muted_title ? theme->muted : theme->paper);
            const char *title = spec->title != NULL && spec->title[0] != '\0'
                ? spec->title : "Untitled PSP game";
            nk_ui_text(ui, nk_ui_child_id(id, 6u),
                       nk_ui_text_clamp(ui, title, &text, spec->width - 28.0f, 2), &text);
            nk_ui_end_container(ui);
        }
        layout = nk_ui_row(nk_ui_grow(), nk_ui_fixed(16.0f), NK_UI_SPACE_2);
        if (nk_ui_begin_container(ui, nk_ui_child_id(id, 7u), &layout, NULL)) {
            const char *word = nk_ui_caps(ui, spec->status_word);
            if (spec->status_marker != NULL && spec->status_marker[0] != '\0') {
                (void)snprintf(meta, sizeof(meta), "%s %s", spec->status_marker, word);
            } else {
                (void)snprintf(meta, sizeof(meta), "%s", word);
            }
            nk_ui_text_role(ui, nk_ui_child_id(id, 8u), meta, NK_UI_TYPE_LABEL_S,
                            spec->status_rgba != 0u ? spec->status_rgba : theme->muted);
            nk_ui_spacer(ui, nk_ui_child_id(id, 9u), nk_ui_grow(), nk_ui_fixed(1.0f));
            if (spec->disc_id != NULL && spec->disc_id[0] != '\0') {
                nk_ui_text_role(ui, nk_ui_child_id(id, 10u), spec->disc_id,
                                NK_UI_TYPE_LABEL_S, theme->dim);
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
    return activated;
}

bool nk_ui_add_tile(NkUi *ui, NkUiId id, const char *title, const char *detail,
                    float width, float height) {
    const NkUiTheme *theme;
    NkUiLayout layout;
    NkUiStyle style;
    bool focused;
    bool activated;
    if (ui == NULL) {
        return false;
    }
    theme = nk_ui_theme(ui);
    nk_ui_register_button(ui, id, 0u);
    focused = nk_ui_button_focused(ui, id);
    layout = nk_ui_column(nk_ui_fixed(width), nk_ui_fixed(height), NK_UI_SPACE_3);
    layout.align_x = NK_UI_ALIGN_CENTER;
    layout.align_y = NK_UI_ALIGN_CENTER;
    nk_ui_pad(&layout, NK_UI_SPACE_5, NK_UI_SPACE_5, NK_UI_SPACE_5, NK_UI_SPACE_5);
    style = nk_ui_fill(focused ? theme->surface_raised : theme->surface, theme->line_strong,
                       NK_UI_RADIUS_TILE);
    if (!nk_ui_begin_container(ui, id, &layout, &style)) {
        return false;
    }
    if (focused) {
        nk_ui_focus_ring(ui, id, NK_UI_RADIUS_TILE);
    }
    activated = nk_ui_take_activation(ui, id);
    {
        NkUiLayout icon = nk_ui_row(nk_ui_fixed(44.0f), nk_ui_fixed(44.0f), 0.0f);
        NkUiStyle outline = nk_ui_fill(0u, theme->placeholder, NK_UI_RADIUS_TILE);
        outline.border_width = 2.0f;
        if (nk_ui_begin_container(ui, nk_ui_child_id(id, 1u), &icon, &outline)) {
            nk_ui_shapes(ui, nk_ui_child_id(id, 2u), nk_ui_grow(), nk_ui_grow(), &g_plus_art);
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiTextStyle heading = nk_ui_type(NK_UI_TYPE_TILE_TITLE, theme->paper);
        heading.wrap = false;
        nk_ui_text(ui, nk_ui_child_id(id, 3u), title != NULL ? title : "", &heading);
    }
    if (detail != NULL && detail[0] != '\0') {
        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
        text.wrap = false;
        nk_ui_text(ui, nk_ui_child_id(id, 4u), detail, &text);
    }
    nk_ui_end_container(ui);
    return activated;
}

void nk_ui_hero(NkUi *ui, NkUiId id, NkUiArtKind art, const char *art_key,
                const NkUiImage *icon, const NkUiImage *picture,
                float width, const char *caption, const char *size_note) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    float height = (float)(int)(width * NK_UI_HERO_ASPECT + 0.5f);
    NkUiLayout layout = nk_ui_column(nk_ui_fixed(width), nk_ui_fixed(height), NK_UI_SPACE_2);
    NkUiStyle style = nk_ui_fill(nk_ui_art_fill(art, art_key), 0u, NK_UI_RADIUS_TILE);
    nk_ui_pad(&layout, NK_UI_SPACE_3, NK_UI_SPACE_3, NK_UI_SPACE_3, NK_UI_SPACE_3);
    if (!nk_ui_begin_container(ui, id, &layout, &style)) {
        return;
    }
    nk_ui_art_content(ui, nk_ui_child_id(id, 1u), art, icon, picture,
                      height - NK_UI_SPACE_3 * 2.0f - NK_UI_BADGE_HEIGHT - NK_UI_SPACE_2,
                      0.78f, "ART FROM YOUR DISC SHOWS HERE");
    layout = nk_ui_row(nk_ui_grow(), nk_ui_fixed(NK_UI_BADGE_HEIGHT), NK_UI_SPACE_2);
    if (nk_ui_begin_container(ui, nk_ui_child_id(id, 2u), &layout, NULL)) {
        if (caption != NULL && caption[0] != '\0') {
            nk_ui_badge(ui, nk_ui_child_id(id, 3u), caption);
        }
        nk_ui_spacer(ui, nk_ui_child_id(id, 4u), nk_ui_grow(), nk_ui_fixed(1.0f));
        if (size_note != NULL && size_note[0] != '\0') {
            nk_ui_text_role(ui, nk_ui_child_id(id, 5u), size_note, NK_UI_TYPE_MICRO,
                            theme->art_label);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

/* ---- Bars -------------------------------------------------------------------------- */

/* The input chip names the pad the way a player would: by the family its button
   labels follow, never by the glyph set's internal name. */
const char *nk_ui_input_label(NkUiPromptStyle style) {
    switch (style) {
    case NK_UI_PROMPT_KEYCAPS: return "KEYBOARD AND MOUSE";
    case NK_UI_PROMPT_LETTERS_A_SOUTH: return "XBOX-STYLE CONTROLLER";
    case NK_UI_PROMPT_LETTERS_A_EAST: return "NINTENDO-STYLE CONTROLLER";
    case NK_UI_PROMPT_SHAPES: return "PLAYSTATION-STYLE CONTROLLER";
    case NK_UI_PROMPT_NEUTRAL:
    default: return "CONTROLLER";
    }
}

int nk_ui_top_bar(NkUi *ui, NkUiId id, const NkUiSection *sections, size_t count,
                  const char *input_label, bool input_active) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout layout = nk_ui_row(nk_ui_grow(), nk_ui_fixed(NK_UI_TOP_BAR_HEIGHT - 1.0f),
                                  NK_UI_SPACE_6);
    size_t i;
    int chosen = -1;
    int active = -1;
    nk_ui_pad(&layout, 0.0f, NK_UI_GUTTER, 0.0f, NK_UI_GUTTER);
    if (!nk_ui_begin_container(ui, id, &layout, NULL)) {
        return -1;
    }
    /* Brand: the N1 mark and the wordmark. */
    layout = nk_ui_row(nk_ui_fit(), nk_ui_fit(), NK_UI_SPACE_3);
    if (nk_ui_begin_container(ui, nk_ui_child_id(id, 1u), &layout, NULL)) {
        nk_ui_shapes(ui, nk_ui_child_id(id, 2u), nk_ui_fixed(28.0f), nk_ui_fixed(28.0f),
                     nk_ui_art_n1_mark());
        nk_ui_text_role(ui, nk_ui_child_id(id, 3u), "NAKAGAWA RECOMP", NK_UI_TYPE_WORDMARK,
                        theme->paper);
        nk_ui_end_container(ui);
    }
    /* Sections: full-height tabs; the active one carries a 3 px accent rule. */
    layout = nk_ui_row(nk_ui_grow(), nk_ui_grow(), NK_UI_SPACE_1);
    layout.align_y = NK_UI_ALIGN_START;
    if (nk_ui_begin_container(ui, nk_ui_child_id(id, 4u), &layout, NULL)) {
        for (i = 0; sections != NULL && i < count; ++i) {
            NkUiLayout tab = nk_ui_column(nk_ui_fit(), nk_ui_grow(), 0.0f);
            NkUiId tab_id = nk_ui_child_id(id, 16u + (uint32_t)i * 4u);
            /* A focusable tab takes focus on its label pill, so the ring stays
               inside the bar instead of running off the window's top edge. */
            NkUiId label_id = sections[i].id != 0u ? sections[i].id : nk_ui_child_id(tab_id, 2u);
            NkUiStyle label_style;
            bool focused = false;
            tab.align_x = NK_UI_ALIGN_CENTER;
            if (sections[i].active) active = (int)i;
            memset(&label_style, 0, sizeof(label_style));
            label_style.corner_radius = NK_UI_RADIUS_CONTROL;
            if (sections[i].id != 0u) {
                nk_ui_register_button(ui, label_id, NK_UI_BUTTON_NOT_DEFAULT);
                focused = nk_ui_button_focused(ui, label_id);
                if (focused) label_style.fill_rgba = theme->surface_high;
            }
            if (!nk_ui_begin_container(ui, tab_id, &tab, NULL)) {
                continue;
            }
            nk_ui_spacer(ui, nk_ui_child_id(tab_id, 1u), nk_ui_fixed(1.0f), nk_ui_grow());
            {
                NkUiLayout label = nk_ui_row(nk_ui_fit(), nk_ui_fixed(34.0f), 0.0f);
                NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_LABEL,
                                                sections[i].active || focused ? theme->paper
                                                                              : theme->muted);
                nk_ui_pad(&label, 0.0f, NK_UI_SPACE_3, 0.0f, NK_UI_SPACE_3);
                label.align_y = NK_UI_ALIGN_CENTER;
                text.letter_spacing = 1.2f;
                if (nk_ui_begin_container(ui, label_id, &label, &label_style)) {
                    if (focused) {
                        nk_ui_focus_ring(ui, label_id, NK_UI_RADIUS_CONTROL);
                    }
                    if (sections[i].id != 0u && nk_ui_take_activation(ui, label_id)) {
                        chosen = (int)i;
                    }
                    nk_ui_text(ui, nk_ui_child_id(tab_id, 3u),
                               nk_ui_caps(ui, sections[i].label), &text);
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_spacer(ui, nk_ui_child_id(tab_id, 4u), nk_ui_fixed(1.0f), nk_ui_grow());
            {
                NkUiLayout rule = nk_ui_row(nk_ui_grow(), nk_ui_fixed(3.0f), 0.0f);
                NkUiStyle fill = nk_ui_fill(sections[i].active ? theme->accent_dark : 0u, 0u,
                                            0.0f);
                if (nk_ui_begin_container(ui, nk_ui_child_id(tab_id, 5u), &rule, &fill)) {
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    /* Input chip. */
    if (input_label != NULL && input_label[0] != '\0') {
        NkUiLayout chip = nk_ui_row(nk_ui_fit(), nk_ui_fixed(30.0f), NK_UI_SPACE_2);
        NkUiStyle fill = nk_ui_fill(theme->surface_raised, theme->line, 15.0f);
        nk_ui_pad(&chip, 0.0f, NK_UI_SPACE_3, 0.0f, NK_UI_SPACE_3);
        if (nk_ui_begin_container(ui, nk_ui_child_id(id, 5u), &chip, &fill)) {
            NkUiLayout dot = nk_ui_row(nk_ui_fixed(8.0f), nk_ui_fixed(8.0f), 0.0f);
            NkUiStyle dot_fill = nk_ui_fill(input_active ? theme->ready : theme->dim, 0u, 4.0f);
            if (nk_ui_begin_container(ui, nk_ui_child_id(id, 6u), &dot, &dot_fill)) {
                nk_ui_end_container(ui);
            }
            nk_ui_text_role(ui, nk_ui_child_id(id, 7u), input_label, NK_UI_TYPE_LABEL_S,
                            theme->muted);
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
    nk_ui_divider(ui, nk_ui_child_id(id, 8u), true);
    /* L / R (Q / E) step through the focusable sections from the active one. */
    {
        NkUiNavDir dir = NK_UI_DIR_NONE;
        if (chosen < 0 && active >= 0 && nk_ui_take_event(ui, NK_UI_EV_TAB, &dir)) {
            int step = dir == NK_UI_DIR_LEFT ? -1 : 1;
            int next = active + step;
            while (next >= 0 && next < (int)count && sections[next].id == 0u) next += step;
            if (next >= 0 && next < (int)count) chosen = next;
        }
    }
    return chosen;
}

void nk_ui_prompt_bar(NkUi *ui, NkUiId id, const char *hint,
                      const NkUiPromptItem *items, size_t count) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout layout = nk_ui_row(nk_ui_grow(), nk_ui_fixed(NK_UI_PROMPT_BAR_HEIGHT - 1.0f),
                                  NK_UI_SPACE_5);
    NkUiStyle style = nk_ui_fill(theme->surface_raised, 0u, 0.0f);
    size_t i;
    nk_ui_divider(ui, nk_ui_child_id(id, 1u), true);
    nk_ui_pad(&layout, 0.0f, NK_UI_GUTTER, 0.0f, NK_UI_GUTTER);
    if (!nk_ui_begin_container(ui, id, &layout, &style)) {
        return;
    }
    {
        NkUiLayout hint_box = nk_ui_row(nk_ui_grow(), nk_ui_fit(), 0.0f);
        if (nk_ui_begin_container(ui, nk_ui_child_id(id, 2u), &hint_box, NULL)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
            text.wrap = false;
            if (hint != NULL && hint[0] != '\0') {
                nk_ui_text(ui, nk_ui_child_id(id, 3u), hint, &text);
            }
            nk_ui_end_container(ui);
        }
    }
    layout = nk_ui_row(nk_ui_fit(), nk_ui_fit(), 18.0f);
    if (nk_ui_begin_container(ui, nk_ui_child_id(id, 4u), &layout, NULL)) {
        for (i = 0; items != NULL && i < count; ++i) {
            NkUiId item = nk_ui_child_id(id, 16u + (uint32_t)i * 4u);
            NkUiLayout row = nk_ui_row(nk_ui_fit(), nk_ui_fit(), NK_UI_SPACE_2);
            if (!nk_ui_begin_container(ui, item, &row, NULL)) {
                continue;
            }
            nk_ui_prompt_glyph(ui, nk_ui_child_id(item, 1u), items[i].action,
                               NK_UI_GLYPH_SIZE);
            nk_ui_text_role(ui, nk_ui_child_id(item, 2u), nk_ui_caps(ui, items[i].label),
                            NK_UI_TYPE_LABEL, theme->paper);
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

/* ---- Dialogs and progress ------------------------------------------------------ */

bool nk_ui_dialog_begin(NkUi *ui, NkUiId id, float width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout layout = nk_ui_column(nk_ui_fixed(width), nk_ui_fit(), 14.0f);
    NkUiStyle style = nk_ui_fill(theme->surface_raised, theme->line_strong, NK_UI_RADIUS_DIALOG);
    nk_ui_pad(&layout, 26.0f, 28.0f, 22.0f, 28.0f);
    return nk_ui_begin_container(ui, id, &layout, &style);
}

float nk_ui_progress_sweep_offset(float travel, double phase_s) {
    /* Out and back every 2.4 s, eased at both ends, so the segment never
       jumps from the right edge to the left. */
    double cycle;
    double t;
    double tri;
    if (!(travel > 0.0f) || !(phase_s > 0.0)) return 0.0f;
    cycle = phase_s / 2.4;
    t = cycle - (double)(long long)cycle;
    tri = t < 0.5 ? t * 2.0 : (1.0 - t) * 2.0;
    return (float)(tri * tri * (3.0 - 2.0 * tri)) * travel;
}

void nk_ui_progress_bar(NkUi *ui, NkUiId id, float width, float fraction, double phase_s) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout track = nk_ui_row(nk_ui_fixed(width), nk_ui_fixed(6.0f), 0.0f);
    NkUiStyle rest = nk_ui_fill(theme->line_strong, 0u, 3.0f);
    float start = 0.0f;
    float span;
    track.align_y = NK_UI_ALIGN_START;
    if (!nk_ui_begin_container(ui, id, &track, &rest)) {
        return;
    }
    if (fraction >= 0.0f) {
        span = width * (fraction > 1.0f ? 1.0f : fraction);
    } else {
        span = width * 0.32f;
        start = nk_ui_progress_sweep_offset(width - span, phase_s);
    }
    if (start > 0.0f) {
        nk_ui_spacer(ui, nk_ui_child_id(id, 1u), nk_ui_fixed(start), nk_ui_fixed(6.0f));
    }
    if (span > 0.0f) {
        NkUiLayout fill = nk_ui_row(nk_ui_fixed(span), nk_ui_fixed(6.0f), 0.0f);
        NkUiStyle accent = nk_ui_fill(theme->accent_dark, 0u, 3.0f);
        if (nk_ui_begin_container(ui, nk_ui_child_id(id, 2u), &fill, &accent)) {
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}
