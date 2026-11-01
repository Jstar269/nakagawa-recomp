/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Title detail: the hero art and package status on the left; the name, status
   chips, actions and compatibility card on the right; the prompt bar below.
   Layout follows the design system (docs/PLAYER_UI_DESIGN.md). */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>
#include <string.h>

#define TITLE_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_TITLE, (offset))
#define TITLE_ID_ROOT TITLE_ID(0x000001u)
#define TITLE_ID_TOP TITLE_ID(0x000010u)
#define TITLE_ID_BODY TITLE_ID(0x000020u)
#define TITLE_ID_ASIDE TITLE_ID(0x000021u)
#define TITLE_ID_MAIN TITLE_ID(0x000022u)
#define TITLE_ID_HERO TITLE_ID(0x000023u)
#define TITLE_ID_STATUS_CARD TITLE_ID(0x000009u)
#define TITLE_ID_STATUS_HEAD TITLE_ID(0x000024u)
#define TITLE_ID_STATUS_EYEBROW TITLE_ID(0x000025u)
#define TITLE_ID_STATUS_PILL TITLE_ID(0x000026u)
#define TITLE_ID_STATUS_DETAIL TITLE_ID(0x00000Au)
#define TITLE_ID_NOTE TITLE_ID(0x00000Bu)
#define TITLE_ID_NOTE_TEXT TITLE_ID(0x00000Cu)
#define TITLE_ID_CRUMB TITLE_ID(0x000002u)
#define TITLE_ID_NAME TITLE_ID(0x000003u)
#define TITLE_ID_DISC TITLE_ID(0x000004u)
#define TITLE_ID_CHIPS TITLE_ID(0x000027u)
#define TITLE_ID_CHIP_STATUS TITLE_ID(0x000028u)
#define TITLE_ID_CHIP_BADGE TITLE_ID(0x000029u)
#define TITLE_ID_ACTIONS TITLE_ID(0x00002Au)
#define TITLE_ID_BUILD_PREFLIGHT TITLE_ID(0x000030u)
#define TITLE_ID_BUILD_FOLDER TITLE_ID(0x000031u)
#define TITLE_ID_INPUT_ACTIONS TITLE_ID(0x000032u)
#define TITLE_ID_INPUT_GUIDE TITLE_ID(0x000100u)
#define TITLE_ID_STALL_NOTICE TITLE_ID(0x000035u)
#define TITLE_ID_STALL_EYEBROW TITLE_ID(0x000036u)
#define TITLE_ID_STALL_TEXT TITLE_ID(0x000037u)
#define TITLE_ID_STALL_ACTIONS TITLE_ID(0x000038u)
#define TITLE_ID_START_CARD TITLE_ID(0x000039u)
#define TITLE_ID_START_NAME TITLE_ID(0x00003Au)
#define TITLE_ID_START_TEXT TITLE_ID(0x00003Bu)
#define TITLE_ID_START_BAR TITLE_ID(0x00003Cu)
#define TITLE_ID_COMPAT TITLE_ID(0x000005u)
#define TITLE_ID_COMPAT_HEAD TITLE_ID(0x00002Cu)
#define TITLE_ID_COMPAT_EYEBROW TITLE_ID(0x000006u)
#define TITLE_ID_COMPAT_TIER TITLE_ID(0x000007u)
#define TITLE_ID_COMPAT_EXPECT TITLE_ID(0x000008u)
#define TITLE_ID_COMPAT_ISSUES TITLE_ID(0x00002Eu)
#define TITLE_ID_COMPAT_CATEGORY TITLE_ID(0x00002Du)
#define TITLE_ID_REMOVE_ROW TITLE_ID(0x00002Fu)
#define TITLE_ID_ENH_CARD TITLE_ID(0x000060u)
#define TITLE_ID_ENH_HEAD TITLE_ID(0x000061u)
#define TITLE_ID_ENH_EYEBROW TITLE_ID(0x000062u)
#define TITLE_ID_ENH_TAG TITLE_ID(0x000063u)
#define TITLE_ID_ENH_ROW TITLE_ID(0x000064u)
#define TITLE_ID_ENH_TEXT TITLE_ID(0x000065u)
#define TITLE_ID_ENH_PROBLEM TITLE_ID(0x000066u)
#define TITLE_ID_ENH_SESSION TITLE_ID(0x000067u)
#define TITLE_ID_ACE_ROW TITLE_ID(0x000068u)
#define TITLE_ID_ACE_TEXT TITLE_ID(0x000069u)
#define TITLE_ID_PROMPTS TITLE_ID(0x000040u)
#define TITLE_ID_WAVES TITLE_ID(0x000050u)

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
            return "Built, in use by the running game. Stop the game to rebuild or change "
                   "settings.";
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
    layout.scroll = page_scrolls(ui);
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
                    if (title->package_state == PLAYER_UI_PACKAGE_STARTING ||
                        title->package_state == PLAYER_UI_PACKAGE_RUNNING) {
                        /* The header badge says Starting or Running once; the
                           package card describes the package. */
                        nk_ui_status_pill(ui, TITLE_ID_STATUS_PILL, "\xE2\x97\x8F", "Ready",
                                          theme->ready);
                    } else if (title->library_status != NULL &&
                               title->library_status[0] != '\0') {
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
            /* Disc verification: what the badge's Verified retail, Modified
               disc or Unknown dump means for this image. */
            if (title->disc_check_note != NULL && title->disc_check_note[0] != '\0') {
                nk_ui_text_role(ui, PLAYER_UI_ID_DISC_VERIFY_TITLE_NOTE, title->disc_check_note,
                                NK_UI_TYPE_BODY_S, theme->muted);
            }
            if (model->stalled_game_notice_visible &&
                nk_ui_card_begin(ui, TITLE_ID_STALL_NOTICE, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_2)) {
                nk_ui_eyebrow(ui, TITLE_ID_STALL_EYEBROW,
                              model->stalled_game_dark_picture ? "Black screen" : "No new frames");
                nk_ui_text_role(ui, TITLE_ID_STALL_TEXT,
                                model->stalled_game_dark_picture
                                    ? "The game is running but has shown only a black screen for 30 seconds. It may be stuck."
                                    : "The game hasn't drawn anything new for 30 seconds. It may be stuck.",
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
            {
                /* A larger text size can wrap the row onto a second one. */
                NkUiFlowRow flow;
                if (nk_ui_flow_begin(ui, &flow, TITLE_ID_ACTIONS, &row,
                                     width - 2.0f * gutter - NK_UI_DETAIL_GAP - aside)) {
                    /* While the no-new-frames card is up, its STOP GAME is the only
                       one on the page. */
                    bool stop_in_stall_card = model->stalled_game_notice_visible &&
                        title->primary_action == PLAYER_UI_PRIMARY_STOP_GAME;
                    const char *primary = safe_text(title->primary_action_label, "Unavailable");
                    uint32_t primary_flags = NK_UI_BUTTON_LARGE |
                        (title->primary_action_enabled ? NK_UI_BUTTON_PRIMARY
                                                       : NK_UI_BUTTON_DISABLED);
                    uint32_t save_flags = NK_UI_BUTTON_LARGE |
                        (title->save_folder_enabled ? 0u : NK_UI_BUTTON_DISABLED);
                    if (!stop_in_stall_card) {
                        nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, primary, primary_flags));
                        if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_PRIMARY, primary, primary_flags)) {
                            result = action(PLAYER_UI_ACTION_PRIMARY, model->title_index,
                                            title->primary_action);
                        }
                    }
                    /* An older Nakagawa built the package: PLAY stays first, and
                       Rebuild sits beside it (the package card says why). */
                    if (title->rebuild_offered) {
                        nk_ui_flow_item(ui, &flow,
                                        nk_ui_button_width(ui, "Rebuild", NK_UI_BUTTON_LARGE));
                        if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_REBUILD, "Rebuild",
                                         NK_UI_BUTTON_LARGE | NK_UI_BUTTON_NOT_DEFAULT)) {
                            result = action(PLAYER_UI_ACTION_REBUILD, model->title_index,
                                            (PlayerUiPrimaryAction)PLAYER_UI_REBUILD_TITLE);
                        }
                    }
                    nk_ui_flow_item(ui, &flow,
                                    nk_ui_button_width(ui, "Open save folder", save_flags));
                    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_SAVE_FOLDER, "Open save folder",
                                     save_flags)) {
                        result = action(PLAYER_UI_ACTION_OPEN_SAVE_FOLDER,
                                        model->title_index, PLAYER_UI_PRIMARY_NONE);
                    }
                    nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, "Back", NK_UI_BUTTON_LARGE));
                    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_BACK, "Back", NK_UI_BUTTON_LARGE)) {
                        result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                                        PLAYER_UI_PRIMARY_NONE);
                    }
                    nk_ui_flow_end(ui, &flow);
                }
            }
            if (scr_input_guide_present(&model->input_guide)) {
                /* The build waits for decrypted copies from the user's disc
                   (#308): which files, where they go, and a check that runs
                   again without adding the disc again. */
                float main_width = width - 2.0f * gutter - NK_UI_DETAIL_GAP - aside;
                scr_input_guide_box(ui, TITLE_ID_INPUT_GUIDE, &model->input_guide, main_width,
                                height >= 680.0f ? PLAYER_UI_INPUT_FILES : 2u);
                row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
                row.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, TITLE_ID_INPUT_ACTIONS, &row, NULL)) {
                    if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_CHECK_AGAIN, "Check again", 0u)) {
                        result = action(PLAYER_UI_ACTION_CHECK_INPUT_AGAIN,
                                        model->title_index, PLAYER_UI_PRIMARY_NONE);
                    }
                    if (model->input_guide.folder != NULL &&
                        model->input_guide.folder[0] != '\0' &&
                        nk_ui_button(ui, PLAYER_UI_ID_TITLE_OPEN_INPUT_FOLDER, "Open folder",
                                     0u)) {
                        result = action(PLAYER_UI_ACTION_OPEN_BUILD_INPUT_FOLDER,
                                        model->title_index, PLAYER_UI_PRIMARY_NONE);
                    }
                    nk_ui_end_container(ui);
                }
            } else if (title->build_preflight_message != NULL &&
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
            /* The first start of a new package took 7 to 9 s before its window
               opened (v0.0.1 walkthrough); a chip and one line in the package
               card are easy to miss in that wait, so a card says what is
               happening, with a moving bar. */
            if (title->package_state == PLAYER_UI_PACKAGE_STARTING &&
                nk_ui_card_begin(ui, TITLE_ID_START_CARD, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_3)) {
                float bar = width - 2.0f * gutter - NK_UI_DETAIL_GAP - aside -
                            2.0f * NK_UI_SPACE_5;
                char line[320];
                NkUiTextStyle name = nk_ui_type(NK_UI_TYPE_HEADING, theme->paper);
                (void)snprintf(line, sizeof(line), "Starting %s\xE2\x80\xA6",
                               safe_text(title->title, "the game"));
                nk_ui_text(ui, TITLE_ID_START_NAME,
                           nk_ui_text_clamp(ui, line, &name, bar, 1), &name);
                nk_ui_text_role(ui, TITLE_ID_START_TEXT,
                                "Its window opens when it is ready. The first start can "
                                "take several seconds.",
                                NK_UI_TYPE_BODY_S, theme->muted);
                nk_ui_progress_bar(ui, TITLE_ID_START_BAR, bar > 40.0f ? bar : 40.0f,
                                   -1.0f, model->now_s);
                nk_ui_end_container(ui);
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
            /* Enhancements, apart from what to expect of the game as released:
               Ace Mode for every game (src/rt/ace.h), and an optional enhancement
               made for this game (60 FPS game logic, docs/ENHANCEMENT_CONTRACT.md
               section 9). Off, and Authentic, are the original game. */
            if ((title->enhancement_shown || title->ace_shown) &&
                nk_ui_card_begin(ui, TITLE_ID_ENH_CARD, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_3)) {
                NkUiLayout head = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_3);
                head.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, TITLE_ID_ENH_HEAD, &head, NULL)) {
                    nk_ui_eyebrow(ui, TITLE_ID_ENH_EYEBROW, "Enhancements");
                    if (title->enhancement_shown) nk_ui_tag(ui, TITLE_ID_ENH_TAG, "Experimental");
                    nk_ui_end_container(ui);
                }
                /* Ace Mode (src/rt/ace.h): library default, on, or authentic for this game;
                   then the 60 FPS switch on the same row where the game has a package. */
                {
                    float inner = width - 2.0f * gutter - NK_UI_DETAIL_GAP - aside -
                                  2.0f * NK_UI_SPACE_5;
                    NkUiFlowRow buttons;
                    char label[48];
                    (void)snprintf(label, sizeof(label), "60 FPS game logic: %s",
                                   title->enhancement_on ? "On" : "Off");
                    row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
                    if (nk_ui_flow_begin(ui, &buttons, TITLE_ID_ACE_ROW, &row, inner)) {
                        if (title->ace_shown) {
                            const char *ace = safe_text(title->ace_label, "Ace Mode");
                            nk_ui_flow_item(ui, &buttons, nk_ui_button_width(ui, ace, 0u));
                            if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_ACE, ace,
                                             NK_UI_BUTTON_NOT_DEFAULT |
                                                 (title->ace_in_effect ? NK_UI_BUTTON_PRIMARY : 0u) |
                                                 (title->ace_enabled ? 0u
                                                                     : NK_UI_BUTTON_DISABLED))) {
                                result = action(PLAYER_UI_ACTION_CYCLE_ACE, model->title_index,
                                                PLAYER_UI_PRIMARY_NONE);
                            }
                        }
                        if (title->enhancement_shown) {
                            nk_ui_flow_item(ui, &buttons, nk_ui_button_width(ui, label, 0u));
                            if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_60FPS, label,
                                             NK_UI_BUTTON_NOT_DEFAULT |
                                             (title->enhancement_on ? NK_UI_BUTTON_PRIMARY : 0u) |
                                             (title->enhancement_enabled ? 0u
                                                                         : NK_UI_BUTTON_DISABLED))) {
                                result = action(PLAYER_UI_ACTION_TOGGLE_60FPS, model->title_index,
                                                PLAYER_UI_PRIMARY_NONE);
                            }
                        }
                        nk_ui_flow_end(ui, &buttons);
                    }
                    /* What Ace Mode turns on, then what it did last time: one paragraph of
                       two lines at most. Beside the 60 FPS paragraph it has one line, the last
                       session when there is one (the button already says the state), so the
                       card fits the smallest window. */
                    if (title->ace_shown && title->ace_line != NULL && title->ace_line[0] != '\0') {
                        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
                        char paragraph[480];
                        bool last = title->ace_last_session != NULL &&
                                    title->ace_last_session[0] != '\0';
                        if (title->enhancement_shown) {
                            (void)snprintf(paragraph, sizeof(paragraph), "%s",
                                           last ? title->ace_last_session : title->ace_line);
                        } else {
                            (void)snprintf(paragraph, sizeof(paragraph), "%s%s%s", title->ace_line,
                                           last ? " " : "", last ? title->ace_last_session : "");
                        }
                        nk_ui_text(ui, TITLE_ID_ACE_TEXT,
                                   nk_ui_text_clamp(ui, paragraph, &text, inner,
                                                    title->enhancement_shown ? 1 : 2),
                                   &text);
                    }
                }
                if (title->enhancement_shown) {
                    nk_ui_text_role(ui, TITLE_ID_ENH_TEXT,
                                    "An optional enhancement made for this game: its logic runs "
                                    "60 times a second instead of 30, from its own package. Off "
                                    "plays the original game as released. If this PC falls "
                                    "behind, it says SLOWED.",
                                    NK_UI_TYPE_BODY_S, theme->paper);
                    if (title->enhancement_problem != NULL &&
                        title->enhancement_problem[0] != '\0') {
                        char line[256];
                        (void)snprintf(line, sizeof(line),
                                       "%s Turn it off to play the original game.",
                                       title->enhancement_problem);
                        nk_ui_text_role(ui, TITLE_ID_ENH_PROBLEM, nk_ui_frame_text(ui, line),
                                        NK_UI_TYPE_BODY_S, theme->error);
                    }
                    if (title->enhancement_session) {
                        nk_ui_text_role(ui, TITLE_ID_ENH_SESSION,
                                        "On for this session: Nakagawa was started with "
                                        "--experimental-60hz.",
                                        NK_UI_TYPE_CAPTION, theme->muted);
                    }
                }
                nk_ui_end_container(ui);
            }
            /* Report a problem, a link the user's browser opens, and Remove
               from library (#1304), apart from the play actions: it deletes
               what Nakagawa built, so it asks first. */
            bool can_report = title->disc_id != NULL && title->disc_id[0] != '\0';
            if (can_report || title->can_remove) {
                NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                             NK_UI_SPACE_3);
                if (nk_ui_begin_container(ui, TITLE_ID_REMOVE_ROW, &row, NULL)) {
                    if (can_report &&
                        nk_ui_button(ui, PLAYER_UI_ID_TITLE_REPORT, "Report a problem", 0u)) {
                        result = action(PLAYER_UI_ACTION_REPORT_PROBLEM, model->title_index,
                                        PLAYER_UI_PRIMARY_NONE);
                    }
                    if (title->can_remove &&
                        nk_ui_button(ui, PLAYER_UI_ID_TITLE_REMOVE, "Remove from library",
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
