/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_nk.h"
#include "nk_ui_text.h"
#include "nk_ui_theme.h"
#include "nk_ui_widgets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NK_UI_SCREEN_CAPTURE)
#include <SDL3/SDL.h>
#include "backend/nk_ui_sdlr.h"
#endif

#define DUMP_CAP 1048576u
#define CASE_DUMP_CAP 131072u

static int failures;
static const uint8_t k_test_glyph_pixel = 0xFFu;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
        ++failures; \
    } \
} while (0)

static bool screen_test_raster(void *user, int font, int px, uint32_t glyph,
                               NkUiGlyphBitmap *out) {
    (void)user;
    (void)font;
    (void)glyph;
    if (out == NULL || px <= 0) return false;
    memset(out, 0, sizeof(*out));
    out->pixels = &k_test_glyph_pixel;
    out->width = 1;
    out->height = 1;
    out->pitch = 1;
    out->bearing_y = 1;
    out->advance_x = (float)px * 0.5f;
    return true;
}

static NkUiTextBackend screen_test_text_backend(void) {
    NkUiTextBackend backend = nk_ui_text_fixed();
    backend.raster = screen_test_raster;
    return backend;
}

static void set_test_viewport(NkUi *ui, int width, int height) {
    NkUiViewport viewport = {width, height, 1.0f, 100, 100, 0.0f, 0.0f};
    nk_ui_set_viewport(ui, &viewport);
}

static const PlayerUiTitle k_titles[] = {
    {"Synthetic Showcase Scene", "TEST00007", "Ready", "",
     "Showcase profile-zero acceptance remains open.", "", NULL, "",
     "", "", "PLAY", "●", 0x2DD4BFFFu, PLAYER_UI_PRIMARY_PLAY, true, true,
     NK_UI_ART_SHOWCASE_SCENE, NULL, NULL, NULL, PLAYER_UI_PACKAGE_READY,
     PLAYER_UI_TIER_PLAYABLE, false, false},
    {"Synthetic Showcase Breakout", "TEST00008", "Needs setup", "",
     "Showcase profile-zero acceptance remains open.", "", NULL, "",
     "", "", "BUILD PACKAGE", "▲", 0xDBB024FFu, PLAYER_UI_PRIMARY_BUILD, true, true,
     NK_UI_ART_SHOWCASE_BREAKOUT, NULL, NULL, NULL, PLAYER_UI_PACKAGE_MISSING,
     PLAYER_UI_TIER_PLAYABLE, false, false},
    {"Synthetic Experimental Fixture", "TEST00006", "Experimental", "",
     "Source-owned compatibility fixture.", "", NULL, "", "", "",
     "RETRY PACKAGE CHECK", "◆", 0x6CB6FFFFu,
     PLAYER_UI_PRIMARY_RETRY_PACKAGE_CHECK, true, true,
     0u, NULL, NULL, NULL, PLAYER_UI_PACKAGE_MISSING,
     PLAYER_UI_TIER_NONE, true, false},
    {"Synthetic Untested Fixture", "TEST00005", "Untested", "",
     "No compatibility record exists.", "", NULL, "", "", "",
     "BUILD PACKAGE", "?", 0x6CB6FFFFu, PLAYER_UI_PRIMARY_BUILD, true, true,
     0u, NULL, NULL, NULL, PLAYER_UI_PACKAGE_MISSING,
     PLAYER_UI_TIER_NONE, true, false},
    {"Synthetic Phase Five", "TEST00004", "Needs setup", "",
     "Player launch stops at the package boundary.", "", NULL, "",
     "", "", "BUILD PACKAGE", "▲", 0xDBB024FFu, PLAYER_UI_PRIMARY_BUILD, true, true,
     0u, NULL, NULL, NULL, PLAYER_UI_PACKAGE_MISSING,
     PLAYER_UI_TIER_WONT_BOOT, true, false},
    {"Synthetic Title Two", "TEST00002", "Needs setup", "",
     "Source-owned runtime fixture.", "", NULL, "", "", "",
     "BUILD PACKAGE", "▲", 0xDBB024FFu, PLAYER_UI_PRIMARY_BUILD, true, true,
     0u, NULL, NULL, NULL, PLAYER_UI_PACKAGE_MISSING,
     PLAYER_UI_TIER_PERFECT, true, false},
    {"Synthetic Profile", "TEST00001", "Ready", "",
     "Source-owned runtime fixture.", "", NULL, "", "", "",
     "PLAY", "●", 0x2DD4BFFFu, PLAYER_UI_PRIMARY_PLAY, true, true,
     0u, NULL, NULL, NULL, PLAYER_UI_PACKAGE_READY,
     PLAYER_UI_TIER_PERFECT, true, false}
};

/* A library with the demos that come with Nakagawa among the user's games, in
   the order the player keeps them: demos at 0 and 2, games at 1 and 3. */
static const PlayerUiTitle k_demo_titles[] = {
    {"Synthetic Breakout Demo", "TEST00008", "Ready", "",
     "Plays a synthetic game with sound.", "", NULL, "", "", "",
     "PLAY", "●", 0x2DD4BFFFu, PLAYER_UI_PRIMARY_PLAY, true, true,
     NK_UI_ART_SHOWCASE_BREAKOUT, "Showcase demo", NULL, NULL, PLAYER_UI_PACKAGE_READY,
     PLAYER_UI_TIER_PLAYABLE, false, true},
    {"Synthetic Title Two", "TEST00002", "Needs setup", "",
     "Source-owned runtime fixture.", "", NULL, "", "", "",
     "BUILD PACKAGE", "▲", 0xDBB024FFu, PLAYER_UI_PRIMARY_BUILD, true, true,
     0u, NULL, NULL, NULL, PLAYER_UI_PACKAGE_MISSING,
     PLAYER_UI_TIER_PERFECT, true, false},
    {"Synthetic Scene Demo", "TEST00007", "Ready", "",
     "Shows a synthetic cube with sound.", "", NULL, "", "", "",
     "PLAY", "●", 0x2DD4BFFFu, PLAYER_UI_PRIMARY_PLAY, true, true,
     NK_UI_ART_SHOWCASE_SCENE, "Showcase demo", NULL, NULL, PLAYER_UI_PACKAGE_READY,
     PLAYER_UI_TIER_PLAYABLE, false, true},
    {"Synthetic Profile", "TEST00001", "Ready", "",
     "Source-owned runtime fixture.", "", NULL, "", "", "",
     "PLAY", "●", 0x2DD4BFFFu, PLAYER_UI_PRIMARY_PLAY, true, true,
     0u, NULL, NULL, NULL, PLAYER_UI_PACKAGE_READY,
     PLAYER_UI_TIER_PERFECT, true, false}
};

/* The demos alone: a first start with nothing added yet. */
static const PlayerUiTitle k_demo_only_titles[] = {
    {"Synthetic Breakout Demo", "TEST00008", "Ready", "",
     "Plays a synthetic game with sound.", "", NULL, "", "", "",
     "PLAY", "●", 0x2DD4BFFFu, PLAYER_UI_PRIMARY_PLAY, true, true,
     NK_UI_ART_SHOWCASE_BREAKOUT, "Showcase demo", NULL, NULL, PLAYER_UI_PACKAGE_READY,
     PLAYER_UI_TIER_PLAYABLE, false, true},
    {"Synthetic Scene Demo", "TEST00007", "Ready", "",
     "Shows a synthetic cube with sound.", "", NULL, "", "", "",
     "PLAY", "●", 0x2DD4BFFFu, PLAYER_UI_PRIMARY_PLAY, true, true,
     NK_UI_ART_SHOWCASE_SCENE, "Showcase demo", NULL, NULL, PLAYER_UI_PACKAGE_READY,
     PLAYER_UI_TIER_PLAYABLE, false, true}
};

/* What-to-expect cards: a player tier with plain words, with known issues, the
   developer view's category, and the screen's own fallback with no tier. */
static const struct {
    PlayerUiTier tier;
    const char *expect;
    const char *known_issues;
    const char *category;
} k_compatibility_cases[] = {
    {PLAYER_UI_TIER_PERFECT, "Plays a full synthetic match.", "", NULL},
    {PLAYER_UI_TIER_PLAYABLE, "Plays a full synthetic match.",
     "Known issues: low frame rate, sound drops out, and movies show black.", NULL},
    {PLAYER_UI_TIER_TITLE_SCREEN, "Plays a full synthetic match.", "",
     "Boots to a named boundary"},
    {PLAYER_UI_TIER_IN_GAME, "Plays a synthetic stage.", "", NULL},
    {PLAYER_UI_TIER_MENU, "Reaches the synthetic menu.", "", NULL},
    {PLAYER_UI_TIER_WONT_BOOT, "Does not start yet.", "", NULL},
    {PLAYER_UI_TIER_NONE, NULL, NULL, NULL}
};

/* Synthetic flow states: a build, the build tool download, an error. */
static const PlayerUiPrereqItem k_prereqs[] = {
    {"synthetic-compiler", "1.0", "example.invalid", "MIT", 52428800ull},
    {"synthetic-runtime", "2.1", "example.invalid", "BSD-3-Clause", 1048576ull},
    {"synthetic-headers", "0.9", "example.invalid", "NOASSERTION", 4096ull}
};

typedef enum { FLOW_NONE = 0, FLOW_BUILD, FLOW_BUILT, FLOW_BUILD_PARTS, FLOW_CONSENT,
               FLOW_PROGRESS, FLOW_ERROR, FLOW_WIZ_WELCOME, FLOW_WIZ_CHOOSE,
               FLOW_WIZ_BAD_DISC, FLOW_WIZ_CHOSEN,
               FLOW_WIZ_COPYING, FLOW_WIZ_VERIFY, FLOW_WIZ_EXPERIMENTAL, FLOW_WIZ_FONTS,
               FLOW_WIZ_READY, FLOW_WIZ_STAGED, FLOW_WIZ_OPTIONS, FLOW_WIZ_BLOCKED,
               FLOW_WIZ_COPY_FAILED, FLOW_ERROR_STOP, FLOW_DISC_CHECKING, FLOW_DISC_SUPPORTED,
               FLOW_DISC_UNTESTED, FLOW_DISC_FONTS, FLOW_DISC_BLOCKED, FLOW_DISC_UNSUPPORTED,
               FLOW_SETTINGS, FLOW_SETTINGS_ONLINE, FLOW_SETTINGS_NOTICES,
               FLOW_SETTINGS_REMOVE, FLOW_SETTINGS_BUILD_CACHE, FLOW_SETTINGS_FONTS,
               FLOW_CONTROLLER,
               FLOW_CONTROLLER_GAME, FLOW_CONTROLLER_KEYS,
               FLOW_CALIBRATE_REST, FLOW_CALIBRATE_EXTREMES, FLOW_CALIBRATE_RESULT,
               FLOW_DEVLOG, FLOW_DEVLOG_EMPTY,
               /* Whole pages for the window-fit sweep only. */
               FLOW_PAGE_LIBRARY, FLOW_PAGE_TITLE, FLOW_PAGE_ABOUT, FLOW_PAGE_REMOVE,
               FLOW_PAGE_STOP,
               FLOW_CONSENT_FULL, FLOW_COUNT } FlowCase;

/* The window size the game-found and Settings cases lay out for. */
static float g_flow_width = 1280.0f;
static float g_flow_height = 800.0f;

/* The check rows player_state.c gives a disc, in its words (#1302). */
static const PlayerUiCheckRow k_disc_rows_ready[] = {
    {"Ready", "Game program", "Found the game's program.", 0x2DD4BFFFu},
    {"Ready", "Game modules", "Found the game's other program files.", 0x2DD4BFFFu},
    {"Not built yet", "Game build",
     "Not built yet. After you add the game, build it from the Library.", 0xDBB024FFu},
    {"Checked later", "Game data", "Nakagawa checks the game's files again after you build it.",
     0xDBB024FFu},
    {"Ready", "Fonts", "PSP system fonts found.", 0x2DD4BFFFu},
    {"Ready", "Sound", "Sound plays through your default audio device. With no device, the "
     "game runs silently.", 0x2DD4BFFFu}
};

static const PlayerUiCheckRow k_disc_rows_fonts[] = {
    {"Ready", "Game program", "Found the game's program.", 0x2DD4BFFFu},
    {"Ready", "Game modules", "Found the game's other program files.", 0x2DD4BFFFu},
    {"Not built yet", "Game build",
     "Not built yet. After you add the game, build it from the Library.", 0xDBB024FFu},
    {"Checked later", "Game data", "Nakagawa checks the game's files again after you build it.",
     0xDBB024FFu},
    {"Some missing", "Fonts", "Some PSP system fonts were not found, so some text may look "
     "different. You can import your own fonts in Settings, PSP fonts.", 0xDBB024FFu},
    {"Ready", "Sound", "Sound plays through your default audio device. With no device, the "
     "game runs silently.", 0x2DD4BFFFu}
};

static PlayerUiAction draw_disc_case(NkUi *ui, FlowCase which) {
    PlayerUiDiscModel disc;
    const PlayerUiCheckRow *rows = k_disc_rows_ready;
    size_t count = sizeof(k_disc_rows_ready) / sizeof(k_disc_rows_ready[0]);
    memset(&disc, 0, sizeof(disc));
    disc.title_name = "Synthetic Showcase Scene";
    disc.disc_id = "TEST00007";
    disc.iso_path = "C:/synthetic/Showcase/TEST00007.iso";
    disc.logical_width = g_flow_width;
    disc.logical_height = g_flow_height;
    switch (which) {
        case FLOW_DISC_CHECKING: disc.kind = PLAYER_UI_DISC_CHECKING; count = 0u; break;
        case FLOW_DISC_SUPPORTED:
            disc.kind = PLAYER_UI_DISC_SUPPORTED;
            disc.disc_id = "TEST00001";
            break;
        case FLOW_DISC_UNTESTED: disc.kind = PLAYER_UI_DISC_UNTESTED; break;
        case FLOW_DISC_FONTS:
            disc.kind = PLAYER_UI_DISC_SUPPORTED;
            disc.disc_id = "TEST00001";
            rows = k_disc_rows_fonts;
            count = sizeof(k_disc_rows_fonts) / sizeof(k_disc_rows_fonts[0]);
            break;
        case FLOW_DISC_BLOCKED:
            disc.kind = PLAYER_UI_DISC_BLOCKED;
            disc.reason = "Nakagawa couldn't decrypt the game's program: it uses an encryption "
                          "Nakagawa doesn't cover yet. You can put a decrypted EBOOT.elf from your own copy in "
                          "D:/Synthetic Players/Long User Name/Nakagawa Recomp/data/runtime/"
                          "titles/TEST00007/decrypted. Then open the disc again.";
            /* The checks after the program's: what the player shows for one. */
            count = 4u;
            rows = k_disc_rows_fonts + 2u;
            break;
        case FLOW_DISC_UNSUPPORTED:
        default:
            disc.kind = PLAYER_UI_DISC_UNSUPPORTED;
            disc.disc_id = "ULES99999";
            disc.title_name = NULL;
            disc.profile_details = "Synthetic profile refusal: shown as data.";
            count = 0u;
            break;
    }
    for (size_t i = 0u; i < count && i < PLAYER_UI_DISC_CHECKS; ++i) disc.checks[i] = rows[i];
    disc.check_count = count;
    return player_ui_screen_disc(ui, &disc);
}

static const char *const k_font_lines[18];

static const char *const k_settings_tools[] = {
    "synthetic-compiler 14.2 \xC2\xB7 GPL-3.0-or-later",
    "synthetic-headers 0.9 \xC2\xB7 licence review in the works"
};

static PlayerUiAction draw_settings_case(NkUi *ui, FlowCase which) {
    PlayerUiSettingsModel settings;
    memset(&settings, 0, sizeof(settings));
    settings.page = which == FLOW_SETTINGS_ONLINE ? PLAYER_UI_SETTINGS_PAGE_ONLINE
        : which == FLOW_SETTINGS_NOTICES ? PLAYER_UI_SETTINGS_PAGE_NOTICES
        : which == FLOW_SETTINGS_REMOVE ? PLAYER_UI_SETTINGS_PAGE_REMOVE_TOOLS
        : which == FLOW_SETTINGS_BUILD_CACHE ? PLAYER_UI_SETTINGS_PAGE_BUILD_CACHE
        : which == FLOW_SETTINGS_FONTS ? PLAYER_UI_SETTINGS_PAGE_FONTS
        : PLAYER_UI_SETTINGS_PAGE_MAIN;
    settings.resolution = "2x (960x544)";
    settings.pacing = "PSP timing";
    settings.scaling = "Sharp";
    settings.vsync = true;
    settings.volume = 80;
    settings.controller_line = "keyboard and mouse";
    settings.saves_folder = "C:/synthetic/Nakagawa/saves";
    settings.online_mode = true;
    settings.developer_mode = true;
    settings.share_system_libraries = true;
    if (which == FLOW_SETTINGS_BUILD_CACHE) {
        settings.notice = "Shared library cache cleared. The next build compiles its libraries again.";
    }
    if (which == FLOW_SETTINGS_FONTS) {
        for (size_t i = 0u; i < sizeof(k_font_lines) / sizeof(k_font_lines[0]); ++i) {
            settings.font_lines[settings.font_line_count++] = k_font_lines[i];
        }
        settings.font_message = "Imported 3 fonts from C:/synthetic/psp-fonts.";
        settings.font_message_kind = PLAYER_UI_NOTE_DONE;
    }
    settings.tools[0] = k_settings_tools[0];
    settings.tools[1] = k_settings_tools[1];
    settings.tool_count = 2u;
    settings.logical_width = g_flow_width;
    settings.logical_height = g_flow_height;
    return player_ui_screen_settings(ui, &settings);
}

/* input_settings.c's names and its default bindings, in its own words: the
   longest it writes, so the rows are laid out as tight as they get. */
static const char *const k_pad_names[PLAYER_UI_CONTROLLER_BUTTONS] = {
    "Select", "Start", "D-Pad Up", "D-Pad Right", "D-Pad Down", "D-Pad Left",
    "L Trigger (L1)", "R Trigger (R1)", "Triangle", "Circle", "Cross", "Square", "Home", "Hold"
};
static const char *const k_pad_bindings[PLAYER_UI_CONTROLLER_BUTTONS] = {
    "Back (Select)", "Start", "D-Pad Up", "D-Pad Right", "D-Pad Down",
    "D-Pad Left", "LB (L1) / LT (L2)", "RB (R1) / RT (R2)",
    "Y (Triangle)", "B (Circle)", "A (Cross)", "X (Square)", "Guide (Home)", "None (Unbound)"
};
/* When set, Select's controller binding in the controller cases. */
static const char *k_pad_bindings_override;
/* input_settings.c's words for the profile's default keyboard keys (#1299). */
static const char *const k_key_bindings[PLAYER_UI_CONTROLLER_BUTTONS] = {
    "Shift", "Enter", "Up Arrow", "Right Arrow", "Down Arrow", "Left Arrow",
    "Q", "W", "S", "Z", "X", "A", "None", "None"
};
/* The glyphs player_ui_nk.c gives the defaults (#1299): the controller's in the set of
   an Xbox-style pad, the keys as keycaps; Home and Hold are unbound. */
#define GLYPH(action, label) {true, NK_UI_PROMPT_ACT_##action, label}
#define NO_GLYPH {false, NK_UI_PROMPT_ACT_CONFIRM, NULL}
static const PlayerUiBindingGlyph k_pad_glyphs[PLAYER_UI_CONTROLLER_BUTTONS] = {
    GLYPH(TAB_PREV, "VIEW"), GLYPH(MENU, NULL), GLYPH(MOVE, NULL), GLYPH(MOVE, NULL),
    GLYPH(MOVE, NULL), GLYPH(MOVE, NULL), GLYPH(TAB_PREV, NULL), GLYPH(TAB_NEXT, NULL),
    GLYPH(DETAILS, NULL), GLYPH(BACK, NULL), GLYPH(CONFIRM, NULL), GLYPH(OPTIONS, NULL),
    NO_GLYPH, NO_GLYPH
};
static const PlayerUiBindingGlyph k_key_glyphs[PLAYER_UI_CONTROLLER_BUTTONS] = {
    GLYPH(CONFIRM, "SHIFT"), GLYPH(CONFIRM, "ENTER"), GLYPH(MOVE, NULL), GLYPH(MOVE, NULL),
    GLYPH(MOVE, NULL), GLYPH(MOVE, NULL), GLYPH(CONFIRM, "Q"), GLYPH(CONFIRM, "W"),
    GLYPH(CONFIRM, "S"), GLYPH(CONFIRM, "Z"), GLYPH(CONFIRM, "X"), GLYPH(CONFIRM, "A"),
    NO_GLYPH, NO_GLYPH
};
#undef GLYPH
#undef NO_GLYPH

static PlayerUiAction draw_controller_case(NkUi *ui, FlowCase which) {
    PlayerUiControllerModel pad;
    memset(&pad, 0, sizeof(pad));
    pad.page = which == FLOW_CALIBRATE_REST ? PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_REST
        : which == FLOW_CALIBRATE_EXTREMES ? PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_EXTREMES
        : which == FLOW_CALIBRATE_RESULT ? PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_RESULT
        : PLAYER_UI_CONTROLLER_PAGE_MAPPING;
    for (size_t i = 0u; i < PLAYER_UI_CONTROLLER_BUTTONS; ++i) {
        pad.names[i] = k_pad_names[i];
        pad.bindings[i] = which == FLOW_CONTROLLER_KEYS ? k_key_bindings[i] : k_pad_bindings[i];
        pad.glyphs[i] = which == FLOW_CONTROLLER_KEYS ? k_key_glyphs[i] : k_pad_glyphs[i];
    }
    pad.glyph_style = which == FLOW_CONTROLLER_KEYS ? NK_UI_PROMPT_KEYCAPS
                                                    : NK_UI_PROMPT_LETTERS_A_SOUTH;
    if (k_pad_bindings_override != NULL && which != FLOW_CONTROLLER_KEYS) {
        pad.bindings[0] = k_pad_bindings_override;
    }
    pad.capturing = -1;
    pad.scope = "global mapping";
    pad.deadzone = "Dead zone 24%";
    pad.trigger = "Triggers at 49%";
    pad.live[0] = "Stick  -32768,  32767";
    pad.live[1] = "PSP      0, 255  moving";
    pad.live[2] = "Triggers L 32767  R 0";
    pad.live[3] = "Pressed south, east, left_shoulder";
    pad.live_count = 4u;
    if (which == FLOW_CONTROLLER_GAME) {
        /* One game's own mapping, a control waiting for a press, and a conflict. */
        pad.scope = "TEST00005 only";
        pad.scope_title = true;
        pad.offer_scope = true;
        pad.capturing = 10;
        pad.conflicted[8] = true;
        pad.conflicted[11] = true;
        pad.status = "Conflict: Y (Triangle) bound to both Triangle and Square";
        pad.status_error = true;
    } else if (which == FLOW_CONTROLLER_KEYS) {
        /* The keyboard keys of a game's own mapping, with Start waiting for a key. */
        pad.keyboard = true;
        pad.scope = "TEST00005 only";
        pad.scope_title = true;
        pad.offer_scope = true;
        pad.capturing = 1;
        pad.status = "Press the key for Start. 5 s left; Esc stops.";
    } else if (which == FLOW_CALIBRATE_REST) {
        pad.calibration_fraction = 0.4f;
        pad.live_count = 2u;
    } else if (which == FLOW_CALIBRATE_RESULT) {
        pad.live_count = 3u;
    }
    pad.logical_width = g_flow_width;
    pad.logical_height = g_flow_height;
    return player_ui_screen_controller(ui, &pad);
}

