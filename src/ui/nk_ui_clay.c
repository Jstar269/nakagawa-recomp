/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui.h"
#include "nk_ui_draw.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Clay is vendored as upstream wrote it: C99 plus anonymous unions and empty
   initializers, with a few unused locals and mixed-sign compares. Its warnings
   are silenced here, at the one place it is compiled, and nowhere else. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#if defined(_MSC_VER)
#pragma warning(push)
/* 4244/4305 narrowing, 4201 nameless union, 4189 unused local, 4018/4389 sign. */
#pragma warning(disable: 4244 4305 4201 4189 4018 4389)
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
    Clay_Dimensions dimensions; /* the layout's size, given to Clay as each frame begins */
    bool frame_open;
    bool failed;                /* a fatal layout error this frame */
    void (*report_error)(void *user, const NkUiLayoutError *error);
    void *report_user;
    uint32_t frame_errors;      /* one bit per Clay_ErrorType raised this frame */
    uint32_t previous_errors;   /* the same for the frame before */
    /* Where scroll containers Clay has not laid out yet start, applied as each
       opens; a request no container took is dropped when the frame ends. */
    struct {
        NkUiId id;
        float offset;
    } starts[NK_UI_SCROLL_MAX];
    size_t start_count;
    bool transitioning;         /* a transition ran in the last layout */
} NkUiClay;

/* Clay calls a transition's handler, with no user pointer, for each element
   still transitioning while Clay_EndLayout runs; nk_ui_clay_end clears this
   before the layout and reads it after, on the thread that lays out. */
static bool g_nk_ui_clay_eased;

static bool nk_ui_clay_ease(Clay_TransitionCallbackArguments arguments) {
    g_nk_ui_clay_eased = true;
    return Clay_EaseOut(arguments);
}

/* Clay moves a scroll container 10 px per unit of wheel delta. */
#define NK_UI_CLAY_WHEEL_UNIT 10.0f

static Clay_ElementId nk_ui_clay_container_id(NkUiId id) {
    return Clay_GetElementIdWithIndex(CLAY_STRING("nk_ui.container"), id);
}

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

/* Names a Clay error type and returns whether it is fatal. After a duplicate
   id Clay keeps the first element under that id, a floating element whose
   parent is missing is laid out from the root, and a percentage over 100% is
   laid out as given: the frame is whole. Every other type leaves elements,
   words or memory out of the layout, and a type this table does not know is
   treated the same way. */
bool nk_ui_clay_error_kind(int type, const char **kind) {
    switch (type) {
    case CLAY_ERROR_TYPE_TEXT_MEASUREMENT_FUNCTION_NOT_PROVIDED:
        *kind = "TEXT_MEASUREMENT_FUNCTION_NOT_PROVIDED";
        return true;
    case CLAY_ERROR_TYPE_ARENA_CAPACITY_EXCEEDED:
        *kind = "ARENA_CAPACITY_EXCEEDED";
        return true;
    case CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED:
        *kind = "ELEMENTS_CAPACITY_EXCEEDED";
        return true;
    case CLAY_ERROR_TYPE_TEXT_MEASUREMENT_CAPACITY_EXCEEDED:
        *kind = "TEXT_MEASUREMENT_CAPACITY_EXCEEDED";
        return true;
    case CLAY_ERROR_TYPE_DUPLICATE_ID:
        *kind = "DUPLICATE_ID";
        return false;
    case CLAY_ERROR_TYPE_FLOATING_CONTAINER_PARENT_NOT_FOUND:
        *kind = "FLOATING_CONTAINER_PARENT_NOT_FOUND";
        return false;
    case CLAY_ERROR_TYPE_PERCENTAGE_OVER_1:
        *kind = "PERCENTAGE_OVER_1";
        return false;
    case CLAY_ERROR_TYPE_INTERNAL_ERROR:
        *kind = "INTERNAL_ERROR";
        return true;
    case CLAY_ERROR_TYPE_UNBALANCED_OPEN_CLOSE:
        *kind = "UNBALANCED_OPEN_CLOSE";
        return true;
    case CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED:
        *kind = "HASH_MAP_CAPACITY_EXCEEDED";
        return true;
    default:
        *kind = "UNKNOWN";
        return true;
    }
}

