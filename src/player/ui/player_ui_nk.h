/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef PLAYER_UI_NK_H
#define PLAYER_UI_NK_H

#include "nk_ui.h"
#include "nk_ui_input.h"  /* NkUiPromptAction: the Controls page's binding glyphs */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PlayerApp PlayerApp;
typedef struct PlayerUiNk PlayerUiNk;
typedef struct NkUiRouter NkUiRouter;
struct NkUiImage;
struct SDL_Renderer;
struct SDL_Window;
union SDL_Event;

typedef enum {
    PLAYER_UI_ROUTE_LIBRARY = 1,
    PLAYER_UI_ROUTE_TITLE = 2,
    PLAYER_UI_ROUTE_ABOUT = 3,
    PLAYER_UI_ROUTE_REMOVE = 4,    /* confirm Remove from library (#1304) */
    /* Settings is a view over the routes, never pushed; the top bar takes it to
       show the SETTINGS tab as the active one. */
    PLAYER_UI_ROUTE_SETTINGS = 5,
    PLAYER_UI_ROUTE_STOP = 6       /* confirm STOP GAME */
} PlayerUiRoute;

/* The player's version: the one place it is written. The About page shows
   "v" NK_PLAYER_VERSION " (MM-DD-YYYY)"; nothing else (tags, releases, the
   tools' package version) reads it. */
#define NK_PLAYER_VERSION "0.0.2-dev"

typedef enum {
    PLAYER_UI_PRIMARY_NONE = 0,
    PLAYER_UI_PRIMARY_PLAY,
    PLAYER_UI_PRIMARY_BUILD,
    PLAYER_UI_PRIMARY_RETRY_PACKAGE_CHECK,
    PLAYER_UI_PRIMARY_STOP_GAME,
    PLAYER_UI_PRIMARY_REBUILD   /* an INCOMPATIBLE package: check, then build again */
} PlayerUiPrimaryAction;

/* Where this title's package stands, which picks the package card's text
   (#1297). The primary action does not: while a game runs, it is STOP GAME
   whatever the package is. */
typedef enum {
    PLAYER_UI_PACKAGE_MISSING = 0, /* not built, stale or incompatible */
    PLAYER_UI_PACKAGE_READY,
    PLAYER_UI_PACKAGE_CHECKING,
    PLAYER_UI_PACKAGE_BUILDING,
    PLAYER_UI_PACKAGE_CHECK_FAILED,
    PLAYER_UI_PACKAGE_STARTING,    /* PLAY was pressed; the game window is not up yet */
    PLAYER_UI_PACKAGE_RUNNING      /* this title is the game that runs now */
} PlayerUiPackageState;

/* The player tier on the title page's compatibility chip (#1297): the words
   players know from other compatibility lists. NONE draws no chip, for a
   title no one has tried. */
typedef enum {
    PLAYER_UI_TIER_NONE = 0,
    PLAYER_UI_TIER_WONT_BOOT,
    PLAYER_UI_TIER_TITLE_SCREEN,
    PLAYER_UI_TIER_MENU,
    PLAYER_UI_TIER_IN_GAME,
    PLAYER_UI_TIER_PLAYABLE,
    PLAYER_UI_TIER_PERFECT
} PlayerUiTier;

/* The chip word for a tier ("Perfect" ... "Won't boot"); NULL for NONE. */
const char *player_ui_tier_word(PlayerUiTier tier);
/* The tier a docs/COMPATIBILITY.md category gives a title that has no
   player record of its own; NONE for Experimental, Untested and an unknown
   word. This is the one mapping table (scr_title.c). */
PlayerUiTier player_ui_tier_for_category(const char *category);

typedef enum {
    PLAYER_UI_ACTION_NONE = 0,
    PLAYER_UI_ACTION_OPEN_TITLE,
    PLAYER_UI_ACTION_ADD_DISC,
    PLAYER_UI_ACTION_PAGE_PREVIOUS,
    PLAYER_UI_ACTION_PAGE_NEXT,
    PLAYER_UI_ACTION_BACK,
    PLAYER_UI_ACTION_PRIMARY,
    PLAYER_UI_ACTION_OPEN_SAVE_FOLDER,
    PLAYER_UI_ACTION_START_WIZARD,
    PLAYER_UI_ACTION_OPEN_SETTINGS,
    PLAYER_UI_ACTION_CANCEL_BUILD,     /* stop the running build, back to the library */
    PLAYER_UI_ACTION_LEAVE_BUILD,      /* the package is written: back to the library */
    PLAYER_UI_ACTION_PREREQ_ACCEPT,    /* download the listed build tools */
    PLAYER_UI_ACTION_PREREQ_CANCEL,
    PLAYER_UI_ACTION_ERROR_RECOVER,    /* the error card's recovery action */
    PLAYER_UI_ACTION_ERROR_RETRY,      /* Restart game / Try again (ErrorState.retry) */
    PLAYER_UI_ACTION_ERROR_COPY,       /* put the problem report on the clipboard */
    PLAYER_UI_ACTION_WIZARD,           /* a setup wizard control; .primary carries the op */
    PLAYER_UI_ACTION_OPEN_LIBRARY,
    PLAYER_UI_ACTION_OPEN_ABOUT,
    PLAYER_UI_ACTION_OPEN_NOTICES,
    PLAYER_UI_ACTION_REMOVE_TITLE,         /* the title page asks to remove it (#1304) */
    PLAYER_UI_ACTION_REMOVE_TOGGLE_SAVES,  /* the confirm screen's saves box */
    PLAYER_UI_ACTION_REMOVE_CONFIRM,       /* remove it; .primary is 1 to delete saves */
    PLAYER_UI_ACTION_STALL_KEEP_WAITING,
    PLAYER_UI_ACTION_STALL_STOP_GAME,
    PLAYER_UI_ACTION_OPEN_BUILD_INPUT_FOLDER, /* fix a package-build preflight blocker */
    PLAYER_UI_ACTION_DISC,             /* a game-found card control; .primary carries the op */
    PLAYER_UI_ACTION_SETTINGS,         /* a Settings control; .primary carries the op */
    PLAYER_UI_ACTION_CONTROLLER,       /* a controller settings control; .primary carries the
                                          op, .title_index the PSP control it rebinds */
    PLAYER_UI_ACTION_DEVELOPER_LOG     /* a developer log control; .primary carries the op */
} PlayerUiActionKind;

