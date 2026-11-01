/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Setup wizard: the first-run path in five steps (Welcome, Select game, Verify, Fonts and
   system, Ready) on the setup wizard state in player_state.c. Every control returns
   PLAYER_UI_ACTION_WIZARD with the op main.c runs, the same handlers the existing
   renderer's wizard buttons call. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>
#include <string.h>

#define WIZ_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_WIZARD, (offset))
#define WIZ_ID_STEPS WIZ_ID(0x000001u)
#define WIZ_ID_STEPS_SPAN 0x8u
#define WIZ_ID_STEP_LABELS WIZ_ID(0x000101u)
#define WIZ_ID_STEP_LABELS_SPAN 0x8u
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
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
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
        if (scr_wizard_box_begin(ui, 0u)) {
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
        if (scr_wizard_box_begin(ui, 0u)) {
            WizardColumn column = scr_online_developer_column(ui, model->online_mode,
                                                          model->developer_mode, inner - 32.0f);
            if (scr_wizard_switch(ui, PLAYER_UI_ID_WIZARD_ONLINE, WIZ_ID_TEXT + 0x80u, "Online mode",
                              WIZARD_ONLINE_SENTENCE, model->online_mode, inner - 32.0f,
                              &column)) {
                result = wizard_op(PLAYER_UI_WIZARD_TOGGLE_ONLINE);
            }
            if (scr_wizard_switch(ui, PLAYER_UI_ID_WIZARD_DEVELOPER, WIZ_ID_TEXT + 0x90u,
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
        if (scr_wizard_box_begin(ui, model->has_iso ? theme->ready : 0u)) {
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
        bool guided = blocked && scr_input_guide_present(&model->input_guide);
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
        if (scr_wizard_box_begin(ui, tone)) {
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
                scr_input_guide_box(ui, WIZ_ID_GUIDE, &model->input_guide, inner - 32.0f,
                                model->logical_height >= 800.0f ? PLAYER_UI_INPUT_FILES
                                : model->logical_height >= 680.0f ? 2u : 1u);
            } else {
                size_t rows = model->check_count < PLAYER_UI_WIZARD_CHECKS
                    ? model->check_count : PLAYER_UI_WIZARD_CHECKS;
                float status_w = scr_check_column_width(ui, model->checks, rows, true,
                                                    NK_UI_TYPE_LABEL_S, "", 110.0f);
                float name_w = scr_check_column_width(ui, model->checks, rows, false,
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
        if (scr_wizard_box_begin(ui, 0u)) {
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
        if (scr_wizard_box_begin(ui, model->runtime_ready ? theme->ready : theme->warning)) {
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
    scr_flow_end(ui, "", prompts, 3u);
    return result;
}
