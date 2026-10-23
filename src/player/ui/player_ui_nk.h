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
    PLAYER_UI_PRIMARY_STOP_GAME
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
    PLAYER_UI_ID_LIBRARY_ITEM_BASE = 0x1100u,
    PLAYER_UI_ID_TITLE_BACK = 0x2001u,
    PLAYER_UI_ID_TITLE_PRIMARY = 0x2002u,
    PLAYER_UI_ID_TITLE_SAVE_FOLDER = 0x2003u,
    PLAYER_UI_ID_TAB_LIBRARY = 0x0F01u,
    PLAYER_UI_ID_TAB_ABOUT = 0x0F02u,
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
PlayerUiAction player_ui_screen_about(NkUi *ui,
                                      const PlayerUiAboutModel *model);

/* The top bar every launcher screen draws: LIBRARY and ABOUT tabs, focusable
   and switched with L / R. Returns the action for a tab the user chose this
   frame (OPEN_LIBRARY or OPEN_ABOUT), else NONE. */
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
