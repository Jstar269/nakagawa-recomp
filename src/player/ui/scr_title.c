/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Title detail: the hero art and package status on the left; the name, status
   chips, actions and compatibility card on the right; the prompt bar below.
   Layout follows the design system (docs/PLAYER_UI_DESIGN.md). */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"

#include <math.h>
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

/* Whether a page's body between the top bar and the prompt bar scrolls. At the
   design text size every page fits the window, and the screen tests hold it to
   that; a larger text size can make one taller than the room, and then its
   body scrolls (with the focused control brought into view) instead of pushing
   the prompt bar off the window. */
static bool page_scrolls(const NkUi *ui) {
    return nk_ui_text_scale(ui) > 1.0f;
}

/* An outcome line's colour: only a warning is amber, and only a failure red. */
static uint32_t note_rgba(const NkUiTheme *theme, PlayerUiNoteKind kind) {
    switch (kind) {
    case PLAYER_UI_NOTE_DONE: return theme->ready;
    case PLAYER_UI_NOTE_WARNING: return theme->warning;
    case PLAYER_UI_NOTE_ERROR: return theme->error;
    case PLAYER_UI_NOTE_NEUTRAL:
    default: return theme->muted;
    }
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
            return "Built, in use by the running game. Stop the game to rebuild or change "
                   "settings.";
        case PLAYER_UI_PACKAGE_MISSING:
        default: return "The package for this game is not ready yet.";
    }
}

/* ---- Files a disc needs (#308) ----------------------------------------------------
   Why the files are needed, one row per file (still needed, or a copy to
   replace; the file to add; what it is a copy of), a count and the folder.
   The title page, the game-found card and the wizard's Verify step share it;
   each keeps its own buttons (Check again, Open folder). The guide shows only
   when built-in decryption (#295) stopped; its words never ask for a key and
   never say how to decrypt. */

#define INPUT_GUIDE_LEAD \
    "Nakagawa couldn't decrypt some of this game's program files from your disc. It can " \
    "use decrypted copies you make from your own disc: put these files in the folder " \
    "below, then choose Check again."

static bool input_guide_present(const PlayerUiInputGuide *guide) {
    return guide != NULL && guide->file_count > 0u;
}

/* max_rows bounds the listed files on a short window; the count line says how
   many are not shown. A short window (max_rows of 2 or fewer) also gets the
   compact form: no eyebrow, two lines of the reason, one of the folder, and the
   Replace marks without their sentence. */
static void input_guide_box(NkUi *ui, NkUiId base, const PlayerUiInputGuide *guide,
                            float width, size_t max_rows) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout box = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 6.0f);
    NkUiStyle style;
    NkUiTextStyle lead = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
    NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
    float inner = width - 28.0f;
    size_t shown;
    bool replace = false;
    bool compact = max_rows > 0u && max_rows <= 2u;
    if (!input_guide_present(guide)) return;
    if (inner < 120.0f) inner = 120.0f;
    memset(&style, 0, sizeof(style));
    style.fill_rgba = theme->surface;
    style.border_rgba = theme->line;
    style.border_width = NK_UI_BORDER_HAIRLINE;
    style.corner_radius = 10.0f;
    box.padding_top = box.padding_bottom = compact ? 8.0f : 12.0f;
    box.padding_left = box.padding_right = 14.0f;
    if (compact) box.gap = 4.0f;
    if (!nk_ui_begin_container(ui, base, &box, &style)) return;
    if (!compact) nk_ui_eyebrow(ui, base + 1u, "Files to add");
    nk_ui_text(ui, base + 2u,
               nk_ui_text_clamp(ui, INPUT_GUIDE_LEAD, &lead, inner, compact ? 2 : 3), &lead);
    shown = guide->file_count < PLAYER_UI_INPUT_FILES ? guide->file_count : PLAYER_UI_INPUT_FILES;
    if (max_rows > 0u && shown > max_rows) shown = max_rows;
    for (size_t i = 0u; i < guide->file_count && i < PLAYER_UI_INPUT_FILES; ++i) {
        if (guide->files[i].replace) replace = true;
    }
    for (size_t i = 0u; i < shown; ++i) {
        const PlayerUiInputFile *file = &guide->files[i];
        NkUiId row_id = (NkUiId)(base + 0x10u + (NkUiId)i * 8u);
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
        row.align_y = NK_UI_ALIGN_CENTER;
        if (nk_ui_begin_container(ui, row_id, &row, NULL)) {
            NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(84.0f), sizing_fit(), 0.0f);
            NkUiTextStyle name = nk_ui_type(NK_UI_TYPE_DATA, theme->paper);
            NkUiTextStyle source = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
            char copy_of[200];
            float rest = inner - 84.0f - 150.0f - 2.0f * NK_UI_SPACE_3;
            if (nk_ui_begin_container(ui, row_id + 1u, &cell, NULL)) {
                nk_ui_text_role(ui, row_id + 4u, file->replace ? "REPLACE" : "NEEDED",
                                NK_UI_TYPE_LABEL_S, file->replace ? theme->error : theme->warning);
                nk_ui_end_container(ui);
            }
            cell.width = sizing_fixed(150.0f);
            if (nk_ui_begin_container(ui, row_id + 2u, &cell, NULL)) {
                nk_ui_text(ui, row_id + 5u,
                           nk_ui_text_clamp(ui, safe_text(file->name, "?"), &name, 150.0f, 1),
                           &name);
                nk_ui_end_container(ui);
            }
            (void)snprintf(copy_of, sizeof(copy_of), "copy of %s",
                           safe_text(file->source, "a file on the disc"));
            if (rest < 60.0f) rest = 60.0f;
            nk_ui_text(ui, row_id + 3u, nk_ui_text_clamp(ui, copy_of, &source, rest, 1), &source);
            nk_ui_end_container(ui);
        }
    }
    {
        char count[192];
        size_t hidden = guide->file_count > shown ? guide->file_count - shown : 0u;
        if (hidden > 0u) {
            (void)snprintf(count, sizeof(count), "%s \xC2\xB7 %u more not shown",
                           safe_text(guide->summary, ""), (unsigned)hidden);
        } else {
            (void)snprintf(count, sizeof(count), "%s", safe_text(guide->summary, ""));
        }
        if (count[0] != '\0') {
            NkUiTextStyle caption = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
            nk_ui_text(ui, base + 3u, nk_ui_text_clamp(ui, count, &caption, inner, 1), &caption);
        }
    }
    if (replace && !compact) {
        NkUiTextStyle caption = nk_ui_type(NK_UI_TYPE_CAPTION, theme->error);
        nk_ui_text(ui, base + 4u,
                   nk_ui_text_clamp(ui,
                                    "A file marked Replace is in the folder but isn't a working "
                                    "program file. Put a new copy in its place.",
                                    &caption, inner, 2),
                   &caption);
    }
    if (guide->folder != NULL && guide->folder[0] != '\0') {
        data.wrap = true;
        nk_ui_text(ui, base + 5u,
                   nk_ui_text_clamp(ui, guide->folder, &data, inner, compact ? 1 : 2), &data);
    }
    nk_ui_end_container(ui);
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
            if (input_guide_present(&model->input_guide)) {
                /* The build waits for decrypted copies from the user's disc
                   (#308): which files, where they go, and a check that runs
                   again without adding the disc again. */
                float main_width = width - 2.0f * gutter - NK_UI_DETAIL_GAP - aside;
                input_guide_box(ui, TITLE_ID_INPUT_GUIDE, &model->input_guide, main_width,
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

/* ---- Progress and dialogs ------------------------------------------------------
   The screens a first run passes through after the library: the package build
   with its live stages, the one-time build tool download (consent, then
   progress), and the error card. Each is one centred dialog card over the
   launcher background, with the top bar's brand and the prompt bar. */

#define FLOW_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_FLOW, (offset))
#define FLOW_ID_ROOT FLOW_ID(0x000001u)
#define FLOW_ID_TOP FLOW_ID(0x000010u)
#define FLOW_ID_BODY FLOW_ID(0x000020u)
#define FLOW_ID_CARD FLOW_ID(0x000021u)
#define FLOW_ID_EYEBROW FLOW_ID(0x000022u)
#define FLOW_ID_TITLE FLOW_ID(0x000023u)
#define FLOW_ID_LEAD FLOW_ID(0x000024u)
#define FLOW_ID_STAGES FLOW_ID(0x000030u)
#define FLOW_ID_STAGES_SPAN 0x10u
#define FLOW_ID_STAGE_LABELS FLOW_ID(0x000130u)
#define FLOW_ID_STAGE_LABELS_SPAN 0x10u
#define FLOW_ID_BAR FLOW_ID(0x000040u)
#define FLOW_ID_STATUS FLOW_ID(0x000041u)
#define FLOW_ID_STATUS_SPACER FLOW_ID(0x000141u)
#define FLOW_ID_MESSAGE FLOW_ID(0x000042u)
#define FLOW_ID_ELAPSED FLOW_ID(0x000043u)
#define FLOW_ID_NOTICE FLOW_ID(0x000044u)
#define FLOW_ID_LOG FLOW_ID(0x000050u)
#define FLOW_ID_LOG_SPAN 0x10u
#define FLOW_ID_ROWS FLOW_ID(0x000060u)
#define FLOW_ID_ROWS_SPAN 0x10u
#define FLOW_ID_ROWS_MORE FLOW_ID(0x000150u)       /* "and 3 more" under the rows */
#define FLOW_ID_ITEMS FLOW_ID(0x000200u)           /* Remove: + 0x10 * part + item */
#define FLOW_ID_ITEMS_SPAN 0x40u
#define FLOW_ID_TOTAL FLOW_ID(0x000070u)
#define FLOW_ID_ACTIONS FLOW_ID(0x000080u)
#define FLOW_ID_DETAILS FLOW_ID(0x000090u)
#define FLOW_ID_DETAILS_SPAN 0x8u
#define FLOW_ID_REPORT_TEXT FLOW_ID(0x000098u)
#define FLOW_ID_REPORT_ROW FLOW_ID(0x000099u)
#define FLOW_ID_PROMPTS FLOW_ID(0x0000A0u)
#define FLOW_ID_WAVES FLOW_ID(0x0000B0u)

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
    /* A card taller than the room between the bars scrolls there instead of
       covering them (page_scrolls). */
    if (!nk_ui_dialog_begin_scroll(ui, FLOW_ID_CARD, card,
                                   page_scrolls(ui)
                                       ? height - NK_UI_TOP_BAR_HEIGHT - NK_UI_PROMPT_BAR_HEIGHT -
                                             2.0f * NK_UI_SPACE_4
                                       : 0.0f)) {
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
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_STAGE_LABELS + (NkUiId)i),
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
            nk_ui_spacer(ui, FLOW_ID_STATUS_SPACER, sizing_grow(), sizing_fixed(1.0f));
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
                if (nk_ui_begin_container(ui, (NkUiId)(FLOW_ID_ITEMS + (NkUiId)i), &line,
                                          NULL)) {
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ITEMS + 0x10u + (NkUiId)i),
                                    safe_text(model->items[i].label, "Built files"),
                                    NK_UI_TYPE_BODY_S, theme->paper);
                    nk_ui_spacer(ui, (NkUiId)(FLOW_ID_ITEMS + 0x20u + (NkUiId)i),
                                 sizing_grow(), sizing_fixed(1.0f));
                    flow_bytes(size, sizeof(size), model->items[i].bytes);
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ITEMS + 0x30u + (NkUiId)i),
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

