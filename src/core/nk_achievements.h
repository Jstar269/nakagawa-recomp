/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_ACHIEVEMENTS_H
#define NK_ACHIEVEMENTS_H

/* RetroAchievements (#1316): achievements for a PSP title, evaluated on guest memory by
 * rcheevos (third_party/rcheevos/, RetroAchievements' own C library) and its rc_client.
 *
 * This module is the bridge between rc_client and Nakagawa. It holds no network code and opens
 * no connection: every request rc_client makes reaches the caller's NkAchTransport, and only
 * after nk_ach_url_allowed() has passed its URL. The caller is the one place that may carry it
 * to the network, as an opt-in feature the user turned on (AGENTS.md section 5; the "Network
 * use" table in docs/SETUP.md lists it once that transport ships). With no transport, nothing
 * is sent and every request fails as a refused client call.
 *
 * Softcore only: hardcore mode needs RetroAchievements' compliance review first, so
 * nk_ach_create turns hardcore off and nothing here turns it on. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The only host rc_client's requests may name. rcheevos' default host is
 * "https://retroachievements.org" (src/rapi/rc_api_common.c), and this bridge never changes it. */
#define NK_ACH_HOST "retroachievements.org"
#define NK_ACH_URL_PREFIX "https://" NK_ACH_HOST "/"

/* RetroAchievements' console ID for the PSP (rc_consoles.h RC_CONSOLE_PSP). Its memory map
 * (src/rcheevos/consoleinfo.c) puts RA address 0 at guest 0x08000000: kernel RAM, user RAM
 * from 0x08800000, and extended RAM from 0x0A000000, up to RA address 0x03FFFFFF. */
#define NK_ACH_CONSOLE_PSP 41u

/* Guest memory as the bridge may read it: the guest bytes [guest_start, guest_start + size)
 * live at host address base. A read outside that span reads nothing. */
typedef struct NkAchMemory {
    const uint8_t *base;
    uint32_t guest_start;
    uint32_t size;
} NkAchMemory;

/* One request rc_client wants sent, after nk_ach_url_allowed() passed its URL. */
typedef struct NkAchRequest {
    const char *url;
    const char *post_data;    /* may be NULL (a GET) */
    const char *content_type; /* may be NULL */
} NkAchRequest;

/* The answer: the HTTP status and the body. A transport that reached no server answers
 * NK_ACH_HTTP_CLIENT_ERROR (give up) or NK_ACH_HTTP_RETRY (rc_client retries later). */
#define NK_ACH_HTTP_CLIENT_ERROR (-1)
#define NK_ACH_HTTP_RETRY (-2)
typedef struct NkAchResponse {
    const char *body;
    size_t body_length;
    int http_status;
} NkAchResponse;

typedef void (*NkAchDone)(const NkAchResponse *response, void *done_ctx);
/* Sends one request and calls done exactly once, now or later on the thread that calls
 * nk_ach_frame. The request's strings are valid only until done is called. */
typedef void (*NkAchTransport)(void *ctx, const NkAchRequest *request, NkAchDone done,
                               void *done_ctx);

typedef enum NkAchEventKind {
    NK_ACH_EVENT_LOGIN_OK = 1,
    NK_ACH_EVENT_LOGIN_FAILED,
    NK_ACH_EVENT_GAME_LOADED,      /* id: the RA game ID; count: achievements in its core set */
    NK_ACH_EVENT_GAME_FAILED,
    NK_ACH_EVENT_UNLOCKED,         /* id, title, points: the achievement just earned */
    NK_ACH_EVENT_SERVER_ERROR,     /* message: the server's error, not retried */
    NK_ACH_EVENT_DISCONNECTED,     /* an unlock is waiting to be sent */
    NK_ACH_EVENT_RECONNECTED,      /* every waiting unlock was sent */
    NK_ACH_EVENT_DISABLED          /* id: an achievement rc_client turned off because guest memory
                                    * it reads could not be read; message: rc_client's note */
} NkAchEventKind;

typedef struct NkAchEvent {
    NkAchEventKind kind;
    uint32_t id;
    uint32_t points;   /* UNLOCKED and LOGIN_OK only; 0 otherwise */
    uint32_t count;    /* GAME_LOADED only; 0 otherwise */
    char title[96];
    char message[160];
} NkAchEvent;

#define NK_ACH_EVENT_QUEUE 32u

typedef struct NkAch NkAch;

/* True only for an https URL on NK_ACH_HOST itself (no other host, port, user part or
 * scheme). */
bool nk_ach_url_allowed(const char *url);

/* Reads up to n bytes at RA address ra_address through the PSP memory map. Returns the bytes
 * read: 0 when the address is outside the map or outside mem's span, fewer than n when the
 * span or the map ends first. The PSP's three regions are one run of guest memory, so a read may
 * cross from one into the next. Nothing outside mem's span is ever read. */
uint32_t nk_ach_read_memory(const NkAchMemory *mem, uint32_t ra_address, uint8_t *out,
                            uint32_t n);

/* A client over mem (copied) with transport, which may be NULL: then every request is refused
 * and nothing is sent. NULL when rc_client cannot be created. Hardcore is off. */
NkAch *nk_ach_create(const NkAchMemory *mem, NkAchTransport transport, void *transport_ctx);
void nk_ach_destroy(NkAch *ach);

/* Signs in with an RA API token (never a password: the token comes from the user's own
 * sign-in, kept in the OS credential store by the player). False only for a missing name or
 * token; otherwise the outcome arrives as a LOGIN_OK or LOGIN_FAILED event. */
bool nk_ach_begin_login(NkAch *ach, const char *username, const char *token);
/* Loads the achievement set for the disc whose RA hash (32 lowercase hex digits, the MD5 RA's
 * PSP rule takes of PARAM.SFO then EBOOT.BIN) is hash. False only for a malformed hash;
 * otherwise the outcome is a GAME_LOADED or GAME_FAILED event. */
bool nk_ach_begin_load_game(NkAch *ach, const char *hash);

/* Once per guest frame, at the vblank boundary: evaluates the achievements on guest memory. */
void nk_ach_frame(NkAch *ach);
/* While the guest is paused: keeps rc_client's retries and pings going without evaluating. */
void nk_ach_idle(NkAch *ach);

bool nk_ach_logged_in(const NkAch *ach);
bool nk_ach_game_loaded(const NkAch *ach);
/* Always false: see the softcore note at the top. */
bool nk_ach_hardcore(const NkAch *ach);

/* Moves up to max queued events, oldest first, into out; returns how many. When more than
 * NK_ACH_EVENT_QUEUE are waiting, the oldest are dropped and nk_ach_events_dropped counts
 * them. */
size_t nk_ach_take_events(NkAch *ach, NkAchEvent *out, size_t max);
uint32_t nk_ach_events_dropped(const NkAch *ach);

/* The rc_client behind ach, for the bridge's own tests: they point it at another host to prove
 * that the request gate refuses it. Nothing else needs it. */
struct rc_client_t *nk_ach_rc_client(NkAch *ach);

#ifdef __cplusplus
}
#endif

#endif /* NK_ACHIEVEMENTS_H */
