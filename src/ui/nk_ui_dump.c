/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui_draw.h"

#include <stdarg.h>
#include <stdio.h>

static size_t nk_ui_dump_append(char *out, size_t cap, size_t used,
                                const char *format, ...) {
    va_list args;
    int needed;
    size_t available = used < cap ? cap - used : 0u;
    char *destination = out != NULL && available > 0u ? out + used : NULL;
    va_start(args, format);
    needed = vsnprintf(destination, available, format, args);
    va_end(args);
    if (needed < 0) {
        return used;
    }
    return used + (size_t)needed;
}

static size_t nk_ui_dump_append_text(char *out, size_t cap, size_t used,
                                     const char *text, size_t length) {
    size_t i;
    for (i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)text[i];
        if (value == '\\' || value == '"') {
            used = nk_ui_dump_append(out, cap, used, "\\%c", (int)value);
        } else if (value == '\n') {
            used = nk_ui_dump_append(out, cap, used, "\\n");
        } else if (value == '\r') {
            used = nk_ui_dump_append(out, cap, used, "\\r");
        } else if (value == '\t') {
            used = nk_ui_dump_append(out, cap, used, "\\t");
        } else if (value < 0x20u || value == 0x7Fu) {
            used = nk_ui_dump_append(out, cap, used, "\\x%02X", (unsigned)value);
        } else {
            used = nk_ui_dump_append(out, cap, used, "%c", (int)value);
        }
    }
    return used;
}

static const char *nk_ui_dump_kind(int kind, uint32_t custom_kind) {
    switch (kind) {
    case NK_UI_DRAW_RECTANGLE: return "RECT";
    case NK_UI_DRAW_BORDER: return "BORDER";
    case NK_UI_DRAW_TEXT: return "TEXT";
    case NK_UI_DRAW_SCISSOR_START: return "CLIP+";
    case NK_UI_DRAW_SCISSOR_END: return "CLIP-";
    case NK_UI_DRAW_CUSTOM:
        return custom_kind == NK_UI_CUSTOM_WAVES ? "WAVES"
             : custom_kind == NK_UI_CUSTOM_IMAGE ? "IMAGE" : "CUSTOM";
    default: return "NONE";
    }
}

size_t nk_ui_dump_draw_list(const NkUiDrawList *list, char *out, size_t cap) {
    size_t used = 0u;
    uint32_t i;
    if (cap > 0u && out != NULL) {
        out[0] = '\0';
    }
    if (list == NULL) {
        return 0u;
    }
    for (i = 0; i < list->ncmd; ++i) {
        const NkUiDrawCmd *command = &list->cmd[i];
        const char *text = "";
        size_t text_length = 0u;
        char image_size[32];
        if (command->kind == NK_UI_DRAW_CUSTOM && command->custom_kind == NK_UI_CUSTOM_IMAGE &&
            command->custom != NULL) {
            /* The image's size stands in for its pixels, which are not dumped. */
            const NkUiImage *image = (const NkUiImage *)command->custom;
            int n = snprintf(image_size, sizeof(image_size), "%ux%u",
                             (unsigned)image->width, (unsigned)image->height);
            text = image_size;
            text_length = n > 0 && (size_t)n < sizeof(image_size) ? (size_t)n : 0u;
        }
        if (command->kind == NK_UI_DRAW_TEXT && command->custom != NULL) {
            const NkUiTextRun *run = (const NkUiTextRun *)command->custom;
            text = run->text != NULL ? run->text : "";
            text_length = run->length;
        }
        used = nk_ui_dump_append(out, cap, used,
            "%08X %s %d %d %d %d %08X \"",
            command->element_id, nk_ui_dump_kind(command->kind, command->custom_kind),
            command->bounds[0], command->bounds[1], command->bounds[2],
            command->bounds[3], command->rgba_pm);
        used = nk_ui_dump_append_text(out, cap, used, text, text_length);
        used = nk_ui_dump_append(out, cap, used, "\"\n");
    }
    if (out != NULL && cap > 0u) {
        size_t end = used < cap ? used : cap - 1u;
        out[end] = '\0';
    }
    return used;
}