/* Stop game: the same question quitting with a game running asks, in a flow
   card. KEEP PLAYING holds focus first; STOP GAME is the danger button. */
PlayerUiAction player_ui_screen_stop(NkUi *ui, const PlayerUiStopModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Keep playing"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char text[320];
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    inner = flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 640.0f);
    if (inner <= 0.0f) return result;
    flow_eyebrow(ui, FLOW_MARK_TRIANGLE, "Stop game", theme->warning);
    (void)snprintf(text, sizeof(text), "Stop %s?", safe_text(model->title, "the game"));
    nk_ui_text_role(ui, FLOW_ID_TITLE, nk_ui_frame_text(ui, text), NK_UI_TYPE_TITLE,
                    theme->paper);
    nk_ui_text_role(ui, FLOW_ID_LEAD,
                    "The game closes now, and anything since its last save is lost.",
                    NK_UI_TYPE_BODY_S, theme->muted);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_STOP_KEEP, "Keep playing", 0u)) {
                result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                                PLAYER_UI_PRIMARY_NONE);
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_STOP_CONFIRM, "Stop game", NK_UI_BUTTON_DANGER)) {
                result = action(PLAYER_UI_ACTION_PRIMARY, model->title_index,
                                PLAYER_UI_PRIMARY_STOP_GAME);
            }
            nk_ui_end_container(ui);
        }
    }
    flow_end(ui, "", prompts, 3u);
    return result;
}

/* Games an older Nakagawa built: the once-per-update prompt (Rebuild all,
   Later) or, with a title, the reminder a launch shows (Rebuild now, Play
   anyway). The rebuild is the primary choice and holds focus first; Back is
   the second choice, so nothing is rebuilt by accident. */
PlayerUiAction player_ui_screen_rebuild(NkUi *ui, const PlayerUiRebuildModel *model) {
    static const NkUiPromptItem prompts_update[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Later"}
    };
    static const NkUiPromptItem prompts_launch[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Play anyway"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    bool launch;
    float inner;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    launch = model->title != NULL;
    inner = flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 640.0f);
    if (inner <= 0.0f) return result;
    flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Older build", theme->info);
    nk_ui_text_role(ui, FLOW_ID_TITLE,
                    launch ? nk_ui_frame_text(ui, model->title) : "Rebuild your games",
                    NK_UI_TYPE_TITLE, theme->paper);
    nk_ui_text_role(ui, FLOW_ID_LEAD, safe_text(model->text, ""), NK_UI_TYPE_BODY_S,
                    theme->muted);
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_REBUILD_PRIMARY,
                             launch ? "Rebuild now" : "Rebuild all", NK_UI_BUTTON_PRIMARY)) {
                result = action(PLAYER_UI_ACTION_REBUILD, 0u,
                                (PlayerUiPrimaryAction)(launch ? PLAYER_UI_REBUILD_NOW
                                                               : PLAYER_UI_REBUILD_ALL));
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_REBUILD_SECOND,
                             launch ? "Play anyway" : "Later", NK_UI_BUTTON_NOT_DEFAULT)) {
                result = action(PLAYER_UI_ACTION_REBUILD, 0u,
                                (PlayerUiPrimaryAction)(launch ? PLAYER_UI_REBUILD_PLAY_ANYWAY
                                                               : PLAYER_UI_REBUILD_LATER));
            }
            nk_ui_end_container(ui);
        }
    }
    if (result.kind == PLAYER_UI_ACTION_NONE && nk_ui_take_event(ui, NK_UI_EV_BACK, NULL)) {
        result = action(PLAYER_UI_ACTION_REBUILD, 0u,
                        (PlayerUiPrimaryAction)(launch ? PLAYER_UI_REBUILD_PLAY_ANYWAY
                                                       : PLAYER_UI_REBUILD_LATER));
    }
    flow_end(ui, "", launch ? prompts_launch : prompts_update, 3u);
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
                    nk_ui_text(ui, FLOW_ID_ROWS_MORE, text, &line);
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
    /* Report this game: links the user's browser opens, on a row of their
       own. A known cause leads with its issue and Report steps back. */
    if (model->report_available) {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.align_y = NK_UI_ALIGN_CENTER;
        if (model->known_issue != 0u) {
            char known[64];
            (void)snprintf(known, sizeof(known), "This is a known issue (#%u).",
                           model->known_issue);
            nk_ui_text_role(ui, FLOW_ID_REPORT_TEXT, nk_ui_frame_text(ui, known),
                            NK_UI_TYPE_BODY_S, theme->paper);
        }
        if (nk_ui_begin_container(ui, FLOW_ID_REPORT_ROW, &row, NULL)) {
            if (model->known_issue != 0u) {
                char issue[32];
                (void)snprintf(issue, sizeof(issue), "See issue #%u", model->known_issue);
                if (nk_ui_button(ui, PLAYER_UI_ID_ERROR_ISSUE, nk_ui_frame_text(ui, issue),
                                 0u)) {
                    result = action(PLAYER_UI_ACTION_ERROR_KNOWN_ISSUE, 0u,
                                    PLAYER_UI_PRIMARY_NONE);
                }
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_ERROR_REPORT, "Report this game",
                             model->known_issue != 0u ? NK_UI_BUTTON_SMALL : 0u)) {
                result = action(PLAYER_UI_ACTION_ERROR_REPORT, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_ERROR_REPORTS, "See existing reports",
                             NK_UI_BUTTON_SMALL)) {
                result = action(PLAYER_UI_ACTION_ERROR_SEE_REPORTS, 0u, PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_end_container(ui);
        }
    }
    flow_end(ui, model->report_available
                     ? "Copy details uses the clipboard and Report opens your browser. "
                       "Nothing is sent anywhere."
                 : model->copy_available
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

#define ABOUT_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_ABOUT, (offset))
#define ABOUT_ID_ROOT ABOUT_ID(0x000001u)
#define ABOUT_ID_TOP ABOUT_ID(0x000010u)
#define ABOUT_ID_BODY ABOUT_ID(0x000020u)
#define ABOUT_ID_COLUMN ABOUT_ID(0x000021u)
#define ABOUT_ID_HEAD ABOUT_ID(0x000022u)
#define ABOUT_ID_MARK ABOUT_ID(0x000023u)
#define ABOUT_ID_NAMES ABOUT_ID(0x000024u)
#define ABOUT_ID_NAME ABOUT_ID(0x000025u)
#define ABOUT_ID_VERSION ABOUT_ID(0x000026u)
#define ABOUT_ID_LEAD ABOUT_ID(0x000027u)
#define ABOUT_ID_CARDS ABOUT_ID(0x000028u)
#define ABOUT_ID_LICENSE ABOUT_ID(0x000029u)
#define ABOUT_ID_CREDITS ABOUT_ID(0x00002Au)
#define ABOUT_ID_NOTICE ABOUT_ID(0x00002Bu)
#define ABOUT_ID_LINK ABOUT_ID(0x00002Cu)
#define ABOUT_ID_BUILD ABOUT_ID(0x00002Du)
#define ABOUT_ID_FOOT ABOUT_ID(0x00002Eu)
#define ABOUT_ID_PROMPTS ABOUT_ID(0x000040u)
#define ABOUT_ID_WAVES ABOUT_ID(0x000050u)

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
    layout.scroll = page_scrolls(ui);
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
                /* 300 px, or wide enough for its button at a larger text size. */
                float licence = nk_ui_button_width(ui, "Third-party notices", 0u) +
                                2.0f * NK_UI_SPACE_5;
                if (licence < 300.0f) licence = 300.0f;
                if (nk_ui_card_begin(ui, ABOUT_ID_LICENSE, sizing_fixed(licence), sizing_grow(),
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
            /* The project link and the source commit share one line, so the
               page fits the 1280 x 720 design size. */
            row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_6);
            row.align_y = NK_UI_ALIGN_CENTER;
            if (nk_ui_begin_container(ui, ABOUT_ID_FOOT, &row, NULL)) {
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

#define WIZ_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_WIZARD, (offset))
#define WIZ_ID_STEPS WIZ_ID(0x000001u)
#define WIZ_ID_STEPS_SPAN 0x8u
#define WIZ_ID_STEP_LABELS WIZ_ID(0x000101u)
#define WIZ_ID_STEP_LABELS_SPAN 0x8u
#define WIZ_ID_BOX WIZ_ID(0x000010u)
#define WIZ_ID_ROWS WIZ_ID(0x000200u)
#define WIZ_ID_ROWS_SPAN 0x100u
#define WIZ_ID_BUTTONS WIZ_ID(0x000030u)
#define WIZ_ID_EXTRA WIZ_ID(0x000031u)
#define WIZ_ID_TEXT WIZ_ID(0x001000u)
#define WIZ_ID_TEXT_SPAN 0x1000u
#define WIZ_ID_GUIDE WIZ_ID(0x000300u)

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

/* The chips name the steps a first run shows (PLAYER_UI_WIZARD_ROUTE_STEPS):
   copying the files ends on the library card, so no Fonts or Ready chip
   promises a step that never comes. */
static void wizard_steps(NkUi *ui, int step) {
    static const char *const names[PLAYER_UI_WIZARD_ROUTE_STEPS] = {"Welcome", "Options",
                                                                    "Game", "Verify"};
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_2);
    if (!nk_ui_begin_container(ui, WIZ_ID_STEPS, &row, NULL)) return;
    for (int i = 0; i < PLAYER_UI_WIZARD_ROUTE_STEPS; ++i) {
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
            nk_ui_text_role(ui, (NkUiId)(WIZ_ID_STEP_LABELS + (NkUiId)i),
                            nk_ui_caps(ui, label), NK_UI_TYPE_LABEL_S,
                            current ? theme->ink : done ? theme->ready : theme->muted);
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

/* The width a column of check rows takes: its design width, or the widest of
   its entries (the status words in caps, or the names) in `role` when a larger
   text size makes one wider. */
static float check_column_width(NkUi *ui, const PlayerUiCheckRow *checks, size_t count,
                                bool status, NkUiTypeRole role, const char *fallback,
                                float design) {
    NkUiTextStyle style = nk_ui_type(role, 0u);
    float width = design;
    for (size_t i = 0u; checks != NULL && i < count; ++i) {
        const char *text = status ? nk_ui_caps(ui, safe_text(checks[i].status, fallback))
                                  : safe_text(checks[i].name, fallback);
        float need = ceilf(nk_ui_text_width(ui, text, &style));
        if (need > width) width = need;
    }
    return width;
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

/* The button column of a setting row, and the least room its sentence keeps
   beside it before the sentences move under the buttons. */
#define WIZARD_VALUE_CELL 220.0f
#define WIZARD_VALUE_SENTENCE_MIN 260.0f

/* What one setting row shows: its button reads "name: value". */
typedef struct {
    const char *name;
    const char *value;
    const char *sentence;
} WizardRow;

/* How a group of setting rows shares its box: the button column they line up on,
   and whether each sentence sits under its button instead of beside it. */
typedef struct {
    float cell;
    bool stacked;
} WizardColumn;

/* Whether nk_ui_text_clamp cut `text` short to give `shown`. */
static bool text_was_cut(const char *shown, const char *text) {
    static const char ellipsis[] = "\xE2\x80\xA6";
    size_t n = sizeof(ellipsis) - 1u;
    size_t shown_length = strlen(shown);
    size_t text_length = strlen(text);
    return shown_length >= n && memcmp(shown + shown_length - n, ellipsis, n) == 0 &&
           !(text_length >= n && memcmp(text + text_length - n, ellipsis, n) == 0);
}

/* The column a group of rows `width` px across shares: the design's 220 px, or
   as wide as the widest label when that is wider, at every text size (a label
   such as "Interface size: Automatic" outgrew 220 px in the real font and was
   cut). The buttons line up, and when the column leaves a sentence too little
   room beside its button, or would cut one short of its two lines, every
   sentence goes under its button at the full width. */
static WizardColumn wizard_column(NkUi *ui, const WizardRow *rows, size_t count, float width) {
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, 0u);
    WizardColumn column = {WIZARD_VALUE_CELL, false};
    char label[48];
    float room;
    for (size_t i = 0u; i < count; ++i) {
        float need;
        (void)snprintf(label, sizeof(label), "%s: %s", rows[i].name, rows[i].value);
        need = nk_ui_button_width(ui, label, 0u);
        if (need > column.cell) column.cell = need;
    }
    /* At the design text size the sentences stay beside their buttons, as the
       design lays the page out; only larger type stacks them. */
    if (!(nk_ui_text_scale(ui) > 1.0f)) {
        return column;
    }
    room = width - column.cell - NK_UI_SPACE_4;
    column.stacked = room < WIZARD_VALUE_SENTENCE_MIN;
    for (size_t i = 0u; i < count && !column.stacked; ++i) {
        column.stacked = text_was_cut(nk_ui_text_clamp(ui, rows[i].sentence, &text, room, 2),
                                      rows[i].sentence);
    }
    return column;
}

/* A setting's button, "Name: value", with the sentence that explains it beside
   it (or under it, as `column` says; NULL lays the row out on its own); the
   button is filled while the setting is in effect. Returns true when the
   button is activated. */
static bool wizard_value_state(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                               const char *value, const char *sentence, bool on, bool disabled,
                               float width, const WizardColumn *column);

static bool wizard_value(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                         const char *value, const char *sentence, bool on, float width,
                         const WizardColumn *column) {
    return wizard_value_state(ui, id, text_id, name, value, sentence, on, false, width, column);
}

/* As wizard_value; a disabled row cannot be focused and its words are dim, sentence included. */
static bool wizard_value_state(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                               const char *value, const char *sentence, bool on, bool disabled,
                               float width, const WizardColumn *column) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_4);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, disabled ? theme->dim : theme->muted);
    WizardColumn own;
    char label[48];
    float room;
    bool pressed = false;
    if (column == NULL) {
        WizardRow self = {name, value, sentence};
        own = wizard_column(ui, &self, 1u, width);
        column = &own;
    }
    (void)snprintf(label, sizeof(label), "%s: %s", name, value);
    room = width - column->cell - NK_UI_SPACE_4;
    if (column->stacked) {
        /* The sentence sits close under its own button, and the next row's
           button further off, so each pair reads as one. */
        row = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), NK_UI_SPACE_1);
        row.padding_bottom = NK_UI_SPACE_3;
        room = width;
    }
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, text_id, &row, NULL)) return false;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(column->cell), sizing_fit(),
                                      0.0f);
        if (nk_ui_begin_container(ui, text_id + 1u, &cell, NULL)) {
            pressed = nk_ui_button(ui, id, label,
                                   NK_UI_BUTTON_GROW | (on && !disabled ? NK_UI_BUTTON_PRIMARY : 0u) |
                                       (disabled ? NK_UI_BUTTON_DISABLED : 0u));
            nk_ui_end_container(ui);
        }
    }
    nk_ui_text(ui, text_id + 2u, nk_ui_text_clamp(ui, sentence, &text, room, 2), &text);
    nk_ui_end_container(ui);
    return pressed;
}

