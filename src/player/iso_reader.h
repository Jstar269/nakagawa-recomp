/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NAKAGAWA_ISO_READER_H
#define NAKAGAWA_ISO_READER_H

#include "player_state.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool success;
    char disc_id[MAX_DISC_ID_LEN];
    char title_name[MAX_TITLE_LEN];
    char disc_version[16];
    char error_message[256];
    uint64_t file_size;
    bool is_supported;
    GameSupportStatus status;
} IsoInspectResult;

/* Inspects a raw PSP ISO9660 disc image directly */
bool iso_inspect_file(const char *iso_path, IsoInspectResult *out_result);

#endif /* NAKAGAWA_ISO_READER_H */
