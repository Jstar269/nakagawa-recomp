// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#include <stdio.h>
#include <stdlib.h>

/* Upstream's own warnings, silenced as src/ui/nk_ui_clay.c does. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define CLAY_IMPLEMENTATION
#include "../../third_party/clay/clay.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static int clay_error_count;
static Clay_ErrorType clay_last_error;

static void record_clay_error(Clay_ErrorData error)
{
    clay_error_count++;
    clay_last_error = error.errorType;
    fprintf(stderr, "Clay error %d\n", (int)error.errorType);
}

/* Every character is 10 x 10, so a word's width is ten times its length. */
static Clay_Dimensions measure_text(Clay_StringSlice text,
                                    Clay_TextElementConfig *config,
                                    void *user_data)
{
    (void)config;
    (void)user_data;
    if (text.length <= 0) {
        return (Clay_Dimensions){ 0.0f, 0.0f };
    }
    return (Clay_Dimensions){ 10.0f * (float)text.length, 10.0f };
}

static int expect_rectangle(const char *name, Clay_ElementId id,
                            float x, float y, float width, float height)
{
    Clay_ElementData element = Clay_GetElementData(id);

    if (!element.found) {
        fprintf(stderr, "FAIL: Clay element %s was not found\n", name);
        return 0;
    }

    if (element.boundingBox.x != x || element.boundingBox.y != y ||
        element.boundingBox.width != width ||
        element.boundingBox.height != height) {
        fprintf(stderr,
                "FAIL: %s rectangle expected (%g, %g, %g, %g), got (%g, %g, %g, %g)\n",
                name, (double)x, (double)y, (double)width, (double)height,
                (double)element.boundingBox.x, (double)element.boundingBox.y,
                (double)element.boundingBox.width,
                (double)element.boundingBox.height);
        return 0;
    }
    return 1;
}