/* One option switch: a button reading "Name: On" or "Name: Off" (accent while
   on) with its sentence beside it. Returns true when activated. */
static bool wizard_switch(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                          const char *sentence, bool on, float width,
                          const WizardColumn *column) {
    return wizard_value(ui, id, text_id, name, on ? "On" : "Off", sentence, on, width, column);
}

/* The online and developer switches, in the wizard and in Settings. */
#define WIZARD_ONLINE_SENTENCE                                                    \
    "Check nakagawa.jaycast.net for news and let you send reports. Nothing is " \
    "sent without you."
#define WIZARD_DEVELOPER_SENTENCE \
    "Show a developer log on screen to help track down problems. Stays on this PC."

static WizardColumn online_developer_column(NkUi *ui, bool online, bool developer, float width) {
    const WizardRow rows[] = {
        {"Online mode", online ? "On" : "Off", WIZARD_ONLINE_SENTENCE},
        {"Developer mode", developer ? "On" : "Off", WIZARD_DEVELOPER_SENTENCE}
    };
    return wizard_column(ui, rows, sizeof(rows) / sizeof(rows[0]), width);
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
            wizard_text(ui, 3u, "A disc image (.iso) of a PSP game you own", NK_UI_TYPE_BODY_S,
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
            WizardColumn column = online_developer_column(ui, model->online_mode,
                                                          model->developer_mode, inner - 32.0f);
            if (wizard_switch(ui, PLAYER_UI_ID_WIZARD_ONLINE, WIZ_ID_TEXT + 0x80u, "Online mode",
                              WIZARD_ONLINE_SENTENCE, model->online_mode, inner - 32.0f,
                              &column)) {
                result = wizard_op(PLAYER_UI_WIZARD_TOGGLE_ONLINE);
            }
            if (wizard_switch(ui, PLAYER_UI_ID_WIZARD_DEVELOPER, WIZ_ID_TEXT + 0x90u,
                              "Developer mode", WIZARD_DEVELOPER_SENTENCE,
                              model->developer_mode, inner - 32.0f, &column)) {
                result = wizard_op(PLAYER_UI_WIZARD_TOGGLE_DEVELOPER);
            }
            nk_ui_end_container(ui);
        }
        wizard_text(ui, 2u,
                    "News and reports aren't built yet, so online mode makes no connection. "
                    "Your choice is kept for when they arrive.",
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
        /* Encrypted program files the user can supply (#308). */
        bool guided = blocked && input_guide_present(&model->input_guide);
        const char *badge = guided ? "Needs your files" : blocked ? "Not playable yet"
            : model->recorded ? "On the list"
            : (model->experimental ? "Not tested yet"
               : (model->supported ? "Title profiled" : "Profile missing"));
        uint32_t tone = guided ? theme->warning : blocked ? theme->error
            : model->recorded ? theme->ready
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
        } else if (guided) {
            note = "This game needs files from your own disc before Nakagawa can use it.";
            note_tone = theme->warning;
        } else if (blocked) {
            note = "This game can't be played in this version of Nakagawa yet.";
            note_tone = theme->error;
        } else if (model->recorded) {
            note = "On Nakagawa's compatibility list. Add it to your library, then build it "
                   "there to play.";
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
            /* A guided step on a short window gives the name's line to the
               files; the disc ID chip above still says which disc it is. */
            if (!guided || model->logical_height >= 680.0f) {
                wizard_text(ui, 1u, safe_text(model->title_name, "PlayStation Portable title"),
                            NK_UI_TYPE_TITLE, theme->paper);
            }
            /* A guided step says it in the guide itself. */
            if (!guided) wizard_text(ui, 2u, note, NK_UI_TYPE_BODY_S, note_tone);
            if (blocked && !model->extracting && !guided) {
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
            } else if (guided) {
                /* The files take the room the check rows had; the rows come
                   back once Check again lets the disc in. */
                input_guide_box(ui, WIZ_ID_GUIDE, &model->input_guide, inner - 32.0f,
                                model->logical_height >= 800.0f ? PLAYER_UI_INPUT_FILES
                                : model->logical_height >= 680.0f ? 2u : 1u);
            } else {
                size_t rows = model->check_count < PLAYER_UI_WIZARD_CHECKS
                    ? model->check_count : PLAYER_UI_WIZARD_CHECKS;
                float status_w = check_column_width(ui, model->checks, rows, true,
                                                    NK_UI_TYPE_LABEL_S, "", 110.0f);
                float name_w = check_column_width(ui, model->checks, rows, false,
                                                  NK_UI_TYPE_DATA, "", 150.0f);
                for (size_t i = 0u; i < rows; ++i) {
                    const PlayerUiCheckRow *check = &model->checks[i];
                    NkUiLayout crow = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                                  NK_UI_SPACE_3);
                    NkUiId base = (NkUiId)(WIZ_ID_ROWS + (NkUiId)i * 8u);
                    if (nk_ui_begin_container(ui, base, &crow, NULL)) {
                        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(status_w),
                                                      sizing_fit(), 0.0f);
                        NkUiTextStyle msg = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
                            nk_ui_text_role(ui, base + 4u, nk_ui_caps(ui, safe_text(check->status, "")),
                                            NK_UI_TYPE_LABEL_S, check->rgba);
                            nk_ui_end_container(ui);
                        }
                        cell.width = sizing_fixed(name_w);
                        if (nk_ui_begin_container(ui, base + 2u, &cell, NULL)) {
                            nk_ui_text_role(ui, base + 5u, safe_text(check->name, ""),
                                            NK_UI_TYPE_DATA, theme->paper);
                            nk_ui_end_container(ui);
                        }
                        nk_ui_text(ui, base + 3u,
                                   nk_ui_text_clamp(ui, safe_text(check->message, ""), &msg,
                                                    inner - 32.0f - status_w - name_w -
                                                        2.0f * NK_UI_SPACE_3, 1), &msg);
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
        } else if (guided) {
            /* Check again reads the same disc again once the files are in
               the folder; nothing is imported again (#308). */
            WizardButton buttons[4] = {
                {PLAYER_UI_ID_WIZARD_PRIMARY, "Check again", NK_UI_BUTTON_PRIMARY,
                 PLAYER_UI_WIZARD_CHECK_AGAIN},
                {PLAYER_UI_ID_WIZARD_OPEN_FOLDER, "Open folder", 0u,
                 PLAYER_UI_WIZARD_OPEN_INPUT_FOLDER},
                {PLAYER_UI_ID_WIZARD_SECOND, "Choose a different one", 0u,
                 PLAYER_UI_WIZARD_CHOOSE_DIFFERENT},
                {PLAYER_UI_ID_WIZARD_CANCEL, "Cancel", 0u, PLAYER_UI_WIZARD_CANCEL}
            };
            if (model->input_guide.folder != NULL && model->input_guide.folder[0] != '\0') {
                wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 4u, &result);
            } else {
                buttons[1] = buttons[2];
                buttons[2] = buttons[3];
                wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 3u, &result);
            }
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
            {PLAYER_UI_ID_WIZARD_PSP_FONTS, "Get fonts from my PSP", NK_UI_BUTTON_SMALL,
             PLAYER_UI_WIZARD_PSP_FONTS},
            {PLAYER_UI_ID_WIZARD_IMPORT_FONTS, "Choose font files", NK_UI_BUTTON_SMALL,
             PLAYER_UI_WIZARD_IMPORT_FONTS},
            {PLAYER_UI_ID_WIZARD_REMOVE_FONTS, "Remove imported fonts", NK_UI_BUTTON_SMALL,
             PLAYER_UI_WIZARD_REMOVE_FONTS}
        };
        wizard_text(ui, 0u, "Fonts and display", NK_UI_TYPE_TITLE_XL, theme->paper);
        if (wizard_box_begin(ui, 0u)) {
            wizard_text(ui, 1u, model->resolution_line, NK_UI_TYPE_BODY, theme->paper);
            wizard_text(ui, 2u, model->controller_line, NK_UI_TYPE_BODY_S, theme->ready);
            wizard_text(ui, 6u, model->font_summary, NK_UI_TYPE_BODY_S,
                        model->fonts_all_own ? theme->ready : theme->warning);
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
                                  "To get your PSP's own fonts, connect it by USB (docs/SETUP.md, "
                                  "\"Copying the fonts from your PSP\") and choose Get fonts "
                                  "from my PSP."),
                        NK_UI_TYPE_CAPTION,
                        model->font_message != NULL && model->font_message[0] != '\0'
                            ? note_rgba(theme, model->font_message_kind) : theme->muted);
            wizard_buttons(ui, WIZ_ID_EXTRA, font_buttons, 3u, &result);
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

