/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* About: the page a viewer can read in one look and explain in one sentence: what the
   program is, which build this is, its licence and the notices, the credits, and that the
   project is independent. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>

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