typedef struct {
    PlayerUiActionKind kind;
    PlayerUiPrimaryAction primary;
    size_t title_index;
} PlayerUiAction;

typedef struct {
    const char *title;
    const char *disc_id;
    const char *library_status;
    const char *library_status_detail;
    /* What to expect on this PC, in plain words, and the known issues as one
       sentence (empty when there are none) (#1297). */
    const char *compatibility_expect;
    const char *compatibility_known_issues;
    /* The docs/COMPATIBILITY.md category, for the developer view only: NULL
       or empty hides it. */
    const char *compatibility_category;
    const char *input_note;
    const char *build_preflight_message;
    const char *build_preflight_folder;
    const char *primary_action_label;
    const char *status_marker;
    uint32_t status_rgba;
    PlayerUiPrimaryAction primary_action;
    bool primary_action_enabled;
    bool save_folder_enabled;
    /* Presentation only (zero is fine): the project-drawn picture for the
       project's own showcase demos (an NkUiArtKind), and the badge over the art
       (NULL: "Your disc"). */
    unsigned art_kind;
    const char *badge;
    /* The disc's own ICON0.PNG and PIC1.PNG, decoded at run time from the
       user's disc image (never stored); NULL until read or when absent. */
    const struct NkUiImage *icon;
    const struct NkUiImage *picture;
    PlayerUiPackageState package_state;
    PlayerUiTier compatibility_tier;
    /* REMOVE FROM LIBRARY is offered: not for a showcase demo, and not while a
       game starts or runs, a package builds or a disc is copied (#1304). */
    bool can_remove;
    /* A demo that comes with Nakagawa (a showcase package beside the player),
       not one of the user's games: the Library lists it under TEST THIS PC,
       apart from the grid. */
    bool is_demo;
} PlayerUiTitle;

typedef struct {
    const PlayerUiTitle *titles;
    size_t title_count;
    size_t page_index;
    size_t page_size;
    size_t columns;
    size_t selected_index;
    float logical_width;
    const char *page_label;
    float logical_height;   /* 0 selects 800 */
    /* "Version 0.0.2 is out" after the opt-in check at start, or NULL. Its button
       opens Settings > Updates (PLAYER_UI_ACTION_SETTINGS). */
    const char *update_notice;
} PlayerUiLibraryModel;

/* What a setup wizard control asks for (PLAYER_UI_ACTION_WIZARD; the op rides
   in PlayerUiAction.primary). main.c runs the same handlers the existing
   renderer's wizard buttons call. */
typedef enum {
    PLAYER_UI_WIZARD_NONE = 0,
    PLAYER_UI_WIZARD_NEXT,
    PLAYER_UI_WIZARD_BACK,
    PLAYER_UI_WIZARD_CANCEL,             /* leave the wizard (skip to library, cancel) */
    PLAYER_UI_WIZARD_CHOOSE_ISO,         /* open the file picker */
    PLAYER_UI_WIZARD_CHOOSE_DIFFERENT,   /* back to Select Game and open the picker */
    PLAYER_UI_WIZARD_ADD_EXPERIMENTAL,   /* add the inspected experimental title */
    PLAYER_UI_WIZARD_ACCEPT_FONTS,       /* confirm the fonts step and continue */
    PLAYER_UI_WIZARD_CYCLE_RESOLUTION,
    PLAYER_UI_WIZARD_IMPORT_FONTS,       /* open the font folder picker */
    PLAYER_UI_WIZARD_REMOVE_FONTS,
    PLAYER_UI_WIZARD_FINISH,             /* save to the library, and launch when ready */
    PLAYER_UI_WIZARD_GO_TO_LIBRARY,
    PLAYER_UI_WIZARD_TOGGLE_ONLINE,      /* the options step's two switches */
    PLAYER_UI_WIZARD_TOGGLE_DEVELOPER,
    PLAYER_UI_WIZARD_SKIP_OPTIONS        /* both off, on to the game step */
} PlayerUiWizardOp;

typedef struct {
    const char *status;   /* OK, MISSING, ... */
    const char *code;
    const char *message;
    uint32_t rgba;        /* the status word's colour */
} PlayerUiCheckRow;

#define PLAYER_UI_WIZARD_FONT_LINES 24u
#define PLAYER_UI_WIZARD_CHECKS 10u

/* What a one-line outcome says, which picks its colour: muted, ready,
   warning or error (PlayerNoteKind in player_state.h, in the same order). */
typedef enum {
    PLAYER_UI_NOTE_NEUTRAL = 0,
    PLAYER_UI_NOTE_DONE,
    PLAYER_UI_NOTE_WARNING,
    PLAYER_UI_NOTE_ERROR
} PlayerUiNoteKind;

/* The setup wizard's steps that a first run shows: Welcome, Online and
   developer options, Select game and Verify. Copying the files ends on the
   library card, and the fonts are imported in Settings > PSP fonts. */
#define PLAYER_UI_WIZARD_ROUTE_STEPS 4

/* The setup wizard (VIEW_SETUP_WIZARD). step is 0-5: Welcome, Online and
   developer options, Select game, Verify, then Fonts and system and Ready,
   which only the older state-only callers reach. */