/* The error bit of a render command nk_ui does not draw, beside Clay's own
   error types. */
#define NK_UI_CLAY_UNDRAWN_BIT (1u << 31)

/* Records an error for this frame and reports it once a frame, and not again
   while it repeats frame after frame. */
static void nk_ui_clay_report(NkUiClay *clay, uint32_t bit, NkUiLayoutError report) {
    bool ongoing = ((clay->frame_errors | clay->previous_errors) & bit) != 0u;
    if (report.fatal) {
        clay->failed = true;
    }
    clay->frame_errors |= bit;
    if (!ongoing && clay->report_error != NULL) {
        clay->report_error(clay->report_user, &report);
    }
}

static void nk_ui_clay_error(Clay_ErrorData error) {
    NkUiClay *clay = (NkUiClay *)error.userData;
    NkUiLayoutError report;
    unsigned type = (unsigned)error.errorType;
    if (clay == NULL) {
        return;
    }
    report.fatal = nk_ui_clay_error_kind((int)type, &report.kind);
    report.text = error.errorText.chars;
    report.text_length = error.errorText.length > 0 ? (size_t)error.errorText.length : 0u;
    nk_ui_clay_report(clay, type < 31u ? 1u << type : 0u, report);
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
    float min = isfinite(sizing.min) && sizing.min > 0.0f ? sizing.min : 0.0f;
    float max = isfinite(sizing.max) && sizing.max > min ? sizing.max : FLT_MAX;
    switch (sizing.kind) {
    case NK_UI_SIZE_FIXED:
        return CLAY_SIZING_FIXED(sizing.value > 0.0f ? sizing.value : 0.0f);
    case NK_UI_SIZE_PERCENT: {
        float percent = isfinite(sizing.value) && sizing.value > 0.0f ? sizing.value : 0.0f;
        return CLAY_SIZING_PERCENT(percent > 1.0f ? 1.0f : percent);
    }
    case NK_UI_SIZE_GROW:
        return CLAY_SIZING_GROW(min, max);
    case NK_UI_SIZE_FIT:
    default:
        return CLAY_SIZING_FIT(min, max);
    }
}

void *nk_ui_clay_create(size_t arena_bytes, float width, float height,
                        const NkUiTextBackend *text,
                        void (*report_error)(void *user, const NkUiLayoutError *error),
                        void *report_user) {
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
    clay->report_error = report_error;
    clay->report_user = report_user;
    previous = Clay_GetCurrentContext();
    arena = Clay_CreateArenaWithCapacityAndMemory(arena_bytes, clay->memory);
    errors.errorHandlerFunction = nk_ui_clay_error;
    errors.userData = clay;
    clay->dimensions = (Clay_Dimensions){width > 0.0f ? width : 1.0f,
                                         height > 0.0f ? height : 1.0f};
    clay->context = Clay_Initialize(arena, clay->dimensions, errors);
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
    clay->dimensions = (Clay_Dimensions){width, height};
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
    clay->previous_errors = clay->frame_errors;
    clay->frame_errors = 0u;
    clay->frame_open = true;
    /* Every frame, as Clay's lifecycle asks: Clay compares the size with the
       last frame's, and only on the frame it changes does it skip moving
       transitions (a resize moves everything at once). Given only when the
       window resized, the flag stayed set and no element ever eased. */
    Clay_SetLayoutDimensions(clay->dimensions);
    Clay_BeginLayout();
    return true;
}

