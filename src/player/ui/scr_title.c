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
    if (bytes >= 1024ull * 1024ull) {
        (void)snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        (void)snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
    } else {
        (void)snprintf(out, cap, "%llu bytes", bytes);
    }
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
                    "only the build tools you approve. It does not decrypt games: it uses the "
                    "decrypted files from your own copy.",
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
                wizard_text(ui, 5u,
                            "Choose a disc image below, or drop an .iso file onto this window.",
                            NK_UI_TYPE_BODY_S, theme->muted);
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
            badge = "Copying files";
            tone = theme->warning;
        } else if (model->extraction_complete) {
            badge = "Files staged";
        } else if (model->extraction_failed) {
            badge = "Copy failed";
            tone = theme->error;
        }
        if (model->extracting) {
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
        wizard_text(ui, 0u, model->extracting ? "Copying the game's files" : "Check the disc",
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
            if (model->extracting) {
                (void)snprintf(line, sizeof(line), "Copied %d of %d files" FLOW_MIDDOT "%d%%",
                               model->files_extracted, model->total_files,
                               model->extraction_percent);
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
            static const WizardButton buttons[] = {
                {PLAYER_UI_ID_WIZARD_CANCEL, "Cancel copying", NK_UI_BUTTON_DANGER,
                 PLAYER_UI_WIZARD_CANCEL}
            };
            wizard_buttons(ui, WIZ_ID_BUTTONS, buttons, 1u, &result);
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
