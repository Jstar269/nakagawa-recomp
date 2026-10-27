/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Title detail: the hero art and package status on the left; the name, status
   chips, actions and compatibility card on the right; the prompt bar below.
   Layout follows the design system (docs/PLAYER_UI_DESIGN.md). */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"

#include <stdio.h>
#include <string.h>

#define TITLE_ID_ROOT 0x20000001u
#define TITLE_ID_TOP 0x20000010u
#define TITLE_ID_BODY 0x20000020u
#define TITLE_ID_ASIDE 0x20000021u
#define TITLE_ID_MAIN 0x20000022u
#define TITLE_ID_HERO 0x20000023u
#define TITLE_ID_STATUS_CARD 0x20000009u
#define TITLE_ID_STATUS_HEAD 0x20000024u
#define TITLE_ID_STATUS_EYEBROW 0x20000025u
#define TITLE_ID_STATUS_PILL 0x20000026u
#define TITLE_ID_STATUS_DETAIL 0x2000000Au
#define TITLE_ID_NOTE 0x2000000Bu
#define TITLE_ID_NOTE_TEXT 0x2000000Cu
#define TITLE_ID_CRUMB 0x20000002u
#define TITLE_ID_NAME 0x20000003u
#define TITLE_ID_DISC 0x20000004u
#define TITLE_ID_CHIPS 0x20000027u
#define TITLE_ID_CHIP_STATUS 0x20000028u
#define TITLE_ID_CHIP_BADGE 0x20000029u
#define TITLE_ID_ACTIONS 0x2000002Au
#define TITLE_ID_BUILD_PREFLIGHT 0x20000030u
#define TITLE_ID_BUILD_FOLDER 0x20000031u
#define TITLE_ID_STALL_NOTICE 0x20000035u
#define TITLE_ID_STALL_EYEBROW 0x20000036u
#define TITLE_ID_STALL_TEXT 0x20000037u
#define TITLE_ID_STALL_ACTIONS 0x20000038u
#define TITLE_ID_COMPAT 0x20000005u
#define TITLE_ID_COMPAT_HEAD 0x2000002Cu
#define TITLE_ID_COMPAT_EYEBROW 0x20000006u
#define TITLE_ID_COMPAT_TIER 0x20000007u
#define TITLE_ID_COMPAT_EXPECT 0x20000008u
#define TITLE_ID_COMPAT_ISSUES 0x2000002Eu
#define TITLE_ID_COMPAT_CATEGORY 0x2000002Du
#define TITLE_ID_REMOVE_ROW 0x2000002Fu
#define TITLE_ID_PROMPTS 0x20000040u
#define TITLE_ID_WAVES 0x20000050u

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

static PlayerUiAction action(PlayerUiActionKind kind, size_t title_index,
                             PlayerUiPrimaryAction primary) {
    PlayerUiAction result;
    result.kind = kind;
    result.primary = primary;
    result.title_index = title_index;
    return result;
}

static const char *safe_text(const char *text, const char *fallback) {
    return text != NULL && text[0] != '\0' ? text : fallback;
}

/* docs/COMPATIBILITY.md category -> player tier, for a title with no player
   record of its own (#1297). The same table is in the "Status words" section
   of docs/COMPATIBILITY.md; test_player_screens.c pins both. A category
   describes the project's test route, so each maps to the most a player can
   count on: a title that only boots to a named boundary is not promised more
   than its title screen. Experimental and Untested have no tier. */
static const struct {
    const char *category;
    PlayerUiTier tier;
} s_tier_by_category[] = {
    {"Works", PLAYER_UI_TIER_PERFECT},
    {"Partially works", PLAYER_UI_TIER_IN_GAME},
    {"Boots to a named boundary", PLAYER_UI_TIER_TITLE_SCREEN},
    {"Not started", PLAYER_UI_TIER_WONT_BOOT},
    {"Not supported", PLAYER_UI_TIER_WONT_BOOT},
    {"Experimental", PLAYER_UI_TIER_NONE},
    {"Untested", PLAYER_UI_TIER_NONE}
};

PlayerUiTier player_ui_tier_for_category(const char *category) {
    if (category == NULL) return PLAYER_UI_TIER_NONE;
    for (size_t i = 0u; i < sizeof(s_tier_by_category) / sizeof(s_tier_by_category[0]);
         ++i) {
        if (strcmp(category, s_tier_by_category[i].category) == 0) {
            return s_tier_by_category[i].tier;
        }
    }
    return PLAYER_UI_TIER_NONE;
}

const char *player_ui_tier_word(PlayerUiTier tier) {
    switch (tier) {
        case PLAYER_UI_TIER_PERFECT: return "Perfect";
        case PLAYER_UI_TIER_PLAYABLE: return "Playable";
        case PLAYER_UI_TIER_IN_GAME: return "In-game";
        case PLAYER_UI_TIER_MENU: return "Menu";
        case PLAYER_UI_TIER_TITLE_SCREEN: return "Title screen";
        case PLAYER_UI_TIER_WONT_BOOT: return "Won't boot";
        case PLAYER_UI_TIER_NONE:
        default: return NULL;
    }
}

/* How a tier chip is drawn; the word itself comes from the host's data. */
static uint32_t tier_rgba(const NkUiTheme *theme, PlayerUiTier tier) {
    switch (tier) {
        case PLAYER_UI_TIER_PERFECT:
        case PLAYER_UI_TIER_PLAYABLE: return theme->ready;
        case PLAYER_UI_TIER_IN_GAME:
        case PLAYER_UI_TIER_MENU: return theme->info;
        case PLAYER_UI_TIER_TITLE_SCREEN: return theme->warning;
        case PLAYER_UI_TIER_WONT_BOOT: return theme->error;
        case PLAYER_UI_TIER_NONE:
        default: return theme->muted;
    }
}

/* The package card's sentence for each package state (#1297). */
static const char *package_state_text(PlayerUiPackageState state) {
    switch (state) {
        case PLAYER_UI_PACKAGE_READY: return "Built for this PC and ready to play.";
        case PLAYER_UI_PACKAGE_CHECKING: return "Checking the package for this game.";
        case PLAYER_UI_PACKAGE_BUILDING: return "Building the package for this game.";
        case PLAYER_UI_PACKAGE_CHECK_FAILED: return "The package could not be checked.";
        case PLAYER_UI_PACKAGE_STARTING:
            return "Starting the game. Its window opens when it is ready.";
        case PLAYER_UI_PACKAGE_RUNNING:
            return "Running now. Stop the game to rebuild or change settings.";
        case PLAYER_UI_PACKAGE_MISSING:
        default: return "The package for this game is not ready yet.";
    }
}

PlayerUiAction player_ui_screen_title(NkUi *ui,
                                      const PlayerUiTitleModel *model) {
    const NkUiTheme *theme;
    const PlayerUiTitle *title;
    NkUiLayout layout;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float width;
    float height;
    float gutter;
    float aside;
    if (ui == NULL || model == NULL || model->title == NULL) return result;
    theme = nk_ui_theme(ui);
    title = model->title;
    width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    gutter = width < NK_UI_NARROW_WIDTH ? NK_UI_GUTTER_NARROW : NK_UI_GUTTER;
    aside = (width - 2.0f * gutter - NK_UI_DETAIL_GAP) * 0.45f;
    if (aside > NK_UI_DETAIL_ASIDE_WIDTH) aside = NK_UI_DETAIL_ASIDE_WIDTH;
    aside = (float)(int)aside;

    (void)nk_ui_waves_element(ui, TITLE_ID_WAVES, (NkUiRect){0.0f, 0.0f, width, height}, 0u);
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, TITLE_ID_ROOT, &layout, NULL)) {
        return result;
    }
    {
        PlayerUiActionKind tab = player_ui_top_bar(ui, TITLE_ID_TOP, PLAYER_UI_ROUTE_TITLE);
        if (tab != PLAYER_UI_ACTION_NONE) {
            result = action(tab, model->title_index, PLAYER_UI_PRIMARY_NONE);
        }
    }

    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_grow(), NK_UI_DETAIL_GAP);
    layout.padding_top = NK_UI_BODY_PAD_TOP;
    layout.padding_bottom = NK_UI_BODY_PAD_BOTTOM;
    layout.padding_left = gutter;
    layout.padding_right = gutter;
    if (nk_ui_begin_container(ui, TITLE_ID_BODY, &layout, NULL)) {
        /* Left column: the art and what the package needs. */
        layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(aside), sizing_grow(),
                             NK_UI_SPACE_4);
        if (nk_ui_begin_container(ui, TITLE_ID_ASIDE, &layout, NULL)) {
            bool demo = title->art_kind != NK_UI_ART_NONE;
            nk_ui_hero(ui, TITLE_ID_HERO, (NkUiArtKind)title->art_kind, title->disc_id,
                       title->icon, title->picture, aside,
                       demo ? "Picture made by the project" : "Your disc",
                       "480 \xC3\x97 272");
            if (nk_ui_card_begin(ui, TITLE_ID_STATUS_CARD, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_3)) {
                NkUiLayout head = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_3);
                head.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, TITLE_ID_STATUS_HEAD, &head, NULL)) {
                    nk_ui_eyebrow(ui, TITLE_ID_STATUS_EYEBROW, "Package");
                    if (title->library_status != NULL && title->library_status[0] != '\0') {
                        nk_ui_status_pill(ui, TITLE_ID_STATUS_PILL, title->status_marker,
                                          title->library_status, title->status_rgba);
                    }
                    nk_ui_end_container(ui);
                }
                {
                    /* A starting or running game says so; otherwise the host's
                       reason, or the package state's own sentence. */
                    bool live = title->package_state == PLAYER_UI_PACKAGE_STARTING ||
                                title->package_state == PLAYER_UI_PACKAGE_RUNNING;
                    const char *state_text = package_state_text(title->package_state);
                    nk_ui_text_role(ui, TITLE_ID_STATUS_DETAIL,
                                    live ? state_text
                                         : safe_text(title->library_status_detail, state_text),
                                    NK_UI_TYPE_BODY_S, theme->muted);
                }
                /* The decrypted-files note belongs to the steps that need those
                   files: a built, starting or running package has them (#1297). */
                if (title->input_note != NULL && title->input_note[0] != '\0' &&
                    (title->package_state == PLAYER_UI_PACKAGE_MISSING ||
                     title->package_state == PLAYER_UI_PACKAGE_CHECKING ||
                     title->package_state == PLAYER_UI_PACKAGE_CHECK_FAILED)) {
                    NkUiLayout note = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(),
                                                  sizing_fit(), 0.0f);
                    NkUiStyle note_style;
                    memset(&note_style, 0, sizeof(note_style));
                    note_style.fill_rgba = theme->surface;
                    note_style.border_rgba = theme->line_strong;
                    note_style.border_width = NK_UI_BORDER_HAIRLINE;
                    note_style.corner_radius = NK_UI_RADIUS_CONTROL;
                    note.padding_top = note.padding_bottom = 10.0f;
                    note.padding_left = note.padding_right = 14.0f;
                    if (nk_ui_begin_container(ui, TITLE_ID_NOTE, &note, &note_style)) {
                        nk_ui_text_role(ui, TITLE_ID_NOTE_TEXT, title->input_note,
                                        NK_UI_TYPE_CAPTION, theme->info);
                        nk_ui_end_container(ui);
                    }
                }
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }

        /* Right column: name, status, actions, compatibility. */
        layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 14.0f);
        if (nk_ui_begin_container(ui, TITLE_ID_MAIN, &layout, NULL)) {
            NkUiLayout row;
            nk_ui_eyebrow(ui, TITLE_ID_CRUMB, "Library \xE2\x80\xBA Title");
            nk_ui_text_role(ui, TITLE_ID_NAME, safe_text(title->title, "Untitled PSP game"),
                            NK_UI_TYPE_TITLE_XL, theme->paper);
            if (title->disc_id != NULL && title->disc_id[0] != '\0') {
                nk_ui_text_role(ui, TITLE_ID_DISC, title->disc_id, NK_UI_TYPE_DATA,
                                theme->muted);
            }
            row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_2);
            if (nk_ui_begin_container(ui, TITLE_ID_CHIPS, &row, NULL)) {
                if (title->library_status != NULL && title->library_status[0] != '\0') {
                    nk_ui_status_pill(ui, TITLE_ID_CHIP_STATUS, title->status_marker,
                                      title->library_status, title->status_rgba);
                }
                nk_ui_tag(ui, TITLE_ID_CHIP_BADGE,
                          safe_text(title->badge, title->art_kind != NK_UI_ART_NONE
                                                      ? "Showcase demo" : "Your disc"));
                nk_ui_end_container(ui);
            }
            if (model->stalled_game_notice_visible &&
                nk_ui_card_begin(ui, TITLE_ID_STALL_NOTICE, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_2)) {
                nk_ui_eyebrow(ui, TITLE_ID_STALL_EYEBROW, "No new frames");
                nk_ui_text_role(ui, TITLE_ID_STALL_TEXT,
                                "The game hasn't drawn anything new for 30 seconds. It may be stuck.",
                                NK_UI_TYPE_BODY_S, theme->paper);
                row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
                row.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, TITLE_ID_STALL_ACTIONS, &row, NULL)) {
                    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_STALL_KEEP_WAITING,
                                     "Keep waiting", NK_UI_BUTTON_LARGE)) {
                        result = action(PLAYER_UI_ACTION_STALL_KEEP_WAITING,
                                        model->title_index, PLAYER_UI_PRIMARY_NONE);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_STALL_STOP_GAME,
                                     "Stop game", NK_UI_BUTTON_LARGE | NK_UI_BUTTON_DANGER)) {
                        result = action(PLAYER_UI_ACTION_STALL_STOP_GAME,
                                        model->title_index, PLAYER_UI_PRIMARY_NONE);
                    }
                    nk_ui_end_container(ui);
                }
                nk_ui_end_container(ui);
            }
            /* One action row on the column's left edge, every button the same
               52 px height; the focus ring may spill into the column gap. */
            row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
            row.align_y = NK_UI_ALIGN_CENTER;
            row.padding_top = NK_UI_SPACE_2;
            row.padding_bottom = NK_UI_SPACE_2;
            if (nk_ui_begin_container(ui, TITLE_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_PRIMARY,
                                 safe_text(title->primary_action_label, "Unavailable"),
                                 NK_UI_BUTTON_LARGE |
                                 (title->primary_action_enabled
                                      ? NK_UI_BUTTON_PRIMARY : NK_UI_BUTTON_DISABLED))) {
                    result = action(PLAYER_UI_ACTION_PRIMARY, model->title_index,
                                    title->primary_action);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_SAVE_FOLDER, "Open save folder",
                                 NK_UI_BUTTON_LARGE |
                                 (title->save_folder_enabled ? 0u : NK_UI_BUTTON_DISABLED))) {
                    result = action(PLAYER_UI_ACTION_OPEN_SAVE_FOLDER,
                                    model->title_index, PLAYER_UI_PRIMARY_NONE);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_BACK, "Back", NK_UI_BUTTON_LARGE)) {
                    result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                                    PLAYER_UI_PRIMARY_NONE);
                }
                nk_ui_end_container(ui);
            }
            if (title->build_preflight_message != NULL &&
                title->build_preflight_message[0] != '\0') {
                nk_ui_text_role(ui, TITLE_ID_BUILD_PREFLIGHT,
                                title->build_preflight_message,
                                NK_UI_TYPE_BODY_S, theme->error);
                if (title->build_preflight_folder != NULL &&
                    title->build_preflight_folder[0] != '\0' &&
                    nk_ui_button(ui, TITLE_ID_BUILD_FOLDER, "Open folder", 0u)) {
                    result = action(PLAYER_UI_ACTION_OPEN_BUILD_INPUT_FOLDER,
                                    model->title_index, PLAYER_UI_PRIMARY_NONE);
                }
            }
            /* The player tier, then what to expect in plain words and any
               known issues (#1297). The docs category describes the
               project's test route, so only the developer view names it. */
            if (nk_ui_card_begin(ui, TITLE_ID_COMPAT, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_3)) {
                NkUiLayout head = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_3);
                head.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, TITLE_ID_COMPAT_HEAD, &head, NULL)) {
                    nk_ui_eyebrow(ui, TITLE_ID_COMPAT_EYEBROW, "What to expect");
                    const char *tier = player_ui_tier_word(title->compatibility_tier);
                    if (tier != NULL) {
                        nk_ui_status_pill(ui, TITLE_ID_COMPAT_TIER, NULL, tier,
                                          tier_rgba(theme, title->compatibility_tier));
                    }
                    nk_ui_end_container(ui);
                }
                nk_ui_text_role(ui, TITLE_ID_COMPAT_EXPECT,
                                safe_text(title->compatibility_expect,
                                          "No one has tried this game in Nakagawa yet."),
                                NK_UI_TYPE_BODY_S, theme->paper);
                if (title->compatibility_known_issues != NULL &&
                    title->compatibility_known_issues[0] != '\0') {
                    nk_ui_text_role(ui, TITLE_ID_COMPAT_ISSUES,
                                    title->compatibility_known_issues,
                                    NK_UI_TYPE_BODY_S, theme->muted);
                }
                if (title->compatibility_category != NULL &&
                    title->compatibility_category[0] != '\0') {
                    char line[160];
                    (void)snprintf(line, sizeof(line),
                                   "Developer view: \"%s\" in docs/COMPATIBILITY.md.",
                                   title->compatibility_category);
                    nk_ui_text_role(ui, TITLE_ID_COMPAT_CATEGORY, nk_ui_frame_text(ui, line),
                                    NK_UI_TYPE_FOOTNOTE, theme->dim);
                }
                nk_ui_end_container(ui);
            }
            /* Remove from library (#1304), apart from the play actions: it
               deletes what Nakagawa built, so it asks first. */
            if (title->can_remove) {
                NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                             NK_UI_SPACE_3);
                if (nk_ui_begin_container(ui, TITLE_ID_REMOVE_ROW, &row, NULL)) {
                    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_REMOVE, "Remove from library",
                                     NK_UI_BUTTON_DANGER)) {
                        result = action(PLAYER_UI_ACTION_REMOVE_TITLE, model->title_index,
                                        PLAYER_UI_PRIMARY_NONE);
                    }
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    {
        static const NkUiPromptItem prompts[] = {
            {NK_UI_PROMPT_ACT_MOVE, "Move"},
            {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
            {NK_UI_PROMPT_ACT_BACK, "Back"}
        };
        nk_ui_prompt_bar(ui, TITLE_ID_PROMPTS, "", prompts,
                         sizeof(prompts) / sizeof(prompts[0]));
    }
    nk_ui_end_container(ui);
    return result;
}

