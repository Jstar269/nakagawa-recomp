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

/* Clay hashes fontId into its text-measure cache but stores letter spacing as a
   whole number, so the letter spacing travels in fontId's high bits instead (in
   eighths of a logical pixel) and Clay's own letterSpacing stays zero. */
#define NK_UI_CLAY_FONT_BITS 4u
#define NK_UI_CLAY_FONT_MASK 0x000Fu
#define NK_UI_CLAY_SPACING_STEPS 8.0f

static uint16_t nk_ui_clay_pack_font(int font, float letter_spacing) {
    unsigned steps = 0u;
    if (font < 0) font = 0;
    if (font > (int)NK_UI_CLAY_FONT_MASK) font = (int)NK_UI_CLAY_FONT_MASK;
    if (isfinite(letter_spacing) && letter_spacing > 0.0f) {
        float scaled = floorf(letter_spacing * NK_UI_CLAY_SPACING_STEPS + 0.5f);
        steps = scaled > 4095.0f ? 4095u : (unsigned)scaled;
    }
    return (uint16_t)((steps << NK_UI_CLAY_FONT_BITS) | (unsigned)font);
}

static int nk_ui_clay_font(uint16_t packed) {
    return (int)(packed & NK_UI_CLAY_FONT_MASK);
}

static float nk_ui_clay_spacing(uint16_t packed) {
    return (float)(packed >> NK_UI_CLAY_FONT_BITS) / NK_UI_CLAY_SPACING_STEPS;
}

static size_t nk_ui_clay_codepoints(const char *text, size_t length) {
    size_t count = 0u;
    size_t i;
    for (i = 0; i < length; ++i) {
        if (((unsigned char)text[i] & 0xC0u) != 0x80u) {
            ++count;
        }
    }
    return count;
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
        uint16_t packed = config != NULL ? config->fontId : 0u;
        int font = nk_ui_clay_font(packed);
        float spacing = nk_ui_clay_spacing(packed);
        NkUiTextSize measured = clay->text.measure(clay->text.user, font, device_px,
                                                   text.chars, (size_t)text.length);
        if (isfinite(measured.width) && measured.width > 0.0f) {
            result.width = measured.width / clay->density;
            /* As CSS letter-spacing: the gap follows every character,
               the last one too, snapped to whole device pixels as the
               renderer snaps it. Clay measures a run in word slices, and
               the per-character rule keeps their sum equal to the whole. */
            if (spacing > 0.0f) {
                size_t codepoints = nk_ui_clay_codepoints(text.chars, (size_t)text.length);
                float device_spacing = floorf(spacing * clay->density + 0.5f);
                result.width += device_spacing * (float)codepoints / clay->density;
            }
        }
        if (isfinite(measured.height) && measured.height > 0.0f) {
            result.height = measured.height / clay->density;
        }
        /* A run's box is its line height, so a one-line label centres on the
           same box the renderer centres the face's line in. */
        if (config != NULL && (float)config->lineHeight > result.height) {
            result.height = (float)config->lineHeight;
        }
    }
    return result;
}

bool nk_ui_clay_text_styled(void *opaque_clay, NkUiId id, const char *text, size_t length,
                            const NkUiTextStyle *style);

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

