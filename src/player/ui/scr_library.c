/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Library: top bar, the LIBRARY heading with its count, a grid of game cards
   ending in the add card, and the prompt bar, over the launcher waves. Sizes and
   colours come from the design system (nk_ui_theme.h, docs/PLAYER_UI_DESIGN.md). */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"

#include <stdio.h>
#include <string.h>

#define LIBRARY_ID_ROOT 0x10000001u
#define LIBRARY_ID_TOP 0x10000010u
#define LIBRARY_ID_BODY 0x10000020u
#define LIBRARY_ID_HEAD 0x10000021u
#define LIBRARY_ID_HEADING 0x10000022u
#define LIBRARY_ID_COUNT 0x10000023u
#define LIBRARY_ID_HEAD_SPACER 0x10000024u
#define LIBRARY_ID_PAGER 0x10000025u
#define LIBRARY_ID_PAGE_LABEL 0x10000007u
#define LIBRARY_ID_GRID 0x10000030u
#define LIBRARY_ID_ROW_BASE 0x10000100u
#define LIBRARY_ID_PROMPTS 0x10000040u
#define LIBRARY_ID_WAVES 0x10000050u
#define LIBRARY_ID_EMPTY 0x10000005u
#define LIBRARY_ID_EMPTY_ICON 0x10000060u
#define LIBRARY_ID_EMPTY_TITLE 0x10000004u
#define LIBRARY_ID_EMPTY_TEXT 0x10000006u
#define LIBRARY_ID_EMPTY_WRAP 0x10000061u