/* ---- Progress and dialogs ------------------------------------------------------
   The screens a first run passes through after the library: the package build
   with its live stages, the one-time build tool download (consent, then
   progress), and the error card. Each is one centred dialog card over the
   launcher background, with the top bar's brand and the prompt bar. */

#define FLOW_ID_ROOT 0x60000001u
#define FLOW_ID_TOP 0x60000010u
#define FLOW_ID_BODY 0x60000020u
#define FLOW_ID_CARD 0x60000021u
#define FLOW_ID_EYEBROW 0x60000022u
#define FLOW_ID_TITLE 0x60000023u
#define FLOW_ID_LEAD 0x60000024u
#define FLOW_ID_STAGES 0x60000030u
#define FLOW_ID_BAR 0x60000040u
#define FLOW_ID_STATUS 0x60000041u
#define FLOW_ID_MESSAGE 0x60000042u
#define FLOW_ID_ELAPSED 0x60000043u
#define FLOW_ID_NOTICE 0x60000044u
#define FLOW_ID_LOG 0x60000050u
#define FLOW_ID_ROWS 0x60000060u
#define FLOW_ID_TOTAL 0x60000070u
#define FLOW_ID_ACTIONS 0x60000080u
#define FLOW_ID_DETAILS 0x60000090u
#define FLOW_ID_PROMPTS 0x600000A0u
#define FLOW_ID_WAVES 0x600000B0u

#define FLOW_MARK_DIAMOND "\xE2\x97\x86"
#define FLOW_MARK_DOT "\xE2\x97\x8F"
#define FLOW_MARK_CROSS "\xC3\x97" /* multiplication sign: in every UI face */
#define FLOW_MARK_TRIANGLE "\xE2\x96\xB2"
#define FLOW_MARK_CHECK "\xE2\x9C\x93 "
#define FLOW_MIDDOT " \xC2\xB7 "

/* Opens the shared frame and the dialog card; returns the card's inner width,
   or 0 when nothing was opened (then nothing must be closed). */
static float flow_begin(NkUi *ui, float width, float height, float card_width) {
    NkUiLayout layout;
    float card;
    (void)nk_ui_waves_element(ui, FLOW_ID_WAVES, (NkUiRect){0.0f, 0.0f, width, height}, 0u);
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, FLOW_ID_ROOT, &layout, NULL)) {
        return 0.0f;
    }
    (void)nk_ui_top_bar(ui, FLOW_ID_TOP, NULL, 0u, nk_ui_input_label(nk_ui_prompt_style(ui)),
                        nk_ui_prompt_style(ui) != NK_UI_PROMPT_KEYCAPS);
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_grow(), 0.0f);
    layout.align_x = NK_UI_ALIGN_CENTER;
    layout.align_y = NK_UI_ALIGN_CENTER;
    layout.padding_left = layout.padding_right = NK_UI_GUTTER_NARROW;
    layout.padding_top = layout.padding_bottom = NK_UI_SPACE_4;
    if (!nk_ui_begin_container(ui, FLOW_ID_BODY, &layout, NULL)) {
        nk_ui_end_container(ui);
        return 0.0f;
    }
    card = width - 2.0f * NK_UI_GUTTER_NARROW;
    if (card > card_width) card = card_width;
    if (!nk_ui_dialog_begin(ui, FLOW_ID_CARD, card)) {
        nk_ui_end_container(ui);
        nk_ui_end_container(ui);
        return 0.0f;
    }
    return card - 56.0f;
}

static void flow_end(NkUi *ui, const char *hint, const NkUiPromptItem *prompts, size_t count) {
    nk_ui_end_container(ui); /* card */
    nk_ui_end_container(ui); /* body */
    nk_ui_prompt_bar(ui, FLOW_ID_PROMPTS, hint, prompts, count);
    nk_ui_end_container(ui); /* root */
}

static void flow_eyebrow(NkUi *ui, const char *marker, const char *text, uint32_t rgba) {
    char line[160];
    (void)snprintf(line, sizeof(line), "%s %s", marker, text);
    nk_ui_text_role(ui, FLOW_ID_EYEBROW, nk_ui_caps(ui, line), NK_UI_TYPE_EYEBROW, rgba);
}

static NkUiStyle flow_box_style(const NkUiTheme *theme, uint32_t fill) {
    NkUiStyle style;
    memset(&style, 0, sizeof(style));
    style.fill_rgba = fill;
    style.border_rgba = theme->line;
    style.border_width = NK_UI_BORDER_HAIRLINE;
    style.corner_radius = 10.0f;
    return style;
}

PlayerUiAction player_ui_screen_build(NkUi *ui, const PlayerUiBuildModel *model) {
    static const char *const stages[4] = {"Preflight", "Extract", "Compile", "Package"};
    static const NkUiPromptItem prompts_running[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"}, {NK_UI_PROMPT_ACT_BACK, "Cancel build"}
    };
    static const NkUiPromptItem prompts_done[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"}, {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char elapsed[48];
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    inner = flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 720.0f);
    if (inner <= 0.0f) return result;
    flow_eyebrow(ui, model->built ? FLOW_MARK_DOT : FLOW_MARK_DIAMOND,
                 model->built ? "Package built" : "Building the package",
                 model->built ? theme->ready : theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, safe_text(model->title_line, "Package build"),
                    NK_UI_TYPE_TITLE, theme->paper);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                     NK_UI_SPACE_2);
        if (nk_ui_begin_container(ui, FLOW_ID_STAGES, &row, NULL)) {
            for (int i = 0; i < 4; ++i) {
                bool done = model->built || model->stage > i;
                bool current = !model->built && model->stage == i;
                NkUiLayout chip = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(),
                                              sizing_fixed(NK_UI_CHIP_HEIGHT + 4.0f), 6.0f);
                NkUiStyle style;
                char label[48];
                memset(&style, 0, sizeof(style));
                style.corner_radius = NK_UI_RADIUS_CHIP;
                style.border_width = NK_UI_BORDER_HAIRLINE;
                style.fill_rgba = current ? theme->accent_dark : theme->surface_high;
                style.border_rgba = current ? theme->accent_dark
                                            : done ? theme->ready : theme->line_strong;
                chip.align_x = NK_UI_ALIGN_CENTER;
                chip.align_y = NK_UI_ALIGN_CENTER;
                (void)snprintf(label, sizeof(label), "%s%s", done ? FLOW_MARK_CHECK : "",
                               stages[i]);
                if (nk_ui_begin_container(ui, (NkUiId)(FLOW_ID_STAGES + 1u + (NkUiId)i),
                                          &chip, &style)) {
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_STAGES + 0x100u + (NkUiId)i),
                                    nk_ui_caps(ui, label), NK_UI_TYPE_LABEL_S,
                                    current ? theme->ink : done ? theme->ready : theme->muted);
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
    }
    /* Determinate while the compile stage counts its parts; a sweep otherwise. */
    nk_ui_progress_bar(ui, FLOW_ID_BAR, inner, model->built ? 1.0f : model->fraction,
                       model->now_s);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                     NK_UI_SPACE_4);
        row.align_y = NK_UI_ALIGN_CENTER;
        if (nk_ui_begin_container(ui, FLOW_ID_STATUS, &row, NULL)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
            nk_ui_text(ui, FLOW_ID_MESSAGE,
                       nk_ui_text_clamp(ui, model->built
                           ? "Package written. Checking it before PLAY is offered."
                           : safe_text(model->message, "Building the package..."),
                           &text, inner - 140.0f, 2), &text);
            nk_ui_spacer(ui, FLOW_ID_STATUS + 0x100u, sizing_grow(), sizing_fixed(1.0f));
            (void)snprintf(elapsed, sizeof(elapsed), "%u.%u s",
                           model->elapsed_ms / 1000u, (model->elapsed_ms % 1000u) / 100u);
            nk_ui_text_role(ui, FLOW_ID_ELAPSED, elapsed, NK_UI_TYPE_DATA, theme->muted);
            nk_ui_end_container(ui);
        }
    }
    if (!model->built && model->notice != NULL && model->notice[0] != '\0') {
        /* A calm note that other apps may feel slower: muted body text, no warning
           colour or mark, because nothing is wrong. */
        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
        nk_ui_text(ui, FLOW_ID_NOTICE, nk_ui_text_clamp(ui, model->notice, &text, inner, 2),
                   &text);
    }
    {
        NkUiLayout log = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fixed(104.0f),
                                     2.0f);
        NkUiStyle style = flow_box_style(theme, 0x080B0FFFu);
        log.padding_top = log.padding_bottom = 10.0f;
        log.padding_left = log.padding_right = 14.0f;
        if (nk_ui_begin_container(ui, FLOW_ID_LOG, &log, &style)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
            text.line_height = 20.0f;
            for (size_t i = 0u; i < model->line_count && i < 4u; ++i) {
                nk_ui_text(ui, (NkUiId)(FLOW_ID_LOG + 1u + (NkUiId)i),
                           nk_ui_text_clamp(ui, safe_text(model->lines[i], " "), &text,
                                            inner - 28.0f, 1), &text);
            }
            if (model->line_count == 0u) {
                nk_ui_text(ui, FLOW_ID_LOG + 8u, "Waiting for the first build output...", &text);
            }
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_BUILD_ACTION,
                             model->built ? "Back to library" : "Cancel build",
                             model->built ? NK_UI_BUTTON_PRIMARY : NK_UI_BUTTON_DANGER)) {
                result = action(model->built ? PLAYER_UI_ACTION_LEAVE_BUILD
                                             : PLAYER_UI_ACTION_CANCEL_BUILD,
                                0u, PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_end_container(ui);
        }
    }
    flow_end(ui, "The game is translated and compiled on this PC. The first build takes a while.",
             model->built ? prompts_done : prompts_running, 2u);
    return result;
}

static void flow_bytes(char *out, size_t cap, unsigned long long bytes) {
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        (void)snprintf(out, cap, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        (void)snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        (void)snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
    } else {
        (void)snprintf(out, cap, "%llu bytes", bytes);
    }
}

/* Remove from library (#1304): the game, what Nakagawa built for it with
   sizes, the promise about the disc image, and the saves box, which stays
   off until the user ticks it. Cancel holds focus first. */
PlayerUiAction player_ui_screen_remove(NkUi *ui, const PlayerUiRemoveModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Cancel"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char text[320];
    char size[32];
    unsigned long long total = 0ull;
    size_t shown;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    inner = flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 640.0f);
    if (inner <= 0.0f) return result;
    flow_eyebrow(ui, FLOW_MARK_TRIANGLE, "Remove from library", theme->warning);
    (void)snprintf(text, sizeof(text), "Remove %s?", safe_text(model->title, "this game"));
    nk_ui_text_role(ui, FLOW_ID_TITLE, nk_ui_frame_text(ui, text), NK_UI_TYPE_TITLE,
                    theme->paper);
    shown = model->items != NULL ? model->item_count : 0u;
    if (shown > PLAYER_UI_REMOVE_ITEM_CAP) shown = PLAYER_UI_REMOVE_ITEM_CAP;
    nk_ui_text_role(ui, FLOW_ID_LEAD,
                    shown > 0u
                        ? "It leaves your library, and Nakagawa deletes what it built for it:"
                        : "It leaves your library. Nakagawa has not built anything for it yet.",
                    NK_UI_TYPE_BODY_S, theme->muted);
    if (shown > 0u) {
        NkUiLayout rows = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 4.0f);
        NkUiStyle style = flow_box_style(theme, theme->surface);
        rows.padding_top = rows.padding_bottom = 10.0f;
        rows.padding_left = rows.padding_right = 14.0f;
        if (nk_ui_begin_container(ui, FLOW_ID_ROWS, &rows, &style)) {
            for (size_t i = 0u; i < shown; ++i) {
                NkUiLayout line = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_3);
                total += model->items[i].bytes;
                if (nk_ui_begin_container(ui, (NkUiId)(FLOW_ID_ROWS + 0x10u + (NkUiId)i),
                                          &line, NULL)) {
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ROWS + 0x20u + (NkUiId)i),
                                    safe_text(model->items[i].label, "Built files"),
                                    NK_UI_TYPE_BODY_S, theme->paper);
                    nk_ui_spacer(ui, (NkUiId)(FLOW_ID_ROWS + 0x30u + (NkUiId)i),
                                 sizing_grow(), sizing_fixed(1.0f));
                    flow_bytes(size, sizeof(size), model->items[i].bytes);
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ROWS + 0x40u + (NkUiId)i),
                                    nk_ui_frame_text(ui, size), NK_UI_TYPE_DATA, theme->muted);
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
        flow_bytes(size, sizeof(size), total);
        (void)snprintf(text, sizeof(text), "Frees %s", size);
        nk_ui_text_role(ui, FLOW_ID_TOTAL, nk_ui_caps(ui, text), NK_UI_TYPE_LABEL_S,
                        theme->warning);
    }
    nk_ui_text_role(ui, FLOW_ID_DETAILS,
                    FLOW_MARK_CHECK "The disc image on your PC is not touched.",
                    NK_UI_TYPE_BODY_S, theme->ready);
    if (model->has_saves) {
        flow_bytes(size, sizeof(size), model->saves_bytes);
        (void)snprintf(text, sizeof(text), "%sAlso delete this game's saves (%s)",
                       model->delete_saves ? FLOW_MARK_CHECK : "", size);
        if (nk_ui_button(ui, PLAYER_UI_ID_REMOVE_SAVES, nk_ui_frame_text(ui, text), 0u)) {
            result = action(PLAYER_UI_ACTION_REMOVE_TOGGLE_SAVES, model->title_index,
                            PLAYER_UI_PRIMARY_NONE);
        }
        nk_ui_text_role(ui, FLOW_ID_MESSAGE,
                        model->delete_saves ? "Your saves for this game will be deleted too."
                                            : "Your saves stay.",
                        NK_UI_TYPE_BODY_S, model->delete_saves ? theme->error : theme->muted);
    } else {
        nk_ui_text_role(ui, FLOW_ID_MESSAGE, "This game has no saves.", NK_UI_TYPE_BODY_S,
                        theme->muted);
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_REMOVE_CANCEL, "Cancel", 0u)) {
                result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                                PLAYER_UI_PRIMARY_NONE);
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_REMOVE_CONFIRM, "Remove", NK_UI_BUTTON_DANGER)) {
                result = action(PLAYER_UI_ACTION_REMOVE_CONFIRM, model->title_index,
                                model->delete_saves ? (PlayerUiPrimaryAction)1
                                                    : PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_end_container(ui);
        }
    }
    flow_end(ui, "", prompts, 3u);
    return result;
}