static bool nk_ui_clay_open(NkUiClay *clay, NkUiId id, const NkUiLayout *layout,
                            const NkUiStyle *style, void *custom_data,
                            const NkUiFloating *floating) {
    Clay_ElementDeclaration declaration;
    if (clay == NULL || !clay->frame_open || layout == NULL) {
        return false;
    }
    Clay_SetCurrentContext(clay->context);
    memset(&declaration, 0, sizeof(declaration));
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
    if (layout->scroll) {
        declaration.clip.vertical = true;
    }
    if (custom_data != NULL) {
        declaration.custom.customData = custom_data;
    }
    if (floating != NULL) {
        declaration.floating.attachTo = CLAY_ATTACH_TO_PARENT;
        declaration.floating.expand = (Clay_Dimensions){floating->expand, floating->expand};
        declaration.floating.zIndex = floating->z;
        if (floating->centered) {
            declaration.floating.attachPoints.element = CLAY_ATTACH_POINT_CENTER_CENTER;
            declaration.floating.attachPoints.parent = CLAY_ATTACH_POINT_CENTER_CENTER;
        }
        declaration.floating.pointerCaptureMode = floating->passthrough
            ? CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH : CLAY_POINTER_CAPTURE_MODE_CAPTURE;
        declaration.floating.clipTo = floating->clip_to_parent ? CLAY_CLIP_TO_ATTACHED_PARENT
                                                              : CLAY_CLIP_TO_NONE;
        if (floating->draw_id != 0u) {
            declaration.userData = (void *)(uintptr_t)floating->draw_id;
        }
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
        /* A change of target eases there; an element's first frame and its
           last are cuts (no enter or exit state). */
        if (style->transition != 0u && isfinite(style->transition_s) &&
            style->transition_s > 0.0f) {
            uint32_t properties = 0u;
            if (style->transition & NK_UI_TRANSITION_COLOR) {
                properties |= CLAY_TRANSITION_PROPERTY_BACKGROUND_COLOR |
                              CLAY_TRANSITION_PROPERTY_BORDER_COLOR;
            }
            if (style->transition & NK_UI_TRANSITION_BOX) {
                properties |= CLAY_TRANSITION_PROPERTY_BOUNDING_BOX;
            }
            declaration.transition.handler = nk_ui_clay_ease;
            declaration.transition.duration = style->transition_s;
            declaration.transition.properties = (Clay_TransitionProperty)properties;
            declaration.transition.interactionHandling =
                CLAY_TRANSITION_ALLOW_INTERACTIONS_WHILE_TRANSITIONING_POSITION;
        }
    }
    /* nk_ui opens and closes a container in separate calls, which the CLAY()
       macro's scope cannot span, so the glue makes the calls the macro makes. */
    Clay__OpenElementWithId(nk_ui_clay_container_id(id));
    if (!layout->scroll) {
        Clay__ConfigureOpenElement(declaration);
        return true;
    }
    {
        /* Clay keeps the offset; the children follow it, or the start a
           screen asked for before Clay laid the container out. */
        size_t i;
        float start = -1.0f;
        for (i = 0; i < clay->start_count; ++i) {
            if (clay->starts[i].id == id) {
                start = clay->starts[i].offset;
                clay->starts[i] = clay->starts[--clay->start_count];
                break;
            }
        }
        declaration.clip.childOffset = start >= 0.0f ? (Clay_Vector2){0.0f, -start}
                                                     : Clay_GetScrollOffset();
        Clay__ConfigureOpenElement(declaration);
        if (start >= 0.0f) {
            Clay_ScrollContainerData data = Clay_GetScrollContainerData(nk_ui_clay_container_id(id));
            if (data.found) {
                data.scrollPosition->y = -start;
            }
        }
    }
    return true;
}

bool nk_ui_clay_open_element(void *opaque_clay, NkUiId id, const NkUiLayout *layout,
                             const NkUiStyle *style, void *custom_data) {
    return nk_ui_clay_open((NkUiClay *)opaque_clay, id, layout, style, custom_data, NULL);
}