bool nk_ui_clay_open_element(void *opaque_clay, NkUiId id, const NkUiLayout *layout,
                             const NkUiStyle *style, void *custom_data) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_ElementDeclaration declaration;
    if (clay == NULL || !clay->frame_open || layout == NULL) {
        return false;
    }
    Clay_SetCurrentContext(clay->context);
    memset(&declaration, 0, sizeof(declaration));
    declaration.id = Clay_GetElementIdWithIndex(CLAY_STRING("nk_ui.container"), id);
    /* The draw list names elements by their nk_ui id, not Clay's hash. */
    declaration.userData = (void *)(uintptr_t)id;
    declaration.layout.sizing.width = nk_ui_clay_sizing(layout->width);
    declaration.layout.sizing.height = nk_ui_clay_sizing(layout->height);
    declaration.layout.layoutDirection = layout->direction == NK_UI_LAYOUT_ROW ?
                                         CLAY_LEFT_TO_RIGHT : CLAY_TOP_TO_BOTTOM;
    declaration.layout.childGap = nk_ui_clay_u16(layout->gap);
    declaration.layout.padding.left = nk_ui_clay_u16(layout->padding_left);
    declaration.layout.padding.top = nk_ui_clay_u16(layout->padding_top);
    declaration.layout.padding.right = nk_ui_clay_u16(layout->padding_right);
    declaration.layout.padding.bottom = nk_ui_clay_u16(layout->padding_bottom);
    declaration.layout.childAlignment.x =
        layout->align_x == NK_UI_ALIGN_CENTER ? CLAY_ALIGN_X_CENTER :
        layout->align_x == NK_UI_ALIGN_END ? CLAY_ALIGN_X_RIGHT : CLAY_ALIGN_X_LEFT;
    declaration.layout.childAlignment.y =
        layout->align_y == NK_UI_ALIGN_CENTER ? CLAY_ALIGN_Y_CENTER :
        layout->align_y == NK_UI_ALIGN_END ? CLAY_ALIGN_Y_BOTTOM : CLAY_ALIGN_Y_TOP;
    if (layout->clip) {
        declaration.clip.horizontal = true;
        declaration.clip.vertical = true;
    }
    if (custom_data != NULL) {
        declaration.custom.customData = custom_data;
    }
    if (style != NULL) {
        declaration.backgroundColor = (Clay_Color){
            (float)((style->fill_rgba >> 24) & 0xFFu),
            (float)((style->fill_rgba >> 16) & 0xFFu),
            (float)((style->fill_rgba >> 8) & 0xFFu),
            (float)(style->fill_rgba & 0xFFu)
        };
        {
            float radius = isfinite(style->corner_radius) && style->corner_radius > 0.0f ?
                           style->corner_radius : 0.0f;
            uint32_t square = style->square_corners;
            declaration.cornerRadius.topLeft = (square & NK_UI_CORNER_SQUARE_TL) ? 0.0f : radius;
            declaration.cornerRadius.topRight = (square & NK_UI_CORNER_SQUARE_TR) ? 0.0f : radius;
            declaration.cornerRadius.bottomRight = (square & NK_UI_CORNER_SQUARE_BR) ? 0.0f : radius;
            declaration.cornerRadius.bottomLeft = (square & NK_UI_CORNER_SQUARE_BL) ? 0.0f : radius;
        }
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

bool nk_ui_clay_open_container(void *opaque_clay, NkUiId id,
                               const NkUiLayout *layout, const NkUiStyle *style) {
    return nk_ui_clay_open_element(opaque_clay, id, layout, style, NULL);
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
    NkUiTextStyle style;
    memset(&style, 0, sizeof(style));
    style.font = font;
    style.size_px = size_px;
    style.rgba = rgba;
    style.wrap = true;
    return nk_ui_clay_text_styled(opaque_clay, id, text, length, &style);
}

bool nk_ui_clay_text_styled(void *opaque_clay, NkUiId id, const char *text, size_t length,
                            const NkUiTextStyle *style) {
    int font = style != NULL ? style->font : 0;
    int size_px = style != NULL ? style->size_px : 0;
    uint32_t rgba = style != NULL ? style->rgba : 0u;
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
    /* A wrapping run takes the row's width so its lines break there; a
       one-line run fits its text. */
    declaration.layout.sizing.width = nk_ui_clay_sizing((NkUiSizing){
        style->wrap ? NK_UI_SIZE_GROW : NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f});
    declaration.layout.sizing.height = nk_ui_clay_sizing((NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f});
    declaration.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    memset(&text_config, 0, sizeof(text_config));
    text_config.textColor = (Clay_Color){
        (float)((rgba >> 24) & 0xFFu),
        (float)((rgba >> 16) & 0xFFu),
        (float)((rgba >> 8) & 0xFFu),
        (float)(rgba & 0xFFu)
    };
    text_config.fontId = nk_ui_clay_pack_font(font, style->letter_spacing);
    text_config.fontSize = (uint16_t)(size_px > 65535 ? 65535 : size_px);
    text_config.lineHeight = isfinite(style->line_height) &&
                             style->line_height >= (float)text_config.fontSize
        ? nk_ui_clay_u16(style->line_height) : text_config.fontSize;
    text_config.wrapMode = style->wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE;
    text_config.userData = (void *)(uintptr_t)id;
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

bool nk_ui_clay_element_rect(void *opaque_clay, NkUiId id, NkUiRect *out) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_Context *restore;
    Clay_ElementData data;
    if (clay == NULL || clay->context == NULL || out == NULL || id == 0u) {
        return false;
    }
    restore = Clay_GetCurrentContext();
    Clay_SetCurrentContext(clay->context);
    data = Clay_GetElementData(Clay_GetElementIdWithIndex(
        CLAY_STRING("nk_ui.container"), id));
    Clay_SetCurrentContext(restore);
    if (!data.found || !isfinite(data.boundingBox.x) ||
        !isfinite(data.boundingBox.y) || !isfinite(data.boundingBox.width) ||
        !isfinite(data.boundingBox.height) || data.boundingBox.width < 0.0f ||
        data.boundingBox.height < 0.0f) {
        return false;
    }
    out->x = data.boundingBox.x;
    out->y = data.boundingBox.y;
    out->w = data.boundingBox.width;
    out->h = data.boundingBox.height;
    return true;
}

bool nk_ui_clay_end(void *opaque_clay, NkUiDrawBuffer *draw, float density,
                    NkUiClayPaint paint, void *paint_user) {
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
        uint32_t id = (uint32_t)(uintptr_t)command->userData;
        switch (command->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
            const Clay_CornerRadius *corner = &command->renderData.rectangle.cornerRadius;
            const float radii[4] = {corner->topLeft, corner->topRight,
                                    corner->bottomRight, corner->bottomLeft};
            ok = nk_ui_draw_add_rect_corners(draw, rect, density, radii,
                nk_ui_clay_color(command->renderData.rectangle.backgroundColor), id) && ok;
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_BORDER: {
            const Clay_BorderWidth *width = &command->renderData.border.width;
            uint16_t thickness = width->left;
            if (width->right > thickness) thickness = width->right;
            if (width->top > thickness) thickness = width->top;
            if (width->bottom > thickness) thickness = width->bottom;
            const Clay_CornerRadius *corner = &command->renderData.border.cornerRadius;
            const float radii[4] = {corner->topLeft, corner->topRight,
                                    corner->bottomRight, corner->bottomLeft};
            ok = nk_ui_draw_add_border_corners(draw, rect, density, (float)thickness, radii,
                nk_ui_clay_color(command->renderData.border.color), id) && ok;
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_TEXT: {
            Clay_TextRenderData *text = &command->renderData.text;
            NkUiPixelRect bounds = nk_ui_clay_round_box(box, density);
            int pixel_size = (int)floorf((float)text->fontSize * density + 0.5f);
            ok = nk_ui_draw_add_text_spaced(draw, bounds, text->stringContents.chars,
                (size_t)text->stringContents.length, nk_ui_clay_font(text->fontId), pixel_size,
                floorf(nk_ui_clay_spacing(text->fontId) * density + 0.5f),
                nk_ui_clay_color(text->textColor), id) && ok;
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
            ok = nk_ui_draw_add_scissor(draw, true, nk_ui_clay_round_scissor(box, density), id) && ok;
            break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
            ok = nk_ui_draw_add_scissor(draw, false, nk_ui_clay_round_scissor(box, density), id) && ok;
            break;
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
            if (paint != NULL && command->renderData.custom.customData != NULL) {
                ok = paint(paint_user, command->renderData.custom.customData, rect,
                           density, id) && ok;
            }
            break;
        case CLAY_RENDER_COMMAND_TYPE_IMAGE:
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