#define DISC_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_DISC, (offset))
#define DISC_ID_CHIPS DISC_ID(0x000001u)
#define DISC_ID_CHIPS_SPAN 0x8u
#define DISC_ID_BOX DISC_ID(0x000010u)
#define DISC_ID_ROWS DISC_ID(0x000100u)
#define DISC_ID_ROWS_SPAN 0x200u
#define DISC_ID_DETAILS DISC_ID(0x000020u)
#define DISC_ID_DETAILS_SPAN 0x8u
#define DISC_ID_TEXT DISC_ID(0x001000u)
#define DISC_ID_TEXT_SPAN 0x1000u
#define DISC_ID_GUIDE DISC_ID(0x000300u)

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
    size_t rows = model->check_count < PLAYER_UI_DISC_CHECKS ? model->check_count
                                                              : PLAYER_UI_DISC_CHECKS;
    float status_w = check_column_width(ui, model->checks, rows, true, NK_UI_TYPE_LABEL,
                                        "Checked", 132.0f);
    float name_w = check_column_width(ui, model->checks, rows, false, NK_UI_TYPE_BODY_S, "Check",
                                      124.0f);
    float message = inner - 32.0f - status_w - name_w - 2.0f * NK_UI_SPACE_3;
    box.padding_top = box.padding_bottom = lines > 1 ? 12.0f : 8.0f;
    box.padding_left = box.padding_right = 16.0f;
    if (lines < 2) box.gap = 2.0f;
    if (model->check_count == 0u || !nk_ui_begin_container(ui, DISC_ID_BOX, &box, &style)) return;
    for (size_t i = 0u; i < rows; ++i) {
        const PlayerUiCheckRow *check = &model->checks[i];
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_3);
        NkUiId base = (NkUiId)(DISC_ID_ROWS + (NkUiId)i * 8u);
        if (nk_ui_begin_container(ui, base, &row, NULL)) {
            NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(status_w), sizing_fit(),
                                          0.0f);
            NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
            cell.padding_top = 2.0f;
            if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
                nk_ui_text_role(ui, base + 4u, nk_ui_caps(ui, safe_text(check->status, "Checked")),
                                NK_UI_TYPE_LABEL, check->rgba);
                nk_ui_end_container(ui);
            }
            cell.width = sizing_fixed(name_w);
            cell.padding_top = 0.0f;
            if (nk_ui_begin_container(ui, base + 2u, &cell, NULL)) {
                nk_ui_text_role(ui, base + 5u, safe_text(check->name, "Check"), NK_UI_TYPE_BODY_S,
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
    bool guided;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    guided = model->kind == PLAYER_UI_DISC_BLOCKED && input_guide_present(&model->input_guide);
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
            marker = guided ? FLOW_MARK_TRIANGLE : FLOW_MARK_CROSS;
            badge = guided ? "Needs your files" : "Can't play yet";
            tone = guided ? theme->warning : theme->error;
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
                nk_ui_tag(ui, DISC_ID_CHIPS + 3u, safe_text(model->badge, "Your disc"));
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
    if (guided) {
        /* Encrypted program files the user can supply (#308): the next step,
           not a dead end. */
        lead = "This game needs files from your own disc before Nakagawa can use it.";
    }
    disc_text(ui, 1u, lead, NK_UI_TYPE_BODY, model->kind == PLAYER_UI_DISC_BLOCKED && !guided
                                                 ? theme->error : theme->muted);
    if (guided) {
        input_guide_box(ui, DISC_ID_GUIDE, &model->input_guide, inner,
                        height >= 680.0f ? PLAYER_UI_INPUT_FILES : 2u);
    } else if (model->kind == PLAYER_UI_DISC_BLOCKED && model->reason != NULL) {
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
    } else if (!guided && (model->kind != PLAYER_UI_DISC_BLOCKED || height >= 680.0f)) {
        /* A guided card gives the room to the files it needs; the other checks
           come back once Check again lets the disc in. */
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
                    /* Adding it would only fail later, at build time (#1280).
                       Files the user can supply get Check again, which reads
                       the same disc again, and the folder they go in (#308). */
                    if (guided) {
                        if (nk_ui_button(ui, PLAYER_UI_ID_DISC_PRIMARY, "Check again",
                                         NK_UI_BUTTON_PRIMARY)) {
                            result = disc_op(PLAYER_UI_DISC_CHECK_AGAIN);
                        }
                        if (model->input_guide.folder != NULL &&
                            model->input_guide.folder[0] != '\0' &&
                            nk_ui_button(ui, PLAYER_UI_ID_DISC_OPEN_FOLDER, "Open folder", 0u)) {
                            result = disc_op(PLAYER_UI_DISC_OPEN_FOLDER);
                        }
                    } else if (nk_ui_button(ui, PLAYER_UI_ID_DISC_PRIMARY, "Choose another disc",
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

#define SET_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_SETTINGS, (offset))
#define SET_ID_ROOT SET_ID(0x000001u)
#define SET_ID_TOP SET_ID(0x000010u)
#define SET_ID_BODY SET_ID(0x000020u)
#define SET_ID_COLUMN SET_ID(0x000021u)
#define SET_ID_HEAD SET_ID(0x000022u)
#define SET_ID_CARDS SET_ID(0x000023u)
#define SET_ID_GAME SET_ID(0x000024u)
#define SET_ID_LAUNCHER SET_ID(0x000025u)
#define SET_ID_ACTIONS SET_ID(0x000026u)
#define SET_ID_PROMPTS SET_ID(0x000040u)
#define SET_ID_WAVES SET_ID(0x000050u)
#define SET_ID_ROWS SET_ID(0x000100u)
#define SET_ID_ROWS_SPAN 0x100u
#define SET_ID_TEXT SET_ID(0x001000u)
#define SET_ID_TEXT_SPAN 0x1000u

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
    {
        /* The names take what the widest value button leaves, at least its
           design 190 px; at a larger text size the names wrap instead. */
        const char *values[] = {
            safe_text(model->resolution, "2x"), safe_text(model->pacing, "PSP timing"),
            safe_text(model->scaling, "Sharp"), "Size, contrast, motion",
            "Controller and keyboard", "Check for updates", "Off", "Filtering and more"
        };
        float buttons = 190.0f;
        for (size_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i) {
            float need = nk_ui_button_width(ui, values[i], NK_UI_BUTTON_SMALL);
            if (need > buttons) buttons = need;
        }
        name_width = card - 40.0f - NK_UI_SPACE_3 - buttons;
    }
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
            if (settings_row(ui, 3u, PLAYER_UI_ID_SETTINGS_FULLSCREEN, "Game fullscreen",
                             settings_on_off(model->fullscreen), model->fullscreen,
                             name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_FULLSCREEN);
            }
            if (settings_row(ui, 4u, PLAYER_UI_ID_SETTINGS_SCALING, "Scaling",
                             safe_text(model->scaling, "Sharp"), false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_SCALING);
            }
            /* Ace Mode (src/rt/ace.h): the library's switch, and its page. */
            if (settings_row(ui, 11u, PLAYER_UI_ID_SETTINGS_ACE, "Ace Mode",
                             settings_on_off(model->ace_enabled), model->ace_enabled, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_ACE);
            }
            if (settings_row(ui, 12u, PLAYER_UI_ID_SETTINGS_ACE_OPEN, "Ace Mode options",
                             "Filtering and more", false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_OPEN_ACE);
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
                             "Launcher fullscreen (F11)", settings_on_off(model->launcher_fullscreen),
                             model->launcher_fullscreen, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_LAUNCHER_FULLSCREEN);
            }
            if (settings_row(ui, 7u, PLAYER_UI_ID_SETTINGS_ACCESSIBILITY, "Accessibility",
                             "Size, contrast, motion", false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_OPEN_ACCESSIBILITY);
            }
            if (settings_row(ui, 8u, PLAYER_UI_ID_SETTINGS_UI_SOUNDS, "Menu sounds",
                             settings_on_off(model->ui_sounds), model->ui_sounds, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_UI_SOUNDS);
            }
            if (settings_row(ui, 9u, PLAYER_UI_ID_SETTINGS_CONTROLLER, "Controls",
                             "Controller and keyboard", false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_OPEN_CONTROLLER);
            }
            if (settings_row(ui, 10u, PLAYER_UI_ID_SETTINGS_UPDATES, "Updates",
                             "Check for updates", false, name_width)) {
                *result = settings_op(PLAYER_UI_SETTINGS_CHECK_UPDATES);
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
    {
        /* One row at the design text size; a larger one wraps it. */
        static const struct {
            NkUiId id;
            const char *label;
            uint32_t flags;
            PlayerUiSettingsOp op;
        } actions[] = {
            {PLAYER_UI_ID_SETTINGS_DONE, "Done", NK_UI_BUTTON_PRIMARY, PLAYER_UI_SETTINGS_DONE},
            {PLAYER_UI_ID_SETTINGS_ONLINE, "Online and developer", 0u,
             PLAYER_UI_SETTINGS_OPEN_ONLINE},
            {PLAYER_UI_ID_SETTINGS_NOTICES, "Third-party notices", 0u,
             PLAYER_UI_SETTINGS_OPEN_NOTICES},
            {PLAYER_UI_ID_SETTINGS_BUILD_CACHE, "Build tools and cache", 0u,
             PLAYER_UI_SETTINGS_OPEN_BUILD_CACHE},
            {PLAYER_UI_ID_SETTINGS_FONTS, "PSP fonts", 0u, PLAYER_UI_SETTINGS_OPEN_FONTS},
            {PLAYER_UI_ID_SETTINGS_FACTORY_RESET, "Factory reset", NK_UI_BUTTON_DANGER,
             PLAYER_UI_SETTINGS_OPEN_FACTORY_RESET}
        };
        NkUiFlowRow flow;
        if (nk_ui_flow_begin(ui, &flow, SET_ID_ACTIONS, &layout, column)) {
            for (size_t i = 0u; i < sizeof(actions) / sizeof(actions[0]); ++i) {
                nk_ui_flow_item(ui, &flow,
                                nk_ui_button_width(ui, actions[i].label, actions[i].flags));
                /* Left and Right stay on this row: the cards' last rows end just above it. */
                if (nk_ui_button(ui, actions[i].id, actions[i].label,
                                 actions[i].flags | NK_UI_BUTTON_ROW)) {
                    *result = settings_op(actions[i].op);
                }
            }
            nk_ui_flow_end(ui, &flow);
        }
    }
    nk_ui_end_container(ui);
}

/* Text only wraps at spaces, so a folder path is one word that would run out of
   the card. Each word longer than max_word bytes keeps its start and its end
   (the folder's own name) around "...". */
static void settings_shorten_words(const char *in, char *out, size_t cap, size_t max_word) {
    size_t used = 0u;
    if (cap == 0u) return;
    while (*in != '\0' && used + 1u < cap) {
        size_t word = strcspn(in, " ");
        if (word > max_word) {
            size_t head = 12u;
            size_t tail = max_word - head - 3u;
            while (tail > 0u && ((unsigned char)in[word - tail] & 0xC0u) == 0x80u) --tail;
            (void)snprintf(out + used, cap - used, "%.*s...%.*s", (int)head, in, (int)tail,
                           in + word - tail);
        } else {
            (void)snprintf(out + used, cap - used, "%.*s", (int)word, in);
        }
        used += strlen(out + used);
        in += word;
        while (*in == ' ' && used + 1u < cap) out[used++] = *in++;
        out[used] = '\0';
    }
    out[used < cap ? used : cap - 1u] = '\0';
}

/* Settings > Updates: one card that says where the check is, with the next step
   first. Nothing here reaches the network on its own: every button is a click. */
static void settings_updates(NkUi *ui, const PlayerUiSettingsModel *model, float inner,
                             PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    PlayerUiUpdateStep step = model->update_step;
    const char *version = safe_text(model->update_version, "");
    bool busy = step == PLAYER_UI_UPDATE_CHECKING || step == PLAYER_UI_UPDATE_DOWNLOADING ||
                step == PLAYER_UI_UPDATE_INSTALLING;
    char lead[192];
    char line[640];
    char a[32];
    char b[32];
    uint32_t lead_rgba = theme->muted;
    flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, "Updates", NK_UI_TYPE_TITLE, theme->paper);
    switch (step) {
        case PLAYER_UI_UPDATE_CHECKING:
            (void)snprintf(lead, sizeof(lead), "Asking GitHub for the newest version...");
            break;
        case PLAYER_UI_UPDATE_CURRENT:
            (void)snprintf(lead, sizeof(lead), "You have the newest version.");
            lead_rgba = theme->ready;
            break;
        case PLAYER_UI_UPDATE_AVAILABLE:
            (void)snprintf(lead, sizeof(lead), "Version %s is out.", version);
            lead_rgba = theme->ready;
            break;
        case PLAYER_UI_UPDATE_DOWNLOADING:
            (void)snprintf(lead, sizeof(lead), "Downloading version %s...", version);
            break;
        case PLAYER_UI_UPDATE_DOWNLOADED:
            (void)snprintf(lead, sizeof(lead), "Version %s is downloaded and checked.", version);
            lead_rgba = theme->ready;
            break;
        case PLAYER_UI_UPDATE_INSTALLING:
            (void)snprintf(lead, sizeof(lead), "Unpacking version %s...", version);
            break;
        case PLAYER_UI_UPDATE_INSTALLED:
            (void)snprintf(lead, sizeof(lead), "Version %s is ready to start.", version);
            lead_rgba = theme->ready;
            break;
        case PLAYER_UI_UPDATE_FAILED:
            (void)snprintf(lead, sizeof(lead), "%s",
                           safe_text(model->update_error_title, "Something went wrong"));
            lead_rgba = theme->warning;
            break;
        case PLAYER_UI_UPDATE_IDLE:
        default:
            (void)snprintf(lead, sizeof(lead),
                           "Nakagawa can ask GitHub whether a newer version is out. The request "
                           "carries nothing about you, this PC or your games.");
            break;
    }
    nk_ui_text_role(ui, FLOW_ID_LEAD, lead, NK_UI_TYPE_BODY_S, lead_rgba);
    if (wizard_box_begin(ui, 0u)) {
        NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
        if (version[0] != '\0') {
            const char *published = safe_text(model->update_published, "");
            char about[48] = "";
            if (model->update_prerelease && published[0] != '\0') {
                (void)snprintf(about, sizeof(about), " (pre-release, %s)", published);
            } else if (model->update_prerelease) {
                (void)snprintf(about, sizeof(about), " (pre-release)");
            } else if (published[0] != '\0') {
                (void)snprintf(about, sizeof(about), " (%s)", published);
            }
            (void)snprintf(line, sizeof(line), "This version: %s    Newest: %s%s",
                           safe_text(model->current_version, "unknown"), version, about);
        } else {
            (void)snprintf(line, sizeof(line), "This version: %s",
                           safe_text(model->current_version, "unknown"));
        }
        nk_ui_text_role(ui, SET_ID_TEXT + 0xD0u, line, NK_UI_TYPE_DATA, theme->paper);
        switch (step) {
            case PLAYER_UI_UPDATE_AVAILABLE:
                if (model->update_notes && model->update_notes[0]) {
                    nk_ui_text(ui, SET_ID_TEXT + 0xD1u,
                               nk_ui_text_clamp(ui, model->update_notes, &text, inner - 32.0f, 4),
                               &text);
                }
                if (model->update_page_url && model->update_page_url[0]) {
                    nk_ui_text_role(ui, SET_ID_TEXT + 0xD5u, model->update_page_url,
                                    NK_UI_TYPE_CAPTION, theme->dim);
                }
                if (!model->update_has_zip) {
                    (void)snprintf(line, sizeof(line),
                                   "This release has no Windows download. Its page on GitHub "
                                   "lists what it offers.");
                } else if (model->update_unverifiable) {
                    (void)snprintf(line, sizeof(line),
                                   "This build can't check update signatures, so it doesn't "
                                   "download updates itself. Download the new version from the "
                                   "release page on GitHub.");
                } else if (model->update_unsigned) {
                    (void)snprintf(line, sizeof(line),
                                   "This release isn't signed, so Nakagawa won't download it. "
                                   "Its page on GitHub has the download.");
                } else if (!model->update_can_download) {
                    (void)snprintf(line, sizeof(line),
                                   "Download it from the release page on GitHub.");
                } else {
                    flow_bytes(a, sizeof(a), model->update_zip_bytes);
                    (void)snprintf(line, sizeof(line),
                                   "Download fetches %s (%s) and checks it against the "
                                   "release's signed SHA-256. Nothing is installed until you "
                                   "choose Install.",
                                   safe_text(model->update_zip_name, "the Windows ZIP"), a);
                }
                nk_ui_text(ui, SET_ID_TEXT + 0xD2u, line, &text);
                break;
            case PLAYER_UI_UPDATE_DOWNLOADING:
                nk_ui_progress_bar(ui, SET_ID_TEXT + 0xD3u, inner - 32.0f,
                                   model->update_zip_bytes > 0ull
                                       ? (float)((double)model->update_received_bytes /
                                                 (double)model->update_zip_bytes)
                                       : -1.0f,
                                   0.0);
                flow_bytes(a, sizeof(a), model->update_received_bytes);
                flow_bytes(b, sizeof(b), model->update_zip_bytes);
                (void)snprintf(line, sizeof(line), "%s of %s", a, b);
                nk_ui_text_role(ui, SET_ID_TEXT + 0xD4u, line, NK_UI_TYPE_DATA, theme->muted);
                break;
            case PLAYER_UI_UPDATE_DOWNLOADED: {
                char folder[96];
                settings_shorten_words(safe_text(model->update_folder, "the downloads folder"),
                                       folder, sizeof(folder), 72u);
                (void)snprintf(line, sizeof(line),
                               "Its SHA-256 matches the release's signed checksum. Saved in "
                               "%s. Install unpacks it into a new folder of its own; this "
                               "version stays as it is.", folder);
                nk_ui_text(ui, SET_ID_TEXT + 0xD2u,
                           nk_ui_text_clamp(ui, line, &text, inner - 32.0f, 4), &text);
                break;
            }
            case PLAYER_UI_UPDATE_INSTALLED: {
                char folder[96];
                settings_shorten_words(safe_text(model->update_folder, "a new folder"), folder,
                                       sizeof(folder), 72u);
                (void)snprintf(line, sizeof(line),
                               "It is in %s. Close Nakagawa, then start bin\\nakagawa_player.exe "
                               "from that folder. Your library, saves and settings stay where "
                               "they are and carry over.", folder);
                nk_ui_text(ui, SET_ID_TEXT + 0xD2u,
                           nk_ui_text_clamp(ui, line, &text, inner - 32.0f, 5), &text);
                break;
            }
            case PLAYER_UI_UPDATE_FAILED:
                text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
                settings_shorten_words(safe_text(model->update_error_message, ""), line,
                                       sizeof(line), 72u);
                nk_ui_text(ui, SET_ID_TEXT + 0xD2u,
                           nk_ui_text_clamp(ui, line, &text, inner - 32.0f, 5), &text);
                break;
            default:
                break;
        }
        if (!busy &&
            wizard_switch(ui, PLAYER_UI_ID_UPDATE_AT_START, SET_ID_TEXT + 0xE0u,
                          "Check at start",
                          "Asks GitHub once each time Nakagawa starts. Off unless you turn it "
                          "on.",
                          model->check_updates_at_start, inner - 32.0f, NULL)) {
            *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_UPDATE_AT_START);
        }
        nk_ui_end_container(ui);
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            switch (step) {
                case PLAYER_UI_UPDATE_AVAILABLE:
                    if (model->update_can_download &&
                        nk_ui_button(ui, PLAYER_UI_ID_UPDATE_DOWNLOAD, "Download",
                                     NK_UI_BUTTON_PRIMARY)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_DOWNLOAD_UPDATE);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_UPDATE_PAGE, "Open release page",
                                     model->update_can_download ? 0u : NK_UI_BUTTON_PRIMARY)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_OPEN_RELEASE_PAGE);
                    }
                    break;
                case PLAYER_UI_UPDATE_DOWNLOADING:
                    if (nk_ui_button(ui, PLAYER_UI_ID_UPDATE_CANCEL, "Cancel", 0u)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_CANCEL_UPDATE);
                    }
                    break;
                case PLAYER_UI_UPDATE_DOWNLOADED:
                    if (nk_ui_button(ui, PLAYER_UI_ID_UPDATE_INSTALL, "Install",
                                     NK_UI_BUTTON_PRIMARY)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_INSTALL_UPDATE);
                    }
                    if (nk_ui_button(ui, PLAYER_UI_ID_UPDATE_FOLDER, "Open folder", 0u)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_OPEN_UPDATE_FOLDER);
                    }
                    break;
                case PLAYER_UI_UPDATE_INSTALLED:
                    if (nk_ui_button(ui, PLAYER_UI_ID_UPDATE_FOLDER, "Open folder",
                                     NK_UI_BUTTON_PRIMARY)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_OPEN_UPDATE_FOLDER);
                    }
                    break;
                case PLAYER_UI_UPDATE_IDLE:
                case PLAYER_UI_UPDATE_CURRENT:
                case PLAYER_UI_UPDATE_FAILED:
                    if (nk_ui_button(ui, PLAYER_UI_ID_UPDATE_CHECK,
                                     step == PLAYER_UI_UPDATE_IDLE ? "Check for updates"
                                                                   : "Check again",
                                     NK_UI_BUTTON_PRIMARY)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_CHECK_UPDATES);
                    }
                    /* A refused download still has its release page, where the
                       user can download it by hand. */
                    if (step == PLAYER_UI_UPDATE_FAILED &&
                        model->update_failed_step == PLAYER_UI_UPDATE_DOWNLOADING &&
                        model->update_page_url && model->update_page_url[0] &&
                        nk_ui_button(ui, PLAYER_UI_ID_UPDATE_PAGE, "Open release page", 0u)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_OPEN_RELEASE_PAGE);
                    }
                    /* A failed install leaves the checked ZIP to unpack by hand. */
                    if (step == PLAYER_UI_UPDATE_FAILED &&
                        model->update_failed_step == PLAYER_UI_UPDATE_INSTALLING &&
                        model->update_folder && model->update_folder[0] &&
                        nk_ui_button(ui, PLAYER_UI_ID_UPDATE_FOLDER, "Open folder", 0u)) {
                        *result = settings_op(PLAYER_UI_SETTINGS_OPEN_UPDATE_FOLDER);
                    }
                    break;
                default:
                    break;
            }
            if (step != PLAYER_UI_UPDATE_DOWNLOADING &&
                nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back",
                             busy ? NK_UI_BUTTON_PRIMARY : 0u)) {
                *result = settings_op(PLAYER_UI_SETTINGS_BACK);
            }
            nk_ui_end_container(ui);
        }
    }
}