static NkUiSizing sizing_grow(void) {
    NkUiSizing sizing = {NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    return sizing;
}

static NkUiSizing sizing_fit(void) {
    NkUiSizing sizing = {NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
    return sizing;
}

static NkUiSizing sizing_fixed(float value) {
    NkUiSizing sizing = {NK_UI_SIZE_FIXED, value, 0.0f, 0.0f};
    return sizing;
}

static NkUiLayout layout_make(NkUiLayoutDirection direction, NkUiSizing width,
                              NkUiSizing height, float gap) {
    NkUiLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.width = width;
    layout.height = height;
    layout.direction = direction;
    layout.gap = gap;
    return layout;
}

static PlayerUiAction action(PlayerUiActionKind kind, size_t title_index) {
    PlayerUiAction result;
    result.kind = kind;
    result.primary = PLAYER_UI_PRIMARY_NONE;
    result.title_index = title_index;
    return result;
}

static float library_gutter(float width) {
    return width < NK_UI_NARROW_WIDTH ? NK_UI_GUTTER_NARROW : NK_UI_GUTTER;
}

static float library_tile_width(float width, size_t columns) {
    float available = width - 2.0f * library_gutter(width);
    float tile;
    if (columns == 0u) columns = 1u;
    tile = (available - NK_UI_TILE_GAP * (float)(columns - 1u)) / (float)columns;
    if (tile < 150.0f) tile = 150.0f;
    return (float)(int)tile;
}

static float library_tile_height(float tile_width) {
    return (float)(int)(tile_width * NK_UI_TILE_ART_RATIO + 0.5f) + NK_UI_TILE_BODY_HEIGHT;
}

size_t player_ui_library_page_size(float logical_width, float logical_height,
                                   size_t *columns_out) {
    float width = logical_width > 80.0f ? logical_width : 1280.0f;
    float height = logical_height > 80.0f ? logical_height : 800.0f;
    float available = width - 2.0f * library_gutter(width);
    size_t columns = (size_t)((available + NK_UI_TILE_GAP) /
                              (NK_UI_TILE_MIN_WIDTH + NK_UI_TILE_GAP));
    float grid_height;
    float tile_height;
    size_t rows;
    if (columns < 1u) columns = 1u;
    if (columns > NK_UI_TILE_MAX_COLUMNS) columns = NK_UI_TILE_MAX_COLUMNS;
    tile_height = library_tile_height(library_tile_width(width, columns));
    /* Window minus the bars, the body padding and the heading row. */
    grid_height = height - NK_UI_TOP_BAR_HEIGHT - NK_UI_PROMPT_BAR_HEIGHT -
                  NK_UI_BODY_PAD_TOP - NK_UI_BODY_PAD_BOTTOM - 32.0f - NK_UI_SPACE_4;
    rows = grid_height > tile_height
        ? (size_t)((grid_height + NK_UI_TILE_ROW_GAP) / (tile_height + NK_UI_TILE_ROW_GAP))
        : 1u;
    if (rows < 1u) rows = 1u;
    if (columns_out != NULL) *columns_out = columns;
    return columns * rows;
}

static const char *safe_text(const char *text, const char *fallback) {
    return text != NULL && text[0] != '\0' ? text : fallback;
}

static void library_top_bar(NkUi *ui) {
    static const NkUiSection sections[] = {{"Library", true}};
    nk_ui_top_bar(ui, LIBRARY_ID_TOP, sections, 1u,
                  nk_ui_input_label(nk_ui_prompt_style(ui)),
                  nk_ui_prompt_style(ui) != NK_UI_PROMPT_KEYCAPS);
}

static void library_prompts(NkUi *ui, bool has_titles) {
    static const NkUiPromptItem with_titles[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"},
        {NK_UI_PROMPT_ACT_CONFIRM, "Open"}
    };
    static const NkUiPromptItem empty[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"}
    };
    nk_ui_prompt_bar(ui, LIBRARY_ID_PROMPTS, "Bring your own game. Nothing is downloaded.",
                     has_titles ? with_titles : empty,
                     has_titles ? sizeof(with_titles) / sizeof(with_titles[0])
                                : sizeof(empty) / sizeof(empty[0]));
}

/* The empty library: one card that says what to do, with the one action. */
static PlayerUiAction library_empty(NkUi *ui) {
    static const NkUiShape disc[] = {
        {NK_UI_SHAPE_CIRCLE, 0xD7F24AFFu, 0.0f, 0u, {32.0f, 32.0f, 16.5f}},
        {NK_UI_SHAPE_CIRCLE, 0x18202BFFu, 0.0f, 0u, {32.0f, 32.0f, 13.5f}},
        {NK_UI_SHAPE_CIRCLE, 0xD7F24AFFu, 0.0f, 0u, {32.0f, 32.0f, 4.0f}}
    };
    static const NkUiShapeArt disc_art = {0.0f, 0.0f, 64.0f, 64.0f, disc, 3u};
    const NkUiTheme *theme = nk_ui_theme(ui);
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u);
    NkUiLayout wrap = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_grow(), 0.0f);
    wrap.align_x = NK_UI_ALIGN_CENTER;
    wrap.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, LIBRARY_ID_EMPTY_WRAP, &wrap, NULL)) {
        return result;
    }
    if (nk_ui_card_begin(ui, LIBRARY_ID_EMPTY, sizing_fixed(640.0f), sizing_fit(),
                         NK_UI_SPACE_4)) {
        NkUiLayout icon = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(56.0f),
                                      sizing_fixed(56.0f), 0.0f);
        NkUiStyle icon_style;
        memset(&icon_style, 0, sizeof(icon_style));
        icon_style.fill_rgba = theme->surface_high;
        icon_style.border_rgba = theme->line_strong;
        icon_style.border_width = 2.0f;
        icon_style.corner_radius = NK_UI_RADIUS_DIALOG;
        if (nk_ui_begin_container(ui, LIBRARY_ID_EMPTY_ICON, &icon, &icon_style)) {
            nk_ui_shapes(ui, LIBRARY_ID_EMPTY_ICON + 0x100u, sizing_grow(), sizing_grow(), &disc_art);
            nk_ui_end_container(ui);
        }
        nk_ui_text_role(ui, LIBRARY_ID_EMPTY_TITLE, "Your library is empty",
                        NK_UI_TYPE_TITLE, theme->paper);
        nk_ui_text_role(ui, LIBRARY_ID_EMPTY_TEXT,
                        "Select a PSP ISO you own to add a game to your library. "
                        "No game data comes with the player, and nothing is downloaded.",
                        NK_UI_TYPE_BODY, theme->muted);
        if (nk_ui_button(ui, PLAYER_UI_ID_LIBRARY_EMPTY_ADD, "Add a PSP disc image",
                         NK_UI_BUTTON_PRIMARY)) {
            result = action(PLAYER_UI_ACTION_ADD_DISC, 0u);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
    return result;
}

