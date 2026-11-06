/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The player's host seam: what the launcher needs from the desktop it runs on
 * and that needs SDL, so it cannot live in src/core/nk_platform.h. The rest of
 * the player calls these instead of testing _WIN32, __APPLE__, __linux__ or
 * __ANDROID__ itself (docs/PLATFORM_PORTABILITY.md, "One platform seam"). A new
 * host (Android #360, macOS #329) changes this file and the nk_platform
 * backends, not the launcher's callers. */

#ifndef PLAYER_HOST_H
#define PLAYER_HOST_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a file picker is for; each kind has its own filters. */
typedef enum {
    PLAYER_HOST_PICK_DISC = 0,  /* a disc image or PSN package; one file */
    PLAYER_HOST_PICK_FONTS,     /* the PSP .pgf fonts; any number, one folder */
    PLAYER_HOST_PICK_SAVE       /* Add a save (#1883): one save's folder, a folder picker */
} PlayerHostPick;

/* Show the host's open-file picker for `what` (the folder picker for a save),
 * starting in start_folder (NULL: the host's choice), and call
 * callback(userdata, files, filter) when the user picks or cancels. SDL may call
 * it on another thread. start_folder must stay valid until the callback runs. */
void player_host_show_picker(PlayerHostPick what, SDL_Window *window,
                             const char *start_folder,
                             SDL_DialogFileCallback callback, void *userdata);

/* Ask the host's file manager to show the folder at path. False when it could
 * not be asked; SDL_GetError() then says why, when it knows. Windows hands the
 * folder to its shell, which reports its own failures; elsewhere SDL opens a
 * file:// URL (xdg-open on Linux). */
bool player_host_open_folder(const char *path);

/* Give the launcher window the app icon where the host does not take it from
 * the executable. Windows needs nothing: the executable carries the icon
 * (assets/brand/nakagawa.rc). */
void player_host_set_window_icon(SDL_Window *window);

/* Give this process the player's identity (#2006, NK_APP_* in src/core/nk_launch.h) before
 * SDL starts and before any window exists: the Windows AppUserModelID, so the launcher, every
 * game it starts and a pinned shortcut share one taskbar group, and SDL's app metadata (the
 * Wayland app ID and X11 class elsewhere). A refusal only costs the grouping; it is logged. */
void player_host_set_app_identity(void);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_HOST_H */