/* Settings > Accessibility: the interface size, the text size, the
   high-contrast focus ring and Reduce motion. Each button steps through its
   values; Automatic follows the system, and the sentence says what the system
   asks for now. */
static void settings_accessibility(NkUi *ui, const PlayerUiSettingsModel *model, float inner,
                                   PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const char *system = safe_text(model->system_name, "your system");
    char interface_line[256];
    char contrast_line[256];
    char motion_line[256];
    flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, "Accessibility", NK_UI_TYPE_TITLE, theme->paper);
    nk_ui_text_role(ui, FLOW_ID_LEAD,
                    "These change the launcher, not a running game. Automatic follows the "
                    "matching setting of your system, and changes when it does.",
                    NK_UI_TYPE_BODY_S, theme->muted);
    (void)snprintf(interface_line, sizeof(interface_line),
                   "Grows text and controls in a large enough window. Display scale in "
                   "%s: %d%%.",
                   system, model->system_scale_pct);
    (void)snprintf(contrast_line, sizeof(contrast_line),
                   "A white ring between black lines marks the selection. High contrast "
                   "in %s: %s.",
                   system, model->system_high_contrast ? "on" : "off");
    (void)snprintf(motion_line, sizeof(motion_line),
                   "Holds the background still; pulses and sweeps become cuts. "
                   "Animations in %s: %s.",
                   system, model->system_reduced_motion ? "off" : "on");
    if (wizard_box_begin(ui, 0u)) {
        const WizardRow rows[] = {
            {"Interface", safe_text(model->interface_size, "Automatic"), interface_line},
            {"Text size", safe_text(model->text_size, "Standard"),
             "Makes the launcher's text larger. Controls keep their size."},
            {"Contrast", safe_text(model->high_contrast_choice, "Automatic"), contrast_line},
            {"Motion", safe_text(model->reduce_motion_choice, "Automatic"), motion_line}
        };
        /* The four buttons line up on one column at every text size. */
        WizardColumn column = wizard_column(ui, rows, sizeof(rows) / sizeof(rows[0]),
                                            inner - 32.0f);
        if (wizard_value(ui, PLAYER_UI_ID_SETTINGS_INTERFACE_SIZE, SET_ID_TEXT + 0x300u,
                         rows[0].name, rows[0].value, rows[0].sentence, false, inner - 32.0f,
                         &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_INTERFACE_SIZE);
        }
        if (wizard_value(ui, PLAYER_UI_ID_SETTINGS_TEXT_SIZE, SET_ID_TEXT + 0x30Cu,
                         rows[1].name, rows[1].value, rows[1].sentence, false, inner - 32.0f,
                         &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_TEXT_SIZE);
        }
        if (wizard_value(ui, PLAYER_UI_ID_SETTINGS_HIGH_CONTRAST, SET_ID_TEXT + 0x304u,
                         rows[2].name, rows[2].value, rows[2].sentence, model->high_contrast,
                         inner - 32.0f, &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_HIGH_CONTRAST);
        }
        if (wizard_value(ui, PLAYER_UI_ID_SETTINGS_REDUCE_MOTION, SET_ID_TEXT + 0x308u,
                         rows[3].name, rows[3].value, rows[3].sentence, model->reduce_motion,
                         inner - 32.0f, &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_REDUCE_MOTION);
        }
        nk_ui_end_container(ui);
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back", NK_UI_BUTTON_PRIMARY)) {
                *result = settings_op(PLAYER_UI_SETTINGS_BACK);
            }
            nk_ui_end_container(ui);
        }
    }
}

