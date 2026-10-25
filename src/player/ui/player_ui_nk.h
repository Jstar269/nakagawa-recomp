/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef PLAYER_UI_NK_H
#define PLAYER_UI_NK_H

#include "nk_ui.h"

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
    PLAYER_UI_ROUTE_ABOUT = 3
} PlayerUiRoute;

/* The player's version: the one place it is written. The About page shows
   "v" NK_PLAYER_VERSION " (MM-DD-YYYY)"; nothing else (tags, releases, the
   tools' package version) reads it. */
#define NK_PLAYER_VERSION "0.0.1"

typedef enum {
    PLAYER_UI_PRIMARY_NONE = 0,
    PLAYER_UI_PRIMARY_PLAY,
    PLAYER_UI_PRIMARY_BUILD,
    PLAYER_UI_PRIMARY_RETRY_PACKAGE_CHECK,
    PLAYER_UI_PRIMARY_STOP_GAME,
    PLAYER_UI_PRIMARY_REBUILD   /* an INCOMPATIBLE package: check, then build again */
} PlayerUiPrimaryAction;

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
    PLAYER_UI_ACTION_OPEN_NOTICES
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
    const char *compatibility_word;
    const char *compatibility_detail;
    const char *input_note;
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

/* The setup wizard (VIEW_SETUP_WIZARD). step is 0-5: Welcome, Online and
   developer options, Select game, Verify, Fonts and system, Ready. */
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
    bool extraction_complete;
    bool extraction_failed;
    bool experimental;
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
    /* Ready */
    bool runtime_ready;
    const char *summary_line;
    /* Online and developer options */
    bool online_mode;
    bool developer_mode;
    float logical_width;
    float logical_height;
} PlayerUiWizardModel;

/* Package build progress (VIEW_BUILDING_PACKAGE). stage is 0-3 for Preflight,
   Extract, Compile and Package, 4 when every stage is done. */
typedef struct {
    const char *title_line;        /* "Game name (DISC-ID)" */
    int stage;
    bool built;                    /* written; being checked before PLAY */
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
    PLAYER_UI_ID_TAB_LIBRARY = 0x0F01u,
    PLAYER_UI_ID_TAB_ABOUT = 0x0F02u,
    PLAYER_UI_ID_TAB_SETTINGS = 0x0F03u,
    PLAYER_UI_ID_ABOUT_NOTICES = 0x3001u
};

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

PlayerUiAction player_ui_screen_library(NkUi *ui,
                                        const PlayerUiLibraryModel *model);
PlayerUiAction player_ui_screen_title(NkUi *ui,
                                      const PlayerUiTitleModel *model);
PlayerUiAction player_ui_screen_build(NkUi *ui, const PlayerUiBuildModel *model);
PlayerUiAction player_ui_screen_prereq(NkUi *ui, const PlayerUiPrereqModel *model);
PlayerUiAction player_ui_screen_error(NkUi *ui, const PlayerUiErrorModel *model);
PlayerUiAction player_ui_screen_wizard(NkUi *ui, const PlayerUiWizardModel *model);
PlayerUiAction player_ui_screen_about(NkUi *ui,
                                      const PlayerUiAboutModel *model);

/* The top bar every launcher screen draws: LIBRARY, ABOUT and SETTINGS tabs,
   focusable and switched with L / R. Returns the action for a tab the user
   chose this frame (OPEN_LIBRARY, OPEN_ABOUT or OPEN_SETTINGS; the menu
   prompt, Start or F1, also opens Settings), else NONE. */
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
void player_ui_nk_destroy(PlayerUiNk *player);
bool player_ui_nk_view_active(const PlayerUiNk *player, const PlayerApp *app);
/* True when the next UI shows the Library at the bottom of its stack (Esc
   there asks to quit, as in the existing UI). */
bool player_ui_nk_at_library_root(const PlayerUiNk *player);
#ifdef NK_PLAYER_UI_REGRESSION_TEST
/* Test only: a title's model as the last frame filled it, or NULL. */
const PlayerUiTitle *player_ui_nk_test_title(const PlayerUiNk *player, size_t index);
/* Test only: the route the next UI shows, or 0 when it has none. */
int player_ui_nk_test_route(const PlayerUiNk *player);
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