typedef struct {
    int step;
    /* Select game */
    bool has_iso;
    bool iso_selected;
    const char *iso_path;
    const char *iso_detail;         /* "Size: ... · Disc ID: ..." */
    const char *status_message;
    /* Verify */
    bool extracting;
    /* The progress is a PSN package turning back into its disc image (#295). */
    bool converting_package;
    bool extraction_complete;
    bool extraction_failed;
    bool experimental;
    bool recorded;   /* experimental, but on the compatibility list (Hot Shots Tennis) */
    bool supported;
    /* Experimental, but this version cannot use the disc (#1280): the reason
       and its next step, and no Add to library. NULL when it can be added. */
    const char *blocked_reason;
    /* Why the copy failed, without its code (#1278). */
    const char *extraction_error;
    const char *disc_id;
    const char *title_name;
    PlayerUiCheckRow checks[PLAYER_UI_WIZARD_CHECKS];
    size_t check_count;
    int files_extracted;
    int total_files;
    int extraction_percent;
    const char *current_file;
    /* Fonts and system */
    const char *resolution_line;
    const char *controller_line;
    const char *font_lines[PLAYER_UI_WIZARD_FONT_LINES];
    size_t font_line_count;
    const char *font_message;
    PlayerUiNoteKind font_message_kind;
    /* Ready */
    bool runtime_ready;
    const char *summary_line;
    /* Online and developer options */
    bool online_mode;
    bool developer_mode;
    float logical_width;
    float logical_height;
} PlayerUiWizardModel;

/* The card a disc gets when it is added from the library: checking it
   (VIEW_INSPECTING), then found (VIEW_SUPPORTED_TITLE), found but not tested
   (VIEW_EXPERIMENTAL_TITLE, blocked when this version cannot use it), or not
   on the list (VIEW_UNSUPPORTED_TITLE). */
typedef enum {
    PLAYER_UI_DISC_CHECKING = 0,
    PLAYER_UI_DISC_SUPPORTED,
    PLAYER_UI_DISC_UNTESTED,
    PLAYER_UI_DISC_BLOCKED,
    PLAYER_UI_DISC_UNSUPPORTED
} PlayerUiDiscKind;

/* What a game-found card control asks for (PLAYER_UI_ACTION_DISC). */
typedef enum {
    PLAYER_UI_DISC_NONE = 0,
    PLAYER_UI_DISC_ADD,             /* add the disc to the library */
    PLAYER_UI_DISC_BACK,            /* back to the library, nothing added */
    PLAYER_UI_DISC_CHOOSE_ANOTHER   /* open the file picker */
} PlayerUiDiscOp;

#define PLAYER_UI_DISC_CHECKS 10u

typedef struct {
    PlayerUiDiscKind kind;
    const char *title_name;
    const char *disc_id;
    const char *iso_path;           /* checking: the file being read */
    const char *reason;             /* blocked: why, and what to do next */
    const char *profile_note;       /* a refused local title profile, in words */
    const char *profile_details;    /* its refusal, shown as details */
    PlayerUiCheckRow checks[PLAYER_UI_DISC_CHECKS];
    size_t check_count;
    double now_s;                   /* drives the checking bar (0 holds it still) */
    float logical_width;
    float logical_height;
} PlayerUiDiscModel;

/* The Settings pages: the main page (VIEW_SETTINGS), online and developer
   options (VIEW_ONLINE_SETTINGS), the third-party notices for downloaded build
   tools (VIEW_PREREQ_ABOUT), the build tool removal check
   (VIEW_CONFIRM_REMOVE_TOOLS), build tools and cache (VIEW_BUILD_CACHE, #1306),
   the page that opens the removal check, and Updates (VIEW_UPDATES). */
typedef enum {
    PLAYER_UI_SETTINGS_PAGE_MAIN = 0,
    PLAYER_UI_SETTINGS_PAGE_ONLINE,
    PLAYER_UI_SETTINGS_PAGE_NOTICES,
    PLAYER_UI_SETTINGS_PAGE_REMOVE_TOOLS,
    PLAYER_UI_SETTINGS_PAGE_BUILD_CACHE,
    PLAYER_UI_SETTINGS_PAGE_FONTS,
    PLAYER_UI_SETTINGS_PAGE_UPDATES,      /* VIEW_UPDATES: check for updates */
    PLAYER_UI_SETTINGS_PAGE_FACTORY_RESET /* VIEW_CONFIRM_FACTORY_RESET */
} PlayerUiSettingsPage;

/* What a Settings control asks for (PLAYER_UI_ACTION_SETTINGS). main.c runs the
   same player_state handlers the existing renderer's Settings buttons call. */
typedef enum {
    PLAYER_UI_SETTINGS_NONE = 0,
    PLAYER_UI_SETTINGS_CYCLE_RESOLUTION,
    PLAYER_UI_SETTINGS_TOGGLE_PACING,
    PLAYER_UI_SETTINGS_TOGGLE_VSYNC,
    PLAYER_UI_SETTINGS_TOGGLE_FULLSCREEN,
    PLAYER_UI_SETTINGS_CYCLE_SCALING,
    PLAYER_UI_SETTINGS_VOLUME_DOWN,
    PLAYER_UI_SETTINGS_VOLUME_UP,
    PLAYER_UI_SETTINGS_TOGGLE_UI_SOUNDS,
    PLAYER_UI_SETTINGS_TOGGLE_REDUCE_MOTION,
    PLAYER_UI_SETTINGS_TOGGLE_LAUNCHER_FULLSCREEN,
    PLAYER_UI_SETTINGS_OPEN_CONTROLLER,
    PLAYER_UI_SETTINGS_OPEN_ONLINE,
    PLAYER_UI_SETTINGS_OPEN_NOTICES,
    PLAYER_UI_SETTINGS_OPEN_REMOVE_TOOLS,
    PLAYER_UI_SETTINGS_DONE,              /* save, back to the library */
    PLAYER_UI_SETTINGS_BACK,              /* a Settings page, back to the main page */
    PLAYER_UI_SETTINGS_TOGGLE_ONLINE,
    PLAYER_UI_SETTINGS_TOGGLE_NEWS,
    PLAYER_UI_SETTINGS_TOGGLE_REPORTS,
    PLAYER_UI_SETTINGS_TOGGLE_DEVELOPER,
    PLAYER_UI_SETTINGS_OPEN_DEVELOPER_LOG,
    PLAYER_UI_SETTINGS_OPEN_LICENSE_TEXTS,
    PLAYER_UI_SETTINGS_REMOVE_TOOLS,
    PLAYER_UI_SETTINGS_OPEN_BUILD_CACHE,
    PLAYER_UI_SETTINGS_TOGGLE_SHARE_LIBRARIES,
    PLAYER_UI_SETTINGS_CLEAR_SHARED_LIBRARIES,
    PLAYER_UI_SETTINGS_KEEP_TOOLS,         /* the removal check, back to build tools and cache */
    PLAYER_UI_SETTINGS_OPEN_FONTS,         /* Settings > PSP fonts */
    PLAYER_UI_SETTINGS_IMPORT_FONTS,       /* the folder picker, then the import */
    PLAYER_UI_SETTINGS_REMOVE_FONTS,       /* remove the imported fonts */
    PLAYER_UI_SETTINGS_CHECK_UPDATES,      /* open Updates and ask GitHub now */
    PLAYER_UI_SETTINGS_OPEN_UPDATES,       /* open Updates as it is (the library notice) */
    PLAYER_UI_SETTINGS_TOGGLE_UPDATE_AT_START,
    PLAYER_UI_SETTINGS_DOWNLOAD_UPDATE,
    PLAYER_UI_SETTINGS_CANCEL_UPDATE,      /* stop the download */
    PLAYER_UI_SETTINGS_INSTALL_UPDATE,
    PLAYER_UI_SETTINGS_OPEN_UPDATE_FOLDER, /* the downloads or the new version's folder */
    PLAYER_UI_SETTINGS_OPEN_RELEASE_PAGE,  /* the release page, in the browser */
    PLAYER_UI_SETTINGS_OPEN_FACTORY_RESET, /* the factory reset check */
    PLAYER_UI_SETTINGS_FACTORY_RESET       /* move the user data aside and start afresh */
} PlayerUiSettingsOp;