PlayerUiAction player_ui_screen_prereq(NkUi *ui, const PlayerUiPrereqModel *model) {
    static const NkUiPromptItem prompts_consent[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Cancel"}
    };
    static const NkUiPromptItem prompts_progress[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"}, {NK_UI_PROMPT_ACT_BACK, "Cancel"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char text[320];
    char a[32];
    char b[32];
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    inner = flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 820.0f);
    if (inner <= 0.0f) return result;
    if (!model->in_progress) {
        size_t shown = model->item_count;
        flow_eyebrow(ui, FLOW_MARK_DIAMOND, "One-time build tool download", theme->info);
        nk_ui_text_role(ui, FLOW_ID_TITLE,
                        "Download the pinned tools needed to build this game?",
                        NK_UI_TYPE_TITLE, theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "Nothing is downloaded until you choose DOWNLOAD, and your answer applies "
                        "to this build only. Each file is checked against its pinned hash.",
                        NK_UI_TYPE_BODY_S, theme->muted);
        {
            NkUiLayout rows = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 0.0f);
            NkUiStyle style = flow_box_style(theme, theme->surface);
            rows.padding_top = rows.padding_bottom = 8.0f;
            rows.padding_left = rows.padding_right = 14.0f;
            if (shown > 9u) shown = 9u;
            if (nk_ui_begin_container(ui, FLOW_ID_ROWS, &rows, &style)) {
                NkUiTextStyle line = nk_ui_type(NK_UI_TYPE_DATA, theme->paper);
                line.line_height = 20.0f;
                for (size_t i = 0u; i < shown; ++i) {
                    const PlayerUiPrereqItem *item = &model->items[i];
                    const char *license = item->license != NULL &&
                        strcmp(item->license, "NOASSERTION") == 0
                        ? "licence review in the works" : safe_text(item->license, "");
                    flow_bytes(a, sizeof(a), item->bytes);
                    (void)snprintf(text, sizeof(text), "%s %s" FLOW_MIDDOT "%s" FLOW_MIDDOT "%s"
                                   FLOW_MIDDOT "%s",
                                   safe_text(item->name, "?"), safe_text(item->version, ""),
                                   safe_text(item->host, ""), a, license);
                    nk_ui_text(ui, (NkUiId)(FLOW_ID_ROWS + 1u + (NkUiId)i),
                               nk_ui_text_clamp(ui, text, &line, inner - 28.0f, 1), &line);
                }
                if (model->item_count > shown) {
                    (void)snprintf(text, sizeof(text), "and %zu more", model->item_count - shown);
                    nk_ui_text(ui, FLOW_ID_ROWS + 0x40u, text, &line);
                }
                nk_ui_end_container(ui);
            }
        }
        flow_bytes(a, sizeof(a), model->total_bytes);
        (void)snprintf(text, sizeof(text), "Total %s across %zu components", a, model->item_count);
        nk_ui_text_role(ui, FLOW_ID_TOTAL, nk_ui_caps(ui, text), NK_UI_TYPE_LABEL_S,
                        theme->warning);
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_PREREQ_ACCEPT, "Download",
                                 NK_UI_BUTTON_PRIMARY)) {
                    result = action(PLAYER_UI_ACTION_PREREQ_ACCEPT, 0u, PLAYER_UI_PRIMARY_NONE);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_PREREQ_CANCEL, "Cancel", 0u)) {
                    result = action(PLAYER_UI_ACTION_PREREQ_CANCEL, 0u, PLAYER_UI_PRIMARY_NONE);
                }
                nk_ui_end_container(ui);
            }
        }
        flow_end(ui, "", prompts_consent, 3u);
        return result;
    }
    flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Installing build tools", theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, "The files are checked before they are installed.",
                    NK_UI_TYPE_TITLE, theme->paper);
    (void)snprintf(text, sizeof(text), "Now: %s",
                   safe_text(model->current_item, "Starting the verified fetcher"));
    nk_ui_text_role(ui, FLOW_ID_LEAD, text, NK_UI_TYPE_BODY_S, theme->paper);
    nk_ui_progress_bar(ui, FLOW_ID_BAR, inner,
                       model->total_bytes > 0ull
                           ? (float)((double)model->total_received_bytes /
                                     (double)model->total_bytes) : -1.0f, 0.0);
    flow_bytes(a, sizeof(a), model->total_received_bytes);
    flow_bytes(b, sizeof(b), model->total_bytes);
    (void)snprintf(text, sizeof(text), "%s of %s", a, b);
    nk_ui_text_role(ui, FLOW_ID_TOTAL, text, NK_UI_TYPE_DATA, theme->muted);
    flow_bytes(a, sizeof(a), model->item_received_bytes);
    flow_bytes(b, sizeof(b), model->item_total_bytes);
    (void)snprintf(text, sizeof(text), "This item: %s of %s", a, b);
    nk_ui_text_role(ui, FLOW_ID_DETAILS, text, NK_UI_TYPE_DATA, theme->dim);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_PREREQ_CANCEL, "Cancel", 0u)) {
                result = action(PLAYER_UI_ACTION_PREREQ_CANCEL, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_end_container(ui);
        }
    }
    flow_end(ui, "The build starts by itself once the tools are installed.", prompts_progress, 2u);
    return result;
}

PlayerUiAction player_ui_screen_error(NkUi *ui, const PlayerUiErrorModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"}, {NK_UI_PROMPT_ACT_BACK, "Back to library"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    float height = model != NULL && model->logical_height > 80.0f ? model->logical_height : 800.0f;
    bool has_details = model != NULL && model->details != NULL && model->details[0] != '\0';
    int detail_lines;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    /* A details box (a path list, a boundary) gets a wider card, and every 16 px
       of height past the 540 px minimum window buys one more line of it, so the
       fix it names is not cut off. */
    inner = flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       height, has_details ? 880.0f : 700.0f);
    if (inner <= 0.0f) return result;
    detail_lines = 6 + (height > 540.0f ? (int)((height - 540.0f) / 16.0f) : 0);
    if (detail_lines > 16) detail_lines = 16;
    flow_eyebrow(ui, FLOW_MARK_CROSS, safe_text(model->badge, "Error"), theme->error);
    nk_ui_text_role(ui, FLOW_ID_TITLE, safe_text(model->title, "Something went wrong"),
                    NK_UI_TYPE_TITLE, theme->paper);
    if (model->failed_stage != NULL && model->failed_stage[0] != '\0') {
        char stage[96];
        (void)snprintf(stage, sizeof(stage), "Failed stage: %s", model->failed_stage);
        nk_ui_status_pill(ui, FLOW_ID_STAGES, FLOW_MARK_TRIANGLE, stage, theme->warning);
    }
    if (model->message != NULL && model->message[0] != '\0') {
        nk_ui_text_role(ui, FLOW_ID_MESSAGE, model->message, NK_UI_TYPE_BODY, theme->muted);
    }
    if ((model->details != NULL && model->details[0] != '\0') ||
        (model->log_path != NULL && model->log_path[0] != '\0')) {
        NkUiLayout box = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 4.0f);
        NkUiStyle style = flow_box_style(theme, theme->surface);
        box.padding_top = box.padding_bottom = 12.0f;
        box.padding_left = box.padding_right = 14.0f;
        if (nk_ui_begin_container(ui, FLOW_ID_DETAILS, &box, &style)) {
            NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
            data.wrap = true;
            if (model->details != NULL && model->details[0] != '\0') {
                nk_ui_eyebrow(ui, FLOW_ID_DETAILS + 1u, "Details");
                nk_ui_text(ui, FLOW_ID_DETAILS + 2u,
                           nk_ui_text_clamp(ui, model->details, &data, inner - 28.0f,
                                            detail_lines), &data);
            }
            if (model->log_path != NULL && model->log_path[0] != '\0') {
                char log[600];
                (void)snprintf(log, sizeof(log), "Log file: %s", model->log_path);
                nk_ui_text(ui, FLOW_ID_DETAILS + 3u,
                           nk_ui_text_clamp(ui, log, &data, inner - 28.0f, 2), &data);
            }
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_ERROR_RECOVER,
                             safe_text(model->recovery_label, "Back to library"),
                             NK_UI_BUTTON_PRIMARY)) {
                result = action(PLAYER_UI_ACTION_ERROR_RECOVER, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            if (model->retry_label != NULL && model->retry_label[0] != '\0' &&
                nk_ui_button(ui, PLAYER_UI_ID_ERROR_RETRY, model->retry_label, 0u)) {
                result = action(PLAYER_UI_ACTION_ERROR_RETRY, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            if (model->copy_available &&
                nk_ui_button(ui, PLAYER_UI_ID_ERROR_COPY,
                             model->copied ? "Details copied" : "Copy details", 0u)) {
                result = action(PLAYER_UI_ACTION_ERROR_COPY, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_end_container(ui);
        }
    }
    flow_end(ui, model->copy_available
                     ? "Copy details puts a short report on the clipboard. Nothing is sent "
                       "anywhere."
                     : "",
             prompts, 2u);
    return result;
}

/* ---- About -------------------------------------------------------------------
   The page a viewer can read in one look and explain in one sentence: what the
   program is, which build this is, its licence and the notices, the credits,
   and that the project is independent. */

#define ABOUT_ID_ROOT 0x50000001u
#define ABOUT_ID_TOP 0x50000010u
#define ABOUT_ID_BODY 0x50000020u
#define ABOUT_ID_COLUMN 0x50000021u
#define ABOUT_ID_HEAD 0x50000022u
#define ABOUT_ID_MARK 0x50000023u
#define ABOUT_ID_NAMES 0x50000024u
#define ABOUT_ID_NAME 0x50000025u
#define ABOUT_ID_VERSION 0x50000026u
#define ABOUT_ID_LEAD 0x50000027u
#define ABOUT_ID_CARDS 0x50000028u
#define ABOUT_ID_LICENSE 0x50000029u
#define ABOUT_ID_CREDITS 0x5000002Au
#define ABOUT_ID_NOTICE 0x5000002Bu
#define ABOUT_ID_LINK 0x5000002Cu
#define ABOUT_ID_BUILD 0x5000002Du
#define ABOUT_ID_PROMPTS 0x50000040u
#define ABOUT_ID_WAVES 0x50000050u

static void about_lines(NkUi *ui, NkUiId base, const char *const *lines, size_t count,
                        NkUiTypeRole role, uint32_t rgba) {
    for (size_t i = 0u; i < count; ++i) {
        nk_ui_text_role(ui, (NkUiId)(base + (NkUiId)i), lines[i], role, rgba);
    }
}

PlayerUiAction player_ui_screen_about(NkUi *ui, const PlayerUiAboutModel *model) {
    static const char *const credits[] = {
        "Began as a fork of sal063's PSP Recompilation Project (GPL-2.0-or-later).",
        "Carries PPSSPP lineage, FFmpeg's ATRAC3 decoders and the Clay layout library.",
        "Drawn with SDL and SDL_ttf, and set in Chakra Petch, Geist and Noto Sans JP."
    };
    const NkUiTheme *theme;
    NkUiLayout layout;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float width;
    float height;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;

    (void)nk_ui_waves_element(ui, ABOUT_ID_WAVES, (NkUiRect){0.0f, 0.0f, width, height}, 0u);
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, ABOUT_ID_ROOT, &layout, NULL)) {
        return result;
    }
    {
        PlayerUiActionKind tab = player_ui_top_bar(ui, ABOUT_ID_TOP, PLAYER_UI_ROUTE_ABOUT);
        if (tab != PLAYER_UI_ACTION_NONE) result = action(tab, 0u, PLAYER_UI_PRIMARY_NONE);
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_grow(), 0.0f);
    layout.align_x = NK_UI_ALIGN_CENTER;
    layout.padding_top = NK_UI_SPACE_8;
    layout.padding_left = layout.padding_right = NK_UI_GUTTER;
    if (nk_ui_begin_container(ui, ABOUT_ID_BODY, &layout, NULL)) {
        float column = width - 2.0f * NK_UI_GUTTER;
        if (column > 880.0f) column = 880.0f;
        layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(column), sizing_fit(),
                             NK_UI_SPACE_6);
        if (nk_ui_begin_container(ui, ABOUT_ID_COLUMN, &layout, NULL)) {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                         NK_UI_SPACE_6);
            row.align_y = NK_UI_ALIGN_CENTER;
            if (nk_ui_begin_container(ui, ABOUT_ID_HEAD, &row, NULL)) {
                NkUiLayout names = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(),
                                               sizing_fit(), NK_UI_SPACE_2);
                nk_ui_shapes(ui, ABOUT_ID_MARK, sizing_fixed(72.0f), sizing_fixed(72.0f),
                             nk_ui_art_n1_mark());
                if (nk_ui_begin_container(ui, ABOUT_ID_NAMES, &names, NULL)) {
                    nk_ui_text_role(ui, ABOUT_ID_NAME, "NAKAGAWA RECOMP", NK_UI_TYPE_DISPLAY_XL,
                                    theme->paper);
                    nk_ui_text_role(ui, ABOUT_ID_VERSION,
                                    safe_text(model->version_line, "v" NK_PLAYER_VERSION),
                                    NK_UI_TYPE_LABEL_L, theme->accent_dark);
                    nk_ui_end_container(ui);
                }
                nk_ui_end_container(ui);
            }
            nk_ui_text_role(ui, ABOUT_ID_LEAD,
                            "A PSP static recompiler: your own game, recompiled to run natively. "
                            "Your games stay on this PC. Nakagawa never uploads "
                            "anything, and downloads only the build tools you approve.",
                            NK_UI_TYPE_BODY_L, theme->muted);
            row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
            if (nk_ui_begin_container(ui, ABOUT_ID_CARDS, &row, NULL)) {
                if (nk_ui_card_begin(ui, ABOUT_ID_LICENSE, sizing_fixed(300.0f), sizing_grow(),
                                     NK_UI_SPACE_3)) {
                    nk_ui_eyebrow(ui, 0x50000200u, "Licence");
                    nk_ui_text_role(ui, 0x50000201u, "GPL-3.0-or-later",
                                    NK_UI_TYPE_HEADING, theme->paper);
                    nk_ui_text_role(ui, 0x50000202u,
                                    "Free software. Third-party parts keep their own licences.",
                                    NK_UI_TYPE_CAPTION, theme->muted);
                    if (nk_ui_button(ui, PLAYER_UI_ID_ABOUT_NOTICES, "Third-party notices", 0u)) {
                        result = action(PLAYER_UI_ACTION_OPEN_NOTICES, 0u, PLAYER_UI_PRIMARY_NONE);
                    }
                    nk_ui_end_container(ui);
                }
                if (nk_ui_card_begin(ui, ABOUT_ID_CREDITS, sizing_grow(), sizing_grow(),
                                     NK_UI_SPACE_2)) {
                    nk_ui_eyebrow(ui, 0x50000300u, "Credits");
                    about_lines(ui, 0x50000310u, credits,
                                sizeof(credits) / sizeof(credits[0]), NK_UI_TYPE_BODY_S,
                                theme->paper);
                    nk_ui_text_role(ui, 0x500003F0u,
                                    "The full list is in CREDITS.md and NOTICE.md.",
                                    NK_UI_TYPE_FOOTNOTE, theme->dim);
                    nk_ui_end_container(ui);
                }
                nk_ui_end_container(ui);
            }
            nk_ui_text_role(ui, ABOUT_ID_NOTICE,
                            "Not affiliated with or endorsed by Sony Interactive Entertainment. "
                            "PSP is a trademark of its owner. No game data is included.",
                            NK_UI_TYPE_CAPTION, theme->muted);
            nk_ui_text_role(ui, ABOUT_ID_LINK, "github.com/Jstar269/nakagawa-recomp",
                            NK_UI_TYPE_DATA, theme->paper);
            if (model->build_commit != NULL && model->build_commit[0] != '\0') {
                char build[64];
                (void)snprintf(build, sizeof(build), "Source commit %s", model->build_commit);
                nk_ui_text_role(ui, ABOUT_ID_BUILD, build, NK_UI_TYPE_FOOTNOTE, theme->dim);
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    {
        static const NkUiPromptItem prompts[] = {
            {NK_UI_PROMPT_ACT_MOVE, "Move"},
            {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
            {NK_UI_PROMPT_ACT_BACK, "Back"}
        };
        nk_ui_prompt_bar(ui, ABOUT_ID_PROMPTS, "", prompts,
                         sizeof(prompts) / sizeof(prompts[0]));
    }
    nk_ui_end_container(ui);
    return result;
}

/* ---- Setup wizard ---------------------------------------------------------------
   The first-run path in five steps (Welcome, Select game, Verify, Fonts and
   system, Ready) on the setup wizard state in player_state.c. Every control
   returns PLAYER_UI_ACTION_WIZARD with the op main.c runs, the same handlers
   the existing renderer's wizard buttons call. */

#define WIZ_ID_STEPS 0x61000001u
#define WIZ_ID_BOX 0x61000010u
#define WIZ_ID_ROWS 0x61000200u
#define WIZ_ID_BUTTONS 0x61000030u
#define WIZ_ID_EXTRA 0x61000031u
#define WIZ_ID_TEXT 0x61001000u

static PlayerUiAction wizard_op(PlayerUiWizardOp op) {
    PlayerUiAction result;
    result.kind = PLAYER_UI_ACTION_WIZARD;
    result.primary = (PlayerUiPrimaryAction)op;
    result.title_index = 0u;
    return result;
}

static void wizard_text(NkUi *ui, uint32_t k, const char *text, NkUiTypeRole role, uint32_t rgba) {
    if (text != NULL && text[0] != '\0') {
        nk_ui_text_role(ui, (NkUiId)(WIZ_ID_TEXT + k), text, role, rgba);
    }
}

static void wizard_steps(NkUi *ui, int step) {
    static const char *const names[6] = {"Welcome", "Options", "Game", "Verify", "Fonts",
                                         "Ready"};
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_2);
    if (!nk_ui_begin_container(ui, WIZ_ID_STEPS, &row, NULL)) return;
    for (int i = 0; i < 6; ++i) {
        bool done = i < step;
        bool current = i == step;
        NkUiLayout chip = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(),
                                      sizing_fixed(NK_UI_CHIP_HEIGHT + 4.0f), 6.0f);
        NkUiStyle style;
        char label[48];
        memset(&style, 0, sizeof(style));
        style.corner_radius = NK_UI_RADIUS_CHIP;
        style.border_width = NK_UI_BORDER_HAIRLINE;
        style.fill_rgba = current ? theme->accent_dark : theme->surface_high;
        style.border_rgba = current ? theme->accent_dark
                                    : done ? theme->ready : theme->line_strong;
        chip.align_x = NK_UI_ALIGN_CENTER;
        chip.align_y = NK_UI_ALIGN_CENTER;
        if (done) {
            (void)snprintf(label, sizeof(label), FLOW_MARK_CHECK "%s", names[i]);
        } else {
            (void)snprintf(label, sizeof(label), "%d  %s", i + 1, names[i]);
        }
        if (nk_ui_begin_container(ui, (NkUiId)(WIZ_ID_STEPS + 1u + (NkUiId)i), &chip, &style)) {
            nk_ui_text_role(ui, (NkUiId)(WIZ_ID_STEPS + 0x100u + (NkUiId)i),
                            nk_ui_caps(ui, label), NK_UI_TYPE_LABEL_S,
                            current ? theme->ink : done ? theme->ready : theme->muted);
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

/* A dark inset panel for the step's facts. Returns true when opened. */
static bool wizard_box_begin(NkUi *ui, uint32_t edge) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout box = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 6.0f);
    NkUiStyle style = flow_box_style(theme, theme->surface);
    if (edge != 0u) style.border_rgba = edge;
    box.padding_top = box.padding_bottom = 14.0f;
    box.padding_left = box.padding_right = 16.0f;
    return nk_ui_begin_container(ui, WIZ_ID_BOX, &box, &style);
}

/* One option switch: a button reading "Name: On" or "Name: Off" (accent while
   on) with its sentence beside it. Returns true when activated. */
static bool wizard_switch(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                          const char *sentence, bool on, float width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
    char label[48];
    bool pressed = false;
    row.align_y = NK_UI_ALIGN_CENTER;
    (void)snprintf(label, sizeof(label), "%s: %s", name, on ? "On" : "Off");
    if (!nk_ui_begin_container(ui, text_id, &row, NULL)) return false;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(220.0f), sizing_fit(), 0.0f);
        if (nk_ui_begin_container(ui, text_id + 1u, &cell, NULL)) {
            pressed = nk_ui_button(ui, id, label,
                                   NK_UI_BUTTON_GROW | (on ? NK_UI_BUTTON_PRIMARY : 0u));
            nk_ui_end_container(ui);
        }
    }
    nk_ui_text(ui, text_id + 2u,
               nk_ui_text_clamp(ui, sentence, &text, width - 220.0f - NK_UI_SPACE_4, 2), &text);
    nk_ui_end_container(ui);
    return pressed;
}

