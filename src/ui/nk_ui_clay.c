/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui.h"
#include "nk_ui_draw.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4244 4305)
#endif
#define CLAY_IMPLEMENTATION
#include "clay.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef struct {
    Clay_Context *context;
    Clay_Context *previous_context;
    char *memory;
    size_t memory_size;
    NkUiTextBackend text;
    float density;
    bool frame_open;
    bool failed;
} NkUiClay;

static int nk_ui_clay_clamp_int(double value) {
    if (value <= (double)INT_MIN + 2.0) {
        return INT_MIN + 2;
    }
    if (value >= (double)INT_MAX - 2.0) {
        return INT_MAX - 2;
    }
    return (int)value;
}

static NkUiPixelRect nk_ui_clay_round_box(Clay_BoundingBox box, float density) {
    NkUiRect rect = {box.x, box.y, box.width, box.height};
    return nk_ui_round_rect(rect, density);
}

static NkUiPixelRect nk_ui_clay_round_scissor(Clay_BoundingBox box, float density) {
    NkUiPixelRect rect;
    rect.x0 = nk_ui_clay_clamp_int(floor((double)box.x * (double)density));
    rect.y0 = nk_ui_clay_clamp_int(floor((double)box.y * (double)density));
    rect.x1 = nk_ui_clay_clamp_int(ceil((double)(box.x + box.width) * (double)density));
    rect.y1 = nk_ui_clay_clamp_int(ceil((double)(box.y + box.height) * (double)density));
    return rect;
}

static uint32_t nk_ui_clay_color(Clay_Color color) {
    unsigned r;
    unsigned g;
    unsigned b;
    unsigned a;
    float values[4] = {color.r, color.g, color.b, color.a};
    unsigned channels[4];
    unsigned i;
    for (i = 0; i < 4u; ++i) {
        float value = values[i];
        if (!isfinite(value) || value < 0.0f) {
            value = 0.0f;
        } else if (value > 255.0f) {
            value = 255.0f;
        }
        channels[i] = (unsigned)floorf(value + 0.5f);
    }
    r = channels[0];
    g = channels[1];
    b = channels[2];
    a = channels[3];
    return (r << 24) | (g << 16) | (b << 8) | a;
}

static Clay_Dimensions nk_ui_clay_measure(Clay_StringSlice text,
                                         Clay_TextElementConfig *config,
                                         void *user_data) {
    NkUiClay *clay = (NkUiClay *)user_data;
    Clay_Dimensions result = {0.0f, 0.0f};
    int logical_px = config != NULL ? (int)config->fontSize : 16;
    int device_px;
    if (clay == NULL || clay->text.measure == NULL || text.chars == NULL ||
        text.length < 0 || logical_px <= 0 || clay->density <= 0.0f) {
        return result;
    }
    device_px = (int)floorf((float)logical_px * clay->density + 0.5f);
    if (device_px < 1) {
        device_px = 1;
    }
    {
        int font = config != NULL ? (int)config->fontId : 0;
        NkUiTextSize measured = clay->text.measure(clay->text.user, font, device_px,
                                                   text.chars, (size_t)text.length);
        if (isfinite(measured.width) && measured.width > 0.0f) {
            result.width = measured.width / clay->density;
        }
        if (isfinite(measured.height) && measured.height > 0.0f) {
            result.height = measured.height / clay->density;
        }
    }
    return result;
}

static void nk_ui_clay_error(Clay_ErrorData error) {
    NkUiClay *clay = (NkUiClay *)error.userData;
    if (clay != NULL) {
        clay->failed = true;
    }
}

static uint16_t nk_ui_clay_u16(float value) {
    float rounded;
    if (!isfinite(value) || value <= 0.0f) {
        return 0u;
    }
    rounded = floorf(value + 0.5f);
    if (rounded > 65535.0f) {
        rounded = 65535.0f;
    }
    return (uint16_t)rounded;
}