#define PLAYER_UI_SETTINGS_TOOLS 16u
#define PLAYER_UI_RESET_FOLDERS 5u   /* PLAYER_FACTORY_RESET_FOLDERS */

/* Where Check for updates is, in the order of player_state's PlayerUpdatePhase. */
typedef enum {
    PLAYER_UI_UPDATE_IDLE = 0,
    PLAYER_UI_UPDATE_CHECKING,
    PLAYER_UI_UPDATE_CURRENT,
    PLAYER_UI_UPDATE_AVAILABLE,
    PLAYER_UI_UPDATE_DOWNLOADING,
    PLAYER_UI_UPDATE_DOWNLOADED,
    PLAYER_UI_UPDATE_INSTALLING,
    PLAYER_UI_UPDATE_INSTALLED,
    PLAYER_UI_UPDATE_FAILED
} PlayerUiUpdateStep;

typedef struct {
    PlayerUiSettingsPage page;
    /* Main page: the current value of each control, in words */
    const char *resolution;         /* "2x (960x544)" */
    const char *pacing;             /* "PSP timing" */
    const char *scaling;            /* "Sharp" */
    bool vsync;
    bool fullscreen;
    int volume;                     /* 0-100 */
    bool ui_sounds;
    bool reduce_motion;
    bool launcher_fullscreen;
    const char *controller_line;    /* "Keyboard ready" or the pad's name */
    const char *saves_folder;
    const char *notice;             /* a settings notice (a failed save), or NULL */
    /* Build tools and cache: builds share compiled system libraries (#1306) */
    bool share_system_libraries;
    /* Updates (VIEW_UPDATES): what the last step found, in words */
    PlayerUiUpdateStep update_step;
    PlayerUiUpdateStep update_failed_step;   /* CHECKING, DOWNLOADING or INSTALLING */
    bool check_updates_at_start;
    const char *current_version;    /* "0.0.2-dev" */
    const char *update_version;     /* "0.0.2", once checked */
    bool update_prerelease;
    const char *update_published;   /* "2026-11-04", or "" */
    const char *update_notes;
    const char *update_page_url;
    bool update_has_zip;
    bool update_can_download;       /* a Windows ZIP and a Windows build */
    const char *update_zip_name;
    unsigned long long update_zip_bytes;
    unsigned long long update_received_bytes;
    const char *update_folder;      /* the downloads folder or the new version's folder */
    const char *update_error_title;
    const char *update_error_message;
    /* Online and developer options */
    bool online_mode;
    bool online_news;
    bool online_reports;
    bool developer_mode;
    /* Third-party notices: "name version · licence" per downloaded tool */
    const char *tools[PLAYER_UI_SETTINGS_TOOLS];
    size_t tool_count;
    /* PSP fonts: one status line per slot (Japanese, the 16 Latin indices,
       Korean) and the last import's result */
    const char *font_lines[18];
    size_t font_line_count;
    const char *font_message;
    PlayerUiNoteKind font_message_kind;
    /* Factory reset (VIEW_CONFIRM_FACTORY_RESET): the folders that move aside */
    const char *reset_folders[PLAYER_UI_RESET_FOLDERS];
    size_t reset_folder_count;
    float logical_width;
    float logical_height;
} PlayerUiSettingsModel;

/* The controller settings page (VIEW_CONTROLLER_SETTINGS): the controller or
   keyboard mapping, or a step of the guided stick and trigger calibration while
   one runs. */
typedef enum {
    PLAYER_UI_CONTROLLER_PAGE_MAPPING = 0,
    PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_REST,      /* step 1: leave the stick alone */
    PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_EXTREMES,  /* step 2: move it to its limits */
    PLAYER_UI_CONTROLLER_PAGE_CALIBRATE_RESULT     /* step 3: accept or cancel */
} PlayerUiControllerPage;

/* What a controller settings control asks for (PLAYER_UI_ACTION_CONTROLLER).
   main.c runs the same input_settings handlers the existing renderer's
   controller buttons call. */
