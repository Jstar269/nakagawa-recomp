/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_BACKEND_VK_H
#define NK_UI_BACKEND_VK_H

#include "nk_ui_draw.h"

typedef struct NkUiVk NkUiVk;
bool nk_ui_vk_available(void);

/* Draws one frame's list from nk_ui_end(). Returns true when the list was drawn.
   C3a ships a stub that draws nothing and returns false; C3c supplies the Vulkan
   implementation behind this declaration. */
bool nk_ui_vk_render(NkUiVk *vk, const NkUiDrawList *list);

#endif /* NK_UI_BACKEND_VK_H */
