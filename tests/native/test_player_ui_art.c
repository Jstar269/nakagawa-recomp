/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The next UI's disc-art loader and launcher sounds (src/player/ui/player_ui_art.c,
   player_ui_sound.c). The art reader is a fake: it hands back project-owned PNG
   bytes (the embedded Kenney glyphs) and hostile ones, so no disc image is
   needed. The sounds run on SDL's dummy audio driver, so no device is opened. */

#include "player_ui_art.h"
#include "player_ui_sound.h"
#include "nk_ui_assets.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL (line %d): %s\n", __LINE__, message); \
        return 0; \
    } \
} while (0)

static SDL_AtomicInt g_reads;

static uint8_t *copy_bytes(const unsigned char *bytes, size_t size) {
    uint8_t *copy = (uint8_t *)malloc(size);
    if (copy != NULL) memcpy(copy, bytes, size);
    return copy;
}

static uint8_t *embedded_copy(const char *name, size_t *size) {
    const NkUiEmbeddedAsset *asset = nk_ui_embedded_asset(name);
    *size = asset != NULL ? asset->size : 0u;
    return asset != NULL ? copy_bytes(asset->bytes, asset->size) : NULL;
}

/* "good": both images; "bad": bytes that are not a PNG; "huge": a PNG header
   claiming 5000 x 5000; "slow": the icon after 50 ms; anything else: nothing. */
static void fake_reader(const char *iso_path, unsigned wanted, uint8_t **icon,
                        size_t *icon_size, uint8_t **picture, size_t *picture_size) {
    static const unsigned char huge[33] = {
        0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n', 0u, 0u, 0u, 13u, 'I', 'H', 'D', 'R',
        0u, 0u, 0x13u, 0x88u, 0u, 0u, 0x13u, 0x88u, 8u, 6u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
    };
    *icon = NULL;
    *icon_size = 0u;
    *picture = NULL;
    *picture_size = 0u;
    SDL_AddAtomicInt(&g_reads, 1);
    if (strcmp(iso_path, "slow") == 0) {
        SDL_Delay(50u);
    }
    if (strcmp(iso_path, "good") == 0 || strcmp(iso_path, "slow") == 0) {
        if ((wanted & PLAYER_UI_ART_ICON) != 0u) {
            *icon = embedded_copy("input-prompts/xbox_button_a.png", icon_size);
        }
        if ((wanted & PLAYER_UI_ART_PICTURE) != 0u) {
            *picture = embedded_copy("input-prompts/kbd_enter.png", picture_size);
        }
    } else if (strcmp(iso_path, "bad") == 0) {
        *icon = copy_bytes((const unsigned char *)"GIF89a, not a PNG", 17u);
        *icon_size = 17u;
    } else if (strcmp(iso_path, "huge") == 0) {
        *icon = copy_bytes(huge, sizeof(huge));
        *icon_size = sizeof(huge);
    }
}

/* Pumps frames until the loader is idle (at most two seconds). */
static int settle(PlayerUiArt *art) {
    int i;
    for (i = 0; i < 400; ++i) {
        player_ui_art_pump(art);
        if (!player_ui_art_busy(art)) return 1;
        SDL_Delay(5u);
    }
    return 0;
}

static int test_art_loads_once_and_falls_back(void) {
    PlayerUiArt *art = player_ui_art_create(fake_reader);
    const NkUiImage *icon = NULL;
    const NkUiImage *picture = NULL;
    int reads;
    CHECK(player_ui_art_create(NULL) == NULL, "a reader is required");
    CHECK(art != NULL, "create the loader");
    player_ui_art_pump(art);
    player_ui_art_get(art, "good", false, &icon, &picture);
    CHECK(icon == NULL && picture == NULL, "nothing before the read");
    CHECK(settle(art), "the icon read finishes");
    player_ui_art_get(art, "good", false, &icon, &picture);
    CHECK(icon != NULL && icon->width == 64u && icon->height == 64u && picture == NULL,
          "the icon is decoded; the picture was not asked for");
    player_ui_art_get(art, "good", true, &icon, &picture);
    CHECK(settle(art), "the picture read finishes");
    player_ui_art_get(art, "good", true, &icon, &picture);
    CHECK(icon != NULL && picture != NULL && picture->width == 64u,
          "the picture joins the icon");
    reads = SDL_GetAtomicInt(&g_reads);
    CHECK(reads == 2, "one read for the icon, one for the picture");

    player_ui_art_get(art, "bad", true, &icon, &picture);
    player_ui_art_get(art, "huge", false, &icon, NULL);
    player_ui_art_get(art, "none", true, &icon, &picture);
    CHECK(settle(art), "the failing reads finish");
    player_ui_art_get(art, "bad", true, &icon, &picture);
    CHECK(icon == NULL && picture == NULL, "a file that is not a PNG leaves the placeholder");
    player_ui_art_get(art, "huge", false, &icon, NULL);
    CHECK(icon == NULL, "an oversized PNG is refused, not decoded");
    player_ui_art_get(art, "none", true, &icon, &picture);
    CHECK(icon == NULL && picture == NULL, "a disc without art has none");
    reads = SDL_GetAtomicInt(&g_reads);
    CHECK(settle(art) && SDL_GetAtomicInt(&g_reads) == reads, "a settled disc is never read again");
    player_ui_art_get(art, "", false, &icon, NULL);
    player_ui_art_get(art, NULL, false, &icon, NULL);
    CHECK(icon == NULL && !player_ui_art_busy(art), "an empty path asks for nothing");
    player_ui_art_destroy(art);
    player_ui_art_destroy(NULL);
    return 1;
}