typedef enum {
    PLAYER_UI_CONTROLLER_NONE = 0,
    PLAYER_UI_CONTROLLER_REBIND,             /* wait for a press for .title_index, or stop */
    PLAYER_UI_CONTROLLER_DEADZONE_DOWN,
    PLAYER_UI_CONTROLLER_DEADZONE_UP,
    PLAYER_UI_CONTROLLER_TRIGGER_DOWN,
    PLAYER_UI_CONTROLLER_TRIGGER_UP,
    PLAYER_UI_CONTROLLER_CALIBRATE,
    PLAYER_UI_CONTROLLER_SAVE,
    PLAYER_UI_CONTROLLER_RESET,
    PLAYER_UI_CONTROLLER_BACK,               /* back to Settings */
    PLAYER_UI_CONTROLLER_CLOSE,              /* a top-bar tab or Start: Settings closes */
    PLAYER_UI_CONTROLLER_TOGGLE_SCOPE,       /* this game's own mapping, or the global one */
    PLAYER_UI_CONTROLLER_CALIBRATION_FINISH, /* step 2 done: show the result */
    PLAYER_UI_CONTROLLER_CALIBRATION_ACCEPT,
    PLAYER_UI_CONTROLLER_CALIBRATION_CANCEL,
    PLAYER_UI_CONTROLLER_SHOW_CONTROLLER,    /* show and edit the controller buttons */
    PLAYER_UI_CONTROLLER_SHOW_KEYBOARD       /* show and edit the keyboard keys (#1299) */
} PlayerUiControllerOp;

#define PLAYER_UI_CONTROLLER_BUTTONS 14u   /* INPUT_CONTROL_DIGITAL_COUNT */
/* The characters a binding's button shows whole at every launcher size: the
   label is mono caps, so this is a width (INPUT_SETTINGS_BINDING_TEXT_MAX). */
#define PLAYER_UI_CONTROLLER_BINDING_CHARS 20u
#define PLAYER_UI_CONTROLLER_LIVE_LINES 4u

/* A binding's prompt glyph (#1299): the shape `action` has in the model's glyph
   style, with `label` in place of the action's own text when set. */
typedef struct {
    bool shown;                     /* false: unbound, no glyph */
    NkUiPromptAction action;
    const char *label;              /* a key's legend, a pad button's name; NULL: the action's */
} PlayerUiBindingGlyph;

typedef struct {
    PlayerUiControllerPage page;
    bool keyboard;                  /* the mapping shows the keyboard keys, not the controller */
    /* Mapping: each PSP control's name and what it is bound to, in words */
    const char *names[PLAYER_UI_CONTROLLER_BUTTONS];
    const char *bindings[PLAYER_UI_CONTROLLER_BUTTONS];
    /* and as the glyph the device shown prints: keycaps for the keys, the pad's own set
       for the controller */
    NkUiPromptStyle glyph_style;
    PlayerUiBindingGlyph glyphs[PLAYER_UI_CONTROLLER_BUTTONS];
    bool conflicted[PLAYER_UI_CONTROLLER_BUTTONS];
    int capturing;                  /* the control waiting for a press, or -1 */
    const char *scope;              /* "global mapping", "TEST00005 only" */
    bool scope_title;               /* editing one game's own mapping */
    bool offer_scope;               /* a game is selected, so the choice is offered */
    const char *status;             /* a conflict, a load or save note, or the pad */
    bool status_error;              /* status is a conflict */
    const char *deadzone;           /* "Dead zone 9%" */
    const char *trigger;            /* "Trigger point 49%" */
    /* The live input, and on a calibration page its numbers */
    const char *live[PLAYER_UI_CONTROLLER_LIVE_LINES];
    size_t live_count;
    float calibration_fraction;     /* step 1: 0-1 of the sampling time */
    float logical_width;
    float logical_height;
} PlayerUiControllerModel;

/* What a developer log control asks for (PLAYER_UI_ACTION_DEVELOPER_LOG). */
typedef enum {
    PLAYER_UI_DEVELOPER_LOG_NONE = 0,
    PLAYER_UI_DEVELOPER_LOG_COPY,
    PLAYER_UI_DEVELOPER_LOG_SAVE,
    PLAYER_UI_DEVELOPER_LOG_CLOSE
} PlayerUiDeveloperLogOp;

#define PLAYER_UI_DEVELOPER_LOG_LINES 256u   /* PLAYER_DEV_LOG_LINES */

/* The developer log panel (VIEW_DEVELOPER_LOG): the newest lines that fit,
   oldest first. */
typedef struct {
    const char *lines[PLAYER_UI_DEVELOPER_LOG_LINES];
    size_t line_count;
    const char *notice;             /* "Copied 12 lines.", or NULL */
    float logical_width;
    float logical_height;
} PlayerUiDeveloperLogModel;

/* Package build progress (VIEW_BUILDING_PACKAGE). stage is 0-3 for Preflight,
   Extract, Compile and Package, 4 when every stage is done. */
typedef struct {
    const char *title_line;        /* "Game name (DISC-ID)" */
    int stage;
    bool built;                    /* written; being checked before PLAY */
    float fraction;                /* compiled share of the parts, -1 when unknown */
    const char *notice;            /* compile-stage "other apps may feel slower" note */
    const char *message;
    unsigned elapsed_ms;
    const char *lines[4];          /* the last output lines, oldest first */
    size_t line_count;
    double now_s;                  /* drives the running bar (0 holds it still) */
    float logical_width;
    float logical_height;
} PlayerUiBuildModel;

typedef struct {
    const char *name;
    const char *version;
    const char *host;
    const char *license;
    unsigned long long bytes;
} PlayerUiPrereqItem;

/* The one-time build tool download: consent (VIEW_PREREQ_CONSENT) or progress
   (VIEW_PREREQ_PROGRESS). */
typedef struct {
    bool in_progress;
    const PlayerUiPrereqItem *items;
    size_t item_count;
    unsigned long long total_bytes;
    const char *current_item;
    unsigned long long item_received_bytes;
    unsigned long long item_total_bytes;
    unsigned long long total_received_bytes;
    float logical_width;
    float logical_height;
} PlayerUiPrereqModel;

