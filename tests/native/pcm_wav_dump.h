/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Test and debug helper: writes decoded PCM from a selftest (an ATRAC3+ or SAS
   output buffer, for example) to a WAV file through the vendored dr_wav writer
   (third_party/dr_libs/). It is compiled only into selftests and is never
   linked into the native player or the runtime. */

#ifndef NK_PCM_WAV_DUMP_H
#define NK_PCM_WAV_DUMP_H

#include <stddef.h>
#include <stdint.h>

/* The environment variable that asks selftests to dump their PCM. */
#define NK_PCM_WAV_DIR_ENV "NK_PCM_WAV_DIR"

/* Writes frames of interleaved signed 16-bit PCM as a 16-bit PCM WAV file.
   channels is 1 to 8 and sample_rate 1 to 384000; frames may be 0. Returns 1
   on success and 0 on a refused argument or a write failure, after which no
   partial file is kept. */
int nk_pcm_wav_write(const char *path, const int16_t *pcm, size_t frames,
                     unsigned channels, unsigned sample_rate);

/* When NK_PCM_WAV_DIR names a directory, writes <dir>/<name>.wav with
   nk_pcm_wav_write. name is 1 to 64 characters of [A-Za-z0-9_-]. Returns 1
   when written, 0 when the variable is unset or empty, and -1 on a refused
   name or a write failure. */
int nk_pcm_wav_dump(const char *name, const int16_t *pcm, size_t frames,
                    unsigned channels, unsigned sample_rate);

#endif /* NK_PCM_WAV_DUMP_H */
