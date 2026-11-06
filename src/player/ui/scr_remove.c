/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Remove from library (#1304): the game, what Nakagawa built for it with
   sizes, the promise about the disc image, and two boxes that stay off until
   the user ticks them: the saves, and the decrypted files and disc copy
   (#2003). Cancel holds focus first.

   Saves (#1883), the title page's other dialog, is here too:
   player_ui_screen_saves. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>

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
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                       model->logical_height > 80.0f ? model->logical_height : 800.0f, 640.0f);
    if (inner <= 0.0f) return result;
    scr_flow_eyebrow(ui, FLOW_MARK_TRIANGLE, "Remove from library", theme->warning);
    (void)snprintf(text, sizeof(text), "Remove %s?", safe_text(model->title, "this game"));
    nk_ui_text_role(ui, FLOW_ID_TITLE, nk_ui_frame_text(ui, text), NK_UI_TYPE_TITLE,
                    theme->paper);
    shown = model->items != NULL ? model->item_count : 0u;
    if (shown > PLAYER_UI_REMOVE_ITEM_CAP) shown = PLAYER_UI_REMOVE_ITEM_CAP;
    nk_ui_text_role(ui, FLOW_ID_LEAD,
                    model->files_shared
                        ? "It leaves your library. What Nakagawa built for it and its saves "
                          "stay: another image of this disc in your library uses them."
                    : shown > 0u
                        ? "It leaves your library, and Nakagawa deletes what it built for it:"
                        : "It leaves your library. Nakagawa has not built anything for it yet.",
                    NK_UI_TYPE_BODY_S, theme->muted);
    if (shown > 0u) {
        NkUiLayout rows = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 4.0f);
        NkUiStyle style = scr_flow_box_style(theme, theme->surface);
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
                    scr_flow_bytes(size, sizeof(size), model->items[i].bytes);
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ITEMS + 0x30u + (NkUiId)i),
                                    nk_ui_frame_text(ui, size), NK_UI_TYPE_DATA, theme->muted);
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
        scr_flow_bytes(size, sizeof(size), total);
        (void)snprintf(text, sizeof(text), "Frees %s", size);
        nk_ui_text_role(ui, FLOW_ID_TOTAL, nk_ui_caps(ui, text), NK_UI_TYPE_LABEL_S,
                        theme->warning);
    }
    nk_ui_text_role(ui, FLOW_ID_DETAILS,
                    FLOW_MARK_CHECK "The disc image on your PC is not touched.",
                    NK_UI_TYPE_BODY_S, theme->ready);
    if (model->has_saves) {
        scr_flow_bytes(size, sizeof(size), model->saves_bytes);
        (void)snprintf(text, sizeof(text), "%sAlso delete this game's saves (%s)",
                       model->delete_saves ? FLOW_MARK_CHECK : "", size);
        if (nk_ui_button(ui, PLAYER_UI_ID_REMOVE_SAVES, nk_ui_frame_text(ui, text), 0u)) {
            result = action(PLAYER_UI_ACTION_REMOVE_TOGGLE_SAVES, model->title_index,
                            PLAYER_UI_PRIMARY_NONE);
        }
        nk_ui_text_role(ui, FLOW_ID_MESSAGE,
                        model->delete_saves
                            ? "Your saves for this game will be deleted too. A backup of "
                              "them is kept first: Saves can restore it if you add the "
                              "game again."
                            : "Your saves stay.",
                        NK_UI_TYPE_BODY_S, model->delete_saves ? theme->error : theme->muted);
    } else if (!model->files_shared) {
        nk_ui_text_role(ui, FLOW_ID_MESSAGE, "This game has no saves.", NK_UI_TYPE_BODY_S,
                        theme->muted);
    }
    if (model->has_disc_copy && !model->files_shared) {
        scr_flow_bytes(size, sizeof(size), model->disc_copy_bytes);
        (void)snprintf(text, sizeof(text),
                       "%sAlso delete the decrypted files and disc copy (%s)",
                       model->delete_disc_copy ? FLOW_MARK_CHECK : "", size);
        if (nk_ui_button(ui, PLAYER_UI_ID_REMOVE_DISC_COPY, nk_ui_frame_text(ui, text), 0u)) {
            result = action(PLAYER_UI_ACTION_REMOVE_TOGGLE_DISC_COPY, model->title_index,
                            PLAYER_UI_PRIMARY_NONE);
        }
        nk_ui_text_role(ui, FLOW_ID_NOTICE,
                        model->delete_disc_copy
                            ? "They go too, with any decrypted files you added yourself. "
                              "Your disc image stays."
                            : "They stay, so adding this game again is quicker.",
                        NK_UI_TYPE_BODY_S,
                        model->delete_disc_copy ? theme->error : theme->muted);
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
                unsigned also =
                    (model->delete_saves ? PLAYER_UI_REMOVE_ALSO_SAVES : 0u) |
                    (model->delete_disc_copy && model->has_disc_copy && !model->files_shared
                         ? PLAYER_UI_REMOVE_ALSO_DISC_COPY
                         : 0u);
                result = action(PLAYER_UI_ACTION_REMOVE_CONFIRM, model->title_index,
                                (PlayerUiPrimaryAction)also);
            }
            nk_ui_end_container(ui);
        }
    }
    scr_flow_end(ui, "", prompts, 3u);
    return result;
}

