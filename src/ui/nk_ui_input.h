/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_INPUT_H
#define NK_UI_INPUT_H

#include "nk_ui.h"

typedef struct {
    uint32_t initial_delay_ms;
    uint32_t slow_ms;
    uint32_t normal_ms;
    uint32_t fast_ms;
} NkUiRepeatConfig;

bool nk_ui_input_reserved_api(void);

#endif /* NK_UI_INPUT_H */
