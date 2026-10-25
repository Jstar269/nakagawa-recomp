/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"

#include <stdio.h>
#include <stdlib.h>

#define PLAYER_UI_ROUTE_DEPTH_MAX 16u

typedef struct {
    PlayerUiRoute route;
    NkUiId focus;
} PlayerUiRouteEntry;

struct NkUiRouter {
    PlayerUiRouteEntry entries[PLAYER_UI_ROUTE_DEPTH_MAX];
    size_t depth;
    NkUiId focus_restore;
    bool focus_restore_pending;
};

NkUiRouter *nk_ui_router_create(void) {
    NkUiRouter *router = (NkUiRouter *)calloc(1u, sizeof(*router));
    if (router != NULL) {
        router->entries[0].route = PLAYER_UI_ROUTE_LIBRARY;
        router->depth = 1u;
    }
    return router;
}

void nk_ui_router_destroy(NkUiRouter *router) {
    free(router);
}

/* Every route the player draws. A route missing here is refused by replace and
   push, and its screen can never open (#1281: About was missing). */
static bool nk_ui_router_known(PlayerUiRoute route) {
    return route == PLAYER_UI_ROUTE_LIBRARY || route == PLAYER_UI_ROUTE_TITLE ||
           route == PLAYER_UI_ROUTE_ABOUT;
}

bool nk_ui_router_replace(NkUiRouter *router, PlayerUiRoute route) {
    if (router == NULL || !nk_ui_router_known(route)) {
        return false;
    }
    router->entries[0].route = route;
    router->entries[0].focus = 0u;
    router->depth = 1u;
    router->focus_restore = 0u;
    router->focus_restore_pending = false;
    return true;
}

bool nk_ui_router_push(NkUiRouter *router, PlayerUiRoute route) {
    PlayerUiRouteEntry *entry;
    if (router == NULL || !nk_ui_router_known(route) || router->depth == 0u ||
        router->depth >= PLAYER_UI_ROUTE_DEPTH_MAX) {
        return false;
    }
    entry = &router->entries[router->depth++];
    entry->route = route;
    entry->focus = 0u;
    router->focus_restore = 0u;
    router->focus_restore_pending = false;
    return true;
}

bool nk_ui_router_pop(NkUiRouter *router) {
    if (router == NULL || router->depth <= 1u) {
        return false;
    }
    --router->depth;
    router->focus_restore = router->entries[router->depth - 1u].focus;
    router->focus_restore_pending = router->focus_restore != 0u;
    return true;
}

bool nk_ui_router_handle_event(NkUiRouter *router, const NkUiEvent *event) {
    if (event == NULL || event->type != NK_UI_EV_BACK) {
        return false;
    }
    (void)nk_ui_router_pop(router);
    return true;
}

PlayerUiRoute nk_ui_router_current(const NkUiRouter *router) {
    return router != NULL && router->depth > 0u
        ? router->entries[router->depth - 1u].route : 0;
}

size_t nk_ui_router_depth(const NkUiRouter *router) {
    return router != NULL ? router->depth : 0u;
}

void nk_ui_router_set_focus(NkUiRouter *router, NkUiId id) {
    if (router != NULL && router->depth > 0u) {
        router->entries[router->depth - 1u].focus = id;
    }
}

NkUiId nk_ui_router_take_focus_restore(NkUiRouter *router) {
    NkUiId id;
    if (router == NULL || !router->focus_restore_pending) {
        return 0u;
    }
    id = router->focus_restore;
    router->focus_restore = 0u;
    router->focus_restore_pending = false;
    return id;
}

/* Days since 1970-01-01 to a proleptic Gregorian date (civil_from_days, after
   Howard Hinnant's public-domain date algorithms), so the date never depends on
   the host's gmtime or time zone. */
bool player_ui_build_date(long long epoch, char *out, size_t cap) {
    long long z;
    long long era;
    long long doe;
    long long yoe;
    long long doy;
    long long mp;
    long long day;
    long long month;
    long long year;
    int written;
    if (epoch < 0 || out == NULL || cap < 11u) {
        return false;
    }
    z = epoch / 86400 + 719468;
    era = z / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    if (year < 1 || year > 9999) {
        return false;
    }
    written = snprintf(out, cap, "%02lld-%02lld-%04lld", month, day, year);
    return written == 10;
}

void player_ui_version_line(const char *version, long long epoch, char *out, size_t cap) {
    char date[16];
    if (out == NULL || cap == 0u) {
        return;
    }
    if (version == NULL || version[0] == '\0') {
        version = NK_PLAYER_VERSION;
    }
    if (player_ui_build_date(epoch, date, sizeof(date))) {
        (void)snprintf(out, cap, "v%s (%s)", version, date);
    } else {
        (void)snprintf(out, cap, "v%s (dev build)", version);
    }
}

PlayerUiActionKind player_ui_top_bar(NkUi *ui, NkUiId id, PlayerUiRoute active) {
    NkUiSection sections[3];
    int chosen;
    sections[0].label = "Library";
    sections[0].active = active != PLAYER_UI_ROUTE_ABOUT;
    sections[0].id = PLAYER_UI_ID_TAB_LIBRARY;
    sections[1].label = "About";
    sections[1].active = active == PLAYER_UI_ROUTE_ABOUT;
    sections[1].id = PLAYER_UI_ID_TAB_ABOUT;
    /* Settings is its own view, so its tab is never the active one. */
    sections[2].label = "Settings";
    sections[2].active = false;
    sections[2].id = PLAYER_UI_ID_TAB_SETTINGS;
    chosen = nk_ui_top_bar(ui, id, sections, 3u,
                           nk_ui_input_label(nk_ui_prompt_style(ui)),
                           nk_ui_prompt_style(ui) != NK_UI_PROMPT_KEYCAPS);
    if (chosen == 0 && !sections[0].active) return PLAYER_UI_ACTION_OPEN_LIBRARY;
    if (chosen == 1 && !sections[1].active) return PLAYER_UI_ACTION_OPEN_ABOUT;
    if (chosen == 2 || (chosen < 0 && nk_ui_take_event(ui, NK_UI_EV_MENU, NULL))) {
        return PLAYER_UI_ACTION_OPEN_SETTINGS;
    }
    return PLAYER_UI_ACTION_NONE;
}
