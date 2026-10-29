/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Library: top bar, the LIBRARY heading with its count, a grid of game cards
   ending in the add card (or, with no games yet, the card that starts the setup
   wizard), the TEST THIS PC row with the demos that come with Nakagawa, and the
   prompt bar, over the launcher waves. Sizes and colours come from the design
   system (nk_ui_theme.h, docs/PLAYER_UI_DESIGN.md). */

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
#define LIBRARY_ID_EMPTY_ACTIONS 0x10000062u
#define LIBRARY_ID_EMPTY_NOTE 0x10000063u
#define LIBRARY_ID_DEMOS 0x10000070u
#define LIBRARY_ID_DEMOS_LABEL 0x10000071u
#define LIBRARY_ID_DEMOS_EYEBROW 0x10000072u
#define LIBRARY_ID_DEMOS_LEAD 0x10000073u
#define LIBRARY_ID_DEMOS_ROW_BASE 0x10000080u

/* One row of the TEST THIS PC section: the section's name and one line about it
   in the first column, then a compact card per demo, each one grid column wide. */
#define LIBRARY_DEMO_ROW_HEIGHT 72.0f
#define LIBRARY_DEMO_ART_WIDTH 72.0f
/* The first-start card's height with its icon, rounded up. */
#define LIBRARY_EMPTY_CARD_HEIGHT 300.0f
/* The LIBRARY heading's line height (NK_UI_TYPE_DISPLAY), reserved when working
   out how much room is left for the grid or the empty card. */
#define LIBRARY_HEADING_ROW_HEIGHT 32.0f

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
    /* Whole logical pixels, so every card in a row has the same width. */
    return (float)(int)tile;
}

static float library_tile_height(float tile_width) {
    return (float)(int)(tile_width * NK_UI_TILE_ART_RATIO + 0.5f) + NK_UI_TILE_BODY_HEIGHT;
}

static size_t library_columns(float width) {
    float available = width - 2.0f * library_gutter(width);
    size_t columns = (size_t)((available + NK_UI_TILE_GAP) /
                              (NK_UI_TILE_MIN_WIDTH + NK_UI_TILE_GAP));
    if (columns < 1u) columns = 1u;
    if (columns > NK_UI_TILE_MAX_COLUMNS) columns = NK_UI_TILE_MAX_COLUMNS;
    return columns;
}

/* Demo cards per TEST THIS PC row: the first column holds the section's name. */
static size_t library_demos_per_row(size_t columns) {
    return columns > 1u ? columns - 1u : 1u;
}

static size_t library_demo_rows(size_t demo_count, size_t columns) {
    size_t per_row = library_demos_per_row(columns);
    return demo_count == 0u ? 0u : (demo_count + per_row - 1u) / per_row;
}

/* The section's height, with the gap above it; 0 without demos. */
static float library_demos_height(size_t demo_count, size_t columns) {
    size_t rows = library_demo_rows(demo_count, columns);
    if (rows == 0u) return 0.0f;
    return NK_UI_SPACE_4 + (float)rows * LIBRARY_DEMO_ROW_HEIGHT +
           (float)(rows - 1u) * NK_UI_TILE_ROW_GAP;
}

size_t player_ui_library_page_size_with_demos(float logical_width, float logical_height,
                                              size_t demo_count, size_t *columns_out) {
    float width = logical_width > 80.0f ? logical_width : 1280.0f;
    float height = logical_height > 80.0f ? logical_height : 800.0f;
    size_t columns = library_columns(width);
    float grid_height;
    float tile_height;
    size_t rows;
    tile_height = library_tile_height(library_tile_width(width, columns));
    /* Window minus the bars, the body padding, the heading row and the TEST THIS
       PC section under the grid. */
    grid_height = height - NK_UI_TOP_BAR_HEIGHT - NK_UI_PROMPT_BAR_HEIGHT -
                  NK_UI_BODY_PAD_TOP - NK_UI_BODY_PAD_BOTTOM - LIBRARY_HEADING_ROW_HEIGHT -
                  NK_UI_SPACE_4 - library_demos_height(demo_count, columns);
    rows = grid_height > tile_height
        ? (size_t)((grid_height + NK_UI_TILE_ROW_GAP) / (tile_height + NK_UI_TILE_ROW_GAP))
        : 1u;
    if (rows < 1u) rows = 1u;
    if (columns_out != NULL) *columns_out = columns;
    return columns * rows;
}