int main(void)
{
    uint32_t memory_size = Clay_MinMemorySize();
    void *memory = malloc(memory_size);
    Clay_Arena arena;
    Clay_ErrorHandler error_handler = { record_clay_error, NULL };

    if (!memory) {
        fprintf(stderr, "FAIL: unable to allocate Clay arena (%u bytes)\n",
                (unsigned)memory_size);
        return EXIT_FAILURE;
    }

    arena = Clay_CreateArenaWithCapacityAndMemory(memory_size, memory);
    if (!Clay_Initialize(arena, (Clay_Dimensions){ 120.0f, 80.0f },
                         error_handler)) {
        fprintf(stderr, "FAIL: Clay initialization failed\n");
        free(memory);
        return EXIT_FAILURE;
    }
    Clay_SetMeasureTextFunction(measure_text, NULL);

    Clay_BeginLayout();
    CLAY(CLAY_ID("root"), {
        .layout = {
            .sizing = {
                .width = CLAY_SIZING_FIXED(120.0f),
                .height = CLAY_SIZING_FIXED(80.0f),
            },
            .padding = { .left = 10, .right = 10, .top = 10, .bottom = 10 },
            .childGap = 5,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
        },
    }) {
        CLAY(CLAY_ID("fixed-a"), {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_FIXED(20.0f),
                    .height = CLAY_SIZING_FIXED(20.0f),
                },
            },
        }) {}
        CLAY(CLAY_ID("fixed-b"), {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_FIXED(30.0f),
                    .height = CLAY_SIZING_FIXED(20.0f),
                },
            },
        }) {}
        CLAY(CLAY_ID("grow"), {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_GROW(0),
                    .height = CLAY_SIZING_FIXED(20.0f),
                },
            },
        }) {}
    }
    (void)Clay_EndLayout(0.0f);

    if (clay_error_count != 0 ||
        !expect_rectangle("root", CLAY_ID("root"), 0.0f, 0.0f, 120.0f, 80.0f) ||
        !expect_rectangle("fixed-a", CLAY_ID("fixed-a"), 10.0f, 10.0f, 20.0f, 20.0f) ||
        !expect_rectangle("fixed-b", CLAY_ID("fixed-b"), 35.0f, 10.0f, 30.0f, 20.0f) ||
        !expect_rectangle("grow", CLAY_ID("grow"), 70.0f, 10.0f, 40.0f, 20.0f)) {
        free(memory);
        return EXIT_FAILURE;
    }

    /* An overfull column keeps a wrapped paragraph at its wrapped height. The
       four words wrap to four 10 px lines in the 50 px column; with the fixed
       box under them the content is 120 px in a 100 px column. Compressing the
       column must not squeeze the paragraph back toward one line, or the box
       paints over its later lines. */
    Clay_BeginLayout();
    CLAY(CLAY_ID("column"), {
        .layout = {
            .sizing = {
                .width = CLAY_SIZING_FIXED(50.0f),
                .height = CLAY_SIZING_FIXED(100.0f),
            },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
        },
    }) {
        CLAY(CLAY_ID("paragraph"), {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_GROW(0),
                    .height = CLAY_SIZING_FIT(0),
                },
            },
        }) {
            CLAY_TEXT(CLAY_STRING("aaaa bbbb cccc dddd"),
                      CLAY_TEXT_CONFIG({ .fontSize = 10, .lineHeight = 10,
                                         .wrapMode = CLAY_TEXT_WRAP_WORDS }));
        }
        CLAY(CLAY_ID("after"), {
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_FIXED(50.0f),
                    .height = CLAY_SIZING_FIXED(80.0f),
                },
            },
        }) {}
    }
    (void)Clay_EndLayout(0.0f);

    if (clay_error_count != 0 ||
        !expect_rectangle("paragraph", CLAY_ID("paragraph"), 0.0f, 0.0f, 50.0f, 40.0f) ||
        !expect_rectangle("after", CLAY_ID("after"), 0.0f, 40.0f, 50.0f, 80.0f)) {
        free(memory);
        return EXIT_FAILURE;
    }

    /* An arena request whose byte count passes 32 bits is refused. Formed in
       32 bits, 65536 items of 65536 bytes wrap to 0 and the request
       "succeeds" with no room behind it. */
    {
        char small[256];
        Clay_Arena tiny = Clay_CreateArenaWithCapacityAndMemory(sizeof(small), small);
        void *block = Clay__Array_Allocate_Arena(65536, 65536u, &tiny);

        if (block != NULL || clay_error_count != 1 ||
            clay_last_error != CLAY_ERROR_TYPE_ARENA_CAPACITY_EXCEEDED) {
            fprintf(stderr, "FAIL: a 4 GiB arena request was not refused\n");
            free(memory);
            return EXIT_FAILURE;
        }
    }

    /* A layout past the element limit fills the element-id map. The next
       layout, back under the limit, reuses the slots pruning freed and lays
       out without an error. */
    Clay_BeginLayout();
    for (int i = 0; i < Clay_GetMaxElementCount() + 16; ++i) {
        CLAY(CLAY_IDI("crowd", (uint32_t)i), {
            .layout = { .sizing = { .width = CLAY_SIZING_FIXED(1.0f),
                                    .height = CLAY_SIZING_FIXED(1.0f) } },
        }) {}
    }
    (void)Clay_EndLayout(0.0f);
    clay_error_count = 0;
    Clay_BeginLayout();
    CLAY(CLAY_ID("after-crowd"), {
        .layout = { .sizing = { .width = CLAY_SIZING_FIXED(30.0f),
                                .height = CLAY_SIZING_FIXED(20.0f) } },
    }) {}
    (void)Clay_EndLayout(0.0f);
    if (clay_error_count != 0 ||
        !expect_rectangle("after-crowd", CLAY_ID("after-crowd"), 0.0f, 0.0f, 30.0f, 20.0f)) {
        fprintf(stderr, "FAIL: the layout after a full element-id map did not recover\n");
        free(memory);
        return EXIT_FAILURE;
    }

    free(memory);
    puts("Clay layout selftest passed: root, padding, child gap, fixed children, grow child, "
         "wrapped text kept whole in an overfull column, an oversized arena request refused, "
         "and a layout after a full element-id map");
    return EXIT_SUCCESS;
}
