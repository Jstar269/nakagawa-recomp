/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The one-card frame the build, remove, stop, rebuild, prerequisite and error
   pages share (the launcher background, the top bar, the dialog card and the
   prompt bar), and the box that lists the files a disc needs. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>
#include <string.h>

/* Files a disc needs (#308): why the files are needed, one row per file (still needed, or
   a copy to replace; the file to add; what it is a copy of), a count and the folder. The
   title page, the game-found card and the wizard's Verify step share it; each keeps its
   own buttons (Check again, Open folder). The guide shows only when built-in decryption
   (#295) stopped; its words never ask for a key and never say how to decrypt. */

#define INPUT_GUIDE_LEAD \
    "Nakagawa couldn't decrypt some of this game's program files from your disc. It can " \
    "use decrypted copies you make from your own disc: put these files in the folder " \
    "below, then choose Check again."

bool scr_input_guide_present(const PlayerUiInputGuide *guide) {
    return guide != NULL && guide->file_count > 0u;
}

/* max_rows bounds the listed files on a short window; the count line says how
   many are not shown. A short window (max_rows of 2 or fewer) also gets the
   compact form: no eyebrow, two lines of the reason, one of the folder, and the
   Replace marks without their sentence. */
void scr_input_guide_box(NkUi *ui, NkUiId base, const PlayerUiInputGuide *guide,
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
    if (!scr_input_guide_present(guide)) return;
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

/* Opens the shared frame and the dialog card; returns the card's inner width,
   or 0 when nothing was opened (then nothing must be closed). */
float scr_flow_begin(NkUi *ui, float width, float height, float card_width) {
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

void scr_flow_end(NkUi *ui, const char *hint, const NkUiPromptItem *prompts, size_t count) {
    nk_ui_end_container(ui); /* card */
    nk_ui_end_container(ui); /* body */
    nk_ui_prompt_bar(ui, FLOW_ID_PROMPTS, hint, prompts, count);
    nk_ui_end_container(ui); /* root */
}

void scr_flow_eyebrow(NkUi *ui, const char *marker, const char *text, uint32_t rgba) {
    char line[160];
    (void)snprintf(line, sizeof(line), "%s %s", marker, text);
    nk_ui_text_role(ui, FLOW_ID_EYEBROW, nk_ui_caps(ui, line), NK_UI_TYPE_EYEBROW, rgba);
}

NkUiStyle scr_flow_box_style(const NkUiTheme *theme, uint32_t fill) {
    NkUiStyle style;
    memset(&style, 0, sizeof(style));
    style.fill_rgba = fill;
    style.border_rgba = theme->line;
    style.border_width = NK_UI_BORDER_HAIRLINE;
    style.corner_radius = 10.0f;
    return style;
}

void scr_flow_bytes(char *out, size_t cap, unsigned long long bytes) {
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