typedef struct {
    NkUiId id;
    const char *label;
    uint32_t flags;
    PlayerUiWizardOp op;
} WizardButton;

static void wizard_buttons(NkUi *ui, NkUiId row_id, const WizardButton *buttons, size_t count,
                           PlayerUiAction *result) {
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
    row.padding_top = NK_UI_SPACE_1;
    if (!nk_ui_begin_container(ui, row_id, &row, NULL)) return;
    for (size_t i = 0u; i < count; ++i) {
        if (nk_ui_button(ui, buttons[i].id, buttons[i].label, buttons[i].flags)) {
            *result = wizard_op(buttons[i].op);
        }
    }
    nk_ui_end_container(ui);
}

PlayerUiAction player_ui_screen_wizard(NkUi *ui, const PlayerUiWizardModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char line[256];
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    inner = flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 860.0f);
    if (inner <= 0.0f) return result;
    wizard_steps(ui, model->step);
    switch (model->step) {
    case 0: {
        static const WizardButton buttons[] = {
            {PLAYER_UI_ID_WIZARD_PRIMARY, "Get started", NK_UI_BUTTON_PRIMARY | NK_UI_BUTTON_LARGE,
             PLAYER_UI_WIZARD_NEXT},
            {PLAYER_UI_ID_WIZARD_CANCEL, "Skip to library", NK_UI_BUTTON_LARGE,
             PLAYER_UI_WIZARD_CANCEL}
        };
        wizard_text(ui, 0u, "Set up your first game", NK_UI_TYPE_TITLE_XL, theme->paper);
        wizard_text(ui, 1u,
                    "This wizard checks a disc image of a PSP game you own, stages its files on "
                    "this PC, and gets it ready to build and play.",
                    NK_UI_TYPE_BODY_L, theme->muted);
        if (wizard_box_begin(ui, 0u)) {
            nk_ui_eyebrow(ui, WIZ_ID_TEXT + 2u, "What you need");
            wizard_text(ui, 3u, "A decrypted .iso of a PSP game you own", NK_UI_TYPE_BODY_S,
                        theme->paper);
            wizard_text(ui, 4u, "A PC with a recent graphics driver", NK_UI_TYPE_BODY_S,
                        theme->paper);
            wizard_text(ui, 5u, "Keyboard and mouse, or a controller", NK_UI_TYPE_BODY_S,
                        theme->paper);
            nk_ui_end_container(ui);
        }
        wizard_text(ui, 6u,
                    "Your games stay on this PC. Nakagawa never uploads anything, and downloads "
                    "only the build tools you approve. It decrypts your own disc here and never "
                    "touches firmware.",
                    NK_UI_TYPE_CAPTION, theme->muted);
        wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 2u, &result);
        break;
    }
    case 1: {
        static const WizardButton buttons[] = {
            {PLAYER_UI_ID_WIZARD_PRIMARY, "Continue", NK_UI_BUTTON_PRIMARY,
             PLAYER_UI_WIZARD_NEXT},
            {PLAYER_UI_ID_WIZARD_SECOND, "Skip", 0u, PLAYER_UI_WIZARD_SKIP_OPTIONS},
            {PLAYER_UI_ID_WIZARD_BACK, "Back", 0u, PLAYER_UI_WIZARD_BACK}
        };
        wizard_text(ui, 0u, "Online and developer options", NK_UI_TYPE_TITLE_XL, theme->paper);
        wizard_text(ui, 1u, "Both start off. You can change either one later in Settings.",
                    NK_UI_TYPE_BODY, theme->muted);
        if (wizard_box_begin(ui, 0u)) {
            if (wizard_switch(ui, PLAYER_UI_ID_WIZARD_ONLINE, WIZ_ID_TEXT + 0x80u, "Online mode",
                              "Check nakagawa.jaycast.net for updates and news, and let you send "
                              "reports. Nothing is sent without you.",
                              model->online_mode, inner - 32.0f)) {
                result = wizard_op(PLAYER_UI_WIZARD_TOGGLE_ONLINE);
            }
            if (wizard_switch(ui, PLAYER_UI_ID_WIZARD_DEVELOPER, WIZ_ID_TEXT + 0x90u,
                              "Developer mode",
                              "Show a developer log on screen to help track down problems. "
                              "Stays on this PC.",
                              model->developer_mode, inner - 32.0f)) {
                result = wizard_op(PLAYER_UI_WIZARD_TOGGLE_DEVELOPER);
            }
            nk_ui_end_container(ui);
        }
        wizard_text(ui, 2u,
                    "This version has no online features yet and makes no connection. Your "
                    "choice is kept for when they arrive.",
                    NK_UI_TYPE_CAPTION, theme->warning);
        wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 3u, &result);
        break;
    }
    case 2: {
        static const WizardButton choose[] = {
            {PLAYER_UI_ID_WIZARD_PRIMARY, "Choose a disc image", NK_UI_BUTTON_PRIMARY,
             PLAYER_UI_WIZARD_CHOOSE_ISO},
            {PLAYER_UI_ID_WIZARD_BACK, "Back", 0u, PLAYER_UI_WIZARD_BACK},
            {PLAYER_UI_ID_WIZARD_CANCEL, "Cancel", 0u, PLAYER_UI_WIZARD_CANCEL}
        };
        static const WizardButton proceed[] = {
            {PLAYER_UI_ID_WIZARD_PRIMARY, "Verify this disc", NK_UI_BUTTON_PRIMARY,
             PLAYER_UI_WIZARD_NEXT},
            {PLAYER_UI_ID_WIZARD_SECOND, "Choose a different one", 0u,
             PLAYER_UI_WIZARD_CHOOSE_DIFFERENT},
            {PLAYER_UI_ID_WIZARD_BACK, "Back", 0u, PLAYER_UI_WIZARD_BACK},
            {PLAYER_UI_ID_WIZARD_CANCEL, "Cancel", 0u, PLAYER_UI_WIZARD_CANCEL}
        };
        wizard_text(ui, 0u, "Choose your disc image", NK_UI_TYPE_TITLE_XL, theme->paper);
        wizard_text(ui, 1u,
                    "Pick the .iso of a PSP game you own. Nakagawa reads its file system and "
                    "PARAM.SFO to find out which game it is.",
                    NK_UI_TYPE_BODY, theme->muted);
        if (wizard_box_begin(ui, model->has_iso ? theme->ready : 0u)) {
            if (model->has_iso) {
                nk_ui_status_pill(ui, WIZ_ID_TEXT + 2u, FLOW_MARK_DOT, "Disc image chosen",
                                  theme->ready);
                wizard_text(ui, 3u, model->iso_detail, NK_UI_TYPE_DATA, theme->muted);
                {
                    NkUiTextStyle path = nk_ui_type(NK_UI_TYPE_DATA, theme->paper);
                    nk_ui_text(ui, WIZ_ID_TEXT + 4u,
                               nk_ui_text_clamp(ui, safe_text(model->iso_path, ""), &path,
                                                inner - 32.0f, 2), &path);
                }
                wizard_text(ui, 5u, safe_text(model->status_message, "Ready to verify."),
                            NK_UI_TYPE_BODY_S, theme->ready);
            } else {
                nk_ui_status_pill(ui, WIZ_ID_TEXT + 2u, FLOW_MARK_TRIANGLE, "No file chosen",
                                  theme->warning);
                if (model->status_message != NULL && model->status_message[0] != '\0') {
                    wizard_text(ui, 5u, model->status_message,
                                NK_UI_TYPE_BODY_S, theme->warning);
                } else {
                    wizard_text(ui, 5u,
                                "Choose a disc image below, or drop an .iso file onto this window.",
                                NK_UI_TYPE_BODY_S, theme->muted);
                }
            }
            nk_ui_end_container(ui);
        }
        if (model->iso_selected) {
            wizard_buttons(ui, WIZ_ID_BUTTONS, proceed, 4u, &result);
        } else {
            wizard_buttons(ui, WIZ_ID_BUTTONS, choose, 3u, &result);
        }
        break;
    }
    case 3: {
        bool blocked = model->experimental && model->blocked_reason != NULL &&
                       model->blocked_reason[0] != '\0';
        const char *badge = blocked ? "Not playable yet"
            : (model->experimental ? "Not tested yet"
               : (model->supported ? "Title profiled" : "Profile missing"));
        uint32_t tone = blocked ? theme->error
            : (model->experimental ? theme->warning
               : (model->supported ? theme->ready : theme->error));
        const char *note;
        uint32_t note_tone = theme->muted;
        if (model->extracting) {
            badge = model->converting_package ? "Converting package" : "Copying files";
            tone = theme->warning;
        } else if (model->extraction_complete) {
            badge = "Files staged";
        } else if (model->extraction_failed) {
            badge = "Copy failed";
            tone = theme->error;
        }
        if (model->extracting && model->converting_package) {
            note = "The PSN package is being turned back into its disc image on this PC. "
                   "This happens once.";
        } else if (model->extracting) {
            note = "The disc's files are being copied into a local staging folder.";
        } else if (model->extraction_failed && model->extraction_error != NULL &&
                   model->extraction_error[0] != '\0') {
            /* Why the copy failed and what to do next, in place of the catalog note. */
            note = model->extraction_error;
            note_tone = theme->error;
        } else if (blocked) {
            note = "This game can't be played in this version of Nakagawa yet.";
            note_tone = theme->error;
        } else if (model->experimental) {
            note = "Nakagawa hasn't been tested with this game, so it may not start or may "
                   "stop early. You can add it and try.";
            note_tone = theme->warning;
        } else if (model->supported) {
            note = "This disc is listed in the native title catalog.";
        } else {
            note = "This disc is not listed in the native title catalog.";
            note_tone = theme->warning;
        }
        wizard_text(ui, 0u, model->converting_package ? "Converting the PSN package"
                            : (model->extracting ? "Copying the game's files" : "Check the disc"),
                    NK_UI_TYPE_TITLE_XL, theme->paper);
        if (wizard_box_begin(ui, tone)) {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_2);
            if (nk_ui_begin_container(ui, WIZ_ID_TEXT + 0x20u, &row, NULL)) {
                nk_ui_status_pill(ui, WIZ_ID_TEXT + 0x21u, NULL, badge, tone);
                nk_ui_tag(ui, WIZ_ID_TEXT + 0x22u, safe_text(model->disc_id, "Unknown"));
                nk_ui_end_container(ui);
            }
            wizard_text(ui, 1u, safe_text(model->title_name, "PlayStation Portable title"),
                        NK_UI_TYPE_TITLE, theme->paper);
            wizard_text(ui, 2u, note, NK_UI_TYPE_BODY_S, note_tone);
            if (blocked && !model->extracting) {
                wizard_text(ui, 6u, model->blocked_reason, NK_UI_TYPE_BODY_S, theme->paper);
            }
            if (model->extracting && model->converting_package) {
                (void)snprintf(line, sizeof(line), "Converted %d%%", model->extraction_percent);
            } else if (model->extracting) {
                (void)snprintf(line, sizeof(line), "Copied %d of %d files" FLOW_MIDDOT "%d%%",
                               model->files_extracted, model->total_files,
                               model->extraction_percent);
            }
            if (model->extracting) {
                wizard_text(ui, 3u, line, NK_UI_TYPE_DATA, theme->paper);
                nk_ui_progress_bar(ui, WIZ_ID_TEXT + 0x30u, inner - 32.0f,
                                   (float)model->extraction_percent / 100.0f, 0.0);
                {
                    NkUiTextStyle file = nk_ui_type(NK_UI_TYPE_DATA, theme->dim);
                    nk_ui_text(ui, WIZ_ID_TEXT + 4u,
                               nk_ui_text_clamp(ui, safe_text(model->current_file,
                                                              "Preparing the disc image..."),
                                                &file, inner - 32.0f, 1), &file);
                }
            } else {
                for (size_t i = 0u; i < model->check_count && i < PLAYER_UI_WIZARD_CHECKS; ++i) {
                    const PlayerUiCheckRow *check = &model->checks[i];
                    NkUiLayout crow = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                                  NK_UI_SPACE_3);
                    NkUiId base = (NkUiId)(WIZ_ID_ROWS + (NkUiId)i * 8u);
                    if (nk_ui_begin_container(ui, base, &crow, NULL)) {
                        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(110.0f),
                                                      sizing_fit(), 0.0f);
                        NkUiTextStyle msg = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
                            nk_ui_text_role(ui, base + 4u, nk_ui_caps(ui, safe_text(check->status, "")),
                                            NK_UI_TYPE_LABEL_S, check->rgba);
                            nk_ui_end_container(ui);
                        }
                        cell.width = sizing_fixed(150.0f);
                        if (nk_ui_begin_container(ui, base + 2u, &cell, NULL)) {
                            nk_ui_text_role(ui, base + 5u, safe_text(check->code, ""),
                                            NK_UI_TYPE_DATA, theme->paper);
                            nk_ui_end_container(ui);
                        }
                        nk_ui_text(ui, base + 3u,
                                   nk_ui_text_clamp(ui, safe_text(check->message, ""), &msg,
                                                    inner - 32.0f - 284.0f, 1), &msg);
                        nk_ui_end_container(ui);
                    }
                }
            }
            nk_ui_end_container(ui);
        }
        if (model->extracting) {
            static const WizardButton copying[] = {
                {PLAYER_UI_ID_WIZARD_CANCEL, "Cancel copying", NK_UI_BUTTON_DANGER,
                 PLAYER_UI_WIZARD_CANCEL}
            };
            static const WizardButton converting[] = {
                {PLAYER_UI_ID_WIZARD_CANCEL, "Cancel converting", NK_UI_BUTTON_DANGER,
                 PLAYER_UI_WIZARD_CANCEL}
            };
            wizard_buttons(ui, WIZ_ID_BUTTONS,
                           model->converting_package ? converting : copying, 1u, &result);
        } else if (blocked) {
            /* Adding it would only fail later, at build time (#1280). */
            static const WizardButton buttons[] = {
                {PLAYER_UI_ID_WIZARD_PRIMARY, "Choose a different one", NK_UI_BUTTON_PRIMARY,
                 PLAYER_UI_WIZARD_CHOOSE_DIFFERENT},
                {PLAYER_UI_ID_WIZARD_CANCEL, "Cancel", 0u, PLAYER_UI_WIZARD_CANCEL}
            };
            wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 2u, &result);
        } else if (model->experimental) {
            static const WizardButton buttons[] = {
                {PLAYER_UI_ID_WIZARD_PRIMARY, "Add to library", NK_UI_BUTTON_PRIMARY,
                 PLAYER_UI_WIZARD_ADD_EXPERIMENTAL},
                {PLAYER_UI_ID_WIZARD_SECOND, "Choose a different one", 0u,
                 PLAYER_UI_WIZARD_CHOOSE_DIFFERENT},
                {PLAYER_UI_ID_WIZARD_CANCEL, "Cancel", 0u, PLAYER_UI_WIZARD_CANCEL}
            };
            wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 3u, &result);
        } else if (model->supported) {
            WizardButton buttons[3] = {
                {PLAYER_UI_ID_WIZARD_PRIMARY, "Copy files and continue", NK_UI_BUTTON_PRIMARY,
                 PLAYER_UI_WIZARD_NEXT},
                {PLAYER_UI_ID_WIZARD_BACK, "Back", 0u, PLAYER_UI_WIZARD_BACK},
                {PLAYER_UI_ID_WIZARD_CANCEL, "Cancel", 0u, PLAYER_UI_WIZARD_CANCEL}
            };
            if (model->extraction_complete) buttons[0].label = "Continue";
            else if (model->extraction_failed) buttons[0].label = "Try copying again";
            wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 3u, &result);
        } else {
            static const WizardButton buttons[] = {
                {PLAYER_UI_ID_WIZARD_PRIMARY, "Choose a different one", NK_UI_BUTTON_PRIMARY,
                 PLAYER_UI_WIZARD_CHOOSE_DIFFERENT},
                {PLAYER_UI_ID_WIZARD_BACK, "Back", 0u, PLAYER_UI_WIZARD_BACK},
                {PLAYER_UI_ID_WIZARD_CANCEL, "Cancel", 0u, PLAYER_UI_WIZARD_CANCEL}
            };
            wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 3u, &result);
        }
        break;
    }
    case 4: {
        static const WizardButton buttons[] = {
            {PLAYER_UI_ID_WIZARD_PRIMARY, "Accept and continue", NK_UI_BUTTON_PRIMARY,
             PLAYER_UI_WIZARD_ACCEPT_FONTS},
            {PLAYER_UI_ID_WIZARD_SECOND, "Change resolution", 0u,
             PLAYER_UI_WIZARD_CYCLE_RESOLUTION},
            {PLAYER_UI_ID_WIZARD_BACK, "Back", 0u, PLAYER_UI_WIZARD_BACK},
            {PLAYER_UI_ID_WIZARD_CANCEL, "Cancel", 0u, PLAYER_UI_WIZARD_CANCEL}
        };
        static const WizardButton font_buttons[] = {
            {PLAYER_UI_ID_WIZARD_IMPORT_FONTS, "Import fonts folder", NK_UI_BUTTON_SMALL,
             PLAYER_UI_WIZARD_IMPORT_FONTS},
            {PLAYER_UI_ID_WIZARD_REMOVE_FONTS, "Remove imported fonts", NK_UI_BUTTON_SMALL,
             PLAYER_UI_WIZARD_REMOVE_FONTS}
        };
        wizard_text(ui, 0u, "Fonts and display", NK_UI_TYPE_TITLE_XL, theme->paper);
        if (wizard_box_begin(ui, 0u)) {
            wizard_text(ui, 1u, model->resolution_line, NK_UI_TYPE_BODY, theme->paper);
            wizard_text(ui, 2u, model->controller_line, NK_UI_TYPE_BODY_S, theme->ready);
            nk_ui_eyebrow(ui, WIZ_ID_TEXT + 3u, "PSP font slots (fallbacks shown)");
            {
                NkUiLayout cols = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_4);
                if (nk_ui_begin_container(ui, WIZ_ID_TEXT + 0x40u, &cols, NULL)) {
                    float column = (inner - 32.0f - 2.0f * NK_UI_SPACE_4) / 3.0f;
                    for (int c = 0; c < 3; ++c) {
                        NkUiLayout col = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(column),
                                                     sizing_fit(), 2.0f);
                        if (!nk_ui_begin_container(ui, (NkUiId)(WIZ_ID_TEXT + 0x41u + (NkUiId)c),
                                                   &col, NULL)) {
                            continue;
                        }
                        for (size_t r = 0u; r < 6u; ++r) {
                            size_t item = (size_t)c * 6u + r;
                            NkUiTextStyle font = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                            if (item >= model->font_line_count) break;
                            nk_ui_text(ui, (NkUiId)(WIZ_ID_TEXT + 0x60u + (NkUiId)item),
                                       nk_ui_text_clamp(ui, safe_text(model->font_lines[item], ""),
                                                        &font, column, 1), &font);
                        }
                        nk_ui_end_container(ui);
                    }
                    nk_ui_end_container(ui);
                }
            }
            wizard_text(ui, 4u,
                        safe_text(model->font_message,
                                  "Importing reads the .pgf files in a folder you choose."),
                        NK_UI_TYPE_CAPTION, theme->warning);
            wizard_buttons(ui, WIZ_ID_EXTRA, font_buttons, 2u, &result);
            nk_ui_end_container(ui);
        }
        wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 4u, &result);
        break;
    }
    case 5:
    default: {
        WizardButton buttons[3] = {
            {PLAYER_UI_ID_WIZARD_PRIMARY, "Play",
             NK_UI_BUTTON_PRIMARY | NK_UI_BUTTON_LARGE, PLAYER_UI_WIZARD_FINISH},
            {PLAYER_UI_ID_WIZARD_SECOND, "Go to library", NK_UI_BUTTON_LARGE,
             PLAYER_UI_WIZARD_GO_TO_LIBRARY},
            {PLAYER_UI_ID_WIZARD_BACK, "Back", NK_UI_BUTTON_LARGE, PLAYER_UI_WIZARD_BACK}
        };
        if (!model->runtime_ready) buttons[0].label = "Save to library";
        wizard_text(ui, 0u, model->runtime_ready ? "Ready to play"
                                                 : "Files staged: build the package next",
                    NK_UI_TYPE_TITLE_XL, theme->paper);
        if (wizard_box_begin(ui, model->runtime_ready ? theme->ready : theme->warning)) {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_2);
            if (nk_ui_begin_container(ui, WIZ_ID_TEXT + 0x20u, &row, NULL)) {
                nk_ui_status_pill(ui, WIZ_ID_TEXT + 0x21u,
                                  model->runtime_ready ? FLOW_MARK_DOT : FLOW_MARK_TRIANGLE,
                                  model->runtime_ready ? "Ready to play" : "Files staged",
                                  model->runtime_ready ? theme->ready : theme->warning);
                if (model->disc_id != NULL && model->disc_id[0] != '\0') {
                    nk_ui_tag(ui, WIZ_ID_TEXT + 0x22u, model->disc_id);
                }
                nk_ui_end_container(ui);
            }
            wizard_text(ui, 1u, safe_text(model->title_name, "PlayStation Portable title"),
                        NK_UI_TYPE_TITLE, theme->paper);
            wizard_text(ui, 2u, model->summary_line, NK_UI_TYPE_DATA, theme->muted);
            wizard_text(ui, 3u,
                        model->runtime_ready
                            ? "Play starts the game; the library keeps it for next time."
                            : "The game is saved to your library. Open it there and choose BUILD "
                              "PACKAGE to make it playable.",
                        NK_UI_TYPE_BODY_S, theme->muted);
            nk_ui_end_container(ui);
        }
        wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 3u, &result);
        break;
    }
    }
    flow_end(ui, "", prompts, 3u);
    return result;
}