static PlayerUiAction draw_developer_log_case(NkUi *ui, FlowCase which) {
    static char lines[PLAYER_UI_DEVELOPER_LOG_LINES][160];
    PlayerUiDeveloperLogModel log;
    memset(&log, 0, sizeof(log));
    if (which == FLOW_DEVLOG) {
        /* A full log: the newest lines are the ones shown. */
        for (size_t i = 0u; i < PLAYER_UI_DEVELOPER_LOG_LINES; ++i) {
            (void)snprintf(lines[i], sizeof(lines[i]),
                           "[%03zu] [runtime] synthetic line %zu from %%USERPROFILE%%\\fixture "
                           "that is long enough to be cut at the edge of the box",
                           i, i);
            log.lines[i] = lines[i];
        }
        log.line_count = PLAYER_UI_DEVELOPER_LOG_LINES;
        log.notice = "Copied 256 lines.";
    }
    log.logical_width = g_flow_width;
    log.logical_height = g_flow_height;
    return player_ui_screen_developer_log(ui, &log);
}

static const char *const k_font_lines[18] = {
    "Japanese: synthetic fallback", "Latin 1: synthetic fallback",
    "Latin 2: synthetic fallback", "Latin 3: synthetic fallback",
    "Latin 4: synthetic fallback", "Latin 5: synthetic fallback",
    "Latin 6: synthetic fallback", "Latin 7: synthetic fallback",
    "Latin 8: synthetic fallback", "Latin 9: synthetic fallback",
    "Latin 10: synthetic fallback", "Latin 11: synthetic fallback",
    "Latin 12: synthetic fallback", "Latin 13: synthetic fallback",
    "Latin 14: synthetic fallback", "Latin 15: synthetic fallback",
    "Latin 16: synthetic fallback", "Korean: synthetic fallback"
};

/* The setup wizard's steps on synthetic state. */
static PlayerUiAction draw_wizard_case(NkUi *ui, FlowCase which) {
    PlayerUiWizardModel wizard;
    memset(&wizard, 0, sizeof(wizard));
    wizard.logical_width = g_flow_width;
    wizard.logical_height = g_flow_height;
    wizard.disc_id = "TEST00007";
    wizard.title_name = "Synthetic Showcase Scene";
    switch (which) {
        case FLOW_WIZ_CHOOSE:
            wizard.step = 2;
            break;
        case FLOW_WIZ_BAD_DISC:
            wizard.step = 2;
            wizard.status_message = "This file isn't a PSP disc image.";
            break;
        case FLOW_WIZ_CHOSEN:
            wizard.step = 2;
            wizard.has_iso = true;
            wizard.iso_selected = true;
            wizard.iso_path = "C:/synthetic/discs/synthetic-showcase.iso";
            wizard.iso_detail = "Size: 12.00 MB \xC2\xB7 Disc ID: TEST00007";
            wizard.status_message = "Disc identified: Synthetic Showcase Scene.";
            break;
        case FLOW_WIZ_COPYING:
            wizard.step = 3;
            wizard.supported = true;
            wizard.extracting = true;
            wizard.files_extracted = 41;
            wizard.total_files = 96;
            wizard.extraction_percent = 42;
            wizard.current_file = "PSP_GAME/USRDIR/synthetic.bin";
            break;
        case FLOW_WIZ_VERIFY:
        case FLOW_WIZ_EXPERIMENTAL:
            wizard.step = 3;
            wizard.supported = which == FLOW_WIZ_VERIFY;
            wizard.experimental = which == FLOW_WIZ_EXPERIMENTAL;
            wizard.extraction_complete = which == FLOW_WIZ_VERIFY;
            wizard.checks[0] = (PlayerUiCheckRow){"OK", "DISC_IDENTITY",
                                                  "The disc ID matches a catalog entry.",
                                                  0x2DD4BFFFu};
            wizard.checks[1] = (PlayerUiCheckRow){"OK", "ASSETS_STAGED",
                                                  "96 files staged locally.", 0x2DD4BFFFu};
            wizard.checks[2] = (PlayerUiCheckRow){"Missing", "RUNTIME_PACKAGE",
                                                  "Build the package from the library.",
                                                  0xDBB024FFu};
            wizard.check_count = 3u;
            break;
        case FLOW_WIZ_FONTS:
            wizard.step = 4;
            wizard.resolution_line = "Internal resolution: 2x (960x544)";
            wizard.controller_line = "Controller: keyboard and mouse";
            for (size_t i = 0u; i < sizeof(k_font_lines) / sizeof(k_font_lines[0]); ++i) {
                wizard.font_lines[wizard.font_line_count++] = k_font_lines[i];
            }
            break;
        case FLOW_WIZ_READY:
        case FLOW_WIZ_STAGED:
            wizard.step = 5;
            wizard.runtime_ready = which == FLOW_WIZ_READY;
            wizard.summary_line = "Resolution 2x (960x544) \xC2\xB7 Windowed \xC2\xB7 "
                                  "Sound 80% \xC2\xB7 Package ready";
            break;
        case FLOW_WIZ_OPTIONS:
            wizard.step = 1;
            wizard.online_mode = true;
            break;
        case FLOW_WIZ_BLOCKED:
            /* #1280: an experimental disc this version cannot use. */
            wizard.step = 3;
            wizard.experimental = true;
            wizard.blocked_reason = "EBOOT.BIN is encrypted and built-in decryption stopped. "
                                    "Then open the disc again.";
            wizard.checks[0] = (PlayerUiCheckRow){"Not built yet", "Game build",
                                                  "Not built yet.", 0xDBB024FFu};
            wizard.check_count = 1u;
            break;
        case FLOW_WIZ_COPY_FAILED:
            /* #1278: the copy failed; the card says why. */
            wizard.step = 3;
            wizard.extraction_failed = true;
            wizard.extraction_error = "Synthetic reason: the title manifest asks for a folder "
                                      "that is not on this disc image.";
            break;
        case FLOW_WIZ_WELCOME:
        default:
            wizard.step = 0;
            break;
    }
    return player_ui_screen_wizard(ui, &wizard);
}
/* The build tool consent with as many items as a clean PC downloads. */
static PlayerUiPrereqItem g_full_prereqs[35];

/* Whole pages at the sweep's window size: the Library with a page of cards,
   a title page, About, and the remove dialog. */
static PlayerUiAction draw_page_case(NkUi *ui, FlowCase which) {
    PlayerUiAction none = {PLAYER_UI_ACTION_NONE, PLAYER_UI_PRIMARY_NONE, 0u};
    if (which == FLOW_PAGE_LIBRARY) {
        PlayerUiLibraryModel library;
        memset(&library, 0, sizeof(library));
        library.page_size = player_ui_library_page_size(g_flow_width, g_flow_height,
                                                        &library.columns);
        library.logical_width = g_flow_width;
        library.logical_height = g_flow_height;
        library.titles = k_titles;
        library.title_count = sizeof(k_titles) / sizeof(k_titles[0]);
        library.page_label = "Page 1 of 2";
        return player_ui_screen_library(ui, &library);
    }
    if (which == FLOW_PAGE_TITLE) {
        static PlayerUiTitle fixture;
        PlayerUiTitleModel title;
        fixture = k_titles[0];
        fixture.compatibility_tier = k_compatibility_cases[1].tier;
        fixture.compatibility_expect = k_compatibility_cases[1].expect;
        fixture.compatibility_known_issues = k_compatibility_cases[1].known_issues;
        memset(&title, 0, sizeof(title));
        title.title = &fixture;
        title.title_index = 0u;
        title.logical_width = g_flow_width;
        title.logical_height = g_flow_height;
        return player_ui_screen_title(ui, &title);
    }
    if (which == FLOW_PAGE_ABOUT) {
        static char line[64];
        PlayerUiAboutModel about;
        player_ui_version_line(NK_PLAYER_VERSION, 1792713600LL, line, sizeof(line));
        memset(&about, 0, sizeof(about));
        about.version_line = line;
        about.build_commit = "0123abc";
        about.logical_width = g_flow_width;
        about.logical_height = g_flow_height;
        return player_ui_screen_about(ui, &about);
    }
    if (which == FLOW_PAGE_REMOVE) {
        static const PlayerUiRemoveItem items[] = {
            {"Built package", 3u * 1024u * 1024u}, {"Build cache", 512u * 1024u},
            {"Disc files Nakagawa copied", 40u * 1024u * 1024u}
        };
        PlayerUiRemoveModel remove;
        memset(&remove, 0, sizeof(remove));
        remove.title = "Synthetic Untested Fixture";
        remove.disc_id = "TEST00009";
        remove.has_saves = true;
        remove.saves_bytes = 2u * 1024u * 1024u;
        remove.items = items;
        remove.item_count = sizeof(items) / sizeof(items[0]);
        remove.logical_width = g_flow_width;
        remove.logical_height = g_flow_height;
        return player_ui_screen_remove(ui, &remove);
    }
    if (which == FLOW_PAGE_STOP) {
        PlayerUiStopModel stop;
        memset(&stop, 0, sizeof(stop));
        stop.title = "Synthetic Showcase Scene";
        stop.title_index = 0u;
        stop.logical_width = g_flow_width;
        stop.logical_height = g_flow_height;
        return player_ui_screen_stop(ui, &stop);
    }
    if (which == FLOW_CONSENT_FULL) {
        PlayerUiPrereqModel pre;
        unsigned long long total = 0u;
        for (size_t i = 0u; i < sizeof(g_full_prereqs) / sizeof(g_full_prereqs[0]); ++i) {
            g_full_prereqs[i] = k_prereqs[i % 3u];
            g_full_prereqs[i].name = "synthetic-ucrt-x86_64-toolchain-component-with-a-long-name";
            total += g_full_prereqs[i].bytes;
        }
        memset(&pre, 0, sizeof(pre));
        pre.items = g_full_prereqs;
        pre.item_count = sizeof(g_full_prereqs) / sizeof(g_full_prereqs[0]);
        pre.total_bytes = total;
        pre.logical_width = g_flow_width;
        pre.logical_height = g_flow_height;
        return player_ui_screen_prereq(ui, &pre);
    }
    return none;
}

static FlowCase g_flow_case;

static PlayerUiAction draw_flow_case(NkUi *ui, FlowCase which) {
    PlayerUiAction none = {PLAYER_UI_ACTION_NONE, PLAYER_UI_PRIMARY_NONE, 0u};
    if (which == FLOW_ERROR_STOP) {
        /* #1282: a game that stopped early, with Restart game and Copy details. */
        PlayerUiErrorModel error;
        memset(&error, 0, sizeof(error));
        error.badge = "Game stopped";
        error.title = "Game Stopped Early";
        error.message = "The game needed ad hoc wireless play that this version can't run yet.";
        error.details = "Stopped at a synthetic boundary (not supported yet).";
        error.recovery_label = "Return to Library";
        error.retry_label = "Restart game";
        error.copy_available = true;
        error.logical_width = g_flow_width;
        error.logical_height = g_flow_height;
        return player_ui_screen_error(ui, &error);
    }
    if (which >= FLOW_PAGE_LIBRARY) return draw_page_case(ui, which);
    if (which >= FLOW_DEVLOG) return draw_developer_log_case(ui, which);
    if (which >= FLOW_CONTROLLER) return draw_controller_case(ui, which);
    if (which >= FLOW_SETTINGS) return draw_settings_case(ui, which);
    if (which >= FLOW_DISC_CHECKING) return draw_disc_case(ui, which);
    if (which >= FLOW_WIZ_WELCOME) return draw_wizard_case(ui, which);
    if (which == FLOW_BUILD || which == FLOW_BUILT || which == FLOW_BUILD_PARTS) {
        PlayerUiBuildModel build;
        memset(&build, 0, sizeof(build));
        build.title_line = "Synthetic Showcase Scene (TEST00007)";
        build.stage = which == FLOW_BUILT ? 4 : 2;
        build.built = which == FLOW_BUILT;
        build.fraction = which == FLOW_BUILD_PARTS ? 0.25f : -1.0f;
        build.message = which == FLOW_BUILD_PARTS
            ? "Compiling part 4 of 12"
            : "Compiling the translated game for this PC.";
        build.notice = which == FLOW_BUILD_PARTS
            ? "Building uses most of your PC's power for a few minutes, so other apps "
              "may feel slower until it finishes."
            : NULL;
        build.elapsed_ms = 83400u;
        build.lines[0] = "[compile] 1204 of 3311 functions";
        build.lines[1] = "[compile] chunk 12 of 31";
        build.lines[2] = "[compile] chunk 13 of 31";
        build.line_count = 3u;
        build.logical_width = g_flow_width;
        build.logical_height = g_flow_height;
        return player_ui_screen_build(ui, &build);
    }
    if (which == FLOW_CONSENT || which == FLOW_PROGRESS) {
        PlayerUiPrereqModel pre;
        memset(&pre, 0, sizeof(pre));
        pre.in_progress = which == FLOW_PROGRESS;
        pre.items = k_prereqs;
        pre.item_count = sizeof(k_prereqs) / sizeof(k_prereqs[0]);
        pre.total_bytes = 53481472ull;
        pre.current_item = "synthetic-compiler";
        pre.item_received_bytes = 20971520ull;
        pre.item_total_bytes = 52428800ull;
        pre.total_received_bytes = 20971520ull;
        pre.logical_width = g_flow_width;
        pre.logical_height = g_flow_height;
        return player_ui_screen_prereq(ui, &pre);
    }
    if (which == FLOW_ERROR) {
        PlayerUiErrorModel error;
        memset(&error, 0, sizeof(error));
        error.badge = "PACKAGE_BUILD_FAILED";
        error.title = "The package could not be built";
        error.failed_stage = "Compile";
        error.message = "The compiler stopped before the package was written.";
        error.details = "Synthetic details: a source-owned fixture error, shown as data.";
        error.log_path = "C:/synthetic/logs/build.log";
        error.recovery_label = "BACK TO LIBRARY";
        error.logical_width = g_flow_width;
        error.logical_height = g_flow_height;
        return player_ui_screen_error(ui, &error);
    }
    return none;
}

static bool append_text(char *out, size_t cap, size_t *used,
                        const char *text, size_t length) {
    if (out == NULL || used == NULL || text == NULL ||
        length > cap || *used > cap - length) {
        return false;
    }
    memcpy(out + *used, text, length);
    *used += length;
    return true;
}

static const PlayerUiAboutModel *g_about_case;

static bool append_case(char *out, size_t cap, size_t *used,
                        const char *name, NkUi *ui,
                        const PlayerUiLibraryModel *library,
                        const PlayerUiTitleModel *title) {
    const NkUiDrawList *draw;
    size_t dumped;
    static char dump[CASE_DUMP_CAP];
    int written = snprintf(dump, sizeof(dump), "--- %s ---\n", name);
    if (written < 0 || (size_t)written >= sizeof(dump) ||
        !append_text(out, cap, used, dump, (size_t)written)) {
        return false;
    }
    nk_ui_begin(ui, 0.0);
    if (g_flow_case != FLOW_NONE) {
        (void)draw_flow_case(ui, g_flow_case);
    } else if (library != NULL) {
        (void)player_ui_screen_library(ui, library);
    } else if (title != NULL) {
        (void)player_ui_screen_title(ui, title);
    } else {
        (void)player_ui_screen_about(ui, g_about_case);
    }
    draw = nk_ui_end(ui);
    if (draw == NULL) return false;
    dumped = nk_ui_dump(ui, dump, sizeof(dump));
    while (dumped > 0u &&
           (dump[dumped - 1u] == '\n' || dump[dumped - 1u] == '\r')) {
        --dumped;
    }
    if (dumped >= sizeof(dump) ||
        !append_text(out, cap, used, dump, dumped) ||
        !append_text(out, cap, used, "\n", 1u)) {
        return false;
    }
    return true;
}

/* Return the x extent of the dump row for element id, or false if it is absent. */
static bool dump_row_x_extent(const char *dump, NkUiId id, int *out_x0, int *out_x1) {
    char prefix[16];
    const char *row = dump;
    (void)snprintf(prefix, sizeof(prefix), "%08X RECT ", (unsigned)id);
    while (row != NULL && *row != '\0') {
        if (strncmp(row, prefix, strlen(prefix)) == 0) {
            int x0 = 0, y0 = 0, x1 = 0;
            if (sscanf(row + strlen(prefix), "%d %d %d", &x0, &y0, &x1) == 3) {
                *out_x0 = x0;
                *out_x1 = x1;
                return true;
            }
        }
        row = strchr(row, '\n');
        if (row != NULL) ++row;
    }
    return false;
}

/* While the compile stage counts its parts, the build card's bar is filled to the
   compiled share instead of sweeping. */
static void test_build_bar_shows_compiled_parts(void) {
    static char out[CASE_DUMP_CAP];
    size_t used = 0u;
    NkUiConfig config;
    NkUi *ui;
    NkUiTextBackend text = screen_test_text_backend();
    int track_x0 = 0, track_x1 = 0, fill_x0 = 0, fill_x1 = 0;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create the build-card UI");
    if (ui == NULL) return;
    set_test_viewport(ui, 1280, 800);
    g_flow_case = FLOW_BUILD_PARTS;
    CHECK(append_case(out, sizeof(out), &used, "flow-build-parts", ui, NULL, NULL),
          "draw the build card with a part count");
    g_flow_case = FLOW_NONE;
    nk_ui_destroy(ui);
    CHECK(dump_row_x_extent(out, 0x60000040u, &track_x0, &track_x1),
          "the build card draws its progress track");
    CHECK(dump_row_x_extent(out, nk_ui_child_id(0x60000040u, 2u), &fill_x0, &fill_x1),
          "the build card fills its progress track");
    CHECK(fill_x0 == track_x0, "the compiled share starts at the track's left edge");
    {
        int expected = (track_x1 - track_x0) / 4;
        int width = fill_x1 - fill_x0;
        CHECK(width >= expected - 1 && width <= expected + 1,
              "a quarter of the parts fills a quarter of the track");
    }
    CHECK(strstr(out, "\"Compiling part 4 of 12\"") != NULL,
          "the build card names the part being compiled");
    {
        /* The note sits under the bar in the same muted colour as the elapsed time:
           body text, not a warning. */
        const char *notice = strstr(out, "60000044 TEXT ");
        const char *elapsed = strstr(out, "60000043 TEXT ");
        CHECK(notice != NULL && strstr(notice, "other apps may feel slower") != NULL,
              "the build card notes that other apps may feel slower");
        if (notice != NULL && elapsed != NULL) {
            int notice_y = 0, bar_y = 0, x0 = 0, x1 = 0;
            char notice_rgba[9] = "", elapsed_rgba[9] = "";
            CHECK(sscanf(notice + 14, "%*d %d %*d %*d %8s", &notice_y, notice_rgba) == 2 &&
                  sscanf(elapsed + 14, "%*d %*d %*d %*d %8s", elapsed_rgba) == 1,
                  "read the note's and the elapsed time's rows");
            CHECK(strcmp(notice_rgba, elapsed_rgba) == 0,
                  "the note is drawn in the muted colour");
            {
                char prefix[16];
                const char *row;
                (void)snprintf(prefix, sizeof(prefix), "%08X RECT ", 0x60000040u);
                row = strstr(out, prefix);
                CHECK(row != NULL && sscanf(row + strlen(prefix), "%d %d %d", &x0, &bar_y, &x1) == 3,
                      "read the progress track's row");
            }
            CHECK(notice_y > bar_y, "the note sits under the progress bar");
        }
    }
    used = 0u;
    out[0] = '\0';
    {
        NkUi *plain = nk_ui_create(&config);
        CHECK(plain != NULL, "create the plain build-card UI");
        if (plain != NULL) {
            set_test_viewport(plain, 1280, 800);
            g_flow_case = FLOW_BUILD;
            CHECK(append_case(out, sizeof(out), &used, "flow-build-running", plain, NULL, NULL),
                  "draw the build card without a part count");
            g_flow_case = FLOW_NONE;
            nk_ui_destroy(plain);
            CHECK(strstr(out, "60000044 TEXT ") == NULL,
                  "a build without a note draws none");
        }
    }
}

static bool build_library_golden(char *out, size_t cap, size_t *used) {
    NkUiConfig config;
    NkUi *ui;
    PlayerUiLibraryModel model;
    NkUiTextBackend text = screen_test_text_backend();
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    if (ui == NULL) return false;
    set_test_viewport(ui, 1280, 800);
    memset(&model, 0, sizeof(model));
    model.page_size = player_ui_library_page_size(1280.0f, 800.0f, &model.columns);
    model.logical_width = 1280.0f;
    model.logical_height = 800.0f;
    CHECK(model.columns == 4u && model.page_size == 8u,
          "a 1280x800 window shows two rows of four cards");
    if (!append_case(out, cap, used, "library-empty", ui, &model, NULL)) {
        nk_ui_destroy(ui);
        return false;
    }
    model.titles = &k_titles[0];
    model.title_count = 1u;
    if (!append_case(out, cap, used, "library-one", ui, &model, NULL)) {
        nk_ui_destroy(ui);
        return false;
    }
    model.titles = k_titles;
    model.title_count = sizeof(k_titles) / sizeof(k_titles[0]);
    model.page_label = "Page 1 of 2";
    if (!append_case(out, cap, used, "library-many-statuses", ui, &model, NULL) ||
        !append_case(out, cap, used, "library-many-statuses-layout", ui,
                     &model, NULL)) {
        nk_ui_destroy(ui);
        return false;
    }
    nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(0u));
    nk_ui_begin(ui, 0.0);
    (void)player_ui_screen_library(ui, &model);
    if (nk_ui_end(ui) == NULL) {
        nk_ui_destroy(ui);
        return false;
    }
    {
        NkUiEvent move;
        memset(&move, 0, sizeof(move));
        move.type = NK_UI_EV_NAV;
        move.dir = NK_UI_DIR_RIGHT;
        nk_ui_push_event(ui, &move);
    }
    if (!append_case(out, cap, used, "library-focus-moved", ui, &model, NULL)) {
        nk_ui_destroy(ui);
        return false;
    }
    CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_LIBRARY_ITEM(1u),
          "right navigation moves focus to the adjacent title card");
    {
        /* Each later press moves on from where focus is now: right, right
           again, then left back. */
        static const NkUiNavDir dirs[3] = {NK_UI_DIR_RIGHT, NK_UI_DIR_LEFT, NK_UI_DIR_LEFT};
        static const size_t expect[3] = {2u, 1u, 0u};
        for (size_t step = 0u; step < 3u; ++step) {
            NkUiEvent move;
            memset(&move, 0, sizeof(move));
            move.type = NK_UI_EV_NAV;
            move.dir = dirs[step];
            nk_ui_push_event(ui, &move);
            nk_ui_begin(ui, 0.0);
            (void)player_ui_screen_library(ui, &model);
            (void)nk_ui_end(ui);
            CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_LIBRARY_ITEM(expect[step]),
                  "repeated navigation keeps moving from the current card");
        }
        nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(1u));
    }
    /* The prompt bar in each input mode: keyboard and mouse, then a pad. */
    {
        NkUiPrefs prefs;
        memset(&prefs, 0, sizeof(prefs));
        prefs.prompt_style = NK_UI_PROMPT_KEYCAPS;
        nk_ui_set_prefs(ui, &prefs);
        if (!append_case(out, cap, used, "library-prompts-keyboard", ui, &model, NULL)) {
            nk_ui_destroy(ui);
            return false;
        }
        prefs.prompt_style = NK_UI_PROMPT_LETTERS_A_SOUTH;
        nk_ui_set_prefs(ui, &prefs);
        if (!append_case(out, cap, used, "library-prompts-controller", ui, &model, NULL)) {
            nk_ui_destroy(ui);
            return false;
        }
        prefs.prompt_style = NK_UI_PROMPT_NEUTRAL;
        nk_ui_set_prefs(ui, &prefs);
    }
    /* The demos that come with Nakagawa sit under TEST THIS PC: under the
       first-start card while there are no games, under the grid once there are. */
    nk_ui_focus(ui, 0u);
    model.titles = k_demo_only_titles;
    model.title_count = sizeof(k_demo_only_titles) / sizeof(k_demo_only_titles[0]);
    model.page_label = NULL;
    model.page_size = player_ui_library_page_size_with_demos(1280.0f, 800.0f, 2u,
                                                             &model.columns);
    if (!append_case(out, cap, used, "library-demos-no-games", ui, &model, NULL)) {
        nk_ui_destroy(ui);
        return false;
    }
    model.titles = k_demo_titles;
    model.title_count = sizeof(k_demo_titles) / sizeof(k_demo_titles[0]);
    nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(1u));
    if (!append_case(out, cap, used, "library-demos-with-games", ui, &model, NULL)) {
        nk_ui_destroy(ui);
        return false;
    }
    nk_ui_destroy(ui);
    return true;
}

