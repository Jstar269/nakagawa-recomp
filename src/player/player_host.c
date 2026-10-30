/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The player's host seam (player_host.h). Host tests live here and in the
 * src/core/nk_platform_*.c backends only; tools/test_platform_seam.py holds
 * every other player and core file to its recorded count. */

#include "player_host.h"
#include "player_state.h"
#include "nk_platform.h"
#include "nk_ui_widgets.h"

#include <stdio.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <shellapi.h>
#endif

void player_host_show_picker(PlayerHostPick what, SDL_Window *window,
                             const char *start_folder,
                             SDL_DialogFileCallback callback, void *userdata) {
    /* SDL keeps reading the filters until the dialog's callback runs, so they
       are static. */
    static const SDL_DialogFileFilter disc_filters[] = {
        { "PSP Disc Images and PSN Packages (*.iso, *.cso, *.zso, *.chd, EBOOT.PBP)",
          "iso;cso;zso;chd;pbp" },
        { "All Files (*.*)", "*" }
    };
    static const SDL_DialogFileFilter font_filters[] = {
        { "PSP fonts (*.pgf)", "pgf" }
    };
    /* The fonts import shows the .pgf files themselves, which a folder picker
       hides, so the user can see they are in the right folder. */
    if (what == PLAYER_HOST_PICK_FONTS) {
        SDL_ShowOpenFileDialog(callback, userdata, window, font_filters, 1,
                               start_folder, true);
    } else {
        SDL_ShowOpenFileDialog(callback, userdata, window, disc_filters, 2,
                               start_folder, false);
    }
}

bool player_host_open_folder(const char *path) {
    if (!path || !path[0]) return false;
#if defined(_WIN32) || defined(_WIN64)
    /* The shell shows its own message when the folder cannot be opened. */
    (void)ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
    return true;
#else
    char url[NK_MAX_PATH * 3u];
    return player_file_url(path, url, sizeof(url)) && SDL_OpenURL(url);
#endif
}

void player_host_set_window_icon(SDL_Window *window) {
#if defined(_WIN32) || defined(_WIN64)
    /* The executable carries the icon (assets/brand/nakagawa.rc) and SDL gives
       every window its sizes, where SDL_SetWindowIcon would stretch one bitmap
       to all. */
    (void)window;
#else
    /* The app icon (the N1 mark on its ink tile): 32 px, with larger copies SDL
       picks for high-density displays. */
    static const int sizes[] = { 32, 48, 64, 128, 256 };
    SDL_Surface *icon = NULL;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        SDL_Surface *image = SDL_CreateSurface(sizes[i], sizes[i], SDL_PIXELFORMAT_RGBA32);
        if (!image || !nk_ui_app_icon_rgba(sizes[i], (unsigned char *)image->pixels,
                                           (size_t)image->pitch)) {
            SDL_DestroySurface(image);
            break;
        }
        if (!icon) {
            icon = image;
        } else {
            SDL_AddSurfaceAlternateImage(icon, image);
            SDL_DestroySurface(image);
        }
    }
    if (!icon || !SDL_SetWindowIcon(window, icon)) {
        fprintf(stderr, "The window icon was not set: %s\n", SDL_GetError());
    }
    SDL_DestroySurface(icon);
#endif
}
