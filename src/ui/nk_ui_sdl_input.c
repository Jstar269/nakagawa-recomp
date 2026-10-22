/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui.h"

/* C3a reserves this edge without importing SDL into the core. */
bool nk_ui_sdl_input_available(void) {
    return false;
}