/* ---- Game found ------------------------------------------------------------------
   The card a disc gets when it is added from the library (#1302): the game's
   name, one sentence on what Nakagawa can do with it, every check in plain
   words, and Add to library when the disc can be added. Only a check that
   keeps the disc out of the library reads as an error. */

#define DISC_ID_CHIPS 0x62000001u
#define DISC_ID_BOX 0x62000010u
#define DISC_ID_ROWS 0x62000100u
#define DISC_ID_DETAILS 0x62000020u
#define DISC_ID_TEXT 0x62001000u

static PlayerUiAction disc_op(PlayerUiDiscOp op) {
    PlayerUiAction result;
    result.kind = PLAYER_UI_ACTION_DISC;
    result.primary = (PlayerUiPrimaryAction)op;
    result.title_index = 0u;
    return result;
}

static void disc_text(NkUi *ui, uint32_t k, const char *text, NkUiTypeRole role, uint32_t rgba) {
    if (text != NULL && text[0] != '\0') {
        nk_ui_text_role(ui, (NkUiId)(DISC_ID_TEXT + k), text, role, rgba);
    }
}

/* The check rows: the status word in its colour, what was checked, and the
   sentence, two lines of it when the window has the room. */
static void disc_rows(NkUi *ui, const PlayerUiDiscModel *model, float inner, int lines) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout box = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 4.0f);
    NkUiStyle style = flow_box_style(theme, theme->surface);
    float message = inner - 32.0f - 132.0f - 124.0f - 2.0f * NK_UI_SPACE_3;
    box.padding_top = box.padding_bottom = lines > 1 ? 12.0f : 8.0f;
    box.padding_left = box.padding_right = 16.0f;
    if (lines < 2) box.gap = 2.0f;
    if (model->check_count == 0u || !nk_ui_begin_container(ui, DISC_ID_BOX, &box, &style)) return;
    for (size_t i = 0u; i < model->check_count && i < PLAYER_UI_DISC_CHECKS; ++i) {
        const PlayerUiCheckRow *check = &model->checks[i];
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
        NkUiId base = (NkUiId)(DISC_ID_ROWS + (NkUiId)i * 8u);
        if (nk_ui_begin_container(ui, base, &row, NULL)) {
            NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(132.0f), sizing_fit(),
                                          0.0f);
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
            cell.padding_top = 2.0f;
            if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
                nk_ui_text_role(ui, base + 4u, nk_ui_caps(ui, safe_text(check->status, "Checked")),
                                NK_UI_TYPE_LABEL, check->rgba);
                nk_ui_end_container(ui);
            }
            cell.width = sizing_fixed(124.0f);
            cell.padding_top = 0.0f;
            if (nk_ui_begin_container(ui, base + 2u, &cell, NULL)) {
                nk_ui_text_role(ui, base + 5u, safe_text(check->code, "Check"), NK_UI_TYPE_BODY_S,
                                theme->paper);
                nk_ui_end_container(ui);
            }
            text.rgba = theme->muted;
            nk_ui_text(ui, base + 3u,
                       nk_ui_text_clamp(ui, safe_text(check->message, ""), &text, message, lines),
                       &text);
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

