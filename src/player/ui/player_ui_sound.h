/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef PLAYER_UI_SOUND_H
#define PLAYER_UI_SOUND_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional launcher sounds: Kenney's CC0 "Interface Sounds" clips, embedded by
   tools/gen_ui_assets.py, for moving focus, confirming and going back. Off by
   default (the ui_sounds setting). While they are off no audio device is
   opened and SDL's audio subsystem is never started. They never play while a
   game runs, so the game's own audio is left alone, and a missing or failing
   audio device leaves the launcher silent rather than reporting an error. */

typedef enum {
    PLAYER_UI_SOUND_MOVE = 0,
    PLAYER_UI_SOUND_CONFIRM,
    PLAYER_UI_SOUND_BACK,
    PLAYER_UI_SOUND_COUNT
} PlayerUiSound;

/* The embedded clip name (nk_ui_assets.h) for a sound, or NULL. */
const char *player_ui_sound_asset_name(PlayerUiSound sound);

typedef struct PlayerUiSounds PlayerUiSounds;

PlayerUiSounds *player_ui_sounds_create(void);
void player_ui_sounds_destroy(PlayerUiSounds *sounds);

/* Turning sounds on opens the default playback device on the first play;
   turning them off closes it and stops SDL audio again. */
void player_ui_sounds_set_enabled(PlayerUiSounds *sounds, bool enabled);
bool player_ui_sounds_enabled(const PlayerUiSounds *sounds);

/* Plays one clip, cutting off the one before it. Does nothing while disabled. */
void player_ui_sounds_play(PlayerUiSounds *sounds, PlayerUiSound sound);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_UI_SOUND_H */
