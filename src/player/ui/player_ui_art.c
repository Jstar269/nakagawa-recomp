/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_art.h"

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

/* Long enough for any path the library stores (NK_MAX_PATH). */
#define PLAYER_UI_ART_PATH_MAX 1024u

typedef enum {
    PLAYER_UI_ART_NOT_WANTED = 0,
    PLAYER_UI_ART_WANTED,
    PLAYER_UI_ART_READING,
    PLAYER_UI_ART_DONE        /* decoded, or settled as absent */
} PlayerUiArtState;

typedef struct {
    char iso_path[PLAYER_UI_ART_PATH_MAX];  /* "" : free slot */
    NkUiImage icon;
    NkUiImage picture;
    PlayerUiArtState icon_state;
    PlayerUiArtState picture_state;
    uint64_t last_used;                     /* frame of the last get */
} PlayerUiArtEntry;

typedef struct {
    PlayerUiArtReader reader;
    char iso_path[PLAYER_UI_ART_PATH_MAX];
    unsigned wanted;
    NkUiImage icon;
    NkUiImage picture;
    SDL_AtomicInt done;
    SDL_Thread *thread;
} PlayerUiArtJob;

struct PlayerUiArt {
    PlayerUiArtReader reader;
    PlayerUiArtEntry entries[PLAYER_UI_ART_SLOTS];
    PlayerUiArtJob *job;
    size_t job_entry;
    uint64_t frame;
};