static int test_art_slots_and_shutdown(void) {
    PlayerUiArt *art = player_ui_art_create(fake_reader);
    const NkUiImage *icon = NULL;
    char path[32];
    unsigned i;
    CHECK(art != NULL, "create the loader");
    player_ui_art_pump(art);
    /* Every slot shown in one frame: one more disc gets no slot rather than
       evicting an image the frame may already draw. */
    for (i = 0; i < PLAYER_UI_ART_SLOTS; ++i) {
        snprintf(path, sizeof(path), "disc-%u", i);
        player_ui_art_get(art, path, false, &icon, NULL);
    }
    player_ui_art_get(art, "good", false, &icon, NULL);
    CHECK(settle(art), "the slot reads finish");
    player_ui_art_get(art, "good", false, &icon, NULL);
    CHECK(icon == NULL, "on a later frame the least recently shown slot is reused");
    CHECK(settle(art), "the evicting read finishes");
    player_ui_art_get(art, "good", false, &icon, NULL);
    CHECK(icon != NULL, "the reused slot holds the new disc's icon");
    /* Destroying with a read in flight waits for it. */
    player_ui_art_get(art, "slow", false, &icon, NULL);
    player_ui_art_pump(art);
    CHECK(player_ui_art_busy(art), "the slow read runs");
    player_ui_art_destroy(art);
    return 1;
}

static int test_sounds(void) {
    PlayerUiSounds *sounds = player_ui_sounds_create();
    int i;
    CHECK(sounds != NULL, "create the sounds");
    for (i = 0; i < PLAYER_UI_SOUND_COUNT; ++i) {
        const NkUiEmbeddedAsset *asset =
            nk_ui_embedded_asset(player_ui_sound_asset_name((PlayerUiSound)i));
        SDL_AudioSpec spec;
        Uint8 *pcm = NULL;
        Uint32 size = 0u;
        CHECK(asset != NULL, "every sound is embedded");
        CHECK(SDL_LoadWAV_IO(SDL_IOFromConstMem(asset->bytes, asset->size), true, &spec, &pcm,
                             &size), "every clip decodes as WAV");
        CHECK(spec.format == SDL_AUDIO_S16LE && spec.channels == 1 && spec.freq == 44100 &&
              size > 0u, "every clip is 44.1 kHz mono 16-bit");
        SDL_free(pcm);
    }
    CHECK(player_ui_sound_asset_name(PLAYER_UI_SOUND_COUNT) == NULL, "no clip past the set");
    CHECK(!player_ui_sounds_enabled(sounds), "sounds start off");
    player_ui_sounds_play(sounds, PLAYER_UI_SOUND_CONFIRM);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0, "while off, SDL audio is never started");
    player_ui_sounds_set_enabled(sounds, true);
    CHECK(player_ui_sounds_enabled(sounds) && SDL_WasInit(SDL_INIT_AUDIO) == 0,
          "turning them on opens nothing until a sound plays");
    player_ui_sounds_play(sounds, PLAYER_UI_SOUND_MOVE);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) != 0, "the first sound opens the (dummy) device");
    player_ui_sounds_play(sounds, PLAYER_UI_SOUND_BACK);
    player_ui_sounds_play(sounds, PLAYER_UI_SOUND_COUNT);
    player_ui_sounds_set_enabled(sounds, false);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0, "turning them off stops SDL audio again");
    player_ui_sounds_destroy(sounds);
    player_ui_sounds_destroy(NULL);
    return 1;
}

int main(void) {
    SDL_SetAtomicInt(&g_reads, 0);
    (void)SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!test_art_loads_once_and_falls_back() || !test_art_slots_and_shutdown() ||
        !test_sounds()) {
        return 1;
    }
    puts("player UI disc art and launcher sounds passed");
    return 0;
}