/* Run one Library frame and return its dump (the draw list as text). */
static const char *library_frame_dump(NkUi *ui, const PlayerUiLibraryModel *model,
                                      PlayerUiAction *action_out) {
    static char dump[CASE_DUMP_CAP];
    PlayerUiAction action;
    nk_ui_begin(ui, 0.0);
    action = player_ui_screen_library(ui, model);
    if (action_out != NULL) *action_out = action;
    if (nk_ui_end(ui) == NULL) return "";
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    return dump;
}

static void push_nav(NkUi *ui, NkUiNavDir dir) {
    NkUiEvent move;
    memset(&move, 0, sizeof(move));
    move.type = NK_UI_EV_NAV;
    move.dir = dir;
    nk_ui_push_event(ui, &move);
}

static bool focused_on_demo(const NkUi *ui, const PlayerUiTitle *titles, size_t count) {
    for (size_t i = 0u; i < count; ++i) {
        if (titles[i].is_demo && nk_ui_focused(ui) == PLAYER_UI_ID_LIBRARY_ITEM(i)) {
            return true;
        }
    }
    return false;
}

/* TEST THIS PC: the demos that come with Nakagawa are listed apart from the
   user's games, on the first-start screen and under the grid, and a controller
   reaches them. */
static void test_library_demos(void) {
    NkUiConfig config;
    NkUi *ui;
    PlayerUiLibraryModel model;
    PlayerUiAction action;
    NkUiTextBackend text = screen_test_text_backend();
    const char *dump;
    size_t columns = 0u;
    const size_t demo_count = sizeof(k_demo_titles) / sizeof(k_demo_titles[0]);

    CHECK(player_ui_library_demo_count(k_demo_titles, demo_count) == 2u &&
              player_ui_library_demo_count(k_titles, sizeof(k_titles) / sizeof(k_titles[0])) ==
                  0u &&
              player_ui_library_demo_count(NULL, 3u) == 0u,
          "the Library counts the demos that come with Nakagawa");
    CHECK(player_ui_library_grid_position(k_demo_titles, demo_count, 1u) == 0u &&
              player_ui_library_grid_position(k_demo_titles, demo_count, 3u) == 1u &&
              player_ui_library_grid_position(k_demo_titles, demo_count, 0u) == demo_count &&
              player_ui_library_grid_position(k_demo_titles, demo_count, 2u) == demo_count &&
              player_ui_library_grid_position(k_demo_titles, demo_count, 9u) == demo_count,
          "a game's grid position skips the demos, and a demo has none");
    CHECK(player_ui_library_page_size_with_demos(1280.0f, 800.0f, 0u, &columns) == 8u &&
              columns == 4u,
          "without demos a 1280x800 window keeps two rows of four cards");
    CHECK(player_ui_library_page_size_with_demos(1280.0f, 800.0f, 2u, &columns) == 8u &&
              columns == 4u,
          "a 1280x800 window keeps two rows of four cards above TEST THIS PC");
    CHECK(player_ui_library_page_size_with_demos(1280.0f, 720.0f, 0u, &columns) == 8u &&
              player_ui_library_page_size_with_demos(1280.0f, 720.0f, 2u, &columns) == 4u &&
              columns == 4u,
          "at 1280x720, TEST THIS PC takes the room of the second grid row");
    CHECK(player_ui_library_page_size_with_demos(1920.0f, 1080.0f, 2u, &columns) == 18u &&
              columns == 6u,
          "a 1920x1080 window keeps three rows of six cards above TEST THIS PC");
    CHECK(player_ui_library_page_size_with_demos(960.0f, 540.0f, 2u, &columns) == 3u &&
              columns == 3u,
          "the 960x540 minimum keeps one row of three cards above TEST THIS PC");

    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create a UI for the TEST THIS PC section");
    if (ui == NULL) return;
    set_test_viewport(ui, 1280, 800);
    memset(&model, 0, sizeof(model));
    model.logical_width = 1280.0f;
    model.logical_height = 800.0f;

    /* A first start with only the demos: the setup wizard is still offered and
       is still the default focus, and the demos are listed under it. */
    model.titles = k_demo_only_titles;
    model.title_count = sizeof(k_demo_only_titles) / sizeof(k_demo_only_titles[0]);
    model.page_size = player_ui_library_page_size_with_demos(1280.0f, 800.0f, 2u,
                                                             &model.columns);
    (void)library_frame_dump(ui, &model, NULL);
    dump = library_frame_dump(ui, &model, NULL);
    CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_LIBRARY_EMPTY_WIZARD,
          "with only demos, START SETUP WIZARD is the default focus");
    CHECK(strstr(dump, "\"Add your first game\"") != NULL &&
              strstr(dump, "\"No games yet\"") != NULL,
          "with only demos, the Library still says there are no games yet");
    CHECK(strstr(dump, "\"TEST THIS PC\"") != NULL &&
              strstr(dump, "\"Synthetic Breakout Demo\"") != NULL &&
              strstr(dump, "\"Synthetic Scene Demo\"") != NULL,
          "the first-start screen lists the demos under TEST THIS PC");
    CHECK(strstr(dump, "Play a demo that comes with Nakagawa") != NULL,
          "TEST THIS PC says what the demos are for");
    CHECK(strstr(dump, "SHOWCASE DEMO") == NULL && strstr(dump, "showcase demo") == NULL,
          "the count line no longer counts showcase demos as titles");
    push_nav(ui, NK_UI_DIR_DOWN);
    (void)library_frame_dump(ui, &model, NULL);
    CHECK(focused_on_demo(ui, model.titles, model.title_count),
          "Down from the setup wizard reaches a demo");
    {
        NkUiId first_demo = nk_ui_focused(ui);
        NkUiEvent activate;
        memset(&activate, 0, sizeof(activate));
        activate.type = NK_UI_EV_ACTIVATE;
        activate.down = true;
        nk_ui_push_event(ui, &activate);
        (void)library_frame_dump(ui, &model, &action);
        CHECK(action.kind == PLAYER_UI_ACTION_OPEN_TITLE &&
                  PLAYER_UI_ID_LIBRARY_ITEM(action.title_index) == first_demo,
              "a demo opens its own title page, where it plays as today");
    }
    push_nav(ui, NK_UI_DIR_UP);
    (void)library_frame_dump(ui, &model, NULL);
    CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_LIBRARY_EMPTY_WIZARD ||
              nk_ui_focused(ui) == PLAYER_UI_ID_LIBRARY_EMPTY_ADD,
          "Up from a demo returns to the first-start card");

    /* With games: the grid holds only the games and the add card, and the
       demos follow under TEST THIS PC. */
    nk_ui_focus(ui, 0u);
    model.titles = k_demo_titles;
    model.title_count = demo_count;
    model.page_size = player_ui_library_page_size_with_demos(1280.0f, 800.0f, 2u,
                                                             &model.columns);
    (void)library_frame_dump(ui, &model, NULL);
    dump = library_frame_dump(ui, &model, NULL);
    CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_LIBRARY_ITEM(1u),
          "the first game, not a demo, is the default focus");
    CHECK(strstr(dump, "\"2 titles\"") != NULL,
          "the count names the user's games only");
    CHECK(strstr(dump, "\"Add your first game\"") == NULL &&
              strstr(dump, "\"TEST THIS PC\"") != NULL,
          "with games, the demos sit under the grid");
    push_nav(ui, NK_UI_DIR_RIGHT);
    (void)library_frame_dump(ui, &model, NULL);
    CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_LIBRARY_ITEM(3u),
          "Right from the first game moves to the next game, past the demo between them");
    push_nav(ui, NK_UI_DIR_DOWN);
    (void)library_frame_dump(ui, &model, NULL);
    CHECK(focused_on_demo(ui, model.titles, model.title_count),
          "Down from the grid reaches a demo");
    push_nav(ui, NK_UI_DIR_UP);
    (void)library_frame_dump(ui, &model, NULL);
    CHECK(!focused_on_demo(ui, model.titles, model.title_count) &&
              nk_ui_focused(ui) != 0u,
          "Up from a demo returns to the grid");
    {
        /* The demos are drawn below every card of the grid. */
        const NkUiDrawList *draw;
        const NkUiDrawCmd *add = NULL;
        const NkUiDrawCmd *demo = NULL;
        nk_ui_begin(ui, 0.0);
        (void)player_ui_screen_library(ui, &model);
        draw = nk_ui_end(ui);
        for (uint32_t i = 0u; draw != NULL && i < draw->ncmd; ++i) {
            const NkUiDrawCmd *cmd = &draw->cmd[i];
            if (cmd->kind != NK_UI_DRAW_RECTANGLE) continue;
            if (add == NULL && cmd->element_id == PLAYER_UI_ID_LIBRARY_ADD) add = cmd;
            if (demo == NULL && cmd->element_id == PLAYER_UI_ID_LIBRARY_ITEM(0u)) demo = cmd;
        }
        CHECK(add != NULL && demo != NULL, "the add card and a demo are both drawn");
        if (add != NULL && demo != NULL) {
            CHECK(demo->bounds[1] > add->bounds[3],
                  "TEST THIS PC sits below the grid of games");
            CHECK(demo->bounds[3] <= 800 - (int)NK_UI_PROMPT_BAR_HEIGHT,
                  "TEST THIS PC stays above the prompt bar at 1280x800");
        }
    }
    {
        /* At the 960x540 minimum the first-start card leaves out its icon, so it
           and TEST THIS PC both fit above the prompt bar. */
        static const struct { int h; bool icon; } sizes[] = {{540, false}, {720, true}};
        for (size_t s = 0u; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
            const NkUiDrawList *draw = NULL;
            int demo_bottom = -1;
            int card_bottom = -1;
            bool icon = false;
            set_test_viewport(ui, sizes[s].h == 540 ? 960 : 1280, sizes[s].h);
            nk_ui_focus(ui, 0u);
            model.titles = k_demo_only_titles;
            model.title_count = sizeof(k_demo_only_titles) / sizeof(k_demo_only_titles[0]);
            model.logical_width = sizes[s].h == 540 ? 960.0f : 1280.0f;
            model.logical_height = (float)sizes[s].h;
            model.page_size = player_ui_library_page_size_with_demos(
                model.logical_width, model.logical_height, 2u, &model.columns);
            for (int frame = 0; frame < 2; ++frame) {
                nk_ui_begin(ui, 0.0);
                (void)player_ui_screen_library(ui, &model);
                draw = nk_ui_end(ui);
            }
            for (uint32_t i = 0u; draw != NULL && i < draw->ncmd; ++i) {
                const NkUiDrawCmd *cmd = &draw->cmd[i];
                if (cmd->element_id == 0x10000060u) icon = true;
                if (cmd->element_id == 0x10000005u && cmd->bounds[3] > card_bottom) {
                    card_bottom = cmd->bounds[3];
                }
                if ((cmd->element_id == PLAYER_UI_ID_LIBRARY_ITEM(0u) ||
                     cmd->element_id == PLAYER_UI_ID_LIBRARY_ITEM(1u)) &&
                    cmd->bounds[3] > demo_bottom) {
                    demo_bottom = cmd->bounds[3];
                }
            }
            CHECK(icon == sizes[s].icon,
                  "the first-start card keeps its icon only when there is room for it");
            CHECK(card_bottom > 0 && demo_bottom > card_bottom &&
                      demo_bottom <= sizes[s].h - (int)NK_UI_PROMPT_BAR_HEIGHT,
                  "the first-start card and TEST THIS PC fit above the prompt bar");
        }
    }
    nk_ui_destroy(ui);
}

static bool build_title_golden(char *out, size_t cap, size_t *used) {
    NkUiConfig config;
    NkUi *ui;
    PlayerUiTitle fixture = k_titles[0];
    PlayerUiTitleModel model;
    NkUiTextBackend text = screen_test_text_backend();
    memset(&model, 0, sizeof(model));
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    if (ui == NULL) return false;
    set_test_viewport(ui, 1280, 800);
    model.title = &fixture;
    model.title_index = 0u;
    for (size_t i = 0u;
         i < sizeof(k_compatibility_cases) / sizeof(k_compatibility_cases[0]);
         ++i) {
        char name[96];
        fixture.compatibility_tier = k_compatibility_cases[i].tier;
        fixture.compatibility_expect = k_compatibility_cases[i].expect;
        fixture.compatibility_known_issues = k_compatibility_cases[i].known_issues;
        fixture.compatibility_category = k_compatibility_cases[i].category;
        snprintf(name, sizeof(name), "title-compatibility-%zu", i);
        if (!append_case(out, cap, used, name, ui, NULL, &model)) {
            nk_ui_destroy(ui);
            return false;
        }
    }
    fixture = k_titles[0];
    {
        /* The package card while a game starts and while it runs (#1297, #1298). */
        static const struct { PlayerUiPackageState state; const char *name; } live[] = {
            {PLAYER_UI_PACKAGE_STARTING, "title-game-starting"},
            {PLAYER_UI_PACKAGE_RUNNING, "title-game-running"}
        };
        for (size_t i = 0u; i < sizeof(live) / sizeof(live[0]); ++i) {
            PlayerUiTitle running = k_titles[0];
            PlayerUiTitleModel running_model = model;
            running.package_state = live[i].state;
            running.primary_action_label = "STOP GAME";
            running.primary_action = PLAYER_UI_PRIMARY_STOP_GAME;
            running_model.title = &running;
            if (!append_case(out, cap, used, live[i].name, ui, NULL, &running_model)) {
                nk_ui_destroy(ui);
                return false;
            }
        }
    }
    nk_ui_focus(ui, PLAYER_UI_ID_TITLE_SAVE_FOLDER);
    if (!append_case(out, cap, used, "title-save-folder-focused", ui,
                     NULL, &model)) {
        nk_ui_destroy(ui);
        return false;
    }
    CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_TITLE_SAVE_FOLDER,
          "the save-folder action can keep focus");
    {
        static const struct { FlowCase which; const char *name; } flows[] = {
            {FLOW_BUILD, "flow-build-running"}, {FLOW_BUILT, "flow-build-written"},
            {FLOW_CONSENT, "flow-prereq-consent"}, {FLOW_PROGRESS, "flow-prereq-progress"},
            {FLOW_ERROR, "flow-error"},
            {FLOW_WIZ_WELCOME, "wizard-welcome"}, {FLOW_WIZ_CHOOSE, "wizard-choose"},
            {FLOW_WIZ_CHOSEN, "wizard-chosen"}, {FLOW_WIZ_COPYING, "wizard-copying"},
            {FLOW_WIZ_VERIFY, "wizard-verify"},
            {FLOW_WIZ_EXPERIMENTAL, "wizard-experimental"},
            {FLOW_WIZ_FONTS, "wizard-fonts"}, {FLOW_WIZ_READY, "wizard-ready"},
            {FLOW_WIZ_STAGED, "wizard-staged"}, {FLOW_WIZ_OPTIONS, "wizard-options"}
        };
        for (size_t i = 0u; i < sizeof(flows) / sizeof(flows[0]); ++i) {
            g_flow_case = flows[i].which;
            if (!append_case(out, cap, used, flows[i].name, ui, NULL, &model)) {
                g_flow_case = FLOW_NONE;
                nk_ui_destroy(ui);
                return false;
            }
        }
        g_flow_case = FLOW_NONE;
    }
    {
        /* The About page, with a fixed build date (SOURCE_DATE_EPOCH-style). */
        char line[64];
        PlayerUiAboutModel about;
        player_ui_version_line(NK_PLAYER_VERSION, 1792713600LL, line, sizeof(line));
        memset(&about, 0, sizeof(about));
        about.version_line = line;
        about.build_commit = "0123abc";
        about.logical_width = 1280.0f;
        about.logical_height = 800.0f;
        g_about_case = &about;
        nk_ui_focus(ui, PLAYER_UI_ID_ABOUT_NOTICES);
        if (!append_case(out, cap, used, "about", ui, NULL, NULL)) {
            nk_ui_destroy(ui);
            return false;
        }
        CHECK(strstr(out, "\"v" NK_PLAYER_VERSION " (10-23-2026)\"") != NULL,
              "the About page draws the version line exactly");
        g_about_case = NULL;
    }
    nk_ui_destroy(ui);
    return true;
}

/* The version line: one version, the build date as MM-DD-YYYY in UTC from the
   build epoch (SOURCE_DATE_EPOCH or the commit time), "dev build" without one. */
static void test_version_line(void) {
    char out[64];
    player_ui_version_line(NK_PLAYER_VERSION, 1792713600LL, out, sizeof(out));
    CHECK(strcmp(out, "v" NK_PLAYER_VERSION " (10-23-2026)") == 0, "the owner's example line");
    player_ui_version_line(NK_PLAYER_VERSION, 1792713600LL + 86399LL, out, sizeof(out));
    CHECK(strcmp(out, "v" NK_PLAYER_VERSION " (10-23-2026)") == 0, "the last second of the day is the same date");
    player_ui_version_line(NK_PLAYER_VERSION, 1792713600LL + 86400LL, out, sizeof(out));
    CHECK(strcmp(out, "v" NK_PLAYER_VERSION " (10-24-2026)") == 0, "midnight UTC starts the next date");
    player_ui_version_line(NK_PLAYER_VERSION, 951782400LL, out, sizeof(out));
    CHECK(strcmp(out, "v" NK_PLAYER_VERSION " (02-29-2000)") == 0, "a leap day");
    player_ui_version_line(NK_PLAYER_VERSION, 0LL, out, sizeof(out));
    CHECK(strcmp(out, "v" NK_PLAYER_VERSION " (01-01-1970)") == 0, "the epoch itself");
    player_ui_version_line(NK_PLAYER_VERSION, -1LL, out, sizeof(out));
    CHECK(strcmp(out, "v" NK_PLAYER_VERSION " (dev build)") == 0, "no build epoch says dev build");
    CHECK(!player_ui_build_date(1792713600LL, out, 8u), "a short buffer is refused");
}

