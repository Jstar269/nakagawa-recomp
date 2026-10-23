/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Title detail: the hero art and package status on the left; the name, status
   chips, actions and compatibility card on the right; the prompt bar below.
   Layout follows the design system (docs/PLAYER_UI_DESIGN.md). */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"

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
#define TITLE_ID_ACTIONS_2 0x2000002Bu
#define TITLE_ID_COMPAT 0x20000005u
#define TITLE_ID_COMPAT_HEAD 0x2000002Cu
#define TITLE_ID_COMPAT_EYEBROW 0x20000006u
#define TITLE_ID_COMPAT_WORD 0x20000007u
#define TITLE_ID_COMPAT_DETAIL 0x20000008u
#define TITLE_ID_COMPAT_SOURCE 0x2000002Du
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

/* Presentation colour for a compatibility word. The word itself comes from the
   host's data; this only picks how it is drawn. */
static uint32_t compatibility_rgba(const NkUiTheme *theme, const char *word) {
    if (word == NULL) return theme->muted;
    if (strcmp(word, "Works") == 0) return theme->ready;
    if (strcmp(word, "Partially works") == 0 || strcmp(word, "Experimental") == 0) {
        return theme->info;
    }
    if (strcmp(word, "Boots to a named boundary") == 0) return theme->warning;
    if (strcmp(word, "Not supported") == 0) return theme->error;
    return theme->muted;
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
        static const NkUiSection sections[] = {{"Library", true}};
        nk_ui_top_bar(ui, TITLE_ID_TOP, sections, 1u,
                      nk_ui_input_label(nk_ui_prompt_style(ui)),
                      nk_ui_prompt_style(ui) != NK_UI_PROMPT_KEYCAPS);
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
            nk_ui_hero(ui, TITLE_ID_HERO, (NkUiArtKind)title->art_kind, title->disc_id, aside,
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
                nk_ui_text_role(ui, TITLE_ID_STATUS_DETAIL,
                                safe_text(title->library_status_detail,
                                          title->primary_action == PLAYER_UI_PRIMARY_PLAY
                                              ? "Built for this PC and ready to play."
                                              : "The package for this game is not ready yet."),
                                NK_UI_TYPE_BODY_S, theme->muted);
                if (title->input_note != NULL && title->input_note[0] != '\0') {
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
            row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
            row.align_y = NK_UI_ALIGN_CENTER;
            row.padding_top = NK_UI_SPACE_2;
            row.padding_left = NK_UI_FOCUS_RING_GAP + NK_UI_FOCUS_RING_WIDTH;
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
                nk_ui_end_container(ui);
            }
            row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(), NK_UI_SPACE_3);
            row.padding_left = NK_UI_FOCUS_RING_GAP + NK_UI_FOCUS_RING_WIDTH;
            row.padding_bottom = NK_UI_SPACE_1;
            if (nk_ui_begin_container(ui, TITLE_ID_ACTIONS_2, &row, NULL)) {
                if (nk_ui_button(ui, PLAYER_UI_ID_TITLE_BACK, "Back to library", 0u)) {
                    result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                                    PLAYER_UI_PRIMARY_NONE);
                }
                nk_ui_end_container(ui);
            }
            if (nk_ui_card_begin(ui, TITLE_ID_COMPAT, sizing_grow(), sizing_fit(),
                                 NK_UI_SPACE_3)) {
                NkUiLayout head = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_3);
                const char *word = safe_text(title->compatibility_word, "Untested");
                head.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, TITLE_ID_COMPAT_HEAD, &head, NULL)) {
                    nk_ui_eyebrow(ui, TITLE_ID_COMPAT_EYEBROW, "Compatibility");
                    nk_ui_status_pill(ui, TITLE_ID_COMPAT_WORD, NULL, word,
                                      compatibility_rgba(theme, word));
                    nk_ui_end_container(ui);
                }
                if (title->compatibility_detail != NULL &&
                    title->compatibility_detail[0] != '\0') {
                    nk_ui_text_role(ui, TITLE_ID_COMPAT_DETAIL, title->compatibility_detail,
                                    NK_UI_TYPE_BODY_S, theme->paper);
                }
                nk_ui_text_role(ui, TITLE_ID_COMPAT_SOURCE,
                                "Words come from docs/COMPATIBILITY.md for this build.",
                                NK_UI_TYPE_FOOTNOTE, theme->dim);
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
        nk_ui_prompt_bar(ui, TITLE_ID_PROMPTS,
                         "Your disc stays on this PC. Nothing is copied out or uploaded.",
                         prompts, sizeof(prompts) / sizeof(prompts[0]));
    }
    nk_ui_end_container(ui);
    return result;
}