static Clay_SizingAxis nk_ui_clay_sizing(NkUiSizing sizing) {
    Clay_SizingAxis axis;
    memset(&axis, 0, sizeof(axis));
    switch (sizing.kind) {
    case NK_UI_SIZE_FIXED:
        axis.type = CLAY__SIZING_TYPE_FIXED;
        axis.size.minMax.min = sizing.value > 0.0f ? sizing.value : 0.0f;
        axis.size.minMax.max = axis.size.minMax.min;
        break;
    case NK_UI_SIZE_PERCENT:
        axis.type = CLAY__SIZING_TYPE_PERCENT;
        axis.size.percent = isfinite(sizing.value) && sizing.value > 0.0f ? sizing.value : 0.0f;
        if (axis.size.percent > 1.0f) {
            axis.size.percent = 1.0f;
        }
        break;
    case NK_UI_SIZE_GROW:
        axis.type = CLAY__SIZING_TYPE_GROW;
        axis.size.minMax.min = isfinite(sizing.min) && sizing.min > 0.0f ? sizing.min : 0.0f;
        axis.size.minMax.max = isfinite(sizing.max) && sizing.max > axis.size.minMax.min ?
                               sizing.max : FLT_MAX;
        break;
    case NK_UI_SIZE_FIT:
    default:
        axis.type = CLAY__SIZING_TYPE_FIT;
        axis.size.minMax.min = isfinite(sizing.min) && sizing.min > 0.0f ? sizing.min : 0.0f;
        axis.size.minMax.max = isfinite(sizing.max) && sizing.max > axis.size.minMax.min ?
                               sizing.max : FLT_MAX;
        break;
    }
    return axis;
}

void *nk_ui_clay_create(size_t arena_bytes, float width, float height,
                        const NkUiTextBackend *text) {
    NkUiClay *clay;
    size_t minimum;
    Clay_Arena arena;
    Clay_ErrorHandler errors;
    Clay_Context *previous;
    minimum = (size_t)Clay_MinMemorySize();
    if (minimum == 0u) {
        return NULL;
    }
    if (arena_bytes == 0u) {
        arena_bytes = minimum;
    }
    if (arena_bytes < minimum) {
        return NULL;
    }
    clay = (NkUiClay *)calloc(1, sizeof(*clay));
    if (clay == NULL) {
        return NULL;
    }
    clay->memory = (char *)malloc(arena_bytes);
    if (clay->memory == NULL) {
        free(clay);
        return NULL;
    }
    clay->memory_size = arena_bytes;
    if (text != NULL) {
        clay->text = *text;
    }
    clay->density = 1.0f;
    previous = Clay_GetCurrentContext();
    arena = Clay_CreateArenaWithCapacityAndMemory(arena_bytes, clay->memory);
    errors.errorHandlerFunction = nk_ui_clay_error;
    errors.userData = clay;
    clay->context = Clay_Initialize(arena,
        (Clay_Dimensions){width > 0.0f ? width : 1.0f,
                          height > 0.0f ? height : 1.0f}, errors);
    if (clay->context == NULL) {
        Clay_SetCurrentContext(previous);
        free(clay->memory);
        free(clay);
        return NULL;
    }
    Clay_SetCurrentContext(clay->context);
    Clay_SetMeasureTextFunction(nk_ui_clay_measure, clay);
    Clay_SetCurrentContext(previous);
    return clay;
}

void nk_ui_clay_destroy(void *opaque_clay) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_Context *current;
    if (clay == NULL) {
        return;
    }
    current = Clay_GetCurrentContext();
    if (current == clay->context) {
        Clay_SetCurrentContext(NULL);
    }
    free(clay->memory);
    free(clay);
}

bool nk_ui_clay_set_dimensions(void *opaque_clay, float width, float height, float density) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_Context *previous;
    if (clay == NULL || clay->context == NULL || !isfinite(width) || !isfinite(height) ||
        !isfinite(density) || density <= 0.0f) {
        return false;
    }
    previous = Clay_GetCurrentContext();
    Clay_SetCurrentContext(clay->context);
    if (clay->density != density) {
        clay->density = density;
        Clay_ResetMeasureTextCache();
    }
    Clay_SetLayoutDimensions((Clay_Dimensions){width, height});
    Clay_SetCurrentContext(previous);
    return true;
}

