/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "pcm_wav_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* dr_wav (Unlicense option elected; pinned in third_party/dr_libs/PROVENANCE.md).
   Only the writer is used here; the reader serves the round-trip test. */
#define DR_WAV_IMPLEMENTATION
#include "../../third_party/dr_libs/dr_wav.h"

#define NK_PCM_WAV_MAX_CHANNELS 8u
#define NK_PCM_WAV_MAX_RATE 384000u
#define NK_PCM_WAV_MAX_NAME 64u

int nk_pcm_wav_write(const char *path, const int16_t *pcm, size_t frames,
                     unsigned channels, unsigned sample_rate) {
    drwav_data_format format;
    drwav wav;
    drwav_uint64 written;
    if (path == NULL || path[0] == '\0' || (pcm == NULL && frames != 0u) ||
        channels == 0u || channels > NK_PCM_WAV_MAX_CHANNELS ||
        sample_rate == 0u || sample_rate > NK_PCM_WAV_MAX_RATE) {
        return 0;
    }
    /* The data chunk size is a 32-bit field: refuse what cannot be described. */
    if (frames > (size_t)(0xFFFFFFFFu - 44u) / (2u * channels)) {
        return 0;
    }
    memset(&format, 0, sizeof(format));
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = channels;
    format.sampleRate = sample_rate;
    format.bitsPerSample = 16;
    if (!drwav_init_file_write(&wav, path, &format, NULL)) {
        return 0;
    }
    written = frames != 0u ? drwav_write_pcm_frames(&wav, (drwav_uint64)frames, pcm) : 0u;
    if (drwav_uninit(&wav) != DRWAV_SUCCESS || written != (drwav_uint64)frames) {
        remove(path);
        return 0;
    }
    return 1;
}

static int nk_pcm_wav_name_ok(const char *name) {
    size_t n = 0;
    if (name == NULL) {
        return 0;
    }
    for (; name[n] != '\0'; ++n) {
        char c = name[n];
        if (n >= NK_PCM_WAV_MAX_NAME ||
            !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
    }
    return n != 0u;
}

int nk_pcm_wav_dump(const char *name, const int16_t *pcm, size_t frames,
                    unsigned channels, unsigned sample_rate) {
    const char *dir = getenv(NK_PCM_WAV_DIR_ENV);
    char *path;
    size_t need;
    int ok;
    if (dir == NULL || dir[0] == '\0') {
        return 0;
    }
    if (!nk_pcm_wav_name_ok(name)) {
        return -1;
    }
    need = strlen(dir) + 1u + strlen(name) + sizeof(".wav");
    path = (char *)malloc(need);
    if (path == NULL) {
        return -1;
    }
    snprintf(path, need, "%s/%s.wav", dir, name);
    ok = nk_pcm_wav_write(path, pcm, frames, channels, sample_rate);
    free(path);
    return ok ? 1 : -1;
}
