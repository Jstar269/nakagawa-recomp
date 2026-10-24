/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_sound.h"

#include "nk_ui_assets.h"

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

/* UI sounds sit well under the game's level. */
#define PLAYER_UI_SOUND_GAIN 0.5f

typedef struct {
    Uint8 *pcm;
    Uint32 size;
    SDL_AudioSpec spec;
} PlayerUiClip;

struct PlayerUiSounds {
    bool enabled;
    bool audio_started;   /* this module started SDL's audio subsystem */
    bool failed;          /* the device could not open: stay silent */
    SDL_AudioStream *stream;
    PlayerUiClip clips[PLAYER_UI_SOUND_COUNT];
};

const char *player_ui_sound_asset_name(PlayerUiSound sound) {
    switch (sound) {
    case PLAYER_UI_SOUND_MOVE: return "interface-sounds/tick_001.wav";
    case PLAYER_UI_SOUND_CONFIRM: return "interface-sounds/confirmation_001.wav";
    case PLAYER_UI_SOUND_BACK: return "interface-sounds/back_001.wav";
    case PLAYER_UI_SOUND_COUNT:
    default: return NULL;
    }
}

PlayerUiSounds *player_ui_sounds_create(void) {
    return (PlayerUiSounds *)calloc(1u, sizeof(PlayerUiSounds));
}

static void player_ui_sounds_close(PlayerUiSounds *sounds) {
    size_t i;
    if (sounds->stream != NULL) {
        SDL_DestroyAudioStream(sounds->stream);
        sounds->stream = NULL;
    }
    for (i = 0; i < PLAYER_UI_SOUND_COUNT; ++i) {
        SDL_free(sounds->clips[i].pcm);
        memset(&sounds->clips[i], 0, sizeof(sounds->clips[i]));
    }
    if (sounds->audio_started) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sounds->audio_started = false;
    }
}

void player_ui_sounds_destroy(PlayerUiSounds *sounds) {
    if (sounds == NULL) {
        return;
    }
    player_ui_sounds_close(sounds);
    free(sounds);
}

void player_ui_sounds_set_enabled(PlayerUiSounds *sounds, bool enabled) {
    if (sounds == NULL || sounds->enabled == enabled) {
        return;
    }
    sounds->enabled = enabled;
    sounds->failed = false;
    if (!enabled) {
        player_ui_sounds_close(sounds);
    }
}

bool player_ui_sounds_enabled(const PlayerUiSounds *sounds) {
    return sounds != NULL && sounds->enabled;
}

/* Decodes the clips and opens one stream on the default playback device. Every
   clip shares the first clip's format (the embedded set is 44.1 kHz mono
   16-bit); a clip in another format is left silent. */
static bool player_ui_sounds_open(PlayerUiSounds *sounds) {
    size_t i;
    if (sounds->stream != NULL) {
        return true;
    }
    if (sounds->failed) {
        return false;
    }
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            sounds->failed = true;
            return false;
        }
        sounds->audio_started = true;
    }
    for (i = 0; i < PLAYER_UI_SOUND_COUNT; ++i) {
        const NkUiEmbeddedAsset *asset =
            nk_ui_embedded_asset(player_ui_sound_asset_name((PlayerUiSound)i));
        SDL_IOStream *io = asset != NULL ? SDL_IOFromConstMem(asset->bytes, asset->size) : NULL;
        if (io == NULL || !SDL_LoadWAV_IO(io, true, &sounds->clips[i].spec,
                                          &sounds->clips[i].pcm, &sounds->clips[i].size)) {
            memset(&sounds->clips[i], 0, sizeof(sounds->clips[i]));
        }
    }
    if (sounds->clips[0].pcm == NULL) {
        sounds->failed = true;
        player_ui_sounds_close(sounds);
        return false;
    }
    sounds->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                               &sounds->clips[0].spec, NULL, NULL);
    if (sounds->stream == NULL) {
        sounds->failed = true;
        player_ui_sounds_close(sounds);
        return false;
    }
    (void)SDL_SetAudioStreamGain(sounds->stream, PLAYER_UI_SOUND_GAIN);
    (void)SDL_ResumeAudioStreamDevice(sounds->stream);
    return true;
}

void player_ui_sounds_play(PlayerUiSounds *sounds, PlayerUiSound sound) {
    const PlayerUiClip *clip;
    if (sounds == NULL || !sounds->enabled || (unsigned)sound >= (unsigned)PLAYER_UI_SOUND_COUNT ||
        !player_ui_sounds_open(sounds)) {
        return;
    }
    clip = &sounds->clips[sound];
    if (clip->pcm == NULL || clip->size == 0u ||
        clip->spec.format != sounds->clips[0].spec.format ||
        clip->spec.channels != sounds->clips[0].spec.channels ||
        clip->spec.freq != sounds->clips[0].spec.freq || clip->size > (Uint32)SDL_MAX_SINT32) {
        return;
    }
    (void)SDL_ClearAudioStream(sounds->stream);
    (void)SDL_PutAudioStreamData(sounds->stream, clip->pcm, (int)clip->size);
}
