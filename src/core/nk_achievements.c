/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The RetroAchievements bridge (#1316): see nk_achievements.h. */

#include "nk_achievements.h"

#include "rc_client.h"
#include "rc_consoles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The transport's sentinels pass straight through as rc_client's status codes. */
typedef char nk_ach_client_error_matches_rcheevos
    [(NK_ACH_HTTP_CLIENT_ERROR == RC_API_SERVER_RESPONSE_CLIENT_ERROR) ? 1 : -1];
typedef char nk_ach_retry_matches_rcheevos
    [(NK_ACH_HTTP_RETRY == RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR) ? 1 : -1];

struct NkAch {
    rc_client_t *client;
    NkAchMemory mem;
    NkAchTransport transport;
    void *transport_ctx;
    NkAchEvent events[NK_ACH_EVENT_QUEUE];
    size_t event_head; /* oldest */
    size_t event_count;
    uint32_t events_dropped;
};

bool nk_ach_url_allowed(const char *url) {
    static const char prefix[] = NK_ACH_URL_PREFIX;
    /* The prefix ends in the '/' after the host, so "retroachievements.org.example/",
     * "retroachievements.org:8080/" and "user@..." forms cannot match. */
    return url != NULL && strncmp(url, prefix, sizeof prefix - 1u) == 0;
}

uint32_t nk_ach_read_memory(const NkAchMemory *mem, uint32_t ra_address, uint8_t *out,
                            uint32_t n) {
    if (!mem || !mem->base || !out || n == 0u) return 0u;
    const rc_memory_regions_t *regions = rc_console_memory_regions(NK_ACH_CONSOLE_PSP);
    if (!regions) return 0u;
    for (uint32_t i = 0; i < regions->num_regions; i++) {
        const rc_memory_region_t *r = &regions->region[i];
        if (ra_address < r->start_address || ra_address > r->end_address) continue;
        /* 64-bit arithmetic: no guest address or span end can wrap. */
        const uint64_t guest = (uint64_t)r->real_address + (ra_address - r->start_address);
        const uint64_t span_end = (uint64_t)mem->guest_start + mem->size;
        if (guest < mem->guest_start || guest >= span_end) return 0u;
        /* The run of regions that follow on in both RA and guest addresses (all three, for the
         * PSP) is one piece of memory: a read stops only at its end and at the end of the span. */
        uint32_t run_end = r->end_address;
        for (uint32_t k = i + 1u; k < regions->num_regions; k++) {
            const rc_memory_region_t *next = &regions->region[k];
            const rc_memory_region_t *prev = &regions->region[k - 1u];
            if ((uint64_t)next->start_address != (uint64_t)prev->end_address + 1u ||
                (uint64_t)next->real_address !=
                    (uint64_t)prev->real_address + (prev->end_address - prev->start_address) + 1u)
                break;
            run_end = next->end_address;
        }
        uint64_t avail = span_end - guest;
        const uint64_t run_left = (uint64_t)run_end - ra_address + 1u;
        if (run_left < avail) avail = run_left;
        const uint32_t count = (uint32_t)(avail < n ? avail : n);
        memcpy(out, mem->base + (size_t)(guest - mem->guest_start), count);
        return count;
    }
    return 0u;
}

static void ach_push(NkAch *ach, NkAchEventKind kind, uint32_t id, uint32_t points,
                     uint32_t count, const char *title, const char *message) {
    if (ach->event_count == NK_ACH_EVENT_QUEUE) {
        ach->event_head = (ach->event_head + 1u) % NK_ACH_EVENT_QUEUE;
        ach->event_count--;
        ach->events_dropped++;
    }
    NkAchEvent *e = &ach->events[(ach->event_head + ach->event_count) % NK_ACH_EVENT_QUEUE];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->id = id;
    e->points = points;
    e->count = count;
    if (title) snprintf(e->title, sizeof e->title, "%s", title);
    if (message) snprintf(e->message, sizeof e->message, "%s", message);
    ach->event_count++;
}

static uint32_t RC_CCONV ach_read(uint32_t address, uint8_t *buffer, uint32_t num_bytes,
                                  rc_client_t *client) {
    const NkAch *ach = (const NkAch *)rc_client_get_userdata(client);
    return ach ? nk_ach_read_memory(&ach->mem, address, buffer, num_bytes) : 0u;
}

/* One request in flight: rc_client's callback, carried through the caller's transport. */
typedef struct AchCall {
    rc_client_server_callback_t callback;
    void *callback_data;
} AchCall;

static void ach_done(const NkAchResponse *response, void *done_ctx) {
    AchCall *call = (AchCall *)done_ctx;
    rc_api_server_response_t server;
    memset(&server, 0, sizeof server);
    if (response) {
        server.body = response->body;
        server.body_length = response->body ? response->body_length : 0u;
        server.http_status_code = response->http_status;
    } else {
        server.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
    }
    call->callback(&server, call->callback_data);
    free(call);
}

static void ach_refuse(rc_client_server_callback_t callback, void *callback_data,
                       const char *why) {
    rc_api_server_response_t server;
    memset(&server, 0, sizeof server);
    server.body = why;
    server.body_length = strlen(why);
    server.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
    callback(&server, callback_data);
}

static void RC_CCONV ach_server_call(const rc_api_request_t *request,
                                     rc_client_server_callback_t callback, void *callback_data,
                                     rc_client_t *client) {
    NkAch *ach = (NkAch *)rc_client_get_userdata(client);
    if (!ach || !request || !nk_ach_url_allowed(request->url)) {
        ach_refuse(callback, callback_data, "refused: not a RetroAchievements server");
        return;
    }
    if (!ach->transport) {
        ach_refuse(callback, callback_data, "refused: online achievements are off");
        return;
    }
    AchCall *call = (AchCall *)malloc(sizeof *call);
    if (!call) {
        ach_refuse(callback, callback_data, "refused: out of memory");
        return;
    }
    call->callback = callback;
    call->callback_data = callback_data;
    NkAchRequest out = {request->url, request->post_data, request->content_type};
    ach->transport(ach->transport_ctx, &out, ach_done, call);
}

static void RC_CCONV ach_event(const rc_client_event_t *event, rc_client_t *client) {
    NkAch *ach = (NkAch *)rc_client_get_userdata(client);
    if (!ach || !event) return;
    switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        if (event->achievement)
            ach_push(ach, NK_ACH_EVENT_UNLOCKED, event->achievement->id,
                     event->achievement->points, 0u, event->achievement->title, NULL);
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        ach_push(ach, NK_ACH_EVENT_SERVER_ERROR,
                 event->server_error ? event->server_error->related_id : 0u, 0u, 0u, NULL,
                 event->server_error ? event->server_error->error_message : NULL);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        ach_push(ach, NK_ACH_EVENT_DISCONNECTED, 0u, 0u, 0u, NULL, NULL);
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        ach_push(ach, NK_ACH_EVENT_RECONNECTED, 0u, 0u, 0u, NULL, NULL);
        break;
    default:
        break;
    }
}