/* The error card (VIEW_ERROR); the words come from player_state's error. */
typedef struct {
    const char *badge;
    const char *title;
    const char *failed_stage;
    const char *message;
    const char *details;
    const char *log_path;
    const char *recovery_label;
    const char *retry_label;    /* "Restart game", "Try again", or NULL for none */
    bool copy_available;        /* offer Copy details */
    bool copied;                /* the report is on the clipboard: say so */
    float logical_width;
    float logical_height;
} PlayerUiErrorModel;

typedef struct {
    const PlayerUiTitle *title;
    size_t title_index;
    bool stalled_game_notice_visible;
    /* The notice is for frames that show nothing, not for frames that stopped. */
    bool stalled_game_dark_picture;
    double now_s;           /* drives the starting bar (0 holds it still) */
    float logical_width;    /* 0 selects 1280 */
    float logical_height;   /* 0 selects 800 */
} PlayerUiTitleModel;

typedef struct {
    const char *version_line;   /* "v0.0.1 (10-23-2026)" (player_ui_version_line) */
    const char *build_commit;   /* short source commit, or NULL / "" */
    float logical_width;
    float logical_height;
} PlayerUiAboutModel;

enum {
    PLAYER_UI_ID_LIBRARY_EMPTY_ADD = 0x1001u,
    PLAYER_UI_ID_LIBRARY_ADD = 0x1002u,
    PLAYER_UI_ID_LIBRARY_PAGE_PREVIOUS = 0x1003u,
    PLAYER_UI_ID_LIBRARY_PAGE_NEXT = 0x1004u,
    PLAYER_UI_ID_LIBRARY_EMPTY_WIZARD = 0x1005u,
    PLAYER_UI_ID_BUILD_ACTION = 0x4001u,
    PLAYER_UI_ID_PREREQ_ACCEPT = 0x4101u,
    PLAYER_UI_ID_PREREQ_CANCEL = 0x4102u,
    PLAYER_UI_ID_ERROR_RECOVER = 0x4201u,
    PLAYER_UI_ID_ERROR_RETRY = 0x4202u,
    PLAYER_UI_ID_ERROR_COPY = 0x4203u,
    PLAYER_UI_ID_WIZARD_PRIMARY = 0x4301u,
    PLAYER_UI_ID_WIZARD_SECOND = 0x4302u,
    PLAYER_UI_ID_WIZARD_BACK = 0x4303u,
    PLAYER_UI_ID_WIZARD_CANCEL = 0x4304u,
    PLAYER_UI_ID_WIZARD_IMPORT_FONTS = 0x4305u,
    PLAYER_UI_ID_WIZARD_REMOVE_FONTS = 0x4306u,
    PLAYER_UI_ID_WIZARD_ONLINE = 0x4307u,
    PLAYER_UI_ID_WIZARD_DEVELOPER = 0x4308u,
    PLAYER_UI_ID_LIBRARY_ITEM_BASE = 0x1100u,
    PLAYER_UI_ID_TITLE_BACK = 0x2001u,
    PLAYER_UI_ID_TITLE_PRIMARY = 0x2002u,
    PLAYER_UI_ID_TITLE_SAVE_FOLDER = 0x2003u,
    PLAYER_UI_ID_TITLE_REMOVE = 0x2004u,
    PLAYER_UI_ID_TITLE_STALL_KEEP_WAITING = 0x2005u,
    PLAYER_UI_ID_TITLE_STALL_STOP_GAME = 0x2006u,
    PLAYER_UI_ID_REMOVE_CANCEL = 0x4401u,
    PLAYER_UI_ID_REMOVE_CONFIRM = 0x4402u,
    PLAYER_UI_ID_REMOVE_SAVES = 0x4403u,
    PLAYER_UI_ID_STOP_KEEP = 0x4411u,
    PLAYER_UI_ID_STOP_CONFIRM = 0x4412u,
    PLAYER_UI_ID_TAB_LIBRARY = 0x0F01u,
    PLAYER_UI_ID_TAB_ABOUT = 0x0F02u,
    PLAYER_UI_ID_TAB_SETTINGS = 0x0F03u,
    PLAYER_UI_ID_ABOUT_NOTICES = 0x3001u,
    PLAYER_UI_ID_DISC_PRIMARY = 0x4601u,
    PLAYER_UI_ID_DISC_SECOND = 0x4602u,
    PLAYER_UI_ID_SETTINGS_RESOLUTION = 0x4501u,
    PLAYER_UI_ID_SETTINGS_PACING = 0x4502u,
    PLAYER_UI_ID_SETTINGS_VSYNC = 0x4503u,
    PLAYER_UI_ID_SETTINGS_FULLSCREEN = 0x4504u,
    PLAYER_UI_ID_SETTINGS_SCALING = 0x4505u,
    PLAYER_UI_ID_SETTINGS_VOLUME_DOWN = 0x4506u,
    PLAYER_UI_ID_SETTINGS_VOLUME_UP = 0x4507u,
    PLAYER_UI_ID_SETTINGS_UI_SOUNDS = 0x4508u,
    PLAYER_UI_ID_SETTINGS_REDUCE_MOTION = 0x4509u,
    PLAYER_UI_ID_SETTINGS_LAUNCHER_FULLSCREEN = 0x450Au,
    PLAYER_UI_ID_SETTINGS_CONTROLLER = 0x450Bu,
    PLAYER_UI_ID_SETTINGS_DONE = 0x450Cu,
    PLAYER_UI_ID_SETTINGS_ONLINE = 0x450Du,
    PLAYER_UI_ID_SETTINGS_NOTICES = 0x450Eu,
    PLAYER_UI_ID_SETTINGS_REMOVE_TOOLS = 0x450Fu,
    PLAYER_UI_ID_SETTINGS_ONLINE_MODE = 0x4511u,
    PLAYER_UI_ID_SETTINGS_NEWS = 0x4513u,
    PLAYER_UI_ID_SETTINGS_REPORTS = 0x4514u,
    PLAYER_UI_ID_SETTINGS_DEVELOPER = 0x4515u,
    PLAYER_UI_ID_SETTINGS_DEVELOPER_LOG = 0x4516u,
    PLAYER_UI_ID_SETTINGS_BACK = 0x4517u,
    PLAYER_UI_ID_SETTINGS_LICENSE_TEXTS = 0x4518u,
    PLAYER_UI_ID_SETTINGS_CONFIRM_REMOVE = 0x4519u,
    PLAYER_UI_ID_SETTINGS_BUILD_CACHE = 0x451Au,
    PLAYER_UI_ID_SETTINGS_SHARE_LIBRARIES = 0x451Bu,
    PLAYER_UI_ID_SETTINGS_CLEAR_LIBRARIES = 0x451Cu,
    PLAYER_UI_ID_SETTINGS_FONTS = 0x451Du,
    PLAYER_UI_ID_SETTINGS_IMPORT_FONTS = 0x451Eu,
    PLAYER_UI_ID_SETTINGS_REMOVE_FONTS = 0x451Fu,
    PLAYER_UI_ID_SETTINGS_UPDATES = 0x4520u,          /* the Updates row's button */
    PLAYER_UI_ID_UPDATE_CHECK = 0x4521u,
    PLAYER_UI_ID_UPDATE_AT_START = 0x4522u,
    PLAYER_UI_ID_UPDATE_DOWNLOAD = 0x4523u,
    PLAYER_UI_ID_UPDATE_CANCEL = 0x4524u,
    PLAYER_UI_ID_UPDATE_INSTALL = 0x4525u,
    PLAYER_UI_ID_UPDATE_FOLDER = 0x4526u,
    PLAYER_UI_ID_UPDATE_PAGE = 0x4527u,
    PLAYER_UI_ID_LIBRARY_UPDATE_NOTICE = 0x4528u,     /* "Version 0.0.2 is out" on the library */
    PLAYER_UI_ID_SETTINGS_FACTORY_RESET = 0x4529u,    /* Settings: opens the check */
    PLAYER_UI_ID_SETTINGS_CONFIRM_RESET = 0x452Au,    /* the check's Reset button */
    PLAYER_UI_ID_CONTROLLER_BUTTON_BASE = 0x4600u,   /* + the PSP control, 0-13 */
    PLAYER_UI_ID_CONTROLLER_DEADZONE_DOWN = 0x4621u,
    PLAYER_UI_ID_CONTROLLER_DEADZONE_UP = 0x4622u,
    PLAYER_UI_ID_CONTROLLER_TRIGGER_DOWN = 0x4623u,
    PLAYER_UI_ID_CONTROLLER_TRIGGER_UP = 0x4624u,
    PLAYER_UI_ID_CONTROLLER_CALIBRATE = 0x4625u,
    PLAYER_UI_ID_CONTROLLER_SAVE = 0x4626u,
    PLAYER_UI_ID_CONTROLLER_RESET = 0x4627u,
    PLAYER_UI_ID_CONTROLLER_BACK = 0x4628u,
    PLAYER_UI_ID_CONTROLLER_SCOPE = 0x4629u,
    PLAYER_UI_ID_CONTROLLER_CALIBRATION_PRIMARY = 0x462Au,
    PLAYER_UI_ID_CONTROLLER_CALIBRATION_CANCEL = 0x462Bu,
    PLAYER_UI_ID_CONTROLLER_SHOW_PAD = 0x462Cu,
    PLAYER_UI_ID_CONTROLLER_SHOW_KEYBOARD = 0x462Du,
    PLAYER_UI_ID_CONTROLLER_GLYPH_BASE = 0x4640u,    /* + the PSP control, 0-13; not focusable */
    PLAYER_UI_ID_DEVELOPER_LOG_COPY = 0x4701u,
    PLAYER_UI_ID_DEVELOPER_LOG_SAVE = 0x4702u,
    PLAYER_UI_ID_DEVELOPER_LOG_CLOSE = 0x4703u
};