/* Settings > Ace Mode (src/rt/ace.h): the library's switch and each feature, a sentence beside
   each that says what it does and what it costs. While the switch is off the feature rows are
   dim and cannot be focused; their values are kept. Features this build does not carry
   (ultrawide, smooth motion) have no row. */
static void settings_ace(NkUi *ui, const PlayerUiSettingsModel *model, float inner,
                         PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const bool off = !model->ace_enabled;
    flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, "Ace Mode", NK_UI_TYPE_TITLE, theme->paper);
    nk_ui_text_role(ui, FLOW_ID_LEAD,
                    "Modern touches for every game. The games themselves run as released. "
                    "Each game's page can turn it on or off for that game alone.",
                    NK_UI_TYPE_BODY_S, theme->muted);
    if (wizard_box_begin(ui, 0u)) {
        const WizardRow rows[] = {
            {"Ace Mode", settings_on_off(model->ace_enabled),
             "Turns all of these on or off together."},
            {"Texture filtering", safe_text(model->ace_filtering, "16x"),
             "Keeps slanted surfaces clear and steady. Save pictures a game takes keep the "
             "filtering."},
            {"Crisp system text", settings_on_off(model->ace_text),
             "Draws the on-screen keyboard at your screen's resolution."},
            {"Speed readout", settings_on_off(model->ace_readout),
             "Shows the game's speed and frame rate in a corner, over the game's own HUD."},
            {"Game badge", settings_on_off(model->ace_notice),
             "Says Ace Mode is on for a few seconds when a game starts."}
        };
        WizardColumn column = wizard_column(ui, rows, sizeof(rows) / sizeof(rows[0]), inner - 32.0f);
        if (wizard_value(ui, PLAYER_UI_ID_SETTINGS_ACE_MODE, SET_ID_TEXT + 0x400u, rows[0].name,
                         rows[0].value, rows[0].sentence, model->ace_enabled, inner - 32.0f,
                         &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_ACE);
        }
        if (wizard_value_state(ui, PLAYER_UI_ID_SETTINGS_ACE_FILTERING, SET_ID_TEXT + 0x404u,
                               rows[1].name, rows[1].value, rows[1].sentence,
                               strcmp(rows[1].value, "Off") != 0, off, inner - 32.0f, &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_ACE_FILTERING);
        }
        if (wizard_value_state(ui, PLAYER_UI_ID_SETTINGS_ACE_TEXT, SET_ID_TEXT + 0x408u,
                               rows[2].name, rows[2].value, rows[2].sentence, model->ace_text, off,
                               inner - 32.0f, &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_ACE_TEXT);
        }
        if (wizard_value_state(ui, PLAYER_UI_ID_SETTINGS_ACE_READOUT, SET_ID_TEXT + 0x40Cu,
                               rows[3].name, rows[3].value, rows[3].sentence, model->ace_readout,
                               off, inner - 32.0f, &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_ACE_READOUT);
        }
        if (wizard_value_state(ui, PLAYER_UI_ID_SETTINGS_ACE_NOTICE, SET_ID_TEXT + 0x410u,
                               rows[4].name, rows[4].value, rows[4].sentence, model->ace_notice,
                               off, inner - 32.0f, &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_ACE_NOTICE);
        }
        nk_ui_end_container(ui);
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
        row.padding_top = NK_UI_SPACE_1;
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Done", NK_UI_BUTTON_PRIMARY)) {
                *result = settings_op(PLAYER_UI_SETTINGS_BACK);
            }
            nk_ui_end_container(ui);
        }
    }
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
                        "News and reports aren't built yet, so online mode makes no "
                        "connection. Your choice is kept for when they arrive.",
                        NK_UI_TYPE_BODY_S, theme->warning);
        if (wizard_box_begin(ui, 0u)) {
            WizardColumn column = online_developer_column(ui, model->online_mode,
                                                          model->developer_mode, inner - 32.0f);
            if (wizard_switch(ui, PLAYER_UI_ID_SETTINGS_ONLINE_MODE, SET_ID_TEXT + 0x80u,
                              "Online mode", WIZARD_ONLINE_SENTENCE, model->online_mode,
                              inner - 32.0f, &column)) {
                *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_ONLINE);
            }
            if (model->online_mode) {
                NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                             NK_UI_SPACE_3);
                char label[48];
                if (nk_ui_begin_container(ui, SET_ID_TEXT + 0x88u, &row, NULL)) {
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
                              "Developer mode", WIZARD_DEVELOPER_SENTENCE,
                              model->developer_mode, inner - 32.0f, &column)) {
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
    case PLAYER_UI_SETTINGS_PAGE_ACCESSIBILITY:
        settings_accessibility(ui, model, inner, result);
        break;
    case PLAYER_UI_SETTINGS_PAGE_ACE:
        settings_ace(ui, model, inner, result);
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
                              model->share_system_libraries, inner - 32.0f, NULL)) {
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
    case PLAYER_UI_SETTINGS_PAGE_UPDATES:
        settings_updates(ui, model, inner, result);
        break;
    case PLAYER_UI_SETTINGS_PAGE_FONTS:
        /* Walkthrough blocker: the wizard's Fonts step is not on the Add to
           library path, so the import lives here, with each slot's status. */
        flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "PSP fonts", NK_UI_TYPE_TITLE, theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "Optional. Games look right with your PSP's own fonts: connect your PSP "
                        "by USB and choose Get fonts from my PSP, or copy the .pgf files from "
                        "flash0:/font on your PSP to a folder on this PC (docs/SETUP.md, "
                        "\"Copying the fonts from your PSP\") and choose them here. The import "
                        "reads ltn0.pgf to ltn15.pgf, jpn0.pgf and kr0.pgf from that folder; "
                        "until then similar fonts stand in.",
                        NK_UI_TYPE_BODY_S, theme->muted);
        if (model->font_summary != NULL && model->font_summary[0] != '\0') {
            nk_ui_text_role(ui, SET_ID_TEXT + 0xDDu, model->font_summary, NK_UI_TYPE_BODY_S,
                            model->fonts_all_own ? theme->ready : theme->warning);
        }
        if (wizard_box_begin(ui, 0u)) {
            NkUiLayout cols = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                          NK_UI_SPACE_4);
            if (nk_ui_begin_container(ui, SET_ID_TEXT + 0xD0u, &cols, NULL)) {
                float column = (inner - 32.0f - 2.0f * NK_UI_SPACE_4) / 3.0f;
                for (int c = 0; c < 3; ++c) {
                    NkUiLayout col = layout_make(NK_UI_LAYOUT_COLUMN, sizing_fixed(column),
                                                 sizing_fit(), 2.0f);
                    if (!nk_ui_begin_container(ui, (NkUiId)(SET_ID_TEXT + 0xD1u + (NkUiId)c),
                                               &col, NULL)) {
                        continue;
                    }
                    for (size_t r = 0u; r < 6u; ++r) {
                        size_t item = (size_t)c * 6u + r;
                        NkUiTextStyle font = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
                        if (item >= model->font_line_count) break;
                        nk_ui_text(ui, (NkUiId)(SET_ID_TEXT + 0xE0u + (NkUiId)item),
                                   nk_ui_text_clamp(ui, safe_text(model->font_lines[item], ""),
                                                    &font, column, 1), &font);
                    }
                    nk_ui_end_container(ui);
                }
                nk_ui_end_container(ui);
            }
            if (model->font_message != NULL && model->font_message[0] != '\0') {
                NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_CAPTION,
                                                note_rgba(theme, model->font_message_kind));
                nk_ui_text(ui, SET_ID_TEXT + 0xDCu,
                           nk_ui_text_clamp(ui, model->font_message, &text, inner - 32.0f, 2),
                           &text);
            }
            nk_ui_end_container(ui);
        }
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_PSP_FONTS, "Get fonts from my PSP",
                                 NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_PSP_FONTS);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_IMPORT_FONTS, "Choose font files",
                                 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_IMPORT_FONTS);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_REMOVE_FONTS, "Remove imported fonts",
                                 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_REMOVE_FONTS);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Back", 0u)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_BACK);
                }
                nk_ui_end_container(ui);
            }
        }
        break;
    case PLAYER_UI_SETTINGS_PAGE_FACTORY_RESET:
        /* Owner, 2026-10-29: back to a first run, with the user data moved
           aside (player_app_factory_reset), never deleted. */
        flow_eyebrow(ui, FLOW_MARK_TRIANGLE, "Factory reset", theme->warning);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Reset Nakagawa Recomp to a first run?",
                        NK_UI_TYPE_TITLE, theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "Your library, saves, imported PSP fonts, keys, settings, built games "
                        "and downloaded build tools move into backup folders. Nakagawa Recomp "
                        "then starts again with the setup it shows the first time. Nothing is "
                        "deleted, and your disc images and logs stay where they are.",
                        NK_UI_TYPE_BODY, theme->muted);
        if (wizard_box_begin(ui, 0u)) {
            NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, theme->paper);
            nk_ui_text_role(ui, SET_ID_TEXT + 0x200u,
                            model->reset_folder_count > 0u
                                ? "These folders get \"-backup-\" and today's date and time "
                                  "added to their names:"
                                : "Nakagawa Recomp has no user data on this PC yet.",
                            NK_UI_TYPE_BODY_S, theme->muted);
            for (size_t i = 0u; i < model->reset_folder_count && i < PLAYER_UI_RESET_FOLDERS;
                 ++i) {
                /* The folders differ at their ends (config, data, cache,
                   saves), so a long one keeps its end and loses its middle. */
                char folder[600];
                settings_shorten_words(safe_text(model->reset_folders[i], ""), folder,
                                       sizeof(folder), 72u);
                nk_ui_text(ui, (NkUiId)(SET_ID_TEXT + 0x201u + (NkUiId)i),
                           nk_ui_text_clamp(ui, folder, &data, inner - 32.0f, 1), &data);
            }
            if (model->reset_folder_count > 0u) {
                nk_ui_text_role(ui, SET_ID_TEXT + 0x20Fu,
                                "To undo it, close Nakagawa Recomp and give each folder its "
                                "old name back.",
                                NK_UI_TYPE_CAPTION, theme->muted);
            }
            nk_ui_end_container(ui);
        }
        {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            row.padding_top = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_BACK, "Keep everything",
                                 NK_UI_BUTTON_PRIMARY)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_BACK);
                }
                if (model->reset_folder_count > 0u &&
                    nk_ui_button(ui, PLAYER_UI_ID_SETTINGS_CONFIRM_RESET, "Reset",
                                 NK_UI_BUTTON_DANGER)) {
                    *result = settings_op(PLAYER_UI_SETTINGS_FACTORY_RESET);
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
    layout.scroll = page_scrolls(ui);
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
   bindings the rows show; for the keyboard the stick card holds the stick's four
   keys and names the keys Nakagawa keeps, for the controller it chooses the stick
   (#1299). Every change is saved as it is made. While a guided calibration runs,
   its step replaces the cards. Under a top bar with SETTINGS active, as the
   Settings page. */

#define PAD_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_CONTROLS, (offset))
#define PAD_ID_ROOT PAD_ID(0x000001u)
#define PAD_ID_TOP PAD_ID(0x000010u)
#define PAD_ID_BODY PAD_ID(0x000020u)
#define PAD_ID_COLUMN PAD_ID(0x000021u)
#define PAD_ID_HEAD PAD_ID(0x000022u)
#define PAD_ID_CARDS PAD_ID(0x000023u)
#define PAD_ID_BUTTONS PAD_ID(0x000024u)
#define PAD_ID_STICK PAD_ID(0x000025u)
#define PAD_ID_ACTIONS PAD_ID(0x000026u)
#define PAD_ID_GRID PAD_ID(0x000027u)
#define PAD_ID_GRID_SPAN 0x4u
#define PAD_ID_LIVE PAD_ID(0x000030u)
#define PAD_ID_SPACER PAD_ID(0x000031u)
#define PAD_ID_DEVICE PAD_ID(0x000032u)
#define PAD_ID_DEVICE_BUTTONS PAD_ID(0x000033u)
#define PAD_ID_PROMPTS PAD_ID(0x000040u)
#define PAD_ID_WAVES PAD_ID(0x000050u)
#define PAD_ID_ROWS PAD_ID(0x000100u)         /* + control * 4, + 0-2: a binding row's parts */
#define PAD_ID_ROWS_SPAN 0x80u
#define PAD_ID_GLYPH_CELLS PAD_ID(0x000200u)  /* + control: the cell a binding's glyph sits in */
#define PAD_ID_GLYPH_CELLS_SPAN 0x20u
#define PAD_ID_STEPPERS PAD_ID(0x000280u)     /* + k * 4, + 0-2: a stick card row's parts */
#define PAD_ID_STEPPERS_SPAN 0x10u
#define PAD_ID_TEXT PAD_ID(0x001000u)
#define PAD_ID_TEXT_SPAN 0x1000u

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

/* A binding's glyph, centred in a cell as wide as the widest one (CREATE, six letters
   on a pill, is 43.6 px at 20 px tall), so the buttons after it line up. */
#define PAD_GLYPH_HEIGHT 20.0f
#define PAD_GLYPH_CELL 44.0f
static void controller_glyph(NkUi *ui, const PlayerUiControllerModel *model, size_t control) {
    const PlayerUiBindingGlyph *glyph = &model->glyphs[control];
    NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(PAD_GLYPH_CELL), sizing_fit(),
                                  0.0f);
    cell.align_x = NK_UI_ALIGN_CENTER;
    cell.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, (NkUiId)(PAD_ID_GLYPH_CELLS + (NkUiId)control), &cell,
                               NULL)) {
        return;
    }
    if (glyph->shown) {
        (void)nk_ui_prompt_glyph_in(ui, PLAYER_UI_ID_CONTROLLER_GLYPH(control), model->glyph_style,
                                    glyph->action, glyph->label, PAD_GLYPH_HEIGHT);
    }
    nk_ui_end_container(ui);
}