bool nk_ui_clay_begin(void *opaque_clay) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    if (clay == NULL || clay->context == NULL || clay->frame_open) {
        return false;
    }
    clay->previous_context = Clay_GetCurrentContext();
    Clay_SetCurrentContext(clay->context);
    clay->failed = false;
    clay->frame_open = true;
    Clay_BeginLayout();
    return true;
}

bool nk_ui_clay_open_container(void *opaque_clay, NkUiId id,
                               const NkUiLayout *layout, const NkUiStyle *style) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_ElementDeclaration declaration;
    if (clay == NULL || !clay->frame_open || layout == NULL) {
        return false;
    }
    Clay_SetCurrentContext(clay->context);
    memset(&declaration, 0, sizeof(declaration));
    declaration.id = Clay_GetElementIdWithIndex(CLAY_STRING("nk_ui.container"), id);
    declaration.layout.sizing.width = nk_ui_clay_sizing(layout->width);
    declaration.layout.sizing.height = nk_ui_clay_sizing(layout->height);
    declaration.layout.layoutDirection = layout->direction == NK_UI_LAYOUT_ROW ?
                                         CLAY_LEFT_TO_RIGHT : CLAY_TOP_TO_BOTTOM;
    declaration.layout.childGap = nk_ui_clay_u16(layout->gap);
    declaration.layout.padding.left = nk_ui_clay_u16(layout->padding_left);
    declaration.layout.padding.top = nk_ui_clay_u16(layout->padding_top);
    declaration.layout.padding.right = nk_ui_clay_u16(layout->padding_right);
    declaration.layout.padding.bottom = nk_ui_clay_u16(layout->padding_bottom);
    if (style != NULL) {
        declaration.backgroundColor = (Clay_Color){
            (float)((style->fill_rgba >> 24) & 0xFFu),
            (float)((style->fill_rgba >> 16) & 0xFFu),
            (float)((style->fill_rgba >> 8) & 0xFFu),
            (float)(style->fill_rgba & 0xFFu)
        };
        declaration.cornerRadius = CLAY_CORNER_RADIUS(
            isfinite(style->corner_radius) && style->corner_radius > 0.0f ?
            style->corner_radius : 0.0f);
        declaration.border.color = (Clay_Color){
            (float)((style->border_rgba >> 24) & 0xFFu),
            (float)((style->border_rgba >> 16) & 0xFFu),
            (float)((style->border_rgba >> 8) & 0xFFu),
            (float)(style->border_rgba & 0xFFu)
        };
        declaration.border.width.left = nk_ui_clay_u16(style->border_width);
        declaration.border.width.right = nk_ui_clay_u16(style->border_width);
        declaration.border.width.top = nk_ui_clay_u16(style->border_width);
        declaration.border.width.bottom = nk_ui_clay_u16(style->border_width);
    }
    Clay__OpenElement();
    Clay__ConfigureOpenElement(declaration);
    return true;
}

void nk_ui_clay_close_container(void *opaque_clay) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    if (clay != NULL && clay->frame_open) {
        Clay_SetCurrentContext(clay->context);
        Clay__CloseElement();
    }
}

