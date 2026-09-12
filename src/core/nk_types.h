/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_TYPES_H
#define NK_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_MAX_PATH 512
#define NK_MAX_TITLE_LEN 128
#define NK_MAX_DISC_ID_LEN 32
#define NK_MAX_GAMES 64

typedef enum {
    NK_OK = 0,
    NK_ERROR_GENERIC = -1,
    NK_ERROR_FILE_NOT_FOUND = -2,
    NK_ERROR_INVALID_ISO = -3,
    NK_ERROR_UNSUPPORTED_TITLE = -4,
    NK_ERROR_IO = -5,
    NK_ERROR_OUT_OF_MEMORY = -6,
    NK_ERROR_PROCESS_SPAWN = -7,
    NK_ERROR_PERMISSION = -8,
    NK_ERROR_CANCELLED = -9,
    NK_ERROR_ALREADY_EXISTS = -10
} NkResult;

typedef enum {
    NK_STATUS_UNIDENTIFIED = 0,
    NK_STATUS_IDENTIFIED,
    NK_STATUS_SUPPORTED_PREPARATION,
    NK_STATUS_PREPARED,
    NK_STATUS_BOOTS,
    NK_STATUS_PLAYABLE,
    NK_STATUS_VERIFIED
} NkGameSupportStatus;

typedef struct {
    char disc_id[NK_MAX_DISC_ID_LEN];
    char title_name[NK_MAX_TITLE_LEN];
    char disc_version[16];
    char iso_path[NK_MAX_PATH];
    char prepared_root[NK_MAX_PATH];
    char title_id[64];
    uint64_t iso_size_bytes;
    NkGameSupportStatus status;
    bool is_prepared;
    char last_played[32];
} NkGameEntry;

#ifdef __cplusplus
}
#endif

#endif /* NK_TYPES_H */