/* rc_client raises no event when a short read turns an achievement off; it only logs
 * "Disabled achievement <id>. Invalid address <addr>" (rc_client.c,
 * rc_client_invalidate_memref_achievements). That warning becomes a DISABLED event, so the user is
 * told. achievements-selftest checks the text, so an rcheevos update that changes it fails there. */
static const char k_disabled_prefix[] = "Disabled achievement ";

static void RC_CCONV ach_log(const char *message, const rc_client_t *client) {
    NkAch *ach = (NkAch *)rc_client_get_userdata(client);
    if (!ach || !message || strncmp(message, k_disabled_prefix, sizeof k_disabled_prefix - 1u))
        return;
    const char *p = message + sizeof k_disabled_prefix - 1u;
    uint32_t id = 0u;
    while (*p >= '0' && *p <= '9' && id <= 0x0fffffffu) id = id * 10u + (uint32_t)(*p++ - '0');
    ach_push(ach, NK_ACH_EVENT_DISABLED, id, 0u, 0u, NULL, message);
}

NkAch *nk_ach_create(const NkAchMemory *mem, NkAchTransport transport, void *transport_ctx) {
    NkAch *ach = (NkAch *)calloc(1u, sizeof *ach);
    if (!ach) return NULL;
    if (mem) ach->mem = *mem;
    ach->transport = transport;
    ach->transport_ctx = transport_ctx;
    ach->client = rc_client_create(ach_read, ach_server_call);
    if (!ach->client) {
        free(ach);
        return NULL;
    }
    rc_client_set_userdata(ach->client, ach);
    rc_client_set_event_handler(ach->client, ach_event);
    rc_client_enable_logging(ach->client, RC_CLIENT_LOG_LEVEL_WARN, ach_log);
    rc_client_set_hardcore_enabled(ach->client, 0);
    rc_client_set_encore_mode_enabled(ach->client, 0);
    rc_client_set_unofficial_enabled(ach->client, 0);
    return ach;
}