#define PLAYER_UI_ID_CONTROLLER_BUTTON(control) \
    ((NkUiId)(PLAYER_UI_ID_CONTROLLER_BUTTON_BASE + (uint32_t)(control)))
#define PLAYER_UI_ID_CONTROLLER_GLYPH(control) \
    ((NkUiId)(PLAYER_UI_ID_CONTROLLER_GLYPH_BASE + (uint32_t)(control)))

#define PLAYER_UI_ID_LIBRARY_ITEM(index) \
    ((NkUiId)(PLAYER_UI_ID_LIBRARY_ITEM_BASE + (uint32_t)(index)))

NkUiRouter *nk_ui_router_create(void);
void nk_ui_router_destroy(NkUiRouter *router);
bool nk_ui_router_replace(NkUiRouter *router, PlayerUiRoute route);
bool nk_ui_router_push(NkUiRouter *router, PlayerUiRoute route);
bool nk_ui_router_pop(NkUiRouter *router);
bool nk_ui_router_handle_event(NkUiRouter *router, const NkUiEvent *event);
PlayerUiRoute nk_ui_router_current(const NkUiRouter *router);
size_t nk_ui_router_depth(const NkUiRouter *router);
void nk_ui_router_set_focus(NkUiRouter *router, NkUiId id);
NkUiId nk_ui_router_take_focus_restore(NkUiRouter *router);

/* Library grid for a logical window size: the number of columns that fit at the
   minimum tile width, and the number of grid slots (titles plus the add card)
   one page shows. */
size_t player_ui_library_page_size(float logical_width, float logical_height,
                                   size_t *columns_out);
/* The same with `demo_count` demos under TEST THIS PC, whose rows take their
   height from the grid's. */
size_t player_ui_library_page_size_with_demos(float logical_width, float logical_height,
                                              size_t demo_count, size_t *columns_out);
/* How many titles are demos (PlayerUiTitle.is_demo). */
size_t player_ui_library_demo_count(const PlayerUiTitle *titles, size_t count);
/* Where title `index` sits in the grid of the user's games (titles that are
   not demos), from 0; `count` for a demo or an index out of range. */
size_t player_ui_library_grid_position(const PlayerUiTitle *titles, size_t count,
                                       size_t index);