bool nk_ui_clay_open_container(void *opaque_clay, NkUiId id,
                               const NkUiLayout *layout, const NkUiStyle *style) {
    return nk_ui_clay_open((NkUiClay *)opaque_clay, id, layout, style, NULL, NULL);
}

bool nk_ui_clay_open_floating(void *opaque_clay, NkUiId id, const NkUiLayout *layout,
                              const NkUiStyle *style, const NkUiFloating *floating) {
    return floating != NULL &&
           nk_ui_clay_open((NkUiClay *)opaque_clay, id, layout, style, NULL, floating);
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
    Clay__OpenElementWithId(Clay_GetElementIdWithIndex(CLAY_STRING("nk_ui.text"), id));
    Clay__ConfigureOpenElement(declaration);
    Clay__OpenTextElement(contents, text_config);
    Clay__CloseElement();
    return true;
}

/* Tells Clay where the pointer is, between frames, as its lifecycle asks:
   Clay finds the elements under it in the last layout, inside their clip
   containers. down is whether the primary button is held now. */
void nk_ui_clay_set_pointer(void *opaque_clay, float x, float y, bool down) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_Context *restore;
    if (clay == NULL || clay->context == NULL || clay->frame_open ||
        !isfinite(x) || !isfinite(y)) {
        return;
    }
    restore = Clay_GetCurrentContext();
    Clay_SetCurrentContext(clay->context);
    Clay_SetPointerState((Clay_Vector2){x, y}, down);
    Clay_SetCurrentContext(restore);
}

/* Moves scroll container id to show its content from offset down: Clay's own
   offset when Clay knows the container, clamped to its last content, or the
   start it opens at. */
void nk_ui_clay_scroll_to(void *opaque_clay, NkUiId id, float offset) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_Context *restore;
    Clay_ScrollContainerData data;
    size_t i;
    if (clay == NULL || clay->context == NULL || id == 0u || !isfinite(offset)) {
        return;
    }
    if (offset < 0.0f) {
        offset = 0.0f;
    }
    restore = Clay_GetCurrentContext();
    Clay_SetCurrentContext(clay->context);
    data = Clay_GetScrollContainerData(nk_ui_clay_container_id(id));
    Clay_SetCurrentContext(restore);
    if (data.found) {
        float max = data.contentDimensions.height - data.scrollContainerDimensions.height;
        data.scrollPosition->y = -(offset < max ? offset : (max > 0.0f ? max : 0.0f));
        return;
    }
    i = 0;
    while (i < clay->start_count && clay->starts[i].id != id) {
        ++i;
    }
    if (i == clay->start_count) {
        if (clay->start_count == NK_UI_SCROLL_MAX) {
            return;
        }
        ++clay->start_count;
    }
    clay->starts[i].id = id;
    clay->starts[i].offset = offset;
}

bool nk_ui_clay_scroll_state(void *opaque_clay, NkUiId id, NkUiScrollState *out) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_Context *restore;
    Clay_ScrollContainerData data;
    float max;
    if (clay == NULL || clay->context == NULL || id == 0u || out == NULL) {
        return false;
    }
    restore = Clay_GetCurrentContext();
    Clay_SetCurrentContext(clay->context);
    data = Clay_GetScrollContainerData(nk_ui_clay_container_id(id));
    Clay_SetCurrentContext(restore);
    if (!data.found) {
        return false;
    }
    max = data.contentDimensions.height - data.scrollContainerDimensions.height;
    out->offset = -data.scrollPosition->y;
    out->max_offset = max > 0.0f ? max : 0.0f;
    out->viewport = data.scrollContainerDimensions.height;
    return true;
}

/* Whether the wheel at the pointer scrolls something: a vertical scroll
   container under it (a screen's, or one in Clay's layout inspector) has more
   content than room. */