/* One PSP control: its name, the glyph of what it is bound to, then its binding in
   words on the button that rebinds it. The control the status note names (it gave
   its input to the last binding) has its name in the note's colour. */
static void controller_row(NkUi *ui, const PlayerUiControllerModel *model, size_t control,
                           const char *name, float name_width, float button_width,
                           PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_2);
    NkUiId base = (NkUiId)(PAD_ID_ROWS + (NkUiId)control * 4u);
    bool capturing = model->capturing == (int)control;
    bool noted = model->noted == (int)control;
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S,
                                    model->conflicted[control] || (noted && model->status_error)
                                        ? theme->error
                                        : noted ? theme->accent_dark : theme->paper);
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, base, &row, NULL)) return;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(name_width), sizing_fit(),
                                      0.0f);
        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
            nk_ui_text(ui, base + 2u,
                       nk_ui_text_clamp(ui, safe_text(name, "Button"), &text, name_width, 1),
                       &text);
            nk_ui_end_container(ui);
        }
    }
    controller_glyph(ui, model, control);
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
    NkUiId base = (NkUiId)(PAD_ID_STEPPERS + k * 4u);
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

/* Which controller stick drives the PSP stick, named on the button that switches it:
   124 px, or as wide as its longer word at a larger text size. */
#define PAD_STICK_BUTTON_WIDTH 124.0f
static void controller_stick_choice(NkUi *ui, const PlayerUiControllerModel *model, float width,
                                    PlayerUiAction *result) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), NK_UI_SPACE_2);
    NkUiId base = (NkUiId)(PAD_ID_STEPPERS + 2u * 4u);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->paper);
    float button = PAD_STICK_BUTTON_WIDTH;
    {
        /* controller_button_label keeps 2 px spare beside the padding. */
        float need = nk_ui_button_width(ui, "Right stick", NK_UI_BUTTON_SMALL) + 2.0f;
        if (need > button) button = need;
    }
    row.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, base, &row, NULL)) return;
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(), 0.0f);
        if (nk_ui_begin_container(ui, base + 1u, &cell, NULL)) {
            nk_ui_text(ui, base + 2u,
                       nk_ui_text_clamp(ui, "PSP stick", &text, width - button - NK_UI_SPACE_2,
                                        1),
                       &text);
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiLayout cell = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(button), sizing_fit(),
                                      0.0f);
        if (nk_ui_begin_container(ui, base + 3u, &cell, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_CONTROLLER_STICK,
                             controller_button_label(ui, model->stick_right ? "Right stick"
                                                                            : "Left stick",
                                                     button),
                             NK_UI_BUTTON_SMALL | NK_UI_BUTTON_GROW)) {
                *result = controller_op(PLAYER_UI_CONTROLLER_STICK, 0u);
            }
            nk_ui_end_container(ui);
        }
    }
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