/* Saves (#1883): what the title's saves are, Back up saves, Add a save and Open
   save folder, the backups a page at a time with a Restore on each, and the
   question Restore and a replacing Add ask first, which holds the card until it
   is answered (Cancel holds its focus). A game that runs keeps every change off. */
PlayerUiAction player_ui_screen_saves(NkUi *ui, const PlayerUiSavesModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"}, {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    const NkUiTheme *theme;
    PlayerUiAction result = action(PLAYER_UI_ACTION_NONE, 0u, PLAYER_UI_PRIMARY_NONE);
    float inner;
    char text[320];
    char size[32];
    size_t shown;
    bool blocked;
    if (ui == NULL || model == NULL) return result;
    theme = nk_ui_theme(ui);
    blocked = model->blocked != NULL && model->blocked[0] != '\0';
    shown = model->rows != NULL ? model->row_count : 0u;
    if (shown > PLAYER_UI_SAVES_ROW_CAP) shown = PLAYER_UI_SAVES_ROW_CAP;
    inner = scr_flow_begin(ui, model->logical_width > 80.0f ? model->logical_width : 1280.0f,
                           model->logical_height > 80.0f ? model->logical_height : 800.0f,
                           640.0f);
    if (inner <= 0.0f) return result;
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Saves", theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, safe_text(model->title, "This game"), NK_UI_TYPE_TITLE,
                    theme->paper);
    if (model->has_saves) {
        scr_flow_bytes(size, sizeof(size), model->saves_bytes);
        (void)snprintf(text, sizeof(text),
                       "Its saves and game data take %s. Backups are copies kept on this PC.",
                       size);
    } else {
        (void)snprintf(text, sizeof(text), "%s",
                       "No saves yet. Add a save copies one in from a folder you pick.");
    }
    nk_ui_text_role(ui, FLOW_ID_LEAD, nk_ui_frame_text(ui, text), NK_UI_TYPE_BODY_S,
                    theme->muted);
    if (blocked) {
        nk_ui_text_role(ui, FLOW_ID_NOTICE, model->blocked, NK_UI_TYPE_BODY_S, theme->warning);
    }
    if (model->ask != PLAYER_UI_SAVES_ASK_NONE && !blocked) {
        bool restore = model->ask == PLAYER_UI_SAVES_ASK_RESTORE;
        const char *when = restore && model->ask_row < shown
            ? safe_text(model->rows[model->ask_row].when, "this backup") : "this backup";
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        if (restore) {
            (void)snprintf(text, sizeof(text), "Restore the backup from %s?", when);
        } else {
            (void)snprintf(text, sizeof(text), "A save named %s is already here. Replace it?",
                           safe_text(model->ask_name, "like this one"));
        }
        nk_ui_text_role(ui, FLOW_ID_MESSAGE, nk_ui_frame_text(ui, text), NK_UI_TYPE_BODY,
                        theme->paper);
        nk_ui_text_role(ui, FLOW_ID_DETAILS,
                        restore ? "Your saves now are backed up first and kept, so you can "
                                  "go back to them."
                                : "Your saves now are backed up first, so you can go back "
                                  "to them.",
                        NK_UI_TYPE_BODY_S, theme->muted);
        if (nk_ui_begin_container(ui, FLOW_ID_ACTIONS, &row, NULL)) {
            if (nk_ui_button(ui, PLAYER_UI_ID_SAVES_CANCEL, "Cancel", 0u)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_CANCEL);
            }
            if (nk_ui_button(ui, PLAYER_UI_ID_SAVES_CONFIRM, restore ? "Restore" : "Replace",
                             restore ? NK_UI_BUTTON_PRIMARY : NK_UI_BUTTON_DANGER)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_CONFIRM);
            }
            nk_ui_end_container(ui);
        }
        scr_flow_end(ui, "", prompts, 3u);
        return result;
    }
    {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        NkUiFlowRow flow;
        uint32_t change = blocked ? NK_UI_BUTTON_DISABLED : 0u;
        uint32_t back_up = model->has_saves && !blocked ? NK_UI_BUTTON_PRIMARY
                                                        : NK_UI_BUTTON_DISABLED;
        if (nk_ui_flow_begin(ui, &flow, FLOW_ID_ACTIONS, &row, inner)) {
            nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, "Back up saves", back_up));
            if (nk_ui_button(ui, PLAYER_UI_ID_SAVES_BACK_UP, "Back up saves", back_up)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_BACK_UP);
            }
            nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, "Add a save", change));
            if (nk_ui_button(ui, PLAYER_UI_ID_SAVES_ADD, "Add a save", change)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_ADD);
            }
            nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, "Open save folder", 0u));
            if (nk_ui_button(ui, PLAYER_UI_ID_SAVES_OPEN_FOLDER, "Open save folder", 0u)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_OPEN_FOLDER);
            }
            nk_ui_flow_item(ui, &flow, nk_ui_button_width(ui, "Back", 0u));
            if (nk_ui_button(ui, PLAYER_UI_ID_SAVES_BACK, "Back", 0u)) {
                result = action(PLAYER_UI_ACTION_BACK, model->title_index,
                                PLAYER_UI_PRIMARY_NONE);
            }
            nk_ui_flow_end(ui, &flow);
        }
    }
    if (model->status != NULL && model->status[0] != '\0') {
        nk_ui_text_role(ui, FLOW_ID_STATUS, model->status, NK_UI_TYPE_BODY_S,
                        note_rgba(theme, model->status_kind));
    }
    if (model->total == 0u) {
        nk_ui_text_role(ui, FLOW_ID_TOTAL, nk_ui_caps(ui, "No backups yet"),
                        NK_UI_TYPE_LABEL_S, theme->muted);
    } else {
        if (shown > 0u) {
            (void)snprintf(text, sizeof(text), "Backups %lu-%lu of %lu, newest first",
                           (unsigned long)(model->first_row + 1u),
                           (unsigned long)(model->first_row + shown),
                           (unsigned long)model->total);
        } else {
            (void)snprintf(text, sizeof(text), "Backups: %lu", (unsigned long)model->total);
        }
        nk_ui_text_role(ui, FLOW_ID_TOTAL, nk_ui_caps(ui, text), NK_UI_TYPE_LABEL_S,
                        theme->muted);
    }
    if (shown > 0u) {
        NkUiLayout rows = layout_make(NK_UI_LAYOUT_COLUMN, sizing_grow(), sizing_fit(), 4.0f);
        NkUiStyle style = scr_flow_box_style(theme, theme->surface);
        rows.padding_top = rows.padding_bottom = 8.0f;
        rows.padding_left = rows.padding_right = 14.0f;
        if (nk_ui_begin_container(ui, FLOW_ID_ROWS, &rows, &style)) {
            for (size_t i = 0u; i < shown; ++i) {
                NkUiLayout line = layout_make(NK_UI_LAYOUT_ROW, sizing_grow(), sizing_fit(),
                                              NK_UI_SPACE_3);
                line.align_y = NK_UI_ALIGN_CENTER;
                if (nk_ui_begin_container(ui, (NkUiId)(FLOW_ID_ITEMS + (NkUiId)i), &line,
                                          NULL)) {
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ITEMS + 0x10u + (NkUiId)i),
                                    safe_text(model->rows[i].when, "Backup"),
                                    NK_UI_TYPE_BODY_S, theme->paper);
                    nk_ui_text_role(ui, (NkUiId)(FLOW_ID_ITEMS + 0x20u + (NkUiId)i),
                                    safe_text(model->rows[i].detail, ""), NK_UI_TYPE_DATA,
                                    theme->muted);
                    nk_ui_spacer(ui, (NkUiId)(FLOW_ID_ITEMS + 0x30u + (NkUiId)i),
                                 sizing_grow(), sizing_fixed(1.0f));
                    if (nk_ui_button(ui, (NkUiId)(PLAYER_UI_ID_SAVES_RESTORE_BASE + (NkUiId)i),
                                     "Restore", NK_UI_BUTTON_SMALL | NK_UI_BUTTON_NOT_DEFAULT |
                                                    (blocked ? NK_UI_BUTTON_DISABLED : 0u))) {
                        result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                        (PlayerUiPrimaryAction)(PLAYER_UI_SAVES_RESTORE_BASE +
                                                                (int)i));
                    }
                    nk_ui_end_container(ui);
                }
            }
            nk_ui_end_container(ui);
        }
    }
    if (model->first_row > 0u || model->first_row + shown < model->total) {
        NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                     NK_UI_SPACE_3);
        if (nk_ui_begin_container(ui, FLOW_ID_REPORT_ROW, &row, NULL)) {
            if (model->first_row > 0u &&
                nk_ui_button(ui, PLAYER_UI_ID_SAVES_NEWER, "Newer",
                             NK_UI_BUTTON_SMALL | NK_UI_BUTTON_NOT_DEFAULT)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_NEWER);
            }
            if (model->first_row + shown < model->total &&
                nk_ui_button(ui, PLAYER_UI_ID_SAVES_OLDER, "Older",
                             NK_UI_BUTTON_SMALL | NK_UI_BUTTON_NOT_DEFAULT)) {
                result = action(PLAYER_UI_ACTION_SAVES, model->title_index,
                                (PlayerUiPrimaryAction)PLAYER_UI_SAVES_OLDER);
            }
            nk_ui_end_container(ui);
        }
    }
    scr_flow_end(ui, "", prompts, 3u);
    return result;
}