bool nk_ui_clay_transitioning(void *opaque_clay) {
    const NkUiClay *clay = (const NkUiClay *)opaque_clay;
    return clay != NULL && clay->transitioning;
}

bool nk_ui_clay_wheel_scrolls(void *opaque_clay) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_Context *restore;
    Clay_ElementIdArray over;
    bool scrolls = false;
    int32_t i;
    if (clay == NULL || clay->context == NULL) {
        return false;
    }
    restore = Clay_GetCurrentContext();
    Clay_SetCurrentContext(clay->context);
    over = Clay_GetPointerOverIds();
    for (i = 0; i < over.length && !scrolls; ++i) {
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(over.internalArray[i]);
        scrolls = data.found && data.config.vertical &&
                  data.contentDimensions.height > data.scrollContainerDimensions.height;
    }
    Clay_SetCurrentContext(restore);
    return scrolls;
}

/* Clay's layout inspector (Clay_SetDebugModeEnabled): a panel down the
   window's right side that lists the layout and outlines the element under
   the pointer. */
void nk_ui_clay_set_debug(void *opaque_clay, bool enabled) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_Context *restore;
    if (clay == NULL || clay->context == NULL || clay->frame_open) {
        return;
    }
    restore = Clay_GetCurrentContext();
    Clay_SetCurrentContext(clay->context);
    Clay_SetDebugModeEnabled(enabled);
    Clay_SetCurrentContext(restore);
}

/* Runs Clay's scrolling for this frame, between frames as its lifecycle asks:
   the wheel (wheel_px, positive towards the top), a drag or touch with the
   pointer held, and the momentum a drag leaves. Returns how far, in logical px,
   the container in ids that moved most has moved. */
float nk_ui_clay_update_scroll(void *opaque_clay, const NkUiId *ids, size_t count,
                               float wheel_px, float delta_s) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_Context *restore;
    float before[NK_UI_SCROLL_MAX];
    float moved = 0.0f;
    size_t i;
    if (clay == NULL || clay->context == NULL || clay->frame_open || ids == NULL ||
        count > NK_UI_SCROLL_MAX || !isfinite(wheel_px) || !isfinite(delta_s)) {
        return 0.0f;
    }
    restore = Clay_GetCurrentContext();
    Clay_SetCurrentContext(clay->context);
    for (i = 0; i < count; ++i) {
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(nk_ui_clay_container_id(ids[i]));
        before[i] = data.found ? data.scrollPosition->y : 0.0f;
    }
    Clay_UpdateScrollContainers(true, (Clay_Vector2){0.0f, wheel_px / NK_UI_CLAY_WHEEL_UNIT},
                                delta_s);
    for (i = 0; i < count; ++i) {
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(nk_ui_clay_container_id(ids[i]));
        if (data.found && fabsf(data.scrollPosition->y - before[i]) > moved) {
            moved = fabsf(data.scrollPosition->y - before[i]);
        }
    }
    Clay_SetCurrentContext(restore);
    return moved;
}

/* The innermost container under the pointer that accept takes, or 0. Clay
   lists each element under the pointer after its ancestors, so the list is
   walked from its end. */
NkUiId nk_ui_clay_pointer_target(void *opaque_clay, bool (*accept)(void *user, NkUiId id),
                                 void *user) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_Context *restore;
    Clay_ElementIdArray over;
    uint32_t container;
    NkUiId target = 0u;
    int32_t i;
    if (clay == NULL || clay->context == NULL || accept == NULL) {
        return 0u;
    }
    restore = Clay_GetCurrentContext();
    Clay_SetCurrentContext(clay->context);
    over = Clay_GetPointerOverIds();
    container = Clay_GetElementId(CLAY_STRING("nk_ui.container")).baseId;
    for (i = over.length - 1; i >= 0 && target == 0u; --i) {
        Clay_ElementId element = over.internalArray[i];
        if (element.baseId == container && accept(user, (NkUiId)element.offset)) {
            target = (NkUiId)element.offset;
        }
    }
    Clay_SetCurrentContext(restore);
    return target;
}