/* CONTROLLER and KEYBOARD: whose bindings the rows show, the one shown filled. The
   pair is 240 px, or as wide as a larger text size makes its two words. */
#define PAD_DEVICE_WIDTH 240.0f
static float controller_device_width(NkUi *ui) {
    /* controller_button_label keeps 2 px spare beside the padding. */
    float each = nk_ui_button_width(ui, "Controller", NK_UI_BUTTON_SMALL) + 2.0f;
    float pair = 2.0f * each + NK_UI_SPACE_2;
    return pair > PAD_DEVICE_WIDTH ? pair : PAD_DEVICE_WIDTH;
}

static void controller_device(NkUi *ui, const PlayerUiControllerModel *model,
                              PlayerUiAction *result) {
    float width = controller_device_width(ui);
    NkUiLayout pair = layout_make(NK_UI_LAYOUT_ROW, sizing_fixed(width), sizing_fit(),
                                  NK_UI_SPACE_2);
    float each = (width - NK_UI_SPACE_2) * 0.5f;
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

/* For the keyboard, the stick card holds the stick's four keys, each rebound like a
   button's (#1299), then says which keys stay with Nakagawa. */
#define PAD_STICK_NAME_WIDTH 48.0f
static void controller_keyboard_card(NkUi *ui, const PlayerUiControllerModel *model, float card,
                                     PlayerUiAction *result) {
    static const char *const kept[] = {"Esc or F1 pauses the game.", "F3 shows the frame rate."};
    static const char *const directions[PLAYER_UI_CONTROLLER_STICK_KEYS] = {"Up", "Down", "Left",
                                                                            "Right"};
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
    float inner = card - 2.0f * NK_UI_SPACE_5;
    if (!nk_ui_card_begin(ui, PAD_ID_STICK, sizing_fixed(card), sizing_grow(), NK_UI_SPACE_2)) {
        return;
    }
    nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x21u, "Analog stick");
    for (size_t i = 0u; i < PLAYER_UI_CONTROLLER_STICK_KEYS; ++i) {
        controller_row(ui, model, PLAYER_UI_CONTROLLER_STICK_KEY + i, directions[i],
                       PAD_STICK_NAME_WIDTH,
                       inner - PAD_STICK_NAME_WIDTH - PAD_GLYPH_CELL - 2.0f * NK_UI_SPACE_2,
                       result);
    }
    nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x20u, "Keys Nakagawa keeps");
    for (size_t i = 0u; i < sizeof(kept) / sizeof(kept[0]); ++i) {
        nk_ui_text(ui, (NkUiId)(PAD_ID_TEXT + 0x50u + (NkUiId)i),
                   nk_ui_text_clamp(ui, kept[i], &text, inner, 2), &text);
    }
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
    /* The longest name, "L Trigger (L1)", needs about 100 px; the rest of the row is
       the binding's glyph and its words. */
    name_width = half * 0.36f;
    if (name_width > 104.0f) name_width = 104.0f;
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
                                            &text,
                                            column - controller_device_width(ui) - NK_UI_SPACE_3,
                                            1),
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
                        size_t control = col * (PLAYER_UI_CONTROLLER_BUTTONS / 2u) + row;
                        controller_row(ui, model, control, model->names[control], name_width,
                                       half - name_width - PAD_GLYPH_CELL - 2.0f * NK_UI_SPACE_2,
                                       result);
                    }
                    nk_ui_end_container(ui);
                }
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }
        if (model->keyboard) {
            controller_keyboard_card(ui, model, stick_card, result);
        } else if (nk_ui_card_begin(ui, PAD_ID_STICK, sizing_fixed(stick_card), sizing_grow(),
                                    NK_UI_SPACE_2)) {
            float inner = stick_card - 2.0f * NK_UI_SPACE_5;
            nk_ui_eyebrow(ui, PAD_ID_TEXT + 0x20u, "Stick and triggers");
            controller_stick_choice(ui, model, inner, result);
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
   The whole log in the data face on a dark inset that scrolls, opening on the
   newest lines, with Copy, Save and Close under it. Only the lines the inset
   shows are laid out; spacers stand for the rest. player_state.c has already
   written the user's folders in the lines as %USERPROFILE%. */

#define DEV_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_DEVELOPER_LOG, (offset))
#define DEV_ID_ROOT DEV_ID(0x000001u)
#define DEV_ID_TOP DEV_ID(0x000010u)
#define DEV_ID_BODY DEV_ID(0x000020u)
#define DEV_ID_CARD DEV_ID(0x000021u)
#define DEV_ID_BOX DEV_ID(0x000022u)
#define DEV_ID_ACTIONS DEV_ID(0x000023u)
#define DEV_ID_PROMPTS DEV_ID(0x000040u)
#define DEV_ID_WAVES DEV_ID(0x000050u)
#define DEV_ID_TEXT DEV_ID(0x000100u)
#define DEV_ID_TEXT_SPAN 0x100u
#define DEV_ID_LINES DEV_ID(0x001000u)
#define DEV_ID_LINES_SPAN 0x1000u

/* A log line at the design text size; dev_line_height grows it with the text. */
#define DEV_LINE_HEIGHT 16.0f

/* The log's line height at the current text size. */
static float dev_line_height(const NkUi *ui) {
    NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, 0u);
    data.line_height = DEV_LINE_HEIGHT;
    return nk_ui_line_height(ui, &data);
}

/* The card around the log box: its padding, the eyebrow, the sentence, the
   buttons, the gaps between them and the box's own padding (169 px at the
   design text size). */
static float dev_card_chrome(const NkUi *ui) {
    NkUiTextStyle eyebrow = nk_ui_type(NK_UI_TYPE_EYEBROW, 0u);
    NkUiTextStyle sentence = nk_ui_type(NK_UI_TYPE_BODY_S, 0u);
    NkUiTextStyle label = nk_ui_type(NK_UI_TYPE_LABEL, 0u);
    return 34.0f + nk_ui_line_height(ui, &eyebrow) + nk_ui_line_height(ui, &sentence) +
           nk_ui_line_box(ui, NK_UI_CONTROL_HEIGHT, &label) + 3.0f * NK_UI_SPACE_3 + 20.0f;
}

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
    float line_h;
    float end;
    float offset;
    NkUiScrollState scroll;
    NkUiNavDir page;
    size_t count;
    size_t first;
    size_t last;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    width = model->logical_width > 80.0f ? model->logical_width : 1280.0f;
    height = model->logical_height > 80.0f ? model->logical_height : 800.0f;
    column = width - 2.0f * (width < NK_UI_NARROW_WIDTH ? NK_UI_GUTTER_NARROW : NK_UI_GUTTER);
    inner = column - 2.0f * NK_UI_SPACE_5;
    line_h = dev_line_height(ui);
    lines_height = height - NK_UI_TOP_BAR_HEIGHT - NK_UI_PROMPT_BAR_HEIGHT -
                   2.0f * NK_UI_SPACE_4 - dev_card_chrome(ui);
    if (lines_height < 4.0f * line_h) lines_height = 4.0f * line_h;
    count = model->line_count < PLAYER_UI_DEVELOPER_LOG_LINES ? model->line_count
                                                               : PLAYER_UI_DEVELOPER_LOG_LINES;
    end = (float)count * line_h > lines_height
        ? (float)count * line_h - lines_height : 0.0f;
    /* The log opens on its newest lines and follows new ones while it shows the
       end; scrolled up, it stays where it was put. Page Up and Page Down (the
       pad's triggers) move it a box less one line, so a line carries over. */
    offset = end;
    if (nk_ui_scroll_state(ui, DEV_ID_BOX, &scroll) && scroll.offset < scroll.max_offset) {
        offset = scroll.offset;
    }
    while (nk_ui_take_event(ui, NK_UI_EV_PAGE, &page)) {
        offset += page == NK_UI_DIR_RIGHT ? lines_height - line_h : line_h - lines_height;
    }
    if (offset > end) offset = end;
    if (offset < 0.0f) offset = 0.0f;
    nk_ui_scroll_to(ui, DEV_ID_BOX, offset);
    /* Where Clay will draw the log this frame, which holds new lines back a
       frame while their room is laid out. */
    if (nk_ui_scroll_state(ui, DEV_ID_BOX, &scroll)) offset = scroll.offset;
    first = (size_t)(offset / line_h);
    last = (size_t)ceilf((offset + lines_height) / line_h);
    if (first > count) first = count;
    if (last > count) last = count;
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
                box.scroll = true;
                if (nk_ui_begin_container(ui, DEV_ID_BOX, &box, &style)) {
                    NkUiTextStyle data = nk_ui_type(NK_UI_TYPE_DATA, theme->muted);
                    data.line_height = DEV_LINE_HEIGHT;
                    nk_ui_spacer(ui, DEV_ID_TEXT + 4u, sizing_fixed(1.0f),
                                 sizing_fixed((float)first * line_h));
                    for (size_t i = first; i < last; ++i) {
                        nk_ui_text(ui, (NkUiId)(DEV_ID_LINES + (NkUiId)i),
                                   nk_ui_text_clamp(ui, safe_text(model->lines[i], " "), &data,
                                                    inner - 28.0f, 1), &data);
                    }
                    nk_ui_spacer(ui, DEV_ID_TEXT + 5u, sizing_fixed(1.0f),
                                 sizing_fixed((float)(count - last) * line_h));
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
                if (nk_ui_button(ui, PLAYER_UI_ID_DEVELOPER_LOG_INSPECT,
                                 model->inspecting ? "Layout inspector: On"
                                                   : "Layout inspector: Off", 0u)) {
                    result = developer_log_op(PLAYER_UI_DEVELOPER_LOG_INSPECT);
                }
                if (nk_ui_button(ui, PLAYER_UI_ID_DEVELOPER_LOG_CLOSE, "Close",
                                 NK_UI_BUTTON_PRIMARY)) {
                    result = developer_log_op(PLAYER_UI_DEVELOPER_LOG_CLOSE);
                }
                if (model->notice != NULL && model->notice[0] != '\0') {
                    NkUiTextStyle note = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
                    /* The four buttons take about 480 px of the row, more at a
                       larger text size. */
                    float buttons = nk_ui_button_width(ui, "Copy", 0u) +
                                    nk_ui_button_width(ui, "Save", 0u) +
                                    nk_ui_button_width(ui, "Layout inspector: Off", 0u) +
                                    nk_ui_button_width(ui, "Close", NK_UI_BUTTON_PRIMARY) +
                                    4.0f * NK_UI_SPACE_3;
                    float room = inner - (buttons > 500.0f ? buttons : 500.0f);
                    nk_ui_text(ui, DEV_ID_TEXT + 3u,
                               nk_ui_text_clamp(ui, model->notice, &note, room, 1), &note);
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