static void player_ui_art_decode(const uint8_t *bytes, size_t size, uint32_t max_side,
                                 NkUiImage *out) {
    NkUiImageLimits limits;
    limits.max_bytes = PLAYER_UI_ART_MAX_BYTES;
    limits.max_width = max_side;
    limits.max_height = max_side;
    if (bytes != NULL && size > 0u) {
        (void)nk_ui_image_decode_png(bytes, size, &limits, out);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

static int SDLCALL player_ui_art_worker(void *data) {
    PlayerUiArtJob *job = (PlayerUiArtJob *)data;
    uint8_t *icon = NULL;
    uint8_t *picture = NULL;
    size_t icon_size = 0u;
    size_t picture_size = 0u;
    job->reader(job->iso_path, job->wanted, &icon, &icon_size, &picture, &picture_size);
    if ((job->wanted & PLAYER_UI_ART_ICON) != 0u) {
        player_ui_art_decode(icon, icon_size, PLAYER_UI_ART_ICON_MAX_SIDE, &job->icon);
    }
    if ((job->wanted & PLAYER_UI_ART_PICTURE) != 0u) {
        player_ui_art_decode(picture, picture_size, PLAYER_UI_ART_PICTURE_MAX_SIDE,
                             &job->picture);
    }
    free(icon);
    free(picture);
    SDL_SetAtomicInt(&job->done, 1);
    return 0;
}

PlayerUiArt *player_ui_art_create(PlayerUiArtReader reader) {
    PlayerUiArt *art;
    if (reader == NULL) {
        return NULL;
    }
    art = (PlayerUiArt *)calloc(1u, sizeof(*art));
    if (art != NULL) {
        art->reader = reader;
    }
    return art;
}

static void player_ui_art_entry_clear(PlayerUiArtEntry *entry) {
    nk_ui_image_free(&entry->icon);
    nk_ui_image_free(&entry->picture);
    memset(entry, 0, sizeof(*entry));
}

/* Waits for the running job and hands its images to its entry. */
static void player_ui_art_finish(PlayerUiArt *art) {
    PlayerUiArtJob *job = art->job;
    PlayerUiArtEntry *entry;
    if (job == NULL) {
        return;
    }
    SDL_WaitThread(job->thread, NULL);
    entry = &art->entries[art->job_entry];
    if (strcmp(entry->iso_path, job->iso_path) == 0) {
        if ((job->wanted & PLAYER_UI_ART_ICON) != 0u) {
            entry->icon = job->icon;
            entry->icon_state = PLAYER_UI_ART_DONE;
            memset(&job->icon, 0, sizeof(job->icon));
        }
        if ((job->wanted & PLAYER_UI_ART_PICTURE) != 0u) {
            entry->picture = job->picture;
            entry->picture_state = PLAYER_UI_ART_DONE;
            memset(&job->picture, 0, sizeof(job->picture));
        }
    }
    nk_ui_image_free(&job->icon);
    nk_ui_image_free(&job->picture);
    free(job);
    art->job = NULL;
}

void player_ui_art_destroy(PlayerUiArt *art) {
    size_t i;
    if (art == NULL) {
        return;
    }
    player_ui_art_finish(art);
    for (i = 0; i < PLAYER_UI_ART_SLOTS; ++i) {
        player_ui_art_entry_clear(&art->entries[i]);
    }
    free(art);
}

/* Starts a read for the most recently shown entry that wants an image. */
static void player_ui_art_start(PlayerUiArt *art) {
    PlayerUiArtEntry *best = NULL;
    PlayerUiArtJob *job;
    size_t best_index = 0u;
    size_t i;
    for (i = 0; i < PLAYER_UI_ART_SLOTS; ++i) {
        PlayerUiArtEntry *entry = &art->entries[i];
        if (entry->iso_path[0] == '\0' ||
            (entry->icon_state != PLAYER_UI_ART_WANTED &&
             entry->picture_state != PLAYER_UI_ART_WANTED)) {
            continue;
        }
        if (best == NULL || entry->last_used > best->last_used) {
            best = entry;
            best_index = i;
        }
    }
    if (best == NULL) {
        return;
    }
    job = (PlayerUiArtJob *)calloc(1u, sizeof(*job));
    if (job == NULL) {
        return; /* try again next frame */
    }
    job->reader = art->reader;
    memcpy(job->iso_path, best->iso_path, sizeof(job->iso_path));
    if (best->icon_state == PLAYER_UI_ART_WANTED) job->wanted |= PLAYER_UI_ART_ICON;
    if (best->picture_state == PLAYER_UI_ART_WANTED) job->wanted |= PLAYER_UI_ART_PICTURE;
    job->thread = SDL_CreateThread(player_ui_art_worker, "nakagawa-disc-art", job);
    if (job->thread == NULL) {
        /* No thread: settle this disc as having no art rather than retrying
           every frame. */
        if ((job->wanted & PLAYER_UI_ART_ICON) != 0u) best->icon_state = PLAYER_UI_ART_DONE;
        if ((job->wanted & PLAYER_UI_ART_PICTURE) != 0u) best->picture_state = PLAYER_UI_ART_DONE;
        free(job);
        return;
    }
    if ((job->wanted & PLAYER_UI_ART_ICON) != 0u) best->icon_state = PLAYER_UI_ART_READING;
    if ((job->wanted & PLAYER_UI_ART_PICTURE) != 0u) best->picture_state = PLAYER_UI_ART_READING;
    art->job = job;
    art->job_entry = best_index;
}

void player_ui_art_pump(PlayerUiArt *art) {
    if (art == NULL) {
        return;
    }
    ++art->frame;
    if (art->job != NULL && SDL_GetAtomicInt(&art->job->done) != 0) {
        player_ui_art_finish(art);
    }
    if (art->job == NULL) {
        player_ui_art_start(art);
    }
}

/* The entry for a path, claiming a free slot or the least recently shown one
   that is neither shown this frame nor being read. NULL when none can go. */
static PlayerUiArtEntry *player_ui_art_entry(PlayerUiArt *art, const char *iso_path) {
    PlayerUiArtEntry *victim = NULL;
    size_t i;
    for (i = 0; i < PLAYER_UI_ART_SLOTS; ++i) {
        if (strcmp(art->entries[i].iso_path, iso_path) == 0) {
            return &art->entries[i];
        }
    }
    for (i = 0; i < PLAYER_UI_ART_SLOTS; ++i) {
        PlayerUiArtEntry *entry = &art->entries[i];
        if (entry->iso_path[0] == '\0') {
            victim = entry;
            break;
        }
        if (entry->last_used == art->frame || (art->job != NULL && i == art->job_entry)) {
            continue;
        }
        if (victim == NULL || entry->last_used < victim->last_used) {
            victim = entry;
        }
    }
    if (victim != NULL) {
        player_ui_art_entry_clear(victim);
        memcpy(victim->iso_path, iso_path, strlen(iso_path) + 1u);
    }
    return victim;
}

void player_ui_art_get(PlayerUiArt *art, const char *iso_path, bool want_picture,
                       const NkUiImage **icon, const NkUiImage **picture) {
    PlayerUiArtEntry *entry;
    if (icon != NULL) *icon = NULL;
    if (picture != NULL) *picture = NULL;
    if (art == NULL || iso_path == NULL || iso_path[0] == '\0' ||
        strlen(iso_path) >= PLAYER_UI_ART_PATH_MAX) {
        return;
    }
    entry = player_ui_art_entry(art, iso_path);
    if (entry == NULL) {
        return;
    }
    entry->last_used = art->frame;
    if (entry->icon_state == PLAYER_UI_ART_NOT_WANTED) {
        entry->icon_state = PLAYER_UI_ART_WANTED;
    }
    if (want_picture && entry->picture_state == PLAYER_UI_ART_NOT_WANTED) {
        entry->picture_state = PLAYER_UI_ART_WANTED;
    }
    if (icon != NULL && entry->icon.rgba_pm != NULL) *icon = &entry->icon;
    if (picture != NULL && entry->picture.rgba_pm != NULL) *picture = &entry->picture;
}

bool player_ui_art_busy(const PlayerUiArt *art) {
    size_t i;
    if (art == NULL) {
        return false;
    }
    if (art->job != NULL) {
        return true;
    }
    for (i = 0; i < PLAYER_UI_ART_SLOTS; ++i) {
        if (art->entries[i].icon_state == PLAYER_UI_ART_WANTED ||
            art->entries[i].picture_state == PLAYER_UI_ART_WANTED) {
            return true;
        }
    }
    return false;
}
