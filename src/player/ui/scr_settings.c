/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Settings: one page of named rows, each with its value on the button that changes it, in
   two cards: what a game uses the next time it starts, and the launcher and sound. Its
   pages (online and developer options, third-party notices, build tools and cache,
   removing the build tools) are dialog cards like the rest of the flow. Every change is
   saved as it is made. */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"
#include "scr_shared.h"

#include <stdio.h>
#include <string.h>

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
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
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
    if (scr_wizard_box_begin(ui, 0u)) {
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
                    scr_flow_bytes(a, sizeof(a), model->update_zip_bytes);
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
                scr_flow_bytes(a, sizeof(a), model->update_received_bytes);
                scr_flow_bytes(b, sizeof(b), model->update_zip_bytes);
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
            scr_wizard_switch(ui, PLAYER_UI_ID_UPDATE_AT_START, SET_ID_TEXT + 0xE0u,
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
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
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
    if (scr_wizard_box_begin(ui, 0u)) {
        const WizardRow rows[] = {
            {"Interface", safe_text(model->interface_size, "Automatic"), interface_line},
            {"Text size", safe_text(model->text_size, "Standard"),
             "Makes the launcher's text larger. Controls keep their size."},
            {"Contrast", safe_text(model->high_contrast_choice, "Automatic"), contrast_line},
            {"Motion", safe_text(model->reduce_motion_choice, "Automatic"), motion_line}
        };
        /* The four buttons line up on one column at every text size. */
        WizardColumn column = scr_wizard_column(ui, rows, sizeof(rows) / sizeof(rows[0]),
                                            inner - 32.0f);
        if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_INTERFACE_SIZE, SET_ID_TEXT + 0x300u,
                         rows[0].name, rows[0].value, rows[0].sentence, false, inner - 32.0f,
                         &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_INTERFACE_SIZE);
        }
        if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_TEXT_SIZE, SET_ID_TEXT + 0x30Cu,
                         rows[1].name, rows[1].value, rows[1].sentence, false, inner - 32.0f,
                         &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_TEXT_SIZE);
        }
        if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_HIGH_CONTRAST, SET_ID_TEXT + 0x304u,
                         rows[2].name, rows[2].value, rows[2].sentence, model->high_contrast,
                         inner - 32.0f, &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_HIGH_CONTRAST);
        }
        if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_REDUCE_MOTION, SET_ID_TEXT + 0x308u,
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
    scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
    nk_ui_text_role(ui, FLOW_ID_TITLE, "Ace Mode", NK_UI_TYPE_TITLE, theme->paper);
    nk_ui_text_role(ui, FLOW_ID_LEAD,
                    "Modern touches for every game. The games themselves run as released. "
                    "Each game's page can turn it on or off for that game alone.",
                    NK_UI_TYPE_BODY_S, theme->muted);
    if (scr_wizard_box_begin(ui, 0u)) {
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
        WizardColumn column = scr_wizard_column(ui, rows, sizeof(rows) / sizeof(rows[0]), inner - 32.0f);
        if (scr_wizard_value(ui, PLAYER_UI_ID_SETTINGS_ACE_MODE, SET_ID_TEXT + 0x400u, rows[0].name,
                         rows[0].value, rows[0].sentence, model->ace_enabled, inner - 32.0f,
                         &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_ACE);
        }
        if (scr_wizard_value_state(ui, PLAYER_UI_ID_SETTINGS_ACE_FILTERING, SET_ID_TEXT + 0x404u,
                               rows[1].name, rows[1].value, rows[1].sentence,
                               strcmp(rows[1].value, "Off") != 0, off, inner - 32.0f, &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_CYCLE_ACE_FILTERING);
        }
        if (scr_wizard_value_state(ui, PLAYER_UI_ID_SETTINGS_ACE_TEXT, SET_ID_TEXT + 0x408u,
                               rows[2].name, rows[2].value, rows[2].sentence, model->ace_text, off,
                               inner - 32.0f, &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_ACE_TEXT);
        }
        if (scr_wizard_value_state(ui, PLAYER_UI_ID_SETTINGS_ACE_READOUT, SET_ID_TEXT + 0x40Cu,
                               rows[3].name, rows[3].value, rows[3].sentence, model->ace_readout,
                               off, inner - 32.0f, &column)) {
            *result = settings_op(PLAYER_UI_SETTINGS_TOGGLE_ACE_READOUT);
        }
        if (scr_wizard_value_state(ui, PLAYER_UI_ID_SETTINGS_ACE_NOTICE, SET_ID_TEXT + 0x410u,
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
    float inner = scr_flow_begin(ui, width, height, 760.0f);
    if (inner <= 0.0f) return;
    switch (model->page) {
    case PLAYER_UI_SETTINGS_PAGE_ONLINE:
        scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Online and developer options", NK_UI_TYPE_TITLE,
                        theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "News and reports aren't built yet, so online mode makes no "
                        "connection. Your choice is kept for when they arrive.",
                        NK_UI_TYPE_BODY_S, theme->warning);
        if (scr_wizard_box_begin(ui, 0u)) {
            WizardColumn column = scr_online_developer_column(ui, model->online_mode,
                                                          model->developer_mode, inner - 32.0f);
            if (scr_wizard_switch(ui, PLAYER_UI_ID_SETTINGS_ONLINE_MODE, SET_ID_TEXT + 0x80u,
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
            if (scr_wizard_switch(ui, PLAYER_UI_ID_SETTINGS_DEVELOPER, SET_ID_TEXT + 0x90u,
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
        scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Third-party notices", theme->accent_dark);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Build tools and their licences", NK_UI_TYPE_TITLE,
                        theme->paper);
        if (scr_wizard_box_begin(ui, 0u)) {
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
        scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Build tools and cache", NK_UI_TYPE_TITLE,
                        theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "A game disc carries system libraries that other games often share. "
                        "The compiled copies stay in the app-data folder on this PC and are "
                        "never uploaded or shared.",
                        NK_UI_TYPE_BODY_S, theme->muted);
        if (scr_wizard_box_begin(ui, 0u)) {
            NkUiLayout row = layout_make(NK_UI_LAYOUT_ROW, sizing_fit(), sizing_fit(),
                                         NK_UI_SPACE_3);
            if (scr_wizard_switch(ui, PLAYER_UI_ID_SETTINGS_SHARE_LIBRARIES, SET_ID_TEXT + 0xC0u,
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
        scr_flow_eyebrow(ui, FLOW_MARK_DIAMOND, "Settings", theme->accent_dark);
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
        if (scr_wizard_box_begin(ui, 0u)) {
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
        scr_flow_eyebrow(ui, FLOW_MARK_TRIANGLE, "Factory reset", theme->warning);
        nk_ui_text_role(ui, FLOW_ID_TITLE, "Reset Nakagawa Recomp to a first run?",
                        NK_UI_TYPE_TITLE, theme->paper);
        nk_ui_text_role(ui, FLOW_ID_LEAD,
                        "Your library, saves, imported PSP fonts, keys, settings, built games "
                        "and downloaded build tools move into backup folders. Nakagawa Recomp "
                        "then starts again with the setup it shows the first time. Nothing is "
                        "deleted, and your disc images and logs stay where they are.",
                        NK_UI_TYPE_BODY, theme->muted);
        if (scr_wizard_box_begin(ui, 0u)) {
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
        scr_flow_eyebrow(ui, FLOW_MARK_TRIANGLE, "Remove build tools", theme->warning);
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
    scr_flow_end(ui, "", prompts, 3u);
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
