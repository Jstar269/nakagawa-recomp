// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* The RetroAchievements bridge (src/core/nk_achievements.c, #1316) on the vendored rcheevos
 * rc_client, against an in-process fake server: no socket, no network (AGENTS.md section 5).
 * The fake answers in the response shapes rcheevos' parsers read (src/rapi/rc_api_user.c,
 * src/rapi/rc_api_runtime.c); the game, achievement and user are synthetic. */

#include "nk_achievements.h"

#include "rc_consoles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(cond, ...)                                                                     \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            g_failures++;                                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                             \
            fprintf(stderr, __VA_ARGS__);                                                    \
            fputc('\n', stderr);                                                             \
        }                                                                                    \
    } while (0)

#define USER_RAM 0x08800000u
#define USER_RAM_SIZE 0x01800000u /* 24 MB */
#define HASH "0123456789abcdef0123456789abcdef"
#define ACH_ADDR 0x00100u           /* user RAM offset the synthetic achievement watches */

/* ---- the fake server ------------------------------------------------------------------ */

typedef struct Fake {
    int requests;
    int refused_urls;        /* requests that reached the transport with a disallowed URL */
    char log[16][512];       /* the post data of each request, in order */
    int award_status;        /* the HTTP status awardachievement answers */
} Fake;

static const char k_login[] =
    "{\"Success\":true,\"User\":\"Player\",\"Token\":\"TOKEN\",\"Score\":0,"
    "\"SoftcoreScore\":12,\"Messages\":0}";
static const char k_sets[] =
    "{\"Success\":true,\"GameId\":1234,\"Title\":\"Synthetic Game\",\"ConsoleId\":41,"
    "\"ImageIconUrl\":\"\",\"RichPresenceGameId\":1234,\"RichPresencePatch\":\"\","
    "\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1234,\"Title\":null,\"Type\":\"core\","
    "\"ImageIconUrl\":\"\",\"Achievements\":["
    "{\"ID\":5501,\"Title\":\"Five\",\"Description\":\"The watched byte reads 5\","
    "\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH800100=5\",\"Author\":\"Synthetic\","
    "\"BadgeName\":\"00001\",\"Created\":1700000000,\"Modified\":1700000000}"
    "],\"Leaderboards\":[]}]}";
static const char k_session[] =
    "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[],\"ServerNow\":1700000000}";
static const char k_award[] =
    "{\"Success\":true,\"Score\":0,\"SoftcoreScore\":17,\"AchievementID\":5501,"
    "\"AchievementsRemaining\":0}";
static const char k_ok[] = "{\"Success\":true}";

static int starts_with(const char *s, const char *prefix) {
    return s && strncmp(s, prefix, strlen(prefix)) == 0;
}

static void fake_transport(void *ctx, const NkAchRequest *req, NkAchDone done, void *done_ctx) {
    Fake *f = (Fake *)ctx;
    const char *post = req->post_data ? req->post_data : "";
    if (f->requests < 16) snprintf(f->log[f->requests], sizeof f->log[0], "%s", post);
    f->requests++;
    if (!nk_ach_url_allowed(req->url)) f->refused_urls++;
    NkAchResponse r = {k_ok, sizeof k_ok - 1u, 200};
    if (starts_with(post, "r=login2&")) r.body = k_login;
    else if (starts_with(post, "r=achievementsets&")) r.body = k_sets;
    else if (starts_with(post, "r=startsession&")) r.body = k_session;
    else if (starts_with(post, "r=awardachievement&")) {
        r.body = k_award;
        r.http_status = f->award_status;
        if (f->award_status < 0) r.body = "";
    }
    r.body_length = strlen(r.body);
    done(&r, done_ctx);
}

static int find_request(const Fake *f, const char *prefix) {
    for (int i = 0; i < f->requests && i < 16; i++)
        if (starts_with(f->log[i], prefix)) return i;
    return -1;
}

static size_t take(NkAch *ach, NkAchEvent *ev, size_t max) { return nk_ach_take_events(ach, ev, max); }

static int has_event(const NkAchEvent *ev, size_t n, NkAchEventKind kind) {
    for (size_t i = 0; i < n; i++)
        if (ev[i].kind == kind) return (int)i;
    return -1;
}

/* ---- tests ------------------------------------------------------------------------------ */

