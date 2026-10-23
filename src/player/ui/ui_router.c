/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_nk.h"

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

bool nk_ui_router_replace(NkUiRouter *router, PlayerUiRoute route) {
    if (router == NULL || (route != PLAYER_UI_ROUTE_LIBRARY &&
                           route != PLAYER_UI_ROUTE_TITLE)) {
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
    if (router == NULL || (route != PLAYER_UI_ROUTE_LIBRARY &&
                           route != PLAYER_UI_ROUTE_TITLE) ||
        router->depth == 0u || router->depth >= PLAYER_UI_ROUTE_DEPTH_MAX) {
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