PlayerUiAction player_ui_screen_disc(NkUi *ui, const PlayerUiDiscModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Back to library"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    const char *marker = FLOW_MARK_DOT;
    const char *badge = "Game found";
    const char *lead = NULL;
    uint32_t tone;
    float inner;
    float height;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    inner = flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       height, 820.0f);
    if (inner <= 0.0f) return result;
    tone = theme->ready;
    switch (model->kind) {
        case PLAYER_UI_DISC_CHECKING:
            marker = FLOW_MARK_DIAMOND;
            badge = "Checking the disc";
            tone = theme->accent_dark;
            lead = "Nakagawa is reading the disc image to find out which game it is.";
            break;
        case PLAYER_UI_DISC_SUPPORTED:
            lead = "On Nakagawa's list. Add it to your library, then build it there to play.";
            break;
        case PLAYER_UI_DISC_UNTESTED:
            marker = "?";
            badge = "Not tested yet";
            tone = theme->info;
            lead = "Not tested with this game yet. You can add it and try.";
            break;
        case PLAYER_UI_DISC_BLOCKED:
            marker = FLOW_MARK_CROSS;
            badge = "Can't play yet";
            tone = theme->error;
            lead = "This game can't be played in this version of Nakagawa yet.";
            break;
        case PLAYER_UI_DISC_UNSUPPORTED:
        default:
            marker = FLOW_MARK_CROSS;
            badge = "Not on the list yet";
            tone = theme->error;
            lead = model->profile_details != NULL && model->profile_details[0] != '\0'
                ? "Nakagawa couldn't use this game's title profile, so it can't be added yet."
                : "This game isn't on Nakagawa's list yet, so it can't be added.";
            break;
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_2);
        if (nk_ui_begin_container(ui, DISC_ID_CHIPS, &row, NULL)) {
            nk_ui_status_pill(ui, DISC_ID_CHIPS + 1u, marker, badge, tone);
            if (model->disc_id != NULL && model->disc_id[0] != '\0') {
                nk_ui_tag(ui, DISC_ID_CHIPS + 2u, model->disc_id);
            }
            if (model->kind != PLAYER_UI_DISC_CHECKING) {
                nk_ui_tag(ui, DISC_ID_CHIPS + 3u, "Your disc");
            }
            nk_ui_end_container(ui);
        }
    }
    if (model->kind == PLAYER_UI_DISC_CHECKING) {
        disc_text(ui, 0u, "Checking your disc", NK_UI_TYPE_TITLE, theme->paper);
    } else {
        disc_text(ui, 0u, safe_text(model->title_name, "PSP game"), NK_UI_TYPE_TITLE,
                  theme->paper);
    }
    disc_text(ui, 1u, lead, NK_UI_TYPE_BODY, model->kind == PLAYER_UI_DISC_BLOCKED
                                                 ? theme->error : theme->muted);
    if (model->kind == PLAYER_UI_DISC_BLOCKED && model->reason != NULL) {
        /* The reason names the folder a file goes in, so it is kept whole. */
        NkUiTextStyle reason = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
        nk_ui_text(ui, DISC_ID_TEXT + 2u,
                   nk_ui_text_clamp(ui, model->reason, &reason, inner, 5), &reason);
    }
    disc_text(ui, 3u, model->profile_note, NK_UI_TYPE_BODY_S, theme->warning);
    {
        if (model->profile_details != NULL && model->profile_details[0] != '\0') {
            NkUiLayout box = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 4.0f);
            NkUiStyle style = flow_box_style(theme, theme->surface);
            box.padding_top = box.padding_bottom = 10.0f;
            box.padding_left = box.padding_right = 14.0f;
            if (nk_ui_begin_container(ui, DISC_ID_DETAILS, &box, &style)) {
                NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
                data.wrap = true;
                nk_ui_eyebrow(ui, DISC_ID_DETAILS + 1u, "Details");
                nk_ui_text(ui, DISC_ID_DETAILS + 2u,
                           nk_ui_text_clamp(ui, model->profile_details, &data, inner - 28.0f, 2),
                           &data);
                nk_ui_end_container(ui);
            }
        }
    }
    if (model->kind == PLAYER_UI_DISC_CHECKING) {
        NkUiTextStyle path = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
        if (model->iso_path != NULL && model->iso_path[0] != '\0') {
            nk_ui_text(ui, DISC_ID_TEXT + 4u,
                       nk_ui_text_clamp(ui, model->iso_path, &path, inner, 2), &path);
        }
        nk_ui_progress_bar(ui, DISC_ID_TEXT + 5u, inner, -1.0f, model->now_s);
    } else if (model->kind != PLAYER_UI_DISC_BLOCKED || height >= 680.0f) {
        /* Two lines of each sentence from a 680 px tall window; one below it,
           so the card still fits the 540 px minimum. A disc that can't be added
           gives the room to its reason there. */
        disc_rows(ui, model, inner, height >= 680.0f ? 2 : 1);
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, DISC_ID_TEXT + 0x40u, &row, NULL)) {
            switch (model->kind) {
                case PLAYER_UI_DISC_CHECKING:
                    if (nk_ui_button(ui, PLAYER_UI_ID_DISC_PRIMARY, "Cancel", 0u)) {
                        result = disc_op(PLAYER_UI_DISC_BACK);
                    }
                    break;
                case PLAYER_UI_DISC_SUPPORTED:
                case PLAYER_UI_DISC_UNTESTED:
                    if (nk_ui_button(ui, PLAYER_UI_ID_DISC_PRIMARY, "Add to library",
                                     NK_UI_BUTTON_PRIMARY)) {
                        result = disc_op(PLAYER_UI_DISC_ADD);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_DISC_SECOND, "Back", 0u)) {
                        result = disc_op(PLAYER_UI_DISC_BACK);
                    }
                    break;
                case PLAYER_UI_DISC_BLOCKED:
                    /* Adding it would only fail later, at build time (#1280). */
                    if (nk_ui_button(ui, PLAYER_UI_ID_DISC_PRIMARY, "Choose another disc",
                                     NK_UI_BUTTON_PRIMARY)) {
                        result = disc_op(PLAYER_UI_DISC_CHOOSE_ANOTHER);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_DISC_SECOND, "Back", 0u)) {
                        result = disc_op(PLAYER_UI_DISC_BACK);
                    }
                    break;
                case PLAYER_UI_DISC_UNSUPPORTED:
                default:
                    if (nk_ui_button(ui, PLAYER_UI_ID_DISC_PRIMARY, "Back to library",
                                     NK_UI_BUTTON_PRIMARY)) {
                        result = disc_op(PLAYER_UI_DISC_BACK);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_DISC_SECOND, "Choose another disc", 0u)) {
                        result = disc_op(PLAYER_UI_DISC_CHOOSE_ANOTHER);
                    }
                    break;
            }
            nk_ui_end_container(ui);
        }
    }
    flow_end(ui, model->kind == PLAYER_UI_DISC_SUPPORTED || model->kind == PLAYER_UI_DISC_UNTESTED
                     ? "Your disc image stays where it is. Nothing is uploaded."
                     : "",
             prompts, 3u);
    return result;
}

/* ---- Settings ----------------------------------------------------------------------
   One page of named rows, each with its value on the button that changes it, in
   two cards: what a game uses the next time it starts, and the launcher and
   sound. Its pages (online and developer options, third-party notices, build
   tools and cache, removing the build tools) are dialog cards like the rest of
   the flow. Every change is saved as it is made. */

#define SET_ID_ROOT 0x63000001u
#define SET_ID_TOP 0x63000010u
#define SET_ID_BODY 0x63000020u
#define SET_ID_COLUMN 0x63000021u
#define SET_ID_HEAD 0x63000022u
#define SET_ID_CARDS 0x63000023u
#define SET_ID_GAME 0x63000024u
#define SET_ID_LAUNCHER 0x63000025u
#define SET_ID_ACTIONS 0x63000026u
#define SET_ID_PROMPTS 0x63000040u
#define SET_ID_WAVES 0x63000050u
#define SET_ID_ROWS 0x63000100u
#define SET_ID_TEXT 0x63001000u

static PlayerUiAction settings_op(PlayerUiSettingsOp op) {
    PlayerUiAction result;
    result.kind = PLAYER_UI_ACTION_SETTINGS;
    result.primary = (PlayerUiPrimaryAction)op;
    result.title_index = 0u;
    return result;
}

static const char *settings_on_off(bool on) {
    return on ? "On" : "Off";
}

/* A named row with its value on the button beside it. Returns true when the
   button is activated. */
static bool settings_row(NkUi *ui, uint32_t k, NkUiId button, const char *name,
                         const char *value, bool on, float name_width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
    NkUiId base = (NkUiId)(SET_ID_ROWS + k * 4u);
    bool pressed = false;
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, base, &row, NULL)) return false;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(name_width), sizing_fit(),
                                      0.0f);
        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
            nk_ui_text_role(ui, base + 2u, name, NK_UI_TYPE_BODY, theme->paper);
            nk_ui_end_container(ui);
        }
    }
    /* Left and Right stay in the row, so Right from a GAME row reaches the
       LAUNCHER AND SOUND row beside it (Controls among them), not the top bar. */
    pressed = nk_ui_button(ui, button, value,
                           NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW | NK_UI_BUTTON_ROW |
                               (on ? NK_UI_BUTTON_PRIMARY : 0u));
    nk_ui_end_container(ui);
    return pressed;
}

static void settings_main(NkUi *ui, const PlayerUiSettingsModel *model, float width,
                          float height, PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    float column = width - 2.0f * (width < 900.0f ? NK_UI_GUTTER_NARROW : NK_UI_GUTTER);
    float card;
    float name_width;
    char line[192];
    NkUiLayout layout;
    if (column > 1040.0f) column = 1040.0f;
    card = (column - NK_UI_SPACE_4) * 0.5f;
    name_width = card - 40.0f - NK_UI_SPACE_3 - 190.0f;
    if (name_width > 200.0f) name_width = 200.0f;
    if (name_width < 110.0f) name_width = 110.0f;
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(column), sizing_fit(), NK_UI_SPACE_4);
    if (!nk_ui_begin_container(ui, SET_ID_COLUMN, &layout, NULL)) return;
    {
        NkUiLayout head = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(),
                                      NK_UI_SPACE_1);
        if (nk_ui_begin_container(ui, SET_ID_HEAD, &head, NULL)) {
            /* The SETTINGS tab names the page, so a short window drops the
               heading to keep every row on screen. */
            if (height >= 680.0f) {
                nk_ui_text_role(ui, SET_ID_TEXT + 0u, "SETTINGS", NK_UI_TYPE_DISPLAY,
                                theme->paper);
            }
            nk_ui_text_role(ui, SET_ID_TEXT + 1u,
                            safe_text(model->notice,
                                      "Changes are saved as you make them. Game settings apply "
                                      "the next time a game starts."),
                            NK_UI_TYPE_BODY_S,
                            model->notice != NULL && model->notice[0] != '\0'
                                ? theme->warning : theme->muted);
            nk_ui_end_container(ui);
        }
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
    if (nk_ui_begin_container(ui, SET_ID_CARDS, &layout, NULL)) {
        if (nk_ui_card_begin(ui, SET_ID_GAME, sizing_fixed(card), sizing_grow(), NK_UI_SPACE_2)) {
            nk_ui_eyebrow(ui, SET_ID_TEXT + 0x10u, "Game");
            if (settings_row(ui, 0u, PLAYER_UI_ID_SETTINGS_RESOLUTION, "Resolution",
                             safe_text(model->resolution, "2x"), false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_RESOLUTION);
            }
            if (settings_row(ui, 1u, PLAYER_UI_ID_SETTINGS_PACING, "Frame pacing",
                             safe_text(model->pacing, "PSP timing"), false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_PACING);
            }
            if (settings_row(ui, 2u, PLAYER_UI_ID_SETTINGS_VSYNC, "VSync",
                             settings_on_off(model->vsync), model->vsync, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_VSYNC);
            }
            if (settings_row(ui, 3u, PLAYER_UI_ID_SETTINGS_FULLSCREEN, "Fullscreen",
                             settings_on_off(model->fullscreen), model->fullscreen,
                             name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_FULLSCREEN);
            }
            if (settings_row(ui, 4u, PLAYER_UI_ID_SETTINGS_SCALING, "Scaling",
                             safe_text(model->scaling, "Sharp"), false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_SCALING);
            }
            nk_ui_end_container(ui);
        }
        if (nk_ui_card_begin(ui, SET_ID_LAUNCHER, sizing_fixed(card), sizing_grow(),
                             NK_UI_SPACE_2)) {
            nk_ui_eyebrow(ui, SET_ID_TEXT + 0x20u, "Launcher and sound");
            {
                /* Volume: the two steps either side of the level. */
                NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                             NK_UI_SPACE_3);
                NkUiId base = (NkUiId)(SET_ID_ROWS + 5u * 4u);
                row.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, base, &row, NULL)) {
                    NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(name_width),
                                                  sizing_fit(), 0.0f);
                    if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
                        nk_ui_text_role(ui, base + 2u, "Game volume", NK_UI_TYPE_BODY,
                                        theme->paper);
                        nk_ui_end_container(ui);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_VOLUME_DOWN, "-",
                                     NK_UI_BUTTON_SMALL)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_VOLUME_DOWN);
                    }
                    (void)snprintf(line, sizeof(line), "%d%%", model->volume);
                    cell.width = sizing_fixed(52.0f);
                    cell.align_x = NK_UI_ALIGN_CENTER;
                    if (nk_ui_begin_container(ui, base + 3u, &cell, NULL)) {
                        nk_ui_text_role(ui, SET_ID_TEXT + 0x11u, line, NK_UI_TYPE_DATA,
                                        theme->paper);
                        nk_ui_end_container(ui);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_VOLUME_UP, "+",
                                     NK_UI_BUTTON_SMALL)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_VOLUME_UP);
                    }
                    nk_ui_end_container(ui);
                }
            }
            if (settings_row(ui, 6u, PLAYER_UI_ID_SETTINGS_LAUNCHER_FULLSCREEN,
                             "Fullscreen (F11)", settings_on_off(model->launcher_fullscreen),
                             model->launcher_fullscreen, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_LAUNCHER_FULLSCREEN);
            }
            if (settings_row(ui, 7u, PLAYER_UI_ID_SETTINGS_REDUCE_MOTION, "Reduce motion",
                             settings_on_off(model->reduce_motion), model->reduce_motion,
                             name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_REDUCE_MOTION);
            }
            if (settings_row(ui, 8u, PLAYER_UI_ID_SETTINGS_UI_SOUNDS, "Menu sounds",
                             settings_on_off(model->ui_sounds), model->ui_sounds, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_UI_SOUNDS);
            }
            if (settings_row(ui, 9u, PLAYER_UI_ID_SETTINGS_CONTROLLER, "Controls",
                             "Controller and keyboard", false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_OPEN_CONTROLLER);
            }
            {
                NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                (void)snprintf(line, sizeof(line), "Controller: %s",
                               safe_text(model->controller_line, "keyboard and mouse"));
                nk_ui_text(ui, SET_ID_TEXT + 0x21u,
                           nk_ui_text_clamp(ui, line, &text, card - 40.0f, 1), &text);
                if (model->saves_folder != NULL && model->saves_folder[0] != '\0') {
                    char saves[600];
                    text = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                    /* A no-break space keeps the folder on the line, so a long
                       one is cut short rather than dropped. */
                    (void)snprintf(saves, sizeof(saves), "Saves:\xC2\xA0%s",
                                   model->saves_folder);
                    nk_ui_text(ui, SET_ID_TEXT + 0x22u,
                               nk_ui_text_clamp(ui, saves, &text, card - 40.0f, 1), &text);
                }
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
    if (nk_ui_begin_container(ui, SET_ID_ACTIONS, &layout, NULL)) {
        if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_DONE, "Done", NK_UI_BUTTON_PRIMARY)) {
            *result = settings_op(PLAYER_UI_SETTINGS_DONE);
        }
        if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_ONLINE, "Online and developer", 0u)) {
            *result = settings_op(PLAYER_UI_SETTINGS_OPEN_ONLINE);
        }
        if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_NOTICES, "Third-party notices", 0u)) {
            *result = settings_op(PLAYER_UI_SETTINGS_OPEN_NOTICES);
        }
        if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BUILD_CACHE, "Build tools and cache", 0u)) {
            *result = settings_op(PLAYER_UI_SETTINGS_OPEN_BUILD_CACHE);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

/* The Settings pages that are a dialog card over the flow frame. */
static void settings_dialog(NkUi *ui, const PlayerUiSettingsModel *model, float width,
                            float height, PlayerUiAction *result) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Back to settings"}
    };
    const NkUiTheme *theme = nk_ui_theme(ui);
    float inner = flow_begin(ui, width, height, 760.0f);
    if (inner <= 0.0f) return;
    switch (model->page) {
    case PLAYER_UI_SETTINGS_PAGE_ONLINE:
        flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Online and developer options", NK_UI_TYPE_TITLE,
                        theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "This version has no online features yet and makes no connection. Your "
                        "choice is kept for when they arrive.",
                        NK_UI_TYPE_BODY_S, theme->warning);
        if (wizard_box_begin(ui, 0u)) {
            if (wizard_switch(ui, PLAYER_UI_ID_SETTINGS_ONLINE_MODE, SET_ID_TEXT + 0x80u,
                              "Online mode",
                              "Check nakagawa.jaycast.net for updates and news, and let you send "
                              "reports. Nothing is sent without you.",
                              model->online_mode, inner - 32.0f)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_ONLINE);
            }
            if (model->online_mode) {
                NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                             NK_UI_SPACE_3);
                char label[48];
                if (nk_ui_begin_container(ui, SET_ID_TEXT + 0x88u, &row, NULL)) {
                    (void)snprintf(label, sizeof(label), "Update check: %s",
                                   settings_on_off(model->online_updates));
                    if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_UPDATES, label,
                                     NK_UI_BUTTON_SMALL |
                                         (model->online_updates ? NK_UI_BUTTON_PRIMARY : 0u))) {
                        *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_UPDATES);
                    }
                    (void)snprintf(label, sizeof(label), "News: %s",
                                   settings_on_off(model->online_news));
                    if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_NEWS, label,
                                     NK_UI_BUTTON_SMALL |
                                         (model->online_news ? NK_UI_BUTTON_PRIMARY : 0u))) {
                        *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_NEWS);
                    }
                    (void)snprintf(label, sizeof(label), "Reports: %s",
                                   settings_on_off(model->online_reports));
                    if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_REPORTS, label,
                                     NK_UI_BUTTON_SMALL |
                                         (model->online_reports ? NK_UI_BUTTON_PRIMARY : 0u))) {
                        *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_REPORTS);
                    }
                    nk_ui_end_container(ui);
                }
            }
            if (wizard_switch(ui, PLAYER_UI_ID_SETTINGS_DEVELOPER, SET_ID_TEXT + 0x90u,
                              "Developer mode",
                              "Show a developer log on screen to help track down problems. "
                              "Stays on this PC.",
                              model->developer_mode, inner - 32.0f)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_DEVELOPER);
            }
            nk_ui_end_container(ui);
        }
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back", NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_BACK);
                }
                if (model->developer_mode &&
                    nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_DEVELOPER_LOG,
                                 "Open developer log (F12)", 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_OPEN_DEVELOPER_LOG);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    case PLAYER_UI_SETTINGS_PAGE_NOTICES:
        flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Third-party notices", theme->accent_dark);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Build tools and their licences", NK_UI_TYPE_TITLE,
                        theme->paper);
        if (wizard_box_begin(ui, 0u)) {
            NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, theme->paper);
            for (size_t i = 0u; i < model->tool_count && i < PLAYER_UI_SETTINGS_TOOLS; ++i) {
                nk_ui_text(ui, (NkUiId)(SET_ID_TEXT + 0xA0u + (NkUiId)i),
                           nk_ui_text_clamp(ui, safe_text(model->tools[i], ""), &data,
                                            inner - 32.0f, 1), &data);
            }
            if (model->tool_count == 0u) {
                nk_ui_text_role(ui, SET_ID_TEXT + 0x9Fu,
                                "No build tools have been downloaded on this PC yet.",
                                NK_UI_TYPE_BODY_S, theme->muted);
            }
            nk_ui_end_container(ui);
        }
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "Nakagawa Recomp is free software (GPL-3.0-or-later; see LICENSE and "
                        "NOTICE.md). Not affiliated with or endorsed by Sony Interactive "
                        "Entertainment; PlayStation and PSP are its trademarks.",
                        NK_UI_TYPE_CAPTION, theme->muted);
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back", NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_BACK);
                }
                if (model->tool_count > 0u &&
                    nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_LICENSE_TEXTS, "Open licence texts",
                                 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_OPEN_LICENSE_TEXTS);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    case PLAYER_UI_SETTINGS_PAGE_BUILD_CACHE:
        /* #1306: the shared library switch, its clear button and tool removal. */
        flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Build tools and cache", NK_UI_TYPE_TITLE,
                        theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "A game disc carries system libraries that other games often share. "
                        "The compiled copies stay in the app-data folder on this PC and are "
                        "never uploaded or shared.",
                        NK_UI_TYPE_BODY_S, theme->muted);
        if (wizard_box_begin(ui, 0u)) {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            if (wizard_switch(ui, PLAYER_UI_ID_SETTINGS_SHARE_LIBRARIES, SET_ID_TEXT + 0xC0u,
                              "Share libraries",
                              "Builds keep each compiled library and reuse it for another game "
                              "with the same one, so those builds finish sooner.",
                              model->share_system_libraries, inner - 32.0f)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_SHARE_LIBRARIES);
            }
            if (nk_ui_begin_container(ui, SET_ID_TEXT + 0xC8u, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_CLEAR_LIBRARIES,
                                 "Clear shared library cache", NK_UI_BUTTON_SMALL)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_CLEAR_SHARED_LIBRARIES);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_REMOVE_TOOLS, "Remove build tools",
                                 NK_UI_BUTTON_SMALL)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_OPEN_REMOVE_TOOLS);
                }
                nk_ui_end_container(ui);
            }
            if (model->notice != NULL && model->notice[0] != '\0') {
                NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->warning);
                nk_ui_text(ui, SET_ID_TEXT + 0xCCu,
                           nk_ui_text_clamp(ui, model->notice, &text, inner - 32.0f, 2), &text);
            }
            nk_ui_end_container(ui);
        }
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back", NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_BACK);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    case PLAYER_UI_SETTINGS_PAGE_REMOVE_TOOLS:
    default:
        flow_eyebrow(ui, FLOW_MARK_TRIANGLE, "Remove build tools", theme->warning);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Remove the downloaded build tools?",
                        NK_UI_TYPE_TITLE, theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "This removes the build tools Nakagawa downloaded. Your library, disc "
                        "images, saves and built games are kept. Nakagawa asks before it "
                        "downloads them again, the next time you build a game.",
                        NK_UI_TYPE_BODY, theme->muted);
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Keep them",
                                 NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_KEEP_TOOLS);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_CONFIRM_REMOVE, "Remove tools",
                                 NK_UI_BUTTON_DANGER)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_REMOVE_TOOLS);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    }
    flow_end(ui, "", prompts, 3u);
}