size_t player_ui_library_page_size(float logical_width, float logical_height,
                                   size_t *columns_out) {
    return player_ui_library_page_size_with_demos(logical_width, logical_height, 0u,
                                                  columns_out);
}

size_t player_ui_library_demo_count(const PlayerUiTitle *titles, size_t count) {
    size_t demos = 0u;
    for (size_t i = 0u; titles != NULL && i < count; ++i) {
        if (titles[i].is_demo) ++demos;
    }
    return demos;
}

size_t player_ui_library_grid_position(const PlayerUiTitle *titles, size_t count,
                                       size_t index) {
    size_t position = 0u;
    if (titles == NULL || index >= count || titles[index].is_demo) return count;
    for (size_t i = 0u; i < index; ++i) {
        if (!titles[i].is_demo) ++position;
    }
    return position;
}

/* The title shown at grid position `position` (the position-th title that is
   not a demo), or `count` when there is none. */
static size_t library_grid_title(const PlayerUiTitle *titles, size_t count, size_t position) {
    for (size_t i = 0u; titles != NULL && i < count; ++i) {
        if (titles[i].is_demo) continue;
        if (position == 0u) return i;
        --position;
    }
    return count;
}

static const char *safe_text(const char *text, const char *fallback) {
    return text != NULL && text[0] != '\0' ? text : fallback;
}

static void library_prompts(NkUi *ui, bool has_titles) {
    static const NkUiPromptItem with_titles[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"},
        {NK_UI_PROMPT_ACT_CONFIRM, "Open"},
        {NK_UI_PROMPT_ACT_MENU, "Settings"}
    };
    static const NkUiPromptItem empty[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_MENU, "Settings"}
    };
    nk_ui_prompt_bar(ui, LIBRARY_ID_PROMPTS, "",
                     has_titles ? with_titles : empty,
                     has_titles ? sizeof(with_titles) / sizeof(with_titles[0])
                                : sizeof(empty) / sizeof(empty[0]));
}

/* The empty library: one card that says what to do, with the one action. It
   leaves out its icon when TEST THIS PC needs the room below it. */
static PlayerUiAction library_empty(NkUi *ui, bool with_icon) {
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
        if (with_icon &&
            nk_ui_begin_container(ui, LIBRARY_ID_EMPTY_ICON, &icon, &icon_style)) {
            nk_ui_shapes(ui, LIBRARY_ID_EMPTY_ICON + 0x100u, sizing_grow(), sizing_grow(), &disc_art);
            nk_ui_end_container(ui);
        }
        nk_ui_text_role(ui, LIBRARY_ID_EMPTY_TITLE, "Add your first game",
                        NK_UI_TYPE_TITLE, theme->paper);
        nk_ui_text_role(ui, LIBRARY_ID_EMPTY_TEXT,
                        "The setup wizard checks a disc image (.iso) of a PSP game you "
                        "own and gets it ready to play on this PC.",
                        NK_UI_TYPE_BODY, theme->muted);
        {
            NkUiLayout actions = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                             NK_UI_SPACE_3);
            actions.align_y = NK_UI_ALIGN_CENTER;
            actions.padding_top = NK_UI_SPACE_2;
            if (nk_ui_begin_container(ui, LIBRARY_ID_EMPTY_ACTIONS, &actions, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_LIBRARY_EMPTY_WIZARD, "Start setup wizard",
                                 NK_UI_BUTTON_PRIMARY | NK_UI_BUTTON_LARGE)) {
                    result = action(PLAYER_UI_ACTION_START_WIZARD, 0u);
                }
                /* Not a default: START SETUP WIZARD is the one default action. */
                if (nk_ui_button(ui, PLAYER_UI_ID_LIBRARY_EMPTY_ADD, "Choose a disc image",
                                 NK_UI_BUTTON_LARGE | NK_UI_BUTTON_NOT_DEFAULT)) {
                    result = action(PLAYER_UI_ACTION_ADD_DISC, 0u);
                }
                nk_ui_end_container(ui);
            }
        }
        nk_ui_text_role(ui, LIBRARY_ID_EMPTY_NOTE,
                        "Or drop an .iso onto this window.",
                        NK_UI_TYPE_CAPTION, theme->dim);
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
    return result;
}