static void test_url_gate(void) {
    CHECK(nk_ach_url_allowed("https://retroachievements.org/dorequest.php"), "RA server");
    static const char *refused[] = {
        "http://retroachievements.org/dorequest.php",
        "https://retroachievements.org.example.net/dorequest.php",
        "https://retroachievements.org:8443/dorequest.php",
        "https://user@retroachievements.org/dorequest.php",
        "https://example.net/?https://retroachievements.org/",
        "https://media.retroachievements.org/Badge/00001.png",
        "https://RETROACHIEVEMENTS.ORG/dorequest.php",
        "https://retroachievements.org",
        "",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        CHECK(!nk_ach_url_allowed(refused[i]), "refuses %s", refused[i]);
    CHECK(!nk_ach_url_allowed(NULL), "refuses NULL");
}

static void test_memory_map(uint8_t *ram) {
    /* The map the bridge relies on: RA 0..0x03FFFFFF is guest 0x08000000.. in one piece. */
    const rc_memory_regions_t *regions = rc_console_memory_regions(NK_ACH_CONSOLE_PSP);
    CHECK(regions && regions->num_regions == 3u, "the PSP map has three regions");
    if (regions) {
        uint32_t next = 0u;
        for (uint32_t i = 0; i < regions->num_regions; i++) {
            CHECK(regions->region[i].start_address == next, "region %u starts where the last ended", i);
            CHECK(regions->region[i].real_address == 0x08000000u + regions->region[i].start_address,
                  "region %u is guest 0x08000000 + its RA address", i);
            next = regions->region[i].end_address + 1u;
        }
        CHECK(next == 0x04000000u, "the map ends at RA 0x04000000");
    }

    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    uint8_t buf[8];
    ram[ACH_ADDR] = 0x5a;
    CHECK(nk_ach_read_memory(&mem, 0x00800000u + ACH_ADDR, buf, 1u) == 1u && buf[0] == 0x5a,
          "RA 0x00800100 is guest 0x08800100");
    /* Kernel RAM (RA below 0x00800000) and extended RAM are outside this span: nothing read. */
    CHECK(nk_ach_read_memory(&mem, 0x00000000u, buf, 4u) == 0u, "kernel RAM is outside the span");
    CHECK(nk_ach_read_memory(&mem, 0x007ffffcu, buf, 8u) == 0u, "a read starting below the span");
    CHECK(nk_ach_read_memory(&mem, 0x02000000u, buf, 4u) == 0u, "extended RAM is outside the span");
    CHECK(nk_ach_read_memory(&mem, 0x04000000u, buf, 4u) == 0u, "past the map");
    CHECK(nk_ach_read_memory(&mem, 0xfffffffcu, buf, 4u) == 0u, "the top of the address space");
    /* A read that runs off the end of the span stops there. */
    memset(buf, 0xee, sizeof buf);
    ram[USER_RAM_SIZE - 2u] = 1;
    ram[USER_RAM_SIZE - 1u] = 2;
    CHECK(nk_ach_read_memory(&mem, 0x00800000u + USER_RAM_SIZE - 2u, buf, 4u) == 2u &&
              buf[0] == 1 && buf[1] == 2 && buf[2] == 0xee,
          "a read at the end of the span stops at its last byte");
    const NkAchMemory none = {NULL, USER_RAM, USER_RAM_SIZE};
    CHECK(nk_ach_read_memory(&none, 0x00800000u, buf, 1u) == 0u, "no memory, nothing read");
}

/* No transport: rc_client's requests are refused before anything is sent. */
static void test_no_transport_sends_nothing(uint8_t *ram) {
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, NULL, NULL);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    CHECK(!nk_ach_hardcore(ach), "softcore");
    CHECK(!nk_ach_begin_login(ach, "", "TOKEN"), "an empty name is refused");
    CHECK(!nk_ach_begin_login(ach, "Player", NULL), "a missing token is refused");
    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "login starts");
    NkAchEvent ev[8];
    const size_t n = take(ach, ev, 8);
    CHECK(has_event(ev, n, NK_ACH_EVENT_LOGIN_FAILED) >= 0, "login fails with no transport");
    CHECK(!nk_ach_logged_in(ach), "not logged in");
    nk_ach_destroy(ach);
}