PlayerUiAction player_ui_screen_settings(NkUi *ui, const PlayerUiSettingsModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Change"},
        {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    NkUiLayout layout;
    float width;
    float height;
    if (ui == NULL || model == NULL) return result;
    width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    if (model->page != PLAYER_UI_SETTINGS_PAGE_MAIN) {
        settings_dialog(ui, model, width, height, &result);
        return result;
    }
    (void)nk_ui_waves_element(ui, SET_ID_WAVES, (NkUiRect){0.0f, 0.0f, width, height}, 0u);
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, SET_ID_ROOT, &layout, NULL)) return result;
    {
        PlayerUiActionKind tab = player_ui_top_bar(ui, SET_ID_TOP, PLAYER_UI_ROUTE_SETTINGS);
        if (tab != PLAYER_UI_ACTION_NONE) result = action(tab, 0u, PLAYER_UI_PRIMARY_NONE);
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_grow(), 0.0f);
    layout.align_x = NK_UI_ALIGN_CENTER;
    layout.padding_top = height >= 680.0f ? NK_UI_SPACE_7 : NK_UI_SPACE_4;
    if (nk_ui_begin_container(ui, SET_ID_BODY, &layout, NULL)) {
        settings_main(ui, model, width, height, &result);
        nk_ui_end_container(ui);
    }
    nk_ui_prompt_bar(ui, SET_ID_PROMPTS, "", prompts, sizeof(prompts) / sizeof(prompts[0]));
    nk_ui_end_container(ui);
    return result;
}

/* ---- Controller settings ------------------------------------------------------------
   The PSP buttons in two columns of named rows, each binding on the button that
   rebinds it, beside the stick and trigger card with the live input. The
   mapping being edited is named on the buttons card, so the scope is stated,
   never implied (#520). CONTROLLER and KEYBOARD above the cards choose whose
   bindings the rows show; for the keyboard the stick card names the keys
   Nakagawa keeps (#1299). While a guided calibration runs, its step replaces the
   cards. Under a top bar with SETTINGS active, as the Settings page. */

#define PAD_ID_ROOT 0x64000001u
#define PAD_ID_TOP 0x64000010u
#define PAD_ID_BODY 0x64000020u
#define PAD_ID_COLUMN 0x64000021u
#define PAD_ID_HEAD 0x64000022u
#define PAD_ID_CARDS 0x64000023u
#define PAD_ID_BUTTONS 0x64000024u
#define PAD_ID_STICK 0x64000025u
#define PAD_ID_ACTIONS 0x64000026u
#define PAD_ID_GRID 0x64000027u
#define PAD_ID_LIVE 0x64000030u
#define PAD_ID_SPACER 0x64000031u
#define PAD_ID_DEVICE 0x64000032u
#define PAD_ID_DEVICE_BUTTONS 0x64000033u
#define PAD_ID_PROMPTS 0x64000040u
#define PAD_ID_WAVES 0x64000050u
#define PAD_ID_ROWS 0x64000100u
#define PAD_ID_TEXT 0x64001000u

static PlayerUiAction controller_op(PlayerUiControllerOp op, size_t control) {
    PlayerUiAction result;
    result.kind = PLAYER_UI_ACTION_CONTROLLER;
    result.primary = (PlayerUiPrimaryAction)op;
    result.title_index = control;
    return result;
}

/* A label cut to the room a small button of `width` has for it. */
static const char *controller_button_label(NkUi *ui, const char *label, float width) {
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_LABEL_S, 0u);
    return nk_ui_text_clamp(ui, nk_ui_caps(ui, label), &text,
                            width - 2.0f * NK_UI_SPACE_3 - 2.0f, 1);
}

/* One PSP control: its name, then its binding on the button that rebinds it. */
static void controller_row(NkUi *ui, const PlayerUiControllerModel *model, size_t control,
                           float name_width, float button_width, PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_2);
    NkUiId base = (NkUiId)(PAD_ID_ROWS + (NkUiId)control * 4u);
    bool capturing = model->capturing == (int)control;
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, model->conflicted[control] ? theme->error
                                                                                  : theme->paper);
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, base, &row, NULL)) return;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(name_width), sizing_fit(),
                                      0.0f);
        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
            nk_ui_text(ui, base + 2u,
                       nk_ui_text_clamp(ui, safe_text(model->names[control], "Button"), &text,
                                        name_width, 1), &text);
            nk_ui_end_container(ui);
        }
    }
    if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_BUTTON(control),
                     controller_button_label(ui, capturing ? (model->keyboard ? "Press a key..."
                                                                              : "Press a button...")
                                                           : safe_text(model->bindings[control],
                                                                       "None"),
                                             button_width),
                     NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW |
                         (capturing ? NK_UI_BUTTON_PRIMARY : 0u))) {
        *result = controller_op(PLAYER_UI_CONTROLLER_REBIND, control);
    }
    nk_ui_end_container(ui);
}

/* A value in words with the - and + steps that change it. */
static void controller_stepper(NkUi *ui, uint32_t k, const char *value, float width,
                               NkUiId down, NkUiId up, PlayerUiControllerOp down_op,
                               PlayerUiControllerOp up_op, PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_2);
    NkUiId base = (NkUiId)(PAD_ID_ROWS + 0x80u + k * 4u);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, base, &row, NULL)) return;
    {
        /* Each step is its sign and the small button's padding. */
        float text_width = width - 2.0f * (2.0f * NK_UI_SPACE_3 + 10.0f) - 2.0f * NK_UI_SPACE_2;
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), 0.0f);
        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
            nk_ui_text(ui, base + 2u, nk_ui_text_clamp(ui, safe_text(value, ""), &text,
                                                       text_width, 1), &text);
            nk_ui_end_container(ui);
        }
    }
    if (nk_ui_button(ui, down, "-", NK_UI_BUTTON_SMALL)) *result = controller_op(down_op, 0u);
    if (nk_ui_button(ui, up, "+", NK_UI_BUTTON_SMALL)) *result = controller_op(up_op, 0u);
    nk_ui_end_container(ui);
}

static void controller_live(NkUi *ui, const PlayerUiControllerModel *model, float width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout lines = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 2.0f);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
    if (!nk_ui_begin_container(ui, PAD_ID_LIVE, &lines, NULL)) return;
    for (size_t i = 0u; i < model->live_count && i < PLAYER_UI_CONTROLLER_LIVE_LINES; ++i) {
        nk_ui_text(ui, (NkUiId)(PAD_ID_TEXT + 0x40u + (NkUiId)i),
                   nk_ui_text_clamp(ui, safe_text(model->live[i], " "), &text, width, 1), &text);
    }
    nk_ui_end_container(ui);
}

/* CONTROLLER and KEYBOARD: whose bindings the rows show, the one shown filled. */
#define PAD_DEVICE_WIDTH 240.0f
static void controller_device(NkUi *ui, const PlayerUiControllerModel *model,
                              PlayerUiAction *result) {
    NkUiLayout pair = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(PAD_DEVICE_WIDTH), sizing_fit(),
                                  NK_UI_SPACE_2);
    float each = (PAD_DEVICE_WIDTH - NK_UI_SPACE_2) * 0.5f;
    uint32_t flags = NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW | NK_UI_BUTTON_ROW;
    if (!nk_ui_begin_container(ui, PAD_ID_DEVICE_BUTTONS, &pair, NULL)) return;
    if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_SHOW_PAD,
                     controller_button_label(ui, "Controller", each),
                     flags | (model->keyboard ? 0u : NK_UI_BUTTON_PRIMARY))) {
        *result = controller_op(PLAYER_UI_CONTROLLER_SHOW_CONTROLLER, 0u);
    }
    if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_SHOW_KEYBOARD,
                     controller_button_label(ui, "Keyboard", each),
                     flags | (model->keyboard ? NK_UI_BUTTON_PRIMARY : 0u))) {
        *result = controller_op(PLAYER_UI_CONTROLLER_SHOW_KEYBOARD, 0u);
    }
    nk_ui_end_container(ui);
}

/* For the keyboard, the stick card says which keys stay with Nakagawa. */
static void controller_keyboard_card(NkUi *ui, float card) {
    static const char *const kept[] = {"Esc or F1 pauses the game.", "F3 shows the frame rate."};
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
    float inner = card - 2.0f * NK_UI_SPACE_5;
    if (!nk_ui_card_begin(ui, PAD_ID_STICK, sizing_fixed(card), sizing_grow(), NK_UI_SPACE_2)) {
        return;
    }
    nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x20u, "Keys Nakagawa keeps");
    for (size_t i = 0u; i < sizeof(kept) / sizeof(kept[0]); ++i) {
        nk_ui_text(ui, (NkUiId)(PAD_ID_TEXT + 0x50u + (NkUiId)i),
                   nk_ui_text_clamp(ui, kept[i], &text, inner, 2), &text);
    }
    nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x21u, "Analog stick");
    nk_ui_text(ui, PAD_ID_TEXT + 0x58u,
               nk_ui_text_clamp(ui, "The stick has no keys. Use a controller for it.", &text,
                                inner, 2), &text);
    nk_ui_end_container(ui);
}