static bool write_golden(const char *path, const char *contents, size_t size) {
    FILE *file = fopen(path, "wb");
    bool ok;
    if (file == NULL) return false;
    ok = fwrite(contents, 1u, size, file) == size;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool read_golden(const char *path, char *out, size_t cap,
                        size_t *out_size) {
    FILE *file = fopen(path, "rb");
    size_t size;
    if (file == NULL) return false;
    size = fread(out, 1u, cap, file);
    if (ferror(file) || !feof(file)) {
        fclose(file);
        return false;
    }
    if (fclose(file) != 0) return false;
    *out_size = size;
    return true;
}

static void compare_golden(const char *path, const char *actual,
                           size_t actual_size) {
    static char expected[DUMP_CAP];
    size_t expected_size = 0u;
    if (!read_golden(path, expected, sizeof(expected), &expected_size)) {
        fprintf(stderr, "FAIL: could not read screen golden %s\n", path);
        ++failures;
        return;
    }
    if (expected_size != actual_size ||
        memcmp(expected, actual, expected_size < actual_size
            ? expected_size : actual_size) != 0) {
        size_t mismatch = 0u;
        size_t limit = expected_size < actual_size ? expected_size : actual_size;
        while (mismatch < limit && expected[mismatch] == actual[mismatch]) {
            ++mismatch;
        }
        fprintf(stderr, "FAIL: %s dump changed at byte %zu (expected=%zu actual=%zu)\n",
                path, mismatch, expected_size, actual_size);
        ++failures;
    }
}

static void test_router(void) {
    NkUiRouter *router = nk_ui_router_create();
    NkUiEvent back;
    CHECK(router != NULL, "router allocates");
    if (router == NULL) return;
    CHECK(nk_ui_router_current(router) == PLAYER_UI_ROUTE_LIBRARY,
          "router starts at Library");
    CHECK(nk_ui_router_depth(router) == 1u, "router has one root screen");
    nk_ui_router_set_focus(router, PLAYER_UI_ID_LIBRARY_ITEM(3u));
    CHECK(nk_ui_router_push(router, PLAYER_UI_ROUTE_TITLE),
          "router pushes Title detail");
    CHECK(nk_ui_router_current(router) == PLAYER_UI_ROUTE_TITLE,
          "pushed screen is current");
    CHECK(nk_ui_router_depth(router) == 2u, "push keeps the Library underneath");
    nk_ui_router_set_focus(router, PLAYER_UI_ID_TITLE_SAVE_FOLDER);
    memset(&back, 0, sizeof(back));
    back.type = NK_UI_EV_BACK;
    CHECK(nk_ui_router_handle_event(router, &back),
          "Back event is consumed by the router");
    CHECK(nk_ui_router_current(router) == PLAYER_UI_ROUTE_LIBRARY,
          "Back pops to Library");
    CHECK(nk_ui_router_take_focus_restore(router) ==
          PLAYER_UI_ID_LIBRARY_ITEM(3u), "pop restores the saved Library focus");
    CHECK(nk_ui_router_take_focus_restore(router) == 0u,
          "focus restore is consumed once");
    CHECK(nk_ui_router_replace(router, PLAYER_UI_ROUTE_TITLE),
          "router replaces the root screen");
    CHECK(nk_ui_router_depth(router) == 1u &&
          nk_ui_router_current(router) == PLAYER_UI_ROUTE_TITLE,
          "replace discards the old stack");
    CHECK(nk_ui_router_handle_event(router, &back),
          "root Back is consumed without changing route");
    CHECK(nk_ui_router_current(router) == PLAYER_UI_ROUTE_TITLE,
          "Back at root leaves the route in place");
    /* #1281: About is a route the router accepts, opened over the screen it
       was chosen from; Back returns there with the ABOUT tab focused. */
    nk_ui_router_set_focus(router, PLAYER_UI_ID_TAB_ABOUT);
    CHECK(nk_ui_router_push(router, PLAYER_UI_ROUTE_ABOUT),
          "router pushes About");
    CHECK(nk_ui_router_current(router) == PLAYER_UI_ROUTE_ABOUT &&
          nk_ui_router_depth(router) == 2u, "About sits over the screen it came from");
    CHECK(nk_ui_router_handle_event(router, &back) &&
          nk_ui_router_current(router) == PLAYER_UI_ROUTE_TITLE,
          "Back from About returns to that screen");
    CHECK(nk_ui_router_take_focus_restore(router) == PLAYER_UI_ID_TAB_ABOUT,
          "Back from About restores the ABOUT tab focus");
    CHECK(nk_ui_router_replace(router, PLAYER_UI_ROUTE_ABOUT) &&
          nk_ui_router_current(router) == PLAYER_UI_ROUTE_ABOUT,
          "router replaces the root screen with About");
    CHECK(!nk_ui_router_replace(router, (PlayerUiRoute)0) &&
          !nk_ui_router_push(router, (PlayerUiRoute)0) &&
          !nk_ui_router_replace(router, (PlayerUiRoute)(PLAYER_UI_ROUTE_REMOVE + 1)) &&
          !nk_ui_router_push(router, (PlayerUiRoute)(PLAYER_UI_ROUTE_REMOVE + 1)) &&
          !nk_ui_router_push(router, (PlayerUiRoute)(PLAYER_UI_ROUTE_STOP + 1)),
          "router refuses routes it does not draw");
    CHECK(nk_ui_router_current(router) == PLAYER_UI_ROUTE_ABOUT &&
          nk_ui_router_depth(router) == 1u, "a refused route leaves the stack alone");
    nk_ui_router_destroy(router);
}

static PlayerUiAction activate_library_button(NkUiId id,
                                               const PlayerUiLibraryModel *model) {
    NkUiConfig config;
    NkUiEvent activate;
    NkUi *ui;
    PlayerUiAction result = {PLAYER_UI_ACTION_NONE,
                             PLAYER_UI_PRIMARY_NONE, 0u};
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    ui = nk_ui_create(&config);
    if (ui == NULL) return result;
    set_test_viewport(ui, 1280, 800);
    nk_ui_focus(ui, id);
    memset(&activate, 0, sizeof(activate));
    activate.type = NK_UI_EV_ACTIVATE;
    activate.down = true;
    nk_ui_push_event(ui, &activate);
    nk_ui_begin(ui, 0.0);
    result = player_ui_screen_library(ui, model);
    (void)nk_ui_end(ui);
    nk_ui_destroy(ui);
    return result;
}

static PlayerUiAction activate_title_button(NkUiId id,
                                             const PlayerUiTitleModel *model) {
    NkUiConfig config;
    NkUiEvent activate;
    NkUi *ui;
    PlayerUiAction result = {PLAYER_UI_ACTION_NONE,
                             PLAYER_UI_PRIMARY_NONE, 0u};
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    ui = nk_ui_create(&config);
    if (ui == NULL) return result;
    set_test_viewport(ui, 1280, 800);
    nk_ui_focus(ui, id);
    memset(&activate, 0, sizeof(activate));
    activate.type = NK_UI_EV_ACTIVATE;
    activate.down = true;
    nk_ui_push_event(ui, &activate);
    nk_ui_begin(ui, 0.0);
    result = player_ui_screen_title(ui, model);
    (void)nk_ui_end(ui);
    nk_ui_destroy(ui);
    return result;
}

static PlayerUiAction activate_flow_button(NkUiId id, FlowCase which) {
    NkUiConfig config;
    NkUiEvent activate;
    NkUi *ui;
    PlayerUiAction result = {PLAYER_UI_ACTION_NONE,
                             PLAYER_UI_PRIMARY_NONE, 0u};
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    ui = nk_ui_create(&config);
    if (ui == NULL) return result;
    set_test_viewport(ui, 1280, 800);
    nk_ui_focus(ui, id);
    memset(&activate, 0, sizeof(activate));
    activate.type = NK_UI_EV_ACTIVATE;
    activate.down = true;
    nk_ui_push_event(ui, &activate);
    nk_ui_begin(ui, 0.0);
    result = draw_flow_case(ui, which);
    (void)nk_ui_end(ui);
    nk_ui_destroy(ui);
    return result;
}

static bool wizard_op_is(PlayerUiAction action, PlayerUiWizardOp op) {
    return action.kind == PLAYER_UI_ACTION_WIZARD && (PlayerUiWizardOp)action.primary == op;
}

/* Every wizard control returns the op main.c runs with the existing handlers. */
static void test_wizard_actions(void) {
    static const struct { FlowCase which; NkUiId id; PlayerUiWizardOp op; const char *what; }
        cases[] = {
        {FLOW_WIZ_WELCOME, PLAYER_UI_ID_WIZARD_PRIMARY, PLAYER_UI_WIZARD_NEXT,
         "Get started moves to the next step"},
        {FLOW_WIZ_WELCOME, PLAYER_UI_ID_WIZARD_CANCEL, PLAYER_UI_WIZARD_CANCEL,
         "Skip to library leaves the wizard"},
        {FLOW_WIZ_CHOOSE, PLAYER_UI_ID_WIZARD_PRIMARY, PLAYER_UI_WIZARD_CHOOSE_ISO,
         "Choose a disc image opens the file picker"},
        {FLOW_WIZ_CHOOSE, PLAYER_UI_ID_WIZARD_BACK, PLAYER_UI_WIZARD_BACK,
         "Back on Select game goes back"},
        {FLOW_WIZ_CHOSEN, PLAYER_UI_ID_WIZARD_PRIMARY, PLAYER_UI_WIZARD_NEXT,
         "Verify this disc moves on"},
        {FLOW_WIZ_CHOSEN, PLAYER_UI_ID_WIZARD_SECOND, PLAYER_UI_WIZARD_CHOOSE_DIFFERENT,
         "a chosen disc can be swapped"},
        {FLOW_WIZ_COPYING, PLAYER_UI_ID_WIZARD_CANCEL, PLAYER_UI_WIZARD_CANCEL,
         "Cancel copying cancels the staging"},
        {FLOW_WIZ_VERIFY, PLAYER_UI_ID_WIZARD_PRIMARY, PLAYER_UI_WIZARD_NEXT,
         "Continue after staging moves on"},
        {FLOW_WIZ_EXPERIMENTAL, PLAYER_UI_ID_WIZARD_PRIMARY, PLAYER_UI_WIZARD_ADD_EXPERIMENTAL,
         "an experimental disc is added to the library"},
        {FLOW_WIZ_BLOCKED, PLAYER_UI_ID_WIZARD_PRIMARY, PLAYER_UI_WIZARD_CHOOSE_DIFFERENT,
         "a disc this version cannot use is not offered to the library (#1280)"},
        {FLOW_WIZ_FONTS, PLAYER_UI_ID_WIZARD_PRIMARY, PLAYER_UI_WIZARD_ACCEPT_FONTS,
         "Accept and continue confirms the fonts step"},
        {FLOW_WIZ_FONTS, PLAYER_UI_ID_WIZARD_SECOND, PLAYER_UI_WIZARD_CYCLE_RESOLUTION,
         "Change resolution cycles the scale"},
        {FLOW_WIZ_FONTS, PLAYER_UI_ID_WIZARD_IMPORT_FONTS, PLAYER_UI_WIZARD_IMPORT_FONTS,
         "Import fonts folder opens the folder picker"},
        {FLOW_WIZ_FONTS, PLAYER_UI_ID_WIZARD_REMOVE_FONTS, PLAYER_UI_WIZARD_REMOVE_FONTS,
         "Remove imported fonts removes them"},
        {FLOW_WIZ_READY, PLAYER_UI_ID_WIZARD_PRIMARY, PLAYER_UI_WIZARD_FINISH,
         "Play now finishes the wizard"},
        {FLOW_WIZ_STAGED, PLAYER_UI_ID_WIZARD_SECOND, PLAYER_UI_WIZARD_GO_TO_LIBRARY,
         "Go to library saves and leaves"},
        {FLOW_WIZ_OPTIONS, PLAYER_UI_ID_WIZARD_PRIMARY, PLAYER_UI_WIZARD_NEXT,
         "Continue leaves the options step as they are"},
        {FLOW_WIZ_OPTIONS, PLAYER_UI_ID_WIZARD_SECOND, PLAYER_UI_WIZARD_SKIP_OPTIONS,
         "Skip turns both options off"},
        {FLOW_WIZ_OPTIONS, PLAYER_UI_ID_WIZARD_ONLINE, PLAYER_UI_WIZARD_TOGGLE_ONLINE,
         "the online switch toggles online mode"},
        {FLOW_WIZ_OPTIONS, PLAYER_UI_ID_WIZARD_DEVELOPER, PLAYER_UI_WIZARD_TOGGLE_DEVELOPER,
         "the developer switch toggles developer mode"}
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(wizard_op_is(activate_flow_button(cases[i].id, cases[i].which), cases[i].op),
              cases[i].what);
    }
}

/* Glyphs as wide as the UI's mono face (about 0.62 em), so line breaks land
   where they do on screen. */
static NkUiTextSize screen_test_wide_measure(void *user, int font, int px, const char *text,
                                             size_t length) {
    NkUiTextSize measured = {0.0f, 0.0f, 0.0f};
    size_t codepoints = 0u;
    (void)user;
    (void)font;
    if (text == NULL || px <= 0) return measured;
    for (size_t i = 0u; i < length; ++i) {
        if (((unsigned char)text[i] & 0xC0u) != 0x80u) ++codepoints;
    }
    measured.width = (float)codepoints * (float)px * 0.62f;
    measured.height = (float)px;
    measured.baseline = (float)px * 0.8f;
    return measured;
}

/* The error card keeps a CLI_NOT_FOUND-sized details text whole in the
   960 x 540 minimum window: the fix it ends with must not be cut off. It was,
   at six lines of a 700 px card. */
/* The text a flow case lays out, for checks on what the card says. */
static bool dump_flow_case(FlowCase which, char *dump, size_t cap) {
    NkUiConfig config;
    NkUiTextBackend text = screen_test_text_backend();
    NkUi *ui;
    size_t used = 0u;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    if (ui == NULL) return false;
    set_test_viewport(ui, 1280, 800);
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 0.0);
        (void)draw_flow_case(ui, which);
        (void)nk_ui_end(ui);
    }
    used = nk_ui_dump(ui, dump, cap);
    nk_ui_destroy(ui);
    return used > 0u;
}

/* #1282, #1280, #1278: the cards' actions and words. */
static void test_plain_cards(void) {
    static char dump[CASE_DUMP_CAP];
    CHECK(activate_flow_button(PLAYER_UI_ID_ERROR_RETRY, FLOW_ERROR_STOP).kind ==
              PLAYER_UI_ACTION_ERROR_RETRY,
          "Restart game on a stopped game's card asks for the relaunch");
    CHECK(activate_flow_button(PLAYER_UI_ID_ERROR_COPY, FLOW_ERROR_STOP).kind ==
              PLAYER_UI_ACTION_ERROR_COPY,
          "Copy details asks for the report on the clipboard");
    CHECK(activate_flow_button(PLAYER_UI_ID_ERROR_RECOVER, FLOW_ERROR_STOP).kind ==
              PLAYER_UI_ACTION_ERROR_RECOVER,
          "Return to Library still leaves the card");
    CHECK(activate_flow_button(PLAYER_UI_ID_ERROR_RETRY, FLOW_ERROR).kind ==
              PLAYER_UI_ACTION_NONE,
          "a card without a retry action draws no retry button");
    CHECK(dump_flow_case(FLOW_ERROR_STOP, dump, sizeof(dump)), "lay out the stopped game's card");
    /* Buttons are drawn in capitals. */
    CHECK(strstr(dump, "\"RESTART GAME\"") != NULL && strstr(dump, "\"COPY DETAILS\"") != NULL,
          "the stopped game's card offers Restart game and Copy details");
    CHECK(strstr(dump, "CONTINUE") == NULL && strstr(dump, "ontinue") == NULL,
          "the stopped game's card never offers to continue");
    CHECK(strstr(dump, "Nothing is sent anywhere") != NULL,
          "the card says the report stays on this computer");
    CHECK(dump_flow_case(FLOW_WIZ_BLOCKED, dump, sizeof(dump)) &&
              strstr(dump, "ADD TO LIBRARY") == NULL &&
              strstr(dump, "EBOOT.BIN is encrypted") != NULL &&
              strstr(dump, "can't be played") != NULL,
          "a disc this version cannot use says why, with no Add to library");
    CHECK(dump_flow_case(FLOW_WIZ_BAD_DISC, dump, sizeof(dump)) &&
              strstr(dump, "This file isn't a PSP disc image.") != NULL &&
              strstr(dump, "\"CHOOSE A DISC IMAGE\"") != NULL,
          "a rejected file keeps its plain reason and next step visible in the wizard");
    CHECK(dump_flow_case(FLOW_WIZ_COPY_FAILED, dump, sizeof(dump)) &&
              strstr(dump, "Synthetic reason") != NULL &&
              strstr(dump, "listed in the native title catalog") == NULL,
          "a failed copy shows its reason in place of the catalog note");
}

static bool disc_op_is(PlayerUiAction action, PlayerUiDiscOp op) {
    return action.kind == PLAYER_UI_ACTION_DISC && (PlayerUiDiscOp)action.primary == op;
}

static bool settings_op_is(PlayerUiAction action, PlayerUiSettingsOp op) {
    return action.kind == PLAYER_UI_ACTION_SETTINGS && (PlayerUiSettingsOp)action.primary == op;
}

/* The launcher's viewport for a window of width x height pixels at 100%: its
   screens keep at least the 1280 x 720 design layout (player_ui_nk.c). */
static NkUiViewport launcher_viewport(int width, int height, float display_scale) {
    NkUiViewport viewport = {width, height, display_scale, 0, 100, 1280.0f, 720.0f};
    return viewport;
}

/* Lays a flow case out in a launcher window of width x height pixels with
   glyphs as wide as the mono face, and says whether every box and text run is
   inside the window and no text is painted over. */
static bool flow_case_fits(FlowCase which, int width, int height, char *dump, size_t cap) {
    NkUiConfig config;
    NkUiTextBackend text = screen_test_text_backend();
    NkUiViewport viewport = launcher_viewport(width, height, 1.0f);
    NkUiScale scale = nk_ui_scale_for_viewport(&viewport);
    const NkUiDrawList *draw = NULL;
    bool inside = true;
    NkUi *ui;
    text.measure = screen_test_wide_measure;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    if (ui == NULL) return false;
    nk_ui_set_viewport(ui, &viewport);
    g_flow_width = scale.logical_w;
    g_flow_height = scale.logical_h;
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 0.0);
        (void)draw_flow_case(ui, which);
        draw = nk_ui_end(ui);
    }
    if (draw == NULL) inside = false;
    for (uint32_t i = 0u; draw != NULL && i < draw->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &draw->cmd[i];
        /* Every box and run is on screen: a page that runs off the bottom
           pushes the prompt bar (or its own buttons) out of the window. */
        if (cmd->kind != NK_UI_DRAW_SCISSOR_START && cmd->kind != NK_UI_DRAW_SCISSOR_END &&
            (cmd->bounds[0] < 0 || cmd->bounds[1] < 0 || cmd->bounds[2] > width ||
             cmd->bounds[3] > height)) {
            inside = false;
        }
        if (cmd->kind != NK_UI_DRAW_TEXT) continue;
        /* A squeezed layout draws text, or a box's fill, over earlier text. */
        for (uint32_t j = i + 1u; j < draw->ncmd; ++j) {
            const NkUiDrawCmd *other = &draw->cmd[j];
            if ((other->kind == NK_UI_DRAW_TEXT || other->kind == NK_UI_DRAW_RECTANGLE) &&
                cmd->bounds[0] < other->bounds[2] && other->bounds[0] < cmd->bounds[2] &&
                cmd->bounds[1] < other->bounds[3] && other->bounds[1] < cmd->bounds[3]) {
                inside = false;
            }
        }
    }
    if (dump != NULL) (void)nk_ui_dump(ui, dump, cap);
    g_flow_width = 1280.0f;
    g_flow_height = 800.0f;
    nk_ui_destroy(ui);
    return inside;
}

/* #1302: the card a disc gets when it is added. Every check row has a name and
   a sentence; the card never reads "Not supported" beside Add to library; a disc
   this version cannot use is not offered to the library. */
static void test_disc_cards(void) {
    static char dump[CASE_DUMP_CAP];
    static const struct { FlowCase which; NkUiId id; PlayerUiDiscOp op; const char *what; }
        cases[] = {
        {FLOW_DISC_SUPPORTED, PLAYER_UI_ID_DISC_PRIMARY, PLAYER_UI_DISC_ADD,
         "a supported disc is added to the library"},
        {FLOW_DISC_SUPPORTED, PLAYER_UI_ID_DISC_SECOND, PLAYER_UI_DISC_BACK,
         "Back leaves a supported disc's card"},
        {FLOW_DISC_UNTESTED, PLAYER_UI_ID_DISC_PRIMARY, PLAYER_UI_DISC_ADD,
         "an untested disc can be added and tried"},
        {FLOW_DISC_FONTS, PLAYER_UI_ID_DISC_PRIMARY, PLAYER_UI_DISC_ADD,
         "a disc is still added when PSP fonts are missing"},
        {FLOW_DISC_BLOCKED, PLAYER_UI_ID_DISC_PRIMARY, PLAYER_UI_DISC_CHOOSE_ANOTHER,
         "a disc this version cannot use offers another disc"},
        {FLOW_DISC_BLOCKED, PLAYER_UI_ID_DISC_SECOND, PLAYER_UI_DISC_BACK,
         "Back leaves a blocked disc's card"},
        {FLOW_DISC_UNSUPPORTED, PLAYER_UI_ID_DISC_PRIMARY, PLAYER_UI_DISC_BACK,
         "Back to library leaves a disc that is not on the list"},
        {FLOW_DISC_UNSUPPORTED, PLAYER_UI_ID_DISC_SECOND, PLAYER_UI_DISC_CHOOSE_ANOTHER,
         "a disc that is not on the list offers another disc"},
        {FLOW_DISC_CHECKING, PLAYER_UI_ID_DISC_PRIMARY, PLAYER_UI_DISC_BACK,
         "Cancel stops checking a disc"}
    };
    static const FlowCase offered[] = {FLOW_DISC_SUPPORTED, FLOW_DISC_UNTESTED, FLOW_DISC_FONTS};
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(disc_op_is(activate_flow_button(cases[i].id, cases[i].which), cases[i].op),
              cases[i].what);
    }
    for (size_t i = 0u; i < sizeof(offered) / sizeof(offered[0]); ++i) {
        const PlayerUiCheckRow *rows = offered[i] == FLOW_DISC_FONTS ? k_disc_rows_fonts
                                                                     : k_disc_rows_ready;
        CHECK(dump_flow_case(offered[i], dump, sizeof(dump)), "lay out a disc's card");
        CHECK(strstr(dump, "\"ADD TO LIBRARY\"") != NULL, "the card offers Add to library");
        CHECK(strstr(dump, "\"Synthetic Showcase Scene\"") != NULL, "the card names the game");
        CHECK(strstr(dump, "ot supported") == NULL && strstr(dump, "OT SUPPORTED") == NULL,
              "a disc that can be added never reads Not supported");
        CHECK(strstr(dump, " .\"") == NULL, "no sentence ends with an empty name");
        for (size_t row = 0u; row < sizeof(k_disc_rows_ready) / sizeof(k_disc_rows_ready[0]);
             ++row) {
            char quoted[192];
            snprintf(quoted, sizeof(quoted), "\"%s\"", rows[row].code);
            CHECK(strstr(dump, quoted) != NULL, "every check row is named");
            /* A long sentence wraps, so its start is what the dump has whole. */
            snprintf(quoted, sizeof(quoted), "\"%.24s", rows[row].message);
            CHECK(strstr(dump, quoted) != NULL, "every check row has its sentence");
        }
    }
    CHECK(dump_flow_case(FLOW_DISC_FONTS, dump, sizeof(dump)) &&
              strstr(dump, "\"SOME MISSING\"") != NULL &&
              strstr(dump, "import your own fonts") != NULL && strstr(dump, "jpn0") == NULL,
          "missing PSP fonts are a plain warning, with no font file names");
    CHECK(dump_flow_case(FLOW_DISC_UNTESTED, dump, sizeof(dump)) &&
              strstr(dump, "Not tested with this game yet") != NULL,
          "an untested disc says so in plain words");
    CHECK(dump_flow_case(FLOW_DISC_BLOCKED, dump, sizeof(dump)) &&
              strstr(dump, "ADD TO LIBRARY") == NULL &&
              strstr(dump, "couldn't decrypt the game's program") != NULL &&
              strstr(dump, "can't be played") != NULL,
          "a disc this version cannot use says why, with no Add to library");
    CHECK(dump_flow_case(FLOW_DISC_UNSUPPORTED, dump, sizeof(dump)) &&
              strstr(dump, "\"PSP game\"") != NULL && strstr(dump, "ADD TO LIBRARY") == NULL &&
              strstr(dump, "Synthetic profile refusal") != NULL,
          "a disc with no name and a refused profile still reads in words");
    {
        static const FlowCase fit[] = {FLOW_DISC_SUPPORTED, FLOW_DISC_FONTS, FLOW_DISC_BLOCKED,
                                       FLOW_DISC_UNSUPPORTED, FLOW_DISC_CHECKING};
        for (size_t i = 0u; i < sizeof(fit) / sizeof(fit[0]); ++i) {
            CHECK(flow_case_fits(fit[i], 960, 540, dump, sizeof(dump)),
                  "the game-found card fits the 960 x 540 minimum window");
        }
        CHECK(flow_case_fits(FLOW_DISC_FONTS, 960, 540, dump, sizeof(dump)) &&
                  strstr(dump, "\"ADD TO LIBRARY\"") != NULL && strstr(dump, "\"Sound\"") != NULL,
              "every row and Add to library are on screen at 960 x 540");
        CHECK(flow_case_fits(FLOW_DISC_BLOCKED, 960, 540, dump, sizeof(dump)) &&
                  strstr(dump, "open the disc again.\"") != NULL,
              "a blocked disc's reason, with its folder, is whole at 960 x 540");
        CHECK(flow_case_fits(FLOW_DISC_BLOCKED, 1280, 720, dump, sizeof(dump)) &&
                  strstr(dump, "open the disc again.\"") != NULL &&
                  strstr(dump, "\"Fonts\"") != NULL,
              "a blocked disc shows its reason and checks at 1280 x 720");
    }
}

/* #1302: Settings in the next UI. Every control returns the op main.c runs with
   the existing handlers, and each page fits the minimum window. */