static void test_login_load_unlock(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = 200;
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];

    CHECK(nk_ach_begin_login(ach, "Player", "TOKEN"), "login starts");
    size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int login = has_event(ev, n, NK_ACH_EVENT_LOGIN_OK);
    CHECK(login >= 0, "login succeeds");
    if (login >= 0) CHECK(strcmp(ev[login].title, "Player") == 0, "display name %s", ev[login].title);
    CHECK(nk_ach_logged_in(ach), "logged in");
    const int lr = find_request(&fake, "r=login2&");
    CHECK(lr >= 0 && strstr(fake.log[lr], "u=Player") && strstr(fake.log[lr], "t=TOKEN") &&
              !strstr(fake.log[lr], "p="),
          "login sends the name and token, never a password: %s", lr >= 0 ? fake.log[lr] : "");

    CHECK(!nk_ach_begin_load_game(ach, "0123456789ABCDEF0123456789ABCDEF"), "upper-case hash");
    CHECK(!nk_ach_begin_load_game(ach, "0123"), "short hash");
    CHECK(nk_ach_begin_load_game(ach, HASH), "load starts");
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int loaded = has_event(ev, n, NK_ACH_EVENT_GAME_LOADED);
    CHECK(loaded >= 0, "game loads");
    if (loaded >= 0)
        CHECK(ev[loaded].id == 1234u && ev[loaded].points == 1u, "game 1234 with one achievement "
              "(id %u, %u)", ev[loaded].id, ev[loaded].points);
    CHECK(nk_ach_game_loaded(ach), "game loaded");
    const int sets = find_request(&fake, "r=achievementsets&");
    CHECK(sets >= 0 && strstr(fake.log[sets], "m=" HASH), "the hash identifies the game");
    const int session = find_request(&fake, "r=startsession&");
    CHECK(session >= 0 && strstr(fake.log[session], "&h=0"), "the session is softcore: %s",
          session >= 0 ? fake.log[session] : "");

    /* The achievement must be seen false before it can trigger. */
    for (int i = 0; i < 4; i++) nk_ach_frame(ach);
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_UNLOCKED) < 0, "no unlock while the byte reads 0");
    CHECK(find_request(&fake, "r=awardachievement&") < 0, "no award request yet");

    ram[ACH_ADDR] = 5;
    nk_ach_frame(ach);
    n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    const int unlocked = has_event(ev, n, NK_ACH_EVENT_UNLOCKED);
    CHECK(unlocked >= 0, "the achievement unlocks when guest 0x08800100 reads 5");
    if (unlocked >= 0)
        CHECK(ev[unlocked].id == 5501u && ev[unlocked].points == 5u &&
                  strcmp(ev[unlocked].title, "Five") == 0,
              "unlock 5501 Five 5 (got %u %s %u)", ev[unlocked].id, ev[unlocked].title,
              ev[unlocked].points);
    const int award = find_request(&fake, "r=awardachievement&");
    CHECK(award >= 0 && strstr(fake.log[award], "a=5501") && strstr(fake.log[award], "&h=0"),
          "a softcore award for 5501 is sent: %s", award >= 0 ? fake.log[award] : "");
    CHECK(fake.refused_urls == 0, "every request named the RetroAchievements server");
    CHECK(!nk_ach_hardcore(ach), "still softcore");
    nk_ach_destroy(ach);
}

/* An award that cannot reach the server stays pending: rc_client reports a disconnect. */
static void test_award_offline_is_pending(uint8_t *ram) {
    Fake fake;
    memset(&fake, 0, sizeof fake);
    fake.award_status = NK_ACH_HTTP_RETRY;
    memset(ram, 0, USER_RAM_SIZE);
    const NkAchMemory mem = {ram, USER_RAM, USER_RAM_SIZE};
    NkAch *ach = nk_ach_create(&mem, fake_transport, &fake);
    CHECK(ach != NULL, "create");
    if (!ach) return;
    NkAchEvent ev[NK_ACH_EVENT_QUEUE];
    nk_ach_begin_login(ach, "Player", "TOKEN");
    nk_ach_begin_load_game(ach, HASH);
    for (int i = 0; i < 4; i++) nk_ach_frame(ach);
    (void)take(ach, ev, NK_ACH_EVENT_QUEUE);
    ram[ACH_ADDR] = 5;
    nk_ach_frame(ach);
    const size_t n = take(ach, ev, NK_ACH_EVENT_QUEUE);
    CHECK(has_event(ev, n, NK_ACH_EVENT_UNLOCKED) >= 0, "the unlock is still shown");
    CHECK(has_event(ev, n, NK_ACH_EVENT_DISCONNECTED) >= 0, "the award is pending");
    CHECK(has_event(ev, n, NK_ACH_EVENT_SERVER_ERROR) < 0, "a retry is not a server error");
    nk_ach_destroy(ach);
}

int main(void) {
    uint8_t *ram = (uint8_t *)calloc(1u, USER_RAM_SIZE);
    if (!ram) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    test_url_gate();
    test_memory_map(ram);
    test_no_transport_sends_nothing(ram);
    test_login_load_unlock(ram);
    test_award_offline_is_pending(ram);
    free(ram);
    if (g_failures) {
        fprintf(stderr, "test_nk_achievements: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_nk_achievements: PASS\n");
    return 0;
}