/* A compact, focusable card for one demo: its project picture, its name (two
   lines at most) and its status. It is never the screen's default focus. */
static bool library_demo_card(NkUi *ui, NkUiId id, const PlayerUiTitle *title, float width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const NkUiShapeArt *picture = nk_ui_art_picture((NkUiArtKind)title->art_kind);
    NkUiLayout layout = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(width),
                                    sizing_fixed(LIBRARY_DEMO_ROW_HEIGHT), 0.0f);
    NkUiStyle style;
    bool focused;
    bool activated;
    char meta[96];
    if (picture == NULL) picture = nk_ui_art_n1_mark();
    nk_ui_register_button(ui, id, NK_UI_BUTTON_NOT_DEFAULT);
    focused = nk_ui_button_focused(ui, id);
    memset(&style, 0, sizeof(style));
    style.fill_rgba = theme->surface_raised;
    style.border_rgba = focused ? theme->line_strong : theme->line;
    style.border_width = NK_UI_BORDER_HAIRLINE;
    style.corner_radius = NK_UI_RADIUS_TILE;
    if (!nk_ui_begin_container(ui, id, &layout, &style)) return false;
    if (focused) nk_ui_focus_ring(ui, id, NK_UI_RADIUS_TILE);
    activated = nk_ui_take_activation(ui, id);
    {
        /* The art panel, flush with the card's left edge. */
        NkUiLayout art = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(LIBRARY_DEMO_ART_WIDTH),
                                     sizing_grow(), 0.0f);
        NkUiStyle art_style;
        float picture_box = LIBRARY_DEMO_ART_WIDTH - 2.0f * NK_UI_SPACE_2;
        float picture_h = LIBRARY_DEMO_ROW_HEIGHT - 2.0f * NK_UI_SPACE_2;
        float picture_w = picture_h * picture->view_w / picture->view_h;
        if (picture_w > picture_box) {
            picture_w = picture_box;
            picture_h = picture_w * picture->view_h / picture->view_w;
        }
        art.align_x = NK_UI_ALIGN_CENTER;
        art.align_y = NK_UI_ALIGN_CENTER;
        memset(&art_style, 0, sizeof(art_style));
        art_style.fill_rgba = nk_ui_art_tint(title->disc_id);
        art_style.corner_radius = NK_UI_RADIUS_TILE;
        art_style.square_corners = NK_UI_CORNER_SQUARE_TR | NK_UI_CORNER_SQUARE_BR;
        if (nk_ui_begin_container(ui, nk_ui_child_id(id, 1u), &art, &art_style)) {
            nk_ui_shapes(ui, nk_ui_child_id(id, 2u), sizing_fixed(picture_w),
                         sizing_fixed(picture_h), picture);
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiLayout body = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(),
                                      NK_UI_SPACE_1);
        float text_width = width - LIBRARY_DEMO_ART_WIDTH - 2.0f * NK_UI_SPACE_3;
        body.padding_left = NK_UI_SPACE_3;
        body.padding_right = NK_UI_SPACE_3;
        body.align_y = NK_UI_ALIGN_CENTER;
        if (nk_ui_begin_container(ui, nk_ui_child_id(id, 3u), &body, NULL)) {
            NkUiTextStyle name = nk_ui_type(NK_UI_TYPE_TILE_TITLE, theme->paper);
            const char *status_word = nk_ui_caps(ui, safe_text(title->library_status, ""));
            nk_ui_text(ui, nk_ui_child_id(id, 4u),
                       nk_ui_text_clamp(ui, safe_text(title->title, "Nakagawa demo"), &name,
                                        text_width, 2), &name);
            if (title->status_marker != NULL && title->status_marker[0] != '\0') {
                (void)snprintf(meta, sizeof(meta), "%s %s", title->status_marker, status_word);
            } else {
                (void)snprintf(meta, sizeof(meta), "%s", status_word);
            }
            nk_ui_text_role(ui, nk_ui_child_id(id, 5u), meta, NK_UI_TYPE_LABEL_S,
                            title->status_rgba != 0u ? title->status_rgba : theme->muted);
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
    return activated;
}