static void test_settings_pages(void) {
    static char dump[CASE_DUMP_CAP];
    static const struct { FlowCase which; NkUiId id; PlayerUiSettingsOp op; const char *what; }
        cases[] = {
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_RESOLUTION, PLAYER_UI_SETTINGS_CYCLE_RESOLUTION,
         "the resolution button cycles the scale"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_PACING, PLAYER_UI_SETTINGS_TOGGLE_PACING,
         "the pacing button switches the pacing"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_VSYNC, PLAYER_UI_SETTINGS_TOGGLE_VSYNC,
         "the VSync button toggles VSync"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_FULLSCREEN, PLAYER_UI_SETTINGS_TOGGLE_FULLSCREEN,
         "the fullscreen button toggles game fullscreen"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_SCALING, PLAYER_UI_SETTINGS_CYCLE_SCALING,
         "the scaling button cycles the scaling"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_VOLUME_DOWN, PLAYER_UI_SETTINGS_VOLUME_DOWN,
         "minus lowers the volume"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_VOLUME_UP, PLAYER_UI_SETTINGS_VOLUME_UP,
         "plus raises the volume"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_UI_SOUNDS, PLAYER_UI_SETTINGS_TOGGLE_UI_SOUNDS,
         "the menu sounds button toggles them"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_REDUCE_MOTION,
         PLAYER_UI_SETTINGS_TOGGLE_REDUCE_MOTION, "the reduce motion button toggles it"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_LAUNCHER_FULLSCREEN,
         PLAYER_UI_SETTINGS_TOGGLE_LAUNCHER_FULLSCREEN, "the launcher fullscreen button toggles it"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_CONTROLLER, PLAYER_UI_SETTINGS_OPEN_CONTROLLER,
         "Controller settings opens the controller screen"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_DONE, PLAYER_UI_SETTINGS_DONE,
         "Done saves and closes Settings"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_ONLINE, PLAYER_UI_SETTINGS_OPEN_ONLINE,
         "Online and developer opens its page"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_NOTICES, PLAYER_UI_SETTINGS_OPEN_NOTICES,
         "Third-party notices opens its page"},
        {FLOW_SETTINGS, PLAYER_UI_ID_SETTINGS_BUILD_CACHE, PLAYER_UI_SETTINGS_OPEN_BUILD_CACHE,
         "Build tools and cache opens its page"},
        {FLOW_SETTINGS_BUILD_CACHE, PLAYER_UI_ID_SETTINGS_SHARE_LIBRARIES,
         PLAYER_UI_SETTINGS_TOGGLE_SHARE_LIBRARIES, "the share switch toggles library sharing"},
        {FLOW_SETTINGS_BUILD_CACHE, PLAYER_UI_ID_SETTINGS_CLEAR_LIBRARIES,
         PLAYER_UI_SETTINGS_CLEAR_SHARED_LIBRARIES, "Clear shared library cache clears it"},
        {FLOW_SETTINGS_BUILD_CACHE, PLAYER_UI_ID_SETTINGS_REMOVE_TOOLS,
         PLAYER_UI_SETTINGS_OPEN_REMOVE_TOOLS, "Remove build tools asks first"},
        {FLOW_SETTINGS_BUILD_CACHE, PLAYER_UI_ID_SETTINGS_BACK, PLAYER_UI_SETTINGS_BACK,
         "Back returns to Settings from build tools and cache"},
        {FLOW_SETTINGS_ONLINE, PLAYER_UI_ID_SETTINGS_ONLINE_MODE, PLAYER_UI_SETTINGS_TOGGLE_ONLINE,
         "the online switch toggles online mode"},
        {FLOW_SETTINGS_ONLINE, PLAYER_UI_ID_SETTINGS_UPDATES, PLAYER_UI_SETTINGS_TOGGLE_UPDATES,
         "the update check switch toggles it"},
        {FLOW_SETTINGS_ONLINE, PLAYER_UI_ID_SETTINGS_NEWS, PLAYER_UI_SETTINGS_TOGGLE_NEWS,
         "the news switch toggles it"},
        {FLOW_SETTINGS_ONLINE, PLAYER_UI_ID_SETTINGS_REPORTS, PLAYER_UI_SETTINGS_TOGGLE_REPORTS,
         "the reports switch toggles it"},
        {FLOW_SETTINGS_ONLINE, PLAYER_UI_ID_SETTINGS_DEVELOPER,
         PLAYER_UI_SETTINGS_TOGGLE_DEVELOPER, "the developer switch toggles developer mode"},
        {FLOW_SETTINGS_ONLINE, PLAYER_UI_ID_SETTINGS_DEVELOPER_LOG,
         PLAYER_UI_SETTINGS_OPEN_DEVELOPER_LOG, "developer mode offers the developer log"},
        {FLOW_SETTINGS_ONLINE, PLAYER_UI_ID_SETTINGS_BACK, PLAYER_UI_SETTINGS_BACK,
         "Back returns to Settings"},
        {FLOW_SETTINGS_NOTICES, PLAYER_UI_ID_SETTINGS_LICENSE_TEXTS,
         PLAYER_UI_SETTINGS_OPEN_LICENSE_TEXTS, "Open licence texts opens the folder"},
        {FLOW_SETTINGS_NOTICES, PLAYER_UI_ID_SETTINGS_BACK, PLAYER_UI_SETTINGS_BACK,
         "Back leaves the notices"},
        {FLOW_SETTINGS_REMOVE, PLAYER_UI_ID_SETTINGS_CONFIRM_REMOVE,
         PLAYER_UI_SETTINGS_REMOVE_TOOLS, "Remove tools removes them"},
        {FLOW_SETTINGS_REMOVE, PLAYER_UI_ID_SETTINGS_BACK, PLAYER_UI_SETTINGS_KEEP_TOOLS,
         "Keep them leaves the tools alone and returns to build tools and cache"}
    };
    static const FlowCase pages[] = {FLOW_SETTINGS, FLOW_SETTINGS_ONLINE, FLOW_SETTINGS_NOTICES,
                                     FLOW_SETTINGS_REMOVE, FLOW_SETTINGS_BUILD_CACHE};
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(settings_op_is(activate_flow_button(cases[i].id, cases[i].which), cases[i].op),
              cases[i].what);
    }
    CHECK(activate_flow_button(PLAYER_UI_ID_TAB_LIBRARY, FLOW_SETTINGS).kind ==
              PLAYER_UI_ACTION_OPEN_LIBRARY,
          "the LIBRARY tab on Settings opens the library");
    CHECK(activate_flow_button(PLAYER_UI_ID_TAB_ABOUT, FLOW_SETTINGS).kind ==
              PLAYER_UI_ACTION_OPEN_ABOUT,
          "the ABOUT tab on Settings opens About");
    CHECK(dump_flow_case(FLOW_SETTINGS, dump, sizeof(dump)) &&
              strstr(dump, "\"2X (960X544)\"") != NULL && strstr(dump, "\"80%\"") != NULL &&
              strstr(dump, "saved as you make them") != NULL,
          "Settings shows each value on the button that changes it");
    CHECK(dump_flow_case(FLOW_SETTINGS_NOTICES, dump, sizeof(dump)) &&
              strstr(dump, "synthetic-compiler 14.2") != NULL &&
              strstr(dump, "GPL-3.0-or-later; see LICENSE") != NULL,
          "the notices list the downloaded tools and the project's licence");
    for (size_t i = 0u; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        CHECK(flow_case_fits(pages[i], 960, 540, dump, sizeof(dump)),
              "each Settings page fits the 960 x 540 minimum window");
    }
    CHECK(flow_case_fits(FLOW_SETTINGS, 960, 540, dump, sizeof(dump)) &&
              strstr(dump, "\"DONE\"") != NULL &&
              strstr(dump, "\"BUILD TOOLS AND CACHE\"") != NULL,
          "Done and every Settings button are on screen at 960 x 540");
    CHECK(flow_case_fits(FLOW_SETTINGS_BUILD_CACHE, 960, 540, dump, sizeof(dump)) &&
              strstr(dump, "\"SHARE LIBRARIES: ON\"") != NULL &&
              strstr(dump, "\"REMOVE BUILD TOOLS\"") != NULL &&
              strstr(dump, "Shared library cache cleared.") != NULL,
          "build tools and cache shows the switch, tool removal and the clear notice");
}

/* Walkthrough blocker: Settings > PSP fonts imports and removes the user's
   PSP fonts and shows every slot's status, with the plain hint. */
static void test_psp_fonts_page(void) {
    static char dump[CASE_DUMP_CAP];
    CHECK(activate_flow_button(PLAYER_UI_ID_SETTINGS_FONTS, FLOW_SETTINGS).kind ==
              PLAYER_UI_ACTION_SETTINGS &&
              (PlayerUiSettingsOp)activate_flow_button(PLAYER_UI_ID_SETTINGS_FONTS,
                                                       FLOW_SETTINGS).primary ==
              PLAYER_UI_SETTINGS_OPEN_FONTS,
          "Settings opens PSP fonts");
    CHECK((PlayerUiSettingsOp)activate_flow_button(PLAYER_UI_ID_SETTINGS_IMPORT_FONTS,
                                                   FLOW_SETTINGS_FONTS).primary ==
              PLAYER_UI_SETTINGS_IMPORT_FONTS,
          "Import fonts folder asks for the folder picker");
    CHECK((PlayerUiSettingsOp)activate_flow_button(PLAYER_UI_ID_SETTINGS_REMOVE_FONTS,
                                                   FLOW_SETTINGS_FONTS).primary ==
              PLAYER_UI_SETTINGS_REMOVE_FONTS,
          "Remove imported fonts removes them");
    CHECK(dump_flow_case(FLOW_SETTINGS_FONTS, dump, sizeof(dump)) &&
              strstr(dump, "Optional.") != NULL && strstr(dump, "flash0:/font") != NULL &&
              strstr(dump, "Copying the fonts from your PSP") != NULL &&
              strstr(dump, "Japanese: synthetic fallback") != NULL &&
              strstr(dump, "Korean: synthetic fallback") != NULL &&
              strstr(dump, "Latin 16: synthetic fallback") != NULL &&
              strstr(dump, "Imported 3 fonts") != NULL,
          "the page says the fonts are optional, how to copy them, and every slot's status");
}

static bool controller_op_is(PlayerUiAction action, PlayerUiControllerOp op, size_t control) {
    return action.kind == PLAYER_UI_ACTION_CONTROLLER &&
           (PlayerUiControllerOp)action.primary == op && action.title_index == control;
}

static bool developer_log_op_is(PlayerUiAction action, PlayerUiDeveloperLogOp op) {
    return action.kind == PLAYER_UI_ACTION_DEVELOPER_LOG &&
           (PlayerUiDeveloperLogOp)action.primary == op;
}

/* Controller settings and the developer log in the next UI. Every control
   returns the op main.c runs with the existing renderer's handlers, and each
   page fits the 960 x 540 minimum window with no text over other text. */
static void test_controller_and_developer_log_pages(void) {
    static char dump[CASE_DUMP_CAP];
    static const struct { FlowCase which; NkUiId id; PlayerUiControllerOp op; const char *what; }
        cases[] = {
        {FLOW_CONTROLLER, PLAYER_UI_ID_CONTROLLER_DEADZONE_DOWN,
         PLAYER_UI_CONTROLLER_DEADZONE_DOWN, "minus narrows the stick dead zone"},
        {FLOW_CONTROLLER, PLAYER_UI_ID_CONTROLLER_DEADZONE_UP, PLAYER_UI_CONTROLLER_DEADZONE_UP,
         "plus widens the stick dead zone"},
        {FLOW_CONTROLLER, PLAYER_UI_ID_CONTROLLER_TRIGGER_DOWN, PLAYER_UI_CONTROLLER_TRIGGER_DOWN,
         "minus lowers the trigger point"},
        {FLOW_CONTROLLER, PLAYER_UI_ID_CONTROLLER_TRIGGER_UP, PLAYER_UI_CONTROLLER_TRIGGER_UP,
         "plus raises the trigger point"},
        {FLOW_CONTROLLER, PLAYER_UI_ID_CONTROLLER_CALIBRATE, PLAYER_UI_CONTROLLER_CALIBRATE,
         "Calibrate stick starts the guided calibration"},
        {FLOW_CONTROLLER, PLAYER_UI_ID_CONTROLLER_SAVE, PLAYER_UI_CONTROLLER_SAVE,
         "Save profile writes the mapping"},
        {FLOW_CONTROLLER, PLAYER_UI_ID_CONTROLLER_RESET, PLAYER_UI_CONTROLLER_RESET,
         "Reset to defaults resets the mapping"},
        {FLOW_CONTROLLER, PLAYER_UI_ID_CONTROLLER_BACK, PLAYER_UI_CONTROLLER_BACK,
         "Back returns to Settings"},
        {FLOW_CONTROLLER_GAME, PLAYER_UI_ID_CONTROLLER_SCOPE, PLAYER_UI_CONTROLLER_TOGGLE_SCOPE,
         "the mapping choice switches between the game's own and the global one (#520)"},
        {FLOW_CALIBRATE_REST, PLAYER_UI_ID_CONTROLLER_CALIBRATION_CANCEL,
         PLAYER_UI_CONTROLLER_CALIBRATION_CANCEL, "Cancel stops the calibration at rest"},
        {FLOW_CALIBRATE_EXTREMES, PLAYER_UI_ID_CONTROLLER_CALIBRATION_PRIMARY,
         PLAYER_UI_CONTROLLER_CALIBRATION_FINISH, "Finish ends the range sampling"},
        {FLOW_CALIBRATE_EXTREMES, PLAYER_UI_ID_CONTROLLER_CALIBRATION_CANCEL,
         PLAYER_UI_CONTROLLER_CALIBRATION_CANCEL, "Cancel stops the range sampling"},
        {FLOW_CALIBRATE_RESULT, PLAYER_UI_ID_CONTROLLER_CALIBRATION_PRIMARY,
         PLAYER_UI_CONTROLLER_CALIBRATION_ACCEPT, "Accept keeps the calibration"},
        {FLOW_CALIBRATE_RESULT, PLAYER_UI_ID_CONTROLLER_CALIBRATION_CANCEL,
         PLAYER_UI_CONTROLLER_CALIBRATION_CANCEL, "Cancel drops the calibration"}
    };
    static const FlowCase pages[] = {FLOW_CONTROLLER, FLOW_CONTROLLER_GAME, FLOW_CALIBRATE_REST,
                                     FLOW_CALIBRATE_EXTREMES, FLOW_CALIBRATE_RESULT, FLOW_DEVLOG,
                                     FLOW_DEVLOG_EMPTY};
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(controller_op_is(activate_flow_button(cases[i].id, cases[i].which), cases[i].op, 0u),
              cases[i].what);
    }
    for (size_t control = 0u; control < PLAYER_UI_CONTROLLER_BUTTONS; ++control) {
        CHECK(controller_op_is(activate_flow_button(PLAYER_UI_ID_CONTROLLER_BUTTON(control),
                                                    FLOW_CONTROLLER),
                               PLAYER_UI_CONTROLLER_REBIND, control),
              "each PSP control's button rebinds that control");
    }
    CHECK(activate_flow_button(PLAYER_UI_ID_CONTROLLER_SCOPE, FLOW_CONTROLLER).kind ==
              PLAYER_UI_ACTION_NONE,
          "with no game selected there is no mapping choice");
    CHECK(activate_flow_button(PLAYER_UI_ID_CONTROLLER_CALIBRATION_PRIMARY,
                               FLOW_CALIBRATE_REST).kind == PLAYER_UI_ACTION_NONE,
          "the resting step only offers Cancel");
    CHECK(activate_flow_button(PLAYER_UI_ID_TAB_LIBRARY, FLOW_CONTROLLER).kind ==
              PLAYER_UI_ACTION_OPEN_LIBRARY,
          "the LIBRARY tab on the controller page opens the library");
    CHECK(developer_log_op_is(activate_flow_button(PLAYER_UI_ID_DEVELOPER_LOG_COPY, FLOW_DEVLOG),
                              PLAYER_UI_DEVELOPER_LOG_COPY),
          "Copy puts the log on the clipboard");
    CHECK(developer_log_op_is(activate_flow_button(PLAYER_UI_ID_DEVELOPER_LOG_SAVE, FLOW_DEVLOG),
                              PLAYER_UI_DEVELOPER_LOG_SAVE),
          "Save writes the log to the logs folder");
    CHECK(developer_log_op_is(activate_flow_button(PLAYER_UI_ID_DEVELOPER_LOG_CLOSE,
                                                   FLOW_DEVLOG_EMPTY),
                              PLAYER_UI_DEVELOPER_LOG_CLOSE),
          "Close leaves the developer log");
    CHECK(dump_flow_case(FLOW_CONTROLLER, dump, sizeof(dump)) &&
              strstr(dump, "\"BUTTONS \xC2\xB7 GLOBAL MAPPING\"") != NULL &&
              strstr(dump, "\"Cross\"") != NULL && strstr(dump, "\"A (CROSS)\"") != NULL &&
              strstr(dump, "\"Dead zone 24%\"") != NULL && strstr(dump, "\"SAVE PROFILE\"") != NULL,
          "the mapping names each control beside its binding, and the mapping it edits");
    CHECK(dump_flow_case(FLOW_CONTROLLER_GAME, dump, sizeof(dump)) &&
              strstr(dump, "TEST00005 ONLY") != NULL &&
              strstr(dump, "\"PRESS A BUTTON...\"") != NULL &&
              strstr(dump, "Conflict: Y (Triangle)") != NULL &&
              strstr(dump, "\"USE THE GLOBAL MAPPING\"") != NULL,
          "a game's own mapping is named, with the waiting control and the conflict");
    CHECK(dump_flow_case(FLOW_DEVLOG, dump, sizeof(dump)) &&
              strstr(dump, "[255] [runtime]") != NULL && strstr(dump, "[000] [runtime]") == NULL &&
              strstr(dump, "\"Copied 256 lines.\"") != NULL,
          "the log shows its newest lines and the last notice");
    CHECK(dump_flow_case(FLOW_DEVLOG_EMPTY, dump, sizeof(dump)) &&
              strstr(dump, "\"Nothing logged yet.\"") != NULL,
          "an empty log says so");
    for (size_t i = 0u; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        CHECK(flow_case_fits(pages[i], 960, 540, dump, sizeof(dump)),
              "each controller and developer log page fits the 960 x 540 minimum window");
        CHECK(flow_case_fits(pages[i], 1280, 800, dump, sizeof(dump)),
              "each controller and developer log page fits 1280 x 800");
    }
    CHECK(flow_case_fits(FLOW_CONTROLLER_GAME, 960, 540, dump, sizeof(dump)) &&
              strstr(dump, "\"Hold\"") != NULL && strstr(dump, "\"LIVE INPUT\"") != NULL &&
              strstr(dump, "\"USE THE GLOBAL MAPPING\"") != NULL,
          "every control, the live input and the mapping choice are on screen at 960 x 540");
    CHECK(flow_case_fits(FLOW_DEVLOG, 960, 540, dump, sizeof(dump)) &&
              strstr(dump, "[255] [runtime]") != NULL && strstr(dump, "\"CLOSE\"") != NULL,
          "the newest log line and Close are on screen at 960 x 540");
}

/* True when the dump has a draw command of `kind` for element `id`, with `text` when
   that is not NULL; the first such command's left and right edges go to x0 and x1. */
static bool dump_has_element(const char *dump, NkUiId id, const char *kind, const char *text,
                             int *x0, int *x1) {
    char prefix[32];
    char quoted[40];
    const char *row = dump;
    (void)snprintf(prefix, sizeof(prefix), "%08X %s ", (unsigned)id, kind);
    if (text != NULL) (void)snprintf(quoted, sizeof(quoted), "\"%s\"\n", text);
    while (row != NULL && *row != '\0') {
        const char *end = strchr(row, '\n');
        if (strncmp(row, prefix, strlen(prefix)) == 0 &&
            (text == NULL || (end != NULL && (size_t)(end - row) + 1u >= strlen(quoted) &&
                              strncmp(end + 1 - strlen(quoted), quoted, strlen(quoted)) == 0))) {
            int left = 0, top = 0, right = 0;
            if (sscanf(row + strlen(prefix), "%d %d %d", &left, &top, &right) != 3) return false;
            if (x0 != NULL) *x0 = left;
            if (x1 != NULL) *x1 = right;
            return true;
        }
        row = end != NULL ? end + 1 : NULL;
    }
    return false;
}

/* #1299: each binding's prompt glyph, in the set of the device shown, between the
   control's name and its binding: keycaps with the key's legend for the keyboard, the
   pad's own face letters, shoulders, D-pad and pills for the controller. */
static void test_controller_binding_glyphs(void) {
    static char dump[CASE_DUMP_CAP];
    static const FlowCase pages[] = {FLOW_CONTROLLER, FLOW_CONTROLLER_KEYS};
    static const char *const pad_text[PLAYER_UI_CONTROLLER_BUTTONS] = {
        "VIEW", NULL, NULL, NULL, NULL, NULL, "L", "R", "Y", "B", "A", "X", NULL, NULL
    };
    for (size_t p = 0u; p < sizeof(pages) / sizeof(pages[0]); ++p) {
        bool keys = pages[p] == FLOW_CONTROLLER_KEYS;
        const PlayerUiBindingGlyph *glyphs = keys ? k_key_glyphs : k_pad_glyphs;
        CHECK(dump_flow_case(pages[p], dump, sizeof(dump)), "the Controls page draws");
        for (size_t control = 0u; control < PLAYER_UI_CONTROLLER_BUTTONS; ++control) {
            NkUiId glyph = PLAYER_UI_ID_CONTROLLER_GLYPH(control);
            const char *text = keys ? glyphs[control].label : pad_text[control];
            int glyph_x1 = 0, words_x0 = 0;
            if (!glyphs[control].shown) {
                CHECK(!dump_has_element(dump, glyph, "RECT", NULL, NULL, NULL) &&
                          !dump_has_element(dump, glyph, "TEXT", NULL, NULL, NULL),
                      "an unbound control has no glyph");
                continue;
            }
            CHECK(dump_has_element(dump, glyph, "RECT", NULL, NULL, &glyph_x1),
                  "every binding has its glyph");
            if (text != NULL) {
                CHECK(dump_has_element(dump, glyph, "TEXT", text, NULL, NULL),
                      "the glyph prints the key's legend or the pad button's name");
            } else {
                CHECK(!dump_has_element(dump, glyph, "TEXT", NULL, NULL, NULL),
                      "the D-pad, arrow keys and menu button glyphs are drawn, not written");
            }
            CHECK(dump_has_element(dump, PLAYER_UI_ID_CONTROLLER_BUTTON(control), "TEXT", NULL,
                                   &words_x0, NULL) &&
                      glyph_x1 < words_x0,
                  "the glyph sits before the binding in words");
        }
    }
    CHECK(dump_flow_case(FLOW_CONTROLLER_KEYS, dump, sizeof(dump)) &&
              dump_has_element(dump, PLAYER_UI_ID_CONTROLLER_GLYPH(1u), "TEXT", "ENTER", NULL,
                               NULL) &&
              strstr(dump, "\"PRESS A KEY...\"") != NULL,
          "the control waiting for a key keeps the glyph of the key it has");
}

/* #1299: the keyboard keys on the Controls page. CONTROLLER and KEYBOARD choose
   whose bindings the rows show, each row's button rebinds that control's key,
   and the page names every PSP control with its key and fits the minimum window. */
static void test_controller_keyboard_page(void) {
    static char dump[CASE_DUMP_CAP];
    char quoted[96];
    CHECK(controller_op_is(activate_flow_button(PLAYER_UI_ID_CONTROLLER_SHOW_KEYBOARD,
                                                FLOW_CONTROLLER),
                           PLAYER_UI_CONTROLLER_SHOW_KEYBOARD, 0u),
          "KEYBOARD on the controller buttons shows the keyboard keys");
    CHECK(controller_op_is(activate_flow_button(PLAYER_UI_ID_CONTROLLER_SHOW_PAD,
                                                FLOW_CONTROLLER_KEYS),
                           PLAYER_UI_CONTROLLER_SHOW_CONTROLLER, 0u),
          "CONTROLLER on the keyboard keys shows the controller buttons");
    CHECK(controller_op_is(activate_flow_button(PLAYER_UI_ID_CONTROLLER_RESET, FLOW_CONTROLLER_KEYS),
                           PLAYER_UI_CONTROLLER_RESET, 0u),
          "Reset to defaults is offered for the keyboard keys");
    CHECK(controller_op_is(activate_flow_button(PLAYER_UI_ID_CONTROLLER_SAVE, FLOW_CONTROLLER_KEYS),
                           PLAYER_UI_CONTROLLER_SAVE, 0u),
          "Save profile saves the keys with the mapping");
    for (size_t control = 0u; control < PLAYER_UI_CONTROLLER_BUTTONS; ++control) {
        CHECK(controller_op_is(activate_flow_button(PLAYER_UI_ID_CONTROLLER_BUTTON(control),
                                                    FLOW_CONTROLLER_KEYS),
                               PLAYER_UI_CONTROLLER_REBIND, control),
              "each PSP control's key button rebinds that control's key");
    }
    CHECK(activate_flow_button(PLAYER_UI_ID_CONTROLLER_CALIBRATE, FLOW_CONTROLLER_KEYS).kind ==
              PLAYER_UI_ACTION_NONE,
          "the keyboard keys offer no stick calibration");
    CHECK(dump_flow_case(FLOW_CONTROLLER_KEYS, dump, sizeof(dump)), "the keyboard keys draw");
    for (size_t control = 0u; control < PLAYER_UI_CONTROLLER_BUTTONS; ++control) {
        size_t n;
        (void)snprintf(quoted, sizeof(quoted), "\"%s\"", k_pad_names[control]);
        CHECK(strstr(dump, quoted) != NULL, "every PSP control is named beside its key");
        if (control == 1u) continue; /* Start waits for its key */
        n = (size_t)snprintf(quoted, sizeof(quoted), "\"%s\"", k_key_bindings[control]);
        for (size_t c = 0u; c < n && c < sizeof(quoted); ++c) {
            if (quoted[c] >= 'a' && quoted[c] <= 'z') quoted[c] = (char)(quoted[c] - 'a' + 'A');
        }
        CHECK(strstr(dump, quoted) != NULL, "every PSP control shows its keyboard key");
    }
    CHECK(strstr(dump, "\"PRESS A KEY...\"") != NULL &&
              strstr(dump, "\"KEYS \xC2\xB7 TEST00005 ONLY\"") != NULL,
          "the control waiting for a key says so, and the mapping being edited is named");
    CHECK(strstr(dump, "Esc or F1 pauses the game.") != NULL &&
              strstr(dump, "\"Dead zone 24%\"") == NULL,
          "for the keyboard the stick card names the keys Nakagawa keeps");
    CHECK(dump_flow_case(FLOW_CONTROLLER, dump, sizeof(dump)) &&
              strstr(dump, "\"KEYBOARD\"") != NULL && strstr(dump, "\"Esc or F1") == NULL,
          "the controller buttons offer the keyboard keys");
    CHECK(flow_case_fits(FLOW_CONTROLLER_KEYS, 960, 540, dump, sizeof(dump)) &&
              strstr(dump, "\"Hold\"") != NULL && strstr(dump, "\"RESET TO DEFAULTS\"") != NULL &&
              strstr(dump, "\"KEYBOARD\"") != NULL,
          "every key, the device choice and Reset to defaults are on screen at 960 x 540");
    CHECK(flow_case_fits(FLOW_CONTROLLER_KEYS, 1280, 800, dump, sizeof(dump)),
          "the keyboard keys fit 1280 x 800");
}