static void controller_mapping(NkUi *ui, const PlayerUiControllerModel *model, float width,
                               float height, PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    bool tall = height >= 680.0f;
    float column = width - 2.0f * (width < NK_UI_NARROW_WIDTH ? NK_UI_GUTTER_NARROW
                                                              : NK_UI_GUTTER);
    float stick_card;
    float buttons_inner;
    float half;
    float name_width;
    char eyebrow[160];
    NkUiLayout layout;
    if (column > 1120.0f) column = 1120.0f;
    stick_card = column >= 1000.0f ? 320.0f : 260.0f;
    /* Cards pad 20 px either side; the two columns of rows have 16 px between. */
    buttons_inner = column - NK_UI_SPACE_4 - stick_card - 2.0f * NK_UI_SPACE_5;
    half = (buttons_inner - NK_UI_SPACE_4) * 0.5f;
    name_width = half * 0.36f;
    if (name_width > 132.0f) name_width = 132.0f;
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(column), sizing_fit(),
                         tall ? NK_UI_SPACE_4 : NK_UI_SPACE_2);
    if (!nk_ui_begin_container(ui, PAD_ID_COLUMN, &layout, NULL)) return;
    {
        NkUiLayout head = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(),
                                      NK_UI_SPACE_1);
        if (nk_ui_begin_container(ui, PAD_ID_HEAD, &head, NULL)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S,
                                            model->status_error ? theme->error : theme->muted);
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.align_y = NK_UI_ALIGN_CENTER;
            /* The SETTINGS tab names the area, so a short window drops the
               heading to keep every row on screen. */
            if (tall) {
                nk_ui_text_role(ui, PAD_ID_TEXT + 0u, "CONTROLS", NK_UI_TYPE_DISPLAY,
                                theme->paper);
            }
            if (nk_ui_begin_container(ui, PAD_ID_DEVICE, &row, NULL)) {
                controller_device(ui, model, result);
                nk_ui_text(ui, PAD_ID_TEXT + 1u,
                           nk_ui_text_clamp(ui, safe_text(model->status,
                                                          "Choose a button to change it, then "
                                                          "press the controller button to use."),
                                            &text, column - PAD_DEVICE_WIDTH - NK_UI_SPACE_3, 1),
                           &text);
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
    if (nk_ui_begin_container(ui, PAD_ID_CARDS, &layout, NULL)) {
        if (nk_ui_card_begin(ui, PAD_ID_BUTTONS, sizing_grow(), sizing_grow(), NK_UI_SPACE_2)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_EYEBROW,
                                            model->scope_title ? theme->ready : theme->muted);
            (void)snprintf(eyebrow, sizeof(eyebrow), "%s" FLOW_MIDDOT "%s",
                           model->keyboard ? "Keys" : "Buttons",
                           safe_text(model->scope, "global mapping"));
            nk_ui_text(ui, PAD_ID_TEXT + 0x10u,
                       nk_ui_text_clamp(ui, nk_ui_caps(ui, eyebrow), &text, buttons_inner, 1),
                       &text);
            layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
            if (nk_ui_begin_container(ui, PAD_ID_GRID, &layout, NULL)) {
                for (size_t col = 0u; col < 2u; ++col) {
                    NkUiLayout list = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(half),
                                                  sizing_fit(),
                                                  tall ? NK_UI_SPACE_2 : NK_UI_SPACE_1);
                    if (!nk_ui_begin_container(ui, (NkUiId)(PAD_ID_GRID + 1u + (NkUiId)col),
                                               &list, NULL)) {
                        continue;
                    }
                    for (size_t row = 0u; row < PLAYER_UI_CONTROLLER_BUTTONS / 2u; ++row) {
                        controller_row(ui, model, col * (PLAYER_UI_CONTROLLER_BUTTONS / 2u) + row,
                                       name_width, half - name_width - NK_UI_SPACE_2, result);
                    }
                    nk_ui_end_container(ui);
                }
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }
        if (model->keyboard) {
            controller_keyboard_card(ui, stick_card);
        } else if (nk_ui_card_begin(ui, PAD_ID_STICK, sizing_fixed(stick_card), sizing_grow(),
                                    NK_UI_SPACE_2)) {
            float inner = stick_card - 2.0f * NK_UI_SPACE_5;
            nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x20u, "Stick and triggers");
            controller_stepper(ui, 0u, model->deadzone, inner,
                               PLAYER_UI_ID_CONTROLLER_DEADZONE_DOWN,
                               PLAYER_UI_ID_CONTROLLER_DEADZONE_UP,
                               PLAYER_UI_CONTROLLER_DEADZONE_DOWN,
                               PLAYER_UI_CONTROLLER_DEADZONE_UP, result);
            controller_stepper(ui, 1u, model->trigger, inner,
                               PLAYER_UI_ID_CONTROLLER_TRIGGER_DOWN,
                               PLAYER_UI_ID_CONTROLLER_TRIGGER_UP,
                               PLAYER_UI_CONTROLLER_TRIGGER_DOWN,
                               PLAYER_UI_CONTROLLER_TRIGGER_UP, result);
            if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_CALIBRATE, "Calibrate stick",
                             NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW)) {
                *result = controller_op(PLAYER_UI_CONTROLLER_CALIBRATE, 0u);
            }
            nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x21u, "Live input");
            controller_live(ui, model, inner);
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    /* Back sits at the right end, under the stick card, so Down from Calibrate
       reaches it rather than a binding in the grid; Left and Right stay in the row. */
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
    if (nk_ui_begin_container(ui, PAD_ID_ACTIONS, &layout, NULL)) {
        if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_SAVE, "Save profile",
                         NK_UI_BUTTON_PRIMARY | NK_UI_BUTTON_ROW)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_SAVE, 0u);
        }
        if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_RESET, "Reset to defaults",
                         NK_UI_BUTTON_ROW)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_RESET, 0u);
        }
        /* Global or this game's own, offered only with a game selected (#520). */
        if (model->offer_scope &&
            nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_SCOPE,
                         model->scope_title ? "Use the global mapping"
                                            : "Use this game's own mapping",
                         NK_UI_BUTTON_ROW)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_TOGGLE_SCOPE, 0u);
        }
        nk_ui_spacer(ui, PAD_ID_SPACER, sizing_grow(), sizing_fixed(1.0f));
        if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_BACK, "Back", NK_UI_BUTTON_ROW)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_BACK, 0u);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

/* A calibration step: what to do, its numbers, and its buttons. */
static void controller_calibration(NkUi *ui, const PlayerUiControllerModel *model, float width,
                                   PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const char *step;
    const char *title;
    const char *lead;
    const char *primary = NULL;
    PlayerUiControllerOp primary_op = PLAYER_UI_CONTROLLER_NONE;
    float card = width - 2.0f * NK_UI_GUTTER_NARROW;
    float inner;
    NkUiLayout row;
    if (card > 720.0f) card = 720.0f;
    inner = card - 56.0f;
    switch (model->page) {
        case PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_REST:
            step = "Step 1 of 3: rest";
            title = "Leave the stick and triggers alone";
            lead = "Don't touch the stick or the triggers for about a second while Nakagawa "
                   "reads where they rest.";
            break;
        case PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_EXTREMES:
            step = "Step 2 of 3: full range";
            title = "Move the stick and press the triggers";
            lead = "Turn the stick round its edge a few times and press both triggers all the "
                   "way, then choose Finish.";
            primary = "Finish";
            primary_op = PLAYER_UI_CONTROLLER_CALIBRATION_FINISH;
            break;
        case PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_RESULT:
        default:
            step = "Step 3 of 3: check";
            title = "Use this calibration?";
            lead = "These are the resting points and ranges Nakagawa read. Accept uses them for "
                   "the mapping you are editing.";
            primary = "Accept";
            primary_op = PLAYER_UI_CONTROLLER_CALIBRATION_ACCEPT;
            break;
    }
    if (!nk_ui_dialog_begin(ui, FLOW_ID_CARD, card)) return;
    flow_eyebrow(ui, FLOW_MARK_DIAMOND, step, theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, title, NK_UI_TYPE_TITLE, theme->paper);
    {
        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY, theme->muted);
        nk_ui_text(ui, FLOW_ID_LEAD, nk_ui_text_clamp(ui, lead, &text, inner, 3), &text);
    }
    if (model->page == PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_REST) {
        nk_ui_progress_bar(ui, FLOW_ID_BAR, inner, model->calibration_fraction, 0.0);
    }
    if (wizard_box_begin(ui, 0u)) {
        controller_live(ui, model, inner - 32.0f);
        nk_ui_end_container(ui);
    }
    row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
    row.padding_top = NK_UI_SPACE_1;
    if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
        if (primary != NULL &&
            nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_CALIBRATION_PRIMARY, primary,
                         NK_UI_BUTTON_PRIMARY)) {
            *result = controller_op(primary_op, 0u);
        }
        if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_CALIBRATION_CANCEL, "Cancel",
                         primary == NULL ? NK_UI_BUTTON_PRIMARY : 0u)) {
            *result = controller_op(PLAYER_UI_CONTROLLER_CALIBRATION_CANCEL, 0u);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
}

PlayerUiAction player_ui_screen_controller(NkUi *ui, const PlayerUiControllerModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Change"},
        {NK_UI_PROMPT_ACT_BACK, "Back to settings"}
    };
    static const NkUiPromptItem prompts_calibrating[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"}, {NK_UI_PROMPT_ACT_BACK, "Cancel"}
    };
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    bool calibrating;
    NkUiLayout layout;
    float width;
    float height;
    if (ui == NULL || model == NULL) return result;
    width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    calibrating = model->page != PLAYER_UI_CONTROLLER_PAGE_MAPPING;
    (void)nk_ui_waves_element(ui, PAD_ID_WAVES, (NkUiRect){0.0f, 0.0f, width, height}, 0u);
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, PAD_ID_ROOT, &layout, NULL)) return result;
    {
        PlayerUiActionKind tab = player_ui_top_bar(ui, PAD_ID_TOP, PLAYER_UI_ROUTE_SETTINGS);
        if (tab != PLAYER_UI_ACTION_NONE) result = action(tab, 0u, PLAYER_UI_PRIMARY_NONE);
    }
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_grow(), 0.0f);
    layout.align_x = NK_UI_ALIGN_CENTER;
    if (calibrating) {
        layout.align_y = NK_UI_ALIGN_CENTER;
        layout.padding_top = layout.padding_bottom = NK_UI_SPACE_4;
    } else {
        layout.padding_top = height >= 680.0f ? NK_UI_SPACE_7 : NK_UI_SPACE_4;
    }
    if (nk_ui_begin_container(ui, PAD_ID_BODY, &layout, NULL)) {
        if (calibrating) controller_calibration(ui, model, width, &result);
        else controller_mapping(ui, model, width, height, &result);
        nk_ui_end_container(ui);
    }
    nk_ui_prompt_bar(ui, PAD_ID_PROMPTS, "", calibrating ? prompts_calibrating : prompts,
                     calibrating ? 2u : 3u);
    nk_ui_end_container(ui);
    return result;
}

/* ---- Developer log --------------------------------------------------------------------
   The newest lines the window has room for, in the data face on a dark inset,
   with Copy, Save and Close under them. player_state.c has already written the
   user's folders in the lines as %USERPROFILE%. */

#define DEV_ID_ROOT 0x65000001u
#define DEV_ID_TOP 0x65000010u
#define DEV_ID_BODY 0x65000020u
#define DEV_ID_CARD 0x65000021u
#define DEV_ID_BOX 0x65000022u
#define DEV_ID_ACTIONS 0x65000023u
#define DEV_ID_PROMPTS 0x65000040u
#define DEV_ID_WAVES 0x65000050u
#define DEV_ID_TEXT 0x65000100u
#define DEV_ID_LINES 0x65001000u

#define DEV_LINE_HEIGHT 16.0f
/* The card around the log box: its padding, the eyebrow, the sentence, the
   buttons, the gaps between them and the box's own padding. */
#define DEV_CARD_CHROME (34.0f + 14.0f + 21.0f + NK_UI_CONTROL_HEIGHT + 3.0f * NK_UI_SPACE_3 + \
                         20.0f)

static PlayerUiAction developer_log_op(PlayerUiDeveloperLogOp op) {
    PlayerUiAction result;
    result.kind = PLAYER_UI_ACTION_DEVELOPER_LOG;
    result.primary = (PlayerUiPrimaryAction)op;
    result.title_index = 0u;
    return result;
}

PlayerUiAction player_ui_screen_developer_log(NkUi *ui, const PlayerUiDeveloperLogModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Close"}
    };
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    const NkUiTheme *theme;
    NkUiLayout layout;
    float width;
    float height;
    float column;
    float inner;
    float lines_height;
    size_t rows;
    size_t first;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    column = width - 2.0f * (width < NK_UI_NARROW_WIDTH ? NK_UI_GUTTER_NARROW : NK_UI_GUTTER);
    inner = column - 2.0f * NK_UI_SPACE_5;
    lines_height = height - NK_UI_TOP_BAR_HEIGHT - NK_UI_PROMPT_BAR_HEIGHT -
                   2.0f * NK_UI_SPACE_4 - DEV_CARD_CHROME;
    if (lines_height < 4.0f * DEV_LINE_HEIGHT) lines_height = 4.0f * DEV_LINE_HEIGHT;
    rows = (size_t)(lines_height / DEV_LINE_HEIGHT);
    first = model->line_count > rows ? model->line_count - rows : 0u;
    (void)nk_ui_waves_element(ui, DEV_ID_WAVES, (NkUiRect){0.0f, 0.0f, width, height}, 0u);
    layout = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_grow(), 0.0f);
    if (!nk_ui_begin_container(ui, DEV_ID_ROOT, &layout, NULL)) return result;
    (void)nk_ui_top_bar(ui, DEV_ID_TOP, NULL, 0u, nk_ui_input_label(nk_ui_prompt_style(ui)),
                        nk_ui_prompt_style(ui) != NK_UI_PROMPT_KEYCAPS);
    layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_grow(), 0.0f);
    layout.align_x = NK_UI_ALIGN_CENTER;
    layout.padding_top = layout.padding_bottom = NK_UI_SPACE_4;
    if (nk_ui_begin_container(ui, DEV_ID_BODY, &layout, NULL)) {
        if (nk_ui_card_begin(ui, DEV_ID_CARD, sizing_fixed(column), sizing_fit(),
                             NK_UI_SPACE_3)) {
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
            NkUiTextStyle eyebrow = nk_ui_type(NK_UI_TYPE_EYEBROW, theme->warning);
            nk_ui_text(ui, DEV_ID_TEXT + 0u, nk_ui_caps(ui, FLOW_MARK_TRIANGLE " Developer mode"),
                       &eyebrow);
            nk_ui_text(ui, DEV_ID_TEXT + 1u,
                       nk_ui_text_clamp(ui, "Developer log: on this PC only. Paths show as "
                                            "%USERPROFILE%.", &text, inner, 1), &text);
            {
                NkUiLayout box = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(),
                                             sizing_fixed(lines_height + 20.0f), 0.0f);
                NkUiStyle style = flow_box_style(theme, 0x080B0FFFu);
                box.padding_top = box.padding_bottom = 10.0f;
                box.padding_left = box.padding_right = 14.0f;
                if (nk_ui_begin_container(ui, DEV_ID_BOX, &box, &style)) {
                    NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
                    data.line_height = DEV_LINE_HEIGHT;
                    for (size_t i = first; i < model->line_count &&
                                           i < PLAYER_UI_DEVELOPER_LOG_LINES; ++i) {
                        nk_ui_text(ui, (NkUiId)(DEV_ID_LINES + (NkUiId)(i - first)),
                                   nk_ui_text_clamp(ui, safe_text(model->lines[i], " "), &data,
                                                    inner - 28.0f, 1), &data);
                    }
                    if (model->line_count == 0u) {
                        nk_ui_text(ui, DEV_ID_TEXT + 2u, "Nothing logged yet.", &data);
                    }
                    nk_ui_end_container(ui);
                }
            }
            layout = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
            layout.align_y = NK_UI_ALIGN_CENTER;
            if (nk_ui_begin_container(ui, DEV_ID_ACTIONS, &layout, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_DEVELOPER_LOG_COPY, "Copy", 0u)) {
                    result = developer_log_op(PLAYER_UI_DEVELOPER_LOG_COPY);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_DEVELOPER_LOG_SAVE, "Save", 0u)) {
                    result = developer_log_op(PLAYER_UI_DEVELOPER_LOG_SAVE);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_DEVELOPER_LOG_CLOSE, "Close",
                                 NK_UI_BUTTON_PRIMARY)) {
                    result = developer_log_op(PLAYER_UI_DEVELOPER_LOG_CLOSE);
                }
                if (model->notice != NULL && model->notice[0] != '\0') {
                    NkUiTextStyle note = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
                    /* The three buttons take about 300 px of the row. */
                    nk_ui_text(ui, DEV_ID_TEXT + 3u,
                               nk_ui_text_clamp(ui, model->notice, &note, inner - 320.0f, 1),
                               &note);
                }
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    nk_ui_prompt_bar(ui, DEV_ID_PROMPTS, "", prompts, sizeof(prompts) / sizeof(prompts[0]));
    nk_ui_end_container(ui);
    return result;
}
