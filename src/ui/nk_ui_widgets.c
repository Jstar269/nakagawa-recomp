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

/* The art itself, centred in a growing area: the picture at `scale` of the
   area's height, or a labelled placeholder box. */
static void nk_ui_art_content(NkUi *ui, NkUiId id, NkUiArtKind art, float area_h,
                              float scale, const char *placeholder) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const NkUiShapeArt *picture = nk_ui_art_picture(art);
    NkUiLayout layout = nk_ui_row(nk_ui_grow(), nk_ui_grow(), 0.0f);
    layout.align_x = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, id, &layout, NULL)) {
        return;
    }
    if (picture != NULL) {
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
        nk_ui_art_content(ui, nk_ui_child_id(id, 3u), spec->art,
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
                float width, const char *caption, const char *size_note) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    float height = (float)(int)(width * NK_UI_HERO_ASPECT + 0.5f);
    NkUiLayout layout = nk_ui_column(nk_ui_fixed(width), nk_ui_fixed(height), NK_UI_SPACE_2);
    NkUiStyle style = nk_ui_fill(nk_ui_art_fill(art, art_key), 0u, NK_UI_RADIUS_TILE);
    nk_ui_pad(&layout, NK_UI_SPACE_3, NK_UI_SPACE_3, NK_UI_SPACE_3, NK_UI_SPACE_3);
    if (!nk_ui_begin_container(ui, id, &layout, &style)) {
        return;
    }
    nk_ui_art_content(ui, nk_ui_child_id(id, 1u), art,
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

const char *nk_ui_input_label(NkUiPromptStyle style) {
    switch (style) {
    case NK_UI_PROMPT_KEYCAPS: return "KEYBOARD AND MOUSE";
    case NK_UI_PROMPT_LETTERS_A_SOUTH: return "CONTROLLER \xC2\xB7 LETTERS, A SOUTH";
    case NK_UI_PROMPT_LETTERS_A_EAST: return "CONTROLLER \xC2\xB7 LETTERS, A EAST";
    case NK_UI_PROMPT_NEUTRAL:
    default: return "CONTROLLER \xC2\xB7 NEUTRAL PROMPTS";
    }
}

void nk_ui_top_bar(NkUi *ui, NkUiId id, const NkUiSection *sections, size_t count,
                   const char *input_label, bool input_active) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout layout = nk_ui_row(nk_ui_grow(), nk_ui_fixed(NK_UI_TOP_BAR_HEIGHT - 1.0f),
                                  NK_UI_SPACE_6);
    size_t i;
    nk_ui_pad(&layout, 0.0f, NK_UI_GUTTER, 0.0f, NK_UI_GUTTER);
    if (!nk_ui_begin_container(ui, id, &layout, NULL)) {
        return;
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
            tab.align_x = NK_UI_ALIGN_CENTER;
            if (!nk_ui_begin_container(ui, tab_id, &tab, NULL)) {
                continue;
            }
            nk_ui_spacer(ui, nk_ui_child_id(tab_id, 1u), nk_ui_fixed(1.0f), nk_ui_grow());
            {
                NkUiLayout label = nk_ui_row(nk_ui_fit(), nk_ui_fit(), 0.0f);
                NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_LABEL,
                                                sections[i].active ? theme->paper : theme->muted);
                nk_ui_pad(&label, 0.0f, NK_UI_SPACE_3, 0.0f, NK_UI_SPACE_3);
                text.letter_spacing = 1.2f;
                if (nk_ui_begin_container(ui, nk_ui_child_id(tab_id, 2u), &label, NULL)) {
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
        /* Indeterminate: a third of the track sweeps across every 1.6 s; a
           phase of 0 (reduced motion) holds it at the start. */
        double cycle = phase_s / 1.6;
        span = width * 0.32f;
        start = (float)(cycle - (double)(long long)cycle) * (width - span);
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