bool nk_ui_clay_appeared(void *opaque_clay, NkUiId id) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    const Clay_LayoutElementHashMapItem *item;
    if (clay == NULL || !clay->frame_open || id == 0u) {
        return false;
    }
    Clay_SetCurrentContext(clay->context);
    item = Clay__GetHashMapItem(nk_ui_clay_container_id(id).id);
    return item != &Clay_LayoutElementHashMapItem_DEFAULT && item->appearedThisFrame;
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
    data = Clay_GetElementData(nk_ui_clay_container_id(id));
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
                    float delta_s, NkUiClayPaint paint, void *paint_user) {
    NkUiClay *clay = (NkUiClay *)opaque_clay;
    Clay_RenderCommandArray commands;
    Clay_Context *restore;
    int32_t i;
    bool ok = true;
    if (clay == NULL || !clay->frame_open || draw == NULL || density <= 0.0f) {
        return false;
    }
    Clay_SetCurrentContext(clay->context);
    g_nk_ui_clay_eased = false;
    commands = Clay_EndLayout(delta_s);
    clay->transitioning = g_nk_ui_clay_eased;
    /* Past its element limit Clay drops the elements that do not fit and
       stops closing elements, so the only error it raises is the unbalanced
       open and close that follows. Report the cause too. */
    if (clay->context->booleanWarnings.maxElementsExceeded) {
        nk_ui_clay_error((Clay_ErrorData){
            CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED,
            CLAY_STRING("Layout elements exceeded the maximum element count."), clay});
    }
    if (clay->failed) {
        ok = false;
    }
    /* A fatal error leaves nothing worth drawing. */
    for (i = 0; ok && i < commands.length; ++i) {
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
                nk_ui_clay_color(command->renderData.rectangle.backgroundColor), id);
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
                nk_ui_clay_color(command->renderData.border.color), id);
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_TEXT: {
            Clay_TextRenderData *text = &command->renderData.text;
            NkUiPixelRect bounds = nk_ui_clay_round_box(box, density);
            int pixel_size = (int)floorf((float)text->fontSize * density + 0.5f);
            ok = nk_ui_draw_add_text_spaced(draw, bounds, text->stringContents.chars,
                (size_t)text->stringContents.length, nk_ui_clay_font(text->fontId), pixel_size,
                floorf(nk_ui_clay_spacing(text->fontId) * density + 0.5f),
                nk_ui_clay_color(text->textColor), id);
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
            ok = nk_ui_draw_add_scissor(draw, true, nk_ui_clay_round_scissor(box, density), id);
            break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
            ok = nk_ui_draw_add_scissor(draw, false, nk_ui_clay_round_scissor(box, density), id);
            break;
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
            if (paint != NULL && command->renderData.custom.customData != NULL) {
                ok = paint(paint_user, command->renderData.custom.customData, rect,
                           density, id);
            }
            break;
        case CLAY_RENDER_COMMAND_TYPE_NONE:
            break;
        default: {
            /* nk_ui declares no Clay image or overlay-colour element: pictures
               are custom elements (nk_ui_image_art). A command it cannot draw
               drops the frame, so a picture never just goes missing. */
            static const char text[] = "A render command nk_ui does not draw: an image "
                                       "or overlay-colour element.";
            NkUiLayoutError report = {"UNDRAWN_RENDER_COMMAND", text, sizeof(text) - 1u, true};
            nk_ui_clay_report(clay, NK_UI_CLAY_UNDRAWN_BIT, report);
            ok = false;
            break;
        }
        }
    }
    clay->start_count = 0u;
    restore = clay->previous_context;
    clay->previous_context = NULL;
    clay->frame_open = false;
    clay->failed = !ok;
    Clay_SetCurrentContext(restore);
    return ok;
}