PlayerUiAction player_ui_screen_library(NkUi *ui,
                                        const PlayerUiLibraryModel *model) {
    const NkUiTheme *theme;
    NkUiLayout layout;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u);
    float width;
    float height;
    float gutter;
    bool has_titles;
    if (ui == NULL || model == NULL) {
        return result;
    }
    theme = nk_ui_theme(ui);
    width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    gutter = library_gutter(width);
    has_titles = model->title_count > 0u && model->titles != NULL;

    (void)nk_ui_waves_element(ui, LIBRARY_ID_WAVES, (NkUiRect){0.0f, 0.0f, width, height}, 0u);
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, LIBRARY_ID_ROOT, &layout, NULL)) {
        return result;
    }
    library_top_bar(ui);

    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), NK_UI_SPACE_4);
    layout.padding_top = NK_UI_BODY_PAD_TOP;
    layout.padding_bottom = NK_UI_BODY_PAD_BOTTOM;
    layout.padding_left = gutter;
    layout.padding_right = gutter;
    if (nk_ui_begin_container(ui, LIBRARY_ID_BODY, &layout, NULL)) {
        size_t columns = model->columns > 0u ? model->columns : 1u;
        size_t page_size;
        size_t item_count = has_titles ? model->title_count + 1u : 0u;
        size_t page_count;
        size_t start;
        size_t visible;
        float tile_width;
        float tile_height;
        char count_text[96];
        size_t demos = 0u;

        if (columns > NK_UI_TILE_MAX_COLUMNS) columns = NK_UI_TILE_MAX_COLUMNS;
        page_size = model->page_size > 0u ? model->page_size : columns * 2u;
        if (page_size < columns) page_size = columns;
        page_count = item_count == 0u ? 1u : (item_count + page_size - 1u) / page_size;

        /* Heading row: the screen name, its count, and the pager when needed. */
        layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
        layout.align_y = NK_UI_ALIGN_END;
        if (nk_ui_begin_container(ui, LIBRARY_ID_HEAD, &layout, NULL)) {
            nk_ui_text_role(ui, LIBRARY_ID_HEADING, "LIBRARY", NK_UI_TYPE_DISPLAY,
                            theme->paper);
            if (has_titles) {
                for (size_t i = 0u; i < model->title_count; ++i) {
                    if (model->titles[i].art_kind != NK_UI_ART_NONE) ++demos;
                }
                if (demos > 0u) {
                    (void)snprintf(count_text, sizeof(count_text),
                                   "%zu title%s \xC2\xB7 %zu showcase demo%s",
                                   model->title_count, model->title_count == 1u ? "" : "s",
                                   demos, demos == 1u ? "" : "s");
                } else {
                    (void)snprintf(count_text, sizeof(count_text), "%zu title%s",
                                   model->title_count, model->title_count == 1u ? "" : "s");
                }
            } else {
                (void)snprintf(count_text, sizeof(count_text), "No games yet");
            }
            {
                NkUiLayout count_box = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(),
                                                   sizing_fit(), 0.0f);
                count_box.padding_bottom = 3.0f;
                if (nk_ui_begin_container(ui, LIBRARY_ID_COUNT, &count_box, NULL)) {
                    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                    text.wrap = false;
                    nk_ui_text(ui, LIBRARY_ID_COUNT + 0x100u, count_text, &text);
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_spacer(ui, LIBRARY_ID_HEAD_SPACER, sizing_grow(), sizing_fixed(1.0f));
            if (page_count > 1u) {
                NkUiLayout pager = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                               NK_UI_SPACE_3);
                pager.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, LIBRARY_ID_PAGER, &pager, NULL)) {
                    if (nk_ui_button(ui, PLAYER_UI_ID_LIBRARY_PAGE_PREVIOUS, "Previous",
                                     NK_UI_BUTTON_SMALL |
                                     (model->page_index == 0u ? NK_UI_BUTTON_DISABLED : 0u))) {
                        result = action(PLAYER_UI_ACTION_PAGE_PREVIOUS, 0u);
                    }
                    nk_ui_text_role(ui, LIBRARY_ID_PAGE_LABEL,
                                    nk_ui_caps(ui, safe_text(model->page_label, "More titles")),
                                    NK_UI_TYPE_LABEL_S, theme->muted);
                    if (nk_ui_button(ui, PLAYER_UI_ID_LIBRARY_PAGE_NEXT, "Next",
                                     NK_UI_BUTTON_SMALL |
                                     (model->page_index + 1u >= page_count
                                          ? NK_UI_BUTTON_DISABLED : 0u))) {
                        result = action(PLAYER_UI_ACTION_PAGE_NEXT, 0u);
                    }
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }

        if (!has_titles) {
            PlayerUiAction empty = library_empty(ui);
            if (empty.kind != PLAYER_UI_ACTION_NONE) result = empty;
        } else {
            start = model->page_index > SIZE_MAX / page_size
                ? item_count : model->page_index * page_size;
            if (start > item_count) start = item_count;
            visible = item_count - start;
            if (visible > page_size) visible = page_size;
            tile_width = library_tile_width(width, columns);
            tile_height = library_tile_height(tile_width);
            layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(),
                                 NK_UI_TILE_ROW_GAP);
            if (nk_ui_begin_container(ui, LIBRARY_ID_GRID, &layout, NULL)) {
                size_t rows = (visible + columns - 1u) / columns;
                for (size_t row = 0u; row < rows; ++row) {
                    NkUiLayout row_layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(),
                                                        sizing_fit(), NK_UI_TILE_GAP);
                    if (!nk_ui_begin_container(ui, (NkUiId)(LIBRARY_ID_ROW_BASE + row),
                                               &row_layout, NULL)) {
                        continue;
                    }
                    for (size_t col = 0u; col < columns; ++col) {
                        size_t local = row * columns + col;
                        size_t index;
                        if (local >= visible) break;
                        index = start + local;
                        if (index < model->title_count) {
                            const PlayerUiTitle *title = &model->titles[index];
                            NkUiTileSpec tile;
                            memset(&tile, 0, sizeof(tile));
                            tile.title = safe_text(title->title, "Untitled PSP game");
                            tile.disc_id = title->disc_id;
                            tile.status_marker = title->status_marker;
                            tile.status_word = safe_text(title->library_status, "");
                            tile.status_rgba = title->status_rgba;
                            tile.badge = title->art_kind != NK_UI_ART_NONE && title->badge == NULL
                                ? "Showcase demo" : title->badge;
                            tile.art = (NkUiArtKind)title->art_kind;
                            tile.width = tile_width;
                            if (nk_ui_tile(ui, PLAYER_UI_ID_LIBRARY_ITEM(index), &tile)) {
                                result = action(PLAYER_UI_ACTION_OPEN_TITLE, index);
                            }
                        } else if (nk_ui_add_tile(ui, PLAYER_UI_ID_LIBRARY_ADD,
                                                  "Add a disc image you own",
                                                  "Choose an .iso. Nothing is downloaded.",
                                                  tile_width, tile_height)) {
                            result = action(PLAYER_UI_ACTION_ADD_DISC, 0u);
                        }
                    }
                    nk_ui_end_container(ui);
                }
                nk_ui_end_container(ui);
            }
        }
        nk_ui_end_container(ui);
    }
    library_prompts(ui, has_titles);
    nk_ui_end_container(ui);
    return result;
}