void nk_ach_destroy(NkAch *ach) {
    if (!ach) return;
    rc_client_destroy(ach->client);
    free(ach);
}

static void RC_CCONV ach_login_done(int result, const char *error_message,
                                    rc_client_t *client, void *userdata) {
    (void)userdata;
    NkAch *ach = (NkAch *)rc_client_get_userdata(client);
    if (!ach) return;
    if (result == RC_OK) {
        const rc_client_user_t *user = rc_client_get_user_info(client);
        ach_push(ach, NK_ACH_EVENT_LOGIN_OK, 0u, user ? user->score_softcore : 0u, 0u,
                 user ? user->display_name : NULL, NULL);
    } else {
        ach_push(ach, NK_ACH_EVENT_LOGIN_FAILED, 0u, 0u, 0u, NULL,
                 error_message ? error_message : rc_error_str(result));
    }
}

bool nk_ach_begin_login(NkAch *ach, const char *username, const char *token) {
    if (!ach || !username || !*username || !token || !*token) return false;
    /* rc_client returns no handle for a call it finished at once; either way the outcome
     * arrives as an event. */
    (void)rc_client_begin_login_with_token(ach->client, username, token, ach_login_done, NULL);
    return true;
}

static void RC_CCONV ach_load_done(int result, const char *error_message,
                                   rc_client_t *client, void *userdata) {
    (void)userdata;
    NkAch *ach = (NkAch *)rc_client_get_userdata(client);
    if (!ach) return;
    const rc_client_game_t *game = rc_client_get_game_info(client);
    if (result == RC_OK && game) {
        rc_client_user_game_summary_t summary;
        memset(&summary, 0, sizeof summary);
        rc_client_get_user_game_summary(client, &summary);
        ach_push(ach, NK_ACH_EVENT_GAME_LOADED, game->id, 0u, summary.num_core_achievements,
                 game->title, NULL);
    } else {
        ach_push(ach, NK_ACH_EVENT_GAME_FAILED, 0u, 0u, 0u, NULL,
                 error_message ? error_message : rc_error_str(result));
    }
}

static bool ach_hash_ok(const char *hash) {
    if (!hash) return false;
    for (int i = 0; i < 32; i++) {
        const char c = hash[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return hash[32] == '\0';
}

bool nk_ach_begin_load_game(NkAch *ach, const char *hash) {
    if (!ach || !ach_hash_ok(hash)) return false;
    (void)rc_client_begin_load_game(ach->client, hash, ach_load_done, NULL);
    return true;
}

void nk_ach_frame(NkAch *ach) {
    if (ach) rc_client_do_frame(ach->client);
}

void nk_ach_idle(NkAch *ach) {
    if (ach) rc_client_idle(ach->client);
}

bool nk_ach_logged_in(const NkAch *ach) {
    return ach && rc_client_get_user_info(ach->client) != NULL;
}

bool nk_ach_game_loaded(const NkAch *ach) {
    return ach && rc_client_is_game_loaded(ach->client) != 0;
}

bool nk_ach_hardcore(const NkAch *ach) {
    return ach && rc_client_get_hardcore_enabled(ach->client) != 0;
}

size_t nk_ach_take_events(NkAch *ach, NkAchEvent *out, size_t max) {
    if (!ach || !out) return 0u;
    size_t n = 0;
    while (n < max && ach->event_count > 0u) {
        out[n++] = ach->events[ach->event_head];
        ach->event_head = (ach->event_head + 1u) % NK_ACH_EVENT_QUEUE;
        ach->event_count--;
    }
    return n;
}

uint32_t nk_ach_events_dropped(const NkAch *ach) {
    return ach ? ach->events_dropped : 0u;
}

struct rc_client_t *nk_ach_rc_client(NkAch *ach) {
    return ach ? ach->client : NULL;
}