bool nk_ui_clay_text(void *opaque_clay, NkUiId id, const char *text, size_t length,
                     int font, int size_px, uint32_t rgba) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_ElementDeclaration declaration;
    Clay_TextElementConfig text_config;
    Clay_String contents;
    if (clay == NULL || !clay->frame_open || text == NULL || size_px <= 0 ||
        length > (size_t)INT32_MAX) {
        return false;
    }
    Clay_SetCurrentContext(clay->context);
    memset(&declaration, 0, sizeof(declaration));
    declaration.id = Clay_GetElementIdWithIndex(CLAY_STRING("nk_ui.text"), id);
    declaration.layout.sizing.width = nk_ui_clay_sizing((NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f});
    declaration.layout.sizing.height = nk_ui_clay_sizing((NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f});
    declaration.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    memset(&text_config, 0, sizeof(text_config));
    text_config.textColor = (Clay_Color){
        (float)((rgba >> 24) & 0xFFu),
        (float)((rgba >> 16) & 0xFFu),
        (float)((rgba >> 8) & 0xFFu),
        (float)(rgba & 0xFFu)
    };
    text_config.fontId = font < 0 ? 0u : (uint16_t)(font > 65535 ? 65535 : font);
    text_config.fontSize = (uint16_t)(size_px > 65535 ? 65535 : size_px);
    text_config.lineHeight = text_config.fontSize;
    text_config.wrapMode = CLAY_TEXT_WRAP_WORDS;
    text_config.textAlignment = CLAY_TEXT_ALIGN_LEFT;
    contents.isStaticallyAllocated = false;
    contents.length = (int32_t)length;
    contents.chars = text;
    Clay__OpenElement();
    Clay__ConfigureOpenElement(declaration);
    Clay__OpenTextElement(contents, Clay__StoreTextElementConfig(text_config));
    Clay__CloseElement();
    return true;
}

bool nk_ui_clay_end(void *opaque_clay, NkUiDrawBuffer *draw, float density) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_RenderCommandArray commands;
    Clay_Context *restore;
    int32_t i;
    bool ok = true;
    if (clay == NULL || !clay->frame_open || draw == NULL || density <= 0.0f) {
        return false;
    }
    Clay_SetCurrentContext(clay->context);
    commands = Clay_EndLayout();
    if (clay->failed) {
        ok = false;
    }
    for (i = 0; i < commands.length; ++i) {
        Clay_RenderCommand *command = &commands.internalArray[i];
        Clay_BoundingBox box = command->boundingBox;
        NkUiRect rect = {box.x, box.y, box.width, box.height};
        uint32_t id = command->id;
        switch (command->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
            ok = nk_ui_draw_add_rect(draw, rect, density,
                command->renderData.rectangle.cornerRadius.topLeft,
                nk_ui_clay_color(command->renderData.rectangle.backgroundColor), id) && ok;
            break;
        case CLAY_RENDER_COMMAND_TYPE_BORDER: {
            const Clay_BorderWidth *width = &command->renderData.border.width;
            uint16_t thickness = width->left;
            if (width->right > thickness) thickness = width->right;
            if (width->top > thickness) thickness = width->top;
            if (width->bottom > thickness) thickness = width->bottom;
            ok = nk_ui_draw_add_border(draw, rect, density, (float)thickness,
                nk_ui_clay_color(command->renderData.border.color), id) && ok;
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_TEXT: {
            Clay_TextRenderData *text = &command->renderData.text;
            NkUiPixelRect bounds = nk_ui_clay_round_box(box, density);
            int pixel_size = (int)floorf((float)text->fontSize * density + 0.5f);
            ok = nk_ui_draw_add_text(draw, bounds, text->stringContents.chars,
                (size_t)text->stringContents.length, (int)text->fontId, pixel_size,
                nk_ui_clay_color(text->textColor), id) && ok;
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
            ok = nk_ui_draw_add_scissor(draw, true, nk_ui_clay_round_scissor(box, density), id) && ok;
            break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
            ok = nk_ui_draw_add_scissor(draw, false, nk_ui_clay_round_scissor(box, density), id) && ok;
            break;
        case CLAY_RENDER_COMMAND_TYPE_IMAGE:
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
        case CLAY_RENDER_COMMAND_TYPE_NONE:
        default:
            break;
        }
        if (!ok) {
            break;
        }
    }
    restore = clay->previous_context;
    clay->previous_context = NULL;
    clay->frame_open = false;
    clay->failed = !ok;
    Clay_SetCurrentContext(restore);
    return ok;
}
