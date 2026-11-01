/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Developer log: the whole log in the data face on a dark inset that scrolls, opening on
   the newest lines, with Copy, Save and Close under it. Only the lines the inset shows
   are laid out; spacers stand for the rest. player_state.c has already written the user's
   folders in the lines as %USERPROFILE%. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <math.h>

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
                NkUiStyle style = scr_flow_box_style(theme, 0x080B0FFFu);
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