/* Every screen fits the default 1280 x 720 window and the 960 x 540 minimum
   (logical px): no text outside the window, no text painted over, and so the
   buttons are all on screen. A wrapped paragraph used to be laid out one line
   tall when its page was overfull, and the next block painted over its later
   lines (launcher review B2). */
static void test_every_screen_fits_the_window(void) {
    static char dump[CASE_DUMP_CAP];
    /* The default window, the minimum, a small laptop's maximised window, and
       the 1280 x 720 pixels of a 150% display (853 x 480 at 1x). */
    static const struct { int w; int h; } sizes[] = {
        {1280, 720}, {960, 540}, {1366, 705}, {853, 480}
    };
    {
        /* A window under the design size draws the 1280 x 720 layout smaller;
           a larger one keeps its density and gains room. */
        NkUiViewport small = launcher_viewport(960, 540, 1.0f);
        NkUiViewport hidpi = launcher_viewport(1920, 1080, 1.5f);
        NkUiViewport hidpi_small = launcher_viewport(1440, 810, 1.5f);
        NkUiViewport large = launcher_viewport(1920, 1080, 1.0f);
        NkUiScale a = nk_ui_scale_for_viewport(&small);
        NkUiScale b = nk_ui_scale_for_viewport(&hidpi);
        NkUiScale c = nk_ui_scale_for_viewport(&hidpi_small);
        NkUiScale d = nk_ui_scale_for_viewport(&large);
        CHECK(a.density == 0.75f && a.logical_w == 1280.0f && a.logical_h == 720.0f,
              "a 960 x 540 window lays the launcher out at 1280 x 720, drawn at 0.75");
        CHECK(b.density == 1.5f && b.logical_w == 1280.0f && b.logical_h == 720.0f,
              "a 1280 x 720 window on a 150% display keeps its density");
        CHECK(c.density == 1.125f && c.logical_w == 1280.0f && c.logical_h == 720.0f,
              "the 960 x 540 minimum on a 150% display lays out at 1280 x 720");
        CHECK(d.density == 1.0f && d.logical_w == 1920.0f && d.logical_h == 1080.0f,
              "a large window keeps its density and gains room");
    }
    for (size_t s = 0u; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        for (int which = FLOW_BUILD; which < FLOW_COUNT; ++which) {
            char message[160];
            if (which == FLOW_DEVLOG) continue; /* cuts its lines to the box by design */
            (void)snprintf(message, sizeof(message),
                           "screen case %d fits a %d x %d window", which, sizes[s].w, sizes[s].h);
            CHECK(flow_case_fits((FlowCase)which, sizes[s].w, sizes[s].h, dump, sizeof(dump)),
                  message);
        }
    }
}

static void test_error_details_fit_the_minimum_window(void) {
    static char dump[CASE_DUMP_CAP];
    static const char details[] =
        "tools/nk_cli.py was not found. Searched, in order: the NK_INSTALL_ROOT environment "
        "variable, D:/Synthetic/Players/Install Folder/Nakagawa Recomp/bin/tools, "
        "D:/Synthetic/Players/Install Folder/Nakagawa Recomp/bin/../tools, "
        "D:/Synthetic/Players/Install Folder/Nakagawa Recomp/bin/../source/tools (the "
        "release layout), the working directory and its parent. Fix: run the "
        "player from a Nakagawa Recomp source checkout, keep a tools folder beside "
        "nakagawa_player.exe, or set NK_INSTALL_ROOT to the folder that contains tools/ "
        "(for example the release's source folder).";
    NkUiConfig config;
    NkUiTextBackend text = screen_test_text_backend();
    PlayerUiErrorModel error;
    text.measure = screen_test_wide_measure;
    NkUi *ui;
    const NkUiDrawList *draw = NULL;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create a UI for the error card at the minimum window");
    if (ui == NULL) return;
    set_test_viewport(ui, 960, 540);
    memset(&error, 0, sizeof(error));
    error.badge = "CLI_NOT_FOUND";
    error.title = "Build Tools Not Found";
    error.message = "Nakagawa Recomp's build tools were not found next to the app. Reinstall "
                    "Nakagawa Recomp and keep its folder together.";
    error.details = details;
    error.recovery_label = "Return to Library";
    error.logical_width = 960.0f;
    error.logical_height = 540.0f;
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 0.0);
        (void)player_ui_screen_error(ui, &error);
        draw = nk_ui_end(ui);
    }
    CHECK(draw != NULL, "lay out the error card at the minimum window");
    if (draw != NULL) {
        size_t used = nk_ui_dump(ui, dump, sizeof(dump));
        CHECK(used > 0u && strstr(dump, "folder).") != NULL,
              "the error details keep their last sentence at 960 x 540");
    }
    nk_ui_destroy(ui);
}

static void test_screen_actions(void) {
    PlayerUiLibraryModel library;
    PlayerUiTitleModel detail;
    PlayerUiAction action;
    memset(&library, 0, sizeof(library));
    memset(&detail, 0, sizeof(detail));
    library.columns = 2u;
    library.page_size = 4u;
    library.logical_width = 640.0f;
    action = activate_library_button(PLAYER_UI_ID_LIBRARY_EMPTY_ADD, &library);
    CHECK(action.kind == PLAYER_UI_ACTION_ADD_DISC,
          "empty Library action requests a PSP disc image");
    action = activate_library_button(PLAYER_UI_ID_LIBRARY_EMPTY_WIZARD, &library);
    CHECK(action.kind == PLAYER_UI_ACTION_START_WIZARD,
          "the empty Library's main action starts the setup wizard");
    /* #1281: the top bar opens About, and Settings has a visible tab there. */
    action = activate_library_button(PLAYER_UI_ID_TAB_SETTINGS, &library);
    CHECK(action.kind == PLAYER_UI_ACTION_OPEN_SETTINGS,
          "the empty Library's SETTINGS tab opens Settings");
    action = activate_library_button(PLAYER_UI_ID_TAB_ABOUT, &library);
    CHECK(action.kind == PLAYER_UI_ACTION_OPEN_ABOUT, "the ABOUT tab opens About");
    {
        /* The menu prompt (Start, F1) opens Settings as well. */
        NkUiConfig config;
        NkUiEvent menu;
        NkUi *ui;
        memset(&config, 0, sizeof(config));
        config.host = NK_UI_HOST_LAUNCHER;
        ui = nk_ui_create(&config);
        CHECK(ui != NULL, "create a UI for the Library's menu prompt");
        if (ui != NULL) {
            set_test_viewport(ui, 1280, 800);
            memset(&menu, 0, sizeof(menu));
            menu.type = NK_UI_EV_MENU;
            menu.down = true;
            nk_ui_push_event(ui, &menu);
            nk_ui_begin(ui, 0.0);
            action = player_ui_screen_library(ui, &library);
            (void)nk_ui_end(ui);
            CHECK(action.kind == PLAYER_UI_ACTION_OPEN_SETTINGS,
                  "the Library's menu prompt opens Settings");
            nk_ui_destroy(ui);
        }
    }
    {
        /* With nothing focused, START SETUP WIZARD is the one default. */
        NkUiConfig config;
        NkUi *ui;
        memset(&config, 0, sizeof(config));
        config.host = NK_UI_HOST_LAUNCHER;
        ui = nk_ui_create(&config);
        CHECK(ui != NULL, "create a UI for the empty Library's default focus");
        if (ui != NULL) {
            set_test_viewport(ui, 1280, 800);
            for (int frame = 0; frame < 2; ++frame) {
                nk_ui_begin(ui, 0.0);
                (void)player_ui_screen_library(ui, &library);
                (void)nk_ui_end(ui);
            }
            CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_LIBRARY_EMPTY_WIZARD,
                  "the empty Library focuses START SETUP WIZARD by default");
            nk_ui_destroy(ui);
        }
    }
    library.titles = k_titles;
    library.title_count = sizeof(k_titles) / sizeof(k_titles[0]);
    action = activate_library_button(PLAYER_UI_ID_LIBRARY_ITEM(2u), &library);
    CHECK(action.kind == PLAYER_UI_ACTION_OPEN_TITLE && action.title_index == 2u,
          "a Library card opens its matching Title detail");
    action = activate_library_button(PLAYER_UI_ID_TAB_SETTINGS, &library);
    CHECK(action.kind == PLAYER_UI_ACTION_OPEN_SETTINGS,
          "the Library's SETTINGS tab opens Settings");
    detail.title = &k_titles[0];
    detail.title_index = 0u;
    action = activate_title_button(PLAYER_UI_ID_TITLE_PRIMARY, &detail);
    CHECK(action.kind == PLAYER_UI_ACTION_PRIMARY &&
          action.primary == PLAYER_UI_PRIMARY_PLAY,
          "the primary button returns its existing Play action");
    {
        /* An INCOMPATIBLE package (#1147): REBUILD PACKAGE, not a dead label. */
        PlayerUiTitle incompatible = k_titles[1];
        incompatible.library_status_detail =
            "Built for another player version, or its package file is unreadable.";
        incompatible.primary_action_label = "REBUILD PACKAGE";
        incompatible.primary_action = PLAYER_UI_PRIMARY_REBUILD;
        detail.title = &incompatible;
        action = activate_title_button(PLAYER_UI_ID_TITLE_PRIMARY, &detail);
        CHECK(action.kind == PLAYER_UI_ACTION_PRIMARY &&
              action.primary == PLAYER_UI_PRIMARY_REBUILD,
              "the REBUILD PACKAGE button returns the rebuild action");
        detail.title = &k_titles[0];
    }
    action = activate_title_button(PLAYER_UI_ID_TITLE_SAVE_FOLDER, &detail);
    CHECK(action.kind == PLAYER_UI_ACTION_OPEN_SAVE_FOLDER,
          "the save-folder button requests the existing per-title folder");
    action = activate_title_button(PLAYER_UI_ID_TITLE_BACK, &detail);
    CHECK(action.kind == PLAYER_UI_ACTION_BACK,
          "the back button returns to the Library");
    library.page_size = 8u;
    library.columns = 4u;
    library.logical_width = 1280.0f;
    action = activate_library_button(PLAYER_UI_ID_LIBRARY_ADD, &library);
    CHECK(action.kind == PLAYER_UI_ACTION_ADD_DISC,
          "the add card at the end of the grid requests a disc image");
}

/* Design-system checks on the real layout: the focus ring sits outside the
   focused card, the primary label is centred in its button, and the card grid
   keeps its gaps. */
static const NkUiDrawCmd *find_command(const NkUiDrawList *list, NkUiId id, int kind,
                                       uint32_t rgba) {
    for (uint32_t i = 0u; list != NULL && i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->element_id == id && cmd->kind == kind &&
            (rgba == 0u || cmd->rgba_pm == rgba)) {
            return cmd;
        }
    }
    return NULL;
}

static void test_design_system_layout(void) {
    NkUiConfig config;
    NkUi *ui;
    PlayerUiLibraryModel library;
    PlayerUiTitleModel detail;
    const NkUiDrawList *draw;
    const NkUiTheme *theme = nk_ui_theme_n1();
    NkUiTextBackend text = screen_test_text_backend();
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create the design-system probe");
    if (ui == NULL) return;
    set_test_viewport(ui, 1280, 800);
    memset(&library, 0, sizeof(library));
    library.titles = k_titles;
    library.title_count = sizeof(k_titles) / sizeof(k_titles[0]);
    library.page_size = player_ui_library_page_size(1280.0f, 800.0f, &library.columns);
    library.logical_width = 1280.0f;
    library.logical_height = 800.0f;
    nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(0u));
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 37.0);
        (void)player_ui_screen_library(ui, &library);
        draw = nk_ui_end(ui);
    }
    CHECK(draw != NULL, "lay out the Library");
    {
        const NkUiDrawCmd *card = find_command(draw, PLAYER_UI_ID_LIBRARY_ITEM(0u),
                                               NK_UI_DRAW_RECTANGLE, 0u);
        const NkUiDrawCmd *next = find_command(draw, PLAYER_UI_ID_LIBRARY_ITEM(1u),
                                               NK_UI_DRAW_RECTANGLE, 0u);
        const NkUiDrawCmd *ring = NULL;
        int ring_x0 = 1 << 30;
        int ring_x1 = -(1 << 30);
        for (uint32_t i = 0u; i < draw->ncmd; ++i) {
            const NkUiDrawCmd *cmd = &draw->cmd[i];
            if (cmd->element_id == PLAYER_UI_ID_LIBRARY_ITEM(0u) &&
                cmd->kind == NK_UI_DRAW_BORDER && cmd->rgba_pm == theme->focus) {
                ring = cmd;
                if (cmd->bounds[0] < ring_x0) ring_x0 = cmd->bounds[0];
                if (cmd->bounds[2] > ring_x1) ring_x1 = cmd->bounds[2];
            }
        }
        CHECK(card != NULL && next != NULL, "the first two cards are drawn");
        CHECK(ring != NULL, "the focused card carries the accent focus ring");
        if (card != NULL && ring != NULL) {
            CHECK(ring_x0 == card->bounds[0] - 6 && ring_x1 == card->bounds[2] + 6,
                  "the ring is 3 px wide, 3 px outside the card");
        }
        if (card != NULL && next != NULL) {
            CHECK(next->bounds[0] - card->bounds[2] == (int)NK_UI_TILE_GAP,
                  "cards keep the 20 px grid gap");
            CHECK(card->bounds[0] == (int)NK_UI_GUTTER, "the grid starts at the page gutter");
        }
    }
    memset(&detail, 0, sizeof(detail));
    detail.title = &k_titles[0];
    detail.logical_width = 1280.0f;
    detail.logical_height = 800.0f;
    nk_ui_focus(ui, PLAYER_UI_ID_TITLE_PRIMARY);
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 37.0);
        (void)player_ui_screen_title(ui, &detail);
        draw = nk_ui_end(ui);
    }
    CHECK(draw != NULL, "lay out Title detail");
    {
        const NkUiDrawCmd *button = find_command(draw, PLAYER_UI_ID_TITLE_PRIMARY,
                                                 NK_UI_DRAW_RECTANGLE, theme->accent_dark);
        const NkUiDrawCmd *label = find_command(draw, PLAYER_UI_ID_TITLE_PRIMARY,
                                                NK_UI_DRAW_TEXT, 0u);
        CHECK(button != NULL && label != NULL, "the primary action is drawn with its label");
        if (button != NULL && label != NULL) {
            int left = label->bounds[0] - button->bounds[0];
            int right = button->bounds[2] - label->bounds[2];
            int top = label->bounds[1] - button->bounds[1];
            int bottom = button->bounds[3] - label->bounds[3];
            CHECK(button->bounds[3] - button->bounds[1] == (int)NK_UI_CONTROL_HEIGHT_LARGE,
                  "the primary action is the 52 px large button");
            CHECK(left - right <= 1 && right - left <= 1,
                  "the label is centred across the button");
            CHECK(top - bottom <= 1 && bottom - top <= 1,
                  "the label is centred down the button, not stuck to its top");
        }
    }
    nk_ui_destroy(ui);
}

#if defined(NK_UI_SCREEN_CAPTURE) && defined(NK_UI_WITH_SDL) && \
    defined(NK_UI_WITH_SDL_TTF)
typedef struct {
    NkUi *ui;
    NkUiSdlr *backend;
    SDL_Renderer *renderer;
    SDL_Surface *surface;
    const char *directory;
} CaptureRig;

/* Lays the screen out twice (the focus graph uses the previous frame's
   rectangles), draws it and saves <directory>/<name>.bmp. */
static bool capture_one(CaptureRig *rig, const char *name,
                        const PlayerUiLibraryModel *library,
                        const PlayerUiTitleModel *title) {
    const NkUiDrawList *draw = NULL;
    char path[4096];
    FlowCase flow = g_flow_case;
    const char separator =
#if defined(_WIN32) || defined(_WIN64)
        '\\';
#else
        '/';
#endif
    int written = snprintf(path, sizeof(path), "%s%c%s.bmp", rig->directory, separator, name);
    if (written <= 0 || (size_t)written >= sizeof(path)) return false;
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(rig->ui, 37.0);
        if (flow != FLOW_NONE) {
            (void)draw_flow_case(rig->ui, flow);
        } else if (library != NULL) {
            (void)player_ui_screen_library(rig->ui, library);
        } else if (title != NULL) {
            (void)player_ui_screen_title(rig->ui, title);
        } else {
            (void)player_ui_screen_about(rig->ui, g_about_case);
        }
        draw = nk_ui_end(rig->ui);
    }
    if (draw == NULL ||
        !SDL_SetRenderDrawColor(rig->renderer, 11u, 15u, 20u, 255u) ||
        !SDL_RenderClear(rig->renderer) ||
        !nk_ui_sdlr_render(rig->backend, draw)) {
        return false;
    }
    SDL_RenderPresent(rig->renderer);
    return SDL_SaveBMP(rig->surface, path);
}

static void capture_prefs(NkUi *ui, NkUiPromptStyle style, bool high_contrast) {
    NkUiPrefs prefs;
    memset(&prefs, 0, sizeof(prefs));
    prefs.prompt_style = style;
    prefs.high_contrast_focus = high_contrast;
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
}

/* One capture at another window size and UI scale, with its own surface,
   renderer, layout core and backend (the logical layout follows the scale). */
static bool capture_scaled(const char *directory, const NkUiTextBackend *text,
                           const char *name, int pixel_w, int pixel_h, float display_scale,
                           const PlayerUiTitle *titles, size_t title_count,
                           const PlayerUiTitle *detail, NkUiId focus) {
    SDL_Surface *surface = SDL_CreateSurface(pixel_w, pixel_h, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer *renderer = surface != NULL ? SDL_CreateSoftwareRenderer(surface) : NULL;
    NkUiConfig config;
    NkUi *ui = NULL;
    NkUiSdlr *backend = NULL;
    CaptureRig rig;
    bool ok = false;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = text;
    if (renderer != NULL) ui = nk_ui_create(&config);
    if (ui != NULL) backend = nk_ui_sdlr_create(renderer, text);
    if (backend != NULL) {
        NkUiViewport viewport = launcher_viewport(pixel_w, pixel_h, display_scale);
        NkUiScale scale = nk_ui_scale_for_viewport(&viewport);
        nk_ui_set_viewport(ui, &viewport);
        nk_ui_sdlr_set_scale(backend, scale.density);
        capture_prefs(ui, NK_UI_PROMPT_KEYCAPS, false);
        rig.ui = ui;
        rig.backend = backend;
        rig.renderer = renderer;
        rig.surface = surface;
        rig.directory = directory;
        nk_ui_focus(ui, focus);
        if (detail == NULL) {
            PlayerUiLibraryModel library;
            memset(&library, 0, sizeof(library));
            library.titles = titles;
            library.title_count = title_count;
            size_t demos = player_ui_library_demo_count(titles, title_count);
            library.page_size = player_ui_library_page_size_with_demos(
                scale.logical_w, scale.logical_h, demos, &library.columns);
            library.logical_width = scale.logical_w;
            library.logical_height = scale.logical_h;
            if (library.title_count - demos > 0u &&
                library.title_count - demos + 1u > library.page_size) {
                library.page_label = "Page 1 of 2";
            }
            ok = capture_one(&rig, name, &library, NULL);
        } else {
            PlayerUiTitleModel model;
            memset(&model, 0, sizeof(model));
            model.title = detail;
            model.logical_width = scale.logical_w;
            model.logical_height = scale.logical_h;
            ok = capture_one(&rig, name, NULL, &model);
        }
    }
    nk_ui_sdlr_destroy(backend);
    nk_ui_destroy(ui);
    if (renderer != NULL) SDL_DestroyRenderer(renderer);
    if (surface != NULL) SDL_DestroySurface(surface);
    return ok;
}

/* A project-made stand-in for disc art: a diagonal gradient, so a capture shows
   how a disc's ICON0.PNG / PIC1.PNG sits on the card and the hero without any
   game image. */
static bool capture_gradient(NkUiImage *image, uint32_t w, uint32_t h, uint64_t serial) {
    image->rgba_pm = (uint8_t *)malloc((size_t)w * h * 4u);
    if (image->rgba_pm == NULL) return false;
    image->width = w;
    image->height = h;
    image->serial = serial;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t *p = image->rgba_pm + ((size_t)y * w + x) * 4u;
            p[0] = (uint8_t)(40u + 180u * x / w);
            p[1] = (uint8_t)(60u + 120u * y / h);
            p[2] = (uint8_t)(200u - 120u * x / w);
            p[3] = 255u;
        }
    }
    return true;
}