/* TEST THIS PC: the demos that come with Nakagawa, apart from the user's own
   games. Each opens its title page, where it plays like any other title. */
static PlayerUiAction library_demos(NkUi *ui, const PlayerUiLibraryModel *model,
                                    float width, size_t columns) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u);
    size_t per_row = library_demos_per_row(columns);
    size_t demo_count = player_ui_library_demo_count(model->titles, model->title_count);
    size_t rows = library_demo_rows(demo_count, columns);
    float cell = library_tile_width(width, columns);
    NkUiLayout layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(),
                                    NK_UI_TILE_ROW_GAP);
    size_t next = 0u;
    if (rows == 0u || !nk_ui_begin_container(ui, LIBRARY_ID_DEMOS, &layout, NULL)) {
        return result;
    }
    for (size_t row = 0u; row < rows; ++row) {
        NkUiLayout row_layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(),
                                            sizing_fixed(LIBRARY_DEMO_ROW_HEIGHT),
                                            NK_UI_TILE_GAP);
        if (!nk_ui_begin_container(ui, (NkUiId)(LIBRARY_ID_DEMOS_ROW_BASE + row),
                                   &row_layout, NULL)) {
            continue;
        }
        if (row == 0u) {
            NkUiLayout label = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(cell),
                                           sizing_grow(), NK_UI_SPACE_1);
            label.align_y = NK_UI_ALIGN_CENTER;
            if (nk_ui_begin_container(ui, LIBRARY_ID_DEMOS_LABEL, &label, NULL)) {
                NkUiTextStyle lead = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                nk_ui_eyebrow(ui, LIBRARY_ID_DEMOS_EYEBROW, "Test this PC");
                nk_ui_text(ui, LIBRARY_ID_DEMOS_LEAD,
                           nk_ui_text_clamp(ui,
                                            "Play a demo that comes with Nakagawa to check "
                                            "graphics, sound and controls.",
                                            &lead, cell, 2),
                           &lead);
                nk_ui_end_container(ui);
            }
        } else {
            nk_ui_spacer(ui, (NkUiId)(LIBRARY_ID_DEMOS_ROW_BASE + 0x40u + row),
                         sizing_fixed(cell), sizing_fixed(1.0f));
        }
        for (size_t placed = 0u; placed < per_row && next < model->title_count; ++next) {
            if (!model->titles[next].is_demo) continue;
            if (library_demo_card(ui, PLAYER_UI_ID_LIBRARY_ITEM(next), &model->titles[next],
                                  cell)) {
                result = action(PLAYER_UI_ACTION_OPEN_TITLE, next);
            }
            ++placed;
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
    size_t demo_count;
    size_t game_count;
    bool has_games;
    if (ui == NULL || model == NULL) {
        return result;
    }
    theme = nk_ui_theme(ui);
    width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    gutter = library_gutter(width);
    /* The user's own games fill the grid; the demos that come with Nakagawa sit
       apart, under TEST THIS PC. */
    demo_count = model->titles != NULL
        ? player_ui_library_demo_count(model->titles, model->title_count) : 0u;
    game_count = model->titles != NULL ? model->title_count - demo_count : 0u;
    has_games = game_count > 0u;

    (void)nk_ui_waves_element(ui, LIBRARY_ID_WAVES, (NkUiRect){0.0f, 0.0f, width, height}, 0u);
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, LIBRARY_ID_ROOT, &layout, NULL)) {
        return result;
    }
    {
        PlayerUiActionKind tab = player_ui_top_bar(ui, LIBRARY_ID_TOP, PLAYER_UI_ROUTE_LIBRARY);
        if (tab != PLAYER_UI_ACTION_NONE) result = action(tab, 0u);
    }

    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), NK_UI_SPACE_4);
    layout.padding_top = NK_UI_BODY_PAD_TOP;
    layout.padding_bottom = NK_UI_BODY_PAD_BOTTOM;
    layout.padding_left = gutter;
    layout.padding_right = gutter;
    if (nk_ui_begin_container(ui, LIBRARY_ID_BODY, &layout, NULL)) {
        size_t columns = model->columns > 0u ? model->columns : 1u;
        size_t page_size;
        /* The add card ends the grid, so it counts as one more item to page. */
        size_t item_count = has_games ? game_count + 1u : 0u;
        size_t page_count;
        size_t start;
        size_t visible;
        float tile_width;
        float tile_height;
        char count_text[96];

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
            if (has_games) {
                (void)snprintf(count_text, sizeof(count_text), "%zu title%s",
                               game_count, game_count == 1u ? "" : "s");
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
            /* The check at start found a newer version: one quiet button, never a
               screen of its own. */
            if (model->update_notice != NULL &&
                nk_ui_button(ui, PLAYER_UI_ID_LIBRARY_UPDATE_NOTICE, model->update_notice,
                             NK_UI_BUTTON_SMALL)) {
                result.kind = PLAYER_UI_ACTION_SETTINGS;
                result.primary = (PlayerUiPrimaryAction)PLAYER_UI_SETTINGS_OPEN_UPDATES;
            }
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

        if (!has_games) {
            /* Room for the card under the heading, above TEST THIS PC. */
            float room = height - NK_UI_TOP_BAR_HEIGHT - NK_UI_PROMPT_BAR_HEIGHT -
                         NK_UI_BODY_PAD_TOP - NK_UI_BODY_PAD_BOTTOM -
                         LIBRARY_HEADING_ROW_HEIGHT - NK_UI_SPACE_4 -
                         library_demos_height(demo_count, columns);
            PlayerUiAction empty = library_empty(ui, room >= LIBRARY_EMPTY_CARD_HEIGHT);
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
                        index = start + local < game_count
                            ? library_grid_title(model->titles, model->title_count,
                                                 start + local)
                            : model->title_count;
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
                            tile.icon = title->icon;
                            tile.width = tile_width;
                            if (nk_ui_tile(ui, PLAYER_UI_ID_LIBRARY_ITEM(index), &tile)) {
                                result = action(PLAYER_UI_ACTION_OPEN_TITLE, index);
                            }
                        } else if (nk_ui_add_tile(ui, PLAYER_UI_ID_LIBRARY_ADD,
                                                  "Add a disc image you own",
                                                  "Choose an .iso file",
                                                  tile_width, tile_height)) {
                            result = action(PLAYER_UI_ACTION_ADD_DISC, 0u);
                        }
                    }
                    nk_ui_end_container(ui);
                }
                nk_ui_end_container(ui);
            }
        }
        {
            PlayerUiAction demo = library_demos(ui, model, width, columns);
            if (demo.kind != PLAYER_UI_ACTION_NONE) result = demo;
        }
        nk_ui_end_container(ui);
    }
    library_prompts(ui, has_games);
    nk_ui_end_container(ui);
    return result;
}