PlayerUiAction player_ui_screen_library(NkUi *ui,
                                        const PlayerUiLibraryModel *model);
PlayerUiAction player_ui_screen_title(NkUi *ui,
                                      const PlayerUiTitleModel *model);
PlayerUiAction player_ui_screen_build(NkUi *ui, const PlayerUiBuildModel *model);

/* Remove from library (#1304): what goes, with sizes, and the saves box. */
#define PLAYER_UI_REMOVE_ITEM_CAP 4u
typedef struct {
    const char *label;          /* "Built package" */
    uint64_t bytes;
} PlayerUiRemoveItem;

typedef struct {
    const char *title;
    const char *disc_id;
    size_t title_index;
    const PlayerUiRemoveItem *items;   /* what Nakagawa built; may be empty */
    size_t item_count;
    bool has_saves;             /* the game has saves; false hides the saves box */
    uint64_t saves_bytes;
    bool delete_saves;          /* the saves box; off until the user ticks it */
    float logical_width;
    float logical_height;
} PlayerUiRemoveModel;

PlayerUiAction player_ui_screen_remove(NkUi *ui, const PlayerUiRemoveModel *model);

/* Stop game: STOP GAME asks first, as quitting with a game running does. KEEP
   PLAYING holds focus first and Back is KEEP PLAYING; STOP GAME returns
   PLAYER_UI_ACTION_PRIMARY with PLAYER_UI_PRIMARY_STOP_GAME. */
typedef struct {
    const char *title;          /* the game that runs */
    size_t title_index;
    float logical_width;
    float logical_height;
} PlayerUiStopModel;

PlayerUiAction player_ui_screen_stop(NkUi *ui, const PlayerUiStopModel *model);
PlayerUiAction player_ui_screen_prereq(NkUi *ui, const PlayerUiPrereqModel *model);
PlayerUiAction player_ui_screen_error(NkUi *ui, const PlayerUiErrorModel *model);
PlayerUiAction player_ui_screen_wizard(NkUi *ui, const PlayerUiWizardModel *model);
PlayerUiAction player_ui_screen_about(NkUi *ui,
                                      const PlayerUiAboutModel *model);
PlayerUiAction player_ui_screen_disc(NkUi *ui, const PlayerUiDiscModel *model);
PlayerUiAction player_ui_screen_settings(NkUi *ui, const PlayerUiSettingsModel *model);
PlayerUiAction player_ui_screen_controller(NkUi *ui, const PlayerUiControllerModel *model);
PlayerUiAction player_ui_screen_developer_log(NkUi *ui, const PlayerUiDeveloperLogModel *model);

/* The top bar every launcher screen draws: LIBRARY, ABOUT and SETTINGS tabs,
   focusable and switched with L / R. Returns the action for a tab the user
   chose this frame (OPEN_LIBRARY, OPEN_ABOUT or OPEN_SETTINGS; the menu
   prompt, Start or F1, also opens Settings), else NONE. `active` is the route
   shown, or PLAYER_UI_ROUTE_SETTINGS on the Settings page. */
PlayerUiActionKind player_ui_top_bar(NkUi *ui, NkUiId id, PlayerUiRoute active);

/* The build date for an epoch (Unix seconds, UTC) as MM-DD-YYYY. Returns false
   for a negative epoch or a short buffer. */
bool player_ui_build_date(long long epoch, char *out, size_t cap);
/* "v<version> (MM-DD-YYYY)", or "v<version> (dev build)" when epoch is
   negative (no SOURCE_DATE_EPOCH and no commit time at build). */
void player_ui_version_line(const char *version, long long epoch, char *out, size_t cap);

PlayerUiNk *player_ui_nk_create(struct SDL_Renderer *renderer,
                                struct SDL_Window *window,
                                char *error, size_t error_size);
/* False when this build was compiled without the SDL_Renderer or SDL3_ttf
   support player_ui_nk_create needs, so it can never succeed here. */
bool player_ui_nk_built_with_launcher(void);
void player_ui_nk_destroy(PlayerUiNk *player);
bool player_ui_nk_view_active(const PlayerUiNk *player, const PlayerApp *app);
/* True when the launcher draws this view. Every PlayerView is drawn by it: the
   shipped player has no other launcher. */
bool player_ui_nk_draws_view(int view);
/* True when the next UI shows the Library at the bottom of its stack (Esc
   there asks to quit, as in the existing UI). */
bool player_ui_nk_at_library_root(const PlayerUiNk *player);
/* The most titles one frame shows: a library page's cards and its demos. */
#define PLAYER_UI_SHOWN_TITLES_MAX 256u
/* The library indices of the titles the last frame showed (the cards of the
   library page and its demos, or the title page's title), at most `cap` of
   them. The package check looks at these first: checking every title of a
   large library reads every disc. */
size_t player_ui_nk_shown_titles(const PlayerUiNk *player, int *out, size_t cap);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
/* Test only: a title's model as the last frame filled it, or NULL. */
const PlayerUiTitle *player_ui_nk_test_title(const PlayerUiNk *player, size_t index);
/* Test only: the route the next UI shows, or 0 when it has none. */
int player_ui_nk_test_route(const PlayerUiNk *player);
/* Test only: the control the next UI focuses, or 0. */
unsigned player_ui_nk_test_focus(const PlayerUiNk *player);
/* Test only: print each compatibility record's category tier and tier. */
void player_ui_nk_test_report_compatibility(void);
#endif
bool player_ui_nk_handle_event(PlayerUiNk *player,
                               const union SDL_Event *event);
/* A pad was opened (connected) or went away: the prompt glyphs and the top
   bar's input chip follow it. `which` is the SDL joystick id. NULL player is a
   no-op. */
void player_ui_nk_note_gamepad(PlayerUiNk *player, uint32_t which, bool connected);
bool player_ui_nk_render(PlayerUiNk *player, PlayerApp *app,
                         struct SDL_Renderer *renderer,
                         struct SDL_Window *window,
                         PlayerUiAction *out_action);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_UI_NK_H */