static int capture_screens(const char *directory) {
    static NkUiPromptImages prompt_images;
    NkUiImage icon_art = {0u, 0u, NULL, 0u};
    NkUiImage picture_art = {0u, 0u, NULL, 0u};
    SDL_Surface *surface = NULL;
    SDL_Renderer *renderer = NULL;
    NkUiTtf *ttf = NULL;
    NkUiSdlr *backend = NULL;
    NkUi *ui = NULL;
    NkUiTextBackend text;
    NkUiConfig config;
    PlayerUiLibraryModel library;
    PlayerUiTitleModel title;
    PlayerUiTitle title_fixture = k_titles[0];
    PlayerUiTitle setup_fixture = k_titles[4];
    CaptureRig rig;
    bool ok = false;

    if (SDL_Init(SDL_INIT_VIDEO) == false) {
        fprintf(stderr, "SKIP: SDL video is unavailable for screen captures: %s\n",
                SDL_GetError());
        return 77;
    }
    surface = SDL_CreateSurface(1280, 800, SDL_PIXELFORMAT_RGBA32);
    if (surface == NULL) goto done;
    renderer = SDL_CreateSoftwareRenderer(surface);
    if (renderer == NULL) goto done;
    ttf = nk_ui_ttf_create(NULL, 0u);
    if (ttf == NULL) {
        fprintf(stderr, "SKIP: SDL3_ttf is unavailable for screen captures: %s\n",
                nk_ui_ttf_last_error());
        ok = false;
        goto done;
    }
    for (int i = 0; i < NK_UI_FONT_COUNT; ++i) {
        if (!nk_ui_ttf_add_font(ttf, i, nk_ui_font_path(i))) goto done;
    }
    for (int i = 0; i < NK_UI_FONT_COUNT; ++i) {
        if (i == NK_UI_FONT_CJK) continue;
        if (i != NK_UI_FONT_BODY && !nk_ui_ttf_set_fallback(ttf, i, NK_UI_FONT_BODY)) goto done;
        if (!nk_ui_ttf_set_fallback(ttf, i, NK_UI_FONT_CJK)) goto done;
    }
    text = nk_ui_ttf_backend(ttf);
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    if (ui == NULL) goto done;
    backend = nk_ui_sdlr_create(renderer, &text);
    if (backend == NULL) goto done;
    {
        NkUiViewport viewport = {1280, 800, 1.0f, 100, 100, 0.0f, 0.0f};
        nk_ui_set_viewport(ui, &viewport);
        nk_ui_sdlr_set_scale(backend, 1.0f);
    }
    rig.ui = ui;
    rig.backend = backend;
    rig.renderer = renderer;
    rig.surface = surface;
    rig.directory = directory;
    /* The player draws the embedded Kenney glyphs; so do the captures. */
    if (nk_ui_prompt_images_load(&prompt_images) == 0u) goto done;
    nk_ui_set_prompt_images(ui, &prompt_images);

    memset(&library, 0, sizeof(library));
    library.titles = k_titles;
    library.title_count = sizeof(k_titles) / sizeof(k_titles[0]);
    library.page_size = player_ui_library_page_size(1280.0f, 800.0f, &library.columns);
    library.logical_width = 1280.0f;
    library.logical_height = 800.0f;
    capture_prefs(ui, NK_UI_PROMPT_KEYCAPS, false);
    nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(0u));
    if (!capture_one(&rig, "library", &library, NULL)) goto done;
    nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ADD);
    if (!capture_one(&rig, "library-add-focused", &library, NULL)) goto done;
    capture_prefs(ui, NK_UI_PROMPT_NEUTRAL, true);
    nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(1u));
    if (!capture_one(&rig, "library-controller-high-contrast", &library, NULL)) goto done;
    {
        static const struct { NkUiPromptStyle style; const char *name; } sets[] = {
            {NK_UI_PROMPT_LETTERS_A_SOUTH, "library-prompts-letters-a-south"},
            {NK_UI_PROMPT_LETTERS_A_EAST, "library-prompts-letters-a-east"},
            {NK_UI_PROMPT_SHAPES, "library-prompts-shapes"}
        };
        for (size_t i = 0u; i < sizeof(sets) / sizeof(sets[0]); ++i) {
            capture_prefs(ui, sets[i].style, false);
            nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(0u));
            if (!capture_one(&rig, sets[i].name, &library, NULL)) goto done;
        }
    }
    capture_prefs(ui, NK_UI_PROMPT_KEYCAPS, false);
    {
        /* Disc art: every title but the showcase demos gets the stand-in icon. */
        static PlayerUiTitle art_titles[sizeof(k_titles) / sizeof(k_titles[0])];
        PlayerUiLibraryModel art = library;
        if (!capture_gradient(&icon_art, 144u, 80u, 0xA47001ull) ||
            !capture_gradient(&picture_art, 480u, 272u, 0xA47002ull)) {
            goto done;
        }
        for (size_t i = 0u; i < sizeof(k_titles) / sizeof(k_titles[0]); ++i) {
            art_titles[i] = k_titles[i];
            if (art_titles[i].art_kind == NK_UI_ART_NONE) art_titles[i].icon = &icon_art;
        }
        art.titles = art_titles;
        nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(2u));
        if (!capture_one(&rig, "library-disc-art", &art, NULL)) goto done;
    }
    {
        PlayerUiLibraryModel empty = library;
        empty.titles = NULL;
        empty.title_count = 0u;
        nk_ui_focus(ui, 0u);
        if (!capture_one(&rig, "library-empty", &empty, NULL)) goto done;
    }
    {
        PlayerUiLibraryModel paged = library;
        paged.page_size = 4u;
        paged.page_index = 1u;
        paged.page_label = "Page 2 of 2";
        nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(4u));
        if (!capture_one(&rig, "library-second-page", &paged, NULL)) goto done;
    }

    memset(&title, 0, sizeof(title));
    title_fixture.compatibility_expect =
        "Shows a lit, rotating cube that answers the stick and buttons, with sound.";
    title_fixture.compatibility_known_issues = "";
    title.title = &title_fixture;
    title.title_index = 0u;
    title.logical_width = 1280.0f;
    title.logical_height = 800.0f;
    nk_ui_focus(ui, PLAYER_UI_ID_TITLE_PRIMARY);
    if (!capture_one(&rig, "title", NULL, &title)) goto done;
    nk_ui_focus(ui, PLAYER_UI_ID_TITLE_SAVE_FOLDER);
    if (!capture_one(&rig, "title-save-folder-focused", NULL, &title)) goto done;
    capture_prefs(ui, NK_UI_PROMPT_LETTERS_A_SOUTH, false);
    nk_ui_focus(ui, PLAYER_UI_ID_TITLE_PRIMARY);
    if (!capture_one(&rig, "title-controller", NULL, &title)) goto done;
    capture_prefs(ui, NK_UI_PROMPT_KEYCAPS, false);
    {
        PlayerUiTitle with_art = k_titles[4];
        PlayerUiTitleModel art_title = title;
        with_art.icon = &icon_art;
        with_art.picture = &picture_art;
        art_title.title = &with_art;
        art_title.title_index = 4u;
        nk_ui_focus(ui, PLAYER_UI_ID_TITLE_PRIMARY);
        if (!capture_one(&rig, "title-disc-art", NULL, &art_title)) goto done;
        with_art.picture = NULL;
        if (!capture_one(&rig, "title-disc-icon-only", NULL, &art_title)) goto done;
    }
    title.title = &setup_fixture;
    title.title_index = 4u;
    setup_fixture.library_status_detail =
        "Build the package for this PC before playing. It takes a few minutes.";
    setup_fixture.input_note =
        "Nakagawa decrypts this game's program from your own disc, on this PC.";
    nk_ui_focus(ui, PLAYER_UI_ID_TITLE_PRIMARY);
    if (!capture_one(&rig, "title-needs-setup", NULL, &title)) goto done;
    {
        /* The showcase demo's title page while it runs (#1297). */
        PlayerUiTitle running = title_fixture;
        PlayerUiTitleModel running_title = title;
        running.library_status = "Running";
        running.package_state = PLAYER_UI_PACKAGE_RUNNING;
        running.primary_action_label = "STOP GAME";
        running.primary_action = PLAYER_UI_PRIMARY_STOP_GAME;
        running_title.title = &running;
        running_title.title_index = 0u;
        nk_ui_focus(ui, PLAYER_UI_ID_TITLE_PRIMARY);
        if (!capture_one(&rig, "title-game-running", NULL, &running_title)) goto done;
        /* The same title before its window opens: the starting card. */
        running.library_status = "Starting";
        running.package_state = PLAYER_UI_PACKAGE_STARTING;
        running_title.now_s = 0.4;
        if (!capture_one(&rig, "title-game-starting", NULL, &running_title)) goto done;
    }
    {
        static const struct { FlowCase which; const char *name; NkUiId focus; } flows[] = {
            {FLOW_BUILD, "flow-build-running", PLAYER_UI_ID_BUILD_ACTION},
            {FLOW_BUILT, "flow-build-written", PLAYER_UI_ID_BUILD_ACTION},
            {FLOW_CONSENT, "flow-prereq-consent", PLAYER_UI_ID_PREREQ_ACCEPT},
            {FLOW_PROGRESS, "flow-prereq-progress", PLAYER_UI_ID_PREREQ_CANCEL},
            {FLOW_ERROR, "flow-error", PLAYER_UI_ID_ERROR_RECOVER},
            {FLOW_WIZ_WELCOME, "wizard-welcome", PLAYER_UI_ID_WIZARD_PRIMARY},
            {FLOW_WIZ_CHOSEN, "wizard-chosen", PLAYER_UI_ID_WIZARD_PRIMARY},
            {FLOW_WIZ_COPYING, "wizard-copying", PLAYER_UI_ID_WIZARD_CANCEL},
            {FLOW_WIZ_VERIFY, "wizard-verify", PLAYER_UI_ID_WIZARD_PRIMARY},
            {FLOW_WIZ_FONTS, "wizard-fonts", PLAYER_UI_ID_WIZARD_PRIMARY},
            {FLOW_WIZ_READY, "wizard-ready", PLAYER_UI_ID_WIZARD_PRIMARY},
            {FLOW_WIZ_OPTIONS, "wizard-options", PLAYER_UI_ID_WIZARD_PRIMARY},
            {FLOW_DISC_CHECKING, "disc-checking", PLAYER_UI_ID_DISC_PRIMARY},
            {FLOW_DISC_SUPPORTED, "disc-supported", PLAYER_UI_ID_DISC_PRIMARY},
            {FLOW_DISC_UNTESTED, "disc-untested", PLAYER_UI_ID_DISC_PRIMARY},
            {FLOW_DISC_FONTS, "disc-fonts-missing", PLAYER_UI_ID_DISC_PRIMARY},
            {FLOW_DISC_BLOCKED, "disc-blocked", PLAYER_UI_ID_DISC_PRIMARY},
            {FLOW_DISC_UNSUPPORTED, "disc-unsupported", PLAYER_UI_ID_DISC_PRIMARY},
            {FLOW_SETTINGS, "settings", PLAYER_UI_ID_SETTINGS_RESOLUTION},
            {FLOW_SETTINGS_ONLINE, "settings-online", PLAYER_UI_ID_SETTINGS_ONLINE_MODE},
            {FLOW_SETTINGS_NOTICES, "settings-notices", PLAYER_UI_ID_SETTINGS_BACK},
            {FLOW_SETTINGS_REMOVE, "settings-remove-tools", PLAYER_UI_ID_SETTINGS_BACK},
            {FLOW_SETTINGS_BUILD_CACHE, "settings-build-cache",
             PLAYER_UI_ID_SETTINGS_SHARE_LIBRARIES},
            {FLOW_CONTROLLER, "controller", PLAYER_UI_ID_CONTROLLER_BUTTON(0u)},
            {FLOW_CONTROLLER_GAME, "controller-game-mapping", PLAYER_UI_ID_CONTROLLER_BUTTON(10u)},
            {FLOW_CONTROLLER_KEYS, "controller-keyboard", PLAYER_UI_ID_CONTROLLER_BUTTON(1u)},
            {FLOW_CALIBRATE_REST, "controller-calibrate-rest",
             PLAYER_UI_ID_CONTROLLER_CALIBRATION_CANCEL},
            {FLOW_CALIBRATE_EXTREMES, "controller-calibrate-range",
             PLAYER_UI_ID_CONTROLLER_CALIBRATION_PRIMARY},
            {FLOW_CALIBRATE_RESULT, "controller-calibrate-result",
             PLAYER_UI_ID_CONTROLLER_CALIBRATION_PRIMARY},
            {FLOW_DEVLOG, "developer-log", PLAYER_UI_ID_DEVELOPER_LOG_CLOSE}
        };
        for (size_t i = 0u; i < sizeof(flows) / sizeof(flows[0]); ++i) {
            g_flow_case = flows[i].which;
            nk_ui_focus(ui, flows[i].focus);
            if (!capture_one(&rig, flows[i].name, NULL, &title)) {
                g_flow_case = FLOW_NONE;
                goto done;
            }
        }
        g_flow_case = FLOW_NONE;
    }
    {
        char line[64];
        PlayerUiAboutModel about;
        player_ui_version_line(NK_PLAYER_VERSION, 1792713600LL, line, sizeof(line));
        memset(&about, 0, sizeof(about));
        about.version_line = line;
        about.build_commit = "0123abc";
        about.logical_width = 1280.0f;
        about.logical_height = 800.0f;
        g_about_case = &about;
        nk_ui_focus(ui, PLAYER_UI_ID_TAB_ABOUT);
        if (!capture_one(&rig, "about", NULL, NULL)) goto done;
        capture_prefs(ui, NK_UI_PROMPT_LETTERS_A_SOUTH, false);
        nk_ui_focus(ui, PLAYER_UI_ID_ABOUT_NOTICES);
        if (!capture_one(&rig, "about-controller", NULL, NULL)) goto done;
        capture_prefs(ui, NK_UI_PROMPT_KEYCAPS, false);
        g_about_case = NULL;
    }
    /* The same layouts at 150% on a 1920x1200 display, and in a small window. */
    if (!capture_scaled(directory, &text, "library-1920x1200-150pct", 1920, 1200, 1.5f,
                        k_titles, sizeof(k_titles) / sizeof(k_titles[0]), NULL,
                        PLAYER_UI_ID_LIBRARY_ITEM(0u)) ||
        !capture_scaled(directory, &text, "title-1920x1200-150pct", 1920, 1200, 1.5f,
                        k_titles, 0u, &title_fixture, PLAYER_UI_ID_TITLE_PRIMARY) ||
        !capture_scaled(directory, &text, "library-960x600", 960, 600, 1.0f,
                        k_titles, sizeof(k_titles) / sizeof(k_titles[0]), NULL,
                        PLAYER_UI_ID_LIBRARY_ITEM(0u)) ||
        !capture_scaled(directory, &text, "title-960x600", 960, 600, 1.0f,
                        k_titles, 0u, &title_fixture, PLAYER_UI_ID_TITLE_PRIMARY)) {
        goto done;
    }
    {
        /* TEST THIS PC at the window sizes people start in: the 960x540
           minimum, the 1280x720 default, a 1366x705 laptop and 1920x1009. */
        static const struct { const char *name; int w; int h; } sizes[] = {
            {"960x540", 960, 540}, {"1280x720", 1280, 720},
            {"1366x705", 1366, 705}, {"1920x1009", 1920, 1009}
        };
        for (size_t i = 0u; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            char name[64];
            (void)snprintf(name, sizeof(name), "library-demos-no-games-%s", sizes[i].name);
            if (!capture_scaled(directory, &text, name, sizes[i].w, sizes[i].h, 1.0f,
                                k_demo_only_titles,
                                sizeof(k_demo_only_titles) / sizeof(k_demo_only_titles[0]),
                                NULL, PLAYER_UI_ID_LIBRARY_ITEM(0u))) {
                goto done;
            }
            (void)snprintf(name, sizeof(name), "library-demos-with-games-%s", sizes[i].name);
            if (!capture_scaled(directory, &text, name, sizes[i].w, sizes[i].h, 1.0f,
                                k_demo_titles,
                                sizeof(k_demo_titles) / sizeof(k_demo_titles[0]),
                                NULL, PLAYER_UI_ID_LIBRARY_ITEM(2u))) {
                goto done;
            }
        }
    }
    ok = true;
done:
    if (!ok && SDL_GetError()[0] != '\0') {
        fprintf(stderr, "FAIL: screen capture failed: %s\n", SDL_GetError());
    }
    nk_ui_sdlr_destroy(backend);
    nk_ui_destroy(ui);
    nk_ui_prompt_images_free(&prompt_images);
    free(icon_art.rgba_pm);
    free(picture_art.rgba_pm);
    nk_ui_ttf_destroy(ttf);
    if (renderer != NULL) SDL_DestroyRenderer(renderer);
    if (surface != NULL) SDL_DestroySurface(surface);
    SDL_Quit();
    if (!ok) return 1;
    printf("synthetic Library and Title detail captures saved in %s\n", directory);
    return 0;
}
#else
static int capture_screens(const char *directory) {
    (void)directory;
    puts("SKIP: screen captures need SDL3 and SDL3_ttf headers");
    return 77;
}
#endif

/* #1297: the title page of a game that runs now. Failing before: the package
   card said "not ready yet" under a READY pill (its text followed the primary
   action, STOP GAME), the decryption note showed on a built package, and the
   compatibility card carried a "Boots to a named boundary" chip. */
/* Launcher review B6: focus lands on the page's main action. Opening a title
   from a focused library card focuses PLAY (or BUILD PACKAGE), not the first
   control drawn (the LIBRARY tab); with that action off it lands on the next
   enabled action, still not a tab. A running build focuses nothing, so a
   stray Enter or South cannot cancel it, and a move reaches CANCEL BUILD. */
static void test_default_focus_lands_on_the_main_action(void) {
    NkUiConfig config;
    NkUi *ui;
    NkUiTextBackend text = screen_test_text_backend();
    PlayerUiLibraryModel library;
    PlayerUiTitleModel title;
    PlayerUiTitle off = k_titles[1];
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create the default-focus UI");
    if (ui == NULL) return;
    set_test_viewport(ui, 1280, 800);
    memset(&library, 0, sizeof(library));
    library.page_size = player_ui_library_page_size(1280.0f, 800.0f, &library.columns);
    library.titles = k_titles;
    library.title_count = 3u;
    library.logical_width = 1280.0f;
    library.logical_height = 800.0f;
    nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(0u));
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 0.0);
        (void)player_ui_screen_library(ui, &library);
        (void)nk_ui_end(ui);
    }
    CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_LIBRARY_ITEM(0u), "the first card has focus");
    memset(&title, 0, sizeof(title));
    title.title = &k_titles[0];
    title.logical_width = 1280.0f;
    title.logical_height = 800.0f;
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 0.0);
        (void)player_ui_screen_title(ui, &title);
        (void)nk_ui_end(ui);
    }
    CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_TITLE_PRIMARY,
          "opening a title focuses PLAY, not the LIBRARY tab");

    off.primary_action_enabled = false;
    title.title = &off;
    nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(1u));
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 0.0);
        (void)player_ui_screen_title(ui, &title);
        (void)nk_ui_end(ui);
    }
    CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_TITLE_SAVE_FOLDER,
          "with BUILD PACKAGE off, focus lands on the next action, not a tab");

    {
        PlayerUiBuildModel build;
        NkUiEvent move;
        memset(&build, 0, sizeof(build));
        build.title_line = "Synthetic Showcase Scene (TEST00007)";
        build.stage = 2;
        build.fraction = -1.0f;
        build.message = "Compiling the translated game for this PC.";
        build.logical_width = 1280.0f;
        build.logical_height = 800.0f;
        nk_ui_focus(ui, 0u);
        for (int frame = 0; frame < 2; ++frame) {
            nk_ui_begin(ui, 0.0);
            (void)player_ui_screen_build(ui, &build);
            (void)nk_ui_end(ui);
        }
        CHECK(nk_ui_focused(ui) != PLAYER_UI_ID_BUILD_ACTION,
              "CANCEL BUILD is never the default focus");
        memset(&move, 0, sizeof(move));
        move.type = NK_UI_EV_NAV;
        move.dir = NK_UI_DIR_DOWN;
        nk_ui_push_event(ui, &move);
        nk_ui_begin(ui, 0.0);
        (void)player_ui_screen_build(ui, &build);
        (void)nk_ui_end(ui);
        CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_BUILD_ACTION,
              "a move from no focus reaches CANCEL BUILD");
        build.built = true;
        nk_ui_focus(ui, 0u);
        for (int frame = 0; frame < 2; ++frame) {
            nk_ui_begin(ui, 0.0);
            (void)player_ui_screen_build(ui, &build);
            (void)nk_ui_end(ui);
        }
        CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_BUILD_ACTION,
              "a finished build focuses BACK TO LIBRARY");
    }
    nk_ui_destroy(ui);
}

/* The owner: STOP GAME once. While the no-new-frames card is up, its STOP
   GAME is the only one; the primary row drops its own. */
static void test_stalled_game_offers_stop_once(void) {
    static char dump[CASE_DUMP_CAP];
    NkUiConfig config;
    NkUi *ui;
    NkUiTextBackend text = screen_test_text_backend();
    PlayerUiTitle running = k_titles[0];
    PlayerUiTitleModel model;
    size_t stops = 0u;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create the stalled-game title UI");
    if (ui == NULL) return;
    set_test_viewport(ui, 1280, 800);
    running.library_status = "Running";
    running.package_state = PLAYER_UI_PACKAGE_RUNNING;
    running.primary_action_label = "STOP GAME";
    running.primary_action = PLAYER_UI_PRIMARY_STOP_GAME;
    memset(&model, 0, sizeof(model));
    model.title = &running;
    model.stalled_game_notice_visible = true;
    model.logical_width = 1280.0f;
    model.logical_height = 800.0f;
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 0.0);
        (void)player_ui_screen_title(ui, &model);
        (void)nk_ui_end(ui);
    }
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    for (const char *at = strstr(dump, "STOP GAME"); at != NULL; at = strstr(at + 1, "STOP GAME")) {
        ++stops;
    }
    CHECK(stops == 1u, "a stalled game's page offers STOP GAME once");
    CHECK(strstr(dump, "KEEP WAITING") != NULL, "the stall card keeps Keep waiting");
    CHECK(strstr(dump, "hasn't drawn anything new") != NULL, "a no-new-frames card says so");
    /* #308: frames keep coming but show nothing: the same card, worded for a black screen. */
    model.stalled_game_dark_picture = true;
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 0.0);
        (void)player_ui_screen_title(ui, &model);
        (void)nk_ui_end(ui);
    }
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "only a black screen") != NULL, "a dark-picture card names the black screen");
    CHECK(strstr(dump, "hasn't drawn anything new") == NULL, "a dark-picture card drops the no-frames text");
    CHECK(strstr(dump, "KEEP WAITING") != NULL, "the dark-picture card keeps Keep waiting");
    nk_ui_destroy(ui);
}

static void test_title_with_a_running_game(void) {
    static char dump[CASE_DUMP_CAP];
    NkUiConfig config;
    NkUi *ui;
    NkUiTextBackend text = screen_test_text_backend();
    PlayerUiTitle running = k_titles[0];
    PlayerUiTitleModel model;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create the running-game title UI");
    if (ui == NULL) return;
    set_test_viewport(ui, 1280, 800);
    running.library_status = "Running";
    running.package_state = PLAYER_UI_PACKAGE_RUNNING;
    running.primary_action_label = "STOP GAME";
    running.primary_action = PLAYER_UI_PRIMARY_STOP_GAME;
    running.input_note =
        "Nakagawa decrypts this game's program from your own disc, on this PC.";
    running.compatibility_expect = "Plays full matches and Story Mode.";
    running.compatibility_known_issues =
        "Known issues: low frame rate, sound drops out, and movies show black.";
    running.compatibility_category = NULL;
    running.compatibility_tier = PLAYER_UI_TIER_PLAYABLE;
    memset(&model, 0, sizeof(model));
    model.title = &running;
    model.logical_width = 1280.0f;
    model.logical_height = 800.0f;
    nk_ui_begin(ui, 0.0);
    (void)player_ui_screen_title(ui, &model);
    CHECK(nk_ui_end(ui) != NULL, "draw the running-game title");
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    {
        /* The owner: "Running" once on the page. The header badge says it; the
           package card says the package is ready and in use. */
        size_t running = 0u;
        for (const char *at = strstr(dump, "RUNNING"); at != NULL; at = strstr(at + 1, "RUNNING")) {
            ++running;
        }
        CHECK(running == 1u, "a running game's title page says RUNNING once");
    }
    CHECK(strstr(dump, "Built, in use by the running game.") != NULL,
          "the package card says the game runs now");
    CHECK(strstr(dump, "not ready yet") == NULL,
          "a running game's ready package is never called not ready");
    CHECK(strstr(dump, "Starting Synthetic") == NULL,
          "a game whose window is up has no starting card");
    CHECK(strstr(dump, "decrypts this game's program") == NULL,
          "the decryption note stays off a running game's built package");
    CHECK(strstr(dump, "Plays full matches and Story Mode.") != NULL &&
              strstr(dump, "Known issues: low frame rate") != NULL,
          "the compatibility card says what to expect and the known issues");
    CHECK(strstr(dump, "PLAYABLE") != NULL || strstr(dump, "Playable") != NULL,
          "the compatibility card carries the player tier chip");
    CHECK(strstr(dump, "named boundary") == NULL && strstr(dump, "NAMED BOUNDARY") == NULL &&
              strstr(dump, "COMPATIBILITY.md") == NULL,
          "the compatibility card names no docs category outside the developer view");
    /* #1289: a neutral no-frame observation stays actionable while the game
       process remains alive; choosing either button returns its real action. */
    model.stalled_game_notice_visible = true;
    nk_ui_begin(ui, 0.0);
    (void)player_ui_screen_title(ui, &model);
    (void)nk_ui_end(ui);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "The game hasn't drawn anything new for 30 seconds. It may be stuck.") != NULL,
          "the running title displays the no-frame notice");
    CHECK(strstr(dump, "KEEP WAITING") != NULL && strstr(dump, "STOP GAME") != NULL,
          "the running title offers both no-frame choices");
    PlayerUiAction keep_waiting = activate_title_button(
        PLAYER_UI_ID_TITLE_STALL_KEEP_WAITING, &model);
    CHECK(keep_waiting.kind == PLAYER_UI_ACTION_STALL_KEEP_WAITING,
          "Keep waiting returns the live-session action");
    PlayerUiAction stop_game = activate_title_button(
        PLAYER_UI_ID_TITLE_STALL_STOP_GAME, &model);
    CHECK(stop_game.kind == PLAYER_UI_ACTION_STALL_STOP_GAME,
          "Stop game returns the stop-session action");
    model.stalled_game_notice_visible = false;
    running.compatibility_tier = PLAYER_UI_TIER_NONE;
    nk_ui_begin(ui, 0.0);
    (void)player_ui_screen_title(ui, &model);
    (void)nk_ui_end(ui);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "PLAYABLE") == NULL && strstr(dump, "Playable") == NULL,
          "a title with no tier draws no tier chip");
    running.compatibility_tier = PLAYER_UI_TIER_PLAYABLE;
    /* The same game while its window is not up yet, then with its package
       missing: the note comes back only where decrypted files are needed. */
    running.package_state = PLAYER_UI_PACKAGE_STARTING;
    nk_ui_begin(ui, 0.0);
    (void)player_ui_screen_title(ui, &model);
    (void)nk_ui_end(ui);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "Starting the game. Its window opens when it is ready.") != NULL &&
              strstr(dump, "decrypts this game's program") == NULL,
          "a starting game gets the starting card and no decryption note");
    /* The showcase walkthrough: the first start waited 7 to 9 s with only a
       chip and that line. The main column names the game being started. */
    CHECK(strstr(dump, "Starting Synthetic Showcase Scene\xE2\x80\xA6") != NULL &&
              strstr(dump, "The first start can take several seconds.") != NULL,
          "a starting game gets a card naming it");
    running.package_state = PLAYER_UI_PACKAGE_MISSING;
    running.library_status = "Needs setup";
    nk_ui_begin(ui, 0.0);
    (void)player_ui_screen_title(ui, &model);
    (void)nk_ui_end(ui);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "not ready yet") != NULL && strstr(dump, "decrypts this game's program") != NULL,
          "a missing package keeps its text and the decryption note");
    running.compatibility_category = "Boots to a named boundary";
    nk_ui_begin(ui, 0.0);
    (void)player_ui_screen_title(ui, &model);
    (void)nk_ui_end(ui);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "Developer view: \\\"Boots to a named boundary\\\"") != NULL ||
              strstr(dump, "Developer view: \"Boots to a named boundary\"") != NULL,
          "the developer view still names the category");
    nk_ui_destroy(ui);
}

static PlayerUiAction activate_remove_button(NkUiId id, const PlayerUiRemoveModel *model) {
    NkUiConfig config;
    NkUiEvent activate;
    NkUi *ui;
    PlayerUiAction result = {PLAYER_UI_ACTION_NONE, PLAYER_UI_PRIMARY_NONE, 0u};
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    ui = nk_ui_create(&config);
    if (ui == NULL) return result;
    set_test_viewport(ui, 1280, 800);
    nk_ui_focus(ui, id);
    memset(&activate, 0, sizeof(activate));
    activate.type = NK_UI_EV_ACTIVATE;
    activate.down = true;
    nk_ui_push_event(ui, &activate);
    nk_ui_begin(ui, 0.0);
    result = player_ui_screen_remove(ui, model);
    (void)nk_ui_end(ui);
    nk_ui_destroy(ui);
    return result;
}

/* #1304: Remove from library sits on a disc's title page, never a showcase
   demo's, and asks first: the dialog lists what Nakagawa built with sizes,
   says the disc image stays, and keeps the saves unless their box is ticked. */
static void test_remove_from_library(void) {
    static char dump[CASE_DUMP_CAP];
    static const PlayerUiRemoveItem items[] = {
        {"Built package", 3u * 1024u * 1024u}, {"Build cache", 512u * 1024u},
        {"Disc files Nakagawa copied", 40u * 1024u * 1024u}
    };
    NkUiConfig config;
    NkUi *ui;
    NkUiTextBackend text = screen_test_text_backend();
    PlayerUiTitleModel title;
    PlayerUiRemoveModel remove;
    PlayerUiAction action;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create the remove-from-library UI");
    if (ui == NULL) return;
    set_test_viewport(ui, 1280, 800);
    memset(&title, 0, sizeof(title));
    title.title = &k_titles[3];
    title.title_index = 3u;
    title.logical_width = 1280.0f;
    title.logical_height = 800.0f;
    nk_ui_begin(ui, 0.0);
    (void)player_ui_screen_title(ui, &title);
    (void)nk_ui_end(ui);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "REMOVE FROM LIBRARY") != NULL, "a disc's title page offers removal");
    action = activate_title_button(PLAYER_UI_ID_TITLE_REMOVE, &title);
    CHECK(action.kind == PLAYER_UI_ACTION_REMOVE_TITLE && action.title_index == 3u,
          "Remove from library asks for the dialog for that title");
    title.title = &k_titles[0];
    nk_ui_begin(ui, 0.0);
    (void)player_ui_screen_title(ui, &title);
    (void)nk_ui_end(ui);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "REMOVE FROM LIBRARY") == NULL,
          "a showcase demo's title page offers no removal");

    memset(&remove, 0, sizeof(remove));
    remove.title = "Synthetic Untested Fixture";
    remove.disc_id = "TEST00005";
    remove.title_index = 3u;
    remove.items = items;
    remove.item_count = sizeof(items) / sizeof(items[0]);
    remove.has_saves = true;
    remove.saves_bytes = 2048u;
    remove.logical_width = 1280.0f;
    remove.logical_height = 800.0f;
    nk_ui_begin(ui, 0.0);
    (void)player_ui_screen_remove(ui, &remove);
    (void)nk_ui_end(ui);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "Remove Synthetic Untested Fixture?") != NULL,
          "the dialog names the game");
    CHECK(strstr(dump, "Built package") != NULL && strstr(dump, "3.0 MB") != NULL &&
              strstr(dump, "Build cache") != NULL && strstr(dump, "512.0 KB") != NULL &&
              strstr(dump, "Disc files Nakagawa copied") != NULL &&
              strstr(dump, "40.0 MB") != NULL,
          "the dialog lists each item with its size");
    CHECK(strstr(dump, "FREES 43.5 MB") != NULL, "the dialog says how much it frees");
    CHECK(strstr(dump, "The disc image on your PC is not touched.") != NULL,
          "the dialog says the disc image stays");
    CHECK(strstr(dump, "Your saves stay.") != NULL &&
              strstr(dump, "will be deleted too") == NULL,
          "the saves box starts unticked and the saves stay");
    action = activate_remove_button(PLAYER_UI_ID_REMOVE_CONFIRM, &remove);
    CHECK(action.kind == PLAYER_UI_ACTION_REMOVE_CONFIRM && action.title_index == 3u &&
              action.primary == PLAYER_UI_PRIMARY_NONE,
          "Remove without the box keeps the saves");
    action = activate_remove_button(PLAYER_UI_ID_REMOVE_SAVES, &remove);
    CHECK(action.kind == PLAYER_UI_ACTION_REMOVE_TOGGLE_SAVES, "the saves box toggles");
    action = activate_remove_button(PLAYER_UI_ID_REMOVE_CANCEL, &remove);
    CHECK(action.kind == PLAYER_UI_ACTION_BACK, "Cancel leaves the dialog");
    remove.delete_saves = true;
    nk_ui_begin(ui, 0.0);
    (void)player_ui_screen_remove(ui, &remove);
    (void)nk_ui_end(ui);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "Your saves for this game will be deleted too.") != NULL,
          "a ticked box says the saves go too");
    action = activate_remove_button(PLAYER_UI_ID_REMOVE_CONFIRM, &remove);
    CHECK(action.kind == PLAYER_UI_ACTION_REMOVE_CONFIRM && action.primary == 1,
          "Remove with the box ticked deletes the saves");
    remove.item_count = 0u;
    remove.has_saves = false;
    remove.delete_saves = false;
    nk_ui_begin(ui, 0.0);
    (void)player_ui_screen_remove(ui, &remove);
    (void)nk_ui_end(ui);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "Nakagawa has not built anything for it yet.") != NULL &&
              strstr(dump, "This game has no saves.") != NULL &&
              strstr(dump, "The disc image on your PC is not touched.") != NULL,
          "a game with nothing built says so and still promises the disc image");
    nk_ui_destroy(ui);
}

/* The walkthrough: the wizard's chips promised Fonts and Ready steps that a
   first run never shows (copying the files ends on the library card). Every
   step a first run shows names only the steps on that route. */
static void test_wizard_steps_match_the_route(void) {
    static char dump[CASE_DUMP_CAP];
    static const FlowCase route[] = {FLOW_WIZ_WELCOME, FLOW_WIZ_OPTIONS, FLOW_WIZ_CHOOSE,
                                     FLOW_WIZ_VERIFY, FLOW_WIZ_COPYING};
    for (size_t i = 0u; i < sizeof(route) / sizeof(route[0]); ++i) {
        CHECK(dump_flow_case(route[i], dump, sizeof(dump)), "lay out a wizard step");
        CHECK(strstr(dump, "FONTS\"") == NULL && strstr(dump, "READY\"") == NULL,
              "no Fonts or Ready chip promises a step the first run never shows");
        CHECK(strstr(dump, "WELCOME\"") != NULL && strstr(dump, "OPTIONS\"") != NULL &&
                  strstr(dump, "GAME\"") != NULL && strstr(dump, "VERIFY\"") != NULL,
              "the chips name Welcome, Options, Game and Verify");
    }
    CHECK(dump_flow_case(FLOW_WIZ_WELCOME, dump, sizeof(dump)) &&
              strstr(dump, "\"4  VERIFY\"") != NULL,
          "Verify is the last of four steps");
}

/* The walkthrough: lines that report something that worked, or nothing at
   all, were drawn in the warning colour. Only a warning is amber. */
static void test_outcome_lines_take_their_colour(void) {
    static char dump[CASE_DUMP_CAP];
    char expect[96];
    const NkUiTheme *theme = nk_ui_theme_n1();
    (void)snprintf(expect, sizeof(expect), "%08X \"Imported 3 fonts", (unsigned)theme->ready);
    CHECK(dump_flow_case(FLOW_SETTINGS_FONTS, dump, sizeof(dump)) && strstr(dump, expect) != NULL,
          "a font import that worked reads in the ready colour");
    (void)snprintf(expect, sizeof(expect), "%08X \"Importing reads", (unsigned)theme->muted);
    CHECK(dump_flow_case(FLOW_WIZ_FONTS, dump, sizeof(dump)) && strstr(dump, expect) != NULL,
          "the Fonts step's hint, before any import, is muted");
    (void)snprintf(expect, sizeof(expect), "%08X \"Choose a disc image below",
                   (unsigned)theme->muted);
    CHECK(dump_flow_case(FLOW_WIZ_CHOOSE, dump, sizeof(dump)) && strstr(dump, expect) != NULL,
          "the Game step with nothing chosen yet shows the muted hint");
}

static PlayerUiAction activate_stop_button(NkUiId id, const PlayerUiStopModel *model) {
    NkUiConfig config;
    NkUiEvent activate;
    NkUi *ui;
    PlayerUiAction result = {PLAYER_UI_ACTION_NONE, PLAYER_UI_PRIMARY_NONE, 0u};
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    ui = nk_ui_create(&config);
    if (ui == NULL) return result;
    set_test_viewport(ui, 1280, 800);
    nk_ui_focus(ui, id);
    memset(&activate, 0, sizeof(activate));
    activate.type = NK_UI_EV_ACTIVATE;
    activate.down = true;
    nk_ui_push_event(ui, &activate);
    nk_ui_begin(ui, 0.0);
    result = player_ui_screen_stop(ui, model);
    (void)nk_ui_end(ui);
    nk_ui_destroy(ui);
    return result;
}

/* The walkthrough: STOP GAME ended the game at once. It asks first, as
   quitting with a game running does: KEEP PLAYING is focused first, STOP
   GAME is the danger button, and Back (the router's pop) keeps playing. */
static void test_stop_game_asks_first(void) {
    static char dump[CASE_DUMP_CAP];
    NkUiConfig config;
    NkUi *ui;
    NkUiTextBackend text = screen_test_text_backend();
    NkUiRouter *router;
    NkUiEvent back;
    PlayerUiStopModel stop;
    PlayerUiAction action;
    char expect[96];
    const NkUiTheme *theme = nk_ui_theme_n1();
    memset(&stop, 0, sizeof(stop));
    stop.title = "Synthetic Showcase Scene";
    stop.title_index = 2u;
    stop.logical_width = 1280.0f;
    stop.logical_height = 800.0f;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create the stop-game UI");
    if (ui == NULL) return;
    set_test_viewport(ui, 1280, 800);
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 0.0);
        (void)player_ui_screen_stop(ui, &stop);
        (void)nk_ui_end(ui);
    }
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_STOP_KEEP,
          "Keep playing has the first focus, never Stop game");
    CHECK(strstr(dump, "\"Stop Synthetic Showcase Scene?\"") != NULL &&
              strstr(dump, "anything since its last save is lost") != NULL,
          "the dialog names the game and says what stopping loses");
    (void)snprintf(expect, sizeof(expect), "%08X \"STOP GAME\"", (unsigned)theme->error);
    CHECK(strstr(dump, expect) != NULL, "Stop game is the danger button");
    nk_ui_destroy(ui);
    action = activate_stop_button(PLAYER_UI_ID_STOP_CONFIRM, &stop);
    CHECK(action.kind == PLAYER_UI_ACTION_PRIMARY &&
              action.primary == PLAYER_UI_PRIMARY_STOP_GAME && action.title_index == 2u,
          "Stop game stops the game");
    action = activate_stop_button(PLAYER_UI_ID_STOP_KEEP, &stop);
    CHECK(action.kind == PLAYER_UI_ACTION_BACK, "Keep playing leaves the dialog");
    router = nk_ui_router_create();
    CHECK(router != NULL, "router allocates");
    if (router == NULL) return;
    CHECK(nk_ui_router_push(router, PLAYER_UI_ROUTE_TITLE) &&
              nk_ui_router_push(router, PLAYER_UI_ROUTE_STOP) &&
              nk_ui_router_current(router) == PLAYER_UI_ROUTE_STOP,
          "the question opens over the title page");
    memset(&back, 0, sizeof(back));
    back.type = NK_UI_EV_BACK;
    CHECK(nk_ui_router_handle_event(router, &back) &&
              nk_ui_router_current(router) == PLAYER_UI_ROUTE_TITLE,
          "Back (Esc or B) closes the question and the game keeps running");
    nk_ui_router_destroy(router);
}

/* The walkthrough: three of the Controls page's binding buttons ended in
   "...". The words input_settings.c writes for the defaults and for any one
   input stay within PLAYER_UI_CONTROLLER_BINDING_CHARS, and a label that long
   shows whole at every launcher size. */
static void test_binding_labels_fit(void) {
    static char dump[CASE_DUMP_CAP];
    static const struct { int w; int h; } sizes[] = {
        {1280, 720}, {960, 540}, {1366, 705}, {853, 480}, {1920, 1080}
    };
    static char widest[PLAYER_UI_CONTROLLER_BINDING_CHARS + 1u];
    char caps[64];
    memset(widest, 'W', PLAYER_UI_CONTROLLER_BINDING_CHARS);
    widest[PLAYER_UI_CONTROLLER_BINDING_CHARS] = '\0';
    for (size_t s = 0u; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        for (int keys = 0; keys < 2; ++keys) {
            const char *const *words = keys ? k_key_bindings : k_pad_bindings;
            CHECK(flow_case_fits(keys ? FLOW_CONTROLLER_KEYS : FLOW_CONTROLLER, sizes[s].w,
                                 sizes[s].h, dump, sizeof(dump)),
                  "the Controls page fits the window");
            for (size_t i = 0u; i < PLAYER_UI_CONTROLLER_BUTTONS; ++i) {
                size_t n = 0u;
                if (keys && i == 1u) continue; /* Start waits for a key in that case */
                caps[n++] = '"';
                for (const char *c = words[i]; *c != '\0' && n + 2u < sizeof(caps); ++c) {
                    caps[n++] = (*c >= 'a' && *c <= 'z') ? (char)(*c - 'a' + 'A') : *c;
                }
                caps[n++] = '"';
                caps[n] = '\0';
                CHECK(strstr(dump, caps) != NULL, "every default binding shows whole");
            }
        }
    }
    /* The widest label the words may take, in place of Select's binding. */
    k_pad_bindings_override = widest;
    for (size_t s = 0u; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        (void)flow_case_fits(FLOW_CONTROLLER, sizes[s].w, sizes[s].h, dump, sizeof(dump));
        (void)snprintf(caps, sizeof(caps), "\"%s\"", widest);
        CHECK(strstr(dump, caps) != NULL,
              "a binding of PLAYER_UI_CONTROLLER_BINDING_CHARS characters shows whole");
    }
    k_pad_bindings_override = NULL;
}

/* The walkthrough: the top bar's chip read "Controller · Letters, A south".
   It names the pad as a player would. */
static void test_input_chip_words(void) {
    CHECK(strcmp(nk_ui_input_label(NK_UI_PROMPT_KEYCAPS), "KEYBOARD AND MOUSE") == 0,
          "the keyboard chip");
    CHECK(strcmp(nk_ui_input_label(NK_UI_PROMPT_LETTERS_A_SOUTH), "XBOX-STYLE CONTROLLER") == 0,
          "A on the south button is an Xbox-style controller");
    CHECK(strcmp(nk_ui_input_label(NK_UI_PROMPT_LETTERS_A_EAST),
                 "NINTENDO-STYLE CONTROLLER") == 0,
          "A on the east button is a Nintendo-style controller");
    CHECK(strcmp(nk_ui_input_label(NK_UI_PROMPT_SHAPES), "PLAYSTATION-STYLE CONTROLLER") == 0,
          "shape labels are a PlayStation-style controller");
    CHECK(strcmp(nk_ui_input_label(NK_UI_PROMPT_NEUTRAL), "CONTROLLER") == 0,
          "any other pad is a controller");
}

/* The category -> player tier table in scr_title.c and the "Status words"
   table in docs/COMPATIBILITY.md say the same thing, row for row (#1297). */
static void test_tier_table_matches_the_docs(void) {
    static const struct { const char *category; const char *tier; } expected[] = {
        {"Works", "Perfect"},
        {"Partially works", "In-game"},
        {"Boots to a named boundary", "Title screen"},
        {"Not started", "Won't boot"},
        {"Not supported", "Won't boot"},
        {"Experimental", NULL},
        {"Untested", NULL},
        {"A word the docs never use", NULL},
        {NULL, NULL}
    };
    for (size_t i = 0u; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        const char *word = player_ui_tier_word(player_ui_tier_for_category(expected[i].category));
        CHECK((word == NULL && expected[i].tier == NULL) ||
                  (word != NULL && expected[i].tier != NULL &&
                   strcmp(word, expected[i].tier) == 0),
              "a docs category maps to its player tier");
    }
    CHECK(player_ui_tier_word(PLAYER_UI_TIER_PLAYABLE) != NULL &&
              strcmp(player_ui_tier_word(PLAYER_UI_TIER_PLAYABLE), "Playable") == 0 &&
              strcmp(player_ui_tier_word(PLAYER_UI_TIER_MENU), "Menu") == 0 &&
              player_ui_tier_word(PLAYER_UI_TIER_NONE) == NULL,
          "every tier has its word, and no tier has none");

    FILE *docs = fopen("docs/COMPATIBILITY.md", "rb");
    CHECK(docs != NULL, "open docs/COMPATIBILITY.md from the repository root");
    if (docs == NULL) return;
    char line[1024];
    bool in_table = false;
    size_t rows = 0u;
    while (fgets(line, sizeof(line), docs) != NULL) {
        if (strncmp(line, "| Status | Player tier | Meaning |", 34u) == 0) {
            in_table = true;
            continue;
        }
        if (!in_table) continue;
        if (line[0] != '|') break;
        if (strncmp(line, "| :---", 6u) == 0) continue;
        char category[96];
        char tier[64];
        const char *a = line + 2;
        const char *b = strstr(a, " | ");
        if (b == NULL || (size_t)(b - a) >= sizeof(category)) break;
        memcpy(category, a, (size_t)(b - a));
        category[b - a] = '\0';
        const char *c = b + 3;
        const char *d = strstr(c, " | ");
        if (d == NULL || (size_t)(d - c) >= sizeof(tier)) break;
        memcpy(tier, c, (size_t)(d - c));
        tier[d - c] = '\0';
        const char *word = player_ui_tier_word(player_ui_tier_for_category(category));
        if (strcmp(tier, "No tier") == 0) {
            CHECK(word == NULL, "a docs row with no tier maps to no tier");
        } else {
            CHECK(word != NULL && strcmp(word, tier) == 0,
                  "a docs row's player tier matches scr_title.c");
        }
        ++rows;
    }
    fclose(docs);
    CHECK(rows == 6u, "the docs Status words table has its six rows");
}

int main(int argc, char **argv) {
    static char library[DUMP_CAP];
    static char title[DUMP_CAP];
    size_t library_size = 0u;
    size_t title_size = 0u;
    bool write = argc > 1 && strcmp(argv[1], "--write-goldens") == 0;
    const char *capture_dir = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--capture-dir=", 14u) == 0) {
            capture_dir = argv[i] + 14u;
        }
    }
    library[0] = '\0';
    title[0] = '\0';
    test_router();
    test_screen_actions();
    test_error_details_fit_the_minimum_window();
    test_every_screen_fits_the_window();
    test_psp_fonts_page();
    test_wizard_actions();
    test_plain_cards();
    test_disc_cards();
    test_settings_pages();
    test_controller_and_developer_log_pages();
    test_controller_keyboard_page();
    test_controller_binding_glyphs();
    test_version_line();
    test_design_system_layout();
    test_title_with_a_running_game();
    test_stalled_game_offers_stop_once();
    test_default_focus_lands_on_the_main_action();
    test_tier_table_matches_the_docs();
    test_remove_from_library();
    test_wizard_steps_match_the_route();
    test_outcome_lines_take_their_colour();
    test_stop_game_asks_first();
    test_binding_labels_fit();
    test_input_chip_words();
    test_build_bar_shows_compiled_parts();
    test_library_demos();
    CHECK(build_library_golden(library, sizeof(library), &library_size),
          "build Library screen dumps");
    CHECK(build_title_golden(title, sizeof(title), &title_size),
          "build Title detail screen dumps");
    if (write && failures == 0) {
        CHECK(write_golden("fixtures/nk_ui/library.golden", library, library_size),
              "write Library screen golden");
        CHECK(write_golden("fixtures/nk_ui/title.golden", title, title_size),
              "write Title detail screen golden");
    } else if (failures == 0) {
        compare_golden("fixtures/nk_ui/library.golden", library, library_size);
        compare_golden("fixtures/nk_ui/title.golden", title, title_size);
    }
    if (capture_dir != NULL && failures == 0) {
        int capture_result = capture_screens(capture_dir);
        if (capture_result != 0 && capture_result != 77) ++failures;
    }
    if (failures != 0) return 1;
    puts("player screen snapshots and router focus tests passed");
    return 0;
}
